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
    vowels("Sister", "IH ER");
    vowels("Agnes", "AH EH");
    vowels("moon", "OO");
    vowels("make", "EH");   /* silent final e: AY, EH gliding to EE */
    vowels("the", "UH");
    vowels("yes", "EH");    /* word-initial y is a consonant */
    vowels("happy", "AH EE");
    vowels("hmm", "UH");    /* no vowel: one closed hum */

    /* Each word's first syllable: its vowel, the vowel it glides to, its
     * consonant's noise and its ending. */
    static const struct { const char *word; uint8_t vowel, glide, onset, coda; } words[] = {
        {"hey", MUMBLE_E, MUMBLE_I, MUMBLE_NOISE_BREATH, MUMBLE_CODA_NONE},
        {"day", MUMBLE_E, MUMBLE_I, MUMBLE_NOISE_BURST | MUMBLE_NOISE_VOICED, MUMBLE_CODA_NONE},
        {"my", MUMBLE_A, MUMBLE_I, MUMBLE_NOISE_NONE, MUMBLE_CODA_NONE},
        {"go", MUMBLE_O, MUMBLE_OO, MUMBLE_NOISE_BURST | MUMBLE_NOISE_VOICED, MUMBLE_CODA_NONE},
        {"now", MUMBLE_A, MUMBLE_OO, MUMBLE_NOISE_NONE, MUMBLE_CODA_NONE},
        {"boy", MUMBLE_O, MUMBLE_I, MUMBLE_NOISE_BURST | MUMBLE_NOISE_VOICED, MUMBLE_CODA_NONE},
        {"book", MUMBLE_UU, MUMBLE_VOWELS, MUMBLE_NOISE_BURST | MUMBLE_NOISE_VOICED, MUMBLE_CODA_STOP},
        {"her", MUMBLE_ER, MUMBLE_VOWELS, MUMBLE_NOISE_BREATH, MUMBLE_CODA_R},
        {"sit", MUMBLE_IH, MUMBLE_VOWELS, MUMBLE_NOISE_HISS, MUMBLE_CODA_STOP},
        {"site", MUMBLE_A, MUMBLE_I, MUMBLE_NOISE_HISS, MUMBLE_CODA_STOP},
        {"come", MUMBLE_U, MUMBLE_VOWELS, MUMBLE_NOISE_BURST, MUMBLE_CODA_NASAL},
        {"dogs", MUMBLE_O, MUMBLE_VOWELS, MUMBLE_NOISE_BURST | MUMBLE_NOISE_VOICED, MUMBLE_CODA_HISS},
        {"here", MUMBLE_I, MUMBLE_ER, MUMBLE_NOISE_BREATH, MUMBLE_CODA_R},
        {"sing", MUMBLE_IH, MUMBLE_VOWELS, MUMBLE_NOISE_HISS, MUMBLE_CODA_NASAL},
        {"street", MUMBLE_I, MUMBLE_VOWELS, MUMBLE_NOISE_HISS, MUMBLE_CODA_STOP},
        {"tea", MUMBLE_I, MUMBLE_VOWELS, MUMBLE_NOISE_BURST, MUMBLE_CODA_NONE},
        {"key", MUMBLE_I, MUMBLE_VOWELS, MUMBLE_NOISE_BURST, MUMBLE_CODA_NONE},
        {"she", MUMBLE_I, MUMBLE_VOWELS, MUMBLE_NOISE_HISS, MUMBLE_CODA_NONE}};
    for (unsigned w = 0; w < sizeof words / sizeof words[0]; ++w) {
        struct mumble_syllable y[MUMBLE_SYLLABLES];
        unsigned got = plan(words[w].word, 0, y);
        CHECK(got >= 1 && y[0].vowel == words[w].vowel && y[0].glide == words[w].glide && y[0].onset == words[w].onset &&
                  y[got - 1].coda == words[w].coda,
              "%s: vowel %u glide %u onset %u coda %u (expected %u %u %u %u)", words[w].word, y[0].vowel, y[0].glide,
              y[0].onset, y[got - 1].coda, words[w].vowel, words[w].glide, words[w].onset, words[w].coda);
    }
    /* Where the consonants sit: t high, s high, sh lower, k in the middle. */
    {
        struct mumble_syllable y[MUMBLE_SYLLABLES];
        plan("tea", 0, y); unsigned t_hz = y[0].onset_hz;
        plan("sea", 0, y); unsigned s_hz = y[0].onset_hz;
        plan("she", 0, y); unsigned sh_hz = y[0].onset_hz;
        plan("key", 0, y); unsigned k_hz = y[0].onset_hz;
        CHECK(s_hz > sh_hz && t_hz > k_hz && s_hz > k_hz, "consonant places: s %u sh %u t %u k %u", s_hz, sh_hz, t_hz, k_hz);
    }

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
    struct mumble_singer solo = {&mumble_cast[0].voice, 0, 53, 0};
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
        struct mumble_singer one = {&mumble_cast[0].voice, 0, (uint8_t)triad[k], 0};
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
        struct mumble_singer hero = {&mumble_cast[0].voice, 0, 57, 0}, kid = {&mumble_cast[3].voice, 3, 64, 0},
                             elder = {&mumble_cast[5].voice, 5, 60, 0};
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
    parts[1] = (struct mumble_part){"Halt! Who goes there?", 21, &mumble_cast[1].voice, 0, 100, NULL, 0, NULL, 0};
    parts[3] = (struct mumble_part){"It's me! It's me!", 17, &mumble_cast[3].voice, 0, 100, NULL, 0, NULL, 0};
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
    parts[3] = (struct mumble_part){"ha", 2, &mumble_cast[3].voice, 0, 100, NULL, 0, NULL, 0};
    parts[1] = (struct mumble_part){"ho", 2, &mumble_cast[1].voice, 500, 100, NULL, 0, NULL, 0};
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
    CHECK(guard_first >= 500 && guard_first < 620, "a track starts at its offset, after its h (%u ms)", guard_first);
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
    const struct mumble_note take[1] = {{300, 400, 0, 60, 0, 0}};
    parts[0] = (struct mumble_part){NULL, 0, &mumble_cast[0].voice, 0, 100, take, 1, NULL, 0};
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
    /* LINE mode: a syllable's sound held on a key sustains until let go, and a
     * take's syllable note plays back as long as it was held. */
    mumble_stop();
    for (unsigned b = 0; b < 1200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    struct mumble_syllable heh[MUMBLE_SYLLABLES];
    unsigned heh_n = plan("Hello", 0, heh);
    CHECK(heh_n >= 2, "Hello has syllables (%u)", heh_n);
    const struct mumble_sound lo = {heh[1].vowel, heh[1].locus, heh[1].flags, heh[1].glide, 0, 0, 0, 0};
    struct mumble_singer sister = {&mumble_cast[2].voice, 2, 62, 0};
    CHECK(!mumble_press_sound(5, &lo, &sister, 1, 0), "a syllable is pressed");
    for (unsigned b = 0; b < 520; ++b) { memset(z, 0, sizeof z); mumble_render(z); }   /* 1.5 s */
    struct mumble_status syl_held; mumble_status(&syl_held);
    mumble_release(5);
    for (unsigned b = 0; b < 100; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    struct mumble_status syl_gone; mumble_status(&syl_gone);
    CHECK(syl_held.track_level[2] && syl_held.track_vowel[2] == lo.vowel && !syl_gone.track_level[2],
          "a held syllable sustains its vowel until its key is let go");
    /* (slot 1 of a two-syllable line: "lo") */
    const struct mumble_sound syl_line[2] = {{heh[0].vowel, heh[0].locus, heh[0].flags, heh[0].glide, 0, 0, 0, 0}, lo};
    const struct mumble_note syl_take[1] = {{200, 600, 1, 62, 0, 0}};
    memset(parts, 0, sizeof parts);
    parts[2] = (struct mumble_part){NULL, 0, &mumble_cast[2].voice, 0, 100, syl_take, 1, syl_line, 2};
    mumble_song(parts, 1000, 1, 0);
    unsigned syl_mid = 0, syl_after = 0;
    for (unsigned b = 0; b < 344; ++b) {
        memset(z, 0, sizeof z); mumble_render(z);
        struct mumble_status st; mumble_status(&st);
        unsigned ms = b * FM1_BLOCK_FRAMES * 1000U / FM1_SAMPLE_RATE;
        if (ms >= 450 && ms < 750) syl_mid += st.track_level[2] && st.track_vowel[2] == lo.vowel;
        if (ms >= 950) syl_after += st.track_level[2] != 0;
    }
    CHECK(syl_mid > 90 && !syl_after, "a take's slot plays its line's syllable while held, then lets go (%u, %u)", syl_mid,
          syl_after);
    mumble_song_stop();

    /* The compressor: a chord of all eight characters, shouted, stays off the
     * soft clip's ceiling (0.71 of full scale: above 0.6 it is saturating);
     * one voice alone keeps its level. */
    mumble_stop();
    for (unsigned b = 0; b < 1200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    float levels[2] = {0}, near_top = 0.0f, samples = 0.0f;
    for (unsigned chord = 0; chord < 2; ++chord) {
        unsigned voices = chord ? MUMBLE_CAST : 1;
        for (unsigned v = 0; v < voices; ++v) {
            struct mumble_singer one = {&mumble_cast[v].voice, (uint8_t)v, (uint8_t)(55 + v * 4), 0};
            mumble_press(v * 3U, &one, 1, chord);
        }
        for (unsigned b = 0; b < 690; ++b) {   /* 2 s */
            memset(z, 0, sizeof z); mumble_render(z);
            if (b < 170) continue;             /* from 0.5 s: held */
            for (unsigned i = 0; i < FM1_BLOCK_WORDS; i += 2) {
                float x = z[i] / 8388608.0f;
                levels[chord] += x * x;
                if (chord) { near_top += x > 0.6f || x < -0.6f; samples += 1.0f; }
            }
        }
        mumble_stop();
        for (unsigned b = 0; b < 1200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    }
    printf("compressor: one voice rms %.3f, eight shouted rms %.3f, %.2f%% near the top\n",
           sqrt(levels[0] / (520 * 128)), sqrt(levels[1] / (520 * 128)), 100.0f * near_top / samples);
    CHECK(near_top < 0.002f * samples, "a chord of eight stays clear of the clip (%.2f%% of samples)", 100.0f * near_top / samples);
    CHECK(levels[1] > 1.5f * levels[0], "a chord is still louder than one voice");

    /* Stopping the song lets a sustained note go at once (PLAY, a panic). */
    const struct mumble_note long_take[1] = {{100, 5000, 0, 60, 0, 0}};
    memset(parts, 0, sizeof parts);
    parts[0] = (struct mumble_part){NULL, 0, &mumble_cast[0].voice, 0, 100, long_take, 1, NULL, 0};
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

    /* The singing voice, measured on HERO with no echo or room (no tails). */
    {
        struct mumble_voice dry = mumble_cast[0].voice;
        dry.value[MUMBLE_ECHO] = dry.value[MUMBLE_ROOM] = 0;
        dry.value[MUMBLE_VIBRATO] = 0;
        struct mumble_singer one = {&dry, 0, 57, 0};
        const struct mumble_sound plain = {MUMBLE_A, 0, 0, MUMBLE_VOWELS, 0, 0, 0, 0};
        struct mumble_status st;
        /* The rest of these: 0.35 s to let tails go. */
#define SETTLE() do { mumble_stop(); for (unsigned b_ = 0; b_ < 1200; ++b_) { memset(z, 0, sizeof z); mumble_render(z); } } while (0)
        /* A hiss before the vowel: high-frequency energy in its first 60 ms. */
        float hf[2] = {0};
        for (unsigned v = 0; v < 2; ++v) {
            SETTLE();
            struct mumble_sound snd = plain;
            if (v) { snd.onset = MUMBLE_NOISE_HISS; snd.onset_hz = 60; }
            mumble_press_sound(1, &snd, &one, 1, 0);
            float last = 0.0f;
            for (unsigned b = 0; b < 21; ++b) {
                memset(z, 0, sizeof z); mumble_render(z);
                for (unsigned i = 0; i < FM1_BLOCK_WORDS; i += 2) { float x = z[i] / 8388608.0f, d = x - last; hf[v] += d * d; last = x; }
            }
            mumble_release(1);
        }
        CHECK(hf[1] > 4.0f * hf[0], "an s is heard before its vowel (hf %.5f vs %.5f)", hf[1], hf[0]);
        /* An unvoiced t holds the voice back; a voiced d does not. */
        unsigned first_voice[3] = {0};
        for (unsigned v = 0; v < 3; ++v) {
            SETTLE();
            struct mumble_sound snd = plain;
            if (v) { snd.onset = (uint8_t)(MUMBLE_NOISE_BURST | (v == 2 ? MUMBLE_NOISE_VOICED : 0)); snd.onset_hz = 40; }
            mumble_press_sound(1, &snd, &one, 1, 0);
            for (unsigned b = 0; b < 40 && !first_voice[v]; ++b) {
                memset(z, 0, sizeof z); mumble_render(z);
                mumble_status(&st);
                if (st.track_level[0]) first_voice[v] = b + 1;
            }
            mumble_release(1);
        }
        CHECK(first_voice[1] >= first_voice[0] + 6 && first_voice[2] <= first_voice[0] + 1,
              "a t holds the voice back about 25 ms, a d does not (blocks %u, %u, %u)", first_voice[0], first_voice[1], first_voice[2]);
        /* A held AY: EH while held, EE as it is let go. */
        SETTLE();
        struct mumble_sound ay = plain;
        ay.vowel = MUMBLE_E; ay.glide = MUMBLE_I;
        mumble_press_sound(1, &ay, &one, 1, 0);
        for (unsigned b = 0; b < 300; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
        mumble_status(&st);
        unsigned held_vowel = st.track_vowel[0];
        mumble_release(1);
        for (unsigned b = 0; b < 35; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
        mumble_status(&st);
        CHECK(held_vowel == MUMBLE_E && st.track_vowel[0] == MUMBLE_I, "a held AY glides EH to EE on release (%u, %u)",
              held_vowel, st.track_vowel[0]);
        /* Endings: a nasal closes slowly (160 ms), a stop cuts in 25 ms, an open vowel 120. */
        unsigned gone[3] = {0};
        for (unsigned v = 0; v < 3; ++v) {
            SETTLE();
            struct mumble_sound snd = plain;
            if (v == 1) { snd.coda = MUMBLE_CODA_NASAL; snd.coda_hz = 11; }
            if (v == 2) { snd.coda = MUMBLE_CODA_STOP; snd.coda_hz = 40; }
            mumble_press_sound(1, &snd, &one, 1, 0);
            for (unsigned b = 0; b < 200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
            mumble_release(1);
            for (unsigned b = 0; b < 100 && !gone[v]; ++b) {
                memset(z, 0, sizeof z); mumble_render(z);
                mumble_status(&st);
                if (!st.track_level[0]) gone[v] = b + 1;
            }
        }
        CHECK(gone[1] > gone[0] + 8 && gone[2] + 25 < gone[0], "an m closes slower, a t cuts sooner (blocks %u, %u, %u)",
              gone[0], gone[1], gone[2]);
        /* A hiss ending: noise after the voice has gone. */
        float after[2] = {0};
        for (unsigned v = 0; v < 2; ++v) {
            SETTLE();
            struct mumble_sound snd = plain;
            if (v) { snd.coda = MUMBLE_CODA_HISS; snd.coda_hz = 60; }
            mumble_press_sound(1, &snd, &one, 1, 0);
            for (unsigned b = 0; b < 200; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
            mumble_release(1);
            for (unsigned b = 0; b < 70; ++b) {
                memset(z, 0, sizeof z); mumble_render(z);
                if (b < 45) continue;            /* from 130 ms: the voice has let go */
                for (unsigned i = 0; i < FM1_BLOCK_WORDS; i += 2) { float x = z[i] / 8388608.0f; after[v] += x * x; }
            }
        }
        CHECK(after[1] > 10.0f * after[0] + 1e-6f, "an s trails a hiss after the voice (%.6f vs %.6f)", after[1], after[0]);
        /* GLIDE: a note slid into from an octave below starts low and arrives. */
        SETTLE();
        dry.value[MUMBLE_GLIDE] = 200;
        struct mumble_singer slid = {&dry, 0, 57, -12};
        mumble_press_sound(1, &plain, &slid, 1, 0);
        int early = 0, late = 0;
        for (unsigned b = 0; b < 200; ++b) {
            memset(z, 0, sizeof z); mumble_render(z);
            mumble_status(&st);
            if (b == 4) early = st.cents;
            if (b == 199) late = st.cents;
        }
        mumble_release(1);
        CHECK(late - early > 900, "a slid note starts an octave down and arrives (%d -> %d cents)", early, late);
        /* VIBRATO: a held note sways once it has been held a while; none without. */
        int sway[2] = {0};
        for (unsigned v = 0; v < 2; ++v) {
            SETTLE();
            dry.value[MUMBLE_GLIDE] = 0;
            dry.value[MUMBLE_VIBRATO] = v ? 100 : 0;
            struct mumble_singer held = {&dry, 0, 57, 0};
            mumble_press_sound(1, &plain, &held, 1, 0);
            int lo = 100000, hi = -100000;
            for (unsigned b = 0; b < 450; ++b) {
                memset(z, 0, sizeof z); mumble_render(z);
                if (b < 250) continue;
                mumble_status(&st);
                if (st.cents < lo) lo = st.cents;
                if (st.cents > hi) hi = st.cents;
            }
            sway[v] = hi - lo;
            mumble_release(1);
        }
        CHECK(sway[1] > 60 && sway[0] < 5, "vibrato sways a held note (%d cents; none: %d)", sway[1], sway[0]);
        /* EFFORT: more of the voice's energy up high. */
        float ratio[2] = {0};
        for (unsigned v = 0; v < 2; ++v) {
            SETTLE();
            dry.value[MUMBLE_VIBRATO] = 0;
            dry.value[MUMBLE_AIR] = 0;              /* (the voiced buzz alone: breath is AIR's) */
            dry.value[MUMBLE_EFFORT] = v ? 100 : 0;
            struct mumble_singer held = {&dry, 0, 57, 0};
            mumble_press_sound(1, &plain, &held, 1, 0);
            /* (the share of the energy over 2.5 kHz: two second-order high-passes, so
             * F1 and F2, 30 dB stronger, do not leak in) */
            const double w0 = 2.0 * 3.14159265 * 2500.0 / FM1_SAMPLE_RATE, al = sin(w0) / 1.4, cw = cos(w0);
            const double b0 = (1 + cw) / 2 / (1 + al), b1 = -(1 + cw) / (1 + al), a1 = -2 * cw / (1 + al), a2 = (1 - al) / (1 + al);
            double st_[2][4] = {{0}};
            float all = 0.0f, high = 0.0f;
            for (unsigned b = 0; b < 200; ++b) {
                memset(z, 0, sizeof z); mumble_render(z);
                for (unsigned i = 0; i < FM1_BLOCK_WORDS; i += 2) {
                    double x = z[i] / 8388608.0, y = x;
                    for (unsigned f = 0; f < 2; ++f) {
                        double *q = st_[f], o = b0 * y + b1 * q[0] + b0 * q[1] - a1 * q[2] - a2 * q[3];
                        q[1] = q[0]; q[0] = y; q[3] = q[2]; q[2] = o; y = o;
                    }
                    if (b >= 50) { all += (float)(x * x); high += (float)(y * y); }
                }
            }
            ratio[v] = high / (all + 1e-12f);
            mumble_release(1);
        }
        CHECK(ratio[1] > 2.0f * ratio[0], "effort brightens the voice (%.4f vs %.4f of it over 2.5 kHz)", ratio[1], ratio[0]);
        SETTLE();
#undef SETTLE
    }

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
