#include "game/fps_mode.h"

#include <algorithm>
#include <cmath>

namespace rl::game {

namespace {
constexpr float kPi = 3.14159265f;
constexpr float kMoveSpeed = 5.0f;
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
    {"DEMON",      150.f, 0.50f, 1.6f, AttackKind::Melee,      -1,          3.6f, false, 1.0f, 14.f, 0.f,  350,  5.f,  0, 0.f},
    {"CACODEMON",  400.f, 0.65f, 1.8f, AttackKind::Projectile, kProjCaco,   1.8f, true,  2.4f, 16.f, 10.f, 600,  3.5f, 0, 0.f},
    {"BARON",     1000.f, 0.70f, 2.3f, AttackKind::Projectile, kProjBaron,  1.2f, false, 2.8f, 28.f, 10.f, 1200, 2.5f, 1, 0.f},
    {"CYBERDEMON",2200.f, 0.90f, 3.3f, AttackKind::Projectile, kProjRocket, 1.6f, false, 3.0f, 40.f, 14.f, 2500, 2.0f, 1, 2.0f},
    {"SPIDER",    2600.f, 1.20f, 3.0f, AttackKind::Hitscan,    -1,          1.4f, false, 0.9f, 8.f,  0.f,  3000, 1.5f, 1, 0.f},
};

float absorbPeriodForLevel(float base, int level) { return std::max(8.f, base - 1.f * static_cast<float>(level - 1)); }

//                 name              cycle  dmg   proj   type          speed  blast ammo max  spread
const WeaponDef kWeapons[kWeaponCount] = {
    {"SHOTGUN",        0.75f, 40.f,  false, -1,           0.f,   0.f,  0,   0,   0.f},
    {"CHAINGUN",       0.10f, 12.f,  false, -1,           0.f,   0.f,  60,  200, 0.03f},
    {"ROCKET LAUNCHER",0.80f, 110.f, true,  kProjRocket,  22.f,  2.2f, 4,   30,  0.f},
    {"PLASMA RIFLE",   0.12f, 22.f,  true,  kProjPlasma,  28.f,  0.f,  40,  200, 0.01f},
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

const EnemyStats& enemyStats(int tier) { return kStats[std::clamp(tier, 0, kMaxTier)]; }
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
            const EnemyStats& st = enemyStats(tier);
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
    push(FpsEvent::Type::EnemySight, playerPos_, enemies_.empty() ? 0 : enemies_.front().tier, static_cast<int>(enemies_.size()));
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

void FpsMode::spawnDebris(const glm::vec3& pos, const glm::vec3& color, int count, bool red) {
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    for (int i = 0; i < count; ++i) {
        Debris d;
        d.pos = pos + glm::vec3(u(rng_), u(rng_), u(rng_)) * 0.3f;
        d.vel = glm::vec3(u(rng_) * 4.f, 3.5f + u(rng_) * 3.f, u(rng_) * 4.f);
        d.color = color;
        d.ttl = 1.2f + 0.8f * std::fabs(u(rng_));
        d.size = 0.12f + 0.15f * std::fabs(u(rng_));
        d.red = red;
        debris_.push_back(d);
    }
}

void FpsMode::hurtPlayer(float dmg, glm::vec3 from) {
    if (health_ <= 0.f || god_ || invulnT_ > 0.f) return;
    // Armour soaks half of any hit until it is spent.
    if (shield_ > 0.f) {
        float absorbed = std::min(shield_, dmg * 0.5f);
        shield_ -= absorbed;
        dmg -= absorbed;
    }
    health_ = std::max(0.f, health_ - dmg);
    damageFlash_ = 1.f;
    push(FpsEvent::Type::PlayerHit, from);
    if (health_ <= 0.f) push(FpsEvent::Type::PlayerDead, from);
}

// ---------------------------------------------------------------------------
void FpsMode::damageEnemy(Enemy& e, float dmg, glm::vec3 hitPos) {
    if (!e.alive()) return;
    e.hp -= dmg;
    push(FpsEvent::Type::EnemyHit, hitPos, e.tier);
    spawnDebris(hitPos, {0.6f, 0.05f, 0.05f}, 3, true);
    if (e.hp <= 0.f) {
        health_ = std::max(health_, std::min(100.f, health_ + 5.f));   // small heal per kill keeps long fights winnable
        e.state = Enemy::State::Dying;
        e.stateT = 0.f;
        e.animT = 0.f;
        push(FpsEvent::Type::EnemyDied, e.pos, e.tier);
        dropLoot(e);
    } else if (e.state != Enemy::State::Emerging) {
        e.state = Enemy::State::Pain;
        e.stateT = 0.f;
        e.attacked = false;
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

void FpsMode::hitscan(glm::vec3 o, glm::vec3 d, float damage, core::Game& game) {
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
        if (blockT < 60.f) spawnDebris(o + d * blockT, {0.6f, 0.6f, 0.6f}, 2, false);
        return;
    }
    damageEnemy(*best, damage * (bestT < 2.5f ? 1.4f : 1.f), o + d * bestT);
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
    if (w.spread > 0.f) {
        std::uniform_real_distribution<float> u(-1.f, 1.f);
        glm::vec3 right(-std::cos(yaw_), 0.f, std::sin(yaw_));
        glm::vec3 up = glm::normalize(glm::cross(right, d));
        d = glm::normalize(d + right * (u(rng_) * w.spread) + up * (u(rng_) * w.spread));
    }
    if (!w.projectile) {
        hitscan(o, d, w.damage, game);
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
        if (d < radius + e.radius) damageEnemy(e, damage * (1.f - std::max(0.f, d - e.radius) / radius), c);
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
                game.clearCell(x, y);
                spawnDebris(flatCellCentre(x, y), {0.85f, 0.85f, 0.85f}, 5, false);
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
void FpsMode::enemyBlast(glm::vec3 pos, float radius, float damage, core::Game& game) {
    float pd = glm::length((playerPos_ + glm::vec3(0.f, 0.6f, 0.f)) - pos);
    if (pd < radius) hurtPlayer(damage * (1.f - pd / radius), pos);
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
    for (auto [c, r] : e.cells) {
        for (int y = r - 2; y <= r + 2; ++y)
            for (int x = c - 2; x <= c + 2; ++x) {
                if (x < 0 || x >= core::kBoardW || y < 0 || y >= core::kBoardH) continue;
                float dx = static_cast<float>(x - c), dy = static_cast<float>(y - r);
                if (dx * dx + dy * dy > kExplosionRadius * kExplosionRadius + 1e-4f) continue;
                if (game.at(x, y).kind == core::CellKind::Normal) victims.push_back({x, y});
            }
        destroyed += game.explodeAt(c, r, kExplosionRadius);
    }
    for (auto [x, y] : victims) spawnDebris(flatCellCentre(x, y), {0.85f, 0.85f, 0.85f}, 5, false);
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
    const EnemyStats& st = enemyStats(e.tier);
    int points = static_cast<int>(static_cast<float>(st.scoreValue) * (1.f + 0.1f * static_cast<float>(level_ - 1))) + 25 * destroyed;
    game.addScore(points);
    push(FpsEvent::Type::Score, e.pos + glm::vec3(0.f, 1.5f, 0.f), e.tier, points);
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
    game.clearCell(c, r);
    spawnDebris(flatCellCentre(c, r), {0.85f, 0.85f, 0.85f}, 5, false);
    Explosion ex;
    ex.pos = flatCellCentre(c, r);
    ex.radius = 0.9f;
    ex.duration = 0.35f;
    explosions_.push_back(ex);
}

void FpsMode::breakBlock(Enemy& e, core::Game& game) {
    const EnemyStats& st = enemyStats(e.tier);
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
    if (e.tier >= growCap || u(rng_) > 0.75f) return;
    int ec, er;
    if (!flatToCell(glm::vec3(e.pos.x, 0.5f, e.pos.z), ec, er)) return;
    std::vector<std::pair<int, int>> food;
    for (int y = er - 3; y <= er + 3; ++y)
        for (int x = ec - 3; x <= ec + 3; ++x) {
            if (x < 0 || x >= core::kBoardW || y < 0 || y >= core::kBoardH) continue;
            float dx = static_cast<float>(x - ec), dy = static_cast<float>(y - er);
            if (dx * dx + dy * dy > 2.5f * 2.5f) continue;
            if (game.at(x, y).kind == core::CellKind::Normal) food.push_back({x, y});
        }
    if (food.empty()) return;
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
    const EnemyStats& st = enemyStats(e.tier);
    float hpScale = std::clamp(0.45f + 0.15f * static_cast<float>(level_ - 1), 0.45f, 2.5f);
    e.maxHp = e.hp = st.hp * hpScale;
    e.radius = st.radius;
    e.height = st.height;
    e.growT = 1.f;
    e.breakTimer = st.breakInterval * 0.5f;
    e.attackTimer = 1.0f;
    e.state = Enemy::State::Idle;
    e.stateT = 0.f;
    push(FpsEvent::Type::Absorb, e.pos + glm::vec3(0.f, 1.f, 0.f), e.tier, static_cast<int>(food.size()));
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
    int alive = enemiesLeft();
    float crowd = std::max(1.f, static_cast<float>(alive) * 0.35f);
    float cadence = std::max(0.5f, 1.f - 0.06f * static_cast<float>(level_ - 1));
    glm::vec3 playerCentre = playerPos_ + glm::vec3(0.f, 0.6f, 0.f);
    for (Enemy& e : enemies_) {
        const EnemyStats& st = enemyStats(e.tier);
        e.stateT += dt;
        e.animT += dt;
        e.flashT = std::max(0.f, e.flashT - dt * 6.f);
        e.growT = std::max(0.f, e.growT - dt * 1.5f);
        e.ammoDropCooldown = std::max(0.f, e.ammoDropCooldown - dt);
        if (in.warmup && e.state != Enemy::State::Emerging) {   // countdown: hold position
            e.pos.y = st.flies ? 0.8f : 0.f;
            continue;
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
            // Rise out of the board.
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
            if (st.speed > 0.f && health_ > 0.f) {
                float want = (st.attack == AttackKind::Melee) ? 0.9f : 5.f;
                if (dist > want) {
                    glm::vec3 delta = dir * st.speed * dt;
                    if (st.flies) e.pos += delta;
                    else moveWithCollision(e.pos, delta, e.radius, game);
                }
            }
            e.attackTimer -= dt;
            bool canAttack = health_ > 0.f && e.attackTimer <= 0.f;
            if (st.attack == AttackKind::Melee) canAttack = canAttack && dist < 1.3f;
            else canAttack = canAttack && los;
            if (canAttack) {
                e.state = Enemy::State::Attack;
                e.stateT = 0.f;
                e.animT = 0.f;
                e.attacked = false;
            } else if (e.attackTimer <= 0.f && !los && st.speed == 0.f) {
                e.attackTimer = 0.4f;   // stationary and no line of sight: retry soon
            }
            break;
        }
        case Enemy::State::Attack: {
            e.pos.y = restY;
            if (e.stateT >= 0.35f && !e.attacked) {
                e.attacked = true;
                e.attackTimer = st.attackInterval * (0.7f + 0.6f * u(rng_)) * crowd * cadence;
                push(FpsEvent::Type::EnemyAttack, e.pos + glm::vec3(0.f, 1.f, 0.f), e.tier);
                switch (st.attack) {
                case AttackKind::Hitscan: {
                    e.flashT = 1.f;
                    if (lineOfSight(e.pos + glm::vec3(0.f, 1.2f, 0.f), playerCentre, game) && u(rng_) < 0.65f) hurtPlayer(st.damage, e.pos);
                    break;
                }
                case AttackKind::Projectile: {
                    Projectile p;
                    p.pos = e.pos + glm::vec3(0.f, st.flies ? 0.9f : 1.0f, 0.f) + dir * 0.4f;
                    p.vel = glm::normalize(playerCentre - p.pos) * st.projSpeed;
                    p.type = st.projectile;
                    p.damage = st.damage;
                    p.blast = st.projBlast;
                    projectiles_.push_back(p);
                    break;
                }
                case AttackKind::Melee:
                    if (dist < 1.5f) hurtPlayer(st.damage, e.pos);
                    break;
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
        p.pos += p.vel * dt;
        p.ttl -= dt;
        p.animT += dt;
        bool remove = p.ttl <= 0.f || p.pos.y < 0.f || solidAt(p.pos, game) || std::fabs(p.pos.x) > 16.f || p.pos.z > 24.f || p.pos.z < -3.f;
        if (p.fromPlayer) {
            for (Enemy& e : enemies_) {
                if (!e.alive() || e.state == Enemy::State::Emerging) continue;
                glm::vec2 d(p.pos.x - e.pos.x, p.pos.z - e.pos.z);
                if (glm::length(d) < e.radius + 0.15f && p.pos.y > e.pos.y - 0.1f && p.pos.y < e.pos.y + e.height + 0.1f) {
                    if (p.blast <= 0.f) damageEnemy(e, p.damage, p.pos);
                    remove = true;
                    break;
                }
            }
        } else {
            glm::vec3 dp = p.pos - playerCentre;
            if (std::fabs(dp.x) < 0.45f && std::fabs(dp.z) < 0.45f && dp.y > -0.7f && dp.y < 0.8f) {
                hurtPlayer(p.damage, p.pos);
                remove = true;
            }
        }
        if (remove) {
            if (p.fromPlayer && p.blast > 0.f) {
                rocketBlast(prev, p.blast, p.damage, game);
            } else if (!p.fromPlayer && p.blast > 0.f) {
                enemyBlast(prev, p.blast, p.damage, game);
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

    // --- end conditions ----------------------------------------------------
    if (!finished_ && !in.warmup && enemiesLeft() == 0) {
        finishDelay_ += dt;
        if (finishDelay_ >= 1.2f) {
            finished_ = true;
            push(FpsEvent::Type::AllClear);
        }
    }
}

}  // namespace rl::game
