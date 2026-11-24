#!/usr/bin/env python3
# Build a 3D-printable, ball-jointed figure from a VF_DUMP_FIGHTER dump.
#
# Each body part in the dump carries the matrix that poses it; the matrix's
# translation is that part's joint pivot (where it attaches to its parent). We
# recover the skeleton by linking each part to the nearest more-proximal part
# (a spanning tree rooted at the torso), then at every joint we add:
#   - a BALL (sphere on a short neck) to the child, pointing into the parent,
#   - a SOCKET (thick hemispherical cup with a retaining opening) to the parent,
#     facing the child.
# Ball and socket share the pivot, so the printed child snaps into the printed
# parent and rotates: a ball joint. Everything is emitted as overlapping
# watertight solids in each part's STL; a slicer unions them, so no CSG is
# needed. Assembled (not print-in-place): print the parts, then click together.
#
#   ball_joints.py dump.json OUTDIR [--height-mm 180] [--clearance-mm 0.3]
#                  [--ball-frac 0.45] [--zup]
#
# numpy only. Safe under `python3 -I`.

import argparse, json, os, struct, sys
import numpy as np

# ---- shared mesh helpers (match build_print.py) --------------------------
def weld(pts, eps=1e-4):
    pts = np.asarray(pts, float)
    key = np.round(pts / eps).astype(np.int64)
    seen = {}; remap = np.empty(len(pts), int); V = []
    for i, k in enumerate(map(tuple, key)):
        j = seen.get(k)
        if j is None:
            j = seen[k] = len(V); V.append(pts[i])
        remap[i] = j
    return (np.array(V) if V else np.zeros((0, 3))), remap

def pose(m, P):
    A = np.array(m[:9]).reshape(3, 3).T
    return np.asarray(P) @ A.T + np.array(m[9:12])

def smooth_taubin(V, F, iters, lam=0.5, mu=-0.53):
    """Low-pass the game-mesh vertex noise without shrinking (Taubin lambda/mu)."""
    if iters <= 0 or len(F) == 0:
        return V
    from collections import defaultdict
    nb = defaultdict(set)
    for f in F:
        for a, b in ((f[0], f[1]), (f[1], f[2]), (f[2], f[0])):
            nb[a].add(b); nb[b].add(a)
    idx = [np.array(sorted(nb[i])) if nb.get(i) else np.array([i]) for i in range(len(V))]
    V = np.asarray(V, float).copy()
    def lap(P):
        out = np.empty_like(P)
        for i in range(len(P)):
            out[i] = P[idx[i]].mean(0) - P[i]
        return out
    for _ in range(iters):
        V = V + lam * lap(V); V = V + mu * lap(V)
    return V

def faces_of(quads, remap):
    F = []
    for q in quads:
        if len(q) < 4: continue
        a, b, c, d = (remap[x] for x in q[:4])
        for t in ((a, b, c), (a, c, d)):
            if len(set(t)) == 3: F.append(list(t))
    return F

def boundary_loops(F):
    from collections import defaultdict, deque
    cnt = defaultdict(int)
    for f in F:
        for u, v in ((f[0], f[1]), (f[1], f[2]), (f[2], f[0])): cnt[(u, v)] += 1
    out = defaultdict(deque)
    for (u, v), c in cnt.items():
        if cnt.get((v, u), 0) == 0: out[u].append(v)
    loops = []
    for s in list(out):
        while out[s]:
            loop = []; u = s
            while out[u]:
                v = out[u].popleft(); loop.append(u); u = v
                if u == s: break
            if len(loop) >= 3: loops.append(loop)
    return loops

def cap(V, F):
    V = list(V)
    for loop in boundary_loops(F):
        c = len(V); V.append(np.mean([V[i] for i in loop], axis=0))
        n = len(loop)
        for i in range(n):
            a, b = loop[i], loop[(i + 1) % n]; F.append([b, a, c])
    return np.array(V), F

