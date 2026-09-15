#include "game/highscores.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace rl::game {

void HighScores::load(const std::string& path) {
    entries_.clear();
    if (!path.empty()) path_ = path;
    if (path_.empty()) {
        char* pref = SDL_GetPrefPath("redline", "redline");
        if (pref) {
            path_ = std::string(pref) + "highscores.txt";
            SDL_free(pref);
        } else {
            path_ = "highscores.txt";
        }
    }
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        HighScore h;
        if (ss >> h.score >> h.level >> h.redLines >> h.lines >> h.date) entries_.push_back(h);
    }
    std::sort(entries_.begin(), entries_.end(), [](const HighScore& a, const HighScore& b) { return a.score > b.score; });
    if (entries_.size() > kMax) entries_.resize(kMax);
}

void HighScores::save() const {
    std::ofstream out(path_);
    for (const HighScore& h : entries_) out << h.score << ' ' << h.level << ' ' << h.redLines << ' ' << h.lines << ' ' << h.date << '\n';
}

int HighScores::add(const HighScore& entry) {
    HighScore h = entry;
    if (h.date.empty()) {
        std::time_t t = std::time(nullptr);
        char buf[16];
        std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&t));
        h.date = buf;
    }
    if (h.score <= 0) return 0;
    auto pos = std::find_if(entries_.begin(), entries_.end(), [&](const HighScore& e) { return h.score > e.score; });
    int rank = static_cast<int>(pos - entries_.begin()) + 1;
    if (rank > static_cast<int>(kMax)) return 0;
    entries_.insert(pos, h);
    if (entries_.size() > kMax) entries_.resize(kMax);
    save();
    return rank;
}

}  // namespace rl::game
