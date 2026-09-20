#include "game/dungeon.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace rl::game {

bool Dungeon::worldToTile(float x, float z, int& i, int& j) const {
    if (tiles_.empty()) return false;
    i = static_cast<int>(std::floor(x - originX()));
    j = static_cast<int>(std::floor(kZTop - z));
    return i >= 0 && i < w_ && j >= 0 && j < d_;
}

bool Dungeon::floorAt(float x, float z) const {
    int i, j;
    if (!worldToTile(x, z, i, j)) return false;
    return tile(i, j) == Floor;
}

int Dungeon::ceilingAt(int i, int j) const {
    const Tile t = tile(i, j);
    if (t == Rock) return 0;
    if (t == Floor) return (!rooms_.empty() && bossRoom().contains(i, j)) ? kHallHeight : kRoomHeight;
    int h = 0;
    for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di)
            if (tile(i + di, j + dj) == Floor) h = std::max(h, ceilingAt(i + di, j + dj));
    return h;
}

float Dungeon::ceilingAt(float x, float z) const {
    int i, j;
    if (!worldToTile(x, z, i, j)) return 0.f;
    return static_cast<float>(ceilingAt(i, j));
}

void Dungeon::carveRoom(const DungeonRoom& r) {
    for (int j = r.z0; j < r.z0 + r.d; ++j)
        for (int i = r.x0; i < r.x0 + r.w; ++i) set(i, j, Floor);
}

// An L-shaped corridor: along x first, then along z (the bend is where the two meet).
void Dungeon::carveCorridor(int x0, int z0, int x1, int z1, int width) {
    const int half = width / 2;
    for (int i = std::min(x0, x1); i <= std::max(x0, x1); ++i)
        for (int k = -half; k < width - half; ++k) set(i, z0 + k, Floor);
    for (int j = std::min(z0, z1); j <= std::max(z0, z1); ++j)
        for (int k = -half; k < width - half; ++k) set(x1 + k, j, Floor);
}

