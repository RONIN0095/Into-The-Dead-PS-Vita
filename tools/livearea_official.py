#!/usr/bin/env python3
"""
LiveArea pictures from the game's own artwork, taken from YOUR copy of the
APK. Nothing of the game is stored in this repository: the pictures are
made on your machine and put into a VPK you already have.

  tools/livearea_official.py <base.apk> <in.vpk> <out.vpk>

Uses the launcher icon, the red crowd background of the adaptive icon, the
store banner and the title logo from the Unity data (needs Pillow, numpy
and UnityPy).
"""
import io
import sys
import tempfile
import zipfile

import numpy as np
from PIL import Image, ImageFilter

APK, VPK_IN, VPK_OUT = sys.argv[1:4]


def apk_png(z, name):
    return Image.open(io.BytesIO(z.read(name))).convert("RGBA")


def unity_texture(z, name):
    import UnityPy
    with tempfile.TemporaryDirectory() as d:
        for n in z.namelist():
            if n.startswith("assets/bin/Data/") and not n.endswith("/") and ("data.unity3d" in n or "sharedassets" in n or n.endswith(".resS") or "resources" in n):
                z.extract(n, d)
        env = UnityPy.load(d + "/assets/bin/Data")
        for obj in env.objects:
            if obj.type.name == "Texture2D":
                t = obj.read()
                if t.m_Name == name:
                    return t.image.convert("RGBA")
    raise SystemExit("texture %s not found" % name)


def cover(img, w, h, focus_y=0.5):
    s = max(w / img.width, h / img.height)
    im = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    x = (im.width - w) // 2
    y = int((im.height - h) * focus_y)
    return im.crop((x, y, x + w, y + h))


def shade(img, top, bottom, vign):
    arr = np.asarray(img.convert("RGB"), np.float32)
    h, w = arr.shape[:2]
    y = np.linspace(0, 1, h, dtype=np.float32)[:, None]
    x = np.linspace(-1, 1, w, dtype=np.float32)[None, :]
    g = 1 - top * np.clip(1 - y * 2.5, 0, 1) - bottom * np.clip((y - 0.6) / 0.4, 0, 1)
    v = 1 - vign * np.clip(np.sqrt(x ** 2 + ((y - 0.5) * 2) ** 2) - 0.6, 0, 1)
    arr *= (g * v * 0.8)[..., None]
    rng = np.random.default_rng(1)
    arr += rng.normal(0, 2.5, (h, w))[..., None]
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))


def with_logo(bg, logo, width_frac, cy):
    logo = logo.crop(logo.split()[3].point(lambda v: 255 if v > 90 else 0).getbbox())
    lw = int(bg.width * width_frac)
    lh = round(logo.height * lw / logo.width)
    logo = logo.resize((lw, lh), Image.LANCZOS)
    # soft dark halo so the logo stands off the red
    a = logo.split()[3].point(lambda v: 255 if v > 90 else 0).filter(ImageFilter.GaussianBlur(max(1, lw // 90)))
    halo = Image.new("RGBA", logo.size, (0, 0, 0, 0))
    halo.putalpha(a.point(lambda v: int(v * 0.6)))
    out = bg.convert("RGBA")
    pos = ((bg.width - lw) // 2, int(bg.height * cy - lh / 2))
    out.alpha_composite(halo, pos)
    out.alpha_composite(logo, pos)
    return out.convert("RGB")


def png8(img):
    b = io.BytesIO()
    img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT,
                                dither=Image.Dither.FLOYDSTEINBERG).save(b, "PNG", optimize=True)
    return b.getvalue()


with zipfile.ZipFile(APK) as z:
    icon = apk_png(z, "res/mipmap-xxxhdpi-v4/app_icon.png")
    crowd = apk_png(z, "res/mipmap-xxxhdpi-v4/ic_launcher_background.png").filter(ImageFilter.GaussianBlur(1.2))
    logo = unity_texture(z, "logo_intothedead_ugui")

pics = {
    "sce_sys/icon0.png": icon.resize((128, 128), Image.LANCZOS),
    "sce_sys/pic0.png": with_logo(shade(cover(crowd, 960, 544, 0.42), 0.55, 0.75, 0.6), logo, 0.62, 0.5),
    "sce_sys/livearea/contents/bg0.png": shade(cover(crowd, 840, 500, 0.42), 0.35, 0.7, 0.5),
    "sce_sys/livearea/contents/startup.png": with_logo(shade(cover(crowd, 280, 158, 0.42), 0.5, 0.7, 0.6), logo, 0.82, 0.5),
}

with zipfile.ZipFile(VPK_IN) as zi, zipfile.ZipFile(VPK_OUT, "w", zipfile.ZIP_DEFLATED) as zo:
    for item in zi.infolist():
        data = png8(pics[item.filename]) if item.filename in pics else zi.read(item.filename)
        zo.writestr(item, data)
for k, v in pics.items():
    v.save(VPK_OUT + "." + k.split("/")[-1])  # previews next to the VPK
print("written", VPK_OUT)
