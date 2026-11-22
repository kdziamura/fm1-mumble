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
         bandana, VIBRATO long hair, WOBBLE bald, GLIDE ELDER's pointed hat
  face   CRUSH a visor, WOBBLE a beard (low voices), JITTER + EFFORT an eye
         patch, a very crisp DICTION glasses
  body   AIR a ghost, CRUSH a robot, ROOM a robe, a drilled voice armour,
         JITTER stripes, PITCH a tee
The face also says where its mouth and eyes are (the rig the talking and
blinking overlays need), so nothing is placed by hand.

  base/.venv/bin/python tools/faces.py [OUTDIR]   contact sheets into OUTDIR (app/build/faces)
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
MEAN = [sum(v[i] for _, v in CAST) / len(CAST) for i in range(len(PARAMS))]
SPREAD = [max((sum((v[i] - MEAN[i]) ** 2 for _, v in CAST) / len(CAST)) ** 0.5, 0.12 * (hi - lo))
          for i, (_, lo, hi) in enumerate(PARAMS)]


def traits(values):
    """Each setting as 0..1 of its range, by name (lower case)."""
    return {n.lower(): (v - lo) / (hi - lo) for (n, lo, hi), v in zip(PARAMS, values)}


def standing(values):
    """How far each setting stands out from the cast: in spreads from its average."""
    return {n.lower(): (v - MEAN[i]) / SPREAD[i] for i, ((n, _, _), v) in enumerate(zip(PARAMS, values))}


def pick(candidates, threshold=0.9):
    """The part whose setting stands out most, past the threshold (else None);
    a candidate (part, standing, level) needs its setting at least that high
    too (0..1 of its range), so a little of a rare setting is not a part."""
    best = max(candidates, key=lambda c: c[1] if len(c) < 3 or c[2] else -9)
    return best[0] if best[1] >= threshold and (len(best) < 3 or best[2]) else None


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


def outline(shape, face, colour, broken=False, open_below=False):
    """The cells of a shape with an outside neighbour, drawn as its line
    (open_below: the screen's bottom edge is not an edge of it)."""
    for (x, y) in shape:
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if (nx, ny) not in shape and not (open_below and ny >= N):
                if not broken or (x + y) % 2 == 0:
                    face.set(x, y, colour)
                break


# ELDER's hat, drawn by hand: a bent tip, a star, a band and a curled brim
# (its last row the head's top).
WIZARD_HAT = [
    "...............bb.......",
    "..............b.b.......",
    ".............b..b.......",
    "............b.o..b......",
    "...........b.ooo.b......",
    "..........b...o...b.....",
    ".........oooooooooo.....",
    "..bbbbbbbbbbbbbbbbbbbb..",
    ".b..bbbbbbbbbbbbbbbb..b.",
]


