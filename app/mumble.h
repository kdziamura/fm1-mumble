/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* MUMBLE: retro-game character voices built from text, no recordings.
 *
 * A line types out on a letter clock. Each vowel group that starts a syllable
 * plays one short "hum": a jittery glottal buzz through a four-formant tube.
 * Punctuation sets the rhythm (pauses) and the tune (phrase declination,
 * question rise, exclamation lift, ellipsis fade, CAPS shout).
 *
 * Control task: mumble_say / mumble_type / mumble_stop / mumble_status.
 * Audio ISR: mumble_render mixes into the finished instrument block. */
#ifndef FM1_MUMBLE_H
#define FM1_MUMBLE_H
#include "fm1.h"

enum { MUMBLE_TEXT = 96, MUMBLE_SYLLABLES = 48, MUMBLE_CAST = 8 };
enum { MUMBLE_A, MUMBLE_E, MUMBLE_I, MUMBLE_O, MUMBLE_OO, MUMBLE_U, MUMBLE_VOWELS };
enum {
    MUMBLE_PITCH,  /* Hz */
    MUMBLE_TUBE,   /* % vocal tract scale: <100 longer/deeper, >100 shorter */
    MUMBLE_MOUTH,  /* % jaw/lip opening */
    MUMBLE_AIR,    /* % breath noise */
    MUMBLE_SPEED,  /* % letter rate */
    MUMBLE_WOBBLE, /* % slow 2 Hz pitch wobble (100 = 1 semitone) */
    MUMBLE_JITTER, /* % per-period and per-syllable imperfection */
    MUMBLE_TUNE,   /* % intonation range */
    MUMBLE_PARAMS
};
struct mumble_voice { uint16_t value[MUMBLE_PARAMS]; };
struct mumble_param_info { const char *name; uint16_t min, max; };
extern const struct mumble_param_info mumble_params[MUMBLE_PARAMS];
struct mumble_character { const char *name; uint16_t color; struct mumble_voice voice; };
extern const struct mumble_character mumble_cast[MUMBLE_CAST];
extern const char *const mumble_vowel_names[MUMBLE_VOWELS];

/* One planned syllable. Times are milliseconds from the start of the line. */
struct mumble_syllable {
    uint16_t ms, dur_ms;
    int16_t cents;      /* pitch offset from the voice */
    uint8_t vowel, locus; /* locus: consonant F2 start / 10 Hz, 0 = none */
    uint8_t gain;       /* 64 = unity */
    uint8_t flags;      /* MUMBLE_SHOUT, MUMBLE_HUM */
    uint8_t index;      /* character that starts it */
};
enum { MUMBLE_SHOUT = 1, MUMBLE_HUM = 2 };

/* Pure text analysis, usable anywhere. open=1 treats the last word as still
 * being typed. char_ms (optional) receives each character's reveal time.
 * Returns the syllable count; *end_ms receives the line length. */
unsigned mumble_plan(const char *text, unsigned length, const struct mumble_voice *voice,
                     unsigned open, struct mumble_syllable *out, unsigned capacity,
                     uint16_t *char_ms, uint16_t *end_ms);

void mumble_init(void);
/* Start speaking a line, replacing any line in progress. 0, or -1 invalid. */
int mumble_say(const char *text, unsigned length, const struct mumble_voice *voice);
/* Speak without a line to reveal (a greeting): status stays not speaking. */
int mumble_aside(const char *text, unsigned length, const struct mumble_voice *voice);
/* Live typing: text already ends with the new character. Speaks its syllable
 * if that character starts one. Returns 1 when a syllable was queued. */
int mumble_type(const char *text, unsigned length, const struct mumble_voice *voice);
void mumble_stop(void);
struct mumble_status {
    uint8_t speaking;  /* a line is still typing */
    uint8_t revealed;  /* characters of that line shown so far */
    uint8_t vowel;     /* loudest sounding vowel, MUMBLE_VOWELS when silent */
    uint8_t level;     /* 0..255 envelope of that vowel */
    int16_t cents;     /* its pitch offset */
};
void mumble_status(struct mumble_status *status);

/* Audio ISR: add the voice to a finished Q23 stereo block. */
FM1_AUDIO_CODE void mumble_render(int32_t *output);
#endif
