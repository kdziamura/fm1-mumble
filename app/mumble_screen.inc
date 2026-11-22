/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* The VOICE page: the MUMBLE retro-game voice synthesizer.
 *
 * One line, sung by eight characters. ALGO turns between three modes:
 *   TYPE  keys type the line; a vowel speaks the moment it is typed.
 *   SING  keys sing the line a syllable at a time at their pitch, held while
 *         held. The key that starts a syllable leads it: keys pressed while
 *         it is held sing it too (a chord); once it is let go the next key
 *         moves on, round the line. Each character sings one note at a time;
 *         the keys held are shared out among the choir (selected first).
 *   LOOP  the multitrack view: a lane of the loop per character; the keys
 *         sing as in SING, the knobs set the loop.
 * The loop (always looping, BARS x BPM) plays a recorded sequence: REC
 * records what is sung, pass over pass. A note keeps its time, length,
 * pitch, singer and syllable slot, not the words: it sings the line as it is
 * now, so a new line changes what the loop sings. Each pass of the loop sings
 * from the top of the line.
 *
 *   PLAY loop on/off   REC record (from TYPE: to SING first; the loop stopped,
 *        it waits, lit blinking, and starts with the first key)   GLO the mode
 *   SELECT character   ALGO mode   PRESET page (SING) or tab (LOOP)   SAVE choir
 *   GLO   the system screen (load, battery; EDIT, then held 3 s: reset)   HOME held: help
 *   ARP   the arpeggiator (a tap, as it is let go): where the keys sing, the
 *         keys held are sung one at a time (on the loop's time while it runs),
 *         round the choir, a syllable each. Held, the knobs set it: K1 the
 *         rate, K2 the pattern, K3 octaves, K4 the gate, ALGO swing
 *   TYPE  keys type at the cursor   K1 cursor   K2 the letter under it
 *         OCT- delete   OCT+ CAPS   FX ,  SCL .  ENV ?  LFO !
 *         EDIT a word back; held 1 s: clear the line, then the loop
 *   SING  ALGO / SEQ page (SING only)   K1..K4 sound (EDIT held: back to the character's)
 *         OCT- OCT+ octave   FX from the top of the line   SCL the scale the keys keep to
 *         LFO a new voice for the character, rolled
 *         EDIT clear the selected character's notes; held 1 s: the whole loop
 *   LOOP  ALGO the tab: MIX (K1 the character's level, 0 muted; K2 swing;
 *         K3 bars; K4 BPM), ARP (rate, pattern, octaves, gate), CHORD
 *         (shape, inversion, spread), SCALE (root, scale); EDIT held with a knob: back to where it starts; OCT, FX and
 *         EDIT as in SING
 * Every button says what it did on the hint line.
 *
 * Included by app.c: uses paint.inc's helpers and draw cache. */
#include "mumble.h"
#include "mumble_faces.inc"

enum { VOICE_TRACE = 21, VOICE_ROWS = 3, VOICE_COLUMNS = 18 };
#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
enum { WHITE = 0xffff };
static const uint16_t voice_knob_colors[4] = {0x2b7b, 0x2d8d, 0xf7be, 0xf325};
/* AH EH EE AW OO UH, then IH UU ER (each near the vowel it sits between) */
static const uint16_t vowel_colors[MUMBLE_VOWELS] = {0xf325, 0x2d8d, 0x2b7b, 0xf7be, 0xb47f, 0xfe87,
                                                     0x2c5f, 0xd51f, 0xfc90};
static const char *const voice_page_names[MUMBLE_PAGES] = {"VOICE", "FEEL", "FX", "STYLE"};
/* ALGO turns TYPE, SING, LOOP; the keys sing in both of the last two. */
enum { VOICE_TYPE, VOICE_SING, VOICE_LOOP, VOICE_MODES };
/* LOOP's tabs (ALGO turns them): the knobs set the mix, the arpeggiator, the scale. */
enum { TAB_MIX, TAB_ARP, TAB_CHORD, TAB_SCALE, VOICE_TABS };
static const char *const voice_tab_names[VOICE_TABS] = {"MIX", "ARP", "CHORD", "SCALE"};
static const char *const voice_mode_names[VOICE_MODES] = {"TYPE", "SING", "LOOP"};
static const char *const voice_greetings[MUMBLE_CAST] = {
    "Hey there!", "Halt.", "Bless you, child.", "Hiya! Hiya!", "Arr...", "Hmm, yes... yes.",
    "Hello. Hello.", "Whooo..."};
/* The line at the first boot (and after a reset): the user's to change or clear. */
static const char voice_first_line[] = "Our princess is in another castle!";
/* The hint line's tip in each mode, and HOME's help card. */
static const char *const voice_tips[VOICE_MODES] = {"K1 CURSOR  K2 LETTER  HOLD HOME: HELP",
                                                    "KEYS SING THE LINE  HOLD HOME: HELP",
                                                    "PRESET: THE TAB  HOLD HOME: HELP"};
enum { VOICE_HELP_ROWS = 6, VOICE_HELP_COLUMNS = 37 };     /* (37 letters fill the box to its border) */
static const char *const voice_help_rows[VOICE_MODES][VOICE_HELP_ROWS] = {
    {"KEYS TYPE AT THE CURSOR  ALGO: MODE",
     "K1 CURSOR  K2 LETTER  OCT- DELETE",
     "OCT+ CAPS  FX: START  LFO: SYLLABLE",
     "EDIT: A WORD BACK  HOLD: CLEAR",
     "SELECT: WHO  SAVE: CHOIR  PLAY: LOOP",
     "REC: SING AND RECORD  GLO: SYSTEM"},
    {"KEYS SING THE LINE  HELD: A CHORD",
     "PRESET: PAGE  K1-K4 SOUND  ALGO: MODE",
     "OCT- OCT+ OCTAVE  FX: TOP  SCL: SCALE",
     "EDIT: CLEAR SINGER  HOLD: CLEAR LOOP",
     "ARP: ARPEGGIO  SEQ: AUTO-CHORD",
     "SAVE: CHOIR  LFO: ROLL  ENV: UNROLL"},
    {"A LANE EACH  KEYS SING  ALGO: MODE",
     "PRESET: TAB  MIX ARP CHORD SCALE",
     "EDIT+KNOB: AS IT WAS",
     "PLAY: LOOP  REC: RECORD  GLO: SYSTEM",
     "EDIT: CLEAR SINGER  HOLD: CLEAR LOOP",
     "SELECT: WHO  SAVE: CHOIR"}};
/* K2's letters in TYPE mode, round and round. */
static const char voice_alphabet[] = "abcdefghijklmnopqrstuvwxyz ,.?!'-";
enum { VOICE_ALPHABET = sizeof voice_alphabet - 1, VOICE_HOLD_MS = 1000 };

/* A character: sound, choir, and place in the loop. */
enum { VOICE_PAGES = MUMBLE_PAGES, VOICE_SEQ = 128 };
struct voice_track {
    uint8_t choir, level;                    /* level: percent of its notes in the loop (0: muted) */
    struct mumble_voice sound;
};
static struct voice_track voice_tracks[MUMBLE_CAST];
/* The line, shared by everyone (empty: the demo line is sung). */
static char voice_text[MUMBLE_TEXT + 1];
static unsigned voice_length;
static unsigned voice_cast, voice_page, voice_caps, voice_ready, voice_tab;
/* SCL where the keys sing: the scale (in C) a key's note keeps to, the note
 * under it if it is not in it; 0: none. A bit for each pitch class. */
static const uint16_t voice_scales[] = {0xFFF, 0xAB5, 0x5AD, 0x295, 0x4E9, 0x6AD, 0x6B5, 0x9AD};
static const char *const voice_scale_names[] = {"OFF", "MAJOR", "MINOR", "PENTA", "BLUES", "DORIAN", "MIXO", "HARMONIC"};
static const char *const voice_note_names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
enum { VOICE_SCALES = sizeof voice_scales / sizeof voice_scales[0] };
static unsigned voice_scale, voice_root;           /* the scale, and its root: 0 C .. 11 B */
/* "D DORIAN", or "OFF". */
static const char *voice_scale_name(char out[16])
{
    if (!voice_scale) return "OFF";
    unsigned n = 0;
    for (const char *p = voice_note_names[voice_root]; *p; ++p) out[n++] = *p;
    out[n++] = ' ';
    for (const char *p = voice_scale_names[voice_scale]; *p && n < 15U; ++p) out[n++] = *p;
    out[n] = 0;
    return out;
}
/* The loop: BARS and BPM size it, it always loops. song is 1 when started, 2
 * once it was heard (the audio task adopts it a block later). */
static unsigned voice_loop = 1, voice_song, voice_song_dirty, voice_bars = 2, voice_bpm = 120;
static unsigned voice_fm1_playing, voice_lanes_version;
static struct mumble_status voice_now;
static struct { uint8_t vowel, level, track; int16_t cents; } voice_trace[VOICE_TRACE];
static uint32_t voice_trace_ms;
static uint32_t voice_keys_lit;
/* The mode, and whether the keys sing (SING or LOOP); held keys, octave
 * shift, and the last key sung and its note. */
static unsigned voice_mode, voice_play, voice_sung = MUMBLE_LETTERS, voice_sung_note;
static int voice_octave;
static uint32_t voice_held;
/* Every character sings one key at a time: each singer's key + 1 (0: silent)
 * and when it began, and when each key went down. */
static uint8_t voice_sings[MUMBLE_CAST];
static uint32_t voice_sing_order[MUMBLE_CAST], voice_key_order[MUMBLE_LETTERS], voice_order;
/* Legato: each singer's last pitch and when it stopped, so a note sung on
 * straight after slides from it (GLIDE). */
static uint8_t voice_sing_pitch[MUMBLE_CAST];
static uint32_t voice_sing_end[MUMBLE_CAST];
static int8_t voice_rec_bend[MUMBLE_CAST];
/* The line's syllables, the current one, whether one has been sung yet, the
 * key leading it (MUMBLE_LETTERS: none), and each key's slot and sound. */
static unsigned voice_lyric_at, voice_lyric_count, voice_lyric_started, voice_lyric_lead = MUMBLE_LETTERS;
static uint32_t voice_lyric_sig;
static struct mumble_syllable voice_lyric_plan[MUMBLE_SYLLABLES];
static struct mumble_sound voice_line_sounds[MUMBLE_SYLLABLES];
static uint8_t voice_key_slot[MUMBLE_LETTERS];
/* The sequence: notes grouped by singer (for the engine's parts), each with
 * its singer. */
static struct mumble_note voice_seq[VOICE_SEQ];
static uint8_t voice_seq_track[VOICE_SEQ];
static unsigned voice_seq_count;
/* Recording: a singer being recorded, where its note began, when, and its
 * note and slot. */
static unsigned voice_recording, voice_rec_armed, voice_click;
static uint32_t voice_rec_held, voice_rec_since[MUMBLE_CAST];
static uint16_t voice_rec_at[MUMBLE_CAST];
static uint8_t voice_rec_note[MUMBLE_CAST], voice_rec_slot[MUMBLE_CAST];
static uint8_t voice_rec_vowel[MUMBLE_CAST];        /* a chord's other note: recorded as the vowel alone */
static int32_t voice_loop_last_ms = -1;
/* TYPE mode's cursor (0..voice_length: keys type before it, K2 changes the
 * letter under it) and when it last moved (its blink starts lit). */
static unsigned voice_cursor;
static uint32_t voice_cursor_ms;
/* HOME held: the help card. EDIT: when it went down, and whether it was spent
 * (held long enough to clear, or a knob turned with it): a tap acts on release. */
static unsigned voice_help, voice_edit_down, voice_edit_spent;
static uint32_t voice_edit_since;
/* LFO and ENV: a tap acts as it is let go, held VOICE_HOLD_MS it acts on the
 * whole cast (as EDIT: since, spent once it acted, down last frame). */
struct voice_hold { uint32_t since; uint8_t down, spent; };
static struct voice_hold voice_lfo_hold, voice_env_hold;
enum { HOLD_NONE, HOLD_TAP, HOLD_LONG };
static unsigned voice_hold_step(struct voice_hold *h, unsigned held, unsigned pressed, unsigned holds)
{
    unsigned what = HOLD_NONE;
    if (pressed) h->since = frame_time, h->spent = 0;
    if (holds && held && !h->spent && frame_time - h->since >= VOICE_HOLD_MS) h->spent = 1, what = HOLD_LONG;
    if (!held && h->down && !h->spent) what = HOLD_TAP;
    h->down = (uint8_t)held;
    return what;
}
static char voice_said[40];
static void voice_rec_stop(void);
static void voice_notice(const char *message);
static void voice_say(const char *a, const char *b, const char *c);
static void voice_say_number(const char *a, int value, const char *c);

static struct voice_track *voice_track(void) { return &voice_tracks[voice_cast]; }
static unsigned voice_in_choir(unsigned i) { return i == voice_cast || voice_tracks[i].choir; }
/* ARP: on; its settings (ARP held and the knobs): the rate (in MIDI clock
 * ticks, 24 a beat), the pattern, octaves 1..3, the gate (percent of a step a
 * note sounds) and swing (percent of a step every other step comes late).
 * Then the step it sang last (-1: none yet), how many it has sung since the
 * keys went down, when its own clock started (no loop running), when the
 * note sung lets go, and ARP held (spent: a knob turned with it). */
static const uint8_t voice_arp_rates[] = {24, 12, 8, 6, 4, 3};
static const char *const voice_arp_rate_names[] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const char *const voice_arp_pattern_names[] = {"UP", "DOWN", "UP-DN", "RAND", "PLAYED"};
enum { ARP_RATES = sizeof voice_arp_rates, ARP_UP = 0, ARP_DOWN, ARP_UPDOWN, ARP_RANDOM, ARP_PLAYED, ARP_PATTERNS,
       ARP_RATE = 3, ARP_GATE = 50, ARP_SWING_MAX = 66 };
static unsigned voice_arp, voice_arp_rate = ARP_RATE, voice_arp_pattern, voice_arp_octaves = 1, voice_arp_gate = ARP_GATE,
                voice_arp_swing;
static unsigned voice_arp_step, voice_arp_count;
/* Auto-chord (SEQ; LOOP's CHORD tab): a key sings a chord stacked up the
 * scale from its note (with the scale off: up the major scale on the root),
 * one note a singer: the shape (scale steps above the key's), the inversion
 * (the lowest notes up an octave), the spread (close, open, wide). */
static const uint8_t voice_chord_shapes[][4] = {{0, 2, 4, 0}, {0, 2, 4, 6}, {0, 4, 7, 0}, {0, 1, 4, 0}, {0, 3, 4, 0},
                                                {0, 2, 4, 8}};
static const uint8_t voice_chord_sizes[] = {3, 4, 3, 3, 3, 4};
static const char *const voice_chord_names[] = {"TRIAD", "7TH", "POWER", "SUS2", "SUS4", "ADD9"};
static const char *const voice_spread_names[] = {"CLOSE", "OPEN", "WIDE"};
enum { CHORD_SHAPES = sizeof voice_chord_sizes, CHORD_SPREADS = 3 };
static unsigned voice_chord, voice_chord_shape, voice_chord_inversion, voice_chord_spread;
static int32_t voice_arp_index = -1;
static uint32_t voice_arp_t0, voice_arp_off, voice_arp_dice = 0x2545F491U;
static int8_t voice_pans[MUMBLE_CAST];   /* where the band was put (INT8_MIN: not yet told) */
/* Notes in the sequence for track t (all of them for MUMBLE_CAST). */
static unsigned voice_notes_of(unsigned t)
{
    unsigned n = 0;
    for (unsigned k = 0; k < voice_seq_count; ++k) n += t >= MUMBLE_CAST || voice_seq_track[k] == t;
    return n;
}
/* Something the loop plays changed: refresh it while it plays. */
static void voice_touch(void)
{
    ++voice_lanes_version;
    if (voice_song) voice_song_dirty = 1;
}
static void voice_set_text(const char *s)
{
    unsigned n = (unsigned)strlen(s);
    if (n > MUMBLE_TEXT) n = MUMBLE_TEXT;
    memcpy(voice_text, s, n);
    voice_text[n] = 0;
    voice_length = n;
    voice_cursor = n;
}

/* The loop follows the FM1 tempo while its sequencer plays. */
static unsigned voice_tempo(void)
{
    unsigned bpm = voice_fm1_playing && frame_views[0].tempo ? frame_views[0].tempo : voice_bpm;
    return bpm < 40U ? 40U : bpm;
}
static uint32_t voice_step_ms(unsigned step) { return step * 15000U / voice_tempo(); }
static uint32_t voice_loop_ms(void) { return voice_step_ms(voice_bars * 16U); }
static unsigned voice_steps(void) { return voice_bars * 16U; }

/* The sequence onto the loop, each singer a part singing the line as it is. */
static void voice_publish(unsigned restart, uint32_t phase_ms)
{
    struct mumble_part parts[MUMBLE_CAST];
    memset(parts, 0, sizeof parts);
    for (unsigned i = 0, k = 0; i < MUMBLE_CAST; ++i) {
        unsigned first = k;
        while (k < voice_seq_count && voice_seq_track[k] == i) ++k;
        struct voice_track *t = &voice_tracks[i];
        parts[i] = (struct mumble_part){NULL, 0, &t->sound, 0, t->level,
                                        voice_seq + first, k - first, voice_line_sounds, voice_lyric_count};
    }
    voice_song_dirty = 0;
    mumble_song(parts, voice_loop_ms(), restart, phase_ms);
    if (restart || !voice_song) voice_song = 1;
}
/* Start the loop; with the FM1 sequencer playing, join it at its step. */
/* Where a MIDI clock says the loop is: its ticks since Start, round the
 * loop (24 a beat, 2500 / BPM ms each). */
enum { VOICE_DRIFT_MS = 40 };
static uint32_t voice_clock_ms(void) { return frame_views[0].play_tick % (voice_steps() * 6U) * 2500U / voice_tempo(); }
static void voice_start(void)
{
    if (voice_rec_armed) voice_rec_armed = 0, voice_recording = 1;   /* the loop starts: so does the recording */
    voice_publish(1, voice_fm1_playing ? voice_clock_ms() : 0);
    voice_loop_last_ms = -1;
}
static void voice_stop(void)
{
    voice_rec_stop();
    mumble_stop();
    memset(voice_sings, 0, sizeof voice_sings);
    voice_lyric_lead = MUMBLE_LETTERS;
    voice_song = 0;
    voice_held = 0;
}

/* ------------------------------------------------------------------------
 * What is kept over power-off (OBJ_SETTINGS): the line, each character's
 * sound, choir, mute and level, the loop and its size, and the panel's
 * settings. It is saved VOICE_SAVE_MS after the last change, once the FM-1
 * has been quiet for VOICE_QUIET_MS (the loop stopped, nothing sounding,
 * nothing held or recording): a save erases a flash sector, which silences
 * the audio and holds everything else for up to a few hundred milliseconds,
 * so it must not land in a performance. It is read back whole at power-on,
 * or not at all when any of it is out of range.
 * ---------------------------------------------------------------------- */
enum { VOICE_STORE_MAGIC = 0x424D554DU /* "MUMB" */, VOICE_STORE_VERSION = 5, VOICE_SAVE_MS = 3000, VOICE_QUIET_MS = 1000 };
struct voice_store {
    uint32_t magic;
    uint16_t version, size;
    char text[MUMBLE_TEXT + 1];
    uint8_t length, cast, caps, scale, arp, bars, bpm, seq_count;
    uint8_t arp_rate, arp_pattern, arp_octaves, arp_gate, arp_swing, root;
    uint8_t chord, chord_shape, chord_inversion, chord_spread;
    int8_t octave;
    struct voice_track tracks[MUMBLE_CAST];
    struct mumble_note seq[VOICE_SEQ];
    uint8_t seq_track[VOICE_SEQ];
};
_Static_assert(sizeof(struct voice_store) <= PLAT_STORE_MAX, "the kept state fits a store object");
static struct voice_store voice_kept;               /* (the image saved or read: too big for the stack) */
static uint32_t voice_kept_sig, voice_change_sig, voice_change_ms, voice_noisy_ms;
static void voice_store_fill(struct voice_store *k)
{
    memset(k, 0, sizeof *k);
    k->magic = VOICE_STORE_MAGIC;
    k->version = VOICE_STORE_VERSION;
    k->size = (uint16_t)sizeof *k;
    memcpy(k->text, voice_text, voice_length);
    k->length = (uint8_t)voice_length;
    k->cast = (uint8_t)voice_cast, k->caps = (uint8_t)voice_caps;
    k->scale = (uint8_t)voice_scale, k->arp = (uint8_t)voice_arp;
    k->arp_rate = (uint8_t)voice_arp_rate, k->arp_pattern = (uint8_t)voice_arp_pattern;
    k->arp_octaves = (uint8_t)voice_arp_octaves, k->arp_gate = (uint8_t)voice_arp_gate, k->arp_swing = (uint8_t)voice_arp_swing;
    k->root = (uint8_t)voice_root;
    k->chord = (uint8_t)voice_chord, k->chord_shape = (uint8_t)voice_chord_shape;
    k->chord_inversion = (uint8_t)voice_chord_inversion, k->chord_spread = (uint8_t)voice_chord_spread;
    k->bars = (uint8_t)voice_bars, k->bpm = (uint8_t)voice_bpm, k->octave = (int8_t)voice_octave;
    memcpy(k->tracks, voice_tracks, sizeof voice_tracks);
    k->seq_count = (uint8_t)voice_seq_count;
    memcpy(k->seq, voice_seq, voice_seq_count * sizeof voice_seq[0]);
    memcpy(k->seq_track, voice_seq_track, voice_seq_count);
}
/* All of it in range, or none of it is used. */
static unsigned voice_store_valid(const struct voice_store *k)
{
    if (k->magic != VOICE_STORE_MAGIC || k->version != VOICE_STORE_VERSION || k->size != sizeof *k) return 0;
    if (k->length > MUMBLE_TEXT || k->text[k->length]) return 0;
    for (unsigned i = 0; i < k->length; ++i)
        if ((unsigned char)k->text[i] < 32U || (unsigned char)k->text[i] > 126U) return 0;
    if (k->arp_rate >= ARP_RATES || k->arp_pattern >= ARP_PATTERNS || k->arp_octaves < 1 || k->arp_octaves > 3 ||
        k->arp_gate < 10 || k->arp_gate > 100 || k->arp_swing > ARP_SWING_MAX || k->root > 11 ||
        k->chord > 1 || k->chord_shape >= CHORD_SHAPES || k->chord_inversion > 3 || k->chord_spread >= CHORD_SPREADS)
        return 0;
    if (k->cast >= MUMBLE_CAST || k->caps > 1 || k->scale >= VOICE_SCALES || k->arp > 1 ||
        (k->bars != 1 && k->bars != 2 && k->bars != 4 && k->bars != 8) || k->bpm < 40 || k->bpm > 240 ||
        k->octave < -2 || k->octave > 2 || k->seq_count > VOICE_SEQ)
        return 0;
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        const struct voice_track *t = &k->tracks[i];
        if (t->choir > 1 || t->level > 100) return 0;
        for (unsigned p = 0; p < MUMBLE_PARAMS; ++p)
            if (t->sound.value[p] < mumble_params[p].min || t->sound.value[p] > mumble_params[p].max) return 0;
    }
    for (unsigned n = 0; n < k->seq_count; ++n) {
        const struct mumble_note *q = &k->seq[n];
        if (k->seq_track[n] >= MUMBLE_CAST || (n && k->seq_track[n] < k->seq_track[n - 1U])) return 0;
        if (q->note > 127U || q->len_ms < 30U || q->shout > (1U | MUMBLE_NOTE_VOWEL) || q->slot >= MUMBLE_SYLLABLES || q->bend < -24 || q->bend > 24)
            return 0;
    }
    return 1;
}
static void voice_store_load(void)
{
    if (plat_store_load(OBJ_SETTINGS, &voice_kept, sizeof voice_kept) != (int)sizeof voice_kept ||
        !voice_store_valid(&voice_kept))
        return;
    const struct voice_store *k = &voice_kept;
    memcpy(voice_text, k->text, k->length + 1U);
    voice_length = voice_cursor = k->length;
    voice_cast = k->cast, voice_caps = k->caps, voice_scale = k->scale, voice_arp = k->arp;
    voice_arp_rate = k->arp_rate, voice_arp_pattern = k->arp_pattern, voice_arp_octaves = k->arp_octaves;
    voice_arp_gate = k->arp_gate, voice_arp_swing = k->arp_swing, voice_root = k->root;
    voice_chord = k->chord, voice_chord_shape = k->chord_shape, voice_chord_inversion = k->chord_inversion;
    voice_chord_spread = k->chord_spread;
    voice_bars = k->bars, voice_bpm = k->bpm, voice_octave = k->octave;
    memcpy(voice_tracks, k->tracks, sizeof voice_tracks);
    voice_seq_count = k->seq_count;
    memcpy(voice_seq, k->seq, sizeof voice_seq);
    memcpy(voice_seq_track, k->seq_track, sizeof voice_seq_track);
}
/* Each frame: a change is saved once it has stood VOICE_SAVE_MS and the
 * FM-1 has been quiet VOICE_QUIET_MS (a performance is not interrupted). */
static void voice_store_frame(uint32_t now)
{
    unsigned sounding = 0;
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) sounding |= voice_now.track_level[i] != 0;
    if (voice_song || sounding || voice_held || voice_recording) voice_noisy_ms = now;
    voice_store_fill(&voice_kept);
    uint32_t sig = signature(0x5AFE, &voice_kept, sizeof voice_kept);
    if (sig == voice_kept_sig) return;
    if (sig != voice_change_sig) { voice_change_sig = sig; voice_change_ms = now; return; }
    if (now - voice_change_ms < VOICE_SAVE_MS || now - voice_noisy_ms < VOICE_QUIET_MS) return;
    if (plat_store_save(OBJ_SETTINGS, &voice_kept, sizeof voice_kept) == 0) voice_kept_sig = sig;
    else { voice_notice("COULD NOT SAVE: TRYING AGAIN"); voice_change_ms = now; }
}

