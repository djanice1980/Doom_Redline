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

static void testCorruptionTargetsReddestRow() {
    Rules r = fastRules();
    r.corruptionRate = 50.f;
    r.corruptionTime = 5.f;   // long flicker so the count is observable
    Game g(9, r);
    for (int row = 6; row < 20; ++row) fill(g, row, CellKind::Normal, 0);   // 14 rows, column 0 empty
    fill(g, 15, CellKind::Normal);                                          // row 15 is full ...
    g.setCell(2, 15, Cell{CellKind::Red, 0});                               // ... and has the most red
    g.setCell(7, 15, Cell{CellKind::Red, 0});
    g.setCell(4, 11, Cell{CellKind::Red, 0});                               // a lone red elsewhere
    CHECK(g.corruptionTargetRow() == 15);
    g.forcePiece(Shape::I, {});
    g.spawnNow();
    int firstRow = -1, count = 0;
    for (int i = 0; i < 40 && firstRow < 0 && g.phase() == Phase::Falling; ++i) {
        g.tick(0.02f);
        for (const Event& e : g.drainEvents())
            if (e.type == EventType::CellCorrupting) { if (firstRow < 0) firstRow = e.b; if (e.b == firstRow) ++count; }
    }
    CHECK(firstRow == 15);
    CHECK(count >= 1 && count <= 1 + static_cast<int>(g.danger() * 4.f));
    // Once row 15 is all red/turning, the next target is the row with the lone red.
    for (int c = 0; c < 10; ++c) { Cell cell = g.at(c, 15); if (cell.kind == CellKind::Normal) { cell.corrupt = 5.f; g.setCell(c, 15, cell); } }
    CHECK(g.corruptionTargetRow() == 11);
}

static Rules floorRules() {
    Rules r = fastRules();
    r.lockDelay = 100.f;     // sit on the floor without locking
    r.gravityBase = 0.01f;   // reach it quickly
    return r;
}

static bool touchesFloor(const Game& g) {
    for (auto [c, r] : g.active()->cells()) if (r == 19) return true;
    return false;
}

static void dropToFloor(Game& g) {
    for (int i = 0; i < 400 && g.phase() == Phase::Falling && !touchesFloor(g); ++i) g.tick(0.02f);
}

static void testRotationKicksOnFloor() {
    // A flat I resting on the floor must rotate both ways (SRS kicks lift it),
    // and keep cycling in either direction without ever sticking.
    for (int dir : {1, -1}) {
        Game g(1, floorRules());
        g.forcePiece(Shape::I, {});
        g.spawnNow();
        dropToFloor(g);
        CHECK(g.phase() == Phase::Falling && touchesFloor(g));
        if (dir > 0) g.rotateCW(); else g.rotateCCW();
        int minRow = 99, maxRow = -1, col0 = g.active()->cells()[0].first;
        bool vertical = true;
        for (auto [c, r] : g.active()->cells()) { minRow = std::min(minRow, r); maxRow = std::max(maxRow, r); vertical = vertical && c == col0; }
        CHECK(vertical);
        CHECK(maxRow == 19 && minRow == 16);
        for (int i = 0; i < 8; ++i) {
            int before = g.active()->rot;
            if (dir > 0) g.rotateCW(); else g.rotateCCW();
            CHECK(g.active()->rot != before);
        }
    }
    // A T flat on the floor rotates both ways too.
    {
        Game g(1, floorRules());
        g.forcePiece(Shape::T, {});
        g.spawnNow();
        dropToFloor(g);
        CHECK(g.phase() == Phase::Falling && touchesFloor(g));
        int rot = g.active()->rot;
        g.rotateCW();
        CHECK(g.active()->rot == (rot + 1) % 4);
        g.rotateCCW();
        g.rotateCCW();
        CHECK(g.active()->rot == (rot + 3) % 4);
    }
    // Against the left wall a vertical I still rotates flat (kicked right).
    {
        Game g(1, floorRules());
        g.forcePiece(Shape::I, {});
        g.spawnNow();
        g.rotateCW();
        for (int i = 0; i < 6; ++i) g.moveLeft();
        CHECK(g.active()->cells()[0].first == 0);
        g.rotateCW();
        int minCol = 99;
        for (auto [c, r] : g.active()->cells()) minCol = std::min(minCol, c);
        CHECK(minCol >= 0 && g.active()->rot == 2);
    }
}

