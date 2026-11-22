/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* libFuzzer harness and property checks for the MUMBLE firmware: app/app.c,
 * app/paint.inc and the engine (app/engine.c) exactly as the device builds
 * them, on a stand-in for fm1-base's platform. Each input is a performance on
 * the panel (buttons, keys, encoders, MASTER, waits, a panic), played from a
 * fresh boot: the app's whole state is put back between inputs, so every
 * failure replays from its input alone.
 *
 * Besides ASan/UBSan findings and hangs, it fails when:
 *   P1  a blit leaves the 240x240 screen, or its pixels are not inside one of
 *       the painter's two band buffers;
 *   P2  a band buffer changes while its blit is still going out (lcd_blit is a
 *       DMA the next LCD call waits for: the device would show torn bands);
 *   P3  the screen after the frames' damage-only redraws differs from a full
 *       repaint of the same state (a missed damage mark or a stale cache);
 *   P4  the LEDs name a button or key the FM-1 does not have;
 *   P5  the page is no longer MUMBLE's (HOME must do nothing here);
 *   P6  audio is still sounding 3 seconds after app_panic (an update would
 *       rewrite the flash under a voice);
 *   P7  a draw pass changes what it draws (the same state, two different
 *       screens) or the same input plays differently twice.
 * And on the page's state, every frame:
 *   S1  every field is in its range and agrees with the others and with the
 *       lights (the line printable text of its length, the sequence grouped
 *       by singer with its notes and passes in range, one key per singer and
 *       none held unsung while a choir member is free, recording only in
 *       SING or LOOP, the metronome exactly while recording, sounds in
 *       range ...);
 *   S2  the frame did what a model of the controls says: GLO steps TYPE,
 *       SING, LOOP, REC records (from TYPE: to SING first), PLAY starts or
 *       stops the loop, SELECT and ALGO move by their detents, in SING a knob
 *       moves its parameter of the selected sound by the accelerated amount
 *       (with EDIT held, back to the character's), in LOOP the knobs move the
 *       start, level, bars and BPM (EDIT held: back), in TYPE K1 moves the cursor and
 *       K2 changes only the letter under it; EDIT let go unspent undoes
 *       exactly the last pass (SING) or cuts the word before the cursor
 *       (TYPE), held 1 s it clears the loop (SING) or the line, then the loop
 *       (TYPE); FX rewinds the line (SING); with nothing pressed or turned no
 *       setting changes, TYPE types exactly the keys' letters at the cursor,
 *       SING holds exactly the
 *       keys still down and the new ones, the last key pressed is sung at
 *       once, the queue moves on only with a key pressed after the leading one
 *       was let go and goes back to the top as the loop wraps, and the
 *       sequence only gains notes, each from a singer being recorded, first
 *       with the slot it was singing.
 * And once, at start-up, every button combination survives the panel's
 * button-order mapping both ways.
 *
 *   tests/fuzz.sh [libFuzzer options]     e.g. -max_total_time=600 -jobs=8
 */
#define PLAT_HOST 1
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------- the stand-in --- */
static uint16_t fb[240 * 240];
static const uint16_t *in_flight;        /* the last blit's pixels, while "the DMA" sends them */
static uint32_t in_flight_n, in_flight_sum;
static uint32_t now_ms, held_buttons, held_keys, lit_buttons, lit_keys;
static int32_t turned[7];
static const char *phase = "boot";

static void fail(const char *what)
{
    fprintf(stderr, "MUMBLE-FM1 FUZZ FAILURE (%s): %s\n", phase, what);
    abort();
}
static uint32_t sum(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
    return h;
}
/* The device's next LCD call first waits for the last blit: it must still be
 * what was sent. */
static void lcd_wait(void)
{
    if (in_flight && sum(in_flight, in_flight_n * 2u) != in_flight_sum) fail("P2 band buffer changed while sent");
    in_flight = NULL;
}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    lcd_wait();
    if (x + w > 240u || y + h > 240u) fail("P1 fill off screen");
    uint16_t sc = (uint16_t)((c >> 8) | (c << 8));
    for (uint32_t j = y; j < y + h; ++j)
        for (uint32_t i = x; i < x + w; ++i) fb[j * 240u + i] = sc;
}
static int inside_band_buffers(const uint16_t *px, uint32_t n);
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    lcd_wait();
    if (!w || !h || x >= 240u || y >= 240u || w > 240u - x || h > 240u - y) fail("P1 blit off screen");
    if (!inside_band_buffers(px, w * h)) fail("P1 blit pixels outside the band buffers");
    for (uint32_t j = 0; j < h; ++j)
        for (uint32_t i = 0; i < w; ++i) fb[(y + j) * 240u + x + i] = px[j * w + i];
    in_flight = px, in_flight_n = w * h, in_flight_sum = sum(px, w * h * 2u);
}