static void voice_close(void);
/* The faces, built from the voices (mumble_faces.inc): the selected
 * character's portrait and rig, every character's icon. Rebuilt here, on the
 * control side, when a voice changes (a knob, LFO, EDIT, a load) or another
 * character is selected; the drawing only reads them, and signs with
 * voice_faces_version. */
static struct voice_face voice_portrait, voice_face_work;
static char voice_icons[MUMBLE_CAST][FACE_ICON][FACE_ICON];
static uint32_t voice_face_sigs[MUMBLE_CAST], voice_faces_version;
static unsigned voice_portrait_of = MUMBLE_CAST;
static void voice_faces_update(void)
{
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        uint32_t sig = signature(0xFACE0 + i, &voice_tracks[i].sound, sizeof voice_tracks[i].sound);
        unsigned portrait = i == voice_cast && voice_portrait_of != voice_cast;
        if (sig == voice_face_sigs[i] && !portrait) continue;
        struct voice_face *f = i == voice_cast ? &voice_portrait : &voice_face_work;
        voice_face_build(&voice_tracks[i].sound, f);
        voice_face_icon(f, voice_icons[i]);
        if (i == voice_cast) voice_portrait_of = i;
        voice_face_sigs[i] = sig;
        ++voice_faces_version;
    }
}

/* The system screen (GLO): the audio's load (now and the most since boot),
 * dropouts, the battery, how long it has been on, the store, the clock;
 * snapshotted every half second on the control side. There EDIT tapped,
 * then held 3 s, resets everything (voice_resets counts them, for the
 * tests): voice_reset_armed 1 after the tap (for 3 s), 2 while it is held,
 * 3 once it has reset (until EDIT is let go). */
enum { VOICE_RESET_MS = 3000 };
static unsigned voice_system, voice_reset_armed, voice_resets;
static uint32_t voice_reset_until, voice_sys_ms;
static struct { uint32_t cpu, cpu_peak, xruns, up_s; int battery, raw, charging, sync; } voice_sys;
static void voice_system_frame(uint32_t now)
{
    uint32_t cpu = plat_cpu_pct();
    if (cpu > voice_sys.cpu_peak) voice_sys.cpu_peak = cpu;
    if (now - voice_sys_ms < 500U && voice_sys_ms) return;
    voice_sys_ms = now;
    voice_sys.cpu = cpu;
    voice_sys.xruns = plat_xruns();
    voice_sys.up_s = now / 1000U;
    voice_sys.battery = plat_battery();
    voice_sys.raw = (int)plat_battery_raw();
    voice_sys.charging = plat_battery_charging();
    voice_sys.sync = (int)voice_fm1_playing;
}
static void voice_close(void);
static void voice_stop(void);
static void voice_seq_clear(void);
/* Everything as at the first boot: the line, every voice, the choir, the
 * loop, its size, the scale, the arpeggiator, auto-chord (then saved, once
 * quiet, as any change). */
