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
/* Its state (and the engine's, fuzz_engine.c) in one section, kept as it was
 * before the first boot and put back for every input. */
#include "app.h"
#pragma clang section bss = "mumble_bss" data = "mumble_data"
#include "../app/app.c"
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
/* One UI frame (16 ms) and the audio blocks that fall due meanwhile. */
static void frame(void)
{
    now_ms += 16;
    audio_debt += 16u * 44100u;
    while (audio_debt >= APP_BLOCK * 1000u) {
        audio_debt -= APP_BLOCK * 1000u;
        render_block();
    }
    app_frame();
    if (page != VOICE) fail("P5 the page left MUMBLE");
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
    state_copy(__start_mumble_bss, pristine_bss, bss_size());
    state_copy(__start_mumble_data, pristine_data, data_size());
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
    if (!bss_size() || !data_size()) fail("the app's state sections are empty");
    pristine_bss = malloc(bss_size());
    pristine_data = malloc(data_size());
    state_copy(pristine_bss, __start_mumble_bss, bss_size());
    state_copy(pristine_data, __start_mumble_data, data_size());
    /* The sections must hold the page's state and the engine's, static locals too. */
    if (!in_state(&voice_cast) || !in_state(&paint_left) || !in_state(&controls))
        fail("the app's state is not all in its sections");
    check_button_order();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint32_t first = play(data, size);
    /* Every 16th input again from a fresh boot: the same performance plays the same. */
    if (size && !(data[0] & 0xF0) && play(data, size) != first) fail("P7 the same input played differently");
    return 0;
}
