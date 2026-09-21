# REDLINE design notes

## Rules engine (`src/core/tetris.h`)

Phases: `Spawning -> Falling -> (lock) -> Clearing -> Settling -> Spawning`,
with `RedLine` and `GameOver` as terminal-until-resumed states.

- **Red minos.** `Rules::redChanceBase + redChancePerLevel * (level-1)` per mino,
  capped by `redChanceMax` and `maxRedPerPiece`.
- **Rotation.** SRS kick tables (`kKicksJLSTZ`, `kKicksI`, y negated because
  rows grow downward), tried in order after the plain rotation. The upward
  kicks are what let a piece rotate while resting on the floor or the stack.
- **Lock.** Minos are written as `Normal` or `Red` cells. Any mino still above
  row 0 at lock time ends the game.
- **Clear check.** Rows that are full *and not entirely red* flash for
  `clearAnimTime`, then `finishClear()` removes their normal cells with a
  per-column shift: for each cleared row and column, if the cell is red nothing
  moves in that column; otherwise the column above it drops by one. Cleared rows
  are processed top to bottom so the row indices stay valid.
- **Settle.** Red minos stay with the piece they landed in; they only move
  when a line clear beneath them (per-column collapse) or the post-fight
  collapse (`collapseAll_`, every cell falls one row per `settleStepTime`)
  drops them. `Rules::redCellsSettle` (off by default) is the opt-in "sand"
  variant where loose red cells fall after every lock. Every settle step also
  applies *sticky gravity*: the grid is flood-filled into 4-connected groups
  (red cells included) and any group with no cell on the bottom row and
  nothing beneath it from another group falls one row. This is what stops a
  clump from hanging in the air after a per-column clear left it unsupported
  (`testFloatingGroupFalls`). When nothing moves, the clear check runs again
  (a fall can complete a row, scored as a chain), then the red-line check,
  then the next piece spawns.
- **Corruption.** `danger()` = clamp((stackRows - corruptionStartRows) /
  (20 - corruptionStartRows)). While a piece is falling, corruption events
  fire at `corruptionRate * danger^2 * (1 + 0.1 (level-1))` per second. Each
  one turns `1 .. 1 + danger * corruptionBurst` cells. With probability
  `corruptionHoleBias` they come from `holeSurroundTargets()` (for each hole
  of a row with <= `corruptionHoleMaxEmpties` empties and a block above, best
  hole first: above, below, left, right), otherwise from
  `corruptionTargetRow()` (most red or turning cells, then most filled, then
  lowest, skipping rows with nothing left to turn);
  `Cell::corrupt` counts down `corruptionTime` seconds (the
  renderer flickers it faster as it approaches zero), then the cell becomes
  Red and `CellTurnedRed` fires. A red row formed this way triggers the red
  line immediately; `spawn()` keeps the interrupted active piece so it resumes
  after the fight.
- **Prizes.** `finishClear` adds `prizeHealth/Shield/Invuln[n] * mult` to
  `prizes_` (caps 100 / 200 / 0.9). `App` calls `takePrizes()` at FlyIn and
  passes them to `FpsMode::begin`: health = 100 + bonus (cap 200), shield,
  and one roll against `invulnChance` for `invulnSeconds` of immunity. A
  failed roll is announced with the percentage, so a 90 % miss is visibly bad
  luck rather than a bug.
  `hurtPlayer` ignores damage while invulnerable and lets armour absorb half
  of a hit first. Health pickups never raise health above 100.
- **Panic.** `panic()` is true within `panicRows` (4) of the top; corruption
  and evil-spawn rates are then at least `panicRate` (6/s) and corruption
  bursts get `panicBurst` (3) extra cells. This is the intended escape from a
  stack about to overflow.
- **Levels.** `linesPerLevel` defaults to 0: only `resumeAfterRedLine` raises
  the level.
- **Secret: BFG9000.** Holding B, F and G together for two seconds while
  stacking, or both sticks pressed in on a pad, which has no letters to hold
  (`App::bfgHoldT_`, once per playthrough via `bfgUsed_`; a green
  glow builds while charging and resets if the chord is released) calls
  `Game::purgeRed()`: every red
  block is erased, all flickering and spawning stops, the red minos of the
  falling and next pieces become normal, and the whole stack collapses to the
  floor (Settling with `collapseAll_`, the post-fight collapse; the falling
  piece waits and resumes). Green flash, DSBFG, "BFG SPENT" in
  the side panel afterwards. Not mentioned in the README, the CONTROLS page or
  any in-game hint. `--keys &@<frame>` presses both sticks in a scripted run.
- **Evil spawn.** `evilSpawnCandidates()` lists empty cells whose four
  neighbours are all red or corrupting: the floor and side walls count as red,
  the cell above must be a real block (open sky disqualifies), and at least
  two neighbours must be real red blocks. So on the bottom row it is left,
  right and top; mid-board it is a full surround. While a piece falls, one random candidate starts
  spawning at `evilSpawnRate * (0.3 + 0.7 danger)` per second; `Cell::corrupt`
  on an *Empty* cell marks it (`spawning()`), the renderer grows a flickering
  red cube there, and after `evilSpawnTime` it becomes Red (waiting if the
  active piece is sitting in it). A completed red row triggers the fight.
- **Scoring.** `lineScore[n] * level * (1 + comboStep (combo-1)) * (1 +
  chainStep * chain)`. `combo_` counts consecutive clearing pieces (reset by a
  piece that clears nothing); `chain_` counts clears produced by a collapse
  (`clearFromSettle_`) after the last lock. Kills are scored in
  `FpsMode::explodeEnemy`: `scoreValue * (1 + 0.1 (level-1)) + 25 * destroyed`.
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
  the player's eye, rising 6 m mid-way), `Countdown` (3 s: `FpsInput::warmup`,
  monsters emerge and then hold, mouse look only, big 3-2-1 with a tick per
  second and a FIGHT flash), `FlyOut` (1.6 s, the reverse of the fly-in).