static void voice_reset_everything(void)
{
    if (voice_song) voice_stop();
    voice_close();
    voice_seq_clear();
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        voice_tracks[i].sound = mumble_cast[i].voice;
        voice_tracks[i].choir = 0;
        voice_tracks[i].level = 100;
    }
    voice_set_text(voice_first_line);
    voice_cast = voice_caps = voice_scale = voice_root = voice_arp = voice_page = voice_tab = 0;
    voice_arp_rate = ARP_RATE, voice_arp_pattern = ARP_UP, voice_arp_octaves = 1, voice_arp_gate = ARP_GATE, voice_arp_swing = 0;
    voice_chord = voice_chord_shape = voice_chord_inversion = voice_chord_spread = 0;
    voice_bars = 2, voice_bpm = 120, voice_octave = 0;
    voice_reset_armed = 0;
    ++voice_resets;
    voice_touch();
    voice_notice("EVERYTHING IS RESET");
}

/* Power-off, as far as RAM goes: the page starts over (and loads what was
 * kept) at the next voice_open. */
static void voice_forget(void)
{
    voice_close();
    voice_ready = 0;
    voice_set_text("");
    voice_seq_count = 0;
    voice_cast = voice_caps = voice_scale = voice_arp = 0;
    voice_arp_rate = ARP_RATE, voice_arp_pattern = ARP_UP, voice_arp_octaves = 1, voice_arp_gate = ARP_GATE, voice_arp_swing = 0;
    voice_root = 0;
    voice_chord = voice_chord_shape = voice_chord_inversion = voice_chord_spread = 0;
    voice_bars = 2, voice_bpm = 120, voice_octave = 0;
    voice_portrait_of = MUMBLE_CAST;
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) voice_face_sigs[i] = 0;
}

static void voice_open(void)
{
    if (!voice_ready) {
        mumble_init();
        voice_pans[0] = INT8_MIN;
        for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
            struct voice_track *t = &voice_tracks[i];
            t->sound = mumble_cast[i].voice;
            t->choir = 0;
            t->level = 100;
        }
        voice_set_text(voice_first_line);
        voice_seq_count = 0;
        voice_cast = 0;
        voice_store_load();
        voice_store_fill(&voice_kept);
        voice_kept_sig = voice_change_sig = signature(0x5AFE, &voice_kept, sizeof voice_kept);
        voice_ready = 1;
    }
    voice_page = 0;
    voice_mode = voice_play = 0;
    voice_held = 0;
    voice_song = 0;
    memset(voice_trace, 0, sizeof voice_trace);
    for (unsigned i = 0; i < VOICE_TRACE; ++i) voice_trace[i].vowel = MUMBLE_VOWELS;
    voice_faces_update();
}
static void voice_close(void)
{
    if (voice_ready) mumble_stop(); /* also releases held letters */
    voice_rec_stop();
    memset(voice_sings, 0, sizeof voice_sings);
    voice_lyric_lead = MUMBLE_LETTERS;
    voice_held = 0;
    voice_song = 0;
}
/* A letter typed at the cursor. On a full line the oldest letter falls off
 * (typing at the end) or the last one does (typing inside the line). */
static void voice_type(char c)
{
    if (voice_length >= MUMBLE_TEXT) {
        if (voice_cursor < voice_length) --voice_length;
        else {
            memmove(voice_text, voice_text + 1, MUMBLE_TEXT - 1);
            voice_length = MUMBLE_TEXT - 1;
            voice_cursor = voice_length;
        }
    }
    memmove(voice_text + voice_cursor + 1, voice_text + voice_cursor, voice_length - voice_cursor);
    voice_text[voice_cursor++] = c;
    voice_text[++voice_length] = 0;
    voice_cursor_ms = frame_time;
    /* The live lane says what ends at the cursor now; the loop sings the new
     * line from the next syllable it reaches. */
    mumble_type(voice_cast, voice_text, voice_cursor, &voice_track()->sound);
}
/* Letters from..to-1 out of the line, the cursor kept on what follows. */
static void voice_cut(unsigned from, unsigned to)
{
    memmove(voice_text + from, voice_text + to, voice_length - to + 1U);
    voice_length -= to - from;
    voice_cursor = voice_cursor >= to ? voice_cursor - (to - from) : voice_cursor > from ? from : voice_cursor;
    voice_cursor_ms = frame_time;
}
/* EDIT's tap in TYPE: the word before the cursor (and the spaces after it). */
static unsigned voice_cut_word(void)
{
    unsigned from = voice_cursor;
    while (from && voice_text[from - 1U] == ' ') --from;
    while (from && voice_text[from - 1U] != ' ') --from;
    if (from == voice_cursor) return 0;
    voice_cut(from, voice_cursor);
    return 1;
}
static void voice_move_cursor(int delta)
{
    voice_cursor = (unsigned)clamp((int)voice_cursor + delta, 0, (int)voice_length);
    voice_cursor_ms = frame_time;
}
/* K2 in TYPE: the letter under the cursor round the alphabet (keeping its
 * case); at the line's end, a new one. */
