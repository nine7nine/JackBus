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
cc -O2 $tcf -Wall -I"$root/common" -o "$out/rt_faults" "$here/rt_faults.c" -L"$fork_lib" -ljack || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/stale_client" "$here/stale_client.c" -L"$fork_lib" -ljack || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/mixdown" "$here/mixdown.c" -L"$fork_lib" -ljack || exit 1
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
if "$out/memops_consistency" >"$out/memops.log" 2>&1; then echo "PASS memops: $(tail -1 "$out/memops.log")"
else echo "FAIL memops:"; sed 's/^/  /' "$out/memops.log" | tail -6; fail=1; fi

# Input-port mixdown: exact sums of 0..6 sources, at a period that is not a
# multiple of the vector width (scalar tail) and at a normal one; 64- and
# 32-bit libjack (the multiversioned mixdown runs in the client).
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

if [ -n "$san" ]; then
    echo "SKIP rt-syscalls (sanitizer run)"
elif command -v strace >/dev/null 2>&1; then
    "$here/rt_syscalls.sh" fork || fail=1
else
    echo "SKIP rt-syscalls: strace not installed"
fi

if [ -x "$JACK_BUILD/dbus/jackdbus" ]; then
    "$here/jackdbus.sh" "$JACK_BUILD/dbus/jackdbus" 2>&1 | grep -v 'Segmentation fault' || true
    "$here/jackdbus.sh" "$JACK_BUILD/dbus/jackdbus" >/dev/null 2>&1 || fail=1
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
