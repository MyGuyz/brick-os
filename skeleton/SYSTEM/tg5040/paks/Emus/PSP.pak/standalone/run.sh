#!/bin/sh
# Standalone PPSSPP (Vulkan, built by workspace/all/prebuilts/ppsspp.sh) for the PSP games listed in games.txt.
# Called by PSP.pak/launch.sh with the ROM path; every other game plays through minarch.
D="$(cd "$(dirname "$0")" && pwd)"
ROM="$1"
CONF="$SHARED_USERDATA_PATH/PSP-ppsspp-standalone"
P="$CONF/.config/ppsspp/PSP"
LOG="$LOGS_PATH/PSP-standalone.txt"
exec >"$LOG" 2>&1
echo "$0 $ROM"

mkdir -p "$P/SYSTEM" "$P/SAVEDATA" "$P/PPSSPP_STATE" "$SDCARD_PATH/Saves/PSP/SAVEDATA"
[ -f "$P/SYSTEM/ppsspp.ini" ] || cp "$D/ppsspp.ini" "$P/SYSTEM/ppsspp.ini"
[ -f "$P/SYSTEM/controls.ini" ] || cp "$D/controls.ini" "$P/SYSTEM/controls.ini" 2>/dev/null
cp "$D/compat.ini" "$P/SYSTEM/compat.ini"   # ours: God of War at 30 fps (updates with the system)
# nothing phones home
sed -i 's/^CheckForNewVersion = True/CheckForNewVersion = False/; s/^DiscordPresence = True/DiscordPresence = False/' "$P/SYSTEM/ppsspp.ini"
# Same memory-stick saves as the minarch core: a game saved in one opens in the other
mount -o bind "$SDCARD_PATH/Saves/PSP/SAVEDATA" "$P/SAVEDATA"

cleanup() {
	echo 1 >/sys/devices/system/cpu/cpu3/online
	rm -f /tmp/stay_awake
	killall sleepmon.elf 2>/dev/null
	umount "$P/SAVEDATA" 2>/dev/null
}
trap cleanup EXIT INT TERM HUP QUIT
echo 1 >/tmp/stay_awake

# As the stock firmware runs it: performance governor, three cores
echo performance >/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq >/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
echo 1416000 >/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
for c in 1 2; do echo 1 >/sys/devices/system/cpu/cpu$c/online; done
echo 0 >/sys/devices/system/cpu/cpu3/online

export HOME="$CONF"
export NX_PPSSPP_ASSETS="$(dirname "$D")/PPSSPP"   # the core's assets, shared
export LD_LIBRARY_PATH="/usr/trimui/lib:/usr/lib:$D:$LD_LIBRARY_PATH"   # the system's own last: syncsettings, sleepmon
export SDL_GAMECONTROLLERCONFIG_FILE="$D/assets/gamecontrollerdb.txt"
cd "$D"
# PPSSPP counts its running copies in shared memory; a copy that was killed never counts itself out,
# and the next one then thinks it is a second copy and plays no sound. Only one ever runs here.
rm -f /dev/shm/PPSSPP_ID
(sleep 5; syncsettings.elf) &
sleepmon.elf &
./PPSSPPSDL --fullscreen --pause-menu-exit $PPSSPP_ARGS "$ROM"
echo "exit $?"
