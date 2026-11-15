#!/bin/sh
# Sync mode (-S) with a sound card bridge and a client that is late now and then (the suite's
# client_timings: 10 callbacks, 50 cycles apart, each 25 periods long: longer than the master
# waits for the graph's end, so those cycles end unfinished). Nothing may hang or crash, the
# bridge stays locked (no loss, no reopen), and its reader keeps running. A cycle where the
# master does not run the graph at all (Engine::Process false) is reported when it happens.
# snd-aloop by default (nothing is played on a real card); isolated, the card held and muted:
#   nspa/tests/bridge/sync-late.sh [card id=Loopback] [seconds=20] [master period=256]
set -u
card=${1:-Loopback}; secs=${2:-20}; period=${3:-256}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$here/with-cards.sh" "$card" "$0" "$@"
fi
work=$(mktemp -d /tmp/nspa-sync-late.XXXXXX)
cc -O2 -Wall ${NSPA_TEST_CFLAGS:-} -I"$root/common" -o "$work/client" "$here/bridge_client.c" -L"$JACK_BUILD/common" -ljack -lm || exit 1
cc -O2 -Wall ${NSPA_TEST_CFLAGS:-} -I"$root/common" -o "$work/timings" "$root/nspa/tests/client_timings.c" -L"$JACK_BUILD/common" -ljack || exit 1
export JACK_DEFAULT_SERVER="nspa-sync-late-$$" JACK_NO_AUDIO_RESERVATION=1
jackd=$JACK_BUILD/jackd; [ -x "$jackd" ] || jackd=$JACK_BUILD/linux/jackd
"$jackd" -n "$JACK_DEFAULT_SERVER" -R -P 70 -S --bridge "hw:$card" -d dummy -r 48000 -p "$period" > "$work/jackd.log" 2>&1 &
jd=$!
i=0; until grep -q "bridge $card: locked\|Cannot bridge\|cannot open" "$work/jackd.log" 2>/dev/null || [ $i -gt 600 ]; do sleep 0.1; i=$((i+1)); done
fail=0
if ! grep -q "bridge $card: locked" "$work/jackd.log"; then
    echo "FAIL bridge-sync-late: the bridge did not lock"; cat "$work/jackd.log"; kill $jd; exit 1
fi
"$work/client" "$card:capture_1" "$card:playback_1" "$secs" > "$work/client.log" 2>&1 &
cp=$!
sleep 1
# the late client: stays until its standard input closes
mkfifo "$work/hold"
"$work/timings" 10 25 < "$work/hold" > "$work/timings.log" 2>&1 &
tp=$!
exec 3> "$work/hold"
wait $cp
cstatus=$?
exec 3>&-
wait $tp
kill -0 $jd 2>/dev/null || { echo "FAIL bridge-sync-late: the server died"; fail=1; }
kill -INT $jd 2>/dev/null
wait $jd
jstatus=$?
unfinished=$(grep -c 'ProcessGraphSync: Process error' "$work/jackd.log")
timedout=$(grep -c 'SuspendRefNum error' "$work/jackd.log")
bad=$(grep -c "bridge $card: the card is gone\|bridge $card: the card is back\|Cannot\|cannot" "$work/jackd.log")
cycles=$(sed -n 's/.*cycles=\([0-9]*\).*/\1/p' "$work/client.log" | tail -1)
expect=$(( secs * 48000 / period ))
echo "sync-late: $timedout waits for the graph's end timed out, $unfinished cycles not run, reader cycles ${cycles:-0} of about $expect, server exit $jstatus, client exit $cstatus"
grep "^bridge" "$work/jackd.log" | sed 's/^/  /'
grep -q '^ready' "$work/timings.log" || { echo "FAIL bridge-sync-late: the late client did not finish: $(cat "$work/timings.log")"; fail=1; }
[ "$timedout" -gt 0 ] || { echo "FAIL bridge-sync-late: no late cycle (the late client did not hold the graph up)"; fail=1; }
grep -q "bridge $card: locked" "$work/jackd.log" || { echo "FAIL bridge-sync-late: the bridge lost its lock"; fail=1; }
[ "$bad" = 0 ] || { echo "FAIL bridge-sync-late: the card was lost or a call failed"; fail=1; }
[ "$jstatus" = 0 ] && [ "$cstatus" = 0 ] || { echo "FAIL bridge-sync-late: exit status"; fail=1; }
# each late callback costs the graph about 25 periods
[ "${cycles:-0}" -ge $(( expect * 8 / 10 - 250 )) ] || { echo "FAIL bridge-sync-late: the reader ran too few cycles"; fail=1; }
[ $fail = 0 ] && echo "PASS bridge-sync-late"
rm -rf "$work"
exit $fail
