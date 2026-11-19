# Arduino Mega VT102 display over serial w/MCUFriend ILI9481 3.5" Colour LCD

A VT102-style terminal on an Arduino Mega 2560 driving an MCUFRIEND 3.5" TFT
(ILI9481, 480×320). The display uses a 400×320 region with a 66×30 character
grid (6×8 px cells).

## Hardware

- **Display:** MCUFRIEND 3.5" shield, controller ID `0x9481` (forced), rotation 1, inverted colours. Uses the local patched `lib/MCUFRIEND_kbv` snapshot.
- **Terminal port:** Serial3 on Mega pins **14 (TX3)** and **15 (RX3)**, **115200 baud**. Connect an RS-232 transceiver (e.g. MAX3232) between these pins and your host: transceiver TX → pin 15, transceiver RX → pin 14, common GND.
- **Debug/upload:** Serial0 (USB) at 9600 baud. Data received on Serial0 is also fed into the VT parser and echoed to Serial3, so you can test from a PC serial monitor without extra hardware.

## Building

```bash
~/.platformio/penv/bin/pio run
```

## Escape sequence notation

The commands in this document are **control bytes**, not printable text.
Nothing shown in the tables should be typed literally; each entry describes a
byte sequence to send. Three forms are used:

| Written here | Meaning | Bytes on the wire (Hexidecimal notation)| `printf` form (octal notation)|
|--------------|---------|-------------------|---------------|
| `ESC c` | ESC followed by one final byte | `0x1B`, `c` | `\033c` |
| `CSI r ; c H` | CSI with numeric parameters and a final byte | `0x1B`, `[`, row, `;`, column, `H` | `\033[16;20H` (row 16, column 20) |
| `CSI ? 25 l` | CSI with a DEC private-mode prefix | `0x1B`, `[`, `?`, `25`, `l` | `\033[?25l` |

The two right-hand columns use different number bases, which is why they look
unrelated: **"Bytes on the wire" are hexadecimal**, while the **"`printf` form"
uses octal escapes** (`\033` = octal 33 = decimal 27 = hex `0x1B`). Printable
bytes such as `[`, `H` and `?` are the same in both.

Rules for reading the command tables:

- **`ESC`** is byte `0x1B` (hex), written as `\033` (octal) in `printf`.
  **`CSI`** is the two-byte sequence `ESC [`, written `\033[`.
- `printf` consumes up to three octal digits after a backslash, so never put a
  digit directly after `\033`: `\03331m` is parsed as octal `0333` (`0xDB`),
  not ESC followed by `31m`. Keep the `[` immediately after the escape.
- Lower-case words such as `r`, `c`, `n`, `top`, `bot`, `w`, `h` are
  **placeholders for decimal numbers**, not characters to transmit. Substituting
  `r=16` and `c=20` into `CSI r ; c H` gives `CSI 16 ; 20 H`, sent as
  `\033[16;20H`.
- **Spaces are only for readability.** The real byte stream contains no spaces:
  `CSI 1 ; 31 m` is sent as `\033[1;31m`.
- The **final byte** — the letter or symbol at the end (`H`, `J`, `m`, `@`, `r`,
  …) — is mandatory and selects the action. Parameters alone do nothing. A
  letter can play both roles depending on position: `r` is a row number in
  `CSI r ; c H`, but the "set scroll region" final in `CSI top ; bot r`.
- Omitted parameters take their documented defaults, usually `1` for cursor
  movement and `0` for erase operations. `CSI A` therefore means "move up one
  row": `\033[A`.
- A `?` immediately after `[` marks a **DEC private mode**, e.g. `CSI ? 25 l`
  (hide cursor) is sent as `\033[?25l`.
- Character-set selection uses plain `ESC`, not CSI: `ESC ( 0` is `\033(0`,
  `ESC ( B` is `\033(B`.

The most reliable way to send these bytes from Linux is `printf`:

```bash
printf '\033[2J\033[16;20HHello' > /dev/ttyUSB1   # Serial3, 115200 baud
printf '\033[2J\033[16;20HHello' > /dev/ttyUSB0   # Serial0 debug, 9600 baud
```

