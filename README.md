# REDLINE

Falling blocks that refuse to die.

It plays like classic block stacking, except some minos are **red**. Red blocks
refuse to dissolve when a row completes: the normal blocks vanish, the red ones
stay and sink deeper into the stack. When a row is *entirely* red the whole
board tips over onto the floor and you land inside it: every block is now a
chest-high wall, and every connected cluster of red blocks has become a Doom
monster standing in the pocket it left behind. Small clusters are zombies and
imps; big ones are demons, cacodemons and barons. Kill them all with the
shotgun. Every kill explodes and takes the neighbouring blocks with it. Then the
board stands back up, the level goes up, and you keep stacking, faster.

Native Vulkan 1.3 renderer (dynamic rendering, instanced cubes and billboards,
one atlas). Art, sounds and music come straight out of your Doom IWAD when one
is present, with procedural fallbacks so the game runs without it. The music
is Doom's own MUS scores played through a built-in OPL3 (Adlib) synthesizer
using the WAD's GENMIDI instrument bank, the way the game sounded in 1993.

## Build (Arch / CachyOS)

Everything needed is a pacman package:

```bash
sudo pacman -S --needed cmake ninja gcc vulkan-headers vulkan-icd-loader shaderc sdl3 glm vulkan-radeon
```

Then, from this directory:

```bash
cmake -S . -B build -G Ninja
```

```bash
ninja -C build
```

Run the tests (rules engine + WAD reader):

```bash
ctest --test-dir build
```

Run the game:

```bash
./build/redline
```

## Doom assets

The game looks for an IWAD in this order and uses the first that exists:

1. `--wad <file>` on the command line
2. `$REDLINE_WAD`
3. `./wads/*.wad` (this folder is git-ignored; drop or symlink a WAD there)
4. The Steam install of Ultimate Doom / Doom II (`~/.steam/steam/steamapps/common/Ultimate Doom/rerelease/doom.wad`, etc.)

If nothing is found it says so on stderr and uses procedural art. You need to own
the game; the WADs are never copied into the repo.

`build/src/wad/wadinfo <file.wad>` prints what a WAD contains (sprites, textures,
flats, font glyphs, sounds) and can dump individual lumps; `--help` lists flags.

## Music

Doom's soundtrack plays from the WAD: the title fanfare and intermission
theme on the menu, a level track while stacking (changing with the level),
the boss-level themes during fights, and the ending music over game over.
Press M to toggle it; `--music-volume 0.3` sets the level; `--no-music`
keeps only the sound effects.

The 2024 rerelease ships two more soundtracks in `extras.wad` next to the
IWAD, and the game streams them straight out of that file: **Modern** (Andrew
Hulshult's 2023 recordings, `H_*`) and **SC-55** (the original score recorded
on a Roland SC-55, `O_*`, covering most Doom II and a few Doom tracks).
Cycle Classic / SC-55 / Modern with the N key or the MUSIC item on the title
and pause menus; the choice is remembered in `settings.txt` next to the high
scores. `--music classic|sc55|modern` sets it from the command line, and
`REDLINE_EXTRAS=/path/extras.wad` points at the file if it is elsewhere. Any
piece a set lacks falls back to the classic version.

By default the tracks go through the built-in OPL3 emulator. If you prefer
General MIDI, install a soundfont and the game switches to FluidSynth
automatically (rebuild after installing so CMake picks up the library, which
is already on this machine):

```bash
sudo pacman -S --needed soundfont-fluid
```

That installs `/usr/share/soundfonts/FluidR3_GM.sf2`, which is found on its
own; any other `.sf2` works via `REDLINE_SOUNDFONT=/path/to/file.sf2`.

## High scores

The top ten (score, level, red lines survived, lines, date) are kept in
`~/.local/share/redline/redline/highscores.txt`. The title screen shows the
top five and the game-over screen announces your rank.

## Players, trophies and level-ups

The first launch asks for your name. Each player gets a profile (a folder
under `~/.local/share/redline/redline/profiles/`) holding their own high
scores, trophies and settings (music set and volume, stick sensitivity,
inverted look, rumble). Switch or add players from the PLAYER item on the
title screen; the last player is remembered.

