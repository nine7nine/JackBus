#!/bin/sh
# Async endpoints (the shims' settings pa.async, alsa.async, pw.async): an
# application frozen (SIGSTOP) for 2 s while it plays, on an isolated JackBus
# (dummy driver, 48000 Hz, 256 frames), beside another JACK client (the
# victim, xrun_victim) that runs every cycle and counts the xruns JACK tells
# every client of.
#  - setting off (the control): the application's node is in the graph's
#    cycle; frozen, its realtime thread is too, and the victim is told of
#    xruns;
#  - setting on: JackBus moves the application's stream itself; the victim
#    hears of no xrun and runs every cycle, the frozen stream only runs dry,
#    and the application plays on to its end once it runs again (exit 0).
# For paplay (jackbus-pa), aplay (jackbus-alsa) and pw-play (jackbus-pw); a
# missing program is skipped.
#
#   make -C nspa/pa-shim && make -C nspa/jackbus-alsa && make -C nspa/pw-shim &&
#   nspa/tests/async-stall.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$tree/nspa/run-isolated.sh" "$0" "$@"
fi
logs=$(mktemp -d /tmp/jbstall.XXXXXX)
cc -O2 -I"$tree/common" -o "$logs/xrun_victim" "$here/xrun_victim.c" -L"$JACK_BUILD/common" -ljack ||
    { echo "can't build xrun_victim"; exit 2; }

export JACK_DEFAULT_SERVER=jbstall JACKBUS_COMPAT_CONF=$logs/compat.conf
export PULSE_SERVER=unix:/jackbus-test/no-server PIPEWIRE_REMOTE=jackbus-test-no-daemon
alsa_out=$JACK_BUILD/jackbus-alsa
cat >"$logs/asound.conf" <<EOC
</usr/share/alsa/alsa.conf>
pcm_type.jackbus { lib "$alsa_out/libasound_module_pcm_jackbus.so" }
<$tree/nspa/jackbus-alsa/50-jackbus.conf>
EOC
fail=0
pass() { echo "PASS $1"; }
flunk() { echo "FAIL $1"; shift; for l in "$@"; do [ -f "$l" ] && tail -8 "$l" | sed 's/^/    /'; done; fail=1; }
have() { command -v "$1" >/dev/null 2>&1; }
wait_port() {
    i=0; while [ $i -lt 100 ] && ! jack_lsp 2>/dev/null | grep -qE "$1"; do sleep 0.1; i=$((i + 1)); done
    jack_lsp 2>/dev/null | grep -E "$1" | head -1
}
field() { awk -v k="$2" '{ for (i = 1; i < NF; i++) if ($i == k) print $(i + 1) }' "$1"; }

"$JACK_BUILD/jackd" -n jbstall -R -P 70 -d dummy -r 48000 -p 256 >"$logs/jackd.log" 2>&1 &
sp=$!
i=0; while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbstall_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
python3 -I "$tree/nspa/pw-shim/test/mkwav.py" "$logs/s48.wav" 48000 8

# one case: label, the setting's key ("pa" ...), on/off, the port to wait for, command...
run_case() {
    label=$1; key=$2; mode=$3; portre=$4; shift 4
    printf '%s.async = %s\n' "$key" "$mode" >"$logs/compat.conf"
    v="$logs/victim-$key-$mode"
    "$logs/xrun_victim" victim "$v.counts" >"$v.log" 2>&1 &
    vp=$!
    wait_port '^victim:out$' >/dev/null
    "$@" >"$logs/app-$key-$mode.log" 2>&1 &
    ap=$!
    port=$(wait_port "$portre")
    if [ -z "$port" ]; then
        flunk "$label, $key.async = $mode: no port came" "$logs/app-$key-$mode.log"
        kill $ap $vp 2>/dev/null; wait $ap $vp 2>/dev/null
        return
    fi
    sleep 1.5
    x0=$(field "$v.counts" xruns); c0=$(field "$v.counts" cycles)
    kill -STOP $ap; sleep 2; kill -CONT $ap
    sleep 1.5
    x1=$(field "$v.counts" xruns); c1=$(field "$v.counts" cycles)
    # the application's end: the rest of the file, the stall on top
    i=0; while kill -0 $ap 2>/dev/null && [ $i -lt 150 ]; do sleep 0.1; i=$((i + 1)); done
    kill -0 $ap 2>/dev/null && { kill -KILL $ap; rc=killed; } || { wait $ap; rc=$?; }
    kill -TERM $vp; wait $vp 2>/dev/null
    dx=$((x1 - x0)); dc=$((c1 - c0))
    # 3.5 s from x0 to x1: 656 cycles at 48000 Hz / 256
    if [ "$mode" = no ]; then
        if [ $dx -gt 0 ]; then
            pass "$label in the graph's cycle (the control): frozen 2 s, the victim told of $dx xruns ($dc cycles in 3.5 s)"
        else
            flunk "$label in the graph's cycle (the control): no xrun told: the test can't tell the modes apart" "$logs/jackd.log"
        fi
    elif [ $dx -eq 0 ] && [ $dc -ge 620 ] && [ "$rc" = 0 ]; then
        pass "$label async: frozen 2 s, the victim told of no xrun and ran every cycle ($dc in 3.5 s); the application played on to its end, exit 0"
    else
        flunk "$label async: $dx xruns told, $dc cycles in 3.5 s (656 due), exit $rc" "$logs/app-$key-$mode.log" "$logs/jackd.log"
    fi
}

for mode in no yes; do
    have paplay && run_case paplay pa $mode ':FL$' "$tree/nspa/pa-shim/jackbus-pa" paplay "$logs/s48.wav"
    have aplay && [ -e "$alsa_out/libasound_module_pcm_jackbus.so" ] &&
        run_case aplay alsa $mode ':out_1$' env ALSA_CONFIG_PATH="$logs/asound.conf" aplay -q -D jackbus "$logs/s48.wav"
    have pw-play && run_case pw-play pw $mode ':FL$' "$tree/nspa/pw-shim/jackbus-pw" pw-play "$logs/s48.wav"
done

kill -TERM $sp; wait $sp 2>/dev/null
grep -qE 'Segmentation|Assertion|terminate called' "$logs/jackd.log" && flunk "server log" "$logs/jackd.log"
echo "logs: $logs"
[ $fail -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
