#!/bin/sh
# libcanberra (KDE's and GNOME's event sounds: notifications, a settings
# page's previews) through jackbus-pa, on an isolated JackBus (dummy driver):
#  - a sound without a callback, cleaned up by libcanberra when it ends (it
#    disconnects the stream in a callback holding its own lock, which its
#    subscription callback takes: an event delivered inside the disconnect
#    deadlocked the main loop, and the application with it), three in a row;
#  - with a callback, waited for, three in a row;
#  - destroyed while playing;
# each then destroys its context, within a time limit; the tone recorded
# from the stream's port is a continuous 440 Hz sine.
#
#   make -C nspa/pa-shim all test-apps && nspa/pa-shim/test/canberra.sh
# (CANBERRA_PA_LIB=/usr/lib/jackbus-pa: the installed library instead)
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$tree/nspa/run-isolated.sh" "$0" "$@"
fi
out=$JACK_BUILD/pa-shim
lib=${CANBERRA_PA_LIB:-$out/lib}
check=$tree/nspa/tests/tools/sine_check.py
logs=$(mktemp -d /tmp/jbpa-ca.XXXXXX)
[ -x "$out/ca-play" ] || { echo "missing ca-play: make -C nspa/pa-shim test-apps"; exit 2; }
[ -e "$lib/libpulse.so.0" ] || { echo "missing $lib/libpulse.so.0"; exit 2; }

PULSE_SERVER=unix:/jackbus-pa-test/no-server JACK_DEFAULT_SERVER=jbca JACKBUS_COMPAT_CONF=$logs/compat.conf
export PULSE_SERVER JACK_DEFAULT_SERVER JACKBUS_COMPAT_CONF
: >"$logs/compat.conf"
fail=0
pass() { echo "PASS $1"; }
flunk() { echo "FAIL $1"; shift; for l in "$@"; do [ -f "$l" ] && tail -8 "$l" | sed 's/^/    /'; done; fail=1; }
run() {  # label, ca-play arguments...
    l=$1; shift
    LD_LIBRARY_PATH=$lib:$LD_LIBRARY_PATH JACKBUS_PA_MODE=jackbus timeout 30 "$out/ca-play" "$@" >"$logs/$l.log" 2>&1
}

"$JACK_BUILD/jackd" -n jbca -R -P 70 -d dummy -r 48000 -p 256 >"$logs/jackd.log" 2>&1 &
sp=$!
i=0; while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbca_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
python3 -I "$tree/nspa/pw-shim/test/mkwav.py" "$logs/s48.wav" 48000 2
python3 -I "$tree/nspa/pw-shim/test/mkwav.py" "$logs/short.wav" 48000 0.3

# 1. a sound without a callback: recorded, then two more, then destroyed
LD_LIBRARY_PATH=$lib:$LD_LIBRARY_PATH JACKBUS_PA_MODE=jackbus timeout 30 \
    "$out/ca-play" plain "$logs/s48.wav" 3 2 >"$logs/plain.log" 2>&1 &
p=$!
i=0; port=""
while [ $i -lt 100 ] && [ -z "$port" ]; do
    port=$(jack_lsp 2>/dev/null | grep -v '^system' | grep -E ':(FL|MONO)$' | head -1); sleep 0.05; i=$((i + 1))
done
sleep 0.3
timeout 10 jack_rec -f "$logs/plain.wav" -d 1.0 "$port" >/dev/null 2>&1
python3 -I "$check" "$logs/plain.wav" 440 >"$logs/plain.check" 2>&1; r=$?
wait $p; rc=$?
if [ $rc = 0 ] && grep -q '^ok' "$logs/plain.log" && [ $r = 0 ]; then
    pass "3 sounds without a callback (libcanberra cleans up), context destroyed: $(cut -d' ' -f2- "$logs/plain.check" | cut -c1-90)"
else
    flunk "without a callback: exit $rc (124: hung), port '$port'" "$logs/plain.log" "$logs/plain.check"
fi

# 2. with a callback, waited for
run callback callback "$logs/short.wav" 3; rc=$?
[ $rc = 0 ] && grep -q '^ok' "$logs/callback.log" && pass "3 sounds with a callback, each finished, context destroyed" ||
    flunk "with a callback: exit $rc (124: hung)" "$logs/callback.log"

# 3. destroyed while playing
run cancel cancel "$logs/s48.wav"; rc=$?
[ $rc = 0 ] && grep -q '^ok' "$logs/cancel.log" && pass "destroyed while playing: returns" ||
    flunk "destroyed while playing: exit $rc (124: hung)" "$logs/cancel.log"

kill $sp 2>/dev/null; wait $sp 2>/dev/null
if [ $fail = 0 ]; then echo "ALL PASS"; rm -rf "$logs"; else echo "logs: $logs"; exit 1; fi