## FPS phase (`src/game/fps_mode.h`)

- `FpsMode::begin` runs `core::findRedRegions` (4-connected flood fill) and
  turns **every** region into one `Enemy`. Tier by size: 1 zombie, 2-3 imp,
  4-6 demon, 7-11 cacodemon, 12-17 baron, 18-24 cyberdemon (rockets with 2 m
  splash that also break blocks), 25+ spider mastermind (chaingun hitscan);
  a level-scaled roll can bump a tier up (8 % per level, max 50 %) or down.
  `maxTierForLevel` caps spawns (2/3/4/5/6 at levels 1, 2, 3-4, 5-6, 7+);
  a region larger than `maxRegionSizeForTier(cap)` is split with
  `splitRegion` (farthest-point seeds, nearest-seed assignment) into enough
  compact groups, and regions of 7+ (and 15+) cells get a 50 % split anyway.
  Absorbing is capped one class above the level cap. The region's red cells are removed from
  the grid (the monster is standing in the pocket) and remembered on the enemy.
- Stats table in `fps_mode.cpp` (`kStats`): hp, hit cylinder, attack kind
  (hitscan / projectile type / melee), speed, flies, cadence, damage, score.
  `hp *= clamp(0.45 + 0.15 (level-1), 0.45, 2.5)`; cadence `*= max(0.5,
  1 - 0.06 (level-1))` and also scales with the number alive.
- Movement: circle-vs-grid slide for the player and walking monsters
  (`moveWithCollision`); cacodemons fly over blocks. Line of sight and the
  shotgun ray use `rayBlockDistance` (0.05 m march against `solidAt`, which
  treats cells below 1 m and the frame as solid).
- Facing (`Enemy::yaw`, `moveDir`, `moveCount`, `blocked`): a monster wakes
  facing the player. While chasing it picks one of eight compass directions
  closest to the player (`P_NewChaseDir`), holds it 0.1-0.45 s, and turns
  toward it at 360 deg/s; a move that made under 30 % of its expected
  progress marks it blocked, and the next pick sidesteps 45 or 90 degrees.
  Standing in range it tracks the player at 180 deg/s (slow enough to flank);
  entering Attack snaps the heading to the player (`A_FaceTarget`) and keeps
  tracking through the attack frames; Pain, Dying and Dead leave it alone.
  `addFpsActors` passes the heading to `actor()`, which rotates voxel models
  by it and ignores it for sprites (camera-facing billboards).
- Arch-vile (`AttackKind::Vile`, kind 12, a baron-tier variant): entering
  Attack pushes `EnemyAttack` at once (the DSVILATK scream); after
  `kVileFireDelay` (0.3 s) `Enemy::fireT/firePos` place the flame on the
  player (`VileFire` a=0 plays DSFLAMST, a=1 at one second plays DSFLAME) and
  it follows the player while `lineOfSight` holds; at `kVileWindup` (2.4 s)
  with sight: `hurtPlayer(20)`, `enemyBlast` at the flame (1.8 m, 70, breaks
  blocks), `jumpVy_` = 5.5 m/s lifts `eye()` (the arch-vile jump), `VileBlast`
  plays the barrel explosion; without sight nothing happens. The attack frames'
  fps is set so frame O lands on the clasp. `EnemyStats::painChance` (0.12 for
  the vile, 1 for everyone else) gates the Pain state, which cancels the
  attack. `REDLINE_KIND=<kind>` forces every spawn to that kind for testing;
  `REDLINE_LOG_VILE=1` logs each clasp.
- Enemies rise out of the board when the phase starts (their red cubes sink
  away in sync). Zombies hitscan (65 % hit chance with LOS), imps/cacos/barons
  launch BAL1/BAL2/BAL7 fireballs at the player's centre (so chest-high blocks
  stop them), demons charge and bite within 1.3 m.
- Death: `explodeEnemy` calls `Game::explodeAt(cell, 1.5)` for every cell of the
  cluster, spawns debris per destroyed block, a blast sprite and light, and
  hurts the player within `2 + 0.5 tier` m. Each kill heals 5.
- **Cover erosion** (`breakBlock`): every monster has a `breakTimer` seeded
  from its tier's `breakInterval` (11 / 7.5 / 5 / 3.5 / 2.5 s, scaled by the
  level cadence). When it fires, the block that blocks its line of sight to
  the player is destroyed (barons take the 3x3 around it); with a clear line
  it chews a random block within two cells half the time.
