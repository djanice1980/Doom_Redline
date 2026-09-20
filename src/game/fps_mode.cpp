#include "game/fps_mode.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <string>

namespace rl::game {

namespace {
constexpr float kPi = 3.14159265f;
constexpr float kMoveSpeed = 5.0f;
// Doom's A_Chase turns 45 degrees every few tics (a few hundred degrees a second) towards the way
// it is going; A_FaceTarget snaps to the target when it attacks.
constexpr float kChaseTurnRate = 2.f * kPi;     // rad/s while walking
constexpr float kStandTurnRate = kPi;           // rad/s while standing in range: slower, so it can be flanked

float wrapAngle(float a) { while (a > kPi) a -= 2.f * kPi; while (a < -kPi) a += 2.f * kPi; return a; }
void turnTowards(float& yaw, float target, float maxStep) {
    const float d = wrapAngle(target - yaw);
    yaw = wrapAngle(yaw + std::clamp(d, -maxStep, maxStep));
}
constexpr float kRunMultiplier = 1.75f;
constexpr int kBossTier = 5;   // cyberdemon and up: at most one alive at a time
constexpr float kMouseSens = 0.0022f;
constexpr float kPlayerRadius = 0.3f;
constexpr float kExplosionRadius = 1.5f;   // cells, around every cell of the region
constexpr float kBlockTop = 1.0f;          // blocks are unit cubes on the floor

//               name          hp     r      h     attack                 proj         speed flies interval dmg   pspd  score break r  blast
const EnemyStats kStats[] = {
    {"ZOMBIE",      20.f, 0.35f, 1.5f, AttackKind::Hitscan,    -1,          0.f,  false, 2.2f, 5.f,  0.f,  100,  11.f, 0, 0.f},
    {"IMP",         60.f, 0.40f, 1.7f, AttackKind::Projectile, kProjImp,    0.f,  false, 2.6f, 10.f, 8.f,  200,  7.5f, 0, 0.f},
    {"DEMON",      150.f, 0.50f, 1.6f, AttackKind::Melee,      -1,          2.4f, false, 1.0f, 14.f, 0.f,  350,  5.f,  0, 0.f},   // pinky: a third slower than before, the arena is small
    {"CACODEMON",  400.f, 0.65f, 1.8f, AttackKind::Projectile, kProjCaco,   1.8f, true,  2.4f, 16.f, 10.f, 600,  3.5f, 0, 0.f},
    {"BARON",     1000.f, 0.70f, 2.3f, AttackKind::Projectile, kProjBaron,  1.2f, false, 2.8f, 28.f, 10.f, 1200, 2.5f, 1, 0.f},
    {"CYBERDEMON",2200.f, 0.90f, 3.3f, AttackKind::Projectile, kProjRocket, 1.6f, false, 3.0f, 40.f, 14.f, 2500, 2.0f, 1, 2.0f},
    {"SPIDER",    2600.f, 1.20f, 3.0f, AttackKind::Hitscan,    -1,          1.4f, false, 0.9f, 8.f,  0.f,  3000, 1.5f, 1, 0.f},
    // Doom 2 variants (kinds 7-11): stand in for a tier with their own behaviour.
    {"CHAINGUNNER",  70.f, 0.40f, 1.7f, AttackKind::Hitscan,    -1,           0.f,  false, 1.1f, 6.f,  0.f,  250,  7.5f, 0, 0.f},
    {"HELL KNIGHT", 500.f, 0.70f, 2.3f, AttackKind::Projectile, kProjBaron,   1.3f, false, 2.6f, 22.f, 10.f, 800,  3.0f, 1, 0.f},
    {"REVENANT",    300.f, 0.50f, 2.2f, AttackKind::Projectile, kProjRevenant,1.6f, false, 2.8f, 18.f, 7.f,  700,  4.0f, 0, 0.f, 1, true},
    {"MANCUBUS",    600.f, 0.85f, 2.0f, AttackKind::Projectile, kProjMancubus,0.9f, false, 3.0f, 16.f, 10.f, 900,  3.0f, 1, 0.f, 3, false},
    {"ARACHNOTRON", 500.f, 0.90f, 1.6f, AttackKind::Projectile, kProjArach,   1.1f, false, 0.35f, 8.f, 14.f, 900,  4.0f, 1, 0.f},
    {"ARCH-VILE",   700.f, 0.45f, 2.2f, AttackKind::Vile,       -1,           2.0f, false, 4.0f, 20.f, 0.f,  1100, 3.0f, 1, 0.f, 1, false, 0.12f},   // the flame attack; hardly ever staggers
};
// The arch-vile's attack, from Doom's state table: arms go up and the scream plays (A_VileStart),
// the flame appears on the target a third of a second later (A_VileTarget) and follows them
// while the vile can see them (A_Fire), the hands clasp about 2.4 s in (A_VileAttack): 20
// direct, then the flame's blast for 70 that throws the target upward. No line of sight at
// that moment and nothing happens at all. Range 896 map units, about 28 m.
constexpr float kVileFireDelay = 0.3f;
constexpr float kVileWindup = 2.4f;
constexpr float kVileHold = 0.6f;      // the clasped frame before it goes back to chasing
constexpr float kVileRange = 28.f;
constexpr float kVileBlastRadius = 1.8f;
constexpr float kVileBlastDamage = 70.f;
constexpr float kVileJump = 5.5f;      // m/s upward, about 1.5 m of air

float absorbPeriodForLevel(float base, int level) { return std::max(8.f, base - 1.f * static_cast<float>(level - 1)); }

//                 name              cycle  dmg   proj   type          speed  blast ammo max  spread
const WeaponDef kWeapons[kWeaponCount] = {
    {"SHOTGUN",        0.75f, 40.f,  false, -1,           0.f,   0.f,  0,   0,   0.f},
    {"CHAINGUN",       0.10f, 12.f,  false, -1,           0.f,   0.f,  60,  200, 0.03f},
    {"ROCKET LAUNCHER",0.80f, 110.f, true,  kProjRocket,  22.f,  2.2f, 4,   30,  0.f},
    {"PLASMA RIFLE",   0.085f, 20.f, true,  kProjPlasma,  32.f,  0.f,  40,  200, 0.012f},
};

int tierForRegion(int size) {
    if (size <= 1) return 0;
    if (size <= 3) return 1;
    if (size <= 6) return 2;
    if (size <= 11) return 3;
    if (size <= 17) return 4;
    if (size <= 24) return 5;
    return 6;
}

// Split a region into k spatially compact groups: farthest-point seeds, then
// nearest-seed assignment.
std::vector<std::vector<std::pair<int, int>>> splitRegion(const std::vector<std::pair<int, int>>& cells, int k, std::mt19937& rng) {
    std::vector<std::vector<std::pair<int, int>>> groups;
    if (k <= 1 || static_cast<int>(cells.size()) <= k) { groups.push_back(cells); return groups; }
    std::vector<std::pair<int, int>> seeds;
    std::uniform_int_distribution<size_t> pick(0, cells.size() - 1);
    seeds.push_back(cells[pick(rng)]);
    while (static_cast<int>(seeds.size()) < k) {
        std::pair<int, int> best = cells[0];
        int bestD = -1;
        for (auto& c : cells) {
            int d = 1 << 30;
            for (auto& sd : seeds) d = std::min(d, (c.first - sd.first) * (c.first - sd.first) + (c.second - sd.second) * (c.second - sd.second));
            if (d > bestD) { bestD = d; best = c; }
        }
        seeds.push_back(best);
    }
    groups.assign(seeds.size(), {});
    for (auto& c : cells) {
        size_t bi = 0;
        int bestD = 1 << 30;
        for (size_t i = 0; i < seeds.size(); ++i) {
            int d = (c.first - seeds[i].first) * (c.first - seeds[i].first) + (c.second - seeds[i].second) * (c.second - seeds[i].second);
            if (d < bestD) { bestD = d; bi = i; }
        }
        groups[bi].push_back(c);
    }
    groups.erase(std::remove_if(groups.begin(), groups.end(), [](auto& g) { return g.empty(); }), groups.end());
    return groups;
}

std::pair<int, int> anchorOf(const std::vector<std::pair<int, int>>& cells) {
    float sc = 0.f, sr = 0.f;
    for (auto [x, y] : cells) { sc += static_cast<float>(x); sr += static_cast<float>(y); }
    sc /= static_cast<float>(cells.size());
    sr /= static_cast<float>(cells.size());
    std::pair<int, int> best = cells[0];
    float bd = 1e9f;
    for (auto [x, y] : cells) {
        float d = (x - sc) * (x - sc) + (y - sr) * (y - sr);
        if (d < bd) { bd = d; best = {x, y}; }
    }
    return best;
}
}  // namespace

int maxTierForLevel(int level) {
    if (level <= 1) return 2;   // up to demons
    if (level <= 2) return 3;   // cacodemons
    if (level <= 4) return 4;   // barons
    if (level <= 6) return 5;   // cyberdemons
    return kMaxTier;            // spider masterminds
}

int maxRegionSizeForTier(int tier) {
    static const int sizes[kMaxTier + 1] = {1, 3, 6, 11, 17, 24, 1000};
    return sizes[std::clamp(tier, 0, kMaxTier)];
}

const EnemyStats& enemyStats(int kind) { return kStats[std::clamp(kind, 0, kMonsterKinds - 1)]; }

// A tier's stand-in: a Doom 2 variant when its art is there and the dice say so.
int FpsMode::pickKind(int tier) {
    std::uniform_real_distribution<float> u(0.f, 1.f);
    const int* variants = nullptr;
    int n = 0;
    static const int forImp[] = {7}, forCaco[] = {8, 9}, forBaron[] = {10, 11, 12};
    if (const char* forced = std::getenv("REDLINE_KIND")) {   // test knob: every monster is this kind when its art is loaded
        const int k = std::atoi(forced);
        if (k >= 0 && k < kMonsterKinds && kindAvailable_[k]) return k;
    }
    if (tier == 1) { variants = forImp; n = 1; }
    else if (tier == 3) { variants = forCaco; n = 2; }
    else if (tier == 4) { variants = forBaron; n = 3; }
    if (!variants || u(rng_) > 0.5f) return tier;
    const int pick = variants[static_cast<int>(u(rng_) * static_cast<float>(n)) % n];
    return kindAvailable_[pick] ? pick : tier;
}
const WeaponDef& weaponDef(int id) { return kWeapons[std::clamp(id, 0, kWeaponCount - 1)]; }

FpsMode::FpsMode() : rng_(12345) {}

glm::vec3 FpsMode::forward() const {
    return glm::normalize(glm::vec3(std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_), std::cos(yaw_) * std::cos(pitch_)));
}

float FpsMode::recoil() const {
    float cycle = std::max(0.25f, weaponDef(weapon_).cycle);
    if (gunT_ >= cycle) return 0.f;
    float t = gunT_ / cycle;
    return std::sin(t * kPi) * (1.f - t) * (weapon_ == kRocketLauncher ? 1.6f : weapon_ == kShotgun ? 1.f : 0.4f);
}

void FpsMode::selectWeapon(int id) {
    if (id < 0 || id >= kWeaponCount || id == weapon_) return;
    if (!slots_[id].owned || (weaponDef(id).ammoPerPickup > 0 && slots_[id].ammo <= 0)) return;
    weapon_ = id;
    gunT_ = std::max(gunT_, 0.f);
    push(FpsEvent::Type::WeaponSwitch, eye(), 0, id);
}

