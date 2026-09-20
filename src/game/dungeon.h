#pragma once
// The dungeon behind the arena's back wall. Once the arena is cleared the wall
// falls and a procedurally laid-out crypt opens up: rooms joined by corridors
// on a tile grid the way Diablo 2 builds its levels (rectangular rooms placed
// at random, each one linked to the nearest room already reachable, a loop or
// two for good measure), with a big room at the far end for the boss. Pure
// layout: the FpsMode populates it and the App draws it.
#include <random>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace rl::game {

struct DungeonRoom {
    int x0 = 0, z0 = 0, w = 0, d = 0;   // tile rectangle (inclusive x0..x0+w-1, z0..z0+d-1)
    bool entrance = false;
    bool boss = false;
    int cx() const { return x0 + w / 2; }
    int cz() const { return z0 + d / 2; }
    bool contains(int i, int j) const { return i >= x0 && i < x0 + w && j >= z0 && j < z0 + d; }
};

struct DungeonTorch {
    glm::vec3 pos;      // sprite feet, just off the wall face
    glm::vec3 normal;   // wall face normal (into the room)
};

class Dungeon {
public:
    enum Tile : uint8_t { Rock = 0, Floor = 1, Wall = 2 };   // Wall: rock that borders floor (the drawn faces)
    static constexpr int kRoomHeight = 4;   // rooms and corridors are four cubes high, the boss's hall six: a cyberdemon
    static constexpr int kHallHeight = 6;   // (3.3 m) and the mastermind stand up straight and the player can look up at them
    static constexpr float kWallHeight = 4.f;   // the ordinary ceiling height (solid above it)
    // The passage: the arena's back wall stands at z in [-3, -2]; the gate is the
    // section |x| < kGateHalf of it, and the board's end rail in front of it opens too.
    static constexpr float kGateHalf = 2.f;
    static constexpr float kZTop = -3.f;   // the dungeon's first tile row starts here and runs towards -z

    // Lays the dungeon out; bigger and busier with the level.
    void generate(uint32_t seed, int level);
    void clear() { tiles_.clear(); rooms_.clear(); torches_.clear(); }
    bool empty() const { return tiles_.empty(); }

    int width() const { return w_; }
    int depth() const { return d_; }
    Tile tile(int i, int j) const { return (i < 0 || i >= w_ || j < 0 || j >= d_) ? Rock : tiles_[static_cast<size_t>(j * w_ + i)]; }
    // Tile (i, j) covers x in [originX + i, originX + i + 1) and z in (kZTop - j - 1, kZTop - j].
    float originX() const { return -static_cast<float>(w_) * 0.5f; }
    float zMin() const { return kZTop - static_cast<float>(d_); }
    glm::vec3 tileCentre(int i, int j) const { return {originX() + static_cast<float>(i) + 0.5f, 0.f, kZTop - static_cast<float>(j) - 0.5f}; }
    bool worldToTile(float x, float z, int& i, int& j) const;
    bool floorAt(float x, float z) const;   // false outside the grid and on rock
    // Cubes of headroom over a tile: kHallHeight in the boss's hall, kRoomHeight elsewhere;
    // a wall tile is as tall as the tallest floor beside it, rock is 0.
    int ceilingAt(int i, int j) const;
    float ceilingAt(float x, float z) const;

    const std::vector<DungeonRoom>& rooms() const { return rooms_; }
    const DungeonRoom& bossRoom() const { return rooms_[bossRoom_]; }
    const DungeonRoom& entranceRoom() const { return rooms_[0]; }
    const std::vector<DungeonTorch>& torches() const { return torches_; }

    // Floor tiles in rooms other than the entrance and the boss room, spread about; `count` at most.
    std::vector<glm::vec3> spawnSpots(std::mt19937& rng, int count) const;
    // Floor tiles inside the boss room away from its centre (its guards).
    std::vector<glm::vec3> bossGuardSpots(std::mt19937& rng, int count) const;

private:
    void carveRoom(const DungeonRoom& r);
    void carveCorridor(int x0, int z0, int x1, int z1, int width);
    void set(int i, int j, Tile t) { if (i >= 0 && i < w_ && j >= 0 && j < d_) tiles_[static_cast<size_t>(j * w_ + i)] = t; }
    int w_ = 0, d_ = 0;
    int bossRoom_ = 0;
    std::vector<Tile> tiles_;
    std::vector<DungeonRoom> rooms_;
    std::vector<DungeonTorch> torches_;
};

}  // namespace rl::game
