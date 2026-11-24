# Character meshes → STL

Tools to pull the VF characters out as STL meshes you can open in Blender.

## The catch, up front

No models or ROM ship with this project, and none are stored anywhere here. Each
character's geometry lives inside **your own `vf.zip`** and is assembled and
posed by the game at runtime (head + torso + limbs placed by the skeleton every
frame). So these tools don't *convert a file that exists* — they **run the game**
against your ROM, pause on a posed fight frame, dump player 1's body, and turn
that into an STL.

You need the same things a normal build needs (C++20 toolchain, zlib, SDL3,
FreeType, `python3`) plus your own MAME `vf` set at `roms/vf.zip`.

STL also can't carry colour or keep the body parts separate — it's one merged
triangle mesh per character. The per-quad colours and the part breakdown are
still in the `.json` dump if you ever want them.

## Quick start

```sh
# every character -> build/models/*.stl (+ .json + a .png of each frame)
./tools/models/export_models.sh

# just one cast member by its select-cursor slot (0 = Akira)
./tools/models/export_models.sh --one 3

# just one secret character (1 Siba, 2 Jeff, 3 Takeru, 4 Rekka, 5 Score, 6 Aeon, 7 Dural)
./tools/models/export_models.sh --secret 7
```

Output lands in `build/models/` (override with `OUT=…`). For each character you
get `<name>.stl`, the raw `<name>.json`, and `<name>.png` — **open the PNG** to
see exactly which character and pose that STL is.

## About the slot → character order

The cast's select order is defined by bytes in the ROM, so this script can't
name the cast members for you ahead of time — it dumps **cursor slot 0..7** and
labels them `cast_slot0 … cast_slot7`. Slot 0 is always Akira; check each PNG to
see who the rest are, then rename the STLs. The **secret** characters are forced
directly (`VF_AKIRA`), so those are named correctly already.

## Tuning (expect to do a little)

Because this has to drive the game headless, the frame timings are first
guesses. If a mesh is empty, or caught mid-swing instead of a clean idle stance,
open the `.png` and nudge the knobs at the top of `export_models.sh`:

- `BOOT`, `TO_SELECT`, `TO_FIGHT` — frames to wait through each phase.
- `DUMP_FRAME` — the absolute frame the body is dumped on (the `.png` is that frame).
- `CONFIRM_MASK` — the select-confirm button (`01` punch, `02` kick, `04` guard).

All in the game's own 57.5 fps frames.

## Doing it by hand

The export is just these two pieces, if you'd rather drive the game yourself
(e.g. dump a specific move's pose):

1. Run any build with `VF_DUMP_FIGHTER=<frame>:out.json` set, with the fighter
   you want as **player 1**, and let it reach `<frame>`. The engine writes one
   JSON line per body part of P1 at that frame (points, quads, colours, and the
   matrix that poses each part). See `src/machine/model1_video.cpp`.
2. Convert:

   ```sh
   python3 tools/models/dump_to_stl.py out.json out.stl
   ```

## dump_to_stl.py options

- `--scale S`  multiply every coordinate by `S`.
- `--zup`      rotate the game's Y-up to Blender's Z-up (`(x,y,z) → (x,-z,y)`).
- `--ascii`    write an ASCII STL instead of binary.

By default coordinates come out exactly as the game has them, so you may want to
rotate/scale once in Blender (or pass `--zup`). The units are the game's; a VF
character is a few hundred units tall.
