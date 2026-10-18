#!/usr/bin/env python3
"""The app icon: "VF1" in chiselled low-poly letters on a faceted tile, drawn from scratch (no art
from the game). Writes assets/games/vf/icon/icon_<variant>.png at 1024x1024 (the macOS icon grid: an 824px
rounded tile with room for the shadow).

    tools/games/vf/release/make_icon.py [variant ...]      variants: gold, red, steel
"""
import math, os, random, sys
from PIL import Image, ImageDraw, ImageFilter

SS = 4                     # supersampling
N = 1024 * SS
OUT = os.path.join(os.path.dirname(__file__), '..', '..', '..', '..', 'assets', 'games', 'vf', 'icon')

PALETTES = {
    # tile (two corners), letter base colour, letter edge colour
    'gold':  ((18, 22, 70), (70, 20, 90), (255, 196, 40), (200, 60, 10)),
    'red':   ((10, 10, 18), (40, 40, 60), (235, 40, 40), (120, 0, 20)),
    'steel': ((15, 60, 160), (10, 20, 60), (230, 235, 245), (90, 110, 150)),
    # the game's own scheme: the title's blue, the yellow lettering with its warm red-orange edge
    'title': ((20, 70, 210), (8, 20, 90), (255, 214, 30), (210, 50, 10)),
    'title_deep': ((10, 40, 170), (4, 10, 60), (255, 220, 40), (170, 30, 0)),
}

def shade(c, k):
    return tuple(max(0, min(255, int(v * k))) for v in c)

def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

def _skew(p, s=0.22):       # italic, leaning forward
    x, y = p
    return (x + (512 - y) * s, y)

_FIT = None
def skew(p):
    """Slanted, then scaled and centred to sit inside the tile with a margin."""
    global _FIT
    if _FIT is None:
        pts = [_skew(q) for st in strokes() for q in st]
        x0 = min(x for x, _ in pts); x1 = max(x for x, _ in pts)
        y0 = min(y for _, y in pts); y1 = max(y for _, y in pts)
        k = min(640 / (x1 - x0), 420 / (y1 - y0))
        _FIT = (k, 512 - (x0 + x1) / 2 * k, 500 - (y0 + y1) / 2 * k)
    k, ox, oy = _FIT
    x, y = _skew(p)
    return (x * k + ox, y * k + oy)

def strokes():
    """The letters as quads (in a 1024 design space): each stroke is chiselled separately."""
    q = []
    # V: two arms meeting at the bottom
    q.append([(120, 300), (215, 300), (300, 640), (250, 740)])
    q.append([(380, 300), (475, 300), (350, 740), (250, 740)][::1])
    # F: stem, top arm, middle arm
    q.append([(500, 300), (585, 300), (585, 740), (500, 740)])
    q.append([(585, 300), (745, 300), (725, 375), (585, 375)])
    q.append([(585, 480), (695, 480), (675, 550), (585, 550)])
    # 1: flag, stem, foot
    q.append([(770, 360), (860, 300), (860, 380), (770, 420)])
    q.append([(860, 300), (940, 300), (940, 740), (860, 740)])
    return q

def chisel(d, quad, base, edge, light):
    """A bevelled block: the top face (two triangles) and four sloped edges, each flat-shaded."""
    cx = sum(p[0] for p in quad) / 4
    cy = sum(p[1] for p in quad) / 4
    inner = [(cx + (x - cx) * 0.62, cy + (y - cy) * 0.62) for x, y in quad]
    # the inner face shrinks toward the middle more across the stroke than along it
    faces = []
    for i in range(4):
        a, b = quad[i], quad[(i + 1) % 4]
        ia, ib = inner[i], inner[(i + 1) % 4]
        # the facet's outward direction decides its brightness
        nx, ny = (b[1] - a[1]), -(b[0] - a[0])
        ln = math.hypot(nx, ny) or 1
        lit = (nx * light[0] + ny * light[1]) / ln
        faces.append(([a, b, ib, ia], mix(shade(edge, 0.55), shade(base, 1.15), (lit + 1) / 2)))
    faces.append(([inner[0], inner[1], inner[2]], shade(base, 1.08)))
    faces.append(([inner[0], inner[2], inner[3]], shade(base, 0.92)))
    for poly, col in faces:
        d.polygon([(skew(p)[0] * SS, skew(p)[1] * SS) for p in poly], fill=col)

def tile(img, c1, c2):
    """A rounded tile of flat-shaded triangles, darker toward one corner."""
    mask = Image.new('L', (N, N), 0)
    m = 100 * SS
    ImageDraw.Draw(mask).rounded_rectangle((m, m, N - m, N - m), radius=185 * SS, fill=255)
    layer = Image.new('RGB', (N, N))
    d = ImageDraw.Draw(layer)
    rnd = random.Random(7)
    step = 128 * SS
    pts = {}
    for gy in range(0, N + step, step):
        for gx in range(0, N + step, step):
            j = 0 if gx in (0, N) or gy in (0, N) else step * 0.32
            pts[gx, gy] = (gx + rnd.uniform(-j, j), gy + rnd.uniform(-j, j))
    for gy in range(0, N, step):
        for gx in range(0, N, step):
            a, b, c, e = pts[gx, gy], pts[gx + step, gy], pts[gx + step, gy + step], pts[gx, gy + step]
            for tri in ((a, b, c), (a, c, e)):
                cx = sum(p[0] for p in tri) / 3 / N
                cy = sum(p[1] for p in tri) / 3 / N
                col = mix(c1, c2, min(1, max(0, (cx + cy) / 2 + rnd.uniform(-0.08, 0.08))))
                d.polygon(tri, fill=shade(col, rnd.uniform(0.88, 1.12)))
    # a soft drop shadow under the tile
    shadow = Image.new('L', (N, N), 0)
    ImageDraw.Draw(shadow).rounded_rectangle((m, m + 14 * SS, N - m, N - m + 14 * SS), radius=185 * SS, fill=110)
    shadow = shadow.filter(ImageFilter.GaussianBlur(18 * SS))
    img.paste((0, 0, 0, 255), (0, 0), shadow)
    img.paste(layer, (0, 0), mask)

def make(name):
    c1, c2, base, edge = PALETTES[name]
    img = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    tile(img, c1, c2)
    # the letters, with a dark outline (the same shapes, grown) for contrast on the tile
    d = ImageDraw.Draw(img)
    for q in strokes():
        cx = sum(p[0] for p in q) / 4; cy = sum(p[1] for p in q) / 4
        grown = [(cx + (x - cx) * 1.0 + (12 if x > cx else -12), cy + (y - cy) * 1.0 + (12 if y > cy else -12)) for x, y in q]
        d.polygon([(skew(p)[0] * SS, (skew(p)[1] + 10) * SS) for p in grown], fill=(0, 0, 0, 170))
    for q in strokes():
        chisel(d, q, base, edge, (-0.55, -0.83))
    img = img.resize((1024, 1024), Image.LANCZOS)
    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, f'icon_{name}.png'))
    return img

if __name__ == '__main__':
    names = sys.argv[1:] or list(PALETTES)
    ims = [make(n) for n in names]
    sheet = Image.new('RGB', (360 * len(ims) + 20, 400), (236, 236, 240))
    for i, im in enumerate(ims):
        sheet.paste(im.resize((340, 340), Image.LANCZOS), (20 + i * 360, 30), im.resize((340, 340), Image.LANCZOS))
    sheet.save(os.path.join(OUT, 'choices.png'))
