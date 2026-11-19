// What bullet holes and blood stick to, and what gibs bounce off, once the arena's back
// wall has come down: the gate is open and the crypt's own rock takes over. It used to
// be the arena's box throughout, which hung bullet holes and splats in the air of the
// doorway and dragged every gib in the crypt back to it.
#include "game/fps_mode.h"

#include <cmath>
#include <algorithm>
#include <cstdio>

using namespace rl;
using namespace rl::game;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    core::Game game(7);
    FpsMode fps;
    fps.begin(game, 1);
    fps.setBrutal(true);

    // Before the wall falls, the back wall is whole: a shot down the middle meets it at z = -2.
    glm::vec3 n;
    const glm::vec3 eye(0.f, 1.6f, 10.f), ahead(0.f, 0.f, -1.f);
    CHECK(fps.wallAt(glm::vec3(0.f, 1.f, -2.5f)));
    float t = fps.wallHit(eye, ahead, 60.f, n);
    CHECK(std::fabs(t - 12.f) < 0.01f && n.z > 0.99f);

    // Clear the arena and let the wall come down.
    fps.debugClearArena();
    FpsInput in;
    for (int i = 0; i < 600 && fps.stage() != FpsMode::Stage::Dungeon; ++i) fps.update(1.f / 60.f, in, game);
    CHECK(fps.stage() == FpsMode::Stage::Dungeon);

    // The gate is open, the wall either side of it is not, and the arena is still the arena.
    CHECK(!fps.wallAt(glm::vec3(0.f, 1.f, -2.5f)));
    CHECK(fps.wallAt(glm::vec3(5.f, 1.f, -2.5f)));
    CHECK(fps.wallAt(glm::vec3(0.f, 1.f, 23.5f)));
    CHECK(fps.wallAt(glm::vec3(0.f, -0.1f, 5.f)));
    CHECK(!fps.wallAt(glm::vec3(0.f, 1.f, 5.f)));
    // The crypt's first tile behind the gate is floor (the entrance).
    const Dungeon& dg = fps.dungeon();
    CHECK(dg.floorAt(0.f, -3.5f));
    CHECK(!fps.wallAt(glm::vec3(0.f, 1.f, -3.5f)));

    // A shot through the gate goes on into the crypt.
    t = fps.wallHit(eye, ahead, 60.f, n);
    std::printf("shot through the gate: %.2f m (the old back wall is at 12 m), normal %.0f %.0f %.0f\n", t, n.x, n.y, n.z);
    CHECK(t > 13.f && t < 60.f);
    const glm::vec3 at = eye + ahead * t;
    CHECK(at.z < -3.f);

    // Gibs and blood thrown inside the crypt stay in it: none come back to the doorway.
    int i0 = 0, j0 = 0;
    CHECK(dg.worldToTile(0.f, -3.5f, i0, j0));
    // Walk down the entrance column to the deepest open tile still in line with the gate.
    glm::vec3 spot = dg.tileCentre(i0, j0);
    for (int j = j0 + 1; j < j0 + 8 && dg.floorAt(spot.x, dg.tileCentre(i0, j).z); ++j) spot = dg.tileCentre(i0, j);
    std::printf("gore spot %.2f %.2f\n", spot.x, spot.z);
    fps.spawnBlood(spot + glm::vec3(0.f, 1.f, 0.f), glm::vec3(0.f, 0.3f, -1.f), 40, 5.f, 1);   // chunks
    fps.spawnBlood(spot + glm::vec3(0.f, 1.f, 0.f), glm::vec3(0.f, 0.3f, 1.f), 60, 5.f, 0);    // blood
    const size_t decalsBefore = fps.decals().size();
    for (int f = 0; f < 120; ++f) fps.updateGore(1.f / 60.f);
    int nearDoor = 0, inRock = 0;
    for (const Gore& g : fps.gore()) {
        if (g.kind != 1) continue;
        if (g.pos.z > -2.6f && g.pos.z < -1.9f) ++nearDoor;
        if (fps.wallAt(glm::vec3(g.pos.x, std::max(g.pos.y, 0.01f), g.pos.z))) ++inRock;
    }
    std::printf("chunks %zu: %d at the doorway, %d inside rock\n", fps.gore().size(), nearDoor, inRock);
    CHECK(nearDoor == 0);
    CHECK(inRock == 0);
    int hanging = 0;
    for (size_t k = decalsBefore; k < fps.decals().size(); ++k) {
        const Decal& m = fps.decals()[k];
        // A mark must sit on a surface: a hair off it is open, a hair into it is wall.
        const glm::vec3 off = m.pos + m.normal * 0.01f, inside = m.pos - m.normal * 0.05f;
        if (fps.wallAt(off) || !fps.wallAt(inside)) ++hanging;
        if (std::fabs(m.pos.z - (-2.f)) < 0.05f && std::fabs(m.pos.x) < Dungeon::kGateHalf) ++hanging;
    }
    std::printf("marks %zu, %d not on a surface\n", fps.decals().size() - decalsBefore, hanging);
    CHECK(fps.decals().size() > decalsBefore);
    CHECK(hanging == 0);

    // Back to the board: the wall stands again, and nothing from beyond it stays.
    fps.closeDungeon();
    int behind = 0;
    for (const Decal& m : fps.decals()) if (m.pos.z < -2.f) ++behind;
    for (const Gore& g : fps.gore()) if (g.pos.z < -2.f) ++behind;
    CHECK(behind == 0);
    CHECK(fps.wallAt(glm::vec3(0.f, 1.f, -2.5f)));

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("ok\n");
    return 0;
}
