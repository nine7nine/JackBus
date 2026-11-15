#!/bin/sh
# Bridges added while clients come and go: the server starts with bridges (jackd --bridge adds
# them after it accepts clients) while short-lived clients open and close as fast as they can,
# as a desktop's sound server or a session manager may right at start. No client may hang (each
# gets 10 s), a new client must open afterwards, and the log must show no synchro destroyed
# under a bridge and no client refused. Seen 2026-11-15: clients whose close never returned
# (their notification thread cancelled while it held libjack's synchro mutex), and once, with
# two USB bridges and 18 clients opening at start, a bridge's futex gone and from then on every
# client open refused. snd-aloop (both devices bridged) or NSPA_RACE_CARDS (card ids), isolated:
#   nspa/tests/bridge/start-race.sh [rounds=30] [openers=8]
set -u
rounds=${1:-30}; openers=${2:-8}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
cards=${NSPA_RACE_CARDS:-}
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$here/with-cards.sh" "$(echo "${cards:-Loopback}" | tr ' ' ',')" "$0" "$@"
fi
if [ -n "$cards" ]; then
    bridges=$(for c in $cards; do printf ' --bridge hw:%s' "$c"; done)
else
    bridges="--bridge hw:Loopback,0 --bridge hw:Loopback,1"
fi
work=$(mktemp -d /tmp/nspa-start-race.XXXXXX)
cc -O2 -Wall -I"$root/common" -o "$work/bench" "$root/nspa/tests/perf/cycle_bench.c" -L"$JACK_BUILD/common" -ljack || exit 1
export JACK_NO_AUDIO_RESERVATION=1
jackd=$JACK_BUILD/jackd; [ -x "$jackd" ] || jackd=$JACK_BUILD/linux/jackd
fails=0
for r in $(seq 1 "$rounds"); do
    export JACK_DEFAULT_SERVER="race-$$-$r"
    log=$work/jackd-$r.log
    "$jackd" -n "$JACK_DEFAULT_SERVER" -R -P 70 $bridges -d dummy -r 48000 -p 128 > "$log" 2>&1 &
    jd=$!
    pids=""
    : > "$work/hung"
    for o in $(seq 1 "$openers"); do
        ( end=$(( $(date +%s) + 3 )); while [ "$(date +%s)" -lt "$end" ]; do
              timeout -s KILL 10 "$work/bench" -q 2>/dev/null
              [ $? = 137 ] && echo hung >> "$work/hung"
          done ) &
        pids="$pids $!"
    done
    for p in $pids; do wait $p; done
    hung=$(grep -c . "$work/hung")
    sleep 0.5
    if "$work/bench" -q 2>/dev/null; then ok=1; else ok=0; fi
    kill -INT $jd 2>/dev/null; wait $jd
    if [ $ok = 0 ] || [ "$hung" -gt 0 ] || grep -q 'already deallocated\|Cannot create new client\|Cannot notify add client' "$log"; then
        fails=$((fails + 1))
        echo "round $r: FAILED (clients hung: $hung; a new client opened: $ok)"; grep -m5 'deallocated\|Cannot\|fails' "$log" | sed 's/^/  /'
        cp "$log" "/tmp/nspa-start-race-failed-$r.log"
    fi
done
if [ $fails = 0 ]; then
    echo "PASS bridge-start-race: $rounds starts with $openers clients coming and going"
else
    echo "FAIL bridge-start-race: $fails of $rounds starts (logs: /tmp/nspa-start-race-failed-*.log)"
fi
rm -rf "$work"
[ $fails = 0 ]
