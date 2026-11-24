#!/usr/bin/env python3
# The character exactly as the game draws it, as ONE solid statue for printing.
# Input: a full extraction (full_extract.py). Every part (body pieces, hair
# strands, braids, ribbons) becomes a solid, all are merged into one (overlaps
# fuse, so there are no joins), pieces that only touch are welded, and the
# whole is thickened slightly so the thinnest features print on a 0.4 mm nozzle.
#   statue.py full.json OUTDIR [--mm-per-unit 100] [--thicken-mm 0.2]
#             [--base-mm 4] [--base-across 0]
# The game's units are metres; --mm-per-unit 100 is 1:10 scale, the same for
# every character so they stand at their true relative sizes. Writes statue.stl,
# statue_on_base.stl (an octagonal plinth; --base-across sets its width across
# the flats, 0 = just fit this figure) and stand.json (can it stand on its own).
import argparse, json, os, sys
import numpy as np, manifold3d as m3
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hidden_joints as hj, ball_joints as bj

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("full"); ap.add_argument("outdir")
    ap.add_argument("--mm-per-unit", type=float, default=100.0, help="100 = 1:10 scale (game units are metres)")
    ap.add_argument("--base-across", type=float, default=0.0, help="octagon width across flats, mm (0 = fit)")
    ap.add_argument("--thicken-mm", type=float, default=0.2)
    ap.add_argument("--base-mm", type=float, default=4.0)
    a = ap.parse_args(); os.makedirs(a.outdir, exist_ok=True)
    parts = [json.loads(l) for l in open(a.full) if l.strip()]
    allp = np.vstack([bj.pose(p["m"], p["p"]) for p in parts]); lo, hi = allp.min(0), allp.max(0)
    scale = a.mm_per_unit; center = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])
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
    # can it stand on its own? centre of mass vs the feet's contact area
    m = R.to_mesh(); Vv = np.asarray(m.vert_properties)[:, :3]; Ft = np.asarray(m.tri_verts)
    t0, t1, t2 = Vv[Ft[:, 0]], Vv[Ft[:, 1]], Vv[Ft[:, 2]]
    w6 = np.einsum("ij,ij->i", t0, np.cross(t1, t2)); com = ((t0 + t1 + t2) * w6[:, None]).sum(0) / (4 * w6.sum())
    foot = Vv[Vv[:, 2] < 0.8][:, :2]
    hull = m3.CrossSection.hull_points(foot) if len(foot) >= 3 else m3.CrossSection()
    poly = np.vstack(hull.to_polygons()) if not hull.is_empty() else foot
    inside = not hull.is_empty() and not (hull ^ m3.CrossSection.circle(0.01, 8).translate(com[:2])).is_empty()
    # distance from the centre of mass to the nearest edge of the contact area -> how far it can tilt
    def edge_dist(P, q):
        d = 1e9
        for i in range(len(P)):
            a_, b_ = P[i], P[(i + 1) % len(P)]; ab = b_ - a_; t = np.clip(np.dot(q - a_, ab) / (np.dot(ab, ab) + 1e-12), 0, 1)
            d = min(d, np.linalg.norm(q - (a_ + t * ab)))
        return d
    margin = edge_dist(poly, com[:2]) if inside else -edge_dist(poly, com[:2])
    tilt = float(np.degrees(np.arctan2(margin, com[2]))) if margin > 0 else 0.0
    stand = dict(stands_alone=bool(inside and tilt >= 3.0), tip_angle_deg=round(tilt, 1), com_height_mm=round(float(com[2]), 1),
                 com_margin_mm=round(float(margin), 1), contact_area_mm2=round(hull.area() if not hull.is_empty() else 0, 1))
    json.dump(stand, open(os.path.join(a.outdir, "stand.json"), "w"))
    # an octagonal plinth, flats square to the figure, chamfered top edge; both feet sunk
    # SINK mm in, so a foot the game posed slightly off the ground still sits in it
    SINK = 2.0
    sole = Vv[Vv[:, 2] < SINK + 0.5][:, :2]                            # both soles (a foot can sit up to ~1.5 mm high)
    c = (sole.min(0) + sole.max(0)) / 2
    need = float(np.linalg.norm(sole - c, axis=1).max()) + 6.0      # circumradius to clear the feet
    across = a.base_across if a.base_across > 0 else 2 * need * np.cos(np.pi / 8)
    r = across / 2 / np.cos(np.pi / 8)                                 # circumradius from width across flats
    if r < need: print(f"note: base {across:.0f} mm is tighter than this figure's feet want ({2*need*np.cos(np.pi/8):.0f} mm)")
    ch = 1.0
    octo = lambda h, r0, r1: m3.Manifold.cylinder(h, r0, r1, 8).rotate([0, 0, 22.5])
    base = (octo(a.base_mm - ch, r, r) + octo(ch, r, r - ch).translate([0, 0, a.base_mm - ch])).translate([c[0], c[1], -a.base_mm])
    on = (R + base.translate([0, 0, SINK])).translate([0, 0, a.base_mm - SINK])   # feet sunk SINK mm into the plinth
    hj.bj.write_stl(os.path.join(a.outdir, "statue_on_base.stl"), [hj.mesh_of(hj.roundtrip(on))])
    bb = R.bounding_box()
    json.dump(dict(stand, base_needs_across=round(2 * need * np.cos(np.pi / 8), 1), base_across=round(across, 1)), open(os.path.join(a.outdir, "stand.json"), "w"))
    print(f"{len(parts)} parts merged{' (skipped ' + ','.join(skipped) + ')' if skipped else ''}; "
          f"{len(pieces)} solid piece(s), {R.status().name}; {bb[3]-bb[0]:.0f} x {bb[4]-bb[1]:.0f} x {bb[5]-bb[2]:.0f} mm, "
          f"{R.volume()/1000:.1f} cm³; base {across:.0f} mm across; stands alone: {stand['stands_alone']} (tips at {stand['tip_angle_deg']}°)")
    sys.exit(0 if len(pieces) == 1 and R.status().name == "NoError" else 1)

if __name__ == "__main__":
    main()
