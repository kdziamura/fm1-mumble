/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* MUMBLE voice engine. See mumble.h for the model. */
#include "mumble.h"
#include <string.h>

#define RATE ((float)FM1_SAMPLE_RATE)
#define PI 3.14159265f

const struct mumble_param_info mumble_params[MUMBLE_PARAMS] = {
    {"PITCH", 60, 420}, {"TUBE", 75, 145}, {"MOUTH", 30, 130}, {"AIR", 0, 100},
    {"SPEED", 50, 200}, {"WOBBLE", 0, 150}, {"JITTER", 0, 100}, {"TUNE", 0, 160}};

/*                                            pitch tube mouth air speed wob jit tune */
const struct mumble_character mumble_cast[MUMBLE_CAST] = {
    {"HERO",   0x3c9f, {{135, 100, 100, 22, 100,  0, 50, 100}}},
    {"GUARD",  0x9cd3, {{ 92,  86,  90, 18,  82,  0, 35,  60}}},
    {"SISTER", 0xfb25, {{215, 118, 100, 32, 100,  0, 45, 115}}},
    {"KID",    0x2dad, {{300, 134, 110, 20, 120,  0, 60, 140}}},
    {"PIRATE", 0xbb07, {{108,  92,  45, 38,  92, 10, 80,  80}}},
    {"ELDER",  0x9bfa, {{150,  97,  80, 50,  75, 90, 45,  90}}},
    {"BOT",    0x9d14, {{120, 100, 100,  0, 110,  0,  0,   0}}},
    {"GHOST",  0x5e5c, {{175, 110,  70, 95,  70, 50, 30, 120}}}};

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
 * the newest request, and a free one control can always write.
 * ---------------------------------------------------------------------- */
struct script {
    uint8_t count, line, length;
    uint16_t end_ms;
    struct mumble_voice voice;
    struct mumble_syllable syllable[MUMBLE_SYLLABLES];
    uint16_t char_ms[MUMBLE_TEXT];
};
enum { VOICES = 3, SUB = 32, GLOTTAL = 256 };
struct hum {
    int32_t pos;                 /* <0: not yet started */
    uint32_t frames, dur, attack, release, glide;
    float inv_dur, inv_attack, inv_release, inv_glide, wobble_phase, phase, inc, period, amp, semis, gain, air, jitter, wobble, pitch;
    float start[4], target[4], radius[4], y1[4], y2[4], a[4], b[4], c[4], last;
    uint32_t noise;
    uint8_t active, vowel;
};
static struct script scripts[3];
static volatile uint8_t request, ack;
static volatile uint8_t enabled;
static volatile uint32_t clock_frames;          /* frames since the playing script began */
static volatile uint8_t status_vowel = MUMBLE_VOWELS, status_level, status_line;
static volatile int16_t status_cents;
static struct hum hums[VOICES];
static unsigned next_syllable, line_playing;
static float wobble_phase; /* shared 2 Hz wobble, so overlapping hums agree */
static float glottal_flow[GLOTTAL + 1], glottal_slope[GLOTTAL + 1];
static float mix[FM1_BLOCK_FRAMES];

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

static int say(const char *text, unsigned length, const struct mumble_voice *voice, unsigned line)
{
    if (!text || !voice) return -1;
    struct script *s = free_slot();
    if (length > MUMBLE_TEXT) length = MUMBLE_TEXT;
    s->voice = *voice;
    s->line = (uint8_t)line;
    s->length = (uint8_t)length;
    s->count = (uint8_t)mumble_plan(text, length, voice, 0, s->syllable, MUMBLE_SYLLABLES,
                                    s->char_ms, &s->end_ms);
    publish(s);
    return 0;
}
int mumble_say(const char *text, unsigned length, const struct mumble_voice *voice)
{ return say(text, length, voice, 1); }
int mumble_aside(const char *text, unsigned length, const struct mumble_voice *voice)
{ return say(text, length, voice, 0); }

