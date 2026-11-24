#!/usr/bin/env python3
# Rebuild a solid the way an FDM printer lays it down: one slab per layer, each
# layer's outline "opened" by the nozzle (convex corners rounded to the bead
# radius, anything thinner than one line gone), the bead's top edge rounded so
# neighbouring layers leave a groove. Optional height window for close-ups.
#   fdm_layers.py in.stl out.stl --layer 0.12 --nozzle 0.4 [--zmin A --zmax B]
import argparse, sys, os, numpy as np, manifold3d as m3
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_print as cp, hidden_joints as hj
ap = argparse.ArgumentParser(); ap.add_argument("inp"); ap.add_argument("out")
ap.add_argument("--layer", type=float, default=0.12); ap.add_argument("--nozzle", type=float, default=0.4)
ap.add_argument("--zmin", type=float, default=None); ap.add_argument("--zmax", type=float, default=None)
a = ap.parse_args()
M, _ = cp.load(a.inp); bb = M.bounding_box()
z0 = bb[2] if a.zmin is None else max(bb[2], a.zmin); z1 = bb[5] if a.zmax is None else min(bb[5], a.zmax)
h = a.layer; r = a.nozzle / 2; slabs = []
n0 = int(np.floor((z0 - bb[2]) / h)); n1 = int(np.ceil((z1 - bb[2]) / h))
for i in range(n0, n1):
    zb = bb[2] + i * h; cs = M.slice(float(zb + h / 2))
    if cs.is_empty(): continue
    cs = cs.offset(-r, m3.JoinType.Round).offset(r, m3.JoinType.Round)        # what one nozzle line can make
    if cs.is_empty(): continue
    core = cs.extrude(h * 0.7).translate([0, 0, zb])
    lip = cs.offset(-h * 0.35, m3.JoinType.Round)                               # the bead's rounded top edge
    slabs.append(core)
    if not lip.is_empty(): slabs.append(lip.extrude(h * 0.3).translate([0, 0, zb + h * 0.7]))
P = m3.Manifold.batch_boolean(slabs, m3.OpType.Add)
hj.bj.write_stl(a.out, [hj.mesh_of(P)])
print(f"{n1 - n0} layers of {h} mm, {P.num_tri()} triangles -> {a.out}")
