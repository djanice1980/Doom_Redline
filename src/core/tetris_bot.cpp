#include "core/tetris_bot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rl::core {

namespace {

// 0 empty, 1 normal, 2 red.
using Grid = std::array<std::array<uint8_t, kBoardW>, kBoardH>;

bool fitsGrid(const Grid& g, const Piece& p) {
    for (auto [cx, cy] : p.cells()) {
        if (cx < 0 || cx >= kBoardW || cy >= kBoardH) return false;
        if (cy < 0) continue;
        if (g[cy][cx]) return false;
    }
    return true;
}

// Sticky gravity as Game::settleStep does it: every 4-connected clump with nothing
// under any of its cells drops a row, until everything rests.
void settle(Grid& g) {
    for (int guard = 0; guard < kBoardH; ++guard) {
        int comp[kBoardH][kBoardW];
        for (auto& row : comp) for (int& v : row) v = -1;
        int n = 0;
        std::vector<std::pair<int, int>> stack;
        for (int r = 0; r < kBoardH; ++r)
            for (int c = 0; c < kBoardW; ++c) {
                if (!g[r][c] || comp[r][c] >= 0) continue;
                comp[r][c] = n;
                stack.push_back({r, c});
                while (!stack.empty()) {
                    auto [y, x] = stack.back();
                    stack.pop_back();
                    const int dy[4] = {1, -1, 0, 0}, dx[4] = {0, 0, 1, -1};
                    for (int k = 0; k < 4; ++k) {
                        const int ny = y + dy[k], nx = x + dx[k];
                        if (ny < 0 || ny >= kBoardH || nx < 0 || nx >= kBoardW) continue;
                        if (!g[ny][nx] || comp[ny][nx] >= 0) continue;
                        comp[ny][nx] = n;
                        stack.push_back({ny, nx});
                    }
                }
                ++n;
            }
        std::vector<bool> supported(static_cast<size_t>(n), false);
        for (int r = 0; r < kBoardH; ++r)
            for (int c = 0; c < kBoardW; ++c) {
                if (comp[r][c] < 0) continue;
                if (r == kBoardH - 1 || (g[r + 1][c] && comp[r + 1][c] != comp[r][c])) supported[static_cast<size_t>(comp[r][c])] = true;
            }
        bool moved = false;
        for (int r = kBoardH - 2; r >= 0; --r)
            for (int c = 0; c < kBoardW; ++c) {
                if (comp[r][c] < 0 || supported[static_cast<size_t>(comp[r][c])]) continue;
                g[r + 1][c] = g[r][c];
                g[r][c] = 0;
                moved = true;
            }
        if (!moved) return;
    }
}

// Score a stack after the piece lands, the way this game resolves it: a full
// row loses its normal cells (each such column shifts down a row; a red cell
// stays and its column with it), then loose clumps settle.
float evaluate(Grid g, const Piece& landed, bool& dead) {
    dead = false;
    for (int i = 0; i < 4; ++i) {
        auto [cx, cy] = landed.cells()[i];
        if (cy < 0) { dead = true; continue; }   // locked above the board: game over
        g[cy][cx] = landed.red[i] ? 2 : 1;
    }
    int lines = 0;
    for (int r = 0; r < kBoardH; ++r) {
        bool full = true;
        for (int c = 0; c < kBoardW && full; ++c) full = g[r][c] != 0;
        if (!full) continue;
        ++lines;
        for (int c = 0; c < kBoardW; ++c) {
            if (g[r][c] == 2) continue;
            for (int y = r; y > 0; --y) g[y][c] = g[y - 1][c];
            g[0][c] = 0;
        }
    }
    if (lines) settle(g);
    // An empty cell walled in by red (side walls and the floor count, at least two
    // real reds, something on top) is where evil spawns a red block within a couple
    // of seconds: as good as red, and covering it is the point, not a hole.
    // Corruption works the same way from the other side: in a row that is nearly all
    // red it turns the blocks around the last gap red so evil can fill it. So a gap in
    // such a row, walled in by red, is not a hole at all but the last piece of a fight,
    // and a block on top of it is what gets the gap filled.
    int rowReds[kBoardH];
    for (int r = 0; r < kBoardH; ++r) {
        rowReds[r] = 0;
        for (int c = 0; c < kBoardW; ++c) rowReds[r] += g[r][c] == 2;
    }
    auto walledIn = [&](int c, int r) {
        if (g[r][c]) return false;
        auto side = [&](int cc, int rr) { return cc < 0 || cc >= kBoardW || rr >= kBoardH || g[rr][cc] == 2; };   // wall or floor stands in
        return side(c - 1, r) && side(c + 1, r) && side(c, r + 1);
    };
    auto evilReady = [&](int c, int r) { return walledIn(c, r) && rowReds[r] >= kBoardW - 2 && r > 0 && g[r - 1][c] != 0; };
    auto fightFuel = [&](int c, int r) { return walledIn(c, r) && rowReds[r] >= kBoardW - 2; };
    int heights[kBoardW];
    int agg = 0, holes = 0, bump = 0;
    for (int c = 0; c < kBoardW; ++c) {
        int h = 0;
        for (int r = 0; r < kBoardH; ++r) if (g[r][c]) { h = kBoardH - r; break; }
        heights[c] = h;
        agg += h;
        for (int r = kBoardH - h; r < kBoardH; ++r) if (!g[r][c] && !fightFuel(c, r)) ++holes;
    }
    for (int c = 0; c + 1 < kBoardW; ++c) bump += std::abs(heights[c] - heights[c + 1]);
    // Deep wells beside the walls are fine (that is where the I goes); interior ones are not.
    int wells = 0;
    for (int c = 1; c + 1 < kBoardW; ++c) {
        int d = std::min(heights[c - 1], heights[c + 1]) - heights[c];
        if (d >= 3) wells += d - 2;
    }
    // Red rows are the way out of a rising stack: the fight that follows cleanses
    // the board, so a row that is mostly red is worth working towards.
    float redRow = 0.f;
    bool redLine = false;
    for (int r = 0; r < kBoardH; ++r) {
        int reds = 0;
        for (int c = 0; c < kBoardW; ++c) reds += g[r][c] == 2 || evilReady(c, r);
        if (reds == kBoardW) redLine = true;
        redRow = std::max(redRow, static_cast<float>(reds * reds) / static_cast<float>(kBoardW * kBoardW));
    }
    return 0.76f * static_cast<float>(lines) - 0.51f * static_cast<float>(agg) - 0.36f * static_cast<float>(holes)
         - 0.18f * static_cast<float>(bump) - 0.10f * static_cast<float>(wells) + 2.0f * redRow + (redLine ? 6.f : 0.f);
}

}  // namespace

