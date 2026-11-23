#!/bin/sh
# Virtua Fighter Recomp on the TrimUI Smart Pro / Brick Pro: the stock firmware's Apps/ (config.json) or a custom
# firmware's Linux games (game.json), as README.txt says. Your own vf.zip (and model1io.zip, if you have it) go in
# roms/ here; settings, saves, log.txt and launch.log stay in this folder. Select opens the menu; Quit is there.
APP="$(cd "$(dirname "$0")" && pwd)"
cd "$APP" || exit 1
LOG="$APP/launch.log"
CPUFREQ=/sys/devices/system/cpu/cpu0/cpufreq
OLD_GOV="$(cat "$CPUFREQ/scaling_governor" 2>/dev/null)"
PRINTK="$(cut -f1 /proc/sys/kernel/printk 2>/dev/null)"
restore() {
    "$APP/opaco" off >/dev/null 2>&1
    [ -n "$OLD_GOV" ] && echo "$OLD_GOV" 2>/dev/null > "$CPUFREQ/scaling_governor"
    [ -n "$PRINTK" ] && echo "$PRINTK" 2>/dev/null > /proc/sys/kernel/printk
    rm -f /tmp/stay_awake
    sync
}
trap restore EXIT
trap 'exit 143' INT TERM HUP QUIT
echo 1 2>/dev/null > /tmp/stay_awake   # (no auto-suspend while playing, as the firmware's own apps)
# The CPU at its most, 2000 MHz (cpu0 sets all four cores). /run/cpu-gioco keeps it while a custom firmware's menu or
# volume bar is open over the game (Mosaik's); the stock firmware ignores it.
echo userspace 2>/dev/null > "$CPUFREQ/scaling_governor"
echo 2000000 2>/dev/null > "$CPUFREQ/scaling_setspeed"
echo 2000000 2>/dev/null > /run/cpu-gioco
echo 1 2>/dev/null > /proc/sys/kernel/printk   # (kernel messages on the console slow the picture down)
# The PowerVR's EGL picks a surface with alpha, and the display engine then shows the picture transparent (a black
# screen with sound): its layer made opaque meanwhile, as it was at the end (restore)
"$APP/opaco" on >/dev/null 2>&1

# the first start's settings (settings-trimui.ini: 1x, no smooth motion); after that, the menu's
[ -f "$APP/settings.ini" ] || cp "$APP/settings-trimui.ini" "$APP/settings.ini"
export HOME="$APP"
export SDL_VIDEO_DRIVER=mali SDL_AUDIO_DRIVER=alsa
# Switches, each an empty file in this folder:
#   scale2       the 3D at 2x (992 x 768, a Brick Pro's own lines): sharper, about half the pictures a second
#   soundinline  the sound board in the game's frames, as on a PC (to compare: fewer pictures a second)
#   profile      the timings in log.txt every two seconds (VF_PERF)
#   bench        the scripted fight of start_game.txt as it is, with soundinline and with scale2, about a minute and a
#                half each (don't touch the pad); bench.txt has the results, bench-<name>.log each run's log. The file
#                is deleted first, so the next start is a normal game.
[ -f "$APP/scale2" ] && export VF_SCALE2=1
[ -f "$APP/soundinline" ] && export VF_SOUND_INLINE=1
[ -f "$APP/profile" ] && export VF_PERF=1
{
    echo "=== $(date) $(uname -r) $(uname -m) ${TRIMUI_MODEL:-}"
    echo "CPU: $(cat "$CPUFREQ/scaling_governor" 2>/dev/null) (was $OLD_GOV), $(cat "$CPUFREQ/scaling_cur_freq" 2>/dev/null) kHz"
    echo "switches: $(cd "$APP" && ls scale2 soundinline profile bench 2>/dev/null | tr '\n' ' ')"
    [ -f "$APP/roms/vf.zip" ] || echo "roms/vf.zip missing: put your own vf.zip in $APP/roms"
} > "$LOG"
if [ ! -x "$APP/vf" ]; then
    echo "ERROR: vf missing or not executable in $APP (copy the whole folder again)" >> "$LOG"
elif [ -f "$APP/bench" ]; then
    rm -f "$APP/bench" "$APP"/bench-*.log
    echo "bench: $(date)" > "$APP/bench.txt"
    for cfg in base soundinline scale2; do
        E=""
        [ $cfg = soundinline ] && E="VF_SOUND_INLINE=1"
        [ $cfg = scale2 ] && E="VF_SCALE2=1"
        echo "bench $cfg: $E" >> "$LOG"
        env $E VF_PERF=1 VF_SCRIPT="$APP/start_game.txt" VF_QUIT_AFTER=4000 "$APP/vf" >> "$LOG" 2>&1
        echo "exit $?" >> "$LOG"
        cp "$APP/log.txt" "$APP/bench-$cfg.log" 2>/dev/null
        # the fight's last lines (one each 120 passes): the game's frames and the pictures a second, passes a second,
        # the game's ms a frame, the picture's ms
        grep '^Virtua Fighter ' "$APP/log.txt" | tail -8 | sed 's/.* \([0-9.]*\) passes\/s *\([0-9.]*\) shown\/s.*game \([0-9.]*\) ms\/frame.*picture \([0-9.]*\) ms.*game frames \([0-9.]*\)\/s.*/\1 \2 \3 \4 \5/' > /tmp/vf-bench
        awk -v name="$cfg" '{ a += $1; b += $2; g += $3; p += $4; f += $5; n++ }
            END { if (n) printf "%-11s %5.1f game frames/s, %5.1f pictures/s, %5.1f passes/s, game %5.2f ms a frame, picture %5.2f ms\n", name, f / n, b / n, a / n, g / n, p / n
                  else printf "%-11s no result (see bench-%s.log)\n", name, name }' /tmp/vf-bench >> "$APP/bench.txt"
    done
    cat "$APP/bench.txt" >> "$LOG"
else
    "$APP/vf" >> "$LOG" 2>&1
    echo "exit $?" >> "$LOG"
fi
