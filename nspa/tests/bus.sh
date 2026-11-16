#!/bin/sh
# The JackBus socket ($XDG_RUNTIME_DIR/jackbus/<server>): clients that get the
# server's objects as fds, in one graph with protocol-9 clients.
#
#   nspa/run-isolated.sh nspa/tests/bus.sh
#
# Checks: what a JackBus client maps (no shm registry); sample-exact chains
# mixing JackBus clients, protocol-9 clients of this tree's libjack and of the
# system's; a JackBus and a protocol-9 client in one process; a JackBus client
# with an empty /dev/shm; protocol errors (the server drops the connection and
# keeps serving); a server without the socket; nothing left behind.
set -u
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    echo "refusing to run outside nspa/run-isolated.sh" >&2
    exit 2
fi
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${NSPA_TEST_OUT:-/tmp/JackBus-tests}/bus"
mkdir -p "$out"
fork_lib="$JACK_BUILD/common"
tcf="${NSPA_TEST_CFLAGS:-}"   # the suite's extra flags (sanitizers)
san="${NSPA_SANITIZER:-}"
# its protocol-9 clients must stay protocol 9 when the suite runs over the JackBus socket
unset JACKBUS_TRANSPORT

cc -O2 $tcf -Wall -I"$root/common" -o "$out/bus_client" "$here/bus_client.c" -L"$fork_lib" -ljack -ldl || exit 1
cc -O2 $tcf -Wall -o "$out/bus_proto" "$here/bus_proto.c" -lpthread || exit 1
cc -O2 $tcf -Wall -I"$root/common" -o "$out/bus_node" "$here/bus_node.c" -L"$fork_lib" -ljack -lpthread || exit 1
c++ -std=gnu++11 -O2 $tcf -Wall -I"$root/posix" -o "$out/fd_trailer" "$here/fd_trailer.cpp" "$root/posix/JackFdPassing.cpp" -lpthread || exit 1

export JACK_DEFAULT_SERVER=jbbus
fail=0
server_pid=

pass() { echo "PASS $1"; }
flunk() { echo "FAIL $1"; shift; for l in "$@"; do [ -f "$l" ] && sed 's/^/    /' "$l"; done; fail=1; }