static unsigned voice_letter_at(unsigned i)
{
    char c = i < voice_length ? voice_text[i] : 0;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    unsigned at = 0;
    while (at < VOICE_ALPHABET && voice_alphabet[at] != c) ++at;
    return at;                                   /* VOICE_ALPHABET: none (the end, or another mark) */
}
static void voice_letter(int delta)
{
    unsigned at = voice_letter_at(voice_cursor), upper = voice_caps;
    if (voice_cursor < voice_length) {
        char was = voice_text[voice_cursor];
        upper |= was >= 'A' && was <= 'Z';
        if (at == VOICE_ALPHABET) at = delta > 0 ? VOICE_ALPHABET - 1U : 0U;   /* from either end */
    } else {
        if (voice_length >= MUMBLE_TEXT) { voice_notice("THE LINE IS FULL"); return; }
        at = delta > 0 ? VOICE_ALPHABET - 1U : 0U;
        memmove(voice_text + voice_cursor + 1, voice_text + voice_cursor, 1);
        ++voice_length;
    }
    at = (unsigned)(((int)at + delta % (int)VOICE_ALPHABET + (int)VOICE_ALPHABET) % (int)VOICE_ALPHABET);
    char c = voice_alphabet[at];
    if (upper && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    voice_text[voice_cursor] = c;
    voice_cursor_ms = frame_time;
    mumble_type(voice_cast, voice_text, voice_cursor + 1U, &voice_track()->sound);
}
static unsigned voice_parameter(unsigned col) { return voice_page * 4U + col; }
static const uint8_t voice_bar_choices[4] = {1, 2, 4, 8};
static void voice_edit(unsigned col, int delta)
{
    struct voice_track *t = voice_track();
    if (voice_mode == VOICE_LOOP) {                 /* the tab's: by the turn's speed, or a step a detent */
        int step = delta > 0 ? 1 : -1;
        unsigned k = voice_tab * 4U + col;
        if (k == 0) t->level = (uint8_t)clamp((int)t->level + delta, 0, 100);
        else if (k == 1) voice_arp_swing = (unsigned)clamp((int)voice_arp_swing + delta, 0, ARP_SWING_MAX);
        else if (k == 2) {
            unsigned i = 0;
            while (i < 3 && voice_bar_choices[i] != voice_bars) ++i;
            voice_bars = voice_bar_choices[clamp((int)i + step, 0, 3)];
        } else if (k == 3) voice_bpm = (unsigned)clamp((int)voice_bpm + delta, 40, 240);
        else if (k == 4) voice_arp_rate = (unsigned)clamp((int)voice_arp_rate + step, 0, ARP_RATES - 1);
        else if (k == 5) voice_arp_pattern = (unsigned)clamp((int)voice_arp_pattern + step, 0, ARP_PATTERNS - 1);
        else if (k == 6) voice_arp_octaves = (unsigned)clamp((int)voice_arp_octaves + step, 1, 3);
        else if (k == 7) voice_arp_gate = (unsigned)clamp((int)voice_arp_gate + delta, 10, 100);
        else if (k == 8) voice_chord_shape = (unsigned)clamp((int)voice_chord_shape + step, 0, CHORD_SHAPES - 1);
        else if (k == 9) voice_chord_inversion = (unsigned)clamp((int)voice_chord_inversion + step, 0, 3);
        else if (k == 10) voice_chord_spread = (unsigned)clamp((int)voice_chord_spread + step, 0, CHORD_SPREADS - 1);
        else if (k == 12) voice_root = (voice_root + 12U + (unsigned)step) % 12U;
        else if (k == 13) voice_scale = (unsigned)clamp((int)voice_scale + step, 0, VOICE_SCALES - 1);
        voice_touch();
        return;
    }
    unsigned p = voice_parameter(col);
    const struct mumble_param_info *info = &mumble_params[p];
    t->sound.value[p] = (uint16_t)clamp((int)t->sound.value[p] + delta, info->min, info->max);
    voice_touch();
}
/* EDIT held and a knob turned: that setting back to where it starts. */
static void voice_reset(unsigned col)
{
    struct voice_track *t = voice_track();
    if (voice_mode == VOICE_LOOP) {
        static const char *const names[VOICE_TABS * 4] = {"LEVEL", "SWING", "BARS", "BPM", "RATE", "PATTERN", "OCTAVES",
                                                          "GATE", "SHAPE", "INVERSION", "SPREAD", "", "ROOT", "SCALE",
                                                          "", ""};
        unsigned k = voice_tab * 4U + col;
        if (!names[k][0]) return;
        if (k == 0) t->level = 100;
        else if (k == 1) voice_arp_swing = 0;
        else if (k == 2) voice_bars = 2;
        else if (k == 3) voice_bpm = 120;
        else if (k == 4) voice_arp_rate = ARP_RATE;
        else if (k == 5) voice_arp_pattern = ARP_UP;
        else if (k == 6) voice_arp_octaves = 1;
        else if (k == 7) voice_arp_gate = ARP_GATE;
        else if (k == 8) voice_chord_shape = 0;
        else if (k == 9) voice_chord_inversion = 0;
        else if (k == 10) voice_chord_spread = 0;
        else if (k == 12) voice_root = 0;
        else voice_scale = 0;
        voice_touch();
        strcpy(voice_said, names[k]);
    } else {
        unsigned p = voice_parameter(col);
        t->sound.value[p] = mumble_cast[voice_cast].voice.value[p];
        voice_touch();
        strcpy(voice_said, mumble_params[p].name);
    }
    strcat(voice_said, " AS IT WAS");
    voice_notice(voice_said);
}
/* LFO: a voice rolled for a character, each setting halfway between the
 * character's own and a random one (so it is still somebody); ENV: their own
 * voice back. EDIT with a knob puts a single setting back. */
static uint32_t voice_dice = 0x9E3779B9U;
static uint32_t voice_random(void)
{
    voice_dice ^= voice_dice << 13;
    voice_dice ^= voice_dice >> 17;
    voice_dice ^= voice_dice << 5;
    return voice_dice;
}
static void voice_reroll(unsigned who)
{
    struct voice_track *t = &voice_tracks[who];
    voice_dice ^= frame_time * 2654435761U + who;
    for (unsigned p = 0; p < MUMBLE_PARAMS; ++p) {
        const struct mumble_param_info *info = &mumble_params[p];
        unsigned span = info->max - info->min + 1U;
        if (p == MUMBLE_ECHO || p == MUMBLE_ROOM) span = span * 3U / 10U;   /* (a roll adds a little echo and room, not a cave) */
        if (p == MUMBLE_WOBBLE) {                                           /* (and hardly a wobble: 0..10%) */
            t->sound.value[p] = (uint16_t)(voice_random() % 11U);
            continue;
        }
        unsigned r = info->min + voice_random() % span;
        t->sound.value[p] = (uint16_t)((mumble_cast[who].voice.value[p] + r) / 2U);
    }
    voice_touch();
}
static void voice_restore(unsigned who)
{
    voice_tracks[who].sound = mumble_cast[who].voice;
    voice_touch();
}
/* LFO in TYPE: a random syllable at the cursor (a consonant or two and a
 * vowel or two: "ka", "moo", "shee"). */
static void voice_type(char c);
static void voice_type_syllable(void)
{
    static const char *const onsets[] = {"b", "d", "f", "g", "h", "k", "l", "m", "n", "p", "r", "s", "t", "v", "w",
                                         "y", "z", "sh", "ch", "th", "bl", "gr", "st"};
    static const char *const vowels[] = {"a", "e", "i", "o", "u", "ee", "oo", "ai", "ay", "oy", "ow"};
    voice_dice ^= frame_time * 2654435761U;
    const char *parts[2] = {onsets[voice_random() % (sizeof onsets / sizeof onsets[0])],
                            vowels[voice_random() % (sizeof vowels / sizeof vowels[0])]};
    for (unsigned k = 0; k < 2; ++k)
        for (const char *p = parts[k]; *p; ++p) voice_type(voice_caps ? (char)(*p - 'a' + 'A') : *p);
}
static unsigned voice_edited(void)
{
    return memcmp(&voice_track()->sound, &mumble_cast[voice_cast].voice, sizeof(struct mumble_voice)) != 0;
}

/* The line the keys sing, planned again (and sung from its top) when it changes. */
static const char *voice_lyric_text(unsigned *n)
{
    *n = voice_length;
    return voice_text;
}
static void voice_lyric_rewind(void)
{
    voice_lyric_at = 0;
    voice_lyric_started = 0;
    voice_lyric_lead = MUMBLE_LETTERS;
}
static void voice_lyric_sync(void)
{
    unsigned n;
    const char *line = voice_lyric_text(&n);
    uint32_t sig = signature(n, line, n);
    if (sig != voice_lyric_sig) {
        voice_lyric_sig = sig;
        voice_lyric_count = mumble_plan(line, n, &voice_track()->sound, 0, voice_lyric_plan, MUMBLE_SYLLABLES, NULL, NULL);
        for (unsigned k = 0; k < voice_lyric_count; ++k) {
            const struct mumble_syllable *y = &voice_lyric_plan[k];
            voice_line_sounds[k] = (struct mumble_sound){y->vowel, y->locus, (uint8_t)(y->flags & (MUMBLE_HUM | MUMBLE_SHOUT)),
                                                         y->glide, y->onset, y->onset_hz, y->coda, y->coda_hz};
        }
        voice_lyric_rewind();
        voice_touch();                   /* the loop sings the new line */
    }
    if (voice_lyric_at >= voice_lyric_count) voice_lyric_at = 0;
}
/* A key pressed sings the current syllable while its leading key is held;
 * else it moves on to the next and leads it. */
static void voice_lyric_take(unsigned key)
{
    voice_lyric_sync();
    if (voice_lyric_lead >= MUMBLE_LETTERS) {
        if (voice_lyric_started && voice_lyric_count) voice_lyric_at = (voice_lyric_at + 1U) % voice_lyric_count;
        voice_lyric_started = 1;
        voice_lyric_lead = key;
    }
    voice_key_slot[key] = (uint8_t)voice_lyric_at;
}
/* The syllable a key pressed now would sing. */
static unsigned voice_lyric_next(void)
{
    if (!voice_lyric_count || voice_lyric_lead < MUMBLE_LETTERS || !voice_lyric_started) return voice_lyric_at;
    return (voice_lyric_at + 1U) % voice_lyric_count;
}
/* Where syllable k of the line shows: from its consonants before its vowel
 * (back to the last syllable's vowel or the word's start). */
static unsigned voice_lyric_onset(const char *line, unsigned k)
{
    unsigned p = voice_lyric_plan[k].index, floor = k ? voice_lyric_plan[k - 1U].index + 1U : 0U;
    while (p > floor) {
        char c = (char)(line[p - 1U] | 0x20);
        if (c < 'a' || c > 'z' || c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') break;
        --p;
    }
    return p;
}
/* SING's typewriter: the line types out in the box as it is sung, a letter
 * every VOICE_TYPE_MS, up to the end of the syllable sung last (never more
 * than that syllable behind); from the top again when the line starts over. */
enum { VOICE_TYPE_MS = 35 };
static unsigned voice_typed_from, voice_typed_target;
static uint32_t voice_typed_ms, voice_typed_sig;
static unsigned voice_typed(void)
{
    unsigned k = voice_typed_from + (frame_time - voice_typed_ms) / VOICE_TYPE_MS;
    return k < voice_typed_target ? k : voice_typed_target;
}
static void voice_typewriter(void)
{
    unsigned n, target = 0;
    const char *line = voice_lyric_text(&n);
    if (voice_lyric_started && voice_lyric_count)
        target = voice_lyric_at + 1U < voice_lyric_count ? voice_lyric_onset(line, voice_lyric_at + 1U) : n;
    if (voice_typed_sig != voice_lyric_sig || target < voice_typed_target) {
        voice_typed_sig = voice_lyric_sig;
        voice_typed_from = 0;
        voice_typed_ms = frame_time;
    } else if (target > voice_typed_target) {
        /* A new syllable: what comes before it shows at once (a fast
         * arpeggio would leave the typing words behind), it types out. */
        unsigned typed = voice_typed(), sung = voice_lyric_onset(line, voice_lyric_at);
        voice_typed_from = typed > sung ? typed : sung;
        voice_typed_ms = frame_time;
    }
    voice_typed_target = target;
}

/* Recording: where the loop is now, on its nearest 16th (a note a little
 * early or late lands on the beat); -1 when it is not running. */
static int32_t voice_rec_position(void)
{
    int32_t at = mumble_song_position();
    if (at < 0) return voice_song == 1 ? 0 : -1;  /* (just started, not yet heard: its top) */
    uint32_t step = (unsigned)((uint32_t)at % voice_loop_ms() * voice_steps() * 2U / voice_loop_ms() + 1U) / 2U;
    return (int32_t)voice_step_ms(step % voice_steps());
}
static void voice_rec_press(unsigned who, unsigned slot, unsigned note)
{
    int32_t at = voice_rec_position();
    if (at < 0 || at > 0xFFFF) return;
    voice_rec_held |= 1U << who;
    voice_rec_at[who] = (uint16_t)at;
    voice_rec_since[who] = frame_time;
    voice_rec_slot[who] = (uint8_t)slot;
    voice_rec_note[who] = (uint8_t)note;
}
/* A recorded note ended (let go, or its singer moved on): into the
 * sequence, grouped with its singer's, played from the next pass. */
static void voice_rec_release(unsigned who)
{
    if (!(voice_rec_held & (1U << who))) return;
    voice_rec_held &= ~(1U << who);
    if (voice_seq_count >= VOICE_SEQ) { voice_notice("THE LOOP IS FULL"); return; }
    uint32_t len = frame_time - voice_rec_since[who], loop = voice_loop_ms();
    if (len < 30U) len = 30U;
    if (len >= loop) len = loop - 1U;
    if (len > 0xFFFFU) len = 0xFFFFU;
    unsigned at = 0;
    while (at < voice_seq_count && voice_seq_track[at] <= who) ++at;
    memmove(voice_seq + at + 1, voice_seq + at, (voice_seq_count - at) * sizeof voice_seq[0]);
    memmove(voice_seq_track + at + 1, voice_seq_track + at, voice_seq_count - at);
    voice_seq[at] = (struct mumble_note){voice_rec_at[who], (uint16_t)len, voice_rec_slot[who], voice_rec_note[who],
                                         (uint8_t)(voice_caps | (voice_rec_vowel[who] ? MUMBLE_NOTE_VOWEL : 0)),
                                         voice_rec_bend[who]};
    voice_seq_track[at] = (uint8_t)who;
    ++voice_seq_count;
    voice_touch();
}
/* EDIT where the keys sing: one singer's notes out of the loop. */
static unsigned voice_seq_clear_track(unsigned t)
{
    unsigned n = 0, was = voice_seq_count;
    for (unsigned k = 0; k < voice_seq_count; ++k)
        if (voice_seq_track[k] != t) {
            voice_seq[n] = voice_seq[k];
            voice_seq_track[n] = voice_seq_track[k];
            ++n;
        }
    voice_seq_count = n;
    voice_touch();
    return n != was;
}
static void voice_seq_clear(void)
{
    voice_seq_count = 0;
    voice_touch();
}
/* A character stops singing its key, or starts one: the key's syllable, at
 * the character's own register. */
static void voice_note_off(unsigned who)
{
    if (!voice_sings[who]) return;
    mumble_release(voice_sings[who] - 1U);
    voice_rec_release(who);
    voice_sings[who] = 0;
    voice_sing_end[who] = frame_time;
}
/* Singers start a key's syllable together (one press, so letting the key go
 * lets them all go), each at the key's note shifted by shift[i] semitones,
 * kept to the scale, in their own register; the first is the key's own note. */
static int voice_keep_to_scale(int pitch)
{
    while (!(voice_scales[voice_scale] >> ((pitch + 12 - (int)voice_root) % 12) & 1U)) --pitch;   /* (the root is in every scale) */
    return pitch;
}
static void voice_notes_on(const uint8_t *who, const int8_t *shift, unsigned n, unsigned key)
{
    static const struct mumble_sound hum = {MUMBLE_U, 0, MUMBLE_HUM, MUMBLE_VOWELS, 0, 0, 0, 0};
    if (!n) return;
    struct mumble_singer singers[MUMBLE_CAST];
    int pitches[MUMBLE_CAST];
    for (unsigned i = 0; i < n; ++i) {
        const struct mumble_voice *v = &voice_tracks[who[i]].sound;
        int pitch = clamp(53 + (int)key + shift[i] + 12 * (voice_octave + mumble_register(v)), 0, 127);   /* lowest key is F3 */
        pitch = voice_keep_to_scale(pitch);
        /* Straight on from its last note (within 60 ms): slide from it. */
        int bend = 0;
        if (voice_sing_pitch[who[i]] && frame_time - voice_sing_end[who[i]] <= 60U)
            bend = clamp((int)voice_sing_pitch[who[i]] - pitch, -24, 24);
        singers[i] = (struct mumble_singer){v, who[i], (uint8_t)pitch, (int8_t)bend};
        voice_sing_pitch[who[i]] = (uint8_t)pitch;
        voice_rec_bend[who[i]] = (int8_t)bend;
        pitches[i] = pitch;
    }
    unsigned slot = voice_key_slot[key];
    mumble_press_sound(key, voice_lyric_count ? &voice_line_sounds[slot % voice_lyric_count] : &hum, singers, n, voice_caps);
    for (unsigned i = 0; i < n; ++i) {
        voice_sings[who[i]] = (uint8_t)(key + 1U);
        voice_sing_order[who[i]] = ++voice_order;
        voice_rec_vowel[who[i]] = (uint8_t)(i > 0);
        if (voice_recording) voice_rec_press(who[i], slot, (unsigned)pitches[i]);
    }
    voice_sung = key;
    voice_sung_note = (unsigned)pitches[0];
}
static void voice_note_on(unsigned who, unsigned key)
{
    const uint8_t one = (uint8_t)who;
    const int8_t none = 0;
    voice_notes_on(&one, &none, 1, key);
}
/* Auto-chord: the semitones above (or under) a key's note of its chord's
 * notes, the first its own (kept to the scale); returns how many. */
static unsigned voice_chord_shifts(unsigned key, int8_t shift[4])
{
    int base = 53 + (int)key + 12 * voice_octave, own = voice_keep_to_scale(base), tones[4];
    uint16_t mask = voice_scale ? voice_scales[voice_scale] : voice_scales[1];   /* (no scale: major on the root) */
    unsigned n = voice_chord_sizes[voice_chord_shape];
    for (unsigned j = 0; j < n; ++j) {
        int p = own;
        for (unsigned k = 0; k < voice_chord_shapes[voice_chord_shape][j]; ++k)
            do ++p; while (!(mask >> ((p + 12 * 11 - (int)voice_root) % 12) & 1U));
        tones[j] = p;
    }
    for (unsigned j = 0; j < voice_chord_inversion && j < n - 1U; ++j) tones[j] += 12;    /* inverted */
    if (voice_chord_spread >= 1) tones[1] += 12;                                         /* open */
    if (voice_chord_spread >= 2) tones[n - 1U] += 12;                                    /* wide */
    for (unsigned j = 0; j < n; ++j) shift[j] = (int8_t)clamp(tones[j] - base, -36, 48);
    return n;
}
static void voice_release_all(void)
{
    for (unsigned who = 0; who < MUMBLE_CAST; ++who) voice_note_off(who);
    voice_held = 0;
    voice_lyric_lead = MUMBLE_LETTERS;
}
static void voice_rec_stop(void)
{
    for (unsigned who = 0; who < MUMBLE_CAST; ++who) voice_rec_release(who);
    voice_recording = voice_rec_armed = 0;
    if (voice_click) { voice_click = 0; mumble_metronome(0, 0); }
}
/* REC: record into the loop (in SING mode, starting the loop if need be), or stop. */
/* REC: record into the loop as it plays; the loop stopped, wait for the
 * first key (armed) and start it with that; again, stop (or disarm). */
static void voice_rec_toggle(void)
{
    if (voice_recording || voice_rec_armed) {
        unsigned armed = voice_rec_armed;
        voice_rec_stop();
        voice_notice(armed ? "RECORDING CANCELLED" : "RECORDING STOPPED");
        return;
    }
    if (voice_song == 2 && mumble_song_position() < 0) voice_song = 0;   /* (a song over: as stopped) */
    if (!voice_song) {
        voice_rec_armed = 1;
        voice_notice("REC: THE FIRST KEY STARTS THE LOOP");
        return;
    }
    voice_recording = 1;
    voice_notice("RECORDING: SING INTO THE LOOP");
}
/* A key let go frees its singer, who takes up the latest key still held that
 * nobody sings (one character alone: a mono synth's last note). */
static void voice_take_up(unsigned who)
{
    if (!voice_in_choir(who)) return;
    unsigned best = MUMBLE_LETTERS;
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key) {
        if (!(voice_held & (1U << key))) continue;
        unsigned sung = 0;
        for (unsigned i = 0; i < MUMBLE_CAST; ++i) sung |= voice_sings[i] == key + 1U;
        if (!sung && (best == MUMBLE_LETTERS || voice_key_order[key] > voice_key_order[best])) best = key;
    }
    if (best < MUMBLE_LETTERS) voice_note_on(who, best);
}
/* A key pressed goes to a choir member singing nothing (the selected one
 * first), else to the one singing the oldest note. */
static void voice_arp_tick(void);
static void voice_sing(const struct fm1_controls *c, uint32_t down)
{
    if (voice_rec_armed && down) {               /* armed: the first key starts the loop, recording */
        voice_start();
        voice_notice("RECORDING: SING INTO THE LOOP");
    }
    uint32_t up = voice_held & ~c->keys;
    voice_held &= c->keys;
    if (voice_arp) {
        /* The keys are only held: the arpeggiator sings them in turn. */
        for (unsigned who = 0; who < MUMBLE_CAST; ++who)
            if (voice_sings[who] && (up & (1U << (voice_sings[who] - 1U)))) voice_note_off(who);
        if (!voice_held && down) voice_arp_index = -1, voice_arp_count = 0, voice_arp_t0 = frame_time;   /* at once */
        for (unsigned key = 0; key < MUMBLE_LETTERS; ++key)
            if (down & (1U << key)) voice_held |= 1U << key, voice_key_order[key] = ++voice_order;
        voice_arp_tick();
        return;
    }
    if (voice_lyric_lead < MUMBLE_LETTERS && !(voice_held & (1U << voice_lyric_lead))) voice_lyric_lead = MUMBLE_LETTERS;
    for (unsigned who = 0; who < MUMBLE_CAST; ++who)
        if (voice_sings[who] && (up & (1U << (voice_sings[who] - 1U)))) {
            voice_note_off(who);
            voice_take_up(who);
        }
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key) {
        if (!(down & (1U << key))) continue;
        voice_held |= 1U << key;
        voice_key_order[key] = ++voice_order;
        voice_lyric_take(key);
        if (voice_chord) {   /* a chord: as many of its notes, from the root up, as the choir has singers (free ones first) */
            int8_t shift[4];
            uint8_t who[4];
            unsigned n = voice_chord_shifts(key, shift), got = 0;
            for (unsigned pass = 0; pass < 2 && got < n; ++pass)
                for (unsigned i = 0; i < MUMBLE_CAST && got < n; ++i) {
                    unsigned w = (voice_cast + i) % MUMBLE_CAST, taken = 0;
                    for (unsigned k = 0; k < got; ++k) taken |= who[k] == w;
                    if (!taken && voice_in_choir(w) && (pass || !voice_sings[w])) who[got++] = (uint8_t)w;
                }
            for (unsigned k = 0; k < got; ++k) voice_note_off(who[k]);
            voice_notes_on(who, shift, got, key);
            continue;
        }
        unsigned free = MUMBLE_CAST, oldest = MUMBLE_CAST;
        for (unsigned i = 0; i < MUMBLE_CAST && free == MUMBLE_CAST; ++i) {
            unsigned who = (voice_cast + i) % MUMBLE_CAST;
            if (!voice_in_choir(who)) continue;
            if (!voice_sings[who]) free = who;
            else if (oldest == MUMBLE_CAST || voice_sing_order[who] < voice_sing_order[oldest]) oldest = who;
        }
        unsigned who = free < MUMBLE_CAST ? free : oldest;
        voice_note_off(who);
        voice_note_on(who, key);
    }
    /* A choir member free (just joined, say) takes up a key nobody sings. */
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        unsigned who = (voice_cast + i) % MUMBLE_CAST;
        if (!voice_sings[who]) voice_take_up(who);
    }
}

/* The arpeggiator's step: on the loop's time while it runs, else on its own
 * clock from the first key, at the loop's tempo; every other step comes
 * late by the swing. Each step the next of the keys held in the pattern
 * (over the octaves) is sung by the next of the choir, for the gate's part
 * of a step, on the next syllable. */
