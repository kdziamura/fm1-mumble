#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""MUMBLE's faces from their voices: an experiment before the firmware does it.

A character is assembled from parts on the 24x24 grid the portraits use (one
letter a pixel, '.' empty, the palette of app/mumble_sprites.inc), in the hand-
drawn cast's construction: a head with straight sides, a rounded or flat top
and a chin cut in 45-degree steps, ears at the eyes, 2x2 eyes, a nose pixel,
cheeks and a short mouth; a two-pixel neck; shoulders as a line stepping down
into straight sides.

The voice sets the proportions, in whole pixels:
  size (TUBE: a longer tract is a bigger person)   the head's width and height
  PITCH     a smaller head, a chin cut twice, cheeks (and slit eyes when low)
  TUNE      the eyes set apart; MOUTH the mouth 2, 4 or 6 wide

and the parts, from what stands out in the voice against the rest of the
cast (how far each setting is from the cast's average, in its spread): each
slot takes the part of the setting that stands out most, past a threshold.
  top    ROOM a hood, RADIO an antenna, SPEED spikes and a headband, DICTION a
         cap, a drilled voice (pushed, low, crisp, steady) a helmet, JITTER a
         bandana, VIBRATO long hair, WOBBLE bald, GLIDE a propeller beanie,
         PITCH a beanie with a bobble
  face   CRUSH a visor, WOBBLE a beard (low voices), JITTER + EFFORT an eye
         patch, a very crisp DICTION glasses
  body   AIR a ghost, CRUSH a robot, ROOM a robe, a drilled voice armour,
         JITTER stripes, PITCH a tee
The face also says where its mouth and eyes are (the rig the talking and
blinking overlays need), so nothing is placed by hand.

  base/.venv/bin/python tools/faces.py [OUTDIR]   contact sheets into OUTDIR (app/build/faces)
  base/.venv/bin/python tools/faces.py --dump     the check's faces as text (tests/faces.sh)
"""
from __future__ import annotations

import random
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
N = 24
PALETTE = {"w": (235, 235, 235), "m": (110, 110, 118), "b": (40, 110, 220), "g": (40, 175, 105), "o": (245, 100, 40)}


# --------------------------------------------------------------- the voices ---
def read_voices():
    """The settings' names and ranges, and the cast, from app/mumble.c."""
    src = (ROOT / "app/mumble.c").read_text()
    params = [(n, int(lo), int(hi)) for n, lo, hi in re.findall(r'\{"([A-Z]+)", (\d+), (\d+)\}', src)]
    cast = [(name, [int(v) for v in values.split(",")])
            for name, values in re.findall(r'\{"([A-Z]+)",\s*0x[0-9a-f]+,\s*\{\{([^}]*)\}\}\}', src)]
    return params, cast


PARAMS, CAST = read_voices()


# All of it in integers, as the firmware (app/mumble_faces.inc) does it, and with
# C's division (towards zero): the two draw the same pixels.
def tdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def isqrt(n):
    r = 0
    while (r + 1) * (r + 1) <= n:
        r += 1
    return r


K = len(CAST)
SUM = [sum(v[i] for _, v in CAST) for i in range(len(PARAMS))]
# The cast's spread of each setting, K x (its standard deviation), at least 12 % of its range.
SPREAD = [max(isqrt(sum((K * v[i] - SUM[i]) ** 2 for _, v in CAST) // K), K * (hi - lo) * 12 // 100)
          for i, (_, lo, hi) in enumerate(PARAMS)]


def traits(values):
    """Each setting in thousandths of its range, by name (lower case)."""
    return {n.lower(): (v - lo) * 1000 // (hi - lo) for (n, lo, hi), v in zip(PARAMS, values)}


def standing(values):
    """How far each setting stands out from the cast: in hundredths of its spread from its average."""
    return {n.lower(): tdiv((K * v - SUM[i]) * 100, SPREAD[i]) for i, ((n, _, _), v) in enumerate(zip(PARAMS, values))}


def pick(candidates, threshold=90):
    """The part whose setting stands out most, past the threshold (else None):
    candidates (part, standing, allowed), the first of equals; a candidate is
    allowed when its setting is high enough in itself (so a little of a rare
    setting is not a part)."""
    best, score = None, None
    for part, z, allowed in candidates:
        if allowed and (score is None or z > score):
            best, score = part, z
    return best if score is not None and score >= threshold else None


def read_portraits():
    """The hand-drawn portraits, for comparison."""
    src = (ROOT / "app/mumble_sprites.inc").read_text()
    body = src[src.index("voice_portraits"):]
    rows = re.findall(r'"([.a-z]{24})"', body)
    return [rows[i * N:(i + 1) * N] for i in range(len(CAST))]


# ----------------------------------------------------------------- the face ---
@dataclass
class Face:
    grid: list = field(default_factory=lambda: [["."] * N for _ in range(N)])
    mouth: tuple = (12, 16)                      # centre x, row
    mouth_w: int = 4
    eyes: list = field(default_factory=list)     # (x, y, w, h): what a blink closes
    parts: list = field(default_factory=list)    # what was chosen, for the labels

    def set(self, x, y, c):
        if 0 <= x < N and 0 <= y < N:
            self.grid[y][x] = c

    def get(self, x, y):
        return self.grid[y][x] if 0 <= x < N and 0 <= y < N else "."

    def both(self, x, y, c):
        """A pixel and its mirror (the figure is symmetric about x = 11.5)."""
        self.set(x, y, c)
        self.set(N - 1 - x, y, c)

    def rect(self, x, y, w, h, c, mirror=False):
        for j in range(h):
            for i in range(w):
                (self.both if mirror else self.set)(x + i, y + j, c)

    def line(self, x0, y0, x1, y1, c, mirror=False):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            (self.both if mirror else self.set)(x0, y0, c)
            if x0 == x1 and y0 == y1:
                return
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy


def rnd(num, den):
    """num / den to the nearest whole, halves up (num, den >= 0)."""
    return (2 * num + den) // (2 * den)


def build_robot(f, t, size):
    """The hand-drawn BOT's construction: a box head with its corners off,
    bolts at the sides, a screen with green eyes (and mouth), an antenna with
    a lamp, a jointed neck, shoulders over a chest panel. Size sets the
    head's width and height; RADIO the antenna; MOUTH the mouth."""
    half = 6 if size < 400 else 7
    xl = 12 - half
    yb = 13
    yt = yb - (8 + rnd(2 * min(1000, size + 300), 1000))
    f.rect(xl + 1, yt, 2 * half - 2, 1, "w"), f.rect(xl + 1, yb, 2 * half - 2, 1, "w")
    f.line(xl, yt + 1, xl, yb - 1, "w", mirror=True)
    ey = yt + 4
    f.rect(xl - 2, ey - 1, 2, 1, "m", mirror=True), f.rect(xl - 2, ey + 2, 2, 1, "m", mirror=True)
    f.line(xl - 2, ey, xl - 2, ey + 1, "m", mirror=True)
    f.rect(xl + 3, yt + 2, 2 * half - 6, 1, "m"), f.rect(xl + 3, yt + 8, 2 * half - 6, 1, "m")
    f.line(xl + 2, yt + 3, xl + 2, yt + 7, "m", mirror=True)
    left = 12 - 2 - (1 + rnd(t["tune"], 1000))
    f.rect(left, ey, 2, 2, "g", mirror=True)
    f.eyes = [(left, ey, 2, 2), (24 - left - 2, ey, 2, 2)]
    mw = 2 if t["mouth"] < 600 else 4
    f.rect(12 - mw // 2, ey + 3, mw // 2, 1, "g", mirror=True)
    f.mouth, f.mouth_w = (12, ey + 3), mw
    if t["radio"] > 200:
        f.rect(11, yt - 1, 2, 1, "w")
        f.rect(11, yt - 3, 2, 2, "o")
    for y in range(yb + 1, yb + 4):
        f.both(10, y, "m")
    f.rect(10, yb + 2, 4, 1, "m")
    sy = yb + 4
    f.rect(4, sy, 16, 1, "w")
    f.both(3, sy + 1, "w")
    f.line(2, sy + 2, 2, N - 1, "w", mirror=True)
    f.rect(6, sy + 2, 12, 1, "b")
    f.line(6, sy + 3, 6, N - 1, "b", mirror=True)
    if t["crush"] > 700:
        f.rect(9, sy + 4, 2, 1, "o"), f.rect(13, sy + 4, 2, 1, "g")


def build_ghost(f, t, size):
    """The hand-drawn GHOST's construction: a dome, tall eyes, little arms out
    at the sides, a body narrowing to a wavy hem with feet. Size sets its
    width; ECHO hangs dots about it; PITCH cheeks; MOUTH the mouth."""
    xl = 4 if size > 350 else 5
    half = 12 - xl
    yt = 1
    f.rect(xl + 5, yt, 2 * half - 10, 1, "w")
    f.both(xl + 3, yt + 1, "w"), f.both(xl + 4, yt + 1, "w")
    f.both(xl + 2, yt + 2, "w")
    f.line(xl + 1, yt + 3, xl + 1, yt + 4, "w", mirror=True)
    arm = yt + 11
    f.line(xl, yt + 5, xl, arm + 4, "w", mirror=True)
    f.rect(xl - 2, arm, 2, 1, "w", mirror=True), f.both(xl - 3, arm + 1, "w"), f.rect(xl - 2, arm + 2, 2, 1, "w", mirror=True)
    f.line(xl + 1, arm + 5, xl + 1, 21, "w", mirror=True)
    f.both(xl + 4, 21, "w")
    f.rect(xl + 2, 22, 2, 1, "w", mirror=True)
    f.rect(xl + 5, 22, 12 - (xl + 5), 1, "w", mirror=True)
    ey = yt + 6
    eh = 3 if t["pitch"] > 150 else 2
    left = 12 - 2 - (1 + rnd(2 * t["tune"], 1000))
    f.rect(left, ey, 2, eh, "w", mirror=True)
    f.eyes = [(left, ey, 2, eh), (24 - left - 2, ey, 2, eh)]
    if t["pitch"] > 150:
        f.rect(left - 1, ey + 3, 2, 1, "o", mirror=True)
    mw = 2 if t["mouth"] < 600 else 4
    f.rect(12 - mw // 2, ey + 4, mw // 2, 1, "w", mirror=True)
    f.mouth, f.mouth_w = (12, ey + 4), mw
    if t["echo"] > 400:
        for (x, y) in ((1, 3), (22, 5), (2, 19), (21, 18), (0, 9), (23, 11)):
            f.set(x, y, "b")


def build(values, top_override=None) -> Face:
    """A figure in the hand-drawn cast's construction: a head with straight
    sides, a rounded or flat top and a chin cut in 45-degree steps, ears at
    the eyes, 2x2 eyes, a nose, cheeks and a short mouth; a two-pixel neck;
    shoulders as a line stepping down into straight sides. The voice sets the
    proportions in whole pixels and picks the parts."""
    t, z = traits(values), standing(values)
    f = Face()
    size, high = 1000 - t["tube"], t["pitch"]

    # The parts, from what stands out. A drilled voice (pushed, low, crisp,
    # steady) is a soldier's; a pushed, rough one is not.
    drilled = z["effort"] - tdiv(z["pitch"], 2) - tdiv(z["jitter"], 2) + tdiv(z["diction"], 2)
    body = pick([("ghost", z["air"], t["air"] > 600), ("robot", z["crush"], t["crush"] > 400),
                 ("robe", z["room"], t["room"] > 500), ("armour", drilled, t["effort"] > 600),
                 ("stripes", z["jitter"], t["jitter"] > 600), ("tee", z["pitch"], t["pitch"] > 450)])
    if body == "robot":
        f.parts = ["antenna", "robot"] if t["radio"] > 200 else ["robot"]
        build_robot(f, t, size)
        return f
    if body == "ghost":
        f.parts = ["ghost"]
        build_ghost(f, t, size)
        return f
    top = pick([("hood", z["room"], t["room"] > 550), ("antenna", z["radio"], t["radio"] > 350),
                ("spikes", z["speed"], t["speed"] > 400), ("cap", z["diction"], t["diction"] > 500),
                ("helmet", drilled, t["effort"] > 600), ("bandana", z["jitter"], t["jitter"] > 600),
                ("long hair", z["vibrato"], t["vibrato"] > 350), ("bald", z["wobble"], t["wobble"] > 400),
                ("propeller", z["glide"], t["glide"] > 350), ("beanie", z["pitch"], t["pitch"] > 450)])
    extra = pick([("visor", z["crush"], t["crush"] > 500),
                  ("beard", z["wobble"] - tdiv(z["pitch"] * 7, 10), t["wobble"] > 400),
                  ("eye patch", tdiv((z["jitter"] + z["effort"]) * 6, 10) - tdiv(z["mouth"] * 4, 10), t["jitter"] > 600),
                  ("glasses", tdiv(z["diction"] * 8, 10), z["diction"] > 120)], threshold=70)
    if top_override:
        top = top_override
    machine = extra == "visor"
    f.parts = [p for p in (top, extra, body) if p]

    # Proportions, in whole pixels: half the head's width (its left side at
    # 12 - half), its height to the chin line (row 16), the jaw's steps.
    half = 6 if 6 * size + 3 * t["effort"] - 5 * high < 500 else 8 if (size > 900 and high < 400) else 7
    h = 11 + rnd(3 * min(1000, max(0, 250 + tdiv(8 * size - 3 * high, 10))), 1000)
    tall_top = top in ("cap", "bandana", "helmet", "beanie", "propeller")
    if tall_top:
        h = min(h, 11)
    yc = 16
    yt = yc - h + 1
    xl, xr = 12 - half, 11 + half
    jaw = 0 if machine else 2 if (high > 550 and t["effort"] < 600) else 1
    rounded = not machine and top not in ("cap", "bandana", "spikes", "beanie", "propeller", "helmet")

    # The head.
    if rounded:
        f.rect(xl + 3, yt, 2 * half - 6, 1, "w")
        f.both(xl + 1, yt + 1, "w"), f.both(xl + 2, yt + 1, "w")
        side_top = yt + 2
    else:
        f.rect(xl, yt, 2 * half, 1, "w")
        side_top = yt + 1
    side_end = yc - 1 - jaw
    f.line(xl, side_top, xl, side_end, "w", mirror=True)
    for k in range(jaw):
        f.both(xl + 1 + k, side_end + 1 + k, "w")
    f.rect(xl + 1 + jaw, yc, 2 * (half - 1 - jaw), 1, "w")

    # Eyes (TUNE sets them apart), ears or a machine's bolts at them.
    ey = yt + (4 if top == "cap" else 3) if tall_top else yt + rnd(h * 42, 100)
    ew, eh = 2, (1 if high < 50 else 2)
    left = max(12 - ew - (1 + rnd(2 * t["tune"], 1000)), xl + 2)
    f.rect(left, ey, ew, eh, "w", mirror=True)
    f.eyes = [(left, ey, ew, eh), (24 - left - ew, ey, ew, eh)]
    if machine:
        f.rect(xl - 2, ey - 1, 2, 4, "m", mirror=True)
        f.rect(xl - 1, ey, 1, 2, ".", mirror=True)
    elif top not in ("hood", "long hair", "helmet"):
        f.both(xl - 1, ey, "w"), f.both(xl - 2, ey + 1, "w"), f.both(xl - 1, ey + 2, "w")

    # Nose, cheeks, mouth.
    if not machine:
        f.set(12, ey + 2, "m")
        if t["tube"] < 300:
            f.set(12, ey + 3, "m")
    if not machine and high > 150 and extra != "beard" and top != "helmet":
        f.rect(left - 1, ey + 3, 2, 1, "o", mirror=True)
    mw = 2 if t["mouth"] < 450 else 4 if t["mouth"] < 850 else 6
    my = min(ey + 4, yc - 2)
    f.rect(12 - mw // 2, my, mw // 2, 1, "w", mirror=True)
    f.mouth, f.mouth_w = (12, my), mw

    # The face's extra.
    if extra == "visor":
        f.rect(xl + 2, ey - 2, 2 * half - 4, 1, "m"), f.rect(xl + 2, ey + eh + 1, 2 * half - 4, 1, "m")
        f.line(xl + 2, ey - 2, xl + 2, ey + eh + 1, "m", mirror=True)
        f.rect(left, ey, ew, eh, "g", mirror=True)
    elif extra == "beard":
        for y in range(my - 1, yc + 3):
            for x in range(xl + 1, xr):
                if f.get(x, y) == "." and (x + y) % 2 == 0 and not (y == my and 12 - mw // 2 <= x < 12 + mw // 2):
                    f.set(x, y, "m" if y < yc else "w")
    elif extra == "eye patch":                   # (as PIRATE's: a strap across, the patch under it, up to the top)
        f.rect(xl + 1, ey, left + 2 - xl, 1, "m")
        f.rect(left - 1, ey + 1, 3, 2, "m")
        f.rect(left + 3, ey - 1, 2, 1, "m"), f.set(left + 5, ey - 2, "m")
        f.eyes = f.eyes[1:]
    elif extra == "glasses":
        for (x, y, ww, hh) in f.eyes:
            for i in range(-1, ww + 1):
                f.set(x + i, y - 1, "o"), f.set(x + i, y + hh, "o")
            f.set(x - 1, y, "o"), f.set(x + ww, y, "o")
        f.rect(left + ew + 1, ey - 1, 2 * (12 - left - ew - 1), 1, "o")

    # On top.
    if top == "beanie":                          # a knitted hat with a bobble
        f.rect(11, yt - 6, 2, 2, "o")
        f.rect(xl + 3, yt - 4, 2 * half - 6, 1, "b")
        f.both(xl + 2, yt - 3, "b"), f.line(xl + 1, yt - 2, xl + 1, yt - 1, "b", mirror=True)
        f.rect(xl, yt, 2 * half, 1, "b")
        for x in range(xl + 1, xr, 2):
            f.set(x, yt - 1, "w")
    elif top == "propeller":                     # a beanie with a propeller
        f.rect(8, yt - 6, 8, 1, "o"), f.rect(11, yt - 5, 2, 1, "w")
        f.rect(xl + 3, yt - 4, 2 * half - 6, 1, "b")
        f.both(xl + 2, yt - 3, "b"), f.line(xl + 1, yt - 2, xl + 1, yt - 1, "b", mirror=True)
        f.rect(xl, yt, 2 * half, 1, "b")
        f.rect(xl + 1, yt - 2, half - 1, 2, "g")
    elif top == "spikes":
        for x in range(xl, xr + 1):
            k = (x - xl) % 4
            if k == 2:
                f.set(x, yt - 2, "w")
            elif k in (1, 3):
                f.set(x, yt - 1, "w")
        f.rect(xl, yt + 1, 2 * half, 2, "b")
        f.rect(xr + 2, yt, 2, 1, "b"), f.rect(xr + 1, yt + 3, 1, 1, "b"), f.rect(xr + 2, yt + 4, 1, 1, "b")
    elif top == "cap":
        f.rect(xl + 3, yt - 4, 2 * half - 6, 1, "o")
        f.both(xl + 2, yt - 3, "o"), f.both(xl + 1, yt - 2, "o"), f.both(xl + 1, yt - 1, "o")
        f.rect(xl, yt, 2 * half + 4, 1, "o")
        f.set(12 - 2, yt - 2, "w")
    elif top == "bandana":
        f.rect(xl + 3, yt - 4, 2 * half - 6, 1, "o")
        f.both(xl + 2, yt - 3, "o"), f.line(xl + 1, yt - 2, xl + 1, yt - 1, "o", mirror=True)
        f.rect(xl, yt, 2 * half, 1, "o")
        f.set(xr + 2, yt - 2, "o"), f.set(xr + 3, yt - 2, "o"), f.set(xr + 1, yt - 1, "o")
        f.set(xr + 1, yt + 1, "o"), f.set(xr + 2, yt + 2, "o")
        f.set(xl + 3, yt - 2, "w"), f.set(xr - 3, yt - 2, "w")      # a skull's eyes
    elif top == "helmet":                        # a knight's: the head's line its shell, closed round the face
        f.rect(11, yt - 5, 2, 1, "o"), f.rect(10, yt - 4, 4, 1, "o"), f.rect(11, yt - 3, 2, 1, "o")   # a plume
        f.rect(xl + 2, yt - 2, 2 * half - 4, 1, "w")
        f.both(xl, yt - 1, "w"), f.both(xl + 1, yt - 1, "w")
        f.rect(xl, yt, 2 * half, 1, "w")                                    # the brow
        f.rect(xl + 1, yt + 1, 2 * half - 2, 1, "m")
        for y in range(yt + 2, yc - jaw):                                   # cheek plates, down to the jaw
            f.rect(xl + 1, y, 2 if y >= ey + eh else 1, 1, "m", mirror=True)
        f.rect(11, yt + 1, 2, ey + 2 - yt, "m")                             # the nose guard
        for y in (ey + eh + 1, my + 1):
            f.both(xl - 1, y, "m")                                          # the hinges
    elif top == "antenna":
        f.line(11, yt - 1, 11, yt - 2, "w"), f.line(12, yt - 1, 12, yt - 2, "w")
        f.rect(11, yt - 4, 2, 2, "o")
    elif top == "long hair":
        f.rect(xl + 3, yt + 1, 2 * half - 6, 1, "m")
        for y in range(yt + 1, yc + 3):
            f.both(xl - 1 - (y > ey + 2), y, "m")
    elif top == "bald":
        f.both(xl + 1, ey - 2, "m"), f.both(xl + 1, ey - 3, "m"), f.both(xl + 2, ey - 3, "m")
    elif top == "hood":
        f.rect(xl + 2, yt - 3, 2 * half - 4, 1, "b")
        f.both(xl + 1, yt - 2, "b"), f.both(xl, yt - 1, "b")
        f.line(xl - 1, yt, xl - 1, yc, "b", mirror=True)
        f.line(xl - 1, yc + 1, xl - 3, yc + 3, "b", mirror=True)
        f.line(xl - 3, yc + 3, xl - 3, N - 1, "b", mirror=True)
    elif top is None and not machine:
        f.rect(xl + 3, yt + 1, 2 * half - 6, 1, "m")                       # a fringe
        f.both(xl + 1, yt + 2, "m")

    # The body.
    sy = yc + 2
    f.both(10, yc + 1, "w")                                                   # the neck
    wear = "w" if body == "stripes" else "m" if body == "armour" else "b" if body == "robe" else "g" if high > 300 else "b"
    f.rect(4, sy, 6, 1, wear, mirror=True)
    f.both(3, sy + 1, wear)
    f.line(2, sy + 2, 2, N - 1, wear, mirror=True)
    if body == "armour":
        f.both(5, sy + 1, "w"), f.both(6, sy + 2, "w"), f.both(7, sy + 3, "w")
        f.rect(11, sy + 2, 2, 1, "o"), f.rect(10, sy + 3, 4, 1, "o"), f.rect(11, sy + 4, 2, 2, "o")
        f.line(5, sy + 3, 5, N - 1, "m", mirror=True)
    elif body == "robe":
        f.both(10, sy + 1, "w"), f.both(9, sy + 2, "w")
        f.rect(11, sy + 3, 2, 1, "o"), f.rect(10, sy + 4, 4, 1, "o"), f.rect(11, sy + 5, 2, 1, "o")
        f.line(5, sy + 3, 5, N - 1, "m", mirror=True)
    elif body == "stripes":
        f.rect(3, sy + 2, 9, 1, "b", mirror=True), f.rect(3, sy + 4, 9, 1, "b", mirror=True)
    elif body == "tee":
        f.rect(3, sy + 3, 9, 1, "o", mirror=True)
        f.line(5, sy + 4, 5, N - 1, "m", mirror=True)
        f.both(11, sy, "."), f.both(10, sy + 1, "w")
    else:
        f.both(11, sy, "."), f.both(10, sy + 1, "w"), f.both(11, sy + 2, "w")   # a collar
        f.line(5, sy + 3, 5, N - 1, "m", mirror=True)
    return f


ICON_ORDER = "obgwm"


def icon(grid):
    """The 16x16 icon: each of its pixels the strongest stroke of the 1 or 2
    x 1 or 2 portrait pixels under it (orange, blue, green, white, grey: the
    rule the hand-drawn icons follow)."""
    out = []
    for j in range(16):
        row = []
        for i in range(16):
            under = [grid[y][x] for y in range(j * 3 // 2, ((j + 1) * 3 - 1) // 2 + 1)
                     for x in range(i * 3 // 2, ((i + 1) * 3 - 1) // 2 + 1)]
            row.append(next((c for c in ICON_ORDER if c in under), "."))
        out.append(row)
    return out


# ------------------------------------------------------- talking, blinking ---
VOWEL_MOUTH = {"AH": (4, 3), "EE": (4, 1), "OO": (2, 2), "AW": (3, 3)}


def pose(face: Face, vowel=None, blink=False) -> list:
    """The face with its mouth open on a vowel, or its eyes shut."""
    g = [row[:] for row in face.grid]
    if blink:
        for (x, y, w, h) in face.eyes:
            for j in range(h):
                for i in range(w):
                    g[y + j][x + i] = "."
            for i in range(w):
                g[y + h - 1][x + i] = "w"
    if vowel:
        mx, my = face.mouth
        w, h = VOWEL_MOUTH[vowel]
        w = min(w, face.mouth_w)
        for i in range(-face.mouth_w // 2, face.mouth_w // 2):
            g[my][mx + i] = "."
        for j in range(h):
            for i in range(w):
                ring = h >= 3 and w >= 3 and 0 < i < w - 1 and 0 < j < h - 1
                y, x = my + j - (h > 1), mx - w // 2 + i
                if 0 <= y < N and 0 <= x < N:
                    g[y][x] = "." if ring else "w"
    return g


# ----------------------------------------------------------------- sheets ---
SCALE = 3


def draw(grid, img, ox, oy, scale=SCALE):
    d = ImageDraw.Draw(img)
    for y, row in enumerate(grid):
        for x, c in enumerate(row):
            if c in PALETTE:
                d.rectangle([ox + x * scale, oy + y * scale, ox + (x + 1) * scale - 1, oy + (y + 1) * scale - 1],
                            fill=PALETTE[c])


def sheet(cells, columns, label_rows=2, title=None):
    """cells: (grid, label) pairs into a black sheet, a label under each."""
    cw, ch = N * SCALE + 44, N * SCALE + 10 + 10 * label_rows
    rows = (len(cells) + columns - 1) // columns
    top = 18 if title else 0
    img = Image.new("RGB", (columns * cw + 8, rows * ch + 8 + top), (0, 0, 0))
    d = ImageDraw.Draw(img)
    if title:
        d.text((8, 4), title, fill=(160, 160, 170))
    for k, (grid, label) in enumerate(cells):
        x, y = 8 + (k % columns) * cw, 8 + top + (k // columns) * ch
        draw(grid, img, x, y)
        for i, line in enumerate(label.split("\n")[:label_rows]):
            d.text((x, y + N * SCALE + 3 + 10 * i), line, fill=(150, 150, 160))
    return img


def label(name, face):
    return name + "\n" + (" ".join(p.replace(" ", "-") for p in face.parts) or "-")


def check_voices(n=400):
    """The voices tests/faces.sh draws both ways: the cast, then n rolled from
    them (halfway to random, a fixed LCG), then every setting at both ends."""
    voices = [v for _, v in CAST]
    x = 1
    for k in range(n):
        base = CAST[k % len(CAST)][1]
        v = []
        for b, (_, lo, hi) in zip(base, PARAMS):
            x = (x * 1103515245 + 12345) & 0x7FFFFFFF
            v.append((b + lo + (x >> 8) % (hi - lo + 1)) // 2)
        voices.append(v)
    for i, (_, lo, hi) in enumerate(PARAMS):
        for end in (lo, hi):
            v = list(CAST[0][1])
            v[i] = end
            voices.append(v)
    return voices


def dump(f):
    """A face as text: its grid, its rig, its icon."""
    lines = ["".join(r) for r in f.grid]
    lines.append(f"mouth {f.mouth[1]} {f.mouth_w} eyes " + " ".join(f"{x},{y},{w},{h}" for x, y, w, h in f.eyes))
    lines += ["".join(r) for r in icon(f.grid)]
    return "\n".join(lines)


def main():
    if sys.argv[1:2] == ["--dump"]:
        print("\n".join(dump(build(v)) for v in check_voices()))
        return
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "app/build/faces"
    out.mkdir(parents=True, exist_ok=True)
    hand = read_portraits()

    # 1. The cast: drawn by hand, then built from their voices.
    cells = [(g, n + "\n(by hand)") for g, (n, _) in zip(hand, CAST)]
    cells += [(build(v).grid, label(n, build(v))) for n, v in CAST]
    sheet(cells, len(CAST), title="the cast: by hand (top), from their voices (bottom)").save(out / "cast.png")

    # 2. One setting at a time, low to high, the rest HERO's.
    hero = CAST[0][1]
    cells = []
    for i, (name, lo, hi) in enumerate(PARAMS):
        for k in range(5):
            v = list(hero)
            v[i] = lo + (hi - lo) * k // 4
            cells.append((build(v).grid, label(f"{name} {v[i]}", build(v))))
    sheet(cells, 10, title="each setting low to high, the rest HERO's (two settings a row)").save(out / "sweep.png")

    # 3. Rolled voices, as LFO rolls them: halfway between a character and random.
    rng = random.Random(7)
    cells = []
    for k in range(32):
        name, base = CAST[k % len(CAST)]
        v = [(b + rng.randint(lo, hi)) // 2 for b, (_, lo, hi) in zip(base, PARAMS)]
        cells.append((build(v).grid, label(name + " rolled", build(v))))
    sheet(cells, 8, title="rolled voices (LFO): halfway between the character and random").save(out / "rolled.png")

    # 4. The rig: each face talking and blinking where the generator put its parts.
    cells = []
    for name, v in CAST:
        f = build(v)
        for vowel, blink in ((None, False), ("AH", False), ("EE", False), ("OO", False), (None, True)):
            cells.append((pose(f, vowel, blink), f"{name}\n{vowel or ('blink' if blink else 'still')}"))
    sheet(cells, 10, title="the rig: talking (AH EE OO) and blinking, two characters a row").save(out / "rig.png")
    print("\n".join(str(out / n) for n in ("cast.png", "sweep.png", "rolled.png", "rig.png")))


if __name__ == "__main__":
    main()
