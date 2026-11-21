/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* MUMBLE voice engine. See mumble.h for the model. */
#include "mumble.h"
#ifndef MUMBLE_PORT
#include <string.h>   /* a port header declares memset itself */
#endif
#if FM1_MUMBLE   /* (the rest of the file) */

/* Control-task code (text analysis, requests to the audio task) is built
 * for size; the audio path keeps the project's speed settings. */
#ifdef __clang__
#define CONTROL __attribute__((cold, minsize))
#else
#define CONTROL __attribute__((cold))
#endif

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
CONTROL static uint32_t hash(const char *s, unsigned n, uint32_t h)
{
    for (unsigned i = 0; i < n; ++i) h = (h ^ (uint8_t)s[i]) * 16777619U;
    return h;
}
CONTROL static float random_unit(uint32_t *state) /* [-1, 1) */
{
    uint32_t t = (*state += 0x6d2b79f5U);
    t = (t ^ (t >> 15)) * (t | 1U);
    t ^= t + (t ^ (t >> 7)) * (t | 61U);
    t ^= t >> 14;
    return (float)(t >> 8) * (2.0f / 16777216.0f) - 1.0f;
}
CONTROL static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
CONTROL static int is_letter(char c) { c = lower(c); return c >= 'a' && c <= 'z'; }
CONTROL static int is_plain_vowel(char c)
{
    c = lower(c);
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}
/* y is a vowel after a consonant ("happy", "my"), not at a word start ("yes"). */
CONTROL static int vowel_at(const char *w, unsigned i)
{
    if (is_plain_vowel(w[i])) return 1;
    return lower(w[i]) == 'y' && i > 0 && !is_plain_vowel(w[i - 1]);
}
CONTROL static uint8_t vowel_of(const char *g, unsigned n)
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
CONTROL static uint8_t locus_of(char c)
{
    switch (lower(c)) {
    case 'b': case 'p': case 'm': case 'f': case 'v': case 'w': return 85;
    case 't': case 'd': case 'n': case 's': case 'z': case 'l': case 'r': case 'c': return 175;
    case 'k': case 'g': case 'q': case 'x': return 210;
    case 'j': case 'y': return 230;
    default: return 0;
    }
}
CONTROL static unsigned pause_of(char c)
{
    switch (c) {
    case ',': case ';': case ':': return 3;
    case '-': return 2;
    case '.': case '?': case '!': case '\n': return 6;
    default: return 1;
    }
}
CONTROL static int ends_phrase(char c)
{
    return c == ',' || c == ';' || c == ':' || c == '.' || c == '?' || c == '!' || c == '\n';
}
CONTROL static float voice_unit(const struct mumble_voice *v, unsigned p) { return (float)v->value[p] * 0.01f; }