static void voice_arp_tick(void)
{
    if (!voice_held) {
        for (unsigned who = 0; who < MUMBLE_CAST; ++who) voice_note_off(who);
        voice_arp_count = 0;
        return;
    }
    uint32_t step_ms = voice_arp_rates[voice_arp_rate] * 2500U / voice_tempo();
    if (!step_ms) step_ms = 1;
    int32_t at = mumble_song_position();
    uint32_t pos = at >= 0 ? (uint32_t)at % voice_loop_ms() : frame_time - voice_arp_t0;
    int32_t index = (int32_t)(pos / step_ms);
    if ((index & 1) && pos - (uint32_t)index * step_ms < step_ms * voice_arp_swing / 100U) --index;
    if (voice_arp_gate < 100 && (int32_t)(frame_time - voice_arp_off) >= 0)
        for (unsigned who = 0; who < MUMBLE_CAST; ++who) voice_note_off(who);
    if (index == voice_arp_index) return;
    voice_arp_index = index;
    /* The keys held, low to high (or as they were pressed), over the octaves. */
    uint8_t keys[MUMBLE_LETTERS];
    unsigned n = 0;
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key)
        if (voice_held & (1U << key)) keys[n++] = (uint8_t)key;
    if (voice_arp_pattern == ARP_PLAYED)
        for (unsigned i = 1; i < n; ++i)
            for (unsigned j = i; j > 0 && voice_key_order[keys[j - 1]] > voice_key_order[keys[j]]; --j) {
                uint8_t k = keys[j];
                keys[j] = keys[j - 1], keys[j - 1] = k;
            }
    int8_t tones[4] = {0, 0, 0, 0};
    unsigned per = 1;                                /* (auto-chord: each key's chord's notes in turn) */
    if (voice_chord) per = voice_chord_shifts(keys[0], tones);
    unsigned len = n * per * voice_arp_octaves, e, c = voice_arp_count++;
    if (voice_arp_pattern == ARP_DOWN) e = len - 1U - c % len;
    else if (voice_arp_pattern == ARP_UPDOWN) {
        unsigned period = len > 1U ? 2U * len - 2U : 1U;
        e = c % period < len ? c % period : period - c % period;
    } else if (voice_arp_pattern == ARP_RANDOM) {
        voice_arp_dice ^= voice_arp_dice << 13;
        voice_arp_dice ^= voice_arp_dice >> 17;
        voice_arp_dice ^= voice_arp_dice << 5;
        e = voice_arp_dice % len;
    } else e = c % len;
    unsigned key = keys[e / per % n];
    if (voice_chord) voice_chord_shifts(key, tones);
    int8_t shift = (int8_t)(tones[e % per] + 12 * (int)(e / (per * n)));
    unsigned choir[MUMBLE_CAST], singers = 0;
    for (unsigned i = 0; i < MUMBLE_CAST; ++i)
        if (voice_in_choir((voice_cast + i) % MUMBLE_CAST)) choir[singers++] = (voice_cast + i) % MUMBLE_CAST;
    unsigned who = choir[voice_arp_step++ % singers];
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) voice_note_off(i);
    voice_lyric_lead = MUMBLE_LETTERS;
    voice_lyric_take(key);
    voice_lyric_lead = MUMBLE_LETTERS;
    const uint8_t one = (uint8_t)who;
    voice_notes_on(&one, &shift, 1, key);
    voice_arp_off = frame_time + step_ms * voice_arp_gate / 100U;
}
/* ARP on or off: what sings lets go, the keys still held start again. */
static void voice_arp_set(unsigned on)
{
    for (unsigned who = 0; who < MUMBLE_CAST; ++who) voice_note_off(who);
    voice_lyric_lead = MUMBLE_LETTERS;
    voice_arp = on;
    voice_arp_index = -1;
    voice_arp_count = 0;
    voice_arp_t0 = frame_time;
    voice_held = 0;
}
static void voice_notice(const char *message)
{
    notice = message;
    notice_until = frame_time + 1500U;
}
/* A notice made of parts (held in voice_said, so it lasts its 1.5 s), cut
 * at the hint line's width. */
enum { VOICE_HINT_COLUMNS = 38 };
static void voice_say(const char *a, const char *b, const char *c)
{
    const char *parts[3] = {a, b, c};
    unsigned n = 0;
    for (unsigned k = 0; k < 3; ++k)
        for (const char *p = parts[k]; p && *p && n < VOICE_HINT_COLUMNS && n + 1U < sizeof voice_said; ++p)
            voice_said[n++] = *p;
    voice_said[n] = 0;
    voice_notice(voice_said);
}
static void voice_say_number(const char *a, int value, const char *c)
{
    char number[12];
    decimal(number, value, 1);
    voice_say(a, number, c);
}
/* The mode: TYPE, SING, LOOP (recording stops in TYPE; between SING and LOOP
 * the keys and the recording carry on). */
static void voice_mode_to(unsigned mode)
{
    if (mode == VOICE_TYPE || voice_mode == VOICE_TYPE) {
        voice_release_all();
        voice_rec_stop();
    }
    voice_mode = mode;
    voice_play = mode != VOICE_TYPE;
}
/* EDIT let go before it was held 1 s, with no knob turned: where the keys
 * sing, the selected character's notes out of the loop; in TYPE a word back. */
static void voice_edit_tap(void)
{
    if (voice_play)
        voice_say(mumble_cast[voice_cast].name,
                  voice_seq_clear_track(voice_cast) ? "'S NOTES ARE CLEARED" : " HAS NOTHING IN THE LOOP", NULL);
    else if (!voice_cut_word()) voice_notice(voice_length ? "HOLD EDIT TO CLEAR THE LINE" : "HOLD EDIT TO CLEAR THE LOOP");
}
/* EDIT held 1 s: in TYPE the line, or on an empty line the loop; where the
 * keys sing, the loop. */
static unsigned voice_edit_clears_line(void) { return !voice_play && voice_length; }
static void voice_edit_hold(void)
{
    if (voice_edit_clears_line()) {
        voice_set_text("");
        voice_notice("THE LINE IS CLEARED");
    } else {
        voice_rec_stop();
        voice_seq_clear();
        voice_notice("THE LOOP IS CLEARED");
    }
}

/* Returns 0 when a MIDI post must be retried, as apply_controls does. */
static int voice_controls(const struct fm1_controls *c, uint32_t pressed, uint32_t key_down)
{
    if (pressed & BUTTON(HOME)) { voice_close(); page = OVERVIEW; return 1; }
    voice_help = (c->buttons & BUTTON(HOME)) != 0;
    unsigned system = voice_system, edit = (c->buttons & BUTTON(EDIT)) != 0;
    if (pressed & BUTTON(EDIT)) { voice_edit_since = frame_time; voice_edit_spent = 0; }
    if (system) voice_edit_spent = 1;            /* (on the system screen EDIT is the reset's: no tap, no hold) */
    for (unsigned i = 0; i < 7; ++i)
        if (edit && c->encoders[i]) voice_edit_spent = 1;
    if (c->encoders[0]) {
        unsigned busy = voice_song || voice_held || voice_recording;
        voice_release_all();
        voice_cast = (unsigned)((int)voice_cast + c->encoders[0] % MUMBLE_CAST + MUMBLE_CAST) % MUMBLE_CAST;
        /* A greeting in the new voice, unless the loop plays (or keys, or a
         * recording): then it would talk over the music. */
        const char *hello = voice_greetings[voice_cast];
        if (!busy)
            mumble_aside(voice_cast, hello, (unsigned)strlen(hello), &voice_track()->sound);
        ++voice_lanes_version;
    }
    /* The left knobs: SELECT the character (above), ALGO the mode, PRESET
     * SING's sound page or LOOP's tab. */
    if (c->encoders[1]) {
        static const char *const said[VOICE_MODES] = {"TYPE: KEYS TYPE THE LINE", "SING: KEYS SING THE LINE",
                                                      "LOOP: A LANE FOR EACH SINGER"};
        voice_mode_to((unsigned)((int)voice_mode + c->encoders[1] % VOICE_MODES + VOICE_MODES) % VOICE_MODES);
        voice_notice(said[voice_mode]);
    }
    if (voice_mode == VOICE_LOOP) {
        unsigned tabbed = voice_tab;
        if (c->encoders[6]) voice_tab = (unsigned)((int)voice_tab + c->encoders[6] % VOICE_TABS + VOICE_TABS) % VOICE_TABS;
        if (voice_tab != tabbed) voice_say(voice_tab_names[voice_tab], " TAB", NULL);
    } else if (voice_mode == VOICE_SING) {
        unsigned paged = voice_page;
        if (c->encoders[6]) voice_page = (unsigned)((int)voice_page + c->encoders[6] % VOICE_PAGES + VOICE_PAGES) % VOICE_PAGES;
        if (voice_page != paged) voice_say(voice_page_names[voice_page], " PAGE", NULL);
    } else if (c->encoders[6]) voice_notice("PAGES ARE IN SING, TABS IN LOOP");
    for (unsigned i = 0; i < 4; ++i) {
        int turned = c->encoders[2 + i];
        if (!turned) continue;
        if (!voice_play) {                       /* TYPE: K1 the cursor, K2 its letter */
            if (i == 0) voice_move_cursor(fm_encoder_delta(2, turned));
            else if (i == 1) voice_letter(turned);
        } else if (edit) voice_reset(i);
        else voice_edit(i, fm_encoder_delta(2 + i, turned));    /* SING: the page's sounds; LOOP: the loop */
    }
    /* GLO: the system screen, open or closed; there, EDIT tapped and then
     * held 3 s resets everything. */
    if (system && (pressed & BUTTON(EDIT))) {
        if (voice_reset_armed == 1 && (int32_t)(frame_time - voice_reset_until) < 0) voice_reset_armed = 2;
        else voice_reset_armed = 0;
    }
    if (system && voice_reset_armed == 2) {
        if (!edit) voice_reset_armed = 0, voice_notice("NO RESET: HOLD EDIT 3 S");
        else if (frame_time - voice_edit_since >= VOICE_RESET_MS) voice_reset_everything(), voice_reset_armed = 3;
    } else if (voice_reset_armed == 3) {
        if (!edit) voice_reset_armed = 0;        /* (done: let go, and nothing more) */
    } else if (system && voice_edit_down && !edit && !voice_reset_armed) {
        voice_reset_armed = 1, voice_reset_until = frame_time + 3000U, voice_notice("NOW HOLD EDIT 3 S: RESET ALL");
    }
    if (pressed & BUTTON(GLO)) {
        voice_system ^= 1U;
        voice_reset_armed = 0;
        voice_notice(voice_system ? "SYSTEM: EDIT, THEN HOLD IT: RESET" : "SYSTEM CLOSED");
    }
    if ((pressed & BUTTON(REC)) && !voice_play) voice_mode_to(VOICE_SING);  /* REC sings: to SING mode */
    if (pressed & BUTTON(FX)) {                  /* FX: back to the top (TYPE: the cursor to the line's start) */
        if (voice_play) voice_lyric_rewind(), voice_notice("FROM THE TOP OF THE LINE");
        else voice_cursor = 0, voice_cursor_ms = frame_time, voice_notice("THE CURSOR TO THE START");
    }
    if (pressed & BUTTON(SAVE)) {
        struct voice_track *t = voice_track();
        t->choir ^= 1U;
        voice_say(mumble_cast[voice_cast].name, t->choir ? " JOINED THE CHOIR" : " LEFT THE CHOIR", NULL);
        ++voice_lanes_version;
    }
    if (pressed & BUTTON(REC)) voice_rec_toggle();
    if (voice_play) {
        int octave = voice_octave;
        if ((pressed & BUTTON(OCT_UP)) && voice_octave < 2) ++voice_octave;
        if ((pressed & BUTTON(OCT_DOWN)) && voice_octave > -2) --voice_octave;
        if (pressed & (BUTTON(OCT_UP) | BUTTON(OCT_DOWN)))
            voice_say_number(voice_octave > 0 ? "OCTAVE +" : "OCTAVE ", voice_octave,
                             voice_octave == octave ? (voice_octave ? " (AS FAR AS IT GOES)" : "") : "");
    } else {
        if (pressed & BUTTON(OCT_UP)) {
            voice_caps ^= 1U;
            voice_notice(voice_caps ? "CAPS: SHOUTED WORDS" : "CAPS OFF");
        }
        if ((pressed & BUTTON(OCT_DOWN)) && voice_cursor) voice_cut(voice_cursor - 1U, voice_cursor);
    }
    /* EDIT: a tap acts as it is let go; held 1 s it clears. */
    if (edit && !voice_edit_spent && frame_time - voice_edit_since >= VOICE_HOLD_MS) {
        voice_edit_spent = 1;
        voice_edit_hold();
    }
    if (!edit && voice_edit_down && !voice_edit_spent) voice_edit_tap();
    voice_edit_down = edit;
    /* SEQ: auto-chord on or off (LOOP's CHORD tab sets it). */
    if (pressed & BUTTON(SEQ)) {
        for (unsigned who = 0; who < MUMBLE_CAST; ++who) voice_note_off(who);
        voice_lyric_lead = MUMBLE_LETTERS;
        voice_held = 0;
        voice_chord ^= 1U;
        voice_say("AUTO-CHORD ", voice_chord ? "ON: " : "OFF", voice_chord ? voice_chord_names[voice_chord_shape] : NULL);
    }
    /* ARP: the arpeggiator on or off (LOOP's ARP tab sets it). */
    if (pressed & BUTTON(ARP)) {
        voice_arp_set(!voice_arp);
        voice_notice(voice_arp ? (voice_play ? "ARPEGGIATOR: HOLD KEYS" : "ARPEGGIATOR ON (KEYS IN SING)")
                               : "ARPEGGIATOR OFF");
    }
    /* SCL: the next scale. LFO: a new voice (TYPE: a syllable), held, new
     * voices for everyone; ENV: their own voice back, held, everyone's. */
    if (pressed & BUTTON(SCL)) {
        voice_scale = (voice_scale + 1U) % VOICE_SCALES;
        char name[16];
        voice_say("SCALE: ", voice_scale_name(name), voice_scale ? ": KEYS KEEP TO IT" : NULL);
    }
    /* (In TYPE LFO has no hold: let go, however long, it types a syllable.) */
    unsigned lfo = voice_hold_step(&voice_lfo_hold, (c->buttons & BUTTON(LFO)) != 0, (pressed & BUTTON(LFO)) != 0, voice_play);
    if (lfo == HOLD_TAP && !voice_play) voice_type_syllable();
    else if (lfo == HOLD_TAP) voice_reroll(voice_cast), voice_say(mumble_cast[voice_cast].name, " ROLLED A NEW VOICE", NULL);
    else if (lfo == HOLD_LONG) {
        for (unsigned i = 0; i < MUMBLE_CAST; ++i) voice_reroll(i);
        voice_notice("EVERYONE ROLLED A NEW VOICE");
    }
    unsigned env = voice_hold_step(&voice_env_hold, (c->buttons & BUTTON(ENV)) != 0, (pressed & BUTTON(ENV)) != 0, 1);
    if (env == HOLD_TAP) voice_restore(voice_cast), voice_say(mumble_cast[voice_cast].name, " HAS THEIR OWN VOICE", NULL);
    else if (env == HOLD_LONG) {
        for (unsigned i = 0; i < MUMBLE_CAST; ++i) voice_restore(i);
        voice_notice("EVERYONE HAS THEIR OWN VOICE");
    }
    uint32_t typed = key_down & ~blocked_keys;
    if (voice_play) voice_sing(c, typed);
    else
        for (unsigned key = 0; key < 27; ++key)
            if (typed & (1U << key))
                voice_type(key == 26 ? ' ' : (char)((voice_caps ? 'A' : 'a') + key));
    voice_keys_lit = c->keys;
    if (pressed & BUTTON(PLAY)) {
        if (voice_song) voice_stop();
        else voice_start();
        voice_notice(voice_song ? "LOOP PLAYING" : "LOOP STOPPED");
    }
    return 1;
}

