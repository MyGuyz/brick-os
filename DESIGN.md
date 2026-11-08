# Brick OS — DESIGN.md

**Source:** the mockup canvas "Brick OS UI mockups"
- URL: https://claude.ai/artifact/Fa1bGq8ijVgG5UnrRhjh6b
- Version: 14, 2026-11-09
- 6 artboards: Home, Game Switcher, เกม, YouTube, คีย์บอร์ดไทย, ตั้งค่า
- Plus the "Design rules for the real device" note

**Rules for using this file:**
- Every value here is copied from those files. Nothing is invented.
- Where the mockups don't specify something, this file says **"ไม่ได้กำหนดใน mockup"**. Don't guess those values. Ask, or draw a mockup first.
- Sizes are px on the 1024×768 screen (the mockups are 1:1 with the device).

**Decisions from the user (2026-11-09):**
- Everything is bigger and the mockups were redrawn without distortion: every text size, radius and ring is ×1.25 of the first draft.
  - Where that didn't fit 1024×768, fewer items are shown. Items never got smaller.
- Home matches the mockup exactly: no tab row, no play-time stats strip.
- YouTube uses the Google TV layout (icon rail + rows) in these colours.
- Keyboard buttons follow the mockup: X = space, Y = ก/A, L1 = Shift.

---

## 1. Rules for the real device (from the canvas note)

- The UI is drawn by the CPU: solid surfaces only. No blur, no full-screen transparency, no shadows. Rounded corners are the only alpha.
- Fonts: Inter + IBM Plex Sans Thai (OFL). No SF Pro / SF Symbols.
- Icons: stroke line icons, one set.
- Selection = accent ring + a brighter card, no glow.
- Button hints always at the bottom.

## 2. Colours

| Token | Value | Used for |
|---|---|---|
| `bg` | `#000000` | Screen background (Home, เกม, YouTube, Keyboard, ตั้งค่า) |
| `bg-switcher` | `#0B0B0D` | Game Switcher only |
| `surface-1` | `#1C1C1E` | Cards, list groups, tool tiles, search fields, chips, unselected tab chips, hint-bar top border |
| `surface-2` | `#2C2C2E` | Icon wells, key caps, button-hint circles, the selected row in a list group, row separators, the YouTube rail's current-page circle |
| `surface-kb` | `#121214` | Keyboard panel |
| `surface-3` | `#3A3A3C` | Slider track, quality-badge border |
| `dot-off` | `#48484A` | Inactive page dots |
| `text` | `#F2F2F7` | Main text, white pills, the selected tab chip |
| `text-2` | `#A1A1A6` | Secondary text, hint labels, unselected rail icons |
| `text-3` | `#C7C7CC` | Value on the focused row, unselected tab chip text, space-bar label |
| `text-4` | `#6E6E73` | Right-hand note in the hint bar |
| battery outline | `#8E8E93` | Home status bar |
| `accent` | `#0A84FF` (default) | Selection ring, selected list row, selected key, caret, the "ค้นหา" key |
| accent options | `#0A84FF` `#30D158` `#FF9F0A` `#BF5AF2` | ตั้งค่า > สีเน้น |
| `success` | `#30D158` | "มีเวอร์ชันใหม่" badge (black text) |
| `progress` | `#FF453A` | YouTube watched bar |

Home tool icons (stroke icon on a `surface-2` well): เกม `accent` · YouTube `#FF6B5E` · เพลง `#FF7AB6` · วิดีโอ `#FFB340` · Ports `#5EE0A0` · ตั้งค่า `#C7C7CC`.

Settings category chips (white letter on the colour): W WiFi `#0A6CD6` · B Bluetooth `#0A6CD6` · จ หน้าจอและภาษา `#5E5CE6` ·
ส เสียง `#D63A6B` · ก การควบคุม `#3A8A4F` · พ พลังงานและแบตเตอรี่ `#2F8F4E` · ร ร้านเสริม (Xtras) `#B5651D` · i เกี่ยวกับ `#636366`.

The art colours in the mockups (`#1F3A5F`, `#3D2B4F`, …) are placeholders for real box art, screenshots and thumbnails. They are not tokens.

## 3. Type

- Family: `Inter, 'IBM Plex Sans Thai', sans-serif`. The Thai keys on the keyboard use `'IBM Plex Sans Thai'`.

