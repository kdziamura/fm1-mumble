/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* The cast's faces, built from their voices: a 24x24 portrait (drawn at 3x on
 * the stage), its 16x16 icon (the choir strip) and its rig (where the mouth
 * and eyes are, for talking and blinking), in the palette and the
 * construction of the hand-drawn cast (mumble_sprites.inc's letters).
 *
 * tools/faces.py is the same generator in Python, for contact sheets; both
 * work in integers (settings in thousandths of their range, how far each
 * stands out from the cast in hundredths of its spread, C's division) and
 * tests/faces.sh checks they draw the same pixels. Change one, change both.
 *
 * A figure: a head with straight sides, a rounded or flat top and a chin cut
 * in 45-degree steps, ears at the eyes, 2x2 eyes, a nose, cheeks and a short
 * mouth; a two-pixel neck; shoulders as a line stepping down into straight
 * sides. The voice sets the proportions in whole pixels (size from TUBE,
 * PITCH, EFFORT, TUNE, MOUTH) and picks a part for each slot from the setting
 * that stands out most against the rest of the cast:
 *   top    ROOM a hood, RADIO an antenna, SPEED spikes and a headband,
 *          DICTION a cap, a drilled voice (pushed, low, crisp, steady) a
 *          knight's helmet, JITTER a bandana, VIBRATO long hair, WOBBLE bald,
 *          GLIDE a propeller beanie, PITCH a beanie with a bobble, a crisp low
 *          voice a top hat, ECHO headphones, TUNE a crown, a fast pushed one
 *          a mohawk; else one of four hairstyles
 *   face   CRUSH a visor, WOBBLE a beard (low voices), JITTER + EFFORT an eye
 *          patch, a very crisp DICTION glasses, a low pushed voice a
 *          mustache, a high rough one freckles, a crisp slow one a monocle
 *   body   AIR a ghost, CRUSH a robot (each built as the hand-drawn GHOST and
 *          BOT are), ROOM a robe, a drilled voice armour, JITTER stripes,
 *          PITCH a tee, a little AIR a scarf, DICTION a bow tie, GLIDE a cape
 * A woman's voice (an adult's pitch and tract in their usual range) has her
 * own hair (long, a ponytail, a bun or a bob, dark or ginger; long hair under
 * a hat), lashes, earrings and for some coloured lips, and no beard.
 * And the voice sets the face: brows (crossed with EFFORT, raised with
 * GLIDE), the mouth (a frown when strained, a smile with TUNE), sleepy lids
 * with AIR.
 * Main loop only (a few thousand operations a face). */

enum { FACE_N = 24, FACE_ICON = 16 };
enum { TOP_NONE, TOP_HOOD, TOP_ANTENNA, TOP_SPIKES, TOP_CAP, TOP_HELMET, TOP_BANDANA, TOP_LONG_HAIR, TOP_BALD,
       TOP_PROPELLER, TOP_BEANIE, TOP_TOP_HAT, TOP_HEADPHONES, TOP_CROWN, TOP_MOHAWK };
enum { EXTRA_NONE, EXTRA_VISOR, EXTRA_BEARD, EXTRA_EYE_PATCH, EXTRA_GLASSES, EXTRA_MUSTACHE, EXTRA_FRECKLES,
       EXTRA_MONOCLE };
enum { BODY_NONE, BODY_GHOST, BODY_ROBOT, BODY_ROBE, BODY_ARMOUR, BODY_STRIPES, BODY_TEE, BODY_SCARF, BODY_BOW_TIE,
       BODY_CAPE };
struct voice_face {
    char grid[FACE_N][FACE_N];
    uint8_t mouth_y, mouth_w, eyes, top, extra, body;
    uint8_t eye[2][4];                             /* x, y, w, h: what a blink closes */
};

/* The cast's sum of each setting and its spread (K x its standard deviation,
 * at least 12 % of its range), once. */
enum { FACE_K = MUMBLE_CAST };
static int face_sum[MUMBLE_PARAMS], face_spread[MUMBLE_PARAMS], face_ready;
static int face_isqrt(int n)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= n) ++r;
    return r;
}
static void face_stats(void)
{
    if (face_ready) return;
    for (unsigned p = 0; p < MUMBLE_PARAMS; ++p) {
        int sum = 0, squares = 0;
        for (unsigned k = 0; k < FACE_K; ++k) sum += mumble_cast[k].voice.value[p];
        for (unsigned k = 0; k < FACE_K; ++k) {
            int d = FACE_K * (int)mumble_cast[k].voice.value[p] - sum;
            squares += d * d;
        }
        int floor = FACE_K * (mumble_params[p].max - mumble_params[p].min) * 12 / 100;
        int spread = face_isqrt(squares / FACE_K);
        face_sum[p] = sum;
        face_spread[p] = spread > floor ? spread : floor;
    }
    face_ready = 1;
}

