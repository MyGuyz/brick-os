#!/bin/sh
# Files: the Brick OS file manager (workspace/all/files, DESIGN 7.9)
cd "$(dirname "$0")"
echo 1008000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null
./files.elf &> "$LOGS_PATH/files.txt"
