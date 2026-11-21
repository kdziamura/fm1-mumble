/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* VOICE page (HOME+ARP): the MUMBLE retro-game voice synthesizer.
 *
 * Every cast member is a track with its own line, sound and effects. Tracks
 * that join the choir speak (or sing) along with the selected one.
 *
 * The 27 note keys are A..Z and SPACE, lowest key first. GLO switches mode:
 *   TYPE  keys type the selected track's line. A vowel speaks the moment it
 *         is typed; consonants shape how the next vowel starts.
 *   PLAY  keys sing their letter's name ("bee", "kay", "em") at the key's
 *         pitch for as long as they are held; SPACE hums. The choir sings
 *         along, each member in their own register.
 *   SAMPLE every key is a pad. Each starts with a word of its letter
 *         ("Aha!", "Bye!", ... "Zzz..."), said by the selected character.
 *         Holding a pad speaks it; letting go, or another pad, cuts it.
 *         Hold SAVE and tap a key to put the selected track's line on it;
 *         hold EDIT and tap a key to give it back its word.
 *
 *   PLAY speak (choir) / stop  OCT- delete       OCT+ CAPS (shout)
 *   FX ,  SCL .  ENV ?  LFO !  EDIT clear line, again on an empty line: all
 *   SAVE join/leave choir      REC loop          ARP demo line
 *   SELECT character (track)   K1/SEQ page: VOICE, FEEL, FX   K2..K5 sound
 *   PRESETS browse demo lines  GLO TYPE/PLAY/SAMPLE  HOME exit
 *   in PLAY, OCT-/OCT+ shift the octave
 *
 * Included by screen.c: uses its paint helpers and draw cache. */
#include "mumble.h"

enum { VOICE_TRACE = 21, VOICE_ROWS = 3, VOICE_COLUMNS = 18 };
#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
enum { WHITE = 0xffff };
static const uint16_t voice_knob_colors[4] = {0x2b7b, 0x2d8d, 0xf7be, 0xf325};
static const uint16_t vowel_colors[MUMBLE_VOWELS] = {0xf325, 0x2d8d, 0x2b7b, 0xf7be, 0xb47f, 0xfe87};
static const char *const voice_page_names[MUMBLE_PAGES + 1] = {"VOICE", "FEEL", "FX", "LOOP"};
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
static const char *const voice_hints[] = {
    "KEYS TYPE  PLAY SPEAK  HOME EXIT",
    "FX ,  SCL .  ENV ?  LFO !  OCT+ CAPS",
    "OCT- DEL  EDIT CLEAR  ARP DEMO LINE",
    "SAVE CHOIR  REC LOOP  GLO MODE",
    "SELECT WHO  K1 PAGE  PRESETS LINES",
    "K1 TO LOOP: START LEVEL BARS BPM"};
static const char *const voice_sample_hints[] = {
    "HOLD A KEY: ITS WORD SPEAKS",
    "SAVE+KEY: PUT THE LINE ON A KEY",
    "EDIT+KEY: BACK TO ITS WORD  GLO TYPE"};
/* SAMPLE mode's built-in pads: a word per key that starts with (or sounds
 * like) its letter, spoken by the selected character. SPACE muses. */
static const char *const voice_pad_words[] = {
    "Aha!", "Bye!", "Come on!", "Danger!", "Eh?", "Follow me!", "Go!", "Hey!", "I see.",
    "Jump!", "Kaboom!", "Look!", "Mmm...", "No!", "Oh!", "Please?", "Quiet!", "Run!",
    "Sorry.", "Thanks!", "Uh-oh.", "Very good!", "What?", "Excuse me.", "Yes!", "Zzz...", "Hmm..."};
_Static_assert(sizeof voice_pad_words / sizeof voice_pad_words[0] == MUMBLE_LETTERS, "a word per key");
static const char *const voice_play_hints[] = {
    "HOLD KEYS TO SING  GLO TYPE MODE",
    "OCT- OCT+ OCTAVE  SELECT WHO",
    "SAVE: CHOIR SINGS ALONG  HOME EXIT"};

/* A track: one character's line, sound and place in the loop. The plan
 * (reveal times and syllables) is kept for the dialogue box and the lanes. */