- **Absorb** (`tryAbsorb`): `absorbTimer` starts at `max(8, period - (level-1))`
  seconds (period 20 by default, `--absorb` overrides). On expiry the monster
  grows one tier (if below the level's cap + 1, and only one boss at a time):
  normal blocks within 2.5 cells are cleared, fly into it as homing debris and
  join its `cells` (so its death blast covers them), and hp/radius/height reset
  to the new class, at 80 % health with no blocks nearby and up to 120 % with
  many. Growth no longer needs cover or a coin flip, so a lone survivor always
  changes. The sprite pulses red for the last 4 s and
  flashes white while `growT` decays; the HUD shows a warning.
- **Ammo on wound** (`damageEnemy`): a non-lethal hit has a 12 % chance,
  gated by a 2.5 s per-monster cooldown, to spawn ammo for the current weapon
  (random ammo when holding the shotgun).
- **Weapons** (`kWeapons` in `fps_mode.cpp`): shotgun (infinite, 40 dmg,
  0.75 s), chaingun (12 dmg hitscan, 0.1 s, 60 rounds per pickup, 200 max),
  rocket launcher (projectile 22 m/s, 110 dmg, 2.2 m splash that also clears
  normal blocks within a cell of the impact and hurts the player, 4 per
  pickup), plasma rifle (bolts 28 m/s, 22 dmg, 0.12 s, 40 per pickup). Player
  projectiles test a point against each monster's cylinder every frame. Weapon
  slots persist across fights; an empty weapon falls back to the shotgun.
- **Loot** (`dropLoot`): each death drops `1 + tier/2` items (+1 with 25 %):
  32 % health (medikit for tier >= 2), then ammo (favouring owned weapons;
  the band is wider once the player owns an extra weapon), then a weapon whose
  class follows the tier, 12 % nothing. Items pop out with a velocity, land on
  the floor, slide back to the monster's pocket if they land inside a block,
  and are collected within 0.7 m (health items only when not at 100).
- The phase ends 1.2 s after the last monster dies; `Game::resumeAfterRedLine`
  then scores `1000 * level`, raises the level and sets `collapseAll_`, so the
  settle phase drops every remaining cell (not only red ones) one row per step
  until the board is compact again. The player dying ends the game
  (`YOU DIED` screen with the board still flat).

## Dungeon (`src/game/dungeon.h`, the `Stage` machine in `fps_mode.h`)

- `FpsMode` has three stages: `Arena` (as before), `GateFalling` (2.4 s: the
  4 x 4 gate section of the back wall tips over, drawn by `App::addFpsActors`
  as tumbling cubes while `buildEnvironment` leaves the hole) and `Dungeon`.
  With `dungeonEnabled_` the arena's "all dead for 1.2 s" no longer finishes
  the fight but starts the fall; `openDungeon` generates the layout, blasts
  columns 3-6 of the bottom seven rows (the passage), spawns the population
  and pushes `DungeonOpen`. The fight finishes 2.5 s after the boss dies.
- `Dungeon::generate(seed, level)`: a tile grid (`Rock`/`Floor`/`Wall`, wall =
  rock bordering floor in the 8-neighbourhood) of `min(64, 30 + 4L)` x
  `min(80, 34 + 5L)` tiles. Tile (0, 0) is at x = -w/2, z = -3 and the grid
  runs towards -z. An entrance room behind the gate (centred on x = 0), the
  boss's hall at the far end (15-21 tiles square), then `min(8, 2 + (L+1)/2)`
  rooms of 5-11 tiles placed by rejection sampling with a tile of rock between
  any two. Corridors: each unlinked room joins the nearest linked room by an
  L (2 wide; 3 into the hall), the hall last, plus one or two random loops;
  the entrance corridor is 4 wide to match the gate. `ceilingAt` is 4 cubes
  over rooms and corridors, 6 in the hall; a wall tile is as tall as the
  tallest floor beside it. Torches every three tiles round each room (four in
  the hall) and every ninth corridor tile.
- World model: `solidAt` treats z <= -3 as the dungeon's (floor open, anything
  else solid up to `ceilingAt`), and the gate passage (|x| < 2, -3 <= z < 0)
  as open across the board's end rail; `outOfWorld` widens the projectile
  bounds to the grid. Movement, line of sight and hitscans need no other
  changes (a shot into a wall throws stone chips).
