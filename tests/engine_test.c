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
        mumble_say(line, (unsigned)strlen(line), &mumble_cast[c].voice);
        unsigned blocks = (unsigned)((end / 1000.0f / (mumble_cast[c].voice.value[MUMBLE_SPEED] / 100.0f) + 0.5f) * FM1_SAMPLE_RATE / FM1_BLOCK_FRAMES);
        float *pcm = calloc((size_t)blocks * FM1_BLOCK_FRAMES, sizeof *pcm);
        double sum = 0; float peak = 0; unsigned shown = 0, sounding = 0;
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
            if (st.revealed > shown) shown = st.revealed;
            sounding += st.vowel < MUMBLE_VOWELS;
        }
        unsigned frames = blocks * FM1_BLOCK_FRAMES;
        float rms = (float)sqrt(sum / frames);
        printf("%-7s peak %.2f rms %.3f  %.2fs  text %u/%u  voiced blocks %u\n", mumble_cast[c].name, peak, rms,
               (float)frames / FM1_SAMPLE_RATE, shown, (unsigned)strlen(line), sounding);
        CHECK(peak > 0.1f && peak < 0.8f, "%s peak %.2f", mumble_cast[c].name, peak);
        CHECK(shown == strlen(line), "%s reveals the whole line", mumble_cast[c].name);
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
    CHECK(mumble_type("h", 1, &mumble_cast[0].voice) == 0, "consonant is silent");
    CHECK(mumble_type("ha", 2, &mumble_cast[0].voice) == 1, "vowel speaks");
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
    mumble_press(1, 53, 0, &mumble_cast[0].voice); /* b at F3 */
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
    CHECK(window[2] < 1e-9f, "released letter falls silent");
    mumble_press(0, 60, 0, &mumble_cast[0].voice);
    mumble_press(4, 64, 0, &mumble_cast[0].voice);
    mumble_press(7, 67, 0, &mumble_cast[0].voice);
    for (unsigned b = 0; b < 300; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    struct mumble_status chord; mumble_status(&chord);
    CHECK(chord.vowel < MUMBLE_VOWELS, "chord still sounding after 0.9 s");
    mumble_stop();
    for (unsigned b = 0; b < 100; ++b) { memset(z, 0, sizeof z); mumble_render(z); }
    any = 0;
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) any |= z[i] != 0;
    CHECK(!any, "stop releases held letters");

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("mumble: ok\n");
    return 0;
}