/* --------------------------------------------- the app, as built --- */
/* Its state (and the engine's, fuzz_engine.c) in its own sections, kept as it
 * was before the first boot and put back for every input. ASan does not
 * guard globals in sections of their own, so FUZZ_ASAN_GLOBALS builds without
 * them: every global guarded, nothing put back (one input a run: tests/fuzz.sh
 * replays the corpus that way). */
#include "app.h"
#ifndef FUZZ_ASAN_GLOBALS
#pragma clang section bss = "mumble_bss" data = "mumble_data"
#endif
#include "../app/app.c"
#ifndef FUZZ_ASAN_GLOBALS
#pragma clang section bss = "" data = ""
extern char __start_mumble_bss[], __stop_mumble_bss[], __start_mumble_data[], __stop_mumble_data[];
static char *pristine_bss, *pristine_data;
__attribute__((no_sanitize("address"))) static void state_copy(char *to, const char *from, size_t n)
{
    for (size_t i = 0; i < n; ++i) to[i] = from[i];
}
static size_t bss_size(void) { return (size_t)(__stop_mumble_bss - __start_mumble_bss); }
static size_t data_size(void) { return (size_t)(__stop_mumble_data - __start_mumble_data); }
static int in_state(const void *p)
{
    const char *c = p;
    return (c >= __start_mumble_bss && c < __stop_mumble_bss) || (c >= __start_mumble_data && c < __stop_mumble_data);
}
#endif

#ifdef FUZZ_ASAN_GLOBALS
#define FUZZ_ONE_INPUT 1
#else
#define FUZZ_ONE_INPUT 0
#endif
static int inside_band_buffers(const uint16_t *px, uint32_t n)
{
    for (unsigned b = 0; b < 2; ++b)
        if (px >= paint_buffers[b] && px + n <= paint_buffers[b] + SCREEN_W * PAINT_ROWS) return 1;
    return 0;
}

uint32_t plat_ms(void) { return now_ms; }
uint32_t plat_buttons(void) { return held_buttons; }
uint32_t plat_keys(void) { return held_keys; }
int32_t plat_enc(int role)
{
    int32_t v = turned[role];
    turned[role] = 0;
    return v;
}
void plat_leds(uint32_t b, uint32_t k)
{
    if (b & ~((1u << NB) - 1u)) fail("P4 an LED for a button the FM-1 does not have");
    if (k & ~((1u << NKEYS) - 1u)) fail("P4 an LED for a key the FM-1 does not have");
    lit_buttons = b, lit_keys = k;
}

/* ------------------------------------------------------- the run --- */
static uint32_t master_q12 = 4096, audio_debt, audio_sum;
static int32_t audio[2 * APP_BLOCK];
static int32_t audio_peak;

static void render_block(void)
{
    app_render(audio, APP_BLOCK, master_q12);
    for (unsigned i = 0; i < 2 * APP_BLOCK; ++i) {
        int32_t v = audio[i];
        if (v < -8388608 || v > 8388607) fail("audio outside 24 bits");
        if (v < 0) v = -v;
        if (v > audio_peak) audio_peak = v;
    }
    audio_sum = sum(audio, sizeof audio) ^ (audio_sum * 31u);
}
/* ------------------------------------------------- the state --- */
/* S1: after every frame, every field of the page's state is in its range and
 * the fields agree with each other and with the lights. */
