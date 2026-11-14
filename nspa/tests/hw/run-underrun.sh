#!/bin/sh
# Underrun behaviour of an ALSA device: today's stop-and-restart against a spare period with the
# stream never stopped (see underrun_probe.c). Investigation only; nothing here touches JACK.
#
#   nspa/tests/hw/run-underrun.sh [device=hw:U44]
#
# Refuses to run if anything has the device's PCMs open (it never stops a server itself). The
# probe writes zeros only; on top of that, every playback control the card has is set to 0 and
# muted for the run and restored afterwards (a card without controls, like the U-44, has nothing
# to mute: the zeros keep it silent). The probe runs SCHED_FIFO, as JACK's driver thread does.
#
# Environment: UNDERRUN_CONFIGS (mode:queue, default "stop:2 stop:3 spare:2"), UNDERRUN_PERIODS (default "128 256"), UNDERRUN_LATE (periods slept, default
# "0.5 0.9 1.2 1.6 2.2"), UNDERRUN_SECONDS (per run, default 15), UNDERRUN_EVERY (cycles between
# injections, default 100), UNDERRUN_PRIO (default 80), UNDERRUN_OUT (default /tmp/underrun-<card>).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
dev="${1:-hw:U44}"
card="${dev#hw:}"; card="${card%%,*}"
out="${UNDERRUN_OUT:-/tmp/underrun-$card}"
periods="${UNDERRUN_PERIODS:-128 256}"
lates="${UNDERRUN_LATE-0.5 0.9 1.2 1.6 2.2}"   # set but empty: natural lateness only
secs="${UNDERRUN_SECONDS:-15}"
every="${UNDERRUN_EVERY:-100}"
prio="${UNDERRUN_PRIO:-80}"
configs="${UNDERRUN_CONFIGS:-stop:2 stop:3 spare:2}"

[ -e "/proc/asound/$card" ] || { echo "no card $card"; exit 1; }
idx=$(readlink "/proc/asound/$card" | sed 's/^card//')
busy=$(fuser /dev/snd/pcmC"${idx}"D*p /dev/snd/pcmC"${idx}"D*c 2>/dev/null)
[ -z "$busy" ] || { echo "$dev is in use (pids:$busy); stop what holds it first"; exit 1; }

mkdir -p "$out"
probe="$out/underrun_probe"
cc -O2 -Wall -o "$probe" "$here/underrun_probe.c" -lasound || exit 1

# Mute every playback control (restored on exit, whatever happens).
state="$out/mixer.state"
restore() { [ -s "$state" ] && alsactl -f "$state" restore "$idx" >/dev/null 2>&1; }
if amixer -c "$idx" scontrols >/dev/null 2>&1 && [ -n "$(amixer -c "$idx" scontrols)" ]; then
    alsactl -f "$state" store "$idx" || { echo "cannot save the mixer state; not running"; exit 1; }
    trap restore EXIT INT TERM
    amixer -c "$idx" scontrols | sed -n "s/^Simple mixer control '\(.*\)',\([0-9]*\)$/\1,\2/p" |
    while IFS= read -r ctl; do
        caps=$(amixer -c "$idx" sget "$ctl" | sed -n 's/^ *Capabilities: //p')
        case " $caps " in *" pvolume"*) amixer -q -c "$idx" sset "$ctl" 0% ;; esac
        case " $caps " in *" pswitch"*) amixer -q -c "$idx" sset "$ctl" mute ;; esac
    done
    echo "playback controls of $card muted (restored at the end)"
else
    echo "$card has no mixer controls: silent by the probe's zeros"
fi

rt="chrt -f $prio"
$rt true 2>/dev/null || { echo "no SCHED_FIFO $prio allowed: running at normal priority"; rt=""; }

summary="$out/summary.txt"
printf '%-6s %-8s %-5s %6s %5s %6s %8s %8s %7s %9s %10s %6s %8s\n' period config late cycles late stops forwards beyond still cap_lost delay_med min_queue slots > "$summary"
for p in $periods; do
    for cfg in $configs; do
        mode=${cfg%:*}; q=${cfg#*:}
        for l in 0 $lates; do
            k=$every; [ "$l" = 0 ] && k=0
            log="$out/$mode-q$q-${p}-late$l.log"
            since=$(date +%s)
            $rt "$probe" -d "$dev" -p "$p" -m "$mode" -q "$q" -s "$secs" -k "$k" -l "$l" -o "$log"
            rc=$?
            journalctl -k --since "@$since" --no-pager -q 2>/dev/null | grep -i -E "snd|pcm|xrun|usb|hda" > "$log.kernel"
            awk -v p="$p" -v m="$mode:q$q" -v l="$l" -v rc="$rc" -v kern="$(wc -l < "$log.kernel")" '
                /^# summary:/ { s = $0 }
                /^# capture against/ { match($0, /\(([-0-9.]+) periods\)/, a); cap = a[1] }
                /^# least committed/ { match($0, /\(([-0-9.]+) periods\)/, b); mq = b[1] }
                /^# output delay/ { match($0, /median ([-0-9]+)/, d); dm = d[1] }
                /^# output slots/ { match($0, /= ([-+0-9]+) periods/, e); sl = e[1] }
                END {
                    match(s, /cycles ([0-9]+)/, c); match(s, /late ([0-9]+)/, d); match(s, /stopped ([0-9]+)/, e)
                    match(s, /forwards ([0-9]+)/, g); match(s, /beyond the spare ([0-9]+)/, h); match(s, /still past after one: ([0-9]+)/, i)
                    printf "%-6s %-8s %-5s %6s %5s %6s %8s %8s %7s %9s %10s %6s %8s%s%s\n", p, m, l, c[1], d[1], e[1], g[1], h[1], i[1], cap, dm, mq, sl,
                        (rc != 0 ? "  rc=" rc : ""), (kern > 0 ? "  kernel lines: " kern : "")
                }' "$log" >> "$summary"
        done
    done
done
cat "$summary"
