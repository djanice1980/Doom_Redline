#pragma once
// Pure game logic for the block-stacking half of REDLINE. No rendering, no
// platform code, deterministic given a seed. Rules that differ from classic
// Tetris are documented on the Rules struct and in docs/design.md.
#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace rl::core {

constexpr int kBoardW = 10;
constexpr int kBoardH = 20;

enum class CellKind : uint8_t { Empty = 0, Normal = 1, Red = 2 };

struct Cell {
    CellKind kind = CellKind::Empty;
    uint8_t color = 0;   // tetromino colour index 0..6 for Normal cells
    float corrupt = 0.f; // > 0: seconds left before this cell turns Red (a Normal cell flashes; an Empty one is being filled by evil)
    bool empty() const { return kind == CellKind::Empty; }
    bool red() const { return kind == CellKind::Red; }
    bool corrupting() const { return kind == CellKind::Normal && corrupt > 0.f; }
    bool spawning() const { return kind == CellKind::Empty && corrupt > 0.f; }
};

enum class Shape : uint8_t { I, O, T, S, Z, J, L, Count };

struct Piece {
    Shape shape = Shape::I;
    int rot = 0;      // 0..3
    int x = 0;        // board column of the piece origin
    int y = 0;        // board row (0 = top)
    std::array<bool, 4> red{};   // which of the 4 minos are red (index = mino order in the shape table)
    // Absolute board coordinates of the 4 minos for the current rot/x/y.
    std::array<std::pair<int, int>, 4> cells() const;
};

struct Rules {
    // Probability that any given mino of a new piece is red, at level 1.
    float redChanceBase = 0.10f;
    // Added per level.
    float redChancePerLevel = 0.02f;
    float redChanceMax = 0.45f;
    // Max red minos in a single piece.
    int maxRedPerPiece = 2;
    // If true, red cells are loose sand that falls through empty space after
    // every lock. Off by default: a red mino stays part of the piece it landed
    // in and only moves when a line clear beneath it (per-column collapse) or
    // the post-fight collapse drops it.
    bool redCellsSettle = false;
    // Gravity interval at level 1 (seconds per row) and scaling.
    float gravityBase = 0.80f;
    float gravityPerLevel = 0.85f;   // multiplicative per level
    float gravityMin = 0.06f;
    float softDropMultiplier = 12.0f;
    float lockDelay = 0.40f;          // seconds resting before lock
    float clearAnimTime = 0.30f;      // seconds the cleared row flashes
    float settleStepTime = 0.05f;     // seconds per one-row red-cell fall
    int linesPerLevel = 10;
    // Corruption: as the stack climbs, normal blocks turn evil. danger() ramps
    // from 0 at `corruptionStartRows` filled rows to 1 at the top; corruption
    // events fire at corruptionRate * danger^2 per second (plus 10 %/level).
    // Each event picks the row closest to becoming all red (most red cells,
    // then fullest, then lowest) and turns 1..(1 + danger * corruptionBurst)
    // random normal blocks in it, so the stack is pushed towards a fight.
    int corruptionStartRows = 8;
    float corruptionRate = 0.7f;
    float corruptionTime = 1.6f;      // seconds of flashing before the switch
    int corruptionBurst = 4;          // extra blocks per event at full danger
    // Evil spawn: an empty cell fully surrounded by red gets filled by a new
    // red block. All four neighbours (left, right, above, below) must be red or
    // turning; on the bottom row the floor stands in for "below", and the side
    // walls stand in for a missing left/right, but at least two of the
    // neighbours must be real red blocks and there must be a block above.
    // Fires more readily than corruption and does not need a high stack:
    // rate = evilSpawnRate * (0.3 + 0.7 * danger) per second.
    float evilSpawnRate = 1.4f;
    float evilSpawnTime = 1.2f;       // seconds of warning before the block appears
    // Scoring
    int lineScore[5] = {0, 100, 300, 600, 1000};   // x level x combo/chain multipliers
    float comboStep = 0.5f;           // +50 % per consecutive clearing piece
    float chainStep = 1.0f;           // +100 % per cascade step (clear caused by a collapse)
};

enum class Phase : uint8_t {
    Spawning,   // about to spawn next piece
    Falling,    // active piece under player control
    Clearing,   // full rows flashing; normal cells about to vanish
    Settling,   // loose red cells falling one row per step
    RedLine,    // a full red row exists -> the FPS mode takes over (game logic paused)
    GameOver,
};

enum class EventType : uint8_t {
    PieceSpawned,
    PieceMoved,
    PieceRotated,
    PieceLocked,
    LinesCleared,      // a = number of rows
    RedCellFell,       // a = column, b = new row
    RedLine,           // a = row index (first full red row found)
    LevelUp,           // a = new level
    GameOver,
    HardDrop,          // a = rows dropped
    CellCorrupting,    // a = column, b = row: a normal block started turning red
    CellTurnedRed,     // a = column, b = row
    EvilSpawning,      // a = column, b = row: evil is filling an empty cell
    EvilSpawned,       // a = column, b = row: the new red block is in place
};

struct Event {
    EventType type;
    int a = 0;
    int b = 0;
};

class Game {
public:
    explicit Game(uint32_t seed = 1, Rules rules = {});

