#!/usr/bin/env python3
"""The cabinet's marquee for the download page: the app icon's chiselled "VF1" (tools/games/vf/release/make_icon.py, the
'title' palette) on a wide faceted blue panel. Nothing from the game.

    tools/games/vf/site/marquee.py OUT.png
"""
import os, random, sys
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'release'))
import make_icon as mi

W, H = 1440, 312          # 360 x 78 on the page, at 4x


def panel(c1, c2):
    img = Image.new('RGB', (W, H))
    d = ImageDraw.Draw(img)
    rnd = random.Random(11)
    step = 104
    pts = {}
    for gy in range(0, H + step, step):
        for gx in range(0, W + step, step):
            j = 0 if gx <= 0 or gy <= 0 or gx >= W or gy >= H else step * 0.32
            pts[gx, gy] = (gx + rnd.uniform(-j, j), gy + rnd.uniform(-j, j))
    for gy in range(0, H + step - 1, step):
        for gx in range(0, W + step - 1, step):
            if (gx + step, gy + step) not in pts: continue
            a, b, c, e = pts[gx, gy], pts[gx + step, gy], pts[gx + step, gy + step], pts[gx, gy + step]
            for tri in ((a, b, c), (a, c, e)):
                cx = sum(p[0] for p in tri) / 3 / W
                cy = sum(p[1] for p in tri) / 3 / H
                # lit from the middle, like a backlit marquee
                t = min(1, max(0, abs(cx - 0.5) * 1.3 + cy * 0.35 + rnd.uniform(-0.08, 0.08)))
                d.polygon(tri, fill=mi.shade(mi.mix(c1, c2, t), rnd.uniform(0.9, 1.1)))
    return img


def letters(base, edge):
    img = Image.new('RGBA', (mi.N, mi.N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for q in mi.strokes():
        cx = sum(p[0] for p in q) / 4; cy = sum(p[1] for p in q) / 4
        grown = [(x + (12 if x > cx else -12), y + (12 if y > cy else -12)) for x, y in q]
        d.polygon([(mi.skew(p)[0] * mi.SS, (mi.skew(p)[1] + 10) * mi.SS) for p in grown], fill=(0, 0, 0, 170))
    for q in mi.strokes():
        mi.chisel(d, q, base, edge, (-0.55, -0.83))
    img = img.resize((1024, 1024), Image.LANCZOS)
    return img.crop(img.getbbox())


def main(out):
    c1, c2, base, edge = mi.PALETTES['title']
    bg = panel(c1, c2)
    lt = letters(base, edge)
    h = int(H * 0.8)
    lt = lt.resize((int(lt.width * h / lt.height), h), Image.LANCZOS)
    bg.paste(lt, ((W - lt.width) // 2, (H - h) // 2 + 4), lt)
    bg.save(out, optimize=True)


if __name__ == '__main__':
    main(sys.argv[1])
