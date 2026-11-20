/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* VOICE page (HOME+ARP): the MUMBLE retro-game voice synthesizer.
 *
 * Every cast member is a track with its own line, sound and effects. Tracks
 * that join the choir speak (or sing) along with the selected one.
 *
 * The 27 note keys are A..Z and SPACE, lowest key first. GLO switches mode:
 *   TYPE  keys type the selected track's line. A vowel speaks the moment it
 *         is typed; consonants shape how the next vowel starts.
 *   PLAY  keys sing their letter's name ("bee", "kay", "em") at the key's
 *         pitch for as long as they are held; SPACE hums. The choir sings
 *         along, each member in their own register.
 *
 *   PLAY speak (choir) / stop  OCT- delete       OCT+ CAPS (shout)
 *   FX ,  SCL .  ENV ?  LFO !  EDIT clear line, again on an empty line: all
 *   SAVE join/leave choir      REC loop          ARP demo line
 *   SELECT character (track)   K1/SEQ page: VOICE, FEEL, FX   K2..K5 sound
 *   PRESETS browse demo lines  GLO TYPE/PLAY     HOME exit
 *   in PLAY, OCT-/OCT+ shift the octave
 *
 * Included by screen.c: uses its paint helpers and draw cache. */
#include "mumble.h"

enum { VOICE_TRACE = 21, VOICE_ROWS = 3, VOICE_COLUMNS = 18 };
#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
enum {
    WHITE = 0xffff,
    SKIN = RGB(250, 196, 150),
    CHEEK = RGB(240, 110, 120),
    GOLD = RGB(255, 200, 40),
    STEEL = RGB(70, 80, 96),
    INK = RGB(20, 18, 30),
    HAIR = RGB(120, 70, 40),
    PLUME = RGB(220, 40, 50),
    EMBER = RGB(255, 60, 40),
};
static const uint16_t voice_knob_colors[4] = {0x2b7b, 0x2d8d, 0xf7be, 0xf325};
static const uint16_t vowel_colors[MUMBLE_VOWELS] = {0xf325, 0x2d8d, 0x2b7b, 0xf7be, 0xb47f, 0xfe87};
/* Mouth half-width/half-height per vowel, fully open. */
static const uint8_t vowel_mouth[MUMBLE_VOWELS][2] = {{7, 6}, {8, 4}, {9, 2}, {5, 6}, {3, 4}, {6, 4}};
static const char *const voice_page_names[MUMBLE_PAGES] = {"VOICE", "FEEL", "FX"};
static const char *const voice_greetings[MUMBLE_CAST] = {
    "Hey there!", "Halt.", "Bless you, child.", "Hiya! Hiya!", "Arr...", "Hmm, yes... yes.",
    "Hello. Hello.", "Whooo..."};
static const char *const voice_lines[] = {
    "Well, hello there! Have you seen Sister Agnes?",
    "The bridge is closed. Nobody gets through... nobody.",
    "Arr, the sea be calm tonight, matey.",
    "Ooh! Can I come too? Please? PLEASE?",
    "Back in my day, we walked uphill. Both ways.",
    "Beep boop. Your request is being processed.",
    "You found the moon key! Wait... what moon key?",
    "Bread, cheese, a lantern. That will be nine coins.",
    "Who goes there? Is it... dinner time?"};
enum { VOICE_LINES = sizeof voice_lines / sizeof voice_lines[0] };
static const char *const voice_hints[] = {
    "KEYS TYPE  PLAY SPEAK  HOME EXIT",
    "FX ,  SCL .  ENV ?  LFO !  OCT+ CAPS",
    "OCT- DEL  EDIT CLEAR  ARP DEMO LINE",
    "SAVE CHOIR  REC LOOP  GLO PLAY MODE",
    "SELECT WHO  K1 PAGE  PRESETS LINES"};
static const char *const voice_play_hints[] = {
    "HOLD KEYS TO SING  GLO TYPE MODE",
    "OCT- OCT+ OCTAVE  SELECT WHO",
    "SAVE: CHOIR SINGS ALONG  HOME EXIT"};

struct voice_track {
    char text[MUMBLE_TEXT + 1];
    uint8_t length, choir;
    struct mumble_voice sound;
};
static struct voice_track voice_tracks[MUMBLE_CAST];
static unsigned voice_cast, voice_page, voice_caps, voice_demo, voice_ready;
/* A pass is 1 = requested, 2 = heard speaking; it ends when speaking stops. */
static unsigned voice_loop, voice_looping, voice_pass;
static struct mumble_status voice_now;
static struct { uint8_t vowel, level, track; int16_t cents; } voice_trace[VOICE_TRACE];
static uint32_t voice_trace_ms;
static uint32_t voice_keys_lit;
/* PLAY mode: held keys, octave shift, and the last letter sung. */
static unsigned voice_play, voice_sung = MUMBLE_LETTERS, voice_sung_note;
static int voice_octave;
static uint32_t voice_held;