static void check_sound(const struct mumble_voice *v, const char *what)
{
    for (unsigned p = 0; p < MUMBLE_PARAMS; ++p)
        if (v->value[p] < mumble_params[p].min || v->value[p] > mumble_params[p].max) fail(what);
}
static void check_state(void)
{
    if (!voice_ready) fail("S1 the page is not open");
    if (voice_cast >= MUMBLE_CAST) fail("S1 cast out of range");
    if (voice_page >= VOICE_PAGES) fail("S1 page out of range");
    if (voice_mode >= VOICE_MODES || voice_play != (voice_mode != VOICE_TYPE)) fail("S1 the mode out of range");
    if (voice_caps > 1 || voice_play > 1 || voice_song_dirty > 1 || voice_recording > 1 || voice_loop != 1)
        fail("S1 a flag is not 0 or 1 (or the loop not looping)");
    if (voice_song > 2) fail("S1 song state out of range");
    if (voice_demo >= VOICE_LINES) fail("S1 demo line out of range");
    if (voice_bars != 1 && voice_bars != 2 && voice_bars != 4 && voice_bars != 8) fail("S1 bars not 1, 2, 4 or 8");
    if (voice_bpm < 40 || voice_bpm > 240) fail("S1 BPM out of 40..240");
    if (voice_octave < -2 || voice_octave > 2) fail("S1 octave out of -2..2");
    if (voice_length > MUMBLE_TEXT || voice_text[voice_length]) fail("S1 the line is not text of its length");
    if (voice_cursor > voice_length) fail("S1 the cursor past the line's end");
    for (unsigned i = 0; i < voice_length; ++i)
        if ((unsigned char)voice_text[i] < 32u || (unsigned char)voice_text[i] > 126u) fail("S1 the line is not printable");
    if (voice_held & ~held_keys) fail("S1 a key held whose key is up");
    if (!voice_play && voice_held) fail("S1 keys held outside SING");
    uint32_t singing = 0, sung_keys = 0, choir_free = 0;
    for (unsigned who = 0; who < MUMBLE_CAST; ++who) {
        if (voice_sings[who] > MUMBLE_LETTERS) fail("S1 a character sings a key that does not exist");
        if (!voice_sings[who]) { choir_free |= voice_in_choir(who); continue; }
        uint32_t key = 1u << (voice_sings[who] - 1u);
        if (!voice_play || !(voice_held & key)) fail("S1 a character sings a key not held in SING");
        if (sung_keys & key) fail("S1 two characters sing one key");
        sung_keys |= key;
        singing |= 1u << who;
    }
    if (choir_free && (voice_held & ~sung_keys)) fail("S1 a key held unsung while a choir member is free");
    if (voice_rec_held & ~singing) fail("S1 a character recorded while it sings nothing");
    if (voice_recording && !voice_play) fail("S1 recording outside SING");
    if (!voice_recording && voice_rec_held) fail("S1 singers recorded while not recording");
    if (voice_click != (voice_recording ? voice_bars * 4u : 0u)) fail("S1 the metronome not on exactly while recording");
    if (voice_lyric_count > MUMBLE_SYLLABLES || voice_lyric_at >= (voice_lyric_count ? voice_lyric_count : 1u) ||
        voice_lyric_started > 1)
        fail("S1 the queue's syllable out of its line");
    if (voice_lyric_lead != MUMBLE_LETTERS &&
        (voice_lyric_lead > MUMBLE_LETTERS || !voice_play || !(voice_held & (1u << voice_lyric_lead))))
        fail("S1 the leading key is not a key held in SING");
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        const struct voice_track *t = &voice_tracks[i];
        if (t->choir > 1) fail("S1 choir not 0 or 1");
        if (t->start >= voice_steps()) fail("S1 a character starts after the loop's end");
        if (t->level > 100) fail("S1 a level over 100");
        check_sound(&t->sound, "S1 a sound parameter out of its range");
    }
    if (voice_seq_count > VOICE_SEQ) fail("S1 the sequence longer than it holds");
    for (unsigned k = 0; k < voice_seq_count; ++k) {
        const struct mumble_note *q = &voice_seq[k];
        if (voice_seq_track[k] >= MUMBLE_CAST || (k && voice_seq_track[k] < voice_seq_track[k - 1u]))
            fail("S1 the sequence not grouped by singer");
        if (q->note > 127u || q->len_ms < 30u || q->shout > 1u || q->slot >= MUMBLE_SYLLABLES || q->bend < -24 || q->bend > 24)
            fail("S1 a note of the sequence out of range");
        if (!voice_seq_pass[k] || voice_seq_pass[k] > voice_pass) fail("S1 a note's pass out of range");
    }
    if (voice_passes() > VOICE_PASSES) fail("S1 more passes than EDIT can undo");
    if (voice_now.vowel > MUMBLE_VOWELS || voice_now.track >= MUMBLE_CAST) fail("S1 the engine's status out of range");
    /* The lights tell the state. */
    if (!!(lit_buttons & (1u << B_PLAY)) != (voice_song != 0)) fail("S1 PLAY's light disagrees with the loop");
    if (!!(lit_buttons & (1u << B_REC)) != voice_recording) fail("S1 REC's light disagrees with recording");
    if (!!(lit_buttons & (1u << B_SAVE)) != voice_track()->choir) fail("S1 SAVE's light disagrees with the choir");
    if (!!(lit_buttons & (1u << B_GLO)) != voice_play) fail("S1 GLO's light disagrees with the mode");
}

/* S2: a model of each frame. Before it, what the frame will be given; after
 * it, what must have become of the state. */
