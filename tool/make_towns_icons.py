#!/usr/bin/env python3
"""Launcher icon for Retro-Towns, cut programmatically.

Nothing here is a logo: the app's mark is a CD -- the thing the machine is
remembered for -- with the three-band sheen a real disc throws. It is drawn at
the densities Android wants and the adaptive-icon sizes, which is what the
mipmap folders have to hold or the build fails on a missing resource.
"""

import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(HERE, "android", "app", "src", "main", "res")

BG = (16, 20, 30, 255)
DENSITY = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
ADAPTIVE = {"mdpi": 108, "hdpi": 162, "xhdpi": 216, "xxhdpi": 324, "xxxhdpi": 432}


def disc(size, scale=1.0):
    """A CD, drawn into a `size x size` transparent square.

    `scale` exists because the adaptive foreground has to keep its artwork
    inside the safe zone -- Android crops the outer 25% on a masked icon.
    """
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = int(size * 0.30 * scale)
    cx = cy = size // 2
    # The sheen: three arcs of the rainbow a disc actually throws, laid over
    # the silver.
    silver = (196, 206, 216, 255)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=silver)
    inner = int(r * 0.62)
    d.ellipse([cx - inner, cy - inner, cx + inner, cy + inner],
              fill=(150, 162, 176, 255))
    for i, col in enumerate(((255, 96, 120), (255, 198, 92), (86, 220, 196))):
        w = max(2, int(size * 0.012))
        rr = int(r * (0.74 + 0.07 * i)) * 2
        d.arc([cx - rr // 2, cy - rr // 2, cx + rr // 2, cy + rr // 2],
              start=210 + 20 * i, end=40 + 20 * i,
              fill=col + (200,), width=w)
    # The clamp hole, and the rim a CD has.
    hole = int(r * 0.26)
    d.ellipse([cx - hole, cy - hole, cx + hole, cy + hole], fill=BG)
    d.ellipse([cx - r, cy - r, cx + r, cy + r],
              outline=(120, 132, 148, 255), width=max(2, int(size * 0.012)))
    return img


def square(size, scale=1.0):
    """A flat launcher icon: the disc on a rounded plate."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    pad = int(size * 0.06)
    rad = int(size * 0.22)
    plate = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(plate).rounded_rectangle(
        [pad, pad, size - pad, size - pad], rad, fill=BG)
    img = Image.alpha_composite(img, plate)
    return Image.alpha_composite(img, disc(size, scale))


os.makedirs(RES, exist_ok=True)
for name, px in DENSITY.items():
    out = os.path.join(RES, f"mipmap-{name}")
    os.makedirs(out, exist_ok=True)
    square(px).save(os.path.join(out, "ic_launcher.png"))
    square(px).save(os.path.join(out, "ic_launcher_round.png"))
for name, px in ADAPTIVE.items():
    out = os.path.join(RES, f"mipmap-{name}")
    os.makedirs(out, exist_ok=True)
    bg = Image.new("RGBA", (px, px), BG)
    bg.save(os.path.join(out, "ic_launcher_background.png"))
    fg = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    fg = Image.alpha_composite(fg, disc(px, 0.66))
    fg.save(os.path.join(out, "ic_launcher_foreground.png"))

anydpi = os.path.join(RES, "mipmap-anydpi-v26")
os.makedirs(anydpi, exist_ok=True)
xml = """<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@mipmap/ic_launcher_background" />
    <foreground android:drawable="@mipmap/ic_launcher_foreground" />
</adaptive-icon>
"""
for leaf in ("ic_launcher.xml", "ic_launcher_round.xml"):
    with open(os.path.join(anydpi, leaf), "w") as f:
        f.write(xml)

print("wrote launcher icons to", RES)
