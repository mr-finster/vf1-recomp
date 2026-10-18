#!/bin/sh
# The secret characters' select icons (assets/games/vf/select_icons/NAME.png, 56 x 64): their heads rendered by the
# game itself (model1script, from your own vf.zip's regions in build/vf), on player 2's side of the
# select in Akira's place, turned a little further toward the camera than the portrait; then fitted
# into Akira's icon's frame by make_icons.py. Made from the game, so they ship with the downloads,
# not in the repository.   tools/games/vf/select_icons/make.sh [OUT=assets/games/vf/select_icons]
set -e
cd "$(dirname "$0")/../../../.."
OUT=${1:-assets/games/vf/select_icons}
W=build/select_icons; mkdir -p $W $OUT
run() {   # NAME SLOT CAM SCRIPT
	mkdir -p $W/o/$1
	env M1_FREEPLAY=1 M1_AKIRA=$2 M1_CAM="$3" M1_SCALE=$5 build/bin/model1script build/vf $4 $W/o/$1 >/dev/null
}
# player 2 on Akira's place; and player 1's select with Akira's icon in view, for its picture
{ printf 'wait 600\npress 0 01 5\nwait 60\npress 0 20 5\nwait 200\n'; for i in 1 2 3 4 5 6 7; do printf 'press 2 80 4\nwait 20\n'; done; printf 'wait 60\nshot p\n'; } > $W/p2.txt
printf 'wait 600\npress 0 01 5\nwait 60\npress 0 10 5\nwait 200\nicon akira\n' > $W/icon.txt
python3 -c "
import sys; sys.path.insert(0,'tools/games/vf/select_icons')
from cams import icon_cam
for a in range(8): print(a, icon_cam(a, dc=2500))" | while read a cam; do run ic_$a $a "$cam" $W/p2.txt 2 & done; wait
run ic_plate 0 "0 0 100 0 0 0" $W/p2.txt 2
mkdir -p $W/o/icon; env M1_FREEPLAY=1 build/bin/model1script build/vf $W/icon.txt $W/o/icon >/dev/null   # (Akira's real icon, the reference)
python3 tools/games/vf/select_icons/make_icons.py $W fit
python3 tools/games/vf/select_icons/make_icons.py $W make "$PWD/$OUT"
rm -f $OUT/akira.png   # (the fit's check: Akira has his own)