void Dungeon::generate(uint32_t seed, int level) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.f, 1.f);
    const int L = std::max(1, level);
    // Size: level 1 is a crypt of three rooms and the boss's hall, level 9 and up a
    // sprawl of eight rooms. The grid grows with it (a margin of rock all round).
    const int nRooms = std::min(8, 2 + (L + 1) / 2);         // ordinary rooms, entrance included
    w_ = std::min(64, 30 + 4 * L);
    d_ = std::min(80, 34 + 5 * L);
    tiles_.assign(static_cast<size_t>(w_ * d_), Rock);
    rooms_.clear();
    torches_.clear();

    // The entrance room sits right behind the gate, centred on x = 0.
    const int gx = w_ / 2;   // tile column at x = 0
    {
        DungeonRoom r;
        r.w = 7 + static_cast<int>(u(rng) * 3.f);
        r.d = 6 + static_cast<int>(u(rng) * 3.f);
        r.x0 = gx - r.w / 2;
        r.z0 = 4;
        r.entrance = true;
        rooms_.push_back(r);
    }
    // The boss's hall: big, at the far end.
    {
        DungeonRoom r;
        r.w = std::min(w_ - 4, 15 + std::min(6, L / 2));
        r.d = std::min(20, 15 + std::min(5, L / 2));
        r.x0 = 2 + static_cast<int>(u(rng) * static_cast<float>(std::max(1, w_ - 4 - r.w)));
        r.z0 = d_ - 2 - r.d;
        r.boss = true;
        rooms_.push_back(r);
    }
    // Ordinary rooms: random rectangles that keep a tile of rock from every other room.
    auto overlaps = [&](const DungeonRoom& a) {
        for (const DungeonRoom& b : rooms_)
            if (a.x0 < b.x0 + b.w + 2 && b.x0 < a.x0 + a.w + 2 && a.z0 < b.z0 + b.d + 2 && b.z0 < a.z0 + a.d + 2) return true;
        return false;
    };
    for (int k = 1; k < nRooms; ++k) {
        for (int attempt = 0; attempt < 300; ++attempt) {
            DungeonRoom r;
            r.w = 5 + static_cast<int>(u(rng) * 7.f);
            r.d = 5 + static_cast<int>(u(rng) * 7.f);
            r.x0 = 2 + static_cast<int>(u(rng) * static_cast<float>(std::max(1, w_ - 4 - r.w)));
            r.z0 = 2 + static_cast<int>(u(rng) * static_cast<float>(std::max(1, d_ - 4 - r.d)));
            if (r.x0 + r.w > w_ - 2 || r.z0 + r.d > d_ - 2) continue;
            if (overlaps(r)) continue;
            rooms_.push_back(r);
            break;
        }
    }
    for (const DungeonRoom& r : rooms_) carveRoom(r);
    bossRoom_ = 1;

    // The entrance corridor from the gate into the first room.
    for (int j = 0; j <= rooms_[0].z0; ++j)
        for (int i = gx - 2; i < gx + 2; ++i) set(i, j, Floor);

    // Corridors: each room joins the nearest room already connected, starting from
    // the entrance; the boss's hall is joined last and by a wider passage.
    std::vector<bool> linked(rooms_.size(), false);
    linked[0] = true;
    auto dist2 = [](const DungeonRoom& a, const DungeonRoom& b) { const int dx = a.cx() - b.cx(), dz = a.cz() - b.cz(); return dx * dx + dz * dz; };
    for (size_t n = 1; n < rooms_.size(); ++n) {
        // Pick the unlinked, non-boss room nearest to any linked room; the boss last.
        int best = -1, bestFrom = -1, bestD = 1 << 30;
        for (size_t a = 0; a < rooms_.size(); ++a) {
            if (linked[a] || (rooms_[a].boss && n + 1 < rooms_.size())) continue;
            for (size_t b = 0; b < rooms_.size(); ++b) {
                if (!linked[b]) continue;
                const int d = dist2(rooms_[a], rooms_[b]);
                if (d < bestD) { bestD = d; best = static_cast<int>(a); bestFrom = static_cast<int>(b); }
            }
        }
        if (best < 0) break;
        const DungeonRoom& a = rooms_[static_cast<size_t>(best)];
        const DungeonRoom& b = rooms_[static_cast<size_t>(bestFrom)];
        carveCorridor(b.cx(), b.cz(), a.cx(), a.cz(), a.boss ? 3 : 2);
        linked[static_cast<size_t>(best)] = true;
    }
    // A loop or two so there is more than one way round.
    if (rooms_.size() > 3) {
        const int loops = 1 + (L >= 5 ? 1 : 0);
        for (int k = 0; k < loops; ++k) {
            const size_t a = 2 + static_cast<size_t>(u(rng) * static_cast<float>(rooms_.size() - 2)) % (rooms_.size() - 2);
            const size_t b = 2 + static_cast<size_t>(u(rng) * static_cast<float>(rooms_.size() - 2)) % (rooms_.size() - 2);
            if (a != b) carveCorridor(rooms_[a].cx(), rooms_[a].cz(), rooms_[b].cx(), rooms_[b].cz(), 2);
        }
    }
    // Nothing may touch the outer ring (it has to be rock so every face is closed).
    for (int i = 0; i < w_; ++i) { set(i, d_ - 1, Rock); }
    for (int j = 0; j < d_; ++j) { set(0, j, Rock); set(w_ - 1, j, Rock); }
    // The gate row: only the passage is open.
    for (int i = 0; i < w_; ++i) if (i < gx - 2 || i >= gx + 2) set(i, 0, Rock);

    // Walls are the rock tiles that border floor (8-neighbourhood, so corners close).
    for (int j = 0; j < d_; ++j)
        for (int i = 0; i < w_; ++i) {
            if (tile(i, j) != Rock) continue;
            bool border = false;
            for (int dj = -1; dj <= 1 && !border; ++dj)
                for (int di = -1; di <= 1 && !border; ++di) border = tile(i + di, j + dj) == Floor;
            if (border) set(i, j, Wall);
        }

    // Torches on room walls every few tiles, and along the corridors now and then.
    auto torchAt = [&](int i, int j, int ni, int nj) {   // wall tile (i,j), floor neighbour direction (ni,nj)
        glm::vec3 c = tileCentre(i, j);
        glm::vec3 n(static_cast<float>(ni), 0.f, -static_cast<float>(nj));
        torches_.push_back({c + n * 0.62f, n});
    };
    for (const DungeonRoom& r : rooms_) {
        const int step = r.boss ? 4 : 3;
        for (int i = r.x0 + 1; i < r.x0 + r.w; i += step) {
            if (tile(i, r.z0 - 1) == Wall) torchAt(i, r.z0 - 1, 0, 1);
            if (tile(i, r.z0 + r.d) == Wall) torchAt(i, r.z0 + r.d, 0, -1);
        }
        for (int j = r.z0 + 1; j < r.z0 + r.d; j += step) {
            if (tile(r.x0 - 1, j) == Wall) torchAt(r.x0 - 1, j, 1, 0);
            if (tile(r.x0 + r.w, j) == Wall) torchAt(r.x0 + r.w, j, -1, 0);
        }
    }
    int every = 0;
    for (int j = 1; j < d_ - 1; ++j)
        for (int i = 1; i < w_ - 1; ++i) {
            if (tile(i, j) != Floor) continue;
            bool inRoom = false;
            for (const DungeonRoom& r : rooms_) inRoom = inRoom || r.contains(i, j);
            if (inRoom) continue;
            if (++every % 9 != 0) continue;
            if (tile(i - 1, j) == Wall) torchAt(i - 1, j, 1, 0);
            else if (tile(i, j - 1) == Wall) torchAt(i, j - 1, 0, 1);
        }
}

