#!/usr/bin/env python3
# Ball-jointed figure with the joints hidden INSIDE the body: the character
# keeps its original shape, with only thin seam lines at the joints.
#
# At each joint (pivot = the part's pose-matrix translation, from the ROM):
#   - a spherical seam, sized to the limb, centred on the pivot: everything of
#     the two parts inside the sphere goes to the child (its "dome"), the parent
#     keeps the rest, a seam gap away. Concentric spheres rotate without
#     colliding, so the outside looks whole but the child can turn.
#   - hidden print-in-place joint: the parent carries a ball on a post that
#     reaches into the dome; the ball is captive in a cavity (ball + clearance)
#     with a channel just wide enough for the post to swing, kept inside the
#     area the parent's body covers. The figure prints assembled.
# Exact solid booleans via manifold3d (pip install manifold3d).
#
#   hidden_joints.py dump.json OUTDIR [--height-mm 180] [--seam-mm 0.4]
#                    [--clearance-mm 0.3] [--swing 40] [--check]
import argparse, json, os, sys
import numpy as np
import manifold3d as m3
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ball_joints as bj

def to_manifold(V, F):
    F = np.asarray(F); used = np.unique(F.ravel())
    remap = np.full(len(V), -1); remap[used] = np.arange(len(used))
    mesh = m3.Mesh(vert_properties=np.asarray(V, np.float32)[used], tri_verts=remap[F].astype(np.uint32))
    mesh.merge()
    return m3.Manifold(mesh)

def solidify(V, F, t):
    """Give an open sheet a thickness t (both sides t/2): a closed slab."""
    V = np.asarray(V, float); F = [list(f) for f in F]
    n = np.zeros_like(V)
    for a, b, c in F:
        fn = np.cross(V[b] - V[a], V[c] - V[a]); n[a] += fn; n[b] += fn; n[c] += fn
    L = np.linalg.norm(n, axis=1, keepdims=True); n = n / np.where(L == 0, 1, L)
    N = len(V); W = np.vstack([V + n * t / 2, V - n * t / 2])
    out = F + [[a + N, c + N, b + N] for a, b, c in F]
    for loop in bj.boundary_loops(F):
        for i in range(len(loop)):
            u, v = loop[i], loop[(i + 1) % len(loop)]
            out += [[v, u, u + N], [v, u + N, v + N]]
    return W, out

def node_solid(ps, xf, sheet_t):
    solid = None; pts = []
    for p in ps:
        W = bj.pose(p["m"], p["p"]); V, remap = bj.weld(W)
        F = bj.faces_of(p.get("q", []), remap)
        if not F: continue
        Vm = xf(V)
        Vc, Fc = bj.cap(Vm, [list(f) for f in F])
        M = to_manifold(Vc, Fc)
        ext = np.ptp(Vm[np.unique(np.asarray(F))], axis=0)
        if M.status().name != "NoError" or abs(M.volume()) < 0.02 * np.prod(np.sort(ext)[1:]) * sheet_t:
            M = to_manifold(*solidify(Vm, F, sheet_t))      # a sheet: give it thickness
        if M.volume() < 0:
            M = to_manifold(Vc, [[a, c, b] for a, b, c in Fc])
        solid = M if solid is None else solid + M
        pts.append(Vm)
    return solid, np.vstack(pts)

def place(M, c, d):
    """Move a +Z-aligned primitive so its origin is at c and +Z points along d."""
    R = bj.rot_to(np.asarray(d, float))
    return M.transform(np.hstack([R, np.asarray(c, float)[:, None]]))

def rot_about(M, c, axis, ang):
    R = bj.rot_axis_m(np.asarray(axis, float), ang); c = np.asarray(c, float)
    return M.transform(np.hstack([R, (c - R @ c)[:, None]]))

