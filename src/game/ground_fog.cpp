#include "game/ground_fog.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace rl::game {

namespace {
// Per 30 Hz step unless noted.
constexpr float kGravity = 2.f;         // m/s^2 per unit of depth: ridges roll out at about 1.4 m/s
constexpr float kVelocityKeep = 0.975f; // what is left of a push after a step (the rest relaxes to the ambient swirl)
constexpr float kRefill = 0.006f;       // the depth creeps back to its rest value over several seconds
constexpr float kWispRelax = 0.02f;     // the streaks fade back to the idle pattern over a couple of seconds
constexpr float kVelocityBlur = 0.15f;
constexpr float kDensityBlur = 0.04f;
constexpr float kMaxBodySpeed = 14.f;   // m/s: a rocket shoves the fog hard, not supersonically
// How fast the fog moves by itself, as a share of what it was first tuned to: the swirl's
// strength and the clock its patterns drift on. 1 read as a busy churn; a ground mist
// creeps. Pushes (bodies, blasts) are not slowed. volume.frag has the same for its own
// drifting terms (kIdlePace there).
constexpr float kIdlePace = 0.6f;

float smooth01(float f) { return f * f * (3.f - 2.f * f); }

// Smooth value noise in [0, 1] from a 64 x 64 periodic table: cheap enough to sample a
// few times per cell per step.
constexpr int kNoise = 64;
const std::array<float, kNoise * kNoise>& noiseTable() {
    static const std::array<float, kNoise * kNoise> table = [] {
        std::array<float, kNoise * kNoise> a{};
        uint32_t s = 0x9E3779B9u;
        for (float& v : a) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            v = static_cast<float>(s & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
        }
        return a;
    }();
    return table;
}
float noise(float x, float y) {
    const auto& t = noiseTable();
    const float fx = std::floor(x), fy = std::floor(y);
    const int i = static_cast<int>(fx) & (kNoise - 1), j = static_cast<int>(fy) & (kNoise - 1);
    const int i1 = (i + 1) & (kNoise - 1), j1 = (j + 1) & (kNoise - 1);
    const float u = smooth01(x - fx), v = smooth01(y - fy);
    const float a = t[static_cast<size_t>(j * kNoise + i)], b = t[static_cast<size_t>(j * kNoise + i1)];
    const float c = t[static_cast<size_t>(j1 * kNoise + i)], d = t[static_cast<size_t>(j1 * kNoise + i1)];
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}

// The streak pattern the wisp channel drifts back to when nothing stirs it.
float wispTarget(glm::vec2 p, float t) {
    return noise(p.x * 0.85f + t * 0.05f, p.y * 0.85f - t * 0.03f) * 0.65f + noise(p.x * 1.9f - t * 0.08f, p.y * 1.9f + t * 0.06f) * 0.35f;
}
}  // namespace