def build_robot(f, t, size):
    """The hand-drawn BOT's construction: a box head with its corners off,
    bolts at the sides, a screen with green eyes (and mouth), an antenna with
    a lamp, a jointed neck, shoulders over a chest panel. Size sets the
    head's width and height; RADIO the antenna; MOUTH the mouth."""
    half = 6 if size < 0.4 else 7
    xl, xr = 12 - half, 11 + half
    yb = 13
    yt = yb - (8 + round(2 * min(1.0, size + 0.3)))
    f.rect(xl + 1, yt, 2 * half - 2, 1, "w"), f.rect(xl + 1, yb, 2 * half - 2, 1, "w")
    f.line(xl, yt + 1, xl, yb - 1, "w", mirror=True)
    ey = yt + 4
    f.rect(xl - 2, ey - 1, 2, 1, "m", mirror=True), f.rect(xl - 2, ey + 2, 2, 1, "m", mirror=True)
    f.line(xl - 2, ey, xl - 2, ey + 1, "m", mirror=True)
    f.rect(xl + 3, yt + 2, 2 * half - 6, 1, "m"), f.rect(xl + 3, yt + 8, 2 * half - 6, 1, "m")
    f.line(xl + 2, yt + 3, xl + 2, yt + 7, "m", mirror=True)
    gap = 1 + round(t["tune"])
    left = 12 - gap - 2
    f.rect(left, ey, 2, 2, "g", mirror=True)
    f.eyes = [(left, ey, 2, 2), (24 - left - 2, ey, 2, 2)]
    mw = 2 if t["mouth"] < 0.6 else 4
    f.rect(12 - mw // 2, ey + 3, mw // 2, 1, "g", mirror=True)
    f.mouth, f.mouth_w = (12, ey + 3), mw
    if t["radio"] > 0.2:
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
    if t["crush"] > 0.7:
        f.rect(9, sy + 4, 2, 1, "o"), f.rect(13, sy + 4, 2, 1, "g")


def build_ghost(f, t, size):
    """The hand-drawn GHOST's construction: a dome, tall eyes, little arms out
    at the sides, a body narrowing to a wavy hem with feet. Size sets its
    width; ECHO hangs dots about it; PITCH cheeks; MOUTH the mouth."""
    xl = 4 if size > 0.35 else 5
    xr, half = 23 - xl, 12 - xl
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
    eh = 3 if t["pitch"] > 0.15 else 2
    gap = 1 + round(2 * t["tune"])
    left = 12 - gap - 2
    f.rect(left, ey, 2, eh, "w", mirror=True)
    f.eyes = [(left, ey, 2, eh), (24 - left - 2, ey, 2, eh)]
    if t["pitch"] > 0.15:
        f.rect(left - 1, ey + 3, 2, 1, "o", mirror=True)
    mw = 2 if t["mouth"] < 0.6 else 4
    f.rect(12 - mw // 2, ey + 4, mw // 2, 1, "w", mirror=True)
    f.mouth, f.mouth_w = (12, ey + 4), mw
    if t["echo"] > 0.4:
        for (x, y) in ((1, 3), (22, 5), (2, 19), (21, 18), (0, 9), (23, 11)):
            f.set(x, y, "b")


def build(values) -> Face:
    """A figure in the hand-drawn cast's construction: a head with straight
    sides, a rounded or flat top and a chin cut in 45-degree steps, ears at
    the eyes, 2x2 eyes, a nose, cheeks and a short mouth; a two-pixel neck;
    shoulders as a line stepping down into straight sides. The voice sets the
    proportions in whole pixels and picks the parts."""
    t, z = traits(values), standing(values)
    f = Face()
    size, high = 1 - t["tube"], t["pitch"]

    # The parts, from what stands out. A drilled voice (pushed, low, crisp,
    # steady) is a soldier's; a pushed, rough one is not.
    drilled = z["effort"] - 0.5 * z["pitch"] - 0.5 * z["jitter"] + 0.5 * z["diction"]
    body = pick([("ghost", z["air"], t["air"] > 0.6), ("robot", z["crush"], t["crush"] > 0.4),
                 ("robe", z["room"], t["room"] > 0.5), ("armour", drilled, t["effort"] > 0.6),
                 ("stripes", z["jitter"], t["jitter"] > 0.6), ("tee", z["pitch"], t["pitch"] > 0.45)])
    top = pick([("hood", z["room"], t["room"] > 0.55 and body != "ghost"), ("antenna", z["radio"], t["radio"] > 0.35),
                ("spikes", z["speed"], t["speed"] > 0.4), ("cap", z["diction"], t["diction"] > 0.5),
                ("helmet", drilled, t["effort"] > 0.6 and body != "ghost"),
                ("bandana", z["jitter"], t["jitter"] > 0.6 and body != "ghost"),
                ("long hair", z["vibrato"], t["vibrato"] > 0.35), ("bald", z["wobble"], t["wobble"] > 0.4),
                ("pointed hat", z["glide"], t["glide"] > 0.35 and body != "ghost")])
    extra = pick([("visor", z["crush"], t["crush"] > 0.5),
                  ("beard", z["wobble"] - z["pitch"] * 0.7, t["wobble"] > 0.4 and body != "ghost"),
                  ("eye patch", (z["jitter"] + z["effort"]) * 0.6 - z["mouth"] * 0.4, t["jitter"] > 0.6),
                  ("glasses", z["diction"] * 0.8, z["diction"] > 1.2)], threshold=0.7)
    if body == "robot" and top not in ("antenna", "cap", "helmet", "pointed hat"):
        top = "antenna" if t["radio"] > 0.2 else None
    machine = body == "robot" or extra == "visor"
    f.parts = [p for p in (top, extra, body) if p]
    if body == "robot":
        f.parts = ["antenna", "robot"] if t["radio"] > 0.2 else ["robot"]
        build_robot(f, t, size)
        return f
    if body == "ghost":
        f.parts = ["ghost"]
        build_ghost(f, t, size)
        return f

    # Proportions, in whole pixels: half the head's width (its left side at
    # 12 - half), its height to the chin line (row 16), the jaw's steps.
    half = 6 if (0.6 * size + 0.3 * t["effort"] - 0.5 * high) < 0.05 else 8 if (size > 0.9 and high < 0.4) else 7
    h = 11 + round(3 * min(1.0, max(0.0, 0.25 + 0.8 * size - 0.3 * high)))
    tall_top = top in ("pointed hat", "cap", "bandana", "helmet")
    if tall_top:
        h = 9 if top == "pointed hat" else min(h, 11)
    yc = 16 if body != "ghost" else 14
    yt = yc - h + 1
    xl, xr = 12 - half, 11 + half
    jaw = 0 if machine else 2 if (high > 0.55 and t["effort"] < 0.6) else 1
    rounded = not machine and top not in ("cap", "bandana", "spikes", "pointed hat")

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
    ey = yt + (4 if top == "cap" else 3) if tall_top else yt + round(h * 0.42)
    gap = 1 + round(2 * t["tune"])
    ew, eh = 2, (1 if high < 0.05 else 2)
    left = 12 - gap - ew
    if left <= xl + 1:
        left = xl + 2
    f.rect(left, ey, ew, eh, "w", mirror=True)
    f.eyes = [(left, ey, ew, eh), (23 - left - ew + 1, ey, ew, eh)]
    if machine:
        f.rect(xl - 2, ey - 1, 2, 4, "m", mirror=True)
        f.rect(xl - 1, ey, 1, 2, ".", mirror=True)
    elif body != "ghost" and top not in ("hood", "long hair"):
        f.both(xl - 1, ey, "w"), f.both(xl - 2, ey + 1, "w"), f.both(xl - 1, ey + 2, "w")

    # Nose, cheeks, mouth.
    if not machine:
        f.set(12, ey + 2, "m")
        if t["tube"] < 0.3:
            f.set(12, ey + 3, "m")
    if not machine and high > 0.15 and extra not in ("beard",):
        f.rect(left - 1, ey + 3, 2, 1, "o", mirror=True)
    mw = 2 if t["mouth"] < 0.45 else 4 if t["mouth"] < 0.85 else 6
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
    if top == "spikes":
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
    elif top == "helmet":
        f.rect(xl + 3, yt - 2, 2 * half - 6, 1, "w")
        f.both(xl + 1, yt - 1, "w"), f.both(xl + 2, yt - 1, "w")
        f.rect(11, yt - 4, 2, 1, "o"), f.rect(10, yt - 3, 4, 1, "o")
        f.rect(xl, yt, 2 * half, 1, "w")
        f.line(xl + 1, yt + 1, xl + 1, yt + 3, "m", mirror=True)
        f.rect(11, yt + 1, 2, 2, "m")
    elif top == "antenna":
        f.line(11, yt - 1, 11, yt - 2, "w"), f.line(12, yt - 1, 12, yt - 2, "w")
        f.rect(11, yt - 4, 2, 2, "o")
    elif top == "pointed hat":                   # ELDER's, its brim the head's top
        for j, row in enumerate(WIZARD_HAT):
            for x, c in enumerate(row):
                if c != ".":
                    f.set(x, yt - len(WIZARD_HAT) + 1 + j, c)
    elif top == "long hair":
        f.rect(xl + 3, yt + 1, 2 * half - 6, 1, "m")
        for y in range(yt + 1, yc + 3):
            f.both(xl - 1 - (y > ey + 2), y, "m")
    elif top == "bald":
        f.both(xl + 1, ey - 2, "m"), f.both(xl + 1, ey - 3, "m"), f.both(xl + 2, ey - 3, "m")
    elif top is None and body not in ("ghost", "robot") and not machine:
        f.rect(xl + 3, yt + 1, 2 * half - 6, 1, "m")                       # a fringe
        f.both(xl + 1, yt + 2, "m")
    if top == "hood":
        f.rect(xl + 2, yt - 3, 2 * half - 4, 1, "b")
        f.both(xl + 1, yt - 2, "b"), f.both(xl, yt - 1, "b")
        f.line(xl - 1, yt, xl - 1, yc, "b", mirror=True)
        f.line(xl - 1, yc + 1, xl - 3, yc + 3, "b", mirror=True)
        f.line(xl - 3, yc + 3, xl - 3, N - 1, "b", mirror=True)

    # The body.
    sy = yc + 2
    if body == "ghost":
        f.line(xl, yc, xl, 20, "w", mirror=True)
        f.rect(xl - 2, yc - 2, 2, 1, "w", mirror=True), f.both(xl - 3, yc - 1, "w"), f.rect(xl - 2, yc, 2, 1, "w", mirror=True)
        for x in range(xl, 12):
            f.both(x, 21 + (x - xl) % 2, "w")
        f.rect(xl + 1, yc, 2 * half - 2, 1, ".")
        f.line(xl, yc - 1, xl, yc, "w", mirror=True)
        return f
    f.both(10, yc + 1, "w")                                                   # the neck
    wear = "w" if body == "stripes" else "b" if body in ("robe", "robot") else "g" if high > 0.3 else "b"
    if body == "armour":
        wear = "m"
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
    elif body == "robot":
        f.rect(6, sy + 1, 12, 1, "b")
        f.line(6, sy + 1, 6, N - 1, "b", mirror=True)
        f.rect(10, sy + 3, 4, 1, "o")
    elif body == "tee":
        f.rect(3, sy + 3, 9, 1, "o", mirror=True)
        f.line(5, sy + 4, 5, N - 1, "m", mirror=True)
        f.both(11, sy, "."), f.both(10, sy + 1, "w")
    else:
        f.both(11, sy, "."), f.both(10, sy + 1, "w"), f.both(11, sy + 2, "w")   # a collar
        f.line(5, sy + 3, 5, N - 1, "m", mirror=True)
    return f


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


def main():
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
