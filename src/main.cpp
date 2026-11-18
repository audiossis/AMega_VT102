#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include "termfont.h"

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
                          bool blinkPhase = false) {
    if (n <= 0) return;
    if (n > PIXBUF_CHARS) n = PIXBUF_CHARS;
    int x = col * CHAR_W;
    int y = row * CHAR_H;
    int w = n * CHAR_W + extraBg;
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
            for (int i = 0; i < CHAR_W; i++) { // 5 font columns + 1 gap
                uint8_t line = 0;
                if (i < 5) line = pgm_read_byte(&term_font[c * 5 + i]);
                *p++ = (underline || (line & bit)) ? cellFg : cellBg;
            }
        }
        for (int k = 0; k < extraBg; k++) *p++ = bg;
    }
    tft.pushColors(pixbuf, (n * CHAR_W + extraBg) * CHAR_H, true);
}

static void fastDrawRow(int row, const char *str, const uint8_t *attr,
                        uint16_t fg, uint16_t bg, bool blinkPhase = false) {
    const int remainder = TERM_W - COLS * CHAR_W; // right edge not covered by whole cells
    for (int c = 0; c < COLS; c += PIXBUF_CHARS) {
        int n = COLS - c;
        if (n > PIXBUF_CHARS) n = PIXBUF_CHARS;
        int extra = (c + n == COLS) ? remainder : 0;
        fastDrawChars(c, row, str + c, attr ? attr + c : nullptr,
                      n, fg, bg, extra, blinkPhase);
    }
}

enum ParseState { ST_NORMAL, ST_ESC, ST_CSI, ST_OSC };
static ParseState pstate = ST_NORMAL;
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
    if (cx >= COLS) {
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
            cx = COLS - 1;
        }
    }
    screen[cy][cx] = ch;
    cellAttr[cy][cx] = curAttr;
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
        case '\t': cx = (cx + 8) & ~7; if (cx >= COLS) cx = COLS - 1; break;
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
    switch (final) {
        case 'H': case 'f': {
            int r = csiParam(0, 1) - 1; int c = csiParam(1, 1) - 1;
            if (originMode) { r += scrollTop; if (r > scrollBottom) r = scrollBottom; }
            if (r < 0) r = 0; if (r >= ROWS) r = ROWS - 1;
            if (c < 0) c = 0; if (c >= COLS) c = COLS - 1;
            cx = c; cy = r; markDirty(cy); break;
        }
        case 'A': { int n = csiParam(0,1); cy -= n; if (cy < scrollTop) cy = scrollTop; markDirty(cy); break; }
        case 'B': { int n = csiParam(0,1); cy += n; if (cy > scrollBottom) cy = scrollBottom; markDirty(cy); break; }
        case 'C': { int n = csiParam(0,1); cx += n; if (cx >= COLS) cx = COLS-1; break; }
        case 'D': { int n = csiParam(0,1); cx -= n; if (cx < 0) cx = 0; break; }
        case 'J': {
            int mode = csiParam(0, 0);
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
            int n = csiParam(0, 1);
            if (n > COLS - cx) n = COLS - cx;
            for (int c = COLS-1; c >= cx+n; c--) { screen[cy][c] = screen[cy][c-n]; cellAttr[cy][c] = cellAttr[cy][c-n]; }
            for (int c = cx; c < cx+n; c++) { screen[cy][c] = ' '; cellAttr[cy][c] = 0; }
            screen[cy][COLS] = '\0';
            markDirty(cy);
            break;
        }
        case 'P': {
            int n = csiParam(0, 1);
            if (n > COLS - cx) n = COLS - cx;
            for (int c = cx; c < COLS-n; c++) { screen[cy][c] = screen[cy][c+n]; cellAttr[cy][c] = cellAttr[cy][c+n]; }
            for (int c = COLS-n; c < COLS; c++) { screen[cy][c] = ' '; cellAttr[cy][c] = 0; }
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
    else if (ch == 'c') {
        tft.fillScreen(0x0000);
        for (int r = 0; r < ROWS; r++) { for (int c = 0; c < COLS; c++) screen[r][c] = ' '; screen[r][COLS] = '\0'; clearAttrRow(r); }
        blinkTextPresent = false;
        cx = 0; cy = 0; curFg = 0xFFFF; curBg = 0x0000; curAttr = 0;
        scrollTop = 0; scrollBottom = ROWS - 1;
        originMode = false; autoWrap = true; cursorVisible = true;
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
        processChar(ch);
        Serial.write(ch);
    }
    while (Serial.available()) {
        char ch = Serial.read();
        processChar(ch);
        Serial3.write(ch);
    }

    // Redraw dirty rows (debounced: only when no data pending and 20ms since last redraw)
    static unsigned long lastRedraw = 0;
    if (needsRedraw && dirtyMin <= dirtyMax && !Serial3.available() && !Serial.available() && millis() - lastRedraw > 20) {
        for (int r = dirtyMin; r <= dirtyMax; r++) {
            fastDrawRow(r, screen[r], cellAttr[r], curFg, curBg);
        }
        dirtyMin = ROWS;
        dirtyMax = -1;
        needsRedraw = false;
        lastRedraw = millis();
    }

    // cursor + SGR blink (skip during fast data)
    if ((cursorVisible || blinkTextPresent) && !Serial3.available() && !Serial.available() && millis() - lastBlink > 500) {
        lastBlink = millis();
        blinkPhase = !blinkPhase;
        cursorOn = !cursorOn;
        if (blinkTextPresent) {
            for (int r = 0; r < ROWS; r++) {
                for (int c = 0; c < COLS; c++) {
                    if (cellAttr[r][c] & ATTR_BLINK) {
                        fastDrawChars(c, r, &screen[r][c], &cellAttr[r][c], 1,
                                      rowFg[r], rowBg[r], 0, blinkPhase);
                    }
                }
            }
        }
        if (cursorVisible) {
            if (cursorOn) {
                fastDrawChars(cx, cy, " ", nullptr, 1, 0x0000, curFg);
            } else {
                fastDrawChars(cx, cy, &screen[cy][cx], &cellAttr[cy][cx], 1, curFg, curBg);
            }
        }
    }
}