/* --- drawing on the grid: the figure is symmetric about x = 11.5 --- */
static void face_set(struct voice_face *f, int x, int y, char c)
{
    if (x >= 0 && x < FACE_N && y >= 0 && y < FACE_N) f->grid[y][x] = c;
}
static char face_get(const struct voice_face *f, int x, int y)
{
    return x >= 0 && x < FACE_N && y >= 0 && y < FACE_N ? f->grid[y][x] : '.';
}
static void face_both(struct voice_face *f, int x, int y, char c)
{
    face_set(f, x, y, c);
    face_set(f, FACE_N - 1 - x, y, c);
}
static void face_rect(struct voice_face *f, int x, int y, int w, int h, char c, int mirror)
{
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            if (mirror) face_both(f, x + i, y + j, c);
            else face_set(f, x + i, y + j, c);
        }
}
static void face_line(struct voice_face *f, int x0, int y0, int x1, int y1, char c, int mirror)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        if (mirror) face_both(f, x0, y0, c);
        else face_set(f, x0, y0, c);
        if (x0 == x1 && y0 == y1) return;
        int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}
/* num / den to the nearest whole, halves up (num, den >= 0). */
static int face_rnd(int num, int den) { return (2 * num + den) / (2 * den); }
static int face_min(int a, int b) { return a < b ? a : b; }
static int face_max(int a, int b) { return a > b ? a : b; }
static void face_eyes(struct voice_face *f, int left, int ey, int ew, int eh)
{
    const uint8_t eyes[2][4] = {{(uint8_t)left, (uint8_t)ey, (uint8_t)ew, (uint8_t)eh},
                                {(uint8_t)(FACE_N - left - ew), (uint8_t)ey, (uint8_t)ew, (uint8_t)eh}};
    for (unsigned e = 0; e < 2; ++e)
        for (unsigned k = 0; k < 4; ++k) f->eye[e][k] = eyes[e][k];
    f->eyes = 2;
}

/* The part with the highest standing among those allowed, the first of
 * equals, past the threshold (else 0). */
struct face_candidate { uint8_t part; int z, allowed; };
static uint8_t face_pick(const struct face_candidate *c, unsigned n, int threshold)
{
    int best = -1;
    for (unsigned i = 0; i < n; ++i)
        if (c[i].allowed && (best < 0 || c[i].z > c[best].z)) best = (int)i;
    return best >= 0 && c[best].z >= threshold ? c[best].part : 0;
}

enum { T_PITCH, T_TUBE, T_MOUTH, T_AIR, T_SPEED, T_WOBBLE, T_JITTER, T_TUNE, T_CRUSH, T_RADIO, T_ECHO, T_ROOM,
       T_GLIDE, T_VIBRATO, T_DICTION, T_EFFORT };
_Static_assert(T_EFFORT + 1 == MUMBLE_PARAMS, "the settings in mumble.h's order");

static void face_robot(struct voice_face *f, const int *t, int size)
{
    int half = size < 400 ? 6 : 7, xl = 12 - half, yb = 13;
    int yt = yb - (8 + face_rnd(2 * face_min(1000, size + 300), 1000));
    face_rect(f, xl + 1, yt, 2 * half - 2, 1, 'w', 0), face_rect(f, xl + 1, yb, 2 * half - 2, 1, 'w', 0);
    face_line(f, xl, yt + 1, xl, yb - 1, 'w', 1);
    int ey = yt + 4;
    face_rect(f, xl - 2, ey - 1, 2, 1, 'm', 1), face_rect(f, xl - 2, ey + 2, 2, 1, 'm', 1);
    face_line(f, xl - 2, ey, xl - 2, ey + 1, 'm', 1);
    face_rect(f, xl + 3, yt + 2, 2 * half - 6, 1, 'm', 0), face_rect(f, xl + 3, yt + 8, 2 * half - 6, 1, 'm', 0);
    face_line(f, xl + 2, yt + 3, xl + 2, yt + 7, 'm', 1);
    int left = 12 - 2 - (1 + face_rnd(t[T_TUNE], 1000));
    face_rect(f, left, ey, 2, 2, 'g', 1);
    face_eyes(f, left, ey, 2, 2);
    int mw = t[T_MOUTH] < 600 ? 2 : 4;
    face_rect(f, 12 - mw / 2, ey + 3, mw / 2, 1, 'g', 1);
    f->mouth_y = (uint8_t)(ey + 3), f->mouth_w = (uint8_t)mw;
    if (t[T_RADIO] > 200) {
        face_rect(f, 11, yt - 1, 2, 1, 'w', 0);
        face_rect(f, 11, yt - 3, 2, 2, 'o', 0);
    }
    for (int y = yb + 1; y < yb + 4; ++y) face_both(f, 10, y, 'm');
    face_rect(f, 10, yb + 2, 4, 1, 'm', 0);
    int sy = yb + 4;
    face_rect(f, 4, sy, 16, 1, 'w', 0);
    face_both(f, 3, sy + 1, 'w');
    face_line(f, 2, sy + 2, 2, FACE_N - 1, 'w', 1);
    face_rect(f, 6, sy + 2, 12, 1, 'b', 0);
    face_line(f, 6, sy + 3, 6, FACE_N - 1, 'b', 1);
    if (t[T_CRUSH] > 700) face_rect(f, 9, sy + 4, 2, 1, 'o', 0), face_rect(f, 13, sy + 4, 2, 1, 'g', 0);
}

