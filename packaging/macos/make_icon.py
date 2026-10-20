#!/usr/bin/env python3
"""Draws the Kiln app icon: a brick beehive kiln with a glowing firing chamber and square
"pixel" embers rising from it. Writes AppIcon.png (1024 px) and AppIcon.icns next to this file.

    pip install pillow && python3 packaging/macos/make_icon.py
"""
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

S = 2048  # drawn at 2x, then downsampled for clean edges
OUT = Path(__file__).resolve().parent


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def vgrad(w, h, c0, c1):
    g = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(g)
    for y in range(h):
        d.line([(0, y), (w, y)], fill=lerp(c0, c1, y / (h - 1)) + (255,))
    return g


def radial(w, h, cx, cy, r, stops):
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            t = min(1.0, math.hypot(x - cx, y - cy) / r)
            for i in range(len(stops) - 1):
                if stops[i][0] <= t <= stops[i + 1][0]:
                    u = (t - stops[i][0]) / max(stops[i + 1][0] - stops[i][0], 1e-6)
                    px[x, y] = lerp(stops[i][1], stops[i + 1][1], u)
                    break
    return img


def main():
    margin, corner = 200, 370  # macOS icon grid: 824/1024 rounded square
    squircle = Image.new("L", (S, S), 0)
    ImageDraw.Draw(squircle).rounded_rectangle((margin, margin, S - margin, S - margin), corner, fill=255)
    bg = vgrad(S, S, (36, 26, 40), (16, 12, 20))

    # warm floor glow behind the kiln
    q = S // 4
    glow = radial(q, q, q // 2, q // 2 * 1.25, q // 2, [(0, (255, 120, 60, 150)), (1, (255, 80, 60, 0))])
    bg.alpha_composite(glow.resize((S, S), Image.BILINEAR))

    # beehive dome with brick courses
    cx, base_y, dome_w, dome_h = S // 2, 1520, 1180, 1050
    dome = Image.new("L", (S, S), 0)
    dd = ImageDraw.Draw(dome)
    dd.ellipse((cx - dome_w // 2, base_y - dome_h, cx + dome_w // 2, base_y + dome_h), fill=255)
    dd.rectangle((0, base_y, S, S), fill=0)
    clay = vgrad(S, S, (214, 112, 66), (120, 48, 34))
    cd = ImageDraw.Draw(clay)
    rows = 9
    for i in range(1, rows):
        y = base_y - i * dome_h / rows
        half = dome_w / 2 * math.sqrt(max(0.0, 1 - ((base_y - y) / dome_h) ** 2))
        cd.line([(cx - half, y), (cx + half, y)], fill=(90, 34, 26, 255), width=10)
        offset = 0 if i % 2 else 70
        for x in range(int(cx - half) + offset, int(cx + half), 140):
            cd.line([(x, y), (x, y + dome_h / rows)], fill=(100, 38, 28, 255), width=8)
    shade = radial(q, q, q // 2 - 40, q // 2 * 0.55, q // 2 * 1.3,
                   [(0, (255, 220, 180, 70)), (0.5, (0, 0, 0, 0)), (1, (0, 0, 0, 110))])
    clay.alpha_composite(shade.resize((S, S), Image.BILINEAR))
    bg.paste(clay, (0, 0), dome)
    ImageDraw.Draw(bg).rounded_rectangle((cx - dome_w // 2 - 60, base_y - 10, cx + dome_w // 2 + 60, base_y + 70),
                                         28, fill=(70, 30, 26, 255))

    # arched firing chamber: gold core -> orange -> magenta rim, with bloom
    aw, ah = 520, 560
    ax0, ay1 = cx - aw // 2, base_y - 10
    arch = Image.new("L", (S, S), 0)
    ad = ImageDraw.Draw(arch)
    ad.rectangle((ax0, ay1 - ah + aw // 2, ax0 + aw, ay1), fill=255)
    ad.ellipse((ax0, ay1 - ah, ax0 + aw, ay1 - ah + aw), fill=255)
    fire = radial(q, q, cx // 4, (ay1 - 120) // 4, 170,
                  [(0, (255, 248, 200, 255)), (0.35, (255, 190, 70, 255)), (0.7, (255, 95, 80, 255)),
                   (1, (200, 40, 140, 255))]).resize((S, S), Image.BILINEAR)
    bloom = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    bloom.paste(fire, (0, 0), arch)
    bg.alpha_composite(bloom.filter(ImageFilter.GaussianBlur(60)))
    bg.paste(fire, (0, 0), arch)
    rim = ImageChops.subtract(arch.filter(ImageFilter.MaxFilter(31)), arch)
    bg.paste((70, 26, 22, 255), (0, 0), rim)

    # square pixel embers rising off the top of the dome
    apex = base_y - dome_h  # 470
    embers = [  # x, y, half-size, colour
        (cx - 30, apex - 40, 38, (255, 200, 90)),
        (cx + 90, apex - 105, 32, (255, 140, 80)),
        (cx - 115, apex - 140, 26, (255, 110, 150)),
        (cx + 25, apex - 185, 22, (255, 215, 120)),
        (cx + 160, apex - 200, 18, (255, 120, 170)),
    ]
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    for x, y, s, c in embers:
        ld.rectangle((x - s, y - s, x + s, y + s), fill=c + (255,))
    bg.alpha_composite(layer.filter(ImageFilter.GaussianBlur(28)))
    bg.alpha_composite(layer)

    icon = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    icon.paste(bg, (0, 0), squircle)
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 110), (0, 24), squircle)
    final = Image.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(30)), icon)
    final = final.resize((1024, 1024), Image.LANCZOS)
    final.save(OUT / "AppIcon.png")
    final.save(OUT / "AppIcon.icns")
    print("wrote", OUT / "AppIcon.png", "and", OUT / "AppIcon.icns")


if __name__ == "__main__":
    main()
