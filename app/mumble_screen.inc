/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* VOICE page (HOME+ARP): the MUMBLE retro-game voice synthesizer.
 *
 * The 27 note keys are A..Z and SPACE, lowest key first. GLO switches mode:
 *   TYPE  keys type the line. A vowel speaks the moment it is typed;
 *         consonants shape how the next vowel starts.
 *   PLAY  keys sing their letter's name ("bee", "kay", "em") at the key's
 *         pitch for as long as they are held; SPACE hums. Chords work.
 *
 *   PLAY speak line / stop     OCT- delete       OCT+ CAPS (shout)
 *   FX ,  SCL .  ENV ?  LFO !  EDIT clear line   ARP demo line
 *   SELECT character           K1 VOICE/FEEL     K2..K5 sound
 *   PRESETS browse demo lines  SEQ VOICE/FEEL    HOME exit
 *   GLO TYPE/PLAY              in PLAY, OCT-/OCT+ shift the octave
 *
 * Included by screen.c: uses its paint helpers and draw cache. */
#include "mumble.h"

enum { VOICE_TRACE = 21, VOICE_ROWS = 3, VOICE_COLUMNS = 19 };
static const uint16_t voice_knob_colors[4] = {0x2b7b, 0x2d8d, 0xf7be, 0xf325};
static const uint16_t vowel_colors[MUMBLE_VOWELS] = {0xf325, 0x2d8d, 0x2b7b, 0xf7be, 0xb47f, 0xfe87};
/* Mouth half-width/half-height per vowel, fully open. */
static const uint8_t vowel_mouth[MUMBLE_VOWELS][2] = {{12, 11}, {14, 7}, {16, 3}, {9, 10}, {5, 6}, {11, 7}};
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
    "KEYS A-Z SPACE  PLAY SPEAK  HOME EXIT",
    "FX ,  SCL .  ENV ?  LFO !  OCT+ CAPS",
    "OCT- DEL  EDIT CLEAR  ARP DEMO LINE",
    "SELECT WHO  K1 PAGE  GLO PLAY MODE"};
static const char *const voice_play_hints[] = {
    "HOLD KEYS TO SING  GLO TYPE MODE",
    "OCT- OCT+ OCTAVE  SELECT WHO",
    "SPACE HUMS  K1 PAGE  HOME EXIT"};

static char voice_text[MUMBLE_TEXT + 1];
static unsigned voice_length, voice_cast, voice_page, voice_caps, voice_demo, voice_ready;
static struct mumble_voice voice_sound;
static struct mumble_status voice_now;
static struct { uint8_t vowel, level; int16_t cents; } voice_trace[VOICE_TRACE];
static uint32_t voice_trace_ms;
static uint32_t voice_keys_lit;
/* PLAY mode: held keys, octave shift, and the last letter sung. */
static unsigned voice_play, voice_sung = MUMBLE_LETTERS, voice_sung_note;
static int voice_octave;
static uint32_t voice_held;

static void voice_select(unsigned cast)
{
    voice_cast = cast % MUMBLE_CAST;
    voice_sound = mumble_cast[voice_cast].voice;
}
static void voice_set_text(const char *s)
{
    voice_length = (unsigned)strlen(s);
    if (voice_length > MUMBLE_TEXT) voice_length = MUMBLE_TEXT;
    memcpy(voice_text, s, voice_length);
    voice_text[voice_length] = 0;
}
static void voice_open(void)
{
    if (!voice_ready) {
        mumble_init();
        voice_select(0);
        voice_set_text("");
        voice_ready = 1;
    }
    voice_page = 0;
    voice_play = 0;
    voice_held = 0;
    memset(voice_trace, 0, sizeof voice_trace);
    for (unsigned i = 0; i < VOICE_TRACE; ++i) voice_trace[i].vowel = MUMBLE_VOWELS;
}
static void voice_close(void)
{
    if (voice_ready) mumble_stop(); /* also releases held letters */
    voice_held = 0;
}
static void voice_sing(const struct fm1_controls *c, uint32_t down)
{
    uint32_t up = voice_held & ~c->keys;
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key)
        if (up & (1U << key)) mumble_release(key);
    voice_held &= c->keys;
    int shift = 12 * (voice_octave + mumble_register(&voice_sound));
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key) {
        if (!(down & (1U << key))) continue;
        int note = clamp(53 + (int)key + shift, 0, 127); /* lowest key is F3 */
        mumble_press(key, (unsigned)note, voice_caps, &voice_sound);
        voice_held |= 1U << key;
        voice_sung = key;
        voice_sung_note = (unsigned)note;
    }
}
static void voice_type(char c)
{
    if (voice_length >= MUMBLE_TEXT) {
        memmove(voice_text, voice_text + 1, MUMBLE_TEXT - 1);
        voice_length = MUMBLE_TEXT - 1;
    }
    voice_text[voice_length++] = c;
    voice_text[voice_length] = 0;
    mumble_type(voice_text, voice_length, &voice_sound);
}
static void voice_speak(void)
{
    if (!voice_length) voice_set_text(voice_lines[voice_demo % VOICE_LINES]);
    mumble_say(voice_text, voice_length, &voice_sound);
}
static unsigned voice_parameter(unsigned col) { return voice_page * 4U + col; }
static void voice_edit(unsigned col, int delta)
{
    unsigned p = voice_parameter(col);
    const struct mumble_param_info *info = &mumble_params[p];
    voice_sound.value[p] = (uint16_t)clamp((int)voice_sound.value[p] + delta, info->min, info->max);
}
static unsigned voice_edited(void)
{
    return memcmp(&voice_sound, &mumble_cast[voice_cast].voice, sizeof voice_sound) != 0;
}