static struct {
    struct voice_track tracks[MUMBLE_CAST];
    char text[MUMBLE_TEXT + 1];
    unsigned length, cursor, cast, page, caps, mode, play, bars, bpm, demo, song, recording;
    unsigned lyric_at, lyric_count, lyric_started, lyric_lead;
    uint32_t lyric_sig;
    int octave;
    int32_t song_ms;
    struct mumble_note seq[VOICE_SEQ];
    uint8_t seq_track[VOICE_SEQ], seq_pass[VOICE_SEQ], rec_slot[MUMBLE_CAST];
    unsigned seq_count;
    uint32_t pressed, held, key_down, keys, letters, rec_held;
    int32_t turned[7];
} before;
static void step_before(void)
{
    memcpy(before.tracks, voice_tracks, sizeof voice_tracks);
    memcpy(before.text, voice_text, sizeof voice_text);
    before.length = voice_length, before.cursor = voice_cursor, before.cast = voice_cast, before.page = voice_page, before.caps = voice_caps;
    before.mode = voice_mode, before.play = voice_play, before.bars = voice_bars, before.bpm = voice_bpm, before.demo = voice_demo;
    before.song = voice_song, before.recording = voice_recording, before.octave = voice_octave;
    before.lyric_at = voice_lyric_at, before.lyric_count = voice_lyric_count, before.lyric_started = voice_lyric_started;
    before.lyric_lead = voice_lyric_lead, before.lyric_sig = voice_lyric_sig, before.song_ms = voice_now.song_ms;
    memcpy(before.seq, voice_seq, sizeof voice_seq);
    memcpy(before.seq_track, voice_seq_track, sizeof voice_seq_track);
    memcpy(before.seq_pass, voice_seq_pass, sizeof voice_seq_pass);
    memcpy(before.rec_slot, voice_rec_slot, sizeof voice_rec_slot);
    before.seq_count = voice_seq_count, before.letters = voice_held, before.rec_held = voice_rec_held;
    /* As app.c's input() will read them (HOME never reaches the page). */
    before.held = panel_buttons(held_buttons);
    before.pressed = before.held & ~buttons_before & ~BUTTON(HOME);
    before.keys = held_keys & ((1u << NKEYS) - 1u);
    before.key_down = before.keys & ~keys_before;
    for (unsigned i = 0; i < 7; ++i) before.turned[i] = clamp(turned[encoder_of[i]], -127, 127);
}
/* (Passes past VOICE_PASSES merge: a note's pass may be relabelled, the note is the same.) */
static int same_note(unsigned k, unsigned j)
{
    return voice_seq_track[k] == before.seq_track[j] && !memcmp(&voice_seq[k], &before.seq[j], sizeof voice_seq[k]);
}
/* EDIT as the model sees it: when it went down, whether it was spent (held
 * 1 s, or a knob turned with it), whether it was down last frame. */
