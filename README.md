# REDLINE

Falling blocks that refuse to die.

It plays like classic block stacking, except some minos are **red**. Red blocks
refuse to dissolve when a row completes: the normal blocks vanish, the red ones
stay and sink deeper into the stack. When a row is *entirely* red the game
pulls the camera into the board, the red row turns into Doom imps, and you clear
them with a shotgun. Every demon you kill explodes and takes the neighbouring
blocks with it. Then you fly back out and keep stacking.

Native Vulkan 1.3 renderer (dynamic rendering, instanced cubes and billboards,
one atlas). Art and sounds come straight out of your Doom IWAD when one is
present, with procedural fallbacks so the game runs without it.

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

## Controls

Block mode:

| Key | Action |
|---|---|
| Left / Right, A / D | move (auto-repeats) |
| Up, X, W | rotate clockwise |
| Z, Left Ctrl | rotate counter-clockwise |
| Down, S | soft drop |
| Space | hard drop |
| Esc | pause |
| F12 | screenshot to `redline-screenshot.png` |

First-person mode: mouse to look, WASD to move, left click / Space to fire.

## Rules that differ from classic

- Each mino has a chance of spawning red (grows with level, max two per piece).
- Red cells are loose: after a piece locks or a row clears, any red cell with
  empty space below it falls until it lands.
- When a full row clears, the shift-down happens **per column**. A column whose
  cell in that row is red keeps it (and everything stacked on it stays put).
- A row that is entirely red never clears. It triggers the first-person phase.
- Killing a demon removes its cell and every *normal* cell within 1.5 cells.
  Red cells are only removed by killing them.
- Surviving the phase pays 1000 x level; each kill pays 50 + 25 per block destroyed.
- Fireballs hurt. Reaching zero health ends the game.

## Command line

```
--wad <file>        --no-wad           --size WxH        --fullscreen
--igpu              --seed <n>         --scenario title|blocks|redline|fps
--screenshot <png>  --frames <n>       --bot              --mute
```

`--scenario redline` starts with a nearly complete red row and drops the last
red piece for you; `--scenario fps` skips straight to the fight. `--bot` makes
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
src/audio     SDL3 stream mixer
shaders       GLSL, compiled by glslc at build time and embedded
tests         tetris_test, wad_test
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
