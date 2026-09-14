#pragma once
// First-person phase. The board has tipped over onto the floor: every cell is
// a 1 m cube standing on the ground, the player walks between them, and each
// connected region of red cells has become one monster (bigger region, nastier
// monster). Dying monsters drop health, ammo and weapons. Pure simulation; the
// App turns this state into draw calls.
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

// ---------------------------------------------------------------------------
enum class AttackKind { Hitscan, Projectile, Melee };

struct EnemyStats {
    const char* name;
    float hp;
    float radius, height;
    AttackKind attack;
    int projectile;        // projectile type (Projectile attacks)
    float speed;           // m/s, 0 = stationary
    bool flies;            // ignores block collision, hovers
    float attackInterval;  // seconds between attacks (before level scaling)
    float damage;
    float projSpeed;
    int scoreValue;
    float breakInterval;   // seconds between block-destroying acts (cover erodes faster for big monsters)
    int breakRadius;       // 0 = one block, 1 = the block and its neighbours
};
const EnemyStats& enemyStats(int tier);

// Projectile types index Assets::projectile / projectileHit.
enum ProjectileType { kProjImp = 0, kProjCaco = 1, kProjBaron = 2, kProjRocket = 3, kProjPlasma = 4, kProjTypeCount = 5 };

// ---------------------------------------------------------------------------
enum WeaponId { kShotgun = 0, kChaingun = 1, kRocketLauncher = 2, kPlasmaRifle = 3, kWeaponCount = 4 };

struct WeaponDef {
    const char* name;
    float cycle;          // seconds between shots
    float damage;
    bool projectile;
    int projType;
    float projSpeed;
    float blast;          // splash radius (m), 0 = none
    int ammoPerPickup;    // 0 = infinite
    int maxAmmo;
    float spread;         // radians of random cone
};
const WeaponDef& weaponDef(int id);

struct WeaponSlot {
    bool owned = false;
    int ammo = 0;
};

enum class PickupKind { Stim = 0, Medikit, Bullets, Rockets, Cells, Chaingun, RocketLauncher, PlasmaGun, Count };
constexpr int kPickupKinds = static_cast<int>(PickupKind::Count);

struct Pickup {
    PickupKind kind;
    glm::vec3 pos, vel;
    glm::vec3 home;       // a known-open spot (the dead monster's pocket)
    float t = 0.f;
    bool landed = false;
};

// ---------------------------------------------------------------------------
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
    float breakTimer = 5.f;  // next block-destroying act
    float absorbTimer = 20.f;// when it hits zero the monster may absorb nearby blocks and grow a tier
    float ammoDropCooldown = 0.f;
    float growT = 0.f;       // visual flash after growing
    bool alive() const { return state != State::Dead && state != State::Dying; }
};

struct Projectile {
    glm::vec3 pos, vel;
    int type = 0;
    float damage = 10.f;
    float blast = 0.f;       // splash radius (player rockets)
    bool fromPlayer = false;
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
    bool homing = false;     // flies to `target` instead of falling (absorbed blocks)
    glm::vec3 target{0.f};
};

struct FpsInput {
    float moveX = 0.f, moveZ = 0.f;   // -1..1 strafe / forward
    float lookDX = 0.f, lookDY = 0.f; // mouse delta (pixels)
    bool fire = false;
    int selectWeapon = -1;            // 0..3 to switch, -1 none
    int wheel = 0;                    // +1 next / -1 previous weapon
    bool warmup = false;              // countdown: look around only, monsters rise but do nothing
};

struct FpsEvent {
    enum class Type { Shoot, EnemyHit, EnemyDied, EnemyAttack, Explosion, PlayerHit, FireballHit, AllClear, PlayerDead, EnemySight,
                      Pickup, WeaponSwitch, RocketBlast, PlasmaHit, BlockBroken, Absorb } type;
    glm::vec3 pos{0.f};
    int tier = 0;
    int a = 0;   // weapon id (Shoot/WeaponSwitch), pickup kind (Pickup), blocks destroyed (Explosion/RocketBlast)
};