std::vector<uint8_t> GroundFog::wispPattern(int size) {
    // Gradient noise on lattices of period 3, 6, 12, ... cells: the gradients wrap at the
    // period, so the whole pattern tiles. Coordinates run 0..3 across the texture.
    constexpr int kBase = 3, kOctaves = 5;
    struct Grid { int period; std::vector<glm::vec2> g; };
    std::vector<Grid> grids;
    uint32_t s = 0x2545F491u;
    for (int o = 0; o < kOctaves; ++o) {
        Grid gr{kBase << o, {}};
        gr.g.resize(static_cast<size_t>(gr.period) * static_cast<size_t>(gr.period));
        for (glm::vec2& v : gr.g) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            const float a = static_cast<float>(s & 0xFFFFFFu) / static_cast<float>(0x1000000u) * 6.2831853f;
            v = {std::cos(a), std::sin(a)};
        }
        grids.push_back(std::move(gr));
    }
    auto gnoise = [&](const Grid& gr, float x, float y) {
        const float fx0 = std::floor(x), fy0 = std::floor(y);
        const int xi = static_cast<int>(fx0), yi = static_cast<int>(fy0);
        const float fx = x - fx0, fy = y - fy0;
        auto dotg = [&](int i, int j, float dx, float dy) {
            const int P = gr.period;
            const glm::vec2 g = gr.g[static_cast<size_t>(((j % P) + P) % P) * static_cast<size_t>(P) + static_cast<size_t>(((i % P) + P) % P)];
            return g.x * dx + g.y * dy;
        };
        const float n00 = dotg(xi, yi, fx, fy), n10 = dotg(xi + 1, yi, fx - 1.f, fy);
        const float n01 = dotg(xi, yi + 1, fx, fy - 1.f), n11 = dotg(xi + 1, yi + 1, fx - 1.f, fy - 1.f);
        const float u = fx * fx * fx * (fx * (fx * 6.f - 15.f) + 10.f), v = fy * fy * fy * (fy * (fy * 6.f - 15.f) + 10.f);
        const float a = n00 + (n10 - n00) * u, b = n01 + (n11 - n01) * u;
        return a + (b - a) * v;
    };
    auto fbm = [&](float x, float y, int octaves, float gain) {
        float sum = 0.f, amp = 1.f, total = 0.f, f = 1.f;
        for (int o = 0; o < octaves; ++o) {
            sum += amp * gnoise(grids[static_cast<size_t>(o)], x * f, y * f);
            total += amp; amp *= gain; f *= 2.f;
        }
        return sum / total;
    };
    // Warp offsets are whole lattice steps or simple fractions of the base period, and
    // the warp itself tiles, so the warped pattern tiles too.
    constexpr float kWarp = 1.4f;
    std::vector<float> f(static_cast<size_t>(size) * static_cast<size_t>(size));
    float lo = 1e9f, hi = -1e9f;
    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) {
            const float x = (static_cast<float>(i) + 0.5f) / static_cast<float>(size) * kBase;
            const float y = (static_cast<float>(j) + 0.5f) / static_cast<float>(size) * kBase;
            const float qx = fbm(x, y, 4, 0.5f), qy = fbm(x + 0.5f, y + 1.5f, 4, 0.5f);
            const float rx = fbm(x + kWarp * qx + 1.f, y + kWarp * qy + 2.f, 4, 0.5f);
            const float ry = fbm(x + kWarp * qx + 2.f, y + kWarp * qy, 4, 0.5f);
            const float v = fbm(x + kWarp * rx, y + kWarp * ry, 5, 0.55f);
            f[static_cast<size_t>(j) * static_cast<size_t>(size) + static_cast<size_t>(i)] = v;
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
    std::vector<uint8_t> out(f.size());
    for (size_t k = 0; k < f.size(); ++k) out[k] = static_cast<uint8_t>(std::lround((f[k] - lo) / std::max(hi - lo, 1e-6f) * 255.f));
    return out;
}

void GroundFog::setBounds(float x0, float z0, float x1, float z1, float minCell) {
    const float spanX = std::max(x1 - x0, 1.f), spanZ = std::max(z1 - z0, 1.f);
    const float cell = std::max(minCell, std::max(spanX, spanZ) / static_cast<float>(kMaxCells));
    const int w = std::clamp(static_cast<int>(std::ceil(spanX / cell)), 2, kMaxCells);
    const int h = std::clamp(static_cast<int>(std::ceil(spanZ / cell)), 2, kMaxCells);

    // Carry the old field over (resampled) where the two areas overlap.
    GroundFog old;
    const bool carry = !empty();
    if (carry) { old = std::move(*this); time_ = old.time_; swell_ = old.swell_; spinKeep_ = old.spinKeep_; }
    w_ = w; h_ = h; x0_ = x0; z0_ = z0; cell_ = cell;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    d_.assign(n, 0.f); vx_.assign(n, 0.f); vz_.assign(n, 0.f); wisp_.assign(n, 0.f); tmp_.assign(n, 0.f); rest_.assign(n, 0.f);
    nx_.assign(n, 0.f); nz_.assign(n, 0.f); curl_.assign(n, 0.f); sx_.assign(n, 0.f); sz_.assign(n, 0.f); tx_.assign(n, 0.f); tz_.assign(n, 0.f); psi_.assign(n, 0.f); swellRest_.assign(n, 0.f); wispIdle_.assign(n, 0.f);
    fresh_.assign(n, 1);
    solid_.assign(n, 0);
    pixels_.assign(n * 4, 0);
    acc_ = 0.f;
    for (int j = 0; j < h_; ++j)
        for (int i = 0; i < w_; ++i) wisp_[idx(i, j)] = wispTarget(cellCentre(i, j), time_ * kIdlePace);
    refreshIdle(0);
    refreshIdle(2);
    if (carry) {
        for (int j = 0; j < h_; ++j)
            for (int i = 0; i < w_; ++i) {
                const glm::vec2 p = cellCentre(i, j);
                const float ci = (p.x - old.x0_) / old.cell_ - 0.5f, cj = (p.y - old.z0_) / old.cell_ - 0.5f;
                if (ci < 0.f || cj < 0.f || ci > static_cast<float>(old.w_ - 1) || cj > static_cast<float>(old.h_ - 1)) continue;
                const size_t k = idx(i, j);
                d_[k] = old.sample(old.d_, ci, cj);
                vx_[k] = old.sample(old.vx_, ci, cj);
                vz_[k] = old.sample(old.vz_, ci, cj);
                sx_[k] = old.sample(old.sx_, ci, cj);
                sz_[k] = old.sample(old.sz_, ci, cj);
                wisp_[k] = old.sample(old.wisp_, ci, cj);
                fresh_[k] = 0;
            }
    }
    bake();
}