std::vector<glm::vec3> Dungeon::spawnSpots(std::mt19937& rng, int count) const {
    std::vector<glm::vec3> out;
    if (rooms_.size() < 3 || count <= 0) return out;
    std::uniform_real_distribution<float> u(0.f, 1.f);
    // Round-robin over the ordinary rooms so every room gets its share, corridors a few.
    std::vector<size_t> order;
    for (size_t k = 2; k < rooms_.size(); ++k) order.push_back(k);
    for (int n = 0; n < count && !order.empty(); ++n) {
        const DungeonRoom& r = rooms_[order[static_cast<size_t>(n) % order.size()]];
        for (int attempt = 0; attempt < 20; ++attempt) {
            const int i = r.x0 + 1 + static_cast<int>(u(rng) * static_cast<float>(std::max(1, r.w - 2)));
            const int j = r.z0 + 1 + static_cast<int>(u(rng) * static_cast<float>(std::max(1, r.d - 2)));
            if (tile(i, j) != Floor) continue;
            glm::vec3 p = tileCentre(i, j);
            bool close = false;
            for (const glm::vec3& q : out) close = close || glm::length(q - p) < 1.6f;
            if (close) continue;
            out.push_back(p);
            break;
        }
    }
    return out;
}

std::vector<glm::vec3> Dungeon::bossGuardSpots(std::mt19937& rng, int count) const {
    std::vector<glm::vec3> out;
    if (rooms_.empty()) return out;
    const DungeonRoom& r = bossRoom();
    std::uniform_real_distribution<float> u(0.f, 1.f);
    for (int n = 0; n < count; ++n)
        for (int attempt = 0; attempt < 20; ++attempt) {
            const int i = r.x0 + 1 + static_cast<int>(u(rng) * static_cast<float>(std::max(1, r.w - 2)));
            const int j = r.z0 + 1 + static_cast<int>(u(rng) * static_cast<float>(std::max(1, r.d - 2)));
            if (tile(i, j) != Floor) continue;
            if (std::abs(i - r.cx()) < 4 && std::abs(j - r.cz()) < 4) continue;   // leave the middle to the boss
            out.push_back(tileCentre(i, j));
            break;
        }
    return out;
}

}  // namespace rl::game
