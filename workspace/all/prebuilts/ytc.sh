#!/bin/bash
#
# Build the YouTube app (YTC with Brick OS's Thai support) that ships in
# SYSTEM/<platform>/paks/Tools/YouTube.pak/ytc: the app from MyGuyz/ytc at a pinned
# commit, and its media stack (libmpv 0.36 + FFmpeg 6.1, curl/mbedTLS, libass)
# from pinned, sha256-checked sources. The cross-build scripts live in that repo's
# tools/brickos/. tg5040 only.
#
# Runs INSIDE the toolchain container (make build-prebuilt PLATFORM=tg5040 PREBUILT=ytc).
# Output: all/prebuilts/output/tg5040/SYSTEM/tg5040/paks/Tools/YouTube.pak/ytc
#
set -e

if [ "$PLATFORM" != "tg5040" ]; then
    echo "ytc is built for tg5040 only (got '${PLATFORM}')" >&2
    exit 1
fi

YTC_REPO=https://github.com/MyGuyz/ytc
YTC_COMMIT=926ca42d50f93c739bf0d14c2a56dd9685570b7a

PREBUILTS_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK="$PREBUILTS_DIR/work/ytc"
OUT="$PREBUILTS_DIR/output/$PLATFORM/SYSTEM/$PLATFORM/paks/Tools/YouTube.pak/ytc"
mkdir -p "$WORK/src"

fetch() { # URL FILE SHA256
    [ -f "$WORK/src/$2" ] && echo "$3  $WORK/src/$2" | sha256sum -c --quiet - && return
    wget -q --tries=5 --timeout=60 "$1" -O "$WORK/src/$2"
    echo "$3  $WORK/src/$2" | sha256sum -c --quiet - || { echo "sha256 mismatch for $1" >&2; rm -f "$WORK/src/$2"; exit 1; }
}
fetch https://zlib.net/fossils/zlib-1.3.1.tar.gz zlib.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
fetch https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2 mbedtls.tar.bz2 8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
fetch https://curl.se/download/curl-8.11.0.tar.gz curl.tar.gz 264537d90e58d2b09dddc50944baf3c38e7089151c8986715e2aaeaaf2b8118f
fetch https://github.com/fribidi/fribidi/releases/download/v1.0.13/fribidi-1.0.13.tar.xz fribidi.tar.xz 7fa16c80c81bd622f7b198d31356da139cc318a63fc7761217af4130903f54a2
fetch https://github.com/libass/libass/releases/download/0.14.0/libass-0.14.0.tar.xz libass.tar.xz 881f2382af48aead75b7a0e02e65d88c5ebd369fe46bc77d9270a94aa8fd38a2
fetch https://ffmpeg.org/releases/ffmpeg-6.1.tar.xz ffmpeg.tar.xz 488c76e57dd9b3bee901f71d5c95eaf1db4a5a31fe46a28654e837144207c270
fetch https://github.com/mpv-player/mpv/archive/refs/tags/v0.36.0.tar.gz mpv.tar.gz 29abc44f8ebee013bb2f9fe14d80b30db19b534c679056e4851ceadf5a5e8bf6
fetch https://github.com/mesonbuild/meson/releases/download/1.5.2/meson-1.5.2.tar.gz meson.tar.gz f955e09ab0d71ef180ae85df65991d58ed8430323de7d77a37e11c9ea630910b
# the image has no bzip2 tool: recompress with Python's bz2 module
[ -s "$WORK/src/mbedtls.tar.gz" ] || python3 -c "import bz2,gzip,shutil; shutil.copyfileobj(bz2.open('$WORK/src/mbedtls.tar.bz2'), gzip.open('$WORK/src/mbedtls.tar.gz','wb'))"

# YTC at the pinned commit
if [ "$(git -C "$WORK/ytc" rev-parse HEAD 2>/dev/null)" != "$YTC_COMMIT" ]; then
    rm -rf "$WORK/ytc"
    git clone -q "$YTC_REPO" "$WORK/ytc"
    git -C "$WORK/ytc" checkout -q "$YTC_COMMIT"
fi

# tools/brickos expects /deps (sources, work, stamps) and /src (the YTC tree)
ln -sfn "$WORK" /deps
ln -sfn "$WORK/ytc" /src
mkdir -p "$SYSROOT/opt/ytc"
rm -f "$WORK/stamps/"*   # the deps install into this container's sysroot: build them every run
bash "$WORK/ytc/tools/brickos/build-deps.sh"
bash "$WORK/ytc/tools/brickos/build-ytc.sh"

rm -rf "$OUT"
mkdir -p "$(dirname "$OUT")"
cp -a "$WORK/ytc/dist/ytc" "$OUT"
echo "ytc -> $OUT"
