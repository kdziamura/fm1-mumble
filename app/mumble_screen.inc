/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* The VOICE page: the MUMBLE retro-game voice synthesizer.
 *
 * One line, sung by eight characters. GLO switches mode:
 *   TYPE  keys type the line; a vowel speaks the moment it is typed.
 *   SING  keys sing the line a syllable at a time at their pitch, held while
 *         held. The key that starts a syllable leads it: keys pressed while
 *         it is held sing it too (a chord); once it is let go the next key
 *         moves on, round the line. Each character sings one note at a time;
 *         the keys held are shared out among the choir (selected first).
 * The loop (always looping, BARS x BPM) plays a recorded sequence: REC
 * records what is sung, pass over pass. A note keeps its time, length,
 * pitch, singer and syllable slot, not the words: it sings the line as it is
 * now, so a new line changes what the loop sings. Each pass of the loop sings
 * from the top of the line.
 *
 *   PLAY loop on/off   REC record (from TYPE: to SING first)   GLO TYPE/SING
 *   SELECT character   SAVE choir   ARP / PRESET demo lines   HOME held: help
 *   TYPE  keys type at the cursor   K1 cursor   K2 the letter under it
 *         OCT- delete   OCT+ CAPS   FX ,  SCL .  ENV ?  LFO !
 *         EDIT a word back; held 1 s: clear the line, then the loop
 *   SING  ALGO / SEQ page   K1..K4 sound (EDIT held: back to the character's)
 *         OCT- OCT+ octave   FX from the top of the line
 *         EDIT undo a pass; held 1 s: clear the loop
 * Every button says what it did on the hint line.
 *
 * Included by app.c: uses paint.inc's helpers and draw cache. */
#include "mumble.h"

enum { VOICE_TRACE = 21, VOICE_ROWS = 3, VOICE_COLUMNS = 18 };
#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
enum { WHITE = 0xffff };
static const uint16_t voice_knob_colors[4] = {0x2b7b, 0x2d8d, 0xf7be, 0xf325};
/* AH EH EE AW OO UH, then IH UU ER (each near the vowel it sits between) */
static const uint16_t vowel_colors[MUMBLE_VOWELS] = {0xf325, 0x2d8d, 0x2b7b, 0xf7be, 0xb47f, 0xfe87,
                                                     0x2c5f, 0xd51f, 0xfc90};
static const char *const voice_page_names[MUMBLE_PAGES + 1] = {"VOICE", "FEEL", "FX", "SING", "LOOP"};
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
/* The hint line's tip in each mode (TYPE, SING), and HOME's help card. */
static const char *const voice_tips[2] = {"K1 CURSOR  K2 LETTER  HOLD HOME: HELP",
                                          "KEYS SING THE LINE  HOLD HOME: HELP"};
enum { VOICE_HELP_ROWS = 6, VOICE_HELP_COLUMNS = 37 };     /* (37 letters fill the box to its border) */
static const char *const voice_help_rows[2][VOICE_HELP_ROWS] = {
    {"KEYS TYPE AT THE CURSOR   GLO: SING",
     "K1 CURSOR  K2 LETTER  OCT- DELETE",
     "OCT+ CAPS  FX ,  SCL .  ENV ?  LFO !",
     "EDIT: A WORD BACK  HOLD: CLEAR",
     "ARP: DEMO  SAVE: CHOIR  PLAY: LOOP",
     "SELECT: WHO  REC: SING AND RECORD"},
    {"KEYS SING THE LINE  HELD: A CHORD",
     "PLAY: LOOP  REC: RECORD  GLO: TYPE",
     "OCT- OCT+ OCTAVE  FX: FROM THE TOP",
     "EDIT: UNDO A PASS  HOLD: CLEAR LOOP",
     "ALGO PAGE  K1-K4 SOUND  EDIT+K: RESET",
     "SELECT: WHO  SAVE: CHOIR"}};
/* K2's letters in TYPE mode, round and round. */
static const char voice_alphabet[] = "abcdefghijklmnopqrstuvwxyz ,.?!'-";
enum { VOICE_ALPHABET = sizeof voice_alphabet - 1, VOICE_HOLD_MS = 1000 };

/* A character: sound, choir, and place in the loop. */
enum { VOICE_PAGES = MUMBLE_PAGES + 1, LOOP_PAGE = MUMBLE_PAGES, VOICE_SEQ = 128, VOICE_PASSES = 8 };
struct voice_track {
    uint8_t choir, start, level;             /* start: 16th step in the loop; level: percent */
    struct mumble_voice sound;
};
static struct voice_track voice_tracks[MUMBLE_CAST];
/* The line, shared by everyone (empty: the demo line is sung). */
static char voice_text[MUMBLE_TEXT + 1];
static unsigned voice_length;
static unsigned voice_cast, voice_page, voice_caps, voice_demo, voice_ready;
/* The loop: BARS and BPM size it, it always loops. song is 1 when started, 2
 * once it was heard (the audio task adopts it a block later). */
static unsigned voice_loop = 1, voice_song, voice_song_dirty, voice_bars = 2, voice_bpm = 120;
static unsigned voice_fm1_playing, voice_lanes_version;
static struct mumble_status voice_now;
static struct { uint8_t vowel, level, track; int16_t cents; } voice_trace[VOICE_TRACE];
static uint32_t voice_trace_ms;
static uint32_t voice_keys_lit;
/* SING mode: held keys, octave shift, and the last key sung and its note. */
static unsigned voice_play, voice_sung = MUMBLE_LETTERS, voice_sung_note;
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
 * its singer and the pass that recorded it (EDIT undoes the last pass). */
static struct mumble_note voice_seq[VOICE_SEQ];
static uint8_t voice_seq_track[VOICE_SEQ], voice_seq_pass[VOICE_SEQ];
static unsigned voice_seq_count, voice_pass;
/* Recording: a singer being recorded, where its note began, when, and its
 * note and slot. A pass is open once a note of it is in; the loop's wrap, a
 * new recording, another character or an undo starts the next. */
static unsigned voice_recording, voice_pass_open, voice_click;
static uint32_t voice_rec_held, voice_rec_since[MUMBLE_CAST];
static uint16_t voice_rec_at[MUMBLE_CAST];
static uint8_t voice_rec_note[MUMBLE_CAST], voice_rec_slot[MUMBLE_CAST];
static int32_t voice_loop_last_ms = -1;
/* TYPE mode's cursor (0..voice_length: keys type before it, K2 changes the
 * letter under it) and when it last moved (its blink starts lit). */
static unsigned voice_cursor;
static uint32_t voice_cursor_ms;
/* HOME held: the help card. EDIT: when it went down, and whether it was spent
 * (held long enough to clear, or a knob turned with it): a tap acts on release. */
static unsigned voice_help, voice_edit_down, voice_edit_spent;
static uint32_t voice_edit_since;
static char voice_said[40];
static void voice_rec_stop(void);
static void voice_notice(const char *message);

static struct voice_track *voice_track(void) { return &voice_tracks[voice_cast]; }
static unsigned voice_in_choir(unsigned i) { return i == voice_cast || voice_tracks[i].choir; }
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
        parts[i] = (struct mumble_part){NULL, 0, &t->sound, (uint16_t)voice_step_ms(t->start), t->level,
                                        voice_seq + first, k - first, voice_line_sounds, voice_lyric_count};
    }
    voice_song_dirty = 0;
    mumble_song(parts, voice_loop_ms(), restart, phase_ms);
    if (restart || !voice_song) voice_song = 1;
}
/* Start the loop; with the FM1 sequencer playing, join it at its step. */
static void voice_start(void)
{
    voice_publish(1, voice_fm1_playing ? voice_step_ms(frame_views[0].play_step % voice_steps()) : 0);
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

static void voice_open(void)
{
    if (!voice_ready) {
        mumble_init();
        for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
            struct voice_track *t = &voice_tracks[i];
            t->sound = mumble_cast[i].voice;
            t->choir = 0;
            t->start = 0;
            t->level = 100;
        }
        voice_set_text("");
        voice_seq_count = voice_pass = 0;
        voice_cast = 0;
        if (frame_views[0].tempo) voice_bpm = frame_views[0].tempo;
        voice_ready = 1;
    }
    voice_page = 0;
    voice_play = 0;
    voice_held = 0;
    voice_song = 0;
    memset(voice_trace, 0, sizeof voice_trace);
    for (unsigned i = 0; i < VOICE_TRACE; ++i) voice_trace[i].vowel = MUMBLE_VOWELS;
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
    if (voice_page == LOOP_PAGE) {
        if (col == 0) t->start = (uint8_t)clamp((int)t->start + delta, 0, (int)voice_steps() - 1);
        else if (col == 1) t->level = (uint8_t)clamp((int)t->level + delta, 0, 100);
        else if (col == 2) {
            unsigned i = 0;
            while (i < 3 && voice_bar_choices[i] != voice_bars) ++i;
            voice_bars = voice_bar_choices[clamp((int)i + (delta > 0 ? 1 : -1), 0, 3)];
            for (unsigned k = 0; k < MUMBLE_CAST; ++k)
                if (voice_tracks[k].start >= voice_steps()) voice_tracks[k].start = (uint8_t)(voice_steps() - 1U);
        } else voice_bpm = (unsigned)clamp((int)voice_bpm + delta, 40, 240);
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
    if (voice_page == LOOP_PAGE) {
        static const char *const names[4] = {"START", "LEVEL", "BARS", "BPM"};
        if (col == 0) t->start = 0;
        else if (col == 1) t->level = 100;
        else if (col == 2) voice_bars = 2;
        else voice_bpm = 120;
        for (unsigned k = 0; k < MUMBLE_CAST; ++k)
            if (voice_tracks[k].start >= voice_steps()) voice_tracks[k].start = (uint8_t)(voice_steps() - 1U);
        voice_touch();
        strcpy(voice_said, names[col]);
    } else {
        unsigned p = voice_parameter(col);
        t->sound.value[p] = mumble_cast[voice_cast].voice.value[p];
        voice_touch();
        strcpy(voice_said, mumble_params[p].name);
    }
    strcat(voice_said, " AS IT WAS");
    voice_notice(voice_said);
}
static unsigned voice_edited(void)
{
    return memcmp(&voice_track()->sound, &mumble_cast[voice_cast].voice, sizeof(struct mumble_voice)) != 0;
}

/* The line the keys sing, planned again (and sung from its top) when it changes. */
static const char *voice_lyric_text(unsigned *n)
{
    const char *line = voice_length ? voice_text : voice_lines[voice_demo % VOICE_LINES];
    *n = voice_length ? voice_length : (unsigned)strlen(line);
    return line;
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

/* Recording: where the loop is now, from a track's start; -1 when it is not running. */
static int32_t voice_rec_position(unsigned track)
{
    int32_t at = mumble_song_position();
    if (at < 0) return -1;
    uint32_t loop = voice_loop_ms();
    return (int32_t)(((uint32_t)at + loop - voice_step_ms(voice_tracks[track].start) % loop) % loop);
}
static void voice_rec_press(unsigned who, unsigned slot, unsigned note)
{
    int32_t at = voice_rec_position(who);
    if (at < 0 || at > 0xFFFF) return;
    voice_rec_held |= 1U << who;
    voice_rec_at[who] = (uint16_t)at;
    voice_rec_since[who] = frame_time;
    voice_rec_slot[who] = (uint8_t)slot;
    voice_rec_note[who] = (uint8_t)note;
}
/* The passes in the sequence (EDIT's undo levels). */
static unsigned voice_passes(void)
{
    unsigned n = 0;
    for (unsigned id = 1; id <= voice_pass; ++id)
        for (unsigned k = 0; k < voice_seq_count; ++k)
            if (voice_seq_pass[k] == id) { ++n; break; }
    return n;
}
/* A new pass; past VOICE_PASSES the oldest two become one. */
static void voice_pass_begin(void)
{
    ++voice_pass;
    voice_pass_open = 1;
    if (voice_passes() < VOICE_PASSES) return;
    unsigned oldest = 255, next = 255;
    for (unsigned k = 0; k < voice_seq_count; ++k)
        if (voice_seq_pass[k] < oldest) oldest = voice_seq_pass[k];
    for (unsigned k = 0; k < voice_seq_count; ++k)
        if (voice_seq_pass[k] > oldest && voice_seq_pass[k] < next) next = voice_seq_pass[k];
    for (unsigned k = 0; k < voice_seq_count; ++k)
        if (voice_seq_pass[k] == oldest) voice_seq_pass[k] = (uint8_t)next;
}
/* A recorded note ended (let go, or its singer moved on): into the
 * sequence, grouped with its singer's, played from the next pass. */
static void voice_rec_release(unsigned who)
{
    if (!(voice_rec_held & (1U << who))) return;
    voice_rec_held &= ~(1U << who);
    if (voice_seq_count >= VOICE_SEQ) { voice_notice("THE LOOP IS FULL"); return; }
    if (voice_pass >= 250U) {                /* (ids run out: renumber 1..) */
        uint8_t map[256] = {0};
        unsigned next = 0;
        for (unsigned id = 1; id <= voice_pass; ++id)
            for (unsigned k = 0; k < voice_seq_count; ++k)
                if (voice_seq_pass[k] == id) { map[id] = (uint8_t)++next; break; }
        for (unsigned k = 0; k < voice_seq_count; ++k) voice_seq_pass[k] = map[voice_seq_pass[k]];
        voice_pass = next;
    }
    if (!voice_pass_open) voice_pass_begin();
    uint32_t len = frame_time - voice_rec_since[who], loop = voice_loop_ms();
    if (len < 30U) len = 30U;
    if (len >= loop) len = loop - 1U;
    if (len > 0xFFFFU) len = 0xFFFFU;
    unsigned at = 0;
    while (at < voice_seq_count && voice_seq_track[at] <= who) ++at;
    memmove(voice_seq + at + 1, voice_seq + at, (voice_seq_count - at) * sizeof voice_seq[0]);
    memmove(voice_seq_track + at + 1, voice_seq_track + at, voice_seq_count - at);
    memmove(voice_seq_pass + at + 1, voice_seq_pass + at, voice_seq_count - at);
    voice_seq[at] = (struct mumble_note){voice_rec_at[who], (uint16_t)len, voice_rec_slot[who], voice_rec_note[who],
                                         (uint8_t)voice_caps, voice_rec_bend[who]};
    voice_seq_track[at] = (uint8_t)who;
    voice_seq_pass[at] = (uint8_t)voice_pass;
    ++voice_seq_count;
    voice_touch();
}
/* EDIT in SING mode: the last pass out of the sequence. */
static unsigned voice_undo(void)
{
    unsigned last = 0, n = 0;
    for (unsigned k = 0; k < voice_seq_count; ++k)
        if (voice_seq_pass[k] > last) last = voice_seq_pass[k];
    if (!last) return 0;
    for (unsigned k = 0; k < voice_seq_count; ++k)
        if (voice_seq_pass[k] != last) {
            voice_seq[n] = voice_seq[k];
            voice_seq_track[n] = voice_seq_track[k];
            voice_seq_pass[n] = voice_seq_pass[k];
            ++n;
        }
    voice_seq_count = n;
    voice_pass_open = 0;
    voice_touch();
    return 1;
}
static void voice_seq_clear(void)
{
    voice_seq_count = voice_pass = 0;
    voice_pass_open = 0;
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
static void voice_note_on(unsigned who, unsigned key)
{
    static const struct mumble_sound hum = {MUMBLE_U, 0, MUMBLE_HUM, MUMBLE_VOWELS, 0, 0, 0, 0};
    const struct mumble_voice *v = &voice_tracks[who].sound;
    int pitch = clamp(53 + (int)key + 12 * (voice_octave + mumble_register(v)), 0, 127);   /* lowest key is F3 */
    /* Straight on from its last note (within 60 ms): slide from it. */
    int bend = 0;
    if (voice_sing_pitch[who] && frame_time - voice_sing_end[who] <= 60U)
        bend = clamp((int)voice_sing_pitch[who] - pitch, -24, 24);
    struct mumble_singer one = {v, (uint8_t)who, (uint8_t)pitch, (int8_t)bend};
    voice_sing_pitch[who] = (uint8_t)pitch;
    voice_rec_bend[who] = (int8_t)bend;
    unsigned slot = voice_key_slot[key];
    mumble_press_sound(key, voice_lyric_count ? &voice_line_sounds[slot % voice_lyric_count] : &hum, &one, 1, voice_caps);
    voice_sings[who] = (uint8_t)(key + 1U);
    voice_sing_order[who] = ++voice_order;
    if (voice_recording) voice_rec_press(who, slot, (unsigned)pitch);
    voice_sung = key;
    voice_sung_note = (unsigned)pitch;
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
    voice_recording = 0;
    if (voice_click) { voice_click = 0; mumble_metronome(0, 0); }
}
/* REC: record into the loop (in SING mode, starting the loop if need be), or stop. */
static void voice_rec_toggle(void)
{
    if (voice_recording) { voice_rec_stop(); return; }
    voice_recording = 1;
    voice_pass_open = 0;
    unsigned restart = !voice_song || (voice_song == 2 && mumble_song_position() < 0);
    if (restart) voice_start();
    voice_notice("RECORDING: SING THE KEYS");
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
static void voice_sing(const struct fm1_controls *c, uint32_t down)
{
    uint32_t up = voice_held & ~c->keys;
    voice_held &= c->keys;
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

static void voice_notice(const char *message)
{
    notice = message;
    notice_until = frame_time + 1500U;
}
/* A notice made of parts (held in voice_said, so it lasts its 1.5 s). */
static void voice_say(const char *a, const char *b, const char *c)
{
    const char *parts[3] = {a, b, c};
    unsigned n = 0;
    for (unsigned k = 0; k < 3; ++k)
        for (const char *p = parts[k]; p && *p && n + 1U < sizeof voice_said; ++p) voice_said[n++] = *p;
    voice_said[n] = 0;
    voice_notice(voice_said);
}
static void voice_say_number(const char *a, int value, const char *c)
{
    char number[12];
    decimal(number, value, 1);
    voice_say(a, number, c);
}
/* GLO: TYPE <-> SING (recording stops in TYPE). */
static void voice_mode_next(void)
{
    voice_release_all();
    voice_rec_stop();
    voice_play ^= 1U;
}
static void voice_demo_line(unsigned demo)
{
    voice_demo = demo;
    voice_set_text(voice_lines[voice_demo]);
    char of[12] = " OF ", count[12];
    decimal(count, VOICE_LINES, 1);
    strcat(of, count);
    voice_say_number("DEMO LINE ", (int)voice_demo + 1, of);
}
/* EDIT let go before it was held 1 s, with no knob turned. */
static void voice_edit_tap(void)
{
    if (voice_play) voice_notice(voice_undo() ? (voice_seq_count ? "LAST PASS UNDONE" : "THE LOOP IS EMPTY") : "NOTHING TO UNDO");
    else if (!voice_cut_word()) voice_notice(voice_length ? "HOLD EDIT TO CLEAR THE LINE" : "HOLD EDIT TO CLEAR THE LOOP");
}
/* EDIT held 1 s: in TYPE the line, or on an empty line the loop; in SING the loop. */
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
    unsigned edit = (c->buttons & BUTTON(EDIT)) != 0;
    if (pressed & BUTTON(EDIT)) { voice_edit_since = frame_time; voice_edit_spent = 0; }
    for (unsigned i = 0; i < 7; ++i)
        if (edit && c->encoders[i]) voice_edit_spent = 1;
    if (c->encoders[0]) {
        voice_release_all();
        voice_pass_open = 0;
        voice_cast = (unsigned)((int)voice_cast + c->encoders[0] % MUMBLE_CAST + MUMBLE_CAST) % MUMBLE_CAST;
        const char *hello = voice_greetings[voice_cast];
        mumble_aside(voice_cast, hello, (unsigned)strlen(hello), &voice_track()->sound);
        ++voice_lanes_version;
    }
    unsigned paged = voice_page;
    if (c->encoders[1]) voice_page = (unsigned)((int)voice_page + c->encoders[1] % VOICE_PAGES + VOICE_PAGES) % VOICE_PAGES;
    if (pressed & BUTTON(SEQ)) voice_page = (voice_page + 1U) % VOICE_PAGES;
    if (voice_page != paged || (pressed & BUTTON(SEQ)))
        voice_say(voice_page_names[voice_page], voice_play ? " PAGE" : " PAGE: ITS KNOBS IN SING", NULL);
    for (unsigned i = 0; i < 4; ++i) {
        int turned = c->encoders[2 + i];
        if (!turned) continue;
        if (!voice_play) {                       /* TYPE: K1 the cursor, K2 its letter */
            if (i == 0) voice_move_cursor(fm_encoder_delta(2, turned));
            else if (i == 1) voice_letter(turned);
        } else if (edit) voice_reset(i);
        else voice_edit(i, fm_encoder_delta(2 + i, turned));
    }
    if (c->encoders[6])
        voice_demo_line((unsigned)((int)voice_demo + c->encoders[6] % (int)VOICE_LINES + (int)VOICE_LINES) % VOICE_LINES);
    if (pressed & BUTTON(GLO)) {
        voice_mode_next();
        voice_notice(voice_play ? "SING: KEYS SING THE LINE" : "TYPE: KEYS TYPE THE LINE");
    }
    if ((pressed & BUTTON(REC)) && !voice_play) voice_mode_next();       /* REC sings: to SING mode */
    if ((pressed & BUTTON(FX)) && voice_play) { voice_lyric_rewind(); voice_notice("FROM THE TOP OF THE LINE"); }
    if (pressed & BUTTON(SAVE)) {
        struct voice_track *t = voice_track();
        t->choir ^= 1U;
        voice_say(mumble_cast[voice_cast].name, t->choir ? " JOINED THE CHOIR" : " LEFT THE CHOIR", NULL);
        ++voice_lanes_version;
    }
    if (pressed & BUTTON(REC)) {
        voice_rec_toggle();
        voice_notice(voice_recording ? "RECORDING: SING INTO THE LOOP" : "RECORDING STOPPED");
    }
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
    if (pressed & BUTTON(ARP)) voice_demo_line((voice_demo + 1U) % VOICE_LINES);
    static const uint8_t marks[4][2] = {{FX, ','}, {SCL, '.'}, {ENV, '?'}, {LFO, '!'}};
    static const char *const mark_names[4] = {"FX", "SCL", "ENV", "LFO"};
    for (unsigned i = 0; i < 4; ++i)
        if (pressed & BUTTON(marks[i][0])) {
            char mark[2] = {(char)marks[i][1], 0};
            if (!voice_play) voice_type(mark[0]);
            else if (i) voice_say(mark_names[i], " TYPES ", mark), strcat(voice_said, " IN TYPE MODE");
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

/* Note-key LEDs mark the vowels; held keys invert theirs. */
static uint32_t voice_led_bits(void)
{
    uint32_t lit = (1U << 0) | (1U << 4) | (1U << 8) | (1U << 14) | (1U << 20) | (1U << 24);
    return lit ^ (voice_keys_lit & ((1U << 27) - 1U));
}
static uint32_t voice_button_bits(void)
{
    uint32_t octave = voice_play ? ((voice_octave > 0) << OCT_UP) | ((voice_octave < 0) << OCT_DOWN)
                                 : voice_caps << OCT_UP;
    return ((voice_song != 0) << PLAY) | octave | (voice_play << GLO) | ((uint32_t)voice_track()->choir << SAVE) |
           (voice_recording << REC) | (1U << ARP);
}

/* Control-side snapshot, taken once per frame before marking damage: the
 * loop, FM1 sync, the line, edits to republish, and the pitch trace. */
static void voice_frame(uint32_t now)
{
    mumble_status(&voice_now);
    /* The audio task adopts a song a block later: only a song that was
     * heard playing can be over. */
    if (voice_song == 1 && voice_now.song_ms >= 0) voice_song = 2;
    else if (voice_song == 2 && voice_now.song_ms < 0) voice_song = 0;
    /* Follow the FM1 sequencer: its start relocks the loop, its tempo sizes it. */
    unsigned playing = frame_views[0].playing != 0;
    static unsigned last_tempo;
    if (playing && !voice_fm1_playing && voice_song) { voice_fm1_playing = 1; voice_publish(1, 0); }
    voice_fm1_playing = playing;
    if (voice_tempo() != last_tempo) {
        last_tempo = voice_tempo();
        voice_touch();
    }
    voice_lyric_sync();
    if (voice_song_dirty && voice_song) voice_publish(0, 0);
    /* Each pass of the loop sings from the top of the line, and is a pass of
     * the recording. */
    if (voice_song && voice_now.song_ms >= 0) {
        if (voice_now.song_ms < voice_loop_last_ms) { voice_lyric_rewind(); voice_pass_open = 0; }
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
 * The cast in OP-1 style line art (mumble_sprites.inc): white strokes and
 * encoder-colour accents on black. 24x24 portraits are drawn at 3x on whole pixels;
 * 16x16 icons fill the choir strip. Mouths, blinks and the shout mark are
 * pixel overlays on a working copy of the portrait.
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

/* Where each face moves: the mouth centre and row with the colour that
 * hides the drawn mouth (skin, beard) and the lip colour of an open one, and
 * the eyes a blink closes (x, y, w, h in portrait pixels): the lid colour
 * fills them, then a lash line in `lash` along their bottom. */
struct voice_rig { uint8_t mouth_x, mouth_y; char fill, lip, lid, lash; uint8_t eyes[2][4]; };
static const struct voice_rig voice_rigs[MUMBLE_CAST] = {
    {12, 13, '.', 'w', '.', 'w', {{8, 9, 2, 2}, {14, 9, 2, 2}}},   /* HERO */
    {12, 13, '.', 'w', '.', 'w', {{8, 9, 2, 2}, {14, 9, 2, 2}}},   /* GUARD */
    {12, 13, '.', 'w', '.', 'w', {{0}}},                           /* SISTER: eyes closed in prayer */
    {12, 13, '.', 'w', '.', 'w', {{8, 9, 2, 2}, {14, 9, 2, 2}}},   /* KID */
    {12, 13, '.', 'o', '.', 'w', {{14, 9, 2, 2}}},                 /* PIRATE: one eye, one patch */
    {12, 15, '.', 'w', '.', 'w', {{8, 11, 2, 2}, {14, 11, 2, 2}}}, /* ELDER: under bushy brows */
    {12, 22, '.', 'g', '.', 'g', {{9, 7, 2, 2}, {13, 7, 2, 2}}},   /* BOT: the mouth is the chest panel */
    {12, 11, '.', 'w', '.', 'w', {{8, 7, 2, 3}, {14, 7, 2, 3}}},   /* GHOST */
};
enum { CAST_KID = 3, CAST_ELDER = 5, CAST_BOT = 6, CAST_GHOST = 7 };
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

struct pose { unsigned cast, open, vowel, blink, shout, tick; };
static void voice_pose(const struct pose *p)
{
    memcpy(voice_grid, voice_portraits[p->cast], sizeof voice_grid);
    const struct voice_rig *rig = &voice_rigs[p->cast];
    if (p->blink)
        for (unsigned e = 0; e < 2 && rig->eyes[e][2]; ++e) {
            const uint8_t *eye = rig->eyes[e];
            grid_rect(eye[0], eye[1], eye[2], eye[3], rig->lid);
            grid_rect(eye[0], eye[1] + eye[3] - 1, eye[2], 1, rig->lash);
        }
    int y = rig->mouth_y;
    if (p->cast == CAST_BOT) {
        /* An equaliser on its chest panel for a mouth; the antenna lamp blinks. */
        for (int b = 0; b < 4; ++b) {
            int h = p->open ? 1 + (int)((p->open * (2U + (unsigned)(b * 3 + (int)p->tick) % 3U)) / 4U) : 1;
            if (h > 3) h = 3;
            grid_rect(8 + b * 2 + (b >= 2), y + 1 - h, 1, h, p->open ? rig->lip : 'm');
        }
        if (p->tick & 8U)
            for (int j = 0; j < 2; ++j)
                for (int i = 9; i < 15; ++i)
                    if (voice_grid[j][i] == 'o') voice_grid[j][i] = 'm';
    } else if (p->open) {
        /* The drawn mouth gives way to an open one: a stroke for narrow
         * vowels, a ring for round ones. */
        unsigned v = p->vowel < MUMBLE_VOWELS ? p->vowel : MUMBLE_U;
        int w = vowel_mouth[v][0], h = (int)((vowel_mouth[v][1] * p->open + 3U) / 4U);
        int x = rig->mouth_x - (w + 1) / 2;
        grid_rect(rig->mouth_x - 2, y, 5, 1, rig->fill);
        grid_rect(x, y, w, h, rig->lip);
        if (h >= 3 && w >= 3) grid_rect(x + 1, y + 1, w - 2, h - 2, '.');
    }
    if (p->cast == CAST_ELDER && (p->tick & 16U)) grid_set(14, 4, 'w');          /* the star twinkles */
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
                   (voice_play << 8) | ((voice_song != 0) << 9) | ((uint32_t)voice_track()->choir << 10);
    if (cache->voice_cache[0] == sig) return;
    cache->voice_cache[0] = sig;
    paint_fill(0, 0, 240, 28, BACK);
    /* The name and the mode, then the pages as tabs: the knobs' page lit (in
     * SING, where the knobs play it), the others dim. */
    text(8, 4, "MUMBLE", 1, TEXT, BACK);
    text(50, 4, voice_play ? "SING" : "TYPE", 1, voice_play ? GREEN : TEXT, BACK);
    if (voice_song) text(80, 4, "LOOP", 1, ACCENT, BACK);
    if (voice_caps) text(110, 4, "CAPS", 1, AMBER, BACK);
    for (unsigned k = 0, x = 8; k < VOICE_PAGES; ++k) {
        unsigned n = (unsigned)strlen(voice_page_names[k]);
        unsigned on = k == voice_page;
        text(x, 16, voice_page_names[k], 1, on ? (voice_play ? TEXT : DIM) : PANEL, BACK);
        if (on) paint_fill(x, 24, n * 6U - 1U, 2, voice_play ? ACCENT : DIM);
        x += (n + 1U) * 6U;
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
    p.cast = who;
    p.vowel = vowel;
    p.open = vowel < MUMBLE_VOWELS ? (level > 180U ? 4U : level / 45U) : 0U;
    p.blink = ((frame_time + who * 977U) % 3700U) < 140U;
    p.shout = voice_caps;
    p.tick = frame_time / 60U;
    /* Motion snaps to icon pixels: a hop on every syllable, the kid
     * bounces; only the ghost floats smoothly. */
    int lift = p.open ? SPRITE_SCALE : 0;
    if (who == CAST_KID && ((frame_time / 450U) & 1U)) lift = SPRITE_SCALE;
    if (who == CAST_GHOST) lift = 1 + voice_wave(2000, 1, 0);
    if (who != CAST_BOT && who != CAST_ELDER) p.tick = 0;
    /* What of the tick shows: the lamp's and the star's bits, and the bot's
     * equaliser while it talks (its bars step with tick % 3). */
    unsigned tick_shown = (p.tick & 24U) | (who == CAST_BOT && p.open ? p.tick % 3U : 0U);
    struct { int lift; unsigned open, vowel, blink, shout, cast, tick; } sig_in = {
        lift, p.open, vowel, p.blink, p.shout, who, tick_shown};
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
    uint32_t sig = signature(0xC402, state, sizeof state);
    if (cache->voice_cache[7] == sig) return;
    cache->voice_cache[7] = sig;
    voice_clip(0, STRIP_TOP, 240, STRIP_BOTTOM);
    vfill(0, STRIP_TOP, 240, STRIP_BOTTOM - STRIP_TOP, BACK);
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        int x = 14 + (int)i * 28, hop = voice_now.track_level[i] > 40U;
        for (int row = 0; row < ICON; ++row) memcpy(voice_grid[row], voice_icons[i][row], ICON);
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
            text(10, y + 3, "* TYPE A LINE (ARP: A DEMO ONE)", 1, DIM, BACK);
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
    if (singers > 1) {
        char choir[16] = "CHOIR ";
        decimal(value, (int)singers, 1);
        strcat(choir, value);
        text(100, 172, choir, 1, ACCENT, BACK);
    }
    if (voice_recording || notes) {
        char take[16];
        strcpy(take, voice_recording ? "REC " : "LOOP ");
        decimal(value, (int)notes, 1);
        strcat(take, value);
        if (voice_recording) paint_fill(172, 172, 7, 7, AMBER);
        text(voice_recording ? 182 : 172, 172, take, 1, voice_recording ? AMBER : DIM, BACK);
    }
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
/* SING mode: the line, the syllable a key would sing now marked, those sung
 * dimmed, and where the queue is. */
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
        unsigned start = from > 6U ? from - 6U : 0U;
        if (n > VOICE_COLUMNS && start + VOICE_COLUMNS > n) start = n - VOICE_COLUMNS;
        for (unsigned c = 0; c < VOICE_COLUMNS && start + c < n; ++c) {
            unsigned i = start + c;
            char glyph[2] = {line[i], 0};
            text(10U + c * 12U, 130, glyph, 2, i < from ? DIM : i < to ? mark : TEXT, BACK);
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

/* LOOP page: eight lanes over one loop, OP-1 tape style. Each lane shows
 * its character's notes in the sequence from its offset, each start marked
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
                   (voice_cast << 4) ^ ((uint32_t)voice_song << 8) ^ ((uint32_t)voice_loop << 10);
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
        unsigned on = voice_in_choir(i) || voice_notes_of(i);
        uint16_t color = on ? mumble_cast[i].color : dimmer(mumble_cast[i].color);
        if (i == voice_cast) vfill(6, y, 2, lane_h - 1, WHITE);
        vfill(12, y + 1, 7, lane_h - 3, color);                  /* the character's colour chip */
        vfill(x0, y + 3, x1 - x0, 1, PANEL);
        uint32_t offset = voice_step_ms(t->start);
        vfill(x0 + (int)(offset * (uint32_t)(x1 - x0) / loop), y, 1, lane_h - 1, color);
        for (unsigned k = 0; k < voice_seq_count; ++k) {
            const struct mumble_note *q = &voice_seq[k];
            if (voice_seq_track[k] != i || q->ms >= loop) continue;   /* (past the end of a shorter loop: not played) */
            uint32_t at = offset + q->ms;
            if (at >= loop) at -= loop;
            int x = x0 + (int)(at * (uint32_t)(x1 - x0) / loop);
            int w = (int)(q->len_ms * (uint32_t)(x1 - x0) / loop);
            if (w < 2) w = 2;
            if (x + w > x1) w = x1 - x;
            vfill(x, y + 1, w, lane_h - 3, color);
            vfill(x, y, 1, lane_h - 1, WHITE);
        }
    }
    if (head >= 0) vfill(head, lane_top - 1, 1, 8 * lane_h + 1, WHITE);
}

/* HOME held: what each control does in this mode, over the dialogue box. */
static void voice_help_draw(void)
{
    if (paint_rendering && (paint_top >= BOX_BOTTOM - 2 || paint_top + PAINT_ROWS <= BOX_TOP + 2)) return;
    uint32_t sig = 0x4E1F0000U | voice_play;
    /* The rows under it redraw when it goes. */
    cache->voice_cache[4] = cache->voice_cache[5] = cache->voice_cache[8] = 0xfffffffeU;
    if (cache->voice_cache[3] == sig) return;
    cache->voice_cache[3] = sig;
    paint_fill(4, BOX_TOP + 2, 232, BOX_BOTTOM - BOX_TOP - 4, BACK);
    for (unsigned r = 0; r < VOICE_HELP_ROWS; ++r)
        text(10, 127U + r * 10U, voice_help_rows[voice_play][r], 1, r ? TEXT : ACCENT, BACK);
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
    if (voice_page == LOOP_PAGE) {
        const struct voice_track *t = voice_track();
        /* START as bar.step, like the FM1 step pages. */
        char step[12];
        decimal(value, t->start / 16U + 1U, 1);
        decimal(step, t->start % 16U + 1U, 2);
        strcat(value, ".");
        strcat(value, step);
        voice_meter(0, "START", value, "", voice_knob_colors[0], t->start, voice_steps() - 1U, 0);
        decimal(value, t->level, 1);
        voice_meter(1, "LEVEL", value, "%", voice_knob_colors[1], t->level, 100, 100);
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
    else if (voice_play && voice_page == LOOP_PAGE) voice_lanes_draw();
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
    const char *hint = holding ? (voice_edit_clears_line() ? "KEEP HOLDING: CLEAR THE LINE" : "KEEP HOLDING: CLEAR THE LOOP")
                     : notice ? notice : voice_tips[voice_play];
    if (holding) progress = (held >= VOICE_HOLD_MS ? VOICE_HOLD_MS : held) * 228U / VOICE_HOLD_MS;
    uint32_t hint_sig = signature(1 + holding + (progress << 1), hint, (unsigned)strlen(hint));
    if (hint_sig != cache->hint_cache) {
        cache->hint_cache = hint_sig;
        paint_fill(6, 230, 228, 10, BACK);
        text(6, 231, hint, 1, holding ? AMBER : notice ? TEXT : DIM, BACK);
        if (holding) paint_fill(6, 238, progress, 2, AMBER);
    }
}
