#!/bin/sh
# JackBus test matrix. Must run inside nspa/run-isolated.sh:
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
out="${NSPA_TEST_OUT:-/tmp/JackBus-tests}"
mkdir -p "$out"
fork_lib="$JACK_BUILD/common"
tcf="${NSPA_TEST_CFLAGS:-}"   # extra flags for test programs (e.g. -fsanitize=thread)
iters="${NSPA_ITERS:-200}"
[ "${1:-}" = quick ] && iters=20

cc -O2 $tcf -Wall -I"$root/common" -o "$out/stress64" "$here/stress.c" -L"$fork_lib" -ljack -lpthread || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/xrun_timer" "$here/xrun_timer.c" -L"$fork_lib" -ljack -lpthread || exit 1
c++ -std=gnu++11 -O2 $tcf -Wall -DSERVER_SIDE -I"$root/common" -I"$root/common/jack" -I"$root/linux" -I"$root/posix" \
    -o "$out/midi_wrap" "$here/midi_wrap.cpp" -L"$fork_lib" -ljackserver || exit 1
# reference: memops.c's portable C paths (no SSE), symbols renamed ref_*
cc -march=x86-64 -mtune=generic -O2 -U__SSE2__ -U__SSE4_1__ -I"$root/common" -I"$root/common/jack" \
    -c -o "$out/memops_ref.o" "$root/common/memops.c" || exit 1
objcopy $(nm --defined-only -g "$out/memops_ref.o" | awk '{print "--redefine-sym " $3 "=ref_" $3}') "$out/memops_ref.o" || exit 1
cc -march=x86-64 -mtune=generic -O2 $tcf -DWITH_REFERENCE -I"$root/common" -I"$root/common/jack" -o "$out/memops_consistency" \
    "$here/memops_consistency.c" "$root/common/memops.c" "$out/memops_ref.o" -lm || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/threads" "$here/threads.c" -L"$fork_lib" -ljack -lpthread || exit 1
c++ -std=gnu++11 -O2 $tcf -DSERVER_SIDE -I"$root/common" -I"$root/common/jack" -I"$root/linux" -I"$root/posix" \
    -o "$out/pi_mutex" "$here/pi_mutex.cpp" -L"$fork_lib" -ljackserver || exit 1
c++ -std=gnu++11 -O2 $tcf -w -DSERVER_SIDE -I"$root/common" -I"$root/common/jack" -I"$root/linux" -I"$root/posix" -I"$JACK_BUILD" \
    -include "$JACK_BUILD/config.h" -o "$out/connection_copy" "$here/connection_copy.cpp" -L"$fork_lib" -ljackserver || exit 1
c++ -std=gnu++11 -O2 $tcf -Wall -I"$root/common" -I"$root/common/jack" -I"$root/linux" -I"$root/posix" -I"$JACK_BUILD" \
    -include "$JACK_BUILD/config.h" -o "$out/nocancel" "$here/nocancel.cpp" -lpthread || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/rt_faults" "$here/rt_faults.c" -L"$fork_lib" -ljack || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/stale_client" "$here/stale_client.c" -L"$fork_lib" -ljack || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/mixdown" "$here/mixdown.c" -L"$fork_lib" -ljack || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/metadata" "$here/metadata.c" -L"$fork_lib" -ljack || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/refnum_bound" "$here/refnum_bound.c" -L"$fork_lib" -ljack || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/client_kill" "$here/client_kill.c" -L"$fork_lib" -ljack || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/notify_lag" "$here/notify_lag.c" -L"$fork_lib" -ljack || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/client_timings" "$here/client_timings.c" -L"$fork_lib" -ljack || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/patchbay_client" "$here/patchbay_client.c" -L"$fork_lib" -ljack || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/eager_client" "$here/eager_client.c" -L"$fork_lib" -ljack || exit 1
cc -m32 -O2 -Wall -I"$root/common" -o "$out/stress32" "$here/stress.c" -L/usr/lib32 -ljack -lpthread 2>/dev/null \
    || { echo "note: 32-bit client not built (no multilib/lib32-jack2?)"; rm -f "$out/stress32"; }