CONTROL unsigned mumble_plan(const char *text, unsigned length, const struct mumble_voice *voice,
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
 * Control -> audio handoff. Two streams, each a three-slot mailbox (the slot
 * audio plays, the newest request, and a free one control can always write):
 *   the SONG: every track's line on one shared clock, once or looping, each
 *     track from its own offset. Its timing is absolute (audio frames), so
 *     republishing an edited song keeps the loop's phase;
 *   the LIVE lane: typing, greetings and sung letters, started at once and
 *     never cutting the song.
 * ---------------------------------------------------------------------- */
enum { SONG_SYLLABLES = 240, LIVE_SYLLABLES = 48 };   /* (counts are uint8_t) */
struct song {
    uint8_t count, restart;
    uint8_t first[MUMBLE_CAST + 1];      /* track t owns syllables first[t]..first[t+1]-1, in time order */
    uint32_t period;                     /* loop length in frames; 0 plays once */
    uint32_t phase;                      /* on restart: frames already into the loop */
    uint32_t length;                     /* once: frames until the last track has finished */
    uint32_t start[MUMBLE_CAST];         /* each track's offset into the loop, frames */
    struct mumble_voice voice[MUMBLE_CAST];
    struct mumble_syllable syllable[SONG_SYLLABLES];
};
struct live {
    uint8_t count;
    struct mumble_voice voice[MUMBLE_CAST];
    struct mumble_syllable syllable[LIVE_SYLLABLES];
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
    uint8_t live, cut;           /* started by the live lane, and the cut count it belongs to */
    uint8_t song, song_cut;      /* started by the song, and the song stop it belongs to */
};
static struct song songs[3];
static struct live lives[3];
/* The presses' syllables last put on the live lane, and when (the audio
 * clock): a new press keeps those not yet started, so chords and quick runs
 * keep every letter's held syllable. Anything else on the live lane drops them. */
static struct {
    uint8_t count;
    uint32_t origin;
    struct mumble_syllable syllable[LIVE_SYLLABLES];
    struct mumble_voice voice[MUMBLE_CAST];
} pressed;
static volatile uint8_t song_request, song_ack, live_request, live_ack;
static volatile uint8_t enabled;
static volatile uint32_t now_frames;            /* the audio clock */
static volatile uint32_t song_origin;           /* start of the current loop cycle */
static volatile uint8_t song_running;
static volatile uint32_t song_period;           /* loop length of the playing song */
static uint8_t song_cursor[MUMBLE_CAST];
static uint32_t song_cycle[MUMBLE_CAST];        /* where this cycle starts for each track */
static uint32_t live_origin;
static uint8_t live_next;
/* Bumped by every press and release of a letter; a held hum lets go when its
 * letter's count moves past the press it started from. */
static volatile uint8_t held_gen[MUMBLE_LETTERS];
/* Bumped by mumble_cut: live-lane hums from before it let go. */
static volatile uint8_t live_cut;
/* Bumped by mumble_song_stop: the song's hums let go (a take's note can sustain for seconds). */
static volatile uint8_t song_cut;
static volatile uint8_t status_vowel = MUMBLE_VOWELS, status_level, status_track;
static volatile uint8_t status_track_vowel[MUMBLE_CAST], status_track_level[MUMBLE_CAST];
static volatile int16_t status_cents;
static struct hum hums[VOICES];
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
/* The metronome: beats a loop and each beat's frames (set by the control
 * task), and the click ringing: a damped sine, y = k1 y1 - k2 y2. */
static volatile uint8_t click_beats;
static volatile uint32_t click_beat_frames;
static uint32_t click_left;
static float click_y1, click_y2, click_k1, click_k2;
static float click_coef[2][3];                   /* plain, accented: k1, k2, first sample */

CONTROL void mumble_init(void)
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
    memset(songs, 0, sizeof songs);
    memset(lives, 0, sizeof lives);
    pressed.count = 0;
    memset(hums, 0, sizeof hums);
    memset(echo_line, 0, sizeof echo_line);
    memset(comb_line, 0, sizeof comb_line);
    memset(allpass_line, 0, sizeof allpass_line);
    echo_at = 0; echo_tone = wet_last = 0.0f; tail_frames = 0;
    /* Clicks: 1.5 kHz, accented 2.2 kHz, fading by 60 dB in about 25 ms. */
    for (unsigned a = 0; a < 2; ++a) {
        float w = 2.0f * PI * (a ? 2200.0f : 1500.0f) / RATE, r = 0.9937f;
        click_coef[a][0] = 2.0f * r * cos_approx(w);
        click_coef[a][1] = r * r;
        click_coef[a][2] = (a ? 0.16f : 0.10f) * cos_approx(w - 0.5f * PI);   /* amplitude * sin(w) */
    }
    click_beats = 0; click_left = 0; click_y1 = click_y2 = 0.0f;
    for (unsigned i = 0; i < 4; ++i) { comb_at[i] = 0; comb_tone[i] = 0.0f; }
    allpass_at[0] = allpass_at[1] = 0;
    song_request = song_ack = live_request = live_ack = 0;
    song_running = 0;
    enabled = 1;
}

