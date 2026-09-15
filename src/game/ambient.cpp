#include "game/ambient.h"

#include <algorithm>
#include <cmath>

#include "game/fps_mode.h"   // enemyStats / AttackKind

namespace rl::game {

namespace {
// The floor strips either side of the standing board (board frame is at x = +-5.5),
// kept to the band in front of the back wall that the overview camera can see:
// its bottom edge meets the floor at about z = 3. Nobody walks in front of the
// board (it would hide the stack), but the ranged classes trade shots across
// it once both stand in front of its face (z >= kFrontZ).
constexpr float kZoneInner = 7.5f, kZoneOuter = 13.5f;
constexpr float kZoneZ0 = -1.2f, kZoneZ1 = 2.6f;
constexpr float kFrontZ = 1.4f;
constexpr int kPerZone = 3;
constexpr int kBrawlBossTier = 5;   // cyberdemon: at most one alive; spiders never brawl

// Tier window per level: zombies and imps early, barons by level 6, a
// cyberdemon from level 8. Health follows the fight's level curve.
int brawlTierLo(int level) { return std::clamp(level / 3, 0, 3); }
int brawlTierHi(int level) { return std::clamp(1 + level / 2, 1, kBrawlBossTier); }
float brawlHpScale(int level) { return std::clamp(0.45f + 0.15f * static_cast<float>(level - 1), 0.45f, 2.5f) * 0.7f; }
}  // namespace

Ambient::Ambient() : rng_(777) {}

glm::vec3 Ambient::randomSpot(int zone) {
    std::uniform_real_distribution<float> ux(kZoneInner, kZoneOuter), uz(kZoneZ0, kZoneZ1);
    float x = ux(rng_);
    return {zone == 0 ? -x : x, 0.f, uz(rng_)};
}

void Ambient::spawn(Brawler& b, int zone) {
    int hi = brawlTierHi(level_);
    if (hi >= kBrawlBossTier)
        for (const Brawler& o : brawlers_) if (&o != &b && o.alive() && o.tier >= kBrawlBossTier) { hi = kBrawlBossTier - 1; break; }
    std::uniform_int_distribution<int> tierPick(std::min(brawlTierLo(level_), hi), hi);
    b = Brawler{};
    b.tier = tierPick(rng_);
    const EnemyStats& st = enemyStats(b.tier);
    b.zone = zone;
    b.pos = randomSpot(zone);
    b.pos.y = st.flies ? 0.8f : 0.f;
    b.maxHp = b.hp = st.hp * brawlHpScale(level_);   // shorter brawls than the real fight, tougher with the level
    b.radius = st.radius;
    b.height = st.height;
    std::uniform_real_distribution<float> u(0.f, 1.f);
    b.attackTimer = 1.f + 2.f * u(rng_);
    b.state = Brawler::State::Emerging;
}

void Ambient::reset(uint32_t seed, int level) {
    rng_.seed(seed);
    level_ = level;
    brawlers_.clear();
    projectiles_.clear();
    blasts_.clear();
    events_.clear();
    elapsed_ = 0.f;
    for (int zone = 0; zone < 2; ++zone)
        for (int i = 0; i < kPerZone; ++i) {
            Brawler b;
            spawn(b, zone);
            b.stateT = -0.4f * static_cast<float>(i);
            brawlers_.push_back(b);
        }
}

void Ambient::clear() {
    brawlers_.clear();
    projectiles_.clear();
    blasts_.clear();
}

std::vector<BrawlEvent> Ambient::drainEvents() {
    std::vector<BrawlEvent> out;
    out.swap(events_);
    return out;
}

void Ambient::hurt(int idx, float dmg, glm::vec3 from) {
    if (idx < 0 || idx >= static_cast<int>(brawlers_.size())) return;
    Brawler& b = brawlers_[static_cast<size_t>(idx)];
    if (!b.alive() || b.state == Brawler::State::Emerging) return;
    b.hp -= dmg;
    if (b.hp <= 0.f) {
        b.state = Brawler::State::Dying;
        b.stateT = 0.f;
        std::uniform_real_distribution<float> u(6.f, 12.f);
        b.respawnT = u(rng_);
        events_.push_back({BrawlEvent::Type::Death, b.tier, b.pos});
    } else {
        b.state = Brawler::State::Pain;
        b.stateT = 0.f;
        b.attacked = false;
        events_.push_back({BrawlEvent::Type::Pain, b.tier, from});
    }
}

void Ambient::update(float dt) {
    elapsed_ += dt;
    std::uniform_real_distribution<float> u(0.f, 1.f);
    for (size_t i = 0; i < brawlers_.size(); ++i) {
        Brawler& b = brawlers_[i];
        const EnemyStats& st = enemyStats(b.tier);
        b.stateT += dt;
        b.animT += dt;
        b.flashT = std::max(0.f, b.flashT - dt * 6.f);
        float restY = st.flies ? 0.8f + 0.15f * std::sin(elapsed_ * 2.f + static_cast<float>(i)) : 0.f;

        // Pick the nearest living opponent: melee classes on their own side,
        // ranged ones on either side of the board.
        if (b.target < 0 || b.target >= static_cast<int>(brawlers_.size()) || !brawlers_[static_cast<size_t>(b.target)].alive() || static_cast<size_t>(b.target) == i) {
            b.target = -1;
            float best = 1e9f;
            for (size_t j = 0; j < brawlers_.size(); ++j) {
                if (j == i || !brawlers_[j].alive() || brawlers_[j].state == Brawler::State::Emerging) continue;
                if (st.attack == AttackKind::Melee && brawlers_[j].zone != b.zone) continue;
                float d = glm::length(brawlers_[j].pos - b.pos);
                if (d < best) { best = d; b.target = static_cast<int>(j); }
            }
        }
        glm::vec3 toT(0.f);
        float dist = 99.f;
        if (b.target >= 0) {
            toT = brawlers_[static_cast<size_t>(b.target)].pos - b.pos;
            toT.y = 0.f;
            dist = glm::length(toT);
            if (dist > 1e-3f) { toT /= dist; b.facingLeft = toT.x < 0.f; }
        }

        switch (b.state) {
        case Brawler::State::Emerging: {
            float t = std::clamp(b.stateT / 0.9f, 0.f, 1.f);
            b.pos.y = -b.height + (b.height + restY) * t * t * (3 - 2 * t);
            if (b.stateT >= 0.9f) { b.state = Brawler::State::Idle; b.stateT = 0.f; b.pos.y = restY; }
            break;
        }
        case Brawler::State::Idle: {
            b.pos.y = restY;
            if (b.target >= 0) {
                const Brawler& tgt = brawlers_[static_cast<size_t>(b.target)];
                float want = (st.attack == AttackKind::Melee) ? 1.0f : 4.f;
                float speed = st.speed > 0.f ? st.speed : 1.2f;   // even stationary classes shuffle around here
                bool otherSide = tgt.zone != b.zone;
                // The board stands between the sides: a shot across it is only
                // taken from in front of its face, so step forward first.
                if (otherSide) { if (b.pos.z < kFrontZ) b.pos.z += speed * dt; }
                else if (dist > want + 0.3f) b.pos += toT * speed * dt;
                else if (dist < want - 0.8f) b.pos -= toT * speed * 0.6f * dt;
                // Stay on this side of the board.
                float ax = std::clamp(std::fabs(b.pos.x), kZoneInner, kZoneOuter);
                b.pos.x = b.zone == 0 ? -ax : ax;
                b.pos.z = std::clamp(b.pos.z, kZoneZ0, kZoneZ1);
                b.attackTimer -= dt;
                bool clearShot = !otherSide || (b.pos.z >= kFrontZ - 0.1f && tgt.pos.z >= kFrontZ - 0.1f);
                bool inRange = clearShot && ((st.attack == AttackKind::Melee) ? dist < 1.5f : dist < 26.f);
                if (b.attackTimer <= 0.f && inRange) { b.state = Brawler::State::Attack; b.stateT = 0.f; b.animT = 0.f; b.attacked = false; }
            }
            break;
        }
        case Brawler::State::Attack: {
            b.pos.y = restY;
            if (b.stateT >= 0.35f && !b.attacked) {
                b.attacked = true;
                b.attackTimer = st.attackInterval * (0.8f + 0.6f * u(rng_));
                events_.push_back({BrawlEvent::Type::Attack, b.tier, b.pos + glm::vec3(0.f, 1.f, 0.f)});
                switch (st.attack) {
                case AttackKind::Hitscan:
                    b.flashT = 1.f;
                    if (u(rng_) < 0.7f) hurt(b.target, st.damage * 2.f, b.pos);
                    break;
                case AttackKind::Projectile:
                    if (b.target >= 0) {
                        BrawlProjectile p;
                        p.pos = b.pos + glm::vec3(0.f, 1.f, 0.f) + toT * 0.4f;
                        glm::vec3 aim = brawlers_[static_cast<size_t>(b.target)].pos + glm::vec3(0.f, 0.9f, 0.f);
                        p.vel = glm::normalize(aim - p.pos) * st.projSpeed;
                        p.type = st.projectile;
                        p.damage = st.damage * 2.f;
                        p.target = b.target;
                        projectiles_.push_back(p);
                    }
                    break;
                case AttackKind::Melee:
                    if (dist < 1.8f) hurt(b.target, st.damage * 2.f, b.pos);
                    break;
                }
            }
            if (b.stateT >= 0.6f) { b.state = Brawler::State::Idle; b.stateT = 0.f; }
            break;
        }
        case Brawler::State::Pain:
            if (b.stateT >= 0.25f) { b.state = Brawler::State::Idle; b.stateT = 0.f; }
            break;
        case Brawler::State::Dying:
            if (b.stateT >= 0.8f) b.state = Brawler::State::Dead;
            break;
        case Brawler::State::Dead:
            b.respawnT -= dt;
            if (b.respawnT <= 0.f) { int zone = b.zone; spawn(b, zone); }
            break;
        }
    }

    for (size_t i = 0; i < projectiles_.size();) {
        BrawlProjectile& p = projectiles_[i];
        glm::vec3 prev = p.pos;
        p.pos += p.vel * dt;
        p.ttl -= dt;
        p.animT += dt;
        bool remove = p.ttl <= 0.f || p.pos.y < 0.f;
        if (p.target >= 0 && p.target < static_cast<int>(brawlers_.size())) {
            const Brawler& t = brawlers_[static_cast<size_t>(p.target)];
            glm::vec2 d(p.pos.x - t.pos.x, p.pos.z - t.pos.z);
            if (t.alive() && glm::length(d) < t.radius + 0.3f && p.pos.y > t.pos.y - 0.2f && p.pos.y < t.pos.y + t.height + 0.3f) {
                hurt(p.target, p.damage, p.pos);
                remove = true;
            }
        }
        if (remove) {
            blasts_.push_back({prev, 0.f, 0.3f, p.type});
            projectiles_[i] = projectiles_.back();
            projectiles_.pop_back();
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < blasts_.size();) {
        blasts_[i].t += dt;
        if (blasts_[i].t >= blasts_[i].duration) { blasts_[i] = blasts_.back(); blasts_.pop_back(); }
        else ++i;
    }
}

}  // namespace rl::game
