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
 * encoders (SELECT, ALGO, K1..K4, PRESET). */
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
/* A clock for the loop to follow: MIDI clock in (24 a beat) from USB or the
 * TRS jack, with Start, Continue and Stop. tempo: its BPM (0: no clock);
 * playing: started and ticking; play_tick: ticks since Start; started /
 * stopped: a Start or a Stop came (the page takes them and clears them). */
static struct { unsigned tempo, playing, play_step, play_tick, started, stopped; } frame_views[1];
/* The clock's tempo from the span of its last MIDI_CLOCK_SPAN ticks (four
 * beats): their times as they landed (fm1.toml midi_stamp); on the host,
 * whose MIDI stamps are its real time, as the frame read them. */
enum { MIDI_CLOCK_LOST_MS = 500, MIDI_CLOCK_SPAN = 96 };
static struct {
    uint32_t t24[MIDI_CLOCK_SPAN], heard_ms;
    unsigned at, count, running;
} midi_clock;
static void midi_clock_poll(uint32_t now)
{
    uint32_t pkt;
    int port;
    if (midi_clock.count && now - midi_clock.heard_ms >= MIDI_CLOCK_LOST_MS) midi_clock.count = 0;   /* gone */
    while (plat_midi_in(&pkt, &port)) {
        unsigned status = pkt >> 8 & 0xFFu;
        if (status == 0xF8u) {
#ifdef PLAT_HOST
            uint32_t t = now * 1000U * PLAT_TICKS_PER_US;
#else
            uint32_t t = plat_midi_in_t24();
#endif
            midi_clock.t24[midi_clock.at] = t;
            midi_clock.at = (midi_clock.at + 1U) % MIDI_CLOCK_SPAN;
            if (midi_clock.count < MIDI_CLOCK_SPAN) ++midi_clock.count;
            midi_clock.heard_ms = now;
            if (midi_clock.running) ++frame_views[0].play_tick;
        } else if (status == 0xFAu) {
            midi_clock.running = 1;
            frame_views[0].play_tick = 0;
            frame_views[0].started = 1;
        } else if (status == 0xFBu) {
            midi_clock.running = 1;
        } else if (status == 0xFCu) {
            midi_clock.running = 0;
            frame_views[0].stopped = 1;
        }
    }
    /* The BPM moves only when the clock is half a BPM away from it (no
     * flicker between two neighbours): 60 s / (24 ticks x a tick's length). */
    unsigned tempo = frame_views[0].tempo;
    if (midi_clock.count < 2) tempo = 0;
    else {
        uint32_t newest = midi_clock.t24[(midi_clock.at + MIDI_CLOCK_SPAN - 1U) % MIDI_CLOCK_SPAN];
        uint32_t oldest = midi_clock.t24[(midi_clock.at + MIDI_CLOCK_SPAN - midi_clock.count) % MIDI_CLOCK_SPAN];
        /* us over count - 1 ticks: under 6 s at 40 BPM, so 32 bits do (and no
         * 64-bit sums on the FM-1: see app_render) */
        uint32_t span = (newest - oldest) / PLAT_TICKS_PER_US;
        uint32_t tenths = span ? 25000000U * (midi_clock.count - 1U) / span : tempo * 10U;
        if (!tenths) tenths = 1200U;                     /* (ticks all at once, and no tempo yet: 120) */
        if (!tempo || tenths > tempo * 10U + 5U || tenths + 5U < tempo * 10U) tempo = (tenths + 5U) / 10U;
        tempo = tempo < 40U ? 40U : tempo > 240U ? 240U : tempo;
    }
    frame_views[0].tempo = tempo;
    frame_views[0].playing = midi_clock.running && tempo;
    frame_views[0].play_step = frame_views[0].play_tick / 6U;
}
static uint32_t frame_time;
static const char *notice;
static uint32_t notice_until;
static uint8_t encoder_speed[7];
/* A quick turn moves further (adapted from FM1 Open; see NOTICE.md). */
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

