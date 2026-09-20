#include "game/dungeon.h"

#include "core/tetris.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    if (t == Floor || t == Door) return (!rooms_.empty() && bossRoom().contains(i, j)) ? kHallHeight : kRoomHeight;
    int h = 0;
    for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di)
            if (tile(i + di, j + dj) == Floor || tile(i + di, j + dj) == Door) h = std::max(h, ceilingAt(i + di, j + dj));
    return h;
}

float Dungeon::ceilingAt(float x, float z) const {
    int i, j;
    if (!worldToTile(x, z, i, j)) return 0.f;
    return static_cast<float>(ceilingAt(i, j));
}

namespace {
// A room shaped like a piece: each of the four minos becomes a `cell` x `cell`
// block of tiles, so an S piece gives a staggered hall and an I piece a long one.
// The blocks overlap by one tile so the room is a single connected floor.
void pieceFootprint(int shape, int rot, int cell, std::vector<std::pair<int, int>>& out, int& w, int& d) {
    out.clear();
    const auto& minos = core::shapeCells(static_cast<core::Shape>(shape), rot);
    int minX = 9, minY = 9, maxX = -9, maxY = -9;
    for (auto [mx, my] : minos) { minX = std::min(minX, mx); minY = std::min(minY, my); maxX = std::max(maxX, mx); maxY = std::max(maxY, my); }
    w = (maxX - minX + 1) * cell;
    d = (maxY - minY + 1) * cell;
    for (auto [mx, my] : minos)
        for (int y = 0; y < cell; ++y)
            for (int x = 0; x < cell; ++x) out.push_back({(mx - minX) * cell + x, (my - minY) * cell + y});
}
}  // namespace

void Dungeon::carveRoom(const DungeonRoom& r) {
    if (r.shape < 0) {
        for (int j = r.z0; j < r.z0 + r.d; ++j)
            for (int i = r.x0; i < r.x0 + r.w; ++i) set(i, j, Floor);
        return;
    }
    // The bounding box was sized from the same footprint, so this fills within it.
    std::vector<std::pair<int, int>> tiles;
    int w = 0, d = 0;
    pieceFootprint(r.shape, r.rot, r.cell, tiles, w, d);
    for (auto [x, y] : tiles) set(r.x0 + x, r.z0 + y, Floor);
}

// Pillars: blocks of rock left standing inside a room. A pillar can never cut a
// room in two as long as it keeps a clear ring, so this is always safe. The boss
// hall always gets them, because a cyberdemon in a bare box is a shooting gallery.
void Dungeon::decorate(const DungeonRoom& r, std::mt19937& rng) {
    std::uniform_real_distribution<float> u(0.f, 1.f);
    const bool hall = r.boss;
    if (r.entrance) return;
    if (!hall && (r.w < 6 || r.d < 6 || u(rng) < 0.5f)) return;
    const int inset = hall ? 3 : 2, step = hall ? 5 : 3, size = hall ? 2 : 1;
    for (int j = r.z0 + inset; j + size <= r.z0 + r.d - inset; j += step)
        for (int i = r.x0 + inset; i + size <= r.x0 + r.w - inset; i += step) {
            // The middle of the hall stays clear: the boss stands there.
            if (hall && std::abs(i - r.cx()) <= 3 && std::abs(j - r.cz()) <= 3) continue;
            bool open = true;
            for (int dj = -1; dj <= size && open; ++dj)
                for (int di = -1; di <= size && open; ++di) open = tile(i + di, j + dj) == Floor;
            if (!open) continue;
            for (int dj = 0; dj < size; ++dj)
                for (int di = 0; di < size; ++di) set(i + di, j + dj, Rock);
        }
}

