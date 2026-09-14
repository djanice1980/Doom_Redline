// Rule tests for the REDLINE block engine. Plain asserts, no framework.
#include "core/tetris.h"

#include <cstdio>
#include <cstdlib>

using namespace rl::core;

static int g_fail = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_fail;                                                                 \
        }                                                                             \
    } while (0)

static Rules fastRules() {
    Rules r;
    r.clearAnimTime = 0.01f;
    r.settleStepTime = 0.01f;
    r.lockDelay = 0.01f;
    r.redChanceBase = 0.f;
    r.redChancePerLevel = 0.f;
    return r;
}

static void fill(Game& g, int row, CellKind k, int except = -1) {
    for (int c = 0; c < kBoardW; ++c)
        if (c != except) g.setCell(c, row, Cell{k, 0});
}

static void runUntil(Game& g, Phase p, int maxTicks = 10000) {
    for (int i = 0; i < maxTicks && g.phase() != p; ++i) g.tick(0.02f);
}

static void testShapesCover() {
    for (int s = 0; s < static_cast<int>(Shape::Count); ++s)
        for (int r = 0; r < 4; ++r) {
            auto& cells = shapeCells(static_cast<Shape>(s), r);
            for (auto [x, y] : cells) CHECK(x >= 0 && x < 4 && y >= 0 && y < 4);
        }
}

static void testClassicLineClear() {
    Game g(1, fastRules());
    // Bottom row full except column 0; drop a vertical I into column 0.
    fill(g, 19, CellKind::Normal, 0);
    fill(g, 18, CellKind::Normal, 0);
    g.setCell(5, 17, Cell{CellKind::Normal, 3});   // a cell above that must fall by 2
    g.forcePiece(Shape::I, {});
    g.spawnNow();
    CHECK(g.phase() == Phase::Falling);
    g.rotateCW();                          // vertical I at x=3+2=5 -> move to column 0
    for (int i = 0; i < 6; ++i) g.moveLeft();
    CHECK(g.active()->cells()[0].first == 0);
    g.hardDrop();
    runUntil(g, Phase::Falling);           // through Clearing, Settling, Spawning
    CHECK(g.lines() == 2);
    CHECK(g.at(5, 19).kind == CellKind::Normal);   // the loose cell fell to the floor
    CHECK(g.at(5, 17).empty());
    CHECK(g.at(0, 19).kind == CellKind::Normal);   // remaining 2 minos of the I
    CHECK(g.at(0, 18).kind == CellKind::Normal);
    CHECK(g.at(0, 17).empty());
    CHECK(g.at(1, 19).empty());
}

static void testRedCellSurvivesClear() {
    Game g(1, fastRules());
    fill(g, 19, CellKind::Normal, 0);
    g.setCell(4, 19, Cell{CellKind::Red, 0});      // red cell inside the row that will clear
    g.setCell(4, 18, Cell{CellKind::Normal, 1});   // cell stacked on the red one
    g.setCell(6, 18, Cell{CellKind::Normal, 1});   // cell stacked on a normal one
    g.forcePiece(Shape::I, {});
    g.spawnNow();
    g.rotateCW();
    for (int i = 0; i < 6; ++i) g.moveLeft();
    g.hardDrop();
    runUntil(g, Phase::Falling);
    CHECK(g.lines() == 1);
    CHECK(g.at(4, 19).red());                      // red stayed
    CHECK(g.at(4, 18).kind == CellKind::Normal);   // column 4 did not shift
    CHECK(g.at(6, 19).kind == CellKind::Normal);   // column 6 shifted down
    CHECK(g.at(6, 18).empty());
    CHECK(g.at(1, 19).empty());
}

static void testRedCellsSettleAfterLock() {
    Game g(1, fastRules());
    // T piece with the top mino red, dropped onto the empty floor: the red
    // mino lands at row 18 above the stem, has nothing below it? No - the T's
    // top mino sits on the middle mino. Use an L with its tip red so the tip
    // hangs over empty space? The L tip sits on row 19 too. Use a vertical I
    // with the top mino red placed over a hole: after locking, the red is
    // supported. So instead build a step: red cell at row 17 over a hole.
    g.setCell(3, 19, Cell{CellKind::Normal, 0});
    g.setCell(3, 18, Cell{CellKind::Normal, 0});
    // Piece: O with left column red, positioned with x=3 so cells at (4,·),(5,·):
    // O occupies (1,0),(2,0),(1,1),(2,1) -> x=3 gives columns 4,5. Column 4 sits at
    // rows 18,19 (floor). Make (1,0) red -> (4,18) red on top of (4,19) normal. Supported.
    // Instead make the O land on the 2-high column 3: move it left by one so it
    // occupies columns 3,4: column 3 rests on the stack (rows 16,17), column 4 hangs
    // over empty rows 18,19 -> the red minos in column 4 must fall to the floor.
    g.forcePiece(Shape::O, {false, true, false, true});   // minos (2,0) and (2,1) red -> column x+2
    g.spawnNow();
    g.moveLeft();   // x=2 -> columns 3 and 4
    g.hardDrop();
    runUntil(g, Phase::Falling);
    CHECK(g.at(3, 17).kind == CellKind::Normal);
    CHECK(g.at(3, 16).kind == CellKind::Normal);
    CHECK(g.at(4, 19).red());
    CHECK(g.at(4, 18).red());
    CHECK(g.at(4, 17).empty());
    CHECK(g.at(4, 16).empty());
}

