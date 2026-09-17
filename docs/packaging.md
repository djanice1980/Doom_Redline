# Building installers

REDLINE ships without any Doom data. The player supplies their own
`doom.wad` or `doom2.wad` (Steam, GOG, or the original disc); `extras.wad`
from the Doom + Doom II rerelease, kept next to it, adds the modern and SC-55
soundtracks. Nothing is copied: the game reads the WAD in place.

## How the game finds the WAD

At start-up `Assets::findWad` tries, in order:

1. `--wad <file>` on the command line
2. `$REDLINE_WAD`
3. the player's saved choice, `wad.txt` in the save folder
   (`~/.local/share/redline/redline/` on Linux, `%APPDATA%\redline\redline\` on Windows)
4. `redline.cfg` next to the executable, a `wad=<path>` line an installer writes
   (an optional `extras=<path>` line names the soundtrack file the same way)
5. `wads/` in the current folder and next to the executable
6. the Steam and GOG install folders of the platform

If none of those hit, the title screen opens **REDLINE NEEDS DOOM** with four
choices: **browse for the WAD file** (the operating system's own open-file
window, via SDL3), **type the path**, **play with placeholder art for now**,
or quit. A chosen file is checked (it has to contain the Doom sprites), the
art, sounds and music are swapped in on the spot, and the path is saved to
`wad.txt` so the question is never asked again. The same thing is available
later under **OPTIONS > DOOM WAD**. So an installer only needs to *offer* the
question; a blank answer is fine.

For a smoke test of the live swap without a dialog:
`redline --reload-wad /path/to/doom.wad` starts on placeholder art and switches
after 30 frames.

## Bundled data that is not the WAD

Three folders ship with every build and are optional at runtime; the game
falls back to WAD art when one is missing. Each is self-contained with its own
licence or credits file, so any of them can be dropped from a release without
touching the code:

| Folder in the repo | Windows zip / installer | Linux install | What it is |
|---|---|---|---|
| `assets/voxel-doom/` | `voxels/` next to the exe | `share/redline/voxel-doom` | Cheello's Voxel Doom models (MIT); OPTIONS > MODELS |
| `assets/materials/` | `materials/` | `share/redline/materials` | Normal and roughness maps for the three arena textures from GZDoom: Ray Traced |
| `assets/brutal/` | `brutal/` | `share/redline/brutal` | Community gore sprites, sounds and voxel gibs (`CREDITS.txt` inside); OPTIONS > DOOM ART > BRUTAL |

`REDLINE_VOXELS`, `REDLINE_NO_MATERIALS=1` and `REDLINE_BRUTAL_PACK` point at
or disable each one for testing.

## Windows (MSVC + vcpkg + Inno Setup)

Everything below runs in a normal Windows command prompt (or PowerShell); no
Linux tools are involved. Total download is a few GB.

1. **Visual Studio 2022 Build Tools** with the *Desktop development with C++*
   workload: https://visualstudio.microsoft.com/visual-cpp-build-tools/
   (the full Visual Studio Community edition works too). Install, then use the
   *Developer Command Prompt for VS 2022* from the Start menu for the rest.
2. **Vulkan SDK** from LunarG: https://vulkan.lunarg.com/sdk/home#windows.
   Run the installer with the defaults; it sets `VULKAN_SDK` and puts `glslc`
   in `%VULKAN_SDK%\Bin`. Open a new prompt afterwards so the variable is seen.
3. **CMake 3.24+** (the VS installer includes one; or https://cmake.org/download/).
4. **vcpkg** for SDL3, glm and libvorbis:
   ```
   git clone https://github.com/microsoft/vcpkg C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   setx VCPKG_ROOT C:\vcpkg
   ```
   Open a new prompt so `VCPKG_ROOT` is set. The repo's `vcpkg.json` lists the
   packages; CMake installs them on the first configure (10-20 minutes).
5. **Build and stage** (from the repo folder):
   ```
   cmake --preset windows-release
   cmake --build --preset windows-release
   ctest --preset windows-release
   cmake --install build-win --config Release
   ```
   `build-win\install\bin\` now holds `redline.exe` and the DLLs it needs
   (SDL3, vorbis, ogg). You can run it from there already.
6. **Inno Setup 6**: https://jrsoftware.org/isdl.php. Install it, open
   `packaging\windows\redline.iss` and press *Compile* (or run
   `"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" packaging\windows\redline.iss`).
   The result is `build-win\redline-0.1.0-setup.exe`.

What the installer does: copies the staged files to `C:\Program Files\REDLINE`,
adds Start-menu and optional desktop shortcuts, and shows a **Doom game data**
page with a file picker. The page is pre-filled when a Steam or GOG Doom is
found. The chosen path is written as `wad=...` to `redline.cfg` next to the
executable (step 4 of the search order); leaving the box empty skips the file
and the game asks on first launch instead. A second, optional box on the same
page takes `EXTRAS.WAD` for the rerelease soundtracks (pre-filled when it sits
next to the WAD) and becomes an `extras=...` line. Uninstall removes `redline.cfg`
but never touches the WAD. Logs go to `%APPDATA%\redline\redline\redline.log`
because the executable is a GUI-subsystem program with no console.

Not yet done on Windows: FluidSynth (optional soundfont backend) is not in
`vcpkg.json`, so music uses the built-in OPL3 emulator, which is the intended
default anyway. Add `"fluidsynth"` to the dependency list to include it.

## Windows build from Linux (Docker, no Windows machine needed)

`packaging/windows/cross-build.sh` produces a test build without touching
Windows: it runs a Fedora container with the mingw-w64 toolchain and Fedora's
mingw packages for SDL3, libvorbis, libogg and the Vulkan loader, builds with
the repo's CMake files through a mingw toolchain file, and stages
`build-win/redline-<version>-win64/` (redline.exe, the DLLs it imports, the
voxel pack, README, LICENSE, docs) plus a zip of it. The unit tests are
compiled as part of the build but not staged; `REDLINE_STAGE_TESTS=1` adds
them for running under Wine. Nothing in the release flows (this script,
`cmake --install`, CPack, the Inno Setup script) ever ships a test binary. Needs Docker
and, on the first run, the network for the image and packages (about 400 MB);
a rebuild takes a couple of minutes.

```bash
packaging/windows/cross-build.sh
```

The C++ runtime and winpthreads are linked statically, so the only DLLs next
to the exe are SDL3, vorbis, vorbisfile and ogg. `vulkan-1.dll` is
deliberately not shipped: on Windows it comes with the graphics driver.
Unzip anywhere and run `redline.exe`; the first launch asks for the WAD.
The zip is a plain test build, not the installer; the Inno Setup script above
still expects the MSVC staging folder, or point its `StageDir` at this one.

## Linux

Build dependencies: a C++20 compiler, CMake 3.24+, Ninja, SDL3, glm, the
Vulkan headers and loader, `glslc` (from `shaderc`), `libvorbis`, `pkgconf`;
FluidSynth is optional.

- Arch / CachyOS: `sudo pacman -S --needed base-devel cmake ninja sdl3 glm vulkan-headers vulkan-icd-loader shaderc libvorbis pkgconf`
- Debian 13+ / Ubuntu 25.04+: `sudo apt install build-essential cmake ninja-build libsdl3-dev libglm-dev libvulkan-dev glslc libvorbis-dev pkgconf`
- Fedora 41+: `sudo dnf install gcc-c++ cmake ninja-build SDL3-devel glm-devel vulkan-headers vulkan-loader-devel glslc libvorbis-devel pkgconf`

Build, test, install:

```bash
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
sudo cmake --install build          # /usr/local/bin/redline + desktop entry + icon
```

Packages, from the build folder:

```bash
cd build
cpack -G TGZ          # redline-0.1.0-Linux.tar.gz (bin/, share/ layout; unpack anywhere)
cpack -G DEB          # on Debian/Ubuntu: dependencies are computed by dpkg-shlibdeps
cpack -G RPM          # on Fedora, needs rpm-build
```

Arch users can build a proper pacman package from the checkout:

```bash
cd packaging/arch
makepkg -si
```

There is nothing WAD-specific in the Linux packages: the first launch shows the
same **REDLINE NEEDS DOOM** screen, whose *browse* button uses the desktop
portal or `kdialog`/`zenity`, whichever SDL3 finds. To pre-seed it system-wide,
put a `redline.cfg` with `wad=/path/to/doom.wad` next to the binary, or per
user write the path to `~/.local/share/redline/redline/wad.txt`.

## What is and is not verified

Verified on this machine (CachyOS): the Linux build, tests, `cmake --install`,
`cpack -G TGZ`, the first-run screen, the typed-path screen, both config
files, and the live asset swap. The native file dialog was opened from the
setup screen without errors but a file was not picked through it
automatically.

Windows: the mingw cross-build (`cross-build.sh`) compiles cleanly, all six
test executables pass under Wine, and `redline.exe` runs under Wine with
Vulkan on the host GPU: it found the Steam WAD passed on the command line,
indexed extras.wad, ran a level-3 fight and wrote a screenshot; without a
WAD it showed the first-run chooser. Not yet verified: the same binary on a
real Windows install, the native file dialog there, gamepad and fullscreen
on Windows, and the MSVC + vcpkg path (never compiled). The Inno Setup
script has not been run.
