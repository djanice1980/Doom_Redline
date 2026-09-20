#pragma once
// A block-phase player for testing: looks at the active piece and the stack,
// tries every rotation and column, and picks the landing that keeps the stack
// low, flat and free of holes while clearing lines (the classic Dellacherie /
// El-Tetris style weights). It never sees the future, never holds, and treats
// red minos like any other: that is enough to play well past level 10, and to
// exercise line clears, red lines, corruption and collapses the way a person's
// game would.
#include "core/tetris.h"

namespace rl::core {

struct BotPlan {
    int rot = 0;       // rotation to reach first (with the game's own kicks)
    int x = 0;         // then the origin column to slide to
    float score = 0.f;
    bool valid = false;
};

// Best landing for the active piece; invalid when nothing is falling.
BotPlan planPlacement(const Game& game);

enum class BotMove { None, Left, Right, RotateCW, RotateCCW, Drop };
// The next input that brings the active piece towards the plan (Drop once it is there).
BotMove nextMove(const Game& game, const BotPlan& plan);

// Drives a game one input per call: plans each new piece, steers it, drops it,
// and replans (or just drops) when the piece stops responding, so a kick that
// shifted it or a wall it cannot slide past never leaves it idle.
class TetrisBot {
public:
    // Returns the move it made (None outside the falling phase).
    BotMove step(Game& game);
    const BotPlan& plan() const { return plan_; }
    int pieces() const { return pieces_; }   // pieces it has dropped
private:
    BotPlan plan_;
    bool havePlan_ = false;
    int stuck_ = 0;
    int pieces_ = 0;
};

}  // namespace rl::core