class FpsMode {
public:
    FpsMode();
    // Turns every red region of the board into a monster (removing those red
    // cells from the grid) and places the player in the empty part of the board.
    void begin(core::Game& game, int level);
    // Tuning knobs (defaults are the shipped values; the CLI can override for testing).
    void setAbsorbPeriod(float seconds) { absorbPeriod_ = seconds; }
    void setGodMode(bool on) { god_ = on; }
    float absorbPeriod() const { return absorbPeriod_; }
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
    float pickupFlash() const { return pickupFlash_; }
    float gunAnimT() const { return gunT_; }
    bool gunFiring() const { return gunT_ < weaponDef(weapon_).cycle * 1.2f && gunT_ < 0.6f; }
    float recoil() const;
    int currentWeapon() const { return weapon_; }
    const WeaponSlot& weapon(int id) const { return slots_[id]; }
    void selectWeapon(int id);
    int enemiesLeft() const;
    int totalEnemies() const { return static_cast<int>(enemies_.size()); }
    float elapsed() const { return elapsed_; }
    int level() const { return level_; }

    const std::vector<Enemy>& enemies() const { return enemies_; }
    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    const std::vector<Explosion>& explosions() const { return explosions_; }
    const std::vector<Debris>& debris() const { return debris_; }
    const std::vector<Pickup>& pickups() const { return pickups_; }
    std::vector<FpsEvent> drainEvents();

    // Solid cell test in flat coordinates (blocks only; enemies are not solid).
    bool solidAt(glm::vec3 p, const core::Game& game) const;
    // First distance along the ray at which a block is hit (or maxT).
    float rayBlockDistance(glm::vec3 o, glm::vec3 d, float maxT, const core::Game& game) const;

    void spawnDebris(const glm::vec3& pos, const glm::vec3& color, int count, bool red);

private:
    void fire(core::Game& game);
    void hitscan(glm::vec3 o, glm::vec3 d, float damage, core::Game& game);
    void damageEnemy(Enemy& e, float dmg, glm::vec3 hitPos);
    void explodeEnemy(Enemy& e, core::Game& game);
    void dropLoot(const Enemy& e);
    void spawnPickup(PickupKind kind, glm::vec3 from, glm::vec3 home);
    void applyPickup(const Pickup& p);
    void breakBlock(Enemy& e, core::Game& game);
    void tryAbsorb(Enemy& e, core::Game& game);
    void breakCell(int c, int r, core::Game& game);
    void rocketBlast(glm::vec3 pos, float radius, float damage, core::Game& game);
    void moveWithCollision(glm::vec3& pos, glm::vec3 delta, float radius, const core::Game& game) const;
    bool lineOfSight(glm::vec3 a, glm::vec3 b, const core::Game& game) const;
    void hurtPlayer(float dmg, glm::vec3 from);
    void push(FpsEvent::Type t, glm::vec3 p = {}, int tier = 0, int a = 0) { events_.push_back({t, p, tier, a}); }

    std::mt19937 rng_;
    std::vector<Enemy> enemies_;
    std::vector<Projectile> projectiles_;
    std::vector<Explosion> explosions_;
    std::vector<Debris> debris_;
    std::vector<Pickup> pickups_;
    std::vector<FpsEvent> events_;
    glm::vec3 playerPos_{0.f, 0.f, 18.5f};
    float yaw_ = 3.14159265f;   // facing -Z (towards the stack)
    float pitch_ = 0.f;
    float health_ = 100.f;
    float damageFlash_ = 0.f;
    float pickupFlash_ = 0.f;
    float gunT_ = 10.f;
    int weapon_ = kShotgun;
    WeaponSlot slots_[kWeaponCount];
    float elapsed_ = 0.f;
    int level_ = 1;
    bool finished_ = false;
    float finishDelay_ = 0.f;
    float absorbPeriod_ = 20.f;
    bool god_ = false;
};

}  // namespace rl::game