static uint32_t edit_since;
static unsigned edit_spent, edit_was;
static void step_after(void)
{
    uint32_t pressed = before.pressed;
    unsigned turned_any = 0;
    for (unsigned i = 0; i < 7; ++i) turned_any |= before.turned[i] != 0;
    unsigned alone = !turned_any && !before.key_down;
    unsigned edit = (before.held & BUTTON(EDIT)) != 0, edit_tap = 0, edit_hold = 0;
    if (pressed & BUTTON(EDIT)) edit_since = now_ms, edit_spent = 0;
    if (edit && turned_any) edit_spent = 1;
    if (edit && !edit_spent && now_ms - edit_since >= VOICE_HOLD_MS) edit_hold = 1, edit_spent = 1;
    if (!edit && edit_was && !edit_spent) edit_tap = 1;
    edit_was = edit;
    /* (A recorded note let go in the same frame joins the loop after EDIT acts.) */
    unsigned let_go = (before.letters & ~before.keys) != 0;
    /* One control at a time, as each does. */
    if (pressed == BUTTON(GLO) && alone) {
        if (voice_mode != (before.mode + 1u) % VOICE_MODES) fail("S2 GLO did not step TYPE, SING, LOOP");
        if (!voice_play && voice_recording) fail("S2 recording went on into TYPE");
        if (before.mode == VOICE_SING && voice_recording != before.recording) fail("S2 GLO from SING to LOOP stopped recording");
    }
    if (pressed == BUTTON(REC) && alone) {
        if (voice_mode != (before.play ? before.mode : (unsigned)VOICE_SING)) fail("S2 REC did not go to SING");
        if (voice_recording != (before.play ? !before.recording : 1u)) fail("S2 REC did not flip recording");
        if (voice_recording && !voice_song) fail("S2 recording without the loop running");
    }
    if (pressed == BUTTON(PLAY) && alone) {
        if (before.song && voice_song) fail("S2 PLAY did not stop the loop");
        if (!before.song && !voice_song) fail("S2 PLAY did not start the loop");
    }
    if (pressed == BUTTON(FX) && alone && before.play &&
        (voice_lyric_at || voice_lyric_started || voice_lyric_lead != MUMBLE_LETTERS))
        fail("S2 FX in SING did not go back to the line's top");
    if (edit_tap && !pressed && alone && !(before.play && let_go)) {
        if (before.play) {
            unsigned last = 0, kept = 0;
            for (unsigned j = 0; j < before.seq_count; ++j)
                if (before.seq_pass[j] > last) last = before.seq_pass[j];
            for (unsigned j = 0; j < before.seq_count; ++j)
                if (before.seq_pass[j] != last) {
                    if (kept >= voice_seq_count || !same_note(kept, j)) fail("S2 EDIT in SING undid more than the last pass");
                    ++kept;
                }
            if (kept != voice_seq_count) fail("S2 EDIT in SING did not undo the last pass");
        } else {
            unsigned from = before.cursor;
            while (from && before.text[from - 1u] == ' ') --from;
            while (from && before.text[from - 1u] != ' ') --from;
            unsigned cut = before.cursor - from;
            if (voice_length != before.length - cut || voice_cursor != from || memcmp(voice_text, before.text, from) ||
                memcmp(voice_text + from, before.text + before.cursor, before.length - before.cursor))
                fail("S2 EDIT in TYPE did not cut just the word before the cursor");
            if (voice_seq_count != before.seq_count) fail("S2 EDIT tapped in TYPE touched the loop");
        }
    }
    if (edit_hold && !pressed && alone && !(before.play && let_go)) {
        if (!before.play && before.length) {
            if (voice_length || voice_seq_count != before.seq_count) fail("S2 EDIT held did not clear just the line");
        } else if (voice_seq_count)
            fail("S2 EDIT held left notes in the loop");
    }
    if (pressed == 0 && before.turned[0] && !before.turned[6]) {
        int n = before.turned[0] % (int)MUMBLE_CAST;
        if (voice_cast != (unsigned)(((int)before.cast + n + (int)MUMBLE_CAST) % (int)MUMBLE_CAST))
            fail("S2 SELECT did not move the cast by its detents");
        if (!before.key_down && (voice_length != before.length || memcmp(voice_text, before.text, voice_length)))
            fail("S2 another character did not keep the line");
    }
    if (!(pressed & BUTTON(SEQ)) && before.turned[1] &&
        voice_page != (unsigned)(((int)before.page + before.turned[1] % (int)VOICE_PAGES + (int)VOICE_PAGES) % (int)VOICE_PAGES))
        fail("S2 ALGO did not move the page by its detents");
    /* SING, the knobs on a sound page: the selected sound, those parameters, by
     * the accelerated amount, kept in range; with EDIT held, the character's. */
    unsigned knobs_alone = !pressed && !before.turned[0] && !before.turned[1] && !before.turned[6];
    if (before.mode == VOICE_SING && knobs_alone) {
        struct mumble_voice want = before.tracks[before.cast].sound;
        for (unsigned c = 0; c < 4; ++c) {
            int v = before.turned[2 + c];
            if (!v) continue;
            int a = v < 0 ? -v : v, gain = a < 2 ? 1 : a < 4 ? 2 : a == 4 ? 3 : 4;
            unsigned p = before.page * 4u + c;
            want.value[p] = edit ? mumble_cast[before.cast].voice.value[p]
                                 : (uint16_t)clamp((int)want.value[p] + v * gain, mumble_params[p].min, mumble_params[p].max);
        }
        if (memcmp(&want, &voice_tracks[before.cast].sound, sizeof want)) fail("S2 a knob moved the sound wrongly");
    }
    /* LOOP, the knobs in turn: the selected one's start and level, the bars
     * (a step of 1, 2, 4, 8), the BPM; with EDIT held, where they start. */
    if (before.mode == VOICE_LOOP && knobs_alone) {
        int start = before.tracks[before.cast].start, level = before.tracks[before.cast].level;
        int bars = (int)before.bars, bpm = (int)before.bpm;
        for (unsigned c = 0; c < 4; ++c) {
            int v = before.turned[2 + c];
            if (!v) continue;
            int a = v < 0 ? -v : v, gain = a < 2 ? 1 : a < 4 ? 2 : a == 4 ? 3 : 4;
            int steps = bars * 16;
            if (c == 0) start = edit ? 0 : clamp(start + v * gain, 0, steps - 1);
            else if (c == 1) level = edit ? 100 : clamp(level + v * gain, 0, 100);
            else if (c == 2) bars = edit ? 2 : clamp(v > 0 ? bars * 2 : bars / 2, 1, 8);
            else bpm = edit ? 120 : clamp(bpm + v * gain, 40, 240);
            if (start >= bars * 16) start = bars * 16 - 1;
        }
        if (voice_tracks[before.cast].start != start || voice_tracks[before.cast].level != level ||
            voice_bars != (unsigned)bars || voice_bpm != (unsigned)bpm)
            fail("S2 a LOOP knob moved the loop wrongly");
        for (unsigned i = 0; i < MUMBLE_CAST; ++i)
            if (memcmp(&before.tracks[i].sound, &voice_tracks[i].sound, sizeof before.tracks[i].sound))
                fail("S2 a LOOP knob changed a sound");
    }
    /* TYPE, K1 then K2 alone: the cursor moved by the accelerated amount, then
     * the letter under it changed (at the end, one added) and nothing else. */
    if (!before.play && !pressed && !before.key_down && !before.turned[0] && !before.turned[6] && !edit_tap &&
        !edit_hold && (before.turned[2] || before.turned[3])) {
        if (memcmp(before.tracks, voice_tracks, sizeof voice_tracks)) fail("S2 a TYPE knob changed a sound");
        int v = before.turned[2], a = v < 0 ? -v : v, gain = a < 2 ? 1 : a < 4 ? 2 : a == 4 ? 3 : 4;
        unsigned at = (unsigned)clamp((int)before.cursor + v * gain, 0, (int)before.length);
        if (voice_cursor != at) fail("S2 K1 did not move the cursor by its detents");
        unsigned grew = before.turned[3] && at == before.length && before.length < MUMBLE_TEXT;
        if (voice_length != before.length + grew) fail("S2 K2 changed the line's length but at its end");
        for (unsigned i = 0; i < before.length; ++i)
            if (i != at && voice_text[i] != before.text[i]) fail("S2 K2 changed a letter not under the cursor");
        if (before.turned[3] && (at < before.length || grew)) {
            char c = voice_text[at];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (!memchr(voice_alphabet, c, VOICE_ALPHABET)) fail("S2 K2 made a letter not in its alphabet");
        }
    }
    if (pressed || turned_any || edit_tap || edit_hold) return;

    /* Nothing pressed or turned: only the keys act, and only as the mode says. */
    if (voice_cast != before.cast || voice_page != before.page || voice_caps != before.caps ||
        voice_mode != before.mode || voice_bars != before.bars || voice_bpm != before.bpm ||
        voice_demo != before.demo || voice_octave != before.octave || voice_recording != before.recording)
        fail("S2 a setting changed with nothing pressed or turned");
    if (!before.song && voice_song) fail("S2 the loop started with nothing pressed");
    if (memcmp(before.tracks, voice_tracks, sizeof voice_tracks)) fail("S2 a character changed with nothing pressed or turned");
    if (!before.play) {
        /* TYPE: each key down types its letter at the cursor (in key order); on
         * a full line the oldest falls off (at the end) or the last (inside). */
        char want[MUMBLE_TEXT + 1];
        unsigned n = before.length, at = before.cursor;
        memcpy(want, before.text, n);
        for (unsigned k = 0; k < 27; ++k) {
            if (!(before.key_down & (1u << k))) continue;
            if (n >= MUMBLE_TEXT) {
                if (at < n) --n;
                else { memmove(want, want + 1, MUMBLE_TEXT - 1); n = at = MUMBLE_TEXT - 1; }
            }
            memmove(want + at + 1, want + at, n - at);
            want[at++] = k == 26 ? ' ' : (char)((before.caps ? 'A' : 'a') + k);
            ++n;
        }
        if (voice_length != n || voice_cursor != at || memcmp(voice_text, want, n))
            fail("S2 typing did not put exactly the keys' letters at the cursor");
    } else if (voice_length != before.length || memcmp(voice_text, before.text, voice_length))
        fail("S2 the line changed in SING with nothing pressed");
    /* The sequence: its notes kept, new ones only from singers being recorded,
     * each singer's first new one with the slot it was singing. */
    unsigned j = 0, added[MUMBLE_CAST] = {0};
    for (unsigned k = 0; k < voice_seq_count; ++k) {
        if (j < before.seq_count && same_note(k, j)) { ++j; continue; }
        unsigned t = voice_seq_track[k];
        if (!before.play || !before.recording || !(before.rec_held & (1u << t)))
            fail("S2 a note joined the loop without its singer being recorded");
        if (!added[t] && voice_seq[k].slot != before.rec_slot[t]) fail("S2 a note joined the loop with the wrong slot");
        /* (its note let go, then each key pressed that it took over and moved on from) */
        if (++added[t] > 2u + (unsigned)__builtin_popcount(before.key_down)) fail("S2 a singer added more than its notes");
    }
    if (j != before.seq_count) fail("S2 the loop lost or changed notes with nothing pressed");
    if (before.play && voice_held != ((before.letters & before.keys) | before.key_down))
        fail("S2 SING does not hold exactly the keys still down and the new ones");
    /* SING: the queue follows its leading key, and goes back to the top as the loop wraps. */
    if (before.play && voice_lyric_sig == before.lyric_sig) {
        unsigned at = before.lyric_at, started = before.lyric_started, lead = before.lyric_lead;
        uint32_t still = before.letters & before.keys;
        if (lead < MUMBLE_LETTERS && !(still & (1u << lead))) lead = MUMBLE_LETTERS;
        for (unsigned k = 0; k < MUMBLE_LETTERS; ++k) {
            if (!(before.key_down & (1u << k)) || lead < MUMBLE_LETTERS) continue;
            if (started && before.lyric_count) at = (at + 1u) % before.lyric_count;
            started = 1, lead = k;
        }
        if (voice_song && before.song_ms >= 0 && voice_now.song_ms >= 0 && voice_now.song_ms < before.song_ms)
            at = 0, started = 0, lead = MUMBLE_LETTERS;
        if (voice_lyric_at != at || voice_lyric_lead != lead || voice_lyric_started != started)
            fail("S2 the queue did not follow its leading key (or the loop's wrap)");
    }
    /* SING: the last key pressed is sung at once. */
    if (before.play && before.key_down) {
        unsigned last = 31u - (unsigned)__builtin_clz(before.key_down), sung = 0;
        for (unsigned who = 0; who < MUMBLE_CAST; ++who) sung |= voice_sings[who] == last + 1u;
        if (!sung) fail("S2 the last key pressed is not sung");
    }
}

