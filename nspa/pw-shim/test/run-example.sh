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
for b in "$out/audio-dsp-filter" "$out/audio-src" "$out/audio-capture" "$out/pw-probe" "$out/pw-twofilter" "$out/jbpw-check" "$out/lib/libpipewire-0.3.so.0"; do
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
    "$JACK_BUILD/jackd" -v -n jbpw -R -P 70 -d dummy -r "$1" -p "$2" >"$logs/jackd-$1-$2.log" 2>&1 &
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
    grep -E 'asked for|forced to|process latency|opened over' "$logs/example-$rate.log" | sed 's/^/  /'
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

    # 3. two filters in one application: one JACK client, beta's ports prefixed
    "$launcher" "$out/pw-twofilter" >"$logs/two-$rate.log" 2>&1 &
    two=$!
    i=0
    while [ $i -lt 100 ] && [ "$(jack_lsp 2>/dev/null | grep -c '^alpha:')" -lt 4 ]; do sleep 0.1; i=$((i + 1)); done
    ports=$(jack_lsp 2>/dev/null | grep '^alpha:' | sort | tr '\n' ' ')
    if [ "$ports" = "alpha:beta/input alpha:beta/output alpha:input alpha:output " ] \
       && [ "$(jack_lsp 2>/dev/null | grep -c '^beta:')" = 0 ] \
       && "$out/jbpw-check" alpha:input alpha:output 1 >"$logs/two-check-a-$rate.log" 2>&1 \
       && "$out/jbpw-check" alpha:beta/input alpha:beta/output 1 >"$logs/two-check-b-$rate.log" 2>&1; then
        res=PASS
    else
        res=FAIL; fail=1
        echo "  ports: $ports"; cat "$logs/two-check-a-$rate.log" "$logs/two-check-b-$rate.log" 2>/dev/null | sed 's/^/  /'
    fi
    kill -INT $two; wait $two || { res=FAIL; fail=1; }
    sed 's/^/  /' "$logs/two-check-a-$rate.log" "$logs/two-check-b-$rate.log" "$logs/two-$rate.log" 2>/dev/null | grep -E 'check:|twofilter:'
    echo "$res pw-twofilter at $rate/$period: one client, ports $ports"

    # 4. pw_stream playback: PipeWire's audio-src (F32, 2 channels, 44100 Hz
    #    only, RT_PROCESS): as it is at 44100, resampled by the shim elsewhere;
    #    a continuous 440 Hz sine either way
    "$launcher" -v "$out/audio-src" >"$logs/src-$rate.log" 2>&1 &
    srcp=$!
    i=0; while [ $i -lt 100 ] && ! jack_lsp 2>/dev/null | grep -q '^audio-src:FL'; do sleep 0.1; i=$((i + 1)); done
    sleep 0.3
    timeout 20 jack_rec -f "$logs/src.wav" -d 2 audio-src:FL >/dev/null 2>&1
    how="as it is"
    if [ "$rate" != 44100 ]; then
        how="resampled 44100 -> $rate"
        grep -q "resampled 44100 Hz -> $rate Hz" "$logs/src-$rate.log" || how="NOT resampled?"
    fi
    if python3 -I "$tree/nspa/tests/tools/sine_check.py" "$logs/src.wav" 440 >"$logs/src-check" 2>&1 &&
       [ "$how" != "NOT resampled?" ]; then
        echo "PASS pw_stream playback (audio-src, 44100 Hz) at $rate, $how: $(cut -d' ' -f2- "$logs/src-check")"
    else
        echo "FAIL pw_stream playback (audio-src) at $rate ($how)"; cat "$logs/src-check" "$logs/src-$rate.log" | tail -8; fail=1
    fi
    kill -INT $srcp 2>/dev/null; wait $srcp 2>/dev/null
    if [ "$rate" != 44100 ]; then
        JACKBUS_PW_RESAMPLE=0 "$launcher" -v "$out/audio-src" >"$logs/src-nors-$rate.log" 2>&1 &
        srcp=$!
        sleep 1.5
        if grep -q "rate 44100 Hz asked, JackBus runs at $rate Hz (resampling is off)" "$logs/src-nors-$rate.log"; then
            echo "PASS pw_stream at $rate with JACKBUS_PW_RESAMPLE=0: audio-src refused, with the reason"
        else
            echo "FAIL pw_stream at $rate: JACKBUS_PW_RESAMPLE=0 did not refuse"; tail -5 "$logs/src-nors-$rate.log"; fail=1
        fi
        kill -INT $srcp 2>/dev/null; wait $srcp 2>/dev/null
    fi

    # 5. pw_stream capture: PipeWire's audio-capture (F32, any rate), fed a tone
    "$launcher" "$out/audio-capture" >"$logs/cap-$rate.log" 2>&1 &
    capp=$!
    "$JACK_BUILD/example-clients/jackbus_tone" -n captone -f 440 -c >/dev/null 2>&1 &
    tonep=$!
    i=0; while [ $i -lt 100 ] && ! { jack_lsp 2>/dev/null | grep -q '^audio-capture:FL' && jack_lsp 2>/dev/null | grep -q '^captone:out'; }; do sleep 0.1; i=$((i + 1)); done
    jack_connect captone:out audio-capture:FL 2>/dev/null
    sleep 1.5
    kill -INT $capp 2>/dev/null; wait $capp 2>/dev/null
    kill -TERM $tonep 2>/dev/null; wait $tonep 2>/dev/null
    peak=$(tr '\033' '\n' <"$logs/cap-$rate.log" | grep -o 'channel 0: [^p]*peak:[0-9.]*' | tail -1 | sed 's/.*peak://')
    if grep -q "capturing rate:$rate channels:2" "$logs/cap-$rate.log" && awk -v p="${peak:-0}" 'BEGIN { exit !(p > 0.15 && p < 0.25) }'; then
        echo "PASS pw_stream capture (audio-capture) at $rate: JackBus's rate, the tone seen (peak $peak)"
    else
        echo "FAIL pw_stream capture (audio-capture) at $rate (peak '$peak')"; grep -a 'capturing' "$logs/cap-$rate.log"; fail=1
    fi

    # with a libjack that has it, the shim's clients open over the JackBus socket
    if nm -D "$JACK_BUILD/common/libjack.so" 2>/dev/null | grep -q ' T jackbus_client_open'; then
        opens=$(grep -c 'BusClientOpen ref' "$logs/jackd-$rate-$period.log")
        if [ "$opens" -ge 3 ] && grep -q 'opened on the JackBus socket' "$logs/example-$rate.log"; then
            echo "PASS JackBus socket: the PipeWire apps' clients opened over it ($opens opens)"
        else
            echo "FAIL JackBus socket: $opens opens"; fail=1
        fi
    fi
    stop_pid "$server_pid" || { echo "FAIL: server did not exit on SIGTERM"; fail=1; }
}

[ $# -gt 0 ] || set -- 44100/256 96000/64 48000/128
for cfg in "$@"; do
    run_config "${cfg%/*}" "${cfg#*/}"
done
echo "logs: $logs"
[ $fail -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
