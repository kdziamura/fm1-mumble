/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* The firmware's face generator (app/mumble_faces.inc) on the voices
 * tools/faces.py --dump draws, as the same text: tests/faces.sh diffs the two. */
#include "mumble.h"
#include <stdint.h>
#include <stdio.h>

#include "mumble_faces.inc"

static void dump(const struct mumble_voice *v)
{
    static struct voice_face f;
    char icon[FACE_ICON][FACE_ICON];
    voice_face_build(v, &f);
    for (unsigned y = 0; y < FACE_N; ++y) printf("%.24s\n", f.grid[y]);
    printf("mouth %u %u %c eyes", f.mouth_y, f.mouth_w, f.lip);
    for (unsigned e = 0; e < f.eyes; ++e) printf(" %u,%u,%u,%u", f.eye[e][0], f.eye[e][1], f.eye[e][2], f.eye[e][3]);
    printf("\n");
    voice_face_icon(&f, icon);
    for (unsigned y = 0; y < FACE_ICON; ++y) printf("%.16s\n", icon[y]);
}

int main(void)
{
    /* tools/faces.py's check_voices: the cast, 400 rolled (a fixed LCG), each setting at both ends. */
    for (unsigned k = 0; k < MUMBLE_CAST; ++k) dump(&mumble_cast[k].voice);
    uint32_t x = 1;
    for (unsigned k = 0; k < 400; ++k) {
        struct mumble_voice v;
        for (unsigned p = 0; p < MUMBLE_PARAMS; ++p) {
            int lo = mumble_params[p].min, hi = mumble_params[p].max;
            x = (x * 1103515245U + 12345U) & 0x7FFFFFFFU;
            v.value[p] = (uint16_t)(((int)mumble_cast[k % MUMBLE_CAST].voice.value[p] + lo + (int)((x >> 8) % (uint32_t)(hi - lo + 1))) / 2);
        }
        dump(&v);
    }
    for (unsigned p = 0; p < MUMBLE_PARAMS; ++p)
        for (unsigned end = 0; end < 2; ++end) {
            struct mumble_voice v = mumble_cast[0].voice;
            v.value[p] = end ? mumble_params[p].max : mumble_params[p].min;
            dump(&v);
        }
    return 0;
}
