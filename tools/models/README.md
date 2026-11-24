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
  pose matrices (each part's matrix translation *is* its joint pivot) and builds
  13 assembled ball joints: neck, shoulders, elbows, wrists, hips, knees, ankles.
  At each joint both limbs get a flat planar cut (triangles are sliced, so the
  cut is capped flat): the parent is cut at the back of the socket cavity, so
  the cavity is empty, and the socket cup's wall fuses to the limb; the child is
  cut just past the socket shell and carries a ball on a neck.

  Defaults (all tunable): `--clearance-mm 0.3` (ball to cavity), `--snap-mm 0.3`
  (the ball is that much wider than the socket opening, per side, so it clicks
  in and holds), `--gap-mm 1.5` (limb ends cut back from the joint),
  `--ball-frac 0.58` (ball size vs the limb). Balls are at least 3.2 mm.

  `--check` prints, per joint: ball, cavity and opening sizes, the snap, the
  swing range (35–44° each way before the neck meets the rim; twist is free),
  and the closest the two limbs get to each other while swinging through that
  range. Defaults pass on Akira with every joint ≥ 0.44 mm clear. Untested on a
  real printer: print one joint first and adjust clearance/snap to your printer.

- **`hidden_joints.py dump.json OUTDIR`** — the finished figure: joints hidden
  **inside** the body, so it looks like the character with thin seam lines
  (needs `pip install manifold3d` for exact solid booleans). Each joint is a
  spherical seam centred on the pivot (sized to swallow where the two parts
  overlapped in the game model): the moving part owns everything inside the
  sphere, the parent the rest, `--seam-mm 0.4` apart, so it turns without
  colliding. Inside, the parent's ball on a post is captive in a cavity in
  the moving part, with a channel kept within the parent's cover so it never
  shows. **Print‑in‑place**: it prints assembled (`figure_assembled.stl`), the
  balls already captive; resin or a well‑tuned FDM printer for the 0.3 mm
  clearances. `--check` reports how far each joint swings before the parts
  touch; in Akira's crouched stance knees reach ~10–20°, elbows and the right
  shoulder up to 70°, others ~20–40°.

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
