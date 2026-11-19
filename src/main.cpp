#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include "termfont.h"
#include "decgraphics.h"

MCUFRIEND_kbv tft;

#define TERM_W   400
#define TERM_H   320
#define CHAR_W   6
#define CHAR_H   8
#define COLS     (TERM_W / CHAR_W)
#define ROWS     30

static char     screen[ROWS][COLS + 1];

// Per-cell SGR flags (colours remain global — applied at redraw time).
#define ATTR_REVERSE   0x01
#define ATTR_UNDERLINE 0x02
#define ATTR_BLINK     0x04
#define ATTR_DECGRAPH  0x08  // render 0x5F..0x7E with DEC line-drawing font
static uint8_t cellAttr[ROWS][COLS]; // 30*66 = 1980 bytes
static uint8_t curAttr = 0;
static uint16_t rowFg[ROWS], rowBg[ROWS]; // colour snapshot for blink redraws (120 bytes)
static bool blinkPhase = false;
static bool blinkTextPresent = false;

static void clearAttrRow(int r) { memset(cellAttr[r], 0, COLS); }
static void shiftAttrUp(int dst, int src) { memcpy(cellAttr[dst], cellAttr[src], COLS); }

static int cx = 0, cy = 0;
static int cx_saved = 0, cy_saved = 0;

static int scrollTop = 0;
static int scrollBottom = ROWS - 1;
static bool originMode = false;
static bool autoWrap = true;
static bool cursorVisible = true;
static unsigned long lastBlink = 0;
static bool cursorOn = true;
static bool justCR = false;

// Dirty row tracking for batched redraws
static bool needsRedraw = false;
static int dirtyMin = ROWS, dirtyMax = -1;
static void markDirty(int r) {
    if (r < 0 || r >= ROWS) return;
    needsRedraw = true;
    if (r < dirtyMin) dirtyMin = r;
    if (r > dirtyMax) dirtyMax = r;
}

static uint16_t curFg = 0xFFFF;
static uint16_t curBg = 0x0000;

// DEC graphics (ESC ( 0 / ESC ( B) is baked into each cell at write time so
// previously drawn boxes stay boxes when the charset is switched back.
static bool decGraphics = false;

// Double-width mode (CSI ?51h / CSI ?50h).  Like DECCOLM, switching clears the
// screen: 66 normal cells (6 px each) or 33 wide cells (12 px each).
static bool doubleWidth = false;
static int effCols() { return doubleWidth ? TERM_W / (CHAR_W * 2) : COLS; }
static int cellWidth() { return doubleWidth ? CHAR_W * 2 : CHAR_W; }

static void setDoubleWidth(bool on) {
    if (doubleWidth == on) return;
    doubleWidth = on;
    tft.fillScreen(curBg);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) screen[r][c] = ' ';
        screen[r][COLS] = '\0';
        clearAttrRow(r);
    }
    blinkTextPresent = false;
    cx = 0; cy = 0; justCR = false;
    for (int r = 0; r < ROWS; r++) markDirty(r);
}

// ---- Tek-style vector graphics mode --------------------------------------
// A dedicated full-screen graphics mode.  The text buffers stay untouched in
// RAM, so exiting simply redraws them.  Coordinates are native pixels.
static bool graphicsMode = false;
static int beamX = 0, beamY = 0;