enum { VOICE_PAGES = MUMBLE_PAGES + 1, LOOP_PAGE = MUMBLE_PAGES };
struct voice_track {
    char text[MUMBLE_TEXT + 1];
    uint8_t length, choir, start, level;     /* start: 16th step in the loop; level: percent */
    struct mumble_voice sound;
    uint8_t planned, syllables;
    uint16_t end_ms, char_ms[MUMBLE_TEXT], syllable_ms[MUMBLE_SYLLABLES], syllable_dur[MUMBLE_SYLLABLES];
};
static struct voice_track voice_tracks[MUMBLE_CAST];
static unsigned voice_cast, voice_page, voice_caps, voice_demo, voice_ready;
/* The song: REC loops it; BARS and BPM size the loop. song is 1 when
 * published, 2 once it was heard (the audio task adopts it a block later). */
static unsigned voice_loop, voice_song, voice_song_dirty, voice_bars = 2, voice_bpm = 120;
static unsigned voice_fm1_playing, voice_lanes_version;
static unsigned voice_talking, voice_shown;      /* the selected track in the song */
static struct mumble_status voice_now;
static struct { uint8_t vowel, level, track; int16_t cents; } voice_trace[VOICE_TRACE];
static uint32_t voice_trace_ms;
static uint32_t voice_keys_lit;
/* PLAY mode: held keys, octave shift, and the last letter sung. */
static unsigned voice_play, voice_sung = MUMBLE_LETTERS, voice_sung_note;
static int voice_octave;
static uint32_t voice_held;
/* SAMPLE mode: a pad per key holds a copy of a track's line and sound;
 * an empty pad (length 0) speaks its built-in word instead. */
struct voice_pad { uint8_t cast, length; char text[MUMBLE_TEXT]; struct mumble_voice sound; };
static struct voice_pad voice_pads[MUMBLE_LETTERS];
static unsigned voice_sample, voice_pad_on = MUMBLE_LETTERS, voice_pad_last = MUMBLE_LETTERS;

