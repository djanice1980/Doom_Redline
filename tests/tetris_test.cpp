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

static void testRedCellsStayInPiece() {
    // An O piece with its right column red lands with that column hanging over
    // empty space. The red minos must stay with the piece, not fall away.
    Game g(1, fastRules());
    g.setCell(3, 19, Cell{CellKind::Normal, 0});
    g.setCell(3, 18, Cell{CellKind::Normal, 0});
    g.forcePiece(Shape::O, {false, true, false, true});   // minos (2,0) and (2,1) red -> column x+2
    g.spawnNow();
    g.moveLeft();   // x=2 -> columns 3 and 4
    g.hardDrop();
    runUntil(g, Phase::Falling);
    CHECK(g.at(3, 17).kind == CellKind::Normal);
    CHECK(g.at(3, 16).kind == CellKind::Normal);
    CHECK(g.at(4, 17).red());
    CHECK(g.at(4, 16).red());
    CHECK(g.at(4, 18).empty());
    CHECK(g.at(4, 19).empty());
    // The opt-in sand rule still works when asked for.
    Rules sand = fastRules();
    sand.redCellsSettle = true;
    Game h(1, sand);
    h.setCell(3, 19, Cell{CellKind::Normal, 0});
    h.setCell(3, 18, Cell{CellKind::Normal, 0});
    h.forcePiece(Shape::O, {false, true, false, true});
    h.spawnNow();
    h.moveLeft();
    h.hardDrop();
    runUntil(h, Phase::Falling);
    CHECK(h.at(4, 19).red() && h.at(4, 18).red());
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
    g.setCell(6, 12, Cell{CellKind::Normal, 2});   // floating leftovers
    g.setCell(6, 11, Cell{CellKind::Normal, 3});
    g.resumeAfterRedLine();
    CHECK(g.collapsing());
    runUntil(g, Phase::Falling);
    CHECK(g.phase() == Phase::Falling);
    CHECK(!g.collapsing());
    // After a fight everything left falls to the floor, keeping its column order.
    CHECK(g.at(2, 19).kind == CellKind::Normal);
    CHECK(g.at(2, 17).empty());
    CHECK(g.at(6, 19).color == 2);
    CHECK(g.at(6, 18).color == 3);
    CHECK(g.at(6, 12).empty() && g.at(6, 11).empty());
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

static void testRedRegions() {
    Game g(1, fastRules());
    fill(g, 19, CellKind::Red);                       // 10-cell row
    g.setCell(3, 18, Cell{CellKind::Red, 0});         // attached above -> same region (11)
    g.setCell(0, 10, Cell{CellKind::Red, 0});         // single
    g.setCell(7, 5, Cell{CellKind::Red, 0});          // pair
    g.setCell(8, 5, Cell{CellKind::Red, 0});
    g.setCell(9, 4, Cell{CellKind::Red, 0});          // diagonal only -> separate single
    auto regions = findRedRegions(g);
    CHECK(regions.size() == 4);
    CHECK(regions[0].size() == 11);
    CHECK(regions[1].size() == 2);
    CHECK(regions[2].size() == 1 && regions[3].size() == 1);
    CHECK(regions[0].anchorRow == 19);                // anchor is a real region cell
    bool anchorInRegion = false;
    for (auto [x, y] : regions[0].cells) if (x == regions[0].anchorCol && y == regions[0].anchorRow) anchorInRegion = true;
    CHECK(anchorInRegion);
    CHECK(regions[1].anchorRow == 5 && (regions[1].anchorCol == 7 || regions[1].anchorCol == 8));
    int before = g.level();
    fill(g, 17, CellKind::Normal, 0);
    g.forcePiece(Shape::I, {});
    g.spawnNow();
    g.rotateCW();
    for (int i = 0; i < 6; ++i) g.moveLeft();
    g.hardDrop();
    runUntil(g, Phase::RedLine);
    CHECK(g.phase() == Phase::RedLine);
    g.clearAllRed();
    CHECK(findRedRegions(g).empty());
    g.resumeAfterRedLine();
    CHECK(g.level() == before + 1);                  // each fight raises the level
}

static int dropIInColumn0(Game& g) {
    g.forcePiece(Shape::I, {});
    g.spawnNow();
    g.rotateCW();
    for (int i = 0; i < 6; ++i) g.moveLeft();
    int before = g.score();
    g.hardDrop();
    runUntil(g, Phase::Falling);
    return g.score() - before - 2 * 0;   // hard-drop bonus is included, same for every call
}

static void testScoring() {
    // 4 lines pay more than 3 + 1, and a second clearing piece in a row pays a combo.
    Game a(1, fastRules());
    for (int r = 16; r < 20; ++r) fill(a, r, CellKind::Normal, 0);
    int four = dropIInColumn0(a);
    Game b(1, fastRules());
    for (int r = 17; r < 20; ++r) fill(b, r, CellKind::Normal, 0);
    int three = dropIInColumn0(b);
    CHECK(four > three);
    CHECK(four >= 1000);
    CHECK(three >= 600 && three < 1000);
    CHECK(a.combo() == 1);
    // Combo: clear again immediately with the next piece -> x1.5.
    for (int r = 16; r < 20; ++r) fill(a, r, CellKind::Normal, 0);
    int fourAgain = dropIInColumn0(a);
    CHECK(a.combo() == 2);
    CHECK(fourAgain > four);
    // A piece that clears nothing ends the combo.
    a.forcePiece(Shape::O, {});
    a.spawnNow();
    a.hardDrop();
    runUntil(a, Phase::Falling);
    CHECK(a.combo() == 0);
}

static void testCascadeChain() {
    // After a fight the collapse completes a row: that clear counts as a chain step.
    Game g(1, fastRules());
    fill(g, 19, CellKind::Red);
    fill(g, 17, CellKind::Normal, 3);              // row 17 full except column 3
    g.setCell(3, 15, Cell{CellKind::Normal, 0});   // will fall into the gap when everything collapses
    g.forcePiece(Shape::I, {});
    g.spawnNow();
    for (int i = 0; i < 200 && g.phase() == Phase::Falling; ++i) g.tick(0.02f);
    runUntil(g, Phase::RedLine);
    CHECK(g.phase() == Phase::RedLine);
    g.clearAllRed();
    int before = g.score();
    g.resumeAfterRedLine();
    runUntil(g, Phase::Falling);
    CHECK(g.lines() >= 1);
    CHECK(g.chain() >= 1);
    CHECK(g.score() - before >= 1000 + 2 * 100 * 2);   // 1000 x level bonus + at least one chained single (x2, level 2)
}

static void testCorruption() {
    Rules r = fastRules();
    r.corruptionRate = 50.f;      // make it near-certain within a few ticks
    r.corruptionTime = 0.05f;
    Game g(3, r);
    for (int row = 4; row < 20; ++row) fill(g, row, CellKind::Normal, 0);   // 16 rows: danger ~0.64
    CHECK(g.stackRows() == 16);
    CHECK(g.danger() > 0.5f && g.danger() < 0.7f);
    g.forcePiece(Shape::I, {});
    g.spawnNow();
    bool sawCorrupting = false, sawTurned = false;
    for (int i = 0; i < 20 && g.phase() == Phase::Falling; ++i) {
        g.tick(0.02f);
        for (const Event& e : g.drainEvents()) {
            if (e.type == EventType::CellCorrupting) sawCorrupting = true;
            if (e.type == EventType::CellTurnedRed) sawTurned = true;
        }
    }
    CHECK(sawCorrupting);
    CHECK(sawTurned);
    int reds = 0;
    for (int row = 0; row < 20; ++row) for (int c = 0; c < 10; ++c) reds += g.at(c, row).red();
    CHECK(reds > 0);
    // A low stack never corrupts.
    Game h(3, r);
    fill(h, 19, CellKind::Normal, 0);
    CHECK(h.danger() == 0.f);
}

int main() {
    testScoring();
    testCascadeChain();
    testCorruption();
    testRedRegions();
    testShapesCover();
    testClassicLineClear();
    testRedCellSurvivesClear();
    testRedCellsStayInPiece();
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
