#!/bin/sh
# Real JACK programs over the JackBus socket: unmodified binaries of
# jack-example-tools (and fluidsynth, Ardour's headless Lua session) that link
# libjack.so.0, loading this tree's libjack with JACKBUS_TRANSPORT=1, in one
# graph with protocol-9 clients. Dummy driver: nothing reaches a sound card.
#
#   nspa/run-isolated.sh nspa/tests/bus-apps.sh
#
# Each check states what it proves. JACKBUS_TRANSPORT=1 has no fallback: a
# program whose JackBus open failed fails. The long-running ones are also
# checked in /proc/<pid>/maps: no shm registry mapped, the alive table mapped.
set -u
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    echo "refusing to run outside nspa/run-isolated.sh" >&2
    exit 2
fi
here="$(cd "$(dirname "$0")" && pwd)"
out="${NSPA_TEST_OUT:-/tmp/JackBus-tests}/bus-apps"
rm -rf "$out"; mkdir -p "$out"
unset JACKBUS_TRANSPORT
export JACK_DEFAULT_SERVER=jbapps
fail=0
pids=""

pass() { echo "PASS $1"; }
flunk() { echo "FAIL $1"; shift; for l in "$@"; do [ -f "$l" ] && tail -15 "$l" | sed 's/^/    /'; done; fail=1; }
bus() { env JACKBUS_TRANSPORT=1 "$@"; }      # over the JackBus socket
p9() { env -u JACKBUS_TRANSPORT "$@"; }      # over protocol 9
# bg <bus|p9> <program> [args]: started through env, never a shell function, so that
# $! is the program itself (a backgrounded function's $! is a subshell: killing it
# would orphan the program)
bg() {
    mode=$1; shift
    # line-buffered: their output is checked while they run (stdbuf execs the program)
    if [ "$mode" = bus ]; then env JACKBUS_TRANSPORT=1 stdbuf -oL "$@" & else env -u JACKBUS_TRANSPORT stdbuf -oL "$@" & fi
    pids="$pids $!"
}
lsp() { p9 timeout 10 jack_lsp "$@" 2>/dev/null; }
bus_checked=0

# on_bus <pid> <label>: the program maps what a JackBus client maps
on_bus() {
    if [ -r "/proc/$1/maps" ] && ! grep -q 'jack-shm-registry' "/proc/$1/maps" && grep -q 'jack_alive\.' "/proc/$1/maps"; then
        bus_checked=$((bus_checked + 1)); return 0
    fi
    echo "    $2 (pid $1): not a JackBus client by its maps"; return 1
}
last_pid() { echo "$pids" | awk '{print $NF}'; }

