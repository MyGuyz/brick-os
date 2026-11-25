#!/bin/bash
#
# Build standalone PPSSPP (SDL, Vulkan through VK_KHR_display) that ships in
# SYSTEM/tg5040/paks/Emus/PSP.pak/standalone for the PSP games too heavy for
# the libretro core (games.txt there). PPSSPP v1.20.4 at a pinned commit +
# ppsspp/brick_patch.py, with the profile of God of War's opening fight
# (ppsspp/gow-profile.tar.xz, gcc 8 .gcda files). tg5040 only.
#
# Runs INSIDE the toolchain container (make build-prebuilt PLATFORM=tg5040 PREBUILT=ppsspp).
#
set -eo pipefail

if [ "$PLATFORM" != "tg5040" ]; then
    echo "standalone ppsspp is built for tg5040 only (got '${PLATFORM}')" >&2
    exit 1
fi

PPSSPP_REPO=https://github.com/hrydgard/ppsspp
PPSSPP_TAG=v1.20.4
PPSSPP_COMMIT=fa50bb1976065c4f8b1b47af227d367fe9771555

PREBUILTS_DIR="$(cd "$(dirname "$0")" && pwd)"
IN="$PREBUILTS_DIR/ppsspp"
WORK="$PREBUILTS_DIR/work/ppsspp"
OUT="$PREBUILTS_DIR/output/$PLATFORM/SYSTEM/$PLATFORM/paks/Emus/PSP.pak/standalone"
HOST=aarch64-nextui-linux-gnu
SYSROOT=${SYSROOT:-/opt/$HOST/$HOST/libc}
git config --global --add safe.directory '*'

if [ "$(git -C "$WORK" rev-parse HEAD 2>/dev/null)" != "$PPSSPP_COMMIT" ]; then
    rm -rf "$WORK"
    git clone -q --depth 1 --branch "$PPSSPP_TAG" "$PPSSPP_REPO" "$WORK"
    [ "$(git -C "$WORK" rev-parse HEAD)" = "$PPSSPP_COMMIT" ] || { echo "$PPSSPP_TAG is not $PPSSPP_COMMIT" >&2; exit 1; }
fi
git -C "$WORK" submodule update -q --init --recursive --depth 1
git -C "$WORK" checkout -q -- .
cd "$WORK"
python3 "$IN/brick_patch.py"

ln -sfn "$WORK" /src   # the profile was made in /src/build-pgo
cd /src
rm -rf build-pgo && mkdir build-pgo
tar xJf "$IN/gow-profile.tar.xz" -C build-pgo
cat > /tmp/tc.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER $HOST-gcc)
set(CMAKE_CXX_COMPILER $HOST-g++)
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
F="-mcpu=cortex-a53 -fprofile-use -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch"
$HOST-gcc -O2 -mcpu=cortex-a53 -c "$IN/aarch64_atomics.c" -o build-pgo/aarch64_atomics.o
cmake -S . -B build-pgo -DCMAKE_TOOLCHAIN_FILE=/tmp/tc.cmake -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_FLAGS=$F" "-DCMAKE_CXX_FLAGS=$F" \
    -DUSING_GLES2=ON -DUSING_EGL=OFF -DVULKAN=ON -DUSE_VULKAN_DISPLAY_KHR=ON -DUSING_X11_VULKAN=OFF \
    -DUSE_WAYLAND_WSI=OFF -DUSE_DISCORD=OFF -DUNITTEST=OFF -DHEADLESS=OFF -DUSE_SYSTEM_FFMPEG=OFF \
    -DUSE_SYSTEM_LIBZIP=OFF -DUSE_SYSTEM_LIBPNG=OFF -DUSE_SYSTEM_ZSTD=OFF -DUSE_SYSTEM_SNAPPY=OFF \
    -DUSE_SYSTEM_MINIUPNPC=OFF -DUSE_SYSTEM_LIBSDL2=ON -DUSING_QT_UI=OFF -DLIBRETRO=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc -Wl,--allow-shlib-undefined" \
    -DCMAKE_CXX_STANDARD_LIBRARIES="/src/build-pgo/aarch64_atomics.o" > /tmp/cmake.log 2>&1 || { tail -30 /tmp/cmake.log; exit 1; }
cmake --build build-pgo -j"$(nproc)" --target PPSSPPSDL > /tmp/make.log 2>&1 || { grep -E "error|Error|undefined" /tmp/make.log | head -20; exit 1; }

rm -rf "$OUT" && mkdir -p "$OUT/assets"
$HOST-strip -o "$OUT/PPSSPPSDL" build-pgo/PPSSPPSDL
# what the core's own assets (../PPSSPP) do not have
cp -R assets/ui_images assets/themes assets/gamecontrollerdb.txt assets/sfx_*.wav "$OUT/assets/"
ls -la "$OUT"