// The boss hall is sealed: every floor tile just outside it that leads in becomes a
// door, solid until the player finds the key.
void Dungeon::sealBossRoom() {
    doorTiles_.clear();
    if (rooms_.size() < 2) return;
    const DungeonRoom& h = bossRoom();
    for (int j = h.z0 - 1; j <= h.z0 + h.d; ++j)
        for (int i = h.x0 - 1; i <= h.x0 + h.w; ++i) {
            const bool ring = (i == h.x0 - 1 || i == h.x0 + h.w || j == h.z0 - 1 || j == h.z0 + h.d);
            if (!ring || tile(i, j) != Floor) continue;
            bool leadsIn = false;
            const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; ++k) {
                const int ni = i + di[k], nj = j + dj[k];
                if (h.contains(ni, nj) && tile(ni, nj) == Floor) leadsIn = true;
            }
            if (leadsIn) { set(i, j, Door); doorTiles_.push_back({i, j}); }
        }
}

// The floor tile of `r` nearest (i, j), so corridors and items start on real floor.
void Dungeon::nearestFloor(const DungeonRoom& r, int& i, int& j) const {
    if (tile(i, j) == Floor) return;
    int bi = i, bj = j, best = 1 << 30;
    for (int y = r.z0; y < r.z0 + r.d; ++y)
        for (int x = r.x0; x < r.x0 + r.w; ++x) {
            if (tile(x, y) != Floor) continue;
            const int dd = (x - i) * (x - i) + (y - j) * (y - j);
            if (dd < best) { best = dd; bi = x; bj = y; }
        }
    i = bi; j = bj;
}

glm::vec3 Dungeon::bossStand() const {
    if (rooms_.size() < 2) return {0.f, 0.f, kZTop - 1.f};
    const DungeonRoom& h = bossRoom();
    int i = h.cx(), j = h.cz();
    nearestFloor(h, i, j);
    return tileCentre(i, j);
}

// Every tile takes the theme of the room whose box it falls in, or the corridor
// set outside them; a wall takes the theme of the floor it faces.
void Dungeon::assignThemes() {
    themes_.assign(static_cast<size_t>(w_ * d_), static_cast<uint8_t>(kCorridorTheme));
    for (const DungeonRoom& r : rooms_)
        for (int j = r.z0; j < r.z0 + r.d; ++j)
            for (int i = r.x0; i < r.x0 + r.w; ++i)
                if (i >= 0 && i < w_ && j >= 0 && j < d_) themes_[static_cast<size_t>(j * w_ + i)] = static_cast<uint8_t>(r.theme);
    // Walls sit outside a room's box, so give each one the theme of a floor beside it.
    std::vector<uint8_t> copy = themes_;
    for (int j = 0; j < d_; ++j)
        for (int i = 0; i < w_; ++i) {
            if (tile(i, j) == Floor) continue;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di)
                    if (tile(i + di, j + dj) == Floor && i + di >= 0 && i + di < w_ && j + dj >= 0 && j + dj < d_)
                        copy[static_cast<size_t>(j * w_ + i)] = themes_[static_cast<size_t>((j + dj) * w_ + i + di)];
        }
    themes_ = copy;
}

