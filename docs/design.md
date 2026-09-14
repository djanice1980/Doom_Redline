# REDLINE design notes

## Rules engine (`src/core/tetris.h`)

Phases: `Spawning -> Falling -> (lock) -> Clearing -> Settling -> Spawning`,
with `RedLine` and `GameOver` as terminal-until-resumed states.

- **Red minos.** `Rules::redChanceBase + redChancePerLevel * (level-1)` per mino,
  capped by `redChanceMax` and `maxRedPerPiece`.
- **Lock.** Minos are written as `Normal` or `Red` cells. Any mino still above
  row 0 at lock time ends the game.
- **Clear check.** Rows that are full *and not entirely red* flash for
  `clearAnimTime`, then `finishClear()` removes their normal cells with a
  per-column shift: for each cleared row and column, if the cell is red nothing
  moves in that column; otherwise the column above it drops by one. Cleared rows
  are processed top to bottom so the row indices stay valid.
- **Settle.** While `redCellsSettle` is on, every red cell with an empty cell
  below moves down one row per `settleStepTime`, bottom-up so stacks move
  together. When nothing moves, the clear check runs again (a settling red cell
  can complete a row), then the red-line check, then the next piece spawns.
- **Red line.** Any row where all ten cells are red sets `Phase::RedLine` and
  emits `EventType::RedLine`. The engine then idles until the FPS phase calls
  `explodeAt()` for each kill and `resumeAfterRedLine()` at the end.
- **explodeAt(col,row,r).** Clears the target cell and every `Normal` cell within
  Euclidean distance `r` (cells). Red cells are untouched.

Why per-column collapse: it is identical to classic Tetris when no red cell is
involved, and it gives red cells the "stay and sink" behaviour without needing
cascade gravity for everything else. Red cells reach the bottom row over time
because each row clear beneath them drops them one further, and the bottom row
therefore fills with red until it triggers. Playtests with `--seed` showed the
first red line around level 2-3 with the default chances.

## World

One scene, two board orientations. Cell `(col,row)` (row 0 = top) has board-plane
coordinates `x = col - 4.5`, `h = 19 - row + 0.5`. `App::boardPosH(x, h)` maps
that onto the world through a hinge along the board's bottom edge:
`(x, h cos t + 0.5 sin t, h sin t)` with `t = tilt * 90 deg`. Tilt 0 is the
standing wall of block mode; tilt 1 lays the board flat on the arena floor with
row 19 at `z = 0.5` and row 0 at `z = 19.5`. The cubes rotate rigidly with it
(per-instance rotation in `CubeInstance::rot`). The frame (two rails and a
lintel) is hinged with the board so it becomes the side walls and far wall of
the play space when flat.

The arena is built from unit cubes: floor, ceiling, back wall at `z = -2.5`,
side walls at `x = +-15.5`, front wall at `z = 23.5`. All of it is one
instanced draw.

- Block camera: `(0, 11, 20.5)` looking at `(0, 10.2, 0)`, 60 deg vertical FOV.
- FPS camera: player feet on the flat board, eye at 1.25 m (blocks are 1 m, so
  they are chest-high cover), starting in the highest empty cell nearest the
  centre column, facing -Z towards the stack. 80 deg FOV.
- Transitions: `Alert` (1.4 s, red row flashes, ambient pulses red), `FlyIn`
  (2.2 s: tilt 0 -> 1 with smoothstep while the camera arcs from the overview to
  the player's eye, rising 6 m mid-way), `FlyOut` (1.6 s, the reverse).

## FPS phase (`src/game/fps_mode.h`)

- `FpsMode::begin` runs `core::findRedRegions` (4-connected flood fill) and
  turns **every** region into one `Enemy`. Tier by size: 1 zombie, 2-3 imp,
  4-6 demon, 7-11 cacodemon, 12+ baron; a level-scaled roll can bump a tier up
  (8 % per level, max 50 %) or down. The region's red cells are removed from
  the grid (the monster is standing in the pocket) and remembered on the enemy.
- Stats table in `fps_mode.cpp` (`kStats`): hp, hit cylinder, attack kind
  (hitscan / projectile type / melee), speed, flies, cadence, damage, score.
  `hp *= clamp(0.45 + 0.15 (level-1), 0.45, 2.5)`; cadence `*= max(0.5,
  1 - 0.06 (level-1))` and also scales with the number alive.
- Movement: circle-vs-grid slide for the player and walking monsters
  (`moveWithCollision`); cacodemons fly over blocks. Line of sight and the
  shotgun ray use `rayBlockDistance` (0.05 m march against `solidAt`, which
  treats cells below 1 m and the frame as solid).
- Enemies rise out of the board when the phase starts (their red cubes sink
  away in sync). Zombies hitscan (65 % hit chance with LOS), imps/cacos/barons
  launch BAL1/BAL2/BAL7 fireballs at the player's centre (so chest-high blocks
  stop them), demons charge and bite within 1.3 m.
- Death: `explodeEnemy` calls `Game::explodeAt(cell, 1.5)` for every cell of the
  cluster, spawns debris per destroyed block, a blast sprite and light, and
  hurts the player within `2 + 0.5 tier` m. Each kill heals 5.
- The phase ends 1.2 s after the last monster dies; `Game::resumeAfterRedLine`
  then scores `1000 * level` and raises the level. The player dying ends the
  game (`YOU DIED` screen with the board still flat).

## Renderer (`src/render/renderer.h`)

- `CubeInstance`: centre + uniform scale, tint, emissive (rgb + strength),
  atlas rect, params (roughness, metallic, anim phase, flags; bit 0 = pulse).
- `QuadInstance`: mode 0 = world billboard rotating about Y only (Doom style),
  mode 1 = screen space in pixels, top-left origin. Anchor is a 0..1 point in
  the quad; sprites use their WAD offsets so the origin is at the feet.
- One descriptor set: frame UBO (matrices, camera, sun, ambient/fog, 16 point
  lights) + atlas sampler (nearest mag, linear min).
- Depth test/write toggled with core 1.3 dynamic state so HUD quads share the
  billboard pipeline.
- Negative-height viewport keeps GL conventions (+Y up, CCW front faces).
- `screenshot()` copies the last presented swapchain image to a PNG (stored
  deflate; no zlib dependency).

## Assets (`src/game/assets.cpp`)

Logical names only. From a Doom IWAD: monsters `POSS` (zombieman), `TROO`
(imp), `SARG` (demon), `HEAD` (cacodemon), `BOSS` (baron) with walk / attack /
pain / death frame runs per the Doom state tables, shotgun `SHTG`/`SHTF`, rocket
blast `MISL` B-D, fireballs `BAL1`/`BAL2`/`BAL7` (A-B flight, C-E impact),
wall `STARTAN3`, floor `FLOOR4_8`, ceiling `CEIL3_5`, font `STCFN*` (converted
to white so HUD tints work), `M_DOOM` title. Sounds: `DSSHOTGN`, `DSBAREXP`,
`DSFIRSHT`, `DSFIRXPL`, `DSPOPAIN`, `DSBGDTH1`, `DSBGSIT1`, `DSPLPAIN`,
`DSPDIEHI`, `DSDMACT`, `DSITEMUP`, `DSPSTOP`, `DSSWTCHN`, `DSSTNMOV`, `DSGETPOW`.
Anything missing falls back to `procedural.cpp`. Block tiles are always ours.
