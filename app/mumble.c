/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* MUMBLE voice engine. See mumble.h for the model. */
#include "mumble.h"
#include <string.h>

#define RATE ((float)FM1_SAMPLE_RATE)
#define PI 3.14159265f

const struct mumble_param_info mumble_params[MUMBLE_PARAMS] = {
    {"PITCH", 60, 420}, {"TUBE", 75, 145}, {"MOUTH", 30, 130}, {"AIR", 0, 100},
    {"SPEED", 50, 200}, {"WOBBLE", 0, 150}, {"JITTER", 0, 100}, {"TUNE", 0, 160},
    {"CRUSH", 0, 100}, {"RADIO", 0, 100}, {"ECHO", 0, 100}, {"ROOM", 0, 100}};

/*                                     pitch tube mouth air speed wob jit tune crush radio echo room */
const struct mumble_character mumble_cast[MUMBLE_CAST] = {
    {"HERO",   0x3c9f, {{135, 100, 100, 22, 100,  0, 50, 100,  0,  0, 10, 20}}},
    {"GUARD",  0x9cd3, {{ 92,  86,  90, 18,  82,  0, 35,  60,  0,  0,  0, 55}}},
    {"SISTER", 0xfb25, {{215, 118, 100, 32, 100,  0, 45, 115,  0,  0, 15, 70}}},
    {"KID",    0x2dad, {{300, 134, 110, 20, 120,  0, 60, 140,  0,  0,  0, 10}}},
    {"PIRATE", 0xbb07, {{108,  92,  45, 38,  92, 10, 80,  80, 12,  0, 25,  0}}},
    {"ELDER",  0x9bfa, {{150,  97,  80, 50,  75, 90, 45,  90,  0,  0,  0, 40}}},
    {"BOT",    0x9d14, {{120, 100, 100,  0, 110,  0,  0,   0, 55, 45,  0,  0}}},
    {"GHOST",  0x5e5c, {{175, 110,  70, 95,  70, 50, 30, 120,  0, 20, 50, 85}}}};

const char *const mumble_vowel_names[MUMBLE_VOWELS] = {"AH", "EH", "EE", "AW", "OO", "UH"};

/* Adult formant centres (Hz), after Peterson & Barney (1952). */
static const float formant_hz[MUMBLE_VOWELS][3] FM1_AUDIO_DATA = {
    {730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010},
    {570, 840, 2410},  {300, 870, 2240},  {640, 1190, 2390}};
static const float bandwidth_hz[4] FM1_AUDIO_DATA = {70, 100, 150, 220};

/* ------------------------------------------------------------------------
 * Small math without libm, safe for RAM-resident audio code.
 * ---------------------------------------------------------------------- */
