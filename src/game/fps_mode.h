#pragma once
// First-person phase. The board has tipped over onto the floor: every cell is
// a 1 m cube standing on the ground, the player walks between them, and each
// connected region of red cells has become one monster (bigger region, nastier
// monster). Pure simulation; the App turns this state into draw calls.
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "core/tetris.h"

namespace rl::game {

// Flat board mapping: cell (col,row) -> floor position of the cell centre.
// Row 19 (bottom of the stack) sits at z = 0.5, row 0 at z = 19.5. The player
// starts in the empty region at high z, facing -Z towards the stack.
inline glm::vec3 flatCellFloor(int col, int row) {
    return {static_cast<float>(col) - core::kBoardW * 0.5f + 0.5f, 0.f, static_cast<float>(core::kBoardH - 1 - row) + 0.5f};
}
inline glm::vec3 flatCellCentre(int col, int row) { return flatCellFloor(col, row) + glm::vec3(0.f, 0.5f, 0.f); }
inline bool flatToCell(glm::vec3 p, int& col, int& row) {
    col = static_cast<int>(std::floor(p.x + core::kBoardW * 0.5f));
    row = core::kBoardH - 1 - static_cast<int>(std::floor(p.z));
    return col >= 0 && col < core::kBoardW && row >= 0 && row < core::kBoardH;
}

enum class AttackKind { Hitscan, Projectile, Melee };

struct EnemyStats {
    const char* name;
    float hp;
    float radius, height;
    AttackKind attack;
    int projectile;        // index into Assets::projectile (Projectile attacks)
    float speed;           // m/s, 0 = stationary
    bool flies;            // ignores block collision, hovers
    float attackInterval;  // seconds between attacks (before level scaling)
    float damage;
    float projSpeed;
    int scoreValue;
};
const EnemyStats& enemyStats(int tier);

struct Enemy {
    enum class State { Emerging, Idle, Attack, Pain, Dying, Dead };
    int tier = 1;
    std::vector<std::pair<int, int>> cells;   // the red region it came from
    glm::vec3 pos{0.f};      // feet
    float hp = 60.f, maxHp = 60.f;
    State state = State::Emerging;
    float stateT = 0.f;
    float attackTimer = 2.f;
    float animT = 0.f;
    bool exploded = false;
    bool attacked = false;   // this attack cycle already fired
    float bobPhase = 0.f;
    float radius = 0.45f;
    float height = 1.6f;
    float flashT = 0.f;      // muzzle flash light (hitscan)
    bool alive() const { return state != State::Dead && state != State::Dying; }
};

struct Projectile {
    glm::vec3 pos, vel;
    int type = 0;
    float damage = 10.f;
    float ttl = 5.f;
    float animT = 0.f;
};

struct Explosion {
    glm::vec3 pos;
    float t = 0.f;
    float duration = 0.45f;
    float radius = 1.5f;
    int hitType = -1;   // -1 = big blast sprite, else projectileHit[type]
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
    enum class Type { Shoot, EnemyHit, EnemyDied, EnemyAttack, Explosion, PlayerHit, FireballHit, AllClear, PlayerDead, EnemySight } type;
    glm::vec3 pos{0.f};
    int tier = 0;
    int a = 0;
};

class FpsMode {
public:
    FpsMode();
    // Turns every red region of the board into a monster (removing those red
    // cells from the grid) and places the player in the empty part of the board.
    void begin(core::Game& game, int level);
    void update(float dt, const FpsInput& in, core::Game& game);
    bool finished() const { return finished_; }
    bool playerDead() const { return health_ <= 0.f; }

    // Player
    static constexpr float kEyeHeight = 1.25f;  // blocks are chest-high walls: see over them, hide behind them
    glm::vec3 eye() const { return playerPos_ + glm::vec3(0.f, kEyeHeight, 0.f); }
    glm::vec3 playerPos() const { return playerPos_; }
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
    int level() const { return level_; }

    const std::vector<Enemy>& enemies() const { return enemies_; }
    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    const std::vector<Explosion>& explosions() const { return explosions_; }
    const std::vector<Debris>& debris() const { return debris_; }
    std::vector<FpsEvent> drainEvents();

    // Solid cell test in flat coordinates (blocks only; enemies are not solid).
    bool solidAt(glm::vec3 p, const core::Game& game) const;
    // First distance along the ray at which a block is hit (or maxT).
    float rayBlockDistance(glm::vec3 o, glm::vec3 d, float maxT, const core::Game& game) const;

    void spawnDebris(const glm::vec3& pos, const glm::vec3& color, int count, bool red);

private:
    void fire(core::Game& game);
    void explodeEnemy(Enemy& e, core::Game& game);
    void moveWithCollision(glm::vec3& pos, glm::vec3 delta, float radius, const core::Game& game) const;
    bool lineOfSight(glm::vec3 a, glm::vec3 b, const core::Game& game) const;
    void hurtPlayer(float dmg, glm::vec3 from);
    void push(FpsEvent::Type t, glm::vec3 p = {}, int tier = 0, int a = 0) { events_.push_back({t, p, tier, a}); }

    std::mt19937 rng_;
    std::vector<Enemy> enemies_;
    std::vector<Projectile> projectiles_;
    std::vector<Explosion> explosions_;
    std::vector<Debris> debris_;
    std::vector<FpsEvent> events_;
    glm::vec3 playerPos_{0.f, 0.f, 18.5f};
    float yaw_ = 3.14159265f;   // facing -Z (towards the stack)
    float pitch_ = 0.f;
    float health_ = 100.f;
    float damageFlash_ = 0.f;
    float gunT_ = 10.f;
    float gunCycle_ = 0.75f;
    float elapsed_ = 0.f;
    int level_ = 1;
    bool finished_ = false;
    float finishDelay_ = 0.f;
};

}  // namespace rl::game
