#include "game/trophies.h"

#include <SDL3/SDL.h>

#include <ctime>
#include <fstream>
#include <sstream>

namespace rl::game {

const std::vector<TrophyDef>& trophyCatalogue() {
    static const std::vector<TrophyDef> kAll = {
        {"first_blood", "FIRST BLOOD", "KILL YOUR FIRST DEMON"},
        {"red_line", "RED LINE", "SURVIVE A FIGHT"},
        {"tetris", "TETRIS", "CLEAR FOUR LINES AT ONCE"},
        {"combo3", "ON A ROLL", "THREE CLEARING PIECES IN A ROW"},
        {"chain", "CHAIN REACTION", "A CLEAR CAUSED BY THE COLLAPSE"},
        {"untouchable", "UNTOUCHABLE", "WIN A FIGHT WITHOUT TAKING DAMAGE"},
        {"boss", "BOSS KILLER", "KILL A BARON OR BIGGER"},
        {"cyber", "CYBER SLAYER", "KILL A CYBERDEMON"},
        {"mastermind", "MASTERMIND", "KILL A SPIDER MASTERMIND"},
        {"arsenal", "FULL ARSENAL", "OWN ALL FOUR WEAPONS"},
        {"bfg", "PANIC BUTTON", "FIRE THE BFG9000"},
        {"invuln", "GOLDEN", "WIN THE INVULNERABILITY ROLL"},
        {"level5", "VETERAN", "REACH LEVEL 5"},
        {"level10", "DOOMED", "REACH LEVEL 10"},
        {"survivor", "SURVIVOR", "SURVIVE FIVE FIGHTS IN ONE GAME"},
        {"demolition", "DEMOLITION", "DESTROY 50 BLOCKS IN ONE GAME"},
        {"collector", "COLLECTOR", "PICK UP 20 ITEMS IN ONE GAME"},
        {"grown", "TOO SLOW", "KILL A DEMON THAT HAS GROWN"},
        {"dungeon", "DUNGEON CRAWLER", "SLAY A DUNGEON BOSS"},
        {"doom_slayer", "DOOM SLAYER!", "SET A NEW HIGH SCORE"},
        {"rip_and_tear", "RIP AND TEAR!!!", "EARN EVERY OTHER TROPHY"},
    };
    return kAll;
}

void Trophies::load(const std::string& path) {
    entries_.clear();
    if (!path.empty()) path_ = path;
    if (path_.empty()) {
        char* pref = SDL_GetPrefPath("redline", "redline");
        path_ = pref ? std::string(pref) + "trophies.txt" : "trophies.txt";
        if (pref) SDL_free(pref);
    }
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        Entry e;
        if (ss >> e.id >> e.date) entries_.push_back(e);
    }
}

void Trophies::save() const {
    std::ofstream out(path_);
    for (const Entry& e : entries_) out << e.id << ' ' << e.date << '\n';
}

bool Trophies::unlocked(const std::string& id) const {
    for (const Entry& e : entries_) if (e.id == id) return true;
    return false;
}

const std::string& Trophies::unlockDate(const std::string& id) const {
    static const std::string none;
    for (const Entry& e : entries_) if (e.id == id) return e.date;
    return none;
}

int Trophies::unlockedCount() const {
    int n = 0;
    for (const TrophyDef& d : trophyCatalogue()) if (unlocked(d.id)) ++n;
    return n;
}

bool Trophies::unlock(const std::string& id) {
    bool known = false;
    for (const TrophyDef& d : trophyCatalogue()) if (id == d.id) known = true;
    if (!known || unlocked(id)) return false;
    std::time_t t = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&t));
    entries_.push_back({id, buf});
    save();
    return true;
}

}  // namespace rl::game