/* Key LEDs: in TYPE they mark the vowels (held keys invert theirs); where
 * the keys sing, a key is lit while held. */
static uint32_t voice_led_bits(void)
{
    uint32_t held = voice_keys_lit & ((1U << 27) - 1U);
    if (voice_play) return held;
    uint32_t vowels = (1U << 0) | (1U << 4) | (1U << 8) | (1U << 14) | (1U << 20) | (1U << 24);
    return vowels ^ held;
}
static uint32_t voice_button_bits(void)
{
    uint32_t octave = voice_play ? ((voice_octave > 0) << OCT_UP) | ((voice_octave < 0) << OCT_DOWN)
                                 : voice_caps << OCT_UP;
    return ((voice_song != 0) << PLAY) | octave | (voice_play << GLO) | ((uint32_t)voice_track()->choir << SAVE) |
           ((voice_recording | (voice_rec_armed & ((frame_time / 250U) & 1U))) << REC) | (voice_arp << ARP) | (voice_chord << SEQ) | ((voice_scale != 0) << SCL);
}

/* The band across the stereo field: those who sing (the choir, and anyone
 * with notes in the loop not muted), in the cast's order, each in the middle
 * of their share of it: one in the middle, two at a quarter and three
 * quarters, three at a sixth, a half, five sixths ... (over 80% of the
 * width to each side); the silent in the middle. */
static void voice_pan_frame(void)
{
    unsigned heard = 0, n = 0;
    for (unsigned k = 0; k < voice_seq_count; ++k)
        if (voice_seq_track[k] < MUMBLE_CAST && voice_tracks[voice_seq_track[k]].level) heard |= 1U << voice_seq_track[k];
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) heard |= (unsigned)voice_in_choir(i) << i, n += (heard >> i) & 1U;
    int8_t place[MUMBLE_CAST] = {0};
    for (unsigned i = 0, k = 0; i < MUMBLE_CAST; ++i)
        if ((heard >> i) & 1U) place[i] = (int8_t)((int)(2U * k++ + 1U) * 80 / (int)n - 80);
    if (memcmp(place, voice_pans, sizeof place)) memcpy(voice_pans, place, sizeof place), mumble_pan(place);
}

/* Control-side snapshot, taken once per frame before marking damage: the
 * loop, FM1 sync, the line, edits to republish, and the pitch trace. */
static void voice_frame(uint32_t now)
{
    mumble_status(&voice_now);
    voice_store_frame(now);
    voice_system_frame(now);
    voice_pan_frame();
    voice_faces_update();
    /* The audio task adopts a song a block later: only a song that was
     * heard playing can be over. */
    if (voice_song == 1 && voice_now.song_ms >= 0) voice_song = 2;
    else if (voice_song == 2 && voice_now.song_ms < 0) voice_song = 0;
    /* Follow a MIDI clock: its Start starts the loop from the top (or relocks
     * it), its Stop stops it, its tempo sizes it; while it plays, a loop more
     * than VOICE_DRIFT_MS off the clock is put back on it. */
    unsigned playing = frame_views[0].playing != 0;
    static unsigned last_tempo;
    if (frame_views[0].stopped) {
        frame_views[0].stopped = 0;
        if (voice_song) { voice_stop(); voice_notice("MIDI STOP"); }
    }
    if (frame_views[0].started) frame_views[0].started = 0, voice_fm1_playing = 0;
    if (playing && !voice_fm1_playing) {
        voice_fm1_playing = 1;
        voice_start();
        voice_notice("MIDI START: THE LOOP FOLLOWS THE CLOCK");
    }
    voice_fm1_playing = playing;
    if (voice_tempo() != last_tempo) {
        last_tempo = voice_tempo();
        voice_touch();
    }
    voice_lyric_sync();
    voice_typewriter();
    if (voice_song_dirty && voice_song) voice_publish(0, 0);
    /* Each pass of the loop sings from the top of the line. */
    if (voice_fm1_playing && voice_song == 2 && voice_now.song_ms >= 0) {
        uint32_t loop = voice_loop_ms(), want = voice_clock_ms();
        uint32_t off = ((uint32_t)voice_now.song_ms + loop - want) % loop;
        if (off > VOICE_DRIFT_MS && loop - off > VOICE_DRIFT_MS) voice_publish(1, want);
    }
    if (voice_song && voice_now.song_ms >= 0) {
        if (voice_now.song_ms < voice_loop_last_ms) voice_lyric_rewind();
        voice_loop_last_ms = voice_now.song_ms;
    } else voice_loop_last_ms = -1;
    /* Recording: the metronome counts the loop's beats. */
    unsigned click = voice_recording ? voice_bars * 4U : 0U;
    static uint32_t click_loop;
    if (click != voice_click || (click && click_loop != voice_loop_ms())) {
        voice_click = click;
        click_loop = voice_loop_ms();
        mumble_metronome(click, click_loop);
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
 * The cast in OP-1 style line art, built from their voices (mumble_faces.inc;
 * the palette in mumble_sprites.inc): white strokes and encoder-colour accents
 * on black. 24x24 portraits are drawn at 3x on whole pixels; 16x16 icons fill
 * the choir strip. Mouths, blinks and the shout mark are pixel overlays on a
 * working copy of the portrait, where the face's rig says.
 * ---------------------------------------------------------------------- */
#include "mumble_sprites.inc"

enum { SPRITE = 24, SPRITE_SCALE = 3, ICON = 16 };
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
static uint16_t dim(uint16_t c) { return (uint16_t)((c >> 1) & 0x7befU); }
static uint16_t dimmer(uint16_t c) { return (uint16_t)((c >> 2) & 0x39e7U); }
/* Triangle wave in -amp..amp over `period` ms, offset by `phase` ms. */
static int voice_wave(unsigned period, int amp, unsigned phase)
{
    unsigned p = (frame_time + phase) % period;
    int x = (int)(p * 4U * (unsigned)amp / period);
    return x < 2 * amp ? x - amp : 3 * amp - x;
}

/* Mouth size per vowel when fully open (icon pixels). */
static const uint8_t vowel_mouth[MUMBLE_VOWELS][2] = {{4, 3}, {4, 2}, {4, 1}, {3, 3}, {2, 2}, {3, 2},
                                                       {4, 1}, {2, 2}, {3, 2}};

static char voice_grid[SPRITE][SPRITE + 1];
static void grid_set(int x, int y, char c)
{
    if (x >= 0 && x < SPRITE && y >= 0 && y < SPRITE) voice_grid[y][x] = c;
}
static void grid_rect(int x, int y, int w, int h, char c)
{
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) grid_set(x + i, y + j, c);
}

struct pose { unsigned open, vowel, blink, shout, tick; };
/* The selected character's portrait as it is now: eyes shut for a blink, the
 * mouth open on a vowel (a robot's green), a robot's lamp blinking, CAPS's "!". */
static void voice_pose(const struct pose *p)
{
    const struct voice_face *f = &voice_portrait;
    for (unsigned y = 0; y < SPRITE; ++y) memcpy(voice_grid[y], f->grid[y], SPRITE);
    char lip = f->body == BODY_ROBOT ? 'g' : f->lip;
    if (p->blink)
        for (unsigned e = 0; e < f->eyes; ++e) {
            const uint8_t *eye = f->eye[e];
            grid_rect(eye[0], eye[1], eye[2], eye[3], '.');
            grid_rect(eye[0], eye[1] + eye[3] - 1, eye[2], 1, f->body == BODY_ROBOT ? 'g' : 'w');
        }
    int y = f->mouth_y;
    if (p->open) {
        /* The drawn mouth gives way to an open one: a stroke for narrow
         * vowels, a ring for round ones. */
        unsigned v = p->vowel < MUMBLE_VOWELS ? p->vowel : MUMBLE_U;
        int w = vowel_mouth[v][0], h = (int)((vowel_mouth[v][1] * p->open + 3U) / 4U);
        int x = 12 - (w + 1) / 2;
        grid_rect(12 - f->mouth_w / 2, y, f->mouth_w, 1, '.');
        grid_rect(x, y, w, h, lip);
        if (h >= 3 && w >= 3) grid_rect(x + 1, y + 1, w - 2, h - 2, '.');
    }
    if (f->top == TOP_ANTENNA && (p->tick & 8U))                                /* the lamp blinks */
        for (int j = 0; j < 6; ++j)
            for (int i = 10; i < 14; ++i)
                if (voice_grid[j][i] == 'o') voice_grid[j][i] = 'm';
    if (p->shout) {                                                             /* "!" */
        grid_rect(23, 0, 1, 3, 'o');
        grid_set(23, 4, 'o');
    }
}
/* Run-length rows of the working grid (size x size) at any scale; faded for
 * the choir strip. */
static void voice_sprite(int ox, int oy, int scale, unsigned faded, int size)
{
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size;) {
            char c = voice_grid[y][x];
            int run = 1;
            while (x + run < size && voice_grid[y][x + run] == c) ++run;
            uint16_t color = voice_palette(c);
            if (color) vfill(ox + x * scale, oy + y * scale, run * scale, scale, faded ? dimmer(color) : color);
            x += run;
        }
}
/* Layout: header 0..27, stage 28..102 (portrait 4..101, pitch roll 106..235),
 * choir strip 103..120, dialogue box 122..186, then knobs and hint. */
enum { STAGE_TOP = 28, STAGE_BOTTOM = 103, PORTRAIT_X1 = 102, STRIP_TOP = 103, STRIP_BOTTOM = 121,
       BOX_TOP = 122, BOX_BOTTOM = 187 };

static void voice_header(void)
{
    if (paint_rendering && paint_top >= 28) return;
    const struct mumble_character *who = &mumble_cast[voice_cast];
    /* (cast: 3 bits, page: 3 bits, then one bit each) */
    uint32_t sig = voice_cast | (voice_page << 3) | (voice_caps << 6) | (voice_edited() << 7) |
                   (voice_mode << 8) | ((voice_song != 0) << 10) | ((uint32_t)voice_track()->choir << 11) |
                   (voice_bars << 12) | (voice_tempo() << 16) | ((uint32_t)voice_fm1_playing << 24);
    sig = signature(sig, &voice_tab, sizeof voice_tab);
    if (cache->voice_cache[0] == sig) return;
    cache->voice_cache[0] = sig;
    paint_fill(0, 0, 240, 28, BACK);
    /* The three modes (the current one lit, a play mark while the loop
     * runs, CAPS in TYPE), and in SING its sound pages, the knobs' one lit. */
    static const uint16_t mode_colors[VOICE_MODES] = {TEXT, GREEN, ACCENT};
    for (unsigned m = 0; m < VOICE_MODES; ++m)
        text(8U + m * 30U, 4, voice_mode_names[m], 1, m == voice_mode ? mode_colors[m] : PANEL, BACK);
    if (voice_song)
        for (unsigned r = 0; r < 7; ++r) paint_fill(98, 4U + r, r < 4U ? r + 1U : 7U - r, 1, ACCENT);
    if (voice_caps) text(110, 4, "CAPS", 1, AMBER, BACK);
    for (unsigned k = 0, x = 8; voice_mode == VOICE_SING && k < VOICE_PAGES; ++k) {
        text(x, 16, voice_page_names[k], 1, k == voice_page ? mode_colors[VOICE_SING] : PANEL, BACK);
        x += ((unsigned)strlen(voice_page_names[k]) + 1U) * 6U;
    }
    for (unsigned k = 0, x = 8; voice_mode == VOICE_LOOP && k < VOICE_TABS; ++k) {   /* LOOP's tabs */
        text(x, 16, voice_tab_names[k], 1, k == voice_tab ? mode_colors[VOICE_LOOP] : PANEL, BACK);
        x += ((unsigned)strlen(voice_tab_names[k]) + 1U) * 6U;
    }
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
    const unsigned who = voice_cast;
    unsigned vowel = voice_now.track_vowel[who], level = voice_now.track_level[who];
    struct pose p;
    p.vowel = vowel;
    p.open = vowel < MUMBLE_VOWELS ? (level > 180U ? 4U : level / 45U) : 0U;
    p.blink = ((frame_time + who * 977U) % 3700U) < 140U;
    p.shout = voice_caps;
    p.tick = frame_time / 60U;
    /* Motion snaps to icon pixels: a hop on every syllable, a beanie
     * bounces; only a ghost floats smoothly. */
    const struct voice_face *f = &voice_portrait;
    int lift = p.open ? SPRITE_SCALE : 0;
    if ((f->top == TOP_BEANIE || f->top == TOP_PROPELLER) && ((frame_time / 450U) & 1U)) lift = SPRITE_SCALE;
    if (f->body == BODY_GHOST) lift = 1 + voice_wave(2000, 1, 0);
    if (f->top != TOP_ANTENNA) p.tick = 0;
    unsigned tick_shown = p.tick & 8U;                  /* (what of the tick shows: the lamp) */
    struct { int lift; unsigned open, vowel, blink, shout, cast, tick, version; } sig_in = {
        lift, p.open, vowel, p.blink, p.shout, who, tick_shown, voice_faces_version};
    uint32_t sig = signature(0x0FACE, &sig_in, sizeof sig_in);
    if (cache->voice_cache[1] == sig) return;
    cache->voice_cache[1] = sig;
    /* Straight on black, as an OP-1 screen draws its graphics. A hop lifts
     * the figure; its last row (the arms) stretches down to the floor. */
    const int size = SPRITE * SPRITE_SCALE, x0 = 53 - size / 2;
    voice_clip(4, STAGE_TOP, PORTRAIT_X1, STAGE_BOTTOM);
    vfill(4, STAGE_TOP, PORTRAIT_X1 - 4, STAGE_BOTTOM - STAGE_TOP, BACK);
    voice_pose(&p);
    voice_sprite(x0, STAGE_BOTTOM - size - lift, SPRITE_SCALE, 0, SPRITE);
    for (int x = 0; lift > 0 && x < SPRITE; ++x) {
        uint16_t color = voice_palette(voice_grid[SPRITE - 1][x]);
        if (color) vfill(x0 + x * SPRITE_SCALE, STAGE_BOTTOM - lift, SPRITE_SCALE, lift, color);
    }
}

