/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* FM1 Open's panel painting (app/screen.c), for the shared MUMBLE screen code
 * (app/mumble_screen.inc) on fm1-base: the same colours, 5x7 font, damage
 * marking and 16-row bands, sent with fm1-base's lcd_blit. Part of app.c. */

enum {
    BACK = 0x0000,
    PANEL = 0x18e3,
    TEXT = 0xef5d,
    ACCENT = 0x07ff,
    DIM = 0x738e,
    GREEN = 0x07e0,
    AMBER = 0xfd20
};
static const uint8_t font[36][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0, 0x42, 0x7f, 0x40, 0},       {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1e}, {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36},
    {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41},
    {0x7f, 0x09, 0x09, 0x09, 0x01}, {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f},
    {0, 0x41, 0x7f, 0x41, 0},       {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41},
    {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f},
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e},
    {0x7f, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01},
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}};
static const uint8_t punctuation[][6] = {
    {45,8,8,8,8,8}, {47,0x60,0x18,6,1,0}, {43,8,8,0x3e,8,8},
    {35,0x14,0x7f,0x14,0x7f,0x14}, {37,0x63,0x13,8,0x64,0x63},
    {62,0x41,0x22,0x14,8,0}, {63,2,1,0x51,9,6},
    {33,0,0,0x5f,0,0}, {34,0,7,0,7,0}, {36,0x24,0x2a,0x7f,0x2a,0x12},
    {38,0x36,0x49,0x55,0x22,0x50}, {39,0,5,3,0,0}, {40,0,0x1c,0x22,0x41,0},
    {41,0,0x41,0x22,0x1c,0}, {42,0x14,8,0x3e,8,0x14}, {44,0,0x50,0x30,0,0},
    {46,0,0x60,0x60,0,0}, {58,0,0x36,0x36,0,0}, {59,0,0x56,0x36,0,0},
    {60,8,0x14,0x22,0x41,0}, {61,0x14,0x14,0x14,0x14,0x14},
    {64,0x32,0x49,0x79,0x41,0x3e}, {91,0,0x7f,0x41,0x41,0},
    {92,1,6,0x18,0x60,0}, {93,0,0x41,0x41,0x7f,0}, {94,4,2,1,2,4},
    {95,0x40,0x40,0x40,0x40,0x40}, {96,0,1,2,4,0}, {123,0,8,0x36,0x41,0},
    {124,0,0,0x7f,0,0}, {125,0,0x41,0x36,8,0}, {126,8,4,8,0x10,8}
};

/* Damage first, pixels second. With paint_rendering 0 the drawing calls only
 * mark the bands (and their columns) they would touch; then each damaged band
 * is drawn again into a 16-row buffer and sent. Two buffers take turns, so a
 * band is composed while the last one is still going out (lcd_blit is a DMA
 * the next LCD call waits for). The LCD takes RGB565 byte-swapped. */
enum { SCREEN_W = 240, SCREEN_H = 240, PAINT_ROWS = 16, PAINT_BANDS = SCREEN_H / PAINT_ROWS };
static uint16_t paint_buffers[2][SCREEN_W * PAINT_ROWS] PLAT_POOL;
static uint16_t *paint_pixels = paint_buffers[0];
static uint8_t paint_left[PAINT_BANDS], paint_right[PAINT_BANDS];
static unsigned paint_rendering, paint_top, paint_x, paint_end;

static uint16_t panel_order(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
static void paint_mark(unsigned x, unsigned y, unsigned w, unsigned h)
{
    for (unsigned b = y / PAINT_ROWS; b <= (y + h - 1) / PAINT_ROWS && b < PAINT_BANDS; ++b) {
        if (x < paint_left[b]) paint_left[b] = (uint8_t)x;
        if (x + w > paint_right[b]) paint_right[b] = (uint8_t)(x + w > SCREEN_W ? SCREEN_W : x + w);
    }
}
static __attribute__((noinline)) void paint_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color)
{
    if (!w || !h || x >= SCREEN_W || y >= SCREEN_H) return;
    if (w > SCREEN_W - x) w = SCREEN_W - x;
    if (h > SCREEN_H - y) h = SCREEN_H - y;
    if (!paint_rendering) { paint_mark(x, y, w, h); return; }
    if (x >= paint_end || x + w <= paint_x) return;
    unsigned right = x + w < paint_end ? x + w : paint_end;
    if (x < paint_x) x = paint_x;
    unsigned first = y < paint_top ? paint_top : y;
    unsigned end = y + h < paint_top + PAINT_ROWS ? y + h : paint_top + PAINT_ROWS;
    uint16_t c = panel_order(color);
    for (unsigned row = first; row < end; ++row)
        for (unsigned col = x; col < right; ++col)
            paint_pixels[(row - paint_top) * (paint_end - paint_x) + col - paint_x] = c;
}
static void text(unsigned x, unsigned y, const char *s, unsigned scale, uint16_t color, uint16_t background)
{
    if (!s || !scale || scale > 4 || x >= SCREEN_W || y >= SCREEN_H || 7U * scale > SCREEN_H - y) return;
    unsigned w = 6U * scale, h = 7U * scale;
    if (paint_rendering && (y >= paint_top + PAINT_ROWS || y + h <= paint_top)) return;
    if (!paint_rendering) {
        unsigned n = 0, available = (SCREEN_W - x) / w;
        while (s[n]) ++n;
        if (n > available) n = available;
        if (n) paint_mark(x, y, n * w, h);
        return;
    }
    uint16_t fg = panel_order(color), bg = panel_order(background);
    for (; *s && x + w <= SCREEN_W; ++s, x += w) {
        if (x >= paint_end) break;
        if (x + w <= paint_x) continue;
        char letter = *s >= 'a' && *s <= 'z' ? (char)(*s - 'a' + 'A') : *s;
        static const uint8_t blank[5] = {0};
        const uint8_t *shape = blank;
        if (*s >= '0' && *s <= '9')
            shape = font[*s - '0'];
        else if (letter >= 'A' && letter <= 'Z')
            shape = font[10 + letter - 'A'];
        else if (*s != ' ')
            for (unsigned i = 0; i < sizeof punctuation / sizeof punctuation[0]; ++i)
                if ((uint8_t)*s == punctuation[i][0]) {
                    shape = punctuation[i] + 1;
                    break;
                }
        unsigned first_row = y < paint_top ? paint_top : y;
        unsigned end_row = y + h < paint_top + PAINT_ROWS ? y + h : paint_top + PAINT_ROWS;
        unsigned first_col = x < paint_x ? paint_x : x;
        unsigned end_col = x + w < paint_end ? x + w : paint_end;
        unsigned stride = paint_end - paint_x;
        for (unsigned row = first_row; row < end_row; ++row) {
            unsigned mask = 1U << ((row - y) / scale);
            uint16_t *pixel = paint_pixels + (row - paint_top) * stride + first_col - paint_x;
            for (unsigned col = first_col; col < end_col; ++col) {
                unsigned column = (col - x) / scale;
                *pixel++ = column < 5U && (shape[column] & mask) ? fg : bg;
            }
        }
    }
}

