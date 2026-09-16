#!/usr/bin/env bash
# Cross-compile a Windows x64 build of REDLINE from Linux using Docker and the
# Fedora mingw-w64 packages (gcc, SDL3, libvorbis, Vulkan loader). Produces
#   build-win/redline-<version>-win64/   redline.exe + the DLLs it needs (+ the unit-test
#                                        exes only with REDLINE_STAGE_TESTS=1)
#   build-win/redline-<version>-win64.zip
# No Doom data is included; the game asks for doom.wad on first launch.
#
# Usage: packaging/windows/cross-build.sh            (from anywhere in the repo)
# Needs: docker (the user must be able to run containers), network for the
# image and packages on the first run. glm is taken from the host's
# /usr/include/glm (header-only) or downloaded if absent.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="$(grep -m1 'project(redline VERSION' "$REPO/CMakeLists.txt" | sed -E 's/.*VERSION ([0-9.]+).*/\1/')"
OUT="$REPO/build-win"
IMAGE="fedora:42"
mkdir -p "$OUT"

GLM_ARGS=()
if [ -d /usr/include/glm ]; then GLM_ARGS=(-v /usr/include/glm:/glm/glm:ro); fi

docker run --rm \
  -v "$REPO:/src:ro" -v "$OUT:/out" "${GLM_ARGS[@]}" \
  -e VERSION="$VERSION" -e REDLINE_STAGE_TESTS="${REDLINE_STAGE_TESTS:-0}" \
  "$IMAGE" bash -euo pipefail -c '
    dnf -q -y install mingw64-gcc-c++ mingw64-SDL3 mingw64-libvorbis mingw64-libogg \
        mingw64-vulkan-headers mingw64-vulkan-loader mingw64-winpthreads-static \
        glslc cmake ninja-build pkgconf-pkg-config git >/dev/null
    SYSROOT=/usr/x86_64-w64-mingw32/sys-root/mingw
    # glm: header-only. Use the host copy when mounted, otherwise fetch 1.0.1.
    if [ ! -d /glm/glm ]; then
      mkdir -p /glm && curl -sL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz | tar xz -C /glm --strip-components=1 glm-1.0.1/glm
    fi
    # A minimal glm CMake package so find_package(glm CONFIG) resolves to the headers.
    mkdir -p /glm/cmake/glm
    cat > /glm/cmake/glm/glmConfig.cmake <<EOF
add_library(glm::glm INTERFACE IMPORTED)
set_target_properties(glm::glm PROPERTIES INTERFACE_INCLUDE_DIRECTORIES /glm)
EOF
    cat > /tmp/toolchain.cmake <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH $SYSROOT)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic")
EOF
    export PKG_CONFIG_LIBDIR=$SYSROOT/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=/usr/x86_64-w64-mingw32/sys-root
    cp -r /src /work && cd /work && rm -rf build-win
    cmake -S . -B /tmp/bw -G Ninja -DCMAKE_TOOLCHAIN_FILE=/tmp/toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
        -DREDLINE_BUILD_TESTS=ON -Dglm_DIR=/glm/cmake/glm -DCMAKE_INSTALL_PREFIX=/tmp/stage >/tmp/configure.log 2>&1 || { tail -40 /tmp/configure.log; exit 1; }
    grep -E "FluidSynth|vorbis|Vulkan|SDL3" /tmp/configure.log || true
    cmake --build /tmp/bw 2>&1 | grep -E "error|warning: unused|FAILED|Linking" | head -40 || true
    test -f /tmp/bw/redline.exe || { echo "BUILD FAILED"; cmake --build /tmp/bw 2>&1 | grep -B2 -A6 "error" | head -80; exit 1; }
    # Stage: exe, the tests, and every mingw DLL the exe pulls in (transitively), except the Vulkan loader,
    # which on Windows comes from the graphics driver in System32.
    DEST=/out/redline-$VERSION-win64
    rm -rf "$DEST" && mkdir -p "$DEST"
    cp /tmp/bw/redline.exe "$DEST"/
    # The unit-test executables are built (so a broken test build fails here) but only
    # staged when asked for, e.g. to run them under Wine: REDLINE_STAGE_TESTS=1.
    if [ "${REDLINE_STAGE_TESTS:-0}" = "1" ]; then cp /tmp/bw/*_test.exe "$DEST"/ 2>/dev/null || true; fi
    cp /work/README.md /work/LICENSE "$DEST"/
    cp -r /work/assets/voxel-doom "$DEST"/voxels     # the MIT-licensed Voxel Doom pack, found next to the exe
    cp -r /work/docs "$DEST"/docs && rm -f "$DEST"/docs/*.pptx
    queue=$(ls "$DEST"/*.exe); seen=""
    while [ -n "$queue" ]; do
      next=""
      for f in $queue; do
        for d in $(x86_64-w64-mingw32-objdump -p "$f" | awk "/DLL Name/ {print \$3}"); do
          case " $seen " in *" $d "*) continue;; esac
          seen="$seen $d"
          [ "$d" = "vulkan-1.dll" ] && continue
          if [ -f "$SYSROOT/bin/$d" ]; then cp "$SYSROOT/bin/$d" "$DEST"/; next="$next $DEST/$d"; fi
        done
      done
      queue="$next"
    done
    ls -la "$DEST"
    cd /out && rm -f redline-$VERSION-win64.zip && (cd "$DEST"/.. && python3 -c "
import shutil; shutil.make_archive(\"redline-$VERSION-win64\", \"zip\", \".\", \"redline-$VERSION-win64\")")
    ls -la /out/redline-$VERSION-win64.zip
  '
echo "Done: $OUT/redline-$VERSION-win64.zip"