static void testEvilSpawn() {
    Rules r = fastRules();
    r.evilSpawnRate = 100.f;    // near-certain within a few ticks
    r.evilSpawnTime = 0.05f;
    r.corruptionRate = 0.f;
    Game g(4, r);
    fill(g, 19, CellKind::Red, 5);                     // bottom row all red except the hole at column 5 ...
    g.setCell(5, 18, Cell{CellKind::Red, 0});          // ... with red above it: left/right/top red, floor below
    for (int c = 0; c < 10; ++c) if (c != 5 && c != 4 && c != 6) g.setCell(c, 17, Cell{CellKind::Normal, 0});
    auto holes = g.evilSpawnCandidates();
    CHECK(holes.size() == 1 && holes[0].first == 5 && holes[0].second == 19);
    g.forcePiece(Shape::O, {});
    g.spawnNow();
    bool sawSpawning = false, sawSpawned = false;
    for (int i = 0; i < 60 && g.phase() == Phase::Falling; ++i) {
        g.tick(0.02f);
        for (const Event& e : g.drainEvents()) {
            if (e.type == EventType::EvilSpawning) sawSpawning = true;
            if (e.type == EventType::EvilSpawned) sawSpawned = true;
        }
    }
    CHECK(sawSpawning && sawSpawned);
    CHECK(g.at(5, 19).red());
    CHECK(g.phase() == Phase::RedLine);               // the filled hole completed a red row
    // Open sky above never qualifies, even as the last gap in a red row.
    Game h(4, r);
    fill(h, 19, CellKind::Red, 5);
    CHECK(h.evilSpawnCandidates().empty());
    // Normal neighbours never qualify; a full red surround does, with the floor as "below".
    Game k(4, r);
    fill(k, 19, CellKind::Normal, 4);
    k.setCell(4, 18, Cell{CellKind::Red, 0});
    CHECK(k.evilSpawnCandidates().empty());
    k.setCell(3, 19, Cell{CellKind::Red, 0});
    k.setCell(5, 19, Cell{CellKind::Red, 0});
    CHECK(k.evilSpawnCandidates().size() == 1);
    // Mid-board: all four must be red; a normal block below breaks it.
    Game m(4, r);
    m.setCell(3, 10, Cell{CellKind::Red, 0});
    m.setCell(5, 10, Cell{CellKind::Red, 0});
    m.setCell(4, 9, Cell{CellKind::Red, 0});
    m.setCell(4, 11, Cell{CellKind::Normal, 0});
    CHECK(m.evilSpawnCandidates().empty());
    m.setCell(4, 11, Cell{CellKind::Red, 0});
    CHECK(m.evilSpawnCandidates().size() == 1 && m.evilSpawnCandidates()[0].first == 4 && m.evilSpawnCandidates()[0].second == 10);
}

static void testCorruptionBuildsSurround() {
    Rules r = fastRules();
    r.corruptionRate = 50.f;
    r.corruptionTime = 5.f;
    r.corruptionHoleBias = 1.f;    // always take the hole path when one exists
    r.evilSpawnRate = 0.f;
    Game g(21, r);
    for (int row = 12; row < 20; ++row) fill(g, row, CellKind::Normal);    // 8 full rows (danger 0)
    for (int row = 15; row < 20; ++row) fill(g, row, CellKind::Normal);
    g.setCell(4, 17, Cell{});                                               // one hole in an otherwise full row, block above it
    auto targets = g.holeSurroundTargets();
    CHECK(targets.size() == 4);
    CHECK(targets[0] == std::make_pair(4, 16) && targets[1] == std::make_pair(4, 18));   // above, below first
    CHECK(targets[2] == std::make_pair(3, 17) && targets[3] == std::make_pair(5, 17));   // then beside
    // No danger yet (8 rows): raise the stack so corruption runs, then the first event must hit above/below the hole.
    for (int row = 6; row < 12; ++row) fill(g, row, CellKind::Normal, 0);
    CHECK(g.danger() > 0.f);
    g.forcePiece(Shape::I, {});
    g.spawnNow();
    std::pair<int, int> first{-1, -1};
    for (int i = 0; i < 40 && first.first < 0 && g.phase() == Phase::Falling; ++i) {
        g.tick(0.02f);
        for (const Event& e : g.drainEvents()) if (e.type == EventType::CellCorrupting && first.first < 0) first = {e.a, e.b};
    }
    CHECK(first == std::make_pair(4, 16));
    // Once the surround is red, the hole is a spawn candidate.
    for (auto [c, row] : std::vector<std::pair<int, int>>{{4, 16}, {4, 18}, {3, 17}, {5, 17}}) g.setCell(c, row, Cell{CellKind::Red, 0});
    auto holes = g.evilSpawnCandidates();
    CHECK(holes.size() == 1 && holes[0] == std::make_pair(4, 17));
    // A hole under open sky is not worth surrounding.
    Game h(21, r);
    fill(h, 19, CellKind::Normal, 4);
    CHECK(h.holeSurroundTargets().empty());
}

