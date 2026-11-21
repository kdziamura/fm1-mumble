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
 *       lights (one mode at a time, letters held only while their keys are,
 *       printable lines of their length, sound parameters in range ...);
 *   S2  the frame did what a model of the controls says: REC flips the loop,
 *       GLO goes on to the next mode, PLAY starts or stops the song, SELECT
 *       and ALGO move by their detents, a knob moves its parameter of the
 *       selected sound by the accelerated amount; with nothing pressed or
 *       turned no setting changes, TYPE adds exactly the keys' letters, PLAY
 *       holds exactly the letters still down and the new ones, and only SAVE/EDIT+key in SAMPLE touch
 *       a pad; REC in PLAY flips recording (looping, the loop running), and
 *       every character sings one key at a time, the last key pressed at
 *       once, no key held unsung while a choir member is free; a take grows
 *       only by what its character sang while recorded;
 *       EDIT undoes the take's last pass (PLAY), else clears the line, else
 *       every line and take; the metronome runs exactly while recording.
 * And once, at start-up, every button combination survives the panel's
 * button-order mapping both ways.
 *
 *   tests/fuzz.sh [libFuzzer options]     e.g. -max_total_time=600 -jobs=8
 */
#define PLAT_HOST 1
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
static void check_text(const char *text, unsigned length, unsigned terminated, const char *what)
{
    if (length > MUMBLE_TEXT) fail(what);
    for (unsigned i = 0; i < length; ++i)
        if ((unsigned char)text[i] < 32u || (unsigned char)text[i] > 126u) fail(what);
    if (terminated && text[length]) fail(what);
}
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
    if (voice_caps > 1 || voice_loop > 1 || voice_play > 1 || voice_sample > 1 || voice_song_dirty > 1 ||
        voice_talking > 1)
        fail("S1 a flag is not 0 or 1");
    if (voice_play && voice_sample) fail("S1 PLAY and SAMPLE at once");
    if (voice_song > 2) fail("S1 song state out of range");
    if (voice_demo >= VOICE_LINES) fail("S1 demo line out of range");
    if (voice_bars != 1 && voice_bars != 2 && voice_bars != 4 && voice_bars != 8) fail("S1 bars not 1, 2, 4 or 8");
    if (voice_bpm < 40 || voice_bpm > 240) fail("S1 BPM out of 40..240");
    if (voice_octave < -2 || voice_octave > 2) fail("S1 octave out of -2..2");
    if (voice_held & ~held_keys) fail("S1 a letter held whose key is up");
    if (voice_recording && (!voice_play || !voice_loop)) fail("S1 recording outside PLAY or without the loop");
    uint32_t singing = 0, sung_keys = 0, choir_free = 0;
    for (unsigned who = 0; who < MUMBLE_CAST; ++who) {
        if (voice_sings[who] > MUMBLE_LETTERS) fail("S1 a character sings a key that does not exist");
        if (!voice_sings[who]) { choir_free |= voice_in_choir(who); continue; }
        uint32_t key = 1u << (voice_sings[who] - 1u);
        if (!voice_play || !(voice_held & key)) fail("S1 a character sings a key not held in PLAY");
        if (sung_keys & key) fail("S1 two characters sing one key");
        sung_keys |= key;
        singing |= 1u << who;
    }
    if (choir_free && (voice_held & ~sung_keys)) fail("S1 a key held unsung while a choir member is free");
    if (voice_rec_held & ~singing) fail("S1 a character recorded while it sings nothing");
    if (!voice_recording && voice_rec_held) fail("S1 letters being recorded while not recording");
    if (voice_click != (voice_recording ? voice_bars * 4u : 0u)) fail("S1 the metronome not on exactly while recording");
    if (!voice_play && voice_held) fail("S1 letters held outside PLAY");
    if (voice_sung > MUMBLE_LETTERS || voice_pad_last > MUMBLE_LETTERS || voice_pad_on > MUMBLE_LETTERS)
        fail("S1 a key index out of range");
    if (voice_pad_on < MUMBLE_LETTERS && (!voice_sample || !(held_keys & (1u << voice_pad_on))))
        fail("S1 a pad speaking whose key is up (or outside SAMPLE)");
    if (voice_shown > voice_track()->length) fail("S1 more of the line shown than there is");
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        const struct voice_track *t = &voice_tracks[i];
        check_text(t->text, t->length, 1, "S1 a line is not printable text of its length");
        if (t->choir > 1) fail("S1 choir not 0 or 1");
        if (t->start >= voice_steps()) fail("S1 a track starts after the loop's end");
        if (t->level > 100) fail("S1 a track level over 100");
        check_sound(&t->sound, "S1 a sound parameter out of its range");
        if (t->planned && t->syllables > MUMBLE_SYLLABLES) fail("S1 too many syllables planned");
        if (t->notes > VOICE_TAKE) fail("S1 a take longer than it holds");
        for (unsigned k = 0; k < t->notes; ++k)
            if (t->take[k].letter >= MUMBLE_LETTERS || t->take[k].note > 127u || t->take[k].len_ms < 30u ||
                t->take[k].shout > 1u)
                fail("S1 a take's note out of range");
        if (t->passes > VOICE_PASSES || !t->notes != !t->passes || (t->passes && t->pass_at[0]))
            fail("S1 a take's passes do not cover it from its first letter");
        for (unsigned k = 0; k < t->passes; ++k)
            if (t->pass_at[k] >= t->notes || (k && t->pass_at[k] <= t->pass_at[k - 1]))
                fail("S1 a take's passes out of order");
    }
    for (unsigned k = 0; k < MUMBLE_LETTERS; ++k) {
        const struct voice_pad *pad = &voice_pads[k];
        check_text(pad->text, pad->length, 0, "S1 a pad's line is not printable text of its length");
        if (pad->length && pad->cast >= MUMBLE_CAST) fail("S1 a pad's character out of range");
        if (pad->length) check_sound(&pad->sound, "S1 a pad's sound out of range");
    }
    if (voice_now.vowel > MUMBLE_VOWELS || voice_now.track >= MUMBLE_CAST) fail("S1 the engine's status out of range");
    /* The lights tell the state. */
    if (!!(lit_buttons & (1u << B_PLAY)) != (voice_song != 0)) fail("S1 PLAY's light disagrees with the song");
    if (!!(lit_buttons & (1u << B_REC)) != (voice_play ? voice_recording : voice_loop))
        fail("S1 REC's light disagrees with the loop (PLAY mode: with recording)");
    if (!!(lit_buttons & (1u << B_SAVE)) != voice_track()->choir) fail("S1 SAVE's light disagrees with the choir");
    if (!!(lit_buttons & (1u << B_GLO)) != (voice_play | voice_sample)) fail("S1 GLO's light disagrees with the mode");
}