static void face_ghost(struct voice_face *f, const int *t, int size)
{
    int xl = size > 350 ? 4 : 5, half = 12 - xl, yt = 1;
    face_rect(f, xl + 5, yt, 2 * half - 10, 1, 'w', 0);
    face_both(f, xl + 3, yt + 1, 'w'), face_both(f, xl + 4, yt + 1, 'w');
    face_both(f, xl + 2, yt + 2, 'w');
    face_line(f, xl + 1, yt + 3, xl + 1, yt + 4, 'w', 1);
    int arm = yt + 11;
    face_line(f, xl, yt + 5, xl, arm + 4, 'w', 1);
    face_rect(f, xl - 2, arm, 2, 1, 'w', 1), face_both(f, xl - 3, arm + 1, 'w'), face_rect(f, xl - 2, arm + 2, 2, 1, 'w', 1);
    face_line(f, xl + 1, arm + 5, xl + 1, 21, 'w', 1);
    face_both(f, xl + 4, 21, 'w');
    face_rect(f, xl + 2, 22, 2, 1, 'w', 1);
    face_rect(f, xl + 5, 22, 12 - (xl + 5), 1, 'w', 1);
    int ey = yt + 6, eh = t[T_PITCH] > 150 ? 3 : 2;
    int left = 12 - 2 - (1 + face_rnd(2 * t[T_TUNE], 1000));
    face_rect(f, left, ey, 2, eh, 'w', 1);
    face_eyes(f, left, ey, 2, eh);
    if (t[T_PITCH] > 150) face_rect(f, left - 1, ey + 3, 2, 1, 'o', 1);
    int mw = t[T_MOUTH] < 600 ? 2 : 4;
    face_rect(f, 12 - mw / 2, ey + 4, mw / 2, 1, 'w', 1);
    f->mouth_y = (uint8_t)(ey + 4), f->mouth_w = (uint8_t)mw;
    if (t[T_ECHO] > 400) {
        static const int8_t dots[6][2] = {{1, 3}, {22, 5}, {2, 19}, {21, 18}, {0, 9}, {23, 11}};
        for (unsigned k = 0; k < 6; ++k) face_set(f, dots[k][0], dots[k][1], 'b');
    }
}

