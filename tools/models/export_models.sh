#!/usr/bin/env bash
# Export VF characters as 3D-printable STL meshes (optionally ball-jointed),
# driving the headless game against YOUR OWN roms/vf.zip.
#
# The models live inside the ROM and are assembled/posed at runtime; nothing
# ships with the project. This reaches a round-1 idle frame for a character,
# dumps player 1's body with the engine's VF_DUMP_FIGHTER debug path, and runs:
#   dump_to_stl.py   - one merged STL (quick look)
#   build_print.py   - per-part watertight solids + a combined figure
#   ball_joints.py   - per-part ball-and-socket parts + a jointed figure
#
# Prerequisites: a C++20 toolchain, zlib, python3 (numpy for the print tools),
# and your own MAME "vf" set at roms/vf.zip. No SDL needed (runs on the
# interpreter). Nothing here downloads game data.
#
#   ./tools/models/export_models.sh                 # all characters -> build/models
#   ./tools/models/export_models.sh --one 0         # one cast member by select slot
#   ./tools/models/export_models.sh --secret 7      # one secret character (7 = Dural)
#
# Slot 0 is always Akira; the rest of the cast order is ROM-defined, so each
# character also gets a screenshot (<name>.png) to confirm who you got.

set -euo pipefail

# ---- validated timing (the game's 57.5 fps clock) -------------------------
BOOT=${BOOT:-260}; TO_SELECT=${TO_SELECT:-300}; DUMP_FRAME=${DUMP_FRAME:-749}
HEIGHT_MM=${HEIGHT_MM:-180}; CLEARANCE_MM=${CLEARANCE_MM:-0.3}; SMOOTH=${SMOOTH:-12}
# ---------------------------------------------------------------------------

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REGIONS=${REGIONS:-"$REPO/build/vf"}
OUT=${OUT:-"$REPO/build/models"}
VFSCRIPT="$REPO/build/bin/vfscript"
HERE="$REPO/tools/models"

SECRET_NAMES=("" "siba" "jeff" "takeru" "rekka" "score" "aeon" "dural")

mode="all"; sel=""
case "${1:-}" in
	--one)    mode="one";    sel="${2:?slot 0-7}";;
	--secret) mode="secret"; sel="${2:?secret 1-7}";;
	--help|-h) sed -n '2,33p' "$0"; exit 0;;
	"") ;;
	*) echo "unknown option: $1 (try --help)" >&2; exit 2;;
esac

# ---- build regions + vfscript (interpreter; no AOT recompile, no SDL) ------
if [ ! -f "$REGIONS/maincpu.bin" ]; then
	[ -f "$REPO/roms/vf.zip" ] || { echo "error: put your MAME 'vf' set at roms/vf.zip" >&2; exit 1; }
	echo "==> building regions from roms/vf.zip"
	( cd "$REPO" && python3 tools/romset.py roms/vf.zip build/vf )
fi
if [ ! -x "$VFSCRIPT" ]; then
	echo "==> building vfscript (interpreter build)"
	# stub the AOT-recompiled table so the link succeeds without a coverage run;
	# vfscript runs on the V60 interpreter unless VF_REC is set, so this is enough.
	mkdir -p "$REPO/build/gen"
	: > "$REPO/build/gen/vf_prog.txt"
	cp "$REPO/src/rt/rec_table_none.cpp" "$REPO/build/gen/vf_rec.cpp"
	( cd "$REPO" && make -j"$(nproc)" SDL_LIBS= build/bin/vfscript )
fi
mkdir -p "$OUT"

# dump + convert one character.  $1 cursor slot, $2 name, $3 VF_AKIRA (optional)
run_one() {
	local ch="$1" name="$2" akira="${3:-}" dir="$OUT/.work_$2"
	mkdir -p "$dir"
	cat > "$dir/s.txt" <<EOF
wait $BOOT
press 0 01 4
press 0 10 4
wait $TO_SELECT
poke8 50749c $(printf '%x' "$ch")
press 1 01 4
poke8 50749c $(printf '%x' "$ch")
press 1 01 4
wait 141
shot $name
wait 20
EOF
	echo "==> $name (slot $ch${akira:+, VF_AKIRA=$akira}) dump@$DUMP_FRAME"
	(
		cd "$REPO"
		export VF_PCMODE=1 VF_FREEPLAY=1
		export VF_DUMP_FIGHTER="$DUMP_FRAME:$OUT/$name.json"
		[ -n "$akira" ] && export VF_AKIRA="$akira"
		"$VFSCRIPT" "$REGIONS" "$dir/s.txt" "$OUT" >/dev/null 2>&1
	) || true
	if [ -s "$OUT/$name.json" ]; then
		python3 -I "$HERE/dump_to_stl.py" "$OUT/$name.json" "$OUT/$name.stl" --zup
		python3 -I "$HERE/build_print.py" "$OUT/$name.json" "$OUT/${name}_parts"  --height-mm "$HEIGHT_MM" --smooth "$SMOOTH" --zup
		python3 -I "$HERE/ball_joints.py" "$OUT/$name.json" "$OUT/${name}_joints" --height-mm "$HEIGHT_MM" --clearance-mm "$CLEARANCE_MM" --smooth "$SMOOTH" --zup
	else
		echo "   !! no dump for $name — check $OUT/$name.png and retune BOOT/TO_SELECT/DUMP_FRAME" >&2
	fi
	rm -rf "$dir"
}

case "$mode" in
	one)    run_one "$sel" "slot$sel";;
	secret) run_one 0 "secret_${SECRET_NAMES[$sel]:-$sel}" "$sel";;
	all)
		for ch in 0 1 2 3 4 5 6 7; do run_one "$ch" "cast_slot$ch"; done
		for w in 1 2 3 4 5 6 7; do run_one 0 "secret_${SECRET_NAMES[$w]}" "$w"; done
		;;
esac
echo; echo "Done -> $OUT  (check each .png to label the cast; _parts = solids, _joints = ball-jointed)"
