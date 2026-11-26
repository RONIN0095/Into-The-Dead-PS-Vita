#!/usr/bin/env python3
"""
Draws the LiveArea pictures (extras/livearea) from scratch: a foggy field
at dusk under a heavy sky, a spruce tree line, a radio mast, smoke and the
silhouettes of the dead walking out of the grass. No artwork is taken from
the game; everything here is generated.

  icon0.png    128x128  bubble
  bg0.png      840x500  LiveArea background
  startup.png  280x158  gate
  pic0.png     960x544  shown while the app starts

Usage: tools/livearea.py <out dir>   (needs Pillow, numpy and DejaVu fonts)
"""
import math
import random
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
SERIF = "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"
SANS = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
SS = 2  # drawn at twice the size, then scaled down (smooth edges)


def noise(w, h, seed, octaves=6, scale=0.5):
    """Fractal value noise in 0..1."""
    rng = np.random.default_rng(seed)
    out = np.zeros((h, w), np.float32)
    amp, total = 1.0, 0.0
    for o in range(octaves):
        cw, ch = max(2, int(w * scale / 64 * 2 ** o) + 2), max(2, int(h * scale / 64 * 2 ** o) + 2)
        layer = Image.fromarray((rng.random((ch, cw)) * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC)
        out += np.asarray(layer, np.float32) / 255 * amp
        total += amp
        amp *= 0.5
    return out / total


def sky(w, h, horizon, seed):
    y = np.linspace(0, 1, h, dtype=np.float32)[:, None]
    # dark at the top, a pale cold glow at the horizon
    glow = np.clip(1 - np.abs(y - horizon / h) * 2.2, 0, 1) ** 2
    base = np.stack([18 + 70 * glow + 6 * y, 22 + 76 * glow + 6 * y, 28 + 80 * glow + 4 * y], -1)
    n = noise(w, h, seed, 7, 0.9)
    n2 = noise(w, h, seed + 7, 5, 0.35)
    clouds = np.clip((n * 0.65 + n2 * 0.55 - 0.45) * 2.2, 0, 1)[..., None]
    lit = np.clip(1 - y * 1.1, 0, 1)[..., None]
    img = base * (1 - clouds * 0.55) + clouds * (np.array([70, 76, 82]) * (0.5 + 0.5 * glow[..., None]) + 25 * lit)
    # brighter break in the clouds
    cx = 0.62
    xs = np.linspace(0, 1, w, dtype=np.float32)[None, :]
    rift = np.exp(-((xs - cx) ** 2) / 0.03 - ((y - horizon / h * 0.8) ** 2) / 0.02)[..., None]
    img = img + rift * np.array([60, 64, 66]) * (0.6 + 0.4 * n[..., None])
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")


def spruce(d, x, base, height, width, color):
    tiers = max(4, int(height / (width * 0.45)))
    for i in range(tiers):
        t = i / tiers
        ty = base - height * t
        tw = width * (1 - t) * 0.5 + width * 0.06
        d.polygon([(x - tw, ty), (x, ty - height / tiers * 1.9), (x + tw, ty)], fill=color)
    d.line([(x, base), (x, base - height)], fill=color, width=max(1, int(width * 0.06)))


def tree_line(d, w, top, bottom, rng, color, density):
    x = -20
    while x < w + 20:
        h = rng.uniform(0.55, 1.0) * (bottom - top)
        spruce(d, x, bottom, h, h * rng.uniform(0.32, 0.45), color)
        x += h * rng.uniform(0.1, 0.25) / density
    d.rectangle([0, bottom - 2, w, bottom + 400], fill=color)


def mast(d, x, base, height, color, s):
    top = base - height
    d.line([(x - 4 * s, base), (x, top)], fill=color, width=max(1, int(1.6 * s)))
    d.line([(x + 4 * s, base), (x, top)], fill=color, width=max(1, int(1.6 * s)))
    for i in range(1, 14):
        y = base - height * i / 14
        hw = 4 * s * (1 - i / 14)
        d.line([(x - hw, y), (x + hw, y - height / 28)], fill=color, width=max(1, s // 2))
    for gx in (-height * 0.55, height * 0.6):  # guy wires
        for f in (0.35, 0.7):
            d.line([(x, base - height * f), (x + gx, base)], fill=color, width=1)
    d.rectangle([x - 3 * s, top - 6 * s, x + 3 * s, top], fill=color)


def smoke(img, x, base, height, seed, s):
    w, h = img.size
    layer = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(layer)
    rng = random.Random(seed)
    for i in range(70):
        t = i / 70
        cx = x + math.sin(t * 5 + seed) * 10 * s + t * height * 0.35
        cy = base - t * height
        r = (5 + 24 * t) * s * rng.uniform(0.7, 1.2)
        d.ellipse([cx - r, cy - r * 0.8, cx + r, cy + r * 0.8], fill=int(150 * (1 - t * 0.6)))
    layer = layer.filter(ImageFilter.GaussianBlur(5 * s))
    dark = Image.new("RGB", (w, h), (14, 15, 16))
    return Image.composite(dark, img, layer)


def limb(d, p0, p1, w0, w1, color):
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    n = math.hypot(dx, dy) or 1
    nx, ny = -dy / n, dx / n
    d.polygon([(p0[0] + nx * w0, p0[1] + ny * w0), (p1[0] + nx * w1, p1[1] + ny * w1),
               (p1[0] - nx * w1, p1[1] - ny * w1), (p0[0] - nx * w0, p0[1] - ny * w0)], fill=color)
    d.ellipse([p1[0] - w1, p1[1] - w1, p1[0] + w1, p1[1] + w1], fill=color)


def walker(d, x, base, H, rng, color, eyes):
    """A shambling silhouette: hunched, head hanging forward, arms limp or reaching."""
    lean = rng.uniform(0.02, 0.1) * H * rng.choice((-1, 1))
    hip = (x + lean * 0.3, base - H * 0.47)
    sh = (x + lean, base - H * 0.8)          # middle of the shoulders
    sw = H * rng.uniform(0.11, 0.13)          # half shoulder width
    tilt = rng.uniform(-0.04, 0.04) * H       # one shoulder lower
    ls, rs = (sh[0] - sw, sh[1] + tilt), (sh[0] + sw, sh[1] - tilt)
    # legs, one forward
    st = rng.uniform(0.04, 0.1) * H
    knee = rng.uniform(0.01, 0.04) * H
    limb(d, (hip[0] - sw * 0.4, hip[1]), (x - st + knee, base - H * 0.24), H * 0.045, H * 0.035, color)
    limb(d, (x - st + knee, base - H * 0.24), (x - st, base), H * 0.035, H * 0.028, color)
    limb(d, (hip[0] + sw * 0.4, hip[1]), (x + st - knee, base - H * 0.25), H * 0.045, H * 0.035, color)
    limb(d, (x + st - knee, base - H * 0.25), (x + st, base), H * 0.035, H * 0.028, color)
    # torso with a torn hem
    hem = [(hip[0] + sw * 0.75, hip[1] + H * 0.03)]
    for i in range(1, 6):
        hem.append((hip[0] + sw * 0.75 - sw * 1.5 * i / 5, hip[1] + H * (0.01 + 0.04 * rng.random())))
    d.polygon([ls, (sh[0], sh[1] - H * 0.015), rs, (hip[0] + sw * 0.8, hip[1] - H * 0.05)] + hem +
              [(hip[0] - sw * 0.8, hip[1] - H * 0.05)], fill=color)
    # arms
    for s0, side in ((ls, -1), (rs, 1)):
        if rng.random() < 0.45:  # reaching forward
            el = (s0[0] + side * H * 0.03 + H * 0.06, s0[1] + H * 0.1)
            ha = (el[0] + H * 0.12, el[1] + H * rng.uniform(-0.03, 0.04))
        else:  # hanging
            el = (s0[0] + side * H * 0.03, s0[1] + H * 0.17)
            ha = (el[0] + side * H * 0.01 + lean * 0.2, el[1] + H * 0.16)
        limb(d, s0, el, H * 0.036, H * 0.03, color)
        limb(d, el, ha, H * 0.03, H * 0.024, color)
    # neck and a head hanging forward and to one side
    hr = H * 0.058
    hx, hy = sh[0] + rng.uniform(0.01, 0.04) * H, sh[1] - H * 0.06
    limb(d, sh, (hx, hy), H * 0.03, H * 0.028, color)
    d.ellipse([hx - hr, hy - hr * 1.15, hx + hr, hy + hr], fill=color)
    if eyes:
        er = max(1.0, hr * 0.16)
        for ex in (hx - hr * 0.32, hx + hr * 0.38):
            d.ellipse([ex - er, hy - er * 0.7, ex + er, hy + er * 0.7], fill=eyes)


def grass(d, w, top, bottom, rng, color, s, count):
    d.rectangle([0, top, w, bottom], fill=color)
    for _ in range(count):
        x = rng.uniform(0, w)
        y = rng.uniform(top - 4 * s, top + 10 * s)
        hgt = rng.uniform(6, 22) * s
        bend = rng.uniform(-6, 6) * s
        d.line([(x, y + 8 * s), (x + bend * 0.5, y - hgt * 0.5), (x + bend, y - hgt)], fill=color, width=max(1, s))
        if rng.random() < 0.12:  # seed heads
            r = 1.6 * s
            d.ellipse([x + bend - r, y - hgt - r, x + bend + r, y - hgt + r], fill=color)


def fog(img, top, bottom, seed, strength):
    w, h = img.size
    n = noise(w, h, seed, 5, 0.6)
    y = np.linspace(0, 1, h, dtype=np.float32)[:, None]
    band = np.exp(-((y - (top + bottom) / 2 / h) ** 2) / (((bottom - top) / h) ** 2 * 0.5))
    a = np.clip(band * (0.5 + 0.7 * n) * strength, 0, 1)[..., None]
    arr = np.asarray(img, np.float32)
    col = np.array([92, 100, 106], np.float32)
    return Image.fromarray(np.clip(arr * (1 - a) + col * a, 0, 255).astype(np.uint8))


def vignette(img, amount):
    w, h = img.size
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    r = np.sqrt(((xs - w / 2) / (w / 2)) ** 2 + ((ys - h / 2) / (h / 2)) ** 2)
    v = np.clip(1 - (r - 0.55) * amount, 0.25, 1)[..., None]
    return Image.fromarray(np.clip(np.asarray(img, np.float32) * v, 0, 255).astype(np.uint8))


def grain(img, seed, amount):
    rng = np.random.default_rng(seed)
    arr = np.asarray(img, np.float32)
    arr += rng.normal(0, amount, arr.shape[:2])[..., None]
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))


def distressed_text(img, text, font_path, size, center, fill, seed, tracking=0.12, shadow=True):
    """Title letters worn by noise, with a soft dark halo behind them."""
    w, h = img.size
    mask = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(mask)
    while True:
        f = ImageFont.truetype(font_path, size)
        widths = [d.textlength(c, font=f) for c in text]
        total = sum(widths) + tracking * size * (len(text) - 1)
        if total <= w * 0.86 or size < 8:
            break
        size = int(size * 0.94)
    x = center[0] - total / 2
    asc, desc = f.getmetrics()
    y = center[1] - (asc + desc) / 2
    for c, cw in zip(text, widths):
        d.text((x, y), c, font=f, fill=255)
        x += cw + tracking * size
    wear = noise(w, h, seed, 4, 6.0)
    holes = (wear > 0.66).astype(np.float32)
    m = np.asarray(mask, np.float32) / 255 * (1 - holes * 0.85)
    mask = Image.fromarray((m * 255).astype(np.uint8))
    if shadow:
        halo = mask.filter(ImageFilter.GaussianBlur(size * 0.18))
        img = Image.composite(Image.new("RGB", (w, h), (0, 0, 0)), img, halo.point(lambda v: min(255, v * 1.6)))
    return Image.composite(Image.new("RGB", (w, h), fill), img, mask)


def scene(w, h, seed, walkers=7, mast_x=0.6, smokes=(0.24, 0.8)):
    W, H = w * SS, h * SS
    rng = random.Random(seed)
    horizon = int(H * 0.56)
    img = sky(W, H, horizon, seed)
    for sx in smokes:
        img = smoke(img, int(W * sx), horizon - int(H * 0.08), int(H * 0.38), seed + int(sx * 100), SS * max(1, w // 300))
    d = ImageDraw.Draw(img)
    mast(d, int(W * mast_x), horizon - int(H * 0.04), int(H * 0.42), (26, 30, 34), SS * max(1, w // 400))
    tree_line(d, W, horizon - int(H * 0.17), horizon, rng, (30, 35, 40), 1.3)
    img = fog(img, horizon - int(H * 0.08), horizon + int(H * 0.04), seed + 1, 0.55)
    d = ImageDraw.Draw(img)
    tree_line(d, W, horizon - int(H * 0.1), horizon + int(H * 0.03), random.Random(seed + 2), (19, 22, 25), 1.0)
    img = fog(img, horizon, horizon + int(H * 0.08), seed + 3, 0.45)
    d = ImageDraw.Draw(img)
    field = horizon + int(H * 0.06)
    grass(d, W, field, H, rng, (34, 38, 41), SS, int(W / SS * 1.4))
    arr = np.asarray(img, np.float32)
    yy = np.linspace(0, 1, H, dtype=np.float32)[:, None, None]
    dark = np.clip((yy - field / H) / (1 - field / H), 0, 1) ** 0.8
    arr = arr * (1 - dark * 0.8)
    img = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(img)
    # tufts of tall grass scattered down to the bottom, darker as they come closer
    g = random.Random(seed * 31)
    for _ in range(int(W * H / SS / SS / 60)):
        t = g.random() ** 0.7
        y = field + (H - field) * t
        c = int(30 * (1 - t) + 6)
        x = g.uniform(0, W)
        for _ in range(g.randint(3, 7)):
            hgt = g.uniform(5, 16) * SS * (0.6 + t)
            bx = x + g.uniform(-4, 4) * SS
            bend = g.uniform(-5, 5) * SS
            d.line([(bx, y), (bx + bend * 0.4, y - hgt * 0.5), (bx + bend, y - hgt)], fill=(c, c + 2, c + 3), width=max(1, SS))
    # the dead, far ones small and grey, near ones large and black
    places = sorted((rng.uniform(0.04, 0.96), rng.random()) for _ in range(walkers))
    for px, depth in sorted(places, key=lambda p: p[1]):
        size = H * (0.13 + 0.32 * depth)
        base = field + int(H * (0.02 + 0.28 * depth))
        shade = int(24 - 18 * depth)
        eyes = (170, 186, 170) if depth > 0.45 else None
        walker(d, W * px, base, size, rng, (shade, shade + 2, shade + 3), eyes)
        # grass in front of the legs
        for _ in range(int(size / SS * 0.5)):
            gx = W * px + rng.uniform(-0.2, 0.2) * size
            hgt = rng.uniform(0.05, 0.14) * size
            bend = rng.uniform(-0.03, 0.03) * size
            d.line([(gx, base + 2), (gx + bend * 0.4, base - hgt * 0.5), (gx + bend, base - hgt)],
                   fill=(shade, shade + 2, shade + 3), width=max(1, SS))
    grass(d, W, int(H * 0.9), H, random.Random(seed + 5), (6, 7, 8), SS, int(W / SS * 0.9))
    img = vignette(img, 0.9)
    return img.resize((w, h), Image.LANCZOS)


def finish(img, seed):
    img = grain(img, seed, 3.0)
    return img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)


def titled(img, title_size, sub, y, seed):
    w, h = img.size
    img = distressed_text(img, "INTO THE DEAD", SERIF, title_size, (w / 2, h * y), (222, 220, 210), seed)
    if sub:
        f = ImageFont.truetype(SANS, max(9, int(title_size * 0.26)))
        d = ImageDraw.Draw(img)
        text = " ".join(sub.upper())
        tw = d.textlength(text, font=f)
        sy = h * y + title_size * 0.72
        lw = min(w * 0.3, tw * 0.5)
        d.line([(w / 2 - tw / 2 - lw - 8, sy + f.size * 0.6), (w / 2 - tw / 2 - 8, sy + f.size * 0.6)], fill=(150, 34, 30), width=max(1, f.size // 8))
        d.line([(w / 2 + tw / 2 + 8, sy + f.size * 0.6), (w / 2 + tw / 2 + lw + 8, sy + f.size * 0.6)], fill=(150, 34, 30), width=max(1, f.size // 8))
        d.text((w / 2 - tw / 2, sy), text, font=f, fill=(176, 180, 176))
    return img


def icon():
    # a single figure in the fog, the title across the top
    W = 128 * 4
    img = sky(W, W, int(W * 0.62), 11)
    img = smoke(img, int(W * 0.78), int(W * 0.58), int(W * 0.45), 4, 4)
    d = ImageDraw.Draw(img)
    tree_line(d, W, int(W * 0.48), int(W * 0.62), random.Random(12), (24, 28, 32), 1.2)
    img = fog(img, int(W * 0.55), int(W * 0.7), 13, 0.7)
    d = ImageDraw.Draw(img)
    grass(d, W, int(W * 0.7), W, random.Random(14), (9, 10, 11), 4, 220)
    walker(d, W * 0.47, W * 1.32, W * 1.12, random.Random(5), (5, 6, 7), (205, 220, 196))
    img = vignette(img, 1.1)
    arr = np.asarray(img, np.float32)
    yy = np.linspace(0, 1, W, dtype=np.float32)[:, None, None]
    arr *= 1 - np.clip((yy - 0.62) / 0.38, 0, 1)[..., :] * 0.75  # dark band for the title
    img = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))
    img = distressed_text(img, "INTO THE", SERIF, int(W * 0.13), (W / 2, W * 0.73), (226, 224, 214), 21, 0.14)
    img = distressed_text(img, "DEAD", SERIF, int(W * 0.22), (W / 2, W * 0.88), (226, 224, 214), 22, 0.16)
    return img.resize((128, 128), Image.LANCZOS)


finish(icon(), 1).save(OUT + "/icon0.png", optimize=True)
finish(scene(840, 500, 2, walkers=8), 2).save(OUT + "/bg0.png", optimize=True)
finish(titled(scene(280, 158, 3, walkers=4, smokes=(0.2,)), 30, "PS Vita", 0.36, 31), 3).save(OUT + "/startup.png", optimize=True)
finish(titled(scene(960, 544, 4, walkers=9), 96, "PS Vita Port", 0.3, 41), 4).save(OUT + "/pic0.png", optimize=True)
