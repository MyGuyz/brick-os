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
export YTC_QUALITY_CEIL=720    # the A133P decodes in software: 1080p drops frames
export YTC_VIEW=4              # TV view: icon rail + rows of tiles
export HOME="$DATA"
export SDL_NOMOUSE=1
export LD_LIBRARY_PATH="$PWD/libs.aarch64:$LD_LIBRARY_PATH"
[ -f /mnt/SDCARD/.system/shared/bin/nx_ca_bundle.sh ] && . /mnt/SDCARD/.system/shared/bin/nx_ca_bundle.sh

# Face buttons by label, as in the rest of the OS (Settings > System > Button layout).
# The Brick's pad reports as an Xbox 360 controller, which SDL maps by position
# (south = A), so on the Nintendo-labelled Brick A and B came out swapped.
NX_BUTTON_LAYOUT=nintendo
[ -f "$SYSTEM_PATH/bin/nx_button_layout.sh" ] && . "$SYSTEM_PATH/bin/nx_button_layout.sh"
if [ "$NX_BUTTON_LAYOUT" = xbox ]; then face="a:b0,b:b1,x:b2,y:b3"; else face="a:b1,b:b0,x:b3,y:b2"; fi
echo "030000005e0400008e02000014010000,Brick,$face,back:b6,start:b7,guide:b8,leftshoulder:b4,rightshoulder:b5,leftstick:b9,rightstick:b10,lefttrigger:a2,righttrigger:a5,leftx:a0,lefty:a1,rightx:a3,righty:a4,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,platform:Linux," > "$DATA/config/gamepad.txt"
export YTC_GAMEPADDB_LOCAL="$DATA/config/gamepad.txt"

# Power button: tap = sleep, or screen off with the sound still playing while a video
# plays (sleepmon reads YTC's media flag); hold = power off.
export YTC_MEDIA_FLAG=/tmp/bos_media_playing YTC_SCREEN_OFF_FLAG=/tmp/bos_screen_off
rm -f "$YTC_MEDIA_FLAG" "$YTC_SCREEN_OFF_FLAG"
sleepmon.elf &

./ytc.aarch64 > "${LOGS_PATH:-/tmp}/youtube.txt" 2>&1

killall sleepmon.elf 2>/dev/null
rm -f "$YTC_MEDIA_FLAG" "$YTC_SCREEN_OFF_FLAG"