float GroundFog::sample(const std::vector<float>& f, float ci, float cj) const {
    ci = std::clamp(ci, 0.f, static_cast<float>(w_ - 1) - 0.001f);
    cj = std::clamp(cj, 0.f, static_cast<float>(h_ - 1) - 0.001f);
    const int i = static_cast<int>(ci), j = static_cast<int>(cj);
    const int i1 = std::min(i + 1, w_ - 1), j1 = std::min(j + 1, h_ - 1);
    const float fx = ci - static_cast<float>(i), fy = cj - static_cast<float>(j);
    const float a = f[idx(i, j)], b = f[idx(i1, j)], c = f[idx(i, j1)], e = f[idx(i1, j1)];
    return (a * (1.f - fx) + b * fx) * (1.f - fy) + (c * (1.f - fx) + e * fx) * fy;
}

float GroundFog::densityAt(float x, float z) const {
    if (empty()) return 0.f;
    return sample(d_, (x - x0_) / cell_ - 0.5f, (z - z0_) / cell_ - 0.5f);
}

float GroundFog::speedAt(float x, float z) const {
    if (empty()) return 0.f;
    const float ci = (x - x0_) / cell_ - 0.5f, cj = (z - z0_) / cell_ - 0.5f;
    return glm::length(glm::vec2(sample(vx_, ci, cj) + sample(sx_, ci, cj), sample(vz_, ci, cj) + sample(sz_, ci, cj)));
}

glm::vec2 GroundFog::flowAt(float x, float z) const {
    if (empty()) return glm::vec2(0.f);
    const float ci = (x - x0_) / cell_ - 0.5f, cj = (z - z0_) / cell_ - 0.5f;
    return {sample(vx_, ci, cj) + sample(sx_, ci, cj), sample(vz_, ci, cj) + sample(sz_, ci, cj)};
}

float GroundFog::spin() const {
    double s = 0.0;
    size_t n = 0;
    const float c2 = 2.f * cell_;
    for (int j = 1; j < h_ - 1; ++j)
        for (int i = 1; i < w_ - 1; ++i) {
            const size_t k = idx(i, j);
            if (solid_[k]) continue;
            const size_t up = k + static_cast<size_t>(w_), dn = k - static_cast<size_t>(w_);
            const float w = (vz_[k + 1] + sz_[k + 1] - vz_[k - 1] - sz_[k - 1]) / c2 - (vx_[up] + sx_[up] - vx_[dn] - sx_[dn]) / c2;
            s += std::fabs(w); ++n;
        }
    return n ? static_cast<float>(s / static_cast<double>(n)) : 0.f;
}

float GroundFog::meanDensity() const {
    double s = 0.0;
    size_t n = 0;
    for (size_t k = 0; k < d_.size(); ++k)
        if (!solid_[k]) { s += d_[k]; ++n; }
    return n ? static_cast<float>(s / static_cast<double>(n)) : 0.f;
}