static void testPanicAndPurge() {
    Rules r = fastRules();
    r.corruptionRate = 0.f;      // only panic can drive it
    r.evilSpawnRate = 0.f;
    r.corruptionTime = 5.f;
    Game g(8, r);
    for (int row = 5; row < 20; ++row) fill(g, row, CellKind::Normal, 0);   // 15 rows: not yet within 4 of the top
    CHECK(!g.panic());
    fill(g, 4, CellKind::Normal, 0);                                        // 16 rows: panic
    CHECK(g.panic());
    g.forcePiece(Shape::I, {});
    g.spawnNow();
    int corrupting = 0;
    for (int i = 0; i < 100 && g.phase() == Phase::Falling; ++i) {
        g.tick(0.02f);
        for (const Event& e : g.drainEvents()) if (e.type == EventType::CellCorrupting) ++corrupting;
    }
    CHECK(corrupting >= 4);   // ~2 s at >= 6 events/s with bursts of up to 4 extra
    // The BFG: every red block and every flicker gone, pieces cleansed.
    g.setCell(3, 19, Cell{CellKind::Red, 0});
    runUntil(g, Phase::Falling);   // the corruption loop may have locked the piece; get one in the air
    g.forcePiece(Shape::T, {true, false, true, false});
    CHECK(g.active() && g.phase() == Phase::Falling);
    int removed = g.purgeRed();
    CHECK(removed >= 1);
    int reds = 0, flick = 0;
    for (int row = 0; row < 20; ++row) for (int c = 0; c < 10; ++c) { reds += g.at(c, row).red(); flick += g.at(c, row).corrupt > 0.f; }
    CHECK(reds == 0 && flick == 0);
    CHECK(!g.next().red[0] && !g.active()->red[0]);
    // ...and the whole stack collapses to the floor, then the same piece resumes falling.
    CHECK(g.phase() == Phase::Settling && g.collapsing());
    Shape held = g.active()->shape;
    runUntil(g, Phase::Falling);
    for (int row = 0; row < 19; ++row)
        for (int c = 0; c < 10; ++c)
            if (!g.at(c, row).empty()) CHECK(!g.at(c, row + 1).empty());   // nothing floats
    CHECK(g.active() && g.active()->shape == held);
    // Lines no longer raise the level by default.
    Game h(1, fastRules());
    for (int row = 16; row < 20; ++row) fill(h, row, CellKind::Normal, 0);
    h.forcePiece(Shape::I, {});
    h.spawnNow();
    h.rotateCW();
    for (int i = 0; i < 6; ++i) h.moveLeft();
    h.hardDrop();
    runUntil(h, Phase::Falling);
    CHECK(h.lines() == 4 && h.level() == 1);
}

static void testPrizes() {
    Game g(1, fastRules());
    CHECK(!g.prizes().any());
    for (int r = 16; r < 20; ++r) fill(g, r, CellKind::Normal, 0);
    dropIInColumn0(g);                                   // tetris
    CHECK(g.prizes().bonusHealth == 40.f && g.prizes().shield == 40.f);
    CHECK(g.prizes().invulnChance > 0.24f && g.prizes().invulnChance < 0.26f);
    for (int r = 17; r < 20; ++r) fill(g, r, CellKind::Normal, 0);
    dropIInColumn0(g);                                   // triple, combo x1.5
    CHECK(g.prizes().bonusHealth == 70.f);               // 40 + 20 * 1.5
    CHECK(g.prizes().shield == 70.f);
    CHECK(g.prizes().invulnChance > 0.39f && g.prizes().invulnChance < 0.41f);   // 0.25 + 0.10 * 1.5
    Prizes taken = g.takePrizes();
    CHECK(taken.shield == 70.f && !g.prizes().any());
    Game h(1, fastRules());
    fill(h, 19, CellKind::Normal, 0);
    dropIInColumn0(h);                                   // single: only a little armour
    CHECK(h.prizes().bonusHealth == 0.f && h.prizes().shield == 5.f && h.prizes().invulnChance == 0.f);
}

int main() {
    testPrizes();
    testPanicAndPurge();
    testCorruptionBuildsSurround();
    testEvilSpawn();
    testRotationKicksOnFloor();
    testScoring();
    testCascadeChain();
    testCorruption();
    testCorruptionTargetsReddestRow();
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