/* One UI frame (16 ms) and the audio blocks that fall due meanwhile. */
static void frame(void)
{
    now_ms += 16;
    audio_debt += 16u * 44100u;
    while (audio_debt >= APP_BLOCK * 1000u) {
        audio_debt -= APP_BLOCK * 1000u;
        render_block();
    }
    step_before();
    app_frame();
    if (page != VOICE) fail("P5 the page left MUMBLE");
    check_state();
    step_after();
}
/* MUMBLE_FUZZ_SCREEN=DIR: a P3 failure leaves both screens there as PPMs. */
static void save_screen(const char *name, const uint16_t *px)
{
    const char *dir = getenv("MUMBLE_FUZZ_SCREEN");
    char path[512];
    if (!dir || snprintf(path, sizeof path, "%s/%s.ppm", dir, name) >= (int)sizeof path) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 240 240 255\n");
    for (unsigned i = 0; i < 240u * 240u; ++i) {
        uint16_t c = (uint16_t)((px[i] >> 8) | (px[i] << 8));
        uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63u) << 2), (uint8_t)((c & 31u) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "screen: %s\n", path);
}
static void check_repaint(void)
{
    static uint16_t incremental[240 * 240];
    memcpy(incremental, fb, sizeof fb);
    paint_all();
    paint_frame(draw);
    lcd_wait();
    if (memcmp(incremental, fb, sizeof fb)) {
        unsigned i = 0;
        while (incremental[i] == fb[i]) ++i;
        fprintf(stderr, "first stale pixel: x %u y %u\n", i % 240u, i / 240u);
        save_screen("incremental", incremental);
        save_screen("repainted", fb);
        fail("P3 the damage-only redraw differs from a full repaint");
    }
    /* Drawn twice from the same state: the same screen. */
    paint_all();
    paint_frame(draw);
    lcd_wait();
    if (memcmp(incremental, fb, sizeof fb)) fail("P7 drawing the same state twice differs");
}
static void check_panic(void)
{
    const char *was = phase;
    phase = "panic";
    app_panic();
    for (unsigned i = 0; i < 3u * 44100u / APP_BLOCK; ++i) render_block();
    audio_peak = 0;
    for (unsigned i = 0; i < 8; ++i) render_block();
    if (audio_peak > 8388608 / 1000) fail("P6 still sounding 3 s after a panic");
    phase = was;
}