bool GroundFog::clearPath(float ci, float cj, int i, int j) const {
    const float dx = static_cast<float>(i) - ci, dz = static_cast<float>(j) - cj;
    const int n = static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dz)) * 2.f));
    for (int s = 1; s < n; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(n);
        const int a = static_cast<int>(std::lround(ci + dx * t)), b = static_cast<int>(std::lround(cj + dz * t));
        if (a >= 0 && b >= 0 && a < w_ && b < h_ && solid_[idx(a, b)]) return false;
    }
    return true;
}

void GroundFog::stir(const Body& b) {
    if (empty() || b.radius <= 0.f || b.strength <= 0.f) return;
    // The stretch the body covered in one step: a rocket crosses several cells per step.
    const glm::vec2 p1(b.pos.x, b.pos.z);
    const glm::vec2 seg = glm::vec2(b.vel.x, b.vel.z) * kStep;
    const glm::vec2 p0 = p1 - seg;
    glm::vec2 v(b.vel.x, b.vel.z);
    float speed = glm::length(v);
    if (speed > kMaxBodySpeed) { v *= kMaxBodySpeed / speed; speed = kMaxBodySpeed; }
    const float thin = 0.03f + 0.45f * std::min(1.f, speed / 4.f);   // per step, at the body's centre
    const float r = b.radius;
    const glm::vec2 lo = glm::min(p0, p1) - r, hi = glm::max(p0, p1) + r;
    const int i0 = std::max(0, static_cast<int>(std::floor((lo.x - x0_) / cell_))), i1 = std::min(w_ - 1, static_cast<int>(std::ceil((hi.x - x0_) / cell_)));
    const int j0 = std::max(0, static_cast<int>(std::floor((lo.y - z0_) / cell_))), j1 = std::min(h_ - 1, static_cast<int>(std::ceil((hi.y - z0_) / cell_)));
    const float segLen2 = glm::dot(seg, seg);
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const size_t k = idx(i, j);
            if (solid_[k]) continue;
            const glm::vec2 q = cellCentre(i, j);
            const float t = segLen2 > 1e-8f ? std::clamp(glm::dot(q - p0, seg) / segLen2, 0.f, 1.f) : 1.f;
            const glm::vec2 off = q - (p0 + seg * t);
            const float dist = glm::length(off);
            if (dist >= r) continue;
            const glm::vec2 near = p0 + seg * t;   // the wake does not reach through a wall
            if (!clearPath((near.x - x0_) / cell_ - 0.5f, (near.y - z0_) / cell_ - 0.5f, i, j)) continue;
            const float f = std::min(1.f, smooth01(1.f - dist / r) * b.strength);
            d_[k] -= d_[k] * thin * f;
            // The fog nearby is dragged along with the body and shouldered out sideways;
            // the shove piles a ridge that rolls on, and the wake fills back in behind.
            const glm::vec2 out = dist > 1e-3f ? off / dist : glm::vec2(0.f);
            const glm::vec2 want = v * 0.8f + out * (speed * 0.6f);
            const float rate = f * 0.25f;
            vx_[k] += (want.x - vx_[k]) * rate;
            vz_[k] += (want.y - vz_[k]) * rate;
        }
}

void GroundFog::blast(glm::vec3 pos, float radius, float strength) {
    if (empty() || radius <= 0.f) return;
    const float r = radius / cell_;
    const float cx = (pos.x - x0_) / cell_ - 0.5f, cz = (pos.z - z0_) / cell_ - 0.5f;
    const int i0 = std::max(0, static_cast<int>(std::floor(cx - r))), i1 = std::min(w_ - 1, static_cast<int>(std::ceil(cx + r)));
    const int j0 = std::max(0, static_cast<int>(std::floor(cz - r))), j1 = std::min(h_ - 1, static_cast<int>(std::ceil(cz + r)));
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const size_t k = idx(i, j);
            if (solid_[k]) continue;
            const float dx = static_cast<float>(i) - cx, dz = static_cast<float>(j) - cz;
            const float dist = std::sqrt(dx * dx + dz * dz);
            if (dist >= r || !clearPath(cx, cz, i, j)) continue;   // the wall takes the blast for the room behind it
            const float f = 1.f - dist / r;
            // Some of it burns off; the rest is thrown outward (strongest part-way out),
            // piles into a ring, and slides back in over the next few seconds.
            d_[k] -= d_[k] * std::min(1.f, 0.5f * strength * smooth01(f));
            if (dist > 1e-3f) {
                const float push = 3.5f * strength * std::sin(f * 3.14159265f);
                vx_[k] += dx / dist * push;
                vz_[k] += dz / dist * push;
            }
        }
}

