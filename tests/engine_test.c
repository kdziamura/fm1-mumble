/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Host test for app/mumble.c: text analysis checks plus a block-by-block
 * render through mumble_render, exactly as fm1_output_mix calls it.
 *
 *   cc -std=c11 -O2 -DFM1_HOST_TEST -Iapp -Iapp/synth -Isdk/include \
 *      app/mumble.c tools/mumble-test.c -o mumble-test && ./mumble-test [wav-dir]
 */
#include "mumble.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned plan(const char *text, unsigned open, struct mumble_syllable *s)
{
    return mumble_plan(text, (unsigned)strlen(text), &mumble_cast[0].voice, open, s, MUMBLE_SYLLABLES, NULL, NULL);
}
static void vowels(const char *word, const char *expected)
{
    struct mumble_syllable s[MUMBLE_SYLLABLES];
    unsigned n = plan(word, 0, s);
    char got[64] = "";
    for (unsigned i = 0; i < n; ++i) {
        if (i) strcat(got, " ");
        strcat(got, mumble_vowel_names[s[i].vowel]);
    }
    CHECK(!strcmp(got, expected), "%s -> '%s', expected '%s'", word, got, expected);
}

static void write_wav(const char *path, const float *x, unsigned n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    unsigned rate = FM1_SAMPLE_RATE, bytes = n * 2;
    unsigned char h[45] = "RIFF....WAVEfmt \x10\0\0\0\1\0\1\0........\2\0\x10\0data....";
#define PUT32(o, v) do { h[o] = (unsigned char)(v); h[o+1] = (unsigned char)((v) >> 8); h[o+2] = (unsigned char)((v) >> 16); h[o+3] = (unsigned char)((v) >> 24); } while (0)
    PUT32(4, 36 + bytes); PUT32(24, rate); PUT32(28, rate * 2); PUT32(40, bytes);
    fwrite(h, 1, 44, f);
    for (unsigned i = 0; i < n; ++i) {
        float v = x[i] * 32767.0f;
        short s = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    vowels("Sister", "EE EH");
    vowels("Agnes", "AH EH");
    vowels("moon", "OO");
    vowels("make", "AH");   /* silent final e */
    vowels("the", "EH");
    vowels("yes", "EH");    /* word-initial y is a consonant */
    vowels("happy", "AH EE");
    vowels("hmm", "UH");    /* no vowel: one closed hum */

    struct mumble_syllable s[MUMBLE_SYLLABLES];
    const char *line = "Well, hello there! Have you seen Sister Agnes? She went... somewhere.";
    uint16_t end = 0, at[MUMBLE_TEXT];
    unsigned n = mumble_plan(line, (unsigned)strlen(line), &mumble_cast[0].voice, 0, s, MUMBLE_SYLLABLES, at, &end);
    CHECK(n >= 14 && n <= 20, "syllables %u", n);
    float rate = (float)n * 1000.0f / end;
    CHECK(rate > 4 && rate < 14, "syllables per second %.1f", rate);
    CHECK(at[1] - at[0] >= 30 && at[1] - at[0] <= 36, "letter step %u ms", at[1] - at[0]);
    CHECK(at[5] - at[4] >= 3 * (at[1] - at[0]) - 2, "comma pause");

    n = plan("Have you seen her?", 0, s);
    CHECK(s[n - 1].cents > s[0].cents + 200, "question rises %d -> %d", s[0].cents, s[n - 1].cents);
    n = plan("well, I suppose so...", 0, s);
    CHECK(s[n - 1].gain < 40, "ellipsis fades: gain %u", s[n - 1].gain);
    n = plan("stop RIGHT there", 0, s);
    CHECK((s[1].flags & MUMBLE_SHOUT) && s[1].gain > s[0].gain, "caps shout");
    n = plan("make", 1, s);
    CHECK(n == 2, "open word keeps its final e (%u)", n);

    /* Render each character through the audio path. */
    mumble_init();
    const char *dir = argc > 1 ? argv[1] : NULL;
    for (unsigned c = 0; c < MUMBLE_CAST; ++c) {
        mumble_say(c, line, (unsigned)strlen(line), &mumble_cast[c].voice);
        unsigned blocks = (unsigned)((end / 1000.0f / (mumble_cast[c].voice.value[MUMBLE_SPEED] / 100.0f) + 0.5f) * FM1_SAMPLE_RATE / FM1_BLOCK_FRAMES);
        float *pcm = calloc((size_t)blocks * FM1_BLOCK_FRAMES, sizeof *pcm);
        double sum = 0; float peak = 0; int32_t reached = -1; unsigned sounding = 0;
        for (unsigned b = 0; b < blocks; ++b) {
            int32_t block[FM1_BLOCK_WORDS] = {0};
            mumble_render(block);
            for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
                CHECK(block[2 * i] == block[2 * i + 1], "mono voice");
                float x = (float)block[2 * i] / 8388608.0f;
                pcm[b * FM1_BLOCK_FRAMES + i] = x;
                sum += x * x; if (fabsf(x) > peak) peak = fabsf(x);
            }
            struct mumble_status st; mumble_status(&st);
            if (st.song_ms > reached) reached = st.song_ms;
            sounding += st.vowel < MUMBLE_VOWELS;
        }
        unsigned frames = blocks * FM1_BLOCK_FRAMES;
        float rms = (float)sqrt(sum / frames);
        printf("%-7s peak %.2f rms %.3f  %.2fs  song reached %d ms  voiced blocks %u\n", mumble_cast[c].name, peak, rms,
               (float)frames / FM1_SAMPLE_RATE, (int)reached, sounding);
        CHECK(peak > 0.1f && peak < 0.8f, "%s peak %.2f", mumble_cast[c].name, peak);
        int32_t expected = (int32_t)(end * 100U / mumble_cast[c].voice.value[MUMBLE_SPEED]);
        CHECK(reached > expected - 150, "%s: the song clock runs through the line (%d of %d ms)", mumble_cast[c].name, (int)reached, (int)expected);
        if (dir) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s.wav", dir, mumble_cast[c].name);
            write_wav(path, pcm, frames);
        }
        free(pcm);
    }

    /* Live typing: a vowel speaks at once, a consonant does not. */
    mumble_stop();
    for (unsigned b = 0; b < 200; ++b) { int32_t z[FM1_BLOCK_WORDS] = {0}; mumble_render(z); }
    CHECK(mumble_type(0, "h", 1, &mumble_cast[0].voice) == 0, "consonant is silent");
    CHECK(mumble_type(0, "ha", 2, &mumble_cast[0].voice) == 1, "vowel speaks");
    int32_t z[FM1_BLOCK_WORDS] = {0};
    int any = 0;
    for (unsigned b = 0; b < 8; ++b) { memset(z, 0, sizeof z); mumble_render(z); for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) any |= z[i] != 0; }
    CHECK(any, "typed syllable is audible within 25 ms");

    /* Play mode: a held letter sustains, a released one stops, chords ring. */
    CHECK(mumble_register(&mumble_cast[0].voice) == -1, "HERO plays an octave down (%d)", mumble_register(&mumble_cast[0].voice));
    CHECK(mumble_register(&mumble_cast[3].voice) == 0, "KID plays at pitch (%d)", mumble_register(&mumble_cast[3].voice));
    for (unsigned b = 0; b < 200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    float window[3] = {0};
    const unsigned spans[3][2] = {{430, 470}, {1030, 1070}, {1400, 1440}}; /* blocks: 1.25 s held, 3 s held, after release */
    struct mumble_singer solo = {&mumble_cast[0].voice, 0, 53};
    mumble_press(1, &solo, 1, 0); /* b at F3 */
    for (unsigned b = 0; b < 1440; ++b) {
        if (b == 1100) mumble_release(1);
        memset(z, 0, sizeof z); mumble_render(z);
        for (unsigned w = 0; w < 3; ++w)
            if (b >= spans[w][0] && b < spans[w][1])
                for (unsigned i = 0; i < FM1_BLOCK_WORDS; i += 2) { float x = z[i] / 8388608.0f; window[w] += x * x; }
    }
    printf("held letter rms: %.4f at 1.3 s, %.4f at 3 s, %.5f after release\n",
           sqrt(window[0] / 5120), sqrt(window[1] / 5120), sqrt(window[2] / 5120));
    CHECK(window[0] > 0.0001f * 5120 && window[1] > 0.5f * window[0], "held letter sustains");
    CHECK(window[2] < 2.5e-4f * window[1], "released letter falls silent (only a faint room tail)");
    const unsigned triad[3] = {60, 64, 67};
    for (unsigned k = 0; k < 3; ++k) {
        struct mumble_singer one = {&mumble_cast[0].voice, 0, (uint8_t)triad[k]};
        mumble_press(k * 4, &one, 1, 0);
    }
    for (unsigned b = 0; b < 300; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    struct mumble_status chord; mumble_status(&chord);
    CHECK(chord.vowel < MUMBLE_VOWELS, "chord still sounding after 0.9 s");
    /* Chords of two-syllable letters (A "ay", M "em"): pressed in one frame,
     * or rolled 30 ms apart, every letter reaches its held syllable. */
    mumble_stop();
    for (unsigned b = 0; b < 1200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    for (unsigned roll = 0; roll < 2; ++roll) {
        struct mumble_singer hero = {&mumble_cast[0].voice, 0, 57}, kid = {&mumble_cast[3].voice, 3, 64},
                             elder = {&mumble_cast[5].voice, 5, 60};
        mumble_press(0, &hero, 1, 0);                                        /* A */
        if (roll) for (unsigned b = 0; b < 10; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
        mumble_press(12, &kid, 1, 0);                                        /* M */
        if (roll) for (unsigned b = 0; b < 10; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
        mumble_press(8, &elder, 1, 0);                                       /* I */
        for (unsigned b = 0; b < 300; ++b) { memset(z, 0, sizeof z); mumble_render(z); }   /* 0.87 s */
        struct mumble_status st; mumble_status(&st);
        CHECK(st.track_level[0] && st.track_level[3] && st.track_level[5],
              "a %s chord holds every letter (%u %u %u)", roll ? "rolled" : "one-frame", st.track_level[0],
              st.track_level[3], st.track_level[5]);
        mumble_stop();
        for (unsigned b = 0; b < 1200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    }
    mumble_stop();
    for (unsigned b = 0; b < 100; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    any = 0;
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) any |= z[i] != 0;
    for (unsigned b = 0; b < 1200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    any = 0;
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) any |= z[i] != 0;
    CHECK(!any, "stop releases held letters and the tails die out");

    /* Choir: two tracks speak together, each with its own voice. */
    struct mumble_part parts[MUMBLE_CAST] = {{0}};
    parts[1] = (struct mumble_part){"Halt! Who goes there?", 21, &mumble_cast[1].voice, 0, 100, NULL, 0};
    parts[3] = (struct mumble_part){"It's me! It's me!", 17, &mumble_cast[3].voice, 0, 100, NULL, 0};
    mumble_song(parts, 0, 1, 0);
    unsigned both = 0;
    int32_t last_ms = -1;
    for (unsigned b = 0; b < 600; ++b) {
        memset(z, 0, sizeof z); mumble_render(z);
        struct mumble_status st; mumble_status(&st);
        both |= st.track_level[1] && st.track_level[3];
        last_ms = st.song_ms;
    }
    CHECK(both, "choir: GUARD and KID sound at the same time");
    CHECK(last_ms == -1, "a song played once ends (%d)", (int)last_ms);

    /* Looper: KID from 0 ms and GUARD from 500 ms in a 1 s loop, three passes. */
    mumble_stop();
    for (unsigned b = 0; b < 1200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    memset(parts, 0, sizeof parts);
    parts[3] = (struct mumble_part){"ha", 2, &mumble_cast[3].voice, 0, 100, NULL, 0};
    parts[1] = (struct mumble_part){"ho", 2, &mumble_cast[1].voice, 500, 100, NULL, 0};
    mumble_song(parts, 1000, 1, 0);
    unsigned guard_first = 0, kid_onsets = 0, kid_was = 0, wraps = 0;
    int32_t previous = 0;
    for (unsigned b = 0; b < 1020; ++b) { /* 2.96 s */
        memset(z, 0, sizeof z); mumble_render(z);
        struct mumble_status st; mumble_status(&st);
        unsigned ms = b * FM1_BLOCK_FRAMES * 1000U / FM1_SAMPLE_RATE;
        if (!guard_first && st.track_level[1]) guard_first = ms;
        kid_onsets += st.track_level[3] && !kid_was;
        kid_was = st.track_level[3] != 0;
        wraps += st.song_ms < previous;
        previous = st.song_ms;
    }
    CHECK(guard_first >= 500 && guard_first < 560, "a track starts at its offset (%u ms)", guard_first);
    CHECK(kid_onsets == 3, "a looping track repeats every pass (%u onsets in 3 s)", kid_onsets);
    CHECK(wraps == 2 && previous >= 0 && previous < 1000, "the song clock wraps per pass (%u, now %d)", wraps, (int)previous);


    /* Editing a looping song keeps its phase; the live lane plays over it. */
    int32_t before = mumble_song_position();
    parts[3].text = "hee"; parts[3].length = 3;
    mumble_song(parts, 1000, 0, 0);
    mumble_aside(5, "hmm", 3, &mumble_cast[5].voice);
    for (unsigned b = 0; b < 4; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    int32_t after = mumble_song_position();
    CHECK(after >= before && after - before < 30, "an edit keeps the loop's phase (%d -> %d ms)", (int)before, (int)after);
    unsigned elder = 0, guard_again = 0;
    for (unsigned b = 0; b < 400; ++b) {
        memset(z, 0, sizeof z); mumble_render(z);
        struct mumble_status st; mumble_status(&st);
        elder |= st.track_level[5] != 0;
        guard_again |= st.track_level[1] != 0;
    }
    CHECK(elder && guard_again, "the live lane plays over the song without stopping it");

    /* A cut silences the live line within its release and leaves the song. */
    const char *long_line = "aaaa eeee iiii oooo uuuu aaaa eeee";
    mumble_aside(5, long_line, (unsigned)strlen(long_line), &mumble_cast[5].voice);
    for (unsigned b = 0; b < 60; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    struct mumble_status cut_before; mumble_status(&cut_before);
    mumble_cut();
    unsigned cut_after = 0, song_on = 0;
    for (unsigned b = 0; b < 400; ++b) {
        memset(z, 0, sizeof z); mumble_render(z);
        struct mumble_status st; mumble_status(&st);
        if (b * FM1_BLOCK_FRAMES * 1000U / FM1_SAMPLE_RATE > 150U) cut_after |= st.track_level[5];
        song_on |= st.track_level[1] != 0;
    }
    CHECK(cut_before.track_level[5] && !cut_after, "a cut live line falls silent within 150 ms");
    CHECK(song_on && mumble_song_position() >= 0, "a cut leaves the song playing");
    mumble_stop();

    /* A take: B held 400 ms from 300 ms, alone in a 1 s loop, three passes. It
     * starts on time, sustains while it was held and is silent after its release. */
    mumble_stop();
    for (unsigned b = 0; b < 1200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    memset(parts, 0, sizeof parts);
    const struct mumble_note take[1] = {{300, 400, 1, 60, 0}};
    parts[0] = (struct mumble_part){NULL, 0, &mumble_cast[0].voice, 0, 100, take, 1};
    mumble_song(parts, 1000, 1, 0);
    unsigned take_onsets = 0, take_was = 0, take_first = 0, held_blocks = 0, late_blocks = 0;
    for (unsigned b = 0; b < 1020; ++b) { /* 2.96 s */
        memset(z, 0, sizeof z); mumble_render(z);
        struct mumble_status st; mumble_status(&st);
        unsigned ms = b * FM1_BLOCK_FRAMES * 1000U / FM1_SAMPLE_RATE, in_pass = ms % 1000U;
        unsigned on = st.track_level[0] != 0;
        if (on && !take_first) take_first = ms;
        take_onsets += on && !take_was;
        take_was = on;
        if (in_pass >= 400 && in_pass < 680) held_blocks += !on;    /* held: never silent */
        if (in_pass >= 900 || in_pass < 280) late_blocks += on;     /* after its release, before its start */
    }
    CHECK(take_first >= 290 && take_first < 360, "a take's note starts where it was played (%u ms)", take_first);
    CHECK(take_onsets == 3, "a take plays once a pass (%u onsets in 3 s)", take_onsets);
    CHECK(!held_blocks, "a take's note sustains for as long as it was held (%u silent blocks)", held_blocks);
    CHECK(!late_blocks, "a take's note lets go when its key did (%u blocks sounding)", late_blocks);
    /* Stopping the song lets a sustained note go at once (PLAY, a panic). */
    const struct mumble_note long_take[1] = {{100, 5000, 14, 60, 0}};
    parts[0].notes = long_take;
    mumble_song(parts, 8000, 1, 0);
    for (unsigned b = 0; b < 200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }   /* 0.58 s: into the note */
    struct mumble_status held_note; mumble_status(&held_note);
    mumble_song_stop();
    for (unsigned b = 0; b < 100; ++b) { memset(z, 0, sizeof z); mumble_render(z); }   /* 0.29 s later */
    struct mumble_status stopped; mumble_status(&stopped);
    CHECK(held_note.track_level[0] && !stopped.track_level[0], "a stopped song lets its sustained note go");

    /* The metronome: an empty 1 s loop (as recording starts on nothing), four
     * beats: a click at 0, 250, 500 and 750 ms each pass, the first accented,
     * quiet between; off, silence. */
    mumble_stop();
    for (unsigned b = 0; b < 1200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    memset(parts, 0, sizeof parts);
    mumble_song(parts, 1000, 1, 0);
    mumble_metronome(4, 1000);
    float beat_energy[8] = {0}, between = 0.0f;
    for (unsigned b = 0; b < 689; ++b) {   /* 2 s */
        memset(z, 0, sizeof z); mumble_render(z);
        unsigned ms = b * FM1_BLOCK_FRAMES * 1000U / FM1_SAMPLE_RATE;
        float e = 0.0f;
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; i += 2) { float x = z[i] / 8388608.0f; e += x * x; }
        unsigned beat = (ms + 125U) / 250U, off = ms % 250U, near = off < 125U ? off : 250U - off;
        if (near < 20U && beat < 8U) beat_energy[beat] += e;
        else if (near > 60U) between += e;
    }
    unsigned beats_heard = 0;
    for (unsigned k = 0; k < 8; ++k) beats_heard += beat_energy[k] > 1e-3f;
    CHECK(beats_heard == 8, "the metronome clicks on every beat (%u of 8)", beats_heard);
    CHECK(between < 1e-4f * beat_energy[1], "the metronome is quiet between beats (%g vs %g)", between, beat_energy[1]);
    CHECK(beat_energy[0] > 1.5f * beat_energy[1] && beat_energy[4] > 1.5f * beat_energy[5], "the first beat is accented");
    mumble_metronome(0, 0);
    mumble_song_stop();
    for (unsigned b = 0; b < 200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    any = 0;
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) any |= z[i] != 0;
    CHECK(!any, "the metronome off: silence");

    /* Effects: a GHOST echo rings on after the line; BOT's crush is stepped. */
    mumble_stop();
    for (unsigned b = 0; b < 1200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    mumble_say(7, "Whooo...", 8, &mumble_cast[7].voice);
    double late = 0;
    for (unsigned b = 0; b < 700; ++b) {
        memset(z, 0, sizeof z); mumble_render(z);
        struct mumble_status st; mumble_status(&st);
        if (b > 450 && st.vowel == MUMBLE_VOWELS)
            for (unsigned i = 0; i < FM1_BLOCK_WORDS; i += 2) late += fabs(z[i] / 8388608.0);
    }
    CHECK(late > 1.0, "GHOST echo and room ring after the voice stops (%.2f)", late);
    struct mumble_voice crushed = mumble_cast[6].voice, clean = mumble_cast[6].voice;
    clean.value[MUMBLE_CRUSH] = clean.value[MUMBLE_RADIO] = 0;
    unsigned steps[2] = {0, 0};
    for (unsigned v = 0; v < 2; ++v) {
        mumble_init(); /* no effect residue */
        mumble_say(6, "beep", 4, v ? &crushed : &clean);
        for (unsigned b = 0; b < 100; ++b) {
            memset(z, 0, sizeof z); mumble_render(z);
            for (unsigned i = 2; i < FM1_BLOCK_WORDS; i += 2) steps[v] += z[i] == z[i - 2];
        }
    }
    CHECK(steps[1] > steps[0] + 3000, "crush holds samples (%u vs %u repeats)", steps[1], steps[0]);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("mumble: ok\n");
    return 0;
}