| Role | Size / weight |
|---|---|
| Page title | 35 / 700 (Home: letter-spacing −0.4) |
| Key cap (character) | 34 |
| Query text (keyboard) | 32 / 500 |
| Switcher game name | 30 / 700 |
| Section / row title | 28 / 700 |
| Tool tile name | 24 / 600 |
| Card / row title | 21 / 600 (games, Continue), 21 / 500 (console, settings groups), 21 (settings rows) |
| Status bar time | 21 / 600 |
| Bottom-row key labels | 22 / 600 (ค้นหา 22 / 700); เว้นวรรค 21 |
| Video title | 20 / 600, one line, ellipsis |
| Value text, keyboard chips | 20 |
| Small | 19 (battery %, counts, hint labels, tab chips, search placeholder) |
| Meta | 18 (when played, tool subtitles, footnotes, settings values, keyboard hints) |
| Tiny | 16 (console badge, resume pill, hint letters, game meta, video meta, quality badge) |
| Duration pill, wide hint labels | 15 / 600–700 |

Weights in use: 400, 500, 600, 700.

## 4. Shape

| Element | Radius |
|---|---|
| Switcher focused card | 30 |
| Tool tile | 24 |
| YouTube search pill / tab chip | 24 / 22 (full pill) |
| Continue art, list groups | 22 |
| Switcher side cards, keyboard query field | 20 |
| YouTube thumbnail, Home icon well | 18 |
| List row inside a group | 15 |
| Key caps | 14 |
| Keyboard chips | 12 |
| Badges / pills | 10–11 |
| Game thumbnail (เกม) | 10 |
| Duration pill | 8 |
| Hint circle | 16 (32×32) |

- Selection ring: `5px solid accent`, offset 4 (Home, YouTube) or 5 (Switcher).
- Unselected Continue cards: opacity 0.72. Switcher side cards: opacity 0.55.

## 5. Shared pieces

### 5.1 Status bar
- Height 56, padding 0 32, items at both ends.
- Left: the time, 21/600. On the Switcher the left side is the page title "สลับเกม".
- Right on Home: WiFi stroke icon 26×19, gap 16, "80%" 19, then a battery 35×18 (2px `#8E8E93` border, radius 5, padding 2, `text` fill).
- Right on เกม, YouTube and ตั้งค่า: "80%" 19 only.
- The keyboard screen has no status bar.

### 5.2 Hint bar
- Height 64, padding 0 40, gap 32, 19px `text-2`, 1px `#1C1C1E` top border.
- A hint is a 32×32 `surface-2` circle with the button letter (16/700 `text`), gap 10, then the label.
- Wide buttons (MENU, L1 R1) use a 32-high pill, padding 0 11–12, 15/700.
- Optional note on the right in `text-4`.

## 6. Screens

### 6.1 Home
1. Status bar.
2. เล่นต่อ (padding 8 40 0, gap 14):
   - Header: "เล่นต่อ" 35/700 on the left, the count ("3 เกม") 19 `text-2` on the right.
   - **3 cards**, each 298 wide, gap 24.
   - Art 298×224 (4:3), radius 22. Console badge bottom-left: 16/600, black pill, radius 10, padding 4 10.
   - Under the art, gap 10: name 21/600, when played 18 `text-2`.
   - Selected card: accent ring. Other cards: opacity 0.72.
3. Tool grid (padding 24 40 0), 3 columns × 2 rows, gap 16:
   - Tile 120 high, radius 24, `surface-1`, padding 0 20, gap 16.
   - Well 62×62, radius 18, `surface-2`, with a stroke icon 34 in the tool's colour.
   - Name 24/600 and subtitle 18 `text-2`.
   - Order: เกม "1,248 เกม" · YouTube "ติดตาม 12 ช่อง" · เพลง "กำลังเล่น · Lo-fi" · วิดีโอ "36 ไฟล์" · Ports "เกม PC 9 เกม" · ตั้งค่า "WiFi · Bluetooth".
4. Hint bar: **A** เล่นต่อ · **Y** ค้นหา · **MENU** สลับเกม · note "Brick OS".
- No tab row and no stats strip.
- **ไม่ได้กำหนดใน mockup:**
  - How focus moves between the cards and the tiles.
  - What shows with fewer than 3 games played.
  - How each subtitle is computed (count, now playing, …).

