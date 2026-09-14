#include "game/fps_mode.h"

#include <algorithm>
#include <cmath>

namespace rl::game {

namespace {
constexpr float kPi = 3.14159265f;
constexpr float kMoveSpeed = 4.5f;
constexpr float kMouseSens = 0.0022f;
constexpr float kShotDamage = 40.f;
constexpr float kPlayerRadius = 0.3f;
constexpr float kExplosionRadius = 1.5f;   // cells, around every cell of the region
constexpr float kBlockTop = 1.0f;          // blocks are unit cubes on the floor

//               name         hp     r      h     attack                 proj speed flies interval dmg   pspd  score
const EnemyStats kStats[] = {
    {"ZOMBIE",     20.f, 0.35f, 1.5f, AttackKind::Hitscan,    -1, 0.f,  false, 2.2f, 5.f,  0.f,  100},
    {"IMP",        60.f, 0.40f, 1.7f, AttackKind::Projectile,  0, 0.f,  false, 2.6f, 10.f, 8.f,  200},
    {"DEMON",     150.f, 0.50f, 1.6f, AttackKind::Melee,      -1, 3.6f, false, 1.0f, 14.f, 0.f,  350},
    {"CACODEMON", 400.f, 0.65f, 1.8f, AttackKind::Projectile,  1, 1.8f, true,  2.4f, 16.f, 10.f, 600},
    {"BARON",    1000.f, 0.70f, 2.3f, AttackKind::Projectile,  2, 1.2f, false, 2.8f, 28.f, 10.f, 1200},
};

int tierForRegion(int size) {
    if (size <= 1) return 0;
    if (size <= 3) return 1;
    if (size <= 6) return 2;
    if (size <= 11) return 3;
    return 4;
}
}  // namespace

const EnemyStats& enemyStats(int tier) { return kStats[std::clamp(tier, 0, 4)]; }

FpsMode::FpsMode() : rng_(12345) {}

glm::vec3 FpsMode::forward() const {
    return glm::normalize(glm::vec3(std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_), std::cos(yaw_) * std::cos(pitch_)));
}

float FpsMode::recoil() const {
    if (gunT_ >= gunCycle_) return 0.f;
    float t = gunT_ / gunCycle_;
    return std::sin(t * kPi) * (1.f - t);
}

