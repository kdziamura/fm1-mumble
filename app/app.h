/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* MUMBLE as its own FM-1 firmware, on fm1-base: the app's half of the
 * contract with the platform (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "MUMBLE"
#define APP_SPLASH_RGB RGB(60, 150, 250)
#define APP_BLOCK 256u                   /* frames per audio block: two of the engine's 128 */

/* the store: the panel calibration (the platform's), and the app's settings (the line, the cast's sounds, the loop:
 * mumble_screen.inc's struct voice_store) */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u}

/* the contract: the platform calls these */
void app_init(void);                     /* boot: the engine and the cast */
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void app_panic(void);                    /* silence now (before an update rewrites the flash) */
void app_ui_init(void);                  /* the first screen, after the splash */
void app_frame(void);                    /* the UI: input and a redraw, about 60 times a second */
void app_input_only(void);               /* between frames: input only */
void app_say(const char *a, const char *b);   /* a short message on the screen and to a screen reader */

/* the host simulator's checks (host/plat_host.c "expect") */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
#define APP_HOST_REBOOT
void app_host_reboot(void);              /* "reboot": the app forgets what RAM would, then boots from the flash */
