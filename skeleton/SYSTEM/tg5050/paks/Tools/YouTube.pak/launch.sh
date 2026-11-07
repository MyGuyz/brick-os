#!/bin/sh
cd "$(dirname "$0")"

export SDL_NOMOUSE=1
export HOME=/mnt/SDCARD
[ -f /mnt/SDCARD/.system/shared/bin/nx_ca_bundle.sh ] && . /mnt/SDCARD/.system/shared/bin/nx_ca_bundle.sh

./youtube.elf > "$LOGS_PATH/youtube.txt" 2>&1
