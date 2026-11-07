#!/bin/sh
# Brick OS YouTube: install or update yt-dlp (aarch64, onedir) and QuickJS into
# this pak's bin/. Run by youtube.elf (Install engine / MENU > Update yt-dlp).
set -eu
PAK=$(cd "$(dirname "$0")" && pwd)
SHARED=/mnt/SDCARD/.system/shared
WGET=$SHARED/bin/wget
UNZIP=$SHARED/bin/7zzs.aarch64
TMP=/mnt/SDCARD/.tmp_youtube

[ -f "$SHARED/bin/nx_ca_bundle.sh" ] && . "$SHARED/bin/nx_ca_bundle.sh"
TLS=""
[ -n "${NX_CA_BUNDLE:-}" ] && TLS="--check-certificate=on --ca-certificate=$NX_CA_BUNDLE"

rm -rf "$TMP"
mkdir -p "$TMP" "$PAK/bin"
$WGET -q $TLS --tries=3 --timeout=60 -O "$TMP/yt.zip" \
	https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux_aarch64.zip
$WGET -q $TLS --tries=3 --timeout=60 -O "$TMP/qjs" \
	https://github.com/quickjs-ng/quickjs/releases/latest/download/qjs-linux-aarch64
"$UNZIP" x -y -o"$TMP/yt" "$TMP/yt.zip" >/dev/null
[ -f "$TMP/yt/yt-dlp_linux_aarch64" ] || { echo "unexpected yt-dlp archive layout"; exit 1; }

rm -rf "$PAK/bin/yt-dlp"
mv "$TMP/yt" "$PAK/bin/yt-dlp"
mv "$TMP/qjs" "$PAK/bin/qjs"
chmod +x "$PAK/bin/yt-dlp/yt-dlp_linux_aarch64" "$PAK/bin/qjs"
rm -rf "$TMP"
"$PAK/bin/yt-dlp/yt-dlp_linux_aarch64" --version
