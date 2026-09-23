#!/usr/bin/env python3
"""Generate OutBun-napoli97 runtime assets.

Every sprite is painted procedurally at 4x resolution with Pillow, box-filtered
down to its runtime size, reduced to at most 15 colours plus transparency, and
stored as nibble run-length rows:

    byte = (local_colour << 4) | (run_length - 1)      local_colour 0 = clear

Each sprite owns a 15-entry look-up table that maps local colours to the
cartridge's shared hardware palette (indices 16..191).  Indices 192..231 are
the per-leg "theme" slots (sky, sea, land, road ...) re-programmed at runtime.

The runtime draws the runs as prg32_gfx_rect_indexed() spans, so the same
bytes serve 1:1 blits, scaled pseudo-3D drawing, mirroring, and car lean.

Outputs:
    src/assets.h                       C tables consumed by src/game.c
    assets/generated/*.png             preview sheets (not shipped in the cart)
"""

from __future__ import annotations

import json
import math
from pathlib import Path

import warnings

from PIL import Image, ImageDraw

warnings.filterwarnings("ignore", category=DeprecationWarning)

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / "assets" / "generated"
SS = 4  # supersampling factor

STATIC_BASE = 16
STATIC_COUNT = 176  # indices 16..191
THEME_BASE = 192


# ---------------------------------------------------------------------------
# Small painting helpers (all coordinates are in final sprite pixels and are
# scaled by SS internally so the art reads naturally).
# ---------------------------------------------------------------------------
class Canvas:
    def __init__(self, w: int, h: int):
        self.w, self.h = w, h
        self.im = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)

    @staticmethod
    def _b(box):
        return [round(v * SS) for v in box]

    def rect(self, box, fill, r=0):
        b = self._b(box)
        if r:
            self.d.rounded_rectangle(b, radius=round(r * SS), fill=fill)
        else:
            self.d.rectangle(b, fill=fill)

    def ell(self, box, fill):
        self.d.ellipse(self._b(box), fill=fill)

    def poly(self, pts, fill):
        self.d.polygon([(round(x * SS), round(y * SS)) for x, y in pts], fill=fill)

    def line(self, pts, fill, width=1.0):
        self.d.line([(round(x * SS), round(y * SS)) for x, y in pts], fill=fill,
                    width=max(1, round(width * SS)))

    def vgrad(self, box, top, bottom, steps=4):
        """Vertical gradient clipped to the currently painted alpha in box."""
        x0, y0, x1, y1 = self._b(box)
        grad = Image.new("RGBA", (x1 - x0, y1 - y0))
        gd = ImageDraw.Draw(grad)
        for y in range(y1 - y0):
            t = y / max(1, (y1 - y0 - 1))
            t = min(steps - 1, int(t * steps)) / max(1, steps - 1)  # flat shading bands
            c = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
            gd.line([(0, y), (x1 - x0, y)], fill=c)
        region = self.im.crop((x0, y0, x1, y1))
        alpha = region.split()[3]
        region.paste(grad, (0, 0), alpha)
        self.im.paste(region, (x0, y0))

    def hshade(self, box, left_gain, right_gain, steps=3):
        """Multiply painted pixels by a horizontal light ramp (left->right)."""
        x0, y0, x1, y1 = self._b(box)
        region = self.im.crop((x0, y0, x1, y1))
        px = region.load()
        for x in range(region.width):
            t = x / max(1, region.width - 1)
            t = min(steps - 1, int(t * steps)) / max(1, steps - 1)
            g = left_gain + (right_gain - left_gain) * t
            for y in range(region.height):
                r, gg, b, a = px[x, y]
                if a:
                    px[x, y] = (min(255, round(r * g)), min(255, round(gg * g)),
                                min(255, round(b * g)), a)
        self.im.paste(region, (x0, y0))

    def finish(self, outline=0.55, colors=15) -> Image.Image:
        small = self.im.resize((self.w, self.h), Image.Resampling.BOX)
        px = small.load()
        for y in range(self.h):
            for x in range(self.w):
                r, g, b, a = px[x, y]
                if a < 110:
                    px[x, y] = (0, 0, 0, 0)
                else:
                    # un-premultiply the soft edge, then make it opaque
                    k = 255 / a
                    px[x, y] = (min(255, round(r * k)) if a < 255 else r,
                                min(255, round(g * k)) if a < 255 else g,
                                min(255, round(b * k)) if a < 255 else b, 255)
        # Box filtering only decides coverage; the colour of every opaque pixel
        # is re-sampled from the supersampled source.
        big = self.im.load()
        for y in range(self.h):
            for x in range(self.w):
                if px[x, y][3]:
                    # dominant colour of the block: crisp pixel-art edges
                    votes = {}
                    for yy in range(y * SS, y * SS + SS):
                        for xx in range(x * SS, x * SS + SS):
                            r, g, b, a = big[xx, yy]
                            if a > 128:
                                votes[(r, g, b)] = votes.get((r, g, b), 0) + 1
                    if votes:
                        px[x, y] = max(votes.items(), key=lambda kv: kv[1])[0] + (255,)
        if outline:
            src = small.copy().load()
            for y in range(self.h):
                for x in range(self.w):
                    if not src[x, y][3]:
                        continue
                    edge = False
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        xx, yy = x + dx, y + dy
                        if xx < 0 or yy < 0 or xx >= self.w or yy >= self.h or not src[xx, yy][3]:
                            edge = True
                            break
                    if edge:
                        r, g, b, _ = src[x, y]
                        px[x, y] = (round(r * outline), round(g * outline), round(b * outline), 255)
        return reduce_colors(small, colors)