BotPlan planPlacement(const Game& game) {
    BotPlan best;
    if (game.phase() != Phase::Falling || !game.active()) return best;
    const Piece& active = *game.active();
    Grid g{};
    for (int r = 0; r < kBoardH; ++r)
        for (int c = 0; c < kBoardW; ++c) g[r][c] = game.at(c, r).empty() ? 0 : game.at(c, r).red() ? 2 : 1;
    const int rots = active.shape == Shape::O ? 1 : 4;
    for (int k = 0; k < rots; ++k) {
        const int rot = (active.rot + k) & 3;
        for (int x = -2; x < kBoardW; ++x) {
            Piece p = active;
            p.rot = rot;
            p.x = x;
            if (!fitsGrid(g, p)) continue;
            // Reachable by sliding at the current height: every column between here and there must be free.
            bool reach = true;
            for (int xx = std::min(x, active.x); xx <= std::max(x, active.x) && reach; ++xx) { Piece q = p; q.x = xx; reach = fitsGrid(g, q); }
            if (!reach) continue;
            while (true) { Piece q = p; q.y += 1; if (!fitsGrid(g, q)) break; p = q; }
            bool dead = false;
            float score = evaluate(g, p, dead);
            if (dead) score -= 1000.f;
            score -= 0.01f * static_cast<float>(k);   // fewer rotations on a tie
            if (!best.valid || score > best.score) { best = {rot, x, score, true}; }
        }
    }
    return best;
}

BotMove nextMove(const Game& game, const BotPlan& plan) {
    if (!plan.valid || game.phase() != Phase::Falling || !game.active()) return BotMove::None;
    const Piece& p = *game.active();
    if (p.shape != Shape::O && p.rot != plan.rot) return ((plan.rot - p.rot) & 3) == 3 ? BotMove::RotateCCW : BotMove::RotateCW;
    if (p.x < plan.x) return BotMove::Right;
    if (p.x > plan.x) return BotMove::Left;
    return BotMove::Drop;
}

BotMove TetrisBot::step(Game& game) {
    if (game.phase() != Phase::Falling || !game.active()) { havePlan_ = false; return BotMove::None; }
    if (!havePlan_) {
        plan_ = planPlacement(game);
        havePlan_ = plan_.valid;
        stuck_ = 0;
        if (!havePlan_) { game.hardDrop(); ++pieces_; return BotMove::Drop; }   // nowhere to go: let it fall where it is
    }
    const Piece before = *game.active();
    BotMove m = nextMove(game, plan_);
    switch (m) {
    case BotMove::Left: game.moveLeft(); break;
    case BotMove::Right: game.moveRight(); break;
    case BotMove::RotateCW: game.rotateCW(); break;
    case BotMove::RotateCCW: game.rotateCCW(); break;
    case BotMove::Drop: game.hardDrop(); ++pieces_; havePlan_ = false; return m;
    default: return m;
    }
    if (!game.active() || game.phase() != Phase::Falling) { havePlan_ = false; return m; }
    const Piece& after = *game.active();
    if (after.x == before.x && after.y == before.y && after.rot == before.rot) {
        // No progress: the plan is out of reach from here. Replan once from where the
        // piece actually is, and if that leads nowhere either, drop it.
        if (++stuck_ >= 2) {
            BotPlan again = planPlacement(game);
            stuck_ = 0;
            if (!again.valid || (again.rot == plan_.rot && again.x == plan_.x)) { game.hardDrop(); ++pieces_; havePlan_ = false; return BotMove::Drop; }
            plan_ = again;
        }
    } else {
        stuck_ = 0;
    }
    return m;
}

}  // namespace rl::core