static inline __attribute__((always_inline)) float exp2_approx(float x)
{
    if (x < -30.0f) return 0.0f;
    if (x > 30.0f) x = 30.0f;
    int whole = (int)x;
    if ((float)whole > x) --whole;
    float f = x - (float)whole;
    float p = 1.0f + f * (0.6951786f + f * (0.2261485f + f * 0.0780415f));
    union { float f; uint32_t u; } scale;
    scale.u = (uint32_t)(whole + 127) << 23;
    return p * scale.f;
}
/* cos(x) for |x| <= pi. */
static inline __attribute__((always_inline)) float cos_approx(float x)
{
    if (x < 0) x = -x;
    float sign = 1.0f;
    if (x > 0.5f * PI) { x = PI - x; sign = -1.0f; }
    float y = x * x;
    return sign * (1.0f + y * (-0.5f + y * (1.0f / 24 + y * (-1.0f / 720 + y * (1.0f / 40320)))));
}
/* Smooth unit step on [0, 1]. */
static inline __attribute__((always_inline)) float smooth(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

/* ------------------------------------------------------------------------
 * Text analysis (control side).
 * ---------------------------------------------------------------------- */
static uint32_t hash(const char *s, unsigned n, uint32_t h)
{
    for (unsigned i = 0; i < n; ++i) h = (h ^ (uint8_t)s[i]) * 16777619U;
    return h;
}
static float random_unit(uint32_t *state) /* [-1, 1) */
{
    uint32_t t = (*state += 0x6d2b79f5U);
    t = (t ^ (t >> 15)) * (t | 1U);
    t ^= t + (t ^ (t >> 7)) * (t | 61U);
    t ^= t >> 14;
    return (float)(t >> 8) * (2.0f / 16777216.0f) - 1.0f;
}
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int is_letter(char c) { c = lower(c); return c >= 'a' && c <= 'z'; }
static int is_plain_vowel(char c)
{
    c = lower(c);
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}
/* y is a vowel after a consonant ("happy", "my"), not at a word start ("yes"). */
static int vowel_at(const char *w, unsigned i)
{
    if (is_plain_vowel(w[i])) return 1;
    return lower(w[i]) == 'y' && i > 0 && !is_plain_vowel(w[i - 1]);
}
static uint8_t vowel_of(const char *g, unsigned n)
{
    char a = lower(g[0]), b = n > 1 ? lower(g[1]) : 0;
    if (a == 'o' && (b == 'o' || b == 'u')) return MUMBLE_OO;
    if (a == 'u' && (b == 'e' || b == 'i')) return MUMBLE_OO;
    if ((a == 'e' && (b == 'e' || b == 'a' || b == 'y')) || (a == 'i' && b == 'e')) return MUMBLE_I;
    if ((a == 'a' && (b == 'i' || b == 'y')) || (a == 'e' && b == 'i')) return MUMBLE_E;
    if ((a == 'a' && b == 'u') || (a == 'o' && b == 'a')) return MUMBLE_O;
    switch (a) {
    case 'a': return MUMBLE_A;
    case 'e': return MUMBLE_E;
    case 'o': return MUMBLE_O;
    case 'u': return MUMBLE_U;
    default: return MUMBLE_I; /* i, y */
    }
}
/* Where F2 starts when a consonant leads into the vowel, in 10 Hz units. */
static uint8_t locus_of(char c)
{
    switch (lower(c)) {
    case 'b': case 'p': case 'm': case 'f': case 'v': case 'w': return 85;
    case 't': case 'd': case 'n': case 's': case 'z': case 'l': case 'r': case 'c': return 175;
    case 'k': case 'g': case 'q': case 'x': return 210;
    case 'j': case 'y': return 230;
    default: return 0;
    }
}
static unsigned pause_of(char c)
{
    switch (c) {
    case ',': case ';': case ':': return 3;
    case '-': return 2;
    case '.': case '?': case '!': case '\n': return 6;
    default: return 1;
    }
}
static int ends_phrase(char c)
{
    return c == ',' || c == ';' || c == ':' || c == '.' || c == '?' || c == '!' || c == '\n';
}
static float voice_unit(const struct mumble_voice *v, unsigned p) { return (float)v->value[p] * 0.01f; }

unsigned mumble_plan(const char *text, unsigned length, const struct mumble_voice *voice,
                     unsigned open, struct mumble_syllable *out, unsigned capacity,
                     uint16_t *char_ms, uint16_t *end_ms)
{
    if (!text || !voice || !out) return 0;
    if (length > MUMBLE_TEXT) length = MUMBLE_TEXT;
    if (capacity > MUMBLE_SYLLABLES) capacity = MUMBLE_SYLLABLES;
    float speed = voice_unit(voice, MUMBLE_SPEED), tune = voice_unit(voice, MUMBLE_TUNE);
    float jitter = voice_unit(voice, MUMBLE_JITTER);
    if (speed < 0.25f) speed = 0.25f;
    float step = 1000.0f / 30.0f / speed; /* one letter every ~33 ms */

    /* 1. Typing clock: each character waits according to itself. */
    uint16_t at[MUMBLE_TEXT];
    float t = 0.0f;
    for (unsigned i = 0; i < length; ++i) {
        at[i] = (uint16_t)(t < 65535.0f ? t : 65535.0f);
        if (char_ms) char_ms[i] = at[i];
        t += step * (float)pause_of(text[i]);
    }
    if (end_ms) *end_ms = (uint16_t)(t < 65535.0f ? t : 65535.0f);

    /* 2. Syllables of each word, with their word and phrase. */
    uint8_t word_of[MUMBLE_SYLLABLES], phrase_of[MUMBLE_SYLLABLES];
    float semis[MUMBLE_SYLLABLES];
    char phrase_end[MUMBLE_SYLLABLES + 1];
    unsigned n = 0, words = 0, phrase = 0;
    memset(phrase_end, '.', sizeof phrase_end);
    for (unsigned i = 0; i < length && n < capacity;) {
        char c = text[i];
        if (ends_phrase(c)) {
            unsigned j = i, dots = 0, ask = 0, shout = 0;
            while (j < length && (ends_phrase(text[j]) || text[j] == ' ')) {
                dots += text[j] == '.'; ask |= text[j] == '?'; shout |= text[j] == '!';
                ++j;
            }
            if (phrase < MUMBLE_SYLLABLES)
                phrase_end[phrase] = ask ? '?' : shout ? '!' : dots > 1 ? '~' : c == '\n' ? '.' : c;
            if (phrase < MUMBLE_SYLLABLES) ++phrase;
            i = j;
            continue;
        }
        if (!is_letter(c)) { ++i; continue; }
        unsigned j = i, upper = 0, letters = 0;
        while (j < length && (is_letter(text[j]) || text[j] == '\'')) {
            if (is_letter(text[j])) { ++letters; upper += text[j] >= 'A' && text[j] <= 'Z'; }
            ++j;
        }
        const char *w = text + i;
        unsigned len = j - i, first = n, shouted = letters > 1 && upper == letters;
        for (unsigned k = 0; k < len && n < capacity; ++k) {
            if (!vowel_at(w, k) || (k > 0 && vowel_at(w, k - 1))) continue;
            unsigned e = k;
            while (e < len && vowel_at(w, e)) ++e;
            struct mumble_syllable *s = &out[n];
            memset(s, 0, sizeof *s);
            s->index = (uint8_t)(i + k);
            s->ms = at[i + k];
            s->vowel = vowel_of(w + k, e - k);
            s->locus = k ? locus_of(w[k - 1]) : 0;
            s->flags = shouted ? MUMBLE_SHOUT : 0;
            word_of[n] = (uint8_t)words; phrase_of[n] = (uint8_t)phrase;
            ++n;
        }
        /* A final lone e after a consonant is usually silent ("make"). */
        if (!(open && j == length) && n - first > 1 && len > 2 &&
            out[n - 1].index == i + len - 1 && lower(w[len - 1]) == 'e')
            --n;
        /* No vowel at all ("hmm", "brr"): one closed-mouth hum. */
        if (n == first && n < capacity && !(open && j == length)) {
            struct mumble_syllable *s = &out[n];
            memset(s, 0, sizeof *s);
            s->index = (uint8_t)i; s->ms = at[i]; s->vowel = MUMBLE_U;
            s->flags = (uint8_t)(MUMBLE_HUM | (shouted ? MUMBLE_SHOUT : 0));
            word_of[n] = (uint8_t)words; phrase_of[n] = (uint8_t)phrase;
            ++n;
        }
        ++words;
        i = j;
    }

    /* 3. Tune: drift down through each phrase, reset at every break; the
     * closing punctuation bends the end. */
    float gain[MUMBLE_SYLLABLES];
    for (unsigned a = 0; a < n;) {
        unsigned b = a;
        while (b < n && phrase_of[b] == phrase_of[a]) ++b;
        unsigned count = b - a, last_word = word_of[b - 1], last_start = b - 1;
        while (last_start > a && word_of[last_start - 1] == last_word) --last_start;
        char end = phrase_end[phrase_of[a]];
        for (unsigned k = a; k < b; ++k) {
            float x = count > 1 ? (float)(k - a) / (float)(count - 1) : 0.0f;
            semis[k] = -3.0f * tune * x;
            gain[k] = 1.0f;
            if (end == '!') { semis[k] += 2.5f * tune; gain[k] = 1.15f; }
            if (end == '~') { semis[k] -= 1.5f * tune * x; gain[k] = 1.0f - 0.65f * x; }
            if (end == '.' && k == b - 1) semis[k] -= tune;
            if (end == '?' && k >= last_start)
                semis[k] = tune * (1.0f + 4.0f * (float)(k - last_start + 1) / (float)(b - last_start));
        }
        a = b;
    }
    uint32_t seed = hash(text, length, 2166136261U) ^ voice->value[MUMBLE_PITCH];
    for (unsigned k = 0; k < n; ++k) {
        semis[k] += random_unit(&seed) * jitter;
        if (out[k].flags & MUMBLE_SHOUT) { semis[k] += 4.0f; gain[k] *= 1.35f; }
        /* 4. Each hum lasts a little longer than the gap to the next one,
         * so syllables smear into one stream of babble. */
        float dur = k + 1 < n && phrase_of[k + 1] == phrase_of[k]
            ? (float)(out[k + 1].ms - out[k].ms) * 1.25f : 90.0f + 40.0f / speed;
        out[k].dur_ms = (uint16_t)(dur < 70.0f ? 70.0f : dur > 260.0f ? 260.0f : dur);
        out[k].cents = (int16_t)(semis[k] * 100.0f);
        float g = gain[k] * 64.0f;
        out[k].gain = (uint8_t)(g > 255.0f ? 255.0f : g);
    }
    return n;
}


/* ------------------------------------------------------------------------
 * Shared control → audio script. Three slots: the one audio plays (ack),
 * the newest request, and a free one control can always write. A script
 * holds every track's syllables, merged in time order.
 * ---------------------------------------------------------------------- */
enum { SCRIPT_SYLLABLES = 160 };
struct script {
    uint8_t count, line, length, reveal;
    uint16_t end_ms;
    struct mumble_voice voice[MUMBLE_CAST];
    struct mumble_syllable syllable[SCRIPT_SYLLABLES];
    uint16_t char_ms[MUMBLE_TEXT];
};
enum { VOICES = 8, SUB = 32, GLOTTAL = 256 };
struct hum {
    int32_t pos;                 /* <0: not yet started */
    uint32_t frames, dur, attack, release, glide;
    float inv_dur, inv_attack, inv_release, inv_glide, wobble_phase, phase, inc, period, amp, semis, gain, air, jitter, wobble, pitch;
    float fall, fade;            /* per frame: pitch fall (semitones) and level fade */
    float start[4], target[4], radius[4], y1[4], y2[4], a[4], b[4], c[4], last;
    /* Effects: crush (hold + quantise), radio (band-pass + drive), sends. */
    float levels, crushed, radio, hp_a, lp_a, hp, lp, drive, echo, room;
    uint32_t noise;
    uint8_t active, vowel, track, crush_hold, crush_count;
    uint8_t tag, gen, hold;      /* held letter + 1 (0 = not held), its press, sustains */
};
static struct script scripts[3];
static volatile uint8_t request, ack;
static volatile uint8_t enabled;
static volatile uint32_t clock_frames;          /* frames since the playing script began */
/* Bumped by every press and release of a letter; a held hum lets go when its
 * letter's count moves past the press it started from. */
static volatile uint8_t held_gen[MUMBLE_LETTERS];
static volatile uint8_t status_vowel = MUMBLE_VOWELS, status_level, status_line, status_track;
static volatile uint8_t status_track_vowel[MUMBLE_CAST], status_track_level[MUMBLE_CAST];
static volatile int16_t status_cents;
static struct hum hums[VOICES];
static unsigned next_syllable, line_playing;
static float wobble_phase; /* shared 2 Hz wobble, so overlapping hums agree */
static float glottal_flow[GLOTTAL + 1], glottal_slope[GLOTTAL + 1];
static float mix[FM1_BLOCK_FRAMES], echo_send[FM1_BLOCK_FRAMES], room_send[FM1_BLOCK_FRAMES];

/* Shared effects run at half rate (22.05 kHz) on 16-bit lines: a dark,
 * lo-fi echo and a small Schroeder room, about 16 KB together. */
enum { ECHO_LENGTH = 5513 };                     /* 250 ms */
static const uint16_t comb_length[4] FM1_AUDIO_DATA = {557, 593, 641, 677};
static const uint16_t allpass_length[2] FM1_AUDIO_DATA = {277, 211};
static int16_t echo_line[ECHO_LENGTH];
static int16_t comb_line[557 + 593 + 641 + 677], allpass_line[277 + 211];
static unsigned echo_at, comb_at[4], allpass_at[2];
static float echo_tone, comb_tone[4], wet_last;
static uint32_t tail_frames;                     /* effect tail still ringing */

void mumble_init(void)
{
    /* Rosenberg glottal pulse: opening 40%, closing 16% of the period.
     * slope is d(flow)/d(phase), i.e. what leaves the lips per period. */
    const float open = 0.4f, close = 0.16f;
    for (unsigned i = 0; i <= GLOTTAL; ++i) {
        float p = (float)i / GLOTTAL, flow = 0.0f, slope = 0.0f;
        if (p < open) {
            flow = 0.5f * (1.0f - cos_approx(PI * p / open));
            slope = 0.5f * PI / open * cos_approx(PI * p / open - 0.5f * PI);
        } else if (p < open + close) {
            float x = 0.5f * PI * (p - open) / close;
            flow = cos_approx(x);
            slope = -0.5f * PI / close * cos_approx(0.5f * PI - x);
        }
        glottal_flow[i] = flow;
        glottal_slope[i] = slope;
    }
    memset(scripts, 0, sizeof scripts);
    memset(hums, 0, sizeof hums);
    memset(echo_line, 0, sizeof echo_line);
    memset(comb_line, 0, sizeof comb_line);
    memset(allpass_line, 0, sizeof allpass_line);
    echo_at = 0; echo_tone = wet_last = 0.0f; tail_frames = 0;
    for (unsigned i = 0; i < 4; ++i) { comb_at[i] = 0; comb_tone[i] = 0.0f; }
    allpass_at[0] = allpass_at[1] = 0;
    request = ack = 0;
    enabled = 1;
}

static struct script *free_slot(void)
{
    unsigned a = ack, r = request;
    for (unsigned i = 0; i < 3; ++i)
        if (i != a && i != r) return &scripts[i];
    return &scripts[(a + 1) % 3];
}
static void publish(struct script *s) { request = (uint8_t)(s - scripts); }

/* Stable insertion sort by start time: audio walks syllables in order. */
static void sort_syllables(struct mumble_syllable *s, unsigned n)
{
    for (unsigned i = 1; i < n; ++i) {
        struct mumble_syllable x = s[i];
        unsigned j = i;
        while (j > 0 && s[j - 1].ms > x.ms) { s[j] = s[j - 1]; --j; }
        s[j] = x;
    }
}

static int choir(const struct mumble_part parts[MUMBLE_CAST], unsigned reveal, unsigned line)
{
    if (!parts) return -1;
    struct script *s = free_slot();
    unsigned n = 0;
    uint16_t end = 0;
    s->length = 0;
    for (unsigned t = 0; t < MUMBLE_CAST; ++t) {
        const struct mumble_part *p = &parts[t];
        if (!p->text || !p->voice || !p->length) continue;
        unsigned length = p->length > MUMBLE_TEXT ? MUMBLE_TEXT : p->length, capacity = SCRIPT_SYLLABLES - n;
        if (capacity > MUMBLE_SYLLABLES) capacity = MUMBLE_SYLLABLES;
        uint16_t e = 0;
        s->voice[t] = *p->voice;
        unsigned got = mumble_plan(p->text, length, p->voice, 0, s->syllable + n, capacity,
                                   t == reveal ? s->char_ms : NULL, &e);
        for (unsigned i = n; i < n + got; ++i) s->syllable[i].track = (uint8_t)t;
        n += got;
        if (e > end) end = e;
        if (t == reveal) s->length = (uint8_t)length;
    }
    sort_syllables(s->syllable, n);
    s->count = (uint8_t)n;
    s->end_ms = end;
    s->line = (uint8_t)line;
    s->reveal = (uint8_t)reveal;
    publish(s);
    return 0;
}
int mumble_choir(const struct mumble_part parts[MUMBLE_CAST], unsigned reveal)
{ return choir(parts, reveal, 1); }

static int say(unsigned track, const char *text, unsigned length, const struct mumble_voice *voice, unsigned line)
{
    if (!text || !voice || track >= MUMBLE_CAST) return -1;
    struct mumble_part parts[MUMBLE_CAST];
    memset(parts, 0, sizeof parts);
    parts[track].text = text;
    parts[track].length = length;
    parts[track].voice = voice;
    return choir(parts, track, line);
}
int mumble_say(unsigned track, const char *text, unsigned length, const struct mumble_voice *voice)
{ return say(track, text, length, voice, 1); }
int mumble_aside(unsigned track, const char *text, unsigned length, const struct mumble_voice *voice)
{ return say(track, text, length, voice, 0); }

int mumble_type(unsigned track, const char *text, unsigned length, const struct mumble_voice *voice)
{
    if (!text || !voice || !length || track >= MUMBLE_CAST) return -1;
    struct script *s = free_slot();
    if (length > MUMBLE_TEXT) { text += length - MUMBLE_TEXT; length = MUMBLE_TEXT; }
    unsigned n = mumble_plan(text, length, voice, 1, s->syllable, MUMBLE_SYLLABLES, NULL, NULL);
    if (!n || s->syllable[n - 1].index != length - 1) return 0;
    struct mumble_syllable last = s->syllable[n - 1];
    /* Typing has no future: the pitch falls with each syllable of the phrase. */
    unsigned k = 0;
    for (unsigned i = n - 1; i > 0; --i) {
        unsigned split = 0;
        for (unsigned c = s->syllable[i - 1].index; c < s->syllable[i].index; ++c)
            split |= ends_phrase(text[c]);
        if (split) break;
        ++k;
    }
    static uint32_t live_seed = 0x9e3779b9U;
    float semis = -0.45f * (float)k;
    if (semis < -3.0f) semis = -3.0f;
    semis = semis * voice_unit(voice, MUMBLE_TUNE) +
            random_unit(&live_seed) * 0.8f * voice_unit(voice, MUMBLE_JITTER);
    if (last.flags & MUMBLE_SHOUT) semis += 4.0f;
    last.ms = 0;
    last.cents = (int16_t)(semis * 100.0f);
    last.gain = last.flags & MUMBLE_SHOUT ? 83 : 64;
    last.dur_ms = (uint16_t)(120.0f + 50.0f * 100.0f / (float)(voice->value[MUMBLE_SPEED] ? voice->value[MUMBLE_SPEED] : 100));
    last.track = (uint8_t)track;
    s->syllable[0] = last;
    s->count = 1;
    s->line = 0;
    s->length = 0;
    s->end_ms = 0;
    s->reveal = (uint8_t)track;
    s->voice[track] = *voice;
    publish(s);
    return 1;
}

void mumble_stop(void)
{
    struct script *s = free_slot();
    s->count = 0; s->line = 0; s->length = 0; s->end_ms = 0;
    publish(s);
    for (unsigned i = 0; i < MUMBLE_LETTERS; ++i) held_gen[i] = (uint8_t)(held_gen[i] + 2U);
}

/* ------------------------------------------------------------------------
 * Play mode: letters sung by name.
 * ---------------------------------------------------------------------- */
const char *const mumble_letter_names[MUMBLE_LETTERS] = {
    "AY", "BEE", "SEE", "DEE", "EE", "EF", "JEE", "AYCH", "EYE", "JAY", "KAY", "EL", "EM",
    "EN", "OH", "PEE", "KYOO", "AR", "ES", "TEE", "YOO", "VEE", "DUBBLYOO", "EX", "WHY",
    "ZEE", "MMM"};
enum { LIP = 8, TIP = 17, VELUM = 21, PALATE = 23 }; /* consonant F2 loci / 100 Hz */
/* Up to three syllables per name: vowel, locus/100 Hz, MUMBLE_HUM. The last one holds. */
static const uint8_t letter_sound[MUMBLE_LETTERS][3][3] = {
    {{MUMBLE_E, 0, 0}, {MUMBLE_I, 0, 0}},              /* a  ay   */
    {{MUMBLE_I, LIP, 0}},                              /* b  bee  */
    {{MUMBLE_I, TIP, 0}},                              /* c  see  */
    {{MUMBLE_I, TIP, 0}},                              /* d  dee  */
    {{MUMBLE_I, 0, 0}},                                /* e  ee   */
    {{MUMBLE_E, 0, 0}},                                /* f  ef   */
    {{MUMBLE_I, PALATE, 0}},                           /* g  jee  */
    {{MUMBLE_E, 0, 0}, {MUMBLE_I, PALATE, 0}},         /* h  aych */
    {{MUMBLE_A, 0, 0}, {MUMBLE_I, 0, 0}},              /* i  eye  */
    {{MUMBLE_E, PALATE, 0}, {MUMBLE_I, 0, 0}},         /* j  jay  */
    {{MUMBLE_E, VELUM, 0}, {MUMBLE_I, 0, 0}},          /* k  kay  */
    {{MUMBLE_E, 0, 0}, {MUMBLE_U, TIP, MUMBLE_HUM}},   /* l  el   */
    {{MUMBLE_E, 0, 0}, {MUMBLE_U, LIP, MUMBLE_HUM}},   /* m  em   */
    {{MUMBLE_E, 0, 0}, {MUMBLE_U, TIP, MUMBLE_HUM}},   /* n  en   */
    {{MUMBLE_O, 0, 0}, {MUMBLE_OO, 0, 0}},             /* o  oh   */
    {{MUMBLE_I, LIP, 0}},                              /* p  pee  */
    {{MUMBLE_OO, VELUM, 0}},                           /* q  kyoo */
    {{MUMBLE_A, 0, 0}, {MUMBLE_U, TIP, MUMBLE_HUM}},   /* r  ar   */
    {{MUMBLE_E, 0, 0}},                                /* s  es   */
    {{MUMBLE_I, TIP, 0}},                              /* t  tee  */
    {{MUMBLE_OO, PALATE, 0}},                          /* u  yoo  */
    {{MUMBLE_I, LIP, 0}},                              /* v  vee  */
    {{MUMBLE_U, TIP, 0}, {MUMBLE_U, LIP, 0}, {MUMBLE_OO, PALATE, 0}}, /* w dub-bl-yoo */
    {{MUMBLE_E, 0, 0}},                                /* x  ex   */
    {{MUMBLE_A, LIP, 0}, {MUMBLE_I, 0, 0}},            /* y  why  */
    {{MUMBLE_I, TIP, 0}},                              /* z  zee  */
    {{MUMBLE_U, 0, MUMBLE_HUM}},                       /* hum     */
};

/* Cents of hz above A4. Linear within a semitone: under a cent of error. */
static int cents_from_a4(float hz)
{
    const float up = 1.0594631f;
    float f = 440.0f;
    int c = 0;
    if (hz < 20.0f) hz = 20.0f;
    while (hz >= f * up) { f *= up; c += 100; }
    while (hz < f) { f /= up; c -= 100; }
    return c + (int)(100.0f * (hz - f) / (f * up - f) + 0.5f);
}

int mumble_register(const struct mumble_voice *voice)
{
    if (!voice) return 0;
    int from_f4 = cents_from_a4((float)voice->value[MUMBLE_PITCH]) + 400; /* F4 is 400 cents under A4 */
    return from_f4 >= 0 ? (from_f4 + 600) / 1200 : -((-from_f4 + 600) / 1200);
}

int mumble_press(unsigned letter, const struct mumble_singer *singers, unsigned count, unsigned shout)
{
    if (letter >= MUMBLE_LETTERS || !singers || !count || count > MUMBLE_CAST) return -1;
    struct script *s = free_slot();
    uint8_t gen = (uint8_t)(held_gen[letter] + 1U);
    unsigned syllables = 0;
    while (syllables < 3 && (syllables == 0 || letter_sound[letter][syllables][0] || letter_sound[letter][syllables][1]))
        ++syllables;
    unsigned n = 0;
    for (unsigned j = 0; j < count; ++j) {
        const struct mumble_singer *who = &singers[j];
        if (!who->voice || who->track >= MUMBLE_CAST || who->note > 127U) return -1;
        int cents = ((int)who->note - 69) * 100 - cents_from_a4((float)who->voice->value[MUMBLE_PITCH]);
        unsigned speed = who->voice->value[MUMBLE_SPEED] ? who->voice->value[MUMBLE_SPEED] : 100U;
        s->voice[who->track] = *who->voice;
        for (unsigned i = 0; i < syllables; ++i, ++n) {
            struct mumble_syllable *y = &s->syllable[n];
            const uint8_t *sound = letter_sound[letter][i];
            y->ms = (uint16_t)(i * 9000U / speed);              /* 90 ms apart at 100% */
            y->dur_ms = (uint16_t)(i + 1 < syllables ? 14000U / speed : 8000U);
            y->cents = (int16_t)cents;
            y->vowel = sound[0];
            y->locus = (uint8_t)(sound[1] * 10U);
            y->gain = shout ? 83 : 64;
            y->flags = (uint8_t)(sound[2] | (shout ? MUMBLE_SHOUT : 0) | (i + 1 == syllables ? MUMBLE_HOLD : 0));
            y->index = (uint8_t)letter;
            y->gen = gen;
            y->track = who->track;
        }
    }
    sort_syllables(s->syllable, n);
    s->count = (uint8_t)n;
    s->line = 0; s->length = 0; s->end_ms = 0; s->reveal = singers[0].track;
    held_gen[letter] = gen;
    publish(s);
    return 0;
}

void mumble_release(unsigned letter)
{
    if (letter < MUMBLE_LETTERS) held_gen[letter] = (uint8_t)(held_gen[letter] + 1U);
}

void mumble_status(struct mumble_status *status)
{
    if (!status) return;
    const struct script *s = &scripts[ack];
    uint32_t ms = (uint32_t)((uint64_t)clock_frames * 1000U / FM1_SAMPLE_RATE);
    unsigned shown = 0;
    if (s->line) while (shown < s->length && s->char_ms[shown] <= ms) ++shown;
    status->speaking = (uint8_t)(status_line && s->line && ms < s->end_ms);
    status->revealed = (uint8_t)(s->line ? shown : 0);
    status->vowel = status_vowel;
    status->level = status_level;
    status->cents = status_cents;
    status->track = status_track;
    for (unsigned t = 0; t < MUMBLE_CAST; ++t) {
        status->track_vowel[t] = status_track_vowel[t];
        status->track_level[t] = status_track_level[t];
    }
}

/* ------------------------------------------------------------------------
 * Audio.
 * ---------------------------------------------------------------------- */
static FM1_AUDIO_CODE void hum_start(const struct mumble_syllable *s, const struct mumble_voice *v,
                                     int32_t delay, uint32_t seed)
{
    /* Steal a free voice, else the oldest; a held letter goes last. */
    struct hum *h = &hums[0];
    for (unsigned i = 0; i < VOICES; ++i) {
        if (!hums[i].active) { h = &hums[i]; break; }
        if ((h->tag && !hums[i].tag) || (!h->tag == !hums[i].tag && hums[i].pos > h->pos)) h = &hums[i];
    }
    float tube = (float)v->value[MUMBLE_TUBE] * 0.01f;
    float mouth = (float)v->value[MUMBLE_MOUTH] * 0.01f;
    if (s->flags & MUMBLE_HUM) mouth *= 0.35f;
    if (s->flags & MUMBLE_SHOUT) mouth *= 1.2f;
    const float *f = formant_hz[s->vowel < MUMBLE_VOWELS ? s->vowel : MUMBLE_U];
    h->target[0] = (330.0f + (f[0] - 330.0f) * mouth) * tube;
    h->target[1] = (1450.0f + (f[1] - 1450.0f) * mouth) * tube;
    h->target[2] = f[2] * tube;
    h->target[3] = 3400.0f * tube;
    for (unsigned k = 0; k < 4; ++k) {
        if (h->target[k] > RATE * 0.45f) h->target[k] = RATE * 0.45f;
        h->start[k] = h->target[k];
    }
    if (s->locus) { h->start[0] = 260.0f * tube; h->start[1] = (float)s->locus * 10.0f * tube; }
    h->air = (float)v->value[MUMBLE_AIR] * 0.01f;
    for (unsigned k = 0; k < 4; ++k) {
        h->radius[k] = exp2_approx(-PI * bandwidth_hz[k] * (1.0f + 0.6f * h->air) / RATE * 1.442695f);
        h->y1[k] = h->y2[k] = 0.0f;
    }
    h->dur = (uint32_t)s->dur_ms * (FM1_SAMPLE_RATE / 100U) / 10U;
    if (h->dur < 64) h->dur = 64;
    h->inv_dur = 1.0f / (float)h->dur;
    h->frames = h->dur + FM1_SAMPLE_RATE * 3U / 100U;
    h->attack = h->dur * 2U / 5U; if (h->attack > 1985U) h->attack = 1985U;    /* 45 ms */
    h->release = h->dur / 2U;    if (h->release > 3528U) h->release = 3528U;   /* 80 ms */
    /* A spoken syllable sags in pitch and level; a held note stays put. */
    h->hold = (s->flags & MUMBLE_HOLD) != 0;
    h->fall = h->hold ? 0.0f : 0.7f * h->inv_dur;
    h->fade = h->hold ? 0.0f : 0.2f * h->inv_dur;
    if (h->hold) h->release = 5292U;                                             /* 120 ms */
    h->tag = h->hold && s->index < MUMBLE_LETTERS ? (uint8_t)(s->index + 1U) : 0U;
    h->gen = s->gen;
    h->glide = s->locus ? 1543U : 0U;                                            /* 35 ms */
    if (!h->attack) h->attack = 1;
    if (!h->release) h->release = 1;
    h->inv_attack = 1.0f / (float)h->attack;
    h->inv_release = 1.0f / (float)h->release;
    h->inv_glide = h->glide ? 1.0f / (float)h->glide : 0.0f;
    h->wobble_phase = wobble_phase + (float)delay * (2.0f / RATE);
    h->pitch = (float)v->value[MUMBLE_PITCH];
    h->semis = (float)s->cents * 0.01f;
    h->gain = (float)s->gain * (0.5f / 64.0f);
    h->jitter = (float)v->value[MUMBLE_JITTER] * 0.01f;
    h->wobble = (float)v->value[MUMBLE_WOBBLE] * 0.01f;
    /* CRUSH: hold each sample 1..9 frames and quantise to 12..3 bits. */
    unsigned crush = v->value[MUMBLE_CRUSH];
    h->crush_hold = (uint8_t)(1U + crush / 12U);
    h->crush_count = 0;
    h->levels = crush ? exp2_approx(12.0f - 0.09f * (float)crush) : 0.0f;
    h->crushed = 0.0f;
    /* RADIO: a narrowing band (300 Hz..900 Hz up to 3.5 kHz..1.5 kHz), driven. */
    h->radio = (float)v->value[MUMBLE_RADIO] * 0.01f;
    h->hp_a = 2.0f * PI * (300.0f + 600.0f * h->radio) / RATE;
    h->lp_a = 2.0f * PI * (3500.0f - 2000.0f * h->radio) / RATE;
    h->drive = 1.0f + 3.0f * h->radio;
    h->hp = h->lp = 0.0f;
    h->echo = (float)v->value[MUMBLE_ECHO] * 0.01f;
    h->room = (float)v->value[MUMBLE_ROOM] * 0.01f;
    h->phase = 0.0f; h->period = 1.0f; h->amp = 1.0f; h->last = 0.0f;
    h->noise = seed | 1U;
    h->vowel = s->vowel;
    h->track = s->track < MUMBLE_CAST ? s->track : 0U;
    h->pos = -delay;
    h->active = 1;
}

static inline __attribute__((always_inline)) float noise(uint32_t *state) /* [-1, 1) */
{
    *state = *state * 1664525U + 1013904223U;
    return (float)(int32_t)*state * (1.0f / 2147483648.0f);
}
static inline __attribute__((always_inline)) float gauss(uint32_t *state)
{
    return (noise(state) + noise(state) + noise(state)) * 0.66f;
}
/* Rounds to the nearest integer without libm. */
static inline __attribute__((always_inline)) float round_float(float x)
{
    return (float)(int32_t)(x + (x >= 0.0f ? 0.5f : -0.5f));
}

static FM1_AUDIO_CODE void hum_render(struct hum *h, unsigned frames)
{
    unsigned i = 0;
    if (h->pos < 0) {
        unsigned wait = (unsigned)-h->pos;
        if (wait >= frames) { h->pos += (int32_t)frames; return; }
        i = wait; h->pos = 0;
    }
    while (i < frames && h->active) {
        unsigned run = frames - i;
        if (run > SUB - ((unsigned)h->pos & (SUB - 1U))) run = SUB - ((unsigned)h->pos & (SUB - 1U));
        /* Per sub-block: formant glide, pitch fall and wobble. */
        float t = (float)h->pos;
        float g = h->glide ? smooth(t * h->inv_glide) : 1.0f;
        for (unsigned k = 0; k < 4; ++k) {
            float f = h->start[k] + (h->target[k] - h->start[k]) * g;
            float r = h->radius[k];
            h->c[k] = -r * r;
            h->b[k] = 2.0f * r * cos_approx(2.0f * PI * f / RATE);
            h->a[k] = 1.0f - h->b[k] - h->c[k];
        }
        float wp = h->wobble_phase;
        wp -= (float)(int)wp;
        float q = wp < 0.5f ? wp : wp - 0.5f, wave = 16.0f * q * (0.5f - q);
        h->wobble_phase = wp + (float)run * (2.0f / RATE);
        if (wp >= 0.5f) wave = -wave;
        float semis = h->semis - h->fall * t + h->wobble * wave;
        float inc = h->pitch * exp2_approx(semis * (1.0f / 12.0f)) / RATE;

        for (unsigned end = i + run; i < end; ++i, ++h->pos) {
            uint32_t pos = (uint32_t)h->pos;
            if (pos >= h->frames) { h->active = 0; break; }
            h->phase += inc * h->period;
            if (h->phase >= 1.0f) {
                h->phase -= 1.0f;
                if (h->phase >= 1.0f) h->phase = 0.0f;
                /* Vocal folds never buzz the same way twice. */
                h->period = 1.0f + gauss(&h->noise) * 0.012f * h->jitter;
                h->amp = 1.0f + gauss(&h->noise) * 0.08f * h->jitter;
            }
            float x = h->phase * GLOTTAL;
            unsigned n = (unsigned)x;
            float frac = x - (float)n;
            float flow = glottal_flow[n] + (glottal_flow[n + 1] - glottal_flow[n]) * frac;
            float slope = glottal_slope[n] + (glottal_slope[n + 1] - glottal_slope[n]) * frac;
            float y = slope * 0.25f * h->amp + noise(&h->noise) * h->air * 0.5f * (0.4f + 0.6f * flow);
            for (unsigned k = 0; k < 4; ++k) {
                float v = h->a[k] * y + h->b[k] * h->y1[k] + h->c[k] * h->y2[k];
                h->y2[k] = h->y1[k]; h->y1[k] = v; y = v;
            }
            /* Gentle pre-emphasis keeps the upper formants audible. */
            float bright = y - 0.82f * h->last;
            h->last = y;
            if (h->radio > 0.0f) {
                h->hp += h->hp_a * (bright - h->hp);
                h->lp += h->lp_a * ((bright - h->hp) - h->lp);
                float d = h->lp * h->drive;
                if (d > 1.5f) d = 1.5f; else if (d < -1.5f) d = -1.5f;
                bright = d - d * d * d * (4.0f / 27.0f);
            }
            /* A voice swells, holds and lets go; it is not struck. */
            float env;
            if (pos < h->attack) env = smooth((float)pos * h->inv_attack);
            else if (pos + h->release < h->dur) env = 1.0f;
            else if (pos < h->dur) env = 1.0f - smooth((float)(pos + h->release - h->dur) * h->inv_release);
            else env = 0.0f;
            env *= 1.0f - h->fade * (float)pos;
            float out = bright * env * h->gain;
            if (h->levels > 0.0f) {
                if (h->crush_count == 0) h->crushed = round_float(out * h->levels) / h->levels;
                if (++h->crush_count >= h->crush_hold) h->crush_count = 0;
                out = h->crushed;
            }
            mix[i] += out;
            echo_send[i] += out * h->echo;
            room_send[i] += out * h->room;
        }
    }
}

static inline __attribute__((always_inline)) int16_t to_line(float x)
{
    float s = x * 32767.0f;
    return (int16_t)(s > 32767.0f ? 32767.0f : s < -32767.0f ? -32767.0f : s);
}

/* One half-rate step of the shared echo and room; returns the wet sample. */
static FM1_AUDIO_CODE float effects_step(float echo_in, float room_in)
{
    /* Echo: 250 ms, darkening repeats. */
    float back = (float)echo_line[echo_at] * (1.0f / 32768.0f);
    echo_tone += 0.45f * (back - echo_tone);
    echo_line[echo_at] = to_line(echo_in + 0.42f * echo_tone);
    if (++echo_at == ECHO_LENGTH) echo_at = 0;
    /* Room: four damped combs into two allpasses. */
    float x = room_in * 0.35f, room = 0.0f;
    unsigned base = 0;
    for (unsigned c = 0; c < 4; ++c) {
        int16_t *line = comb_line + base;
        float y = (float)line[comb_at[c]] * (1.0f / 32768.0f);
        comb_tone[c] = y * 0.7f + comb_tone[c] * 0.3f;
        line[comb_at[c]] = to_line(x + 0.8f * comb_tone[c]);
        if (++comb_at[c] == comb_length[c]) comb_at[c] = 0;
        room += y;
        base += comb_length[c];
    }
    base = 0;
    for (unsigned a = 0; a < 2; ++a) {
        int16_t *line = allpass_line + base;
        float b = (float)line[allpass_at[a]] * (1.0f / 32768.0f);
        line[allpass_at[a]] = to_line(room + 0.5f * b);
        room = b - room;
        if (++allpass_at[a] == allpass_length[a]) allpass_at[a] = 0;
        base += allpass_length[a];
    }
    return back + room * 0.3f;
}

FM1_AUDIO_CODE void mumble_render(int32_t *output)
{
    if (!enabled || !output) return;
    uint8_t wanted = request;
    if (wanted != ack) {
        ack = wanted;
        next_syllable = 0;
        clock_frames = 0;
        line_playing = 1;
        status_line = scripts[wanted].line;
    }
    const struct script *s = &scripts[ack];
    uint32_t now = clock_frames;
    if (line_playing) {
        while (next_syllable < s->count) {
            const struct mumble_syllable *y = &s->syllable[next_syllable];
            uint32_t at = (uint32_t)y->ms * (FM1_SAMPLE_RATE / 100U) / 10U;
            if (at >= now + FM1_BLOCK_FRAMES) break;
            hum_start(y, &s->voice[y->track < MUMBLE_CAST ? y->track : 0], at > now ? (int32_t)(at - now) : 0,
                      (uint32_t)(ack * 977U + next_syllable * 7919U) ^ (now * 2654435761U));
            ++next_syllable;
        }
        if (next_syllable >= s->count &&
            now >= (uint32_t)s->end_ms * (FM1_SAMPLE_RATE / 100U) / 10U) line_playing = 0;
    }
    clock_frames = now + FM1_BLOCK_FRAMES;

    unsigned any = 0;
    for (unsigned v = 0; v < VOICES; ++v) {
        struct hum *h = &hums[v];
        any |= h->active;
        /* A released letter lets go from where it is (after its swell). */
        if (h->active && h->tag && held_gen[h->tag - 1U] != h->gen) {
            uint32_t from = h->pos > 0 ? (uint32_t)h->pos : 0U;
            if (from < h->attack) from = h->attack;
            if (from + h->release < h->dur) {
                h->dur = from + h->release;
                h->frames = h->dur + FM1_SAMPLE_RATE * 3U / 100U;
            }
            h->tag = 0;
        }
    }
    wobble_phase += (float)FM1_BLOCK_FRAMES * (2.0f / RATE);
    if (wobble_phase >= 1.0f) wobble_phase -= 1.0f;
    if (any) tail_frames = FM1_SAMPLE_RATE * 3U;  /* longest echo/room tail */
    else if (tail_frames > FM1_BLOCK_FRAMES) tail_frames -= FM1_BLOCK_FRAMES;
    else tail_frames = 0;
    if (!any) {
        status_vowel = MUMBLE_VOWELS; status_level = 0;
        for (unsigned t = 0; t < MUMBLE_CAST; ++t) { status_track_vowel[t] = MUMBLE_VOWELS; status_track_level[t] = 0; }
        if (!tail_frames) return;
    }
    /* volatile keeps these loops: no libc memset from audio code. */
    volatile float *dry = mix, *echo = echo_send, *room = room_send;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) dry[i] = echo[i] = room[i] = 0.0f;
    unsigned loudest = VOICES;
    float loud = 0.0f, track_loud[MUMBLE_CAST];
    uint8_t track_vowel[MUMBLE_CAST];
    for (unsigned t = 0; t < MUMBLE_CAST; ++t) { track_loud[t] = 0.0f; track_vowel[t] = MUMBLE_VOWELS; }
    for (unsigned v = 0; v < VOICES; ++v) {
        struct hum *h = &hums[v];
        if (!h->active) continue;
        hum_render(h, FM1_BLOCK_FRAMES);
        if (h->pos > 0 && (uint32_t)h->pos < h->dur) {
            float x = (float)h->pos * h->inv_dur, e = (h->hold ? 0.5f : x < 0.5f ? x : 1.0f - x) * h->gain;
            if (e > loud) { loud = e; loudest = v; }
            if (e > track_loud[h->track]) { track_loud[h->track] = e; track_vowel[h->track] = h->vowel; }
        }
    }
    for (unsigned t = 0; t < MUMBLE_CAST; ++t) {
        float level = track_loud[t] * 1600.0f;
        status_track_vowel[t] = track_vowel[t];
        status_track_level[t] = (uint8_t)(level > 255.0f ? 255.0f : level);
    }
    if (loudest < VOICES) {
        status_vowel = hums[loudest].vowel;
        float level = loud * 1600.0f;
        status_level = (uint8_t)(level > 255.0f ? 255.0f : level);
        status_cents = (int16_t)(hums[loudest].semis * 100.0f);
        status_track = hums[loudest].track;
    } else {
        status_vowel = MUMBLE_VOWELS; status_level = 0;
    }
    /* Effects at half rate; the wet signal steps between pairs. */
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; i += 2) {
        float wet = effects_step(0.5f * (echo_send[i] + echo_send[i + 1]), 0.5f * (room_send[i] + room_send[i + 1]));
        mix[i] += 0.5f * (wet_last + wet);
        mix[i + 1] += wet;
        wet_last = wet;
    }
    /* Soft clip (rational tanh), then mix at -2 dB under full scale. */
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        float x = mix[i] * 1.12f;
        if (x > 3.0f) x = 3.0f; else if (x < -3.0f) x = -3.0f;
        float y = x * (27.0f + x * x) / (27.0f + 9.0f * x * x) * 0.9f;
        int32_t sample = (int32_t)(y * 6600000.0f);
        for (unsigned c = 0; c < 2; ++c) {
            int32_t value = output[i * 2 + c] + sample;
            output[i * 2 + c] = value > 8388607 ? 8388607 : value < -8388608 ? -8388608 : value;
        }
    }
}