- Population (`populateDungeon`): `count = clamp(1 + L + blocks/14, 3, 36)`
  demons spread round-robin over the ordinary rooms (`spawnSpots`), tiers
  drawn as `min(cap, floor(u^1.7 * (cap+1)))` with `cap = min(4,
  maxTierForLevel)`; flyers become hell knights or demons. The boss (tier 4 at
  L <= 2, 5 at L <= 5, else 6) stands in the hall's centre with `hp * (1 +
  blocks/80 + 0.1 L)`, plus `2 + L/3` guards away from the middle. One item per
  room (medikit, bullets, rockets, cells, stim in turn), a second medikit in
  every other room, one in the hall's corner.
- Sleepers: dungeon monsters start `dormant`; every 0.15-0.3 s one checks
  `lineOfSight` to the player within 45 m or a distance under 3.5 m, and any
  damage wakes it. Waking pushes `Wake` (its sight sound) and, for the boss,
  `BossSeen` (announcement + the HUD bar). Sleepers do not count towards the
  crowd factor that slows attacks. `boardBlocks_` (non-empty cells when the
  fight began) is what the population and the boss scale by.
- The App rebuilds the static environment (and the torch props) whenever the
  stage or the presence of a dungeon changes; outside fight modes any dungeon
  is closed. In the crypt the sun's shadow box follows the eye (the ceiling
  shades it), ambient drops to 0.10 and the fog thickens.
- **Rooms are shaped like the pieces that built them.** `generate` takes the
  board's own piece histogram (`core::Game::pieceCounts`, counted as each piece
  locks) and both seeds from it and draws each ordinary room's footprint from
  one of the seven shapes, weighted by how often that piece was played with a
  floor of one seventh so no shape takes over. Each mino becomes a 2-4 tile
  block, so an S piece gives a staggered hall and an I piece a long one; the
  entrance and the boss hall stay rectangles, where a predictable floor matters.
  Corridors join from a real floor tile of each room (`nearestFloor`), since the
  bounding-box centre of an L or a T can sit in its notch.
- **Pillars**: blocks of rock left standing inside a room, always with a clear
  ring so they cannot cut one in two. The hall always gets 2x2 ones for cover,
  with the middle three tiles kept clear for the boss to stand in; ordinary
  rooms take single tiles half the time.
- **The hall is sealed.** `sealBossRoom` turns every floor tile just outside the
  hall that leads into it into a `Door`, solid until the player finds the skull
  key hidden in the room furthest from the hall. With the key in hand the door
  stops blocking (so a player, or the test bot, can walk at it) and opens on
  approach with an announcement. A crypt too small to hide a key opens the hall
  from the start, so it is always finishable.
- **What the crypt leaves on the floor comes from a battle simulation**
  (`simulateFight`, after the one in Obsidian's level generator). The roster is
  planned before anything spawns; each monster is then fought one at a time,
  strongest first, with the best weapon still holding ammunition. Time to kill is
  its health over the weapon's damage rate; the cost is its own damage rate over
  that time, discounted for cover and movement (0.34, or 0.22 for melee, which
  has to reach you). The result is the health and rounds the fight is expected to
  need. Whatever the player cannot already pay for is placed as medikits, stims
  and ammunition spread over the rooms, plus any weapon they have not got yet.
  If the cost still exceeds what they could carry and pick up, the weakest
  monsters are dropped until it does, down to a floor of eight.
- **Six texture sets dress it** (`Assets::cryptWall/Floor/Ceil`, keys `cwall0`..):
  four room themes (grey crypt, marble tomb, iron works, flesh), one for the boss
  hall and one for the corridors, each a wall texture plus a floor and ceiling
  flat, every one falling back through several Doom lump names and finally to our
  procedural tile. Each room draws a theme at random; `assignThemes` writes a
  theme per tile, walls taking the theme of the floor they face. `buildEnvironment`
  now buckets its cubes by texture and emits one draw range per bucket, so each
  keeps its own normal and roughness maps. Torch colour follows the theme: blue in
  the marble and iron rooms, fire elsewhere, and the hall's reach further and sit
  higher because it is six cubes tall.
- **Scenery** (`Dungeon::placeDecor`, `DungeonDecor`): columns, candles,
  stalagmites, impaled bodies, hanging corpses and skull piles, placed on floor
  tiles that touch a wall and spaced from each other and from the torches. Rooms
  get two to four, the flesh theme gets the grislier set, the boss hall gets ten,
  and corridors get a candle every so often. Candles carry a small warm light.
- Testing: `--scenario dungeon` (arena pre-cleared), `REDLINE_LOG_DUNGEON=1`
  (ASCII layout with `m`onsters, the `B`oss, `+` items, the `K`ey and the `D`oor,
  plus the budget and any trim), and the bot's `navNext` (a breadth-first search
  over 1 m tiles of everything walkable) which lets `--bot` walk the crypt,
  fetch the key and reach the boss.

## Test bot (`src/core/tetris_bot.h`)

`planPlacement` tries every rotation and every column the active piece can
slide to at its current height, drops each, and scores the result with
`0.76 lines - 0.51 height - 0.36 holes - 0.18 bumpiness - 0.10 wells +
2 redRow + 6 redLine`, resolving the landing the way `Game` does (a full
row loses its normal cells and each such column shifts down, reds stay,
then `settle`'s sticky gravity). A gap walled in by red in a row with at
most two gaps is "fight fuel", not a hole: corruption reddens the block on
top of it and evil fills it. `TetrisBot::step` issues one input per call
(rotate first, then slide, then hard-drop), replans when the piece stops
responding and drops it if the new plan is no better. The App calls it
every 0.12 s in `Mode::Blocks` (a person's pace, which the corruption and
evil-spawn timers assume); at that cadence twelve seeds all reach eight
fights. `tests/tetris_test.cpp` runs it for 400 pieces on a plain board and
through two red lines with the shipped rules.

## Renderer (`src/render/renderer.h`)

- `CubeInstance`: centre + uniform scale, tint, emissive (rgb + strength),
  atlas rect, params (roughness, metallic, anim phase, flags; bit 0 = pulse).
- `QuadInstance`: mode 0 = world billboard rotating about Y only (Doom style),
  mode 1 = screen space in pixels, top-left origin. Anchor is a 0..1 point in
  the quad; sprites use their WAD offsets so the origin is at the feet.
- One descriptor set: frame UBO (matrices, camera, sun, ambient/fog, 32 point
  lights, sun light matrix and shadow params) + atlas sampler (nearest mag,
  linear min) + the shadow map (comparison sampler, white border).
- Sun shadows: before the main pass, cubes, world billboards (alpha tested) and
  meshes are drawn depth-only into a 2048x2048 map from an orthographic box
  (`FrameParams::shadowCenter/shadowRadius`, looking along `-sunDir`). The
  fragment shaders' `shade()` multiplies the sun term by a 3x3 PCF lookup;
  `shadowStrength` scales it (0 = off). The arena has no ceiling so the sun can
  reach the floor.
- Ray-traced shadows (`VK_KHR_ray_query`, when `VkContext::rayQuerySupported()`):
  the device is created with the acceleration-structure, ray-query and
  deferred-host-operations extensions plus buffer device addresses, and the
  five KHR entry points are fetched with `vkGetDeviceProcAddr`. One BLAS for
  the unit cube (built from the cube VB/IB, which then carry the AS-input
  usage) and one per voxel mesh (a float32 copy of the positions, built in
  `createMesh`). `buildTlas` fills a host-visible instance buffer per frame in
  flight from the cube list (translate * Rx * Ry * scale, matching
  `cube.vert`) and the mesh list (their model matrices), builds the TLAS with
  PREFER_FAST_BUILD into a per-frame buffer and scratch (grown on demand
  after `waitIdle`), and a memory barrier hands it to the vertex and fragment
  stages. The descriptor set gains binding 3 (acceleration structure) only
  when supported, rewritten each frame; `cube_rt.frag`, `mesh_rt.frag` and
  `quad_rt.vert` are `#version 460` wrappers that define `RT_SHADOWS` around
  the shared bodies, and `common.glsl`'s `shade()` then calls `rtVisibility`
  (opaque, terminate-on-first-hit, 0.04 normal offset) for the sun when
  `u.counts.y >= 1` and for point lights with `att > 0.015` when
  `u.counts.y == 2`; point-light rays stop 0.6 short of the light so a red
  block does not shadow its own glow. The shadow map pass still runs every
  frame so the map descriptor is always valid. Cost on the RTX 5070 Ti at
  1600x900: about 6 ms per frame for all lights, under 1 ms for the sun only.
