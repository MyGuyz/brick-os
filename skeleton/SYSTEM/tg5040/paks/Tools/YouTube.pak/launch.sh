#!/bin/sh
# Brick OS YouTube: YTC (github.com/MyGuyz/ytc, Thai build). The app lives in this pak;
# everything it saves (settings, favorites, history, downloads) lives in .userdata so
# a system update keeps it.
cd "$(dirname "$0")/ytc"
DATA=/mnt/SDCARD/.userdata/shared/ytc
mkdir -p "$DATA/config"
rm -rf /mnt/SDCARD/.userdata/shared/youtube   # Brick OS 0.3-0.4's yt-dlp app and its 200 MB engine
cp -f config/clients.json "$DATA/config/clients.json"     # client fingerprints ship with the app
[ -f "$DATA/config/settings.json" ] || cp -f config/settings.json "$DATA/config/settings.json"
export YTC_CONFIG="$DATA/config/clients.json"
export YTC_MAXHEIGHT=480
export HOME="$DATA"
export SDL_NOMOUSE=1
export LD_LIBRARY_PATH="$PWD/libs.aarch64:$LD_LIBRARY_PATH"
[ -f /mnt/SDCARD/.system/shared/bin/nx_ca_bundle.sh ] && . /mnt/SDCARD/.system/shared/bin/nx_ca_bundle.sh

./ytc.aarch64 > "${LOGS_PATH:-/tmp}/youtube.txt" 2>&1
