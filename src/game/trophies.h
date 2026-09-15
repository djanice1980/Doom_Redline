#pragma once
// Trophies: one-time achievements persisted as "id date" lines in SDL's
// preference directory (next to the high scores).
#include <string>
#include <vector>

namespace rl::game {

struct TrophyDef {
    const char* id;
    const char* name;
    const char* description;
};

// Fixed catalogue; ids are stable keys for the save file.
const std::vector<TrophyDef>& trophyCatalogue();

class Trophies {
public:
    void load(const std::string& path = "");   // default: SDL pref path
    void save() const;
    bool unlocked(const std::string& id) const;
    // Returns true when this call unlocked it (false if already held or unknown).
    bool unlock(const std::string& id);
    int unlockedCount() const;
    int total() const { return static_cast<int>(trophyCatalogue().size()); }
    const std::string& unlockDate(const std::string& id) const;

private:
    struct Entry { std::string id, date; };
    std::vector<Entry> entries_;
    std::string path_;
};

}  // namespace rl::game
