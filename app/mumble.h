/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* MUMBLE: retro-game character voices built from text, no recordings.
 *
 * A line types out on a letter clock. Each vowel group that starts a syllable
 * plays one short "hum": a jittery glottal buzz through a four-formant tube.
 * Punctuation sets the rhythm (pauses) and the tune (phrase declination,
 * question rise, exclamation lift, ellipsis fade, CAPS shout).
 *
 * Every character of the cast is a track. A song puts several tracks' lines
 * on one shared clock, once or looping, each from its own offset and at its
 * own level, through its own voice and effects (crush, radio, echo send,
 * room send). A separate live lane plays typing, greetings and sung letters
 * at once without disturbing the song.
 *
 * Control task: mumble_song / mumble_say / mumble_aside / mumble_type /
 * mumble_press / mumble_cut / mumble_stop / mumble_status. Audio ISR: mumble_render mixes
 * into the finished instrument block. */
#ifndef FM1_MUMBLE_H
#define FM1_MUMBLE_H
/* The platform: FM1 Open's SDK, or a port header (firmware/mumble-fm1) that
 * defines FM1_SAMPLE_RATE, FM1_BLOCK_FRAMES, FM1_AUDIO_CODE and FM1_AUDIO_DATA. */
#ifdef MUMBLE_PORT
#include MUMBLE_PORT
#else
#include "fm1.h"
#endif

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
    MUMBLE_CRUSH,  /* % sample-rate and bit reduction */
    MUMBLE_RADIO,  /* % band-limited, driven "speaker" tone */
    MUMBLE_ECHO,   /* % send to the shared 250 ms echo */
    MUMBLE_ROOM,   /* % send to the shared room reverb */
    MUMBLE_PARAMS
};
enum { MUMBLE_PAGES = MUMBLE_PARAMS / 4 };
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
    uint8_t flags;      /* MUMBLE_SHOUT, MUMBLE_HUM, MUMBLE_HOLD */
    uint8_t index;      /* character that starts it; with MUMBLE_HOLD, the letter */
    uint8_t gen;        /* with MUMBLE_HOLD: the press it belongs to */
    uint8_t track;      /* cast member (track) whose voice speaks it */
};
enum { MUMBLE_SHOUT = 1, MUMBLE_HUM = 2, MUMBLE_HOLD = 4 };
enum { MUMBLE_LETTERS = 27 }; /* a..z, then a closed hum */

/* Pure text analysis, usable anywhere. open=1 treats the last word as still
 * being typed. char_ms (optional) receives each character's reveal time.
 * Returns the syllable count; *end_ms receives the line length. */
unsigned mumble_plan(const char *text, unsigned length, const struct mumble_voice *voice,
                     unsigned open, struct mumble_syllable *out, unsigned capacity,
                     uint16_t *char_ms, uint16_t *end_ms);

void mumble_init(void);

/* One track's part of a song; length 0 leaves the track silent. start_ms is
 * its offset into the loop, level its volume in percent (100 = as planned). */
struct mumble_part {
    const char *text; unsigned length; const struct mumble_voice *voice;
    uint16_t start_ms; uint8_t level;
};
/* Play every part on one clock: once (loop_ms 0) or looping every loop_ms,
 * cutting a line that outlasts the loop. restart=1 starts the clock phase_ms
 * into the loop; restart=0 swaps in edited parts and keeps the running phase.
 * Timing is sample-accurate. 0, or -1 invalid. */
int mumble_song(const struct mumble_part parts[MUMBLE_CAST], uint32_t loop_ms, unsigned restart, uint32_t phase_ms);
void mumble_song_stop(void);
/* Milliseconds into the song (into the current pass when looping), -1 when
 * it is not playing. */
int32_t mumble_song_position(void);
/* One track speaks a line once (a one-track song). */
int mumble_say(unsigned track, const char *text, unsigned length, const struct mumble_voice *voice);
/* A line on the live lane (a greeting): it never stops the song. */
int mumble_aside(unsigned track, const char *text, unsigned length, const struct mumble_voice *voice);
/* Live typing: text already ends with the new character. Speaks its syllable
 * on the live lane if that character starts one. Returns 1 when queued. */
int mumble_type(unsigned track, const char *text, unsigned length, const struct mumble_voice *voice);
/* Cut the live lane: drop the rest of its line and let what it sounds go
 * with a short release. The song plays on. */
void mumble_cut(void);
/* Stop the song and the live lane and release every held letter. Effect
 * tails ring out. */
void mumble_stop(void);

/* Play mode: sing a letter's name ("bee", "kay", "em"), holding its last
 * vowel until mumble_release. letter 0..25 is a..z and 26 a closed hum.
 * Every singer sings it at once at their own MIDI note: a choir. It plays on
 * the live lane, over a running song; letters already sounding keep ringing,
 * so chords work. 0, or -1 invalid. */
struct mumble_singer { const struct mumble_voice *voice; uint8_t track, note; };
int mumble_press(unsigned letter, const struct mumble_singer *singers, unsigned count, unsigned shout);
void mumble_release(unsigned letter);
/* Octaves to shift the keyboard so its middle F (MIDI 65) sits nearest the voice. */
int mumble_register(const struct mumble_voice *voice);
extern const char *const mumble_letter_names[MUMBLE_LETTERS];

struct mumble_status {
    int32_t song_ms;   /* mumble_song_position() */
    uint8_t vowel;     /* loudest sounding vowel, MUMBLE_VOWELS when silent */
    uint8_t level;     /* 0..255 envelope of that vowel */
    int16_t cents;     /* its pitch offset */
    uint8_t track;     /* the track it belongs to */
    uint8_t track_vowel[MUMBLE_CAST], track_level[MUMBLE_CAST]; /* per track, as above */
};
void mumble_status(struct mumble_status *status);

/* Audio ISR: add the voices and their effects to a finished Q23 stereo block. */
FM1_AUDIO_CODE void mumble_render(int32_t *output);
#endif