    // --- simulation -------------------------------------------------------
    void tick(float dt);

    // --- player input (ignored unless Phase::Falling) ---------------------
    void moveLeft();
    void moveRight();
    void rotateCW();
    void rotateCCW();
    void setSoftDrop(bool on) { softDrop_ = on; }
    void hardDrop();

    // --- FPS-mode interface ----------------------------------------------
    // Rows that are entirely red (the trigger condition). Empty unless phase is RedLine.
    std::vector<int> redRows() const;
    // Called by the FPS mode when an enemy at (col,row) is killed: removes the
    // red cell and destroys Normal cells within `radius` (Euclidean, in cells).
    // Returns number of normal cells destroyed.
    int explodeAt(int col, int row, float radius);
    // Remove a single cell without side effects (e.g. enemy walked away).
    void clearCell(int col, int row);
    // Call when the FPS mode is over; resumes normal play and raises the level
    // by one so each fight makes the stacking harder. Every remaining block
    // (not just red ones) then falls to the floor, one row per settle step.
    void resumeAfterRedLine();
    bool collapsing() const { return collapseAll_; }
    // Removes every red cell (used when the FPS phase consumed them all).
    void clearAllRed();

    // --- state access -----------------------------------------------------
    const Cell& at(int col, int row) const { return grid_[row][col]; }
    Cell& at(int col, int row) { return grid_[row][col]; }
    Phase phase() const { return phase_; }
    const std::optional<Piece>& active() const { return active_; }
    const Piece& next() const { return next_; }
    // Ghost piece landing row for the active piece (same x/rot).
    std::optional<Piece> ghost() const;
    int score() const { return score_; }
    void addScore(int points) { score_ += points; }
    int level() const { return level_; }
    int combo() const { return combo_; }          // consecutive clearing pieces (0 = none active)
    int chain() const { return chain_; }          // cascade depth of the last clear
    int lastClearPoints() const { return lastClearPoints_; }
    int stackRows() const;                        // filled height of the stack in rows
    float danger() const;                         // 0..1 corruption pressure
    int corruptionTargetRow() const;              // row the next corruption hits, -1 if none
    std::vector<std::pair<int, int>> evilSpawnCandidates() const;   // empty cells evil can fill
    int lines() const { return lines_; }
    int redLineCount() const { return redLineEvents_; }
    // Rows currently flashing (only during Clearing).
    const std::vector<int>& clearingRows() const { return clearing_; }
    float clearProgress() const;   // 0..1 during Clearing
    // Events since the last drain; consumed by audio/VFX.
    std::vector<Event> drainEvents();
    const Rules& rules() const { return rules_; }
    uint32_t seed() const { return seed_; }

    // Debug/test helpers
    void setCell(int col, int row, Cell c) { grid_[row][col] = c; }
    void forcePiece(Shape s, std::array<bool, 4> red);   // replaces the next piece (and the active one, if falling)
    void spawnNow();                                     // spawn immediately (phase must be Spawning)
    void setLevel(int level) { level_ = level < 1 ? 1 : level; }
    bool rowFull(int row) const;
    bool rowAllRed(int row) const;

private:
    bool fits(const Piece& p) const;
    bool tryMove(int dx, int dy);
    bool tryRotate(int dir);
    void lock();
    void beginClearOrSettle();
    bool settleStep();            // one row of red-cell falling; false if nothing moved
    bool checkRedLine();          // sets phase RedLine if any full red row
    void finishClear();
    void spawn();
    Piece makePiece();
    float gravityInterval() const;
    void push(EventType t, int a = 0, int b = 0) { events_.push_back({t, a, b}); }

    Rules rules_;
    uint32_t seed_;
    std::mt19937 rng_;
    std::array<std::array<Cell, kBoardW>, kBoardH> grid_{};
    std::optional<Piece> active_;
    Piece next_;
    Phase phase_ = Phase::Spawning;
    float gravityAcc_ = 0.f;
    float lockAcc_ = 0.f;
    float phaseAcc_ = 0.f;
    bool softDrop_ = false;
    std::vector<int> clearing_;
    std::vector<Event> events_;
    int score_ = 0;
    int level_ = 1;
    int lines_ = 0;
    int redLineEvents_ = 0;
    bool collapseAll_ = false;   // settle phase moves every cell, not only red ones
    int combo_ = 0;
    int chain_ = 0;
    bool clearFromSettle_ = false;   // the pending clear was produced by a collapse, not a lock
    int lastClearPoints_ = 0;
    std::vector<Shape> bag_;
};

// A 4-connected group of red cells. Larger groups become bigger enemies.
struct RedRegion {
    std::vector<std::pair<int, int>> cells;   // (col,row)
    float centroidCol = 0.f, centroidRow = 0.f;
    int anchorCol = 0, anchorRow = 0;         // the region cell nearest the centroid
    int size() const { return static_cast<int>(cells.size()); }
};
std::vector<RedRegion> findRedRegions(const Game& g);

// Shape table: 4 rotations x 4 minos, (dx, dy) relative to piece origin.
const std::array<std::pair<int, int>, 4>& shapeCells(Shape s, int rot);

}  // namespace rl::core
