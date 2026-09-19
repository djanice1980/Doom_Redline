#!/usr/bin/env bash
# Builds every release artefact from this checkout, on Linux:
#   build/redline-<v>-Linux.tar.gz            portable tarball (cpack TGZ)
#   build/redline_<v>_amd64.deb               Debian/Ubuntu package (cpack DEB, explicit depends)
#   packaging/arch/redline-<v>-1-x86_64.pkg.tar.zst   Arch / CachyOS package (makepkg)
#   build/redline-<v>-x86_64.AppImage         any distro (linuxdeploy)
#   build-win/redline-<v>-win64.zip           Windows portable zip (Docker + mingw cross-build)
#   build-win/redline-<v>-setup.exe           Windows installer (Inno Setup via Docker + Wine)
#
# Needs: a configured Release build in build/ (cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release),
# makepkg (Arch), docker, and linuxdeploy-x86_64.AppImage (from
# https://github.com/linuxdeploy/linuxdeploy/releases/tag/continuous) either on PATH or
# pointed at by LINUXDEPLOY=/path/to/linuxdeploy-x86_64.AppImage. Pass --no-windows to skip Docker.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"
ver=$(sed -n 's/^project(redline VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
echo "== REDLINE $ver"

echo "== build"
cmake --build build
(cd build && ctest --output-on-failure)

echo "== tarball"
(cd build && cpack -G TGZ)

echo "== deb"
(cd build && cpack -G DEB -D CPACK_DEBIAN_PACKAGE_SHLIBDEPS=OFF -D CPACK_DEBIAN_PACKAGE_ARCHITECTURE=amd64 \
  -D CPACK_DEBIAN_PACKAGE_DEPENDS="libsdl3-0, libvorbisfile3, libvulkan1, libcurl4t64 | libcurl4" \
  -D CPACK_DEBIAN_PACKAGE_MAINTAINER="David Janice <djanice1980@gmail.com>" -D CPACK_DEBIAN_FILE_NAME=DEB-DEFAULT)

if command -v makepkg >/dev/null; then
  echo "== arch package"
  (cd packaging/arch && makepkg -f --noconfirm)
fi

echo "== appimage"
ld=${LINUXDEPLOY:-$(command -v linuxdeploy-x86_64.AppImage || command -v linuxdeploy || true)}
if [ -z "$ld" ] || [ ! -x "$ld" ]; then   # fetch the tool into build/ when it is not around
  ld="$here/build/linuxdeploy-x86_64.AppImage"
  [ -x "$ld" ] || curl -sL -o "$ld" https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage && chmod +x "$ld"
fi
if [ -x "$ld" ]; then
  rm -rf build/AppDir
  DESTDIR="$here/build/AppDir" cmake --install build --prefix /usr >/dev/null
  (cd build && APPIMAGE_EXTRACT_AND_RUN=1 "$ld" --appdir AppDir --output appimage > appimage.log 2>&1 && mv -f REDLINE-x86_64.AppImage "redline-$ver-x86_64.AppImage") || { echo "   AppImage failed, see build/appimage.log"; }
else
  echo "   (skipped: linuxdeploy not found)"
fi

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
ls -la build/redline-*-Linux.tar.gz build/redline_*.deb build/redline-*.AppImage packaging/arch/redline-*.pkg.tar.zst build-win/redline-*-win64.zip build-win/redline-*-setup.exe 2>/dev/null
