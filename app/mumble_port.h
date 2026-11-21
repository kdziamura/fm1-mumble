/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* The MUMBLE engine's platform on fm1-base (app/mumble.h: MUMBLE_PORT).
 * The engine renders 128-frame blocks of 24-bit (Q23) stereo at 44.1 kHz;
 * its audio path runs from RAM, as the platform asks of an audio path. */
#ifndef MUMBLE_PORT_H
#define MUMBLE_PORT_H
#include <stddef.h>
#include <stdint.h>
#include "plat.h"

/* Freestanding on the FM-1: felucca-bsp's libc.c (in the platform's unity
 * build) has memset, memcpy and memcmp and nothing else. */
#ifdef PLAT_HOST
#include <string.h>
#else
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
#endif

#define FM1_SAMPLE_RATE 44100
#define FM1_BLOCK_FRAMES 128
#define FM1_CHANNELS 2
#define FM1_BLOCK_WORDS (FM1_BLOCK_FRAMES * FM1_CHANNELS)
#define FM1_AUDIO_CODE PLAT_RAM_CODE
#define FM1_AUDIO_DATA
#endif
