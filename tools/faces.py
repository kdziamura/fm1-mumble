#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""MUMBLE's faces from their voices: an experiment before the firmware does it.

A character is assembled from parts on the 24x24 grid the portraits use (one
letter a pixel, '.' empty, the palette of app/mumble_sprites.inc).

The head's shape follows the voice continuously:
  size (TUBE: a longer tract is a bigger person)   head, neck and shoulders
  PITCH     a high voice: a rounder head, bigger eyes, a smaller chin
  EFFORT    a wider jaw; CRUSH and RADIO a squarer head
  TUNE      eyes set wider; MOUTH the mouth's width

The parts come from what stands out in the voice against the rest of the
cast (how far each setting is from the cast's average, in its spread): each
slot takes the part of the setting that stands out most, past a threshold.
  top    ROOM a hood, RADIO an antenna, SPEED spikes, DICTION a cap,
         VIBRATO long hair, JITTER wild hair, WOBBLE bald, GLIDE a pointed hat
  face   CRUSH a visor, WOBBLE a beard (low voices), JITTER + EFFORT an eye
         patch, DICTION glasses, PITCH cheeks
  body   AIR a ghost's tail, CRUSH a robot's box, ROOM a cloak, EFFORT armour
  around ECHO dots
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


def head_shape(cx, y0, w, h, n, chin):
    """A superellipse w x h from row y0 (n: 2 round .. 6 square), its lower
    half narrowing to chin (a fraction of the width)."""
    shape, half = set(), w / 2
    for r in range(h):
        u = (r + 0.5) / h * 2 - 1
        hw = half * max(0.0, 1 - abs(u) ** n) ** (1 / n)
        if u > 0:
            hw *= 1 - (1 - chin) * u * u
        hw = max(1, round(hw))
        for x in range(cx - hw, cx + hw):
            shape.add((x, y0 + r))
    return shape


def build(values) -> Face:
    t, z = traits(values), standing(values)
    f = Face()
    size, high = 1 - t["tube"], t["pitch"]
    mech = max(t["crush"], t["radio"])          # how much of a machine

    # The parts, from what stands out.
    body = pick([("ghost", z["air"], t["air"] > 0.6), ("robot", z["crush"], t["crush"] > 0.4),
                 ("cloak", z["room"], t["room"] > 0.5), ("armour", z["effort"] - z["pitch"] * 0.5, t["effort"] > 0.6)])
    top = pick([("hood", z["room"], t["room"] > 0.55 and body != "ghost"), ("antenna", z["radio"], t["radio"] > 0.35),
                ("spikes", z["speed"], t["speed"] > 0.4), ("cap", z["diction"], t["diction"] > 0.5),
                ("long hair", z["vibrato"], t["vibrato"] > 0.35), ("wild hair", z["jitter"], t["jitter"] > 0.6),
                ("bald", z["wobble"], t["wobble"] > 0.4), ("pointed hat", z["glide"], t["glide"] > 0.35 and body != "ghost")])
    extra = pick([("visor", z["crush"], t["crush"] > 0.5),
                  ("beard", z["wobble"] - z["pitch"] * 0.7, t["wobble"] > 0.4 and body != "ghost"),
                  ("eye patch", (z["jitter"] + z["effort"]) * 0.6 - z["mouth"] * 0.4, t["jitter"] > 0.6),
                  ("glasses", z["diction"] * 0.8, t["diction"] > 0.55), ("cheeks", z["pitch"], t["pitch"] > 0.3)],
                 threshold=0.7)
    if body == "robot" and top in (None, "spikes", "long hair", "wild hair", "bald"):
        top = "antenna" if t["radio"] > 0.2 else None
    f.parts = [p for p in (top, extra, body) if p]

    # The head.
    w = round(12 + 4 * size + 2 * t["effort"] - 2.5 * high)
    w = max(10, min(18, w + (w % 2)))
    h = max(9, min(14, round(10 + 3 * size - 1.2 * high)))
    neck = 0 if body == "ghost" else 1 + round(1.5 * size)
    shoulders = 0 if body == "ghost" else max(3, round(6 - 3 * high + 1.5 * size))
    room_above = 6 if top == "pointed hat" else 4 if top in ("antenna", "spikes", "wild hair") else 3
    y0 = N - shoulders - neck - h
    if body == "ghost":
        y0 = N - h - 7
    if y0 < room_above:
        h -= room_above - y0
        y0 = room_above
    cx, half = N // 2, w // 2
    n = 2.0 if body == "ghost" else 2.2 + 4 * min(1.0, mech * 1.6) + 0.6 * t["effort"]
    chin = 0.45 + 0.35 * t["effort"] - 0.3 * high + 0.4 * min(1.0, mech * 1.6)
    head = head_shape(cx, y0, w, h, n, max(0.25, min(1.0, chin)))
    ghostly = False                              # (a broken outline read as noise: kept whole)

    # A hood behind the head.
    if top == "hood":
        hood = {(x + dx, y + dy) for (x, y) in head for dx in range(-2, 3) for dy in range(-2, 1)}
        hood |= {(x, y) for y in range(y0 + h // 2, N) for x in range(cx - half - 2, cx + half + 2)}
        outline(hood, f, "b", open_below=True)

    # The body.
    sy = y0 + h + neck
    if body == "ghost":
        tail = set(head)
        for y in range(y0 + h // 2, N):
            k = y - (y0 + h // 2)
            for x in range(cx - half, cx + half):
                if y < N - 2 or (x + (y - N)) % 4 < 2:
                    tail.add((x, y))
        outline(tail, f, "w", broken=ghostly)
    elif body == "robot":
        box = {(x, y) for y in range(sy, N) for x in range(cx - half - 1, cx + half + 1)}
        outline(box, f, "m", open_below=True)
        f.rect(cx - 2, sy + 2, 4, 2, "g")
    else:
        wear = {"cloak": "b", "armour": "m"}.get(body) or ("o" if t["effort"] > 0.6 else "b" if high < 0.3 else "g")
        reach = min(11, half + 2 + round(3 * size))
        slope = 2 if high > 0.5 else 3
        shape = {(x, y) for i, y in enumerate(range(sy, N))
                 for x in range(cx - min(reach, 2 + i * slope), cx + min(reach, 2 + i * slope))}
        outline(shape, f, wear, open_below=True)
        if body == "armour":
            f.rect(cx - reach + 1, sy + 1, 3, 1, "o", mirror=True)
        if body == "cloak":
            f.rect(cx - 1, sy + 1, 2, 1, "o")
        else:
            f.both(cx - 2, sy, "w"), f.both(cx - 1, sy + 1, "w")      # a collar
    for y in range(y0 + h, sy):                                         # the neck
        f.both(cx - 2, y, "w")

    # The head's line, and ears.
    outline(head, f, "w", broken=ghostly)
    eye_y = y0 + round(h * 0.45)
    if body not in ("ghost", "robot") and top not in ("hood", "long hair"):
        for y in (eye_y, eye_y + 1):
            f.both(cx - half - 1, y, "w")

    # Eyes: slits for a low voice, big rings for a high one; TUNE sets them apart.
    ew, eh = (3, 3) if high > 0.7 else (2, 2 if high > 0.12 else 1)
    gap = max(1, min(half - ew - 1, round(1 + half * 0.25 + 2.5 * t["tune"])))
    left = cx - gap - ew
    f.rect(left, eye_y, ew, eh, "w", mirror=True)
    if (ew, eh) == (3, 3):
        f.both(left + 1, eye_y + 1, ".")
    f.eyes = [(left, eye_y, ew, eh), (N - left - ew, eye_y, ew, eh)]

    # Brows: down at the middle with effort, raised with glide, bushy with age.
    old = z["wobble"] > 0.8 or z["jitter"] > 1.2
    brow = "m" if old else "w"
    by = eye_y - 2 - (t["glide"] > 0.5)
    if t["effort"] > 0.6 and body != "robot":
        f.line(left - 1, by, left + ew - 1, by + 1, brow, mirror=True)
    elif t["glide"] > 0.25 or old:
        f.line(left - (old), by, left + ew - 1 + (old), by, brow, mirror=True)

    # The mouth: MOUTH wide, the corners down with effort, up for a bright voice.
    mw = max(2, round(2 + 4 * t["mouth"]))
    mw += mw % 2
    my = y0 + h - 2 - (1 if h >= 12 else 0) - (1 if high > 0.7 else 0)
    f.rect(cx - mw // 2, my, mw // 2, 1, "w", mirror=True)
    if t["effort"] > 0.65:
        f.both(cx - mw // 2 - 1, my + 1, "w")
    elif high > 0.4 and t["effort"] < 0.55:
        f.both(cx - mw // 2 - 1, my - 1, "w")
    f.mouth, f.mouth_w = (cx, my), mw
    if t["tube"] < 0.45 and body != "robot" and extra != "visor":       # a nose, for a long tract
        f.rect(cx - 1, eye_y + eh, 1, 1 + (t["tube"] < 0.2), "m", mirror=True)

    # The face's extra.
    if extra == "visor":
        f.rect(cx - half + 1, eye_y - 1, 2 * half - 2, eh + 2, "g")
        f.rect(left, eye_y, ew, eh, "w", mirror=True)
    elif extra == "beard":
        for (x, y) in head:
            if y > my and f.get(x, y) == "." and (x + y) % 2 == 0:
                f.set(x, y, "m")
        f.rect(cx - mw // 2 - 1, my - 1, mw // 2 + 1, 1, "m", mirror=True)
    elif extra == "eye patch":
        rx = N - left - ew
        f.rect(rx - 1, eye_y - 1, ew + 2, eh + 2, "m")
        f.line(rx - 1, eye_y - 1, cx - half + 1, y0 + 1, "m")
        f.eyes = f.eyes[:1]
    elif extra == "glasses":
        for (x, y, ww, hh) in f.eyes:
            box = {(x + i, y + j) for i in range(-1, ww + 1) for j in range(-1, hh + 1)}
            outline(box, f, "o")
        f.rect(cx - gap, eye_y, gap, 1, "o", mirror=True)
    elif extra == "cheeks":
        f.rect(left - 1, eye_y + eh + 1, 2, 1, "o", mirror=True)

    # On top.
    if top == "antenna":
        f.line(cx, y0 - 1, cx, y0 - 3, "w"), f.line(cx - 1, y0 - 1, cx - 1, y0 - 3, "w")
        f.rect(cx - 1, y0 - 4, 2, 1, "o")
    elif top == "spikes":
        for k, x in enumerate(range(cx - half + 1, cx)):
            f.both(x, y0 - 1 - (k % 2), "w")
        f.rect(cx - half + 2, y0, 2 * half - 4, 1, "w")
    elif top == "cap":
        f.rect(cx - half + 1, y0, 2 * half - 2, 2, "b")
        f.rect(cx - half, y0 + 2, 2 * half + 3, 1, "b")
    elif top == "long hair":
        f.rect(cx - half + 1, y0 + 1, 2 * half - 2, 1, "m")
        for y in range(y0 + 1, min(N, y0 + h + 2)):
            f.both(cx - half - 1 - (y > y0 + h * 2 // 3), y, "m")
    elif top == "wild hair":
        for x in range(cx - half - 1, cx + half + 1):
            for y in range(y0 - 3, y0 + 2):
                if (x * 7 + y * 13) % 5 < 2 and ((x, y) not in head or y <= y0):
                    f.set(x, y, "m")
    elif top == "bald":
        f.rect(cx - half, eye_y - 3, 1, 3, "m", mirror=True)
    elif top == "pointed hat":
        apex = y0 - 6
        for y in range(apex, y0 + 1):
            reach = 1 + (y - apex) * (half - 1) // 6
            f.both(cx - reach, y, "b")
        f.rect(cx - half - 1, y0 + 1, 2 * half + 2, 1, "b")
        f.rect(cx - 1, apex + 2, 2, 1, "o")
    elif top is None and body not in ("ghost", "robot"):              # short hair, a fringe
        for (x, y) in head:
            if y0 < y <= y0 + 2 and f.get(x, y) == ".":
                f.set(x, y, "m")
        for x in range(cx - half + 2, cx + half - 2, 3):
            f.set(x, y0 + 3, "m")

    # Around: echo's dots.
    if t["echo"] > 0.4:
        drawn = {(x, y) for y in range(N) for x in range(N) if f.grid[y][x] != "."}
        for y in range(N):
            for x in range(N):
                if (x * 3 + y * 2) % 7 == 0 and (x, y) not in drawn and \
                        all((x + a, y + b) not in drawn for a in (-1, 0, 1) for b in (-1, 0, 1)) and \
                        any((x + a, y + b) in drawn for a in (-3, 0, 3) for b in (-3, 0, 3)):
                    f.set(x, y, "b")
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
