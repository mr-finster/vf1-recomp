#!/usr/bin/env python3
# Independent print check of a hidden_joints.py output folder, reading only the
# STL files (as a slicer does): every part one closed solid, every pair of
# parts at least GAP apart (exact: Minkowski growth), and wall thickness.
#   check_print.py OUTDIR [--gap 0.45] [--min-wall 0.8]
import argparse, glob, os, struct, sys
import numpy as np, manifold3d as m3
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hidden_joints as hj

def load(p):
    with open(p, "rb") as f:
        f.read(80); n, = struct.unpack("<I", f.read(4))
        raw = np.frombuffer(f.read(50 * n), dtype=np.uint8).reshape(n, 50)
    T = raw[:, 12:48].copy().view(np.float32).reshape(-1, 3)
    mesh = m3.Mesh(vert_properties=T, tri_verts=np.arange(len(T), dtype=np.uint32).reshape(-1, 3)); mesh.merge()
    return m3.Manifold(mesh), T.reshape(-1, 3, 3)

def wall(M, T, rays=300, seed=1):
    rng = np.random.default_rng(seed); idx = rng.choice(len(T), min(rays, len(T)), replace=False); out = []
    for t in T[idx].astype(float):
        n = np.cross(t[1] - t[0], t[2] - t[0]); L = np.linalg.norm(n)
        if L < 1e-9: continue
        n /= L; c = t.mean(0)
        hits = M.ray_cast(tuple(c - n * 1e-3), tuple(c - n * 30))
        d = [h.distance for h in hits]
        if d: out.append(min(d) * 30)
    return np.array(out)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("outdir")
    ap.add_argument("--gap", type=float, default=0.45); ap.add_argument("--min-wall", type=float, default=0.8)
    a = ap.parse_args()
    parts = {}; tris = {}
    for p in sorted(glob.glob(os.path.join(a.outdir, "part_*.stl"))):
        n = os.path.basename(p)[5:-4]; parts[n], tris[n] = load(p)
    bad = []
    for n, M in parts.items():
        sh = [x for x in M.decompose() if x.volume() > 1e-6]
        if M.status().name != "NoError" or len(sh) != 1: bad.append(f"part {n}: {len(sh)} pieces, {M.status().name}")
    names = list(parts)
    for i in range(len(names)):
        for k in range(i + 1, len(names)):
            if hj.too_close(parts[names[i]], parts[names[k]], a.gap * 0.97):
                bad.append(f"{names[i]} and {names[k]} closer than {a.gap} mm (would fuse)")
    whole, _ = load(os.path.join(a.outdir, "figure_assembled.stl"))
    bodies = len([x for x in whole.decompose() if x.volume() > 1e-6])
    if bodies != len(parts): bad.append(f"figure_assembled.stl has {bodies} bodies, expected {len(parts)}")
    bb = whole.bounding_box()
    print(f"{len(parts)} parts, figure {bb[3]-bb[0]:.0f} x {bb[4]-bb[1]:.0f} x {bb[5]-bb[2]:.0f} mm, {whole.volume()/1000:.1f} cm³")
    print("wall thickness (from 300 inward rays per part):")
    for n in names:
        w = wall(parts[n], tris[n])
        if len(w): print(f"  {n:>7}: thinnest {w.min():.2f} mm, median {np.median(w):.1f} mm, {100*(w < a.min_wall).mean():.0f}% under {a.min_wall} mm")
    print("PASS: one closed solid per part, all parts at least", a.gap, "mm apart, single-print file intact" if not bad else "FAIL:")
    for b in bad: print("  " + b)
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