def build(parts, height_mm=180.0, seam_mm=0.4, clearance_mm=0.3, swing_deg=40.0, sheet_mm=0.8):
    members, origin, far, parent, root = bj.build_skeleton(parts)
    allp = np.vstack([bj.pose(p["m"], p["p"]) for p in parts])
    lo, hi = allp.min(0), allp.max(0); scale = height_mm / ((hi[1] - lo[1]) or 1)
    center = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])
    def xf(V):
        Q = (np.asarray(V, float) - center) * scale
        return Q[..., [0, 2, 1]] * np.array([1, -1, 1])
    def xd(v):
        q = np.asarray(v, float)[[0, 2, 1]] * np.array([1, -1, 1]); return q / np.linalg.norm(q)
    solid = {}; pts = {}
    for n, ps in members.items():
        s, P = node_solid(ps, xf, sheet_mm)
        if s is not None: solid[n] = s; pts[n] = P
    seg = 48; joints = {}; orig = dict(solid)
    def depth(n):
        k = 0
        while parent.get(n): n = parent[n]; k += 1
        return k
    for child in sorted((c for c in parent if parent[c] and c in solid), key=depth):
        par = parent[child]
        if par not in solid: continue
        c = xf(origin[child]); d = xd(far[child] - origin[child])
        # seam sphere: the limbs' cross-section where they meet, so the seam goes all the way round
        Q = pts[child]; rel = Q - c; al = rel @ d            # the moving part's own cross-section
        rad = np.linalg.norm(rel - np.outer(al, d), axis=1)
        rc_child = float(np.median(np.linalg.norm((pts[child] - c) - np.outer((pts[child] - c) @ d, d), axis=1)))
        slab = np.abs(al) < 0.35 * rc_child
        Rs = float(np.percentile(rad[slab], 90)) * 1.05 if slab.sum() > 4 else 1.3 * rc_child
        Rs = min(max(Rs, 4.5), 1.8 * rc_child)
        # where the two parts overlapped in the game model, they'd be left touching
        # (zero gap) outside the seam: grow the seam sphere to swallow that overlap
        ov = solid[child] ^ solid[par]
        if ov.volume() > 1e-3:
            ovv = np.asarray(ov.to_mesh().vert_properties)[:, :3]
            Rs = max(Rs, min(float(np.linalg.norm(ovv - c, axis=1).max()) + 0.6, 2.6 * rc_child))
        rb = float(np.clip(0.36 * Rs, 1.6, Rs - 1.2 - clearance_mm - 1.0))
        # the ball must fit inside the body with a wall round its cavity: shrink it
        # until cavity + 0.8 mm is fully enclosed by the original two parts
        body = orig[par] + orig[child]
        while rb > 1.6 and (m3.Manifold.sphere(rb + clearance_mm + 0.8, seg).translate(c) - body).volume() > 0.05:
            rb *= 0.9
        rb = max(rb, 1.6)
        rin = rb + clearance_mm; rn = 0.42 * rb
        S = m3.Manifold.sphere(Rs, seg).translate(c)
        Sg = m3.Manifold.sphere(Rs + seam_mm, seg).translate(c)
        P, C = solid[par], solid[child]
        dome = (P + C) ^ S
        for o, Mo in solid.items():                                  # nobody else's material
            if o not in (par, child): dome = dome - Mo
        P = P - Sg
        C = dome + ((C - S) - P)
        # hidden print-in-place joint: a ball on a post (parent) captive in a
        # cavity inside the dome (child), with a channel just wide enough for the
        # post to swing in. The figure prints assembled; nothing snaps in.
        u = pts[par].mean(0) - c; u = u / (np.linalg.norm(u) or 1)
        post = Rs + seam_mm + max(2.0 * rb, 3.0)
        mkstem = lambda: place(m3.Manifold.cylinder(post, rn, rn, seg), c, u)
        stem = mkstem() ^ (P + Sg)
        P = P + m3.Manifold.sphere(rb, seg).translate(c) + stem
        # the channel: hull of the post swung swing_deg every way about c ...
        w = np.cross(u, [0, 0, 1.0]); w = w if np.linalg.norm(w) > 1e-6 else np.cross(u, [0, 1.0, 0])
        w /= np.linalg.norm(w); v = np.cross(u, w); sw = [mkstem()]
        for k in range(8):
            t = 2 * np.pi * k / 8
            sw.append(rot_about(mkstem(), c, np.cos(t) * w + np.sin(t) * v, np.radians(swing_deg)))
        swept = m3.Manifold.batch_hull(sw).offset(clearance_mm) if hasattr(m3.Manifold, "offset") else m3.Manifold.batch_hull(sw)
        # ... kept inside the cone the parent's body covers, so it never shows
        band = pts[par][np.abs(np.linalg.norm(pts[par] - c, axis=1) - (Rs + 2.0)) < 2.0]
        ang = np.degrees(np.arccos(np.clip((band - c) @ u / np.linalg.norm(band - c, axis=1), -1, 1))) if len(band) > 8 else np.array([35.0])
        beta = float(np.clip(np.percentile(ang, 25) * 0.85, 12.0, 70.0))
        cone_h = Rs + seam_mm + 1.0
        cover = place(m3.Manifold.cylinder(cone_h, 0.0, cone_h * np.tan(np.radians(beta)), seg), c, u)
        C = C - m3.Manifold.sphere(rin, seg).translate(c) - (swept ^ cover)
        solid[par], solid[child] = P, C
        joints[child] = dict(parent=par, c=c, d=d, u=u, Rs=Rs, rb=rb, rin=rin, rn=rn, beta=beta)
    return dict(solid=solid, joints=joints, parent=parent, root=root)

