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
def clip_plane(V, F, point, n):
    """Slice a closed mesh with a plane, keeping the side n points to, and cap
    the cut flat. Triangles are split along the plane (shared edge points, so
    it stays watertight); the cut loop lies in the plane, so its cap is flat."""
    V = [np.asarray(v, float) for v in V]; point = np.asarray(point); n = np.asarray(n)
    sd = [float(np.dot(v - point, n)) for v in V]
    cache = {}
    def cut(i, j):
        k = (min(i, j), max(i, j))
        if k not in cache:
            t = sd[i] / (sd[i] - sd[j])
            cache[k] = len(V); V.append(V[i] + t * (V[j] - V[i])); sd.append(0.0)
        return cache[k]
    out = []
    for f in F:
        ins = [sd[i] >= 0 for i in f]
        if all(ins): out.append(list(f)); continue
        if not any(ins): continue
        poly = []
        for a in range(3):
            i, j = f[a], f[(a + 1) % 3]
            if sd[i] >= 0: poly.append(i)
            if (sd[i] >= 0) != (sd[j] >= 0): poly.append(cut(i, j))
        for k in range(1, len(poly) - 1):
            if len({poly[0], poly[k], poly[k + 1]}) == 3:
                out.append([poly[0], poly[k], poly[k + 1]])
    if not out:
        return np.array(V), out
    return cap(np.array(V), out)


def trim_joint(V, F, pivot, d, r_sphere, side, reach, offset=0.0):
    """Cut a limb end back at a joint: drop faces with a vertex inside the
    sphere r_sphere about the pivot, and (within `reach` of it) faces on the
    wrong side of the joint plane (side=+1 keeps +d, -1 keeps -d). Re-capped."""
    V = np.asarray(V); pivot = np.asarray(pivot)
    rel = V - pivot; dist = np.linalg.norm(rel, axis=1); along = rel @ d
    bad = (dist < r_sphere) | ((dist < reach) & (side * along < offset))
    keep = [f for f in F if not bad[f].any()]
    return cap(V, keep) if keep else (V, F)


def build_rig(parts, height_mm=180.0, clearance_mm=0.3, ball_frac=0.58,
              snap_mm=0.3, smooth=0, zup=True, gap_mm=1.5, wall_frac=0.45):
    """Everything the STL writer, the checker and the animator need."""
    members, origin, far, parent, root = build_skeleton(parts)
    mesh = {}; radius = {}; allpts = []
    for node, ps in members.items():
        Vl = []; Fl = []
        for p in ps:
            W = pose(p["m"], p["p"]); V, remap = weld(W)
            F = faces_of(p.get("q", []), remap)
            if not F: continue
            V = smooth_taubin(V, F, smooth); V, F = cap(V, F)
            b = sum(len(v) for v in Vl); Vl.append(V); Fl += [[x + b, y + b, z + b] for x, y, z in F]
        if not Fl: continue
        V = np.vstack(Vl); mesh[node] = (V, Fl); allpts.append(V)
        o = origin[node]; ax = far[node] - o; axn = ax / (np.linalg.norm(ax) or 1)
        rel = V - o; perp = rel - np.outer(rel @ axn, axn)
        radius[node] = float(np.median(np.linalg.norm(perp, axis=1))) or 0.05
    allpts = np.vstack(allpts); lo, hi = allpts.min(0), allpts.max(0)
    scale = height_mm / ((hi[1] - lo[1]) or 1.0)
    center = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])
    mm = 1.0 / scale                                  # one millimetre in model units
    clr = clearance_mm * mm; gap = gap_mm * mm
    balls = {n: [] for n in mesh}; sockets = {n: [] for n in mesh}; joints = {}
    for child, par in parent.items():
        if par is None or child not in mesh or par not in mesh: continue
        p = origin[child]; d = far[child] - p
        if np.linalg.norm(d) < 1e-6: d = far[child] - far[par]
        d = d / (np.linalg.norm(d) or 1)
        rb = max(ball_frac * radius[child], 1.6 * mm)     # >= 3.2 mm ball
        rin = rb + clr                                     # socket cavity
        ro = rin + max(wall_frac * rb, 1.0 * mm)            # socket outer (>= 1 mm wall)
        neck = 0.36 * rb
        th0 = float(np.arcsin(min(0.99, max(rb - snap_mm * mm, neck * 1.4) / rin)))
        # child: flat cut just past the socket shell; only its +d side stays
        mesh[child] = clip_plane(*mesh[child], p + d * (ro + gap), d)
        # parent: flat cut at the cavity's back, so the cavity is empty of limb;
        # the socket's outer wall reaches back past it and fuses to the limb.
        # The torso (several joints) gets a local sphere bite instead of a plane.
        if parent.get(par) is None:
            mesh[par] = trim_joint(*mesh[par], p, d, rin + 0.4 * mm, -1, 0.0)
        else:
            mesh[par] = clip_plane(*mesh[par], p - d * rin, -d)
        balls[child].append(uv_sphere(p, rb))
        balls[child].append(cylinder(p + d * (ro + gap + 0.8 * rb), p, neck))
        sockets[par].append(socket_cup(p, d, rin, ro - rin, open_half_angle=th0))
        swing = float(np.degrees(th0 - np.arcsin(min(1.0, neck / rin))))
        joints[child] = dict(parent=par, pivot=p, axis=d, rb=rb, rin=rin, ro=ro,
                             neck=neck, th0=th0, swing=swing)

    def xform(V):
        Q = (np.asarray(V) - center) * scale
        return Q[..., [0, 2, 1]] * np.array([1, -1, 1]) if zup else Q
    return dict(mesh=mesh, balls=balls, sockets=sockets, joints=joints,
                parent=parent, root=root, scale=scale, xform=xform)


