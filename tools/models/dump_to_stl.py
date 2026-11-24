#!/usr/bin/env python3
# Turn a VF_DUMP_FIGHTER dump into an STL mesh.
#
# The game's debug dumper (VF_DUMP_FIGHTER=frame[:file], read in
# src/machine/model1_video.cpp) writes player 1's body parts at one frame as
# JSON, one line per part:
#
#   {"rec":"0affff","tag":"18","poly":123,"m":[m0..m11],
#    "p":[[x,y,z],...],"q":[[a,b,c,d,col],...]}
#
#   m : the part's matrix as drawn. A world point is A*v + t, where A is the
#       column-major 3x3 from m[0..8] and t = m[9..11]. So
#         X = m0*x + m3*y + m6*z + m9
#         Y = m1*x + m4*y + m7*z + m10
#         Z = m2*x + m5*y + m8*z + m11
#   p : the part's own points, [x,y,z].
#   q : quads, four indices into p and a BGR555 colour. Each quad is two
#       triangles (a,b,c) and (a,c,d); triangle-type polygons come through with
#       the 4th point doubling the 3rd, so that second triangle is degenerate
#       and is dropped.
#
# This walks every part in one dump (= one character in one pose), poses each
# part by its matrix, triangulates the quads, and writes one binary STL with the
# whole body merged. STL carries no colour and no part names, so the per-quad
# colour in the dump is discarded here (the .json keeps it if you want it).
#
# Usage:
#   dump_to_stl.py INPUT.json OUTPUT.stl [--scale S] [--zup] [--ascii]
#
#   --scale S  multiply every coordinate by S (default 1).
#   --zup      rotate Y-up to Z-up for Blender (maps (x,y,z) -> (x,-z,y)).
#              Off by default: coordinates come out exactly as the game has them.
#   --ascii    write an ASCII STL instead of binary.
#
# Reads only the dump file you name; safe to run with `python3 -I`.

import argparse
import json
import struct
import sys


def load_parts(path):
    parts = []
    with open(path, "r") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                parts.append(json.loads(line))
            except json.JSONDecodeError as e:
                sys.exit(f"{path}:{lineno}: bad JSON line: {e}")
    if not parts:
        sys.exit(f"{path}: no parts found (is this a VF_DUMP_FIGHTER dump?)")
    return parts


def pose(m, p):
    """World point of local point p under the part's 12-float matrix m."""
    x, y, z = p
    return (
        m[0] * x + m[3] * y + m[6] * z + m[9],
        m[1] * x + m[4] * y + m[7] * z + m[10],
        m[2] * x + m[5] * y + m[8] * z + m[11],
    )


def orient(v, scale, zup):
    x, y, z = v[0] * scale, v[1] * scale, v[2] * scale
    if zup:
        return (x, -z, y)
    return (x, y, z)


def triangles(parts, scale, zup):
    """Yield (v0, v1, v2) world-space triangles for every quad of every part."""
    for part in parts:
        m = part.get("m")
        pts = part.get("p")
        quads = part.get("q", [])
        if not m or len(m) < 12 or not pts:
            continue
        world = [orient(pose(m, p), scale, zup) for p in pts]
        n = len(world)
        for q in quads:
            if len(q) < 4:
                continue
            a, b, c, d = q[0], q[1], q[2], q[3]
            if max(a, b, c, d) >= n or min(a, b, c, d) < 0:
                continue
            for i, j, k in ((a, b, c), (a, c, d)):
                tri = (world[i], world[j], world[k])
                if not degenerate(tri):
                    yield tri


def degenerate(tri):
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = tri
    # cross product of the two edges; zero-length normal -> no area -> drop it.
    ux, uy, uz = bx - ax, by - ay, bz - az
    vx, vy, vz = cx - ax, cy - ay, cz - az
    nx = uy * vz - uz * vy
    ny = uz * vx - ux * vz
    nz = ux * vy - uy * vx
    return (nx * nx + ny * ny + nz * nz) < 1e-20


def normal(tri):
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = tri
    ux, uy, uz = bx - ax, by - ay, bz - az
    vx, vy, vz = cx - ax, cy - ay, cz - az
    nx = uy * vz - uz * vy
    ny = uz * vx - ux * vz
    nz = ux * vy - uy * vx
    length = (nx * nx + ny * ny + nz * nz) ** 0.5
    if length == 0:
        return (0.0, 0.0, 0.0)
    return (nx / length, ny / length, nz / length)


def write_binary(path, tris):
    with open(path, "wb") as f:
        f.write(b"VF1 recomp character mesh".ljust(80, b"\0"))
        f.write(struct.pack("<I", len(tris)))
        for tri in tris:
            nx, ny, nz = normal(tri)
            f.write(struct.pack("<3f", nx, ny, nz))
            for v in tri:
                f.write(struct.pack("<3f", *v))
            f.write(struct.pack("<H", 0))


def write_ascii(path, tris):
    with open(path, "w") as f:
        f.write("solid vf1\n")
        for tri in tris:
            nx, ny, nz = normal(tri)
            f.write(f"  facet normal {nx:g} {ny:g} {nz:g}\n")
            f.write("    outer loop\n")
            for v in tri:
                f.write(f"      vertex {v[0]:g} {v[1]:g} {v[2]:g}\n")
            f.write("    endloop\n")
            f.write("  endfacet\n")
        f.write("solid vf1\n".replace("solid", "endsolid"))


def main(argv=None):
    ap = argparse.ArgumentParser(description="VF_DUMP_FIGHTER JSON -> STL")
    ap.add_argument("input", help="the dump JSON (one character, one frame)")
    ap.add_argument("output", help="the STL to write")
    ap.add_argument("--scale", type=float, default=1.0, help="scale factor")
    ap.add_argument("--zup", action="store_true", help="rotate Y-up to Z-up")
    ap.add_argument("--ascii", action="store_true", help="ASCII STL")
    args = ap.parse_args(argv)

    parts = load_parts(args.input)
    tris = list(triangles(parts, args.scale, args.zup))
    if not tris:
        sys.exit(f"{args.input}: no triangles produced")
    if args.ascii:
        write_ascii(args.output, tris)
    else:
        write_binary(args.output, tris)
    print(f"{args.output}: {len(parts)} parts, {len(tris)} triangles")


if __name__ == "__main__":
    main()