def trim(V, F, center, r):
    """Remove faces reaching within r of center, then re-close the stub. Used to
    cut a limb end back so a ball/socket can bridge the gap at the joint."""
    center = np.asarray(center)
    keep = [f for f in F if all(np.linalg.norm(V[i] - center) > r for i in f)]
    if not keep:
        return V, F
    return cap(np.array(V), keep)

def tri_normal(a, b, c):
    n = np.cross(b - a, c - a); L = np.linalg.norm(n)
    return n / L if L else n

def write_stl(path, meshes):
    """meshes: list of (V, F); all written as one STL (slicer unions them)."""
    tris = []
    for V, F in meshes:
        for a, b, c in F:
            tris.append((V[a], V[b], V[c]))
    with open(path, "wb") as f:
        f.write(b"VF1 recomp ball-joint part".ljust(80, b"\0"))
        f.write(struct.pack("<I", len(tris)))
        for A, B, C in tris:
            f.write(struct.pack("<3f", *tri_normal(A, B, C)))
            for P in (A, B, C): f.write(struct.pack("<3f", *P))
            f.write(struct.pack("<H", 0))

# ---- primitive solids (watertight) ---------------------------------------
def rot_to(d):
    """Rotation mapping +z to unit vector d."""
    d = d / (np.linalg.norm(d) or 1)
    z = np.array([0, 0, 1.0])
    v = np.cross(z, d); s = np.linalg.norm(v); c = float(np.dot(z, d))
    if s < 1e-9:
        return np.eye(3) if c > 0 else np.diag([1, -1, -1.0])
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx * ((1 - c) / (s * s))

def uv_sphere(center, r, seg=20, ring=14):
    V = []; idx = {}
    for i in range(ring + 1):
        th = np.pi * i / ring
        for j in range(seg):
            ph = 2 * np.pi * j / seg
            idx[(i, j)] = len(V)
            V.append(center + r * np.array([np.sin(th) * np.cos(ph), np.sin(th) * np.sin(ph), np.cos(th)]))
    F = []
    for i in range(ring):
        for j in range(seg):
            a = idx[(i, j)]; b = idx[(i, (j + 1) % seg)]
            c = idx[(i + 1, j)]; d = idx[(i + 1, (j + 1) % seg)]
            F += [[a, b, d], [a, d, c]]
    return np.array(V), F

def cylinder(p0, p1, r, seg=20):
    axis = np.array(p1) - np.array(p0); L = np.linalg.norm(axis)
    R = rot_to(axis / (L or 1)); V = []
    for k, z in ((0, 0.0), (1, L)):
        for j in range(seg):
            ph = 2 * np.pi * j / seg
            V.append(np.array(p0) + R @ np.array([r * np.cos(ph), r * np.sin(ph), z]))
    cap0 = len(V); V.append(np.array(p0)); cap1 = len(V); V.append(np.array(p1))
    F = []
    for j in range(seg):
        a = j; b = (j + 1) % seg; c = seg + j; d = seg + (j + 1) % seg
        F += [[a, b, d], [a, d, c]]
        F.append([cap0, b, a])          # bottom
        F.append([cap1, c, d])          # top
    return np.array(V), F

