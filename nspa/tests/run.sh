#!/bin/sh
# jack2-nspa test matrix. Must run inside nspa/run-isolated.sh:
#   nspa/run-isolated.sh nspa/tests/run.sh [quick]
#
# Matrix (server <- client libjack):
#   fork   <- fork        (64-bit)
#   fork   <- system      (64-bit, Arch jack2 1.9.22 /usr/lib/libjack.so.0)
#   system <- fork        (64-bit, /usr/bin/jackd 1.9.22)
#   fork   <- system32    (32-bit, Arch lib32-jack2 /usr/lib32/libjack.so.0)
set -u
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    echo "refusing to run outside nspa/run-isolated.sh" >&2
    exit 2
fi
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${NSPA_TEST_OUT:-/tmp/jack2-nspa-tests}"
mkdir -p "$out"
fork_lib="$JACK_BUILD/common"
iters="${NSPA_ITERS:-200}"
[ "${1:-}" = quick ] && iters=20

cc -O2 -Wall -I"$root/common" -o "$out/stress64" "$here/stress.c" -L"$fork_lib" -ljack -lpthread || exit 1
cc -m32 -O2 -Wall -I"$root/common" -o "$out/stress32" "$here/stress.c" -L/usr/lib32 -ljack -lpthread 2>/dev/null \
    || { echo "note: 32-bit client not built (no multilib/lib32-jack2?)"; rm -f "$out/stress32"; }

fail=0

# start_server <fork|system> <logfile>; sets SERVER_PID
start_server() {
    rm -f /dev/shm/jack_default_*
    if [ "$1" = fork ]; then
        LD_LIBRARY_PATH="$fork_lib" JACK_DRIVER_DIR="$JACK_BUILD" \
            "$JACK_BUILD/jackd" -R -P 70 -d dummy -r 48000 -p 128 >"$2" 2>&1 &
    else
        env -u LD_LIBRARY_PATH -u JACK_DRIVER_DIR \
            /usr/bin/jackd -R -P 70 -d dummy -r 48000 -p 128 >"$2" 2>&1 &
    fi
    SERVER_PID=$!
    i=0
    while [ $i -lt 50 ]; do
        ls /dev/shm/jack_default_*_0 >/dev/null 2>&1 && return 0
        kill -0 "$SERVER_PID" 2>/dev/null || return 1
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

stop_server() {
    kill -TERM "$SERVER_PID" 2>/dev/null
    i=0
    while kill -0 "$SERVER_PID" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "  server did not exit on SIGTERM"; kill -KILL "$SERVER_PID"; return 1
    fi
    wait "$SERVER_PID" 2>/dev/null
    return 0
}

# run_case <name> <server> <client-binary> <client-libdir|system>
run_case() {
    name=$1; server=$2; bin=$3; lib=$4
    [ -x "$bin" ] || { echo "SKIP $name"; return; }
    if ! start_server "$server" "$out/$name.server.log"; then
        echo "FAIL $name: server did not start"; tail -5 "$out/$name.server.log"; fail=1; return
    fi
    if [ "$lib" = system ]; then
        env -u LD_LIBRARY_PATH timeout 120 "$bin" 6 "$iters" >"$out/$name.client.log" 2>&1
    else
        LD_LIBRARY_PATH="$lib" timeout 120 "$bin" 6 "$iters" >"$out/$name.client.log" 2>&1
    fi
    rc=$?
    stop_server || rc=1
    if [ $rc -eq 0 ]; then
        echo "PASS $name: $(tail -1 "$out/$name.client.log")"
    else
        echo "FAIL $name (rc=$rc):"; sed 's/^/  /' "$out/$name.client.log" | tail -8; fail=1
    fi
}

run_case fork-server_fork-lib       fork   "$out/stress64" "$fork_lib"
run_case fork-server_system-lib     fork   "$out/stress64" system
run_case system-server_fork-lib     system "$out/stress64" "$fork_lib"
run_case fork-server_system-lib32   fork   "$out/stress32" system

exit $fail