static struct voice_track *voice_track(void) { return &voice_tracks[voice_cast]; }
/* What a pad says and who says it: its own line, or its word by the selected character. */
static unsigned voice_pad_says(unsigned key, const char **text, unsigned *cast)
{
    const struct voice_pad *pad = &voice_pads[key];
    if (pad->length) { *text = pad->text; *cast = pad->cast; return pad->length; }
    *text = voice_pad_words[key];
    *cast = voice_cast;
    return (unsigned)strlen(voice_pad_words[key]);
}
static unsigned voice_in_choir(unsigned i) { return i == voice_cast || voice_tracks[i].choir; }
/* Something about this track changed: replan it and refresh a playing song. */
static void voice_touch(struct voice_track *t)
{
    t->planned = 0;
    ++voice_lanes_version;
    if (voice_song) voice_song_dirty = 1;
}
static void voice_plan(struct voice_track *t)
{
    if (t->planned) return;
    struct mumble_syllable plan[MUMBLE_SYLLABLES];
    uint16_t end = 0;
    unsigned n = mumble_plan(t->text, t->length, &t->sound, 0, plan, MUMBLE_SYLLABLES, t->char_ms, &end);
    for (unsigned i = 0; i < n; ++i) { t->syllable_ms[i] = plan[i].ms; t->syllable_dur[i] = plan[i].dur_ms; }
    t->syllables = (uint8_t)n;
    t->end_ms = end;
    t->planned = 1;
}
static void voice_set_text(struct voice_track *t, const char *s)
{
    unsigned n = (unsigned)strlen(s);
    if (n > MUMBLE_TEXT) n = MUMBLE_TEXT;
    memcpy(t->text, s, n);
    t->text[n] = 0;
    t->length = (uint8_t)n;
    voice_touch(t);
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

/* Put the selected track and every choir member with a line on the loop. */
static void voice_publish(unsigned restart, uint32_t phase_ms)
{
    struct mumble_part parts[MUMBLE_CAST];
    unsigned any = 0;
    memset(parts, 0, sizeof parts);
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
        struct voice_track *t = &voice_tracks[i];
        if (!voice_in_choir(i) || !t->length) continue;
        parts[i] = (struct mumble_part){t->text, t->length, &t->sound, (uint16_t)voice_step_ms(t->start), t->level};
        any = 1;
    }
    voice_song_dirty = 0;
    if (!any) { mumble_song_stop(); voice_song = 0; return; }
    mumble_song(parts, voice_loop ? voice_loop_ms() : 0, restart, phase_ms);
    if (restart || !voice_song) voice_song = 1;
}
/* Start the song; with the FM1 sequencer playing, join it at its step. */
static void voice_start(void)
{
    struct voice_track *t = voice_track();
    if (!t->length) voice_set_text(t, voice_lines[voice_demo % VOICE_LINES]);
    uint32_t phase = voice_fm1_playing ? voice_step_ms(frame_views[0].play_step % voice_steps()) : 0;
    voice_publish(1, phase);
}
static void voice_stop(void)
{
    mumble_stop();
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
            voice_set_text(t, "");
        }
        voice_cast = 0;
        if (frame_views[0].tempo) voice_bpm = frame_views[0].tempo;
        voice_ready = 1;
    }
    voice_page = 0;
    voice_play = 0;
    voice_sample = 0;
    voice_pad_on = voice_pad_last = MUMBLE_LETTERS;
    voice_held = 0;
    voice_song = 0;
    memset(voice_trace, 0, sizeof voice_trace);
    for (unsigned i = 0; i < VOICE_TRACE; ++i) voice_trace[i].vowel = MUMBLE_VOWELS;
}
static void voice_close(void)
{
    if (voice_ready) mumble_stop(); /* also releases held letters */
    voice_held = 0;
    voice_song = 0;
}
static void voice_type(char c)
{
    struct voice_track *t = voice_track();
    if (t->length >= MUMBLE_TEXT) {
        memmove(t->text, t->text + 1, MUMBLE_TEXT - 1);
        t->length = MUMBLE_TEXT - 1;
    }
    t->text[t->length++] = c;
    t->text[t->length] = 0;
    voice_touch(t);
    /* The live lane plays it now; a looping song picks it up as it goes. */
    mumble_type(voice_cast, t->text, t->length, &t->sound);
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
        ++voice_lanes_version;
        if (voice_song) voice_song_dirty = 1;
        return;
    }
    unsigned p = voice_parameter(col);
    const struct mumble_param_info *info = &mumble_params[p];
    t->sound.value[p] = (uint16_t)clamp((int)t->sound.value[p] + delta, info->min, info->max);
    voice_touch(t);
}
static unsigned voice_edited(void)
{
    return memcmp(&voice_track()->sound, &mumble_cast[voice_cast].voice, sizeof(struct mumble_voice)) != 0;
}
static void voice_release_all(void)
{
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key)
        if (voice_held & (1U << key)) mumble_release(key);
    voice_held = 0;
}
/* PLAY mode: the selected track and the choir sing each pressed key. */
static void voice_sing(const struct fm1_controls *c, uint32_t down)
{
    uint32_t up = voice_held & ~c->keys;
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key)
        if (up & (1U << key)) mumble_release(key);
    voice_held &= c->keys;
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key) {
        if (!(down & (1U << key))) continue;
        struct mumble_singer singers[MUMBLE_CAST];
        unsigned n = 0;
        int note = 0;
        for (unsigned i = 0; i < MUMBLE_CAST; ++i) {
            /* The selected voice leads. */
            unsigned who = (voice_cast + i) % MUMBLE_CAST;
            if (!voice_in_choir(who)) continue;
            const struct mumble_voice *v = &voice_tracks[who].sound;
            int pitch = clamp(53 + (int)key + 12 * (voice_octave + mumble_register(v)), 0, 127); /* lowest key is F3 */
            if (!n) note = pitch;
            singers[n].voice = v;
            singers[n].track = (uint8_t)who;
            singers[n].note = (uint8_t)pitch;
            ++n;
        }
        mumble_press(key, singers, n, voice_caps);
        voice_held |= 1U << key;
        voice_sung = key;
        voice_sung_note = (unsigned)note;
    }
}

static void voice_notice(const char *message)
{
    notice = message;
    notice_until = frame_time + 1500U;
}
/* SAMPLE mode: SAVE+key fills a pad, EDIT+key puts its word back, a key
 * alone speaks its pad until it is let go or another pad starts. */