def socket_cup(center, d, r_in, wall, open_half_angle=1.15, seg=24, ring=12):
    """A thick hemispherical-plus cup: cavity radius r_in, outer r_in+wall, with
    a circular opening of half-angle `open_half_angle` (rad) facing +d. The ball
    (radius ~ r_in-clearance) is retained because the opening is < a hemisphere."""
    R = rot_to(np.array(d, float)); ro = r_in + wall
    th0 = open_half_angle            # polar angle where the rim/opening is (from +z)
    def shell(radius, i0, i1, reverse):
        V = []; idx = {}
        for i in range(i0, i1 + 1):
            th = th0 + (np.pi - th0) * (i - i0) / (i1 - i0)
            for j in range(seg):
                ph = 2 * np.pi * j / seg
                idx[(i, j)] = len(V)
                p = radius * np.array([np.sin(th) * np.cos(ph), np.sin(th) * np.sin(ph), np.cos(th)])
                V.append(center + R @ p)
        F = []
        for i in range(i0, i1):
            for j in range(seg):
                a = idx[(i, j)]; b = idx[(i, (j + 1) % seg)]
                c = idx[(i + 1, j)]; dd = idx[(i + 1, (j + 1) % seg)]
                quad = [[a, b, dd], [a, dd, c]]
                if reverse: quad = [[t[0], t[2], t[1]] for t in quad]
                F += quad
        return np.array(V), F, idx
    Vo, Fo, io = shell(ro, 0, ring, False)       # outer, normals out
    Vi, Fi, ii = shell(r_in, 0, ring, True)      # inner cavity, normals in
    n0 = len(Vo)
    V = np.vstack([Vo, Vi]); F = list(Fo) + [[a + n0, b + n0, c + n0] for a, b, c in Fi]
    # rim ring at th0 joining outer and inner
    for j in range(seg):
        a = io[(0, j)]; b = io[(0, (j + 1) % seg)]
        c = ii[(0, j)] + n0; dd = ii[(0, (j + 1) % seg)] + n0
        F += [[a, dd, b], [a, c, dd]]
    return V, F

# ---- skeleton ------------------------------------------------------------
# The cast shares one skeleton; these record-offset tags are its body slots.
# arm/leg chains: upper -> lower -> extremity. "head" and the hands are added
# by prefix/proximity below.
SKELETON_PARENT = {
    "26": "18", "30": "26",          # arm A: upper <- torso, fore <- upper
    "38": "18", "42": "38",          # arm B
    "54": "18", "58": "54", "62": "58",   # leg A: thigh<-torso/hips, shin, foot
    "66": "18", "70": "66", "74": "70",   # leg B
    "head": "18",                    # head <- torso
}
FOREARMS = ("30", "42")

def group_of(tag):
    """Canonical body node for a dump tag."""
    if tag.startswith("h"):
        return "head"                # all head shells are one rigid piece
    if tag.startswith("k"):
        return tag                   # a hand; parent assigned by proximity
    if tag == "50":
        return "18"                  # hips/pelvis are rigid with the torso
    return tag

def build_skeleton(parts):
    """Group parts into body nodes; return each node's pivot, far point, the
    member tags, and a parent map rooted at the torso."""
    members = {}
    for p in parts:
        members.setdefault(group_of(p["tag"]), []).append(p)
    origin = {}; far = {}
    for node, ps in members.items():
        origin[node] = np.mean([p["m"][9:12] for p in ps], axis=0)
        allW = np.vstack([pose(p["m"], p["p"]) for p in ps])
        far[node] = (allW.min(0) + allW.max(0)) / 2
    root = "18" if "18" in members else min(origin, key=lambda t: origin[t][1])
    parent = {root: None}
    for node in members:
        if node == root:
            continue
        if node in SKELETON_PARENT and SKELETON_PARENT[node] in members:
            parent[node] = SKELETON_PARENT[node]
        elif node.startswith("k"):   # a hand -> nearest forearm, else torso
            cand = [f for f in FOREARMS if f in members] or [root]
            parent[node] = min(cand, key=lambda f: np.linalg.norm(origin[node] - origin[f]))
        else:                        # unknown part -> nearest other origin
            parent[node] = min((o for o in origin if o != node),
                               key=lambda o: np.linalg.norm(origin[node] - origin[o]))
    return members, origin, far, parent, root

