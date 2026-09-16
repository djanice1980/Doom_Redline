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
  variant where loose red cells fall after every lock. When nothing moves, the
  clear check runs again (a collapse can complete a row), then the red-line
  check, then the next piece spawns.
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
  stacking (`App::bfgHoldT_`, once per playthrough via `bfgUsed_`; a green
  glow builds while charging and resets if a key is released) calls
  `Game::purgeRed()`: every red
  block is erased, all flickering and spawning stops, the red minos of the
  falling and next pieces become normal, and the whole stack collapses to the
  floor (Settling with `collapseAll_`, the post-fight collapse; the falling
  piece waits and resumes). Green flash, DSBFG, "BFG SPENT" in
  the side panel afterwards. Not mentioned in the README or in-game hints.
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
to white so HUD tints work), `M_DOOM` title. Sounds: `DSSHOTGN`, `DSBAREXP`,
`DSFIRSHT`, `DSFIRXPL`, `DSPOPAIN`, `DSBGDTH1`, `DSBGSIT1`, `DSPLPAIN`,
`DSPDIEHI`, `DSDMACT`, `DSITEMUP`, `DSPSTOP`, `DSSWTCHN`, `DSSTNMOV`, `DSGETPOW`.
Anything missing falls back to `procedural.cpp`. Block tiles are always ours.

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
- Overlay screens (`screen_`): Options (music set/volume/on, stick
  sensitivity, invert, rumble), Trophies, Players, Name entry (keyboard text
  input via SDL_EVENT_TEXT_INPUT; gamepad letter picker). `screenKey` takes
  both keyboard and pad input mapped to key codes.
- Trophies: fixed catalogue in `trophyCatalogue()`; `App::trophy(id)` unlocks
  once with a banner, jingle and rumble. Hooks: kills (first blood, boss
  tiers, grown), fights survived (red line, untouchable, survivor), clears
  (tetris, combo x3, chain), pickups (collector, arsenal), BFG, invulnerable,
  level-ups, blocks destroyed, a #1 high score (doom_slayer), and
  `rip_and_tear` (RIP AND TEAR!!!), awarded by `App::trophy` itself the moment
  the other 19 are all held.
- Level card: 4 s after the fly-out; block gravity pauses while a piece would
  be falling, the collapse still animates.
- Gamepad: SDL3 gamepad API; axes polled per frame with an 18 % dead zone and
  squared look response (3.4 / 2.2 rad/s at full stick times sensitivity);
  left stick emulates the d-pad with hysteresis in block mode so DAS works.
- Corpses: Dead monsters are drawn for 1.6 s (bosses 1.2 s, cacodemons
  0.35 s) and shrink/darken over the last 0.35 s.