static void voice_sampler(const struct fm1_controls *c, uint32_t down)
{
    if (voice_pad_on < MUMBLE_LETTERS && !(c->keys & (1U << voice_pad_on))) {
        mumble_cut();
        voice_pad_on = MUMBLE_LETTERS;
    }
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key) {
        if (!(down & (1U << key))) continue;
        struct voice_pad *pad = &voice_pads[key];
        const struct voice_track *t = voice_track();
        voice_pad_last = key;
        if (c->buttons & BUTTON(SAVE)) {
            if (!t->length) { voice_notice("TYPE A LINE FIRST"); continue; }
            pad->cast = (uint8_t)voice_cast;
            pad->length = t->length;
            memcpy(pad->text, t->text, t->length);
            pad->sound = t->sound;
            voice_notice("LINE PUT ON THE KEY");
        } else if (c->buttons & BUTTON(EDIT)) {
            pad->length = 0;
            voice_notice("KEY BACK TO ITS WORD");
        } else {
            const char *line;
            unsigned cast, length = voice_pad_says(key, &line, &cast);
            mumble_cut();
            mumble_aside(cast, line, length, pad->length ? &pad->sound : &t->sound);
            voice_pad_on = key;
        }
    }
}
static void voice_mode_next(void)
{
    voice_release_all();
    if (voice_pad_on < MUMBLE_LETTERS) mumble_cut();
    voice_pad_on = voice_pad_last = MUMBLE_LETTERS;
    if (voice_play) { voice_play = 0; voice_sample = 1; }
    else if (voice_sample) voice_sample = 0;
    else voice_play = 1;
}
/* Who the stage shows: in SAMPLE mode, the character of the pad last played. */
static unsigned voice_stage_cast(void)
{
    if (voice_sample && voice_pad_last < MUMBLE_LETTERS && voice_pads[voice_pad_last].length)
        return voice_pads[voice_pad_last].cast;   /* a word pad is the selected character's */
    return voice_cast;
}

/* Returns 0 when a MIDI post must be retried, as apply_controls does. */
static int voice_controls(const struct fm1_controls *c, uint32_t pressed, uint32_t key_down)
{
    if (pressed & BUTTON(HOME)) { voice_close(); page = OVERVIEW; return 1; }
    if (c->encoders[0]) {
        voice_release_all();
        voice_pad_last = MUMBLE_LETTERS;
        unsigned was = voice_cast;
        voice_cast = (unsigned)((int)voice_cast + c->encoders[0] % MUMBLE_CAST + MUMBLE_CAST) % MUMBLE_CAST;
        const char *hello = voice_greetings[voice_cast];
        mumble_aside(voice_cast, hello, (unsigned)strlen(hello), &voice_track()->sound);
        /* The selected track always plays: a new selection changes the song. */
        if (voice_song && (!voice_tracks[was].choir || !voice_track()->choir)) voice_song_dirty = 1;
        ++voice_lanes_version;
    }
    if (c->encoders[1]) voice_page = (unsigned)((int)voice_page + c->encoders[1] % VOICE_PAGES + VOICE_PAGES) % VOICE_PAGES;
    if (pressed & BUTTON(SEQ)) voice_page = (voice_page + 1U) % VOICE_PAGES;
    for (unsigned i = 0; i < 4; ++i)
        if (c->encoders[2 + i]) voice_edit(i, fm_encoder_delta(2 + i, c->encoders[2 + i]));
    if (c->encoders[6]) {
        voice_demo = (unsigned)((int)voice_demo + c->encoders[6] % (int)VOICE_LINES + (int)VOICE_LINES) % VOICE_LINES;
        voice_set_text(voice_track(), voice_lines[voice_demo]);
    }
    if (pressed & BUTTON(GLO)) voice_mode_next();
    /* In SAMPLE mode SAVE and EDIT are held with a key instead. */
    if ((pressed & BUTTON(SAVE)) && !voice_sample) {
        struct voice_track *t = voice_track();
        t->choir ^= 1U;
        voice_notice(t->choir ? "JOINED THE CHOIR" : "LEFT THE CHOIR");
        voice_touch(t);
    }
    if (pressed & BUTTON(REC)) {
        voice_loop ^= 1U;
        if (voice_song) voice_song_dirty = 1;   /* loop on: the song keeps going; off: it ends this pass */
    }
    if (voice_play) {
        if ((pressed & BUTTON(OCT_UP)) && voice_octave < 2) ++voice_octave;
        if ((pressed & BUTTON(OCT_DOWN)) && voice_octave > -2) --voice_octave;
    } else if (!voice_sample) {
        struct voice_track *t = voice_track();
        if (pressed & BUTTON(OCT_UP)) voice_caps ^= 1U;
        if ((pressed & BUTTON(OCT_DOWN)) && t->length) {
            t->text[--t->length] = 0;
            voice_touch(t);
        }
    }
    if ((pressed & BUTTON(EDIT)) && !voice_sample) {
        /* Clear the line; on an empty line, clear every track. */
        struct voice_track *t = voice_track();
        if (t->length) voice_set_text(t, "");
        else {
            for (unsigned i = 0; i < MUMBLE_CAST; ++i) voice_set_text(&voice_tracks[i], "");
            voice_notice("ALL LINES CLEARED");
            voice_stop();
        }
    }
    if (pressed & BUTTON(ARP)) {
        voice_demo = (voice_demo + 1U) % VOICE_LINES;
        voice_set_text(voice_track(), voice_lines[voice_demo]);
        if (!voice_song) voice_start();
    }
    static const uint8_t marks[4][2] = {{FX, ','}, {SCL, '.'}, {ENV, '?'}, {LFO, '!'}};
    for (unsigned i = 0; i < 4 && !voice_play && !voice_sample; ++i)
        if (pressed & BUTTON(marks[i][0])) voice_type((char)marks[i][1]);
    uint32_t typed = key_down & ~blocked_keys;
    if (voice_play) voice_sing(c, typed);
    else if (voice_sample) voice_sampler(c, typed);
    else
        for (unsigned key = 0; key < 27; ++key)
            if (typed & (1U << key))
                voice_type(key == 26 ? ' ' : (char)((voice_caps ? 'A' : 'a') + key));
    voice_keys_lit = c->keys;
    if (pressed & BUTTON(PLAY)) {
        if (voice_song) voice_stop();
        else voice_start();
    }
    return 1;
}

