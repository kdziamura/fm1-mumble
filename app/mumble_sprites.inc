/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* MUMBLE cast's palette: line art in the OP-1 manner, 1-px white strokes and
 * the page's encoder colours on black. The faces are built from the voices
 * (mumble_faces.inc), one letter a pixel, '.' empty; the letters index
 * voice_palette(). The hand-drawn cast they replaced is kept in
 * tools/cast_by_hand.txt. */

static uint16_t voice_palette(char c)
{
    switch (c) {
    case 'w': return RGB(235, 235, 235); /* white line */
    case 'm': return RGB(110, 110, 118); /* grey */
    case 'b': return RGB(40, 110, 220);  /* blue (encoder 1) */
    case 'g': return RGB(40, 175, 105);  /* green (encoder 2) */
    case 'o': return RGB(245, 100, 40);  /* orange (encoder 4) */
    default: return 0;
    }
}