/* Silence now: the engine stopped, and REC no longer armed (its metronome off). */
void app_panic(void)
{
    mumble_stop();
    voice_rec_armed = 0;
}

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
    midi_clock_poll(frame_time);
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
/* The host simulator's "reboot": RAM forgotten, as a power-off does. */
void app_host_reboot(void) { voice_forget(); }
/* The host simulator's checks: what the page is doing. */
int app_expect(const char *what, int *got)
{
    const struct voice_track *t = voice_track();
    if (!strcmp(what, "cast")) *got = (int)voice_cast;
    else if (!strcmp(what, "length")) *got = (int)voice_length;
    else if (!strcmp(what, "song")) *got = (int)voice_song;
    else if (!strcmp(what, "mode")) *got = (int)voice_mode;                 /* 0 TYPE, 1 SING, 2 LOOP */
    else if (!strcmp(what, "cursor")) *got = (int)voice_cursor;             /* TYPE: where keys type */
    else if (!strcmp(what, "help")) *got = (int)voice_help;                 /* HOME held: the help card */
    else if (!strcmp(what, "edited")) *got = (int)voice_edited();           /* the selected sound moved from its character's */
    else if (!strcmp(what, "page")) *got = (int)voice_page;
    else if (!strcmp(what, "system")) *got = (int)voice_system;             /* the system screen open */
    else if (!strcmp(what, "resets")) *got = (int)voice_resets;
    else if (!strcmp(what, "loop")) *got = (int)voice_loop;
    else if (!strcmp(what, "bpm")) *got = (int)voice_bpm;
    else if (!strcmp(what, "tempo")) *got = (int)voice_tempo();             /* the loop's: a MIDI clock's while it plays */
    else if (!strcmp(what, "sync")) *got = (int)voice_fm1_playing;          /* following a MIDI clock */
    else if (!strcmp(what, "level")) *got = (int)voice_track()->level;      /* the selected one's, in the loop (0 muted) */
    else if (!strcmp(what, "tab")) *got = (int)voice_tab;                   /* LOOP's: 0 MIX, 1 ARP, 2 CHORD, 3 SCALE */
    else if (!strcmp(what, "root")) *got = (int)voice_root;                 /* the scale's: 0 C .. 11 B */
    else if (!strcmp(what, "chord")) *got = (int)voice_chord;               /* auto-chord on */
    else if (!strcmp(what, "chord_shape")) *got = (int)voice_chord_shape;   /* 0 triad, 1 7th, 2 power, 3 sus2, 4 sus4, 5 add9 */
    else if (!strcmp(what, "choir")) *got = t->choir;
    else if (!strcmp(what, "sounding")) *got = voice_now.vowel < MUMBLE_VOWELS;
    else if (!strcmp(what, "vowel")) *got = voice_now.vowel;                 /* the loudest sounding, 6: none */
    else if (!strcmp(what, "take")) *got = (int)voice_notes_of(voice_cast);  /* the selected one's notes in the loop */
    else if (!strcmp(what, "takes")) *got = (int)voice_seq_count;           /* all notes in the loop */
    else if (!strcmp(what, "offgrid")) {                                     /* notes not on a 16th (recording quantizes) */
        *got = 0;
        for (unsigned k = 0; k < voice_seq_count; ++k)
            *got += voice_step_ms(voice_seq[k].ms * voice_steps() / voice_loop_ms()) != voice_seq[k].ms &&
                    voice_step_ms(voice_seq[k].ms * voice_steps() / voice_loop_ms() + 1U) != voice_seq[k].ms;
    }
    else if (!strcmp(what, "recording")) *got = (int)voice_recording;
    else if (!strcmp(what, "armed")) *got = (int)voice_rec_armed;            /* REC waiting for the first key */
    else if (!strcmp(what, "metronome")) *got = (int)voice_click;
    else if (!strcmp(what, "sung")) *got = (int)voice_sung;
    else if (!strcmp(what, "pitch_class")) *got = (int)(voice_sung_note % 12U);   /* the last note sung: 0 C .. 11 B */
    else if (!strcmp(what, "typed")) *got = (int)voice_typed();             /* SING: the line typed out so far */
    else if (!strcmp(what, "typed_lag")) {                                  /* how far the typing is behind the syllable sung */
        unsigned n;
        const char *line = voice_lyric_text(&n);
        *got = voice_lyric_started && voice_lyric_count ? (int)voice_lyric_onset(line, voice_lyric_at) - (int)voice_typed() : 0;
    }
    else if (!strcmp(what, "face_top")) *got = (int)voice_portrait.top;     /* the selected one's face: mumble_faces.inc's TOP_* */
    else if (!strcmp(what, "face_body")) *got = (int)voice_portrait.body;   /* BODY_*: 1 ghost, 2 robot ... */
    else if (!strcmp(what, "arp")) *got = (int)voice_arp;
    else if (!strcmp(what, "arp_rate")) *got = (int)voice_arp_rate;          /* 0 1/4 .. 3 1/16 .. 5 1/32 */
    else if (!strcmp(what, "arp_pattern")) *got = (int)voice_arp_pattern;    /* 0 up, 1 down, 2 up-down, 3 random, 4 played */
    else if (!strcmp(what, "arp_octaves")) *got = (int)voice_arp_octaves;
    else if (!strcmp(what, "arp_gate")) *got = (int)voice_arp_gate;
    else if (!strcmp(what, "arp_swing")) *got = (int)voice_arp_swing;
    else if (!strcmp(what, "sung_note")) *got = (int)voice_sung_note;        /* the last note sung (MIDI) */
    else if (!strcmp(what, "scale")) *got = (int)voice_scale;               /* 0 none, 1 C major ... */
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
