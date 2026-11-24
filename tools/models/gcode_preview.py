#!/usr/bin/env python3
# Draw the toolpaths in a G-code file the way a slicer preview does: every
# extrusion line, coloured by what it is (outer wall, wall, infill, support,
# support interface, skirt). A side view of the whole print and a top-down
# view of one layer.   gcode_preview.py file.gcode out_prefix [layer_mm]
import sys, re, numpy as np
from PIL import Image, ImageDraw
COL = {"External perimeter": (230, 120, 40), "Perimeter": (240, 190, 60), "Overhang perimeter": (40, 60, 230),
       "Internal infill": (200, 60, 60), "Solid infill": (170, 50, 170), "Top solid infill": (230, 60, 60),
       "Bridge infill": (60, 120, 230), "Gap fill": (255, 255, 255), "Skirt/Brim": (20, 160, 120),
       "Support material": (60, 170, 60), "Support material interface": (30, 110, 30)}
segs = []; x = y = z = 0.0; typ = "?"
for line in open(sys.argv[1]):
    if line.startswith(";TYPE:"): typ = line[6:].strip(); continue
    if line.startswith(";Z:"): z = float(line[3:]); continue
    if not (line.startswith("G1 ") or line.startswith("G0 ")): continue
    m = dict((k, float(v)) for k, v in re.findall(r"([XYZE])([-\d.]+)", line))
    nx, ny = m.get("X", x), m.get("Y", y)
    if m.get("E", 0) > 0 and (nx != x or ny != y): segs.append((x, y, nx, ny, z, typ))
    x, y = nx, ny
S = np.array([s[:5] for s in segs]); T = [s[5] for s in segs]
print(f"{len(S)} extrusion moves, {len(set(S[:, 4]))} layers")
# side view (3/4): screen x = x*cos + y*sin, screen y = z - depth*0.25
yaw = np.radians(35); W, H = 1400, 1400
def proj(px, py, pz):
    sx = px * np.cos(yaw) - py * np.sin(yaw); d = px * np.sin(yaw) + py * np.cos(yaw)
    return sx, pz + d * 0.25
ax, ay = proj(S[:, 0], S[:, 1], S[:, 4]); bx, by = proj(S[:, 2], S[:, 3], S[:, 4])
allx = np.concatenate([ax, bx]); ally = np.concatenate([ay, by])
span = max(np.ptp(allx), np.ptp(ally)) * 1.05; cx, cy = (allx.min() + allx.max()) / 2, (ally.min() + ally.max()) / 2
X = lambda v: (v - cx) / span * W + W / 2; Y = lambda v: H / 2 - (v - cy) / span * H
img = Image.new("RGB", (W, H), (28, 30, 36)); d = ImageDraw.Draw(img)
order = np.argsort(-(S[:, 0] * np.sin(yaw) + S[:, 1] * np.cos(yaw)))       # far first
for i in order[::1]:
    d.line([X(ax[i]), Y(ay[i]), X(bx[i]), Y(by[i])], fill=COL.get(T[i], (150, 150, 150)), width=1)
img.save(sys.argv[2] + "_toolpaths.png")
# one layer, top-down
lz = float(sys.argv[3]) if len(sys.argv) > 3 else np.median(S[:, 4])
zs = np.unique(S[:, 4]); zl = zs[np.argmin(abs(zs - lz))]; sel = np.where(S[:, 4] == zl)[0]
L = S[sel]; span = max(np.ptp(np.r_[L[:, 0], L[:, 2]]), np.ptp(np.r_[L[:, 1], L[:, 3]])) * 1.1
cx, cy = (L[:, [0, 2]].min() + L[:, [0, 2]].max()) / 2, (L[:, [1, 3]].min() + L[:, [1, 3]].max()) / 2
img = Image.new("RGB", (W, H), (28, 30, 36)); d = ImageDraw.Draw(img)
for i in sel:
    a = ((S[i, 0] - cx) / span * W + W / 2, H / 2 - (S[i, 1] - cy) / span * H); b = ((S[i, 2] - cx) / span * W + W / 2, H / 2 - (S[i, 3] - cy) / span * H)
    d.line([a, b], fill=COL.get(T[i], (150, 150, 150)), width=3)
d.text((20, 20), f"layer at z = {zl:.2f} mm", fill=(255, 255, 255))
img.save(sys.argv[2] + "_layer.png"); print("layer", zl)