static struct voice_track *voice_track(void) { return &voice_tracks[voice_cast]; }
static void voice_set_text(struct voice_track *t, const char *s)
{
    unsigned n = (unsigned)strlen(s);
    if (n > MUMBLE_TEXT) n = MUMBLE_TEXT;
    memcpy(t->text, s, n);
    t->text[n] = 0;
    t->length = (uint8_t)n;
}
static void voice_open(void)
{
    if (!voice_ready) {
        mumble_init();
        for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
            voice_tracks[i].sound = mumble_cast[i].voice;
            voice_set_text(&voice_tracks[i], "");
            voice_tracks[i].choir = 0;
        }
        voice_cast = 0;
        voice_ready = 1;
    }
    voice_page = 0;
    voice_play = 0;
    voice_held = 0;
    voice_looping = 0;
    memset(voice_trace, 0, sizeof voice_trace);
    for (unsigned i = 0; i < VOICE_TRACE; ++i) voice_trace[i].vowel = MUMBLE_VOWELS;
}
static void voice_close(void)
{
    if (voice_ready) mumble_stop(); /* also releases held letters */
    voice_held = 0;
    voice_looping = 0;
}
static void voice_type(char c)
{
    struct voice_track *t = voice_track();
    if (t->length >= MUMBLE_TEXT) {
        memmove(t->text, t->text + 1, MUMBLE_TEXT - 1);
        t->length = MUMBLE_TEXT - 1;
    }
    t->text[t->length++] = c;
    t->text[t->length] = 0;
    /* While a loop plays, typing is silent: the next pass speaks the new text. */
    if (!voice_looping) mumble_type(voice_cast, t->text, t->length, &t->sound);
}
static unsigned voice_in_choir(unsigned i) { return i == voice_cast || voice_tracks[i].choir; }
/* The selected track and every choir member with a line speak together. */
static void voice_speak(void)
{
    struct voice_track *t = voice_track();
    if (!t->length) voice_set_text(t, voice_lines[voice_demo % VOICE_LINES]);
    struct mumble_part parts[MUMBLE_CAST];
    memset(parts, 0, sizeof parts);
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        if (!voice_in_choir(i) || !voice_tracks[i].length) continue;
        parts[i].text = voice_tracks[i].text;
        parts[i].length = voice_tracks[i].length;
        parts[i].voice = &voice_tracks[i].sound;
    }
    mumble_choir(parts, voice_cast);
    voice_looping = voice_loop;
    voice_pass = 1;
}
static void voice_stop(void)
{
    voice_looping = 0;
    voice_pass = 0;
    mumble_stop();
    voice_held = 0;
}
static unsigned voice_parameter(unsigned col) { return voice_page * 4U + col; }
static void voice_edit(unsigned col, int delta)
{
    unsigned p = voice_parameter(col);
    const struct mumble_param_info *info = &mumble_params[p];
    struct mumble_voice *v = &voice_track()->sound;
    v->value[p] = (uint16_t)clamp((int)v->value[p] + delta, info->min, info->max);
}
static unsigned voice_edited(void)
{
    return memcmp(&voice_track()->sound, &mumble_cast[voice_cast].voice, sizeof(struct mumble_voice)) != 0;
}
static void voice_release_all(void)
{
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key)
        if (voice_held & (1U << key)) mumble_release(key);
    voice_held = 0;
}
/* PLAY mode: the selected track and the choir sing each pressed key. */
static void voice_sing(const struct fm1_controls *c, uint32_t down)
{
    uint32_t up = voice_held & ~c->keys;
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key)
        if (up & (1U << key)) mumble_release(key);
    voice_held &= c->keys;
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key) {
        if (!(down & (1U << key))) continue;
        struct mumble_singer singers[MUMBLE_CAST];
        unsigned n = 0;
        int note = 0;
        for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
            /* The selected voice leads. */
            unsigned who = (voice_cast + i) % MUMBLE_CAST;
            if (!voice_in_choir(who)) continue;
            const struct mumble_voice *v = &voice_tracks[who].sound;
            int pitch = clamp(53 + (int)key + 12 * (voice_octave + mumble_register(v)), 0, 127); /* lowest key is F3 */
            if (!n) note = pitch;
            singers[n].voice = v;
            singers[n].track = (uint8_t)who;
            singers[n].note = (uint8_t)pitch;
            ++n;
        }
        mumble_press(key, singers, n, voice_caps);
        voice_held |= 1U << key;
        voice_sung = key;
        voice_sung_note = (unsigned)note;
    }
}

/* Returns 0 when a MIDI post must be retried, as apply_controls does. */
static int voice_controls(const struct fm1_controls *c, uint32_t pressed, uint32_t key_down)
{
    if (pressed & BUTTON(HOME)) { voice_close(); page = OVERVIEW; return 1; }
    if (c->encoders[0]) {
        voice_release_all();
        voice_cast = (unsigned)((int)voice_cast + c->encoders[0] % MUMBLE_CAST + MUMBLE_CAST) % MUMBLE_CAST;
        if (!voice_looping) {
            const char *hello = voice_greetings[voice_cast];
            mumble_aside(voice_cast, hello, (unsigned)strlen(hello), &voice_track()->sound);
        }
    }
    if (c->encoders[1]) voice_page = (unsigned)((int)voice_page + c->encoders[1] % MUMBLE_PAGES + MUMBLE_PAGES) % MUMBLE_PAGES;
    if (pressed & BUTTON(SEQ)) voice_page = (voice_page + 1U) % MUMBLE_PAGES;
    for (unsigned i = 0; i < 4; ++i)
        if (c->encoders[2 + i]) voice_edit(i, fm_encoder_delta(2 + i, c->encoders[2 + i]));
    if (c->encoders[6]) {
        voice_demo = (unsigned)((int)voice_demo + c->encoders[6] % (int)VOICE_LINES + (int)VOICE_LINES) % VOICE_LINES;
        voice_set_text(voice_track(), voice_lines[voice_demo]);
        if (!voice_looping) mumble_stop();
    }
    if (pressed & BUTTON(GLO)) { voice_play ^= 1U; voice_release_all(); }
    if (pressed & BUTTON(SAVE)) {
        struct voice_track *t = voice_track();
        t->choir ^= 1U;
        notice = t->choir ? "JOINED THE CHOIR" : "LEFT THE CHOIR";
        notice_until = frame_time + 1500U;
    }
    if (pressed & BUTTON(REC)) {
        voice_loop ^= 1U;
        struct mumble_status s;
        mumble_status(&s);
        voice_looping = voice_loop && s.speaking;
    }
    if (voice_play) {
        if ((pressed & BUTTON(OCT_UP)) && voice_octave < 2) ++voice_octave;
        if ((pressed & BUTTON(OCT_DOWN)) && voice_octave > -2) --voice_octave;
    } else {
        struct voice_track *t = voice_track();
        if (pressed & BUTTON(OCT_UP)) voice_caps ^= 1U;
        if ((pressed & BUTTON(OCT_DOWN)) && t->length) {
            t->text[--t->length] = 0;
            if (!voice_looping) mumble_stop();
        }
    }
    if (pressed & BUTTON(EDIT)) {
        /* Clear the line; on an empty line, clear every track. */
        struct voice_track *t = voice_track();
        if (t->length) voice_set_text(t, "");
        else {
            for (unsigned i = 0; i < MUMBLE_CAST; ++i) voice_set_text(&voice_tracks[i], "");
            notice = "ALL LINES CLEARED";
            notice_until = frame_time + 1500U;
        }
        voice_stop();
    }
    if (pressed & BUTTON(ARP)) {
        voice_demo = (voice_demo + 1U) % VOICE_LINES;
        voice_set_text(voice_track(), voice_lines[voice_demo]);
        voice_speak();
    }
    static const uint8_t marks[4][2] = {{FX, ','}, {SCL, '.'}, {ENV, '?'}, {LFO, '!'}};
    for (unsigned i = 0; i < 4 && !voice_play; ++i)
        if (pressed & BUTTON(marks[i][0])) voice_type((char)marks[i][1]);
    uint32_t typed = key_down & ~blocked_keys;
    if (voice_play) voice_sing(c, typed);
    else
        for (unsigned key = 0; key < 27; ++key)
            if (typed & (1U << key))
                voice_type(key == 26 ? ' ' : (char)((voice_caps ? 'A' : 'a') + key));
    voice_keys_lit = c->keys;
    if (pressed & BUTTON(PLAY)) {
        struct mumble_status s;
        mumble_status(&s);
        if (s.speaking || voice_looping) voice_stop();
        else voice_speak();
    }
    return 1;
}