CONTROL static unsigned free_index(unsigned ack, unsigned request)
{
    for (unsigned i = 0; i < 3; ++i)
        if (i != ack && i != request) return i;
    return (ack + 1U) % 3U;
}
CONTROL static uint32_t ms_frames(uint32_t ms) { return (uint32_t)((uint64_t)ms * FM1_SAMPLE_RATE / 1000U); }

/* Stable insertion sort by start time: audio walks syllables in order. */
CONTROL static void sort_syllables(struct mumble_syllable *s, unsigned n)
{
    for (unsigned i = 1; i < n; ++i) {
        struct mumble_syllable x = s[i];
        unsigned j = i;
        while (j > 0 && s[j - 1].ms > x.ms) { s[j] = s[j - 1]; --j; }
        s[j] = x;
    }
}

CONTROL static unsigned letter_syllables(unsigned letter, unsigned note, const struct mumble_voice *voice,
                                         unsigned shout, struct mumble_syllable *out);
CONTROL static unsigned sound_syllable(unsigned key, const struct mumble_sound *sound, unsigned note,
                                       const struct mumble_voice *voice, unsigned shout, struct mumble_syllable *out);
/* A recorded note on a track: its letter's syllables (or its one syllable's
 * sound) from note->ms, the last sustained until the key went up (then its
 * 120 ms release). */
CONTROL static unsigned note_syllables(const struct mumble_note *note, const struct mumble_voice *voice,
                                       struct mumble_syllable *out)
{
    unsigned got = note->syllable ? sound_syllable(note->letter, &note->sound, note->note, voice, note->shout, out)
                                  : letter_syllables(note->letter, note->note, voice, note->shout, out);
    for (unsigned i = 0; i < got; ++i) {
        struct mumble_syllable *y = &out[i];
        uint32_t offset = y->ms, at = (uint32_t)note->ms + offset;
        y->ms = (uint16_t)(at > 0xFFFFU ? 0xFFFFU : at);
        if (i + 1 < got) continue;
        uint32_t held = note->len_ms > offset ? note->len_ms - offset : 0U;
        if (held < 60U) held = 60U;
        y->dur_ms = (uint16_t)(held + 120U > 0xFFFFU ? 0xFFFFU : held + 120U);
        y->flags = (uint8_t)((y->flags & ~MUMBLE_HOLD) | MUMBLE_SUSTAIN);
    }
    return got;
}