- `MeshInstance`: a static mesh id (`createMesh` uploads 12-byte vertices:
  int16 corner xyz + face-normal index, RGBA8 colour) with a model matrix, tint
  and emissive passed as 96 bytes of push constants. Used for voxel models;
  the mesh fragment shader bends the axis-aligned normals 0.6 towards the
  viewer so voxel monsters read as brightly as the camera-facing sprites.
- Depth test/write toggled with core 1.3 dynamic state so HUD quads share the
  billboard pipeline.
- Negative-height viewport keeps GL conventions (+Y up, CCW front faces).
- `screenshot()` copies the last presented swapchain image to a PNG (stored
  deflate; no zlib dependency).
- Post chain (see README "Post-processing" and "Ray tracing" for the player's
  view): the world renders to an RGBA16F target, 4x MSAA when enabled, then
  bright-pass, three Gaussian levels, a half-resolution volumetric march
  (`volume.frag`: sun through the shadow map plus point-light haze), and
  `composite.frag` (soft-knee tone map, warm grade). The HUD is drawn LDR
  after the composite; screen quads with `params.w` bit 1 set (weapon, muzzle
  flash) go into the HDR pass instead. Ray-query mode 3 adds floor and block
  reflections (hits resolved through the TLAS custom index into a cube SSBO
  and a mesh-address table) and a three-ray ambient-occlusion term.
- Screen quads: `mode 1` HUD, `mode 2/3` floor/wall decals, `params.w` bit 2
  = soft alpha (no cutoff, no shadow). The atlas has three mip levels with
  sprite edges dilated so the sharpened bilinear sampler (`atlas_frag.glsl`)
  never bleeds transparent texels into a sprite.

## Voxel models (`src/game/kvx.cpp`, `src/game/voxels.cpp`)

`loadKvx` reads mip 0 of a Build-engine `.kvx` (header, per-column slab
offsets relative to the xoffset table, trailing 6-bit palette) into a dense
grid, flips z (KVX z points down) into a Y-up frame with the feet at y = 0,
and greedy-meshes the exposed faces per axis and colour. Cheello's models are
mostly solid inside, so face culling matters more than the slab format's own
cull bits (ignored). `VoxelModels` finds the pack, parses `VOXELDEF.txt`
(`sprite = "file" { AngleOffset = .. Scale = .. }`, comments stripped), maps
atlas keys `TROO_A` to `trooa`, and meshes each frame on first use, caching
misses. `App::actor()` draws a frame as a mesh when voxels are on and the pack
has it, otherwise as the usual billboard. Model matrix: translate(feet) *
rotateY(yaw + AngleOffset - 90) * scale(metresPerPixel * Scale) *
translate(-pivot). Yaw is the world heading (sin, 0, cos): monsters face the
player, brawlers face their opponent, projectiles follow their velocity,
pickups spin, decor faces +Z. Voxel units equal sprite pixels, so the per-class
`metresPerPixel` carries over unchanged.

## Ambient brawlers (`src/game/ambient.cpp`)

Decorative infighting while the board stands: three monsters per side in the
strip x in [7.5, 13.5], z in [-1.2, 2.6] (the only floor the overview camera
sees; its bottom edge meets the floor at z = 3). Tier window per level:
lo = level/3 (max 3), hi = 1 + level/2 (max 5, one cyberdemon at a time,
never spiders); health = class hp x the fight's level curve x 0.7; double
damage so brawls end quickly; hitscan / projectile / melee attacks reuse
`enemyStats()`. Melee classes fight their own side; ranged ones also target
the other side, stepping in front of the board face (z >= 1.4) before
shooting across it, and never walk in front of the stack. Dead ones respawn
after 6-12 s. Cleared on FlyIn, re-seeded at the new level when Blocks
resumes after a fight. Silent; they never touch the player, the board or the
score. `--scenario blocks --level N` seeds them at that level for testing.

## Assets (`src/game/assets.cpp`)

WAD discovery (`Assets::findWad`) takes the pref and executable folders so the
player's saved `wad.txt` and an installer's `redline.cfg` are honoured; the
order and the first-run **REDLINE NEEDS DOOM** screen are described in
`docs/packaging.md`. `App::reloadAssets` rebuilds `Assets` from a new WAD and
swaps the atlas, environment, props, sound bank and music (`initMusic`)
without touching game state, which only refers to art by logical name. The
file browser is SDL3's `SDL_ShowOpenFileDialog`; its callback stores the
result under a mutex and `update()` applies it on the main thread.

