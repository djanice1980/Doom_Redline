#!/usr/bin/env bash
# Builds every release artefact from this checkout, on Linux:
#   build/redline-<v>-Linux.tar.gz            portable tarball (cpack TGZ)
#   build/redline_<v>_amd64.deb               Debian/Ubuntu package (cpack DEB, explicit depends)
#   packaging/arch/redline-<v>-1-x86_64.pkg.tar.zst   Arch / CachyOS package (makepkg)
#   build/redline-<v>-x86_64.AppImage         any distro (linuxdeploy)
#   build-win/redline-<v>-win64.zip           Windows portable zip (Docker + mingw cross-build)
#   build-win/redline-<v>-setup.exe           Windows installer (Inno Setup via Docker + Wine)
#
# Needs: a configured build in build/ (cmake -S . -B build -G Ninja), which it builds
# and tests here first, and docker for everything after that. makepkg, linuxdeploy and
# the mingw toolchain all live inside the containers. Pass --no-windows to skip the
# Windows half.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"
ver=$(sed -n 's/^project(redline VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
echo "== REDLINE $ver"

echo "== build"
cmake --build build
(cd build && ctest --output-on-failure)

# The four Linux artefacts are built in a stock Arch container, not here. A binary
# linked on this machine carries CachyOS's microarchitecture level in its ELF notes
# and is refused by the loader on an ordinary CPU; makepkg would also bake in
# -march=native. See packaging/linux/container-build.sh for the whole story.
echo "== linux artefacts (container)"
packaging/linux/container-build.sh

if [ "${1:-}" != "--no-windows" ]; then
  echo "== windows zip"
  packaging/windows/cross-build.sh
  echo "== windows installer"
  rm -rf build-win/install && mkdir -p build-win/install
  cp -r "build-win/redline-$ver-win64" build-win/install/bin
  rm -f build-win/install/bin/README.md build-win/install/bin/LICENSE   # the script adds them itself
  docker run --rm -v "$here":/work amake/innosetup packaging/windows/redline.iss
fi

echo "== done"
ls -la "build/redline-$ver-Linux.tar.gz" "build/redline_${ver}_amd64.deb" "build/redline-$ver-x86_64.AppImage" \
  "packaging/arch/redline-$ver-1-x86_64.pkg.tar.zst" "build-win/redline-$ver-win64.zip" "build-win/redline-$ver-setup.exe" 2>/dev/null