start_server() {  # [env...]
    rm -f /dev/shm/jack_jbbus_*
    env "$@" "$JACK_BUILD/jackd" -n jbbus -R -P 70 -d dummy -r 48000 -p 128 >"$out/jackd.log" 2>&1 &
    server_pid=$!
    i=0
    while [ $i -lt 50 ]; do
        ls /dev/shm/jack_jbbus_*_0 >/dev/null 2>&1 && return 0
        kill -0 "$server_pid" 2>/dev/null || return 1
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

stop_server() {
    kill -TERM "$server_pid" 2>/dev/null
    i=0
    while kill -0 "$server_pid" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    if kill -0 "$server_pid" 2>/dev/null; then kill -KILL "$server_pid"; return 1; fi
    wait "$server_pid" 2>/dev/null
    return 0
}

bc() { timeout 30 "$out/bus_client" "$@"; }                                      # this tree's libjack
bc_sys() { env -u LD_LIBRARY_PATH timeout 30 "$out/bus_client" "$@"; }          # the system's libjack
bc_noshm() { timeout 30 unshare --mount sh -c 'mount -t tmpfs nspa-empty /dev/shm && exec "$@"' sh "$out/bus_client" "$@"; }

# chain <name> <src-cmd> <thru-cmd> <sink-cmd>: each "<runner> [-b]", src -> thru -> sink
chain() {
    name=$1; s=$2; t=$3; k=$4
    $s -s 4 src "$name-src" >"$out/$name.src" 2>&1 & sp=$!
    $t -s 4 thru "$name-thru" "$name-src:out" >"$out/$name.thru" 2>&1 & tp=$!
    sleep 0.3
    if $k -s 2 sink "$name-sink" "$name-thru:out" >"$out/$name.sink" 2>&1; then rc=0; else rc=1; fi
    wait $sp || rc=1
    wait $tp || rc=1
    if [ $rc -eq 0 ]; then pass "$name: $(cat "$out/$name.sink")"; else flunk "$name" "$out/$name.src" "$out/$name.thru" "$out/$name.sink"; fi
}

# 0. framing
if "$out/fd_trailer" >"$out/fd_trailer.log" 2>&1; then pass "fd_trailer"; else flunk "fd_trailer" "$out/fd_trailer.log"; fi

if ! start_server; then
    flunk "server start" "$out/jackd.log"
    exit 1
fi
[ -S "$XDG_RUNTIME_DIR/jackbus/jbbus" ] && pass "socket $XDG_RUNTIME_DIR/jackbus/jbbus" || flunk "no JackBus socket"

# 1. what each kind of client maps
if bc -b probe probe-bus >"$out/probe-bus" 2>&1 && grep -q 'registry 0, segments 3, futex files [0-9]*, alive table 1' "$out/probe-bus"; then
    pass "probe: JackBus client, $(head -1 "$out/probe-bus" | cut -d' ' -f2-)"
else
    flunk "probe JackBus" "$out/probe-bus"
fi
if bc probe probe-p9 >"$out/probe-p9" 2>&1 && grep -q 'registry 1' "$out/probe-p9"; then
    pass "probe: protocol-9 client, $(head -1 "$out/probe-p9" | cut -d' ' -f2-)"
else
    flunk "probe protocol 9" "$out/probe-p9"
fi

# 2. mixed chains, sample-exact
chain p9-bus-p9   "bc"        "bc -b"     "bc"
if [ -n "$san" ]; then
    echo "SKIP bus-sys-bus, sys-bus-sys (sanitizer run: no uninstrumented system libjack)"
else
    chain bus-sys-bus "bc -b"     "bc_sys"    "bc -b"
    chain sys-bus-sys "bc_sys"    "bc -b"     "bc_sys"
fi
chain bus-bus-bus "bc -b"     "bc -b"     "bc -b"

# 3. a protocol-9 and a JackBus client in one process (shared libjack globals)
bc -b -s 4 src pair-src >"$out/pair.src" 2>&1 & sp=$!
bc -s 4 pair pair pair-src:out >"$out/pair.pair" 2>&1 & pp=$!
sleep 0.5
if bc -s 2 sink pair-sink pair-bus:out >"$out/pair.sink" 2>&1 && wait $pp && wait $sp; then
    pass "one process, both kinds: $(cat "$out/pair.sink")"
else
    wait $pp; wait $sp; flunk "one process, both kinds" "$out/pair.src" "$out/pair.pair" "$out/pair.sink"
fi

# 4. a JackBus client with an empty /dev/shm (as in a sandbox without --device=shm)
chain noshm "bc" "bc_noshm -b" "bc"
if bc_noshm probe noshm-p9 >"$out/noshm-p9" 2>&1; then
    flunk "a protocol-9 client must fail without /dev/shm" "$out/noshm-p9"
else
    pass "without /dev/shm a protocol-9 client fails, as expected"
fi

# 5. protocol errors: dropped or refused, and the server goes on
for c in p9open garbage badver notrailer partial; do
    if timeout 20 "$out/bus_proto" jbbus $c >"$out/proto-$c" 2>&1 && bc -b probe "after-$c" >"$out/after-$c" 2>&1; then
        pass "protocol error $c: $(cat "$out/proto-$c")"
    else
        flunk "protocol error $c" "$out/proto-$c" "$out/after-$c"
    fi
done
# a JackBus client acts for itself only
bc -s 6 src victim >"$out/victim" 2>&1 & vp=$!
sleep 0.5
if timeout 20 "$out/bus_proto" jbbus spoof victim >"$out/proto-spoof" 2>&1; then
    pass "$(cat "$out/proto-spoof")"
else
    flunk "spoof" "$out/proto-spoof"
fi
kill $vp 2>/dev/null; wait $vp 2>/dev/null
# a silent connection doesn't hold anyone up
timeout 20 "$out/bus_proto" jbbus silent >"$out/proto-silent" 2>&1 & pp=$!
sleep 0.5
t0=$(date +%s%N)
bc -b probe during-silent >"$out/during-silent" 2>&1; rc=$?
t1=$(date +%s%N)
wait $pp || rc=1
ms=$(( (t1 - t0) / 1000000 ))
if [ $rc -eq 0 ] && [ $ms -lt 1500 ]; then pass "a silent connection holds nobody up (probe took $ms ms)"; else flunk "silent connection (probe $ms ms)" "$out/proto-silent" "$out/during-silent"; fi

# 6. the native API: nodes (cycle record, buffer table, events, ports while running)
if timeout 90 "$out/bus_node" full >"$out/node-full" 2>&1; then
    pass "nodes: $(grep -c '^PASS' "$out/node-full") checks ($(grep '^PASS audio through' "$out/node-full" | cut -d' ' -f2-))"
else
    flunk "nodes" "$out/node-full"
fi
# writing into an unconnected input (the shared zero buffer) is a crash, and only
# the writer's; the server goes on
# (under ASan, its SEGV handler would catch the deliberate fault: the kernel's must)
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}handle_segv=0" timeout 20 "$out/bus_node" rtwrite >"$out/node-rtwrite" 2>&1; rc=$?
if [ $rc -eq 139 ] && bc -b probe after-rtwrite >"$out/after-rtwrite" 2>&1; then
    pass "a node writing the zero buffer crashes (SIGSEGV), the server goes on"
else
    flunk "zero buffer write (exit $rc)" "$out/node-rtwrite" "$out/after-rtwrite"