static void voice_roll(void)
{
    if (paint_rendering && (paint_top >= STAGE_BOTTOM || paint_top + PAINT_ROWS <= STAGE_TOP)) return;
    /* The label follows the voice every frame, the trace every 50 ms: both sign. */
    uint32_t sig = signature(signature(voice_cast, voice_trace, sizeof voice_trace), &voice_now.vowel,
                             sizeof voice_now.vowel);
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

/* The whole cast at 1x: bright in the choir, underlined when selected,
 * hopping while they sound. */
static void voice_choir_strip(void)
{
    if (paint_rendering && (paint_top >= STRIP_BOTTOM || paint_top + PAINT_ROWS <= STRIP_TOP)) return;
    uint8_t state[MUMBLE_CAST];
    for (unsigned i = 0; i < MUMBLE_CAST; ++i)
        state[i] = (uint8_t)((voice_now.track_level[i] > 40U) | (voice_tracks[i].choir << 2) |
                             ((i == voice_cast) << 3) | ((voice_notes_of(i) != 0) << 4));
    uint32_t sig = signature(0xC402 ^ voice_faces_version, state, sizeof state);
    if (cache->voice_cache[7] == sig) return;
    cache->voice_cache[7] = sig;
    voice_clip(0, STRIP_TOP, 240, STRIP_BOTTOM);
    vfill(0, STRIP_TOP, 240, STRIP_BOTTOM - STRIP_TOP, BACK);
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        int x = 14 + (int)i * 28, hop = voice_now.track_level[i] > 40U;
        for (int row = 0; row < ICON; ++row) memcpy(voice_grid[row], voice_icons[i][row], FACE_ICON);
        voice_sprite(x, STRIP_TOP + 1 - hop, 1, !voice_in_choir(i), ICON);
        if (i == voice_cast) vfill(x + 2, STRIP_BOTTOM - 1, 12, 1, WHITE);
        if (voice_notes_of(i)) vfill(x + 17, STRIP_TOP + 2, 2, 2, voice_in_choir(i) ? TEXT : DIM);   /* in the loop */
    }
}

/* Word-wrap the text into rows of VOICE_COLUMNS and keep the rows that show
 * the cursor (the last ones while it is at the end): returns how many rows
 * there are, and where the cursor is among them. */
static unsigned voice_rows(const char *s, unsigned n, unsigned cursor, char rows[VOICE_ROWS][VOICE_COLUMNS + 1],
                           unsigned *cursor_row, unsigned *cursor_col)
{
    char all[MUMBLE_TEXT / 4 + 8][VOICE_COLUMNS + 1];
    unsigned count = 0, used = 0, at_row = 0, at_col = 0;
    memset(all, 0, sizeof all);
    for (unsigned i = 0; i < n && count < sizeof all / sizeof all[0];) {
        unsigned word = i;
        while (word < n && s[word] != ' ') ++word;
        unsigned len = word - i;
        if (used && used + len > VOICE_COLUMNS) { ++count; used = 0; }
        if (count >= sizeof all / sizeof all[0]) break;
        for (; i < word; ++i) {
            if (used == VOICE_COLUMNS) { ++count; used = 0; if (count >= sizeof all / sizeof all[0]) break; }
            if (i == cursor) at_row = count, at_col = used;
            all[count][used++] = s[i];
        }
        if (i < n && s[i] == ' ') {
            if (i == cursor) at_row = count, at_col = used;
            if (used < VOICE_COLUMNS) all[count][used++] = ' ';
            ++i;
        }
    }
    if (count >= sizeof all / sizeof all[0]) count = sizeof all / sizeof all[0] - 1U;
    if (cursor >= n) at_row = count, at_col = used;
    unsigned total = count + 1U, first = total > VOICE_ROWS ? total - VOICE_ROWS : 0U;
    if (at_row < first) first = at_row;
    for (unsigned r = 0; r < VOICE_ROWS; ++r)
        memcpy(rows[r], first + r < total ? all[first + r] : all[sizeof all / sizeof all[0] - 1], VOICE_COLUMNS + 1);
    *cursor_row = at_row - first;
    *cursor_col = at_col;
    return total - first < VOICE_ROWS ? total - first : VOICE_ROWS;
}

/* An Undertale-style dialogue box; it takes the speaker's colour. */
static void voice_box(void)
{
    if (paint_rendering && (paint_top >= BOX_BOTTOM || paint_top + PAINT_ROWS <= BOX_TOP)) return;
    uint16_t color = voice_song ? mumble_cast[voice_cast].color : TEXT;
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
    unsigned shown = voice_length;
    char rows[VOICE_ROWS][VOICE_COLUMNS + 1];
    unsigned at_row, at_col;
    voice_rows(voice_text, shown, voice_cursor, rows, &at_row, &at_col);
    unsigned cursor_on = ((frame_time - voice_cursor_ms) / 530U) % 2U == 0U;
    for (unsigned r = 0; r < VOICE_ROWS; ++r) {
        unsigned y = 128U + r * 19U;
        unsigned cursor_col = r == at_row ? at_col : ~0U;
        /* (The placeholder, with nothing shown yet, says whether there is a line.) */
        uint32_t sig = signature(cursor_on | (shown == 0) << 1 | (voice_cast << 4), rows[r], VOICE_COLUMNS) ^ cursor_col;
        if (cache->voice_cache[3 + r] == sig) continue;
        cache->voice_cache[3 + r] = sig;
        paint_fill(4, y - 2, 232, 18, BACK);
        if (!shown && r == 0) {
            text(10, y + 3, "* TYPE A LINE", 1, DIM, BACK);
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
            uint16_t ink = letters > 1 && upper == letters ? AMBER : TEXT;
            if (i == cursor_col && cursor_on) {   /* the cursor's letter, inverted */
                paint_fill(9U + i * 12U, y - 1U, 12, 16, AMBER);
                text(10U + i * 12U, y, glyph, 2, BACK, AMBER);
            } else text(10U + i * 12U, y, glyph, 2, ink, BACK);
        }
        /* Past the row's letters (the line's end): a block; under a letter, a
         * line that stays while the block blinks. */
        if (cursor_col <= VOICE_COLUMNS) {
            if (cursor_col >= strlen(rows[r]) && cursor_on) paint_fill(10U + cursor_col * 12U, y, 9, 14, AMBER);
            paint_fill(9U + cursor_col * 12U, y + 15U, 11, 1, AMBER);
        }
    }
}

/* SING: the octave, the choir, and REC n / LOOP n (notes in the loop) along the box's foot. */
static void voice_play_status(unsigned singers, unsigned notes)
{
    char octave[12] = "OCT ", value[12];
    decimal(value, voice_octave, 1);
    if (voice_octave > 0) strcat(octave, "+");
    strcat(octave, value);
    text(10, 172, octave, 1, voice_octave ? AMBER : DIM, BACK);
    char name[16];
    if (voice_scale) text(52, 172, voice_scale_name(name), 1, ACCENT, BACK);
    if (singers > 1) {
        char choir[16] = "CHOIR ";
        decimal(value, (int)singers, 1);
        strcat(choir, value);
        text(100, 172, choir, 1, ACCENT, BACK);
    }
    if (voice_rec_armed) {                       /* waiting for the first key */
        paint_fill(158, 172, 7, 7, AMBER);
        text(168, 172, "REC: A KEY", 1, AMBER, BACK);
    } else if (voice_recording || notes) {
        char take[16];
        strcpy(take, voice_recording ? "REC " : "LOOP ");
        decimal(value, (int)notes, 1);
        strcat(take, value);
        if (voice_recording) paint_fill(172, 172, 7, 7, AMBER);
        text(voice_recording ? 182 : 172, 172, take, 1, voice_recording ? AMBER : DIM, BACK);
    }
}
/* SING mode: the line typed out as it is sung (the syllable sung last in the
 * singer's colour), the one a key would sing now underlined and faint ahead
 * of it, and where the queue is. */
static void voice_line_draw(void)
{
    if (paint_rendering && (paint_top >= BOX_BOTTOM - 2 || paint_top + PAINT_ROWS <= BOX_TOP + 2)) return;
    unsigned n;
    const char *line = voice_lyric_text(&n);
    unsigned held = voice_sung < MUMBLE_LETTERS && (voice_held & (1U << voice_sung)) != 0;
    unsigned singers = 0;
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) singers += voice_in_choir(i);
    unsigned notes = voice_seq_count, at = voice_lyric_next(), count = voice_lyric_count;
    uint32_t sig = signature(0x4C000000U | at | (held << 7) | ((unsigned)(voice_octave + 2) << 8) | (voice_caps << 11) |
                                 (voice_cast << 12) | (singers << 15) | (voice_recording << 19) | (voice_sung_note << 20),
                             &notes, sizeof notes);
    sig = signature(sig, &voice_lyric_sig, sizeof voice_lyric_sig);
    sig = signature(sig, &voice_scale, sizeof voice_scale);
    sig = signature(sig, &voice_rec_armed, sizeof voice_rec_armed);
    unsigned typed = voice_typed();
    sig = signature(sig, &typed, sizeof typed);
    /* The TYPE rows share this area: make them redraw on return. */
    cache->voice_cache[4] = cache->voice_cache[5] = 0xfffffffeU;
    if (cache->voice_cache[3] == sig) return;
    cache->voice_cache[3] = sig;
    cache->voice_cache[8] = 0xfffffffeU;        /* the loop bar, drawn over this */
    paint_fill(4, BOX_TOP + 2, 232, BOX_BOTTOM - BOX_TOP - 4, BACK);
    uint16_t mark = voice_caps ? AMBER : mumble_cast[voice_cast].color;
    char value[12];
    if (!count) text(10, 131, "* NOTHING TO SING IN THIS LINE", 1, DIM, BACK);
    else {
        unsigned from = voice_lyric_onset(line, at), to = at + 1U < count ? voice_lyric_onset(line, at + 1U) : n;
        while (to > from + 1U && ((line[to - 1U] | 0x20) < 'a' || (line[to - 1U] | 0x20) > 'z')) --to;
        /* The window keeps the syllable sung last in view (whole, as it
         * types out), six letters of what came before it, then the next. */
        unsigned sung = voice_lyric_started ? voice_lyric_onset(line, voice_lyric_at) : n;
        unsigned anchor = sung < from ? sung : from;
        unsigned start = anchor > 6U ? anchor - 6U : 0U;
        if (n > VOICE_COLUMNS && start + VOICE_COLUMNS > n) start = n - VOICE_COLUMNS;
        for (unsigned c = 0; c < VOICE_COLUMNS && start + c < n; ++c) {
            unsigned i = start + c;
            char glyph[2] = {line[i], 0};
            if (i < typed) text(10U + c * 12U, 130, glyph, 2, i >= sung ? mark : TEXT, BACK);
            else if (i >= from && i < to) text(10U + c * 12U, 130, glyph, 2, PANEL, BACK);
        }
        unsigned end = to < start + VOICE_COLUMNS ? to : start + VOICE_COLUMNS;
        if (end > from) paint_fill(10U + (from - start) * 12U, 146, (end - from) * 12U - 2U, 2, mark);
        char where[24] = "SYLLABLE ";
        decimal(value, (int)at + 1, 1);
        strcat(where, value);
        strcat(where, " OF ");
        decimal(value, (int)count, 1);
        strcat(where, value);
        text(10, 154, where, 1, DIM, BACK);
    }
    if (voice_sung < MUMBLE_LETTERS) {
        note_name(voice_sung_note, value);
        text(170, 152, value, 2, held ? TEXT : DIM, BACK);
    }
    voice_play_status(singers, notes);
}
/* SING mode, while the loop runs: where it is, its beats ticked, amber while
 * recording. */
static void voice_loop_bar(void)
{
    if (paint_rendering && (paint_top > 181 || paint_top + PAINT_ROWS <= 180)) return;
    const unsigned x0 = 10, w = 220, y = 180;
    unsigned running = voice_song && voice_loop && voice_now.song_ms >= 0, head = 0;
    uint32_t loop = voice_loop_ms();
    if (running) head = (unsigned)((uint32_t)voice_now.song_ms * w / loop);
    if (head > w) head = w;
    uint32_t sig = running ? signature(head | voice_recording << 8 | voice_bars << 9 | voice_cast << 13, &loop, sizeof loop)
                           : 0x10000000U;
    if (cache->voice_cache[8] == sig) return;
    cache->voice_cache[8] = sig;
    /* (Two rows, 180 and 181: the TYPE rows clear them on leaving SING.) */
    paint_fill(x0, y, w + 1, 2, BACK);
    if (!running) return;
    paint_fill(x0, y + 1, w, 1, PANEL);
    unsigned beats = voice_bars * 4U;
    for (unsigned k = 0; k <= beats; ++k)
        paint_fill(x0 + k * w / beats, y, 1, 2, k % 4U ? PANEL : TEXT);
    paint_fill(x0, y, head, 2, voice_recording ? AMBER : mumble_cast[voice_cast].color);
}

/* LOOP mode: eight lanes over one loop, OP-1 tape style. Each lane shows
 * its character's notes in the sequence (dim when muted), each start marked
 * white; the playhead sweeps while the loop plays. */
