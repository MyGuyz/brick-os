#!/bin/sh
# Brick OS YouTube: install or update yt-dlp (aarch64, onedir) and Node.js (which
# solves YouTube's JavaScript challenge) into this pak's bin/. Run by youtube.elf
# (first use, and MENU > Update yt-dlp).
set -eu
PAK=$(cd "$(dirname "$0")" && pwd)
SHARED=/mnt/SDCARD/.system/shared
WGET=$SHARED/bin/wget
UNZIP=$SHARED/bin/7zzs.aarch64
TMP=/mnt/SDCARD/.tmp_youtube
NODE_DIST=https://nodejs.org/dist/latest-v22.x # v22 builds need glibc 2.28, as on the device

[ -f "$SHARED/bin/nx_ca_bundle.sh" ] && . "$SHARED/bin/nx_ca_bundle.sh"
TLS=""
[ -n "${NX_CA_BUNDLE:-}" ] && TLS="--check-certificate=on --ca-certificate=$NX_CA_BUNDLE"
get() { $WGET -q $TLS --tries=3 --timeout=60 -O "$2" "$1"; }

rm -rf "$TMP"
mkdir -p "$TMP" "$PAK/bin"

# yt-dlp: always the latest release
get https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux_aarch64.zip "$TMP/yt.zip"
"$UNZIP" x -y -o"$TMP/yt" "$TMP/yt.zip" >/dev/null
[ -f "$TMP/yt/yt-dlp_linux_aarch64" ] || { echo "unexpected yt-dlp archive layout"; exit 1; }
rm -rf "$PAK/bin/yt-dlp"
mv "$TMP/yt" "$PAK/bin/yt-dlp"
chmod +x "$PAK/bin/yt-dlp/yt-dlp_linux_aarch64"

# Node.js: only when missing (it rarely needs updating for this)
if [ ! -x "$PAK/bin/node" ]; then
	get "$NODE_DIST/SHASUMS256.txt" "$TMP/sums"
	NAME=$(grep -o 'node-v[0-9.]*-linux-arm64.tar.xz' "$TMP/sums" | head -1)
	[ -n "$NAME" ] || { echo "no Node.js arm64 build listed"; exit 1; }
	get "$NODE_DIST/$NAME" "$TMP/$NAME"
	WANT=$(grep " $NAME\$" "$TMP/sums" | cut -d' ' -f1)
	GOT=$(sha256sum "$TMP/$NAME" | cut -d' ' -f1)
	[ "$WANT" = "$GOT" ] || { echo "Node.js checksum mismatch"; exit 1; }
	"$UNZIP" x -y -o"$TMP" "$TMP/$NAME" >/dev/null
	"$UNZIP" x -y -o"$TMP/node" "$TMP/${NAME%.xz}" "${NAME%.tar.xz}/bin/node" >/dev/null
	mv "$TMP/node/${NAME%.tar.xz}/bin/node" "$PAK/bin/node"
	chmod +x "$PAK/bin/node"
fi
rm -rf "$TMP" "$PAK/bin/qjs"
"$PAK/bin/yt-dlp/yt-dlp_linux_aarch64" --version
"$PAK/bin/node" --version