static void testRedLineTrigger() {
    Game g(1, fastRules());
    fill(g, 19, CellKind::Red, 0);
    g.forcePiece(Shape::I, {true, true, true, true});
    g.spawnNow();
    g.rotateCW();
    for (int i = 0; i < 6; ++i) g.moveLeft();
    g.hardDrop();
    runUntil(g, Phase::RedLine);
    CHECK(g.phase() == Phase::RedLine);
    CHECK(g.redRows().size() == 1 && g.redRows()[0] == 19);
    CHECK(g.redLineCount() == 1);
    // Explode the whole row like the FPS mode would, with neighbours to destroy.
    g.setCell(2, 18, Cell{CellKind::Normal, 0});
    g.setCell(2, 17, Cell{CellKind::Normal, 0});   // radius 1.5 must NOT reach two rows up
    int destroyed = 0;
    for (int c = 0; c < kBoardW; ++c) destroyed += g.explodeAt(c, 19, 1.5f);
    CHECK(destroyed == 1);
    CHECK(g.at(2, 18).empty());
    CHECK(g.at(2, 17).kind == CellKind::Normal);
    CHECK(g.redRows().empty());
    g.resumeAfterRedLine();
    runUntil(g, Phase::Falling);
    CHECK(g.phase() == Phase::Falling);
    CHECK(g.at(2, 17).kind == CellKind::Normal);   // normal cells do not fall
}

static void testRedRowDoesNotClear() {
    Game g(1, fastRules());
    fill(g, 19, CellKind::Red);
    fill(g, 18, CellKind::Normal, 0);
    g.forcePiece(Shape::I, {});
    g.spawnNow();
    g.rotateCW();
    for (int i = 0; i < 6; ++i) g.moveLeft();
    g.hardDrop();
    // Row 18 clears, row 19 (all red) does not, and the red line triggers.
    runUntil(g, Phase::RedLine);
    CHECK(g.phase() == Phase::RedLine);
    CHECK(g.lines() == 1);
    for (int c = 0; c < kBoardW; ++c) CHECK(g.at(c, 19).red());
}

static void testGameOver() {
    Game g(1, fastRules());
    for (int r = 0; r < kBoardH; ++r) fill(g, r, CellKind::Normal, 9);
    g.forcePiece(Shape::O, {});
    g.spawnNow();
    // O at x=3 occupies columns 4,5 rows 0,1 which are full -> cannot spawn.
    CHECK(g.phase() == Phase::GameOver);
}

static void testDeterminism() {
    Game a(42), b(42);
    for (int i = 0; i < 2000; ++i) {
        a.tick(0.05f);
        b.tick(0.05f);
        if (i % 7 == 0) { a.rotateCW(); b.rotateCW(); }
        if (i % 3 == 0) { a.moveLeft(); b.moveLeft(); }
        if (i % 11 == 0) { a.hardDrop(); b.hardDrop(); }
    }
    CHECK(a.score() == b.score());
    CHECK(a.phase() == b.phase());
    for (int r = 0; r < kBoardH; ++r)
        for (int c = 0; c < kBoardW; ++c) CHECK(a.at(c, r).kind == b.at(c, r).kind);
}

static void testRedPiecesAppear() {
    Rules r;
    r.redChanceBase = 0.5f;
    Game g(7, r);
    int reds = 0;
    for (int i = 0; i < 50; ++i) {
        for (bool b : g.next().red) reds += b;
        g.forcePiece(g.next().shape, g.next().red);
        g.spawnNow();
        g.hardDrop();
        runUntil(g, Phase::Spawning, 500);
        if (g.phase() == Phase::GameOver || g.phase() == Phase::RedLine) break;
    }
    CHECK(reds > 0);
    CHECK(g.next().red[0] + g.next().red[1] + g.next().red[2] + g.next().red[3] <= r.maxRedPerPiece);
}

int main() {
    testShapesCover();
    testClassicLineClear();
    testRedCellSurvivesClear();
    testRedCellsSettleAfterLock();
    testRedLineTrigger();
    testRedRowDoesNotClear();
    testGameOver();
    testDeterminism();
    testRedPiecesAppear();
    if (g_fail) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("tetris_test: all checks passed\n");
    return 0;
}