# wait_ports <client-regex> <count>: until that many ports of the client exist
wait_ports() {
    i=0
    while [ $i -lt 100 ]; do
        [ "$(lsp | grep -cE "^$1:")" -ge "$2" ] && return 0
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

stop_all() {
    for p in $pids; do kill -TERM "$p" 2>/dev/null; done
    for p in $pids; do wait "$p" 2>/dev/null; done
    pids=""
}

if ! command -v jack_lsp >/dev/null || ! command -v jack_metro >/dev/null; then
    echo "SKIP bus-apps: jack-example-tools not installed"
    exit 0
fi

rm -f /dev/shm/jack_jbapps_*
"$JACK_BUILD/jackd" -n jbapps -R -P 70 -d dummy -r 48000 -p 256 >"$out/jackd.log" 2>&1 &
server_pid=$!
i=0
while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbapps_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done

# jack_wait: is the server there?
if bus timeout 10 jack_wait -c >"$out/wait" 2>&1 && grep -q running "$out/wait"; then pass "jack_wait -c: $(cat "$out/wait")"; else flunk "jack_wait" "$out/wait"; fi

# jack_simple_client: a sine, auto-connected to system:playback (by itself)
bg bus jack_simple_client >"$out/simple" 2>&1
if wait_ports jack_simple_client 2 && lsp -c | grep -A1 '^jack_simple_client:output1' | grep -q 'system:playback' && on_bus "$(last_pid)" jack_simple_client; then
    pass "jack_simple_client: its outputs reach system:playback"
else
    flunk "jack_simple_client" "$out/simple"
fi

# jack_metro, connected and disconnected by jack_connect/jack_disconnect (all JackBus)
bg bus jack_metro -n metro -b 120 >"$out/metro" 2>&1
if wait_ports metro 1 && on_bus "$(last_pid)" jack_metro; then
    mport=$(lsp | grep '^metro:' | head -1)
    bus timeout 10 jack_connect "$mport" system:playback_2 >"$out/connect" 2>&1
    c1=$(bus timeout 10 jack_lsp -c "$mport" | grep -c 'system:playback_2')
    bus timeout 10 jack_disconnect "$mport" system:playback_2 >>"$out/connect" 2>&1
    c2=$(bus timeout 10 jack_lsp -c "$mport" | grep -c 'system:playback_2')
    [ "$c1" = 1 ] && [ "$c2" = 0 ] && pass "jack_metro + jack_connect/jack_disconnect/jack_lsp -c" || flunk "connect/disconnect ($c1 $c2)" "$out/connect"
else
    flunk "jack_metro" "$out/metro"
fi

# MIDI: jack_midiseq (JackBus) -> jack_midi_dump (JackBus) and -> jack_midisine (protocol 9)
bg bus jack_midiseq Sequencer 24000 0 60 6000 12000 64 6000 >"$out/midiseq" 2>&1
bg bus jack_midi_dump dump >"$out/mididump" 2>&1
dump_pid=$(last_pid)
bg p9 jack_midisine >"$out/midisine" 2>&1
if wait_ports Sequencer 1 && wait_ports dump 1 && wait_ports midisine 2 && on_bus "$dump_pid" jack_midi_dump; then
    seqport=$(lsp | grep '^Sequencer:' | head -1)
    dumpport=$(lsp | grep '^dump:' | head -1)
    sineport=$(lsp -t | grep -B1 midi | grep '^midisine:' | head -1)
    p9 timeout 10 jack_connect "$seqport" "$dumpport"
    p9 timeout 10 jack_connect "$seqport" "$sineport"
    sleep 2
    notes=$(grep -c 'note on' "$out/mididump")
    [ "$notes" -ge 2 ] && pass "MIDI jack_midiseq -> jack_midi_dump over JackBus: $notes note-ons in 2 s (and -> jack_midisine, protocol 9)" \
        || flunk "MIDI chain ($notes note-ons)" "$out/mididump" "$out/midiseq"
else
    flunk "MIDI clients" "$out/midiseq" "$out/mididump" "$out/midisine"
fi

# jack_lsp: the whole graph, latencies and types, as a JackBus client sees it
if bus timeout 10 jack_lsp -A -c -l -L -p -t >"$out/lsp" 2>&1 && grep -q '^midisine:' "$out/lsp" && grep -q 'total latency' "$out/lsp"; then
    pass "jack_lsp -AclLpt: $(grep -c '^[^ 	]' "$out/lsp") ports, protocol-9 and JackBus clients"
else
    flunk "jack_lsp" "$out/lsp"
fi

# jack_evmon (JackBus) sees a protocol-9 client come and go, with its port and connection
bg bus jack_evmon >"$out/evmon" 2>&1
ev_pid=$(last_pid)
sleep 0.5
env -u JACKBUS_TRANSPORT jack_metro -n evtest -b 60 >/dev/null 2>&1 & ep=$!
sleep 1
kill -TERM $ep; wait $ep 2>/dev/null
sleep 0.5
if grep -q 'Client evtest registered' "$out/evmon" && grep -q 'Client evtest unregistered' "$out/evmon" && on_bus "$ev_pid" jack_evmon; then
    pass "jack_evmon over JackBus: $(grep -c . "$out/evmon") events, a protocol-9 client's life among them"
else
    flunk "jack_evmon" "$out/evmon"
fi

# jack_bufsize: a JackBus client changes the period; every client follows
bus timeout 10 jack_bufsize 512 >"$out/bufsize" 2>&1
sleep 0.5
now=$(p9 timeout 10 jack_bufsize 2>/dev/null | grep -o '[0-9]*' | head -1)
bus timeout 10 jack_bufsize 256 >>"$out/bufsize" 2>&1
sleep 0.5
back=$(bus timeout 10 jack_bufsize 2>/dev/null | grep -o '[0-9]*' | head -1)
[ "$now" = 512 ] && [ "$back" = 256 ] && pass "jack_bufsize 512 -> 256 (seen by both kinds of client)" || flunk "jack_bufsize ($now $back)" "$out/bufsize"

# jack_samplerate
sr=$(bus timeout 10 jack_samplerate 2>/dev/null | tail -1)
[ "$sr" = 48000 ] && pass "jack_samplerate: $sr" || flunk "jack_samplerate: '$sr'"

# metadata: set over JackBus, read over protocol 9 (one shared database)
bus timeout 10 jack_property -p -s system:playback_1 http://jackaudio.org/metadata/pretty-name "Left speaker" >"$out/prop" 2>&1
if p9 timeout 10 jack_property -p -l system:playback_1 2>&1 | tee -a "$out/prop" | grep -q 'Left speaker'; then
    pass "jack_property: set over JackBus, read over protocol 9"
else
    flunk "jack_property" "$out/prop"
fi
bus timeout 10 jack_property -p -d system:playback_1 >/dev/null 2>&1

# freewheel on and off, from a JackBus client; the running clients survive it
bus timeout 10 jack_freewheel y >"$out/fw" 2>&1
sleep 1
bus timeout 10 jack_freewheel n >>"$out/fw" 2>&1
sleep 0.5
if kill -0 $(echo $pids | cut -d' ' -f1) 2>/dev/null && wait_ports metro 1; then pass "jack_freewheel y/n: clients still running"; else flunk "freewheel" "$out/fw"; fi

# jack_iodelay through a JackBus thru client: a measured round trip
bg bus jack_iodelay >"$out/iodelay" 2>&1
io_pid=$(last_pid)
bg bus jack_thru_client >"$out/thru" 2>&1
if wait_ports jack_delay 2 && wait_ports jack_thru_client 2 && on_bus "$io_pid" jack_iodelay && on_bus "$(last_pid)" jack_thru_client; then
    din=$(lsp | grep '^jack_delay:in' | head -1); dout=$(lsp | grep '^jack_delay:out' | head -1)
    tin=$(lsp | grep '^jack_thru_client:input' | head -1); tout=$(lsp | grep '^jack_thru_client:output' | head -1)
    p9 timeout 10 jack_connect "$dout" "$tin"; p9 timeout 10 jack_connect "$tout" "$din"
    sleep 3
    # the loop closes through a feedback connection: exactly one period
    rt=$(grep 'total roundtrip' "$out/iodelay" | tail -1 | tr -s ' ')
    if echo "$rt" | grep -q ' 256.000 frames'; then
        pass "jack_iodelay -> jack_thru_client -> jack_iodelay:$rt"
    else
        flunk "jack_iodelay" "$out/iodelay"
    fi
else
    flunk "iodelay/thru clients ($(lsp | grep -E 'delay|thru' | tr '\n' ' '))" "$out/iodelay" "$out/thru"
fi

# jack_rec: 2 s from system:capture_1 into a file
if bus timeout 20 jack_rec -f "$out/rec.wav" -d 2 system:capture_1 >"$out/rec" 2>&1 && [ "$(stat -c %s "$out/rec.wav" 2>/dev/null || echo 0)" -gt 100000 ]; then
    pass "jack_rec: $(stat -c %s "$out/rec.wav") bytes in 2 s"
else
    flunk "jack_rec" "$out/rec"
fi

# fluidsynth's JACK audio and MIDI drivers (no soundfont needed for the ports)
if command -v fluidsynth >/dev/null; then
    bg bus fluidsynth -a jack -m jack -i -s -o audio.jack.id=fluid -o midi.jack.id=fluidmidi >"$out/fluid" 2>&1
    # (it opens one client for audio and MIDI)
    if wait_ports 'fluid[a-z]*' 3 && on_bus "$(last_pid)" fluidsynth; then
        p9 timeout 10 jack_connect "$seqport" "$(lsp -t | grep -B1 midi | grep -E '^fluid' | head -1)" 2>>"$out/fluid"
        sleep 1
        pass "fluidsynth: $(lsp | grep -cE '^fluid[a-z]*:') ports over JackBus, fed by jack_midiseq"
    else
        flunk "fluidsynth" "$out/fluid"
    fi
fi

# a stuck client (jack_zombie never returns from a process callback): the server
# copes as it does with any stuck client, and JackBus clients still open afterwards
bg bus jack_zombie >"$out/zombie" 2>&1
sleep 4
if bus timeout 10 jack_lsp >/dev/null 2>&1 && kill -0 "$server_pid"; then pass "jack_zombie: the server and JackBus opens survive it"; else flunk "zombie" "$out/zombie" "$out/jackd.log"; fi

stop_all
left=$(lsp | grep -v '^system:' | cut -d: -f1 | sort -u | tr '\n' ' ')
[ -z "$left" ] && pass "every program exited on SIGTERM" || flunk "still connected after SIGTERM: $left"
kill -TERM "$server_pid"; wait "$server_pid" 2>/dev/null
grep -qE 'Segmentation|Assertion|terminate called|refused' "$out/jackd.log" && flunk "server log" "$out/jackd.log"
[ "$bus_checked" -ge 7 ] && pass "$bus_checked long-running programs checked JackBus clients by their maps" || flunk "only $bus_checked programs checked by their maps"

echo "logs: $out"
[ $fail -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
