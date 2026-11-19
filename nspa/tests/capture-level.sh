#!/bin/sh
# Capture through the shims keeps its level: a tone (peak 0.2) recorded by
# parecord (jackbus-pa) and pw-record (jackbus-pw) in the formats a call app
# asks for (mono and stereo, 16-bit and float, 48000, 44100 and 16000 Hz,
# in the cycle and async) arrives at the level it was sent at, within 0.5 dB.
# On an isolated JackBus (dummy driver, 48000 Hz, 256 frames).
#
#   nspa/tests/capture-level.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$tree/nspa/run-isolated.sh" "$0" "$@"
fi
check=$tree/nspa/tests/tools/level_check.py
logs=$(mktemp -d /tmp/jbcap.XXXXXX)
XDG_RUNTIME_DIR=$logs/run; mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
PULSE_SERVER=unix:/jackbus-test/no-server PIPEWIRE_REMOTE=jackbus-test-no-daemon JACK_DEFAULT_SERVER=jbcap
JACKBUS_COMPAT_CONF=$logs/compat.conf JACKBUS_PA_AUTOCONNECT=0 JACKBUS_PW_AUTOCONNECT=0
export XDG_RUNTIME_DIR PULSE_SERVER PIPEWIRE_REMOTE JACK_DEFAULT_SERVER JACKBUS_COMPAT_CONF \
    JACKBUS_PA_AUTOCONNECT JACKBUS_PW_AUTOCONNECT
fail=0
stop() {
    kill -TERM "$1" 2>/dev/null
    i=0; while kill -0 "$1" 2>/dev/null && [ $i -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
    kill -0 "$1" 2>/dev/null && kill -KILL "$1"
    wait "$1" 2>/dev/null
}

"$JACK_BUILD/jackd" -n jbcap -R -P 70 -d dummy -r 48000 -p 256 >"$logs/jackd.log" 2>&1 &
sp=$!
i=0; while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbcap_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
"$JACK_BUILD/example-clients/jackbus_tone" -n tone -f 440 -c >"$logs/tone.log" 2>&1 &
tp=$!
i=0; while [ $i -lt 50 ] && ! jack_lsp 2>/dev/null | grep -q '^tone:out$'; do sleep 0.1; i=$((i + 1)); done

# rec <label> <async env> <launcher> <program and args, the file last>
rec() {
    label=$1 env=$2; shift 2
    before=$(jack_lsp 2>/dev/null)
    env $env "$@" >"$logs/$label.log" 2>&1 &
    p=$!
    ports=
    i=0; while [ $i -lt 100 ] && [ -z "$ports" ]; do
        sleep 0.1; i=$((i + 1))
        ports=$(jack_lsp 2>/dev/null | grep -vxF "$before" | grep -vE '^(system|tone):')
    done
    sleep 0.2
    ports=$(jack_lsp 2>/dev/null | grep -vxF "$before" | grep -vE '^(system|tone):')
    for port in $ports; do jack_connect tone:out "$port"; done
    sleep 2
    kill -INT $p; wait $p
    if python3 -I "$check" "$logs/$label.wav" 0.2 >"$logs/$label.check" 2>&1; then
        echo "PASS $label: $(cut -d' ' -f2- "$logs/$label.check") [$(echo $ports | tr ' ' ,)]"
    else
        echo "FAIL $label: $(cat "$logs/$label.check") [$(echo $ports | tr ' ' ,)]"
        tail -5 "$logs/$label.log" | sed 's/^/    /'
        fail=1
    fi
}

PA=$tree/nspa/pa-shim/jackbus-pa
PW=$tree/nspa/pw-shim/jackbus-pw
have_pa=0 have_pw=0
command -v parecord >/dev/null && [ -e "$JACK_BUILD/pa-shim/lib/libpulse.so.0" ] && have_pa=1
command -v pw-record >/dev/null && [ -e "$JACK_BUILD/pw-shim/lib/libpipewire-0.3.so.0" ] && have_pw=1
[ $have_pa = 1 ] || echo "SKIP capture through jackbus-pa (no parecord, or the shim is not built)"
[ $have_pw = 1 ] || echo "SKIP capture through jackbus-pw (no pw-record, or the shim is not built)"
for mode in 0 1; do
    m=$([ $mode = 1 ] && echo async || echo cycle)
    a="JACKBUS_PA_ASYNC=$mode"
    [ $have_pa = 1 ] || a=skip
    [ "$a" = skip ] || rec pa-$m-mono-s16-48k "$a" "$PA" parecord --rate=48000 --channels=1 --format=s16le "$logs/pa-$m-mono-s16-48k.wav"
    [ "$a" = skip ] || rec pa-$m-stereo-s16-48k "$a" "$PA" parecord --rate=48000 --channels=2 --format=s16le "$logs/pa-$m-stereo-s16-48k.wav"
    [ "$a" = skip ] || rec pa-$m-mono-f32-48k "$a" "$PA" parecord --rate=48000 --channels=1 --format=float32le "$logs/pa-$m-mono-f32-48k.wav"
    [ "$a" = skip ] || rec pa-$m-mono-s16-16k "$a" "$PA" parecord --rate=16000 --channels=1 --format=s16le "$logs/pa-$m-mono-s16-16k.wav"
    [ "$a" = skip ] || rec pa-$m-stereo-s16-44k "$a" "$PA" parecord --rate=44100 --channels=2 --format=s16le "$logs/pa-$m-stereo-s16-44k.wav"
    a="JACKBUS_PW_ASYNC=$mode"
    [ $have_pw = 1 ] || continue
    rec pw-$m-mono-s16-48k "$a" "$PW" pw-record --rate 48000 --channels 1 --format s16 "$logs/pw-$m-mono-s16-48k.wav"
    rec pw-$m-stereo-f32-48k "$a" "$PW" pw-record --rate 48000 --channels 2 --format f32 "$logs/pw-$m-stereo-f32-48k.wav"
    rec pw-$m-mono-s16-16k "$a" "$PW" pw-record --rate 16000 --channels 1 --format s16 "$logs/pw-$m-mono-s16-16k.wav"
done

stop $tp
stop $sp
[ $fail = 0 ] && rm -rf "$logs" || echo "logs: $logs"
exit $fail
