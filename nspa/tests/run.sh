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
cc -O2 -Wall -I"$root/common" -o "$out/xrun_timer" "$here/xrun_timer.c" -L"$fork_lib" -ljack -lpthread || exit 1
cc -m32 -O2 -Wall -I"$root/common" -o "$out/stress32" "$here/stress.c" -L/usr/lib32 -ljack -lpthread 2>/dev/null \
    || { echo "note: 32-bit client not built (no multilib/lib32-jack2?)"; rm -f "$out/stress32"; }

fail=0

# start_server <fork|system> <logfile> [server-name]; sets SERVER_PID
start_server() {
    sname=${3:-default}
    rm -f /dev/shm/jack_${sname}_*
    if [ "$1" = fork ]; then
        LD_LIBRARY_PATH="$fork_lib" JACK_DRIVER_DIR="$JACK_BUILD" \
            "$JACK_BUILD/jackd" -n "$sname" -R -P 70 ${SERVER_EXTRA:-} -d dummy -r 48000 -p 128 >"$2" 2>&1 &
    else
        env -u LD_LIBRARY_PATH -u JACK_DRIVER_DIR \
            /usr/bin/jackd -n "$sname" -R -P 70 ${SERVER_EXTRA:-} -d dummy -r 48000 -p 128 >"$2" 2>&1 &
    fi
    SERVER_PID=$!
    i=0
    while [ $i -lt 50 ]; do
        ls /dev/shm/jack_${sname}_*_0 >/dev/null 2>&1 && return 0
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
    leaked=$(ls /dev/shm | grep -E '^jack-[0-9]+-[0-9]+$' | tr '\n' ' ')
    if [ -n "$leaked" ]; then
        if [ "$server" = fork ]; then
            echo "  leaked shm segments after server exit: $leaked"; rc=1
        fi
        for f in $leaked; do rm -f "/dev/shm/$f"; done
    fi
    if [ $rc -eq 0 ]; then
        echo "PASS $name: $(tail -1 "$out/$name.client.log")"
    else
        echo "FAIL $name (rc=$rc):"; sed 's/^/  /' "$out/$name.client.log" | tail -8; fail=1
    fi
}

# Two servers share one registry: stress the second, the first must survive.
two_servers() {
    name=two-servers
    start_server fork "$out/$name.A.log" default || { echo "FAIL $name: server A"; fail=1; return; }
    A_PID=$SERVER_PID
    segsA=$(ls /dev/shm | grep -E '^jack-[0-9]+-[0-9]+$' | sort | tr '\n' ' ')
    start_server fork "$out/$name.B.log" nspa2 || { echo "FAIL $name: server B"; kill $A_PID; fail=1; return; }
    B_PID=$SERVER_PID
    JACK_DEFAULT_SERVER=nspa2 LD_LIBRARY_PATH="$fork_lib" timeout 120 "$out/stress64" 4 "$iters" >"$out/$name.client.log" 2>&1
    rc=$?
    SERVER_PID=$B_PID; stop_server || rc=1
    for f in $segsA; do
        [ -e "/dev/shm/$f" ] || { echo "  server A segment $f was removed"; rc=1; }
    done
    LD_LIBRARY_PATH="$fork_lib" timeout 60 "$out/stress64" 2 4 >"$out/$name.clientA.log" 2>&1 \
        || { echo "  new client cannot use server A"; rc=1; }
    SERVER_PID=$A_PID; stop_server || rc=1
    if [ $rc -eq 0 ]; then echo "PASS $name"; else echo "FAIL $name"; tail -3 "$out/$name.client.log"; fail=1; fi
}

# Frame timer across a server-side xrun (sync mode makes the driver cycle overrun).
# xrun_timer_case <name> <server> <client-libdir|system> <gating:1|0>
xrun_timer_case() {
    name=$1; server=$2; lib=$3; gating=$4
    SERVER_EXTRA=-S start_server "$server" "$out/$name.server.log" || { echo "FAIL $name: server"; fail=1; return; }
    if [ "$lib" = system ]; then
        env -u LD_LIBRARY_PATH timeout 60 "$out/xrun_timer" >"$out/$name.client.log" 2>&1
    else
        LD_LIBRARY_PATH="$lib" timeout 60 "$out/xrun_timer" >"$out/$name.client.log" 2>&1
    fi
    rc=$?
    stop_server
    if [ $rc -eq 0 ]; then echo "PASS $name: $(tail -1 "$out/$name.client.log")"
    elif [ "$gating" = 1 ]; then echo "FAIL $name (rc=$rc): $(tail -1 "$out/$name.client.log")"; fail=1
    else echo "INFO $name (rc=$rc, expected to fail on unfixed servers): $(tail -1 "$out/$name.client.log")"; fi
}

run_case fork-server_fork-lib       fork   "$out/stress64" "$fork_lib"
run_case fork-server_system-lib     fork   "$out/stress64" system
run_case system-server_fork-lib     system "$out/stress64" "$fork_lib"
run_case fork-server_system-lib32   fork   "$out/stress32" system
two_servers
xrun_timer_case xrun-timer_fork-server       fork   "$fork_lib" 1
xrun_timer_case xrun-timer_system-server     system system      0

exit $fail