fi
# a node's realtime thread: no system call but futex in its cycles. The node is
# started under strace, so its realtime thread is traced from its creation: it is
# the thread that makes itself SCHED_FIFO, and its cycles run from then until the
# cancellation signal of its close (thread set-up before, teardown after)
rt_cycles() {  # rt_cycles <strace file> -> the realtime thread's lines in its cycles
    rt=$(grep -E 'sched_setscheduler\([0-9]+, SCHED_FIFO' "$1" | head -1 | awk '{print $1}')
    [ -n "$rt" ] || return 1
    awk -v t="$rt" '$1 == t' "$1" | sed -n '/sched_setscheduler(/,$p' | sed '1d' | sed '/<\.\.\. sched_setscheduler resumed>/d' | sed '/ --- SIG/,$d'
}
if [ -n "$san" ]; then
    echo "SKIP node RT system calls (sanitizer run)"
elif command -v strace >/dev/null; then
    timeout 30 strace -f -qq -o "$out/node-steady.strace" "$out/bus_node" steady 3 >"$out/node-steady" 2>&1
    if rt_cycles "$out/node-steady.strace" >"$out/node-steady.loop" && [ -s "$out/node-steady.loop" ]; then
        calls=$(sed -E 's/^[0-9]+ +//; s/\(.*//; s/^<\.\.\. ([a-z_0-9]+) resumed>.*/\1/' "$out/node-steady.loop" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}')
        others=$(grep -vcE '^[0-9]+ +(futex\(|<\.\.\. futex resumed>)' "$out/node-steady.loop")
        if [ "$others" -eq 0 ]; then
            pass "a node's realtime thread, in its cycles from the first on: no system call but futex ($calls)"
        else
            flunk "node RT system calls in its cycles ($calls)" "$out/node-steady.loop"
        fi
    else
        flunk "node RT system calls: no realtime thread traced" "$out/node-steady"
    fi
    # the negative control: one system call in a cycle must be seen
    BUS_NODE_RT_SYSCALL=1 timeout 30 strace -f -qq -o "$out/node-ctl.strace" "$out/bus_node" steady 1 >"$out/node-ctl" 2>&1
    if rt_cycles "$out/node-ctl.strace" | grep -q 'getppid('; then
        pass "the trace sees a system call made in a cycle (negative control)"
    else
        flunk "the trace missed a system call made in a cycle" "$out/node-ctl"
    fi
fi

if grep -qE 'Segmentation|Assertion|terminate called' "$out/jackd.log"; then flunk "server log" "$out/jackd.log"; fi
stop_server || flunk "server did not exit on SIGTERM"

# 7. nothing left behind
left=$(ls /dev/shm | grep -E '^jack_sem\.|^jack_alive\.|^jack-[0-9]+-[0-9]+$' | tr '\n' ' ')
[ -z "$left" ] && pass "nothing left in /dev/shm" || flunk "left in /dev/shm: $left"
[ ! -e "$XDG_RUNTIME_DIR/jackbus/jbbus" ] && pass "socket removed at exit" || flunk "socket left behind"

# 8. the server dies under a running node (SIGKILL, as a crash) and stops (SIGTERM):
# a SHUTDOWN event comes, the process stops, the close returns
for sig in KILL TERM; do
    if start_server; then
        timeout 30 "$out/bus_node" shutdown >"$out/node-shutdown-$sig" 2>&1 & np=$!
        i=0; while [ $i -lt 100 ] && ! grep -q '^ready' "$out/node-shutdown-$sig"; do sleep 0.1; i=$((i + 1)); done
        sleep 0.5
        kill -$sig "$server_pid"; wait "$server_pid" 2>/dev/null
        if wait $np && grep -q '^PASS' "$out/node-shutdown-$sig"; then
            pass "server SIG$sig under a running node: $(grep -E '^PASS' "$out/node-shutdown-$sig" | cut -d' ' -f2-)"
        else
            flunk "server SIG$sig under a running node" "$out/node-shutdown-$sig"
        fi
        rm -f /dev/shm/jack_jbbus_* /dev/shm/jack-[0-9]* /dev/shm/jack_sem.* /dev/shm/jack_alive.* "$XDG_RUNTIME_DIR/jackbus/jbbus"
    else
        flunk "server start (shutdown case)" "$out/jackd.log"
    fi
done

# 9. a server without the socket: JackBus clients fail at once, the rest works
if start_server JACK_NO_BUS_SOCKET=1; then
    if bc -b probe nosock >"$out/nosock" 2>&1; then flunk "JackBus open must fail without the socket" "$out/nosock"; else pass "no socket: jackbus_client_open fails cleanly"; fi
    bc probe nosock-p9 >"$out/nosock-p9" 2>&1 && pass "no socket: protocol 9 unaffected" || flunk "no socket: protocol 9" "$out/nosock-p9"
    stop_server || flunk "server did not exit on SIGTERM"
else
    flunk "server start (no socket)" "$out/jackd.log"
fi

echo "logs: $out"
[ $fail -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