static void gfxClamp(int &x, int &y) {
    if (x < 0) x = 0; else if (x >= TERM_W) x = TERM_W - 1;
    if (y < 0) y = 0; else if (y >= TERM_H) y = TERM_H - 1;
}
static void gfxPoint(int x, int y) { gfxClamp(x, y); tft.drawPixel(x, y, curFg); }
static void gfxLine(int x1, int y1, int x2, int y2) {
    gfxClamp(x1, y1); gfxClamp(x2, y2);
    tft.drawLine(x1, y1, x2, y2, curFg);
}
static void gfxRect(int x, int y, int w, int h) {
    if (w < 1 || h < 1) return;
    int x2 = x + w - 1, y2 = y + h - 1;
    gfxClamp(x, y); gfxClamp(x2, y2);
    tft.drawRect(x, y, x2 - x + 1, y2 - y + 1, curFg);
}
static void gfxCircle(int x, int y, int r) {
    if (r < 0) return;
    gfxClamp(x, y);
    tft.drawCircle(x, y, r, curFg);
}
static void gfxFillRect(int x, int y, int w, int h) {
    if (w < 1 || h < 1) return;
    int x2 = x + w - 1, y2 = y + h - 1;
    gfxClamp(x, y); gfxClamp(x2, y2);
    tft.fillRect(x, y, x2 - x + 1, y2 - y + 1, curFg);
}
static void gfxFillCircle(int x, int y, int r) {
    if (r < 0) return;
    gfxClamp(x, y);
    tft.fillCircle(x, y, r, curFg);
}
static void gfxFillTriangle(int x1, int y1, int x2, int y2, int x3, int y3) {
    gfxClamp(x1, y1); gfxClamp(x2, y2); gfxClamp(x3, y3);
    tft.fillTriangle(x1, y1, x2, y2, x3, y3, curFg);
}
static void gfxEnter() {
    if (graphicsMode) return;
    graphicsMode = true;
    beamX = 0; beamY = 0;
    tft.fillScreen(curBg);
}
static void gfxExit() {
    if (!graphicsMode) return;
    graphicsMode = false;
    for (int r = 0; r < ROWS; r++) markDirty(r);
}

// ILI9481 MADCTL 0x28 has BGR=1, so colour words are BGR565 (R/B swapped).
static const uint16_t ansiColors[16] = {
    0x0000, 0x001F, 0x07E0, 0x07FF,
    0xF800, 0xF81F, 0xFFE0, 0xFFFF,
    0x8410, 0x041F, 0x07E0, 0x07FF,
    0xF800, 0xF81F, 0xFFE0, 0xFFFF
};

// ---- Fast character rendering -------------------------------------------
// tft.print() calls drawPixel() for every one of the ~35 "on" pixels in a
// 5x7 glyph, and each drawPixel() sets the GRAM address window.  That is
// ~35 register setups per character, ~2300 per row, ~70k per full scroll.
// This builds a small RGB565 pixel block and streams it with ONE
// setAddrWindow + pushColors per chunk of 16 characters.
#define PIXBUF_CHARS 16
static uint16_t pixbuf[PIXBUF_CHARS * CHAR_W * CHAR_H + CHAR_W * CHAR_H]; // 768 + 48 px headroom

static void fastDrawChars(int col, int row, const char *str, const uint8_t *attr,
                          int n, uint16_t fg, uint16_t bg, int extraBg = 0,
                          bool blinkPhase = false, int cellW = CHAR_W) {
    if (n <= 0) return;
    int scale = cellW / CHAR_W;             // 1 = normal, 2 = double width
    int maxChars = PIXBUF_CHARS / scale;    // keep the pixel buffer bounded
    if (n > maxChars) n = maxChars;
    int x = col * cellW;
    int y = row * CHAR_H;
    int w = n * cellW + extraBg;
    tft.setAddrWindow(x, y, x + w - 1, y + CHAR_H - 1);
    uint16_t *p = pixbuf;
    // ILI9481 rotation 1 streams GRAM bottom-to-top on the vertical axis,
    // but left-to-right on the horizontal axis.
    for (int py = CHAR_H - 1; py >= 0; py--) {
        uint8_t bit = 0x80 >> py;            // font bit for this glyph row
        for (int ci = 0; ci < n; ci++) {
            unsigned char c = (unsigned char)str[ci];
            if (c < 32 || c > 126) c = ' ';
            uint8_t a = attr ? attr[ci] : 0;
            uint16_t cellFg = fg, cellBg = bg;
            if (a & ATTR_REVERSE) { cellFg = bg; cellBg = fg; }
            if ((a & ATTR_BLINK) && blinkPhase) { cellFg = cellBg; }
            bool underline = (a & ATTR_UNDERLINE) && (py == 0); // font bit 0x80 = glyph bottom row
            const unsigned char *font = term_font;
            int base = c * 5;
            if ((a & ATTR_DECGRAPH) && c >= 0x5F && c <= 0x7E) {
                font = dec_font;
                base = (c - 0x5F) * 5;
            }
            for (int i = 0; i < cellW; i++) { // 5 font columns (scaled) + gap
                int fc = i / scale;           // font column 0..4; >=5 is the gap
                uint8_t line = (fc < 5) ? pgm_read_byte(&font[base + fc]) : 0;
                *p++ = (underline || (line & bit)) ? cellFg : cellBg;
            }
        }
        for (int k = 0; k < extraBg; k++) *p++ = bg;
    }
    tft.pushColors(pixbuf, (n * cellW + extraBg) * CHAR_H, true);
}