Logical names only. From a Doom IWAD: monsters `POSS` (zombieman), `TROO`
(imp), `SARG` (demon), `HEAD` (cacodemon), `BOSS` (baron), `CYBR` (cyberdemon),
`SPID` (spider mastermind) with walk / attack /
pain / death frame runs per the Doom state tables, weapons `SHTG`/`SHTF`,
`CHGG`/`CHGF`, `MISG`/`MISF`, `PLSG`/`PLSF`, pickups `STIM`, `MEDI`, `CLIP`,
`ROCK`, `CELL`, `MGUN`, `LAUN`, `PLAS`, rocket `MISL` (A flight, B-D blast),
plasma `PLSS`/`PLSE`, fireballs `BAL1`/`BAL2`/`BAL7` (A-B flight, C-E impact),
wall `STARTAN3`, floor `FLOOR4_8`, ceiling `CEIL3_5`, font `STCFN*` (converted
to white so HUD tints work, and stored a second time at 4x as
`Assets::fontBig`, which `App::text` uses for anything drawn at 3x or more:
the Real-ESRGAN glyph from `assets/font-hd` when `manifest.txt` matches the
WAD glyph's size and alpha-mask CRC, else the xBR kernel in
`core/pixel_scale.cpp`; `fontHdGlyphs` counts the matches), `M_DOOM` title. Sounds: `DSSHOTGN`, `DSBAREXP`,
`DSFIRSHT`, `DSFIRXPL`, `DSPOPAIN`, `DSBGDTH1`, `DSBGSIT1`, `DSPLPAIN`,
`DSPDIEHI`, `DSDMACT`, `DSITEMUP`, `DSPSTOP`, `DSSWTCHN`, `DSSTNMOV`, `DSGETPOW`.
Anything missing falls back to `procedural.cpp`. Block tiles are always ours.

Brutal mode (`App::brutalActive()` = the BRUTAL option and Doom art on)
draws gore particles (`Gore`: blood, chunks, casings, colour per monster kind),
decals (pools, splats, bullet holes), gib deaths, per-weapon death animations
(`EnemyArt::brDeath`, a `SPRITE:FRAMES:tics` table in `assets.cpp`), Brutal
weapon art and sounds. The art comes from the optional community pack
(`Assets::findBrutalPack`, `assets/brutal/`, credits inside); without it the
WAD's own blood, pool and puff sprites are used. Doom 2 monsters
(chaingunner, hell knight, revenant, mancubus, arachnotron) are tier
*variants* (`Enemy::kind` 7-11) when a companion `doom2.wad` sits next to the
chosen WAD; `FpsMode` is rebuilt per game, so `setBrutal` and
`setKindAvailable` are re-applied after `fps_ = FpsMode()`.

## Music (`src/audio/`)

- `mus.cpp` parses a MUS lump into `MusEvent`s (140 ticks/s; MUS channel 15
  becomes MIDI 9; note-on velocity is "sticky" per channel; MUS controllers
  0-9 map to program change and CC 0/1/7/10/11/91/93/64/67; system events
  10/11 -> all notes off, 14 -> reset controllers).
- `genmidi.cpp` parses `#OPL_II#` + 175 x 36 bytes (+ names): 128 melodic
  programs and 47 percussion voices (notes 35-81), each with two 2-op voices,
  flags (fixed pitch, double voice), fine tune and fixed note. A small
  built-in bank stands in when there is no WAD.
- `opl3.cpp` is a YMF262 emulator written from the chip's behaviour (log-sin
  and exp tables, attenuation-domain operators, envelope generator with KSR
  and KSL, feedback, tremolo/vibrato, 8 waveforms, OPL3 stereo, resampled
  from 49716 Hz). `opl_driver.cpp` is the DMX-style voice driver: 18 voices,
  oldest-stolen; GENMIDI voices programmed per note; volume = velocity x CC7 x
  CC11 turned into TL attenuation; pan into the OPL3 L/R bits; pitch bend
  +-2 semitones into F-Number/BLOCK.
- `music.cpp` owns the sequencer and an `SDL_AudioStream` with a get-callback
  on SDL's audio thread; `play(name, loop, next)`, `stop(fade)`, fades ramped
  per sample. Backend is `OplBackend` or, when a soundfont path resolves and
  the build found fluidsynth, `FluidBackend`.
- Track choice (`App::updateMusic`): title D_INTRO once then D_INTER looped;
  stacking tracks rotate by level over E1-E3 map themes; fights rotate by red
  line count over the boss/late themes; game over plays D_BUNNY once. Doom 2
  names are in the same preference lists.

- `oggstream.cpp` streams Ogg Vorbis through libvorbisfile from a byte range
  of a file (custom read/seek/tell callbacks), linearly resampled to the
  output rate. `Assets` indexes `extras.wad` by reading only its directory
  and the 4-byte header of each `H_`/`O_` lump. `App::resolveTrack` maps a
  classic `D_` name to `H_`/`O_` for the Modern / SC-55 sets with fallback;
  the set and the mute state persist in `settings.txt` (SDL pref path).

## High scores (`src/game/highscores.cpp`)

Top 10 in SDL's pref path as whitespace-separated lines; `add()` returns the
1-based rank (0 = not placed) and saves.

## Mouse in menus (`App::hotText`, `hotspotAt`)

Menus are drawn, not laid out, so clickability is recorded at draw time:
`hotText` draws a label and pushes a `Hotspot` (rect, kind, index); option
rows push one rect spanning label and value. `addHud` clears the list each
frame and the mouse handlers look up the previous frame's list, converting
window coordinates to swapchain pixels. Kinds: menu item (sets `menu_.index`
and calls `menuSelect`), screen item (sets `screenIndex_` and sends Return
through `screenKey`), option row (Left/Right adjust), back (sends Escape).
Hover moves the highlight. `--click x,y@frame` injects a motion + click for
tests and logs what it hit.

## Identity and play statistics (`src/game/playerstats.cpp`)

