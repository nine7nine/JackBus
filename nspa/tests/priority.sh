#!/bin/sh
# A node with priority (jackbus_node_set_priority), on an isolated JackBus
# (dummy driver, 48000 Hz, 256 frames, -P 70): eight nodes the freewheel
# driver wakes at each cycle's start (prio_node), the one opened last (the
# highest refnum: woken last in refnum order) with priority or without. Each
# notes how long after the cycle's start it began (the median of its cycles).
#  - the server takes the flag; its realtime thread runs one step above the
#    others' (66 against 65), and every node still runs every cycle;
#  - woken first: it starts earlier, against the last ordinary node, than
#    without priority (three runs of each: CPU placement and idle states move
#    a node's start as much as its place in the wake order).
#
#   nspa/tests/priority.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$tree/nspa/run-isolated.sh" "$0" "$@"
fi
logs=$(mktemp -d /tmp/jbprio.XXXXXX)
cc -O2 -I"$tree/common" -o "$logs/prio_node" "$here/prio_node.c" -L"$JACK_BUILD/common" -ljack -lpthread ||
    { echo "can't build prio_node"; exit 2; }
export JACK_DEFAULT_SERVER=jbprio
fail=0
pass() { echo "PASS $1"; }
flunk() { echo "FAIL $1"; shift; for l in "$@"; do [ -f "$l" ] && tail -8 "$l" | sed 's/^/    /'; done; fail=1; }
field() { awk -v k="$2" '{ for (i = 1; i < NF; i++) if ($i == k) print $(i + 1) }' "$1"; }

"$JACK_BUILD/jackd" -v -n jbprio -R -P 70 -d dummy -r 48000 -p 256 >"$logs/jackd.log" 2>&1 &
sp=$!
i=0; while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbprio_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done

run() {  # tag, "priority" or ""
    pids=""
    for n in 1 2 3 4 5 6 7; do
        "$logs/prio_node" "n$n" "$logs/n$n.$1" >/dev/null 2>&1 &
        pids="$pids $!"
        sleep 0.15
    done
    "$logs/prio_node" p "$logs/p.$1" $2 >/dev/null 2>&1 &
    pids="$pids $!"
    sleep 4
    kill -TERM $pids; wait $pids 2>/dev/null
}

# how much later than the last ordinary node (n7) the node p starts, in us (medians of a run).
# Its wake moves ~2 us per place in the order, but on which CPU, and from how deep an idle
# state, a node wakes moves it as much: three runs of each, alternated, averaged.
gap() { echo "$(field "$logs/p.$1" median) - $(field "$logs/n7.$1" median)" | bc; }
sum0=0; sum1=0
for k in 1 2 3; do
    run plain$k ""
    run prio$k priority
    sum0=$(echo "$sum0 + $(gap plain$k)" | bc); sum1=$(echo "$sum1 + $(gap prio$k)" | bc)
done
g0=$(echo "scale=1; $sum0 / 3" | bc); g1=$(echo "scale=1; $sum1 / 3" | bc)
pr=$(field "$logs/p.prio3" prio); nr=$(field "$logs/n1.prio3" prio)
c1=$(field "$logs/p.prio3" cycles); cn=$(field "$logs/n7.prio3" cycles)

if grep -q "ClientPriority ref = [0-9]* on" "$logs/jackd.log"; then
    pass "the server takes the flag: $(grep -c 'ClientPriority ref = [0-9]* on' "$logs/jackd.log") nodes flagged, one per run"
else
    flunk "no ClientPriority in the server's log" "$logs/jackd.log"
fi
if [ "$pr" = $((nr + 1)) ] && [ "$c1" -gt 500 ] && [ "$cn" -gt 500 ]; then
    pass "realtime priority $pr against the others' $nr; every node runs every cycle ($c1, $cn in 4 s)"
else
    flunk "priority $pr against $nr; cycles $c1, $cn"
fi
if [ "$(echo "$g1 < $g0 - 2" | bc)" = 1 ]; then
    pass "woken first: p starts $g1 us from the last ordinary node with priority, $g0 us without (3 runs each)"
else
    flunk "woken first: p at $g1 us from the last ordinary node with priority, $g0 us without (3 runs each)"
fi

kill -TERM $sp; wait $sp 2>/dev/null
echo "logs: $logs"
[ $fail -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