### 6.2 Game Switcher
- Background `#0B0B0D`.
- Status bar: "สลับเกม" on the left, "เกมนี้บันทึกแล้ว · 12:28" 19 `text-2` on the right.
- Centre row, gap 28:
  - Side card 180×135, radius 20, opacity 0.55.
  - Focused card 528×396, radius 30, with a 5px accent ring at offset 5, and a "กำลังเล่น" pill bottom-left (18/600, black, radius 11).
  - Side card.
- Under the focused card: name 30/700, then "SNES · เล่นไป 3 ชม. 12 นาที" 19 `text-2`.
- Page dots, gap 12, 20 above the hint bar: inactive 10×10 `#48484A`, current 28×10 `text`.
- Hint bar: **A** เล่นเกมนี้ · **B** กลับเข้าเกม · **X** ปิดเกม · note "กด MENU ค้างเพื่อเปิด".
- **Decisions where the mockup says nothing** (2026-11-09):
  - Opened from a game (MENU held 0.8 s, or MENU+SELECT): B resumes that game, and "กำลังเล่น" marks it.
  - Opened from Home (MENU): B goes back to Home and no card is marked.
  - X removes the game from the switcher (its save stays).
  - The header's time is when the game's resume state was saved.
  - With two games there is one side card; with one game, none.

### 6.3 เกม
- Status bar, then two columns (gap 24, padding 8 32 0).
- **Left column, 340 wide:**
  - Title "เกม" 35/700.
  - A `surface-1` group (radius 22, padding 7) of console rows:
    - Row 60 high, padding 0 16, radius 15.
    - Name 21/500 on the left, count 19 on the right (`text-2`, or white on the selected row).
    - Selected row background = accent.
- **Right column:**
  - Header: console name 28/700 on the left, "เรียง A–Z" 19 `text-2` on the right.
  - A `surface-1` group (radius 22) of game rows, 6 visible; the list scrolls:
    - Row 80 high, padding 0 20, gap 16, 1px `#2C2C2E` separator.
    - Art 60×45, radius 10.
    - Name 21/600 over meta 16 `text-2` (size, or "เล่นล่าสุด …").
    - A "เล่นต่อ" pill (16/600, black on `text`, radius 10) when the game has a resume state.
    - Selected row `#2C2C2E`, the others `#1C1C1E`.
- Hint bar: **A** เล่น · **B** กลับ · **X** รายการโปรด · **Y** ค้นหา.

### 6.4 YouTube (Google TV layout)
- Status bar.
- **Rail** on the left, 96 wide; items start 92 below the status bar, gap 14:
  - 56×56 circles with 30px stroke icons, in order: ค้นหา (magnifier), หน้าแรก (house), ช่องโปรด (heart), ดูภายหลัง (clock), ประวัติ (clock with an arrow), ตั้งค่า (gear).
  - Current page: a `surface-2` circle with a `text` icon. The others: no circle, icon in `text-2`.
- **Top bar** (padding 12 0 0 16, gap 12):
  - Search pill 260×48, radius 24, `surface-1`, magnifier 22, "ค้นหา" 19 `text-2`.
  - Tab chips 44 high, padding 0 20, radius 22, 19px.
    - Selected chip: `text` background, black text, 600.
    - Other chips: `surface-1` background, `text-3` text, 500.
  - Quality badge on the right ("720p", 16 `text-2`, 1px `#3A3A3C`, radius 10).
- **Rows:**
  - Row title 28/700, 22 above and 12 below.
  - Cards 300 wide, gap 20. Thumbnail 300×169, radius 18.
  - Duration pill bottom-right: 15/600, black, radius 8.
  - Red watched bar, 5 high.
  - Title 20/600, one line. Meta 16 `text-2`.
  - Selected card: 5px accent ring, offset 4.
  - The last card in a row is cut by the screen edge. The next row peeks under the hint bar.
- Hint bar: **A** เล่น · **X** ติดตามช่อง · **Y** พิมพ์ค้นหา · **L1 R1** สลับแท็บ.
- **ไม่ได้กำหนดใน mockup:**
  - The rail when it is focused (expanded with labels?).
  - The player screen, the channel page, and YouTube's own settings.

### 6.5 คีย์บอร์ดไทย
- No status bar.
- **Top block** (padding 36 48 0, gap 14):
  - Label "ค้นหาใน YouTube" 21 `text-2`.
  - Query field 80 high, radius 20, `surface-1`, padding 0 24. Text 32/500, caret 3×38 in the accent colour.
  - Suggestion chips, gap 12: padding 10 18, radius 12, `surface-1`, 20px. A history item reads "ประวัติ: …" in `text-2`.
