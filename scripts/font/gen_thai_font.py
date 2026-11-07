"""Build the Brick OS Thai UI font from IBM Plex Sans Thai.

SDL_ttf on the device has no HarfBuzz, so it cannot position Thai marks. This
script asks HarfBuzz (at build time) which glyph and offset each mark gets in
the contexts that need it, and bakes each answer into a Private Use Area glyph.
The device then only swaps code points (workspace/all/common/text_thai.c),
which plain SDL_ttf renders as HarfBuzz would.

The plain marks are moved too: HarfBuzz anchors every mark a little left of
where plain left-to-right drawing puts it, so each default mark glyph is
shifted by its offset after ก.

PUA map (shared with text_thai.c; keep both in sync):
  F700 ฐ without descender        F70F ญ without descender
  F701-F704 ิ ี ึ ื after ป ฝ ฟ ฬ
  F705-F709 ่ ้ ๊ ๋ ์ after ป ฝ ฟ ฬ (no upper vowel)
  F70A-F70E ่ ้ ๊ ๋ ์ above an upper vowel
  F710 ั  F711 ํ  F712 ็  after ป ฝ ฟ ฬ
  F713-F717 ่ ้ ๊ ๋ ์ above an upper vowel, after ป ฝ ฟ ฬ
  F718-F71A ุ ู ฺ under ฎ ฏ          F71B ฎ  F71C ฏ  short forms (with a lower vowel)
  F720-F724 ่ ้ ๊ ๋ ์ above ำ          F725-F729 the same after ป ฝ ฟ ฬ
After ป ฝ ฟ ฬ, text_thai.c writes ำ as F711 + า so its circle moves too.

The family is renamed: IBM Plex is OFL with the Reserved Font Name "Plex".

Usage: python gen_thai_font.py <in.ttf> <out.ttf>
"""
import sys

import uharfbuzz as hb
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

TONES = [0x0E48, 0x0E49, 0x0E4A, 0x0E4B, 0x0E4C]
UPPER = [0x0E34, 0x0E35, 0x0E36, 0x0E37]
LOWER = [0x0E38, 0x0E39, 0x0E3A]
MARKS = [0x0E31, 0x0E34, 0x0E35, 0x0E36, 0x0E37, 0x0E38, 0x0E39, 0x0E3A,
         0x0E47, 0x0E48, 0x0E49, 0x0E4A, 0x0E4B, 0x0E4C, 0x0E4D]
TALL, DESC, DESC2, PLAIN = "ป", "ฎ", "ฏ", "ก"  # ป ฎ ฏ ก
FAMILY = "BrickOS Thai"


class Shaper:
    def __init__(self, path, ttf):
        self.font = hb.Font(hb.Face(hb.Blob.from_file_path(path)))
        self.order = ttf.getGlyphOrder()
        self.hmtx = ttf["hmtx"]

    def glyph(self, text, index):
        """(glyph name, dx, dy) HarfBuzz gives glyph `index` of `text`, the
        offset measured against plain left-to-right placement."""
        buf = hb.Buffer()
        buf.add_str(text)
        buf.guess_segment_properties()
        hb.shape(self.font, buf, {})
        pen_hb = pen_plain = 0
        for i, (info, p) in enumerate(zip(buf.glyph_infos, buf.glyph_positions)):
            name = self.order[info.codepoint]
            if i == index:
                return name, (pen_hb + p.x_offset) - pen_plain, p.y_offset
            pen_hb += p.x_advance
            pen_plain += self.hmtx[name][0]
        raise ValueError(text)


def outline(ttf, name):
    rec = DecomposingRecordingPen(ttf.getGlyphSet())
    ttf.getGlyphSet()[name].draw(rec)
    return rec.value


def put_glyph(ttf, name, ops, dx, dy, adv):
    pen = TTGlyphPen(None)
    for op, args in ops:
        getattr(pen, op)(*[(x + dx, y + dy) for (x, y) in args])
    glyph = pen.glyph()
    glyph.recalcBounds(ttf["glyf"])
    ttf["glyf"][name] = glyph
    ttf["hmtx"][name] = (adv, getattr(glyph, "xMin", 0))


def map_cp(ttf, cp, name):
    for table in ttf["cmap"].tables:
        if table.isUnicode():
            table.cmap[cp] = name


def main(src, dst):
    ttf = TTFont(src)
    sh = Shaper(src, ttf)
    cmap = ttf.getBestCmap()
    originals = {}  # every outline is read before any glyph is moved

    def src_outline(name):
        if name not in originals:
            originals[name] = outline(ttf, name)
        return originals[name]

    plan = []  # (pua, context text, index of the glyph to copy)
    for i, v in enumerate(UPPER):
        plan.append((0xF701 + i, TALL + chr(v), 1))
    for i, t in enumerate(TONES):
        plan.append((0xF705 + i, TALL + chr(t), 1))
        plan.append((0xF70A + i, PLAIN + "ิ" + chr(t), 2))
        plan.append((0xF713 + i, TALL + "ิ" + chr(t), 2))
        plan.append((0xF720 + i, "น" + chr(t) + "ำ", 2))  # HarfBuzz splits ำ: น ํ tone า
        plan.append((0xF725 + i, TALL + chr(t) + "ำ", 2))
    plan.append((0xF710, TALL + "ั", 1))
    plan.append((0xF711, TALL + "ํ", 1))
    plan.append((0xF712, TALL + "็", 1))
    for i, v in enumerate(LOWER):
        plan.append((0xF718 + i, DESC + chr(v), 1))
    for pua, cons in ((0xF70F, "ญ"), (0xF700, "ฐ"), (0xF71B, DESC), (0xF71C, DESC2)):
        plan.append((pua, cons + "ุ", 0))

    made = []
    for pua, text, idx in plan:
        name, dx, dy = sh.glyph(text, idx)
        made.append((pua, name, dx, dy))
        print("U+%04X %-4s %-18s dx=%4d dy=%4d" % (pua, text, name, dx, dy))
    defaults = []
    for cp in MARKS:
        name, dx, dy = sh.glyph(PLAIN + chr(cp), 1)
        defaults.append((cmap[cp], dx, dy))
        print("default %s dx=%d dy=%d" % (cmap[cp], dx, dy))

    for _, name, _, _ in made:
        src_outline(name)
    for name, _, _ in defaults:
        src_outline(name)
    for pua, name, dx, dy in made:
        new = "pua%04X" % pua
        put_glyph(ttf, new, src_outline(name), dx, dy, ttf["hmtx"][name][0])
        map_cp(ttf, pua, new)
    for name, dx, dy in defaults:
        put_glyph(ttf, name, src_outline(name), dx, dy, ttf["hmtx"][name][0])

    for rec in ttf["name"].names:
        if rec.nameID in (1, 16):
            rec.string = FAMILY
        elif rec.nameID == 4:
            rec.string = FAMILY + " Medium"
        elif rec.nameID == 6:
            rec.string = "BrickOSThai-Medium"
        elif rec.nameID == 3:
            rec.string = "BrickOSThai-Medium;based on IBM Plex Sans Thai"
    ttf.save(dst)
    print("wrote", dst)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