static void boot(void)
{
    phase = "boot";
#ifndef FUZZ_ASAN_GLOBALS
    state_copy(__start_mumble_bss, pristine_bss, bss_size());
    state_copy(__start_mumble_data, pristine_data, data_size());
#endif
    memset(fb, 0, sizeof fb);
    in_flight = NULL;
    now_ms = 0, held_buttons = held_keys = lit_buttons = lit_keys = 0;
    memset(turned, 0, sizeof turned);
    master_q12 = 4096, audio_debt = 0, audio_sum = 0, audio_peak = 0;
    edit_since = 0, edit_spent = 0, edit_was = 0;
    app_init();
    app_ui_init();
    frame();
}

/* MUMBLE_FUZZ_EXPLAIN=1: each op printed as the panel action it is, with the
 * time it happens (tests/fuzz.sh --explain / --minimize). */
static int explaining = -1;
static const char *const button_names[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                             "PLAY", "REC", "OCT-", "OCT+"};
static const char *const encoder_names[7] = {"SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"};
static void explain(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void explain(const char *format, ...)
{
    if (explaining < 0) explaining = getenv("MUMBLE_FUZZ_EXPLAIN") != NULL;
    if (!explaining) return;
    va_list list;
    va_start(list, format);
    fprintf(stderr, "%6u ms  ", now_ms);
    vfprintf(stderr, format, list);
    fputc('\n', stderr);
    va_end(list);
}

/* The input: ops of one byte and their arguments. */
static uint32_t play(const uint8_t *data, size_t size)
{
    boot();
    phase = "play";
    size_t at = 0;
#define NEXT() (at < size ? data[at++] : 0u)
    unsigned frames = 0;
    while (at < size && frames < 1500) {
        unsigned op = NEXT();
        unsigned arg = NEXT();
        switch (op % 10) {
        case 0: case 1: /* a button held or let go (the FM-1's B_* order) */
            held_buttons ^= 1u << (arg % NB);
            explain("%s %s", held_buttons & (1u << (arg % NB)) ? "press" : "release", button_names[arg % NB]);
            break;
        case 2: case 3: /* a key */
            held_keys ^= 1u << (arg % NKEYS);
            explain("key %u (%c) %s", arg % NKEYS, arg % NKEYS < 26 ? 'a' + (int)(arg % NKEYS) : '_',
                    held_keys & (1u << (arg % NKEYS)) ? "down" : "up");
            break;
        case 4: { /* an encoder, a few detents either way */
            unsigned enc = arg % 7;
            int amount = (int)(int8_t)NEXT();
            int detents = amount / 16 ? amount / 16 : amount < 0 ? -1 : 1;
            turned[enc] += detents;
            explain("turn %s %+d", encoder_names[enc], detents);
            break;
        }
        case 5: case 6: { /* time passes */
            unsigned n = 1 + arg % 48;
            explain("wait %u frames (%u ms)", n, 16u * n);
            for (unsigned i = 0; i < n; ++i) frame();
            frames += n;
            break;
        }
        case 7: /* MASTER */
            master_q12 = arg * 17u;
            explain("MASTER %u of 4335", master_q12);
            break;
        case 8: /* everything let go */
            held_buttons = held_keys = 0;
            explain("let go of everything");
            frame();
            ++frames;
            break;
        case 9: /* the screen checked against a full repaint now */
            explain(arg & 1 ? "check the screen against a full repaint" : "(a frame)");
            if (arg & 1) check_repaint();
            else frame(), ++frames;
            break;
        }
        frame();
        ++frames;
    }
#undef NEXT
    phase = "end";
    explain("end: let go of everything, 8 frames, check the screen%s", size && data[0] & 1 ? ", then a panic" : "");
    held_buttons = held_keys = 0;
    for (unsigned i = 0; i < 8; ++i) frame();
    check_repaint();
    uint32_t result = audio_sum ^ sum(fb, sizeof fb) * 7u;
    if (size && data[0] & 1) check_panic();
    return result;
}

/* The panel's buttons in the page's order and back: every combination. */
static void check_button_order(void)
{
    uint32_t all = (1u << NB) - 1u;
    for (uint32_t held = 0; held <= all; ++held)
        if (platform_buttons(panel_buttons(held)) != held) fail("button order does not map back");
    for (unsigned b = 0; b < BUTTONS; ++b)
        for (unsigned c = b + 1; c < BUTTONS; ++c)
            if (button_of[b] == button_of[c]) fail("two panel buttons on one FM-1 button");
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc, (void)argv;
#ifndef FUZZ_ASAN_GLOBALS
    if (!bss_size() || !data_size()) fail("the app's state sections are empty");
    pristine_bss = malloc(bss_size());
    pristine_data = malloc(data_size());
    state_copy(pristine_bss, __start_mumble_bss, bss_size());
    state_copy(pristine_data, __start_mumble_data, data_size());
    /* The sections must hold the page's state and the engine's, static locals too. */
    if (!in_state(&voice_cast) || !in_state(&paint_left) || !in_state(&controls))
        fail("the app's state is not all in its sections");
#endif
    check_button_order();
    /* The help card's rows fit the dialogue box (a longer one draws over its border). */
    for (unsigned m = 0; m < 2; ++m)
        for (unsigned r = 0; r < VOICE_HELP_ROWS; ++r)
            if (strlen(voice_help_rows[m][r]) > VOICE_HELP_COLUMNS) fail("a help row wider than the dialogue box");
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static unsigned runs;
    if (++runs > 1 && FUZZ_ONE_INPUT) fail("FUZZ_ASAN_GLOBALS runs one input a process (tests/fuzz.sh replays)");
    uint32_t first = play(data, size);
    /* Every 16th input again from a fresh boot: the same performance plays the same. */
    int was = explaining;
    explaining = 0;                             /* (the replay plays quietly) */
    if (!FUZZ_ONE_INPUT && size && !(data[0] & 0xF0) && play(data, size) != first) fail("P7 the same input played differently");
    explaining = was;
    return 0;
}