// ---------------------------------------------------------------------------
void FpsMode::begin(core::Game& game, int level, const core::Prizes& prizes) {
    stage_ = Stage::Arena;
    stageT_ = 0.f;
    dungeon_.clear();
    bossSeen_ = bossDead_ = false;
    hasKey_ = doorOpen_ = false;
    lockedHintT_ = 0.f;
    boardBlocks_ = 0;
    for (int r = 0; r < core::kBoardH; ++r)
        for (int c = 0; c < core::kBoardW; ++c) boardBlocks_ += !game.at(c, r).empty();
    enemies_.clear();
    projectiles_.clear();
    explosions_.clear();
    debris_.clear();
    pickups_.clear();
    events_.clear();
    finished_ = false;
    finishDelay_ = 0.f;
    elapsed_ = 0.f;
    health_ = std::min(200.f, 100.f + prizes.bonusHealth);
    shield_ = std::min(200.f, prizes.shield);
    damageFlash_ = 0.f;
    damageTaken_ = 0.f;
    pickupFlash_ = 0.f;
    gunT_ = 10.f;
    level_ = level;
    yaw_ = kPi;
    pitch_ = 0.f;
    // Weapons persist across fights within a game; the shotgun is always there.
    slots_[kShotgun].owned = true;
    slots_[kShotgun].ammo = 0;
    if (!slots_[weapon_].owned || (weaponDef(weapon_).ammoPerPickup > 0 && slots_[weapon_].ammo <= 0)) weapon_ = kShotgun;
    rng_.seed(game.seed() * 7919u + static_cast<uint32_t>(game.redLineCount()) * 104729u);
    std::uniform_real_distribution<float> u(0.f, 1.f);
    // The invulnerability prize is a roll made as the monsters rise.
    startedInvuln_ = prizes.invulnChance > 0.f && u(rng_) < prizes.invulnChance;
    invulnT_ = startedInvuln_ ? game.rules().invulnSeconds : 0.f;

    // Difficulty scaling with level: health, chance of upgrading a tier, cadence.
    float hpScale = std::clamp(0.45f + 0.15f * static_cast<float>(level - 1), 0.45f, 2.5f);
    float upgradeChance = std::min(0.5f, 0.08f * static_cast<float>(level - 1));

    const int capTier = maxTierForLevel(level);
    for (const core::RedRegion& region : core::findRedRegions(game)) {
        // Big regions become several monsters: enough that none exceeds the
        // level's biggest allowed class, plus a coin flip to split anyway so a
        // full red row is a crowd rather than one boss.
        int maxSize = maxRegionSizeForTier(capTier);
        int k = (region.size() + maxSize - 1) / maxSize;
        if (k == 1 && region.size() >= 7 && u(rng_) < 0.5f) k = 2;
        if (k == 1 && region.size() >= 15 && u(rng_) < 0.5f) k = 2;
        for (const auto& cells : splitRegion(region.cells, k, rng_)) {
            Enemy e;
            int tier = tierForRegion(static_cast<int>(cells.size()));
            float roll = u(rng_);
            if (roll < upgradeChance && tier < capTier) ++tier;               // random nastier
            else if (roll > 0.85f && tier > 0 && cells.size() > 1) --tier;    // random lucky break
            tier = std::min(tier, capTier);
            // Never more than one boss (cyberdemon / spider) in a fight: extras become barons.
            if (tier >= kBossTier)
                for (const Enemy& other : enemies_) if (other.tier >= kBossTier) { tier = kBossTier - 1; break; }
            e.tier = tier;
            e.kind = pickKind(tier);
            const EnemyStats& st = enemyStats(e.kind);
            e.cells = cells;
            auto [ac, ar] = anchorOf(cells);
            e.pos = flatCellFloor(ac, ar);
            e.pos.y = st.flies ? 0.8f : 0.f;
            e.maxHp = e.hp = st.hp * hpScale;
            e.radius = st.radius;
            e.height = st.height;
            e.attackTimer = (1.2f + 2.0f * u(rng_)) * 1.5f;
            e.breakTimer = st.breakInterval * (0.8f + 0.6f * u(rng_));
            e.absorbTimer = absorbPeriodForLevel(absorbPeriod_, level) * (0.9f + 0.2f * u(rng_));
            e.bobPhase = u(rng_) * 6.28f;
            e.stateT = -0.12f * static_cast<float>(enemies_.size());   // stagger the emergence
            {   // wakes up facing the player
                const glm::vec3 to = playerPos_ - e.pos;
                e.yaw = (std::fabs(to.x) + std::fabs(to.z) > 1e-3f) ? std::atan2(to.x, to.z) : kPi;
                e.moveDir = glm::vec3(std::sin(e.yaw), 0.f, std::cos(e.yaw));
            }
            // The region's cells become the monster's body: clear them so the
            // pocket it stands in is open.
            for (auto [c, r] : cells) game.clearCell(c, r);
            enemies_.push_back(e);
        }
    }

    // Player start: the highest empty cell nearest the centre column.
    playerPos_ = flatCellFloor(4, 1);
    bool placed = false;
    for (int r = 0; r < core::kBoardH && !placed; ++r) {
        const int order[core::kBoardW] = {4, 5, 3, 6, 2, 7, 1, 8, 0, 9};
        for (int k = 0; k < core::kBoardW; ++k) {
            int c = order[k];
            if (!game.at(c, r).empty()) continue;
            bool enemyHere = false;
            for (const Enemy& e : enemies_) if (glm::length(e.pos - flatCellFloor(c, r)) < 1.5f) enemyHere = true;
            if (enemyHere) continue;
            playerPos_ = flatCellFloor(c, r);
            placed = true;
            break;
        }
    }
    push(FpsEvent::Type::EnemySight, playerPos_, enemies_.empty() ? 0 : enemies_.front().tier, static_cast<int>(enemies_.size()), enemies_.empty() ? 0 : enemies_.front().kind);
}

int FpsMode::enemiesLeft() const {
    int n = 0;
    for (const Enemy& e : enemies_) if (e.state != Enemy::State::Dead) ++n;
    return n;
}

std::vector<FpsEvent> FpsMode::drainEvents() {
    std::vector<FpsEvent> out;
    out.swap(events_);
    return out;
}

// ---------------------------------------------------------------------------
bool FpsMode::solidAt(glm::vec3 p, const core::Game& game) const {
    if (stage_ == Stage::Dungeon) {
        // Behind the back wall the dungeon's tiles rule: rock is solid to the ceiling, floor is open.
        if (p.z <= Dungeon::kZTop) {
            // Shut until the key turns up; with the key in hand it is already yielding,
            // so the player (and the test bot) can walk at it rather than path around.
            if (!doorOpen_ && !hasKey_ && dungeon_.doorAt(p.x, p.z)) return true;
            return !dungeon_.floorAt(p.x, p.z) && !dungeon_.doorAt(p.x, p.z) && p.y <= std::max(Dungeon::kWallHeight, dungeon_.ceilingAt(p.x, p.z));
        }
        // The gate: the wall's footprint and the board's end rail in front of it are open.
        if (p.z < 0.f && std::fabs(p.x) < Dungeon::kGateHalf) return false;
    }
    if (p.y > kBlockTop) return false;
    // Board frame: side rails and the end wall.
    if (std::fabs(p.x) > core::kBoardW * 0.5f || p.z > static_cast<float>(core::kBoardH) || p.z < 0.f) return true;
    int c, r;
    if (!flatToCell(p, c, r)) return false;
    return !game.at(c, r).empty();
}

float FpsMode::rayBlockDistance(glm::vec3 o, glm::vec3 d, float maxT, const core::Game& game) const {
    const float step = 0.05f;
    for (float t = step; t < maxT; t += step) {
        glm::vec3 p = o + d * t;
        if (p.y < 0.f) return t;
        if (solidAt(p, game)) return t;
    }
    return maxT;
}

bool FpsMode::lineOfSight(glm::vec3 a, glm::vec3 b, const core::Game& game) const {
    glm::vec3 d = b - a;
    float len = glm::length(d);
    if (len < 1e-4f) return true;
    return rayBlockDistance(a, d / len, len, game) >= len - 1e-3f;
}

// Circle-vs-grid slide: resolve X and Z separately so we glide along blocks.
void FpsMode::moveWithCollision(glm::vec3& pos, glm::vec3 delta, float radius, const core::Game& game) const {
    auto blocked = [&](glm::vec3 p) {
        const float offs[4][2] = {{radius, 0.f}, {-radius, 0.f}, {0.f, radius}, {0.f, -radius}};
        for (auto& o : offs)
            if (solidAt(glm::vec3(p.x + o[0], 0.5f, p.z + o[1]), game)) return true;
        const float dg = radius * 0.7071f;
        const float diag[4][2] = {{dg, dg}, {-dg, dg}, {dg, -dg}, {-dg, -dg}};
        for (auto& o : diag)
            if (solidAt(glm::vec3(p.x + o[0], 0.5f, p.z + o[1]), game)) return true;
        return false;
    };
    glm::vec3 nx = pos + glm::vec3(delta.x, 0.f, 0.f);
    if (!blocked(nx)) pos = nx;
    glm::vec3 nz = pos + glm::vec3(0.f, 0.f, delta.z);
    if (!blocked(nz)) pos = nz;
}

void FpsMode::spawnDebris(const glm::vec3& pos, const glm::vec3& color, int count, bool red, int colorIndex) {
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    for (int i = 0; i < count; ++i) {
        Debris d;
        d.pos = pos + glm::vec3(u(rng_), u(rng_), u(rng_)) * 0.3f;
        d.vel = glm::vec3(u(rng_) * 4.f, 3.5f + u(rng_) * 3.f, u(rng_) * 4.f);
        d.color = color;
        d.colorIndex = colorIndex;
        d.ttl = 1.2f + 0.8f * std::fabs(u(rng_));
        d.size = 0.12f + 0.15f * std::fabs(u(rng_));
        d.red = red;
        debris_.push_back(d);
    }
}

// The arena as App::buildEnvironment lays it out: inner wall faces and the floor top.
namespace {
constexpr float kArenaX = 15.f, kArenaZMin = -2.f, kArenaZMax = 23.f;
constexpr size_t kMaxGore = 900, kMaxDecals = 260;
}  // namespace

void FpsMode::spawnBlood(glm::vec3 pos, glm::vec3 dir, int count, float speed, int kind, int color) {
    if (!brutal_) return;
    if (gore_.size() + static_cast<size_t>(count) > kMaxGore) count = static_cast<int>(kMaxGore - gore_.size());
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    for (int i = 0; i < count; ++i) {
        Gore g;
        g.kind = kind;
        g.pos = pos + glm::vec3(u(rng_), u(rng_), u(rng_)) * 0.12f;
        glm::vec3 scatter(u(rng_), u(rng_) * 0.7f, u(rng_));
        g.vel = dir * speed * (0.4f + 0.6f * std::fabs(u(rng_))) + scatter * speed * 0.55f + glm::vec3(0.f, 1.6f, 0.f);
        g.size = kind == 1 ? 0.05f + 0.07f * std::fabs(u(rng_)) : 0.03f + 0.03f * std::fabs(u(rng_));
        g.ttl = kind == 1 ? 8.f : 3.f;
        g.spin = u(rng_) * 7.f;
        g.variant = static_cast<int>((u(rng_) + 1.f) * 127.5f) & 0xFF;
        g.color = color;
        gore_.push_back(g);
    }
}

void FpsMode::spawnGibs(const Enemy& e) {
    const glm::vec3 chest = e.pos + glm::vec3(0.f, 0.55f * e.height, 0.f);
    const int color = bloodColorForKind(e.kind);
    spawnBlood(chest, glm::vec3(0.f, 0.6f, 0.f), 12 + 5 * e.tier, 5.f, 1, color);
    spawnBlood(chest, glm::vec3(0.f, 0.4f, 0.f), 30 + 8 * e.tier, 5.5f, 0, color);
    addDecal(glm::vec3(e.pos.x, 0.f, e.pos.z), glm::vec3(0.f, 1.f, 0.f), 0.9f + 0.25f * static_cast<float>(e.tier), 0, color);
}

