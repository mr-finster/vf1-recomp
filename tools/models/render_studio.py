# Studio render: one material, key/fill/rim light, 3x supersampled, soft ground shadow.
import numpy as np
from PIL import Image, ImageFilter

def _raster(tris, cols, W, H, proj, zbuf, img):
    for tri, col in zip(tris, cols):
        X, Y, d = proj(tri)
        minx, maxx = int(max(0, X.min())), int(min(W - 1, X.max() + 1))
        miny, maxy = int(max(0, Y.min())), int(min(H - 1, Y.max() + 1))
        if maxx < minx or maxy < miny: continue
        x0, y0, x1, y1, x2, y2 = X[0], Y[0], X[1], Y[1], X[2], Y[2]
        den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(den) < 1e-12: continue
        ys, xs = np.mgrid[miny:maxy + 1, minx:maxx + 1] + 0.5
        a = ((y1 - y2) * (xs - x2) + (x2 - x1) * (ys - y2)) / den
        b = ((y2 - y0) * (xs - x2) + (x0 - x2) * (ys - y2)) / den
        m = (a >= 0) & (b >= 0) & (a + b <= 1)
        z = a * d[0] + b * d[1] + (1 - a - b) * d[2]
        sub = zbuf[miny:maxy + 1, minx:maxx + 1]
        dr = m & (z < sub); sub[dr] = z[dr]
        if img is not None: img[miny:maxy + 1, minx:maxx + 1][dr] = col

def render(tris, out, yaw=0.5, pitch=0.18, res=1100, material=(0.62, 0.64, 0.68),
           ss=3, frame=None, bg_top=(236, 238, 242), bg_bot=(196, 200, 208), colors=None):
    tris = np.asarray(tris, float)                      # (N,3,3) mm, Z up
    cy, sy, cp, sp = np.cos(yaw), np.sin(yaw), np.cos(pitch), np.sin(pitch)
    R = np.array([[cy, -sy, 0], [sp * sy, sp * cy, cp], [cp * sy, cp * cy, -sp]])  # rows: right, up, depth(into)
    def cam(P): return P @ R.T
    C = cam(tris.reshape(-1, 3))
    W = H = res * ss
    if frame is None:
        span = max(np.ptp(C[:, 0]), np.ptp(C[:, 1])) * 1.25
        cx, cyy = (C[:, 0].min() + C[:, 0].max()) / 2, (C[:, 1].min() + C[:, 1].max()) / 2 - 0.04 * span
    else: span, cx, cyy = frame
    def proj(tri):
        c = cam(tri)
        return (c[:, 0] - cx) / span * W + W / 2, H / 2 - (c[:, 1] - cyy) / span * H, c[:, 2]
    # lights (world space, Z up)
    key = np.array([-0.55, -0.65, 0.75]); fill = np.array([0.7, -0.3, 0.35]); rim = np.array([0.2, 0.9, 0.5])
    ly = yaw - 0.55                                     # lights follow the camera (hero yaw = 0.55)
    Rz = np.array([[np.cos(ly), np.sin(ly), 0], [-np.sin(ly), np.cos(ly), 0], [0, 0, 1.0]])
    key, fill, rim = (Rz @ (v / np.linalg.norm(v)) for v in (key, fill, rim))
    view = R[2] * -1                                     # toward the camera
    mat = np.array(material)
    cols = []
    for ti, tri in enumerate(tris):
        if colors is not None: mat = np.asarray(colors[ti], float)
        n = np.cross(tri[1] - tri[0], tri[2] - tri[0]); L = np.linalg.norm(n)
        n = n / L if L else np.array([0, 0, 1.0])
        if np.dot(n, view) < 0: n = -n                   # two-sided
        diff = 0.22 + 0.78 * max(0, n @ key) + 0.28 * max(0, n @ fill)
        h = key + view; h /= np.linalg.norm(h); spec = 0.18 * max(0, n @ h) ** 24
        rimv = 0.25 * (1 - max(0, n @ view)) ** 3
        cols.append(np.clip(255 * (mat * diff + spec + rimv), 0, 255))
    # background gradient
    g = np.linspace(0, 1, H)[:, None, None]
    img = (np.array(bg_top) * (1 - g) + np.array(bg_bot) * g) * np.ones((H, W, 3))
    # soft contact shadow: project the figure straight down onto z=0
    flat = tris.copy(); flat[..., 2] = 0
    lean = np.array([0.18, 0.12])                        # light from above, slightly offset
    flat[..., 0] += tris[..., 2] * lean[0] * 0.0; flat[..., 1] += tris[..., 2] * lean[1] * 0.0
    sh = np.full((H, W), np.inf); _raster(flat, [0] * len(flat), W, H, proj, sh, None)
    mask = Image.fromarray((np.isfinite(sh) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(ss * 9))
    m = np.asarray(mask, float)[..., None] / 255 * 0.38
    img = img * (1 - m)
    zb = np.full((H, W), np.inf); _raster(tris, cols, W, H, proj, zb, img)
    Image.fromarray(img.clip(0, 255).astype(np.uint8)).resize((res, res), Image.LANCZOS).save(out)
    print("->", out)
