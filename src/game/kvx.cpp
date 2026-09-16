#include "game/kvx.h"

#include <array>
#include <cstring>

namespace rl::game {

namespace {

int32_t rd32(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

// Face normals, indexed by MeshVertex::n.
constexpr int kNormalIndex[3][2] = {{0, 1}, {2, 3}, {4, 5}};   // [axis][0 = +, 1 = -]

}  // namespace

bool loadKvx(const std::vector<uint8_t>& d, VoxelMeshData& out, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (d.size() < 28 + 768) return fail("file too small");
    const int32_t numBytes = rd32(&d[0]);
    const int xs = rd32(&d[4]), ys = rd32(&d[8]), zs = rd32(&d[12]);
    const int32_t xp = rd32(&d[16]), yp = rd32(&d[20]), zp = rd32(&d[24]);
    if (xs <= 0 || ys <= 0 || zs <= 0 || xs > 512 || ys > 512 || zs > 512) return fail("bad dimensions");
    if (numBytes <= 24 || static_cast<size_t>(numBytes) + 4 + 768 > d.size()) return fail("bad mip size");
    const size_t xoffAt = 28;
    const size_t xyAt = xoffAt + 4 * static_cast<size_t>(xs + 1);
    const size_t dataBase = 28;   // slab offsets are relative to the start of the xoffset table
    if (xyAt + 2 * static_cast<size_t>(xs) * static_cast<size_t>(ys + 1) > d.size()) return fail("truncated offset tables");
    const uint8_t* pal = &d[d.size() - 768];

    // Dense grid in the output frame: index = (x * H + y) * D + z, with y up.
    const int W = xs, H = zs, D = ys;
    std::vector<uint8_t> grid(static_cast<size_t>(W) * H * D, 0);   // 0 = empty, else palette index + 1
    auto cell = [&](int x, int y, int z) -> uint8_t& { return grid[(static_cast<size_t>(x) * H + y) * D + z]; };
    const size_t mipEnd = 4 + static_cast<size_t>(numBytes);
    int voxels = 0;
    for (int x = 0; x < xs; ++x) {
        const int32_t xo = rd32(&d[xoffAt + 4 * static_cast<size_t>(x)]);
        for (int y = 0; y < ys; ++y) {
            const uint16_t a = rd16(&d[xyAt + 2 * (static_cast<size_t>(x) * (ys + 1) + y)]);
            const uint16_t b = rd16(&d[xyAt + 2 * (static_cast<size_t>(x) * (ys + 1) + y + 1)]);
            size_t s = dataBase + static_cast<size_t>(xo) + a, e = dataBase + static_cast<size_t>(xo) + b;
            if (e > mipEnd || s > e) return fail("slab offset out of range");
            while (s + 3 <= e) {
                const int ztop = d[s], zlen = d[s + 1];
                s += 3;
                if (s + static_cast<size_t>(zlen) > e || ztop + zlen > zs) return fail("slab out of range");
                for (int k = 0; k < zlen; ++k) {
                    const int vz = ztop + k;
                    cell(x, zs - 1 - vz, y) = static_cast<uint8_t>(d[s + static_cast<size_t>(k)] + 1);
                    ++voxels;
                }
                s += static_cast<size_t>(zlen);
            }
        }
    }

    out = {};
    out.sizeX = xs; out.sizeY = ys; out.sizeZ = zs;
    out.voxels = voxels;
    out.pivot = glm::vec3(static_cast<float>(xp) / 256.f, static_cast<float>(zs) - static_cast<float>(zp) / 256.f, static_cast<float>(yp) / 256.f);

    auto colour = [&](uint8_t idx) {
        const uint8_t* c = pal + 3 * (idx - 1);
        auto ex = [](uint8_t v) { return static_cast<uint32_t>((v & 63) * 255 / 63); };
        return ex(c[0]) | (ex(c[1]) << 8) | (ex(c[2]) << 16) | (255u << 24);
    };
    auto at = [&](int x, int y, int z) -> uint8_t {
        if (x < 0 || y < 0 || z < 0 || x >= W || y >= H || z >= D) return 0;
        return cell(x, y, z);
    };

    // Greedy meshing: for each axis, sweep the planes between slices and merge
    // same-coloured, same-facing cells into rectangles.
    const int dims[3] = {W, H, D};
    std::vector<int32_t> mask;
    for (int axis = 0; axis < 3; ++axis) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const int nu = dims[u], nv = dims[v];
        mask.assign(static_cast<size_t>(nu) * nv, 0);
        for (int slice = 0; slice <= dims[axis]; ++slice) {
            int p[3] = {0, 0, 0};
            for (int j = 0; j < nv; ++j)
                for (int i = 0; i < nu; ++i) {
                    p[axis] = slice; p[u] = i; p[v] = j;
                    int q[3] = {p[0], p[1], p[2]};
                    q[axis] = slice - 1;
                    const uint8_t a = at(q[0], q[1], q[2]);   // cell below the plane
                    const uint8_t b = at(p[0], p[1], p[2]);   // cell above the plane
                    int32_t m = 0;
                    if (a && !b) m = static_cast<int32_t>(a);          // a's +axis face
                    else if (b && !a) m = -static_cast<int32_t>(b);    // b's -axis face
                    if (m != 0) {
                        // Baked ambient occlusion: for each corner of this face, count the solid cells
                        // on the outer layer touching that corner (0-4). The counts ride in the mask
                        // above the colour so greedy merging only joins cells with the same shading.
                        const int outer = m > 0 ? slice : slice - 1;
                        int key = 0;
                        for (int c = 0; c < 4; ++c) {
                            const int cu = i + (c & 1), cv = j + (c >> 1);
                            int count = 0;
                            for (int du2 = -1; du2 <= 0; ++du2)
                                for (int dv2 = -1; dv2 <= 0; ++dv2) {
                                    int o[3];
                                    o[axis] = outer; o[u] = cu + du2; o[v] = cv + dv2;
                                    if (at(o[0], o[1], o[2])) ++count;
                                }
                            key = key * 5 + count;
                        }
                        m = (m > 0 ? 1 : -1) * ((m > 0 ? m : -m) + 256 * key);
                    }
                    mask[static_cast<size_t>(j) * nu + i] = m;
                }
            for (int j = 0; j < nv; ++j)
                for (int i = 0; i < nu;) {
                    const int32_t m = mask[static_cast<size_t>(j) * nu + i];
                    if (m == 0) { ++i; continue; }
                    int w = 1;
                    while (i + w < nu && mask[static_cast<size_t>(j) * nu + i + w] == m) ++w;
                    int h = 1;
                    for (bool grow = true; grow && j + h < nv; ) {
                        for (int k = 0; k < w; ++k)
                            if (mask[static_cast<size_t>(j + h) * nu + i + k] != m) { grow = false; break; }
                        if (grow) ++h;
                    }
                    // Emit the quad.
                    const bool positive = m > 0;
                    const int packed = positive ? m : -m;
                    const uint32_t rgba = colour(static_cast<uint8_t>(packed & 0xFF));
                    int aoKey = packed >> 8;
                    int ao[4];   // corner (i,j), (i+1,j), (i,j+1), (i+1,j+1)
                    for (int c = 3; c >= 0; --c) { ao[c] = aoKey % 5; aoKey /= 5; }
                    int base[3] = {0, 0, 0};
                    base[axis] = slice; base[u] = i; base[v] = j;
                    int du[3] = {0, 0, 0}, dv[3] = {0, 0, 0};
                    du[u] = w; dv[v] = h;
                    // Winding: cross(du, dv) must point along the face normal (CCW from outside).
                    glm::vec3 n = glm::cross(glm::vec3(du[0], du[1], du[2]), glm::vec3(dv[0], dv[1], dv[2]));
                    glm::vec3 want(0.f);
                    want[axis] = positive ? 1.f : -1.f;
                    if (glm::dot(n, want) < 0.f) std::swap(du, dv);
                    const uint8_t nIdx = static_cast<uint8_t>(kNormalIndex[axis][positive ? 0 : 1]);
                    const uint32_t first = static_cast<uint32_t>(out.verts.size());
                    auto push = [&](int ox, int oy, int oz) {
                        render::MeshVertex mv;
                        mv.x = static_cast<int16_t>(ox); mv.y = static_cast<int16_t>(oy); mv.z = static_cast<int16_t>(oz);
                        mv.n = static_cast<int16_t>(nIdx);
                        // Which corner of the merged quad this is, in (u, v) terms, picks its occlusion.
                        const int o[3] = {ox, oy, oz};
                        const int corner = (o[u] > base[u] ? 1 : 0) + (o[v] > base[v] ? 2 : 0);
                        const float shade = 1.f - 0.2f * static_cast<float>(ao[corner]);
                        uint32_t c = rgba;
                        uint32_t r = static_cast<uint32_t>((c & 0xFF) * shade), g = static_cast<uint32_t>(((c >> 8) & 0xFF) * shade), b = static_cast<uint32_t>(((c >> 16) & 0xFF) * shade);
                        mv.rgba = (c & 0xFF000000u) | (b << 16) | (g << 8) | r;
                        out.verts.push_back(mv);
                    };
                    push(base[0], base[1], base[2]);
                    push(base[0] + du[0], base[1] + du[1], base[2] + du[2]);
                    push(base[0] + du[0] + dv[0], base[1] + du[1] + dv[1], base[2] + du[2] + dv[2]);
                    push(base[0] + dv[0], base[1] + dv[1], base[2] + dv[2]);
                    out.idx.insert(out.idx.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
                    for (int jj = 0; jj < h; ++jj)
                        for (int k = 0; k < w; ++k) mask[static_cast<size_t>(j + jj) * nu + i + k] = 0;
                    i += w;
                }
        }
    }
    return true;
}

}  // namespace rl::game