def reduce_colors(im: Image.Image, colors: int) -> Image.Image:
    """Median-cut the opaque pixels to <= colors, keep binary alpha."""
    alpha = im.split()[3]
    rgb = im.convert("RGB")
    opaque = [p for p, a in zip(rgb.getdata(), alpha.getdata()) if a]
    if not opaque:
        return im
    distinct = set(opaque)
    if len(distinct) <= colors:
        return im
    strip = Image.new("RGB", (len(opaque), 1))
    strip.putdata(opaque)
    q = strip.quantize(colors=colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[: colors * 3]
    table = [tuple(pal[i * 3:i * 3 + 3]) for i in range(colors)]
    out = Image.new("RGBA", im.size)
    op = out.load()
    ip = im.load()
    cache = {}
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = ip[x, y]
            if not a:
                op[x, y] = (0, 0, 0, 0)
                continue
            key = (r, g, b)
            if key not in cache:
                cache[key] = min(table, key=lambda c: (c[0] - r) ** 2 * 3 + (c[1] - g) ** 2 * 4 + (c[2] - b) ** 2 * 2)
            op[x, y] = cache[key] + (255,)
    return out


def mix(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def shade(c, k):
    return tuple(max(0, min(255, round(v * k))) for v in c)


# ---------------------------------------------------------------------------
# Cars, rear three-quarter-free "OutRun" view.  56x36 at runtime, drawn 1:1 at
# the player depth.  Common parts first.
# ---------------------------------------------------------------------------
CAR_W, CAR_H = 56, 36
TYRE = (26, 26, 30)
TYRE_HI = (70, 70, 76)
CHROME = (226, 230, 236)
CHROME_D = (130, 136, 146)
GLASS = (40, 58, 84)
GLASS_HI = (120, 150, 180)
SHADOW = (18, 20, 24)


def car_base(c: Canvas, track=4.0, tyre_w=11.0):
    # ground shadow and wide tyres
    c.ell((1, 30, 55, 36), SHADOW + (255,))
    for x0 in (track - 1, CAR_W - track - tyre_w + 1):
        c.rect((x0, 25, x0 + tyre_w, 35), TYRE + (255,), r=2.2)
        for k in range(3):
            c.line([(x0 + 1.5, 27.5 + k * 2.4), (x0 + tyre_w - 1.5, 27.5 + k * 2.4)], TYRE_HI + (255,), 0.5)


def plate(c: Canvas, cx, y, text_color=(20, 20, 30)):
    c.rect((cx - 6, y, cx + 6, y + 3.6), (246, 246, 240, 255))
    c.rect((cx - 6, y, cx + 6, y + 3.6), None)
    c.line([(cx - 6, y), (cx + 6, y), (cx + 6, y + 3.6), (cx - 6, y + 3.6), (cx - 6, y)], (40, 40, 50, 255), 0.35)
    # "NA" provincial code and digits suggested by strokes
    c.rect((cx - 4.6, y + 1.0, cx - 3.4, y + 2.8), text_color + (255,))
    c.rect((cx - 2.9, y + 1.0, cx - 1.7, y + 2.8), text_color + (255,))
    for k in range(4):
        c.rect((cx - 0.6 + k * 1.4, y + 1.2, cx + 0.3 + k * 1.4, y + 2.6), text_color + (255,))


def fiat500() -> Image.Image:
    """White Fiat 500 (1965-75), Abarth-style propped engine lid, dual pipes."""
    c = Canvas(CAR_W, CAR_H)
    body, body_d = (242, 242, 236), (176, 178, 184)
    car_base(c, track=5.0, tyre_w=11.5)
    # rounded lower body
    c.rect((4, 12, 52, 30), body + (255,), r=7)
    c.poly([(6, 22), (50, 22), (52, 29), (4, 29)], body + (255,))
    # roof / greenhouse
    c.rect((13, 1.5, 43, 16), body + (255,), r=7)
    c.vgrad((3, 1, 53, 31), (252, 252, 248), body_d)
    c.hshade((3, 1, 53, 31), 1.06, 0.84)
    # rolled canvas sunroof
    c.rect((19, 1.6, 37, 4.2), (150, 120, 88, 255), r=1.3)
    c.line([(20, 2.8), (36, 2.8)], (110, 86, 62, 255), 0.5)
    # rear window
    c.rect((16.5, 5, 39.5, 12.2), GLASS + (255,), r=3.5)
    c.line([(19, 6.3), (26, 6.3)], GLASS_HI + (255,), 0.7)
    c.line([(29, 11), (37, 7)], (60, 84, 116, 255), 0.6)
    # propped Abarth engine lid (raised), engine bay shadow beneath it
    c.rect((17, 17.4, 39, 25.5), (14, 14, 18, 255), r=1.5)
    c.poly([(16.5, 13.2), (39.5, 13.2), (41, 19.2), (15, 19.2)], (236, 236, 232, 255))
    c.hshade((15, 13, 41.5, 19.6), 1.05, 0.86)
    for k in range(4):
        y = 14.5 + k * 1.2
        c.line([(19.5, y), (36.5, y)], (122, 124, 130, 255), 0.45)
    # lid struts
    c.line([(18, 19.2), (18.8, 23.5)], CHROME_D + (255,), 0.5)
    c.line([(38, 19.2), (37.2, 23.5)], CHROME_D + (255,), 0.5)
    # red racing stripe on the lower body (souped-up)
    c.rect((5, 22.2, 51, 23.4), (206, 32, 40, 255))
    # tail lights: vertical ovals, red over amber
    for x0 in (6.2, 45.6):
        c.ell((x0, 13.5, x0 + 4.2, 21.5), (210, 26, 30, 255))
        c.ell((x0 + 0.5, 18.2, x0 + 3.7, 21.4), (244, 150, 26, 255))
        c.ell((x0 + 0.9, 14.3, x0 + 2.2, 16.0), (255, 160, 150, 255))
    # chrome bumper with overriders
    c.rect((4.5, 25.6, 51.5, 27.4), CHROME + (255,), r=0.9)
    c.line([(5, 27.3), (51, 27.3)], CHROME_D + (255,), 0.5)
    for x in (15.5, 40.5):
        c.rect((x - 0.9, 23.8, x + 0.9, 28.6), CHROME + (255,), r=0.6)
    plate(c, 28, 26.2)
    # dual Abarth exhaust
    for x in (20.5, 35.5):
        c.ell((x - 1.6, 29.0, x + 1.6, 31.6), CHROME_D + (255,))
        c.ell((x - 0.8, 29.6, x + 0.8, 31.0), (20, 20, 20, 255))
    return c.finish()


def fiat126() -> Image.Image:
    """Fiat 126 (1972-): boxy rear, black louvred engine lid, wide lamps."""
    c = Canvas(CAR_W, CAR_H)
    body, body_d = (214, 36, 40), (120, 14, 20)
    car_base(c, track=4.0, tyre_w=11.5)
    c.rect((3, 12, 53, 29), body + (255,), r=3)
    c.poly([(12, 13), (44, 13), (41, 1.5), (15, 1.5)], body + (255,))
    c.vgrad((2, 1, 54, 30), (238, 70, 70), body_d)
    c.hshade((2, 1, 54, 30), 1.04, 0.82)
    c.poly([(15.5, 4), (40.5, 4), (42.2, 11.8), (13.8, 11.8)], GLASS + (255,))
    c.line([(17, 5.2), (25, 5.2)], GLASS_HI + (255,), 0.7)
    # louvred engine lid
    c.rect((14, 13.8, 42, 22), (30, 30, 34, 255), r=1)
    for k in range(6):
        y = 14.8 + k * 1.2
        c.line([(15, y), (41, y)], (84, 84, 92, 255), 0.5)
    # wide rectangular tail lights
    for x0 in (4.5, 42.5):
        c.rect((x0, 14, x0 + 9, 20.5), (40, 40, 44, 255), r=0.8)
        c.rect((x0 + 0.6, 14.6, x0 + 8.4, 17.2), (224, 26, 34, 255))
        c.rect((x0 + 0.6, 17.4, x0 + 4.4, 19.9), (250, 150, 30, 255))
        c.rect((x0 + 4.6, 17.4, x0 + 8.4, 19.9), (238, 238, 238, 255))
    # black plastic bumper
    c.rect((3, 24.4, 53, 27.4), (32, 32, 36, 255), r=1)
    c.line([(4, 24.8), (52, 24.8)], (84, 84, 90, 255), 0.4)
    plate(c, 28, 22.6)
    c.ell((38, 28.6, 41.4, 31), CHROME_D + (255,))
    # white twin stripes (tuned "Bis")
    c.rect((25.3, 1.8, 26.6, 12.4), (246, 246, 246, 255))
    c.rect((29.4, 1.8, 30.7, 12.4), (246, 246, 246, 255))
    return c.finish()


def dyane() -> Image.Image:
    """Citroen Dyane: tall narrow tub, grey canvas roof, flared wings."""
    c = Canvas(CAR_W, CAR_H)
    body, body_d = (244, 212, 110), (168, 128, 44)
    car_base(c, track=6.5, tyre_w=9.0)
    # flared rear wings
    for x0 in (3.5, 38.5):
        c.ell((x0, 15, x0 + 14, 31), body + (255,))
    c.poly([(9, 30), (47, 30), (45, 11), (11, 11)], body + (255,))
    c.vgrad((3, 10, 53, 31), (252, 230, 150), body_d)
    c.hshade((3, 10, 53, 31), 1.05, 0.82)
    # canvas roof
    c.rect((12, 1.2, 44, 13), (136, 138, 132, 255), r=5)
    c.vgrad((11, 1, 45, 13), (164, 166, 160), (98, 100, 96))
    for k in range(3):
        c.line([(14, 4 + k * 3), (42, 4 + k * 3)], (110, 112, 106, 255), 0.4)
    c.rect((18, 3.8, 38, 9.6), GLASS + (255,), r=2)
    c.line([(20, 4.8), (26, 4.8)], GLASS_HI + (255,), 0.6)
    # tailgate panel and hinge line
    c.line([(12.5, 12.2), (43.5, 12.2)], shade(body_d, 0.9) + (255,), 0.6)
    c.rect((17, 14, 39, 23.5), (236, 200, 96, 255), r=1.4)
    c.line([(17.5, 23.2), (38.5, 23.2)], shade(body_d, 0.8) + (255,), 0.5)
    # round lamps on wings
    for cx in (9.6, 46.4):
        c.ell((cx - 2.6, 17.4, cx + 2.6, 22.6), (220, 30, 30, 255))
        c.ell((cx - 1.2, 18.2, cx + 0.2, 19.6), (255, 170, 160, 255))
        c.ell((cx - 1.6, 23.2, cx + 1.6, 25.6), (246, 150, 30, 255))
    c.rect((6, 25.6, 50, 27.2), CHROME + (255,), r=0.8)
    plate(c, 28, 20.2)
    c.ell((14, 28.6, 17.2, 30.8), CHROME_D + (255,))
    return c.finish()


def beetle() -> Image.Image:
    """VW Beetle (Maggiolino): domed roof, oval window, big wing lamps."""
    c = Canvas(CAR_W, CAR_H)
    body, body_d = (70, 150, 214), (22, 70, 124)
    car_base(c, track=4.5, tyre_w=11.0)
    # bulbous rear wings
    for x0 in (2.5, 34.5):
        c.ell((x0, 13, x0 + 19, 32), body + (255,))
    c.ell((10, 0.8, 46, 30), body + (255,))
    c.rect((9, 18, 47, 28), body + (255,))
    c.vgrad((2, 0.5, 54, 32), (138, 196, 240), body_d)
    c.hshade((2, 0.5, 54, 32), 1.06, 0.8)
    # oval rear window
    c.ell((20, 4.2, 36, 10.6), GLASS + (255,))
    c.line([(22.5, 5.6), (27, 5.2)], GLASS_HI + (255,), 0.6)
    # engine lid with vents
    c.poly([(21, 12.5), (35, 12.5), (37, 24), (19, 24)], shade(body, 0.92) + (255,))
    for k in range(4):
        c.line([(22.5 + k * 0.1, 13.8 + k * 1.1), (33.5 - k * 0.1, 13.8 + k * 1.1)], shade(body_d, 0.7) + (255,), 0.5)
    c.ell((26.6, 20.6, 29.4, 22.2), CHROME + (255,))
    # tall "elephant foot" wing lamps
    for cx in (9.8, 46.2):
        c.ell((cx - 2.4, 14.5, cx + 2.4, 23.5), (218, 26, 30, 255))
        c.ell((cx - 1.8, 20.3, cx + 1.8, 23.3), (246, 150, 28, 255))
        c.ell((cx - 1.1, 15.4, cx + 0.2, 17.3), (255, 170, 160, 255))
    c.rect((3, 25.2, 53, 27), CHROME + (255,), r=0.9)
    for x in (13, 43):
        c.rect((x - 0.8, 23.4, x + 0.8, 28.2), CHROME + (255,), r=0.5)
    plate(c, 28, 24.0)
    for x in (22.5, 33.5):
        c.ell((x - 1.3, 28.8, x + 1.3, 31.2), CHROME_D + (255,))
    return c.finish()


# ---------------------------------------------------------------------------
# Traffic of the 1997 coast
# ---------------------------------------------------------------------------
def ape() -> Image.Image:
    """Piaggio Ape three-wheeler with a load of Sorrento lemons."""
    c = Canvas(36, 34)
    c.ell((2, 29, 34, 34), SHADOW + (255,))
    for x0 in (4, 25):
        c.rect((x0, 24, x0 + 7, 33), TYRE + (255,), r=1.5)
    cab = (96, 170, 150)
    c.rect((8, 1, 28, 14), cab + (255,), r=4)
    c.vgrad((8, 1, 28, 14), (150, 210, 190), (56, 120, 104))
    c.rect((11, 3, 25, 9), GLASS + (255,), r=2)
    # wooden cargo box
    c.rect((2, 11, 34, 27), (150, 104, 60, 255), r=1)
    for k in range(4):
        c.line([(2.5, 13 + k * 3.8), (33.5, 13 + k * 3.8)], (104, 70, 38, 255), 0.6)
    c.hshade((2, 11, 34, 27), 1.1, 0.8)
    # lemons heaped above the box
    for i, (x, y) in enumerate([(5, 10), (9, 8.6), (13, 9.6), (17, 8.2), (21, 9.4), (25, 8.4), (29, 9.8), (7.5, 11.5), (27, 11.4), (15, 11.2), (22, 11.5)]):
        col = (250, 222, 40) if i % 3 else (232, 196, 20)
        c.ell((x - 2.2, y - 1.8, x + 2.2, y + 1.8), col + (255,))
        c.ell((x - 1.0, y - 1.2, x + 0.2, y - 0.2), (255, 250, 170, 255))
    c.rect((4, 21, 7, 24), (220, 30, 30, 255))
    c.rect((29, 21, 32, 24), (220, 30, 30, 255))
    c.rect((14, 21.5, 22, 24.5), (244, 244, 240, 255))
    return c.finish()


def vespa() -> Image.Image:
    """Vespa rider seen from behind (white shirt, no helmet: it is 1997)."""
    c = Canvas(16, 28)
    c.ell((2, 25, 14, 28), SHADOW + (255,))
    c.rect((6, 20, 10, 27.5), TYRE + (255,), r=1.4)
    # side-panelled mint body
    c.ell((2, 13, 14, 24), (150, 214, 196, 255))
    c.vgrad((2, 13, 14, 24), (200, 240, 226), (80, 150, 134))
    c.rect((6.8, 18, 9.2, 20), (220, 30, 30, 255))
    # rider
    c.rect((4, 5.5, 12, 15), (240, 240, 236, 255), r=2.5)
    c.hshade((4, 5.5, 12, 15), 1.05, 0.8)
    c.ell((5.2, 0.5, 10.8, 6.5), (60, 40, 30, 255))
    c.rect((3, 12, 5, 16), (90, 110, 170, 255), r=1)
    c.rect((11, 12, 13, 16), (90, 110, 170, 255), r=1)
    return c.finish(colors=12)


def sita_bus() -> Image.Image:
    """Blue SITA coach, the Amalfi coast's rolling roadblock."""
    c = Canvas(64, 52)
    c.ell((1, 46, 63, 52), SHADOW + (255,))
    for x0 in (4, 49):
        c.rect((x0, 40, x0 + 11, 51), TYRE + (255,), r=2)
    blue = (36, 92, 176)
    c.rect((2, 1, 62, 46), blue + (255,), r=5)
    c.vgrad((2, 1, 62, 46), (84, 140, 216), (16, 50, 110))
    c.hshade((2, 1, 62, 46), 1.08, 0.82)
    c.rect((6, 4, 58, 20), GLASS + (255,), r=3)
    c.line([(9, 6), (26, 6)], GLASS_HI + (255,), 0.8)
    c.rect((2, 22, 62, 25), (236, 236, 240, 255))
    # "SITA" lettering suggested by white blocks
    for k, w in enumerate((3, 1.4, 3, 3)):
        x = 22 + k * 5.2
        c.rect((x, 27.5, x + w, 31.5), (246, 246, 246, 255))
    c.rect((12, 33, 52, 41), (22, 32, 60, 255), r=1)
    for k in range(4):
        c.line([(13, 34.5 + k * 1.8), (51, 34.5 + k * 1.8)], (60, 76, 120, 255), 0.5)
    for x0 in (4, 54):
        c.rect((x0, 30, x0 + 6, 38), (40, 40, 44, 255), r=1)
        c.rect((x0 + 0.8, 30.8, x0 + 5.2, 34), (226, 28, 34, 255))
        c.rect((x0 + 0.8, 34.2, x0 + 5.2, 37.2), (250, 150, 30, 255))
    c.rect((2, 42, 62, 45), (40, 40, 44, 255), r=1)
    plate(c, 32, 41.4)
    return c.finish()


# ---------------------------------------------------------------------------
# Roadside scenery of the Sorrento and Amalfi coasts
# ---------------------------------------------------------------------------
def umbrella_pine() -> Image.Image:
    c = Canvas(64, 56)
    trunk = (120, 72, 50)
    c.poly([(30, 56), (34, 56), (35, 30), (40, 18), (37.5, 18), (33, 28), (29, 18), (26.5, 18), (31, 30)], trunk + (255,))
    c.hshade((26, 18, 41, 56), 1.25, 0.7)
    lobes = [(2, 6, 30, 22), (14, 1, 44, 18), (32, 4, 62, 20), (8, 12, 36, 26), (28, 11, 58, 26), (20, 8, 46, 24)]
    for b in lobes:
        c.ell(b, (46, 92, 44, 255))
    c.vgrad((1, 0, 63, 27), (104, 150, 70), (26, 58, 30))
    for b in [(6, 7, 22, 13), (18, 2.5, 34, 9), (36, 5, 52, 11), (26, 9, 40, 14)]:
        c.ell(b, (118, 162, 82, 255))
    for b in [(10, 18, 30, 25), (32, 18, 52, 25)]:
        c.ell(b, (30, 64, 34, 255))
    return c.finish()


def palm() -> Image.Image:
    c = Canvas(40, 64)
    for k in range(28):
        y = 64 - k * 1.7
        x = 19 + math.sin(k * 0.12) * 2.2
        col = (128, 94, 60) if k % 2 else (98, 70, 44)
        c.ell((x - 2.6, y - 2.2, x + 2.6, y + 0.4), col + (255,))
    cx, cy = 21.5, 17
    for ang in range(0, 360, 36):
        a = math.radians(ang)
        droop = 10 if math.sin(a) < 0.3 else 4
        pts = []
        for s in range(9):
            t = s / 8
            pts.append((cx + math.cos(a) * 19 * t, cy - math.sin(a) * 13 * t + droop * t * t))
        c.line(pts, (48, 118, 48, 255), 3.0)
        c.line(pts, (104, 168, 74, 255), 1.1)
    c.ell((cx - 3.5, cy - 2.5, cx + 3.5, cy + 3.5), (126, 100, 50, 255))
    return c.finish()


def lemon_tree() -> Image.Image:
    c = Canvas(48, 40)
    for x in (12, 24, 36):
        c.rect((x - 1, 22, x + 1, 40), (104, 74, 48, 255))
    for b in [(0, 4, 22, 28), (12, 0, 36, 26), (26, 4, 48, 28), (6, 12, 42, 30)]:
        c.ell(b, (40, 104, 44, 255))
    c.vgrad((0, 0, 48, 31), (92, 150, 70), (24, 70, 34))
    import random
    rnd = random.Random(7)
    for _ in range(26):
        x, y = rnd.uniform(4, 44), rnd.uniform(4, 26)
        c.ell((x - 1.5, y - 1.2, x + 1.5, y + 1.4), (252, 226, 44, 255))
        c.ell((x - 0.8, y - 0.9, x + 0.1, y - 0.1), (255, 252, 180, 255))
    return c.finish()


def cypress() -> Image.Image:
    c = Canvas(16, 56)
    c.rect((7, 50, 9, 56), (90, 64, 42, 255))
    c.ell((1.5, 2, 14.5, 54), (28, 66, 38, 255))
    c.poly([(8, 0), (12, 16), (4, 16)], (28, 66, 38, 255))
    c.hshade((1, 0, 15, 54), 1.6, 0.7)
    return c.finish(colors=10)


def house() -> Image.Image:
    """Stacked cubic coastal house with arches, shutters and a terrace."""
    c = Canvas(48, 44)
    wall = (240, 200, 170)
    c.rect((2, 12, 46, 44), wall + (255,))
    c.rect((10, 2, 38, 13), wall + (255,))
    c.hshade((2, 2, 46, 44), 1.1, 0.8)
    # vaulted "cupola" roof on the upper cube
    c.ell((12, -3, 36, 6), shade(wall, 1.08) + (255,))
    # terrace railing
    c.rect((2, 11.2, 46, 12.8), (250, 250, 246, 255))
    for x in range(4, 46, 3):
        c.rect((x, 9, x + 0.8, 12), (250, 250, 246, 255))
    # arched windows with green shutters
    for x in (7, 20, 33):
        c.rect((x, 18, x + 8, 28), (40, 46, 60, 255), r=3.8)
        c.rect((x - 2.2, 18.5, x, 28), (60, 130, 90, 255))
        c.rect((x + 8, 18.5, x + 10.2, 28), (60, 130, 90, 255))
    c.rect((17, 33, 27, 44), (96, 60, 40, 255), r=4.6)
    c.rect((15, 4.5, 21, 10.5), (40, 46, 60, 255), r=2.8)
    c.rect((27, 4.5, 33, 10.5), (40, 46, 60, 255), r=2.8)
    # bougainvillea spill
    for x, y in [(36, 30), (39, 33), (42, 31), (44, 35), (40, 37), (37, 34)]:
        c.ell((x - 2.4, y - 2.2, x + 2.4, y + 2.2), (206, 40, 128, 255))
    return c.finish()


def dome_church() -> Image.Image:
    """Church with a yellow-green majolica dome and a white bell tower."""
    c = Canvas(48, 64)
    white = (244, 240, 228)
    c.rect((4, 34, 44, 64), white + (255,))
    c.hshade((4, 34, 44, 64), 1.05, 0.78)
    c.rect((8, 24, 36, 36), white + (255,))
    # majolica dome with tile ribs
    c.ell((8, 8, 36, 40), (60, 140, 90, 255))
    c.rect((6, 24, 38, 36), white + (255,))
    for k in range(-3, 4):
        x = 22 + k * 3.8
        c.line([(22, 8.5), (x, 24)], (244, 206, 40, 255), 1.0)
    c.hshade((8, 8, 36, 25), 1.2, 0.72)
    c.rect((21, 3, 23, 9), (230, 200, 60, 255))
    c.rect((19, 4.6, 25, 5.6), (230, 200, 60, 255))
    # campanile
    c.rect((36, 12, 46, 64), (250, 246, 236, 255))
    c.rect((38, 16, 44, 23), (40, 46, 60, 255), r=3)
    c.ell((35.5, 7.5, 46.5, 15), (60, 140, 90, 255))
    c.hshade((35, 7, 47, 64), 1.0, 0.72)
    c.rect((20, 46, 28, 64), (96, 60, 40, 255), r=4)
    c.ell((20.5, 38, 27.5, 44), (40, 46, 60, 255))
    return c.finish()


def saracen_tower() -> Image.Image:
    c = Canvas(32, 48)
    stone = (200, 184, 150)
    c.poly([(4, 48), (28, 48), (26, 10), (6, 10)], stone + (255,))
    c.rect((3, 4, 29, 11), stone + (255,))
    for x in range(3, 29, 5):
        c.rect((x, 1, x + 3, 5), stone + (255,))
    c.hshade((2, 0, 30, 48), 1.15, 0.66)
    import random
    rnd = random.Random(3)
    for _ in range(26):
        x, y = rnd.uniform(6, 25), rnd.uniform(12, 46)
        c.rect((x, y, x + 2.4, y + 1.2), (160, 144, 116, 255))
    c.rect((13, 20, 18, 27), (40, 36, 34, 255), r=2.4)
    c.rect((12.5, 38, 18.5, 48), (60, 44, 36, 255), r=3)
    return c.finish()


def cliff_rock() -> Image.Image:
    c = Canvas(64, 48)
    c.poly([(0, 48), (0, 16), (8, 6), (18, 10), (26, 0), (38, 4), (46, 12), (56, 8), (64, 18), (64, 48)], (176, 160, 130, 255))
    c.vgrad((0, 0, 64, 48), (214, 200, 168), (110, 96, 78))
    import random
    rnd = random.Random(11)
    for _ in range(8):
        x, y = rnd.uniform(2, 60), rnd.uniform(12, 44)
        c.line([(x, y), (x + rnd.uniform(-6, 6), y + rnd.uniform(3, 8))], (120, 104, 84, 255), 0.7)
    for _ in range(5):
        x, y = rnd.uniform(2, 60), rnd.uniform(8, 44)
        c.ell((x - 3, y - 2, x + 3, y + 2), (70, 120, 56, 255))
        c.ell((x - 1.5, y - 1.6, x + 1, y), (120, 160, 80, 255))
    return c.finish()


def stone_wall() -> Image.Image:
    """Seaside parapet in tufo with a spill of bougainvillea."""
    c = Canvas(64, 24)
    c.rect((0, 10, 64, 24), (212, 196, 170, 255))
    c.rect((0, 8, 64, 11), (236, 228, 212, 255))
    c.vgrad((0, 10, 64, 24), (206, 190, 162), (140, 124, 100))
    for y in (15, 20):
        c.line([(0, y), (64, y)], (150, 134, 110, 255), 0.5)
    for x in range(4, 64, 8):
        c.line([(x, 11), (x, 15)], (150, 134, 110, 255), 0.5)
        c.line([(x + 4, 15), (x + 4, 20)], (150, 134, 110, 255), 0.5)
    import random
    rnd = random.Random(5)
    for _ in range(30):
        x, y = rnd.uniform(28, 62), rnd.uniform(2, 17)
        col = (214, 40, 130) if rnd.random() < 0.7 else (54, 120, 50)
        c.ell((x - 2.2, y - 1.8, x + 2.2, y + 1.8), col + (255,))
    return c.finish()


def lamp_post() -> Image.Image:
    c = Canvas(12, 56)
    c.rect((5, 8, 7, 56), (54, 60, 64, 255))
    c.rect((4, 52, 8, 56), (54, 60, 64, 255))
    c.line([(6, 8), (6, 4), (10, 3)], (54, 60, 64, 255), 1.2)
    c.ell((1, 1, 8, 7), (54, 60, 64, 255))
    c.ell((2.2, 2.5, 6.8, 6), (255, 236, 170, 255))
    return c.finish(colors=6)


def chevron() -> Image.Image:
    """Italian red/white curve delineator (arrow pointing right)."""
    c = Canvas(20, 22)
    c.rect((9, 14, 11, 22), (70, 70, 76, 255))
    c.rect((0, 0, 20, 15), (206, 30, 38, 255))
    c.poly([(4, 2), (10, 7.5), (4, 13), (7, 13), (13, 7.5), (7, 2)], (250, 250, 250, 255))
    c.poly([(10, 2), (16, 7.5), (10, 13), (13, 13), (19, 7.5), (13, 2)], (250, 250, 250, 255))
    return c.finish(colors=6, outline=0.7)


def gozzo() -> Image.Image:
    """Sorrento 'gozzo' fishing boat on the water."""
    c = Canvas(32, 14)
    c.ell((0, 10, 32, 14), (230, 240, 250, 255))
    c.poly([(1, 5), (31, 5), (27, 12), (5, 12)], (246, 246, 244, 255))
    c.rect((2, 7, 30, 8.4), (40, 90, 170, 255))
    c.rect((3, 9, 29, 10), (206, 40, 40, 255))
    c.rect((13, 1, 20, 5.2), (230, 200, 120, 255))
    c.rect((8, 3, 9, 5), (120, 80, 50, 255))
    return c.finish(colors=8)


def agave() -> Image.Image:
    """Prickly pear (fico d'India) clump."""
    c = Canvas(32, 28)
    pads = [(12, 16, 22, 28), (4, 12, 14, 24), (18, 10, 28, 22), (9, 3, 17, 14), (19, 1, 26, 11), (1, 4, 8, 13)]
    for b in pads:
        c.ell(b, (84, 140, 70, 255))
    c.vgrad((0, 0, 32, 28), (128, 180, 96), (44, 92, 44))
    for x, y in [(11, 3.5), (15, 3), (21, 1.5), (24, 2), (3, 4.5), (22, 10.5)]:
        c.ell((x - 1.5, y - 1.5, x + 1.5, y + 1.5), (230, 90, 60, 255))
    return c.finish(colors=10)


# ---------------------------------------------------------------------------
# Ingredients (16x16)
# ---------------------------------------------------------------------------
def ingredient(kind: int) -> Image.Image:
    c = Canvas(16, 16)
    if kind == 0:  # pane rosetta: star-cut rose bun
        c.ell((1, 3, 15, 15), (206, 142, 60, 255))
        c.vgrad((1, 3, 15, 15), (240, 196, 112), (150, 90, 36))
        cx, cy = 8, 8.5
        for k in range(5):
            a = math.radians(90 + k * 72)
            c.line([(cx, cy), (cx + math.cos(a) * 6, cy - math.sin(a) * 5.4)], (130, 76, 30, 255), 0.7)
        c.ell((6.6, 7.2, 9.4, 9.8), (246, 214, 140, 255))
    elif kind == 1:  # pomodorino del piennolo: hanging cluster
        c.line([(8, 0), (8, 5)], (70, 120, 50, 255), 1.0)
        for x, y in [(5, 6), (10, 6), (7.5, 9.5), (4, 11), (11, 10.5), (7.5, 13)]:
            c.ell((x - 2.6, y - 2.6, x + 2.6, y + 2.6), (218, 38, 30, 255))
            c.ell((x - 1.4, y - 1.8, x - 0.2, y - 0.6), (255, 150, 130, 255))
    elif kind == 2:  # provolone del monaco: pear shape tied with string
        c.ell((3, 5, 13, 16), (230, 190, 90, 255))
        c.poly([(6, 6), (10, 6), (9, 2), (7, 2)], (230, 190, 90, 255))
        c.hshade((3, 2, 13, 16), 1.12, 0.72)
        c.line([(8, 0), (8, 2.4)], (160, 120, 70, 255), 0.8)
        c.line([(4, 9), (12, 11)], (150, 110, 60, 255), 0.6)
    elif kind == 3:  # olio extravergine: bottle
        c.rect((5, 5, 11, 16), (80, 120, 30, 255), r=2)
        c.rect((6.5, 1, 9.5, 6), (80, 120, 30, 255))
        c.rect((6.2, 0, 9.8, 2), (200, 170, 60, 255))
        c.hshade((5, 0, 11, 16), 1.4, 0.7)
        c.rect((5.6, 9, 10.4, 13), (246, 240, 210, 255))
    elif kind == 4:  # limone di sorrento with leaf
        c.ell((2, 4, 15, 14), (252, 222, 36, 255))
        c.hshade((2, 4, 15, 14), 1.1, 0.8)
        c.ell((4, 5.5, 7, 7.5), (255, 250, 180, 255))
        c.poly([(8, 4.5), (13, 0.5), (11, 4.5)], (60, 140, 50, 255))
    elif kind == 5:  # zucchine alla Nerano: fried rounds
        for x, y in [(5, 11), (11, 11), (8, 6)]:
            c.ell((x - 4, y - 4, x + 4, y + 4), (130, 160, 60, 255))
            c.ell((x - 2.8, y - 2.8, x + 2.8, y + 2.8), (236, 214, 120, 255))
            c.ell((x - 1, y - 1, x + 0.8, y + 0.8), (200, 170, 80, 255))
    elif kind == 6:  # fior di latte di Agerola
        c.ell((1.5, 3, 14.5, 15), (250, 250, 246, 255))
        c.hshade((1.5, 3, 14.5, 15), 1.0, 0.78)
        c.ell((4, 5, 7.5, 8), (255, 255, 255, 255))
        c.poly([(7, 3.5), (9, 3.5), (8.6, 1), (7.4, 1)], (240, 240, 236, 255))
    elif kind == 7:  # alici di Cetara: three anchovies
        for k in range(3):
            y = 4 + k * 4
            c.ell((1, y, 13, y + 3.2), (120, 150, 170, 255))
            c.poly([(12, y + 1.6), (15.5, y - 0.2), (15.5, y + 3.4)], (120, 150, 170, 255))
            c.line([(2, y + 1.0), (11, y + 1.0)], (200, 220, 230, 255), 0.5)
            c.ell((2.2, y + 0.9, 3.2, y + 1.9), (20, 20, 30, 255))
    else:  # tonno di Cetara: jar
        c.rect((2.5, 4, 13.5, 16), (230, 236, 240, 255), r=2)
        c.rect((3.5, 6, 12.5, 15), (220, 150, 110, 255), r=1.5)
        c.rect((2, 1.5, 14, 4.5), (200, 40, 40, 255), r=1)
        c.hshade((2, 1, 14, 16), 1.15, 0.78)
        c.rect((4, 9, 12, 12), (246, 246, 240, 255))
    return c.finish(colors=10, outline=0.5)




# ---------------------------------------------------------------------------
# Route landmarks and coastal details
# ---------------------------------------------------------------------------
def shipyard_crane() -> Image.Image:
    """Castellammare naval shipyard gantry crane."""
    c = Canvas(40, 64)
    red, white = (206, 52, 44), (236, 236, 232)
    for x0 in (6, 28):
        c.rect((x0, 22, x0 + 4, 64), red + (255,))
        for k in range(5):
            y = 26 + k * 7
            c.line([(x0, y), (x0 + 4, y + 5)], white + (255,), 0.8)
    c.rect((0, 16, 40, 22), red + (255,))
    for k in range(10):
        c.line([(k * 4, 16), (k * 4 + 4, 22)], white + (255,), 0.7)
    c.rect((14, 6, 26, 16), (240, 200, 60, 255))
    c.rect((16, 8, 21, 12), GLASS + (255,))
    c.line([(20, 22), (20, 44)], (40, 40, 44, 255), 0.6)
    c.rect((18, 44, 22, 47), (40, 40, 44, 255))
    c.hshade((0, 0, 40, 64), 1.1, 0.8)
    return c.finish(colors=10)


def amalfi_duomo() -> Image.Image:
    """Duomo di Sant'Andrea: striped Arab-Norman facade above its great stair."""
    c = Canvas(56, 60)
    c.poly([(8, 60), (48, 60), (38, 42), (18, 42)], (220, 214, 200, 255))            # the stair
    for k in range(6):
        y = 44 + k * 3
        c.line([(18 - k * 1.6, y), (38 + k * 1.6, y)], (170, 160, 144, 255), 0.5)
    c.rect((10, 14, 46, 43), (238, 232, 216, 255))
    c.poly([(10, 14), (28, 3), (46, 14)], (238, 232, 216, 255))                       # pediment
    for k in range(4):                                                                 # stripes
        c.rect((10, 16 + k * 6, 46, 18 + k * 6), (60, 90, 70, 255))
    c.ell((22, 5, 34, 12), (200, 150, 60, 255))                                        # gold mosaic
    for x in (13, 21, 29, 37):                                                         # arcade
        c.rect((x, 32, x + 6, 43), (40, 40, 52, 255), r=3)
    c.rect((46, 0, 54, 43), (230, 220, 200, 255))                                      # bell tower
    c.ell((45, -3, 55, 6), (60, 140, 90, 255))
    c.rect((48, 10, 52, 16), (40, 40, 52, 255), r=2)
    c.hshade((8, 0, 56, 60), 1.08, 0.78)
    return c.finish()


def ombrelloni() -> Image.Image:
    """Beach umbrellas and sunbeds (Maiori, Positano's Spiaggia Grande)."""
    c = Canvas(48, 24)
    c.rect((0, 18, 48, 24), (230, 210, 170, 255))
    cols = [(236, 70, 60), (250, 200, 60), (60, 140, 220), (236, 70, 60)]
    for k, x in enumerate((6, 18, 30, 42)):
        c.line([(x, 10), (x, 21)], (90, 80, 70, 255), 0.8)
        c.poly([(x - 7, 11), (x, 5), (x + 7, 11)], cols[k] + (255,))
        c.poly([(x - 2, 11), (x, 5), (x + 2, 11)], (250, 250, 246, 255))
        c.rect((x - 5, 20, x + 3, 22), (250, 250, 246, 255))
    return c.finish(colors=10)


def edicola() -> Image.Image:
    """Roadside shrine (edicola votiva) with flowers."""
    c = Canvas(20, 32)
    c.rect((2, 8, 18, 32), (236, 226, 206, 255))
    c.poly([(0, 9), (10, 1), (20, 9)], (180, 90, 70, 255))
    c.rect((5, 11, 15, 22), (60, 110, 190, 255), r=5)
    c.ell((7.5, 12.5, 12.5, 17), (250, 220, 190, 255))
    c.rect((7, 16, 13, 22), (250, 250, 250, 255))
    for x in (4, 8, 12, 16):
        c.ell((x - 2, 23, x + 2, 27), (230, 60, 110, 255))
    c.hshade((0, 0, 20, 32), 1.05, 0.82)
    return c.finish(colors=10)


def oleander() -> Image.Image:
    """Oleander bush, pink and white flowers on the SS145 verges."""
    c = Canvas(36, 26)
    for b in [(0, 8, 16, 26), (8, 2, 28, 24), (20, 6, 36, 26)]:
        c.ell(b, (54, 110, 60, 255))
    c.vgrad((0, 0, 36, 26), (90, 150, 80), (30, 70, 40))
    import random
    rnd = random.Random(21)
    for _ in range(22):
        x, y = rnd.uniform(3, 33), rnd.uniform(4, 20)
        c.ell((x - 1.6, y - 1.4, x + 1.6, y + 1.4), ((244, 120, 170) if rnd.random() < 0.75 else (252, 240, 245)) + (255,))
    return c.finish(colors=8)


def limoncello_stall() -> Image.Image:
    """Roadside stall with lemons and limoncello bottles."""
    c = Canvas(36, 30)
    c.rect((3, 16, 33, 30), (150, 104, 60, 255))
    for k in range(3):
        c.line([(3, 20 + k * 3.5), (33, 20 + k * 3.5)], (104, 70, 38, 255), 0.5)
    c.poly([(0, 8), (36, 8), (32, 1), (4, 1)], (250, 220, 60, 255))
    for x in range(4, 36, 6):
        c.poly([(x - 3, 8), (x, 8), (x - 1.5, 11)], (250, 220, 60, 255))
    for x in (4, 32):
        c.rect((x - 0.7, 8, x + 0.7, 16), (104, 70, 38, 255))
    for i, x in enumerate(range(6, 31, 4)):
        c.ell((x - 2, 12.5, x + 2, 16), (252, 226, 40, 255))
    for x in (9, 15, 21, 27):
        c.rect((x - 1, 9, x + 1, 13), (230, 240, 120, 255))
    c.hshade((0, 0, 36, 30), 1.08, 0.82)
    return c.finish(colors=10)


# ---------------------------------------------------------------------------
# Sprite table.  world_mult (Q4) sets the on-road world size relative to the
# car scale: 16 means one source pixel == one car pixel.
# ---------------------------------------------------------------------------
SPRITES = [
    # name,              painter,            world_mult_q4
    ("CAR_500",       fiat500,        16),
    ("CAR_126",       fiat126,        16),
    ("CAR_DYANE",     dyane,          16),
    ("CAR_BEETLE",    beetle,         16),
    ("APE",           ape,            18),
    ("VESPA",         vespa,          18),
    ("BUS",           sita_bus,       28),
    ("PINE",          umbrella_pine,  64),
    ("PALM",          palm,           52),
    ("LEMON",         lemon_tree,     40),
    ("CYPRESS",       cypress,        52),
    ("HOUSE",         house,          64),
    ("DOME",          dome_church,    64),
    ("TOWER",         saracen_tower,  60),
    ("ROCK",          cliff_rock,     80),
    ("WALL",          stone_wall,     36),
    ("LAMP",          lamp_post,      40),
    ("CHEVRON",       chevron,        30),
    ("GOZZO",         gozzo,          40),
    ("AGAVE",         agave,          40),
    ("CRANE",         shipyard_crane, 96),
    ("DUOMO",         amalfi_duomo,   80),
    ("BEACH",         ombrelloni,     44),
    ("EDICOLA",       edicola,        26),
    ("OLEANDER",      oleander,       36),
    ("STALL",         limoncello_stall, 34),
] + [(f"ING_{i}", (lambda k: (lambda: ingredient(k)))(i), 22) for i in range(9)]

# Alternative look-up tables recolour houses without new pixels.
DOME_VIETRI = {(60, 140, 90): (40, 110, 200), (244, 206, 40): (250, 220, 90)}  # blue-gold Vietri tiles
HOUSE_RECOLOURS = [
    {(240, 200, 170): (250, 220, 120)},   # Positano ochre
    {(240, 200, 170): (246, 172, 170)},   # pink
    {(240, 200, 170): (246, 244, 236)},   # whitewash
]

# UI / effect colours that also need static palette slots.
UI_COLOURS = {
    "BLACK": (8, 8, 12), "WHITE": (255, 255, 255), "SHADOW": (24, 20, 40),
    "YELLOW": (255, 226, 40), "ORANGE": (255, 150, 30), "RED": (230, 40, 40),
    "DKRED": (130, 16, 24), "GREEN": (70, 200, 90), "BLUE": (40, 110, 220),
    "CYAN": (90, 220, 240), "GREY": (150, 150, 160), "DKGREY": (70, 72, 84),
    "PANEL": (18, 26, 58), "PANEL2": (34, 50, 104), "GOLD": (255, 200, 60),
    "PINK": (255, 120, 170), "LOGO0": (255, 250, 150), "LOGO1": (255, 214, 60),
    "LOGO2": (255, 150, 40), "LOGO3": (240, 80, 40), "LOGO4": (200, 30, 60),
    "LOGO5": (140, 20, 80), "MAPSEA": (40, 96, 170), "MAPLAND": (122, 158, 88),
    "MAPLAND2": (170, 190, 120), "MAPROAD": (255, 70, 60), "BUN": (212, 150, 70),
    "BUN_D": (150, 90, 36), "BUN_L": (246, 206, 130), "BUN_CUT": (250, 236, 200),
    "LETTUCE": (100, 180, 70),
    "PASTEL0": (250, 222, 130), "PASTEL1": (246, 176, 170), "PASTEL2": (250, 248, 238),
    "PASTEL3": (170, 210, 230), "PASTEL4": (240, 190, 120), "PASTEL5": (220, 150, 110),
    "BRAKE": (255, 60, 60), "BRAKE_HI": (255, 190, 170),
    "STONE": (206, 194, 172), "STONE_D": (160, 146, 122), "ROCK": (150, 130, 104),
    "ROCK_D": (104, 88, 70), "LAMP": (255, 190, 90), "DUST": (190, 180, 160),
}

# Theme slots (192..231), re-programmed per leg.  Order matters: game.c uses
# the same enumeration.
THEME_SLOTS = [
    "SKY0", "SKY1", "SKY2", "SKY3", "SKY4", "SKY5", "SKY6", "SKY7",
    "SEA_L", "SEA_D", "SEA_FAR", "SEA_GLINT",
    "LAND_L", "LAND_D", "LAND_FAR",
    "ROAD_L", "ROAD_D", "RUMBLE_L", "RUMBLE_D", "LINE",
    "MTN_FAR", "MTN_FAR_HI", "MTN_NEAR", "MTN_NEAR_HI", "MTN_NEAR_SH",
    "SUN", "SUN_HI", "CLOUD", "CLOUD_SH", "HAZE",
    "WALL_L", "WALL_D", "CEIL", "VALLEY_L", "VALLEY_D", "TOWN_L", "TOWN_D",
]

# (sky top -> horizon ramp endpoints, and material colours) per leg + title.
def theme(sky_top, sky_mid, sky_hor, sea, sea_far, land, road, mtn_far, mtn_near, sun, cloud, town):
    t = {}
    for i in range(8):
        k = i / 7
        if k < 0.55:
            t[f"SKY{i}"] = mix(sky_top, sky_mid, k / 0.55)
        else:
            t[f"SKY{i}"] = mix(sky_mid, sky_hor, (k - 0.55) / 0.45)
    t["SEA_L"] = sea
    t["SEA_D"] = shade(sea, 0.82)
    t["SEA_FAR"] = sea_far
    t["SEA_GLINT"] = mix(sea, (255, 255, 255), 0.55)
    t["LAND_L"] = land
    t["LAND_D"] = shade(land, 0.86)
    t["LAND_FAR"] = mix(land, sky_hor, 0.45)
    t["ROAD_L"] = road
    t["ROAD_D"] = shade(road, 0.9)
    t["RUMBLE_L"] = (238, 238, 232)
    t["RUMBLE_D"] = (200, 40, 44)
    t["LINE"] = (246, 246, 240)
    t["MTN_FAR"] = mtn_far
    t["MTN_FAR_HI"] = mix(mtn_far, sky_hor, 0.35)
    t["MTN_NEAR"] = mtn_near
    t["MTN_NEAR_HI"] = mix(mtn_near, (255, 250, 220), 0.25)
    t["MTN_NEAR_SH"] = shade(mtn_near, 0.78)
    t["SUN"] = sun
    t["SUN_HI"] = mix(sun, (255, 255, 230), 0.6)
    t["CLOUD"] = cloud
    t["CLOUD_SH"] = mix(cloud, sky_mid, 0.4)
    t["HAZE"] = mix(sky_hor, sea_far, 0.5)
    t["WALL_L"] = (92, 84, 76)
    t["WALL_D"] = (62, 56, 52)
    t["CEIL"] = (34, 30, 30)
    t["VALLEY_L"] = shade(mtn_near, 0.8)
    t["VALLEY_D"] = shade(mtn_near, 0.62)
    t["TOWN_L"] = town
    t["TOWN_D"] = shade(town, 0.86)
    return [t[k] for k in THEME_SLOTS]


THEMES = [
    # Napoli -> Castellammare: bright morning, Vesuvius
    theme((40, 110, 210), (100, 170, 240), (206, 230, 246), (30, 110, 180), (90, 150, 200),
          (120, 150, 80), (96, 96, 104), (130, 130, 160), (80, 110, 70), (255, 250, 210), (250, 250, 255), (176, 170, 160)),
    # Castellammare -> Vico: Faito slopes, clear
    theme((34, 104, 206), (96, 168, 236), (200, 226, 244), (24, 104, 176), (84, 146, 198),
          (104, 140, 70), (98, 98, 106), (120, 128, 168), (70, 104, 62), (255, 250, 210), (250, 250, 255), (180, 172, 160)),
    # Vico -> Meta: late morning
    theme((30, 98, 200), (90, 164, 236), (196, 224, 246), (20, 100, 176), (80, 144, 200),
          (112, 144, 72), (100, 100, 108), (122, 130, 170), (76, 110, 64), (255, 250, 210), (250, 250, 255), (184, 176, 162)),
    # Meta -> Sorrento: noon
    theme((26, 94, 198), (86, 160, 236), (192, 222, 248), (16, 96, 180), (76, 140, 204),
          (118, 150, 76), (104, 102, 110), (126, 134, 176), (82, 118, 68), (255, 252, 220), (252, 252, 255), (196, 184, 166)),
    # Sorrento -> Nerano: early afternoon, Capri
    theme((30, 96, 196), (96, 164, 232), (210, 226, 240), (18, 100, 170), (86, 146, 196),
          (124, 148, 78), (104, 102, 108), (116, 120, 164), (86, 118, 70), (255, 244, 200), (250, 248, 250), (198, 184, 164)),
    # Nerano -> Positano: afternoon warmth
    theme((44, 100, 188), (120, 170, 222), (230, 222, 206), (26, 96, 160), (110, 150, 184),
          (136, 146, 80), (104, 100, 104), (132, 122, 150), (94, 116, 70), (255, 232, 170), (255, 244, 236), (206, 186, 160)),
    # Positano -> Amalfi: golden afternoon
    theme((60, 96, 176), (150, 166, 204), (250, 214, 170), (36, 90, 150), (140, 150, 170),
          (146, 142, 80), (104, 98, 100), (150, 120, 140), (102, 112, 68), (255, 214, 130), (255, 236, 214), (212, 184, 150)),
    # Amalfi -> Cetara: late afternoon
    theme((70, 80, 160), (190, 150, 180), (255, 190, 130), (40, 76, 140), (170, 140, 150),
          (140, 128, 76), (98, 90, 96), (160, 110, 130), (96, 96, 66), (255, 186, 90), (255, 214, 196), (206, 170, 140)),
    # Cetara -> Vietri: sunset
    theme((50, 44, 120), (200, 90, 120), (255, 170, 80), (54, 60, 120), (200, 110, 100),
          (120, 100, 70), (88, 80, 90), (140, 80, 110), (80, 70, 60), (255, 150, 50), (255, 180, 170), (190, 150, 120)),
    # Title / attract: glowing dusk
    theme((30, 20, 90), (170, 60, 130), (255, 150, 70), (40, 50, 120), (190, 100, 110),
          (110, 90, 70), (84, 76, 88), (110, 60, 110), (70, 60, 60), (255, 200, 60), (255, 160, 170), (180, 140, 120)),
]


# ---------------------------------------------------------------------------
# 5x7 font (column bitmaps, LSB = top row).
# ---------------------------------------------------------------------------
FONT_ROWS = {
    " ": ["     "] * 7,
    "A": [" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
    "B": ["#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "],
    "C": [" ####", "#    ", "#    ", "#    ", "#    ", "#    ", " ####"],
    "D": ["#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "],
    "E": ["#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"],
    "F": ["#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "],
    "G": [" ####", "#    ", "#    ", "#  ##", "#   #", "#   #", " ### "],
    "H": ["#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
    "I": ["#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "#####"],
    "J": ["  ###", "    #", "    #", "    #", "#   #", "#   #", " ### "],
    "K": ["#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"],
    "L": ["#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"],
    "M": ["#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"],
    "N": ["#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"],
    "O": [" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "],
    "P": ["#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "],
    "Q": [" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"],
    "R": ["#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"],
    "S": [" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "],
    "T": ["#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "],
    "U": ["#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "],
    "V": ["#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "],
    "W": ["#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"],
    "X": ["#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"],
    "Y": ["#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "],
    "Z": ["#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"],
    "0": [" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "],
    "1": ["  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "],
    "2": [" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"],
    "3": ["#### ", "    #", "    #", " ### ", "    #", "    #", "#### "],
    "4": ["   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "],
    "5": ["#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "],
    "6": [" ### ", "#    ", "#    ", "#### ", "#   #", "#   #", " ### "],
    "7": ["#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "],
    "8": [" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "],
    "9": [" ### ", "#   #", "#   #", " ####", "    #", "    #", " ### "],
    ".": ["     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "],
    ",": ["     ", "     ", "     ", "     ", " ##  ", "  #  ", " #   "],
    "'": ["  #  ", "  #  ", " #   ", "     ", "     ", "     ", "     "],
    "-": ["     ", "     ", "     ", "#####", "     ", "     ", "     "],
    "!": ["  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  "],
    ":": ["     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     "],
    "/": ["    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    "],
    "?": [" ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "],
    "+": ["     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "],
    ">": [" #   ", "  #  ", "   # ", "    #", "   # ", "  #  ", " #   "],
    "<": ["   # ", "  #  ", " #   ", "#    ", " #   ", "  #  ", "   # "],
    "*": ["     ", "# # #", " ### ", "#####", " ### ", "# # #", "     "],
    "(": ["   # ", "  #  ", " #   ", " #   ", " #   ", "  #  ", "   # "],
    ")": [" #   ", "  #  ", "   # ", "   # ", "   # ", "  #  ", " #   "],
    "%": ["##   ", "##  #", "   # ", "  #  ", " #   ", "#  ##", "   ##"],
    "#": [" # # ", "#####", " # # ", " # # ", "#####", " # # ", "     "],
}
FONT_FIRST, FONT_LAST = 32, 90


def font_bytes() -> list[int]:
    out = []
    for code in range(FONT_FIRST, FONT_LAST + 1):
        rows = FONT_ROWS.get(chr(code), FONT_ROWS["?"])
        for col in range(5):
            v = 0
            for r in range(7):
                if rows[r][col] == "#":
                    v |= 1 << r
            out.append(v)
    return out


# ---------------------------------------------------------------------------
# Route map (lon, lat).  The coastline is a simplified polygon of the Gulf of
# Naples, the Sorrento peninsula and the Amalfi coast; the route follows the
# SS18, SS145, the Massa Lubrense road and the SS163 through the real towns.
# ---------------------------------------------------------------------------
MAP_W, MAP_H = 200, 154
LON0, LON1, LAT0, LAT1 = 14.20, 14.80, 40.87, 40.52
COAST = [
    (14.20, 40.87), (14.80, 40.87), (14.80, 40.668), (14.77, 40.676), (14.728, 40.670),
    (14.70, 40.645), (14.685, 40.638), (14.67, 40.630), (14.645, 40.643), (14.626, 40.644),
    (14.61, 40.632), (14.60, 40.630), (14.575, 40.612), (14.55, 40.610), (14.53, 40.605),
    (14.50, 40.615), (14.485, 40.623), (14.45, 40.612), (14.41, 40.594), (14.38, 40.586),
    (14.35, 40.580), (14.33, 40.572), (14.322, 40.566), (14.325, 40.584), (14.335, 40.603),
    (14.35, 40.619), (14.375, 40.630), (14.40, 40.637), (14.415, 40.648), (14.42, 40.657),
    (14.43, 40.668), (14.45, 40.683), (14.465, 40.694), (14.48, 40.703), (14.47, 40.722),
    (14.45, 40.745), (14.40, 40.766), (14.37, 40.783), (14.345, 40.806), (14.30, 40.826),
    (14.265, 40.838), (14.24, 40.833), (14.22, 40.828), (14.20, 40.815),
]
CAPRI = [(14.19, 40.556), (14.21, 40.545), (14.25, 40.543), (14.27, 40.550), (14.26, 40.560), (14.22, 40.562)]
ROUTE = [
    (14.262, 40.842), (14.34, 40.812), (14.375, 40.787), (14.45, 40.752), (14.482, 40.707),  # Napoli .. Castellammare
    (14.432, 40.668), (14.417, 40.649), (14.377, 40.631), (14.347, 40.612), (14.337, 40.594),  # Vico, Meta, Sorrento, Massa, Termini
    (14.352, 40.586), (14.372, 40.606), (14.402, 40.617), (14.486, 40.628), (14.53, 40.611),   # Nerano, S.Agata, Colli, Positano, Praiano
    (14.552, 40.615), (14.601, 40.635), (14.626, 40.648), (14.645, 40.648), (14.67, 40.636),   # Furore, Amalfi, Minori, Maiori, C.d'Orso
    (14.70, 40.649), (14.728, 40.673),                                                        # Cetara, Vietri
]
ROUTE_TOWN_INDEX = [0, 4, 5, 6, 7, 10, 13, 16, 20, 21]


def map_xy(p):
    lon, lat = p
    return (round((lon - LON0) / (LON1 - LON0) * MAP_W), round((LAT0 - lat) / (LAT0 - LAT1) * MAP_H))


# ---------------------------------------------------------------------------
# Encoding
# ---------------------------------------------------------------------------
def rgb565(c):
    r, g, b = c
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def from565(v):
    r = (v >> 11) & 31
    g = (v >> 5) & 63
    b = v & 31
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


def kmeans_palette(colours: list[tuple], k: int, weights: list[int]) -> list[tuple]:
    uniq = {}
    for c, w in zip(colours, weights):
        uniq[c] = uniq.get(c, 0) + w
    pts = list(uniq.items())
    if len(pts) <= k:
        return [p for p, _ in pts]
    # farthest-point seeding, then Lloyd iterations
    pts.sort(key=lambda p: -p[1])
    centres = [pts[0][0]]
    dist = {p: 1e18 for p, _ in pts}

    def d2(a, b):
        return (a[0] - b[0]) ** 2 * 3 + (a[1] - b[1]) ** 2 * 4 + (a[2] - b[2]) ** 2 * 2

    while len(centres) < k:
        best, bestv = None, -1
        for p, w in pts:
            dist[p] = min(dist[p], d2(p, centres[-1]))
            v = dist[p] * (1 + math.log(1 + w))
            if v > bestv:
                best, bestv = p, v
        centres.append(best)
    for _ in range(12):
        acc = [[0, 0, 0, 0] for _ in centres]
        for p, w in pts:
            j = min(range(len(centres)), key=lambda i: d2(p, centres[i]))
            a = acc[j]
            a[0] += p[0] * w; a[1] += p[1] * w; a[2] += p[2] * w; a[3] += w
        centres = [(round(a[0] / a[3]), round(a[1] / a[3]), round(a[2] / a[3])) if a[3] else centres[i]
                   for i, a in enumerate(acc)]
    return centres


def nearest(pal, c):
    return min(range(len(pal)), key=lambda i: (pal[i][0] - c[0]) ** 2 * 3 + (pal[i][1] - c[1]) ** 2 * 4 + (pal[i][2] - c[2]) ** 2 * 2)


def rle_rows(img: Image.Image, local: dict) -> bytes:
    """Row groups: [repeat count][runs...] where runs cover the full width.

    Identical consecutive rows share one group, so flat walls, windows and
    bodywork cost one entry (and one tall rectangle at draw time).
    """
    px = img.load()

    def row_runs(y):
        out = bytearray()
        x = 0
        while x < img.width:
            p = px[x, y]
            c = 0 if p[3] == 0 else local[p[:3]]
            n = 1
            while x + n < img.width and n < 16:
                q = px[x + n, y]
                cq = 0 if q[3] == 0 else local[q[:3]]
                if cq != c:
                    break
                n += 1
            out.append((c << 4) | (n - 1))
            x += n
        return bytes(out)

    rows = [row_runs(y) for y in range(img.height)]
    out = bytearray()
    y = 0
    while y < img.height:
        n = 1
        while y + n < img.height and rows[y + n] == rows[y] and n < 255:
            n += 1
        out.append(n)
        out.extend(rows[y])
        y += n
    return bytes(out)


def c_array(name, ctype, values, per_line=16, fmt="0x{:02x}"):
    lines = [f"static const {ctype} {name}[{len(values)}]={{"]
    for i in range(0, len(values), per_line):
        lines.append("  " + ",".join(fmt.format(v) for v in values[i:i + per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    GEN.mkdir(parents=True, exist_ok=True)
    images = []
    for name, painter, mult in SPRITES:
        images.append((name, painter(), mult))

    # Gather colours with pixel weights for the shared palette.
    colours, weights = [], []
    for _, im, _ in images:
        hist = {}
        for p in im.getdata():
            if p[3]:
                hist[p[:3]] = hist.get(p[:3], 0) + 1
        for c, w in hist.items():
            colours.append(c)
            weights.append(w)
    for rec in HOUSE_RECOLOURS + [DOME_VIETRI]:
        for c in rec.values():
            colours.append(c); weights.append(400)
    ui_names = list(UI_COLOURS)
    budget = STATIC_COUNT - len(ui_names)
    sprite_pal = kmeans_palette(colours, budget, weights)
    static_pal = [UI_COLOURS[n] for n in ui_names] + sprite_pal
    assert len(static_pal) <= STATIC_COUNT
    while len(static_pal) < STATIC_COUNT:
        static_pal.append((0, 0, 0))
    static_pal = [from565(rgb565(c)) for c in static_pal]

    sprite_meta = []
    blob = bytearray()
    luts = []
    remapped_images = []
    for name, im, mult in images:
        cols = sorted({p[:3] for p in im.getdata() if p[3]}, key=lambda c: -sum(c))
        assert len(cols) <= 15, (name, len(cols))
        local = {c: i + 1 for i, c in enumerate(cols)}
        lut = [0] * 15
        for c, i in local.items():
            lut[i - 1] = STATIC_BASE + nearest(static_pal, c)
        data = rle_rows(im, local)
        sprite_meta.append((name, im.width, im.height, len(blob), len(luts), mult))
        luts.append(lut)
        blob.extend(data)
        # preview with the final palette
        prev = Image.new("RGBA", im.size)
        pp = prev.load(); ip = im.load()
        for y in range(im.height):
            for x in range(im.width):
                p = ip[x, y]
                pp[x, y] = (0, 0, 0, 0) if not p[3] else static_pal[lut[local[p[:3]] - 1] - STATIC_BASE] + (255,)
        remapped_images.append((name, prev))

    # house recolour LUTs
    house_idx = [m[0] for m in sprite_meta].index("HOUSE")
    house_im = images[house_idx][1]
    hcols = sorted({p[:3] for p in house_im.getdata() if p[3]}, key=lambda c: -sum(c))
    base_lut = luts[sprite_meta[house_idx][4]]
    alt_luts = []
    for rec in HOUSE_RECOLOURS:
        src, dst = next(iter(rec.items()))
        lut = list(base_lut)
        for i, c in enumerate(hcols):
            # wall tones are the family nearest the recoloured wall colour
            dist = sum(abs(c[k] - src[k]) for k in range(3))
            if dist < 120:
                ratio = sum(c) / max(1, sum(src))
                target = shade(dst, ratio)
                lut[i] = STATIC_BASE + nearest(static_pal, target)
        alt_luts.append(lut)
    luts.extend(alt_luts)
    # Vietri dome: swap the green/yellow tiles for Vietri's blue and gold
    dome_idx = [m[0] for m in sprite_meta].index("DOME")
    dome_im = images[dome_idx][1]
    dcols = sorted({p[:3] for p in dome_im.getdata() if p[3]}, key=lambda c: -sum(c))
    vl = list(luts[sprite_meta[dome_idx][4]])
    for i, c in enumerate(dcols):
        for src, dst in DOME_VIETRI.items():
            if sum(abs(c[k] - src[k]) for k in range(3)) < 90:
                ratio = sum(c) / max(1, sum(src))
                vl[i] = STATIC_BASE + nearest(static_pal, shade(dst, ratio))
    luts.append(vl)

    # Preview sheet
    sheet_w = 520
    x = y = 4
    row_h = 0
    placements = []
    for name, prev in remapped_images:
        w, h = prev.size[0] * 2, prev.size[1] * 2
        if x + w > sheet_w:
            x = 4; y += row_h + 6; row_h = 0
        placements.append((prev, x, y, w, h))
        x += w + 6
        row_h = max(row_h, h)
    sheet = Image.new("RGB", (sheet_w, y + row_h + 4), (60, 120, 190))
    for prev, x, y, w, h in placements:
        sheet.paste(prev.resize((w, h), Image.Resampling.NEAREST), (x, y), prev.resize((w, h), Image.Resampling.NEAREST))
    sheet.save(GEN / "sprite_sheet.png", optimize=True)

    cars = Image.new("RGB", (4 * 60 * 3, 44 * 3), (96, 96, 104))
    for i in range(4):
        prev = remapped_images[i][1].resize((56 * 3, 36 * 3), Image.Resampling.NEAREST)
        cars.paste(prev, (i * 180 + 6, 12), prev)
    cars.save(GEN / "cars.png", optimize=True)

    # Theme swatches
    sw = Image.new("RGB", (len(THEME_SLOTS) * 8, len(THEMES) * 8))
    sd = ImageDraw.Draw(sw)
    for j, th in enumerate(THEMES):
        for i, c in enumerate(th):
            sd.rectangle([i * 8, j * 8, i * 8 + 7, j * 8 + 7], fill=c)
    sw.resize((sw.width * 3, sw.height * 3), Image.Resampling.NEAREST).save(GEN / "theme_swatches.png")

    # ---- emit header ----
    h = []
    h.append("/* Generated by tools/generate_assets.py - do not edit. */")
    h.append("#ifndef OUTBUN_ASSETS_H\n#define OUTBUN_ASSETS_H\n#include <stdint.h>")
    h.append(f"#define OB_STATIC_BASE {STATIC_BASE}\n#define OB_STATIC_COUNT {STATIC_COUNT}\n#define OB_THEME_BASE {THEME_BASE}")
    h.append(f"#define OB_THEME_COUNT {len(THEME_SLOTS)}\n#define OB_THEMES {len(THEMES)}")
    for i, n in enumerate(ui_names):
        h.append(f"#define C_{n} {STATIC_BASE + i}")
    for i, n in enumerate(THEME_SLOTS):
        h.append(f"#define T_{n} {THEME_BASE + i}")
    for i, m in enumerate(sprite_meta):
        h.append(f"#define SPR_{m[0]} {i}")
    h.append(f"#define SPR_COUNT {len(sprite_meta)}")
    h.append(f"#define LUT_HOUSE_ALT {len(sprite_meta)}")
    h.append(f"#define LUT_DOME_VIETRI {len(sprite_meta) + len(HOUSE_RECOLOURS)}")
    h.append("typedef struct { uint8_t w, h, lut, mult; uint16_t off; } ob_sprite_t;")
    h.append("static const ob_sprite_t ob_sprites[SPR_COUNT]={")
    for name, w, hh, off, lut, mult in sprite_meta:
        h.append(f"  {{{w},{hh},{lut},{mult},{off}}}, /* {name} */")
    h.append("};")
    h.append(c_array("ob_luts", "uint8_t", [v for lut in luts for v in lut], 15, "{}"))
    h.append(c_array("ob_rle", "uint8_t", list(blob)))
    h.append(c_array("ob_static_palette", "uint16_t", [rgb565(c) for c in static_pal], 12, "0x{:04x}"))
    h.append(c_array("ob_themes", "uint16_t", [rgb565(c) for th in THEMES for c in th], 12, "0x{:04x}"))
    h.append(f"#define FONT_FIRST {FONT_FIRST}\n#define FONT_LAST {FONT_LAST}")
    h.append(c_array("ob_font", "uint8_t", font_bytes(), 15))
    h.append(f"#define MAP_W {MAP_W}\n#define MAP_H {MAP_H}")
    h.append(f"#define MAP_COAST_N {len(COAST)}\n#define MAP_CAPRI_N {len(CAPRI)}\n#define MAP_ROUTE_N {len(ROUTE)}")
    h.append(c_array("map_coast", "uint8_t", [v for p in COAST for v in map_xy(p)], 16, "{}"))
    h.append(c_array("map_capri", "uint8_t", [max(0, min(255, v)) for p in CAPRI for v in map_xy(p)], 16, "{}"))
    h.append(c_array("map_route", "uint8_t", [v for p in ROUTE for v in map_xy(p)], 16, "{}"))
    h.append(c_array("map_town_index", "uint8_t", ROUTE_TOWN_INDEX, 16, "{}"))
    h.append("#endif")
    (ROOT / "src" / "assets.h").write_text("\n".join(h) + "\n")

    stats = {
        "sprites": len(sprite_meta),
        "rle_bytes": len(blob),
        "lut_bytes": len(luts) * 15,
        "palette_entries": STATIC_COUNT,
        "theme_bytes": len(THEMES) * len(THEME_SLOTS) * 2,
        "per_sprite": {m[0]: [m[1], m[2]] for m in sprite_meta},
    }
    (GEN / "asset_stats.json").write_text(json.dumps(stats, indent=1) + "\n")
    print(f"assets: {len(sprite_meta)} sprites, rle={len(blob)} B, luts={len(luts)*15} B, palette={STATIC_COUNT}")


if __name__ == "__main__":
    main()


# ---------------------------------------------------------------------------
# Store icon: a 64x64 pixel-art scene upscaled 4x (flat colours keep the PNG
# small, which matters because the icon is embedded in the 64 KiB cartridge).
# ---------------------------------------------------------------------------
def make_icon(path: Path):
    im = Image.new("RGB", (64, 64))
    d = ImageDraw.Draw(im)
    th = THEMES[9]
    sky = [th[i] for i in range(8)]
    for y in range(34):
        d.line([(0, y), (63, y)], fill=sky[min(7, y * 8 // 34)])
    d.ellipse([34, 14, 54, 34], fill=(255, 200, 70))
    for y in (26, 29, 32):
        d.line([(34, y), (54, y)], fill=sky[min(7, y * 8 // 34)])
    d.polygon([(0, 34), (0, 22), (8, 16), (16, 20), (22, 15), (30, 34)], fill=(110, 60, 110))  # Vesuvio
    d.rectangle([0, 34, 63, 63], fill=(40, 60, 130))
    for y in range(36, 64, 3):
        d.line([(40 + (y * 7) % 20, y), (50 + (y * 7) % 20, y)], fill=(200, 110, 120))
    d.polygon([(26, 34), (38, 34), (60, 64), (4, 64)], fill=(84, 76, 88))
    for y in range(36, 64, 6):
        w = 1 + (y - 34) // 8
        d.rectangle([32 - w // 2, y, 32 + w // 2, y + 2], fill=(246, 246, 240))
    car = fiat500().resize((42, 27), Image.Resampling.NEAREST)
    im.paste(car, (11, 35), car)
    bun = ingredient(0).resize((20, 20), Image.Resampling.NEAREST)
    im.paste(bun, (3, 2), bun)
    icon = im.resize((256, 256), Image.Resampling.NEAREST).quantize(colors=32, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    icon.save(path, optimize=True)


if __name__ == "__main__":
    make_icon(GEN / "icon.png")
    print(f"icon: {(GEN / 'icon.png').stat().st_size} bytes")
