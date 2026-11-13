#!/bin/sh
# Brick OS level HUD (DESIGN.md 5.9): a ready-made picture of the level, shown as a toast's background.
# keymon writes the same message itself; this script is for anything else that wants to show a level.
PIC=$(printf '/usr/trimui/osd/hud/bri_%02d.png' "$1")
printf '{"type":"default","id":"com.trimui.osd.msg.volumeglobal","duration":1200,"size":0,"x":292,"y":76,"w":440,"h":76,"message":" ","font":"","bg":"%s","icon":"","fontsize":24,"fontcolor":"FFFFFFFF"}\n' "$PIC" > /tmp/trimui_osd/osd_toast_msg
