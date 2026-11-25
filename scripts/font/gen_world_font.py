"""Build res/font1-world.ttf: every glyph the UI fonts (Inter, BrickOS Thai) and font1.ttf (MiSans: Chinese, Japanese)
lack. Hangul and CJK odds from Noto Sans CJK KR Medium, other scripts from Noto Sans, symbols and monochrome emoji from
Noto Symbols / Math and Symbola. No shaping tables: the UI draws plain glyphs.
SDL_ttf 2.0.13 only draws the BMP: U+1F000-1FAFF is also at U+E000-EAFF and U+1D400-1D7FF at U+EB00-EEFF.
Usage: gen_world_font.py <dir with the sources> <font1.ttf> <out.ttf>"""
import sys, os
from fontTools.ttLib import TTFont, TTCollection
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.transformPen import TransformPen

src, font1, out = sys.argv[1:4]
UPM = 1000
have = set(TTFont(font1, lazy=True).getBestCmap())


def R(*rs):
    s = set()
    for a, b in rs:
        s.update(range(a, b + 1))
    return s


cjk = [f for f in TTCollection(os.path.join(src, 'NotoSansCJK-Medium.ttc')).fonts
       if f['name'].getDebugName(1).startswith('Noto Sans CJK KR')][0]
N = lambda n: TTFont(os.path.join(src, 'NotoSans%s-Regular.ttf' % n))
ANY = R((0x80, 0x1FFFF))
WANT = [  # (font, code points), first wins
    (cjk, R((0x1100, 0x11FF), (0x3000, 0x33FF), (0xA960, 0xA97F), (0xAC00, 0xD7FF), (0xFE30, 0xFE4F), (0xFF00, 0xFFEF),
            (0x4E00, 0x9FFF), (0xF900, 0xFAFF), (0x2E80, 0x2FDF))),
    (N(''), R((0xA0, 0x58F), (0x1D00, 0x1FFF), (0x2000, 0x2BFF), (0x2C60, 0x2C7F), (0xA640, 0xA7FF), (0xAB30, 0xAB6F))),
] + [(N(n), ANY) for n in (
    'Hebrew Arabic Armenian Georgian Devanagari Bengali Gurmukhi Gujarati Oriya Tamil Telugu Kannada Malayalam Sinhala '
    'Lao Khmer Myanmar Ethiopic Cherokee CanadianAboriginal Mongolian Syriac Thaana Tifinagh Javanese Sundanese Balinese '
    'Math Symbols Symbols2').split()] + [
    (TTFont(os.path.join(src, 'Symbola_hint.ttf')), R((0x2000, 0x2BFF), (0x1D400, 0x1D7FF), (0x1F000, 0x1FAFF))),
]
ks = set()      # the rare hanja of KS X 1001 and Big5 that MiSans lacks; not every ideograph of Noto CJK
for enc in ('euc_kr', 'big5', 'euc_jp', 'gb2312'):
    for a in range(0x81, 0xFF):
        for b in range(0x40, 0xFF):
            try:
                ks.update(map(ord, bytes([a, b]).decode(enc)))
            except UnicodeDecodeError:
                pass
IDEO = R((0x4E00, 0x9FFF), (0xF900, 0xFAFF), (0x2E80, 0x2FDF))
SKIP = R((0x7F, 0x9F), (0xE00, 0xE7F), (0x1800, 0x180F), (0xE000, 0xF8FF), (0xFE00, 0xFE0F), (0x200B, 0x200F), (0x202A, 0x202E),
         (0x2060, 0x206F), (0xD800, 0xDFFF))

glyphs, metrics, cmap, order = {}, {}, {}, ['.notdef']
p = TTGlyphPen(None)
for x0, y0, x1, y1, rev in ((60, 0, 440, 700, 0), (110, 50, 390, 650, 1)):
    pts = [(x0, y0), (x0, y1), (x1, y1), (x1, y0)]
    if rev:
        pts.reverse()
    p.moveTo(pts[0])
    for q in pts[1:]:
        p.lineTo(q)
    p.closePath()
glyphs['.notdef'] = p.glyph()
metrics['.notdef'] = (500, 60)
for font, want in WANT:
    cm = font.getBestCmap()
    gs = font.getGlyphSet()
    k = UPM / font['head'].unitsPerEm
    n = 0
    for cp in sorted(cm):
        if cp not in want or cp in have or cp in cmap or cp in SKIP or (cp in IDEO and cp not in ks):
            continue
        name = 'u%04X' % cp
        tp = TTGlyphPen(None)
        pen = TransformPen(Cu2QuPen(tp, 0.8 if font is cjk else 0.5, reverse_direction='glyf' not in font), (k, 0, 0, k, 0, 0))
        g = gs[cm[cp]]
        try:
            from fontTools.pens.recordingPen import DecomposingRecordingPen
            rp = DecomposingRecordingPen(gs)
            g.draw(rp)
            rp.replay(pen)
        except Exception as e:
            print('skip %04X %s' % (cp, e))
            continue
        gl = tp.glyph(round=lambda v: int(round(v)))
        glyphs[name] = gl
        metrics[name] = (max(0, round(g.width * k)), 0)
        cmap[cp] = name
        order.append(name)
        n += 1
    print(font['name'].getDebugName(4), n)
for a, b, to in ((0x1F000, 0x1FAFF, 0xE000), (0x1D400, 0x1D7FF, 0xEB00)):
    for cp in range(a, b + 1):
        if cp in cmap:
            cmap[to + cp - a] = cmap[cp]
fb = FontBuilder(UPM, isTTF=True)
fb.setupGlyphOrder(order)
fb.setupCharacterMap(cmap)
fb.setupGlyf(glyphs)
gt = fb.font['glyf']
fb.setupHorizontalMetrics({n: (metrics[n][0], getattr(gt[n], 'xMin', 0) or 0) for n in order})
fb.setupHorizontalHeader(ascent=960, descent=-240)
fb.setupNameTable({'familyName': 'BrickOS World', 'styleName': 'Regular',
                   'licenseDescription': 'SIL Open Font License 1.1 (Noto); Symbola glyphs: free for any use (George Douros)'})
fb.setupOS2(sTypoAscender=960, sTypoDescender=-240, usWinAscent=960, usWinDescent=240)
fb.setupPost(keepGlyphNames=False)
fb.save(out)
print(len(order), 'glyphs', os.path.getsize(out))