int mumble_type(const char *text, unsigned length, const struct mumble_voice *voice)
{
    if (!text || !voice || !length) return -1;
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
    s->syllable[0] = last;
    s->count = 1;
    s->line = 0;
    s->length = 0;
    s->end_ms = 0;
    s->voice = *voice;
    publish(s);
    return 1;
}

void mumble_stop(void)
{
    struct script *s = free_slot();
    s->count = 0; s->line = 0; s->length = 0; s->end_ms = 0;
    publish(s);
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
}

/* ------------------------------------------------------------------------
 * Audio.
 * ---------------------------------------------------------------------- */
static FM1_AUDIO_CODE void hum_start(const struct mumble_syllable *s, const struct mumble_voice *v,
                                     int32_t delay, uint32_t seed)
{
    /* Steal the quietest (oldest) voice. */
    struct hum *h = &hums[0];
    for (unsigned i = 0; i < VOICES; ++i) {
        if (!hums[i].active) { h = &hums[i]; break; }
        if (hums[i].pos > h->pos) h = &hums[i];
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
    h->phase = 0.0f; h->period = 1.0f; h->amp = 1.0f; h->last = 0.0f;
    h->noise = seed | 1U;
    h->vowel = s->vowel;
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

static FM1_AUDIO_CODE void hum_render(struct hum *h, float *out, unsigned frames)
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
        float semis = h->semis - 0.7f * t * h->inv_dur + h->wobble * wave;
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
            /* A voice swells, holds and lets go; it is not struck. */
            float env;
            if (pos < h->attack) env = smooth((float)pos * h->inv_attack);
            else if (pos + h->release < h->dur) env = 1.0f;
            else if (pos < h->dur) env = 1.0f - smooth((float)(pos + h->release - h->dur) * h->inv_release);
            else env = 0.0f;
            env *= 1.0f - 0.2f * (float)pos * h->inv_dur;
            out[i] += bright * env * h->gain;
        }
    }
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
            hum_start(y, &s->voice, at > now ? (int32_t)(at - now) : 0,
                      (uint32_t)(ack * 977U + next_syllable * 7919U) ^ (now * 2654435761U));
            ++next_syllable;
        }
        if (next_syllable >= s->count &&
            now >= (uint32_t)s->end_ms * (FM1_SAMPLE_RATE / 100U) / 10U) line_playing = 0;
    }
    clock_frames = now + FM1_BLOCK_FRAMES;

    unsigned any = 0;
    for (unsigned v = 0; v < VOICES; ++v) any |= hums[v].active;
    if (!any) {
        status_vowel = MUMBLE_VOWELS; status_level = 0;
        wobble_phase += (float)FM1_BLOCK_FRAMES * (2.0f / RATE);
        if (wobble_phase >= 1.0f) wobble_phase -= 1.0f;
        return;
    }
    /* volatile keeps this a loop: no libc memset from audio code. */
    volatile float *clear = mix;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) clear[i] = 0.0f;
    unsigned loudest = VOICES;
    float loud = 0.0f;
    for (unsigned v = 0; v < VOICES; ++v) {
        struct hum *h = &hums[v];
        if (!h->active) continue;
        hum_render(h, mix, FM1_BLOCK_FRAMES);
        if (h->pos > 0 && (uint32_t)h->pos < h->dur) {
            float x = (float)h->pos * h->inv_dur, e = x < 0.5f ? x : 1.0f - x;
            if (e * h->gain > loud) { loud = e * h->gain; loudest = v; }
        }
    }
    wobble_phase += (float)FM1_BLOCK_FRAMES * (2.0f / RATE);
    if (wobble_phase >= 1.0f) wobble_phase -= 1.0f;
    if (loudest < VOICES) {
        status_vowel = hums[loudest].vowel;
        float level = loud * 1600.0f;
        status_level = (uint8_t)(level > 255.0f ? 255.0f : level);
        status_cents = (int16_t)(hums[loudest].semis * 100.0f);
    } else {
        status_vowel = MUMBLE_VOWELS; status_level = 0;
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