void GroundFog::gust(float x0, float z0, float x1, float z1, glm::vec2 vel) {
    if (empty()) return;
    const int i0 = std::max(0, static_cast<int>(std::floor((x0 - x0_) / cell_))), i1 = std::min(w_ - 1, static_cast<int>(std::ceil((x1 - x0_) / cell_)));
    const int j0 = std::max(0, static_cast<int>(std::floor((z0 - z0_) / cell_))), j1 = std::min(h_ - 1, static_cast<int>(std::ceil((z1 - z0_) / cell_)));
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const size_t k = idx(i, j);
            if (solid_[k]) continue;
            vx_[k] += (vel.x - vx_[k]) * 0.3f;
            vz_[k] += (vel.y - vz_[k]) * 0.3f;
        }
}

void GroundFog::advect(std::vector<float>& f, const std::vector<float>& ux, const std::vector<float>& uz) {
    // Semi-Lagrangian: each cell takes the value from where its fog came from.
    const float s = kStep / cell_;
    for (int j = 0; j < h_; ++j)
        for (int i = 0; i < w_; ++i) {
            const size_t k = idx(i, j);
            tmp_[k] = sample(f, static_cast<float>(i) - ux[k] * s, static_cast<float>(j) - uz[k] * s);
        }
    f.swap(tmp_);
}

void GroundFog::diffuse(std::vector<float>& f, float amount) {
    tmp_ = f;
    for (int j = 1; j < h_ - 1; ++j)
        for (int i = 1; i < w_ - 1; ++i) {
            const size_t k = idx(i, j);
            const float avg = (f[k - 1] + f[k + 1] + f[k - static_cast<size_t>(w_)] + f[k + static_cast<size_t>(w_)]) * 0.25f;
            tmp_[k] = f[k] + amount * (avg - f[k]);
        }
    f.swap(tmp_);
}

void GroundFog::settleSolid() {
    // Inside walls: still, and as deep as the open cells next to them, so sampling
    // across a wall face neither drains the fog into the stone nor pulls in a hole.
    for (int j = 0; j < h_; ++j)
        for (int i = 0; i < w_; ++i) {
            const size_t k = idx(i, j);
            if (!solid_[k]) continue;
            vx_[k] = vz_[k] = sx_[k] = sz_[k] = 0.f;
            float sd = 0.f, sw = 0.f;
            int n = 0;
            const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
            for (int m = 0; m < 4; ++m) {
                const int a = i + di[m], b = j + dj[m];
                if (a < 0 || b < 0 || a >= w_ || b >= h_ || solid_[idx(a, b)]) continue;
                sd += d_[idx(a, b)]; sw += wisp_[idx(a, b)]; ++n;
            }
            if (n) { d_[k] = sd / static_cast<float>(n); wisp_[k] = sw / static_cast<float>(n); }
            else d_[k] = rest_[k];
        }
}

void GroundFog::refreshIdle(int which) {
    const float t = time_ * kIdlePace;
    for (int j = 0; j < h_; ++j)
        for (int i = 0; i < w_; ++i) {
            const glm::vec2 p = cellCentre(i, j);
            const size_t k = idx(i, j);
            if (which == 0)
                psi_[k] = noise(p.x * 0.16f + t * 0.05f, p.y * 0.16f - t * 0.035f) + 0.5f * noise(p.x * 0.37f - t * 0.07f, p.y * 0.37f + t * 0.04f);
            else if (which == 1)
                swellRest_[k] = rest_[k] * (1.f + 0.8f * swell_ * (noise(p.x * 0.22f + t * 0.03f, p.y * 0.22f + t * 0.05f) - 0.5f));
            else
                wispIdle_[k] = wispTarget(p, t);
        }
}