/* Note-key LEDs mark the vowels; held keys invert theirs. */
static uint32_t voice_led_bits(void)
{
    uint32_t lit = (1U << 0) | (1U << 4) | (1U << 8) | (1U << 14) | (1U << 20) | (1U << 24);
    if (voice_sample) {                     /* SAMPLE: the filled pads */
        lit = 0;
        for (unsigned key = 0; key < MUMBLE_LETTERS; ++key) lit |= (uint32_t)(voice_pads[key].length != 0) << key;
    }
    return lit ^ (voice_keys_lit & ((1U << 27) - 1U));
}
static uint32_t voice_button_bits(void)
{
    uint32_t octave = voice_play ? ((voice_octave > 0) << OCT_UP) | ((voice_octave < 0) << OCT_DOWN)
                                 : voice_caps << OCT_UP;
    return ((voice_song != 0) << PLAY) | octave | ((voice_play | voice_sample) << GLO) |
           ((uint32_t)voice_track()->choir << SAVE) | (voice_loop << REC) | (1U << ARP);
}

/* Where the selected track is in the song: talking, and how much shows. */
static void voice_reveal(void)
{
    struct voice_track *t = voice_track();
    voice_plan(t);
    voice_talking = 0;
    voice_shown = t->length;
    if (voice_song != 2 || voice_now.song_ms < 0 || !t->length) return;
    int32_t at = voice_now.song_ms - (int32_t)voice_step_ms(t->start);
    if (voice_loop && at < 0) at += (int32_t)voice_loop_ms();   /* still in the last pass */
    if (at < 0 || at >= t->end_ms) { voice_shown = 0; return; }
    unsigned shown = 0;
    while (shown < t->length && t->char_ms[shown] <= (uint32_t)at) ++shown;
    voice_talking = 1;
    voice_shown = shown;
}

