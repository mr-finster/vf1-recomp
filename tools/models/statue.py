#!/usr/bin/env python3
# The character exactly as the game draws it, as ONE solid statue for printing.
# Input: a full extraction (full_extract.py). Every part (body pieces, hair
# strands, braids, ribbons) becomes a solid, all are merged into one (overlaps
# fuse, so there are no joins), pieces that only touch are welded, and the
# whole is thickened slightly so the thinnest features print on a 0.4 mm nozzle.
#   statue.py full.json OUTDIR [--height-mm 180] [--thicken-mm 0.2] [--base-mm 3]
# Writes statue.stl and statue_on_base.stl (a round base under the feet).
import argparse, json, os, sys
import numpy as np, manifold3d as m3
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hidden_joints as hj, ball_joints as bj

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("full"); ap.add_argument("outdir")
    ap.add_argument("--height-mm", type=float, default=180.0)
    ap.add_argument("--thicken-mm", type=float, default=0.2)
    ap.add_argument("--base-mm", type=float, default=3.0)
    a = ap.parse_args(); os.makedirs(a.outdir, exist_ok=True)
    parts = [json.loads(l) for l in open(a.full) if l.strip()]
    allp = np.vstack([bj.pose(p["m"], p["p"]) for p in parts]); lo, hi = allp.min(0), allp.max(0)
    scale = a.height_mm / (hi[1] - lo[1]); center = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])
    def xf(V):
        Q = (np.asarray(V, float) - center) * scale
        return Q[..., [0, 2, 1]] * np.array([1, -1, 1])
    solids = []; skipped = []
    for p in parts:
        s, _ = hj.node_solid([p], xf, 0.8)
        if s is None or s.is_empty(): skipped.append(p["tag"]); continue
        if a.thicken_mm > 0: s = s.minkowski_sum(m3.Manifold.sphere(a.thicken_mm, 12))
        solids.append(s)
    body = m3.Manifold.batch_boolean(solids, m3.OpType.Add)
    # weld touching pieces, tie floating ones; a piece far from the body is a
    # wrong extraction, not something to tie on: fail instead
    loose = [x for x in hj.roundtrip(body).decompose() if x.volume() >= 0.3]
    if len(loose) > 1:
        main = max(loose, key=lambda x: x.volume())
        far = [x for x in loose if x is not main and main.min_gap(x, 10.0) >= 5.0]
        if far:
            print(f"FAIL: {len(far)} piece(s) more than 5 mm from the body (wrong extraction?)"); sys.exit(1)
    body = hj.bridge_shells(body, min_vol=0.3)
    body = body.translate([0, 0, -body.bounding_box()[2]])
    R = hj.roundtrip(body); pieces = [x for x in R.decompose() if x.volume() > 1e-6]
    hj.bj.write_stl(os.path.join(a.outdir, "statue.stl"), [hj.mesh_of(R)])
    # a round base under the feet, the feet sunk 0.6 mm into it
    V = np.asarray(R.to_mesh().vert_properties)[:, :3]; foot = V[V[:, 2] < 2.0]
    c = foot[:, :2].mean(0); r = float(np.linalg.norm(foot[:, :2] - c, axis=1).max()) + 8.0
    base = m3.Manifold.cylinder(a.base_mm + 0.6, r, r, 96).translate([c[0], c[1], -a.base_mm])
    on = (R + base).translate([0, 0, a.base_mm])
    hj.bj.write_stl(os.path.join(a.outdir, "statue_on_base.stl"), [hj.mesh_of(hj.roundtrip(on))])
    bb = R.bounding_box()
    print(f"{len(parts)} parts merged{' (skipped ' + ','.join(skipped) + ')' if skipped else ''}; "
          f"{len(pieces)} solid piece(s), {R.status().name}; {bb[3]-bb[0]:.0f} x {bb[4]-bb[1]:.0f} x {bb[5]-bb[2]:.0f} mm, "
          f"{R.volume()/1000:.1f} cm³; base ø{2*r:.0f} mm")
    sys.exit(0 if len(pieces) == 1 and R.status().name == "NoError" else 1)

if __name__ == "__main__":
    main()