// Scenery against the walls: columns and candles in the rooms, and the boss hall
// gets what a boss hall should have.
void Dungeon::placeDecor(std::mt19937& rng) {
    decor_.clear();
    std::uniform_real_distribution<float> u(0.f, 1.f);
    for (const DungeonRoom& r : rooms_) {
        if (r.entrance) continue;
        const int want = r.boss ? 10 : 2 + static_cast<int>(u(rng) * 3.f);
        for (int n = 0; n < want; ++n)
            for (int attempt = 0; attempt < 30; ++attempt) {
                const int i = r.x0 + 1 + static_cast<int>(u(rng) * static_cast<float>(std::max(1, r.w - 2)));
                const int j = r.z0 + 1 + static_cast<int>(u(rng) * static_cast<float>(std::max(1, r.d - 2)));
                if (tile(i, j) != Floor) continue;
                // Against something: a bare middle of the floor is where the fight happens.
                bool nextToWall = false;
                const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) nextToWall = nextToWall || tile(i + di[k], j + dj[k]) == Wall;
                if (!nextToWall) continue;
                glm::vec3 p = tileCentre(i, j);
                bool crowded = false;
                for (const DungeonDecor& d : decor_) crowded = crowded || glm::length(d.pos - p) < 2.2f;
                for (const DungeonTorch& t : torches_) crowded = crowded || glm::length(t.pos - p) < 1.6f;
                if (crowded) continue;
                DungeonDecor d;
                d.pos = p;
                d.variant = static_cast<int>(u(rng) * 3.f) % 3;
                const float roll = u(rng);
                if (r.boss) d.kind = roll < 0.45f ? DungeonDecor::Hanging : roll < 0.7f ? DungeonDecor::Skulls : roll < 0.85f ? DungeonDecor::Impaled : DungeonDecor::Column;
                else if (r.theme == 3) d.kind = roll < 0.4f ? DungeonDecor::Impaled : roll < 0.7f ? DungeonDecor::Stalagmite : DungeonDecor::Skulls;
                else d.kind = roll < 0.45f ? DungeonDecor::Column : roll < 0.8f ? DungeonDecor::Candle : DungeonDecor::Stalagmite;
                decor_.push_back(d);
                break;
            }
    }
    // A candle every so often down the corridors, so they are not bare.
    int every = 0;
    for (int j = 1; j < d_ - 1; ++j)
        for (int i = 1; i < w_ - 1; ++i) {
            if (tile(i, j) != Floor || themeAt(i, j) != kCorridorTheme) continue;
            bool nextToWall = false;
            const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; ++k) nextToWall = nextToWall || tile(i + di[k], j + dj[k]) == Wall;
            if (!nextToWall || ++every % 13 != 0) continue;
            decor_.push_back({tileCentre(i, j), DungeonDecor::Candle, 0});
        }
}

bool Dungeon::doorAt(float x, float z) const {
    int i, j;
    if (!worldToTile(x, z, i, j)) return false;
    return tile(i, j) == Door;
}

// An L-shaped corridor: along x first, then along z (the bend is where the two meet).
void Dungeon::carveCorridor(int x0, int z0, int x1, int z1, int width) {
    const int half = width / 2;
    for (int i = std::min(x0, x1); i <= std::max(x0, x1); ++i)
        for (int k = -half; k < width - half; ++k) set(i, z0 + k, Floor);
    for (int j = std::min(z0, z1); j <= std::max(z0, z1); ++j)
        for (int k = -half; k < width - half; ++k) set(x1 + k, j, Floor);
}

