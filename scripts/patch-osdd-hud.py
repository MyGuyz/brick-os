#!/usr/bin/env python3
"""HUD patch of the TrimUI overlay daemon (trimui_osdd, closed source), Brick Pro build.

Measured 2026-11-13 (scripts/device/hudlat.c, time from writing the toast file to the daemon taking it):
  - a held volume key: every ~1.2 s one step took ~900 ms. While a toast is up the main loop refreshes every
    quick-panel widget once a second (osd_update_widget -> system("<widget>/update.sh") for each), even with the
    panel hidden: a dozen shells forked from a 200 MB process.
  - a first press: 120-230 ms before the pill showed. With nothing on screen the loop sleeps 200 ms a turn.

Patch:
  0x402420  cmp w0,#0          -> bl trimui_osd_is_actived    the widgets are refreshed only while the panel is open
  0x402424  b.le 0x402448      -> cbz w0,0x402448
  0x402538  mov w0,#0xd40      -> mov w0,#0x8234              idle turn 200 ms -> 33 ms (as the active one)
  0x40253c  movk w0,#3,lsl 16  -> nop

Usage: patch_osdd_hud.py <in> <out>   (every original word is checked first; a different build is refused)
"""
import struct
import sys

TEXT_BASE = 0x400000
EDITS = [
    (0x402420, 0x7100001F, 0x94002758),  # cmp w0,#0 -> bl 0x40c180 <trimui_osd_is_actived>
    (0x402424, 0x5400012D, 0x34000120),  # b.le 0x402448 -> cbz w0,0x402448
    (0x402538, 0x5281A800, 0x52904680),  # mov w0,#0xd40 -> mov w0,#0x8234 (33332 us)
    (0x40253C, 0x72A00060, 0xD503201F),  # movk w0,#0x3,lsl #16 -> nop
]
# what must surround them (the call target and the sleep that follows)
CHECKS = [
    (0x40C180, 0x900000C0),  # trimui_osd_is_actived: adrp x0, ...
    (0x40C188, 0x7100001F),
    (0x402540, 0x97FFFE20),  # bl usleep
    (0x402448, 0x52800001),  # mov w1,#0 (after the widget loop)
]


def word(b, a):
    return struct.unpack_from('<I', b, a - TEXT_BASE)[0]


def main():
    src, dst = sys.argv[1], sys.argv[2]
    b = bytearray(open(src, 'rb').read())
    for a, w in CHECKS:
        if word(b, a) != w:
            sys.exit('%s: 0x%x is %08x, not %08x: another build, not patched' % (src, a, word(b, a), w))
    done = 0
    for a, old, new in EDITS:
        w = word(b, a)
        if w == new:
            done += 1
            continue
        if w != old:
            sys.exit('%s: 0x%x is %08x, not %08x: another build, not patched' % (src, a, w, old))
        struct.pack_into('<I', b, a - TEXT_BASE, new)
    if done == len(EDITS):
        print('%s: already patched' % src)
    open(dst, 'wb').write(bytes(b))
    print('%s -> %s: ok' % (src, dst))


main()
