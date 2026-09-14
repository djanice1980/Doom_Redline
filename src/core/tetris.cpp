#include "core/tetris.h"

#include <algorithm>
#include <cmath>

namespace rl::core {

namespace {

// SRS-style shapes, origin is the top-left of a 4x4 (I,O) or 3x3 box.
using C4 = std::array<std::pair<int, int>, 4>;
const std::array<std::array<C4, 4>, static_cast<size_t>(Shape::Count)> kShapes = {{
    // I
    {{{{{0, 1}, {1, 1}, {2, 1}, {3, 1}}}, {{{2, 0}, {2, 1}, {2, 2}, {2, 3}}}, {{{0, 2}, {1, 2}, {2, 2}, {3, 2}}}, {{{1, 0}, {1, 1}, {1, 2}, {1, 3}}}}},
    // O
    {{{{{1, 0}, {2, 0}, {1, 1}, {2, 1}}}, {{{1, 0}, {2, 0}, {1, 1}, {2, 1}}}, {{{1, 0}, {2, 0}, {1, 1}, {2, 1}}}, {{{1, 0}, {2, 0}, {1, 1}, {2, 1}}}}},
    // T
    {{{{{1, 0}, {0, 1}, {1, 1}, {2, 1}}}, {{{1, 0}, {1, 1}, {2, 1}, {1, 2}}}, {{{0, 1}, {1, 1}, {2, 1}, {1, 2}}}, {{{1, 0}, {0, 1}, {1, 1}, {1, 2}}}}},
    // S
    {{{{{1, 0}, {2, 0}, {0, 1}, {1, 1}}}, {{{1, 0}, {1, 1}, {2, 1}, {2, 2}}}, {{{1, 1}, {2, 1}, {0, 2}, {1, 2}}}, {{{0, 0}, {0, 1}, {1, 1}, {1, 2}}}}},
    // Z
    {{{{{0, 0}, {1, 0}, {1, 1}, {2, 1}}}, {{{2, 0}, {1, 1}, {2, 1}, {1, 2}}}, {{{0, 1}, {1, 1}, {1, 2}, {2, 2}}}, {{{1, 0}, {0, 1}, {1, 1}, {0, 2}}}}},
    // J
    {{{{{0, 0}, {0, 1}, {1, 1}, {2, 1}}}, {{{1, 0}, {2, 0}, {1, 1}, {1, 2}}}, {{{0, 1}, {1, 1}, {2, 1}, {2, 2}}}, {{{1, 0}, {1, 1}, {0, 2}, {1, 2}}}}},
    // L
    {{{{{2, 0}, {0, 1}, {1, 1}, {2, 1}}}, {{{1, 0}, {1, 1}, {1, 2}, {2, 2}}}, {{{0, 1}, {1, 1}, {2, 1}, {0, 2}}}, {{{0, 0}, {1, 0}, {1, 1}, {1, 2}}}}},
}};

// Wall kicks tried in order when a rotation collides.
constexpr std::array<std::pair<int, int>, 5> kKicks = {{{0, 0}, {-1, 0}, {1, 0}, {-2, 0}, {2, 0}}};

}  // namespace

const C4& shapeCells(Shape s, int rot) { return kShapes[static_cast<size_t>(s)][rot & 3]; }

C4 Piece::cells() const {
    C4 out;
    const C4& rel = shapeCells(shape, rot);
    for (int i = 0; i < 4; ++i) out[i] = {x + rel[i].first, y + rel[i].second};
    return out;
}

Game::Game(uint32_t seed, Rules rules) : rules_(rules), seed_(seed), rng_(seed) {
    next_ = makePiece();
}

// ---------------------------------------------------------------------------
// Piece generation: 7-bag randomiser, then sprinkle red minos.
Piece Game::makePiece() {
    if (bag_.empty()) {
        for (int i = 0; i < static_cast<int>(Shape::Count); ++i) bag_.push_back(static_cast<Shape>(i));
        std::shuffle(bag_.begin(), bag_.end(), rng_);
    }
    Piece p;
    p.shape = bag_.back();
    bag_.pop_back();
    p.rot = 0;
    p.x = (p.shape == Shape::I || p.shape == Shape::O) ? 3 : 3;
    p.y = 0;
    float chance = std::min(rules_.redChanceMax, rules_.redChanceBase + rules_.redChancePerLevel * static_cast<float>(level_ - 1));
    std::uniform_real_distribution<float> u(0.f, 1.f);
    int reds = 0;
    for (int i = 0; i < 4 && reds < rules_.maxRedPerPiece; ++i) {
        if (u(rng_) < chance) { p.red[i] = true; ++reds; }
    }
    return p;
}

void Game::forcePiece(Shape s, std::array<bool, 4> red) {
    next_.shape = s;
    next_.rot = 0;
    next_.x = 3;
    next_.y = 0;
    next_.red = red;
    if (active_ && phase_ == Phase::Falling) {   // test helper: also swap the piece in the air
        *active_ = next_;
        gravityAcc_ = 0.f;
        lockAcc_ = 0.f;
    }
}

float Game::gravityInterval() const {
    float g = rules_.gravityBase * std::pow(rules_.gravityPerLevel, static_cast<float>(level_ - 1));
    g = std::max(rules_.gravityMin, g);
    if (softDrop_) g /= rules_.softDropMultiplier;
    return g;
}

bool Game::fits(const Piece& p) const {
    for (auto [cx, cy] : p.cells()) {
        if (cx < 0 || cx >= kBoardW || cy >= kBoardH) return false;
        if (cy < 0) continue;   // above the board is allowed while spawning
        if (!grid_[cy][cx].empty()) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
void Game::spawn() {
    if (!active_) {   // a piece interrupted by a red line resumes instead
        active_ = next_;
        next_ = makePiece();
    }
    gravityAcc_ = 0.f;
    lockAcc_ = 0.f;
    if (!fits(*active_)) {
        // Try one row higher (piece pokes above the top) before declaring game over.
        active_->y = -1;
        if (!fits(*active_)) {
            active_.reset();
            phase_ = Phase::GameOver;
            push(EventType::GameOver);
            return;
        }
    }
    phase_ = Phase::Falling;
    push(EventType::PieceSpawned);
}

void Game::spawnNow() {
    if (phase_ == Phase::Spawning) spawn();
}

bool Game::tryMove(int dx, int dy) {
    if (!active_) return false;
    Piece p = *active_;
    p.x += dx;
    p.y += dy;
    if (!fits(p)) return false;
    *active_ = p;
    return true;
}

bool Game::tryRotate(int dir) {
    if (!active_) return false;
    Piece p = *active_;
    p.rot = (p.rot + dir + 4) & 3;
    for (auto [kx, ky] : kKicks) {
        Piece q = p;
        q.x += kx;
        q.y += ky;
        if (fits(q)) {
            *active_ = q;
            return true;
        }
    }
    return false;
}

void Game::moveLeft() {
    if (phase_ == Phase::Falling && tryMove(-1, 0)) { push(EventType::PieceMoved); lockAcc_ = 0.f; }
}
void Game::moveRight() {
    if (phase_ == Phase::Falling && tryMove(1, 0)) { push(EventType::PieceMoved); lockAcc_ = 0.f; }
}
void Game::rotateCW() {
    if (phase_ == Phase::Falling && tryRotate(1)) { push(EventType::PieceRotated); lockAcc_ = 0.f; }
}
void Game::rotateCCW() {
    if (phase_ == Phase::Falling && tryRotate(-1)) { push(EventType::PieceRotated); lockAcc_ = 0.f; }
}

void Game::hardDrop() {
    if (phase_ != Phase::Falling || !active_) return;
    int rows = 0;
    while (tryMove(0, 1)) ++rows;
    score_ += rows * 2;
    push(EventType::HardDrop, rows);
    lock();
}

std::optional<Piece> Game::ghost() const {
    if (!active_) return std::nullopt;
    Piece p = *active_;
    while (true) {
        Piece q = p;
        q.y += 1;
        if (!fits(q)) break;
        p = q;
    }
    return p;
}

// ---------------------------------------------------------------------------
void Game::lock() {
    if (!active_) return;
    const Piece& p = *active_;
    int colour = static_cast<int>(p.shape);
    bool anyAbove = false;
    for (int i = 0; i < 4; ++i) {
        auto [cx, cy] = p.cells()[i];
        if (cy < 0) { anyAbove = true; continue; }
        Cell c;
        c.kind = p.red[i] ? CellKind::Red : CellKind::Normal;
        c.color = static_cast<uint8_t>(colour);
        grid_[cy][cx] = c;
    }
    active_.reset();
    push(EventType::PieceLocked);
    if (anyAbove) {
        phase_ = Phase::GameOver;
        push(EventType::GameOver);
        return;
    }
    clearFromSettle_ = false;
    chain_ = 0;
    beginClearOrSettle();
}

bool Game::rowFull(int row) const {
    for (int c = 0; c < kBoardW; ++c)
        if (grid_[row][c].empty()) return false;
    return true;
}

bool Game::rowAllRed(int row) const {
    for (int c = 0; c < kBoardW; ++c)
        if (!grid_[row][c].red()) return false;
    return true;
}

std::vector<int> Game::redRows() const {
    std::vector<int> out;
    for (int r = 0; r < kBoardH; ++r)
        if (rowAllRed(r)) out.push_back(r);
    return out;
}

bool Game::checkRedLine() {
    auto rows = redRows();
    if (rows.empty()) return false;
    phase_ = Phase::RedLine;
    ++redLineEvents_;
    push(EventType::RedLine, rows.front());
    return true;
}

void Game::beginClearOrSettle() {
    clearing_.clear();
    // A row that is completely red does not clear: it is the trigger.
    for (int r = 0; r < kBoardH; ++r)
        if (rowFull(r) && !rowAllRed(r)) clearing_.push_back(r);
    phaseAcc_ = 0.f;
    if (!clearing_.empty()) {
        phase_ = Phase::Clearing;
        return;
    }
    if (!clearFromSettle_) combo_ = 0;   // a piece that clears nothing ends the combo
    if (checkRedLine()) return;
    phase_ = Phase::Settling;
}

// Clear the flashing rows. Classic Tetris removes the whole row and shifts
// everything above down by one. Here the shift happens PER COLUMN: a column
// whose cell in the cleared row is red keeps that cell (it "refuses" to
// dissolve) and therefore nothing in that column moves. The red cell later
// sinks when a normal cell beneath it is cleared, or immediately in the
// settle phase if there is empty space under it.
void Game::finishClear() {
    int cleared = 0;
    for (int r : clearing_) {   // ascending order: shifting rows above r never disturbs rows below
        for (int c = 0; c < kBoardW; ++c) {
            if (grid_[r][c].red()) continue;
            for (int y = r; y > 0; --y) grid_[y][c] = grid_[y - 1][c];
            grid_[0][c] = Cell{};
        }
        ++cleared;
    }
    clearing_.clear();
    // Score: bigger clears pay disproportionately; consecutive clearing pieces
    // build a combo; clears caused by a collapse (cascade) chain on top.
    if (clearFromSettle_) ++chain_; else combo_ += 1;
    float mult = (1.f + rules_.comboStep * static_cast<float>(std::max(0, combo_ - 1))) * (1.f + rules_.chainStep * static_cast<float>(chain_));
    lastClearPoints_ = static_cast<int>(static_cast<float>(rules_.lineScore[std::min(cleared, 4)] * level_) * mult);
    score_ += lastClearPoints_;
    lines_ += cleared;
    push(EventType::LinesCleared, cleared, lastClearPoints_);
    int newLevel = 1 + lines_ / rules_.linesPerLevel;
    if (newLevel != level_) {
        level_ = newLevel;
        push(EventType::LevelUp, level_);
    }
    phase_ = Phase::Settling;
    phaseAcc_ = 0.f;
}

// Move every unsupported red cell down by one row. Bottom-up so a stack of
// red cells moves together.
bool Game::settleStep() {
    if (!rules_.redCellsSettle && !collapseAll_) return false;
    bool moved = false;
    for (int r = kBoardH - 2; r >= 0; --r) {
        for (int c = 0; c < kBoardW; ++c) {
            bool loose = grid_[r][c].red() || (collapseAll_ && !grid_[r][c].empty());
            if (loose && grid_[r + 1][c].empty()) {
                grid_[r + 1][c] = grid_[r][c];
                grid_[r][c] = Cell{};
                push(EventType::RedCellFell, c, r + 1);
                moved = true;
            }
        }
    }
    if (!moved) collapseAll_ = false;
    return moved;
}

// ---------------------------------------------------------------------------
void Game::tick(float dt) {
    switch (phase_) {
    case Phase::Spawning:
        spawn();
        break;
    case Phase::Falling: {
        if (!active_) { phase_ = Phase::Spawning; break; }
        gravityAcc_ += dt;
        float interval = gravityInterval();
        while (gravityAcc_ >= interval) {
            gravityAcc_ -= interval;
            if (tryMove(0, 1)) {
                if (softDrop_) score_ += 1;
                lockAcc_ = 0.f;
            } else {
                gravityAcc_ = 0.f;
                break;
            }
        }
        // Corruption: flashing blocks count down and turn red; new ones start
        // at a rate that grows with the height of the stack.
        {
            bool turned = false;
            for (int r = 0; r < kBoardH; ++r)
                for (int c = 0; c < kBoardW; ++c) {
                    Cell& cell = grid_[r][c];
                    if (!cell.corrupting()) continue;
                    cell.corrupt -= dt;
                    if (cell.corrupt <= 0.f) {
                        cell.corrupt = 0.f;
                        cell.kind = CellKind::Red;
                        push(EventType::CellTurnedRed, c, r);
                        turned = true;
                    }
                }
            float d = danger();
            if (d > 0.f) {
                float rate = rules_.corruptionRate * d * d * (1.f + 0.1f * static_cast<float>(level_ - 1));
                std::uniform_real_distribution<float> u(0.f, 1.f);
                if (u(rng_) < rate * dt) {
                    std::vector<std::pair<int, int>> candidates;
                    for (int r = 0; r < kBoardH; ++r)
                        for (int c = 0; c < kBoardW; ++c)
                            if (grid_[r][c].kind == CellKind::Normal && grid_[r][c].corrupt <= 0.f) candidates.push_back({c, r});
                    if (!candidates.empty()) {
                        auto [c, r] = candidates[static_cast<size_t>(u(rng_) * static_cast<float>(candidates.size())) % candidates.size()];
                        grid_[r][c].corrupt = rules_.corruptionTime;
                        push(EventType::CellCorrupting, c, r);
                    }
                }
            }
            if (turned && checkRedLine()) break;   // the piece in the air resumes after the fight
        }
        // Lock delay: resting on something for a while locks the piece.
        Piece below = *active_;
        below.y += 1;
        if (!fits(below)) {
            lockAcc_ += dt;
            if (lockAcc_ >= rules_.lockDelay) lock();
        } else {
            lockAcc_ = 0.f;
        }
        break;
    }
    case Phase::Clearing:
        phaseAcc_ += dt;
        if (phaseAcc_ >= rules_.clearAnimTime) finishClear();
        break;
    case Phase::Settling:
        phaseAcc_ += dt;
        if (phaseAcc_ >= rules_.settleStepTime) {
            phaseAcc_ = 0.f;
            if (!settleStep()) {
                // Settling can complete a full row (a red cell landing in a
                // gap) so re-run the clear check before spawning.
                bool anyFull = false;
                for (int r = 0; r < kBoardH; ++r) if (rowFull(r) && !rowAllRed(r)) anyFull = true;
                if (anyFull) { clearFromSettle_ = true; beginClearOrSettle(); break; }
                if (checkRedLine()) break;
                phase_ = Phase::Spawning;
            }
        }
        break;
    case Phase::RedLine:
    case Phase::GameOver:
        break;
    }
}

float Game::clearProgress() const {
    if (phase_ != Phase::Clearing || rules_.clearAnimTime <= 0.f) return 0.f;
    return std::min(1.f, phaseAcc_ / rules_.clearAnimTime);
}

std::vector<Event> Game::drainEvents() {
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

// ---------------------------------------------------------------------------
// FPS-mode hooks
int Game::explodeAt(int col, int row, float radius) {
    int destroyed = 0;
    if (col >= 0 && col < kBoardW && row >= 0 && row < kBoardH) grid_[row][col] = Cell{};
    int rr = static_cast<int>(std::ceil(radius));
    for (int y = row - rr; y <= row + rr; ++y) {
        for (int x = col - rr; x <= col + rr; ++x) {
            if (x < 0 || x >= kBoardW || y < 0 || y >= kBoardH) continue;
            float dx = static_cast<float>(x - col), dy = static_cast<float>(y - row);
            if (dx * dx + dy * dy > radius * radius + 1e-4f) continue;
            if (grid_[y][x].kind == CellKind::Normal) {
                grid_[y][x] = Cell{};
                ++destroyed;
            }
        }
    }
    return destroyed;
}

void Game::clearCell(int col, int row) {
    if (col >= 0 && col < kBoardW && row >= 0 && row < kBoardH) grid_[row][col] = Cell{};
}

void Game::resumeAfterRedLine() {
    if (phase_ != Phase::RedLine) return;
    score_ += 1000 * level_;
    ++level_;
    push(EventType::LevelUp, level_);
    collapseAll_ = true;
    phase_ = Phase::Settling;
    phaseAcc_ = 0.f;
}

int Game::stackRows() const {
    for (int r = 0; r < kBoardH; ++r)
        for (int c = 0; c < kBoardW; ++c)
            if (!grid_[r][c].empty()) return kBoardH - r;
    return 0;
}

float Game::danger() const {
    int rows = stackRows();
    int span = kBoardH - rules_.corruptionStartRows;
    if (rows <= rules_.corruptionStartRows || span <= 0) return 0.f;
    return std::min(1.f, static_cast<float>(rows - rules_.corruptionStartRows) / static_cast<float>(span));
}

void Game::clearAllRed() {
    for (auto& row : grid_)
        for (auto& c : row)
            if (c.red()) c = Cell{};
}

std::vector<RedRegion> findRedRegions(const Game& g) {
    std::vector<RedRegion> out;
    bool seen[kBoardH][kBoardW] = {};
    for (int r = 0; r < kBoardH; ++r) {
        for (int c = 0; c < kBoardW; ++c) {
            if (seen[r][c] || !g.at(c, r).red()) continue;
            RedRegion region;
            std::vector<std::pair<int, int>> stack{{c, r}};
            seen[r][c] = true;
            while (!stack.empty()) {
                auto [x, y] = stack.back();
                stack.pop_back();
                region.cells.push_back({x, y});
                const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    int nx = x + dx[k], ny = y + dy[k];
                    if (nx < 0 || nx >= kBoardW || ny < 0 || ny >= kBoardH) continue;
                    if (seen[ny][nx] || !g.at(nx, ny).red()) continue;
                    seen[ny][nx] = true;
                    stack.push_back({nx, ny});
                }
            }
            float sc = 0.f, sr = 0.f;
            for (auto [x, y] : region.cells) { sc += static_cast<float>(x); sr += static_cast<float>(y); }
            region.centroidCol = sc / static_cast<float>(region.cells.size());
            region.centroidRow = sr / static_cast<float>(region.cells.size());
            float best = 1e9f;
            for (auto [x, y] : region.cells) {
                float d = (x - region.centroidCol) * (x - region.centroidCol) + (y - region.centroidRow) * (y - region.centroidRow);
                if (d < best) { best = d; region.anchorCol = x; region.anchorRow = y; }
            }
            out.push_back(std::move(region));
        }
    }
    // Biggest regions first so the toughest enemies get placed first.
    std::sort(out.begin(), out.end(), [](const RedRegion& a, const RedRegion& b) { return a.size() > b.size(); });
    return out;
}

}  // namespace rl::core
