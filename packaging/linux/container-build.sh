#!/usr/bin/env bash
# Builds every Linux release artefact inside a stock Arch container, which is the
# only way to get portable binaries from a CachyOS machine:
#
#   * CachyOS's glibc is compiled for a microarchitecture level (v3 or v4 depending
#     on which repo the machine is on), and its startup objects carry that level in
#     .note.gnu.property. The linker merges the note into every executable linked
#     against them, so a binary built here is refused by the loader on a lesser CPU
#     with "CPU ISA level is lower than required", even when the code itself is
#     plain x86-64 and would have run.
#   * makepkg exports CFLAGS from /etc/makepkg.conf, which on CachyOS is
#     -march=native. That one emits real AVX-512 on a Zen 5 host, so the package
#     genuinely cannot run anywhere else.
#
# A stock archlinux:base-devel image has baseline startup objects and -march=x86-64,
# so what comes out of it runs on any x86-64 machine with a Vulkan driver.
#
#   build/redline-<v>-Linux.tar.gz              portable tarball
#   build/redline_<v>_amd64.deb                 Debian / Ubuntu
#   build/redline-<v>-x86_64.AppImage           any distro
#   packaging/arch/redline-<v>-1-x86_64.pkg.tar.zst   Arch / CachyOS
#
# Usage: packaging/linux/container-build.sh      (from anywhere in the repo)
# Needs: docker, and network on the first run for the image and its packages.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="$(grep -m1 'project(redline VERSION' "$REPO/CMakeLists.txt" | sed -E 's/.*VERSION ([0-9.]+).*/\1/')"
IMAGE="archlinux:base-devel"
mkdir -p "$REPO/build" "$REPO/packaging/arch"

docker run --rm \
  -v "$REPO:/src:ro" -v "$REPO/build:/out" -v "$REPO/packaging/arch:/outpkg" \
  -e VERSION="$VERSION" \
  "$IMAGE" bash -euo pipefail -c '
    # patchelf, file, desktop-file-utils and squashfs-tools are what linuxdeploy and
    # appimagetool shell out to; without mksquashfs the AppImage step does nothing.
    pacman -Syu --noconfirm --needed cmake ninja sdl3 glm vulkan-headers vulkan-icd-loader \
        shaderc libvorbis pkgconf curl git fluidsynth patchelf file desktop-file-utils \
        squashfs-tools zsync >/dev/null 2>&1
    # Prove the assumption this whole script rests on before spending ten minutes on it.
    readelf -n /usr/lib/Scrt1.o | grep -q "x86-64-v" && { echo "container glibc is not baseline"; exit 1; }
    useradd -m builder
    # Copy the checkout in and clear anything the host left behind: a stale
    # packaging/arch/src holds a CMake cache full of host paths that makepkg trips on.
    cp -r /src /work
    rm -rf /work/build /work/build-win /work/packaging/arch/src /work/packaging/arch/pkg /work/packaging/arch/*.pkg.tar.zst
    chown -R builder /work

    # The tarball, the deb and the tree the AppImage is made from.
    su builder -c "cmake -S /work -B /work/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DREDLINE_BUILD_TESTS=ON" >/tmp/cfg.log 2>&1 \
      || { tail -30 /tmp/cfg.log; exit 1; }
    su builder -c "cmake --build /work/build" >/tmp/build.log 2>&1 || { tail -40 /tmp/build.log; exit 1; }
    su builder -c "cd /work/build && ctest --output-on-failure" | tail -3
    readelf -n /work/build/redline | grep -A2 "x86 ISA" | head -3
    su builder -c "cd /work/build && cpack -G TGZ" >/tmp/tgz.log 2>&1 || { tail -20 /tmp/tgz.log; exit 1; }
    su builder -c "cd /work/build && cpack -G DEB -D CPACK_DEBIAN_PACKAGE_SHLIBDEPS=OFF -D CPACK_DEBIAN_PACKAGE_ARCHITECTURE=amd64 \
        -D CPACK_DEBIAN_PACKAGE_DEPENDS=\"libsdl3-0, libvorbisfile3, libvulkan1, libcurl4t64 | libcurl4\" \
        -D CPACK_DEBIAN_PACKAGE_MAINTAINER=\"David Janice <djanice1980@gmail.com>\" -D CPACK_DEBIAN_FILE_NAME=DEB-DEFAULT" >/tmp/deb.log 2>&1 \
      || { tail -20 /tmp/deb.log; exit 1; }

    # The Arch package, built by makepkg as a normal user, with the stock flags of
    # this container rather than the ones the host machine would have exported.
    echo "== arch package"
    su builder -c "cd /work/packaging/arch && makepkg -f --noconfirm" >/tmp/pkg.log 2>&1 || { tail -30 /tmp/pkg.log; exit 1; }

    # The AppImage: an installed tree plus the shared libraries linuxdeploy finds.
    echo "== appimage"
    curl -sL -o /tmp/linuxdeploy https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
    chmod +x /tmp/linuxdeploy
    rm -rf /work/build/AppDir
    DESTDIR=/work/build/AppDir cmake --install /work/build --prefix /usr >/dev/null
    chown -R builder /work/build
    su builder -c "cd /work/build && APPIMAGE_EXTRACT_AND_RUN=1 /tmp/linuxdeploy --appdir AppDir --output appimage" >/tmp/appimage.log 2>&1 \
      || { tail -25 /tmp/appimage.log; exit 1; }
    img=$(ls /work/build/*.AppImage 2>/dev/null | head -1)
    [ -n "$img" ] || { echo "linuxdeploy produced no AppImage:"; tail -25 /tmp/appimage.log; exit 1; }
    mv -f "$img" "/work/build/redline-$VERSION-x86_64.AppImage"

    cp "/work/build/redline-$VERSION-Linux.tar.gz" "/work/build/redline_${VERSION}_amd64.deb" \
       "/work/build/redline-$VERSION-x86_64.AppImage" /out/
    cp /work/packaging/arch/redline-*.pkg.tar.zst /outpkg/
    chown "$(stat -c %u /out)":"$(stat -c %g /out)" /out/redline* /outpkg/redline-*.pkg.tar.zst
    echo "== container artefacts"
    for f in "/out/redline-$VERSION-Linux.tar.gz" "/out/redline_${VERSION}_amd64.deb" "/out/redline-$VERSION-x86_64.AppImage"; do
      ls -la "$f"
    done
  '

echo "== ISA check (all four must say baseline only)"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
tar -I zstd -xf "$REPO/packaging/arch/redline-$VERSION-1-x86_64.pkg.tar.zst" -C "$tmp" usr/bin/redline
echo -n "arch package: "; readelf -n "$tmp/usr/bin/redline" | grep -m1 "x86 ISA needed"
tar xzf "$REPO/build/redline-$VERSION-Linux.tar.gz" -C "$tmp" --strip-components=1 --wildcards '*/bin/redline'
echo -n "tarball:      "; readelf -n "$tmp/bin/redline" | grep -m1 "x86 ISA needed"