CONTROL int mumble_song(const struct mumble_part parts[MUMBLE_CAST], uint32_t loop_ms, unsigned restart, uint32_t phase_ms)
{
    if (!parts) return -1;
    unsigned slot = free_index(song_ack, song_request);
    struct song *s = &songs[slot];
    unsigned n = 0;
    uint32_t longest = 0;
    s->period = ms_frames(loop_ms);
    for (unsigned t = 0; t < MUMBLE_CAST; ++t) {
        const struct mumble_part *p = &parts[t];
        s->first[t] = (uint8_t)n;
        s->start[t] = 0;
        unsigned has_text = p->text && p->length, has_notes = p->notes && p->note_count;
        if (!p->voice || (!has_text && !has_notes)) continue;
        unsigned from = n;
        uint32_t end = 0;
        if (has_text) {
            unsigned length = p->length > MUMBLE_TEXT ? MUMBLE_TEXT : p->length, capacity = SONG_SYLLABLES - n;
            if (capacity > MUMBLE_SYLLABLES) capacity = MUMBLE_SYLLABLES;
            uint16_t text_end = 0;
            n += mumble_plan(p->text, length, p->voice, 0, s->syllable + n, capacity, NULL, &text_end);
            end = text_end;
        }
        for (unsigned k = 0; has_notes && k < p->note_count && SONG_SYLLABLES - n >= 3U; ++k) {
            const struct mumble_note *q = &p->notes[k];
            if (q->letter >= MUMBLE_LETTERS || q->note > 127U) continue;
            if (s->period && ms_frames(q->ms) >= s->period) continue;   /* past the end of a shorter loop */
            n += note_syllables(q, p->voice, s->syllable + n);
            if ((uint32_t)q->ms + q->len_ms + 120U > end) end = (uint32_t)q->ms + q->len_ms + 120U;
        }
        unsigned level = p->level > 100U ? 100U : p->level;
        for (unsigned i = from; i < n; ++i) {
            s->syllable[i].track = (uint8_t)t;
            s->syllable[i].gain = (uint8_t)(s->syllable[i].gain * level / 100U);
        }
        sort_syllables(s->syllable + from, n - from);   /* the line and the take, in time order */
        s->voice[t] = *p->voice;
        s->start[t] = ms_frames(p->start_ms);
        if (s->period) s->start[t] %= s->period;
        if (s->start[t] + ms_frames(end) > longest) longest = s->start[t] + ms_frames(end);
    }
    s->first[MUMBLE_CAST] = (uint8_t)n;
    s->count = (uint8_t)n;
    s->length = longest;
    s->restart = (uint8_t)(restart != 0);
    s->phase = s->period ? ms_frames(phase_ms) % s->period : 0;
    song_request = (uint8_t)slot;
    return 0;
}
CONTROL int mumble_say(unsigned track, const char *text, unsigned length, const struct mumble_voice *voice)
{
    if (!text || !voice || track >= MUMBLE_CAST) return -1;
    struct mumble_part parts[MUMBLE_CAST];
    memset(parts, 0, sizeof parts);
    parts[track] = (struct mumble_part){text, length, voice, 0, 100, NULL, 0};
    return mumble_song(parts, 0, 1, 0);
}
CONTROL void mumble_metronome(unsigned beats, uint32_t loop_ms)
{
    uint32_t period = ms_frames(loop_ms);
    if (beats > 64U || !period) beats = 0;
    click_beats = 0;                                /* (the audio sees no half-set pair) */
    if (!beats) return;
    click_beat_frames = period / beats;
    click_beats = (uint8_t)beats;
}
CONTROL void mumble_song_stop(void)
{
    song_cut = (uint8_t)(song_cut + 1U);
    unsigned slot = free_index(song_ack, song_request);
    songs[slot].count = 0;
    songs[slot].period = songs[slot].length = 0;
    songs[slot].restart = 1;
    for (unsigned t = 0; t <= MUMBLE_CAST; ++t) songs[slot].first[t] = 0;
    song_request = (uint8_t)slot;
}
CONTROL int32_t mumble_song_position(void)
{
    if (!song_running) return -1;
    uint32_t frames = now_frames - song_origin, period = song_period;
    if (period) while (frames >= period) frames -= period;   /* the origin moves on at the next block */
    return (int32_t)((uint64_t)frames * 1000U / FM1_SAMPLE_RATE);
}

CONTROL static struct live *live_slot(unsigned *slot)
{
    *slot = free_index(live_ack, live_request);
    pressed.count = 0;
    return &lives[*slot];
}
CONTROL int mumble_aside(unsigned track, const char *text, unsigned length, const struct mumble_voice *voice)
{
    if (!text || !voice || track >= MUMBLE_CAST) return -1;
    unsigned slot;
    struct live *s = live_slot(&slot);
    unsigned n = mumble_plan(text, length, voice, 0, s->syllable, LIVE_SYLLABLES, NULL, NULL);
    for (unsigned i = 0; i < n; ++i) s->syllable[i].track = (uint8_t)track;
    s->voice[track] = *voice;
    s->count = (uint8_t)n;
    live_request = (uint8_t)slot;
    return 0;
}

