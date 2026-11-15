#!/bin/sh
# Spike check for jackbus-pw. PipeWire's audio-dsp-filter example and
# pw-probe, both linked against the system's libpipewire-0.3, run through
# the jackbus-pw launcher on an isolated JackBus (dummy driver) at settings
# unlike what they ask for. Shows the rate and period forced, the PipeWire
# view of them, and a sample-exact pass-through in the JACK graph.
#
#   nspa/pw-shim/test/run-example.sh [rate/period ...]   (default 44100/256 96000/64)
#
# Runs itself under nspa/run-isolated.sh: private /dev/shm, IPC and network.
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$tree/nspa/run-isolated.sh" "$0" "$@"
fi

out=$JACK_BUILD/pw-shim
launcher=$here/../jackbus-pw
logs=$(mktemp -d /tmp/jbpw-test.XXXXXX)
for b in "$out/audio-dsp-filter" "$out/pw-probe" "$out/jbpw-check" "$out/lib/libpipewire-0.3.so.0"; do
    [ -e "$b" ] || { echo "missing $b: run make and make test-apps first"; exit 2; }
done

# If the shim were bypassed, the real libpipewire must find no daemon.
XDG_RUNTIME_DIR=$logs/run; mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
PIPEWIRE_REMOTE=jackbus-pw-test-no-daemon
JACK_DEFAULT_SERVER=jbpw
export XDG_RUNTIME_DIR PIPEWIRE_REMOTE JACK_DEFAULT_SERVER

fail=0
server_pid=

start_server() {  # rate period
    "$JACK_BUILD/jackd" -n jbpw -R -P 70 -d dummy -r "$1" -p "$2" >"$logs/jackd-$1-$2.log" 2>&1 &
    server_pid=$!
    i=0
    while [ $i -lt 50 ]; do
        ls /dev/shm/jack_jbpw_*_0 >/dev/null 2>&1 && return 0
        kill -0 "$server_pid" 2>/dev/null || return 1
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

stop_pid() {
    kill -TERM "$1" 2>/dev/null
    i=0
    while kill -0 "$1" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    kill -0 "$1" 2>/dev/null && { kill -KILL "$1"; return 1; }
    wait "$1" 2>/dev/null
}

run_config() {  # rate period
    rate=$1 period=$2
    echo "=== JackBus at $rate Hz, $period frames (the apps ask for 48000 Hz, 128 frames)"
    if ! start_server "$rate" "$period"; then
        echo "FAIL: server did not start"; cat "$logs/jackd-$rate-$period.log"; fail=1; return
    fi

    # 1. PipeWire's example, unmodified, with PipeWire's own env requests on top
    PIPEWIRE_LATENCY=128/48000 PIPEWIRE_RATE=1/48000 \
        "$launcher" -v "$out/audio-dsp-filter" >"$logs/example-$rate.log" 2>&1 &
    app=$!
    if "$out/jbpw-check" audio-filter:input audio-filter:output 2 >"$logs/check-$rate.log" 2>&1; then
        res=PASS
    else
        res=FAIL; fail=1
    fi
    if grep -q "/libpipewire-0.3.so.0" "/proc/$app/maps" 2>/dev/null; then
        maps=$(grep -o '/[^ ]*libpipewire-0.3.so[^ ]*' "/proc/$app/maps" | sort -u | tr '\n' ' ')
    else
        maps="(not mapped)"
    fi
    threads=$(cat /proc/$app/task/*/comm 2>/dev/null | tr '\n' ',' | sed 's/,$//')
    kill -INT "$app"
    wait "$app"; app_rc=$?
    grep -E 'asked for|forced to|process latency' "$logs/example-$rate.log" | sed 's/^/  /'
    echo "  example maps: $maps"
    echo "  example threads: $threads"
    sed 's/^/  /' "$logs/check-$rate.log"
    case $maps in *build/pw-shim/lib/*) ;; *) res=FAIL; fail=1; echo "  the shim was not loaded" ;; esac
    [ "$app_rc" -eq 0 ] || { res=FAIL; fail=1; echo "  example exit status $app_rc after SIGINT"; }
    echo "$res audio-dsp-filter at $rate/$period"

    # 2. pw-probe: what the PipeWire side of the app sees
    if "$launcher" "$out/pw-probe" 2 >"$logs/probe-$rate.log" 2>&1; then
        res=PASS
    else
        res=FAIL; fail=1
    fi
    sed 's/^/  /' "$logs/probe-$rate.log"
    echo "$res pw-probe at $rate/$period"

    stop_pid "$server_pid" || { echo "FAIL: server did not exit on SIGTERM"; fail=1; }
}

[ $# -gt 0 ] || set -- 44100/256 96000/64
for cfg in "$@"; do
    run_config "${cfg%/*}" "${cfg#*/}"
done
echo "logs: $logs"
[ $fail -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
