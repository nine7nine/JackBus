#!/bin/sh
# The server restarted under a long-running process with two contexts (a
# desktop shell: its volume applet and libcanberra's event sounds), through
# jackbus-pa: the applet's new context must find the new server's devices,
# though the other context still holds the node of the server that went away
# (a dead node is never handed out again). Then the same once more.
#
#   make -C nspa/pa-shim && nspa/pa-shim/test/reconnect.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$tree/nspa/run-isolated.sh" "$0" "$@"
fi
L=$here/../jackbus-pa
logs=$(mktemp -d /tmp/jbpa-reconnect.XXXXXX)
cc -O2 -o "$logs/pa-reconnect" "$here/pa-reconnect.c" $(pkg-config --cflags --libs libpulse) ||
    { echo "can't build pa-reconnect"; exit 2; }
export PULSE_SERVER=unix:/jackbus-pa-test/no-server JACK_DEFAULT_SERVER=jbrc JACKBUS_COMPAT_CONF=$logs/compat.conf
: >"$logs/compat.conf"
fail=0
start_server() {
    "$JACK_BUILD/jackd" -n jbrc -R -P 70 -d dummy -r 48000 -p 256 >>"$logs/jackd.log" 2>&1 &
    sp=$!
    i=0; while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbrc_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
    sleep 0.3
}
for round in 1 2; do
    start_server
    JACKBUS_PA_VERBOSE=1 "$L" "$logs/pa-reconnect" >"$logs/client.$round" 2>"$logs/client.$round.err" &
    cp=$!
    i=0; while [ $i -lt 100 ] && ! grep -q '^ready' "$logs/client.$round"; do sleep 0.1; i=$((i + 1)); done
    kill -TERM $sp; wait $sp 2>/dev/null       # the server goes away
    sleep 1
    start_server                                 # and comes back
    wait $cp; rc=$?
    if [ $rc = 0 ]; then
        echo "PASS round $round: $(tail -1 "$logs/client.$round")"
    else
        echo "FAIL round $round: $(tail -1 "$logs/client.$round")"; tail -5 "$logs/client.$round.err" | sed 's/^/    /'; fail=1
    fi
    kill -TERM $sp; wait $sp 2>/dev/null
done
[ $fail = 0 ] && { echo "ALL PASS"; rm -rf "$logs"; } || echo "logs: $logs"
exit $fail
