#!/bin/sh
# A node with priority (jackbus_node_set_priority), on an isolated JackBus
# (dummy driver, 48000 Hz, 256 frames, -P 70): eight nodes the freewheel
# driver wakes at each cycle's start (prio_node), the one opened last (the
# highest refnum: woken last in refnum order) with priority or without. Each
# notes how long after the cycle's start it began (the median of its cycles).
#  - without priority: it starts after the first node;
#  - with: before it (woken first), its realtime thread one step above the
#    others' (66 against 65), and every node still runs every cycle.
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

"$JACK_BUILD/jackd" -n jbprio -R -P 70 -d dummy -r 48000 -p 256 >"$logs/jackd.log" 2>&1 &
sp=$!
i=0; while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbprio_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done

run() {  # with: "priority" or ""
    pids=""
    for n in 1 2 3 4 5 6 7; do
        "$logs/prio_node" "n$n" "$logs/n$n.$1" >/dev/null 2>&1 &
        pids="$pids $!"
        sleep 0.15
    done
    "$logs/prio_node" p "$logs/p.$1" $1 >/dev/null 2>&1 &
    pids="$pids $!"
    sleep 4
    kill -TERM $pids; wait $pids 2>/dev/null
}

run plain
run priority
p0=$(field "$logs/p.plain" median); f0=$(field "$logs/n1.plain" median)
p1=$(field "$logs/p.priority" median); f1=$(field "$logs/n1.priority" median)
pr=$(field "$logs/p.priority" prio); nr=$(field "$logs/n1.priority" prio)
c1=$(field "$logs/p.priority" cycles); cn=$(field "$logs/n7.priority" cycles)
later=$(echo "$p0 > $f0" | bc); earlier=$(echo "$p1 < $f1" | bc)
if [ "$later" = 1 ]; then
    pass "without priority, woken in refnum order: the last node starts $p0 us into the cycle, the first $f0 us"
else
    flunk "without priority: the last node at $p0 us, the first at $f0 us (expected later)"
fi
if [ "$earlier" = 1 ] && [ "$pr" -eq $((nr + 1)) ] && [ "$c1" -gt 500 ] && [ "$cn" -gt 500 ]; then
    pass "with priority: it starts $p1 us into the cycle, before the first node ($f1 us); realtime priority $pr against $nr; the others run every cycle too ($cn cycles)"
else
    flunk "with priority: at $p1 us against $f1 us; priority $pr against $nr; cycles $c1, $cn" "$logs/jackd.log"
fi

kill -TERM $sp; wait $sp 2>/dev/null
echo "logs: $logs"
[ $fail -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
