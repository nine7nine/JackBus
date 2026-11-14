#!/bin/sh
# The ALSA driver's wake timer (-w) on a real card: xruns, DSP load and the driver thread's
# context switches, with the timer off and on, at several period sizes. The fork's jackd runs
# isolated (nspa/run-isolated.sh); a client connected to system:capture_1 / system:playback_1
# writes zeros; every playback control of the card is muted for the run and restored.
# Take the card from a running jackdbus first: nspa/tests/hw/with-card.sh hw:PCH <this script>.
#
#   nspa/tests/hw/wake-timer.sh [card=PCH] [seconds=30] [periods="64 128 256"] [timers="off on adaptive"]
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$root/nspa/run-isolated.sh" "$0" "$@"
fi
card=${1:-PCH}; secs=${2:-30}; periods=${3:-64 128 256}; timers=${4:-off on adaptive}
idx=$(readlink "/proc/asound/$card" | sed 's/^card//')
[ -n "$idx" ] || { echo "SKIP: no card $card"; exit 0; }
work=$(mktemp -d /tmp/nspa-waketimer.XXXXXX)
cc -O2 -Wall ${NSPA_TEST_CFLAGS:-} -I"$root/common" -o "$work/client" "$root/nspa/tests/survivor_client.c" -L"$JACK_BUILD/common" -ljack || exit 1

state="$work/mixer.state"
if [ -n "$(amixer -c "$idx" scontrols 2>/dev/null)" ]; then
    alsactl -f "$state" store "$idx" || { echo "cannot save the mixer: not running"; exit 1; }
    trap 'alsactl -f "$state" restore "$idx" >/dev/null 2>&1' EXIT INT TERM
    amixer -c "$idx" scontrols | sed -n "s/^Simple mixer control '\(.*\)',\([0-9]*\)$/\1,\2/p" | while IFS= read -r c; do
        caps=$(amixer -c "$idx" sget "$c" | sed -n 's/^ *Capabilities: //p')
        case " $caps " in *" pvolume"*) amixer -q -c "$idx" sset "$c" 0% ;; esac
        case " $caps " in *" pswitch"*) amixer -q -c "$idx" sset "$c" mute ;; esac
    done
fi

ctxsw() {  # voluntary + involuntary context switches of jackd's threads
    cat /proc/$1/task/*/status 2>/dev/null | awk '/ctxt_switches/ { s += $2 } END { print s + 0 }'
}
printf '%-6s %-9s %7s %6s %9s %12s %s\n' period timer cycles xruns load_avg ctxsw_per_s timer_log
for p in $periods; do
    for w in $timers; do
        export JACK_DEFAULT_SERVER="nspa-wt-$$"
        jackd=$JACK_BUILD/linux/jackd; [ -x "$jackd" ] || jackd=$JACK_BUILD/jackd
        # the card is held for us by with-card.sh (device reservation on the session bus,
        # which this isolated namespace cannot reach): no reservation from jackd itself
        JACK_NO_AUDIO_RESERVATION=1 "$jackd" -n "$JACK_DEFAULT_SERVER" -R -P 70 -d alsa -d "hw:$card" -r 48000 -p "$p" -n 2 -w "$w" > "$work/jackd-$p-$w.log" 2>&1 &
        jd=$!
        i=0; until grep -q "ALSA: wake timer\|configuring for\|Acquire audio" "$work/jackd-$p-$w.log" 2>/dev/null || [ $i -gt 50 ]; do sleep 0.1; i=$((i+1)); done
        sleep 1
        "$work/client" "$secs" > "$work/client-$p-$w.log" 2>&1 &
        cl=$!
        sleep 2
        c0=$(ctxsw $jd); t0=$(date +%s)
        wait $cl
        c1=$(ctxsw $jd); t1=$(date +%s)
        kill $jd 2>/dev/null; wait $jd 2>/dev/null
        last=$(tail -1 "$work/client-$p-$w.log")
        cyc=$(echo "$last" | sed -n 's/.*cycles=\([0-9]*\).*/\1/p')
        xr=$(echo "$last" | sed -n 's/.*xruns=\([0-9]*\).*/\1/p')
        load=$(awk -F'load=' 'NF > 1 { s += $2; n++ } END { if (n) printf "%.1f", s / n }' "$work/client-$p-$w.log")
        tl=$(grep -o "wake timer[^\"]*\|notifications come late[^\"]*" "$work/jackd-$p-$w.log" | head -1)
        printf '%-6s %-9s %7s %6s %9s %12s %s\n' "$p" "$w" "${cyc:-?}" "${xr:-?}" "${load:-?}" "$(( (c1 - c0) / ( (t1 - t0) > 0 ? (t1 - t0) : 1 ) ))" "${tl:-}"
    done
done
echo "logs: $work"