static void voice_face_build(const struct mumble_voice *v, struct voice_face *f)
{
    face_stats();
    int t[MUMBLE_PARAMS], z[MUMBLE_PARAMS];
    for (unsigned p = 0; p < MUMBLE_PARAMS; ++p) {
        int lo = mumble_params[p].min, hi = mumble_params[p].max;
        t[p] = ((int)v->value[p] - lo) * 1000 / (hi - lo);
        z[p] = (FACE_K * (int)v->value[p] - face_sum[p]) * 100 / face_spread[p];
    }
    for (unsigned y = 0; y < FACE_N; ++y)
        for (unsigned x = 0; x < FACE_N; ++x) f->grid[y][x] = '.';
    f->eyes = 0, f->top = TOP_NONE, f->extra = EXTRA_NONE;
    int size = 1000 - t[T_TUBE], high = t[T_PITCH];

    /* The parts, from what stands out. A drilled voice (pushed, low, crisp,
     * steady) is a soldier's; a pushed, rough one is not. */
    int drilled = z[T_EFFORT] - z[T_PITCH] / 2 - z[T_JITTER] / 2 + z[T_DICTION] / 2;
    const struct face_candidate bodies[] = {
        {BODY_GHOST, z[T_AIR], t[T_AIR] > 600},     {BODY_ROBOT, z[T_CRUSH], t[T_CRUSH] > 400},
        {BODY_ROBE, z[T_ROOM], t[T_ROOM] > 500},    {BODY_ARMOUR, drilled, t[T_EFFORT] > 600},
        {BODY_STRIPES, z[T_JITTER], t[T_JITTER] > 600}, {BODY_TEE, z[T_PITCH], t[T_PITCH] > 450},
        {BODY_SCARF, z[T_AIR], t[T_AIR] > 350 && t[T_AIR] <= 600}, {BODY_BOW_TIE, z[T_DICTION], t[T_DICTION] > 550},
        {BODY_CAPE, z[T_GLIDE], t[T_GLIDE] > 400 && t[T_AIR] <= 600}};
    int body = f->body = face_pick(bodies, 9, 90);
    if (body == BODY_ROBOT) {
        if (t[T_RADIO] > 200) f->top = TOP_ANTENNA;
        face_robot(f, t, size);
        return;
    }
    if (body == BODY_GHOST) {
        face_ghost(f, t, size);
        return;
    }
    /* A woman's voice (an adult's pitch and tract in their usual range, not a
     * child's): their own hair (or long hair under a hat), lashes, earrings, for
     * some coloured lips; no beard. */
    int fem = t[T_PITCH] >= 250 && t[T_PITCH] <= 640 && t[T_TUBE] >= 430 && t[T_TUBE] <= 757;
    int looks = (int)v->value[T_SPEED] + 2 * (int)v->value[T_TUNE] + 3 * (int)v->value[T_MOUTH];   /* (which hair, which colour) */
    const struct face_candidate tops[] = {
        {TOP_HOOD, z[T_ROOM], t[T_ROOM] > 550},          {TOP_ANTENNA, z[T_RADIO], t[T_RADIO] > 350},
        {TOP_SPIKES, z[T_SPEED], t[T_SPEED] > 400},      {TOP_CAP, z[T_DICTION], t[T_DICTION] > 500},
        {TOP_HELMET, drilled, t[T_EFFORT] > 600},        {TOP_BANDANA, z[T_JITTER], t[T_JITTER] > 600},
        {TOP_LONG_HAIR, z[T_VIBRATO], t[T_VIBRATO] > 350}, {TOP_BALD, z[T_WOBBLE], t[T_WOBBLE] > 400},
        {TOP_PROPELLER, z[T_GLIDE], t[T_GLIDE] > 350},   {TOP_BEANIE, z[T_PITCH], t[T_PITCH] > 450},
        {TOP_TOP_HAT, (z[T_DICTION] - z[T_PITCH]) / 2, t[T_DICTION] > 500 && t[T_PITCH] < 300},
        {TOP_HEADPHONES, z[T_ECHO], t[T_ECHO] > 350},  {TOP_CROWN, z[T_TUNE], t[T_TUNE] > 650},
        {TOP_MOHAWK, (z[T_SPEED] + z[T_EFFORT]) / 2, t[T_SPEED] > 450 && t[T_EFFORT] > 550}};
    const struct face_candidate extras[] = {
        {EXTRA_VISOR, z[T_CRUSH], t[T_CRUSH] > 500},
        {EXTRA_BEARD, z[T_WOBBLE] - z[T_PITCH] * 7 / 10, t[T_WOBBLE] > 400 && !fem},
        {EXTRA_EYE_PATCH, (z[T_JITTER] + z[T_EFFORT]) * 6 / 10 - z[T_MOUTH] * 4 / 10, t[T_JITTER] > 600},
        {EXTRA_GLASSES, z[T_DICTION] * 8 / 10, z[T_DICTION] > 120},
        {EXTRA_MUSTACHE, (z[T_EFFORT] - z[T_PITCH]) / 2, t[T_PITCH] < 250 && t[T_EFFORT] > 450 && !fem},
        {EXTRA_FRECKLES, (z[T_PITCH] + z[T_JITTER]) / 2, t[T_PITCH] > 350 && t[T_JITTER] > 450},
        {EXTRA_MONOCLE, (z[T_DICTION] - z[T_SPEED]) / 2, t[T_DICTION] > 500 && t[T_SPEED] < 450}};
    int top = f->top = face_pick(tops, 14, 90), extra = f->extra = face_pick(extras, 7, 70);
    int machine = extra == EXTRA_VISOR;
    fem = fem && !machine;
    if (fem && (top == TOP_SPIKES || top == TOP_BALD || top == TOP_LONG_HAIR || top == TOP_MOHAWK))
        top = f->top = TOP_NONE;                                                 /* (their own hair instead) */

    /* Proportions, in whole pixels: half the head's width (its left side at
     * 12 - half), its height to the chin line (row 16), the jaw's steps. */
    int half = 6 * size + 3 * t[T_EFFORT] - 5 * high < 500 ? 6 : (size > 900 && high < 400) ? 8 : 7;
    int h = 11 + face_rnd(3 * face_min(1000, face_max(0, 250 + (8 * size - 3 * high) / 10)), 1000);
    int tall_top = top == TOP_CAP || top == TOP_BANDANA || top == TOP_HELMET || top == TOP_BEANIE || top == TOP_PROPELLER ||
                   top == TOP_TOP_HAT;
    if (tall_top) h = face_min(h, 11);
    int yc = 16, yt = yc - h + 1, xl = 12 - half, xr = 11 + half;
    int jaw = machine ? 0 : (high > 550 && t[T_EFFORT] < 600) ? 2 : 1;
    int rounded = !machine && !tall_top && top != TOP_SPIKES;

    /* The head. */
    int side_top;
    if (rounded) {
        face_rect(f, xl + 3, yt, 2 * half - 6, 1, 'w', 0);
        face_both(f, xl + 1, yt + 1, 'w'), face_both(f, xl + 2, yt + 1, 'w');
        side_top = yt + 2;
    } else {
        face_rect(f, xl, yt, 2 * half, 1, 'w', 0);
        side_top = yt + 1;
    }
    int side_end = yc - 1 - jaw;
    face_line(f, xl, side_top, xl, side_end, 'w', 1);
    for (int k = 0; k < jaw; ++k) face_both(f, xl + 1 + k, side_end + 1 + k, 'w');
    face_rect(f, xl + 1 + jaw, yc, 2 * (half - 1 - jaw), 1, 'w', 0);

    /* Eyes (TUNE sets them apart), ears or a machine's bolts at them. */
    int ey = tall_top ? yt + (top == TOP_CAP ? 4 : 3) : yt + face_rnd(h * 42, 100);
    int ew = 2, eh = high < 50 ? 1 : 2;
    int left = face_max(12 - ew - (1 + face_rnd(2 * t[T_TUNE], 1000)), xl + 2);
    face_rect(f, left, ey, ew, eh, 'w', 1);
    face_eyes(f, left, ey, ew, eh);
    if (machine) {
        face_rect(f, xl - 2, ey - 1, 2, 4, 'm', 1);
        face_rect(f, xl - 1, ey, 1, 2, '.', 1);
    } else if (top != TOP_HOOD && top != TOP_LONG_HAIR && top != TOP_HELMET && top != TOP_HEADPHONES) {
        face_both(f, xl - 1, ey, 'w'), face_both(f, xl - 2, ey + 1, 'w'), face_both(f, xl - 1, ey + 2, 'w');
        if (fem) face_both(f, xl - 1, ey + 3, 'o');                             /* earrings */
    }
    if (fem) face_both(f, left - 1, ey - 1, 'w');                                /* lashes */
    if (!machine && t[T_AIR] > 450 && eh == 2) face_rect(f, left, ey, ew, 1, 'm', 1);   /* sleepy: lids half down */
    if (!machine && top != TOP_HELMET) {          /* brows: cross with effort, up with glide */
        if (t[T_EFFORT] > 650)
            face_both(f, left - 1, ey - 3, 'w'), face_both(f, left, ey - 2, 'w'), face_both(f, left + 1, ey - 2, 'w');
        else if (t[T_GLIDE] > 550)
            face_both(f, left - 1, ey - 2, 'w'), face_both(f, left, ey - 3, 'w'), face_both(f, left + 1, ey - 3, 'w');
    }

    /* Nose, cheeks, mouth. */
    if (!machine) {
        face_set(f, 12, ey + 2, 'm');
        if (t[T_TUBE] < 300) face_set(f, 12, ey + 3, 'm');
    }
    if (!machine && high > 150 && extra != EXTRA_BEARD && top != TOP_HELMET) face_rect(f, left - 1, ey + 3, 2, 1, 'o', 1);
    int mw = t[T_MOUTH] < 450 ? 2 : t[T_MOUTH] < 850 ? 4 : 6;
    int my = face_min(ey + 4, yc - 2);
    face_rect(f, 12 - mw / 2, my, mw / 2, 1, fem && looks / 4 % 2 ? 'o' : 'w', 1);
    f->mouth_y = (uint8_t)my, f->mouth_w = (uint8_t)mw;
    if (t[T_EFFORT] > 700) face_both(f, 12 - mw / 2 - 1, my + 1, 'w');         /* a frown, a strained voice */
    else if (t[T_TUNE] > 600) face_both(f, 12 - mw / 2 - 1, my - 1, 'w');      /* a smile, a lively one */

    /* The face's extra. */
    if (extra == EXTRA_VISOR) {
        face_rect(f, xl + 2, ey - 2, 2 * half - 4, 1, 'm', 0), face_rect(f, xl + 2, ey + eh + 1, 2 * half - 4, 1, 'm', 0);
        face_line(f, xl + 2, ey - 2, xl + 2, ey + eh + 1, 'm', 1);
        face_rect(f, left, ey, ew, eh, 'g', 1);
    } else if (extra == EXTRA_BEARD) {
        for (int y = my - 1; y < yc + 3; ++y)
            for (int x = xl + 1; x < xr; ++x)
                if (face_get(f, x, y) == '.' && (x + y) % 2 == 0 && !(y == my && 12 - mw / 2 <= x && x < 12 + mw / 2))
                    face_set(f, x, y, y < yc ? 'm' : 'w');
    } else if (extra == EXTRA_EYE_PATCH) {         /* (as PIRATE's: a strap across, the patch under it, up to the top) */
        face_rect(f, xl + 1, ey, left + 2 - xl, 1, 'm', 0);
        face_rect(f, left - 1, ey + 1, 3, 2, 'm', 0);
        face_rect(f, left + 3, ey - 1, 2, 1, 'm', 0), face_set(f, left + 5, ey - 2, 'm');
        for (unsigned k = 0; k < 4; ++k) f->eye[0][k] = f->eye[1][k];
        f->eyes = 1;
    } else if (extra == EXTRA_MUSTACHE) {
        face_rect(f, 12 - mw / 2 - 1, my - 1, mw / 2 + 1, 1, 'm', 1);
        face_both(f, 12 - mw / 2 - 1, my, 'm');
    } else if (extra == EXTRA_FRECKLES) {
        face_rect(f, left - 1, ey + 3, 2, 1, '.', 1);
        face_both(f, left - 2, ey + 3, 'o'), face_both(f, left, ey + 3, 'o'), face_both(f, left - 1, ey + 4, 'o');
    } else if (extra == EXTRA_MONOCLE) {
        int rx = FACE_N - left - ew;
        for (int i = -1; i < ew + 1; ++i) face_set(f, rx + i, ey - 1, 'o'), face_set(f, rx + i, ey + eh, 'o');
        face_set(f, rx - 1, ey, 'o'), face_set(f, rx + ew, ey, 'o');
        face_line(f, rx + ew, ey + eh, rx + ew + 1, yc + 1, 'm', 0);
    } else if (extra == EXTRA_GLASSES) {
        for (unsigned e = 0; e < 2; ++e) {
            int x = f->eye[e][0], y = f->eye[e][1], ww = f->eye[e][2], hh = f->eye[e][3];
            for (int i = -1; i < ww + 1; ++i) face_set(f, x + i, y - 1, 'o'), face_set(f, x + i, y + hh, 'o');
            face_set(f, x - 1, y, 'o'), face_set(f, x + ww, y, 'o');
        }
        face_rect(f, left + ew + 1, ey - 1, 2 * (12 - left - ew - 1), 1, 'o', 0);
    }

    /* On top. */
    char hair = looks / 8 % 3 == 0 ? 'o' : 'm';
    if (fem && (top == TOP_CAP || top == TOP_BEANIE || top == TOP_PROPELLER || top == TOP_CROWN || top == TOP_TOP_HAT ||
                top == TOP_BANDANA || top == TOP_ANTENNA || top == TOP_HEADPHONES))
        for (int y = top == TOP_HEADPHONES ? ey + 2 : yt + 1; y < yc + 4; ++y)   /* long hair from under the hat */
            face_both(f, xl - 1 - (y > ey + 1), y, hair);
    if (top == TOP_TOP_HAT) {                     /* tall, a band round it, a brim */
        face_rect(f, xl + 2, yt - 5, 2 * half - 4, 1, 'w', 0);
        face_line(f, xl + 2, yt - 4, xl + 2, yt - 1, 'w', 1);
        face_rect(f, xl + 3, yt - 1, 2 * half - 6, 1, 'o', 0);
        face_rect(f, xl - 1, yt, 2 * half + 2, 1, 'w', 0);
    } else if (top == TOP_HEADPHONES) {           /* a band over the head, cups on the ears */
        face_rect(f, xl + 2, yt - 1, 2 * half - 4, 1, 'm', 0);
        face_both(f, xl + 1, yt, 'm'), face_line(f, xl, yt + 1, xl, ey - 2, 'm', 1);
        face_rect(f, xl - 2, ey - 1, 2, 4, 'b', 1);
    } else if (top == TOP_CROWN) {
        face_rect(f, xl + 2, yt - 1, 2 * half - 4, 1, 'o', 0);
        face_line(f, xl + 2, yt - 3, xl + 2, yt - 2, 'o', 1), face_line(f, 11, yt - 3, 11, yt - 2, 'o', 1);
        face_both(f, 11, yt - 1, 'b');
    } else if (top == TOP_MOHAWK) {
        face_rect(f, 11, yt - 4, 2, 5, 'o', 0);
        face_both(f, 10, yt - 2, 'o'), face_both(f, 10, yt - 1, 'o');
    } else if (top == TOP_BEANIE || top == TOP_PROPELLER) {   /* a knitted hat with a bobble, or a propeller */
        if (top == TOP_BEANIE) face_rect(f, 11, yt - 6, 2, 2, 'o', 0);
        else face_rect(f, 8, yt - 6, 8, 1, 'o', 0), face_rect(f, 11, yt - 5, 2, 1, 'w', 0);
        face_rect(f, xl + 3, yt - 4, 2 * half - 6, 1, 'b', 0);
        face_both(f, xl + 2, yt - 3, 'b'), face_line(f, xl + 1, yt - 2, xl + 1, yt - 1, 'b', 1);
        face_rect(f, xl, yt, 2 * half, 1, 'b', 0);
        if (top == TOP_BEANIE)
            for (int x = xl + 1; x < xr; x += 2) face_set(f, x, yt - 1, 'w');
        else face_rect(f, xl + 1, yt - 2, half - 1, 2, 'g', 0);
    } else if (top == TOP_SPIKES) {
        for (int x = xl; x <= xr; ++x) {
            int k = (x - xl) % 4;
            if (k == 2) face_set(f, x, yt - 2, 'w');
            else if (k == 1 || k == 3) face_set(f, x, yt - 1, 'w');
        }
        face_rect(f, xl, yt + 1, 2 * half, 2, 'b', 0);
        face_rect(f, xr + 2, yt, 2, 1, 'b', 0), face_set(f, xr + 1, yt + 3, 'b'), face_set(f, xr + 2, yt + 4, 'b');
    } else if (top == TOP_CAP) {
        face_rect(f, xl + 3, yt - 4, 2 * half - 6, 1, 'o', 0);
        face_both(f, xl + 2, yt - 3, 'o'), face_both(f, xl + 1, yt - 2, 'o'), face_both(f, xl + 1, yt - 1, 'o');
        face_rect(f, xl, yt, 2 * half + 4, 1, 'o', 0);
        face_set(f, 10, yt - 2, 'w');
    } else if (top == TOP_BANDANA) {
        face_rect(f, xl + 3, yt - 4, 2 * half - 6, 1, 'o', 0);
        face_both(f, xl + 2, yt - 3, 'o'), face_line(f, xl + 1, yt - 2, xl + 1, yt - 1, 'o', 1);
        face_rect(f, xl, yt, 2 * half, 1, 'o', 0);
        face_set(f, xr + 2, yt - 2, 'o'), face_set(f, xr + 3, yt - 2, 'o'), face_set(f, xr + 1, yt - 1, 'o');
        face_set(f, xr + 1, yt + 1, 'o'), face_set(f, xr + 2, yt + 2, 'o');
        face_set(f, xl + 3, yt - 2, 'w'), face_set(f, xr - 3, yt - 2, 'w');           /* a skull's eyes */
    } else if (top == TOP_HELMET) {               /* a knight's: the head's line its shell, closed round the face */
        face_rect(f, 11, yt - 5, 2, 1, 'o', 0), face_rect(f, 10, yt - 4, 4, 1, 'o', 0), face_rect(f, 11, yt - 3, 2, 1, 'o', 0);
        face_rect(f, xl + 2, yt - 2, 2 * half - 4, 1, 'w', 0);
        face_both(f, xl, yt - 1, 'w'), face_both(f, xl + 1, yt - 1, 'w');
        face_rect(f, xl, yt, 2 * half, 1, 'w', 0);                               /* the brow */
        face_rect(f, xl + 1, yt + 1, 2 * half - 2, 1, 'm', 0);
        for (int y = yt + 2; y < yc - jaw; ++y) face_rect(f, xl + 1, y, y >= ey + eh ? 2 : 1, 1, 'm', 1);   /* cheek plates */
        face_rect(f, 11, yt + 1, 2, ey + 2 - yt, 'm', 0);                        /* the nose guard */
        face_both(f, xl - 1, ey + eh + 1, 'm'), face_both(f, xl - 1, my + 1, 'm');   /* the hinges */
    } else if (top == TOP_ANTENNA) {
        face_line(f, 11, yt - 1, 11, yt - 2, 'w', 0), face_line(f, 12, yt - 1, 12, yt - 2, 'w', 0);
        face_rect(f, 11, yt - 4, 2, 2, 'o', 0);
    } else if (top == TOP_LONG_HAIR) {
        face_rect(f, xl + 3, yt + 1, 2 * half - 6, 1, 'm', 0);
        for (int y = yt + 1; y < yc + 3; ++y) face_both(f, xl - 1 - (y > ey + 2), y, 'm');
    } else if (top == TOP_BALD) {
        face_both(f, xl + 1, ey - 2, 'm'), face_both(f, xl + 1, ey - 3, 'm'), face_both(f, xl + 2, ey - 3, 'm');
    } else if (top == TOP_HOOD) {
        face_rect(f, xl + 2, yt - 3, 2 * half - 4, 1, 'b', 0);
        face_both(f, xl + 1, yt - 2, 'b'), face_both(f, xl, yt - 1, 'b');
        face_line(f, xl - 1, yt, xl - 1, yc, 'b', 1);
        face_line(f, xl - 1, yc + 1, xl - 3, yc + 3, 'b', 1);
        face_line(f, xl - 3, yc + 3, xl - 3, FACE_N - 1, 'b', 1);
    } else if (fem) {                             /* long hair, a ponytail, a bun or a bob; dark or ginger */
        int style = looks % 4;
        face_rect(f, xl + 1, yt + 1, 2 * half - 2, 1, hair, 0);
        face_both(f, xl + 1, yt + 2, hair), face_both(f, xl + 2, yt + 2, hair);
        if (style == 0) {                                                        /* long, past the shoulders */
            for (int y = yt; y < yc + 4; ++y) face_both(f, xl - 1 - (y > ey + 1), y, hair);
            face_both(f, xl - 1, yt, '.');
        } else if (style == 1) {                                                 /* a ponytail, tied at the side */
            face_rect(f, xr + 1, yt + 1, 2, 1, hair == 'm' ? 'o' : 'b', 0);
            for (int y = yt + 2; y < ey + 5; ++y) face_set(f, xr + 2 - (y > ey + 2), y, hair);
        } else if (style == 2) {                                                 /* a bun on top */
            face_rect(f, 10, yt - 3, 4, 1, hair, 0), face_both(f, 9, yt - 2, hair), face_rect(f, 10, yt - 1, 4, 1, hair, 0);
        } else {                                                                 /* a bob, to the jaw */
            face_line(f, xl - 1, yt + 1, xl - 1, my, hair, 1);
            face_rect(f, xl - 1, yt, 2, 1, hair, 1);
        }
    } else if (!machine) {                        /* hair, one of four, from the voice */
        int style = looks % 4;
        if (style == 0) {                                                        /* a fringe */
            face_rect(f, xl + 3, yt + 1, 2 * half - 6, 1, 'm', 0);
            face_both(f, xl + 1, yt + 2, 'm');
        } else if (style == 1) {                                                 /* parted at the side */
            face_rect(f, xl + 1, yt + 1, 2 * half - 2, 1, 'm', 0);
            face_rect(f, xl + 1, yt + 2, 3, 1, 'm', 0);
            face_set(f, xl + 4, yt + 1, '.');
        } else if (style == 2) {                                                 /* curls */
            for (int x = xl + 1; x < xr; ++x) face_set(f, x, yt - 1 + (x % 2), 'm');
            face_both(f, xl + 1, yt + 2, 'm'), face_both(f, xl + 2, yt + 2, 'm');
        } else {                                                                 /* a tuft */
            face_set(f, 13, yt - 1, 'w'), face_set(f, 14, yt - 2, 'w'), face_set(f, 12, yt - 2, 'w');
            face_rect(f, xl + 3, yt + 1, 2 * half - 6, 1, 'm', 0);
        }
    }

    /* The body. */
    int sy = yc + 2;
    face_both(f, 10, yc + 1, 'w');                                               /* the neck */
    char wear = body == BODY_STRIPES ? 'w' : body == BODY_ARMOUR ? 'm' : body == BODY_ROBE ? 'b' : high > 300 ? 'g' : 'b';
    face_rect(f, 4, sy, 6, 1, wear, 1);
    face_both(f, 3, sy + 1, wear);
    face_line(f, 2, sy + 2, 2, FACE_N - 1, wear, 1);
    if (body == BODY_ARMOUR) {
        face_both(f, 5, sy + 1, 'w'), face_both(f, 6, sy + 2, 'w'), face_both(f, 7, sy + 3, 'w');
        face_rect(f, 11, sy + 2, 2, 1, 'o', 0), face_rect(f, 10, sy + 3, 4, 1, 'o', 0), face_rect(f, 11, sy + 4, 2, 2, 'o', 0);
        face_line(f, 5, sy + 3, 5, FACE_N - 1, 'm', 1);
    } else if (body == BODY_ROBE) {
        face_both(f, 10, sy + 1, 'w'), face_both(f, 9, sy + 2, 'w');
        face_rect(f, 11, sy + 3, 2, 1, 'o', 0), face_rect(f, 10, sy + 4, 4, 1, 'o', 0), face_rect(f, 11, sy + 5, 2, 1, 'o', 0);
        face_line(f, 5, sy + 3, 5, FACE_N - 1, 'm', 1);
    } else if (body == BODY_STRIPES) {
        face_rect(f, 3, sy + 2, 9, 1, 'b', 1), face_rect(f, 3, sy + 4, 9, 1, 'b', 1);
    } else if (body == BODY_TEE) {
        face_rect(f, 3, sy + 3, 9, 1, 'o', 1);
        face_line(f, 5, sy + 4, 5, FACE_N - 1, 'm', 1);
        face_both(f, 11, sy, '.'), face_both(f, 10, sy + 1, 'w');
    } else {
        face_both(f, 11, sy, '.'), face_both(f, 10, sy + 1, 'w'), face_both(f, 11, sy + 2, 'w');   /* a collar */
        face_line(f, 5, sy + 3, 5, FACE_N - 1, 'm', 1);
        if (body == BODY_SCARF) face_rect(f, 9, yc + 1, 6, 2, 'o', 0), face_rect(f, 14, yc + 3, 2, 3, 'o', 0);
        else if (body == BODY_BOW_TIE)
            face_both(f, 10, sy, 'o'), face_both(f, 10, sy + 2, 'o'), face_rect(f, 10, sy + 1, 4, 1, 'o', 0);
        else if (body == BODY_CAPE) face_line(f, 3, sy + 1, 0, FACE_N - 1, 'b', 1), face_rect(f, 11, sy, 2, 1, 'o', 0);
    }
}

/* The 16x16 icon: each of its pixels the strongest stroke of the 1 or 2 x 1
 * or 2 portrait pixels under it (orange, blue, green, white, grey: the rule
 * the hand-drawn icons followed). */
static void voice_face_icon(const struct voice_face *f, char icon[FACE_ICON][FACE_ICON])
{
    static const char order[] = "obgwm";
    for (int j = 0; j < FACE_ICON; ++j)
        for (int i = 0; i < FACE_ICON; ++i) {
            char best = '.';
            unsigned rank = sizeof order - 1;
            for (int y = j * 3 / 2; y <= ((j + 1) * 3 - 1) / 2; ++y)
                for (int x = i * 3 / 2; x <= ((i + 1) * 3 - 1) / 2; ++x)
                    for (unsigned r = 0; r < rank; ++r)
                        if (order[r] == f->grid[y][x]) { best = order[r]; rank = r; break; }
            icon[j][i] = best;
        }
}
