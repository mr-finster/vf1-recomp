#!/usr/bin/env python3
# "How it would look printed": studio render of a statue with FDM layer lines
# shaded per pixel at the real layer height (grooves strongest on walls, a
# little filament banding per layer) and rough support scars on the faces the
# supports touch. Matte PLA.   render_print.py statue.stl out.png [layer] [yaw]
import sys, numpy as np
from PIL import Image, ImageFilter
sys.path.insert(0, __import__("os").path.dirname(__file__))
import check_print as cp
def render(T, out, layer=0.12, yaw=0.55, pitch=0.12, res=1000, ss=3, material=(0.70, 0.69, 0.66), frame=None, support_angle=55.0):
    T = np.asarray(T, float)
    cy, sy, cpi, spi = np.cos(yaw), np.sin(yaw), np.cos(pitch), np.sin(pitch)
    R = np.array([[cy, -sy, 0], [spi * sy, spi * cy, cpi], [cpi * sy, cpi * cy, -spi]])
    C = T.reshape(-1, 3) @ R.T
    W = H = res * ss
    if frame is None:
        span = max(np.ptp(C[:, 0]), np.ptp(C[:, 1])) * 1.2; cx = (C[:, 0].min() + C[:, 0].max()) / 2; cyy = (C[:, 1].min() + C[:, 1].max()) / 2 - 0.03 * span
    else: span, cx, cyy = frame
    C = C.reshape(-1, 3, 3)
    X = (C[..., 0] - cx) / span * W + W / 2; Y = H / 2 - (C[..., 1] - cyy) / span * H; D = C[..., 2]
    key = np.array([-0.55, -0.65, 0.75]); fill = np.array([0.7, -0.3, 0.35]); ly = yaw - 0.55
    Rz = np.array([[np.cos(ly), np.sin(ly), 0], [-np.sin(ly), np.cos(ly), 0], [0, 0, 1.0]])
    key = Rz @ (key / np.linalg.norm(key)); fill = Rz @ (fill / np.linalg.norm(fill)); view = -R[2]
    g = np.linspace(0, 1, H)[:, None, None]
    img = (np.array((238, 236, 232)) * (1 - g) + np.array((196, 192, 186)) * g) * np.ones((H, W, 3))
    zb = np.full((H, W), np.inf); mat = np.array(material)
    rng = np.random.default_rng(7); band = 1 + 0.025 * rng.standard_normal(4000)     # per-layer filament banding
    # contact shadow
    from PIL import ImageDraw
    sh = Image.new("L", (W, H), 0); dsh = ImageDraw.Draw(sh)
    F = T.copy(); F[..., 2] = 0; Cf = F.reshape(-1, 3) @ R.T; Cf = Cf.reshape(-1, 3, 3)
    for t in Cf:
        dsh.polygon([((t[k, 0] - cx) / span * W + W / 2, H / 2 - (t[k, 1] - cyy) / span * H) for k in range(3)], fill=255)
    m = np.asarray(sh.filter(ImageFilter.GaussianBlur(ss * 10)), float)[..., None] / 255 * 0.35
    img *= (1 - m)
    for t, x, y, d in zip(T, X, Y, D):
        n = np.cross(t[1] - t[0], t[2] - t[0]); L = np.linalg.norm(n)
        if L == 0: continue
        n /= L
        if n @ view < 0: n = -n
        minx, maxx = int(max(0, x.min())), int(min(W - 1, x.max() + 1)); miny, maxy = int(max(0, y.min())), int(min(H - 1, y.max() + 1))
        if maxx < minx or maxy < miny: continue
        den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
        if abs(den) < 1e-12: continue
        ys, xs = np.mgrid[miny:maxy + 1, minx:maxx + 1] + 0.5
        a = ((y[1] - y[2]) * (xs - x[2]) + (x[2] - x[1]) * (ys - y[2])) / den
        b = ((y[2] - y[0]) * (xs - x[2]) + (x[0] - x[2]) * (ys - y[2])) / den
        c = 1 - a - b; inside = (a >= 0) & (b >= 0) & (c >= 0)
        z = a * d[0] + b * d[1] + c * d[2]
        sub = zb[miny:maxy + 1, minx:maxx + 1]; dr = inside & (z < sub)
        if not dr.any(): continue
        sub[dr] = z[dr]
        wz = a * t[0, 2] + b * t[1, 2] + c * t[2, 2]                                  # world height of each pixel
        k = wz / layer; fr = k - np.floor(k); li = np.floor(k).astype(int) % 4000
        wall = 1 - abs(n[2])                                                          # lines show most on walls
        groove = 1 - (0.10 + 0.22 * wall) * np.exp(-((np.minimum(fr, 1 - fr)) / 0.16) ** 2)
        diff = 0.24 + 0.72 * max(0, n @ key) + 0.26 * max(0, n @ fill)
        hv = key + view; hv /= np.linalg.norm(hv); spec = 0.06 * max(0, n @ hv) ** 12            # matte: a soft sheen
        inten = diff * groove * band[li]
        if n[2] < -np.cos(np.radians(90 - support_angle)) and t[:, 2].mean() > 4.5:          # a support scar
            inten = inten * (0.86 + 0.12 * rng.random(inten.shape))
        col = mat[None, None, :] * inten[..., None] + spec
        img[miny:maxy + 1, minx:maxx + 1][dr] = (np.clip(col, 0, 1) * 255)[dr]
    Image.fromarray(img.clip(0, 255).astype(np.uint8)).resize((res, res), Image.LANCZOS).save(out)
if __name__ == "__main__":
    M, T = cp.load(sys.argv[1]); layer = float(sys.argv[3]) if len(sys.argv) > 3 else 0.12
    render(T, sys.argv[2], layer=layer, yaw=float(sys.argv[4]) if len(sys.argv) > 4 else 0.55); print("->", sys.argv[2])