/* Returns 0 when a MIDI post must be retried, as apply_controls does. */
static int voice_controls(const struct fm1_controls *c, uint32_t pressed, uint32_t key_down)
{
    if (pressed & BUTTON(HOME)) { voice_close(); page = OVERVIEW; return 1; }
    if (c->encoders[0]) {
        voice_select((unsigned)((int)voice_cast + c->encoders[0] % MUMBLE_CAST + MUMBLE_CAST));
        const char *hello = voice_greetings[voice_cast];
        mumble_aside(hello, (unsigned)strlen(hello), &voice_sound);
    }
    if (c->encoders[1] || (pressed & BUTTON(SEQ))) voice_page ^= 1U;
    for (unsigned i = 0; i < 4; ++i)
        if (c->encoders[2 + i]) voice_edit(i, fm_encoder_delta(2 + i, c->encoders[2 + i]));
    if (c->encoders[6]) {
        voice_demo = (unsigned)((int)voice_demo + c->encoders[6] % (int)VOICE_LINES + (int)VOICE_LINES) % VOICE_LINES;
        voice_set_text(voice_lines[voice_demo]);
        mumble_stop();
    }
    if (pressed & BUTTON(GLO)) {
        voice_play ^= 1U;
        for (unsigned key = 0; key < MUMBLE_LETTERS; ++key)
            if (voice_held & (1U << key)) mumble_release(key);
        voice_held = 0;
    }
    if (voice_play) {
        if ((pressed & BUTTON(OCT_UP)) && voice_octave < 2) ++voice_octave;
        if ((pressed & BUTTON(OCT_DOWN)) && voice_octave > -2) --voice_octave;
    } else {
        if (pressed & BUTTON(OCT_UP)) voice_caps ^= 1U;
        if ((pressed & BUTTON(OCT_DOWN)) && voice_length) {
            voice_text[--voice_length] = 0;
            mumble_stop();
        }
    }
    if (pressed & BUTTON(EDIT)) { voice_set_text(""); mumble_stop(); }
    if (pressed & BUTTON(ARP)) {
        voice_demo = (voice_demo + 1U) % VOICE_LINES;
        voice_set_text(voice_lines[voice_demo]);
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
        if (s.speaking) mumble_stop();
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
    return ((voice_now.speaking != 0) << PLAY) | octave | (voice_play << GLO) | (1U << ARP);
}

/* Control-side snapshot, taken once per frame before marking damage. */
static void voice_frame(uint32_t now)
{
    mumble_status(&voice_now);
    if (now - voice_trace_ms < 50U) return;
    voice_trace_ms = now;
    memmove(voice_trace, voice_trace + 1, sizeof voice_trace - sizeof voice_trace[0]);
    voice_trace[VOICE_TRACE - 1].vowel = voice_now.vowel;
    voice_trace[VOICE_TRACE - 1].level = voice_now.level;
    voice_trace[VOICE_TRACE - 1].cents = voice_now.cents;
}

static unsigned isqrt(unsigned v)
{
    unsigned r = 0;
    while ((r + 1) * (r + 1) <= v) ++r;
    return r;
}
static void ellipse(int cx, int cy, unsigned rx, unsigned ry, uint16_t color)
{
    if (!ry) { paint_fill((unsigned)(cx - (int)rx), (unsigned)cy, rx * 2U + 1U, 1, color); return; }
    for (int dy = -(int)ry; dy <= (int)ry; ++dy) {
        unsigned hw = isqrt(rx * rx * (ry * ry - (unsigned)(dy * dy) + ry / 2U) / (ry * ry));
        if (hw > rx) hw = rx;
        paint_fill((unsigned)(cx - (int)hw), (unsigned)(cy + dy), hw * 2U + 1U, 1, color);
    }
}

static void voice_header(void)
{
    if (paint_rendering && paint_top >= 30) return;
    const struct mumble_character *who = &mumble_cast[voice_cast];
    uint32_t sig = voice_cast | (voice_page << 4) | (voice_caps << 5) | (voice_edited() << 6) | (voice_play << 7);
    if (cache->voice_cache[0] == sig) return;
    cache->voice_cache[0] = sig;
    paint_fill(0, 0, 240, 29, BACK);
    text(8, 8, "MUMBLE", 2, TEXT, BACK);
    for (unsigned i = 0; i < 4; ++i) paint_fill(86 + i * 7, 8, 5, 5, voice_knob_colors[i]);
    text(86, 18, voice_page ? "FEEL" : "VOICE", 1, DIM, BACK);
    if (voice_play) text(122, 18, "PLAY", 1, voice_caps ? AMBER : GREEN, BACK);
    else if (voice_caps) text(122, 18, "CAPS", 1, AMBER, BACK);
    unsigned n = (unsigned)strlen(who->name);
    text(232 - n * 12U, 8, who->name, 2, who->color, BACK);
    if (voice_edited()) paint_fill(232 - n * 12U - 6U, 12, 3, 3, AMBER);
}

static void voice_face(void)
{
    if (paint_rendering && (paint_top >= 120 || paint_top + PAINT_ROWS <= 32)) return;
    const unsigned cx = 52, cy = 76, r = 33;
    unsigned vowel = voice_now.vowel, level = voice_now.level;
    unsigned open = vowel < MUMBLE_VOWELS ? (level > 180U ? 4U : level / 45U) : 0U;
    unsigned blink = (frame_time % 3700U) < 120U;
    uint32_t sig = voice_cast | (vowel << 4) | (open << 8) | (blink << 12) | (voice_now.speaking << 13);
    if (cache->voice_cache[1] == sig) return;
    cache->voice_cache[1] = sig;
    uint16_t color = mumble_cast[voice_cast].color;
    paint_fill(8, 34, 90, 84, BACK);
    /* A bob on every syllable. */
    int lift = open ? -2 : 0;
    ellipse((int)cx, (int)cy + lift, r + 2U, r, color);
    for (int side = -1; side <= 1; side += 2) {
        int ex = (int)cx + side * 13;
        if (blink) paint_fill((unsigned)(ex - 3), cy - 9U + (unsigned)lift, 7, 2, BACK);
        else ellipse(ex, (int)cy - 9 + lift, 3, 4, BACK);
    }
    if (voice_cast == 4) { /* PIRATE: eye patch and strap */
        ellipse((int)cx + 13, (int)cy - 9 + lift, 6, 5, BACK);
        line((int)cx - 30, (int)cy - 22 + lift, (int)cx + 30, (int)cy - 6 + lift, BACK);
    }
    if (open) {
        const uint8_t *m = vowel_mouth[vowel];
        ellipse((int)cx, (int)cy + 12 + lift, m[0], m[1] * open / 4U + 1U, BACK);
    } else
        paint_fill(cx - 8U, cy + 12U, 17, 2, BACK);
}

static void voice_roll(void)
{
    if (paint_rendering && (paint_top >= 120 || paint_top + PAINT_ROWS <= 32)) return;
    uint32_t sig = signature(voice_cast, voice_trace, sizeof voice_trace);
    if (cache->voice_cache[2] == sig) return;
    cache->voice_cache[2] = sig;
    const unsigned x0 = 104, y0 = 34, w = 128, h = 84;
    paint_fill(x0, y0, w, h, BACK);
    /* The character's own pitch, and an octave above. */
    for (unsigned x = x0; x < x0 + w; x += 4) {
        paint_fill(x, 92, 1, 1, PANEL);
        paint_fill(x, 44, 1, 1, PANEL);
    }
    for (unsigned i = 0; i < VOICE_TRACE; ++i) {
        unsigned vowel = voice_trace[i].vowel;
        if (vowel >= MUMBLE_VOWELS || !voice_trace[i].level) continue;
        int y = 92 - voice_trace[i].cents * 48 / 1200;
        y = clamp(y, (int)y0 + 3, (int)(y0 + h) - 6);
        unsigned thick = 2U + voice_trace[i].level / 64U;
        paint_fill(x0 + i * 6U + 1U, (unsigned)y - thick / 2U, 5, thick, vowel_colors[vowel]);
    }
    if (voice_now.vowel < MUMBLE_VOWELS)
        text(x0 + w - 12, y0 + h - 8, mumble_vowel_names[voice_now.vowel], 1, vowel_colors[voice_now.vowel], BACK);
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

static void voice_lines_draw(void)
{
    if (paint_rendering && (paint_top >= 186 || paint_top + PAINT_ROWS <= 122)) return;
    unsigned shown = voice_now.speaking ? voice_now.revealed : voice_length;
    if (shown > voice_length) shown = voice_length;
    char rows[VOICE_ROWS][VOICE_COLUMNS + 1];
    unsigned used = voice_rows(voice_text, shown, rows);
    unsigned cursor_on = voice_now.speaking || (frame_time / 530U) % 2U == 0U;
    for (unsigned r = 0; r < VOICE_ROWS; ++r) {
        unsigned y = 126U + r * 20U;
        unsigned cursor_col = r + 1U == used ? (unsigned)strlen(rows[r]) : ~0U;
        uint32_t sig = signature(cursor_on | (shown == 0) << 1 | (voice_now.speaking << 2), rows[r], VOICE_COLUMNS) ^ cursor_col;
        if (cache->voice_cache[3 + r] == sig) continue;
        cache->voice_cache[3 + r] = sig;
        paint_fill(0, y - 2U, 240, 18, BACK);
        if (!shown && r == 0) {
            text(6, y + 3, "TYPE ON THE KEYS...", 1, DIM, BACK);
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
            text(6U + i * 12U, y, glyph, 2, letters > 1 && upper == letters ? AMBER : TEXT, BACK);
        }
        if (cursor_col < VOICE_COLUMNS && cursor_on)
            paint_fill(6U + cursor_col * 12U, y, 9, 14, voice_now.speaking ? mumble_cast[voice_cast].color : AMBER);
    }
}

/* PLAY mode: the letter being sung, its note and how it is said. */
static void voice_play_draw(void)
{
    if (paint_rendering && (paint_top >= 186 || paint_top + PAINT_ROWS <= 122)) return;
    unsigned held = voice_sung < MUMBLE_LETTERS && (voice_held & (1U << voice_sung)) != 0;
    uint32_t sig = 0x50000000U | voice_sung | (voice_sung_note << 5) | ((unsigned)(voice_octave + 2) << 12) |
                   (held << 15) | (voice_caps << 16) | (voice_cast << 17);
    /* The TYPE rows share this area: make them redraw on return. */
    cache->voice_cache[4] = cache->voice_cache[5] = 0xfffffffeU;
    if (cache->voice_cache[3] == sig) return;
    cache->voice_cache[3] = sig;
    paint_fill(0, 122, 240, 64, BACK);
    if (voice_sung >= MUMBLE_LETTERS) {
        text(6, 129, "HOLD THE KEYS TO SING", 1, DIM, BACK);
    } else {
        uint16_t color = !held ? DIM : voice_caps ? AMBER : mumble_cast[voice_cast].color;
        char big[2] = {voice_sung < 26 ? (char)('A' + voice_sung) : '~', 0};
        char name[12];
        text(8, 126, big, 4, color, BACK);
        note_name(voice_sung_note, name);
        text(44, 126, name, 2, held ? TEXT : DIM, BACK);
        text(44, 144, mumble_letter_names[voice_sung], 2, color, BACK);
    }
    char octave[12] = "OCT ", value[12];
    decimal(value, voice_octave, 1);
    if (voice_octave > 0) strcat(octave, "+");
    strcat(octave, value);
    text(6, 172, octave, 1, voice_octave ? AMBER : DIM, BACK);
    text(186, 172, "GLO TYPE", 1, DIM, BACK);
}

static void voice_knobs(void)
{
    for (unsigned col = 0; col < 4; ++col) {
        unsigned p = voice_parameter(col);
        char value[12];
        decimal(value, voice_sound.value[p], 1);
        knob(col, mumble_params[p].name, value, voice_knob_colors[col]);
    }
}

static __attribute__((noinline)) void voice_draw(void)
{
    voice_header();
    voice_face();
    voice_roll();
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
