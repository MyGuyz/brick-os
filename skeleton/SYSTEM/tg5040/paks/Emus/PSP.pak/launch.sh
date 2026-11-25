#!/bin/sh

EMU_EXE=ppsspp
CORES_PATH=$(dirname "$0")

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"

# Games that need more speed than the core gives play in standalone PPSSPP: standalone/games.txt, by the disc ID (in the
# first sectors of an ISO) or, for a compressed image, by the file's name
SA="$CORES_PATH/standalone"
if [ -x "$SA/PPSSPPSDL" ]; then
	ID=$(head -c 262144 "$ROM" | grep -aoE '[A-Z]{4}-[0-9]{5}' | head -n 1)
	case "$(basename "$ROM")" in
	"God of War - Chains of Olympus"*) ID=${ID:-UCUS-98653} ;;
	esac
	[ -n "$ID" ] && grep -qxF "$ID" "$SA/games.txt" && exec "$SA/run.sh" "$ROM"
fi

HOME="$USERDATA_PATH"
cd "$HOME"

# PPSSPP's runtime assets (fonts, flash0, vfpu tables, PPGe atlas) ship in the
# pak and update with the system; Bios/PSP stays the player's.
export NX_PPSSPP_ASSETS="$CORES_PATH/PPSSPP"

# Single cluster: cpu0-3 (Cortex-A53). minarch_cpu_speed = Performance sets the clock.
echo 1 >/sys/devices/system/cpu/cpu1/online 2>/dev/null
echo 1 >/sys/devices/system/cpu/cpu2/online 2>/dev/null
echo 1 >/sys/devices/system/cpu/cpu3/online 2>/dev/null

minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