Typing `\033` literally in minicom sends the characters `\`,`0`,`3`,`3`, not
the ESC byte. If using minicom interactively you must press the actual ESC key,
but many terminals have an ESC-sequence timeout that swallows following
characters — `printf` is recommended.

## VT102 command reference

### Control characters

| Byte | Action |
|------|--------|
| `0x08` (BS) | Back one column, erase that cell |
| `0x09` (TAB) | Advance to next 8-column tab stop |
| `0x0A` (LF) | Line feed (scrolls at bottom of region) |
| `0x0C` (FF) | Clear screen, home cursor, reset scroll region |
| `0x0D` (CR) | Carriage return (column 0) |
| `0x07` (BEL) | Ignored (no buzzer) |

`CR LF` (`\r\n`) is handled correctly — no double-spacing.

### Cursor movement

| Sequence | Action |
|----------|--------|
| `CSI r ; c H` | Move cursor to row `r`, column `c` (1-based). `CSI f` is an alias. |
| `CSI n A` | Move up `n` rows (default 1) |
| `CSI n B` | Move down `n` rows (default 1) |
| `CSI n C` | Move right `n` columns (default 1) |
| `CSI n D` | Move left `n` columns (default 1) |
| `CSI s` | Save cursor position |
| `CSI u` | Restore cursor position |
| `ESC 7` | Save cursor position (VT100 form) |
| `ESC 8` | Restore cursor position (VT100 form) |

### Erasing

| Sequence | Action |
|----------|--------|
| `CSI J` or `CSI 0 J` | Erase from cursor to end of screen |
| `CSI 1 J` | Erase from start of screen to cursor |
| `CSI 2 J` / `CSI 3 J` | Erase entire screen (power-on state for 3) |
| `CSI K` or `CSI 0 K` | Erase from cursor to end of line |
| `CSI 1 K` | Erase from start of line to cursor |
| `CSI 2 K` | Erase entire line |

### Editing

| Sequence | Action |
|----------|--------|
| `CSI n L` | Insert `n` blank lines at cursor (within scroll region) |
| `CSI n M` | Delete `n` lines at cursor (within scroll region) |
| `CSI n @` | Insert `n` blank characters at cursor |
| `CSI n P` | Delete `n` characters at cursor |

### Scroll region and modes

| Sequence | Action |
|----------|--------|
| `CSI top ; bot r` | Set scroll region rows `top`..`bot` (1-based, inclusive) |
| `CSI ? 6 h` | Origin mode on — cursor addressing is relative to scroll region |
| `CSI ? 6 l` | Origin mode off (default) |
| `CSI ? 7 h` | Auto-wrap on (default) |
| `CSI ? 7 l` | Auto-wrap off — text stays at right margin |
| `CSI ? 25 h` | Show blinking cursor (default) |
| `CSI ? 25 l` | Hide cursor |
| `CSI ? 50 h` | Normal width — 66 columns, 6×8 px cells (clears screen) |
| `CSI ? 51 h` | Double width — 33 columns, 12×8 px cells (clears screen) |

Switching width clears the screen and homes the cursor (like DECCOLM on a real
VT). `ESC c` (full reset) also returns to normal width. `CSI ? 50 l` and
`CSI ? 51 l` are accepted as aliases for normal width.

### DEC Special Graphics (line drawing)

`ESC ( 0` selects the DEC Special Graphics character set: bytes `0x5F`–`0x7E`
then render as line-drawing glyphs. `ESC ( B` returns to ASCII. The character
set is recorded per cell, so boxes drawn earlier stay intact after switching
back to ASCII. `ESC )` (G1 designation) is accepted but ignored.

The most useful codes (type the ASCII character shown):

| Char | Glyph | Char | Glyph | Char | Glyph | Char | Glyph |
|------|-------|------|-------|------|-------|------|-------|
| `` ` `` | ◆ | `f` | ° | `j` | ┘ | `q` | ─ |
| `a` | █ | `g` | ± | `k` | ┐ | `x` | │ |
| `b` | ␉ | `y` | ≤ | `l` | ┌ | `t` | ├ |
| `c` | ␌ | `z` | ≥ | `m` | └ | `u` | ┤ |
| `d` | ␍ | `{` | π | `n` | ┼ | `v` | ┴ |
| `e` | ␊ | `\|` | ≠ | `o`,`p` | upper line | `w` | ┬ |
| `i` | ␋ | `}` | £ | `r`,`s` | lower line | `~` | · |

`_` (`0x5F`) is blank. `o`/`p` and `r`/`s` are partial horizontal lines at
different vertical scan positions. DEC graphics works in either width mode.

### Tek-style vector graphics modes

Four graphics modes are available. All use native pixel coordinates
(x 0–399, y 0–319) and SGR foreground colour. `ESC c` (full reset) exits any
graphics mode.

| Sequence | Mode | Description |
|----------|------|-------------|
| `CSI ? 60 h` | Full screen | Display cleared, text suspended. `CSI ? 60 l` restores the previous text screen. |
| `CSI ? 62 h` | Combined | Text and graphics share the screen. Shapes are stored in a 24-command display list and replayed on top after every text redraw/cursor blink. `CSI ? 62 l` restores text. |
| `CSI ? 63 h` | Split screen | Text confined to the top 20 rows (y 0–159); graphics confined to y 160–319. No overlay conflicts. `CSI ? 63 l` restores 30-row text. |
| `CSI ? 64 h` | Static overlay | Shapes drawn directly over text once; cursor blink is disabled so they stay visible. Text written later may erase shapes in its rows. `CSI ? 64 l` restores text. |

Switching directly between graphics modes clears the existing shapes. Exiting
any mode redraws the text buffer (which is left untouched while graphics are
active).

**Combined mode details:** coordinates are recorded per shape along with the
colour active at draw time, so later SGR changes do not recolour old shapes.
The list holds 24 shapes; further commands replace the oldest. `CSI 2 J`
clears both text and the shape list.

| Sequence | Action |
|----------|--------|
| `CSI y ; x M` | Move the beam to (x, y) without drawing |
| `CSI y ; x D` | Draw line from beam to (x, y), then move beam there |
| `CSI y ; x P` | Draw a single point at (x, y) (beam unchanged) |
| `CSI x1 ; y1 ; x2 ; y2 L` | Draw a line between two points (beam unchanged) |
| `CSI x ; y ; w ; h R` | Draw a rectangle outline at (x, y), size w×h |
| `CSI x ; y ; r C` | Draw a circle outline centred at (x, y), radius r |
| `CSI x ; y ; w ; h F` | Draw a filled rectangle at (x, y), size w×h |
| `CSI x ; y ; r G` | Draw a filled circle centred at (x, y), radius r |
| `CSI x1 ; y1 ; x2 ; y2 ; x3 ; y3 T` | Draw a filled triangle |
| `CSI 2 J` | Clear the graphics screen (in graphics modes 60/62/63/64) |

Note the different coordinate orders: `M`/`D`/`P` use **row;column** (matching
`CSI r ; c H`), while `L`/`R`/`C`/`F`/`G`/`T` use **x;y** (matching graphics
convention). All filled shapes use `curFg` (set via SGR) as the fill colour.

Coordinates are clamped to the display; in split mode the lower bound is y=160,
and circle radii are reduced to keep circles inside the graphics band. Drawing
uses Adafruit_GFX primitives (`drawPixel`, `drawLine`, `drawRect`, `drawCircle`),
which are fine for modest shapes but not high-speed animation. `CSI 31 m` etc.
set the drawing colour.

### Select Graphic Rendition (SGR) — `CSI ... m`

**Basic colours**

| Code | Effect |
|------|--------|
| `0` | Reset all attributes (white on black) |
| `30`–`37` | Foreground: black, red, green, yellow, blue, magenta, cyan, white |
| `40`–`47` | Background: black, red, green, yellow, blue, magenta, cyan, white |
| `90`–`97` | Bright foreground |
| `100`–`107` | Bright background |

**Attributes**

| Code | Effect |
|------|--------|
| `1`, `21`, `22` | Bold / normal — parsed but not rendered (no bold font) |
| `4` | Underline on |
| `24` | Underline off |
| `5` | Blink on |
| `25` | Blink off |
| `7` | Reverse video on (swap fg/bg) |
| `27` | Reverse video off |

**Extended colours**

| Form | Effect |
|------|--------|
| `38 ; 5 ; n` | Foreground from xterm 256-colour palette |
| `48 ; 5 ; n` | Background from xterm 256-colour palette |
| `38 ; 2 ; r ; g ; b` | True-colour foreground (RGB, 0–255) |
| `48 ; 2 ; r ; g ; b` | True-colour background (RGB, 0–255) |

Multiple SGR codes can be combined in one sequence, e.g. `CSI 1 ; 31 ; 44 m`.

**Important:** colours and attributes are applied when a row is *redrawn*, and
the screen stores characters only (plus the SGR flag byte per cell). Changing
SGR codes does **not** repaint text already on screen. Send the SGR sequence
*before* printing the text you want styled.

### Miscellaneous

| Sequence | Action |
|----------|--------|
| `ESC c` | Full reset (RIS) — clears screen, resets modes, colours, scroll region |

### Not implemented

- Alternate screen buffer
- OSC sequences (window title etc.) — parsed and ignored
- 256-colour palette is mapped to the nearest RGB565 value; true-colour is quantised to 5/6/5 bits

## Example test commands

All examples write to `/dev/ttyUSB1` (Serial3 at 115200). Use `/dev/ttyUSB0`
for the Serial0 debug port at 9600 baud.

**Important:** SGR codes are cumulative. Always send `ESC[0m` to reset
attributes and colours between independent tests, otherwise blink/reverse/
underline from a previous test carries over.

```bash
# Centre a banner
printf '\033[2J\033[16;28HCENTRE TEST' > /dev/ttyUSB1

# Three coloured lines (no reset needed — each line sets its own fg)
printf '\033[2J\033[31m\033[16;20HRED' > /dev/ttyUSB1
printf '\033[32m\033[17;20HGREEN' > /dev/ttyUSB1
printf '\033[34m\033[18;20HBLUE' > /dev/ttyUSB1

# Reverse video: white bg, red text; ESC[27m turns reverse off only
printf '\033[2J\033[7;31;47m\033[10;10HREVERSE' > /dev/ttyUSB1
printf '\033[27;32m\033[12;10HNORMAL GREEN' > /dev/ttyUSB1

# Underline (line is drawn beneath the glyphs)
printf '\033[2J\033[4;32m\033[10;10HUNDERLINED' > /dev/ttyUSB1
printf '\033[24m\033[12;10HNOT UNDERLINED' > /dev/ttyUSB1

# Blink — text toggles visible/invisible every 500 ms
printf '\033[2J\033[5;33m\033[10;10HBLINKING YELLOW' > /dev/ttyUSB1
printf '\033[0m\033[12;10HSTATIC WHITE' > /dev/ttyUSB1

# 256-colour palette (orange 208, bright green 46, white-on-blue 21/231)
printf '\033[2J\033[38;5;208m\033[10;10HORANGE 208' > /dev/ttyUSB1
printf '\033[38;5;46m\033[12;10HBRIGHT GREEN 46' > /dev/ttyUSB1
printf '\033[48;5;21m\033[38;5;231m\033[14;10HWHITE ON BLUE 21' > /dev/ttyUSB1

# True-colour RGB (orange text, yellow on purple)
printf '\033[2J\033[38;2;255;128;0m\033[10;10HORANGE RGB' > /dev/ttyUSB1
printf '\033[48;2;128;0;128m\033[38;2;255;255;0m\033[12;10HYELLOW ON PURPLE' > /dev/ttyUSB1

# Combined attributes (blinking, underlined, reverse)
printf '\033[2J\033[0m\033[4;5;7;31;47m\033[10;10HALL COMBINED' > /dev/ttyUSB1

# Scroll region: lines scroll only between rows 5 and 20
printf '\033[2J\033[5;20r\033[5;1HTOP' > /dev/ttyUSB1
for i in $(seq 1 25); do printf 'line %d\n' "$i" > /dev/ttyUSB1; done

# Insert characters and lines
printf '\033[2J\033[5;1HABCDEF\033[5;3H\033[2@' > /dev/ttyUSB1   # "AB  CDEF"
printf '\033[5;1H\033[2L' > /dev/ttyUSB1                         # insert 2 lines

# Delete characters and lines
printf '\033[2J\033[5;1HABCDEF\033[5;3H\033[2P' > /dev/ttyUSB1   # "ABEF"
printf '\033[5;1H\033[M' > /dev/ttyUSB1                           # delete 1 line

# Erase in line: type text, then erase from cursor / to cursor / whole line
printf '\033[2J\033[5;1HABCDEFGHIJ\033[5;6H\033[K' > /dev/ttyUSB1  # "ABCDE"
printf '\033[5;6H\033[1K' > /dev/ttyUSB1                           # "      FGHIJ"
printf '\033[5;6H\033[2K' > /dev/ttyUSB1                           # empty line

# Save/restore cursor (both CSI and ESC forms)
printf '\033[2J\033[10;10H\033[s\033[20;30HMOVED\033[uBACK' > /dev/ttyUSB1
printf '\033[10;10H\0337\033[20;30HMOVED2\0338BACK2' > /dev/ttyUSB1

# Cursor movement: START at (10,10), UP-RIGHT above it, DOWN below
printf '\033[2J\033[10;10HSTART\033[2A\033[5CUP-RIGHT\033[2B\033[5CDOWN' > /dev/ttyUSB1

# Hide cursor, disable auto-wrap
printf '\033[?25l\033[?7l' > /dev/ttyUSB1

# DEC graphics: a box (l=┌ q=─ k=┐ x=│ m=└ j=┘)
printf '\033[2J\033(0lqqqqk\033(B' > /dev/ttyUSB1
printf '\033[2;1H\033(0x\033(BHELLO\033(0x\033(B' > /dev/ttyUSB1
printf '\033[3;1H\033(0mqqqqj\033(B' > /dev/ttyUSB1

# DEC graphics + double width (large box)
printf '\033[?51h\033(0lqqqqqk\033(B' > /dev/ttyUSB1
printf '\033[2;1H\033(0x\033(BWIDE\033(0x\033(B' > /dev/ttyUSB1
printf '\033[3;1H\033(0mqqqqqj\033(B' > /dev/ttyUSB1

# Double-width text, then back to normal (each switch clears the screen)
printf '\033[?51h\033[2;1HLARGE TEXT' > /dev/ttyUSB1
printf '\033[?50h\033[5;1HBack to normal width' > /dev/ttyUSB1

# Vector graphics: coloured lines, rectangle, circle, then back to text
printf '\033[?60h' > /dev/ttyUSB1
printf '\033[31m\033[0;0M\033[319;399D' > /dev/ttyUSB1       # red diagonal
printf '\033[32m\033[20;20;100;200R' > /dev/ttyUSB1          # green rectangle
printf '\033[34m\033[160;200;60C' > /dev/ttyUSB1             # blue circle
printf '\033[33m\033[10;20;300;380L' > /dev/ttyUSB1          # yellow line
printf '\033[?60l' > /dev/ttyUSB1

# Filled shapes
printf '\033[?60h' > /dev/ttyUSB1
printf '\033[31m\033[20;20;100;80F' > /dev/ttyUSB1           # red filled rect
printf '\033[32m\033[200;160;50G' > /dev/ttyUSB1             # green filled circle
printf '\033[34m\033[300;20;380;20;340;120T' > /dev/ttyUSB1  # blue filled triangle
printf '\033[33m\033[60;60;80;60F' > /dev/ttyUSB1            # yellow rect (overlap test)
printf '\033[2J' > /dev/ttyUSB1                              # clear graphics screen
printf '\033[37m\033[0;0;399;319F' > /dev/ttyUSB1            # white full-screen fill
printf '\033[?60l' > /dev/ttyUSB1

# Combined: text with a chart drawn on top; typing more text keeps the shape
printf '\033[2J\033[?62h\033[3;1HSales report' > /dev/ttyUSB1
printf '\033[32m\033[20;240;60;40F' > /dev/ttyUSB1           # green bar
printf '\033[31m\033[100;240;60;80F' > /dev/ttyUSB1          # red bar
printf '\033[5;1HTyped while graphics active' > /dev/ttyUSB1
printf '\033[?62l' > /dev/ttyUSB1

# Split screen: 20 text rows above, graphics below y=160
printf '\033[2J\033[?63h\033[2;1HTop text area' > /dev/ttyUSB1
printf '\033[34m\033[0;160;399;319R' > /dev/ttyUSB1          # frame bottom area
printf '\033[33m\033[20;180;60;200L' > /dev/ttyUSB1          # line in bottom area
printf '\033[?63l' > /dev/ttyUSB1

# Static overlay: graphics over existing text, blink disabled
printf '\033[?64h\033[36m\033[50;50;200;100R' > /dev/ttyUSB1
printf '\033[?64l' > /dev/ttyUSB1
```

## Manual verification: text + graphics modes

The sequences below were used to verify each mode on hardware (Serial3 at
115200, `/dev/ttyUSB1`). Use `printf` rather than typing literal `\033`.

### Combined mode (`CSI ? 62 h`)

```bash
printf '\033[2J\033[?62h' > /dev/ttyUSB1
printf '\033[1;1HCOMBINED MODE TEST' > /dev/ttyUSB1
printf '\033[31m\033[20;24;200;48R' > /dev/ttyUSB1
printf '\033[4;1HText typed over the rectangle' > /dev/ttyUSB1
```

Expected: a red rectangle spanning rows 3–6. The last command redraws row 4
with text, but the rectangle edge in that row remains because the shape is
replayed on top. Waiting ~2 s confirms cursor blink does not break it either.

```bash
printf '\033[?62l' > /dev/ttyUSB1
```

Expected: the rectangle disappears, text stays. Colour snapshot test:

```bash
printf '\033[?62h\033[31m\033[20;24;80;48F\033[34m\033[120;24;80;48F' > /dev/ttyUSB1
printf '\033[4;1HRedraw trigger' > /dev/ttyUSB1
```

Expected: the first bar stays red after the second (blue) is drawn and a text
redraw is forced.

### Split mode (`CSI ? 63 h`)

```bash
printf '\033[2J\033[?63h\033[1;1HSPLIT: top 20 rows text' > /dev/ttyUSB1
printf '\033[32m\033[0;160;399;319R' > /dev/ttyUSB1
printf '\033[33m\033[20;180;380;300L' > /dev/ttyUSB1
printf '\033[34m\033[100;240;30G' > /dev/ttyUSB1
```

Expected: text at the top, frame + diagonal + blue circle in the bottom 160 px.
Then scroll text hard:

```bash
printf '\033[1;1H' > /dev/ttyUSB1
for i in $(seq 1 35); do printf 'line %02d\r\n' "$i" > /dev/ttyUSB1; sleep 0.05; done
```

Expected: text stops scrolling at row 20, graphics band untouched.

```bash
printf '\033[?63l' > /dev/ttyUSB1
```

Expected: 30-row text layout restored, including previously hidden rows 20–29.

### Static overlay (`CSI ? 64 h`)

```bash
printf '\033[2J\033[1;1HSTATIC OVERLAY TEST' > /dev/ttyUSB1
printf '\033[?64h\033[36m\033[50;40;200;120R' > /dev/ttyUSB1
```

Expected: cyan rectangle over text, with no cursor blink cutting into it after
~2 s.

```bash
printf '\033[3;1HText may erase shape here' > /dev/ttyUSB1
printf '\033[?64l' > /dev/ttyUSB1
```

Expected: row 3 breaks the rectangle (documented limitation of static mode),
then exiting restores clean text.

### Reset from a graphics mode

```bash
printf '\033[?62h\033[31m\033[20;20;100;100F\033c' > /dev/ttyUSB1
```

Expected: full reset clears everything and returns to normal 30-row text.