void Dungeon::generate(uint32_t seed, int level, const std::array<int, 7>& pieces) {
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
    decor_.clear();

    // The entrance room sits right behind the gate, centred on x = 0.
    const int gx = w_ / 2;   // tile column at x = 0
    {
        DungeonRoom r;
        r.w = 7 + static_cast<int>(u(rng) * 3.f);
        r.d = 6 + static_cast<int>(u(rng) * 3.f);
        r.x0 = gx - r.w / 2;
        r.z0 = 4;
        r.entrance = true;
        r.theme = kCorridorTheme;
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
        r.theme = kHallTheme;
        rooms_.push_back(r);
    }
    // Ordinary rooms take the shape of a piece, drawn from the ones the player
    // actually dropped, so a game full of S pieces gives a crypt of staggered halls.
    int total = 0;
    for (int c : pieces) total += c;
    auto pickShape = [&]() {
        if (total <= 0) return static_cast<int>(u(rng) * 7.f) % 7;
        // Weighted by the piece mix, but never so lopsided that one shape is the whole
        // crypt: every piece keeps a floor of one seventh of the weight.
        float weights[7], sum = 0.f;
        for (int k = 0; k < 7; ++k) { weights[k] = static_cast<float>(pieces[k]) / static_cast<float>(total) + 0.14f; sum += weights[k]; }
        float roll = u(rng) * sum;
        for (int k = 0; k < 7; ++k) { roll -= weights[k]; if (roll <= 0.f) return k; }
        return 6;
    };
    auto overlaps = [&](const DungeonRoom& a) {
        for (const DungeonRoom& b : rooms_)
            if (a.x0 < b.x0 + b.w + 2 && b.x0 < a.x0 + a.w + 2 && a.z0 < b.z0 + b.d + 2 && b.z0 < a.z0 + a.d + 2) return true;
        return false;
    };
    std::vector<std::pair<int, int>> tiles;
    for (int k = 1; k < nRooms; ++k) {
        for (int attempt = 0; attempt < 300; ++attempt) {
            DungeonRoom r;
            r.theme = static_cast<int>(u(rng) * 4.f) % 4;
            r.shape = pickShape();
            r.rot = static_cast<int>(u(rng) * 4.f) & 3;
            r.cell = 2 + static_cast<int>(u(rng) * 2.99f);   // 2..4 tiles per mino
            pieceFootprint(r.shape, r.rot, r.cell, tiles, r.w, r.d);
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
        // A piece-shaped room's bounding-box centre can sit in the notch of an L or a T,
        // so join from a floor tile of each room instead.
        int bx = b.cx(), bz = b.cz(), ax = a.cx(), az = a.cz();
        nearestFloor(b, bx, bz);
        nearestFloor(a, ax, az);
        carveCorridor(bx, bz, ax, az, a.boss ? 3 : 2);
        linked[static_cast<size_t>(best)] = true;
    }
    // A loop or two so there is more than one way round.
    if (rooms_.size() > 3) {
        const int loops = 1 + (L >= 5 ? 1 : 0);
        for (int k = 0; k < loops; ++k) {
            const size_t a = 2 + static_cast<size_t>(u(rng) * static_cast<float>(rooms_.size() - 2)) % (rooms_.size() - 2);
            const size_t b = 2 + static_cast<size_t>(u(rng) * static_cast<float>(rooms_.size() - 2)) % (rooms_.size() - 2);
            if (a != b) {
                int ax = rooms_[a].cx(), az = rooms_[a].cz(), bx = rooms_[b].cx(), bz = rooms_[b].cz();
                nearestFloor(rooms_[a], ax, az);
                nearestFloor(rooms_[b], bx, bz);
                carveCorridor(ax, az, bx, bz, 2);
            }
        }
    }
    // Nothing may touch the outer ring (it has to be rock so every face is closed).
    for (int i = 0; i < w_; ++i) { set(i, d_ - 1, Rock); }
    for (int j = 0; j < d_; ++j) { set(0, j, Rock); set(w_ - 1, j, Rock); }
    // The gate row: only the passage is open.
    for (int i = 0; i < w_; ++i) if (i < gx - 2 || i >= gx + 2) set(i, 0, Rock);

    // Pillars inside the bigger rooms, then the hall is sealed and the key is hidden
    // in the room furthest from it.
    for (const DungeonRoom& r : rooms_) decorate(r, rng);
    sealBossRoom();
    haveKey_ = false;
    if (rooms_.size() > 2 && !doorTiles_.empty()) {
        const DungeonRoom& hall = bossRoom();
        size_t best = 2;
        int bestD = -1;
        for (size_t k = 2; k < rooms_.size(); ++k) {
            const int dx = rooms_[k].cx() - hall.cx(), dz = rooms_[k].cz() - hall.cz();
            const int dd = dx * dx + dz * dz;
            if (dd > bestD) { bestD = dd; best = k; }
        }
        int kx = rooms_[best].cx(), kz = rooms_[best].cz();
        nearestFloor(rooms_[best], kx, kz);
        keyPos_ = tileCentre(kx, kz);
        haveKey_ = true;
    }

    // Walls are the rock tiles that border floor (8-neighbourhood, so corners close).
    for (int j = 0; j < d_; ++j)
        for (int i = 0; i < w_; ++i) {
            if (tile(i, j) != Rock) continue;
            bool border = false;
            for (int dj = -1; dj <= 1 && !border; ++dj)
                for (int di = -1; di <= 1 && !border; ++di) border = tile(i + di, j + dj) == Floor;
            if (border) set(i, j, Wall);
        }

    assignThemes();

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
    placeDecor(rng);
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