# ---- main ----------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump"); ap.add_argument("outdir")
    ap.add_argument("--height-mm", type=float, default=180.0)
    ap.add_argument("--clearance-mm", type=float, default=0.3)
    ap.add_argument("--ball-frac", type=float, default=0.45,
                    help="ball radius as a fraction of the child's cross-section")
    ap.add_argument("--smooth", type=int, default=12,
                    help="Taubin smoothing passes to clean the mesh noise (0 = raw)")
    ap.add_argument("--zup", action="store_true")
    args = ap.parse_args()

    parts = [json.loads(l) for l in open(args.dump) if l.strip()]
    os.makedirs(args.outdir, exist_ok=True)
    members, origin, far, parent, root = build_skeleton(parts)

    # pose+weld+cap each body node (a node may merge several dump parts, e.g.
    # the head's shells); measure cross-section near its pivot
    mesh = {}; radius = {}
    allpts = []
    for node, ps in members.items():
        Vlist = []; Flist = []
        for p in ps:
            W = pose(p["m"], p["p"]); V, remap = weld(W)
            F = faces_of(p.get("q", []), remap)
            if not F:
                continue
            V = smooth_taubin(V, F, args.smooth)
            V, F = cap(V, F)
            base = sum(len(v) for v in Vlist)
            Vlist.append(V); Flist += [[a + base, b + base, c + base] for a, b, c in F]
        if not Flist:
            continue
        V = np.vstack(Vlist); mesh[node] = (V, Flist); allpts.append(V)
        o = origin[node]; ax = far[node] - o; axn = ax / (np.linalg.norm(ax) or 1)
        rel = V - o; perp = rel - np.outer(rel @ axn, axn)
        radius[node] = float(np.median(np.linalg.norm(perp, axis=1))) or 0.05

    allpts = np.vstack(allpts)
    lo, hi = allpts.min(0), allpts.max(0)
    scale = args.height_mm / ((hi[1] - lo[1]) or 1.0)
    center = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])
    clr = args.clearance_mm / scale  # clearance back in model units

    # add joints: trim both limb ends back from the pivot, then bridge the gap
    # with a protruding ball (child) seated in a socket cup (parent).
    joints = 0
    extra = {t: [] for t in mesh}   # extra primitive meshes per part
    for child, par in parent.items():
        if par is None or child not in mesh or par not in mesh:
            continue
        p = origin[child]
        d_child = far[child] - p                    # child extends this way (+d)
        if np.linalg.norm(d_child) < 1e-6:
            d_child = far[child] - far[par]
        d_child /= (np.linalg.norm(d_child) or 1)
        rb = max(args.ball_frac * radius[child], 2.0 / scale)   # >= ~2 mm ball
        rcut = rb * 1.3
        # cut the child's proximal end and the parent's distal end back
        mesh[child] = trim(*mesh[child], p, rcut)
        mesh[par] = trim(*mesh[par], p, rcut)
        # child: ball centred at the pivot, on a neck back to the trimmed stub
        extra[child].append(uv_sphere(p, rb))
        extra[child].append(cylinder(p + d_child * rcut * 1.15, p, rb * 0.5))
        # parent: socket cup at the pivot, cavity = ball + clearance, opening to child
        extra[par].append(socket_cup(p, d_child, rb + clr, wall=max(rb * 0.5, 1.2 / scale)))
        joints += 1

    def xform(V):
        Q = (np.asarray(V) - center) * scale
        if args.zup: Q = Q[:, [0, 2, 1]] * np.array([1, -1, 1])
        return Q

    for t, (V, F) in mesh.items():
        meshes = [(xform(V), F)] + [(xform(ev), ef) for ev, ef in extra[t]]
        name = t.replace("_", "-")
        write_stl(os.path.join(args.outdir, f"part_{name}.stl"), meshes)

    # a combined preview (parts + joints), for a quick look
    allm = []
    for t, (V, F) in mesh.items():
        allm.append((xform(V), F))
        for ev, ef in extra[t]: allm.append((xform(ev), ef))
    write_stl(os.path.join(args.outdir, "figure_jointed.stl"), allm)

    print(f"{len(mesh)} parts, {joints} ball joints; root={root}; "
          f"{args.height_mm:.0f} mm, clearance {args.clearance_mm} mm -> {args.outdir}")
    print("skeleton:")
    for c, pr in parent.items():
        if pr: print(f"  {pr:>5} -> {c}")


if __name__ == "__main__":
    main()
