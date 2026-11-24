# Character meshes → 3D-printable STL

Pull the VF characters out of the ROM as STL meshes you can open in Blender or a
slicer — as a plain mesh, as watertight per-part solids, or as a ball-jointed
figure you could print and pose.

## How it works (and the one catch)

No models or ROM ship with this project. Each character's geometry lives inside
**your own `vf.zip`** and is assembled and posed by the game at runtime. So
these tools *run the game* (headless, on the interpreter — no SDL, no GPU),
drive it to a round‑1 idle frame for a character, dump player 1's body with the
engine's `VF_DUMP_FIGHTER` debug path, and convert the dump.

You need: a C++20 toolchain, zlib, `python3` + `numpy`, and your own MAME `vf`
set at `roms/vf.zip` (the full set, including the `opr-147xx` board ROMs).

## One command

```sh
./tools/models/export_models.sh --one 0     # Akira (cursor slot 0) -> build/models
./tools/models/export_models.sh --secret 7  # Dural
./tools/models/export_models.sh             # all 8 cast + 7 secret characters
```

For each character `<name>` you get, under `build/models/`:

| Output | What it is |
| --- | --- |
| `<name>.png` | a screenshot of the dumped frame — **open it to confirm who you got** |
| `<name>.json` | the raw dump: every body part's points, quads, colours and pose matrix |
| `<name>.stl` | the whole figure as one merged mesh (quick look) |
| `<name>_parts/` | one **watertight solid STL per body part**, + `figure_combined.stl` |
| `<name>_joints/` | one **ball‑jointed STL per part**, + `figure_jointed.stl` |

Slot 0 is always Akira; the rest of the cast order is defined by the ROM, so the
cast parts are labelled `cast_slot0..7` — use the `.png` to name them. The secret
characters are forced directly, so they're named.

## The three converters (run individually if you like)

- **`dump_to_stl.py dump.json out.stl`** — merges the posed parts into one STL.
  `--scale`, `--zup` (Y‑up→Z‑up), `--ascii`.
- **`build_print.py dump.json OUTDIR`** — welds vertices and caps open boundary
  loops so each body part is a **watertight solid**, scaled to a target height.
  `--height-mm 180`, `--zup`. The limb segments are already closed in the ROM
  data; the torso/head/hands/feet shells get capped. (A handful of parts keep a
  few hairline sliver edges from the original triangle strips — any slicer
  auto‑repairs those.)

  The ROM meshes are already clean, intentional low‑poly — **flat‑shade** them
  for the crisp faceted look (in Blender they can otherwise import smooth‑shaded,
  which makes low‑poly geometry look lumpy; that's shading, not the mesh).
  `--smooth N` (default 0, off) is available for optional Taubin smoothing, but
  even a few passes round off the headband, face and fingers, so leave it off
  unless you specifically want a softened look.
- **`ball_joints.py dump.json OUTDIR`** — recovers the skeleton from the part
  pose matrices (each part's matrix translation *is* its joint pivot), then at
  every joint trims the two limb ends back and bridges the gap with a ball
  (on the child) seated in a socket cup (on the parent). `--height-mm 180`,
  `--clearance-mm 0.3`, `--ball-frac 0.45`, `--zup`.

  The ball and socket are emitted as overlapping watertight primitives — a slicer
  unions them, so no CSG library is needed. Joints are **assembled** (print the
  parts, click them together), not print‑in‑place. The ball size and 0.3 mm
  clearance are sensible defaults but want a real test print to dial in.

## Notes

- **Pose**: the dump is a snapshot of the round‑start idle stance (VF has no
  T‑pose). It's a rigid, correctly‑assembled figure, in the game's camera space;
  rescale/rotate to taste in Blender (`--zup` already orients Z‑up).
- **Scale**: a character is ~1 model unit tall; `--height-mm` scales the figure.
- **Colour**: STL carries none. The per‑part split keeps things organised; the
  per‑quad colours are still in the `.json` if you want them later.
- **Timing**: the driver reaches the idle frame with fixed waits that are known
  to work on the interpreter build. If a dump comes out empty or mid‑animation,
  open the `.png` and adjust `BOOT` / `TO_SELECT` / `DUMP_FRAME` at the top of
  `export_models.sh`.

## Doing it by hand

`VF_DUMP_FIGHTER=<frame>:out.json` on any build dumps player 1's body at that
frame (see `src/machine/model1_video.cpp`). Get your fighter on screen as P1,
reach the frame, then run any of the converters above on `out.json`.
