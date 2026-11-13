#!/usr/bin/env python3
"""
Draws the LiveArea pictures (extras/livearea) from scratch: a plain night
field with fog and the port's name (no artwork taken from the game).

  icon0.png    128x128  bubble
  bg0.png      840x500  LiveArea background
  startup.png  280x158  gate
  pic0.png     960x544  shown while the app starts

Usage: tools/livearea.py <out dir>   (needs Pillow and a DejaVu font)
"""
import random
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1]
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def scene(w, h, seed, title_size=0, sub=None):
    random.seed(seed)
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        t = y / (h - 1)
        c = (int(10 + 34 * t), int(14 + 40 * t), int(22 + 38 * t))
        for x in range(w):
            px[x, y] = c
    d = ImageDraw.Draw(im)
    horizon = int(h * 0.68)
    # far tree line
    x = 0
    while x < w:
        tw = random.randint(w // 60 + 2, w // 22 + 4)
        th = random.randint(h // 12, h // 5)
        d.polygon([(x, horizon), (x + tw // 2, horizon - th), (x + tw, horizon)], fill=(6, 9, 12))
        x += tw - 2
    d.rectangle([0, horizon, w, h], fill=(5, 7, 9))
    # fog band over the horizon
    fog = Image.new("RGB", (w, h), (0, 0, 0))
    fd = ImageDraw.Draw(fog)
    for _ in range(14):
        cx, cy = random.randint(0, w), horizon + random.randint(-h // 14, h // 20)
        rx, ry = random.randint(w // 6, w // 2), random.randint(h // 30 + 1, h // 12 + 2)
        fd.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=(46, 54, 58))
    fog = fog.filter(ImageFilter.GaussianBlur(max(2, h // 30)))
    im = Image.blend(im, Image.eval(fog, lambda v: min(255, v * 2)), 0.35)
    d = ImageDraw.Draw(im)
    if title_size:
        f = ImageFont.truetype(FONT, title_size)
        text = "INTO THE DEAD"
        tw = d.textlength(text, font=f)
        d.text(((w - tw) / 2, h * 0.16), text, font=f, fill=(214, 214, 206))
        if sub:
            f2 = ImageFont.truetype(FONT, max(9, title_size // 3))
            sw = d.textlength(sub, font=f2)
            d.text(((w - sw) / 2, h * 0.16 + title_size * 1.35), sub, font=f2, fill=(150, 156, 150))
    return im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)


scene(128, 128, 1, 13).save(OUT + "/icon0.png", optimize=True)
scene(840, 500, 2).save(OUT + "/bg0.png", optimize=True)
scene(280, 158, 3, 26, "PS Vita port").save(OUT + "/startup.png", optimize=True)
scene(960, 544, 4, 72, "PS Vita port").save(OUT + "/pic0.png", optimize=True)