static void fastDrawRow(int row, const char *str, const uint8_t *attr,
                        uint16_t fg, uint16_t bg, bool blinkPhase = false) {
    int cols = effCols();
    int cw = cellWidth();
    int chunk = PIXBUF_CHARS / (cw / CHAR_W);   // 16 normal, 8 double-width
    const int remainder = TERM_W - cols * cw;   // right edge padding (4 px either way)
    for (int c = 0; c < cols; c += chunk) {
        int n = cols - c;
        if (n > chunk) n = chunk;
        int extra = (c + n == cols) ? remainder : 0;
        fastDrawChars(c, row, str + c, attr ? attr + c : nullptr,
                      n, fg, bg, extra, blinkPhase, cw);
    }
}

enum ParseState { ST_NORMAL, ST_ESC, ST_CSI, ST_OSC, ST_CHARSET };
static ParseState pstate = ST_NORMAL;
static bool charsetG1 = false;   // ESC ( = G0, ESC ) = G1 (G1 designations ignored)
static char csiBuf[32];
static uint8_t csiLen = 0;

static int csiParams[8];
static uint8_t csiNParams = 0;

static void parseCsiParams() {
    csiNParams = 0;
    char *p = csiBuf;
    if (*p == '?') p++;
    while (*p && csiNParams < 8) {
        csiParams[csiNParams] = 0;
        while (*p >= '0' && *p <= '9') {
            csiParams[csiNParams] = csiParams[csiNParams] * 10 + (*p - '0');
            p++;
        }
        csiNParams++;
        if (*p == ';') p++;
        else break;
    }
    if (csiNParams == 0) csiNParams = 1;
}

static int csiParam(int idx, int def) {
    if (idx >= csiNParams) return def;
    if (csiParams[idx] == 0 && idx > 0) return def;
    return csiParams[idx];
}

static void drawChar(int col, int row, char ch, uint16_t fg, uint16_t bg) {
    fastDrawChars(col, row, &ch, nullptr, 1, fg, bg);
}

static void scrollUp() {
    for (int r = 0; r < ROWS - 1; r++) { memcpy(screen[r], screen[r+1], COLS+1); shiftAttrUp(r, r+1); }
    for (int c = 0; c < COLS; c++) screen[ROWS-1][c] = ' ';
    screen[ROWS-1][COLS] = '\0';
    clearAttrRow(ROWS - 1);
    for (int r = 0; r < ROWS; r++) markDirty(r);
}

static void newline() {
    markDirty(cy);
    cx = 0;
    cy++;
    if (cy > scrollBottom) {
        int n = cy - scrollBottom;
        for (int i = 0; i < n; i++) {
            for (int r = scrollTop; r <= scrollBottom - 1; r++) { memcpy(screen[r], screen[r+1], COLS+1); shiftAttrUp(r, r+1); }
            for (int c = 0; c < COLS; c++) screen[scrollBottom][c] = ' ';
            screen[scrollBottom][COLS] = '\0';
            clearAttrRow(scrollBottom);
        }
        cy = scrollBottom;
        for (int r = scrollTop; r <= scrollBottom; r++) markDirty(r);
    }
    if (cy >= ROWS) cy = ROWS - 1;
    markDirty(cy);
}

