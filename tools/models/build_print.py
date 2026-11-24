#!/usr/bin/env python3
# Turn a VF_DUMP_FIGHTER dump into watertight, print-ready STL body parts.
#
# What it does, per body part in the dump:
#   - poses the part by its matrix (as drawn),
#   - welds coincident vertices (the dump repeats points),
#   - caps any open boundary loops with a fan, so each part is a closed
#     (watertight) solid a slicer will accept,
#   - scales the whole figure to a target height.
#
# It writes one binary STL per part (so the parts stay separate and movable in
# Blender) and one combined STL of the whole posed figure. STL carries no
# colour; the parts stay separate instead.
#
#   build_print.py dump.json OUTDIR [--height-mm 180] [--zup] [--combined-only]
#
# numpy only. Safe under `python3 -I`.

import argparse, json, os, struct, sys
import numpy as np


def weld(pts, eps):
    pts = np.asarray(pts, float)
    key = np.round(pts / eps).astype(np.int64)
    seen = {}
    remap = np.empty(len(pts), int)
    V = []
    for i, k in enumerate(map(tuple, key)):
        j = seen.get(k)
        if j is None:
            j = seen[k] = len(V)
            V.append(pts[i])
        remap[i] = j
    return np.array(V) if V else np.zeros((0, 3)), remap


def pose(m, P):
    A = np.array(m[:9]).reshape(3, 3).T
    return np.asarray(P) @ A.T + np.array(m[9:12])


def smooth_taubin(V, F, iters, lam=0.5, mu=-0.53):
    """Low-pass the vertex noise of the 1993 game mesh without shrinking it
    (Taubin's lambda/mu): smooths the little bumps, keeps the low-poly form."""
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
        V = V + lam * lap(V)
        V = V + mu * lap(V)
    return V


def faces_of(quads, remap):
    F = []
    for q in quads:
        if len(q) < 4:
            continue
        a, b, c, d = (remap[x] for x in q[:4])
        for tri in ((a, b, c), (a, c, d)):
            if len(set(tri)) == 3:
                F.append(list(tri))
    return F


def boundary_loops(F):
    # Boundary half-edges (reverse absent), decomposed into cycles by consuming
    # every such half-edge exactly once (an Eulerian walk on the boundary graph).
    from collections import defaultdict, deque
    count = defaultdict(int)
    for f in F:
        for u, v in ((f[0], f[1]), (f[1], f[2]), (f[2], f[0])):
            count[(u, v)] += 1
    out = defaultdict(deque)
    for (u, v), c in count.items():
        if count.get((v, u), 0) == 0:
            out[u].append(v)
    loops = []
    for s in list(out):
        while out[s]:
            loop = []
            u = s
            while out[u]:
                v = out[u].popleft()
                loop.append(u)
                u = v
                if u == s:
                    break
            if len(loop) >= 3:
                loops.append(loop)
    return loops


def open_half_edges(F):
    from collections import defaultdict
    count = defaultdict(int)
    for f in F:
        for u, v in ((f[0], f[1]), (f[1], f[2]), (f[2], f[0])):
            count[(u, v)] += 1
    return [(u, v) for (u, v), c in count.items() if count.get((v, u), 0) == 0]


def cap(V, F):
    """Close the part: fan each boundary loop, iterating to catch loops that
    share junction vertices, then fan any stragglers to their centroid."""
    V = list(V)
    for loop in boundary_loops(F):
        c = len(V)
        V.append(np.mean([V[i] for i in loop], axis=0))
        n = len(loop)
        for i in range(n):
            a, b = loop[i], loop[(i + 1) % n]
            F.append([b, a, c])  # opposite winding to the boundary half-edge
    return np.array(V), F


def is_closed(F):
    from collections import Counter
    ec = Counter()
    for f in F:
        for u, v in ((f[0], f[1]), (f[1], f[2]), (f[2], f[0])):
            ec[frozenset((u, v))] += 1
    return sum(1 for c in ec.values() if c != 2) == 0


def tri_normal(a, b, c):
    n = np.cross(b - a, c - a)
    L = np.linalg.norm(n)
    return n / L if L else n


def write_stl(path, V, F):
    with open(path, "wb") as f:
        f.write(b"VF1 recomp part".ljust(80, b"\0"))
        f.write(struct.pack("<I", len(F)))
        for a, b, c in F:
            A, B, C = V[a], V[b], V[c]
            f.write(struct.pack("<3f", *tri_normal(A, B, C)))
            for P in (A, B, C):
                f.write(struct.pack("<3f", *P))
            f.write(struct.pack("<H", 0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("outdir")
    ap.add_argument("--height-mm", type=float, default=180.0)
    ap.add_argument("--zup", action="store_true", help="Z-up for printing/Blender")
    ap.add_argument("--eps", type=float, default=1e-4, help="weld epsilon (model units)")
    ap.add_argument("--smooth", type=int, default=12,
                    help="Taubin smoothing passes to clean the mesh noise (0 = raw)")
    ap.add_argument("--combined-only", action="store_true")
    args = ap.parse_args()

    parts = [json.loads(l) for l in open(args.dump) if l.strip()]
    os.makedirs(args.outdir, exist_ok=True)

    # pose + weld every part; gather for global scaling
    processed = []
    allpts = []
    for p in parts:
        W = pose(p["m"], p["p"])
        V, remap = weld(W, args.eps)
        F = faces_of(p.get("q", []), remap)
        if len(F) == 0:
            continue
        V = smooth_taubin(V, F, args.smooth)
        V, F = cap(V, F)
        processed.append((p["tag"], V, F))
        allpts.append(V)

    allpts = np.vstack(allpts)
    lo, hi = allpts.min(0), allpts.max(0)
    height = (hi[1] - lo[1]) or 1.0           # model Y is up
    scale = args.height_mm / height
    center = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])

    def xform(V):
        Q = (V - center) * scale
        if args.zup:
            Q = Q[:, [0, 2, 1]] * np.array([1, -1, 1])  # Y-up -> Z-up
        return Q

    combined_V, combined_F = [], []
    closed_n = 0
    for tag, V, F in processed:
        Q = xform(V)
        if is_closed(F):
            closed_n += 1
        if not args.combined_only:
            name = tag.replace("_", "-")
            write_stl(os.path.join(args.outdir, f"part_{name}.stl"), Q, F)
        base = len(combined_V)
        combined_V.extend(Q)
        combined_F.extend([[a + base, b + base, c + base] for a, b, c in F])

    write_stl(os.path.join(args.outdir, "figure_combined.stl"),
              np.array(combined_V), combined_F)
    print(f"{len(processed)} parts, {closed_n} watertight; "
          f"figure {args.height_mm:.0f} mm tall -> {args.outdir}")


if __name__ == "__main__":
    main()