def check(rig, max_deg=70, dirs=8, tol=0.5):
    """Per joint: swing (deg) before the child touches its parent (>tol mm^3 of
    overlap), worst and best direction; twist is about the seam sphere."""
    out = []
    for child, j in rig["joints"].items():
        P, C, c, d = rig["solid"][j["parent"]], rig["solid"][child], j["c"], j["d"]
        a = np.cross(d, [0, 0, 1.0]); a = a if np.linalg.norm(a) > 1e-6 else np.cross(d, [0, 1.0, 0])
        a /= np.linalg.norm(a); b = np.cross(d, a)
        rest = (P ^ C).volume()
        lims = []
        for k in range(dirs):
            t = 2 * np.pi * k / dirs; ax = np.cos(t) * a + np.sin(t) * b
            lo_, hi_ = 0.0, float(max_deg)
            if (P ^ rot_about(C, c, ax, np.radians(hi_))).volume() <= tol: lims.append(hi_); continue
            for _ in range(7):
                mid = (lo_ + hi_) / 2
                if (P ^ rot_about(C, c, ax, np.radians(mid))).volume() <= tol: lo_ = mid
                else: hi_ = mid
            lims.append(lo_)
        out.append((j["parent"], child, 2 * j["Rs"], 2 * j["rb"], rest, min(lims), max(lims)))
    return out

def mesh_of(M):
    m = M.to_mesh(); return np.asarray(m.vert_properties)[:, :3], np.asarray(m.tri_verts)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump"); ap.add_argument("outdir")
    ap.add_argument("--height-mm", type=float, default=180.0)
    ap.add_argument("--seam-mm", type=float, default=0.4)
    ap.add_argument("--clearance-mm", type=float, default=0.3)
    ap.add_argument("--swing", type=float, default=40.0, help="post swing the channel allows (deg)")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    parts = [json.loads(l) for l in open(a.dump) if l.strip()]
    os.makedirs(a.outdir, exist_ok=True)
    rig = build(parts, a.height_mm, a.seam_mm, a.clearance_mm, a.swing)
    allm = []
    for n, M in rig["solid"].items():
        V, F = mesh_of(M); bj.write_stl(os.path.join(a.outdir, f"part_{n.replace('_', '-')}.stl"), [(V, F)]); allm.append((V, F))
    bj.write_stl(os.path.join(a.outdir, "figure_assembled.stl"), allm)
    print(f"{len(rig['solid'])} parts, {len(rig['joints'])} hidden joints -> {a.outdir}")
    if a.check:
        print(f"{'joint':>12} {'seam ø':>7} {'ball ø':>7} {'overlap@rest':>13} {'swing min..max':>15}")
        for par, ch, s, b, r, mn, mx in check(rig):
            print(f"{par + '>' + ch:>12} {s:7.1f} {b:7.1f} {r:11.2f}mm³ {mn:6.0f}°..{mx:3.0f}°")

if __name__ == "__main__":
    main()