void GroundFog::step() {
    time_ += kStep;
    const float c2 = 2.f * cell_;
    for (const Body& b : bodies_) stir(b);

    // The idle motion: a slowly drifting potential whose curl is a swirl that moves
    // the fog without bunching it (the swell, further down, does the bunching).
    const bool calm = swell_ <= 0.f;
    if (!calm || phase_ == 2) refreshIdle(phase_);
    phase_ = (phase_ + 1) % 3;
    const float swirl = 2.2f * kIdlePace * swell_;   // m^2/s per unit of potential: drifts of a tenth or two of a metre a second
    auto at = [&](const std::vector<float>& f, int i, int j, float self) {   // a neighbour, or `self` past an edge or wall
        if (i < 0 || j < 0 || i >= w_ || j >= h_) return self;
        const size_t k = idx(i, j);
        return solid_[k] ? self : f[k];
    };
    for (int j = 0; j < h_; ++j)
        for (int i = 0; i < w_; ++i) {
            const size_t k = idx(i, j);
            if (solid_[k]) { vx_[k] = vz_[k] = 0.f; continue; }
            float ax = 0.f, az = 0.f;
            if (!calm) {
                const float ps = psi_[k];
                ax = swirl * (at(psi_, i, j + 1, ps) - at(psi_, i, j - 1, ps)) / c2;
                az = -swirl * (at(psi_, i + 1, j, ps) - at(psi_, i - 1, j, ps)) / c2;
            }
            // Pushes die away into the idle swirl...
            vx_[k] += (ax - vx_[k]) * (1.f - kVelocityKeep);
            vz_[k] += (az - vz_[k]) * (1.f - kVelocityKeep);
            // ...and deep fog slides toward shallow, like water finding its level.
            const float dk = d_[k];
            vx_[k] -= kGravity * (at(d_, i + 1, j, dk) - at(d_, i - 1, j, dk)) / c2 * kStep;
            vz_[k] -= kGravity * (at(d_, i, j + 1, dk) - at(d_, i, j - 1, dk)) / c2 * kStep;
        }

    // The flow keeps its spin, in a layer of its own. Measure how fast the whole flow
    // turns at every cell, then push each cell sideways around the nearest spot that
    // turns faster (vorticity confinement: f = spinKeep * cell * (N x curl), N pointing
    // up the slope of |curl|). The blur and the damping would otherwise smear an eddy
    // out within a second; fed like this, the shear at the edges of a wake rolls up
    // into curls that turn on for a few seconds and drift with the fog.
    // That push is not free of divergence, and in shallow water a flow that runs
    // together piles the fog up: fed into the main flow it filled a runner's trail in
    // within a second. So the spin lives in its own velocity (sx, sz), which drags the
    // wisps (here and, through the texture, in the renderer) but never moves the depth.
    if (spinKeep_ > 0.f) {
        for (int j = 0; j < h_; ++j)
            for (int i = 0; i < w_; ++i) {
                const size_t k = idx(i, j);
                if (solid_[k]) { curl_[k] = 0.f; continue; }
                const float zr = at(vz_, i + 1, j, 0.f) + at(sz_, i + 1, j, 0.f), zl = at(vz_, i - 1, j, 0.f) + at(sz_, i - 1, j, 0.f);
                const float xu = at(vx_, i, j + 1, 0.f) + at(sx_, i, j + 1, 0.f), xd = at(vx_, i, j - 1, 0.f) + at(sx_, i, j - 1, 0.f);
                curl_[k] = (zr - zl - xu + xd) / c2;
            }
        const float gain = spinKeep_ * cell_ * kStep;
        for (int j = 0; j < h_; ++j)
            for (int i = 0; i < w_; ++i) {
                const size_t k = idx(i, j);
                if (solid_[k]) continue;
                sx_[k] *= kVelocityKeep;   // it dies away like a push does
                sz_[k] *= kVelocityKeep;
                const float self = std::fabs(curl_[k]);
                auto mag = [&](int a, int b) { return (a < 0 || b < 0 || a >= w_ || b >= h_ || solid_[idx(a, b)]) ? self : std::fabs(curl_[idx(a, b)]); };
                const float gx = (mag(i + 1, j) - mag(i - 1, j)) / c2, gz = (mag(i, j + 1) - mag(i, j - 1)) / c2;
                const float len = std::sqrt(gx * gx + gz * gz);
                if (len < 1e-4f) continue;
                const float w = curl_[k];
                sx_[k] += gain * (gz / len) * w;
                sz_[k] -= gain * (gx / len) * w;
            }
    }

    // The velocity carries itself (both components from the same, old field), then
    // spreads a little. The spin rides the whole flow.
    {
        const float s = kStep / cell_;
        for (int j = 0; j < h_; ++j)
            for (int i = 0; i < w_; ++i) {
                const size_t k = idx(i, j);
                const float bi = static_cast<float>(i) - vx_[k] * s, bj = static_cast<float>(j) - vz_[k] * s;
                nx_[k] = sample(vx_, bi, bj);
                nz_[k] = sample(vz_, bi, bj);
                if (spinKeep_ > 0.f) {
                    const float ci = static_cast<float>(i) - (vx_[k] + sx_[k]) * s, cj = static_cast<float>(j) - (vz_[k] + sz_[k]) * s;
                    tx_[k] = sample(sx_, ci, cj);
                    tz_[k] = sample(sz_, ci, cj);
                }
            }
        vx_.swap(nx_);
        vz_.swap(nz_);
        if (spinKeep_ > 0.f) { sx_.swap(tx_); sz_.swap(tz_); }
    }
    diffuse(vx_, kVelocityBlur);
    diffuse(vz_, kVelocityBlur);
    if (spinKeep_ > 0.f) { diffuse(sx_, kVelocityBlur); diffuse(sz_, kVelocityBlur); }
    for (size_t k = 0; k < solid_.size(); ++k)
        if (solid_[k]) vx_[k] = vz_[k] = sx_[k] = sz_[k] = 0.f;

    // Where the flow spreads out the fog thins, where it runs together it piles up.
    // Measured before the depth moves, applied after.
    for (int j = 0; j < h_; ++j)
        for (int i = 0; i < w_; ++i) {
            const size_t k = idx(i, j);
            nx_[k] = solid_[k] ? 0.f : (at(vx_, i + 1, j, 0.f) - at(vx_, i - 1, j, 0.f) + at(vz_, i, j + 1, 0.f) - at(vz_, i, j - 1, 0.f)) / c2;
        }
    advect(d_, vx_, vz_);
    for (size_t k = 0; k < tx_.size(); ++k) { tx_[k] = vx_[k] + sx_[k]; tz_[k] = vz_[k] + sz_[k]; }
    advect(wisp_, tx_, tz_);
    diffuse(d_, kDensityBlur);
    for (int j = 0; j < h_; ++j)
        for (int i = 0; i < w_; ++i) {
            const size_t k = idx(i, j);
            if (solid_[k]) continue;
            d_[k] *= 1.f - std::clamp(nx_[k] * kStep, -0.3f, 0.3f);
            // It creeps back toward its rest depth, which itself heaves slowly: banks
            // build and drain away, and the levelling turns them into a gentle swell.
            const float rest = calm ? rest_[k] : swellRest_[k];
            d_[k] = std::clamp(d_[k] + (rest - d_[k]) * kRefill, 0.f, kMaxDepth);
            wisp_[k] += (wispIdle_[k] - wisp_[k]) * kWispRelax;
        }
    settleSolid();
}