/* The draw cache: what each part showed, so unchanged parts mark nothing. */
struct draw_cache {
    uint32_t hint_cache, foot_cache[4], voice_cache[8];
};
static struct draw_cache saved_cache, band_cache, *cache = &saved_cache;
static uint32_t signature(uint32_t seed, const void *bytes, unsigned n)
{
    /* FNV-1a, the seed multiplied in first. A plain "h * 33 + byte" let
     * neighbouring bytes cancel (+5 then -165), and a seed XORed in raw
     * cancelled against the first byte (version ^ 1, head + 1). */
    const uint8_t *p = bytes;
    uint32_t h = (seed ^ 2166136261U) * 16777619U;
    for (unsigned i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619U;
    return h;
}
static __attribute__((noinline)) void decimal(char out[12], int value, unsigned digits)
{
    unsigned n = value < 0 ? (unsigned)-value : (unsigned)value, length = 1;
    for (unsigned v = n; v >= 10; v /= 10) ++length;
    if (length < digits) length = digits;
    unsigned start = value < 0;
    if (start) out[0] = '-';
    out[start + length] = 0;
    while (length) {
        out[start + --length] = (char)('0' + n % 10);
        n /= 10;
    }
}
static unsigned paint_strlen(const char *s) { unsigned n = 0; while (s[n]) ++n; return n; }
static void note_name(unsigned note, char out[12])
{
    static const char names[] = "CCDDEFFGGAAB";
    static const uint16_t sharps = (1U << 1) | (1U << 3) | (1U << 6) | (1U << 8) | (1U << 10);
    unsigned pitch = note % 12, n = 1;
    out[0] = names[pitch];
    if (sharps & (1U << pitch)) out[n++] = '#';
    char oct[12];
    decimal(oct, (int)(note / 12) - 1, 1);
    for (unsigned i = 0; oct[i]; ++i) out[n++] = oct[i];
    out[n] = 0;
}
/* One of the four knobs along the foot: its label, value and colour. */
static void knob(unsigned col, const char *label, const char *value, uint16_t color)
{
    if (paint_rendering && (paint_top >= 227 || paint_top + PAINT_ROWS <= 195)) return;
    uint32_t sig = signature(signature(color, label, paint_strlen(label)), value, paint_strlen(value));
    if (cache->foot_cache[col] == sig) return;
    cache->foot_cache[col] = sig;
    unsigned x = 6 + 59 * col;
    paint_fill(x, 195, 54, 32, BACK);
    paint_fill(x, 195, 54, 2, color);
    text(x, 200, label, 1, TEXT, BACK);
    text(x, 212, value, paint_strlen(value) <= 4 ? 2 : 1, color, BACK);
}

/* Draw: mark the damage, then compose and send each damaged band. */
static void paint_frame(void (*draw)(void))
{
    paint_rendering = 0;
    cache = &saved_cache;
    draw();
    paint_rendering = 1;
    cache = &band_cache;
    for (unsigned band = 0; band < PAINT_BANDS; ++band) {
        if (paint_left[band] >= paint_right[band]) continue;
        paint_top = band * PAINT_ROWS;
        paint_x = paint_left[band];
        paint_end = paint_right[band];
        memset(cache, 0xff, sizeof *cache);
        /* The band starts as background: what the page does not paint is black. */
        for (unsigned i = 0, n = (paint_end - paint_x) * PAINT_ROWS; i < n; ++i) paint_pixels[i] = panel_order(BACK);
        draw();
        lcd_blit(paint_x, paint_top, paint_end - paint_x, PAINT_ROWS, paint_pixels);
        paint_pixels = paint_pixels == paint_buffers[0] ? paint_buffers[1] : paint_buffers[0];
        paint_left[band] = SCREEN_W;
        paint_right[band] = 0;
    }
    paint_rendering = 0;
    cache = &saved_cache;
}
/* Everything is damaged: the next frame paints the whole screen. */
static void paint_all(void)
{
    memset(&saved_cache, 0xff, sizeof saved_cache);
    for (unsigned b = 0; b < PAINT_BANDS; ++b) { paint_left[b] = 0; paint_right[b] = SCREEN_W; }
}
