#!/usr/bin/env bash
# Every character as an exact statue: full_extract dump -> statue.py -> check -> renders -> zip.
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$REPO"
DUMPS=${1:-build/exports/dumps_full}; OUT=${2:-build/exports/statues}; mkdir -p "$OUT"
SUM="$OUT/SUMMARY.md"
printf '# Statues (exact game models, one solid each)\n\n| Character | Result | Size (mm) | Volume | Thinnest walls |\n|---|---|---|---|---|\n' > "$SUM"
for n in akira pai lau wolf jeffry kage sarah jacky dural; do
	D="$OUT/$n"; mkdir -p "$D"
	python3 tools/models/statue.py "$DUMPS/${n}_full.json" "$D" > "$D/build.log" 2>&1; b=$?
	python3 - "$D" "$n" > "$D/check.log" 2>&1 <<'PY'
import sys, numpy as np
sys.path.insert(0, "tools/models"); import check_print as cp
from render_studio import render
from PIL import Image
D, n = sys.argv[1], sys.argv[2]
M, T = cp.load(f"{D}/statue.stl")
pieces = [x for x in M.decompose() if x.volume() > 1e-6]
w = cp.wall(M, T, rays=600)
bb = M.bounding_box()
print(f"pieces {len(pieces)} status {M.status().name} size {bb[3]-bb[0]:.0f} x {bb[4]-bb[1]:.0f} x {bb[5]-bb[2]:.0f} vol {M.volume()/1000:.1f}")
print(f"wall: 1st percentile {np.percentile(w,1):.2f} mm, {100*(w<0.45).mean():.1f}% of surface under 0.45 mm")
Tf = T.astype(float)
for nm, yaw in (("hero", 0.55), ("front", -0.9), ("back", 2.6)):
    render(Tf, f"{D}/{n}_{nm}.png", yaw=yaw, pitch=0.12, res=1000)
ims = [Image.open(f"{D}/{n}_{x}.png") for x in ("hero", "front", "back")]
s = Image.new("RGB", (3000, 1000)); [s.paste(im, (i * 1000, 0)) for i, im in enumerate(ims)]
s.resize((1800, 600), Image.LANCZOS).save(f"{D}/{n}_sheet.png")
print("OK" if len(pieces) == 1 and M.status().name == "NoError" else "BAD")
PY
	cp -f "$DUMPS/$n.png" "$D/${n}_ingame.png"
	( cd "$D" && rm -f "../$n.zip" && zip -q "../$n.zip" statue.stl statue_on_base.stl ./*.png build.log check.log )
	res=$(grep -q '^OK' "$D/check.log" && [ $b -eq 0 ] && echo PASS || echo FAIL)
	sz=$(grep -o 'size [0-9]* x [0-9]* x [0-9]*' "$D/check.log" | sed 's/size //'); vol=$(grep -o 'vol [0-9.]*' "$D/check.log" | sed 's/vol //')
	wl=$(grep -o '[0-9.]*% of surface under 0.45 mm' "$D/check.log")
	echo "| $n | $res | $sz | ${vol} cm³ | $wl |" >> "$SUM"; echo "$n: $res"
done
( cd "$OUT" && rm -f all_statues.zip && zip -q all_statues.zip ./*.zip SUMMARY.md )
echo ALL DONE