/* Control-side snapshot, taken once per frame before marking damage: song
 * state, FM1 sync, edits to republish, and the pitch trace. */
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
    if (playing && !voice_fm1_playing && voice_song && voice_loop) { voice_fm1_playing = 1; voice_publish(1, 0); }
    voice_fm1_playing = playing;
    if (voice_tempo() != last_tempo) {
        last_tempo = voice_tempo();
        ++voice_lanes_version;
        if (voice_song) voice_song_dirty = 1;
    }
    if (voice_song_dirty && voice_song) voice_publish(0, 0);
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) voice_plan(&voice_tracks[i]);
    voice_reveal();
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
static const uint8_t vowel_mouth[MUMBLE_VOWELS][2] = {{4, 3}, {4, 2}, {4, 1}, {3, 3}, {2, 2}, {3, 2}};

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
    uint32_t sig = voice_cast | (voice_page << 4) | (voice_caps << 6) | (voice_edited() << 7) |
                   (voice_play << 8) | (voice_loop << 9) | (voice_sample << 11) | ((uint32_t)voice_track()->choir << 10);
    if (cache->voice_cache[0] == sig) return;
    cache->voice_cache[0] = sig;
    paint_fill(0, 0, 240, 28, BACK);
    text(8, 7, "MUMBLE", 2, TEXT, BACK);
    for (unsigned i = 0; i < 4; ++i) paint_fill(86 + i * 7, 6, 5, 5, voice_knob_colors[i]);
    if (voice_loop) text(120, 6, "LOOP", 1, ACCENT, BACK);
    text(86, 16, voice_page_names[voice_page], 1, DIM, BACK);
    if (voice_play) text(120, 16, "PLAY", 1, voice_caps ? AMBER : GREEN, BACK);
    else if (voice_sample) text(120, 16, "SAMPLE", 1, ACCENT, BACK);
    else if (voice_caps) text(120, 16, "CAPS", 1, AMBER, BACK);
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
    const unsigned who = voice_stage_cast();
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
    struct { int lift; unsigned open, vowel, blink, shout, cast, tick; } sig_in = {
        lift, p.open, vowel, p.blink, p.shout, who, p.tick & 24U};
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
                             ((i == voice_cast) << 3) | ((voice_tracks[i].length != 0) << 4));
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
        if (voice_tracks[i].length) vfill(x + 17, STRIP_TOP + 2, 2, 2, voice_in_choir(i) ? TEXT : DIM);
    }
}

/* Word-wrap the visible text into rows of VOICE_COLUMNS; keep the last rows. */
static unsigned voice_rows(const char *s, unsigned n, char rows[VOICE_ROWS][VOICE_COLUMNS + 1])
{
    char all[MUMBLE_TEXT / 4 + 8][VOICE_COLUMNS + 1];
    unsigned count = 0, used = 0;
    memset(all, 0, sizeof all);
    for (unsigned i = 0; i < n && count < sizeof all / sizeof all[0];) {
        unsigned word = i;
        while (word < n && s[word] != ' ') ++word;
        unsigned len = word - i;
        if (used && used + len > VOICE_COLUMNS) { ++count; used = 0; }
        if (count >= sizeof all / sizeof all[0]) break;
        for (; i < word; ++i) {
            if (used == VOICE_COLUMNS) { ++count; used = 0; if (count >= sizeof all / sizeof all[0]) break; }
            all[count][used++] = s[i];
        }
        if (i < n && s[i] == ' ') {
            if (used < VOICE_COLUMNS) all[count][used++] = ' ';
            ++i;
        }
    }
    unsigned total = count + 1U, first = total > VOICE_ROWS ? total - VOICE_ROWS : 0U;
    for (unsigned r = 0; r < VOICE_ROWS; ++r)
        memcpy(rows[r], first + r < total ? all[first + r] : all[sizeof all / sizeof all[0] - 1], VOICE_COLUMNS + 1);
    return total - first;
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
    const struct voice_track *t = voice_track();
    unsigned shown = voice_shown > t->length ? t->length : voice_shown;
    char rows[VOICE_ROWS][VOICE_COLUMNS + 1];
    unsigned used = voice_rows(t->text, shown, rows);
    unsigned cursor_on = voice_talking || (frame_time / 530U) % 2U == 0U;
    for (unsigned r = 0; r < VOICE_ROWS; ++r) {
        unsigned y = 128U + r * 19U;
        unsigned cursor_col = r + 1U == used ? (unsigned)strlen(rows[r]) : ~0U;
        uint32_t sig = signature(cursor_on | (shown == 0) << 1 | (voice_talking << 2) | (voice_cast << 3),
                                 rows[r], VOICE_COLUMNS) ^ cursor_col;
        if (cache->voice_cache[3 + r] == sig) continue;
        cache->voice_cache[3 + r] = sig;
        paint_fill(4, y - 2, 232, 18, BACK);
        if (!shown && r == 0) {
            text(10, y + 3, t->length ? "* ..." : "* TYPE ON THE KEYS...", 1, DIM, BACK);
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
            text(10U + i * 12U, y, glyph, 2, letters > 1 && upper == letters ? AMBER : TEXT, BACK);
        }
        if (cursor_col < VOICE_COLUMNS && cursor_on)
            paint_fill(10U + cursor_col * 12U, y, 9, 14, voice_talking ? mumble_cast[voice_cast].color : AMBER);
    }
}