// ---------------------------------------------------------------------------
void FpsMode::begin(core::Game& game, int level) {
    enemies_.clear();
    projectiles_.clear();
    explosions_.clear();
    debris_.clear();
    events_.clear();
    finished_ = false;
    finishDelay_ = 0.f;
    elapsed_ = 0.f;
    health_ = 100.f;
    damageFlash_ = 0.f;
    gunT_ = 10.f;
    level_ = level;
    yaw_ = kPi;
    pitch_ = 0.f;
    rng_.seed(game.seed() * 7919u + static_cast<uint32_t>(game.redLineCount()) * 104729u);
    std::uniform_real_distribution<float> u(0.f, 1.f);

    // Difficulty scaling with level: health, chance of upgrading a tier, cadence.
    float hpScale = std::clamp(0.45f + 0.15f * static_cast<float>(level - 1), 0.45f, 2.5f);
    float upgradeChance = std::min(0.5f, 0.08f * static_cast<float>(level - 1));

    for (const core::RedRegion& region : core::findRedRegions(game)) {
        Enemy e;
        int tier = tierForRegion(region.size());
        float roll = u(rng_);
        if (roll < upgradeChance && tier < 4) ++tier;                      // random nastier
        else if (roll > 0.85f && tier > 0 && region.size() > 1) --tier;    // random lucky break
        e.tier = tier;
        const EnemyStats& st = enemyStats(tier);
        e.cells = region.cells;
        e.pos = flatCellFloor(region.anchorCol, region.anchorRow);
        e.pos.y = st.flies ? 0.8f : 0.f;
        e.maxHp = e.hp = st.hp * hpScale;
        e.radius = st.radius;
        e.height = st.height;
        e.attackTimer = (1.2f + 2.0f * u(rng_)) * 1.5f;
        e.bobPhase = u(rng_) * 6.28f;
        e.stateT = -0.12f * static_cast<float>(enemies_.size());   // stagger the emergence
        // The region's cells become the monster's body: clear them so the
        // pocket it stands in is open.
        for (auto [c, r] : region.cells) game.clearCell(c, r);
        enemies_.push_back(e);
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
    if (health_ <= 0.f) return;
    health_ = std::max(0.f, health_ - dmg);
    damageFlash_ = 1.f;
    push(FpsEvent::Type::PlayerHit, from);
    if (health_ <= 0.f) push(FpsEvent::Type::PlayerDead, from);
}

// ---------------------------------------------------------------------------
void FpsMode::fire(core::Game& game) {
    gunT_ = 0.f;
    push(FpsEvent::Type::Shoot, eye());
    glm::vec3 o = eye();
    glm::vec3 d = forward();
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
        if (blockT < 60.f) spawnDebris(o + d * blockT, {0.6f, 0.6f, 0.6f}, 3, false);
        return;
    }
    best->hp -= kShotDamage * (bestT < 2.5f ? 1.5f : 1.f);
    glm::vec3 hitPos = o + d * bestT;
    push(FpsEvent::Type::EnemyHit, hitPos, best->tier);
    spawnDebris(hitPos, {0.6f, 0.05f, 0.05f}, 4, true);
    if (best->hp <= 0.f) {
        health_ = std::min(100.f, health_ + 5.f);   // small heal per kill keeps long fights winnable
        best->state = Enemy::State::Dying;
        best->stateT = 0.f;
        best->animT = 0.f;
        push(FpsEvent::Type::EnemyDied, best->pos, best->tier);
    } else if (best->state != Enemy::State::Emerging) {
        best->state = Enemy::State::Pain;
        best->stateT = 0.f;
        best->attacked = false;
    }
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
    if (health_ > 0.f) moveWithCollision(playerPos_, move * kMoveSpeed * dt, kPlayerRadius, game);
    playerPos_.y = 0.f;
    damageFlash_ = std::max(0.f, damageFlash_ - dt * 2.5f);

    // --- weapon ----------------------------------------------------------
    gunT_ += dt;
    if (in.fire && gunT_ >= gunCycle_ && health_ > 0.f) fire(game);

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
        glm::vec3 dp = p.pos - playerCentre;
        if (std::fabs(dp.x) < 0.45f && std::fabs(dp.z) < 0.45f && dp.y > -0.7f && dp.y < 0.8f) {
            hurtPlayer(p.damage, p.pos);
            remove = true;
        }
        if (remove) {
            Explosion ex;
            ex.pos = prev;
            ex.duration = 0.25f;
            ex.radius = 0.4f;
            ex.hitType = p.type;
            explosions_.push_back(ex);
            push(FpsEvent::Type::FireballHit, p.pos);
            projectiles_[i] = projectiles_.back();
            projectiles_.pop_back();
        } else {
            ++i;
        }
    }

    // --- explosions & debris ------------------------------------------------
    for (size_t i = 0; i < explosions_.size();) {
        explosions_[i].t += dt;
        if (explosions_[i].t >= explosions_[i].duration) { explosions_[i] = explosions_.back(); explosions_.pop_back(); }
        else ++i;
    }
    for (size_t i = 0; i < debris_.size();) {
        Debris& d = debris_[i];
        d.vel.y -= 14.f * dt;
        d.pos += d.vel * dt;
        if (d.pos.y < d.size * 0.5f) { d.pos.y = d.size * 0.5f; d.vel.y *= -0.35f; d.vel.x *= 0.7f; d.vel.z *= 0.7f; }
        d.ttl -= dt;
        if (d.ttl <= 0.f) { debris_[i] = debris_.back(); debris_.pop_back(); }
        else ++i;
    }

    // --- end conditions ----------------------------------------------------
    if (!finished_ && enemiesLeft() == 0) {
        finishDelay_ += dt;
        if (finishDelay_ >= 1.2f) {
            finished_ = true;
            push(FpsEvent::Type::AllClear);
        }
    }
}

}  // namespace rl::game