static void putChar(char ch) {
    int cols = effCols();
    if (cx >= cols) {
        if (autoWrap) {
            markDirty(cy);
            cx = 0; cy++;
            if (cy > scrollBottom) {
                int n = cy - scrollBottom;
                for (int i = 0; i < n; i++) {
                    for (int r = scrollTop; r <= scrollBottom - 1; r++) memcpy(screen[r], screen[r+1], COLS+1);
                    for (int c = 0; c < COLS; c++) screen[scrollBottom][c] = ' ';
                    screen[scrollBottom][COLS] = '\0';
                }
                cy = scrollBottom;
                for (int r = scrollTop; r <= scrollBottom; r++) markDirty(r);
            }
            if (cy >= ROWS) cy = ROWS - 1;
            markDirty(cy);
        } else {
            cx = cols - 1;
        }
    }
    screen[cy][cx] = ch;
    cellAttr[cy][cx] = curAttr | (decGraphics ? ATTR_DECGRAPH : 0);
    rowFg[cy] = curFg; rowBg[cy] = curBg;
    if (curAttr & ATTR_BLINK) blinkTextPresent = true;
    markDirty(cy);
    cx++;
}

static void handleControl(char ch) {
    switch (ch) {
        case '\r': cx = 0; newline(); justCR = true; break;
        case '\n': if (!justCR) newline(); justCR = false; cx = 0; break;
        case '\b': if (cx > 0) { cx--; screen[cy][cx] = ' '; cellAttr[cy][cx] = 0; markDirty(cy); } break;
        case '\t': cx = (cx + 8) & ~7; if (cx >= effCols()) cx = effCols() - 1; break;
        case '\a': break;
        case '\x1B': pstate = ST_ESC; break;
        case '\x0C':
            tft.fillScreen(curBg);
            for (int r = 0; r < ROWS; r++) { for (int c = 0; c < COLS; c++) screen[r][c] = ' '; screen[r][COLS] = '\0'; clearAttrRow(r); }
            blinkTextPresent = false;
            cx = 0; cy = 0; scrollTop = 0; scrollBottom = ROWS - 1;
            for (int r = 0; r < ROWS; r++) markDirty(r);
            break;
        default:
            justCR = false;
            if (ch >= 0x20 && ch < 0x7F) putChar(ch);
            break;
    }
}

