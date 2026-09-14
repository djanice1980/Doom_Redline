#pragma once
// Persistent top-10 table stored as a small text file in SDL's preference
// directory (e.g. ~/.local/share/redline/redline/highscores.txt).
#include <string>
#include <vector>

namespace rl::game {

struct HighScore {
    int score = 0;
    int level = 1;
    int redLines = 0;
    int lines = 0;
    std::string date;   // YYYY-MM-DD
};

class HighScores {
public:
    static constexpr size_t kMax = 10;
    void load();
    void save() const;
    // Inserts if it ranks; returns the 1-based rank or 0 if it did not make the table.
    int add(const HighScore& entry);
    const std::vector<HighScore>& entries() const { return entries_; }
    int best() const { return entries_.empty() ? 0 : entries_.front().score; }
    const std::string& path() const { return path_; }

private:
    std::vector<HighScore> entries_;
    std::string path_;
};

}  // namespace rl::game
