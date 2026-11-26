#!/bin/sh

cd "$(dirname "$0")"

export SDL_NOMOUSE=1
./wifitransfer.elf &> "$LOGS_PATH/wifi-transfer.txt"