void FpsMode::addDecal(glm::vec3 pos, glm::vec3 normal, float size, int kind, int color) {
    if (!brutal_) return;
    if (decals_.size() >= kMaxDecals) decals_.erase(decals_.begin());
    std::uniform_real_distribution<float> u(0.f, 6.2831853f);
    Decal d;
    d.pos = pos;
    d.normal = normal;
    d.size = size;
    d.yaw = u(rng_);
    d.kind = kind;
    d.color = color;
    decals_.push_back(d);
}

void FpsMode::updateGore(float dt) {
    std::uniform_real_distribution<float> u(0.f, 1.f);
    for (size_t i = 0; i < gore_.size();) {
        Gore& g = gore_[i];
        g.ttl -= dt;
        bool gone = g.ttl <= 0.f;
        if (!g.resting) {
            g.vel.y -= (g.kind == 2 ? 10.f : 14.f) * dt;
            g.pos += g.vel * dt;
            const float half = g.size * 0.5f;
            // Walls: blood sticks as a splat, chunks and casings bounce off.
            for (int axis = 0; axis < 2 && !gone; ++axis) {
                float& p = axis == 0 ? g.pos.x : g.pos.z;
                float& v = axis == 0 ? g.vel.x : g.vel.z;
                const float lo = (axis == 0 ? -kArenaX : kArenaZMin) + half, hi = (axis == 0 ? kArenaX : kArenaZMax) - half;
                if (p < lo || p > hi) {
                    const float side = p < lo ? -1.f : 1.f;
                    if (g.kind == 0) {
                        glm::vec3 n = axis == 0 ? glm::vec3(-side, 0.f, 0.f) : glm::vec3(0.f, 0.f, -side);
                        glm::vec3 at = g.pos;
                        (axis == 0 ? at.x : at.z) = (side < 0.f ? lo - half : hi + half) + n[axis == 0 ? 0 : 2] * 0.02f;
                        if (at.y > 0.05f) addDecal(at, n, 0.2f + 0.2f * u(rng_), 1, g.color);
                        gone = true;
                    } else {
                        p = std::clamp(p, lo, hi);
                        v = -v * 0.4f;
                    }
                }
            }
            // Floor: blood becomes a pool, chunks and casings bounce then rest.
            if (!gone && g.pos.y - half <= 0.f && g.vel.y < 0.f) {
                if (g.kind == 0) {
                    addDecal(glm::vec3(g.pos.x, 0.f, g.pos.z), glm::vec3(0.f, 1.f, 0.f), 0.14f + 0.22f * u(rng_), 0, g.color);
                    gone = true;
                } else {
                    g.pos.y = half;
                    ++g.bounces;
                    if (g.kind == 2 && g.bounces == 1) push(FpsEvent::Type::CasingBounce, g.pos, 0, g.variant & 1);
                    if (g.kind == 1 && g.bounces == 1) addDecal(glm::vec3(g.pos.x, 0.f, g.pos.z), glm::vec3(0.f, 1.f, 0.f), 0.25f + 0.3f * u(rng_), 0, g.color);
                    g.vel.y = -g.vel.y * (g.kind == 2 ? 0.3f : 0.35f);
                    g.vel.x *= 0.55f;
                    g.vel.z *= 0.55f;
                    if (g.vel.y < 0.9f) { g.resting = true; g.vel = glm::vec3(0.f); g.spin = 0.f; }
                }
            }
        }
        if (gone) { gore_[i] = gore_.back(); gore_.pop_back(); }
        else ++i;
    }
    for (size_t i = 0; i < decals_.size();) {
        decals_[i].age += dt;
        if (decals_[i].age > 60.f) decals_.erase(decals_.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

void FpsMode::hurtPlayer(float dmg, glm::vec3 from, int kind) {
    if (health_ <= 0.f || god_ || invulnT_ > 0.f) return;
    damageTaken_ += dmg;
    // Armour soaks half of any hit until it is spent.
    if (shield_ > 0.f) {
        float absorbed = std::min(shield_, dmg * 0.5f);
        shield_ -= absorbed;
        dmg -= absorbed;
    }
    health_ = std::max(0.f, health_ - dmg);
    damageFlash_ = 1.f;
    push(FpsEvent::Type::PlayerHit, from, std::max(0, kind), 0, std::max(0, kind));
    if (health_ <= 0.f) push(FpsEvent::Type::PlayerDead, from, std::max(0, kind), 0, std::max(0, kind));
}

// ---------------------------------------------------------------------------
void FpsMode::damageEnemy(Enemy& e, float dmg, glm::vec3 hitPos, glm::vec3 dir, int weapon, bool head) {
    if (!e.alive()) return;
    if (e.dormant) {   // rudely woken
        e.dormant = false;
        e.attackTimer = 0.4f;
        push(FpsEvent::Type::Wake, e.pos + glm::vec3(0.f, 1.f, 0.f), e.tier, 0, e.kind);
        if (e.boss && !bossSeen_) { bossSeen_ = true; push(FpsEvent::Type::BossSeen, e.pos, e.tier, 0, e.kind); }
    }
    e.hp -= dmg;
    push(FpsEvent::Type::EnemyHit, hitPos, e.tier, 0, e.kind);
    if (brutal_) spawnBlood(hitPos, dir, 6 + static_cast<int>(dmg * 0.15f), 4.5f, 0, bloodColorForKind(e.kind));
    else spawnDebris(hitPos, {0.6f, 0.05f, 0.05f}, 3, true);
    if (e.hp <= 0.f) {
        health_ = std::max(health_, std::min(100.f, health_ + 5.f));   // small heal per kill keeps long fights winnable
        if (e.boss && !bossDead_) { bossDead_ = true; finishDelay_ = 0.f; push(FpsEvent::Type::BossDead, e.pos + glm::vec3(0.f, 1.f, 0.f), e.tier, 0, e.kind); }
        e.state = Enemy::State::Dying;
        e.stateT = 0.f;
        e.animT = 0.f;
        // Brutal: overkill (rockets, point-blank shotgun, a big hit past the remaining health)
        // or plain luck blows the monster apart. The two bosses only explode.
        std::uniform_real_distribution<float> lucky(0.f, 1.f);
        e.gibbed = brutal_ && e.tier <= 4 && (dmg >= 60.f || -e.hp >= 0.4f * e.maxHp || lucky(rng_) < 0.3f);
        if (e.gibbed) { spawnGibs(e); push(FpsEvent::Type::EnemyGibbed, e.pos, e.tier, 0, e.kind); }
        // Otherwise the death animation follows the weapon: a close shotgun blast throws the
        // body back, the chaingun shreds, plasma carbonises, rockets that fail to gib still blast,
        // and a high hitscan hit takes the head.
        e.deathKind = -1;
        if (brutal_ && !e.gibbed) {
            const float dist = glm::length(glm::vec3(e.pos.x - playerPos_.x, 0.f, e.pos.z - playerPos_.z));
            const float roll = lucky(rng_);
            if (head && (weapon == kShotgun || weapon == kChaingun) && roll < 0.6f) e.deathKind = kDeathHead;
            else if (weapon == kShotgun) e.deathKind = dist < 4.5f ? kDeathShotgun : (roll < 0.5f ? kDeathAlt : -1);
            else if (weapon == kChaingun) e.deathKind = roll < 0.7f ? kDeathChaingun : kDeathAlt;
            else if (weapon == kPlasmaRifle) e.deathKind = kDeathPlasma;
            else if (weapon == kRocketLauncher) e.deathKind = kDeathBlast;
            else if (roll < 0.35f) e.deathKind = kDeathAlt;
        }
        if (brutal_ && std::getenv("REDLINE_LOG_DEATHS")) std::fprintf(stderr, "[brutal] tier %d killed by weapon %d%s: %s kind %d\n", e.tier, weapon, head ? " (head)" : "", e.gibbed ? "gibbed" : "death", e.deathKind);
        push(FpsEvent::Type::EnemyDied, e.pos, e.tier, e.gibbed ? 1 : 0, e.kind);
        dropLoot(e);
    } else if (e.state != Enemy::State::Emerging && std::uniform_real_distribution<float>(0.f, 1.f)(rng_) < enemyStats(e.kind).painChance) {
        // A stagger cancels whatever it was doing (the arch-vile's flame goes out with it).
        e.state = Enemy::State::Pain;
        e.stateT = 0.f;
        e.attacked = false;
        e.fireT = -1.f;
        // Wounded monsters sometimes shed ammo (for the gun you are holding).
        std::uniform_real_distribution<float> u(0.f, 1.f);
        if (e.ammoDropCooldown <= 0.f && u(rng_) < 0.12f) {
            e.ammoDropCooldown = 2.5f;
            PickupKind kind = PickupKind::Bullets;
            if (weapon_ == kRocketLauncher) kind = PickupKind::Rockets;
            else if (weapon_ == kPlasmaRifle) kind = PickupKind::Cells;
            else if (weapon_ == kShotgun) {
                const PickupKind any[3] = {PickupKind::Bullets, PickupKind::Rockets, PickupKind::Cells};
                kind = any[static_cast<int>(u(rng_) * 3.f) % 3];
            }
            spawnPickup(kind, e.pos + glm::vec3(0.f, 0.9f, 0.f), glm::vec3(e.pos.x, 0.f, e.pos.z));
        }
    }
}

void FpsMode::hitscan(glm::vec3 o, glm::vec3 d, float damage, core::Game& game, int weapon) {
    float blockT = rayBlockDistance(o, d, 60.f, game);
    Enemy* best = nullptr;
    float bestT = blockT;
    for (Enemy& e : enemies_) {
        if (!e.alive() || e.state == Enemy::State::Emerging) continue;
        // Ray vs vertical cylinder, then height check.
        glm::vec2 oc(o.x - e.pos.x, o.z - e.pos.z);
        glm::vec2 dd(d.x, d.z);
        float a = glm::dot(dd, dd);
        float b = 2.f * glm::dot(oc, dd);
        float c = glm::dot(oc, oc) - e.radius * e.radius;
        float disc = b * b - 4 * a * c;
        if (disc < 0.f || a < 1e-6f) continue;
        float t = (-b - std::sqrt(disc)) / (2 * a);
        if (t < 0.f) t = (-b + std::sqrt(disc)) / (2 * a);
        if (t < 0.f) continue;
        float y = o.y + d.y * t;
        if (y < e.pos.y || y > e.pos.y + e.height) continue;
        if (t < bestT) { bestT = t; best = &e; }
    }
    if (!best) {
        // Nothing hit: a bullet hole where the shot meets a wall or the floor (before any block).
        float tWall = 60.f;
        glm::vec3 nWall(0.f, 1.f, 0.f);
        auto plane = [&](float p0, float dir, float target, glm::vec3 n) {
            if (std::fabs(dir) < 1e-5f) return;
            float t = (target - p0) / dir;
            if (t > 0.f && t < tWall) { tWall = t; nWall = n; }
        };
        plane(o.x, d.x, -kArenaX, {1.f, 0.f, 0.f});
        plane(o.x, d.x, kArenaX, {-1.f, 0.f, 0.f});
        plane(o.z, d.z, kArenaZMin, {0.f, 0.f, 1.f});
        plane(o.z, d.z, kArenaZMax, {0.f, 0.f, -1.f});
        plane(o.y, d.y, 0.f, {0.f, 1.f, 0.f});
        if (blockT < 60.f && blockT < tWall) spawnDebris(o + d * blockT, {0.6f, 0.6f, 0.6f}, 2, false);
        else if (tWall < 60.f) { addDecal(o + d * tWall + nWall * 0.02f, nWall, 0.12f, 2); if (brutal_) push(FpsEvent::Type::BulletHole, o + d * tWall + nWall * 0.12f, 0, nWall.y > 0.5f ? 0 : 1); }
        return;
    }
    const glm::vec3 hit = o + d * bestT;
    const bool head = hit.y >= best->pos.y + 0.72f * best->height;
    damageEnemy(*best, damage * (bestT < 2.5f ? 1.4f : 1.f), hit, d, weapon, head);
}

void FpsMode::fire(core::Game& game) {
    const WeaponDef& w = weaponDef(weapon_);
    if (w.ammoPerPickup > 0) {
        if (slots_[weapon_].ammo <= 0) { selectWeapon(kShotgun); weapon_ = kShotgun; return; }
        --slots_[weapon_].ammo;
    }
    gunT_ = 0.f;
    push(FpsEvent::Type::Shoot, eye(), 0, weapon_);
    glm::vec3 o = eye();
    glm::vec3 d = forward();
    if (brutal_ && (weapon_ == kShotgun || weapon_ == kChaingun) && gore_.size() < kMaxGore) {
        // A shell casing ejected to the right of the gun.
        std::uniform_real_distribution<float> u(-1.f, 1.f);
        glm::vec3 right(-std::cos(yaw_), 0.f, std::sin(yaw_));
        Gore g;
        g.kind = 2;
        g.pos = o + right * 0.3f + d * 0.4f - glm::vec3(0.f, 0.25f, 0.f);
        g.vel = right * (2.2f + u(rng_) * 0.6f) + glm::vec3(0.f, 2.2f + u(rng_) * 0.5f, 0.f) + d * 0.4f;
        g.size = weapon_ == kShotgun ? 0.05f : 0.035f;
        g.ttl = 6.f;
        g.spin = u(rng_) * 12.f;
        g.variant = (static_cast<int>((u(rng_) + 1.f) * 127.f) & 0xFE) | (weapon_ == kShotgun ? 1 : 0);
        gore_.push_back(g);
    }
    if (w.spread > 0.f) {
        std::uniform_real_distribution<float> u(-1.f, 1.f);
        glm::vec3 right(-std::cos(yaw_), 0.f, std::sin(yaw_));
        glm::vec3 up = glm::normalize(glm::cross(right, d));
        d = glm::normalize(d + right * (u(rng_) * w.spread) + up * (u(rng_) * w.spread));
    }
    if (!w.projectile) {
        hitscan(o, d, w.damage, game, weapon_);
        return;
    }
    Projectile p;
    p.pos = o + d * 0.6f - glm::vec3(0.f, 0.15f, 0.f);
    p.vel = d * w.projSpeed;
    p.type = w.projType;
    p.damage = w.damage;
    p.blast = w.blast;
    p.fromPlayer = true;
    p.ttl = 4.f;
    projectiles_.push_back(p);
    // Out of ammo after this shot: fall back to the shotgun once the cycle ends.
    if (w.ammoPerPickup > 0 && slots_[weapon_].ammo <= 0) { int prev = weapon_; weapon_ = kShotgun; push(FpsEvent::Type::WeaponSwitch, eye(), 0, kShotgun); (void)prev; }
}

// Splash damage from a player rocket: hurts every monster in range, the player
// if too close, and blasts the blocks around the impact.
void FpsMode::rocketBlast(glm::vec3 pos, float radius, float damage, core::Game& game) {
    for (Enemy& e : enemies_) {
        if (!e.alive()) continue;
        glm::vec3 c = e.pos + glm::vec3(0.f, e.height * 0.5f, 0.f);
        float d = glm::length(c - pos);
        if (d < radius + e.radius) damageEnemy(e, damage * (1.f - std::max(0.f, d - e.radius) / radius), c, glm::vec3(0.f), kRocketLauncher);
    }
    float pd = glm::length((playerPos_ + glm::vec3(0.f, 0.6f, 0.f)) - pos);
    if (pd < radius) hurtPlayer(45.f * (1.f - pd / radius), pos);
    int destroyed = 0;
    int c, r;
    if (flatToCell(glm::vec3(pos.x, 0.5f, pos.z), c, r)) {
        // Blast every normal block within a cell of the impact.
        for (int y = r - 1; y <= r + 1; ++y)
            for (int x = c - 1; x <= c + 1; ++x) {
                if (x < 0 || x >= core::kBoardW || y < 0 || y >= core::kBoardH) continue;
                if (game.at(x, y).kind != core::CellKind::Normal) continue;
                if (glm::length(flatCellCentre(x, y) - pos) > radius * 0.75f) continue;
                const int shade = game.at(x, y).color;
                game.clearCell(x, y);
                spawnDebris(flatCellCentre(x, y), {0.6f, 0.6f, 0.6f}, 5, false, shade);
                ++destroyed;
            }
    }
    Explosion ex;
    ex.pos = pos;
    ex.radius = radius;
    ex.duration = 0.5f;
    explosions_.push_back(ex);
    push(FpsEvent::Type::RocketBlast, pos, 0, destroyed);
}

// A monster's rocket: hurts the player in range and blows the cover apart.
void FpsMode::enemyBlast(glm::vec3 pos, float radius, float damage, core::Game& game, int kind) {
    float pd = glm::length((playerPos_ + glm::vec3(0.f, 0.6f, 0.f)) - pos);
    if (pd < radius) hurtPlayer(damage * (1.f - pd / radius), pos, kind);
    int destroyed = 0;
    int c, r;
    if (flatToCell(glm::vec3(pos.x, 0.5f, pos.z), c, r)) {
        for (int y = r - 1; y <= r + 1; ++y)
            for (int x = c - 1; x <= c + 1; ++x) {
                if (x < 0 || x >= core::kBoardW || y < 0 || y >= core::kBoardH) continue;
                if (game.at(x, y).kind != core::CellKind::Normal) continue;
                if (glm::length(flatCellCentre(x, y) - pos) > radius * 0.75f) continue;
                breakCell(x, y, game);
                ++destroyed;
            }
    }
    Explosion ex;
    ex.pos = pos;
    ex.radius = radius;
    ex.duration = 0.5f;
    explosions_.push_back(ex);
    push(FpsEvent::Type::RocketBlast, pos, 0, destroyed);
}

void FpsMode::explodeEnemy(Enemy& e, core::Game& game) {
    if (e.exploded) return;
    e.exploded = true;
    // Every cell of the region blasts its normal neighbours.
    int destroyed = 0;
    std::vector<std::pair<int, int>> victims;
    std::vector<int> victimShades;
    for (auto [c, r] : e.cells) {
        for (int y = r - 2; y <= r + 2; ++y)
            for (int x = c - 2; x <= c + 2; ++x) {
                if (x < 0 || x >= core::kBoardW || y < 0 || y >= core::kBoardH) continue;
                float dx = static_cast<float>(x - c), dy = static_cast<float>(y - r);
                if (dx * dx + dy * dy > kExplosionRadius * kExplosionRadius + 1e-4f) continue;
                if (game.at(x, y).kind == core::CellKind::Normal) { victims.push_back({x, y}); victimShades.push_back(game.at(x, y).color); }
            }
        destroyed += game.explodeAt(c, r, kExplosionRadius);
    }
    for (size_t v = 0; v < victims.size(); ++v) spawnDebris(flatCellCentre(victims[v].first, victims[v].second), {0.6f, 0.6f, 0.6f}, 5, false, victimShades[v]);
    spawnDebris(e.pos + glm::vec3(0.f, 0.6f, 0.f), {0.9f, 0.1f, 0.1f}, 8 + 3 * e.tier, true);
    Explosion ex;
    ex.pos = e.pos + glm::vec3(0.f, 0.7f, 0.f);
    ex.radius = 1.5f + 0.4f * static_cast<float>(e.tier);
    ex.duration = 0.45f + 0.1f * static_cast<float>(e.tier);
    explosions_.push_back(ex);
    // Standing next to a dying demon hurts.
    float dist = glm::length(playerPos_ - e.pos);
    float blast = 2.0f + 0.5f * static_cast<float>(e.tier);
    if (dist < blast) hurtPlayer((10.f + 6.f * static_cast<float>(e.tier)) * (1.f - dist / blast), e.pos);
    push(FpsEvent::Type::Explosion, ex.pos, e.tier, destroyed);
    // Harder monsters pay more, scaled by level, plus a bounty per block they took with them.
    const EnemyStats& st = enemyStats(e.kind);
    int points = static_cast<int>(static_cast<float>(st.scoreValue) * (1.f + 0.1f * static_cast<float>(level_ - 1))) + 25 * destroyed;
    game.addScore(points);
    push(FpsEvent::Type::Score, e.pos + glm::vec3(0.f, 1.5f, 0.f), e.tier, points, e.kind);
    if (e.grown) push(FpsEvent::Type::KilledGrown, e.pos, e.tier, 0);
}

// ---------------------------------------------------------------------------
// Loot: bigger monsters drop more. Weapons the player lacks are favoured.
void FpsMode::dropLoot(const Enemy& e) {
    std::uniform_real_distribution<float> u(0.f, 1.f);
    int count = 1 + e.tier / 2 + (u(rng_) < 0.25f ? 1 : 0);
    bool anyExtraWeapon = slots_[kChaingun].owned || slots_[kRocketLauncher].owned || slots_[kPlasmaRifle].owned;
    // Big monsters are worth the trouble: cacodemons and up always drop a
    // medikit, barons and up almost always drop a weapon as well.
    if (e.tier >= 3) spawnPickup(PickupKind::Medikit, e.pos + glm::vec3(0.f, 0.8f, 0.f), glm::vec3(e.pos.x, 0.f, e.pos.z));
    if (e.tier >= 4 && u(rng_) < 0.9f) {
        float w = u(rng_) + 0.25f * static_cast<float>(e.tier - 3);
        PickupKind kind = w < 0.6f ? PickupKind::Chaingun : (w < 1.2f ? PickupKind::RocketLauncher : PickupKind::PlasmaGun);
        spawnPickup(kind, e.pos + glm::vec3(0.f, 0.8f, 0.f), glm::vec3(e.pos.x, 0.f, e.pos.z));
    }
    if (e.tier >= 5) spawnPickup(PickupKind::Medikit, e.pos + glm::vec3(0.f, 0.8f, 0.f), glm::vec3(e.pos.x, 0.f, e.pos.z));
    for (int i = 0; i < count; ++i) {
        float roll = u(rng_);
        PickupKind kind;
        if (roll < 0.32f) {
            kind = (e.tier >= 2 || u(rng_) < 0.3f) ? PickupKind::Medikit : PickupKind::Stim;
        } else if (roll < (anyExtraWeapon ? 0.66f : 0.45f)) {
            const PickupKind ammo[3] = {PickupKind::Bullets, PickupKind::Rockets, PickupKind::Cells};
            // Prefer ammo for something the player owns.
            std::vector<PickupKind> owned;
            if (slots_[kChaingun].owned) owned.push_back(PickupKind::Bullets);
            if (slots_[kRocketLauncher].owned) owned.push_back(PickupKind::Rockets);
            if (slots_[kPlasmaRifle].owned) owned.push_back(PickupKind::Cells);
            kind = owned.empty() ? ammo[static_cast<int>(u(rng_) * 3.f) % 3] : owned[static_cast<size_t>(u(rng_) * static_cast<float>(owned.size())) % owned.size()];
        } else if (roll < 0.88f) {
            // Weapon: class roughly follows the monster's tier.
            float w = u(rng_) + 0.25f * static_cast<float>(e.tier);
            kind = w < 0.8f ? PickupKind::Chaingun : (w < 1.5f ? PickupKind::RocketLauncher : PickupKind::PlasmaGun);
        } else {
            continue;   // nothing
        }
        spawnPickup(kind, e.pos + glm::vec3(0.f, 0.8f, 0.f), glm::vec3(e.pos.x, 0.f, e.pos.z));
    }
}

void FpsMode::spawnPickup(PickupKind kind, glm::vec3 from, glm::vec3 home) {
    std::uniform_real_distribution<float> u(0.f, 1.f);
    Pickup p;
    p.kind = kind;
    p.home = home;
    p.pos = from;
    p.vel = glm::vec3((u(rng_) - 0.5f) * 3.f, 3.f + u(rng_) * 2.f, (u(rng_) - 0.5f) * 3.f);
    pickups_.push_back(p);
}

// ---------------------------------------------------------------------------
// Monsters erode the player's cover. The block that hides the player goes
// first; otherwise something nearby.
void FpsMode::breakCell(int c, int r, core::Game& game) {
    if (c < 0 || c >= core::kBoardW || r < 0 || r >= core::kBoardH) return;
    if (game.at(c, r).kind != core::CellKind::Normal) return;
    const int shade = game.at(c, r).color;
    game.clearCell(c, r);
    spawnDebris(flatCellCentre(c, r), {0.6f, 0.6f, 0.6f}, 5, false, shade);
    Explosion ex;
    ex.pos = flatCellCentre(c, r);
    ex.radius = 0.9f;
    ex.duration = 0.35f;
    explosions_.push_back(ex);
}

void FpsMode::breakBlock(Enemy& e, core::Game& game) {
    const EnemyStats& st = enemyStats(e.kind);
    std::uniform_real_distribution<float> u(0.f, 1.f);
    glm::vec3 from = e.pos + glm::vec3(0.f, 1.2f, 0.f);
    glm::vec3 to = playerPos_ + glm::vec3(0.f, 0.6f, 0.f);
    glm::vec3 d = to - from;
    float len = glm::length(d);
    int c = -1, r = -1;
    if (len > 1e-3f) {
        float t = rayBlockDistance(from, d / len, len, game);
        if (t < len - 1e-3f) {
            glm::vec3 p = from + (d / len) * (t + 0.03f);
            if (!flatToCell(glm::vec3(p.x, 0.5f, p.z), c, r) || game.at(c, r).kind != core::CellKind::Normal) c = -1;
        }
    }
    if (c < 0) {
        // Nothing between us: chew on a random block within two cells (half the time).
        if (u(rng_) < 0.5f) return;
        int ec, er;
        if (!flatToCell(glm::vec3(e.pos.x, 0.5f, e.pos.z), ec, er)) return;
        std::vector<std::pair<int, int>> near;
        for (int y = er - 2; y <= er + 2; ++y)
            for (int x = ec - 2; x <= ec + 2; ++x)
                if (x >= 0 && x < core::kBoardW && y >= 0 && y < core::kBoardH && game.at(x, y).kind == core::CellKind::Normal) near.push_back({x, y});
        if (near.empty()) return;
        auto [x, y] = near[static_cast<size_t>(u(rng_) * static_cast<float>(near.size())) % near.size()];
        c = x; r = y;
    }
    int count = 0;
    for (int y = r - st.breakRadius; y <= r + st.breakRadius; ++y)
        for (int x = c - st.breakRadius; x <= c + st.breakRadius; ++x) {
            if (x < 0 || x >= core::kBoardW || y < 0 || y >= core::kBoardH) continue;
            if (game.at(x, y).kind == core::CellKind::Normal) { breakCell(x, y, game); ++count; }
        }
    if (count > 0) push(FpsEvent::Type::BlockBroken, flatCellCentre(c, r), e.tier, count);
}

// Left alive too long, a monster may pull the surrounding blocks into itself
// and come back a tier bigger, at full health.
void FpsMode::tryAbsorb(Enemy& e, core::Game& game) {
    std::uniform_real_distribution<float> u(0.f, 1.f);
    e.absorbTimer = absorbPeriodForLevel(absorbPeriod_, level_) * (0.9f + 0.2f * u(rng_));
    int growCap = std::min(kMaxTier, maxTierForLevel(level_) + 1);
    if (e.tier + 1 >= kBossTier)   // growing into a boss is only allowed if none is alive
        for (const Enemy& other : enemies_) if (&other != &e && other.alive() && other.tier >= kBossTier) return;
    if (e.tier >= growCap) return;
    // Nearby blocks are pulled in and add to the new body's health; with no
    // cover left the monster still grows, it just starts a little weaker.
    int ec, er;
    std::vector<std::pair<int, int>> food;
    if (flatToCell(glm::vec3(e.pos.x, 0.5f, e.pos.z), ec, er))
        for (int y = er - 3; y <= er + 3; ++y)
            for (int x = ec - 3; x <= ec + 3; ++x) {
                if (x < 0 || x >= core::kBoardW || y < 0 || y >= core::kBoardH) continue;
                float dx = static_cast<float>(x - ec), dy = static_cast<float>(y - er);
                if (dx * dx + dy * dy > 2.5f * 2.5f) continue;
                if (game.at(x, y).kind == core::CellKind::Normal) food.push_back({x, y});
            }
    for (auto [x, y] : food) {
        game.clearCell(x, y);
        e.cells.push_back({x, y});   // its death blast now covers them too
        for (int i = 0; i < 3; ++i) {
            Debris d;
            d.pos = flatCellCentre(x, y) + glm::vec3(u(rng_) - 0.5f, u(rng_) - 0.5f, u(rng_) - 0.5f) * 0.4f;
            d.target = e.pos + glm::vec3(0.f, 0.9f, 0.f);
            d.vel = (d.target - d.pos) / 0.55f;
            d.homing = true;
            d.ttl = 0.55f;
            d.size = 0.22f;
            d.color = {0.9f, 0.9f, 0.9f};
            debris_.push_back(d);
        }
    }
    ++e.tier;
    e.kind = pickKind(e.tier);
    const EnemyStats& st = enemyStats(e.kind);
    float hpScale = std::clamp(0.45f + 0.15f * static_cast<float>(level_ - 1), 0.45f, 2.5f);
    e.maxHp = e.hp = st.hp * hpScale * (food.empty() ? 0.8f : std::min(1.2f, 1.f + 0.04f * static_cast<float>(food.size())));
    e.radius = st.radius;
    e.height = st.height;
    e.growT = 1.f;
    e.grown = true;
    e.breakTimer = st.breakInterval * 0.5f;
    e.attackTimer = 1.0f;
    e.state = Enemy::State::Idle;
    e.stateT = 0.f;
    push(FpsEvent::Type::Absorb, e.pos + glm::vec3(0.f, 1.f, 0.f), e.tier, static_cast<int>(food.size()));
}

void FpsMode::giveArsenal(int weapon) {
    for (int i = 0; i < kWeaponCount; ++i) { slots_[i].owned = true; slots_[i].ammo = weaponDef(i).maxAmmo; }
    weapon_ = std::clamp(weapon, 0, kWeaponCount - 1);
}

void FpsMode::applyPickup(const Pickup& p) {
    auto giveWeapon = [&](int id) {
        bool had = slots_[id].owned;
        slots_[id].owned = true;
        slots_[id].ammo = std::min(weaponDef(id).maxAmmo, slots_[id].ammo + weaponDef(id).ammoPerPickup * (had ? 1 : 2));
        if (!had) { weapon_ = id; push(FpsEvent::Type::WeaponSwitch, eye(), 0, id); }
    };
    auto giveAmmo = [&](int id, float mult) {
        slots_[id].ammo = std::min(weaponDef(id).maxAmmo, slots_[id].ammo + static_cast<int>(weaponDef(id).ammoPerPickup * mult));
    };
    switch (p.kind) {
    case PickupKind::Stim: health_ = std::max(health_, std::min(100.f, health_ + 10.f)); break;
    case PickupKind::Medikit: health_ = std::max(health_, std::min(100.f, health_ + 25.f)); break;
    case PickupKind::Bullets: giveAmmo(kChaingun, 1.f); break;
    case PickupKind::Rockets: giveAmmo(kRocketLauncher, 1.f); break;
    case PickupKind::Cells: giveAmmo(kPlasmaRifle, 1.f); break;
    case PickupKind::Chaingun: giveWeapon(kChaingun); break;
    case PickupKind::RocketLauncher: giveWeapon(kRocketLauncher); break;
    case PickupKind::PlasmaGun: giveWeapon(kPlasmaRifle); break;
    case PickupKind::Key: hasKey_ = true; push(FpsEvent::Type::KeyFound, p.pos); break;
    default: break;
    }
    pickupFlash_ = 1.f;
    push(FpsEvent::Type::Pickup, p.pos, 0, static_cast<int>(p.kind));
}

// ---------------------------------------------------------------------------
void FpsMode::update(float dt, const FpsInput& in, core::Game& game) {
    elapsed_ += dt;
    // --- look & move ---------------------------------------------------------
    yaw_ -= in.lookDX * kMouseSens;
    pitch_ = std::clamp(pitch_ - in.lookDY * kMouseSens, -1.2f, 1.2f);
    glm::vec3 fwd(std::sin(yaw_), 0.f, std::cos(yaw_));
    glm::vec3 right(-std::cos(yaw_), 0.f, std::sin(yaw_));   // forward x up
    glm::vec3 move = fwd * in.moveZ + right * in.moveX;
    if (glm::length(move) > 1.f) move = glm::normalize(move);
    if (health_ > 0.f && !in.warmup) moveWithCollision(playerPos_, move * kMoveSpeed * (in.run ? kRunMultiplier : 1.f) * dt, kPlayerRadius, game);
    playerPos_.y = 0.f;
    if (jumpY_ > 0.f || jumpVy_ > 0.f) {   // thrown into the air: the view rises and falls back
        jumpVy_ -= 10.f * dt;
        jumpY_ = std::max(0.f, jumpY_ + jumpVy_ * dt);
        if (jumpY_ <= 0.f) jumpVy_ = 0.f;
    }
    damageFlash_ = std::max(0.f, damageFlash_ - dt * 2.5f);
    pickupFlash_ = std::max(0.f, pickupFlash_ - dt * 3.f);
    if (!in.warmup) invulnT_ = std::max(0.f, invulnT_ - dt);

    // --- weapon ----------------------------------------------------------
    if (in.selectWeapon >= 0) selectWeapon(in.selectWeapon);
    if (in.wheel != 0) {
        for (int step = 1; step < kWeaponCount; ++step) {
            int id = ((weapon_ + in.wheel * step) % kWeaponCount + kWeaponCount) % kWeaponCount;
            if (slots_[id].owned && (weaponDef(id).ammoPerPickup == 0 || slots_[id].ammo > 0)) { selectWeapon(id); break; }
        }
    }
    gunT_ += dt;
    if (in.fire && !in.warmup && gunT_ >= weaponDef(weapon_).cycle && health_ > 0.f) fire(game);

    // --- enemies ---------------------------------------------------------
    std::uniform_real_distribution<float> u(0.f, 1.f);
    int alive = 0;
    for (const Enemy& e : enemies_) alive += e.alive() && !e.dormant;   // sleepers do not crowd the room
    float crowd = std::max(1.f, static_cast<float>(alive) * 0.35f);
    float cadence = std::max(0.5f, 1.f - 0.06f * static_cast<float>(level_ - 1));
    glm::vec3 playerCentre = playerPos_ + glm::vec3(0.f, 0.6f, 0.f);
    for (Enemy& e : enemies_) {
        const EnemyStats& st = enemyStats(e.kind);
        e.stateT += dt;
        e.animT += dt;
        e.flashT = std::max(0.f, e.flashT - dt * 6.f);
        e.growT = std::max(0.f, e.growT - dt * 1.5f);
        e.ammoDropCooldown = std::max(0.f, e.ammoDropCooldown - dt);
        if (in.warmup && e.state != Enemy::State::Emerging) {   // countdown: hold position
            e.pos.y = st.flies ? 0.8f : 0.f;
            continue;
        }
        if (e.dormant) {
            // Asleep in its room until it sees the player or they come close (a hit wakes it too).
            if (!e.alive()) { e.dormant = false; }
            else {
                e.senseT -= dt;
                if (e.senseT > 0.f) continue;
                e.senseT = 0.15f + 0.15f * u(rng_);
                const glm::vec3 to = playerPos_ - e.pos;
                const float d = std::sqrt(to.x * to.x + to.z * to.z);
                const bool sees = d < 45.f && lineOfSight(e.pos + glm::vec3(0.f, 1.2f, 0.f), playerCentre, game);
                if (d > 3.5f && !sees) continue;
                e.dormant = false;
                e.attackTimer = 0.5f + 0.9f * u(rng_);
                if (d > 1e-3f) e.yaw = std::atan2(to.x, to.z);
                push(FpsEvent::Type::Wake, e.pos + glm::vec3(0.f, 1.f, 0.f), e.tier, 0, e.kind);
                if (e.boss && !bossSeen_) { bossSeen_ = true; push(FpsEvent::Type::BossSeen, e.pos, e.tier, 0, e.kind); }
            }
        }
        if (e.alive() && e.state != Enemy::State::Emerging && health_ > 0.f) {
            e.breakTimer -= dt;
            if (e.breakTimer <= 0.f) {
                e.breakTimer = st.breakInterval * cadence * (0.8f + 0.4f * u(rng_));
                breakBlock(e, game);
            }
            e.absorbTimer -= dt;
            if (e.absorbTimer <= 0.f) tryAbsorb(e, game);
        }
        float restY = st.flies ? 0.8f + 0.15f * std::sin(elapsed_ * 2.f + e.bobPhase) : 0.f;
        glm::vec3 toPlayer = playerPos_ - e.pos;
        toPlayer.y = 0.f;
        float dist = glm::length(toPlayer);
        glm::vec3 dir = dist > 1e-4f ? toPlayer / dist : glm::vec3(0.f, 0.f, 1.f);

        switch (e.state) {
        case Enemy::State::Emerging: {
            // Rise out of the board, facing the player.
            if (dist > 1e-3f) e.yaw = std::atan2(dir.x, dir.z);
            float t = std::clamp(e.stateT / 0.9f, 0.f, 1.f);
            float s = t * t * (3 - 2 * t);
            e.pos.y = -e.height + (e.height + restY) * s;
            if (e.stateT >= 0.9f) { e.state = Enemy::State::Idle; e.stateT = 0.f; e.pos.y = restY; }
            break;
        }
        case Enemy::State::Idle: {
            e.pos.y = restY;
            bool los = lineOfSight(e.pos + glm::vec3(0.f, 1.2f, 0.f), playerCentre, game);
            // Movers close in; flyers drift over the blocks.
            bool walking = false;
            if (st.speed > 0.f && health_ > 0.f) {
                float want = (st.attack == AttackKind::Melee) ? 0.9f : 5.f;
                if (dist > want) {
                    walking = true;
                    e.moveCount -= dt;
                    if (e.moveCount <= 0.f) {
                        // P_NewChaseDir: head for the target along one of eight compass
                        // directions and keep it for a moment; if the last move got nowhere,
                        // sidestep 45 or 90 degrees instead of pushing into the obstacle.
                        float a = std::round(std::atan2(dir.x, dir.z) / (kPi / 4.f)) * (kPi / 4.f);
                        if (e.blocked) a += (u(rng_) < 0.5f ? 1.f : -1.f) * (kPi / 4.f) * (u(rng_) < 0.5f ? 1.f : 2.f);
                        e.moveDir = glm::vec3(std::sin(a), 0.f, std::cos(a));
                        e.moveCount = 0.1f + 0.35f * u(rng_);
                        e.blocked = false;
                    }
                    const glm::vec3 delta = e.moveDir * st.speed * dt;
                    const glm::vec3 before = e.pos;
                    if (st.flies && !e.inDungeon) e.pos += delta;
                    else moveWithCollision(e.pos, delta, e.radius, game);
                    if (!st.flies && glm::length(e.pos - before) < 0.3f * glm::length(delta)) { e.blocked = true; e.moveCount = 0.f; }
                    turnTowards(e.yaw, std::atan2(e.moveDir.x, e.moveDir.z), kChaseTurnRate * dt);
                }
            }
            if (!walking && dist > 1e-3f) turnTowards(e.yaw, std::atan2(dir.x, dir.z), kStandTurnRate * dt);
            e.attackTimer -= dt;
            bool canAttack = health_ > 0.f && e.attackTimer <= 0.f;
            if (st.attack == AttackKind::Melee) canAttack = canAttack && dist < 1.3f;
            else canAttack = canAttack && los;
            if (st.attack == AttackKind::Vile) canAttack = canAttack && dist < kVileRange;
            if (canAttack) {
                e.state = Enemy::State::Attack;
                e.stateT = 0.f;
                e.animT = 0.f;
                e.attacked = false;
                if (dist > 1e-3f) e.yaw = std::atan2(dir.x, dir.z);   // A_FaceTarget
                e.fireT = -1.f;
                if (st.attack == AttackKind::Vile) push(FpsEvent::Type::EnemyAttack, e.pos + glm::vec3(0.f, 1.f, 0.f), e.tier, 0, e.kind);   // A_VileStart: the scream as the arms go up
            } else if (e.attackTimer <= 0.f && !los && st.speed == 0.f) {
                e.attackTimer = 0.4f;   // stationary and no line of sight: retry soon
            }
            break;
        }
        case Enemy::State::Attack: {
            e.pos.y = restY;
            if (dist > 1e-3f) turnTowards(e.yaw, std::atan2(dir.x, dir.z), kChaseTurnRate * dt);   // keeps its aim through the attack frames
            if (st.attack == AttackKind::Vile) {
                const glm::vec3 vileEye = e.pos + glm::vec3(0.f, 1.2f, 0.f);
                const bool sight = lineOfSight(vileEye, playerCentre, game);
                if (e.stateT >= kVileFireDelay && !e.attacked) {
                    if (e.fireT < 0.f) {
                        e.fireT = 0.f;
                        e.firePos = playerPos_;
                        push(FpsEvent::Type::VileFire, e.firePos, e.tier, 0, e.kind);
                    } else {
                        if (e.fireT < 1.f && e.fireT + dt >= 1.f) push(FpsEvent::Type::VileFire, e.firePos, e.tier, 1, e.kind);   // A_FireCrackle
                        e.fireT += dt;
                    }
                    if (sight) e.firePos = playerPos_;   // the flame follows while it can see you; break sight and it stays put
                }
                if (e.stateT >= kVileWindup && !e.attacked) {
                    e.attacked = true;
                    e.attackTimer = st.attackInterval * (0.7f + 0.6f * u(rng_)) * crowd * cadence;
                    if (sight) {
                        hurtPlayer(st.damage, e.pos, e.kind);   // the clasp
                        const float pd = glm::length((playerPos_ + glm::vec3(0.f, 0.6f, 0.f)) - (e.firePos + glm::vec3(0.f, 0.3f, 0.f)));
                        enemyBlast(e.firePos + glm::vec3(0.f, 0.3f, 0.f), kVileBlastRadius, kVileBlastDamage, game, e.kind);
                        if (pd < kVileBlastRadius && health_ > 0.f) jumpVy_ = std::max(jumpVy_, kVileJump);   // the arch-vile jump
                        push(FpsEvent::Type::VileBlast, e.firePos, e.tier, 0, e.kind);
                        if (std::getenv("REDLINE_LOG_VILE")) std::fprintf(stderr, "[vile] clasp at %.2fs: hit, blast %.2f m from the player\n", e.stateT, pd);
                    } else if (std::getenv("REDLINE_LOG_VILE")) std::fprintf(stderr, "[vile] clasp at %.2fs: no line of sight, nothing happens\n", e.stateT);
                }
                if (e.stateT >= kVileWindup + kVileHold) { e.state = Enemy::State::Idle; e.stateT = 0.f; e.fireT = -1.f; }
                break;
            }
            if (e.stateT >= 0.35f && !e.attacked) {
                e.attacked = true;
                e.attackTimer = st.attackInterval * (0.7f + 0.6f * u(rng_)) * crowd * cadence;
                push(FpsEvent::Type::EnemyAttack, e.pos + glm::vec3(0.f, 1.f, 0.f), e.tier, 0, e.kind);
                switch (st.attack) {
                case AttackKind::Hitscan: {
                    e.flashT = 1.f;
                    if (lineOfSight(e.pos + glm::vec3(0.f, 1.2f, 0.f), playerCentre, game) && u(rng_) < 0.65f) hurtPlayer(st.damage, e.pos, e.kind);
                    break;
                }
                case AttackKind::Projectile: {
                    // Volleys (the mancubus) fan out around the aim; homing ones (the revenant) steer later.
                    for (int v = 0; v < std::max(1, st.volley); ++v) {
                        Projectile p;
                        p.pos = e.pos + glm::vec3(0.f, st.flies ? 0.9f : 1.0f, 0.f) + dir * 0.4f;
                        glm::vec3 aim = glm::normalize(playerCentre - p.pos);
                        if (st.volley > 1) {
                            const float spread = (static_cast<float>(v) - 0.5f * static_cast<float>(st.volley - 1)) * 0.28f;
                            aim = glm::normalize(glm::vec3(aim.x * std::cos(spread) - aim.z * std::sin(spread), aim.y, aim.x * std::sin(spread) + aim.z * std::cos(spread)));
                        }
                        p.vel = aim * st.projSpeed;
                        p.type = st.projectile;
                        p.damage = st.damage;
                        p.blast = st.projBlast;
                        p.homing = st.homing;
                        p.ownerKind = e.kind;
                        projectiles_.push_back(p);
                    }
                    break;
                }
                case AttackKind::Melee:
                    if (dist < 1.5f) hurtPlayer(st.damage, e.pos, e.kind);
                    break;
                case AttackKind::Vile:
                    break;   // handled above
                }
            }
            if (e.stateT >= 0.6f) { e.state = Enemy::State::Idle; e.stateT = 0.f; }
            break;
        }
        case Enemy::State::Pain:
            if (e.stateT >= 0.25f) { e.state = Enemy::State::Idle; e.stateT = 0.f; }
            break;
        case Enemy::State::Dying:
            if (e.stateT >= 0.45f && !e.exploded) explodeEnemy(e, game);
            if (e.stateT >= 0.8f) e.state = Enemy::State::Dead;
            break;
        case Enemy::State::Dead:
            break;
        }
    }

    // --- projectiles -----------------------------------------------------
    for (size_t i = 0; i < projectiles_.size();) {
        Projectile& p = projectiles_[i];
        glm::vec3 prev = p.pos;
        if (p.homing && !p.fromPlayer && health_ > 0.f) {   // revenant missiles bend towards the player, slowly enough to dodge
            const glm::vec3 to = glm::normalize((playerPos_ + glm::vec3(0.f, 1.0f, 0.f)) - p.pos);
            const float speed = glm::length(p.vel);
            p.vel = glm::normalize(glm::mix(glm::normalize(p.vel), to, std::min(1.f, 1.6f * dt))) * speed;
        }
        p.pos += p.vel * dt;
        p.ttl -= dt;
        p.animT += dt;
        bool remove = p.ttl <= 0.f || p.pos.y < 0.f || solidAt(p.pos, game) || outOfWorld(p.pos);
        if (p.fromPlayer) {
            for (Enemy& e : enemies_) {
                if (!e.alive() || e.state == Enemy::State::Emerging) continue;
                glm::vec2 d(p.pos.x - e.pos.x, p.pos.z - e.pos.z);
                if (glm::length(d) < e.radius + 0.15f && p.pos.y > e.pos.y - 0.1f && p.pos.y < e.pos.y + e.height + 0.1f) {
                    if (p.blast <= 0.f) damageEnemy(e, p.damage, p.pos, glm::normalize(p.vel), p.fromPlayer ? (p.type == kProjPlasma ? kPlasmaRifle : -1) : -1);
                    remove = true;
                    break;
                }
            }
        } else {
            glm::vec3 dp = p.pos - playerCentre;
            if (std::fabs(dp.x) < 0.45f && std::fabs(dp.z) < 0.45f && dp.y > -0.7f && dp.y < 0.8f) {
                hurtPlayer(p.damage, p.pos, p.ownerKind);
                remove = true;
            }
        }
        if (remove) {
            if (p.fromPlayer && p.blast > 0.f) {
                rocketBlast(prev, p.blast, p.damage, game);
            } else if (!p.fromPlayer && p.blast > 0.f) {
                enemyBlast(prev, p.blast, p.damage, game, p.ownerKind);
            } else {
                Explosion ex;
                ex.pos = prev;
                ex.duration = 0.25f;
                ex.radius = 0.4f;
                ex.hitType = p.type;
                explosions_.push_back(ex);
                push(p.fromPlayer ? FpsEvent::Type::PlasmaHit : FpsEvent::Type::FireballHit, p.pos);
            }
            projectiles_[i] = projectiles_.back();
            projectiles_.pop_back();
        } else {
            ++i;
        }
    }

    // --- pickups ---------------------------------------------------------
    for (size_t i = 0; i < pickups_.size();) {
        Pickup& p = pickups_[i];
        p.t += dt;
        if (!p.landed) {
            p.vel.y -= 12.f * dt;
            p.pos += p.vel * dt;
            if (p.pos.y <= 0.f) {
                p.pos.y = 0.f;
                p.landed = true;
                // Landed inside a block? Slide back to the monster's open pocket.
                if (solidAt(glm::vec3(p.pos.x, 0.5f, p.pos.z), game)) p.pos = p.home;
            }
        }
        bool taken = false;
        if (p.landed && health_ > 0.f) {
            glm::vec2 d(p.pos.x - playerPos_.x, p.pos.z - playerPos_.z);
            if (glm::length(d) < 0.7f) {
                bool useful = true;
                if (p.kind == PickupKind::Stim || p.kind == PickupKind::Medikit) useful = health_ < 100.f;
                if (useful) { applyPickup(p); taken = true; }
            }
        }
        if (taken) { pickups_[i] = pickups_.back(); pickups_.pop_back(); }
        else ++i;
    }

    // --- explosions & debris ------------------------------------------------
    for (size_t i = 0; i < explosions_.size();) {
        explosions_[i].t += dt;
        if (explosions_[i].t >= explosions_[i].duration) { explosions_[i] = explosions_.back(); explosions_.pop_back(); }
        else ++i;
    }
    updateGore(dt);
    for (size_t i = 0; i < debris_.size();) {
        Debris& d = debris_[i];
        if (d.homing) {
            d.pos += d.vel * dt;
            d.size = std::max(0.02f, d.size - dt * 0.3f);
        } else {
            d.vel.y -= 14.f * dt;
            d.pos += d.vel * dt;
            if (d.pos.y < d.size * 0.5f) { d.pos.y = d.size * 0.5f; d.vel.y *= -0.35f; d.vel.x *= 0.7f; d.vel.z *= 0.7f; }
        }
        d.ttl -= dt;
        if (d.ttl <= 0.f) { debris_[i] = debris_.back(); debris_.pop_back(); }
        else ++i;
    }

    // --- the boss hall's door ---------------------------------------------
    lockedHintT_ = std::max(0.f, lockedHintT_ - dt);
    if (stage_ == Stage::Dungeon && !doorOpen_ && !dungeon_.doorTiles().empty() && health_ > 0.f) {
        for (const auto& [ti, tj] : dungeon_.doorTiles()) {
            const glm::vec3 c = dungeon_.tileCentre(ti, tj);
            if (std::fabs(c.x - playerPos_.x) > 2.f || std::fabs(c.z - playerPos_.z) > 2.f) continue;
            if (hasKey_) { doorOpen_ = true; push(FpsEvent::Type::DoorOpened, c); }
            else if (lockedHintT_ <= 0.f) { lockedHintT_ = 4.f; push(FpsEvent::Type::DoorLocked, c); }
            break;
        }
    }

    // --- end conditions ----------------------------------------------------
    if (!finished_ && !in.warmup) {
        if (stage_ == Stage::Arena) {
            if (enemiesLeft() == 0) {
                finishDelay_ += dt;
                if (finishDelay_ >= 1.2f) {
                    if (dungeonEnabled_) {
                        // The arena is clear: the back wall comes down and the crypt behind it opens.
                        stage_ = Stage::GateFalling;
                        stageT_ = 0.f;
                        finishDelay_ = 0.f;
                        for (int k = 0; k < 6; ++k) spawnDebris(glm::vec3(-1.5f + static_cast<float>(k % 4), 0.5f + static_cast<float>(k / 4) * 2.f, -2.f), {0.6f, 0.6f, 0.6f}, 6, false);   // mortar flying off the wall
                        push(FpsEvent::Type::GateFalls, glm::vec3(0.f, 1.f, -2.5f));
                    } else {
                        finished_ = true;
                        push(FpsEvent::Type::AllClear);
                    }
                }
            }
        } else if (stage_ == Stage::GateFalling) {
            stageT_ += dt;
            if (stageT_ >= kGateFallTime) openDungeon(game);
        } else {
            stageT_ += dt;
            if (bossDead_) {
                finishDelay_ += dt;
                if (finishDelay_ >= 2.5f) { finished_ = true; push(FpsEvent::Type::AllClear); }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The dungeon.
bool FpsMode::outOfWorld(glm::vec3 p) const {
    if (stage_ == Stage::Dungeon) {
        const float x0 = std::min(-16.f, dungeon_.originX() - 1.f), x1 = std::max(16.f, dungeon_.originX() + static_cast<float>(dungeon_.width()) + 1.f);
        return p.x < x0 || p.x > x1 || p.z > 24.f || p.z < dungeon_.zMin() - 1.f;
    }
    return std::fabs(p.x) > 16.f || p.z > 24.f || p.z < -3.f;
}

const Enemy* FpsMode::boss() const {
    for (const Enemy& e : enemies_) if (e.boss) return &e;
    return nullptr;
}

void FpsMode::closeDungeon() {
    hasKey_ = doorOpen_ = false;
    stage_ = Stage::Arena;
    stageT_ = 0.f;
    dungeon_.clear();
}

void FpsMode::debugClearArena() {
    for (Enemy& e : enemies_) if (!e.inDungeon) { e.state = Enemy::State::Dead; e.exploded = true; }
}

void FpsMode::openDungeon(core::Game& game) {
    // The seed is the game that made it: the board's own seed, the fight number, and
    // the piece mix, so two players who stack differently get different crypts.
    uint32_t seed = game.seed() * 31337u + static_cast<uint32_t>(level_) * 7u + static_cast<uint32_t>(game.redLineCount()) * 104729u;
    for (size_t k = 0; k < game.pieceCounts().size(); ++k) seed = seed * 1664525u + static_cast<uint32_t>(game.pieceCounts()[k]) * 1013904223u;
    dungeon_.generate(seed, level_, game.pieceCounts());
    stage_ = Stage::Dungeon;
    stageT_ = 0.f;
    // The falling wall smashes a passage through the stack in front of the gate.
    for (int r = core::kBoardH - 7; r < core::kBoardH; ++r)
        for (int c = 3; c < 7; ++c)
            if (!game.at(c, r).empty()) {
                const bool wasRed = game.at(c, r).red();
                const int shade = game.at(c, r).color;
                game.clearCell(c, r);
                spawnDebris(flatCellCentre(c, r), wasRed ? glm::vec3(0.9f, 0.1f, 0.1f) : glm::vec3(0.6f, 0.6f, 0.6f), 4, wasRed, wasRed ? -1 : shade);
            }
    Explosion ex;
    ex.pos = glm::vec3(0.f, 0.8f, -1.f);
    ex.radius = 3.f;
    ex.duration = 0.7f;
    explosions_.push_back(ex);
    populateDungeon();
    push(FpsEvent::Type::DungeonOpen, glm::vec3(0.f, 1.f, Dungeon::kZTop));
    if (std::getenv("REDLINE_LOG_DUNGEON")) {   // the layout, top down (the gate at the top, x to the right)
        for (int j = 0; j < dungeon_.depth(); ++j) {
            std::string row;
            for (int i = 0; i < dungeon_.width(); ++i) {
                const Dungeon::Tile t = dungeon_.tile(i, j);
                char ch = t == Dungeon::Floor ? '.' : t == Dungeon::Wall ? '#' : t == Dungeon::Door ? 'D' : ' ';
                const glm::vec3 c = dungeon_.tileCentre(i, j);
                for (const Enemy& e : enemies_) if (e.inDungeon && std::fabs(e.pos.x - c.x) < 0.5f && std::fabs(e.pos.z - c.z) < 0.5f) ch = e.boss ? 'B' : 'm';
                for (const Pickup& p : pickups_) if (std::fabs(p.home.x - c.x) < 0.5f && std::fabs(p.home.z - c.z) < 0.5f) ch = p.kind == PickupKind::Key ? 'K' : '+';
                row += ch;
            }
            std::fprintf(stderr, "[dungeon] %s\n", row.c_str());
        }
    }
}

// A rough battle simulation, after the one in Obsidian's level generator: fight the
// roster one monster at a time with the best weapon to hand, strongest first, and
// add up what it costs in health and ammunition. What the player is short of is
// what the crypt leaves on the floor, which beats guessing from the level number.
FpsMode::FightBudget FpsMode::simulateFight(const std::vector<int>& kinds) const {
    FightBudget b;
    const float hpScale = std::clamp(0.45f + 0.15f * static_cast<float>(level_ - 1), 0.45f, 2.5f);
    // Strongest first, the order a player would take them in.
    std::vector<int> order = kinds;
    std::sort(order.begin(), order.end(), [](int a, int c) { return enemyStats(a).hp > enemyStats(c).hp; });
    // Ammunition the player is carrying, spent as the simulation runs.
    float have[kWeaponCount];
    for (int w = 0; w < kWeaponCount; ++w) have[w] = slots_[w].owned ? static_cast<float>(slots_[w].ammo) : 0.f;
    for (int kind : order) {
        const EnemyStats& st = enemyStats(kind);
        const float hp = st.hp * hpScale;
        // The best weapon still holding ammunition; the shotgun never runs dry.
        int use = kShotgun;
        float bestDps = weaponDef(kShotgun).damage / weaponDef(kShotgun).cycle;
        for (int w = 1; w < kWeaponCount; ++w) {
            if (!slots_[w].owned || have[w] <= 0.f) continue;
            const float dps = weaponDef(w).damage / weaponDef(w).cycle;
            if (dps > bestDps) { bestDps = dps; use = w; }
        }
        const WeaponDef& wd = weaponDef(use);
        const float seconds = hp / std::max(1.f, bestDps);
        if (wd.ammoPerPickup > 0) {
            const float rounds = seconds / wd.cycle;
            b.ammo[use] += rounds;
            have[use] = std::max(0.f, have[use] - rounds);
        }
        // What it does back: its own damage rate, for as long as it takes to kill,
        // discounted because the player is moving, using cover and not always in its
        // line of fire. Melee has to reach you first, so it lands less.
        const float incoming = st.damage / std::max(0.3f, st.attackInterval);
        const float exposure = st.attack == AttackKind::Melee ? 0.22f : 0.34f;
        b.health += incoming * seconds * exposure;
    }
    return b;
}

// Monsters for the rooms, scaled by the level and by how much of a stack there was:
// more blocks, more (and tougher) demons and a bigger boss.
void FpsMode::populateDungeon() {
    std::uniform_real_distribution<float> u(0.f, 1.f);
    const int L = std::max(1, level_);
    const float hpScale = std::clamp(0.45f + 0.15f * static_cast<float>(L - 1), 0.45f, 2.5f);
    const int capTier = std::min(4, maxTierForLevel(L));   // the big two are the boss's job
    // The roster is planned first and only then spawned, so the battle simulation can
    // look at the whole fight and cut it back if it would be hopeless.
    struct Planned { int tier, kind; glm::vec3 pos; float hpMul; bool boss; };
    std::vector<Planned> plan;
    auto make = [&](int tier, glm::vec3 pos, float hpMul, bool boss) {
        int kind = boss ? tier : pickKind(tier);
        if (enemyStats(kind).flies) kind = kindAvailable_[8] && u(rng_) < 0.5f ? 8 : 2;   // no drifting through walls: a hell knight or a demon instead
        plan.push_back({tier, kind, pos, hpMul, boss});
    };
    auto spawn = [&](const Planned& pl) {
        Enemy e;
        e.tier = pl.tier;
        e.kind = pl.kind;
        const EnemyStats& st = enemyStats(e.kind);
        const glm::vec3 pos = pl.pos;
        const float hpMul = pl.hpMul;
        const bool boss = pl.boss;
        e.pos = pos;
        e.maxHp = e.hp = st.hp * hpScale * hpMul;
        e.radius = st.radius;
        e.height = st.height;
        e.state = Enemy::State::Idle;
        e.stateT = 0.f;
        e.attackTimer = 1.f;
        e.breakTimer = 1e9f;   // nothing to chew on down here
        e.absorbTimer = 1e9f;
        e.bobPhase = u(rng_) * 6.28f;
        e.yaw = u(rng_) * 6.28f;
        e.moveDir = glm::vec3(std::sin(e.yaw), 0.f, std::cos(e.yaw));
        e.dormant = true;
        e.inDungeon = true;
        e.boss = boss;
        e.senseT = u(rng_) * 0.3f;
        enemies_.push_back(e);
    };
    const int count = std::clamp(1 + L + boardBlocks_ / 14, 3, 36);
    for (const glm::vec3& p : dungeon_.spawnSpots(rng_, count)) {
        const float roll = std::pow(u(rng_), 1.7f);   // most are small fry
        make(std::min(capTier, static_cast<int>(roll * static_cast<float>(capTier + 1))), p, 1.f, false);
    }
    // The boss: a baron early on, then a cyberdemon, then the mastermind; its health
    // grows with the level and with the stack it came from.
    const int bossTier = L <= 2 ? 4 : L <= 5 ? 5 : 6;
    const float bossHp = 1.0f + static_cast<float>(boardBlocks_) / 80.f + 0.1f * static_cast<float>(L);
    const DungeonRoom& hall = dungeon_.bossRoom();
    make(bossTier, dungeon_.bossStand(), bossHp, true);
    for (const glm::vec3& p : dungeon_.bossGuardSpots(rng_, 2 + L / 3)) make(std::min(capTier, 1 + L / 3), p, 1.f, false);

    // Simulate the whole roster. What the player can bring to it is what they carry
    // plus the items the crypt can hold plus the heal each kill gives. If the fight
    // still costs more than that it is not a fight, so the weakest are dropped until
    // it is one.
    auto costOf = [&](const std::vector<Planned>& p) {
        std::vector<int> kinds;
        float bossMul = 1.f;
        for (const Planned& pl : p) { kinds.push_back(pl.kind); if (pl.boss) bossMul = pl.hpMul; }
        FightBudget b = simulateFight(kinds);
        b.health *= 1.f + 0.25f * (bossMul - 1.f);   // the boss's extra health is extra time under fire
        return b;
    };
    FightBudget need = costOf(plan);
    const int kept0 = static_cast<int>(plan.size());
    while (plan.size() > 8) {
        const float affordable = health_ + shield_ * 0.5f + 300.f + 5.f * static_cast<float>(plan.size());
        if (need.health <= affordable) break;
        // Drop the weakest that is not the boss.
        size_t weakest = plan.size();
        float worst = 1e9f;
        for (size_t k = 0; k < plan.size(); ++k) {
            if (plan[k].boss) continue;
            const float hp = enemyStats(plan[k].kind).hp;
            if (hp < worst) { worst = hp; weakest = k; }
        }
        if (weakest >= plan.size()) break;
        plan.erase(plan.begin() + static_cast<long>(weakest));
        need = costOf(plan);
    }
    if (kept0 != static_cast<int>(plan.size()) && std::getenv("REDLINE_LOG_DUNGEON"))
        std::fprintf(stderr, "[dungeon] roster trimmed from %d to %zu: the fight did not fit what the player could carry\n", kept0, plan.size());
    for (const Planned& pl : plan) spawn(pl);

    // What the crypt leaves lying about comes from the same simulation: whatever the
    // roster is expected to cost that the player cannot already pay for.
    std::vector<PickupKind> wanted;
    // Health: what the fight costs beyond what is carried, in medikits with stims to round off.
    float healthShort = need.health - (health_ + shield_ * 0.5f - 30.f);   // leave 30 in hand at the end
    for (int n = 0; n < 12 && healthShort > 0.f; ++n) {
        if (healthShort > 18.f) { wanted.push_back(PickupKind::Medikit); healthShort -= 25.f; }
        else { wanted.push_back(PickupKind::Stim); healthShort -= 10.f; }
    }
    // Ammunition: per weapon, whatever the simulation spends past what is carried.
    const PickupKind ammoFor[kWeaponCount] = {PickupKind::Bullets, PickupKind::Bullets, PickupKind::Rockets, PickupKind::Cells};
    for (int w = 1; w < kWeaponCount; ++w) {
        if (!slots_[w].owned) continue;
        float shortfall = need.ammo[w] - static_cast<float>(slots_[w].ammo);
        for (int n = 0; n < 8 && shortfall > 0.f; ++n) {
            wanted.push_back(ammoFor[w]);
            shortfall -= static_cast<float>(weaponDef(w).ammoPerPickup);
        }
    }
    // A weapon the player has not got yet is worth more than any of it.
    for (int w = kChaingun; w <= kPlasmaRifle; ++w)
        if (!slots_[w].owned && level_ >= w) wanted.push_back(w == kChaingun ? PickupKind::Chaingun : w == kRocketLauncher ? PickupKind::RocketLauncher : PickupKind::PlasmaGun);
    if (wanted.empty()) wanted.push_back(PickupKind::Stim);
    // Spread them over the rooms, and always leave one medikit in the hall itself.
    std::vector<glm::vec3> spots = dungeon_.spawnSpots(rng_, static_cast<int>(wanted.size()));
    for (size_t k = 0; k < wanted.size() && k < spots.size(); ++k) {
        Pickup p;
        p.kind = wanted[k];
        p.home = spots[k];
        p.pos = spots[k];
        p.landed = true;
        pickups_.push_back(p);
    }
    if (std::getenv("REDLINE_LOG_DUNGEON"))
        std::fprintf(stderr, "[dungeon] budget: %.0f health and %.0f/%.0f/%.0f rounds needed, carrying %.0f health %d armour -> %zu items\n",
                     need.health, need.ammo[kChaingun], need.ammo[kRocketLauncher], need.ammo[kPlasmaRifle], health_, static_cast<int>(shield_), wanted.size());
    {
        const glm::vec3 c = dungeon_.tileCentre(hall.x0 + 1, hall.z0 + 1);
        spawnPickup(PickupKind::Medikit, c + glm::vec3(0.f, 0.6f, 0.f), c);
    }
    // The key to the hall, in the room furthest from it. Without a key the hall is
    // simply open, so a crypt too small to hide one is still finishable.
    if (dungeon_.hasKey()) {
        Pickup k;
        k.kind = PickupKind::Key;
        k.pos = dungeon_.keyPos() + glm::vec3(0.f, 0.55f, 0.f);
        k.home = dungeon_.keyPos();
        k.landed = true;
        k.pos.y = 0.f;
        pickups_.push_back(k);
    } else {
        doorOpen_ = true;
    }
}

// Breadth-first search over 1 m tiles of everything walkable (the board's floor and the
// dungeon's), for the test bot. `next` is the centre of the first tile along the way.
bool FpsMode::navNext(glm::vec3 from, glm::vec3 to, glm::vec3& next, const core::Game& game) const {
    const float x0 = stage_ == Stage::Dungeon ? std::min(-16.f, dungeon_.originX()) : -16.f;
    const float x1 = stage_ == Stage::Dungeon ? std::max(16.f, dungeon_.originX() + static_cast<float>(dungeon_.width())) : 16.f;
    const float z0 = stage_ == Stage::Dungeon ? dungeon_.zMin() : -3.f;
    const int W = static_cast<int>(x1 - x0), D = static_cast<int>(24.f - z0);
    auto idx = [&](int i, int j) { return j * W + i; };
    auto centre = [&](int i, int j) { return glm::vec3(x0 + static_cast<float>(i) + 0.5f, 0.f, z0 + static_cast<float>(j) + 0.5f); };
    auto open = [&](int i, int j) {
        if (i < 0 || i >= W || j < 0 || j >= D) return false;
        const glm::vec3 c = centre(i, j) + glm::vec3(0.f, 0.5f, 0.f);
        const float r = 0.32f;
        return !solidAt(c, game) && !solidAt(c + glm::vec3(r, 0.f, r), game) && !solidAt(c + glm::vec3(-r, 0.f, r), game)
            && !solidAt(c + glm::vec3(r, 0.f, -r), game) && !solidAt(c + glm::vec3(-r, 0.f, -r), game);
    };
    auto tileOf = [&](glm::vec3 p, int& i, int& j) { i = static_cast<int>(std::floor(p.x - x0)); j = static_cast<int>(std::floor(p.z - z0)); };
    int si, sj, gi, gj;
    tileOf(from, si, sj);
    tileOf(to, gi, gj);
    if (si < 0 || si >= W || sj < 0 || sj >= D) return false;
    // The goal may stand against a wall (a fat monster): aim for the nearest open tile around it.
    if (!open(gi, gj)) {
        bool found = false;
        for (int ring = 1; ring <= 3 && !found; ++ring)
            for (int dj = -ring; dj <= ring && !found; ++dj)
                for (int di = -ring; di <= ring && !found; ++di)
                    if (open(gi + di, gj + dj)) { gi += di; gj += dj; found = true; }
        if (!found) return false;
    }
    if (si == gi && sj == gj) { next = centre(gi, gj); return true; }
    std::vector<int> parent(static_cast<size_t>(W * D), -1);
    std::deque<std::pair<int, int>> q;
    q.push_back({si, sj});
    parent[static_cast<size_t>(idx(si, sj))] = idx(si, sj);
    const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
    bool reached = false;
    while (!q.empty() && !reached) {
        auto [i, j] = q.front();
        q.pop_front();
        for (int k = 0; k < 4; ++k) {
            const int ni = i + di[k], nj = j + dj[k];
            if (!open(ni, nj) || parent[static_cast<size_t>(idx(ni, nj))] >= 0) continue;
            parent[static_cast<size_t>(idx(ni, nj))] = idx(i, j);
            if (ni == gi && nj == gj) { reached = true; break; }
            q.push_back({ni, nj});
        }
    }
    if (!reached) return false;
    int cur = idx(gi, gj);
    while (parent[static_cast<size_t>(cur)] != idx(si, sj)) cur = parent[static_cast<size_t>(cur)];
    next = centre(cur % W, cur / W);
    return true;
}

}  // namespace rl::game
