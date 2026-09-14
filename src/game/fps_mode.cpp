#include "game/fps_mode.h"

#include <algorithm>
#include <cmath>

namespace rl::game {

namespace {
constexpr float kPi = 3.14159265f;
constexpr float kMoveSpeed = 6.f;
constexpr float kMouseSens = 0.0022f;
constexpr float kShotDamage = 35.f;
constexpr float kFireballSpeed = 8.f;
constexpr float kFireballDamage = 10.f;
constexpr float kExplosionRadius = 1.5f;   // cells
}  // namespace

FpsMode::FpsMode() : rng_(12345) {}

glm::vec3 FpsMode::forward() const {
    return glm::normalize(glm::vec3(std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_), std::cos(yaw_) * std::cos(pitch_)));
}

float FpsMode::recoil() const {
    if (gunT_ >= gunCycle_) return 0.f;
    float t = gunT_ / gunCycle_;
    return std::sin(t * kPi) * (1.f - t);
}

void FpsMode::begin(core::Game& game, const std::vector<int>& rows) {
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
    playerPos_ = {0.f, 0.f, kPlayerStartZ};
    yaw_ = kPi;
    pitch_ = 0.f;
    std::uniform_real_distribution<float> u(0.f, 1.f);
    for (int r : rows) {
        for (int c = 0; c < core::kBoardW; ++c) {
            if (!game.at(c, r).red()) continue;
            Enemy e;
            e.col = c;
            e.row = r;
            glm::vec3 centre = cellCentre(c, r);
            e.home = centre + glm::vec3(0.f, -0.5f, 0.55f);   // feet at the cell's bottom, just in front of the wall face
            e.pos = e.home;
            e.attackTimer = (1.5f + 2.5f * u(rng_)) * std::max(1.f, static_cast<float>(rows.size()) * 2.f);
            e.bobPhase = u(rng_) * 6.28f;
            e.stateT = -0.15f * static_cast<float>(c);   // stagger the emergence
            enemies_.push_back(e);
        }
    }
    push(FpsEvent::Type::EnemySight, {}, static_cast<int>(enemies_.size()));
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

void FpsMode::spawnDebris(const glm::vec3& pos, const glm::vec3& color, int count, bool red) {
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    for (int i = 0; i < count; ++i) {
        Debris d;
        d.pos = pos + glm::vec3(u(rng_), u(rng_), u(rng_)) * 0.3f;
        d.vel = glm::vec3(u(rng_) * 4.f, 3.f + u(rng_) * 3.f, 2.f + u(rng_) * 4.f);
        d.color = color;
        d.ttl = 1.2f + 0.8f * std::fabs(u(rng_));
        d.size = 0.12f + 0.15f * std::fabs(u(rng_));
        d.red = red;
        debris_.push_back(d);
    }
}

void FpsMode::fire(core::Game& /*game*/) {
    gunT_ = 0.f;
    push(FpsEvent::Type::Shoot, eye());
    glm::vec3 o = eye();
    glm::vec3 d = forward();
    // Closest enemy whose vertical cylinder the ray passes through.
    Enemy* best = nullptr;
    float bestT = 1e9f;
    for (Enemy& e : enemies_) {
        if (e.state == Enemy::State::Dead || e.state == Enemy::State::Dying) continue;
        // Ray vs infinite cylinder around (e.pos.x, e.pos.z), then check height.
        glm::vec2 oc(o.x - e.pos.x, o.z - e.pos.z);
        glm::vec2 dd(d.x, d.z);
        float a = glm::dot(dd, dd);
        float b = 2.f * glm::dot(oc, dd);
        float c = glm::dot(oc, oc) - e.radius * e.radius;
        float disc = b * b - 4 * a * c;
        if (disc < 0.f || a < 1e-6f) continue;
        float t = (-b - std::sqrt(disc)) / (2 * a);
        if (t < 0.f) continue;
        float y = o.y + d.y * t;
        if (y < e.pos.y || y > e.pos.y + e.height) continue;
        if (t < bestT) { bestT = t; best = &e; }
    }
    if (!best) return;
    // Wall blocks in front of the enemy? Enemies stand in front of the wall so no occlusion test needed.
    best->hp -= kShotDamage * (bestT < 3.f ? 1.6f : 1.f);
    glm::vec3 hitPos = o + d * bestT;
    push(FpsEvent::Type::EnemyHit, hitPos);
    spawnDebris(hitPos, {0.6f, 0.05f, 0.05f}, 4, true);
    if (best->hp <= 0.f) {
        health_ = std::min(100.f, health_ + 6.f);   // small heal per kill keeps long fights winnable
        best->state = Enemy::State::Dying;
        best->stateT = 0.f;
        best->animT = 0.f;
        push(FpsEvent::Type::EnemyDied, best->pos);
    } else if (best->state != Enemy::State::Emerging) {
        best->state = Enemy::State::Pain;
        best->stateT = 0.f;
    }
}

void FpsMode::killEnemy(Enemy& e, core::Game& game) {
    if (e.exploded) return;
    e.exploded = true;
    glm::vec3 centre = cellCentre(e.col, e.row);
    // Destroy the cell and its normal neighbours; scatter debris for each.
    std::vector<std::pair<int, int>> victims;
    int rr = 2;
    for (int y = e.row - rr; y <= e.row + rr; ++y)
        for (int x = e.col - rr; x <= e.col + rr; ++x) {
            if (x < 0 || x >= core::kBoardW || y < 0 || y >= core::kBoardH) continue;
            float dx = static_cast<float>(x - e.col), dy = static_cast<float>(y - e.row);
            if (dx * dx + dy * dy > kExplosionRadius * kExplosionRadius + 1e-4f) continue;
            if (game.at(x, y).kind == core::CellKind::Normal) victims.push_back({x, y});
        }
    int destroyed = game.explodeAt(e.col, e.row, kExplosionRadius);
    for (auto [x, y] : victims) spawnDebris(cellCentre(x, y), {0.8f, 0.8f, 0.8f}, 6, false);
    spawnDebris(centre, {0.9f, 0.1f, 0.1f}, 10, true);
    Explosion ex;
    ex.pos = centre + glm::vec3(0.f, 0.f, 0.8f);
    ex.radius = kExplosionRadius;
    explosions_.push_back(ex);
    push(FpsEvent::Type::Explosion, ex.pos, destroyed);
}

void FpsMode::update(float dt, const FpsInput& in, core::Game& game) {
    elapsed_ += dt;
    // --- look & move ---------------------------------------------------------
    yaw_ -= in.lookDX * kMouseSens;
    pitch_ = std::clamp(pitch_ - in.lookDY * kMouseSens, -1.2f, 1.2f);
    glm::vec3 fwd(std::sin(yaw_), 0.f, std::cos(yaw_));
    glm::vec3 right(-std::cos(yaw_), 0.f, std::sin(yaw_));   // forward x up
    glm::vec3 move = fwd * in.moveZ + right * in.moveX;
    if (glm::length(move) > 1.f) move = glm::normalize(move);
    playerPos_ += move * kMoveSpeed * dt;
    playerPos_.x = std::clamp(playerPos_.x, -kArenaHalfW + 0.6f, kArenaHalfW - 0.6f);
    playerPos_.z = std::clamp(playerPos_.z, 2.5f, kArenaDepth - 0.6f);
    playerPos_.y = 0.f;
    damageFlash_ = std::max(0.f, damageFlash_ - dt * 2.5f);

    // --- weapon ----------------------------------------------------------
    gunT_ += dt;
    if (in.fire && gunT_ >= gunCycle_ && health_ > 0.f) fire(game);

    // --- enemies ---------------------------------------------------------
    std::uniform_real_distribution<float> u(0.f, 1.f);
    for (Enemy& e : enemies_) {
        e.stateT += dt;
        e.animT += dt;
        switch (e.state) {
        case Enemy::State::Emerging: {
            float t = std::clamp(e.stateT / 0.9f, 0.f, 1.f);
            float s = t * t * (3 - 2 * t);
            e.pos = e.home + glm::vec3(0.f, 0.f, 1.2f * s);
            if (e.stateT >= 0.9f) { e.state = Enemy::State::Idle; e.stateT = 0.f; e.home = e.pos; }
            break;
        }
        case Enemy::State::Idle: {
            // Hover and drift slightly towards the player on X.
            float bob = 0.08f * std::sin(elapsed_ * 3.f + e.bobPhase);
            glm::vec3 target = e.home;
            target.x += 0.6f * std::sin(elapsed_ * 0.7f + e.bobPhase);
            e.pos = glm::mix(e.pos, target, std::min(1.f, dt * 2.f));
            e.pos.y = e.home.y + bob;
            e.attackTimer -= dt;
            if (e.attackTimer <= 0.f && health_ > 0.f) {
                e.state = Enemy::State::Attack;
                e.stateT = 0.f;
                e.animT = 0.f;
            }
            break;
        }
        case Enemy::State::Attack: {
            if (e.stateT >= 0.35f && e.attackTimer <= 0.f) {   // launch at the mid-point of the wind-up
                Projectile p;
                p.pos = e.pos + glm::vec3(0.f, 1.1f, 0.3f);
                glm::vec3 dir = glm::normalize((playerPos_ + glm::vec3(0.f, 1.3f, 0.f)) - p.pos);
                p.vel = dir * kFireballSpeed;
                projectiles_.push_back(p);
                // Shared fire budget: the more demons alive, the longer each waits.
                float crowd = std::max(1.f, static_cast<float>(enemiesLeft()) * 0.4f);
                e.attackTimer = (2.0f + 2.5f * u(rng_)) * crowd;
                push(FpsEvent::Type::FireballLaunched, p.pos);
            }
            if (e.stateT >= 0.6f) { e.state = Enemy::State::Idle; e.stateT = 0.f; }
            break;
        }
        case Enemy::State::Pain:
            if (e.stateT >= 0.25f) { e.state = Enemy::State::Idle; e.stateT = 0.f; }
            break;
        case Enemy::State::Dying:
            if (e.stateT >= 0.45f && !e.exploded) killEnemy(e, game);
            if (e.stateT >= 0.7f) { e.state = Enemy::State::Dead; }
            break;
        case Enemy::State::Dead:
            break;
        }
    }

    // --- projectiles -----------------------------------------------------
    for (size_t i = 0; i < projectiles_.size();) {
        Projectile& p = projectiles_[i];
        p.pos += p.vel * dt;
        p.ttl -= dt;
        p.animT += dt;
        bool remove = p.ttl <= 0.f || p.pos.y < 0.f || p.pos.z > kArenaDepth + 1.f || std::fabs(p.pos.x) > kArenaHalfW + 1.f;
        glm::vec3 chest = playerPos_ + glm::vec3(0.f, 1.2f, 0.f);
        glm::vec3 dp = p.pos - chest;
        if (std::fabs(dp.x) < 0.5f && std::fabs(dp.z) < 0.5f && dp.y > -1.2f && dp.y < 0.7f) {
            health_ = std::max(0.f, health_ - kFireballDamage);
            damageFlash_ = 1.f;
            push(FpsEvent::Type::PlayerHit, p.pos);
            if (health_ <= 0.f) push(FpsEvent::Type::PlayerDead, p.pos);
            remove = true;
        }
        if (remove) {
            Explosion ex;
            ex.pos = p.pos;
            ex.duration = 0.25f;
            ex.radius = 0.4f;
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
