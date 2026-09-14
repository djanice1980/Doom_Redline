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

One scene, two cameras. Cell `(col,row)` (row 0 = top) sits at
`x = col - 4.5`, `y = 19 - row + 0.5`, `z = 0`. The board is a wall standing on
the floor (`y = 0`). The arena is built from unit cubes: floor, ceiling, back
wall at `z = -2.5`, side walls at `x = +-15.5`, front wall at `z = 23.5`. All
of it is one instanced draw.

- Block camera: `(0, 11, 20.5)` looking at `(0, 10.2, 0)`, 60 deg vertical FOV.
- FPS camera: player feet on the floor, eye at 1.6 m, starting at `z = 11`
  facing `-Z`. WASD moves on the XZ plane clamped to the arena.
- Transitions lerp eye, target and FOV with smoothstep over 1.8 s (in) and
  1.4 s (out). During `Alert` (1.4 s) the red row flashes and the ambient light
  pulses red before the fly-in.

## FPS phase (`src/game/fps_mode.h`)

- One `Enemy` per red cell in each all-red row. Feet at the cell's bottom edge,
  0.55 m in front of the wall; they emerge 1.2 m forward over 0.9 s (staggered).
- States: Emerging, Idle (hover/bob, drift on X), Attack (0.6 s wind-up; the
  fireball launches at 0.35 s), Pain (0.25 s), Dying (explodes at 0.45 s, Dead
  at 0.7 s).
- Attack cadence is scaled by the number of demons alive so ten of them don't
  produce a wall of fire. Killing one heals 6.
- Hit test: ray vs vertical cylinder (r = 0.45 m, h = 1.6 m). Shotgun does 35
  (x1.6 inside 3 m); imps have 60 HP.
- Explosions push `Explosion` (sprite + light), `Debris` cubes (bounce on the
  floor), and call `Game::explodeAt` with radius 1.5.
- The phase ends 1.2 s after the last demon dies; the player dying ends the game.

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

Logical names only. From a Doom IWAD: imp `TROO` (A-D walk, E-G attack, H pain,
I-L death), shotgun `SHTG`/`SHTF`, rocket blast `MISL` B-D, imp fireball `BAL1`,
wall `STARTAN3`, floor `FLOOR4_8`, ceiling `CEIL3_5`, font `STCFN*` (converted
to white so HUD tints work), `M_DOOM` title. Sounds: `DSSHOTGN`, `DSBAREXP`,
`DSFIRSHT`, `DSFIRXPL`, `DSPOPAIN`, `DSBGDTH1`, `DSBGSIT1`, `DSPLPAIN`,
`DSPDIEHI`, `DSDMACT`, `DSITEMUP`, `DSPSTOP`, `DSSWTCHN`, `DSSTNMOV`, `DSGETPOW`.
Anything missing falls back to `procedural.cpp`. Block tiles are always ours.