/* PLAY mode: the letter being sung, its note and how it is said. */
static void voice_play_draw(void)
{
    if (paint_rendering && (paint_top >= BOX_BOTTOM - 2 || paint_top + PAINT_ROWS <= BOX_TOP + 2)) return;
    unsigned held = voice_sung < MUMBLE_LETTERS && (voice_held & (1U << voice_sung)) != 0;
    unsigned singers = 0;
    for (unsigned i = 0; i < MUMBLE_CAST; ++i) singers += voice_in_choir(i);
    uint32_t sig = 0x50000000U | voice_sung | (voice_sung_note << 5) | ((unsigned)(voice_octave + 2) << 12) |
                   (held << 15) | (voice_caps << 16) | (voice_cast << 17) | (singers << 20);
    /* The TYPE rows share this area: make them redraw on return. */
    cache->voice_cache[4] = cache->voice_cache[5] = 0xfffffffeU;
    if (cache->voice_cache[3] == sig) return;
    cache->voice_cache[3] = sig;
    paint_fill(4, BOX_TOP + 2, 232, BOX_BOTTOM - BOX_TOP - 4, BACK);
    if (voice_sung >= MUMBLE_LETTERS) {
        text(10, 131, "* HOLD THE KEYS TO SING", 1, DIM, BACK);
    } else {
        uint16_t color = !held ? DIM : voice_caps ? AMBER : mumble_cast[voice_cast].color;
        char big[2] = {voice_sung < 26 ? (char)('A' + voice_sung) : '~', 0};
        char name[12];
        text(12, 128, big, 4, color, BACK);
        note_name(voice_sung_note, name);
        text(46, 128, name, 2, held ? TEXT : DIM, BACK);
        text(46, 146, mumble_letter_names[voice_sung], 2, color, BACK);
    }
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
}

/* SAMPLE mode: the 27 pads as three rows of nine keys, the ones holding a
 * line marked in their character's colour, the sounding one filled; then
 * who says the last pad and what. */
static void voice_sample_draw(void)
{
    if (paint_rendering && (paint_top >= BOX_BOTTOM - 2 || paint_top + PAINT_ROWS <= BOX_TOP + 2)) return;
    uint8_t state[MUMBLE_LETTERS + 3];
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key)
        state[key] = voice_pads[key].length ? (uint8_t)(1U + voice_pads[key].cast) : 0U;
    state[MUMBLE_LETTERS] = (uint8_t)voice_pad_on;
    state[MUMBLE_LETTERS + 1] = (uint8_t)voice_pad_last;
    state[MUMBLE_LETTERS + 2] = (uint8_t)voice_cast;
    uint32_t sig = signature(0x5A3D1E, state, sizeof state);
    if (voice_pad_last < MUMBLE_LETTERS && voice_pads[voice_pad_last].length)
        sig = signature(sig, voice_pads[voice_pad_last].text, voice_pads[voice_pad_last].length);
    cache->voice_cache[4] = cache->voice_cache[5] = 0xfffffffeU;   /* the TYPE rows redraw on return */
    if (cache->voice_cache[3] == sig) return;
    cache->voice_cache[3] = sig;
    paint_fill(4, BOX_TOP + 2, 232, BOX_BOTTOM - BOX_TOP - 4, BACK);
    for (unsigned key = 0; key < MUMBLE_LETTERS; ++key) {
        const struct voice_pad *pad = &voice_pads[key];
        unsigned x = 10U + (key % 9U) * 25U, y = 127U + (key / 9U) * 15U;
        unsigned cast = pad->length ? pad->cast : voice_cast;
        uint16_t color = pad->length ? mumble_cast[cast].color : TEXT;
        char letter[2] = {key < 26 ? (char)('A' + key) : '~', 0};
        if (key == voice_pad_on) {
            color = mumble_cast[cast].color;
            paint_fill(x, y, 21, 12, color);
            text(x + 3, y + 3, letter, 1, BACK, color);
        } else {
            text(x + 3, y + 3, letter, 1, color, BACK);
            if (pad->length) paint_fill(x + 11, y + 4, 7, 5, color);
            if (key == voice_pad_last) paint_fill(x, y + 11, 21, 1, TEXT);
        }
    }
    if (voice_pad_last < MUMBLE_LETTERS) {
        const char *says;
        unsigned cast, said = voice_pad_says(voice_pad_last, &says, &cast);
        char line[38];
        unsigned n = (unsigned)strlen(mumble_cast[cast].name);
        text(10, 174, mumble_cast[cast].name, 1, mumble_cast[cast].color, BACK);
        unsigned room = 36U - n, length = said < room ? said : room;
        memcpy(line, says, length);
        line[length] = 0;
        text(10U + (n + 1U) * 6U, 174, line, 1, TEXT, BACK);
    } else {
        text(10, 174, "HOLD A KEY  SAVE+KEY PUTS THE LINE ON", 1, DIM, BACK);
    }
}

