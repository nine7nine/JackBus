#!/bin/sh
# One process, several contexts, a "Playback" stream on each (Chromium's way:
# a context per stream, one JackBus node per process): every stream opens,
# on ports of its own, and the server is never asked for a port name that
# exists (no "already exists" in its log). On an isolated JackBus.
#
#   make -C nspa/pa-shim && nspa/pa-shim/test/contexts.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$tree/nspa/run-isolated.sh" "$0" "$@"
fi
L=$here/../jackbus-pa
logs=$(mktemp -d /tmp/jbpa-ctx.XXXXXX)
cc -O2 -o "$logs/pa-contexts" "$here/pa-contexts.c" $(pkg-config --cflags --libs libpulse) ||
    { echo "can't build pa-contexts"; exit 2; }
export PULSE_SERVER=unix:/jackbus-pa-test/no-server JACK_DEFAULT_SERVER=jbctx JACKBUS_COMPAT_CONF=$logs/compat.conf
: >"$logs/compat.conf"
"$JACK_BUILD/jackd" -n jbctx -R -P 70 -d dummy -r 48000 -p 256 >"$logs/jackd.log" 2>&1 &
sp=$!
i=0; while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbctx_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
"$L" "$logs/pa-contexts" 4 >"$logs/out" 2>"$logs/err" & p=$!
i=0; while [ $i -lt 100 ] && ! grep -q '^ready' "$logs/out"; do sleep 0.1; i=$((i + 1)); done
ports=$(jack_lsp 2>/dev/null | grep -c '^Chromium:')
wait $p; rc=$?
kill $sp; wait $sp 2>/dev/null
dup=$(grep -c "already exists" "$logs/jackd.log")
if [ $rc = 0 ] && [ "$ports" = 8 ] && [ "$dup" = 0 ]; then
    echo "PASS 4 contexts in one process: $(cat "$logs/out"), $ports ports, no name asked twice"
    rm -rf "$logs"; exit 0
fi
echo "FAIL 4 contexts: $(cat "$logs/out") (rc $rc), $ports ports, $dup 'already exists' in the server's log"
grep jackbus-pa "$logs/err" | tail -4 | sed 's/^/    /'
echo "logs: $logs"; exit 1