// xterm 256-colour palette, returned as BGR565 for the ILI9481.
static uint16_t xterm256(int n) {
    if (n < 16) return ansiColors[n];
    uint8_t r, g, b;
    if (n < 232) {
        int v = n - 16;
        static const uint8_t lvl[6] = {0, 95, 135, 175, 215, 255};
        r = lvl[v / 36]; g = lvl[(v / 6) % 6]; b = lvl[v % 6];
    } else {
        r = g = b = 8 + 10 * (n - 232);
    }
    return ((uint16_t)(b >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (r >> 3);
}

static void handleCsi(char final) {
    bool privateMode = (csiBuf[0] == '?');
    parseCsiParams();

    // Graphics-mode primitives.  M/D/P use row;col like cursor addressing;
    // L/R/C use x;y like graphics coordinates.  Other finals (m, h, l, ...)
    // fall through to normal handling so SGR colours and mode toggles work.
    if (graphicsMode) {
        switch (final) {
            case 'M': { beamY = csiParam(0, 0); beamX = csiParam(1, 0); return; }
            case 'D': {
                int y = csiParam(0, 0), x = csiParam(1, 0);
                gfxLine(beamX, beamY, x, y); beamX = x; beamY = y; return;
            }
            case 'P': { gfxPoint(csiParam(1, 0), csiParam(0, 0)); return; }
            case 'L': {
                gfxLine(csiParam(0, 0), csiParam(1, 0), csiParam(2, 0), csiParam(3, 0));
                return;
            }
            case 'R': {
                gfxRect(csiParam(0, 0), csiParam(1, 0), csiParam(2, 0), csiParam(3, 0));
                return;
            }
            case 'C': {
                gfxCircle(csiParam(0, 0), csiParam(1, 0), csiParam(2, 0));
                return;
            }
            case 'F': {
                gfxFillRect(csiParam(0, 0), csiParam(1, 0), csiParam(2, 0), csiParam(3, 0));
                return;
            }
            case 'G': {
                gfxFillCircle(csiParam(0, 0), csiParam(1, 0), csiParam(2, 0));
                return;
            }
            case 'T': {
                gfxFillTriangle(csiParam(0, 0), csiParam(1, 0), csiParam(2, 0),
                                csiParam(3, 0), csiParam(4, 0), csiParam(5, 0));
                return;
            }
            default: break;
        }
    }

    switch (final) {
        case 'H': case 'f': {
            int r = csiParam(0, 1) - 1; int c = csiParam(1, 1) - 1;
            if (originMode) { r += scrollTop; if (r > scrollBottom) r = scrollBottom; }
            if (r < 0) r = 0; if (r >= ROWS) r = ROWS - 1;
            if (c < 0) c = 0; if (c >= effCols()) c = effCols() - 1;
            cx = c; cy = r; markDirty(cy); break;
        }
        case 'A': { int n = csiParam(0,1); cy -= n; if (cy < scrollTop) cy = scrollTop; markDirty(cy); break; }
        case 'B': { int n = csiParam(0,1); cy += n; if (cy > scrollBottom) cy = scrollBottom; markDirty(cy); break; }
        case 'C': { int n = csiParam(0,1); cx += n; if (cx >= effCols()) cx = effCols()-1; break; }
        case 'D': { int n = csiParam(0,1); cx -= n; if (cx < 0) cx = 0; break; }
        case 'J': {
            int mode = csiParam(0, 0);
            if (graphicsMode) {
                if (mode == 2 || mode == 3) { tft.fillScreen(curBg); beamX = 0; beamY = 0; }
                break;
            }
            if (mode == 2 || mode == 3) {
                tft.fillScreen(curBg);
                for (int r = 0; r < ROWS; r++) { for (int c = 0; c < COLS; c++) screen[r][c] = ' '; screen[r][COLS] = '\0'; clearAttrRow(r); }
                blinkTextPresent = false;
                cx = 0; cy = originMode ? scrollTop : 0;
                for (int r = 0; r < ROWS; r++) markDirty(r);
            } else if (mode == 0) {
                for (int c = cx; c < COLS; c++) screen[cy][c] = ' ';
                memset(cellAttr[cy] + cx, 0, COLS - cx);
                markDirty(cy);
                for (int r = cy+1; r < ROWS; r++) { for (int c = 0; c < COLS; c++) screen[r][c] = ' '; clearAttrRow(r); markDirty(r); }
            } else if (mode == 1) {
                for (int r = 0; r < cy; r++) { for (int c = 0; c < COLS; c++) screen[r][c] = ' '; clearAttrRow(r); markDirty(r); }
                for (int c = 0; c <= cx; c++) screen[cy][c] = ' ';
                memset(cellAttr[cy], 0, cx + 1);
                markDirty(cy);
            }
            break;
        }
        case 'K': {
            int mode = csiParam(0, 0);
            if (mode == 0) { for (int c = cx; c < COLS; c++) screen[cy][c] = ' '; memset(cellAttr[cy]+cx, 0, COLS-cx); markDirty(cy); }
            else if (mode == 1) { for (int c = 0; c <= cx; c++) screen[cy][c] = ' '; memset(cellAttr[cy], 0, cx+1); markDirty(cy); }
            else if (mode == 2) { for (int c = 0; c < COLS; c++) screen[cy][c] = ' '; clearAttrRow(cy); markDirty(cy); cx = 0; }
            break;
        }
        case 'L': {
            int n = csiParam(0, 1);
            if (cy < scrollTop || cy > scrollBottom) break;
            int linesBelow = scrollBottom - cy + 1;
            if (n > linesBelow) n = linesBelow;
            for (int r = scrollBottom; r >= cy + n; r--) { memcpy(screen[r], screen[r-n], COLS+1); shiftAttrUp(r, r-n); }
            for (int r = cy; r < cy + n; r++) { for (int c = 0; c < COLS; c++) screen[r][c] = ' '; screen[r][COLS] = '\0'; clearAttrRow(r); }
            for (int r = cy; r <= scrollBottom; r++) markDirty(r);
            cx = 0; break;
        }
        case 'M': {
            int n = csiParam(0, 1);
            if (cy < scrollTop || cy > scrollBottom) break;
            int linesBelow = scrollBottom - cy + 1;
            if (n > linesBelow) n = linesBelow;
            for (int r = cy; r <= scrollBottom - n; r++) { memcpy(screen[r], screen[r+n], COLS+1); shiftAttrUp(r, r+n); }
            for (int r = scrollBottom - n + 1; r <= scrollBottom; r++) { for (int c = 0; c < COLS; c++) screen[r][c] = ' '; screen[r][COLS] = '\0'; clearAttrRow(r); }
            for (int r = cy; r <= scrollBottom; r++) markDirty(r);
            cx = 0; break;
        }
        case '@': {
            int cols = effCols();
            int n = csiParam(0, 1);
            if (n > cols - cx) n = cols - cx;
            for (int c = cols-1; c >= cx+n; c--) { screen[cy][c] = screen[cy][c-n]; cellAttr[cy][c] = cellAttr[cy][c-n]; }
            for (int c = cx; c < cx+n; c++) { screen[cy][c] = ' '; cellAttr[cy][c] = 0; }
            screen[cy][COLS] = '\0';
            markDirty(cy);
            break;
        }
        case 'P': {
            int cols = effCols();
            int n = csiParam(0, 1);
            if (n > cols - cx) n = cols - cx;
            for (int c = cx; c < cols-n; c++) { screen[cy][c] = screen[cy][c+n]; cellAttr[cy][c] = cellAttr[cy][c+n]; }
            for (int c = cols-n; c < cols; c++) { screen[cy][c] = ' '; cellAttr[cy][c] = 0; }
            screen[cy][COLS] = '\0';
            markDirty(cy);
            break;
        }
        case 'r': {
            int top = csiParam(0, 1) - 1;
            int bot = csiParam(1, ROWS) - 1;
            if (top < 0) top = 0;
            if (bot >= ROWS) bot = ROWS - 1;
            if (top > bot) { top = 0; bot = ROWS - 1; }
            scrollTop = top; scrollBottom = bot;
            cx = 0; cy = originMode ? scrollTop : 0;
            markDirty(cy);
            break;
        }
        case 'm': {
            if (csiNParams == 0 || (csiNParams == 1 && csiParams[0] == 0)) {
                curFg = 0xFFFF; curBg = 0x0000; curAttr = 0; break;
            }
            for (uint8_t i = 0; i < csiNParams; i++) {
                int p = csiParams[i];
                if (p == 0) { curFg = 0xFFFF; curBg = 0x0000; curAttr = 0; }
                else if (p == 1) { }                         // bold: ignored (no bright font)
                else if (p == 4) { curAttr |= ATTR_UNDERLINE; }
                else if (p == 5) { curAttr |= ATTR_BLINK; }
                else if (p == 7) { curAttr |= ATTR_REVERSE; }
                else if (p == 21 || p == 22) { }             // normal intensity
                else if (p == 24) { curAttr &= ~ATTR_UNDERLINE; }
                else if (p == 25) { curAttr &= ~ATTR_BLINK; }
                else if (p == 27) { curAttr &= ~ATTR_REVERSE; }
                else if (p >= 30 && p <= 37) curFg = ansiColors[p-30];
                else if (p >= 40 && p <= 47) curBg = ansiColors[p-40];
                else if (p >= 90 && p <= 97) curFg = ansiColors[p-90+8];
                else if (p >= 100 && p <= 107) curBg = ansiColors[p-100+8];
                else if (p == 38 || p == 48) {
                    // extended colour: 38;5;n (fg) or 48;5;n (bg), or 38;2;r;g;b
                    if (i + 1 < csiNParams && csiParams[i+1] == 5 && i + 2 < csiNParams) {
                        uint16_t col = xterm256(csiParams[i+2]);
                        if (p == 38) curFg = col; else curBg = col;
                        i += 2;
                    } else if (i + 4 < csiNParams && csiParams[i+1] == 2) {
                        uint8_t r = csiParams[i+2], g = csiParams[i+3], b = csiParams[i+4];
                        uint16_t col = ((uint16_t)(b >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (r >> 3);
                        if (p == 38) curFg = col; else curBg = col;
                        i += 4;
                    }
                }
            }
            break;
        }
        case 's': cx_saved = cx; cy_saved = cy; break;
        case 'u': cx = cx_saved; cy = cy_saved; break;
        case 'h': {
            if (privateMode) {
                for (uint8_t i = 0; i < csiNParams; i++) {
                    if (csiParams[i] == 6) originMode = true;
                    if (csiParams[i] == 7) autoWrap = true;
                    if (csiParams[i] == 25) cursorVisible = true;
                    if (csiParams[i] == 50) setDoubleWidth(false); // normal width
                    if (csiParams[i] == 51) setDoubleWidth(true);  // double width
                    if (csiParams[i] == 60) gfxEnter();              // vector graphics mode
                }
            }
            break;
        }
        case 'l': {
            if (privateMode) {
                for (uint8_t i = 0; i < csiNParams; i++) {
                    if (csiParams[i] == 6) originMode = false;
                    if (csiParams[i] == 7) autoWrap = false;
                    if (csiParams[i] == 25) cursorVisible = false;
                    if (csiParams[i] == 50 || csiParams[i] == 51) setDoubleWidth(false);
                    if (csiParams[i] == 60) gfxExit();               // back to text mode
                }
            }
            break;
        }
        default: break;
    }
}

static void handleEsc(char ch) {
    if (ch == '[') { pstate = ST_CSI; csiLen = 0; csiBuf[0] = '\0'; }
    else if (ch == ']') { pstate = ST_OSC; csiLen = 0; }
    else if (ch == '(' || ch == ')') { charsetG1 = (ch == ')'); pstate = ST_CHARSET; }
    else if (ch == 'c') {
        tft.fillScreen(0x0000);
        for (int r = 0; r < ROWS; r++) { for (int c = 0; c < COLS; c++) screen[r][c] = ' '; screen[r][COLS] = '\0'; clearAttrRow(r); }
        blinkTextPresent = false;
        cx = 0; cy = 0; curFg = 0xFFFF; curBg = 0x0000; curAttr = 0;
        scrollTop = 0; scrollBottom = ROWS - 1;
        originMode = false; autoWrap = true; cursorVisible = true;
        decGraphics = false; doubleWidth = false; graphicsMode = false;
        for (int r = 0; r < ROWS; r++) markDirty(r);
        pstate = ST_NORMAL;
    }
    else if (ch == '7') { cx_saved = cx; cy_saved = cy; pstate = ST_NORMAL; }
    else if (ch == '8') { cx = cx_saved; cy = cy_saved; pstate = ST_NORMAL; }
    else if (ch == 'M') { pstate = ST_NORMAL; }
    else { pstate = ST_NORMAL; }
}

static void handleOsc(char ch) {
    if (ch == '\a' || ch == '\x1B') { pstate = ST_NORMAL; }
}

// Echo only printable data typed in normal state.  Control bytes and any
// character consumed as part of an escape sequence (CSI/OSC params and final
// bytes are printable ASCII) must not be echoed back to the sender.
static bool shouldEcho(char ch) {
    uint8_t b = (uint8_t)ch;
    return pstate == ST_NORMAL && b >= 0x20 && b < 0x7F;
}

static void processChar(char ch) {
    switch (pstate) {
        case ST_NORMAL:
            if (ch == '\x1B') { pstate = ST_ESC; }
            else handleControl(ch);
            break;
        case ST_ESC:
            handleEsc(ch);
            if (pstate == ST_ESC) pstate = ST_NORMAL;
            break;
        case ST_CSI:
            if (ch >= 0x20 && ch <= 0x3F) {
                if (csiLen < sizeof(csiBuf) - 1) { csiBuf[csiLen++] = ch; csiBuf[csiLen] = '\0'; }
            } else if (ch >= 0x40 && ch <= 0x7E) {
                handleCsi(ch);
                pstate = ST_NORMAL;
            } else {
                pstate = ST_NORMAL;
            }
            break;
        case ST_OSC:
            handleOsc(ch);
            break;
        case ST_CHARSET:
            // ESC ( 0 = DEC Special Graphics, ESC ( B = ASCII.  G1 (ESC ))
            // designations are accepted but unused, so just consume them.
            if (!charsetG1) {
                if (ch == '0') decGraphics = true;
                else if (ch == 'B') decGraphics = false;
            }
            pstate = ST_NORMAL;
            break;
    }
}

void setup() {
    Serial.begin(9600);
    Serial3.begin(115200);
    delay(500);

    tft.begin(0x9481);
    tft.setRotation(1);
    tft.invertDisplay(true);
    tft.setTextSize(1);
    tft.fillScreen(0x0000);

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) screen[r][c] = ' ';
        screen[r][COLS] = '\0';
        clearAttrRow(r);
    }

    const char *msg = "VT102 Terminal Ready\r\nSerial3 (pins 14/15) 115200 baud\r\n";
    while (*msg) processChar(*msg++);

    // Redraw after welcome message
    if (needsRedraw && dirtyMin <= dirtyMax) {
        for (int r = dirtyMin; r <= dirtyMax; r++) {
            fastDrawRow(r, screen[r], cellAttr[r], curFg, curBg);
        }
        dirtyMin = ROWS;
        dirtyMax = -1;
        needsRedraw = false;
    }

    Serial.println("Terminal ready on Serial3 (pins 14/15).");
}

void loop() {
    unsigned long batchStart = millis();
    while (Serial3.available() && (millis() - batchStart < 25)) {
        char ch = Serial3.read();
        bool echo = shouldEcho(ch);  // check before parser state changes
        processChar(ch);
        if (echo) Serial3.write(ch); // echo printable data back to sender
        Serial.write(ch);            // mirror everything to debug port
    }
    while (Serial.available()) {
        char ch = Serial.read();
        bool echo = shouldEcho(ch);
        processChar(ch);
        if (echo) Serial.write(ch);  // echo printable data back to debug sender
        Serial3.write(ch);           // forward everything to terminal port
    }

    // Redraw dirty rows (debounced: only when no data pending and 20ms since last redraw)
    static unsigned long lastRedraw = 0;
    if (!graphicsMode && needsRedraw && dirtyMin <= dirtyMax && !Serial3.available() && !Serial.available() && millis() - lastRedraw > 20) {
        for (int r = dirtyMin; r <= dirtyMax; r++) {
            fastDrawRow(r, screen[r], cellAttr[r], curFg, curBg);
        }
        dirtyMin = ROWS;
        dirtyMax = -1;
        needsRedraw = false;
        lastRedraw = millis();
    }

    // cursor + SGR blink (skip during fast data)
    if (!graphicsMode && (cursorVisible || blinkTextPresent) && !Serial3.available() && !Serial.available() && millis() - lastBlink > 500) {
        lastBlink = millis();
        blinkPhase = !blinkPhase;
        cursorOn = !cursorOn;
        int cw = cellWidth();
        if (blinkTextPresent) {
            for (int r = 0; r < ROWS; r++) {
                for (int c = 0; c < effCols(); c++) {
                    if (cellAttr[r][c] & ATTR_BLINK) {
                        fastDrawChars(c, r, &screen[r][c], &cellAttr[r][c], 1,
                                      rowFg[r], rowBg[r], 0, blinkPhase, cw);
                    }
                }
            }
        }
        if (cursorVisible) {
            if (cursorOn) {
                fastDrawChars(cx, cy, " ", nullptr, 1, 0x0000, curFg, 0, false, cw);
            } else {
                fastDrawChars(cx, cy, &screen[cy][cx], &cellAttr[cy][cx], 1, curFg, curBg, 0, false, cw);
            }
        }
    }
}