def check_rig(rig, samples=12):
    """Per joint: ball/socket sizes (mm), snap interference, swing range, and
    the closest the child gets to the parent while swinging through that range
    (vertex distances, limb + neck vs limb + cup)."""
    s = rig["scale"]; rows = []
    for child, j in rig["joints"].items():
        par = j["parent"]; p = j["pivot"]; d = j["axis"]
        used = lambda VF: np.asarray(VF[0])[np.unique(np.asarray(VF[1]).ravel())]  # drop orphaned verts
        cv = used(rig["mesh"][child])       # the limb body (the neck meeting the rim is the stop)
        pv = np.vstack([used(rig["mesh"][par])] + [used(c) for c in rig["sockets"][par]])
        near = np.linalg.norm(pv - p, axis=1) < j["ro"] + 4 * j["rb"]
        pv = pv[near]; cv = cv[np.linalg.norm(cv - p, axis=1) < j["ro"] + 4 * j["rb"]]
        # swing about axes perpendicular to d, up to the socket's limit
        a = np.cross(d, [0, 0, 1.0]);  a = a if np.linalg.norm(a) > 1e-6 else np.cross(d, [0, 1.0, 0])
        a /= np.linalg.norm(a); b = np.cross(d, a)
        worst = 1e9
        for ang in (0.0, j["swing"]):
            for k in range(samples if ang else 1):
                t = 2 * np.pi * k / samples
                R = rot_axis_m(np.cos(t) * a + np.sin(t) * b, np.radians(ang))
                q = (cv - p) @ R.T + p
                if len(q) and len(pv):
                    dd = np.sqrt(((q[:, None, :] - pv[None, :, :]) ** 2).sum(-1)).min()
                    worst = min(worst, dd)
        rows.append((par, child, 2 * j["rb"] * s, 2 * j["rin"] * s,
                     2 * j["rin"] * np.sin(j["th0"]) * s, (j["rb"] - j["rin"] * np.sin(j["th0"])) * s,
                     j["swing"], worst * s))
    return rows


def rot_axis_m(ax, ang):
    ax = ax / (np.linalg.norm(ax) or 1); c, s_ = np.cos(ang), np.sin(ang); x, y, z = ax
    return np.array([[c + x*x*(1-c), x*y*(1-c) - z*s_, x*z*(1-c) + y*s_],
                     [y*x*(1-c) + z*s_, c + y*y*(1-c), y*z*(1-c) - x*s_],
                     [z*x*(1-c) - y*s_, z*y*(1-c) + x*s_, c + z*z*(1-c)]])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump"); ap.add_argument("outdir")
    ap.add_argument("--height-mm", type=float, default=180.0)
    ap.add_argument("--clearance-mm", type=float, default=0.3)
    ap.add_argument("--ball-frac", type=float, default=0.58,
                    help="ball radius as a fraction of the child's cross-section")
    ap.add_argument("--snap-mm", type=float, default=0.3,
                    help="how much the ball is wider than the socket opening, per side")
    ap.add_argument("--gap-mm", type=float, default=1.5,
                    help="how far both limb ends are cut back from the joint")
    ap.add_argument("--smooth", type=int, default=0,
                    help="optional Taubin smoothing passes (0 = raw crisp low-poly)")
    ap.add_argument("--zup", action="store_true")
    ap.add_argument("--check", action="store_true", help="print the joint check")
    args = ap.parse_args()

    parts = [json.loads(l) for l in open(args.dump) if l.strip()]
    os.makedirs(args.outdir, exist_ok=True)
    rig = build_rig(parts, args.height_mm, args.clearance_mm, args.ball_frac,
                    args.snap_mm, args.smooth, args.zup, args.gap_mm)
    xf = rig["xform"]
    allm = []
    for t, (V, F) in rig["mesh"].items():
        meshes = [(xf(V), F)] + [(xf(v), f) for v, f in rig["balls"][t] + rig["sockets"][t]]
        write_stl(os.path.join(args.outdir, f"part_{t.replace('_', '-')}.stl"), meshes)
        allm += meshes
    write_stl(os.path.join(args.outdir, "figure_jointed.stl"), allm)
    print(f"{len(rig['mesh'])} parts, {len(rig['joints'])} ball joints; "
          f"{args.height_mm:.0f} mm, clearance {args.clearance_mm} mm -> {args.outdir}")
    if args.check:
        print(f"{'joint':>12} {'ball':>6} {'cavity':>7} {'opening':>8} {'snap':>6} {'swing':>6} {'min gap':>8}")
        for par, ch, b, cav, op, sn, sw, g in check_rig(rig):
            flag = "" if sn > 0.15 and g > 0.25 else "  <-- check"
            print(f"{par + '>' + ch:>12} {b:6.2f} {cav:7.2f} {op:8.2f} {sn:6.2f} {sw:5.0f}° {g:8.2f}{flag}")
        print("(mm; snap = how much the ball is wider than the opening, per side;"
              " min gap = closest approach, child vs parent, across the swing)")


if __name__ == "__main__":
    main()
