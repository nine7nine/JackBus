#!/bin/sh
# A sound card bridge inside a JACK server (the fork's jackd, isolated: nspa/run-isolated.sh): the
# dummy driver as master, the card bridged (--bridge), a client reading its first capture port
# and writing silence to its first playback port; xruns, the ports' latencies and flags, and the
# server's log. The card's device reservation is held and every playback control of the card is
# set to 0 and muted for the run (restored afterwards). Not the live server's card.
#   nspa/tests/bridge/server-bridge.sh <card id[,card id...]> [seconds=30] [master period=256] [bridge spec suffix, e.g. /64@timer]
# (several cards: all bridged, the client on the first; the HDA card: through nspa/tests/hw/with-card.sh)
set -u
cards=${1:?card id}; secs=${2:-30}; n=${3:-256}; suffix=${4:-}
card=${cards%%,*}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    work=$(mktemp -d /tmp/nspa-bridge-mixer.XXXXXX)
    pids=""
    for c in $(echo "$cards" | tr ',' ' '); do
        idx=$(readlink "/proc/asound/$c" 2>/dev/null | sed 's/^card//')
        [ -n "$idx" ] || { echo "SKIP: no card $c"; exit 0; }
        pw-reserve -n "Audio$idx" -a nspa-bridge-test -p 1000 -r >/dev/null 2>&1 &
        pids="$pids $!"
        if [ -n "$(amixer -c "$idx" scontrols 2>/dev/null)" ] && alsactl -f "$work/$idx.state" store "$idx"; then
            amixer -c "$idx" scontrols | sed -n "s/^Simple mixer control '\(.*\)',\([0-9]*\)$/\1,\2/p" | while IFS= read -r x; do
                caps=$(amixer -c "$idx" sget "$x" | sed -n 's/^ *Capabilities: //p')
                case " $caps " in *" pvolume"*) amixer -q -c "$idx" sset "$x" 0% ;; esac
                case " $caps " in *" pswitch"*) amixer -q -c "$idx" sset "$x" mute ;; esac
            done
        fi
    done
    trap 'for f in "$work"/*.state; do [ -f "$f" ] && alsactl -f "$f" restore "$(basename "$f" .state)" >/dev/null 2>&1; done; for p in $pids; do kill $p 2>/dev/null; done; rm -rf "$work"' EXIT INT TERM
    sleep 1
    "$root/nspa/run-isolated.sh" "$0" "$@"
    exit $?
fi
work=$(mktemp -d /tmp/nspa-bridge-server.XXXXXX)
cc -O2 -Wall ${NSPA_TEST_CFLAGS:-} -I"$root/common" -o "$work/client" "$here/bridge_client.c" -L"$JACK_BUILD/common" -ljack -lm || exit 1
export JACK_DEFAULT_SERVER="nspa-bridge-$$"
jackd=$JACK_BUILD/jackd; [ -x "$jackd" ] || jackd=$JACK_BUILD/linux/jackd
bridges=""
for c in $(echo "$cards" | tr ',' ' '); do bridges="$bridges --bridge hw:$c$suffix"; done
"$jackd" -n "$JACK_DEFAULT_SERVER" -R -P 70 $bridges -d dummy -r 48000 -p "$n" > "$work/jackd.log" 2>&1 &
jd=$!
i=0; until grep -q "bridge $card:\|Cannot bridge\|No bridge" "$work/jackd.log" 2>/dev/null || [ $i -gt 50 ]; do sleep 0.1; i=$((i+1)); done
sleep 1
"$work/client" "$card:capture_1" "$card:playback_1" "$secs" > "$work/client.log" 2>&1
kill $jd 2>/dev/null; wait $jd 2>/dev/null
grep -i "bridge\|error\|xrun" "$work/jackd.log" | head -20
tail -4 "$work/client.log"
echo "logs: $work"