fail=0

# Sanitizer runs (NSPA_SANITIZER=asan|tsan, with a matching build and
# NSPA_TEST_CFLAGS): no mlockall (huge shadow mappings), no uninstrumented
# Arch binaries (false positives), no strace. Reports go to $out/<san>.*
san="${NSPA_SANITIZER:-}"
if [ -n "$san" ]; then
    export JACK_NO_MLOCK=1
    # TSan slows clients down so much that 128-frame cycles overrun; after a
    # timeout the server deliberately lets late clients overlap the next
    # cycle, which TSan reports. Large periods keep the graph in order.
    [ "$san" = tsan ] && SERVER_PERIOD=2048
    export ASAN_OPTIONS="log_path=$out/asan:detect_leaks=0:abort_on_error=0"
    export UBSAN_OPTIONS="log_path=$out/ubsan:print_stacktrace=1"
    export TSAN_OPTIONS="log_path=$out/tsan:halt_on_error=0:report_signal_unsafe=0:second_deadlock_stack=1:handle_segv=0:suppressions=$here/tsan.supp"
    rm -f "$out"/asan.* "$out"/ubsan.* "$out"/tsan.*
fi
skip_system() {  # skip_system <name> <server> <lib>
    if [ -n "$san" ] && { [ "$2" = system ] || [ "$3" = system ]; }; then
        echo "SKIP $1 (sanitizer run)"; return 0
    fi
    # JACKBUS_TRANSPORT=1: this libjack's clients open through the JackBus socket,
    # which the system's jackd doesn't have
    if [ "${JACKBUS_TRANSPORT:-0}" = 1 ] && [ "$2" = system ]; then
        echo "SKIP $1 (system server: no JackBus socket)"; return 0
    fi
    return 1
}

