#!/bin/sh
# JackBus: system calls made by the server's realtime driver thread while
# the graph changes (connect/disconnect) and xruns happen. Must run inside
# nspa/run-isolated.sh. The driver thread is identified as the thread doing the
# periodic clock_nanosleep() of the dummy driver. It must not write() /
# send*() anything (notifications and logging go through lock-free queues).
#
# Usage: rt_syscalls.sh <fork|system> [informational]
set -u
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    echo "refusing to run outside nspa/run-isolated.sh" >&2
    exit 2
fi
which=$1
gating=${2:-gating}
out="${NSPA_TEST_OUT:-/tmp/JackBus-tests}"
trace="$out/rt_syscalls.$which.strace"
stress="$out/stress64"
xrun="$out/xrun_timer"

if [ "$which" = fork ]; then
    env LD_LIBRARY_PATH="$JACK_BUILD/common" JACK_DRIVER_DIR="$JACK_BUILD" \
        "$JACK_BUILD/jackd" -n rtsys -S -R -P 70 -d dummy -r 48000 -p 128 >"$out/rt_syscalls.$which.log" 2>&1 &
    lib="$JACK_BUILD/common"
else
    env -u LD_LIBRARY_PATH -u JACK_DRIVER_DIR \
        /usr/bin/jackd -n rtsys -S -R -P 70 -d dummy -r 48000 -p 128 >"$out/rt_syscalls.$which.log" 2>&1 &
    lib=""
fi
pid=$!
# never leave a server or tracer behind
trap 'kill -KILL $pid $spid 2>/dev/null' EXIT
sleep 1
timeout 120 strace -f -qq -p $pid -o "$trace" &
spid=$!
sleep 1
JACK_DEFAULT_SERVER=rtsys LD_LIBRARY_PATH="$lib" timeout 60 "$stress" 4 40 >/dev/null 2>&1
JACK_DEFAULT_SERVER=rtsys LD_LIBRARY_PATH="$lib" timeout 60 "$xrun" >/dev/null 2>&1
kill -TERM $pid
i=0; while kill -0 $pid 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
kill -KILL $pid 2>/dev/null
wait $spid 2>/dev/null

rt=$(grep 'clock_nanosleep' "$trace" | awk '{print $1}' | sort | uniq -c | sort -rn | head -1 | awk '{print $2}')
if [ -z "$rt" ]; then
    echo "FAIL rt-syscalls-$which: could not identify the driver thread"; exit 1
fi
writes=$(awk -v t="$rt" '$1 == t' "$trace" | grep -cE ' (write|writev|send|sendto|sendmsg)\(')
summary=$(awk -v t="$rt" '$1 == t' "$trace" | sed -E 's/^[0-9]+ +//; s/\(.*//' | sort | uniq -c | sort -rn | awk '{printf "%s:%s ", $2, $1}')
if [ "$writes" -eq 0 ]; then
    echo "PASS rt-syscalls-$which: driver thread made no write/send calls ($summary)"
    exit 0
elif [ "$gating" = gating ]; then
    echo "FAIL rt-syscalls-$which: driver thread made $writes write/send calls ($summary)"
    exit 1
else
    echo "INFO rt-syscalls-$which (expected on unfixed servers): driver thread made $writes write/send calls ($summary)"
    exit 0
fi
