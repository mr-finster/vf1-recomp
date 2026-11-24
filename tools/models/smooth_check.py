#!/usr/bin/env python3
# Compare a statue with the untouched game geometry, by volume:
#  - EXTRA: statue material more than (thicken + tol) outside the game surface
#    (the welds/fills that join pieces, skins of broken shells)
#  - MISSING: game material the statue doesn't have
# and how spiky/sliver-ish the surface is.   smooth_check.py full.json statue.stl
import json, sys, os, numpy as np, manifold3d as m3
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hidden_joints as hj, ball_joints as bj, check_print as cp
full, stl = sys.argv[1], sys.argv[2]
parts = [json.loads(l) for l in open(full) if l.strip()]
allp = np.vstack([bj.pose(p["m"], p["p"]) for p in parts]); lo, hi = allp.min(0), allp.max(0)
sc = 180 / (hi[1] - lo[1]); ce = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])
xf = lambda V: ((np.asarray(V, float) - ce) * sc)[..., [0, 2, 1]] * np.array([1, -1, 1])
orig = m3.Manifold.batch_boolean([s for s in (hj.node_solid([p], xf, 0.8)[0] for p in parts) if s is not None and not s.is_empty()], m3.OpType.Add)
S, T = cp.load(stl)
dz = S.bounding_box()[2] - orig.bounding_box()[2] + 0.2          # statue (thickened 0.2) was dropped onto z=0
orig = orig.translate([0, 0, dz])
extra = S - orig.minkowski_sum(m3.Manifold.sphere(0.45, 12))
missing = orig - S
ex = [x for x in extra.decompose() if x.volume() > 0.05]
mi = [x for x in missing.decompose() if x.volume() > 0.05]
# is each extra region sealed inside the statue, or does it reach the outside surface?
exposed = []
for x in ex:
    out = (x.minkowski_sum(m3.Manifold.sphere(0.6, 8)) - S).volume()      # material just outside it
    exposed.append(out)
vis = [(x, e) for x, e in zip(ex, exposed) if e > 0.05]
print(f"  of which visible on the surface: {len(vis)} spot(s), {sum(x.volume() for x, _ in vis):.1f} mm³"
      + ("" if not vis else " at z=" + ", ".join(f"{(x.bounding_box()[2]+x.bounding_box()[5])/2:.0f}mm ({x.volume():.1f}mm³)" for x, _ in sorted(vis, key=lambda t: -t[0].volume())[:4]))
      + f"; sealed inside (invisible): {sum(x.volume() for x, e in zip(ex, exposed) if e <= 0.05):.1f} mm³")
# triangle quality: needles/slivers (min angle < 2 deg) and tiny edges
a = T[:, 1] - T[:, 0]; b = T[:, 2] - T[:, 0]; c = T[:, 2] - T[:, 1]
def ang(u, v): return np.degrees(np.arccos(np.clip((u * v).sum(1) / (np.linalg.norm(u, axis=1) * np.linalg.norm(v, axis=1) + 1e-12), -1, 1)))
mins = np.minimum(np.minimum(ang(a, b), ang(-a, c)), ang(-b, -c))
area = 0.5 * np.linalg.norm(np.cross(a, b), axis=1)
sliver_area = area[mins < 2].sum() / area.sum() * 100
print(f"extra beyond the game surface: {len(ex)} spot(s), {sum(x.volume() for x in ex):.1f} mm³"
      + ("" if not ex else "; biggest at z=" + ", ".join(f"{(x.bounding_box()[2]+x.bounding_box()[5])/2:.0f}mm ({x.volume():.1f}mm³)" for x in sorted(ex, key=lambda x: -x.volume())[:4])))
print(f"missing game material: {len(mi)} spot(s), {sum(x.volume() for x in mi):.2f} mm³")
print(f"surface: {len(T)} triangles; {sliver_area:.2f}% of area in sliver triangles (min angle < 2°)")