static void voice_lanes_draw(void)
{
    if (paint_rendering && (paint_top >= BOX_BOTTOM - 2 || paint_top + PAINT_ROWS <= BOX_TOP + 2)) return;
    const int x0 = 24, x1 = 232, lane_top = BOX_TOP + 4, lane_h = 7;
    uint32_t loop = voice_loop_ms();
    int head = -1;
    if (voice_song == 2 && voice_now.song_ms >= 0) {
        uint32_t at = (uint32_t)voice_now.song_ms;
        if (!voice_loop) while (at >= loop) at -= loop;
        head = x0 + (int)(at * (uint32_t)(x1 - x0) / loop);
    }
    uint32_t sig = signature(0x1A9E5 ^ voice_lanes_version, &head, sizeof head) ^
                   (voice_cast << 4) ^ ((uint32_t)voice_song << 8) ^ ((uint32_t)voice_loop << 10) ^
                   ((uint32_t)voice_recording << 11);
    /* The TYPE rows share this area: make them redraw on return. */
    cache->voice_cache[4] = cache->voice_cache[5] = 0xfffffffeU;
    if (cache->voice_cache[3] == sig) return;
    cache->voice_cache[3] = sig;
    voice_clip(4, BOX_TOP + 2, 236, BOX_BOTTOM - 2);
    vfill(4, BOX_TOP + 2, 232, BOX_BOTTOM - BOX_TOP - 4, BACK);
    /* Beats and bars. */
    for (unsigned step = 0; step <= voice_steps(); step += 4) {
        int x = x0 + (int)(step * (unsigned)(x1 - x0) / voice_steps());
        vfill(x, lane_top - 2, 1, step % 16U ? 1 : 2, step % 16U ? PANEL : DIM);
        vfill(x, lane_top + 8 * lane_h, 1, step % 16U ? 1 : 2, step % 16U ? PANEL : DIM);
    }
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        const struct voice_track *t = &voice_tracks[i];
        int y = lane_top + (int)i * lane_h;
        unsigned on = (voice_in_choir(i) || voice_notes_of(i)) && t->level;
        uint16_t color = on ? mumble_cast[i].color : dimmer(mumble_cast[i].color);
        if (i == voice_cast) vfill(6, y, 2, lane_h - 1, WHITE);
        vfill(12, y + 1, 7, lane_h - 3, color);                  /* the character's colour chip */
        vfill(x0, y + 3, x1 - x0, 1, PANEL);
        for (unsigned k = 0; k < voice_seq_count; ++k) {
            const struct mumble_note *q = &voice_seq[k];
            if (voice_seq_track[k] != i || q->ms >= loop) continue;   /* (past the end of a shorter loop: not played) */
            uint32_t at = q->ms;
            int x = x0 + (int)(at * (uint32_t)(x1 - x0) / loop);
            int w = (int)(q->len_ms * (uint32_t)(x1 - x0) / loop);
            if (w < 2) w = 2;
            if (x + w > x1) w = x1 - x;
            vfill(x, y + 1, w, lane_h - 3, color);
            vfill(x, y, 1, lane_h - 1, t->level ? WHITE : DIM);
        }
    }
    if (head >= 0) vfill(head, lane_top - 1, 1, 8 * lane_h + 1, voice_recording ? AMBER : WHITE);
}

/* HOME held: what each control does in this mode, over the dialogue box. */
static void voice_help_draw(void)
{
    if (paint_rendering && (paint_top >= BOX_BOTTOM - 2 || paint_top + PAINT_ROWS <= BOX_TOP + 2)) return;
    uint32_t sig = 0x4E1F0000U | voice_mode;
    /* The rows under it redraw when it goes. */
    cache->voice_cache[4] = cache->voice_cache[5] = cache->voice_cache[8] = 0xfffffffeU;
    if (cache->voice_cache[3] == sig) return;
    cache->voice_cache[3] = sig;
    paint_fill(4, BOX_TOP + 2, 232, BOX_BOTTOM - BOX_TOP - 4, BACK);
    for (unsigned r = 0; r < VOICE_HELP_ROWS; ++r)
        text(10, 127U + r * 10U, voice_help_rows[voice_mode][r], 1, r ? TEXT : ACCENT, BACK);
}

/* GLO: the system card, over the dialogue box. */
static void voice_system_draw(void)
{
    if (paint_rendering && (paint_top >= BOX_BOTTOM - 2 || paint_top + PAINT_ROWS <= BOX_TOP + 2)) return;
    uint32_t sig = signature(0x5157E, &voice_sys, sizeof voice_sys);
    cache->voice_cache[4] = cache->voice_cache[5] = cache->voice_cache[8] = 0xfffffffeU;
    if (cache->voice_cache[3] == sig) return;
    cache->voice_cache[3] = sig;
    paint_fill(4, BOX_TOP + 2, 232, BOX_BOTTOM - BOX_TOP - 4, BACK);
    char row[48], value[12];
    text(10, 126, "SYSTEM", 1, ACCENT, BACK);
    strcpy(row, "AUDIO LOAD ");
    decimal(value, (int)voice_sys.cpu, 1), strcat(row, value), strcat(row, "%  MOST ");
    decimal(value, (int)voice_sys.cpu_peak, 1), strcat(row, value), strcat(row, "%");
    text(10, 136, row, 1, voice_sys.cpu_peak > 85U ? AMBER : TEXT, BACK);
    strcpy(row, "DROPOUTS ");
    decimal(value, (int)voice_sys.xruns, 1), strcat(row, value);
    text(10, 146, row, 1, voice_sys.xruns ? AMBER : TEXT, BACK);
    if (voice_sys.battery == 4) strcpy(row, "POWER: USB");
    else if (voice_sys.battery < 0) strcpy(row, "BATTERY: NOT READ YET");
    else {
        static const char *const bars[4] = {"EMPTY", "1 OF 3", "2 OF 3", "FULL"};
        strcpy(row, "BATTERY ");
        strcat(row, bars[voice_sys.battery]);
        if (voice_sys.raw) strcat(row, " ("), decimal(value, voice_sys.raw, 1), strcat(row, value), strcat(row, ")");
        if (voice_sys.charging == 2) strcat(row, " CHARGING");
    }
    text(10, 156, row, 1, voice_sys.battery == 0 ? AMBER : TEXT, BACK);
    strcpy(row, "ON FOR ");
    decimal(value, (int)(voice_sys.up_s / 3600U), 1), strcat(row, value), strcat(row, ":");
    decimal(value, (int)(voice_sys.up_s / 60U % 60U), 2), strcat(row, value), strcat(row, ":");
    decimal(value, (int)(voice_sys.up_s % 60U), 2), strcat(row, value);
    strcat(row, voice_sys.sync ? "  MIDI CLOCK: SYNC" : "  NO MIDI CLOCK");
    text(10, 166, row, 1, TEXT, BACK);
    text(10, 176, "RESET ALL: EDIT, THEN HOLD IT 3 S", 1, DIM, BACK);
}

/* A knob: its name, its value (unit small after it), and over them a bar of
 * where the value sits in its range, ticked where the character has it. */
enum { NO_TICK = 0xffffU };
static void voice_meter(unsigned col, const char *label, const char *value, const char *unit, uint16_t color,
                        unsigned at, unsigned span, unsigned tick)
{
    if (paint_rendering && (paint_top >= 227 || paint_top + PAINT_ROWS <= 194)) return;
    uint32_t sig = signature(signature(color ^ (span << 16), label, (unsigned)strlen(label)), value, (unsigned)strlen(value));
    sig = signature(sig ^ at ^ (tick << 12), unit, (unsigned)strlen(unit));
    if (cache->foot_cache[col] == sig) return;
    cache->foot_cache[col] = sig;
    unsigned x = 6 + 59 * col, w = 54;
    paint_fill(x, 194, w, 33, BACK);
    if (span) {
        unsigned fill = at >= span ? w : at * w / span;
        paint_fill(x, 195, w, 2, dimmer(color));
        paint_fill(x, 195, fill, 2, color);
        if (tick != NO_TICK) paint_fill(x + (tick >= span ? w - 1U : tick * (w - 1U) / span), 194, 1, 4, WHITE);
    } else paint_fill(x, 195, w, 2, PANEL);
    text(x, 200, label, 1, span ? TEXT : DIM, BACK);
    unsigned n = (unsigned)strlen(value);
    unsigned big = n <= 4U && n * 12U + (unsigned)strlen(unit) * 6U <= w;
    text(x, 212, value, big ? 2 : 1, color, BACK);
    text(x + n * (big ? 12U : 6U), big ? 219U : 212U, unit, 1, color, BACK);
}

static void voice_knobs(void)
{
    char value[12], unit[12] = "";
    if (voice_mode == VOICE_LOOP && voice_tab == TAB_ARP) {
        voice_meter(0, "RATE", voice_arp_rate_names[voice_arp_rate], "", voice_knob_colors[0], voice_arp_rate,
                    ARP_RATES - 1, ARP_RATE);
        voice_meter(1, "PATTERN", voice_arp_pattern_names[voice_arp_pattern], "", voice_knob_colors[1], voice_arp_pattern,
                    ARP_PATTERNS - 1, 0);
        decimal(value, (int)voice_arp_octaves, 1);
        voice_meter(2, "OCTAVES", value, "", voice_knob_colors[2], voice_arp_octaves - 1U, 2, 0);
        decimal(value, (int)voice_arp_gate, 1);
        voice_meter(3, "GATE", value, "%", voice_knob_colors[3], voice_arp_gate - 10U, 90, ARP_GATE - 10);
        return;
    }
    if (voice_mode == VOICE_LOOP && voice_tab == TAB_CHORD) {
        voice_meter(0, "SHAPE", voice_chord_names[voice_chord_shape], "", voice_knob_colors[0], voice_chord_shape,
                    CHORD_SHAPES - 1, 0);
        decimal(value, (int)voice_chord_inversion, 1);
        voice_meter(1, "INVERSION", value, "", voice_knob_colors[1], voice_chord_inversion, 3, 0);
        voice_meter(2, "SPREAD", voice_spread_names[voice_chord_spread], "", voice_knob_colors[2], voice_chord_spread,
                    CHORD_SPREADS - 1, 0);
        voice_meter(3, voice_chord ? "SEQ: ON" : "SEQ: OFF", "", "", voice_knob_colors[3], 0, 0, NO_TICK);
        return;
    }
    if (voice_mode == VOICE_LOOP && voice_tab == TAB_SCALE) {
        voice_meter(0, "ROOT", voice_note_names[voice_root], "", voice_knob_colors[0], voice_root, 11, 0);
        voice_meter(1, "SCALE", voice_scale_names[voice_scale], "", voice_knob_colors[1], voice_scale, VOICE_SCALES - 1, 0);
        voice_meter(2, "", "", "", voice_knob_colors[2], 0, 0, NO_TICK);
        voice_meter(3, "", "", "", voice_knob_colors[3], 0, 0, NO_TICK);
        return;
    }
    if (!voice_play) {
        /* TYPE: K1 the cursor, K2 the letter under it. */
        if (voice_cursor < voice_length) decimal(value, (int)voice_cursor + 1, 1);
        else strcpy(value, "END");
        voice_meter(0, "CURSOR", value, "", voice_knob_colors[0], voice_cursor, voice_length ? voice_length : 1U, NO_TICK);
        unsigned at = voice_letter_at(voice_cursor);
        char c = voice_cursor < voice_length ? voice_text[voice_cursor] : 0;
        if (!c) strcpy(value, "+");
        else if (c == ' ') strcpy(value, "SPC");
        else value[0] = c, value[1] = 0;
        voice_meter(1, c ? "LETTER" : "ADD", value, "", voice_knob_colors[1], at < VOICE_ALPHABET ? at : 0U,
                    VOICE_ALPHABET - 1U, NO_TICK);
        voice_meter(2, "", "", "", voice_knob_colors[2], 0, 0, NO_TICK);
        voice_meter(3, "", "", "", voice_knob_colors[3], 0, 0, NO_TICK);
        return;
    }
    if (voice_mode == VOICE_LOOP) {
        const struct voice_track *t = voice_track();
        decimal(value, t->level, 1);
        voice_meter(0, "LEVEL", t->level ? value : "MUTE", t->level ? "%" : "", voice_knob_colors[0], t->level, 100, 100);
        decimal(value, (int)voice_arp_swing, 1);
        voice_meter(1, "SWING", value, "%", voice_knob_colors[1], voice_arp_swing, ARP_SWING_MAX, 0);
        unsigned bars = 0;
        while (bars < 3 && voice_bar_choices[bars] != voice_bars) ++bars;
        decimal(value, (int)voice_bars, 1);
        voice_meter(2, "BARS", value, "", voice_knob_colors[2], bars, 3, 1);
        decimal(value, (int)voice_tempo(), 1);
        voice_meter(3, voice_fm1_playing ? "SYNC" : "BPM", value, "", voice_knob_colors[3], voice_tempo() - 40U, 200, 80);
        return;
    }
    for (unsigned col = 0; col < 4; ++col) {
        unsigned p = voice_parameter(col);
        const struct mumble_param_info *info = &mumble_params[p];
        unsigned v = voice_track()->sound.value[p], d = mumble_cast[voice_cast].voice.value[p];
        decimal(value, (int)v, 1);
        strcpy(unit, p == MUMBLE_PITCH ? "HZ" : p == MUMBLE_GLIDE ? "MS" : "%");
        voice_meter(col, info->name, value, unit, voice_knob_colors[col], v - info->min, info->max - info->min,
                    d - info->min);
    }
}

static __attribute__((noinline)) void voice_draw(void)
{
    voice_header();
    voice_face();
    voice_roll();
    voice_choir_strip();
    voice_box();
    if (voice_help) voice_help_draw();
    else if (voice_system) voice_system_draw();
    else if (voice_mode == VOICE_LOOP) voice_lanes_draw();
    else if (voice_play) {
        voice_line_draw();
        voice_loop_bar();
    } else voice_lines_draw();
    voice_knobs();
    if (paint_rendering && paint_top + PAINT_ROWS <= 229) return;
    /* The hint line: EDIT being held (what it clears, and how long yet), a
     * notice of what a control did, or the mode's tip. */
    uint32_t held = frame_time - voice_edit_since, progress = 0;
    unsigned holding = voice_edit_down && !voice_edit_spent && held >= 150U;
    const char *keep = voice_edit_clears_line() ? "KEEP HOLDING: CLEAR THE LINE" : "KEEP HOLDING: CLEAR THE LOOP";
    const struct voice_hold *holds[2] = {&voice_lfo_hold, &voice_env_hold};
    static const char *const keeps[2] = {"KEEP HOLDING: ROLL EVERYONE", "KEEP HOLDING: EVERYONE'S OWN VOICE"};
    uint32_t hold_ms = VOICE_HOLD_MS;
    if (voice_system && voice_reset_armed == 2 && voice_edit_down)
        holding = 1, keep = "KEEP HOLDING: RESET EVERYTHING", hold_ms = VOICE_RESET_MS;
    for (unsigned k = 0; k < 2 && !holding; ++k)
        if (holds[k]->down && !holds[k]->spent && (k || voice_play) && frame_time - holds[k]->since >= 150U)
            holding = 1, held = frame_time - holds[k]->since, keep = keeps[k];
    const char *hint = holding ? keep
                     : notice ? notice : voice_tips[voice_mode];
    if (holding) progress = (held >= hold_ms ? hold_ms : held) * 228U / hold_ms;
    uint32_t hint_sig = signature(1 + holding + (progress << 1), hint, (unsigned)strlen(hint));
    if (hint_sig != cache->hint_cache) {
        cache->hint_cache = hint_sig;
        paint_fill(6, 230, 228, 10, BACK);
        text(6, 231, hint, 1, holding ? AMBER : notice ? TEXT : DIM, BACK);
        if (holding) paint_fill(6, 238, progress, 2, AMBER);
    }
}
