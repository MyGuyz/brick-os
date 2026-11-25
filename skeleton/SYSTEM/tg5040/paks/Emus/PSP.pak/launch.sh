#!/bin/sh

# Every PSP game plays in standalone PPSSPP (Vulkan), with its own menu and settings (DESIGN.md)
exec "$(dirname "$0")/standalone/run.sh" "$1"
