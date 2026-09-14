#pragma once
// First-person phase: the red row(s) become monsters embedded in the block
// wall. Pure simulation; the App turns this state into draw calls.
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "core/tetris.h"

namespace rl::game {

// World mapping shared by both modes: cell (col,row) centre.
inline glm::vec3 cellCentre(int col, int row) {
    return {static_cast<float>(col) - core::kBoardW * 0.5f + 0.5f, static_cast<float>(core::kBoardH - 1 - row) + 0.5f, 0.f};
}

struct Enemy {
    enum class State { Emerging, Idle, Attack, Pain, Dying, Dead };
    int col = 0, row = 0;
    glm::vec3 pos{0.f};       // feet position (sprite origin)
    glm::vec3 home{0.f};
    float hp = 60.f;
    State state = State::Emerging;
    float stateT = 0.f;
    float attackTimer = 2.f;
    float animT = 0.f;
    bool exploded = false;
    float bobPhase = 0.f;
    float radius = 0.45f;     // cylinder radius for hit tests
    float height = 1.6f;
};

struct Projectile {
    glm::vec3 pos, vel;
    float ttl = 4.f;
    float animT = 0.f;
};

struct Explosion {
    glm::vec3 pos;
    float t = 0.f;
    float duration = 0.45f;
    float radius = 1.5f;
};

struct Debris {
    glm::vec3 pos, vel;
    glm::vec3 color;
    float ttl = 1.5f;
    float size = 0.2f;
    bool red = false;
};

struct FpsInput {
    float moveX = 0.f, moveZ = 0.f;   // -1..1 strafe / forward
    float lookDX = 0.f, lookDY = 0.f; // mouse delta (pixels)
    bool fire = false;
};

struct FpsEvent {
    enum class Type { Shoot, EnemyHit, EnemyDied, Explosion, PlayerHit, FireballLaunched, FireballHit, AllClear, PlayerDead, EnemySight } type;
    glm::vec3 pos{0.f};
    int a = 0;
};

class FpsMode {
public:
    FpsMode();
    void begin(core::Game& game, const std::vector<int>& rows);
    void update(float dt, const FpsInput& in, core::Game& game);
    bool finished() const { return finished_; }
    bool playerDead() const { return health_ <= 0.f; }

    // Player
    glm::vec3 eye() const { return playerPos_ + glm::vec3(0.f, 1.6f, 0.f); }
    glm::vec3 forward() const;
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    float health() const { return health_; }
    float damageFlash() const { return damageFlash_; }
    float gunAnimT() const { return gunT_; }
    bool gunFiring() const { return gunT_ < gunCycle_; }
    float recoil() const;
    int enemiesLeft() const;
    int totalEnemies() const { return static_cast<int>(enemies_.size()); }
    float elapsed() const { return elapsed_; }

    const std::vector<Enemy>& enemies() const { return enemies_; }
    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    const std::vector<Explosion>& explosions() const { return explosions_; }
    const std::vector<Debris>& debris() const { return debris_; }
    std::vector<FpsEvent> drainEvents();

    // Arena bounds (also used by the App for geometry).
    static constexpr float kArenaHalfW = 14.f;
    static constexpr float kArenaDepth = 22.f;   // +Z extent in front of the wall
    static constexpr float kPlayerStartZ = 11.f;

    void spawnDebris(const glm::vec3& pos, const glm::vec3& color, int count, bool red);

private:
    void fire(core::Game& game);
    void killEnemy(Enemy& e, core::Game& game);
    void push(FpsEvent::Type t, glm::vec3 p = {}, int a = 0) { events_.push_back({t, p, a}); }

    std::mt19937 rng_;
    std::vector<Enemy> enemies_;
    std::vector<Projectile> projectiles_;
    std::vector<Explosion> explosions_;
    std::vector<Debris> debris_;
    std::vector<FpsEvent> events_;
    glm::vec3 playerPos_{0.f, 0.f, kPlayerStartZ};
    float yaw_ = 3.14159265f;   // facing -Z (towards the wall)
    float pitch_ = 0.f;
    float health_ = 100.f;
    float damageFlash_ = 0.f;
    float gunT_ = 10.f;
    float gunCycle_ = 0.75f;
    float elapsed_ = 0.f;
    bool finished_ = false;
    float finishDelay_ = 0.f;
};

}  // namespace rl::game