/* LOOP page: eight lanes over one loop, OP-1 tape style. Each lane shows
 * its track's syllables from its offset, bright when it is in the song;
 * the playhead sweeps while the song plays. */
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
        unsigned on = voice_in_choir(i) && t->length;
        uint16_t color = on ? mumble_cast[i].color : dimmer(mumble_cast[i].color);
        if (i == voice_cast) vfill(6, y, 2, lane_h - 1, WHITE);
        vfill(12, y + 1, 7, lane_h - 3, color);                  /* the track's colour chip */
        vfill(x0, y + 3, x1 - x0, 1, PANEL);
        uint32_t offset = voice_step_ms(t->start);
        vfill(x0 + (int)(offset * (uint32_t)(x1 - x0) / loop), y, 1, lane_h - 1, color);
        for (unsigned k = 0; k < t->syllables; ++k) {
            uint32_t at = offset + t->syllable_ms[k];
            if (at >= loop) { if (!voice_loop) break; at -= loop; }
            if (at >= loop) break;   /* longer than the loop: the engine cuts it too */
            if (offset + t->syllable_ms[k] >= offset + loop) break;
            int x = x0 + (int)(at * (uint32_t)(x1 - x0) / loop);
            int w = (int)(t->syllable_dur[k] * (uint32_t)(x1 - x0) / loop);
            if (w < 2) w = 2;
            if (x + w > x1) w = x1 - x;
            vfill(x, y + 1, w, lane_h - 3, color);
        }
    }
    if (head >= 0) vfill(head, lane_top - 1, 1, 8 * lane_h + 1, WHITE);
}

static void voice_knobs(void)
{
    if (voice_page == LOOP_PAGE) {
        const struct voice_track *t = voice_track();
        char start[12], level[12], bars[12], bpm[12];
        /* START as bar.step, like the FM1 step pages. */
        char bar[12], step[12];
        decimal(bar, t->start / 16U + 1U, 1);
        decimal(step, t->start % 16U + 1U, 2);
        strcpy(start, bar); strcat(start, "."); strcat(start, step);
        decimal(level, t->level, 1);
        decimal(bars, (int)voice_bars, 1);
        decimal(bpm, (int)voice_tempo(), 1);
        knob(0, "START", start, voice_knob_colors[0]);
        knob(1, "LEVEL", level, voice_knob_colors[1]);
        knob(2, "BARS", bars, voice_knob_colors[2]);
        knob(3, voice_fm1_playing ? "SYNC" : "BPM", bpm, voice_knob_colors[3]);
        return;
    }
    for (unsigned col = 0; col < 4; ++col) {
        unsigned p = voice_parameter(col);
        char value[12];
        decimal(value, voice_track()->sound.value[p], 1);
        knob(col, mumble_params[p].name, value, voice_knob_colors[col]);
    }
}

static __attribute__((noinline)) void voice_draw(void)
{
    voice_header();
    voice_face();
    voice_roll();
    voice_choir_strip();
    voice_box();
    if (voice_page == LOOP_PAGE) voice_lanes_draw();
    else if (voice_play) voice_play_draw();
    else if (voice_sample) voice_sample_draw();
    else voice_lines_draw();
    voice_knobs();
    if (paint_rendering && paint_top + PAINT_ROWS <= 229) return;
    const char *hint = notice ? notice
                     : voice_play ? voice_play_hints[(frame_time / 2600U) % (sizeof voice_play_hints / sizeof voice_play_hints[0])]
                     : voice_sample ? voice_sample_hints[(frame_time / 2600U) % (sizeof voice_sample_hints / sizeof voice_sample_hints[0])]
                                  : voice_hints[(frame_time / 2600U) % (sizeof voice_hints / sizeof voice_hints[0])];
    uint32_t hint_sig = signature(1, hint, (unsigned)strlen(hint));
    if (hint_sig != cache->hint_cache) {
        cache->hint_cache = hint_sig;
        paint_fill(6, 231, 228, 7, BACK);
        text(6, 231, hint, 1, DIM, BACK);
    }
}
