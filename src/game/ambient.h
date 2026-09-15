#pragma once
// Ambient infighting: a few monsters brawl on the arena floor either side of
// the board while the player stacks blocks. Purely decorative: they cannot
// touch the player or the board, they respawn when killed, and they vanish
// when the first-person phase begins.
#include <random>
#include <vector>

#include <glm/glm.hpp>

namespace rl::game {

struct Brawler {
    enum class State { Emerging, Idle, Attack, Pain, Dying, Dead };
    int tier = 1;
    glm::vec3 pos{0.f};
    float hp = 60.f, maxHp = 60.f;
    State state = State::Emerging;
    float stateT = 0.f;
    float animT = 0.f;
    float attackTimer = 2.f;
    bool attacked = false;
    int target = -1;
    int zone = 0;            // 0 = left of the board, 1 = right
    float respawnT = 0.f;
    float radius = 0.4f, height = 1.6f;
    float flashT = 0.f;      // hitscan muzzle light
    bool facingLeft = false;
    bool alive() const { return state != State::Dead && state != State::Dying; }
};

struct BrawlProjectile {
    glm::vec3 pos, vel;
    int type = 0;
    float damage = 10.f;
    int target = -1;
    float ttl = 4.f;
    float animT = 0.f;
};

struct BrawlBlast {
    glm::vec3 pos;
    float t = 0.f;
    float duration = 0.3f;
    int hitType = 0;
};

struct BrawlEvent {
    enum class Type { Attack, Pain, Death } type;
    int tier;
    glm::vec3 pos;
};

class Ambient {
public:
    Ambient();
    void reset(uint32_t seed, int level);
    void update(float dt);
    void clear();                       // despawn everything (first-person phase)
    bool empty() const { return brawlers_.empty(); }
    const std::vector<Brawler>& brawlers() const { return brawlers_; }
    const std::vector<BrawlProjectile>& projectiles() const { return projectiles_; }
    const std::vector<BrawlBlast>& blasts() const { return blasts_; }
    std::vector<BrawlEvent> drainEvents();

private:
    void spawn(Brawler& b, int zone);
    glm::vec3 randomSpot(int zone);
    void hurt(int idx, float dmg, glm::vec3 from);
    std::mt19937 rng_;
    std::vector<Brawler> brawlers_;
    std::vector<BrawlProjectile> projectiles_;
    std::vector<BrawlBlast> blasts_;
    std::vector<BrawlEvent> events_;
    float elapsed_ = 0.f;
    int level_ = 1;
};

}  // namespace rl::game