CONTROL int mumble_type(unsigned track, const char *text, unsigned length, const struct mumble_voice *voice)
{
    if (!text || !voice || !length || track >= MUMBLE_CAST) return -1;
    if (length > MUMBLE_TEXT) { text += length - MUMBLE_TEXT; length = MUMBLE_TEXT; }
    struct mumble_syllable planned[MUMBLE_SYLLABLES];
    unsigned n = mumble_plan(text, length, voice, 1, planned, MUMBLE_SYLLABLES, NULL, NULL);
    if (!n || planned[n - 1].index != length - 1) return 0;
    struct mumble_syllable last = planned[n - 1];
    /* Typing has no future: the pitch falls with each syllable of the phrase. */
    unsigned k = 0;
    for (unsigned i = n - 1; i > 0; --i) {
        unsigned split = 0;
        for (unsigned c = planned[i - 1].index; c < planned[i].index; ++c)
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
    unsigned slot;
    struct live *s = live_slot(&slot);
    s->syllable[0] = last;
    s->count = 1;
    s->voice[track] = *voice;
    live_request = (uint8_t)slot;
    return 1;
}

CONTROL void mumble_cut(void)
{
    unsigned slot;
    struct live *s = live_slot(&slot);
    s->count = 0;
    live_request = (uint8_t)slot;
    live_cut = (uint8_t)(live_cut + 1U);
}

CONTROL void mumble_stop(void)
{
    mumble_song_stop();
    unsigned slot;
    struct live *s = live_slot(&slot);
    s->count = 0;
    live_request = (uint8_t)slot;
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
CONTROL static int cents_from_a4(float hz)
{
    const float up = 1.0594631f;
    float f = 440.0f;
    int c = 0;
    if (hz < 20.0f) hz = 20.0f;
    while (hz >= f * up) { f *= up; c += 100; }
    while (hz < f) { f /= up; c -= 100; }
    return c + (int)(100.0f * (hz - f) / (f * up - f) + 0.5f);
}

CONTROL int mumble_register(const struct mumble_voice *voice)
{
    if (!voice) return 0;
    int from_f4 = cents_from_a4((float)voice->value[MUMBLE_PITCH]) + 400; /* F4 is 400 cents under A4 */
    return from_f4 >= 0 ? (from_f4 + 600) / 1200 : -((-from_f4 + 600) / 1200);
}

/* A letter's name sung at a MIDI note: up to three syllables from ms 0, the
 * last one held (MUMBLE_HOLD) for 8 s at most. */
CONTROL static unsigned letter_syllables(unsigned letter, unsigned note, const struct mumble_voice *voice,
                                         unsigned shout, struct mumble_syllable *out)
{
    unsigned syllables = 0;
    while (syllables < 3 && (syllables == 0 || letter_sound[letter][syllables][0] || letter_sound[letter][syllables][1]))
        ++syllables;
    int cents = ((int)note - 69) * 100 - cents_from_a4((float)voice->value[MUMBLE_PITCH]);
    unsigned speed = voice->value[MUMBLE_SPEED] ? voice->value[MUMBLE_SPEED] : 100U;
    for (unsigned i = 0; i < syllables; ++i) {
        struct mumble_syllable *y = &out[i];
        const uint8_t *sound = letter_sound[letter][i];
        y->ms = (uint16_t)(i * 9000U / speed);              /* 90 ms apart at 100% */
        y->dur_ms = (uint16_t)(i + 1 < syllables ? 14000U / speed : 8000U);
        y->cents = (int16_t)cents;
        y->vowel = sound[0];
        y->locus = (uint8_t)(sound[1] * 10U);
        y->gain = shout ? 83 : 64;
        y->flags = (uint8_t)(sound[2] | (shout ? MUMBLE_SHOUT : 0) | (i + 1 == syllables ? MUMBLE_HOLD : 0));
        y->index = (uint8_t)letter;
        y->gen = 0;
        y->track = 0;
    }
    return syllables;
}

/* One syllable's sound sung at a MIDI note from ms 0, held (MUMBLE_HOLD) on
 * the key for 8 s at most. */
CONTROL static unsigned sound_syllable(unsigned key, const struct mumble_sound *sound, unsigned note,
                                       const struct mumble_voice *voice, unsigned shout, struct mumble_syllable *out)
{
    unsigned loud = shout || (sound->flags & MUMBLE_SHOUT);
    out->ms = 0;
    out->dur_ms = 8000U;
    out->cents = (int16_t)(((int)note - 69) * 100 - cents_from_a4((float)voice->value[MUMBLE_PITCH]));
    out->vowel = sound->vowel < MUMBLE_VOWELS ? sound->vowel : (uint8_t)MUMBLE_U;
    out->locus = sound->locus;
    out->gain = loud ? 83 : 64;
    out->flags = (uint8_t)((sound->flags & MUMBLE_HUM) | (loud ? MUMBLE_SHOUT : 0) | MUMBLE_HOLD);
    out->index = (uint8_t)key;
    out->gen = 0;
    out->track = 0;
    return 1;
}

static int press(unsigned letter, const struct mumble_sound *sound, const struct mumble_singer *singers,
                 unsigned count, unsigned shout);
CONTROL int mumble_press(unsigned letter, const struct mumble_singer *singers, unsigned count, unsigned shout)
{
    return press(letter, NULL, singers, count, shout);
}
CONTROL int mumble_press_sound(unsigned key, const struct mumble_sound *sound, const struct mumble_singer *singers,
                               unsigned count, unsigned shout)
{
    return sound ? press(key, sound, singers, count, shout) : -1;
}
CONTROL static int press(unsigned letter, const struct mumble_sound *sound, const struct mumble_singer *singers,
                         unsigned count, unsigned shout)
{
    if (letter >= MUMBLE_LETTERS || !singers || !count || count > MUMBLE_CAST) return -1;
    for (unsigned j = 0; j < count; ++j)
        if (!singers[j].voice || singers[j].track >= MUMBLE_CAST || singers[j].note > 127U) return -1;
    /* The earlier presses' syllables still to come, re-timed from now. Not yet
     * taken by the audio (a chord in one frame): all of them. (Should the audio
     * take it meanwhile, a first syllable may start twice: harmless.) */
    uint32_t now = now_frames;
    unsigned taken = live_ack == live_request, carried = pressed.count;
    struct mumble_syllable keep[LIVE_SYLLABLES];
    struct mumble_voice keep_voice[MUMBLE_CAST];
    memcpy(keep_voice, pressed.voice, sizeof keep_voice);
    unsigned n = 0;
    for (unsigned i = 0; i < carried; ++i) {
        const struct mumble_syllable *y = &pressed.syllable[i];
        int32_t left = (int32_t)(pressed.origin + (uint32_t)y->ms * (FM1_SAMPLE_RATE / 100U) / 10U - now);
        if (taken && left <= 0) continue;               /* started already */
        keep[n] = *y;
        keep[n].ms = (uint16_t)(left > 0 ? (uint32_t)left * 10U / (FM1_SAMPLE_RATE / 100U) : 0U);
        ++n;
    }
    unsigned slot;
    struct live *s = live_slot(&slot);
    memcpy(s->syllable, keep, n * sizeof keep[0]);
    for (unsigned i = 0; i < n; ++i) s->voice[keep[i].track] = keep_voice[keep[i].track];
    uint8_t gen = (uint8_t)(held_gen[letter] + 1U);
    for (unsigned j = 0; j < count && n + 3U <= LIVE_SYLLABLES; ++j) {
        const struct mumble_singer *who = &singers[j];
        s->voice[who->track] = *who->voice;
        unsigned got = sound ? sound_syllable(letter, sound, who->note, who->voice, shout, s->syllable + n)
                             : letter_syllables(letter, who->note, who->voice, shout, s->syllable + n);
        for (unsigned i = 0; i < got; ++i, ++n) {
            s->syllable[n].gen = gen;
            s->syllable[n].track = who->track;
        }
    }
    sort_syllables(s->syllable, n);
    s->count = (uint8_t)n;
    pressed.count = (uint8_t)n;
    pressed.origin = now;
    memcpy(pressed.syllable, s->syllable, n * sizeof s->syllable[0]);
    memcpy(pressed.voice, s->voice, sizeof pressed.voice);
    held_gen[letter] = gen;
    live_request = (uint8_t)slot;
    return 0;
}

CONTROL void mumble_release(unsigned letter)
{
    if (letter < MUMBLE_LETTERS) held_gen[letter] = (uint8_t)(held_gen[letter] + 1U);
}

CONTROL void mumble_status(struct mumble_status *status)
{
    if (!status) return;
    status->song_ms = mumble_song_position();
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
static FM1_AUDIO_CODE struct hum *hum_start(const struct mumble_syllable *s, const struct mumble_voice *v,
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
    h->hold = (s->flags & (MUMBLE_HOLD | MUMBLE_SUSTAIN)) != 0;
    h->fall = h->hold ? 0.0f : 0.7f * h->inv_dur;
    h->fade = h->hold ? 0.0f : 0.2f * h->inv_dur;
    if (h->hold) h->release = 5292U;                                             /* 120 ms */
    h->tag = (s->flags & MUMBLE_HOLD) && s->index < MUMBLE_LETTERS ? (uint8_t)(s->index + 1U) : 0U;
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
    h->live = 0;
    h->song = 0;
    h->active = 1;
    return h;
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

static inline __attribute__((always_inline)) uint32_t syllable_frames(const struct mumble_syllable *y)
{
    return (uint32_t)y->ms * (FM1_SAMPLE_RATE / 100U) / 10U;
}

/* A new song: keep the loop's phase unless it restarts, then put every
 * track's cursor at its first syllable still to come in its current cycle. */
static FM1_AUDIO_CODE void song_adopt(const struct song *s, uint32_t now)
{
    song_period = s->period;
    if (s->restart) song_origin = now - s->phase;
    else if (s->period)
        while ((int32_t)(now - song_origin) >= (int32_t)s->period) song_origin += s->period;
    for (unsigned t = 0; t < MUMBLE_CAST; ++t) {
        uint32_t cycle = song_origin + s->start[t];
        if (s->period && (int32_t)(cycle - now) > 0) cycle -= s->period;   /* still in the last pass */
        unsigned i = s->first[t];
        while (i < s->first[t + 1] && (int32_t)(cycle + syllable_frames(&s->syllable[i]) - now) < 0) ++i;
        song_cycle[t] = cycle;
        song_cursor[t] = (uint8_t)i;
    }
}

static FM1_AUDIO_CODE void song_tick(const struct song *s, uint32_t now)
{
    uint32_t end = now + FM1_BLOCK_FRAMES;
    if (s->period)
        while ((int32_t)(now - song_origin) >= (int32_t)s->period) song_origin += s->period;
    for (unsigned t = 0; t < MUMBLE_CAST; ++t) {
        unsigned first = s->first[t], last = s->first[t + 1];
        if (first == last) continue;
        for (unsigned pass = 0; pass < 2; ++pass) {
            while (song_cursor[t] < last) {
                const struct mumble_syllable *y = &s->syllable[song_cursor[t]];
                uint32_t at = song_cycle[t] + syllable_frames(y);
                if ((int32_t)(at - end) >= 0) break;
                int32_t delay = (int32_t)(at - now);
                struct hum *h = hum_start(y, &s->voice[t], delay > 0 ? delay : 0,
                                          (uint32_t)(song_ack * 977U + song_cursor[t] * 7919U) ^ (at * 2654435761U));
                h->song = 1;
                h->song_cut = song_cut;
                ++song_cursor[t];
            }
            /* A loop jumps to its next pass on time, cutting a line too long for it. */
            if (!s->period || (int32_t)(song_cycle[t] + s->period - end) >= 0) break;
            song_cycle[t] += s->period;
            song_cursor[t] = (uint8_t)first;
        }
    }
    /* (An empty loop still runs its clock: a take is being recorded onto it.) */
    song_running = (s->count || s->period) && (s->period || (int32_t)(now - song_origin) < (int32_t)s->length);
}

static FM1_AUDIO_CODE void live_tick(const struct live *s, uint32_t now)
{
    uint32_t end = now + FM1_BLOCK_FRAMES;
    while (live_next < s->count) {
        const struct mumble_syllable *y = &s->syllable[live_next];
        uint32_t at = live_origin + syllable_frames(y);
        if ((int32_t)(at - end) >= 0) break;
        int32_t delay = (int32_t)(at - now);
        struct hum *h = hum_start(y, &s->voice[y->track < MUMBLE_CAST ? y->track : 0], delay > 0 ? delay : 0,
                                  (uint32_t)(live_ack * 131U + live_next * 7919U) ^ (now * 2654435761U));
        h->live = 1;
        h->cut = live_cut;
        ++live_next;
    }
}

FM1_AUDIO_CODE void mumble_render(int32_t *output)
{
    if (!enabled || !output) return;
    uint32_t now = now_frames;
    uint8_t wanted = song_request;
    if (wanted != song_ack) { song_ack = wanted; song_adopt(&songs[wanted], now); }
    wanted = live_request;
    if (wanted != live_ack) { live_ack = wanted; live_origin = now; live_next = 0; }
    song_tick(&songs[song_ack], now);
    live_tick(&lives[live_ack], now);
    now_frames = now + FM1_BLOCK_FRAMES;
    /* A beat of the loop in this block: its click starts at that frame. */
    unsigned click_at = FM1_BLOCK_FRAMES, accent = 0, beats = click_beats;
    if (beats && song_running && song_period) {
        uint32_t into = now - song_origin, beat = click_beat_frames;
        for (unsigned k = 0; k <= beats; ++k) {
            uint32_t at = k == beats ? song_period : k * beat;
            if (at >= into && at - into < FM1_BLOCK_FRAMES) { click_at = at - into; accent = (k % 4U) == 0; break; }
        }
    }
    unsigned clicking = click_left || click_at < FM1_BLOCK_FRAMES;

    unsigned any = 0;
    for (unsigned v = 0; v < VOICES; ++v) {
        struct hum *h = &hums[v];
        any |= h->active;
        /* A released letter, a cut live line or a stopped song lets go from
         * where it is (after its swell). */
        if (h->active && ((h->tag && held_gen[h->tag - 1U] != h->gen) || (h->live && h->cut != live_cut) ||
                          (h->song && h->song_cut != song_cut))) {
            uint32_t from = h->pos > 0 ? (uint32_t)h->pos : 0U;
            if (from < h->attack) from = h->attack;
            if (from + h->release < h->dur) {
                h->dur = from + h->release;
                h->frames = h->dur + FM1_SAMPLE_RATE * 3U / 100U;
            }
            h->tag = 0;
            h->live = 0;
            h->song = 0;
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
        if (!tail_frames && !clicking) return;
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
    /* The click joins the dry mix. */
    for (unsigned i = 0; clicking && i < FM1_BLOCK_FRAMES; ++i) {
        if (i == click_at) {
            click_k1 = click_coef[accent][0];
            click_k2 = click_coef[accent][1];
            click_y2 = 0.0f;
            click_y1 = click_coef[accent][2];
            click_left = FM1_SAMPLE_RATE / 40U;     /* 25 ms */
        }
        if (!click_left) continue;
        float y = click_k1 * click_y1 - click_k2 * click_y2;
        click_y2 = click_y1;
        click_y1 = y;
        mix[i] += click_y2;
        --click_left;
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
#endif /* FM1_MUMBLE */