/* S2: a model of each frame. Before it, what the frame will be given; after
 * it, what must have become of the state. */
static struct {
    struct voice_track tracks[MUMBLE_CAST];
    struct voice_pad pads[MUMBLE_LETTERS];
    unsigned cast, page, caps, loop, play, sample, bars, bpm, demo, song, recording;
    int octave;
    uint32_t pressed, held, key_down, keys, letters, rec_held;
    uint8_t rec_letter[MUMBLE_CAST];
    int32_t turned[7];
} before;
static void step_before(void)
{
    memcpy(before.tracks, voice_tracks, sizeof voice_tracks);
    memcpy(before.pads, voice_pads, sizeof voice_pads);
    before.cast = voice_cast, before.page = voice_page, before.caps = voice_caps, before.loop = voice_loop;
    before.play = voice_play, before.sample = voice_sample, before.bars = voice_bars, before.bpm = voice_bpm;
    before.demo = voice_demo, before.song = voice_song, before.octave = voice_octave, before.letters = voice_held;
    before.recording = voice_recording, before.rec_held = voice_rec_held;
    memcpy(before.rec_letter, voice_rec_letter, sizeof before.rec_letter);
    /* As app.c's input() will read them (HOME never reaches the page). */
    before.held = panel_buttons(held_buttons);
    before.pressed = before.held & ~buttons_before & ~BUTTON(HOME);
    before.keys = held_keys & ((1u << NKEYS) - 1u);
    before.key_down = before.keys & ~keys_before;
    for (unsigned i = 0; i < 7; ++i) before.turned[i] = clamp(turned[encoder_of[i]], -127, 127);
}
static int same_text(const struct voice_track *a, const struct voice_track *b)
{
    return a->length == b->length && !memcmp(a->text, b->text, a->length);
}
static void step_after(void)
{
    uint32_t pressed = before.pressed;
    unsigned turned_any = 0;
    for (unsigned i = 0; i < 7; ++i) turned_any |= before.turned[i] != 0;
    /* One control at a time, as each does. */
    if (pressed == BUTTON(REC) && !turned_any) {
        if (!before.play && voice_loop != !before.loop) fail("S2 REC did not flip the loop");
        if (before.play && voice_recording != !before.recording) fail("S2 REC in PLAY did not flip recording");
        if (before.play && voice_recording && (!voice_loop || !voice_song)) fail("S2 recording without the loop running");
    }
    if (pressed == BUTTON(EDIT) && !turned_any && !before.sample && !before.key_down) {
        const struct voice_track *was = &before.tracks[before.cast], *now = &voice_tracks[before.cast];
        if (before.play && was->notes) {
            if (now->passes + 1u != was->passes || now->notes != was->pass_at[was->passes - 1] || !same_text(was, now) ||
                memcmp(now->take, was->take, now->notes * sizeof now->take[0]))
                fail("S2 EDIT in PLAY did not undo just the take's last pass");
        } else if (was->length) {
            if (now->length || now->notes != was->notes) fail("S2 EDIT did not clear just the line");
        } else
            for (unsigned i = 0; i < MUMBLE_CAST; ++i)
                if (voice_tracks[i].length || voice_tracks[i].notes) fail("S2 EDIT on an empty line left a line or a take");
    }
    if (pressed == BUTTON(GLO) && !turned_any) {
        unsigned mode = before.play ? 1u : before.sample ? 2u : 0u, now = voice_play ? 1u : voice_sample ? 2u : 0u;
        if (now != (mode + 1u) % 3u) fail("S2 GLO did not go on to the next mode");
    }
    if (pressed == BUTTON(PLAY) && !turned_any && !before.key_down) {
        if (before.song && voice_song) fail("S2 PLAY did not stop the song");
        if (!before.song && !voice_song) fail("S2 PLAY did not start the song");
    }
    if (pressed == 0 && before.turned[0] && !before.turned[6]) {
        int n = before.turned[0] % (int)MUMBLE_CAST;
        if (voice_cast != (unsigned)(((int)before.cast + n + (int)MUMBLE_CAST) % (int)MUMBLE_CAST))
            fail("S2 SELECT did not move the cast by its detents");
    }
    if (!(pressed & BUTTON(SEQ)) && before.turned[1] &&
        voice_page != (unsigned)(((int)before.page + before.turned[1] % (int)VOICE_PAGES + (int)VOICE_PAGES) % (int)VOICE_PAGES))
        fail("S2 ALGO did not move the page by its detents");
    /* The knobs on a sound page: the selected sound, those parameters, by the accelerated amount, kept in range. */
    if (!pressed && !before.turned[0] && !before.turned[1] && !before.turned[6] && before.page != LOOP_PAGE) {
        struct mumble_voice want = before.tracks[before.cast].sound;
        for (unsigned c = 0; c < 4; ++c) {
            int v = before.turned[2 + c];
            if (!v) continue;
            int a = v < 0 ? -v : v, gain = a < 2 ? 1 : a < 4 ? 2 : a == 4 ? 3 : 4;
            unsigned p = before.page * 4u + c;
            want.value[p] = (uint16_t)clamp((int)want.value[p] + v * gain, mumble_params[p].min, mumble_params[p].max);
        }
        if (memcmp(&want, &voice_tracks[before.cast].sound, sizeof want)) fail("S2 a knob moved the sound wrongly");
    }
    if (pressed || turned_any) return;

    /* Nothing pressed or turned: only the keys act, and only as the mode says. */
    if (voice_cast != before.cast || voice_page != before.page || voice_caps != before.caps ||
        voice_loop != before.loop || voice_play != before.play || voice_sample != before.sample ||
        voice_bars != before.bars || voice_bpm != before.bpm || voice_demo != before.demo ||
        voice_octave != before.octave)
        fail("S2 a setting changed with nothing pressed or turned");
    if (!before.song && voice_song) fail("S2 a song started with nothing pressed");
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        const struct voice_track *was = &before.tracks[i], *now = &voice_tracks[i];
        if (was->choir != now->choir || was->start != now->start || was->level != now->level ||
            memcmp(&was->sound, &now->sound, sizeof was->sound))
            fail("S2 a track's settings changed with nothing pressed or turned");
        if (i == before.cast && !before.play && !before.sample) {
            /* TYPE: each key down adds its letter (in key order), the oldest falling off a full line. */
            char want[MUMBLE_TEXT + 1];
            unsigned n = was->length;
            memcpy(want, was->text, n);
            for (unsigned k = 0; k < 27; ++k) {
                if (!(before.key_down & (1u << k))) continue;
                if (n >= MUMBLE_TEXT) { memmove(want, want + 1, MUMBLE_TEXT - 1); n = MUMBLE_TEXT - 1; }
                want[n++] = k == 26 ? ' ' : (char)((before.caps ? 'A' : 'a') + k);
            }
            if (now->length != n || memcmp(now->text, want, n)) fail("S2 typing did not add exactly the keys' letters");
        } else if (!same_text(was, now))
            fail("S2 a line changed with nothing pressed or turned");
    }
    /* (A key already down when PLAY began sings once pressed again.) */
    /* Takes: each grows only while its character was being recorded, first by
     * the letter it was singing (let go, or its singer moved on), and by at
     * most one more (a letter taken up and moved on from at once). */
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        const struct voice_track *was = &before.tracks[i], *now = &voice_tracks[i];
        unsigned recorded = before.play && before.recording && (before.rec_held & (1u << i));
        if (now->notes < was->notes || memcmp(now->take, was->take, was->notes * sizeof was->take[0]))
            fail("S2 a take lost or changed letters with nothing pressed or turned");
        if (now->notes > was->notes && (!recorded || now->notes > was->notes + 2u))
            fail("S2 a take grew without its character being recorded");
        if (now->notes > was->notes && now->take[was->notes].letter != before.rec_letter[i])
            fail("S2 a take recorded the wrong letter");
    }
    /* PLAY: the last key pressed is sung at once. */
    if (before.play && voice_play && before.key_down) {
        unsigned last = 31u - (unsigned)__builtin_clz(before.key_down), sung = 0;
        for (unsigned who = 0; who < MUMBLE_CAST; ++who) sung |= voice_sings[who] == last + 1u;
        if (!sung) fail("S2 the last key pressed is not sung");
    }
    if (before.play && voice_held != ((before.letters & before.keys) | before.key_down))
        fail("S2 PLAY does not hold exactly the letters still down and the new ones");
    for (unsigned k = 0; k < MUMBLE_LETTERS; ++k) {
        const struct voice_pad *was = &before.pads[k], *now = &voice_pads[k];
        unsigned hit = before.sample && (before.key_down & (1u << k));
        const struct voice_track *t = &before.tracks[before.cast];
        if (hit && (before.held & BUTTON(SAVE))) {
            if (t->length && (now->length != t->length || now->cast != before.cast || memcmp(now->text, t->text, t->length) ||
                              memcmp(&now->sound, &t->sound, sizeof t->sound)))
                fail("S2 SAVE+key did not put the line on the pad");
            if (!t->length && (now->length != was->length || memcmp(now->text, was->text, was->length)))
                fail("S2 SAVE+key with no line changed the pad");
        } else if (hit && (before.held & BUTTON(EDIT))) {
            if (now->length) fail("S2 EDIT+key did not put the pad back to its word");
        } else if (now->length != was->length || memcmp(now->text, was->text, was->length) ||
                   (was->length && (now->cast != was->cast || memcmp(&now->sound, &was->sound, sizeof was->sound))))
            fail("S2 a pad changed without SAVE or EDIT and its key in SAMPLE");
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
    app_init();
    app_ui_init();
    frame();
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
            break;
        case 2: case 3: /* a key */
            held_keys ^= 1u << (arg % NKEYS);
            break;
        case 4: { /* an encoder, a few detents either way */
            unsigned enc = arg % 7;
            int amount = (int)(int8_t)NEXT();
            turned[enc] += amount / 16;
            if (!(amount / 16)) turned[enc] += amount < 0 ? -1 : 1;
            break;
        }
        case 5: case 6: { /* time passes */
            unsigned n = 1 + arg % 48;
            for (unsigned i = 0; i < n; ++i) frame();
            frames += n;
            break;
        }
        case 7: /* MASTER */
            master_q12 = arg * 17u;
            break;
        case 8: /* everything let go */
            held_buttons = held_keys = 0;
            frame();
            ++frames;
            break;
        case 9: /* the screen checked against a full repaint now */
            if (arg & 1) check_repaint();
            else frame(), ++frames;
            break;
        }
        frame();
        ++frames;
    }
#undef NEXT
    phase = "end";
    held_buttons = held_keys = 0;
    for (unsigned i = 0; i < 8; ++i) frame();
    check_repaint();
    uint32_t result = audio_sum ^ sum(fb, sizeof fb) * 7u;
    if (size && data[0] & 1) check_panic();
    return result;
}

/* The panel's buttons in FM1 Open's order and back: every combination. */
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
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static unsigned runs;
    if (++runs > 1 && FUZZ_ONE_INPUT) fail("FUZZ_ASAN_GLOBALS runs one input a process (tests/fuzz.sh replays)");
    uint32_t first = play(data, size);
    /* Every 16th input again from a fresh boot: the same performance plays the same. */
    if (!FUZZ_ONE_INPUT && size && !(data[0] & 0xF0) && play(data, size) != first) fail("P7 the same input played differently");
    return 0;
}
