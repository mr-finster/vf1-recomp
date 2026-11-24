#!/usr/bin/env bash
# Overnight: every dumped character -> hidden-joint print-in-place figure,
# independent print check, studio renders, zip. Continues past failures.
#   tools/models/batch_export.sh [DUMPS_DIR] [OUT_DIR]
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$REPO"
DUMPS=${1:-build/exports/dumps}; OUT=${2:-build/exports}
ORDER="akira pai lau wolf jeffry kage sarah jacky siba jeff takeru rekka score aeon dural"
SUM="$OUT/SUMMARY.md"
echo "# Character exports" > "$SUM"; echo >> "$SUM"; echo "Started $(date -u '+%Y-%m-%d %H:%M UTC')" >> "$SUM"; echo >> "$SUM"
echo "| Character | Print check | Parts | Size (mm) | Build time |" >> "$SUM"; echo "|---|---|---|---|---|" >> "$SUM"
for n in $ORDER; do
	[ -s "$DUMPS/$n.json" ] || { echo "| $n | no dump | | | |" >> "$SUM"; continue; }
	D="$OUT/$n"; mkdir -p "$D"; t0=$(date +%s)
	if [ -s "$OUT/$n.zip" ] && [ -s "$D/check.log" ]; then      # resumable: already done
		st=$(grep -q '^PASS' "$D/check.log" && echo PASS || echo "FAIL (see $n/check.log)")
		echo "| $n | $st | $(ls "$D"/print/part_*.stl | wc -l) | $(grep -o 'figure [0-9]* x [0-9]* x [0-9]* mm' "$D/check.log" | head -1 | sed 's/figure //;s/ mm//') | (earlier run) |" >> "$SUM"
		echo "    $n: already done, $st"; continue
	fi
	echo "=== $n $(date -u +%H:%M)" 
	python3 tools/models/hidden_joints.py "$DUMPS/$n.json" "$D/print" --check > "$D/build.log" 2>&1; b=$?
	python3 tools/models/check_print.py "$D/print" --min-wall 0.45 > "$D/check.log" 2>&1; c=$?
	python3 - "$D" "$n" > "$D/render.log" 2>&1 <<'PY'
import sys, struct, numpy as np
sys.path.insert(0, "tools/models"); from render_studio import render
from PIL import Image
D, n = sys.argv[1], sys.argv[2]
with open(f"{D}/print/figure_assembled.stl", "rb") as f:
    f.read(80); k, = struct.unpack("<I", f.read(4)); raw = np.frombuffer(f.read(50 * k), np.uint8).reshape(k, 50)
T = raw[:, 12:48].copy().view(np.float32).reshape(-1, 3, 3).astype(float); T[..., 2] -= T[..., 2].min()
for nm, yaw, pit in (("hero", 0.55, 0.12), ("front", -0.9, 0.10), ("back", 2.6, 0.15)):
    render(T, f"{D}/{n}_{nm}.png", yaw=yaw, pitch=pit, res=1000)
ims = [Image.open(f"{D}/{n}_{x}.png") for x in ("hero", "front", "back")]
s = Image.new("RGB", (3000, 1000))
for i, im in enumerate(ims): s.paste(im, (i * 1000, 0))
s.resize((1800, 600), Image.LANCZOS).save(f"{D}/{n}_sheet.png")
PY
	cp -f "$DUMPS/$n.png" "$D/${n}_ingame.png" 2>/dev/null
	( cd "$D" && rm -f "../$n.zip" && zip -qr "../$n.zip" print/*.stl ./*.png build.log check.log )
	t=$(( ($(date +%s) - t0) / 60 ))
	status=$([ $b -eq 0 ] && [ $c -eq 0 ] && echo "PASS" || echo "FAIL (see $n/check.log)")
	parts=$(ls "$D"/print/part_*.stl 2>/dev/null | wc -l)
	size=$(grep -o 'figure [0-9]* x [0-9]* x [0-9]* mm' "$D/check.log" | head -1 | sed 's/figure //;s/ mm//')
	echo "| $n | $status | $parts | $size | ${t} min |" >> "$SUM"
	echo "    $n: $status (${t} min)"
done
echo >> "$SUM"; echo "Finished $(date -u '+%Y-%m-%d %H:%M UTC')" >> "$SUM"
( cd "$OUT" && rm -f all_characters.zip && zip -q all_characters.zip ./*.zip SUMMARY.md )
echo "ALL DONE"