- **Keyboard panel** at the bottom:
  - `#121214`, 1px `#1C1C1E` top border, padding 18 24 14, gap 10 between rows.
  - Keys are 66 high, gap 8 (rows gap 10), radius 14, `surface-2`, 34px, filling the width (24 in from each side).
  - Selected key: accent background, white text.
  - **Every row has the same number of key units, so the keys line up in columns from edge to edge** (user, 2026-11-09):
    - Thai: 12 per row (74 wide).
    - English: 10 per row.
  - Thai Kedmanee rows:
    - `ๅ / - ภ ถ ุ ึ ค ต จ ข ช`
    - `ๆ ไ ำ พ ะ ั ี ร น ย บ ล`
    - `ฟ ห ก ด เ ้ ่ า ส ว ง ฃ`
    - `⇧ ผ ป แ อ ิ ื ท ม ใ ฝ ⌫`
  - A vowel or tone key shows the mark alone, centred on the key, with no dotted circle (user, 2026-11-09).
  - The Shift layer uses the same columns:
    - `+ ๑ ๒ ๓ ๔ ู ฿ ๕ ๖ ๗ ๘ ๙`
    - `๐ " ฎ ฑ ธ ํ ๊ ณ ฯ ญ ฐ ,`
    - `ฤ ฆ ฏ โ ฌ ็ ๋ ษ ศ ซ . ฅ`
    - `⇧ ( ) ฉ ฮ ฺ ์ ? ฒ ฬ ฦ ⌫`
  - Bottom row, as on the English board:
    - ◀ (1)
    - ▶ (1)
    - "เว้นวรรค" (6 units; 4 on English), 21 in `text-3`
    - - (1)
    - "ก / A" (1), 22/600
    - "ค้นหา" (2), accent background, white text, 22/600
  - Hints, 18 `text-2`, gap 28, centred: **A** พิมพ์ · **B** ลบ · **X** เว้นวรรค · **Y** ก/A · **L1** Shift · **START** ค้นหา.
- **ไม่ได้กำหนดใน mockup:**
  - The Shift layer.
  - The English layout.
  - Digits and symbols.
  - Moving the caret.
  - How focus reaches the chips.

### 6.6 ตั้งค่า
- Status bar, then two columns (gap 24, padding 4 32 0).
- **Left column, 400 wide:**
  - Title "ตั้งค่า" 35/700.
  - A `surface-1` group (radius 22, padding 7) of category rows:
    - Row 62 high, padding 0 14, radius 15, gap 14.
    - Chip 38×38, radius 10, letter 16/700.
    - Name 21/500.
    - Value 18 on the right (`text-2`, or white on the selected row).
    - Selected row = accent.
  - Values shown: WiFi "บ้าน" · Bluetooth "ปิด" · เสียง "70%" · พลังงานและแบตเตอรี่ "6 ชม.".
- **Right column:**
  - Section title 28/700.
  - A `surface-1` group (radius 22) of rows:
    - Row 70 high, padding 0 22, label 21, value 20, 1px `#2C2C2E` separators.
    - Focused row: `#2C2C2E` background, value in `text-3`.
  - The rows:
    - ภาษา "ไทย ›"
    - ธีม "มืด ›"
    - สีเน้น: four 28×28 dots, gap 12; the chosen dot has a 3px `text` ring at offset 3
    - ความสว่าง: track 240×8 `#3A3A3C`, radius 4, `text` fill
    - พักหน้าจอหลัง "2 นาที ›"
  - Footnote 18 `text-2`.
  - A separate group: "อัปเดตระบบ" with a "มีเวอร์ชันใหม่" badge (18/600, black on `#30D158`, radius 10).
- Hint bar: **A** เลือก · **B** กลับ · note "Brick OS · ต่อยอดจาก NX Redux".
- **ไม่ได้กำหนดใน mockup:** the pages of the other categories.

## 7. Screens with no mockup

**ไม่ได้กำหนดใน mockup:**
- Music Player
- Media Player (วิดีโอ)
- PortMaster
- Xtras
- Artwork Manager
- The in-game menu
- The YouTube player
- Boot and sleep screens

Draw a mockup before building any of these.
