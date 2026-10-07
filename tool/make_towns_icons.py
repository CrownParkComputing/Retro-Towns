#!/usr/bin/env python3
"""Launcher icon for Retro-Towns, in the family's three-part style: the Retro
script cut from the shared logo, TOWNS beneath it, and the machine's own mark
below that.  The mark here is a front-facing FM Towns tower - floppy bay over a
CD bay, a power LED and a button, and vents - drawn from numbers in the family's
teal, owing nothing to Fujitsu's own emblem.

    python3 tool/make_towns_icons.py

Run from anywhere. Overwrites the Android mipmaps and the 1024 master.
"""

from __future__ import annotations

import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGO = "/home/jon/StudioProjects/Retro-3DO/assets/branding/retro_recomp_logo.png"
FONT_CANDIDATES = [
    "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/Library/Fonts/Arial Bold.ttf",
]
FONT = next((f for f in FONT_CANDIDATES if os.path.exists(f)), None)
if FONT is None:
    raise SystemExit("no usable bold sans font found")

SIZE = 1024

BG_TOP = (8, 16, 22)
BG_BOTTOM = (3, 5, 9)

CHROME = [
    (232, 255, 252),
    (150, 240, 228),
    (86, 220, 196),
    (26, 120, 112),
    (120, 200, 190),
]

TEAL = (86, 220, 196)
YELLOW = (255, 198, 92)
PINK = (255, 96, 120)
INK = (10, 22, 32)


def vertical_gradient(size, colours):
    width, height = size
    grad = Image.new("RGB", (1, height))
    pixels = grad.load()
    steps = len(colours) - 1
    for y in range(height):
        position = y / max(1, height - 1) * steps
        index = min(int(position), steps - 1)
        blend = position - index
        start, end = colours[index], colours[index + 1]
        pixels[0, y] = tuple(
            int(start[c] + (end[c] - start[c]) * blend) for c in range(3)
        )
    return grad.resize((width, height))


def background():
    canvas = vertical_gradient((SIZE, SIZE), [BG_TOP, BG_BOTTOM]).convert("RGBA")
    glow = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(glow)
    draw.ellipse((60, 250, SIZE - 60, SIZE - 120), fill=(20, 90, 90, 110))
    draw.ellipse((200, 520, SIZE - 200, SIZE - 60), fill=(40, 140, 130, 95))
    glow = glow.filter(ImageFilter.GaussianBlur(120))
    return Image.alpha_composite(canvas, glow)


def retro_script(width):
    logo = Image.open(LOGO).convert("RGBA")
    script = logo.crop((168, 0, 578, 92))
    pixels = script.load()
    for y in range(script.height):
        for x in range(script.width):
            r, g, b, a = pixels[x, y]
            if a and b > r:
                pixels[x, y] = (r, g, b, 0)
    height = round(script.height * width / script.width)
    return script.resize((width, height), Image.LANCZOS)


def chrome_text(text, width, height):
    size = 10
    font = ImageFont.truetype(FONT, size)
    while True:
        probe = ImageFont.truetype(FONT, size + 4)
        box = probe.getbbox(text)
        if box[2] - box[0] > width or box[3] - box[1] > height:
            break
        size += 4
        font = probe

    box = font.getbbox(text)
    pad = 18
    layer = Image.new("RGBA", (box[2] - box[0] + pad * 2, box[3] - box[1] + pad * 2))
    ImageDraw.Draw(layer).text(
        (pad - box[0], pad - box[1]), text, font=font, fill=(255, 255, 255, 255)
    )
    mask = layer.split()[3]
    fill = vertical_gradient(layer.size, CHROME).convert("RGBA")
    fill.putalpha(mask)
    outline = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    outline.paste((12, 20, 48, 255), (0, 0), mask.filter(ImageFilter.MaxFilter(9)))
    return Image.alpha_composite(outline, fill)


