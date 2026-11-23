#!/bin/sh
# Checks launch.sh with BusyBox's sh, the consoles' shell, with a stand-in vf that says what it was given and writes
# the game's own perf lines (test-data-log.txt: VF_PERF lines from a run of this build in an arm64 container):
#   docker run --rm -v "$PWD/tools/trimui":/trimui:ro busybox sh /trimui/test-launch.sh
fail=0
check() { if eval "$2"; then echo "ok:   $1"; else echo "FAIL: $1"; fail=1; fi; }
A=/tmp/app
rm -rf $A && mkdir -p $A/roms
cp /trimui/launch.sh /trimui/settings-trimui.ini $A/ && : > $A/start_game.txt && : > $A/roms/vf.zip
printf '#!/bin/sh\nexit 0\n' > $A/opaco
cat > $A/vf <<'V'
#!/bin/sh
{ echo "args: $* env: INLINE=${VF_SOUND_INLINE:-} SCALE2=${VF_SCALE2:-} SCRIPT=${VF_SCRIPT:-} QUIT=${VF_QUIT_AFTER:-} PERF=${VF_PERF:-} VIDEO=${SDL_VIDEO_DRIVER:-} stay_awake=$([ -e /tmp/stay_awake ] && echo yes || echo no)"
  cat /trimui/test-data-log.txt; } > "$(dirname "$0")/log.txt"
V
chmod +x $A/launch.sh $A/opaco $A/vf

# a normal start: the game alone, the mali driver, the first start's settings
rm -f /tmp/stay_awake
sh $A/launch.sh 2>/dev/null
check "a normal start runs the game, with SDL's mali driver" "grep -q 'env: INLINE= SCALE2= SCRIPT= QUIT= PERF= VIDEO=mali' $A/log.txt"
check "the first start's settings copied (1x)" "grep -q '^scale = 1' $A/settings.ini"
check "the game's exit in launch.log" "grep -q '^exit 0' $A/launch.log"
check "no /tmp/stay_awake while the game runs: the console may still sleep on its own (the stock firmware and Mosaik read it)" "grep -q 'stay_awake=no' $A/log.txt"

# switches
: > $A/scale2; : > $A/soundinline; sh $A/launch.sh 2>/dev/null; rm -f $A/scale2 $A/soundinline
check "the scale2 and soundinline files reach the game" "grep -q 'env: INLINE=1 SCALE2=1 SCRIPT= ' $A/log.txt"
check "launch.log names them" "grep -q 'switches: scale2 soundinline' $A/launch.log"

# bench: three runs, the averages of the log's lines, and the file gone
: > $A/bench; sh $A/launch.sh 2>/dev/null
check "bench.txt has a line of numbers for each of the three" "[ \$(grep -cE '^(base|soundinline|scale2) +[0-9.]+ game frames/s, +[0-9.]+ pictures/s, +[0-9.]+ passes/s, game +[0-9.]+ ms a frame, picture +[0-9.]+ ms' $A/bench.txt) = 3 ]"
want=$(awk '{ for (i = 1; i <= NF; i++) { if ($i == "shown/s") s += $(i - 1); if ($i == "frames") f += $(i + 1) + 0 } n++ } END { printf "%5.1f game frames/s, %5.1f pictures/s", f / n, s / n }' /trimui/test-data-log.txt)
check "the numbers are the log's averages ($want)" "grep -q \"$want\" $A/bench.txt"
check "the runs got their settings" "grep -q 'env: INLINE= SCALE2= SCRIPT=$A/start_game.txt QUIT=4000 PERF=1' $A/bench-base.log && grep -q 'env: INLINE=1 SCALE2= ' $A/bench-soundinline.log && grep -q 'env: INLINE= SCALE2=1 ' $A/bench-scale2.log"
check "the bench file is gone (the next start is a game)" "[ ! -f $A/bench ]"

# no game: said in launch.log
rm -f $A/vf; sh $A/launch.sh 2>/dev/null
check "without vf, launch.log says so" "grep -q 'ERROR: vf missing' $A/launch.log"

[ $fail = 0 ] && echo "all passed" || echo "SOME FAILED"
exit $fail
