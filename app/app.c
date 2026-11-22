/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* MUMBLE, a voice synth firmware for the FM-1 on fm1-base. The engine
 * (app/mumble.c, built as engine.c) and the page (app/mumble_screen.inc, with
 * the cast's art); this file gives the page the panel's controls, the
 * painting (paint.inc) and the 1 kHz-free frame loop.
 * Part of the unity build (device/unity.c, host/plat_host.c), after the
 * platform. */
#define MUMBLE_PORT "mumble_port.h"
#include "mumble_port.h"

/* The few string routines the page uses; the FM-1 has no libc. */
static unsigned mumble_strlen(const char *s) { unsigned n = 0; while (s[n]) ++n; return n; }
static char *mumble_strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
static char *mumble_strcat(char *d, const char *s) { mumble_strcpy(d + mumble_strlen(d), s); return d; }
static void *mumble_memmove(void *d, const void *s, unsigned n)
{
    unsigned char *to = d;
    const unsigned char *from = s;
    if (to < from) while (n--) *to++ = *from++;
    else while (n--) to[n] = from[n];
    return d;
}
#define strlen mumble_strlen
#define strcpy mumble_strcpy
#define strcat mumble_strcat
#define memmove mumble_memmove

#include "paint.inc"

/* ------------------------------------------------------------ controls --- */
/* The page reads the panel in its own button order, the keys, and seven
 * encoders (SELECT, K1, K2..K5, PRESETS). */
enum { OCT_DOWN, OCT_UP, FX, SCL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC, BUTTONS };
#define BUTTON(b) (1U << (b))
struct fm1_controls {
    uint32_t keys;
    uint16_t buttons;
    int16_t encoders[7];
};
static const uint8_t button_of[BUTTONS] = {
    [OCT_DOWN] = B_OCTDN, [OCT_UP] = B_OCTUP, [FX] = B_FX, [SCL] = B_SEL, [ENV] = B_ENV, [LFO] = B_LFO,
    [EDIT] = B_EDIT, [GLO] = B_GLO, [HOME] = B_HOME, [SAVE] = B_SAVE, [ARP] = B_ARP, [SEQ] = B_SEQ,
    [PLAY] = B_PLAY, [REC] = B_REC};
static const uint8_t encoder_of[7] = {EN_SELECT, EN_ALGO, EN_K1, EN_K2, EN_K3, EN_K4, EN_PRESET};
/* The page could leave to an overview on HOME; here it is the whole
 * firmware, so HOME never reaches it. */
enum { OVERVIEW, VOICE };
static unsigned page = VOICE;
static uint32_t blocked_keys;
/* A sequencer for the loop to follow: none yet (MIDI clock is the plan). */
static struct { unsigned tempo, playing, play_step; } frame_views[1];
static uint32_t frame_time;
static const char *notice;
static uint32_t notice_until;
static uint8_t encoder_speed[7];
/* A quick turn moves further (adapted from FM1 Open; see LICENSE.md). */
static int fm_encoder_delta(unsigned encoder, int16_t value)
{
    int amount = value < 0 ? -(int)value : value;
    int gain = amount < 2 ? 1 : amount < 4 ? 2 : amount == 4 ? 3 : 4;
    if (gain < encoder_speed[encoder]) gain = encoder_speed[encoder];
    return value * gain;
}

#include "mumble_screen.inc"

/* ----------------------------------------------------------- platform --- */
static struct fm1_controls controls;
static uint32_t buttons_before, keys_before;

static uint16_t panel_buttons(uint32_t held)
{
    uint16_t out = 0;
    for (unsigned b = 0; b < BUTTONS; ++b)
        if (held & (1U << button_of[b])) out |= (uint16_t)BUTTON(b);
    return out;
}
static uint32_t platform_buttons(uint32_t bits)
{
    uint32_t out = 0;
    for (unsigned b = 0; b < BUTTONS; ++b)
        if (bits & BUTTON(b)) out |= 1U << button_of[b];
    return out;
}
/* Read the panel and hand it to the page. */
static void input(void)
{
    controls.buttons = panel_buttons(plat_buttons());
    controls.keys = plat_keys() & ((1U << NKEYS) - 1U);
    for (unsigned i = 0; i < 7; ++i) {
        int32_t turned = plat_enc(encoder_of[i]);
        controls.encoders[i] = (int16_t)clamp(turned, -127, 127);
    }
    uint32_t pressed = controls.buttons & ~buttons_before & ~BUTTON(HOME);
    uint32_t key_down = controls.keys & ~keys_before;
    buttons_before = controls.buttons;
    keys_before = controls.keys;
    voice_controls(&controls, pressed, key_down);
}

void app_init(void)
{
    voice_open();
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    for (uint32_t i = 0; i < 2U * n; ++i) lr[i] = 0;
    for (uint32_t k = 0; k + FM1_BLOCK_FRAMES <= n; k += FM1_BLOCK_FRAMES)
        PLAT_FAR(mumble_render)(lr + 2U * k);
    /* The gain in single precision: a 24-bit sample fits a float exactly, and
     * a 64-bit multiply inlined among loads can corrupt a register on the
     * FM-1 (felucca-bsp docs/CPU.md; the build's check named this loop). */
    const float gain = (float)master_q12 * (1.0f / 4096.0f);
    for (uint32_t i = 0; i < 2U * n; ++i) {
        float v = (float)lr[i] * gain;
        lr[i] = v >= 8388607.0f ? 8388607 : v <= -8388608.0f ? -8388608 : (int32_t)v;
    }
}

void app_panic(void) { mumble_stop(); }

void app_ui_init(void)
{
    lcd_fill(0, 0, SCREEN_W, SCREEN_H, BACK);
    paint_all();
}

static void draw(void) { voice_draw(); }

void app_frame(void)
{
    frame_time = plat_ms();
    if (notice && (int32_t)(frame_time - notice_until) >= 0) notice = NULL;
    input();
    voice_frame(frame_time);
    plat_leds(platform_buttons(voice_button_bits()), voice_led_bits());
    paint_frame(draw);
}

void app_input_only(void) {}

/* A message from the platform: the page's notice line. */
void app_say(const char *a, const char *b)
{
    static char said[40];
    said[0] = 0;
    if (a) mumble_strcat(said, a);
    if (b && b[0] && mumble_strlen(said) + mumble_strlen(b) + 1U < sizeof said) {
        mumble_strcat(said, " ");
        mumble_strcat(said, b);
    }
    notice = said;
    notice_until = plat_ms() + 1500U;
}

#ifdef PLAT_HOST
/* The host simulator's checks: what the page is doing. */
int app_expect(const char *what, int *got)
{
    const struct voice_track *t = voice_track();
    if (!strcmp(what, "cast")) *got = (int)voice_cast;
    else if (!strcmp(what, "length")) *got = (int)voice_length;
    else if (!strcmp(what, "song")) *got = (int)voice_song;
    else if (!strcmp(what, "mode")) *got = (int)voice_play;                 /* 0 TYPE, 1 SING */
    else if (!strcmp(what, "page")) *got = (int)voice_page;
    else if (!strcmp(what, "loop")) *got = (int)voice_loop;
    else if (!strcmp(what, "choir")) *got = t->choir;
    else if (!strcmp(what, "sounding")) *got = voice_now.vowel < MUMBLE_VOWELS;
    else if (!strcmp(what, "vowel")) *got = voice_now.vowel;                 /* the loudest sounding, 6: none */
    else if (!strcmp(what, "take")) *got = (int)voice_notes_of(voice_cast);  /* the selected one's notes in the loop */
    else if (!strcmp(what, "takes")) *got = (int)voice_seq_count;           /* all notes in the loop */
    else if (!strcmp(what, "recording")) *got = (int)voice_recording;
    else if (!strcmp(what, "passes")) *got = (int)voice_passes();
    else if (!strcmp(what, "metronome")) *got = (int)voice_click;
    else if (!strcmp(what, "sung")) *got = (int)voice_sung;
    else if (!strcmp(what, "lyric")) *got = (int)voice_lyric_at;
    else if (!strcmp(what, "lyrics")) *got = (int)voice_lyric_count;
    else if (!strcmp(what, "lyric_next")) *got = (int)voice_lyric_next();
    else if (!strcmp(what, "lead")) *got = voice_lyric_lead < MUMBLE_LETTERS ? (int)voice_lyric_lead : -1;
    else if (!strcmp(what, "singing")) {
        *got = 0;
        for (unsigned i = 0; i < MUMBLE_CAST; ++i) *got += voice_sings[i] != 0;
    }
    else return 0;
    return 1;
}
#endif