/* Note-key LEDs mark the vowels; held keys invert theirs. */
static uint32_t voice_led_bits(void)
{
    uint32_t vowels = (1U << 0) | (1U << 4) | (1U << 8) | (1U << 14) | (1U << 20) | (1U << 24);
    return vowels ^ (voice_keys_lit & ((1U << 27) - 1U));
}
static uint32_t voice_button_bits(void)
{
    uint32_t octave = voice_play ? ((voice_octave > 0) << OCT_UP) | ((voice_octave < 0) << OCT_DOWN)
                                 : voice_caps << OCT_UP;
    return ((voice_now.speaking || voice_looping) << PLAY) | octave | (voice_play << GLO) |
           ((uint32_t)voice_track()->choir << SAVE) | (voice_loop << REC) | (1U << ARP);
}

/* Control-side snapshot, taken once per frame before marking damage. The
 * loop restarts here, so each pass picks up lines edited meanwhile. */
static void voice_frame(uint32_t now)
{
    mumble_status(&voice_now);
    /* The audio task adopts a new line a block later: only a pass that was
     * heard speaking can end, or the loop would restart it every frame. */
    if (voice_pass == 1 && voice_now.speaking) voice_pass = 2;
    else if (voice_pass == 2 && !voice_now.speaking) {
        voice_pass = 0;
        if (voice_looping) voice_speak();
    }
    if (now - voice_trace_ms < 50U) return;
    voice_trace_ms = now;
    memmove(voice_trace, voice_trace + 1, sizeof voice_trace - sizeof voice_trace[0]);
    voice_trace[VOICE_TRACE - 1].vowel = voice_now.vowel;
    voice_trace[VOICE_TRACE - 1].level = voice_now.level;
    voice_trace[VOICE_TRACE - 1].track = voice_now.track;
    voice_trace[VOICE_TRACE - 1].cents = voice_now.cents;
}

/* ------------------------------------------------------------------------
 * Drawing primitives clipped to a box, so a character never spills into
 * the neighbouring cached areas.
 * ---------------------------------------------------------------------- */