Trophies are one-time achievements (19 of them, from FIRST BLOOD to DOOM
SLAYER! for a new #1 high score). Unlocking one pops a gold banner with a
jingle; the full list with unlock dates is under TROPHIES on the title screen
(or press T).

Surviving a fight brings up a level-up card: the new level, demons slain,
blocks destroyed, fight time and damage taken. The collapse plays behind it
and the next piece waits until the card is gone.

## Display

OPTIONS has DISPLAY (windowed, borderless fullscreen at the desktop
resolution, or exclusive fullscreen at a chosen mode) and RESOLUTION (the
sizes your monitor supports plus common windowed sizes). Changes apply at
once and are remembered per machine in `display.txt` next to the profiles;
Alt+Enter toggles windowed and borderless fullscreen anywhere. `--fullscreen`
and `--size WxH` still work for a single run.

## Gamepad

Plug in any controller SDL recognises (Xbox, PlayStation, Switch Pro, most
generic pads). Rumble is used for shots, hits, explosions and trophies.

| | Block mode | Fight |
|---|---|---|
| Left stick / d-pad | move (auto-repeats), down = soft drop | move |
| Right stick | | look |
| A | rotate clockwise | fire |
| B | rotate counter-clockwise | |
| X / d-pad up | hard drop | previous weapon |
| Y | | next weapon |
| Bumpers | rotate | previous / next weapon |
| Right trigger | | fire |
| Left trigger / L3 | | run |
| Start | pause | pause |

Menus: d-pad to pick, A to confirm, B to back out. Name entry on a pad: up
and down pick a letter, A adds it, B deletes, Start confirms. Stick
sensitivity, inverted look and rumble live under OPTIONS.

## Controls

Block mode:

| Key | Action |
|---|---|
| Left / Right, A / D | move (auto-repeats) |
| Up, X, W | rotate clockwise |
| Z, Left Ctrl | rotate counter-clockwise |
| Down, S | soft drop |
| Space | hard drop |
| Esc | pause menu (resume / options / restart / quit) |
| T | trophies (title screen) |
| F12 | screenshot to `redline-screenshot.png` |

First-person mode: mouse to look, WASD to move, Shift to run, left click /
Space to fire, 1-4 or mouse wheel (Q/E) to switch weapons. Menus (title, pause, game over):
Up/Down or W/S to pick, Enter or click to confirm.

## Rules that differ from classic

- Each mino has a chance of spawning red (grows with level, max two per piece).
- Red minos stay part of the piece they land in. They only sink when a row
  clears beneath them or when the board collapses after a fight.
- When a full row clears, the shift-down happens **per column**. A column whose
  cell in that row is red keeps it (and everything stacked on it stays put).
- **Corruption.** Once the stack is 9 rows high, normal blocks start turning
  evil. Within four rows of the top it goes into overdrive (at least six
  events a second, bigger bursts, evil spawns just as fast): a board about to
  overflow is driven into a fight instead, which is your way out. Two thirds of the time an event goes after a hole in an almost-full
  row (two empties or fewer, something solid above it): the blocks above and
  below the hole turn first, then the ones beside it, so a change builds the
  surround for an evil spawn a few seconds later. Otherwise it picks the row
  closest to going all red (most red blocks, then fullest, then lowest). An
  event turns a random number of blocks, from one up to five when the stack
  is at the top. A chosen block flickers
  between its colour and red (faster and faster) for 1.6 s, then becomes a red
  block. Events come more often the higher the stack (square of the height
  above the threshold, plus 10 % per level). A high stack is therefore pushed
  towards a fight rather than merely sprinkled with red. If a turned block
  completes a red row, the fight starts right away and the piece in the air
  resumes afterwards.
- **Evil spawns.** A hole fully surrounded by red (left, right, above and
  below all red or turning) gets filled by evil: a red block grows out of
  nothing over 1.2 s and then is simply there. On the bottom row the floor
  counts as the block below, so left, right and top must be red; the side
  walls count for a missing left or right, but there must always be a real
  block above and at least two real red neighbours. This fires
  more readily than corruption and does not need a tall stack (it only gets
  faster as the stack rises), so a red row with one gap will close itself
  unless you fill it first.
- A row that is entirely red never clears. It triggers the first-person phase.
- In that phase **every** 4-connected red cluster on the board becomes one
  monster; its class depends on the cluster size (1: zombieman, 2-3: imp, 4-6:
  demon, 7-11: cacodemon, 12-17: baron, 18-24: cyberdemon, 25+: spider
  mastermind) with a level-dependent chance of being bumped up a class.
  Each level caps the biggest class that can spawn (level 1: demons, 2:
  cacodemons, 3-4: barons, 5-6: cyberdemons, 7+: spider masterminds); a red
  region too big for the cap is split into several monsters, and regions of 7+
  cells split half the time anyway, so a full red row is a crowd, not one
  boss. Absorbing can grow a monster one class past the cap. There is never
  more than one boss (cyberdemon or spider mastermind) alive at once: extra
  ones spawn as barons, and a monster will not grow into a boss while one
  lives.
  Cyberdemon rockets have splash damage and blow up the blocks they hit. Monster health scales with the level (x0.45 at level 1,
  +0.15 per level, capped at x2.5), and so does their attack cadence.
- Killing a monster explodes every cell of its cluster, destroying the *normal*
  cells within 1.5 cells of each. Standing next to it hurts you too.
- Blocks are chest-high: you can see and shoot over them; fireballs aimed at
  your body are stopped by them. Zombies hitscan, imps/cacodemons/barons throw
  fireballs, demons charge and bite.
- Before the fight starts there is a three-second GET READY countdown: the
  monsters rise out of the board, you can look around, nobody shoots.
- Monsters eat your cover. Each one periodically destroys a block, preferring
  the one that hides you: zombies every ~11 s, imps ~7.5 s, demons ~5 s,
  cacodemons ~3.5 s, barons every ~2.5 s and three blocks at a time. Blocks are
  cover for a limited time only.
- Leave a monster alive too long (about 20 s at level 1, one second less per
  level, floor 8 s) and there is a 75 % chance it absorbs every normal block
  within 2.5 cells and comes back one class bigger at full health, all the way
  up to spider mastermind. It pulses red for the last four seconds and the HUD
  warns you (a top-tier monster never warns, because it cannot grow).
- Wounded monsters sometimes (12 %, at most once per 2.5 s each) shed ammo
  for the gun you are holding.
- Dying monsters drop loot: stimpacks and medikits, ammo, and weapons
  (chaingun, rocket launcher, plasma rifle) that pop out and land next to the
  body. Bigger monsters drop more: cacodemons and up always drop a medikit,
  barons and up almost always a weapon too, cyberdemons and up two medikits. Walk over an item to take it; weapons you
  already own give ammo instead. Weapons persist for the rest of the game.
- The shotgun never runs out. The chaingun is fast hitscan, the plasma rifle
  fires fast bolts, and rockets have splash damage that also blasts the blocks
  around the impact (and you, if you are too close).
- **Prizes.** Every line clear banks something for the next fight, shown
  under NEXT FIGHT in the side panel: a single gives 5 armour; a double 10
  health and 10 armour; a triple 20 / 20 and a 10 % chance of
  invulnerability; a tetris 40 / 40 and 25 %. Combo and chain multipliers
  apply. Health above 100 (cap 200) is bonus you cannot refill; armour soaks
  half of every hit until spent (cap 200); the invulnerability roll happens
  as the monsters rise and gives 10 s of immunity (golden screen, countdown).
- **Scoring.** Line clears pay 100 / 300 / 600 / 1000 for 1 / 2 / 3 / 4 lines,
  times the level. Consecutive clearing pieces build a combo (+50 % per
  step); a clear caused by a collapse after a fight is a chain (+100 % per
  cascade step). Kills pay by class (zombie 100 ... baron 1200, cyberdemon
  2500, spider 3000) scaled +10 % per level, plus 25 per block the death
  blast destroyed. Surviving the phase pays 1000 x level and raises the level
  by one (faster gravity, more red minos, tougher monsters next time).
- When the board stands back up, every remaining block falls to the floor
  (column by column, animated), so the holes the explosions left collapse.
- Reaching zero health ends the game.

## Command line

```
--wad <file>        --no-wad           --size WxH        --fullscreen
--igpu              --seed <n>         --scenario title|blocks|redline|fps
--screenshot <png>  --frames <n>       --bot              --mute
--level <n>         --absorb <sec>     --god
```

`--scenario redline` starts with a nearly complete red row plus a few red
clusters of different sizes and drops the last red piece for you; `--scenario
fps` skips straight to the fight (`--level N` sets the level for it; 8+ adds a
cyberdemon-sized slab); `--scenario corrupt` starts with a tall holey stack so
you can watch blocks turn. `--bot` makes
the player auto-aim and fire, which together with `--frames`/`--screenshot`
gives a headless-ish smoke test of the whole loop:

```bash
REDLINE_NOVSYNC=1 ./build/redline --mute --scenario redline --bot --frames 2400 --screenshot loop.png
```

Environment: `REDLINE_GPU=<index>` forces a Vulkan device (the log lists them),
`REDLINE_VALIDATION=1` enables the Khronos validation layer if installed,
`REDLINE_NOVSYNC=1` uses mailbox/immediate presentation.

## Layout

```
src/core      rules engine (tetris.h) and shared image/PNG helpers - no deps
src/wad       Doom WAD reader: patches, sprites, flats, textures, fonts, DMX sounds
src/render    Vulkan context, atlas packer, instanced renderer
src/game      assets (WAD -> atlas/sounds), FPS simulation, app/modes/HUD
src/audio     SDL3 stream mixer, MUS sequencer, GENMIDI, OPL3 emulator, FluidSynth backend
shaders       GLSL, compiled by glslc at build time and embedded
tests         tetris_test, wad_test, opl_test, music_test
tools         wadinfo, embed.cmake
docs          design.md (rules and scene layout), remix.md (RTX Remix plan)
```

## RTX Remix

See [docs/remix.md](docs/remix.md). Short version: RTX Remix is a Windows
D3D9-replacement runtime with a C API, not a Linux Vulkan library, so this
project keeps a renderer interface whose material model (albedo, roughness,
metallic, emissive, point/distant lights) maps 1:1 onto the Remix API. The
native Vulkan backend is what runs here; a Remix backend is the planned Windows
target.
