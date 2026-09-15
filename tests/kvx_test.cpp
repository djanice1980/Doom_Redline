// KVX loader / greedy mesher checks on synthetic voxel files.
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/kvx.h"

using rl::game::VoxelMeshData;
using rl::game::loadKvx;

namespace {

int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

void put32(std::vector<uint8_t>& v, int32_t x) { uint8_t b[4]; std::memcpy(b, &x, 4); v.insert(v.end(), b, b + 4); }
void put16(std::vector<uint8_t>& v, uint16_t x) { uint8_t b[2]; std::memcpy(b, &x, 2); v.insert(v.end(), b, b + 2); }

// Builds a one-mip KVX from a dense grid `g[x][y][z]` (0 = empty, else palette index + 1).
std::vector<uint8_t> makeKvx(int xs, int ys, int zs, const std::vector<uint8_t>& g, glm::ivec3 pivot256) {
    std::vector<uint8_t> vox;
    std::vector<int32_t> xoff;
    std::vector<uint16_t> xy;
    const size_t tables = 4 * static_cast<size_t>(xs + 1) + 2 * static_cast<size_t>(xs) * static_cast<size_t>(ys + 1);
    for (int x = 0; x < xs; ++x) {
        xoff.push_back(static_cast<int32_t>(tables + vox.size()));   // relative to the xoffset table start
        size_t colStart = vox.size();
        for (int y = 0; y < ys; ++y) {
            xy.push_back(static_cast<uint16_t>(vox.size() - colStart));
            int z = 0;
            while (z < zs) {
                if (!g[(static_cast<size_t>(x) * ys + y) * zs + z]) { ++z; continue; }
                int z0 = z;
                while (z < zs && g[(static_cast<size_t>(x) * ys + y) * zs + z]) ++z;
                vox.push_back(static_cast<uint8_t>(z0));
                vox.push_back(static_cast<uint8_t>(z - z0));
                vox.push_back(0);   // cull bits (unused by the loader)
                for (int k = z0; k < z; ++k) vox.push_back(static_cast<uint8_t>(g[(static_cast<size_t>(x) * ys + y) * zs + k] - 1));
            }
        }
        xy.push_back(static_cast<uint16_t>(vox.size() - colStart));
    }
    xoff.push_back(static_cast<int32_t>(tables + vox.size()));
    std::vector<uint8_t> out;
    put32(out, static_cast<int32_t>(24 + tables + vox.size()));
    put32(out, xs); put32(out, ys); put32(out, zs);
    put32(out, pivot256.x); put32(out, pivot256.y); put32(out, pivot256.z);
    for (int32_t o : xoff) put32(out, o);
    for (uint16_t o : xy) put16(out, o);
    out.insert(out.end(), vox.begin(), vox.end());
    for (int i = 0; i < 256; ++i) { out.push_back(static_cast<uint8_t>(i & 63)); out.push_back(static_cast<uint8_t>((i * 2) & 63)); out.push_back(static_cast<uint8_t>(63 - (i & 63))); }
    return out;
}

}  // namespace

int main() {
    // 1. A solid 2x2x2 block of one colour meshes to exactly 6 quads.
    {
        std::vector<uint8_t> g(8, 5);
        auto bytes = makeKvx(2, 2, 2, g, {256, 256, 512});
        VoxelMeshData m;
        std::string err;
        CHECK(loadKvx(bytes, m, &err));
        CHECK(m.voxels == 8);
        CHECK(m.quads() == 6);
        CHECK(m.verts.size() == 24 && m.idx.size() == 36);
        CHECK(m.sizeX == 2 && m.sizeY == 2 && m.sizeZ == 2);
        // Pivot (1, 1, 2) in KVX -> (1, 0, 1) in the Y-up frame (feet at Y = 0).
        CHECK(std::fabs(m.pivot.x - 1.f) < 1e-5f && std::fabs(m.pivot.y - 0.f) < 1e-5f && std::fabs(m.pivot.z - 1.f) < 1e-5f);
        // Every vertex lies on the grid corners and each face normal is consistent with its winding.
        for (size_t i = 0; i < m.idx.size(); i += 3) {
            auto P = [&](size_t k) { const auto& v = m.verts[m.idx[k]]; return glm::vec3(v.x, v.y, v.z); };
            glm::vec3 n = glm::cross(P(i + 1) - P(i), P(i + 2) - P(i));
            static const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            CHECK(glm::dot(n, normals[m.verts[m.idx[i]].n]) > 0.f);
        }
        // Colour comes from the palette (index 4 -> r = 4 * 255 / 63).
        CHECK(!m.verts.empty() && (m.verts[0].rgba & 0xFF) == static_cast<uint32_t>(4 * 255 / 63));
    }
    // 2. Two side-by-side voxels of different colours: 10 exposed faces, none merged.
    {
        std::vector<uint8_t> g = {1, 2};   // xs = 2, ys = 1, zs = 1
        auto bytes = makeKvx(2, 1, 1, g, {0, 0, 0});
        VoxelMeshData m;
        CHECK(loadKvx(bytes, m));
        CHECK(m.voxels == 2);
        CHECK(m.quads() == 10);
    }
    // 3. Same shape, same colour: the long faces merge (6 quads).
    {
        std::vector<uint8_t> g = {3, 3};
        auto bytes = makeKvx(2, 1, 1, g, {0, 0, 0});
        VoxelMeshData m;
        CHECK(loadKvx(bytes, m));
        CHECK(m.quads() == 6);
    }
    // 4. A hollow shell keeps the inner faces (voxel z is flipped to Y-up correctly: the
    //    top slab of a 1x1x3 column ends up at the highest Y).
    {
        std::vector<uint8_t> g = {7, 0, 9};   // xs = ys = 1, zs = 3: z = 0 (top) solid, z = 2 (bottom) solid
        auto bytes = makeKvx(1, 1, 3, g, {0, 0, 768});
        VoxelMeshData m;
        CHECK(loadKvx(bytes, m));
        CHECK(m.voxels == 2 && m.quads() == 12);
        int topColourMaxY = -1, bottomColourMaxY = -1;
        for (const auto& v : m.verts) {
            int r = static_cast<int>(v.rgba & 0xFF);
            if (r == 6 * 255 / 63) topColourMaxY = std::max(topColourMaxY, static_cast<int>(v.y));
            if (r == 8 * 255 / 63) bottomColourMaxY = std::max(bottomColourMaxY, static_cast<int>(v.y));
        }
        CHECK(topColourMaxY == 3 && bottomColourMaxY == 1);
    }
    // 5. Garbage is rejected, not crashed on.
    {
        std::vector<uint8_t> junk(2000, 0xAB);
        VoxelMeshData m;
        std::string err;
        CHECK(!loadKvx(junk, m, &err));
        CHECK(!err.empty());
    }
    if (failures == 0) std::printf("kvx_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
