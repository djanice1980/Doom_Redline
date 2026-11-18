#pragma once
// Interactive ground fog, the way the 2026 Ocarina of Time remake does it: the fog is
// a 2D field over the floor, simulated like a sheet of shallow water. Each cell holds
// how deep the fog is there and how it is moving. Deep fog spreads into shallow fog,
// so a hole fills back in from its edges and a push piles a ridge that rolls on and
// settles, the way water sloshes. A slow drifting swirl and swell keep it moving
// when nothing touches it, bodies plough through it, fast things drag a wind behind
// them, and blasts throw it outward. The renderer turns the sheet into a layer of
// volume: the deeper the fog, the higher its top stands.
//
// A second, passive channel carries "wisps": fine streaks of thicker and thinner fog
// that ride the flow, so the motion shows up as streaming detail, not just thickness.
//
// It runs on the CPU at a fixed 30 Hz: a 256 x 256 field steps in about a millisecond,
// and the result is a two-channel texture the size of a small icon.
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace rl::game {

class GroundFog {
public:
    static constexpr int kMaxCells = 256;     // the texture the renderer samples is this square
    static constexpr float kStep = 1.f / 30.f;
    static constexpr float kMaxDepth = 2.f;   // fog depth the texture can hold (the rest value is about 1)

    // Covers x in [x0, x1) and z in [z0, z1); cells are square, as small as the
    // kMaxCells cap allows (never under `minCell` metres). What was already
    // simulated is carried over where the old and new areas overlap.
    void setBounds(float x0, float z0, float x1, float z1, float minCell = 0.3f);
    bool empty() const { return w_ == 0; }
    // The depth the fog settles back to, per world position (0..1). Applied to every
    // cell; call after setBounds. Cells the last setBounds added (outside the old area)
    // start at it; `fillNow` snaps every cell to it.
    template <class F> void setRest(F&& restAt, bool fillNow) {
        for (int j = 0; j < h_; ++j)
            for (int i = 0; i < w_; ++i) {
                const glm::vec2 p = cellCentre(i, j);
                const size_t k = idx(i, j);
                const float r = glm::clamp(static_cast<float>(restAt(p.x, p.y)), 0.f, 1.f);
                rest_[k] = r;
                if (fillNow || fresh_[k]) d_[k] = r;
                fresh_[k] = 0;
            }
        refreshIdle(1);
        bake();
    }
    // Where the fog cannot go (walls, rock, a shut door), per world position. The fog
    // neither flows into nor through these cells. Can change at any time (a gate opens).
    template <class F> void setSolid(F&& solidAt) {
        for (int j = 0; j < h_; ++j)
            for (int i = 0; i < w_; ++i) {
                const glm::vec2 p = cellCentre(i, j);
                solid_[idx(i, j)] = solidAt(p.x, p.y) ? 1 : 0;
            }
        settleSolid();
        bake();
    }
    // How much the fog moves by itself (0 = dead still, 1 = a gentle swirl and swell).
    void setSwell(float amount) { swell_ = glm::clamp(amount, 0.f, 2.f); }
    void clear() { *this = GroundFog(); }

    // The bodies in or over the fog this frame: each simulation step applies them all,
    // so the effect does not depend on the frame rate. A body at `pos` moving with `vel`
    // (m/s, xz used) shoulders the fog aside and drags a wake behind it; a fast one is
    // swept along the stretch it covered in a step, so a rocket leaves an unbroken
    // trail. `radius` in metres; `strength` scales the whole effect (a rocket flying
    // above the layer stirs it less than one skimming through it).
    void clearBodies() { bodies_.clear(); }
    void addBody(glm::vec3 pos, glm::vec3 vel, float radius, float strength = 1.f) { bodies_.push_back({pos, vel, radius, strength}); }
    // A blast: throws the fog outwards from `pos` and burns some of it away inside `radius`.
    void blast(glm::vec3 pos, float radius, float strength = 1.f);
    // A sweep: everything inside the box [x0, x1] x [z0, z1] is shoved along `vel`.
    void gust(float x0, float z0, float x1, float z1, glm::vec2 vel);

    // Advances the simulation by `dt` seconds in fixed steps.
    void update(float dt);

    // Two bytes per cell, row j = z0 + j * cell, width() x height(): fog depth
    // (0..kMaxDepth scaled to 0..255), then the wisp channel (0..1 scaled to 0..255).
    const std::vector<uint8_t>& pixels() const { return pixels_; }
    int width() const { return w_; }
    int height() const { return h_; }
    float x0() const { return x0_; }
    float z0() const { return z0_; }
    float cell() const { return cell_; }
    float densityAt(float x, float z) const;   // bilinear, for tests and logging
    float meanDensity() const;                 // over the open cells
    float speedAt(float x, float z) const;     // m/s, for tests

private:
    size_t idx(int i, int j) const { return static_cast<size_t>(j) * static_cast<size_t>(w_) + static_cast<size_t>(i); }
    glm::vec2 cellCentre(int i, int j) const { return {x0_ + (static_cast<float>(i) + 0.5f) * cell_, z0_ + (static_cast<float>(j) + 0.5f) * cell_}; }
    float sample(const std::vector<float>& f, float ci, float cj) const;   // cell coordinates, clamped bilinear
    void step();
    struct Body { glm::vec3 pos, vel; float radius, strength; };
    void stir(const Body& b);
    void advect(std::vector<float>& f);
    void diffuse(std::vector<float>& f, float amount);
    void settleSolid();   // solid cells: no motion, depth copied from the open cells beside them
    bool clearPath(float ci, float cj, int i, int j) const;   // no solid cell on the line from (ci, cj) to cell (i, j)
    void bake();
    void refreshIdle(int which);   // 0 swirl, 1 swell, 2 streaks

    int w_ = 0, h_ = 0;
    float x0_ = 0.f, z0_ = 0.f, cell_ = 1.f;
    std::vector<float> d_, vx_, vz_, wisp_, tmp_, rest_, nx_, nz_;
    // The idle patterns (swirl potential, heaving rest depth, streak pattern): they drift
    // slowly, so each is refreshed every third step, in turn.
    std::vector<float> psi_, swellRest_, wispIdle_;
    int phase_ = 0;
    std::vector<uint8_t> fresh_;   // cells added by the last setBounds, not yet given a rest value
    std::vector<uint8_t> solid_;
    std::vector<uint8_t> pixels_;
    float acc_ = 0.f;
    float time_ = 0.f;
    float swell_ = 1.f;
    std::vector<Body> bodies_;
};

}  // namespace rl::game