static int clip_x0, clip_y0, clip_x1, clip_y1;
static void voice_clip(int x0, int y0, int x1, int y1) { clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1; }
static void vfill(int x, int y, int w, int h, uint16_t color)
{
    if (x < clip_x0) { w -= clip_x0 - x; x = clip_x0; }
    if (y < clip_y0) { h -= clip_y0 - y; y = clip_y0; }
    if (x + w > clip_x1) w = clip_x1 - x;
    if (y + h > clip_y1) h = clip_y1 - y;
    if (w > 0 && h > 0) paint_fill((unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h, color);
}
static unsigned isqrt(unsigned v)
{
    unsigned r = 0;
    while ((r + 1) * (r + 1) <= v) ++r;
    return r;
}
static void vellipse(int cx, int cy, int rx, int ry, uint16_t color)
{
    if (rx < 0 || ry < 0) return;
    if (!ry) { vfill(cx - rx, cy, 2 * rx + 1, 1, color); return; }
    for (int dy = -ry; dy <= ry; ++dy) {
        int hw = (int)isqrt((unsigned)(rx * rx) * ((unsigned)(ry * ry - dy * dy) + (unsigned)ry / 2U) / (unsigned)(ry * ry));
        if (hw > rx) hw = rx;
        vfill(cx - hw, cy + dy, 2 * hw + 1, 1, color);
    }
}
static void vtri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    int t;
#define VSWAP(a, b) (t = (a), (a) = (b), (b) = t)
    if (y1 < y0) { VSWAP(x0, x1); VSWAP(y0, y1); }
    if (y2 < y0) { VSWAP(x0, x2); VSWAP(y0, y2); }
    if (y2 < y1) { VSWAP(x1, x2); VSWAP(y1, y2); }
#undef VSWAP
    for (int y = y0; y <= y2; ++y) {
        int a = y2 > y0 ? x0 + (x2 - x0) * (y - y0) / (y2 - y0) : x0, b;
        if (y < y1) b = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        else if (y2 > y1) b = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        else b = x1;
        if (a > b) { int s = a; a = b; b = s; }
        vfill(a, y, b - a + 1, 1, color);
    }
}
static void vline(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        vfill(x0, y0, 1, 1, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static uint16_t dim(uint16_t c) { return (uint16_t)((c >> 1) & 0x7befU); }
static uint16_t dimmer(uint16_t c) { return (uint16_t)((c >> 2) & 0x39e7U); }
/* Triangle wave in -amp..amp over `period` ms, offset by `phase` ms. */
static int voice_wave(unsigned period, int amp, unsigned phase)
{
    unsigned p = (frame_time + phase) % period;
    int x = (int)(p * 4U * (unsigned)amp / period);
    return x < 2 * amp ? x - amp : 3 * amp - x;
}

/* ------------------------------------------------------------------------
 * The cast. Flat shapes on black, OP-1 style; bold silhouettes and
 * expressive eyes after Hollow Knight, Undertale and Animal Well.
 * ---------------------------------------------------------------------- */
struct pose {
    int x, y;            /* head centre */
    unsigned open;       /* mouth 0..4 */
    unsigned vowel, blink, shout;
    uint16_t color;
};
static void mouth(const struct pose *p, int x, int y, unsigned scale, uint16_t color)
{
    if (!p->open) { vfill(x - (int)scale * 2, y, (int)scale * 4 + 1, 1, color); return; }
    const uint8_t *m = vowel_mouth[p->vowel < MUMBLE_VOWELS ? p->vowel : MUMBLE_U];
    vellipse(x, y, (int)(m[0] * scale / 2U), (int)(m[1] * scale * p->open / 8U) + 1, color);
}

static void draw_hero(const struct pose *p)
{
    vellipse(p->x, p->y + 34, 24, 14, p->color);                   /* cloak */
    vtri(p->x - 8, p->y + 18, p->x + 8, p->y + 18, p->x, p->y + 30, dim(p->color));
    for (int s = -1; s <= 1; s += 2) {                               /* horns */
        vtri(p->x + s * 6, p->y - 12, p->x + s * 15, p->y - 10, p->x + s * 19, p->y - 36, WHITE);
        vtri(p->x + s * 19, p->y - 36, p->x + s * 21, p->y - 34, p->x + s * 15, p->y - 22, WHITE);
    }
    vellipse(p->x, p->y, 20, 17, WHITE);                             /* mask */
    for (int s = -1; s <= 1; s += 2) {
        if (p->blink) vfill(p->x + s * 8 - 3, p->y + 1, 7, 2, INK);
        else {
            vellipse(p->x + s * 8, p->y + 1, 4, p->shout ? 5 : 7, INK);
            vfill(p->x + s * 8 - 1, p->y - 3, 1, 2, STEEL);
        }
    }
    if (p->open) mouth(p, p->x, p->y + 12, 1, INK);
}

static void draw_guard(const struct pose *p)
{
    for (int s = -1; s <= 1; s += 2) vellipse(p->x + s * 22, p->y + 30, 12, 9, STEEL); /* pauldrons */
    vfill(p->x - 14, p->y + 22, 28, 20, dim(p->color));
    vtri(p->x - 2, p->y - 22, p->x + 4, p->y - 22, p->x + 24, p->y - 40, PLUME);
    vellipse(p->x + 14, p->y - 32, 8, 5, PLUME);
    vellipse(p->x, p->y - 2, 21, 23, p->color);                      /* helm */
    vfill(p->x - 21, p->y + 10, 43, 8, p->color);
    vfill(p->x - 16, p->y - 5, 33, 6, INK);                          /* visor */
    if (!p->blink)
        for (int s = -1; s <= 1; s += 2) vfill(p->x + s * 8 - 2, p->y - 3, 5, 2, p->shout ? EMBER : GOLD);
    vfill(p->x - 1, p->y - 5, 3, 15, dim(p->color));                 /* nose guard */
    for (int i = 0; i < 3; ++i) vfill(p->x - 10 + i * 8, p->y + 12, 4, 1, dim(p->color)); /* vents */
    if (p->open) vfill(p->x - 6, p->y + 14, 13, (int)p->open + 1, INK);
}

static void draw_sister(const struct pose *p)
{
    vellipse(p->x, p->y - 32, 13, 3, GOLD);                          /* halo */
    vellipse(p->x, p->y - 32, 10, 1, BACK);
    vtri(p->x - 30, p->y + 44, p->x + 30, p->y + 44, p->x, p->y - 10, p->color); /* veil drape */
    vellipse(p->x, p->y - 2, 25, 25, p->color);
    vellipse(p->x, p->y + 2, 18, 20, WHITE);                         /* wimple */
    vellipse(p->x, p->y + 1, 13, 15, SKIN);
    for (int s = -1; s <= 1; s += 2) {
        int ex = p->x + s * 6;
        if (p->shout && !p->blink) vellipse(ex, p->y - 1, 2, 2, INK);
        else {                                                       /* serene, closed */
            vfill(ex - 2, p->y - 1, 5, 1, INK);
            vfill(ex - 3, p->y - 2, 1, 1, INK);
            vfill(ex + 3, p->y - 2, 1, 1, INK);
        }
        vfill(p->x + s * 9 - 1, p->y + 4, 3, 2, CHEEK);
    }
    mouth(p, p->x, p->y + 9, 1, RGB(150, 40, 50));
}

static void draw_kid(const struct pose *p)
{
    vellipse(p->x, p->y + 34, 18, 12, p->color);                     /* shirt */
    vellipse(p->x, p->y, 24, 22, SKIN);                              /* head */
    vellipse(p->x, p->y - 13, 24, 11, p->color);                     /* cap */
    vfill(p->x - 24, p->y - 13, 49, 4, p->color);
    vfill(p->x + 12, p->y - 10, 18, 4, dim(p->color));               /* brim */
    vtri(p->x - 6, p->y - 22, p->x + 2, p->y - 22, p->x - 10, p->y - 32, HAIR); /* cowlick */
    for (int s = -1; s <= 1; s += 2) {
        int ex = p->x + s * 9;
        if (p->blink) vfill(ex - 5, p->y + 1, 11, 2, INK);
        else {
            vellipse(ex, p->y + 1, 6, 7, WHITE);
            vellipse(ex + 1, p->y + 2, 3, p->shout ? 2 : 4, INK);
            vfill(ex - 1, p->y - 1, 2, 2, WHITE);
        }
        for (int f = 0; f < 3; ++f) vfill(ex + s * 4 + f * 2 - 3, p->y + 10 + (f & 1), 1, 1, HAIR);
    }
    mouth(p, p->x, p->y + 14, 2, INK);
}

static void draw_pirate(const struct pose *p)
{
    vellipse(p->x, p->y + 36, 24, 14, p->color);                     /* coat */
    vellipse(p->x, p->y + 2, 20, 20, SKIN);
    vellipse(p->x, p->y + 15, 20, 13, HAIR);                         /* beard */
    vellipse(p->x, p->y + 9, 9, 3, HAIR);                            /* moustache */
    int tilt = voice_wave(1600, 1, 0);
    vtri(p->x - 30, p->y - 10 + tilt, p->x + 30, p->y - 10 - tilt, p->x, p->y - 36, INK); /* tricorn */
    vfill(p->x - 22, p->y - 13, 45, 4, RGB(150, 30, 40));
    vellipse(p->x, p->y - 22, 3, 3, WHITE);                          /* skull */
    vfill(p->x - 1, p->y - 23, 1, 1, INK);
    vfill(p->x + 1, p->y - 23, 1, 1, INK);
    vline(p->x - 19, p->y - 7, p->x + 19, p->y + 4, INK);            /* patch strap */
    vellipse(p->x + 8, p->y, 5, 4, INK);                             /* patch */
    if (p->blink) vfill(p->x - 11, p->y, 7, 1, INK);
    else { vellipse(p->x - 8, p->y, 2, 3, INK); vfill(p->x - 9, p->y - 2, 1, 1, WHITE); }
    if (p->open) {
        mouth(p, p->x, p->y + 14, 2, INK);
        vfill(p->x + 2, p->y + 13, 2, 2, GOLD);                      /* gold tooth */
    } else vfill(p->x - 4, p->y + 14, 9, 1, INK);
}

static void draw_elder(const struct pose *p, int sway)
{
    vellipse(p->x, p->y + 38, 26, 14, p->color);                     /* robe */
    vellipse(p->x, p->y + 2, 16, 16, SKIN);
    vtri(p->x - 16, p->y + 6, p->x + 16, p->y + 6, p->x + sway, p->y + 44, WHITE); /* beard */
    vellipse(p->x, p->y + 8, 13, 5, WHITE);
    vtri(p->x - 26, p->y - 8, p->x + 26, p->y - 8, p->x + 8 + sway, p->y - 46, p->color); /* hat */
    vellipse(p->x, p->y - 8, 28, 4, p->color);
    vfill(p->x + 1, p->y - 26, 3, 1, GOLD);                          /* star */
    vfill(p->x + 2, p->y - 27, 1, 3, GOLD);
    for (int s = -1; s <= 1; s += 2) {
        vfill(p->x + s * 6 - 4, p->y - 3, 9, 3, WHITE);              /* brows */
        if (!p->blink) vfill(p->x + s * 6, p->y + 1, 2, 2, INK);
    }
    if (p->open) mouth(p, p->x, p->y + 11, 1, INK);
}

static void draw_bot(const struct pose *p)
{
    unsigned lamp = (frame_time / 450U) & 1U;
    vline(p->x, p->y - 20, p->x, p->y - 30, p->color);               /* antenna */
    vellipse(p->x, p->y - 32, 2, 2, lamp ? EMBER : dim(EMBER));
    vfill(p->x - 10, p->y + 18, 20, 4, dim(p->color));               /* neck */
    vfill(p->x - 22, p->y + 22, 44, 30, p->color);                   /* body */
    vfill(p->x - 6, p->y + 28, 12, 4, INK);
    vfill(p->x - 22, p->y - 19, 44, 38, p->color);                   /* head */
    vfill(p->x - 22, p->y - 19, 2, 2, BACK);
    vfill(p->x + 20, p->y - 19, 2, 2, BACK);
    vfill(p->x - 26, p->y - 4, 4, 8, dim(p->color));                 /* bolts */
    vfill(p->x + 22, p->y - 4, 4, 8, dim(p->color));
    vfill(p->x - 17, p->y - 14, 34, 28, INK);                        /* screen */
    for (int s = -1; s <= 1; s += 2)
        vfill(p->x + s * 7 - 3, p->y - 8, 6, p->blink ? 1 : 6, p->shout ? EMBER : ACCENT);
    /* An equaliser for a mouth. */
    for (int b = 0; b < 5; ++b) {
        int h = p->open ? 1 + (int)(p->open * (unsigned)(3 + (b * 5 + (int)(frame_time / 60U)) % 4) / 3U) : 1;
        if (h > 9) h = 9;
        vfill(p->x - 12 + b * 5, p->y + 10 - h, 3, h, p->open ? ACCENT : dim(ACCENT));
    }
}

static void draw_ghost(const struct pose *p)
{
    uint16_t glow = dimmer(p->color);
    vellipse(p->x, p->y + 2, 28, 30, glow);                          /* aura */
    vellipse(p->x, p->y - 4, 20, 20, p->color);
    vfill(p->x - 20, p->y - 4, 41, 26, p->color);
    int shift = (int)((frame_time / 120U) % 10U);                    /* drifting hem */
    for (int k = -1; k < 5; ++k) {
        int hx = p->x - 20 + k * 10 + shift;
        if (hx >= p->x - 22 && hx <= p->x + 22) vellipse(hx, p->y + 22, 5, 4, p->color);
        if (hx + 5 >= p->x - 20 && hx + 5 <= p->x + 20) vellipse(hx + 5, p->y + 23, 3, 3, glow);
    }
    for (int s = -1; s <= 1; s += 2) {
        if (p->blink) vfill(p->x + s * 7 - 3, p->y - 4, 7, 1, INK);
        else vellipse(p->x + s * 7, p->y - 4, 3, 5, INK);
    }
    if (p->open) vellipse(p->x, p->y + 9, 3 + (int)p->open, 2 + (int)p->open * 2, INK);
    else vellipse(p->x, p->y + 8, 2, 2, INK);
    int twinkle = (int)((frame_time / 300U) % 4U);                   /* sparkles */
    vfill(p->x - 26 + twinkle * 3, p->y - 22, 1, 1, WHITE);
    vfill(p->x + 24, p->y - 10 + twinkle * 2, 1, 1, twinkle & 1 ? WHITE : glow);
}

/* Layout: header 0..27, stage 30..103 (portrait 4..101, pitch roll 106..235),
 * choir strip 106..119, dialogue box 122..186, then knobs and hint. */
enum { STAGE_TOP = 30, STAGE_BOTTOM = 104, PORTRAIT_X1 = 102, STRIP_TOP = 106, STRIP_BOTTOM = 120,
       BOX_TOP = 122, BOX_BOTTOM = 187 };

static void voice_header(void)
{
    if (paint_rendering && paint_top >= 28) return;
    const struct mumble_character *who = &mumble_cast[voice_cast];
    uint32_t sig = voice_cast | (voice_page << 4) | (voice_caps << 6) | (voice_edited() << 7) |
                   (voice_play << 8) | (voice_loop << 9) | ((uint32_t)voice_track()->choir << 10);
    if (cache->voice_cache[0] == sig) return;
    cache->voice_cache[0] = sig;
    paint_fill(0, 0, 240, 28, BACK);
    text(8, 7, "MUMBLE", 2, TEXT, BACK);
    for (unsigned i = 0; i < 4; ++i) paint_fill(86 + i * 7, 6, 5, 5, voice_knob_colors[i]);
    if (voice_loop) text(120, 6, "LOOP", 1, ACCENT, BACK);
    text(86, 16, voice_page_names[voice_page], 1, DIM, BACK);
    if (voice_play) text(120, 16, "PLAY", 1, voice_caps ? AMBER : GREEN, BACK);
    else if (voice_caps) text(120, 16, "CAPS", 1, AMBER, BACK);
    unsigned n = (unsigned)strlen(who->name);
    unsigned x = 232 - n * 12U;
    text(x, 7, who->name, 2, who->color, BACK);
    char number[2] = {(char)('1' + voice_cast), 0};
    text(x - 9, 14, number, 1, voice_track()->choir ? who->color : DIM, BACK);
    if (voice_edited()) paint_fill(x - 8, 6, 3, 3, AMBER);
}

static void voice_face(void)
{
    if (paint_rendering && (paint_top >= STAGE_BOTTOM || paint_top + PAINT_ROWS <= STAGE_TOP)) return;
    unsigned vowel = voice_now.track_vowel[voice_cast], level = voice_now.track_level[voice_cast];
    struct pose p;
    p.vowel = vowel;
    p.open = vowel < MUMBLE_VOWELS ? (level > 180U ? 4U : level / 45U) : 0U;
    p.blink = ((frame_time + voice_cast * 977U) % 3700U) < 120U;
    p.shout = voice_caps;
    p.color = mumble_cast[voice_cast].color;
    p.x = 53;
    p.y = 66;
    /* Breathing when idle, a bob on every syllable; the kid bounces, the
     * ghost floats and the elder's beard sways with his wobble. */
    int breathe = voice_wave(2400, 1, voice_cast * 300U), sway = 0;
    if (voice_cast == 3) breathe = voice_wave(900, 2, 0);
    if (voice_cast == 7) breathe = voice_wave(2000, 3, 0);
    if (voice_cast == 5) sway = voice_wave(2000, 2 + (int)(voice_track()->sound.value[MUMBLE_WOBBLE] / 40U), 0);
    p.y += breathe - (p.open ? 2 : 0);
    struct { int y, sway; unsigned open, vowel, blink, shout, cast, anim; } sig_in = {
        p.y, sway, p.open, vowel, p.blink, p.shout, voice_cast,
        voice_cast == 6 ? frame_time / 60U : voice_cast == 7 ? frame_time / 120U : voice_cast == 4 ? frame_time / 400U : 0U};
    uint32_t sig = signature(0x0FACE, &sig_in, sizeof sig_in);
    if (cache->voice_cache[1] == sig) return;
    cache->voice_cache[1] = sig;
    voice_clip(4, STAGE_TOP, PORTRAIT_X1, STAGE_BOTTOM);
    vfill(4, STAGE_TOP, PORTRAIT_X1 - 4, STAGE_BOTTOM - STAGE_TOP, BACK);
    switch (voice_cast) {
    case 0: draw_hero(&p); break;
    case 1: draw_guard(&p); break;
    case 2: draw_sister(&p); break;
    case 3: draw_kid(&p); break;
    case 4: draw_pirate(&p); break;
    case 5: draw_elder(&p, sway); break;
    case 6: draw_bot(&p); break;
    default: draw_ghost(&p); break;
    }
}

static void voice_roll(void)
{
    if (paint_rendering && (paint_top >= STAGE_BOTTOM || paint_top + PAINT_ROWS <= STAGE_TOP)) return;
    uint32_t sig = signature(voice_cast, voice_trace, sizeof voice_trace);
    if (cache->voice_cache[2] == sig) return;
    cache->voice_cache[2] = sig;
    const unsigned x0 = 106, y0 = STAGE_TOP, w = 130, h = STAGE_BOTTOM - STAGE_TOP, mid = 76;
    paint_fill(x0, y0, w, h, BACK);
    /* The character's own pitch, and an octave above. */
    for (unsigned x = x0; x < x0 + w; x += 4) {
        paint_fill(x, mid, 1, 1, PANEL);
        paint_fill(x, mid - 36, 1, 1, PANEL);
    }
    for (unsigned i = 0; i < VOICE_TRACE; ++i) {
        unsigned vowel = voice_trace[i].vowel;
        if (vowel >= MUMBLE_VOWELS || !voice_trace[i].level) continue;
        int y = (int)mid - voice_trace[i].cents * 36 / 1200;
        y = clamp(y, (int)y0 + 3, (int)(y0 + h) - 6);
        unsigned thick = 2U + voice_trace[i].level / 64U;
        unsigned track = voice_trace[i].track < MUMBLE_CAST ? voice_trace[i].track : 0U;
        uint16_t color = track == voice_cast ? vowel_colors[vowel] : dim(mumble_cast[track].color);
        paint_fill(x0 + i * 6U + 2U, (unsigned)y - thick / 2U, 5, thick, color);
    }
    if (voice_now.vowel < MUMBLE_VOWELS)
        text(x0 + w - 16, y0 + h - 9, mumble_vowel_names[voice_now.vowel], 1, vowel_colors[voice_now.vowel], BACK);
}

/* Little heads of the whole cast: bright in the choir, underlined when selected. */
static void voice_choir_strip(void)
{
    if (paint_rendering && (paint_top >= STRIP_BOTTOM || paint_top + PAINT_ROWS <= STRIP_TOP)) return;
    uint8_t state[MUMBLE_CAST];
    for (unsigned i = 0; i < MUMBLE_CAST; ++i)
        state[i] = (uint8_t)((voice_now.track_level[i] / 64U) | (voice_tracks[i].choir << 2) |
                             ((i == voice_cast) << 3) | ((voice_tracks[i].length != 0) << 4));
    uint32_t sig = signature(0xC401, state, sizeof state);
    if (cache->voice_cache[7] == sig) return;
    cache->voice_cache[7] = sig;
    voice_clip(0, STRIP_TOP, 240, STRIP_BOTTOM);
    vfill(0, STRIP_TOP, 240, STRIP_BOTTOM - STRIP_TOP, BACK);
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        int x = 22 + (int)i * 28, y = STRIP_TOP + 7;
        unsigned on = voice_in_choir(i);
        uint16_t c = on ? mumble_cast[i].color : dimmer(mumble_cast[i].color);
        switch (i) { /* one detail each, so they read at 11 px */
        case 0: vfill(x - 5, y - 8, 1, 3, on ? WHITE : DIM); vfill(x + 5, y - 8, 1, 3, on ? WHITE : DIM); break;
        case 1: vfill(x + 1, y - 8, 4, 2, on ? PLUME : DIM); break;
        case 2: vfill(x - 4, y - 8, 9, 1, on ? GOLD : DIM); break;
        case 4: vfill(x - 6, y - 7, 13, 2, on ? INK : DIM); break;
        case 5: vtri(x - 4, y - 4, x + 4, y - 4, x + 2, y - 8, c); break;
        case 6: vfill(x, y - 8, 1, 3, c); break;
        default: break;
        }
        if (i == 6) vfill(x - 5, y - 5, 11, 10, c);
        else vellipse(x, y, 5, 5, c);
        vfill(x - 2, y - 1, 1, 2, INK);
        vfill(x + 2, y - 1, 1, 2, INK);
        unsigned open = voice_now.track_level[i] / 64U;
        vfill(x - 1, y + 2, 3, open ? (int)open : 1, INK);
        if (i == voice_cast) vfill(x - 6, STRIP_BOTTOM - 2, 13, 2, WHITE);
        if (voice_tracks[i].length) vfill(x + 7, y - 6, 2, 2, on ? TEXT : DIM);
    }
}

/* Word-wrap the visible text into rows of VOICE_COLUMNS; keep the last rows. */
static unsigned voice_rows(const char *s, unsigned n, char rows[VOICE_ROWS][VOICE_COLUMNS + 1])
{
    char all[MUMBLE_TEXT / 4 + 8][VOICE_COLUMNS + 1];
    unsigned count = 0, used = 0;
    memset(all, 0, sizeof all);
    for (unsigned i = 0; i < n && count < sizeof all / sizeof all[0];) {
        unsigned word = i;
        while (word < n && s[word] != ' ') ++word;
        unsigned len = word - i;
        if (used && used + len > VOICE_COLUMNS) { ++count; used = 0; }
        if (count >= sizeof all / sizeof all[0]) break;
        for (; i < word; ++i) {
            if (used == VOICE_COLUMNS) { ++count; used = 0; if (count >= sizeof all / sizeof all[0]) break; }
            all[count][used++] = s[i];
        }
        if (i < n && s[i] == ' ') {
            if (used < VOICE_COLUMNS) all[count][used++] = ' ';
            ++i;
        }
    }
    unsigned total = count + 1U, first = total > VOICE_ROWS ? total - VOICE_ROWS : 0U;
    for (unsigned r = 0; r < VOICE_ROWS; ++r)
        memcpy(rows[r], first + r < total ? all[first + r] : all[sizeof all / sizeof all[0] - 1], VOICE_COLUMNS + 1);
    return total - first;
}

/* An Undertale-style dialogue box; it takes the speaker's colour. */
static void voice_box(void)
{
    if (paint_rendering && (paint_top >= BOX_BOTTOM || paint_top + PAINT_ROWS <= BOX_TOP)) return;
    uint16_t color = voice_now.speaking || voice_looping ? mumble_cast[voice_cast].color : TEXT;
    if (cache->voice_cache[6] == color) return;
    cache->voice_cache[6] = color;
    paint_fill(2, BOX_TOP, 236, 2, color);
    paint_fill(2, BOX_BOTTOM - 2, 236, 2, color);
    paint_fill(2, BOX_TOP, 2, BOX_BOTTOM - BOX_TOP, color);
    paint_fill(236, BOX_TOP, 2, BOX_BOTTOM - BOX_TOP, color);
}

static void voice_lines_draw(void)
{
    if (paint_rendering && (paint_top >= BOX_BOTTOM - 2 || paint_top + PAINT_ROWS <= BOX_TOP + 2)) return;
    const struct voice_track *t = voice_track();
    unsigned shown = voice_now.speaking ? voice_now.revealed : t->length;
    if (shown > t->length) shown = t->length;
    char rows[VOICE_ROWS][VOICE_COLUMNS + 1];
    unsigned used = voice_rows(t->text, shown, rows);
    unsigned cursor_on = voice_now.speaking || (frame_time / 530U) % 2U == 0U;
    for (unsigned r = 0; r < VOICE_ROWS; ++r) {
        unsigned y = 128U + r * 19U;
        unsigned cursor_col = r + 1U == used ? (unsigned)strlen(rows[r]) : ~0U;
        uint32_t sig = signature(cursor_on | (shown == 0) << 1 | (voice_now.speaking << 2) | (voice_cast << 3),
                                 rows[r], VOICE_COLUMNS) ^ cursor_col;
        if (cache->voice_cache[3 + r] == sig) continue;
        cache->voice_cache[3 + r] = sig;
        paint_fill(4, y - 2, 232, 18, BACK);
        if (!shown && r == 0) {
            text(10, y + 3, "* TYPE ON THE KEYS...", 1, DIM, BACK);
            continue;
        }
        /* The font has no lower case: shouted (all-caps) words show in amber. */
        for (unsigned i = 0; rows[r][i]; ++i) {
            unsigned b = i, letters = 0, upper = 0;
            while (b > 0 && rows[r][b - 1] != ' ') --b;
            for (; rows[r][b] && rows[r][b] != ' '; ++b) {
                char ch = rows[r][b];
                letters += (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
                upper += ch >= 'A' && ch <= 'Z';
            }
            char glyph[2] = {rows[r][i], 0};
            text(10U + i * 12U, y, glyph, 2, letters > 1 && upper == letters ? AMBER : TEXT, BACK);
        }
        if (cursor_col < VOICE_COLUMNS && cursor_on)
            paint_fill(10U + cursor_col * 12U, y, 9, 14, voice_now.speaking ? mumble_cast[voice_cast].color : AMBER);
    }
}

/* PLAY mode: the letter being sung, its note and how it is said. */
static void voice_play_draw(void)
{
    if (paint_rendering && (paint_top >= BOX_BOTTOM - 2 || paint_top + PAINT_ROWS <= BOX_TOP + 2)) return;
    unsigned held = voice_sung < MUMBLE_LETTERS && (voice_held & (1U << voice_sung)) != 0;
    unsigned singers = 0;
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) singers += voice_in_choir(i);
    uint32_t sig = 0x50000000U | voice_sung | (voice_sung_note << 5) | ((unsigned)(voice_octave + 2) << 12) |
                   (held << 15) | (voice_caps << 16) | (voice_cast << 17) | (singers << 20);
    /* The TYPE rows share this area: make them redraw on return. */
    cache->voice_cache[4] = cache->voice_cache[5] = 0xfffffffeU;
    if (cache->voice_cache[3] == sig) return;
    cache->voice_cache[3] = sig;
    paint_fill(4, BOX_TOP + 2, 232, BOX_BOTTOM - BOX_TOP - 4, BACK);
    if (voice_sung >= MUMBLE_LETTERS) {
        text(10, 131, "* HOLD THE KEYS TO SING", 1, DIM, BACK);
    } else {
        uint16_t color = !held ? DIM : voice_caps ? AMBER : mumble_cast[voice_cast].color;
        char big[2] = {voice_sung < 26 ? (char)('A' + voice_sung) : '~', 0};
        char name[12];
        text(12, 128, big, 4, color, BACK);
        note_name(voice_sung_note, name);
        text(46, 128, name, 2, held ? TEXT : DIM, BACK);
        text(46, 146, mumble_letter_names[voice_sung], 2, color, BACK);
    }
    char octave[12] = "OCT ", value[12];
    decimal(value, voice_octave, 1);
    if (voice_octave > 0) strcat(octave, "+");
    strcat(octave, value);
    text(10, 172, octave, 1, voice_octave ? AMBER : DIM, BACK);
    if (singers > 1) {
        char choir[16] = "CHOIR ";
        decimal(value, (int)singers, 1);
        strcat(choir, value);
        text(100, 172, choir, 1, ACCENT, BACK);
    }
}

static void voice_knobs(void)
{
    for (unsigned col = 0; col < 4; ++col) {
        unsigned p = voice_parameter(col);
        char value[12];
        decimal(value, voice_track()->sound.value[p], 1);
        knob(col, mumble_params[p].name, value, voice_knob_colors[col]);
    }
}

static __attribute__((noinline)) void voice_draw(void)
{
    voice_header();
    voice_face();
    voice_roll();
    voice_choir_strip();
    voice_box();
    if (voice_play) voice_play_draw();
    else voice_lines_draw();
    voice_knobs();
    if (paint_rendering && paint_top + PAINT_ROWS <= 229) return;
    const char *hint = notice ? notice
                     : voice_play ? voice_play_hints[(frame_time / 2600U) % (sizeof voice_play_hints / sizeof voice_play_hints[0])]
                                  : voice_hints[(frame_time / 2600U) % (sizeof voice_hints / sizeof voice_hints[0])];
    uint32_t hint_sig = signature(1, hint, (unsigned)strlen(hint));
    if (hint_sig != cache->hint_cache) {
        cache->hint_cache = hint_sig;
        paint_fill(6, 231, 228, 7, BACK);
        text(6, 231, hint, 1, DIM, BACK);
    }
}