void GroundFog::update(float dt) {
    if (empty()) return;
    acc_ = std::min(acc_ + std::max(dt, 0.f), 4.f * kStep);   // a long hitch does not run a burst of steps
    bool stepped = false;
    while (acc_ >= kStep) { step(); acc_ -= kStep; stepped = true; }
    if (stepped) bake();
}

void GroundFog::bake() {
    for (size_t k = 0; k < d_.size(); ++k) {
        pixels_[4 * k] = static_cast<uint8_t>(std::lround(std::clamp(d_[k] / kMaxDepth, 0.f, 1.f) * 255.f));
        pixels_[4 * k + 1] = static_cast<uint8_t>(std::lround(std::clamp(wisp_[k], 0.f, 1.f) * 255.f));
        // 128 is still (127.5 would be, but a still cell must decode to exactly zero or the
        // wisps creep): the shader decodes (byte - 128) / 127 * kMaxFlow.
        pixels_[4 * k + 2] = static_cast<uint8_t>(128 + std::lround(std::clamp((vx_[k] + sx_[k]) / kMaxFlow, -1.f, 1.f) * 127.f));
        pixels_[4 * k + 3] = static_cast<uint8_t>(128 + std::lround(std::clamp((vz_[k] + sz_[k]) / kMaxFlow, -1.f, 1.f) * 127.f));
    }
}

}  // namespace rl::game