`PlayerStats` owns three files per profile: `identity.txt` (`player_id`, a
version-4 UUID minted on first load; `email`; `email_verified`), `stats.txt`
(lifetime seconds in the block / fight / menu phases, keyboard / mouse / pad
action counts, sessions, first and last played) and `machines/<install_id>.txt`
(the same counters for this machine plus platform, OS name from
`/etc/os-release`, GPU from the Vulkan device, cores, RAM, gamepad model).
The install id lives in `<pref>/install.txt`. `App` feeds it every frame
(`addTime` by mode) and on each key press, mouse button and pad button, saves
every 30 s, on profile switch and at exit. The email is optional, validated
only for shape, lower-cased, and its verified flag resets when it changes;
the future account feature (docs/online-and-releases.md section 7) verifies
it server-side. `stats_test` covers the round trip across two machines.

## Profiles, trophies, gamepad (`src/game/app.cpp`, `trophies.cpp`)

- Profiles live in `<pref>/profiles/<NAME>/{settings,highscores,trophies}.txt`;
  `<pref>/profile.txt` names the last player. `switchProfile` adopts files
  from the old flat layout the first time. `--profile NAME` skips the prompt;
  scripted scenarios default to a `PLAYER` profile.
- Overlay screens (`screen_`): Options (music set, music and SFX volume
  (`Audio::setMasterGain`), music on, stick sensitivity, invert, rumble,
  display, resolution, models, Doom WAD, soundtrack WAD, email, Doom art with
  BRUTAL as a sub-row, ray tracing, anti-aliasing, bloom & haze; BACK is the
  last row, `n - 1`). The option rows are built as a list and only the slice
  that fits between the title and the pinned BACK is drawn (`optionsScroll_`;
  "N MORE ABOVE/BELOW" markers); keyboard and pad moves keep the selection in
  view (`optionsFollow_`), the wheel scrolls three rows and turns that off
  until the next key, and BACK scrolls to the end. Rows off screen register
  no hotspots), Trophies, Players (the profile manager: `loadProfileList`
  reads each profile's high scores, trophies and stats into `profileInfo_`;
  `profileAction` 0/1/2 = rename via the name screen with `nameRename_`,
  email via `switchProfile` + the email screen, delete with a second
  confirmation (`profileConfirmDelete_`); `renameProfile` detaches `stats_`
  before moving the folder; `deleteProfile` switches away first or, with no
  profile left, resets to the name prompt. `profileRequired_` keeps the
  screen up at start-up until a player is chosen; scripted runs skip it
  unless `REDLINE_ASK_PROFILE=1`; `--keys` chords: `#` is Delete, `~` Enter, `` ` `` Esc), Name entry (keyboard text
  input via SDL_EVENT_TEXT_INPUT; gamepad letter picker), Controls (the key
  and pad reference: three sections drawn as label/keyboard/gamepad columns,
  the row step computed from the window height so a short window shrinks the
  page instead of running off the bottom; reached from the title and the
  pause menu, and the only place the bindings are written down in the game).
  `screenKey` takes both keyboard and pad input mapped to key codes.
- Trophies: fixed catalogue in `trophyCatalogue()`; `App::trophy(id)` unlocks
  once and calls `showTrophy` (a `Toast` queue: one card at a time, slides in
  over 0.35 s, holds 5 s (8 s for the ultimate), slides out; the jingle and
  rumble play when a card appears) and, for `rip_and_tear`, `startCelebration`
  (`celebrateT_` runs 8 s: rockets from the bottom edge every 0.2-0.5 s that
  burst into 40-70 sparks with gravity, three confetti pieces a frame from
  the top, rumble pulses, a gold panel pulse, the title slammed in with a
  halo; `Spark` particles are drawn as HDR screen quads so they bloom).
  `REDLINE_TROPHY_DEMO=1` fires both at frame 60 without unlocking. Hooks: kills (first blood, boss
  tiers, grown), fights survived (red line, untouchable, survivor), clears
  (tetris, combo x3, chain), pickups (collector, arsenal), BFG, invulnerable,
  level-ups, blocks destroyed, a #1 high score (doom_slayer), and
  `rip_and_tear` (RIP AND TEAR!!!), awarded by `App::trophy` itself the moment
  the other 19 are all held.
- Online (`src/game/online.*`, `runrecord.*`, `net/http.*`): `OnlineClient`
  runs one worker thread; `App::pollOnline` handles replies on the main
  thread each frame. `recordRun` at game over writes the `RunRecord` (counters
  in `App::run_`, reset in `newGame`) to `<profile>/runs/` or, when ONLINE is
  on and the profile has a token, to `runs/pending/` and uploads it; the
  reply's rank goes on the game-over screen, a failure leaves the file for
  `submitPendingRuns` at the next launch. Registration: `startRegistration`
  opens `kScreenRegister` (consent text) then `kScreenCode`; a confirmed code
  stores the token in `identity.txt` via `PlayerStats::setVerified`. The
  service URL comes from `--online-server`, `REDLINE_ONLINE_URL`,
  `<pref>/online.txt`, `redline.cfg`, or the default. `REDLINE_ONLINE_ALLOW_SCRIPTED=1`
  lets a scripted run upload (tests only). ONLINE defaults on; `onlineAsked_`
  (settings `online_asked=`) gates the one-time `kScreenOnlineAsk` question on
  the title (skipped in scripted runs unless `REDLINE_ASK_ONLINE=1`). A
  registration reply carries a `poll_secret` (`PlayerStats::pendingPoll`,
  `identity.txt pending_poll=`); `update()` polls `/api/registration` every
  4 s on the code screen and every 30 s otherwise until the server reports
  confirmed (token handed over once, `setVerified`), declined or expired.
- Updates: `checkVersion` fetches `/api/version` at launch (ONLINE on), caches
  it in `<pref>/version.json`, and `applyVersionInfo` sets `updateAvailable_`
  when `latest` is newer than `REDLINE_VERSION` (announcement, title line,
  the WHAT'S NEW menu item's label). `kScreenWhatsNew` wraps the changelog
  bullets to the width, scrolls with Up/Down, and Enter calls `SDL_OpenURL`
  on the release page. `REDLINE_UPDATE_DEMO=<version>` fakes a newer release.
- The title card (`buildTitleCard` in assets.cpp): a 320x200 page composed at
  load time out of whatever art the install has, which is the menu logo, the
  WAD's own letters and two of its monsters over a painted sky, a brick course
  and a stack of blocks. It goes into the atlas twice: as it is, and run
  through `shadeBlue` (luminance into a blue ramp) with the bottom band left
  empty. `App::addSplash` draws the first one over everything at startup and
  `startMelt`/`stepSplash` take it away in `kMeltColumns` strips on Doom's
  35 Hz tic, each strip a quad with its own slice of the atlas region and its
  own offset. Any key or button during the hold starts the melt; nothing
  interrupts the melt itself. `addCardBackdrop` draws the blue copy behind
  `kScreenControls` and `kScreenFarewell`. Scripted runs start past the card
  unless `REDLINE_SPLASH` is set, so every other capture lands on its frame.
- The boss hall's door: `Dungeon::Door` tiles are solid in `solidAt` until
  `doorOpen_`, which is what stops the hall's monsters seeing and shooting
  through it (`lineOfSight` and the projectiles both go through `solidAt`). It
  used to become passable the moment the key was picked up, so the player took
  fire from a wall. It is drawn with a real Doom door texture
  (`Assets::cryptDoor`, BIGDOOR7 and friends), a dim emissive, a point light and
  a glowing skull billboard on whichever face the player is on (`addDoorMark`).
  Walking within two metres of a door tile with the key opens it. The door is one
  quad the width of the gap and the height of the ceiling, drawn flat on the face
  the player is on (`addDoorMark`), because a cube carries a whole texture on each
  face and painting the door on the slab gave a grid of little doors with the
  marker floating in front of each one. `REDLINE_AT_DOOR=1` opens the crypt with
  the player in front of the door, key in hand.
- `moveWithCollision` counts how many of the eight probes around a body are in
  something solid and allows a move that leaves the count no worse. The old
  "refuse any move whose probes touch anything" rule wedged wide bodies for
  good: a spider mastermind is 1.2 m in radius, the arena is ten metres across,
  and standing in the board's first row put one probe inside the frame for ever,
  after which every move in every direction was refused. Two runs with a packed
  board went from 63 logged "blocked" seconds to none.
- Alongside it, a body of radius 0.8 or more that is stopped by blocks shoves
  them out of existence (`shoveBlocks`, 0.35 s cooldown), and one that rises in
  the arena clears a disc around itself (`clearAround`). `REDLINE_LOG_BOSS=1`
  prints where the big ones are, how far they moved in the last second, how far
  away the player is, and whether they are blocked.
- Hitscan reach: `hitscanRange(kind)` in fps_mode.cpp, 16 m for a zombieman,
  24 m for a chaingunner, 45 m for the mastermind, 30 m otherwise. Past it they
  hold fire and close in, and they do not wake from beyond it either. Doom had
  no limit because Doom's rooms were small; a crypt corridor is forty metres.
- Floating health bars are drawn only when a ray to the enemy's head is clear;
  the three-line threat list in the corner is always drawn.
- The UNTOUCHABLE trophy uses `damageTakenInArena()`, snapshotted when the gate
  falls, so the crypt and its boss are not part of it.
- Leaving: QUIT and Esc on the title call `quitGame`, which puts
  `kScreenFarewell` up (the blue page, the player's numbers, where to get the
  next version) until any key, or ten seconds. Scripted runs and the bot go
  straight out unless `REDLINE_FAREWELL` is set; the window's close button
  always exits at once.
- The fight bot (`--bot`): aims at a target it holds for three seconds, but only
  pitches off level when it has a clear shot (clamped to 0.45 rad); with no shot
  it faces its next waypoint and keeps the head level. It fetches the skull key
  first, then walks at the door, because a solid door cannot be pathed through.
  `REDLINE_LOG_BOT=1` prints the frame, target, distance, whether the shot is
  clear, the waypoint, the stuck timer and the pitch.
- Ray tracing: `rtShadows_` defaults to 3 but is forced to 0 (and saved) on a
  GPU without ray queries, so the setting file reflects what runs.
- Level card: 4 s after the fly-out; block gravity pauses while a piece would
  be falling, the collapse still animates.
- Gamepad: SDL3 gamepad API; axes polled per frame with an 18 % dead zone and
  squared look response (3.4 / 2.2 rad/s at full stick times sensitivity);
  left stick emulates the d-pad with hysteresis in block mode so DAS works.
- Corpses: Dead monsters are drawn for 1.6 s (bosses 1.2 s, cacodemons
  0.35 s; Brutal deaths as long as their animation) plus 5 s, and
  shrink/darken over the last 0.35 s.
- Evil banner: while `danger() > 0` in block mode the HUD draws the warning
  in the column right of the board (`x = ((W/2 + 0.3H) + W - 24) / 2`), two
  lines, with an eight-way red halo, hash-driven flicker (dropouts scale with
  danger), a scale pop and jitter from `evilJolt_` (set on `CellTurnedRed`),
  constant shake in panic, and `Drip` particles spawned along the last
  banner's extent that run down as viscous blood. Announcements stack under
  it in that column while playing, shrunk to fit; elsewhere they stay centred.