# start_server <fork|system> <logfile> [server-name]; sets SERVER_PID
start_server() {
    sname=${3:-default}
    rm -f /dev/shm/jack_${sname}_*
    if [ "$1" = fork ]; then
        env LD_LIBRARY_PATH="$fork_lib" JACK_DRIVER_DIR="$JACK_BUILD" ${SERVER_ENV:-} \
            "$JACK_BUILD/jackd" -n "$sname" -R -P 70 ${SERVER_EXTRA:-} -d dummy -r 48000 -p ${SERVER_PERIOD:-128} >"$2" 2>&1 &
    else
        env -u LD_LIBRARY_PATH -u JACK_DRIVER_DIR ${SERVER_ENV:-} \
            /usr/bin/jackd -n "$sname" -R -P 70 ${SERVER_EXTRA:-} -d dummy -r 48000 -p ${SERVER_PERIOD:-128} >"$2" 2>&1 &
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
    skip_system "$name" "$server" "$lib" && return
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
    skip_system "$name" "$server" "$lib" && return
    SERVER_EXTRA=-S
    start_server "$server" "$out/$name.server.log" || { echo "FAIL $name: server"; SERVER_EXTRA=; fail=1; return; }
    SERVER_EXTRA=
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

# Frame counter wrap: start the server 0.25 s before 2^32 frames.
wrap_case() {
    name=frame-wrap
    SERVER_ENV="JACK_NSPA_TEST_INITIAL_FRAMES=$((4294967296 - 12000))"
    start_server fork "$out/$name.server.log" || { echo "FAIL $name: server"; SERVER_ENV=; fail=1; return; }
    SERVER_ENV=
    LD_LIBRARY_PATH="$fork_lib" timeout 120 "$out/stress64" 4 "$iters" 1000 >"$out/$name.client.log" 2>&1
    rc=$?
    stop_server || rc=1
    grep -q '(wrapped)' "$out/$name.client.log" || { echo "  frame counter did not wrap during the test"; rc=1; }
    if [ $rc -eq 0 ]; then echo "PASS $name: $(tail -1 "$out/$name.client.log")"
    else echo "FAIL $name (rc=$rc): $(tail -1 "$out/$name.client.log")"; fail=1; fi
}

# A client process that outlives its server (here: hung, ignoring the
# shutdown) must not disturb a client of the same name on the next server.
# stale_case <server> <TERM|KILL> <gating:1|0>
stale_case() {
    name=stale-client_$1-server_$2; server=$1; sig=$2; gating=$3
    skip_system "$name" "$server" fork && return
    start_server "$server" "$out/$name.server1.log" || { echo "FAIL $name: server"; fail=1; return; }
    LD_LIBRARY_PATH="$fork_lib" "$out/stale_client" hang >/dev/null 2>&1 &
    hang=$!
    sleep 1
    kill -$sig "$SERVER_PID"; wait "$SERVER_PID" 2>/dev/null
    rc=0
    if [ "$sig" = TERM ] && [ "$server" = fork ] && ls /dev/shm | grep -q 'jack_sem.*_stale$'; then
        echo "  the stopped server left the client's futex file in /dev/shm"; rc=1
    fi
    if start_server "$server" "$out/$name.server2.log"; then
        LD_LIBRARY_PATH="$fork_lib" timeout 30 "$out/stale_client" check 2 >"$out/$name.client.log" 2>&1 || rc=1
        stop_server || rc=1
    else
        echo "  second server did not start"; rc=1
    fi
    kill -KILL $hang; wait $hang 2>/dev/null
    rm -f /dev/shm/jack_sem.*_stale
    if [ $rc -eq 0 ]; then echo "PASS $name: $(tail -1 "$out/$name.client.log")"
    elif [ "$gating" = 1 ]; then echo "FAIL $name: $(tail -1 "$out/$name.client.log")"; fail=1
    else echo "INFO $name (expected to fail on unfixed servers): $(tail -1 "$out/$name.client.log")"; fi
}

run_case fork-server_fork-lib       fork   "$out/stress64" "$fork_lib"
run_case fork-server_system-lib     fork   "$out/stress64" system
run_case system-server_fork-lib     system "$out/stress64" "$fork_lib"
run_case fork-server_system-lib32   fork   "$out/stress32" system
# fork lib32 libjack (mixed build: nspa/build.sh -o build-mixed --mixed)
lib32="$root/build-mixed/lib32/common"
if [ -f "$lib32/libjack.so" ] && cc -m32 -O2 -Wall -I"$root/common" -o "$out/stress32fork" "$here/stress.c" -L"$lib32" -ljack -lpthread 2>/dev/null; then
    run_case fork-server_fork-lib32     fork   "$out/stress32fork" "$lib32"
    run_case system-server_fork-lib32   system "$out/stress32fork" "$lib32"
fi
two_servers
stale_case fork   TERM 1
stale_case fork   KILL 1
stale_case system TERM 0
xrun_timer_case xrun-timer_fork-server       fork   "$fork_lib" 1
xrun_timer_case xrun-timer_system-server     system system      0
wrap_case
# page faults in process callbacks when the client cannot mlock (small RLIMIT_MEMLOCK)
if [ -n "$san" ]; then
    echo "SKIP rt-faults (sanitizer run: shadow memory faults)"
elif start_server fork "$out/rt_faults.server.log"; then
    if (ulimit -l 64; LD_LIBRARY_PATH="$fork_lib" timeout 60 "$out/rt_faults" 300 >"$out/rt_faults.log" 2>/dev/null); then
        echo "PASS rt-faults (memlock 64 KB): $(tail -1 "$out/rt_faults.log" | sed 's/^PASS: //')"
    else
        echo "FAIL rt-faults (memlock 64 KB): $(tail -1 "$out/rt_faults.log")"; fail=1
    fi
    stop_server || fail=1
else
    echo "FAIL rt-faults: server"; fail=1
fi
# libjack threading (fork server + fork libjack)
if start_server fork "$out/threads.server.log"; then
    if [ "$san" = tsan ]; then
        # one process per subtest: under TSan the async thread cancellation at
        # jack_client_close() aborts the process (TSan limitation)
        : >"$out/threads.log"
        for t in api-concurrency activate-init port-churn sync-callback transport-churn rt-helper-thread; do
            LD_LIBRARY_PATH="$fork_lib" timeout 120 "$out/threads" $t 2>/dev/null | grep -E '^(PASS|FAIL)' >>"$out/threads.log"
        done
    else
        LD_LIBRARY_PATH="$fork_lib" timeout 120 "$out/threads" >"$out/threads.log" 2>&1 || { [ $? -eq 124 ] && echo "TIMEOUT threads (hang)" >>"$out/threads.log"; fail=1; }
    fi
    stop_server || fail=1
    sed 's/^/  /' "$out/threads.log" | sed 's/^  \(PASS\|FAIL\|TIMEOUT\)/\1/'
else
    echo "FAIL threads: server"; fail=1
fi
if LD_LIBRARY_PATH="$fork_lib" "$out/midi_wrap" >"$out/midi_wrap.log" 2>&1; then echo "PASS midi-wrap: $(tail -1 "$out/midi_wrap.log")"
else echo "FAIL midi-wrap:"; sed 's/^/  /' "$out/midi_wrap.log"; fail=1; fi
if LD_LIBRARY_PATH="$fork_lib" "$out/pi_mutex" >"$out/pi_mutex.log" 2>&1; then echo "PASS pi-mutex: $(tail -1 "$out/pi_mutex.log" | sed 's/^PASS: //')"
else echo "FAIL pi-mutex: $(tail -1 "$out/pi_mutex.log")"; fail=1; fi
if LD_LIBRARY_PATH="$fork_lib" "$out/connection_copy" >"$out/connection_copy.log" 2>/dev/null; then echo "PASS connection-copy: $(tail -1 "$out/connection_copy.log" | sed 's/^PASS: //')"
else echo "FAIL connection-copy: $(tail -1 "$out/connection_copy.log")"; fail=1; fi
if "$out/nocancel" >"$out/nocancel.log" 2>&1; then echo "PASS nocancel: $(tail -1 "$out/nocancel.log" | sed 's/^PASS: //')"
else echo "FAIL nocancel: $(tail -2 "$out/nocancel.log" | tr '\n' ' ')"; fail=1; fi
if "$out/memops_consistency" >"$out/memops.log" 2>&1; then echo "PASS memops: $(tail -1 "$out/memops.log")"
else echo "FAIL memops:"; sed 's/^/  /' "$out/memops.log" | tail -6; fail=1; fi

# Per-cycle client loops bounded by the highest refnum in use (server), and
# the chunked successor scan (libjack): clients open and close so that live
# refnums are high, low, and the highest one disappears.
refnum_case() {  # refnum_case <name> <server> <lib|system>
    skip_system "$1" "$2" "$3" && return
    if ! start_server "$2" "$out/$1.server.log"; then echo "FAIL $1: server"; fail=1; return; fi
    if [ "$3" = system ]; then env -u LD_LIBRARY_PATH timeout 60 "$out/refnum_bound" >"$out/$1.log" 2>&1
    else LD_LIBRARY_PATH="$3" timeout 60 "$out/refnum_bound" >"$out/$1.log" 2>&1; fi
    rc=$?
    stop_server || rc=1
    if [ $rc -eq 0 ]; then echo "PASS $1: $(grep -c ' ok$' "$out/$1.log") stages ok"
    else echo "FAIL $1 (rc=$rc):"; sed 's/^/  /' "$out/$1.log" | tail -6; fail=1; fi
}
refnum_case refnum-bound_fork-server_fork-lib fork "$fork_lib"
refnum_case refnum-bound_fork-server_system-lib fork system
refnum_case refnum-bound_system-server_fork-lib system "$fork_lib"

# Metadata across libjack builds: one Berkeley DB environment in
# /dev/shm/jack_db-<uid> is shared by the server and all clients, so every
# build must use the same Berkeley DB version as Arch's jack2 (db5.3).
metadata_case() {  # metadata_case <name> <server> <set-lib|system> <get-lib|system>
    skip_system "$1" "$2" "$3" && return
    skip_system "$1" "$2" "$4" && return
    rm -rf /dev/shm/jack_db-*
    if ! start_server "$2" "$out/$1.server.log"; then echo "FAIL $1: server"; fail=1; return; fi
    if [ "$3" = system ]; then env -u LD_LIBRARY_PATH "$out/metadata" set 5 >"$out/$1.set.log" 2>&1 &
    else LD_LIBRARY_PATH="$3" "$out/metadata" set 5 >"$out/$1.set.log" 2>&1 & fi
    setter=$!
    sleep 1
    if [ "$4" = system ]; then env -u LD_LIBRARY_PATH "$out/metadata" get >"$out/$1.get.log" 2>&1
    else LD_LIBRARY_PATH="$4" "$out/metadata" get >"$out/$1.get.log" 2>&1; fi
    rc=$?
    kill $setter 2>/dev/null; wait $setter 2>/dev/null
    stop_server || rc=1
    if [ $rc -eq 0 ] && grep -q '^PASS' "$out/$1.set.log"; then echo "PASS $1: $(grep -h '^PASS' "$out/$1.set.log" "$out/$1.get.log" | sed 's/^PASS: //' | tr '\n' ';')"
    else echo "FAIL $1: $(grep -hE '^(PASS|FAIL)|DB' "$out/$1.set.log" "$out/$1.get.log" | head -4 | tr '\n' ';')"; fail=1; fi
}
metadata_case metadata_fork-server_fork-set_system-get fork "$fork_lib" system
metadata_case metadata_fork-server_system-set_fork-get fork system "$fork_lib"
metadata_case metadata_system-server_fork-set_fork-get system "$fork_lib" "$fork_lib"
metadata_case metadata_system-server_system-set_fork-get system system "$fork_lib"

# Input-port mixdown: exact sums of 0..6 sources, at a period that is not a
# multiple of the SSE width (scalar tail) and at a normal one; 64- and
# 32-bit libjack (the mixdown runs in the client).
mixdown_case() {  # mixdown_case <name> <period> <binary> <libdir>
    [ -x "$3" ] || { echo "SKIP $1"; return; }
    # not "SERVER_PERIOD=$2 start_server": with sh (bash in POSIX mode) a
    # prefix assignment to a function call persists after it
    saved_period=${SERVER_PERIOD:-}
    SERVER_PERIOD=$2
    if start_server fork "$out/$1.server.log"; then
        if LD_LIBRARY_PATH="$4" timeout 60 "$3" >"$out/$1.log" 2>&1; then echo "PASS $1: $(tail -1 "$out/$1.log" | sed 's/^PASS: //')"
        else echo "FAIL $1: $(tail -1 "$out/$1.log")"; fail=1; fi
        stop_server || fail=1
    else
        echo "FAIL $1: server"; fail=1
    fi
    SERVER_PERIOD=$saved_period
}
mixdown_case mixdown-1021 1021 "$out/mixdown" "$fork_lib"
mixdown_case mixdown-64 64 "$out/mixdown" "$fork_lib"
lib32="$root/build-mixed/lib32/common"
if [ -z "$san" ] && [ -f "$lib32/libjack.so" ] && cc -m32 -O2 -Wall -I"$root/common" -o "$out/mixdown32" "$here/mixdown.c" -L"$lib32" -ljack 2>/dev/null; then
    mixdown_case mixdown-1021-lib32 1021 "$out/mixdown32" "$lib32"
fi

# A client (40 in + 40 out ports, between two others) that goes away while the
# graph runs: closed, or its process gone without jack_client_close (exit,
# SIGKILL, SIGKILL while a child it forked holds libjack's sockets, SIGKILL with
# 256 MB locked so the kernel takes a while to tear it down, SIGKILL after
# jack_deactivate), deactivated and activated again (react), or its own process
# thread returning while the process goes on (threadend). Limits: xruns for all
# trials, cycles the client behind it may miss, port-unregistration callbacks
# exactly once per port; and how many times the server must log that a client's
# realtime thread ended (JackAliveTable: once per death with our libjack, never
# otherwise; "-" = not checked).
# KILL_LIB / KILL_BIN: the client's libjack and test binary (default: the fork's).
kill_case() {  # kill_case <server> <mode> <trials> <mem_mb> <gating 1|0> "<xruns> <cycles> <unreg>" [ended]
    name="client-kill-$2_$1-server${KILL_TAG:-}"
    [ "$4" != 0 ] && name="$name-$4MB"
    skip_system "$name" "$1" fork && return
    limits="$6"
    if [ -n "$san" ]; then
        # sanitizers slow everything down: twice the xruns (and 2 more), 8 cycles
        limits=$(echo "$6" | awk '{print $1 * 2 + 2, ($2 > 0 ? 8 : 0), $3}')
    fi
    saved_period=${SERVER_PERIOD:-}
    [ "$san" = tsan ] || SERVER_PERIOD=256     # TSan keeps its long periods
    if start_server "$1" "$out/$name.server.log"; then
        rc=0
        NSPA_KILL_LIMITS="$limits" LD_LIBRARY_PATH="${KILL_LIB:-$fork_lib}" timeout 180 "${KILL_BIN:-$out/client_kill}" "$2" "$3" 40 "$4" >"$out/$name.log" 2>&1 || rc=1
        stop_server || fail=1
        line="$(tail -1 "$out/$name.log" | sed 's/^PASS: //; s/^FAIL: //')"
        ended=$(grep -c 'realtime thread ended' "$out/$name.server.log")
        # under TSan, clients abort when they close (their threads end): not counted
        if [ -n "${7:-}" ] && [ "$7" != - ] && [ "$san" != tsan ] && [ "$ended" != "$7" ]; then
            rc=1; line="$line; realtime thread ended logged $ended times, expected $7"
        fi
        if [ $rc = 0 ]; then
            echo "PASS $name: $line"
        elif [ "$5" = 1 ]; then
            echo "FAIL $name: $line"; fail=1
        else
            echo "INFO $name (expected to fail on unfixed servers): $line"
        fi
    else
        echo "FAIL $name: server"; fail=1
    fi
    SERVER_PERIOD=$saved_period
}
# the fork's libjack: dying clients found through their slot, before their next cycle
kill_case fork close     6 0   1 "0 0 1" 0
kill_case fork churn     6 0   1 "0 0 1" 0
kill_case fork exit      6 0   1 "3 2 1" 6
kill_case fork kill      6 0   1 "3 2 1" 6
kill_case fork killfork  4 0   1 "2 2 1" 4
# deactkill: its ports are announced as gone twice (as 1.9.22: clients that
# found them with jack_get_ports() rely on the second time), so no unreg check
kill_case fork deactkill 4 0   1 "0 0 0" 0
kill_case fork react     4 0   1 "0 0 0" 0
kill_case fork threadend 4 0   1 "0 0 0" 4
if [ -z "$san" ]; then
    kill_case fork kill  4 256 1 "2 2 1" 4
fi
# a kernel without futex_waitv: slots looked at every 20 ms, so the socket
# closing may come first (either way the client is removed): no count
SERVER_ENV=JACK_NO_FUTEX_WAITV=1 KILL_TAG=-nowaitv kill_case fork kill 6 0 1 "12 4 1" -
SERVER_ENV=
# another libjack (the system one, without slots, or with): found through
# /proc when the graph does not finish, or through its slot
KILL_LIB=/usr/lib KILL_TAG=-systemlib kill_case fork kill 4 0 1 "8 4 1" -
KILL_LIB=/usr/lib KILL_TAG=-systemlib kill_case fork react 4 0 1 "0 0 0" 0
# the fork's 32-bit libjack against the 64-bit server: the slot laid out by
# 32-bit glibc, the kernel's compat robust list
lib32="$root/build-mixed/lib32/common"
if [ -z "$san" ] && [ -f "$lib32/libjack.so" ] && nm -D "$lib32/libjack.so" 2>/dev/null | grep -q pthread_mutex_consistent \
   && cc -m32 -O2 -Wall -I"$root/common" -o "$out/client_kill32" "$here/client_kill.c" -L"$lib32" -ljack 2>/dev/null; then
    KILL_LIB=$lib32 KILL_BIN=$out/client_kill32 KILL_TAG=-lib32 kill_case fork kill  4 0 1 "2 2 1" 4
    KILL_LIB=$lib32 KILL_BIN=$out/client_kill32 KILL_TAG=-lib32 kill_case fork react 4 0 1 "0 0 0" 0
else
    echo "SKIP client-kill lib32 (no 32-bit build of this libjack in build-mixed)"
fi
KILL_LIB= KILL_BIN= KILL_TAG=
kill_case system kill     4 0 0 "8 4 1"
kill_case system killfork 2 0 0 "4 4 1"

# One client's notification thread is slow (1.5 s latency callback) or stuck
# (6 s, while another client registers 1500 ports): other clients' requests
# must not wait for it, it must still get its notifications in order, and a
# buffer-size change must still wait until it has handled it.
lag_case() {  # lag_case <server> <mode> <gating 1|0>
    name="notify-lag-$2_$1-server"
    skip_system "$name" "$1" fork && return
    saved_period=${SERVER_PERIOD:-}
    [ "$san" = tsan ] || SERVER_PERIOD=256     # TSan keeps its long periods
    if start_server "$1" "$out/$name.server.log"; then
        if LD_LIBRARY_PATH="$fork_lib" timeout 120 "$out/notify_lag" "$2" >"$out/$name.log" 2>&1; then
            echo "PASS $name: $(tail -1 "$out/$name.log" | sed 's/^PASS: //')"
        elif [ "$3" = 1 ]; then
            echo "FAIL $name: $(tail -1 "$out/$name.log")"; fail=1
        else
            echo "INFO $name (expected to fail on unfixed servers): $(tail -1 "$out/$name.log" | sed 's/^FAIL: //')"
        fi
        stop_server || fail=1
    else
        echo "FAIL $name: server"; fail=1
    fi
    SERVER_PERIOD=$saved_period
}
lag_case fork slow    1
lag_case fork hung    1
lag_case fork bufsize 1
lag_case system slow  0

# A client whose socket is already closed while the server still notifies it: client A
# is stopped (SIGSTOP), so the request thread waits on its latency reply when a third
# client connects; B is killed meanwhile, and when the request thread goes on, B's
# notifications meet a closed socket before its hang-up is processed. 1.9.22 logged
# three error lines per notification ("Broken pipe", "Could not write notification",
# "ClientNotify fails"), as it did for every client still connected when the server
# stops; nothing is sent to a closed socket now. The lagging A is reported once.
closed_case() {  # closed_case <server> <gating 1|0>
    name="notify-closed_$1-server"
    skip_system "$name" "$1" fork && return
    saved_period=${SERVER_PERIOD:-}
    [ "$san" = tsan ] || SERVER_PERIOD=256
    if start_server "$1" "$out/$name.server.log"; then
        pb="$out/patchbay_client"
        rm -f "$out/$name.fifo"; mkfifo "$out/$name.fifo"; exec 7<>"$out/$name.fifo"
        LD_LIBRARY_PATH="$fork_lib" "$pb" slow_a <&7 >/dev/null 2>&1 & pa=$!
        sleep 0.3
        LD_LIBRARY_PATH="$fork_lib" "$pb" victim_b <&7 >/dev/null 2>&1 & pv=$!
        sleep 0.5
        kill -STOP $pa
        LD_LIBRARY_PATH="$fork_lib" "$pb" trigger <&7 >/dev/null 2>&1 & pt=$!
        sleep 0.15
        kill -KILL $pv
        sleep 3
        kill -CONT $pa
        sleep 1
        exec 7>&-
        kill $pa $pt 2>/dev/null; wait $pa $pt $pv 2>/dev/null
        rm -f "$out/$name.fifo"
        n=$(grep -c 'Broken pipe\|Could not write notification\|ClientNotify fails' "$out/$name.server.log")
        lag=$(grep -c 'slow_a did not answer' "$out/$name.server.log")
        if [ "$n" = 0 ] && [ "$lag" = 1 ]; then
            echo "PASS $name: no notification sent to the dead client's closed socket; the stopped one reported once"
        elif [ "$2" = 1 ]; then
            echo "FAIL $name: $n notification error lines, slow client reported $lag times"; fail=1
        else
            echo "INFO $name (expected to fail on unfixed servers): $n notification error lines"
        fi
        stop_server || fail=1
    else
        echo "FAIL $name: server"; fail=1
    fi
    SERVER_PERIOD=$saved_period
}
closed_case fork   1
closed_case system 0

if [ -n "$san" ]; then
    echo "SKIP rt-syscalls (sanitizer run)"
elif command -v strace >/dev/null 2>&1; then
    "$here/rt_syscalls.sh" fork || fail=1
else
    echo "SKIP rt-syscalls: strace not installed"
fi

# the JackBus socket: fd-passing clients beside protocol-9 ones (fd framing, mixed chains,
# no /dev/shm, protocol errors, refnum spoofing, a server without the socket)
NSPA_SANITIZER="$san" NSPA_TEST_CFLAGS="$tcf" "$here/bus.sh" || fail=1
# real programs (jack-example-tools, fluidsynth) over it; uninstrumented binaries
if [ -n "$san" ]; then
    echo "SKIP bus-apps (sanitizer run)"
else
    "$here/bus-apps.sh" || fail=1
    # a real DAW: Ardour headless (hardour), skipped without Ardour 9
    "$here/bus-ardour.sh" || fail=1
    # PipeWire applications through jackbus-pw (the libpipewire shim on JackBus nodes):
    # needs the shim and PipeWire's examples built (make -C nspa/pw-shim all test-apps
    # PW_EXAMPLES=<pipewire tree>/src/examples)
    if [ -x "$JACK_BUILD/pw-shim/audio-src" ] && [ -e "$JACK_BUILD/pw-shim/lib/libpipewire-0.3.so.0" ]; then
        "$root/nspa/pw-shim/test/run-example.sh" || fail=1
        # the system's PipeWire clients (pw-play, mpv, fluidsynth, SDL3, OpenAL, ...)
        "$root/nspa/pw-shim/test/real-apps.sh" || fail=1
    else
        echo "SKIP jackbus-pw (the shim and PipeWire's examples are not built)"
    fi
fi

if [ -x "$JACK_BUILD/dbus/jackdbus" ]; then
    "$here/jackdbus.sh" "$JACK_BUILD/dbus/jackdbus" 2>&1 | grep -v 'Segmentation fault' || true
    "$here/jackdbus.sh" "$JACK_BUILD/dbus/jackdbus" >/dev/null 2>&1 || fail=1
fi

# sound card hotplug in jackdbus: a real card "unplugged" through bind mounts (skips without
# two cards)
if [ -x "$JACK_BUILD/dbus/jackdbus" ]; then
    "$here/hotplug.sh" "$JACK_BUILD/dbus/jackdbus" || fail=1
fi

# sound card bridges through jackdbus: the "bridges" parameter, Add/Remove/GetBridges, buffer
# size and master rate changes (on snd-aloop: skips without it)
if [ -n "$san" ]; then
    echo "SKIP jackdbus-bridge (sanitizer run)"
elif [ -x "$JACK_BUILD/dbus/jackdbus" ]; then
    "$here/bridge/jackdbus-bridge.sh" || fail=1
fi

if [ "$san" = tsan ]; then
    # Under TSan, JACK's asynchronous thread cancellation aborts clients at
    # close and timing checks are meaningless: only the reports gate the run.
    fail=0
fi
if [ -n "$san" ]; then
    reports=$(cat "$out"/asan.* "$out"/ubsan.* "$out"/tsan.* 2>/dev/null | grep -c -E '^(==[0-9]+==ERROR|WARNING: ThreadSanitizer|.*runtime error:)')
    echo "SANITIZER $san: $reports report(s) in $out"
    if [ "$reports" -gt 0 ]; then
        # distinct reports by their two top frames
        cat "$out"/asan.* "$out"/ubsan.* "$out"/tsan.* 2>/dev/null | awk '
            /^(==[0-9]+==ERROR|WARNING: ThreadSanitizer)/ { kind=$0; sub(/ \(pid=.*/, "", kind); n=0; key=""; next }
            /runtime error:/ { print "  " $0; next }
            /^ *#0 / && kind != "" { f=$0; sub(/^ *#0 /, "", f); sub(/ \(.*$/, "", f); key = key (n ? " | " : "") f; n++
                                     if (n == 2) { print "  " kind ": " key; kind="" } }' | sort | uniq -c | sort -rn | head -20
        fail=1
    fi
fi

exit $fail