def towns_tower(width):
    scale = 4
    w = width * scale
    layer = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    lw = max(5, w // 26)

    pad = w * 0.10
    x0, y0 = pad, w * 0.10
    x1, y1 = w - pad, w - pad

    d.rounded_rectangle([x0, y0, x1, y1], radius=w * 0.06,
                        outline=TEAL + (255,), width=lw)

    fy = y0 + (y1 - y0) * 0.16
    sh = (y1 - y0) * 0.035
    d.rounded_rectangle([x0 + (x1 - x0) * 0.18, fy - sh,
                         x1 - (x1 - x0) * 0.18, fy + sh],
                        radius=lw, outline=TEAL + (255,), width=lw)
    d.line([x0 + (x1 - x0) * 0.22, fy, x1 - (x1 - x0) * 0.22, fy],
           fill=TEAL + (255,), width=lw)

    cy = y0 + (y1 - y0) * 0.38
    dh = (y1 - y0) * 0.09
    d.rounded_rectangle([x0 + (x1 - x0) * 0.18, cy - dh,
                         x1 - (x1 - x0) * 0.18, cy + dh],
                        radius=lw, outline=TEAL + (255,), width=lw)
    ty = cy + dh
    d.line([x0 + (x1 - x0) * 0.22, ty, x1 - (x1 - x0) * 0.22, ty],
           fill=TEAL + (255,), width=lw)

    led = x0 + (x1 - x0) * 0.24
    d.ellipse([led, y0 + (y1 - y0) * 0.52, led + w * 0.055,
               y0 + (y1 - y0) * 0.52 + w * 0.055], fill=PINK + (255,))
    d.rounded_rectangle([x0 + (x1 - x0) * 0.62, y0 + (y1 - y0) * 0.51,
                         x0 + (x1 - x0) * 0.76, y0 + (y1 - y0) * 0.55],
                        radius=lw, fill=YELLOW + (255,))

    for i in range(3):
        vy = y0 + (y1 - y0) * (0.66 + 0.07 * i)
        d.line([x0 + (x1 - x0) * 0.20, vy, x1 - (x1 - x0) * 0.20, vy],
               fill=TEAL + (255,), width=lw)

    shape = layer.split()[3]
    rim = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    rim.paste(INK + (255,), (0, 0), shape.filter(ImageFilter.MaxFilter((lw + 6) | 1)))
    stamped = Image.alpha_composite(rim, layer)
    return stamped.resize((width, width), Image.LANCZOS)


def artwork(width):
    layer = Image.new("RGBA", (width, width), (0, 0, 0, 0))
    script = retro_script(round(width * 0.78))
    name = chrome_text("TOWNS", round(width * 0.66), round(width * 0.17))
    mark = towns_tower(round(width * 0.40))
    stack = script.height + name.height + mark.height + round(width * 0.05)
    top = max(0, (width - stack) // 2)
    layer.alpha_composite(script, ((width - script.width) // 2, top))
    top += script.height + round(width * 0.015)
    layer.alpha_composite(name, ((width - name.width) // 2, top))
    top += name.height + round(width * 0.030)
    layer.alpha_composite(mark, ((width - mark.width) // 2, top))
    return layer


def master():
    canvas = background()
    art = artwork(round(SIZE * 0.86))
    canvas.alpha_composite(art, ((SIZE - art.width) // 2, (SIZE - art.width) // 2))
    return canvas


def foreground():
    layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    art = artwork(round(SIZE * 0.62))
    layer.alpha_composite(art, ((SIZE - art.width) // 2, (SIZE - art.width) // 2))
    return layer


def rounded(image, radius_fraction=0.5):
    mask = Image.new("L", image.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, image.width - 1, image.height - 1),
        radius=round(image.width * radius_fraction), fill=255)
    out = image.copy()
    out.putalpha(mask)
    return out


def main():
    if not os.path.exists(LOGO):
        raise SystemExit("no shared Retro logo at %s" % LOGO)

    brand = os.path.join(HERE, "assets", "branding")
    os.makedirs(brand, exist_ok=True)
    icon = master()
    icon.save(os.path.join(brand, "retrotowns-icon-1024.png"))

    res = os.path.join(HERE, "android", "app", "src", "main", "res")
    legacy = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
    layers = {"mdpi": 108, "hdpi": 162, "xhdpi": 216, "xxhdpi": 324, "xxxhdpi": 432}

    for density, px in legacy.items():
        folder = os.path.join(res, "mipmap-" + density)
        os.makedirs(folder, exist_ok=True)
        icon.resize((px, px), Image.LANCZOS).save(
            os.path.join(folder, "ic_launcher.png"))
        rounded(icon.resize((px, px), Image.LANCZOS)).save(
            os.path.join(folder, "ic_launcher_round.png"))
        bg = Image.new("RGBA", (layers[density],) * 2, BG_TOP + (255,))
        bg.save(os.path.join(folder, "ic_launcher_background.png"))
        fore = foreground().resize((layers[density],) * 2, Image.LANCZOS)
        fore.save(os.path.join(folder, "ic_launcher_foreground.png"))

    print("wrote Retro-Towns launcher icons to", res)


if __name__ == "__main__":
    main()
