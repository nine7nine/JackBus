#!/bin/sh
# jackbus-alsa: ALSA applications, unmodified, straight to JackBus through its
# own ALSA plugin (PCM "jackbus"), on an isolated JackBus (dummy driver, 48000
# Hz, 256 frames):
#  - aplay of S16, S32 and float: a continuous 440 Hz sine on its ports, which
#    went to system:playback by themselves; drained, exit 0;
#  - a 44100 Hz file: resampled (the fallback) at the right pitch; with
#    resampling off, JackBus's rate only;
#  - arecord of a JackBus node's tone (S16 and float, and at 44100 Hz);
#  - sample-accurate timing (alsa-sync): frames played by the application's
#    own account (written - delay) against JACK's frame clock, constant within
#    a cycle over 4 s;
#  - an application whose writing thread is late: the underrun reported to
#    it (EPIPE), and after recovering it goes on;
#  - an application killed: its client goes, the server goes on;
#  - the node's realtime thread: no system call but JACK's futex;
#  - a device with a fallback (the default device PatchBox sets): JackBus
#    while it runs, the fallback PCM while it doesn't (nothing printed); one
#    that falls back to itself fails, no loop.
#
#   make -C nspa/jackbus-alsa all test-apps && nspa/jackbus-alsa/test/run.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$tree/nspa/run-isolated.sh" "$0" "$@"
fi
out=$JACK_BUILD/jackbus-alsa
check=$tree/nspa/tests/tools/sine_check.py
logs=$(mktemp -d /tmp/jbalsa-test.XXXXXX)
[ -e "$out/libasound_module_pcm_jackbus.so" ] && [ -x "$out/alsa-sync" ] ||
    { echo "missing: make -C nspa/jackbus-alsa all test-apps"; exit 2; }
command -v aplay >/dev/null || { echo "SKIP jackbus-alsa: no aplay (alsa-utils)"; exit 0; }

# this tree's plugin and libjack; ALSA's own configuration, plus pcm.jackbus
export JACK_DEFAULT_SERVER=jbalsa LD_LIBRARY_PATH=$JACK_BUILD/common
cat >"$logs/asound.conf" <<EOC
</usr/share/alsa/alsa.conf>
pcm_type.jackbus { lib "$out/libasound_module_pcm_jackbus.so" }
<$here/../50-jackbus.conf>
pcm.jbdefault { type jackbus fallback "null" }
pcm.jbloop { type jackbus fallback "jbloop" }
EOC
export ALSA_CONFIG_PATH=$logs/asound.conf
# settings of the test's own (not the user's ~/.config/jackbus/compat.conf)
export JACKBUS_COMPAT_CONF=/nonexistent/jackbus-test-compat.conf
fail=0
pass() { echo "PASS $1"; }
flunk() { echo "FAIL $1"; shift; for l in "$@"; do [ -f "$l" ] && tail -8 "$l" | sed 's/^/    /'; done; fail=1; }
wait_port() {
    i=0; while [ $i -lt 100 ] && ! jack_lsp 2>/dev/null | grep -qE "$1"; do sleep 0.1; i=$((i + 1)); done
    jack_lsp 2>/dev/null | grep -E "$1" | head -1
}
stop() {
    kill -TERM "$1" 2>/dev/null
    i=0; while kill -0 "$1" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    kill -0 "$1" 2>/dev/null && kill -KILL "$1"
    wait "$1" 2>/dev/null
}

"$JACK_BUILD/jackd" -n jbalsa -R -P 70 -d dummy -r 48000 -p 256 >"$logs/jackd.log" 2>&1 &
sp=$!
i=0; while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbalsa_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
python3 -I "$tree/nspa/pw-shim/test/mkwav.py" "$logs/s48.wav" 48000 3
python3 -I "$tree/nspa/pw-shim/test/mkwav.py" "$logs/s441.wav" 44100 3
python3 -I "$tree/nspa/pw-shim/test/mkwav.py" "$logs/long.wav" 48000 30
# the same sine as raw float and S32 (aplay's -t raw)
python3 -I - "$logs" <<'EOP'
import math, struct, sys
d = sys.argv[1]
n = 48000 * 3
f = open(d + "/s48.f32", "wb"); g = open(d + "/s48.s32", "wb")
for i in range(n):
    v = 0.5 * math.sin(2 * math.pi * 440 * i / 48000)
    f.write(struct.pack("<ff", v, v)); g.write(struct.pack("<ii", int(v * 2147483647), int(v * 2147483647)))
EOP

# 1. aplay: S16 (a WAV), S32 and float (raw)
play() {  # label, aplay arguments...
    label=$1; shift
    aplay -D jackbus "$@" >"$logs/aplay-$label.log" 2>&1 &
    p=$!
    port=$(wait_port '^aplay:out_1$')
    conn=$(jack_lsp -c aplay:out_1 2>/dev/null | sed -n 2p | tr -d ' ')
    sleep 0.3
    timeout 10 jack_rec -f "$logs/rec-$label.wav" -d 1.5 aplay:out_1 >/dev/null 2>&1
    python3 -I "$check" "$logs/rec-$label.wav" 440 >"$logs/rec-$label.check" 2>&1; r=$?
    wait $p; rc=$?
    if [ -n "$port" ] && [ "$conn" = system:playback_1 ] && [ $r -eq 0 ] && [ $rc -eq 0 ]; then
        pass "aplay $label: to $conn by itself, drained, exit 0: $(cut -d' ' -f2- "$logs/rec-$label.check")"
    else
        flunk "aplay $label (port '$port', to '$conn', rc $rc)" "$logs/rec-$label.check" "$logs/aplay-$label.log"
    fi
}
play S16 "$logs/s48.wav"
play S32 -t raw -f S32_LE -r 48000 -c 2 "$logs/s48.s32"
play FLOAT -t raw -f FLOAT_LE -r 48000 -c 2 "$logs/s48.f32"

# 2. a 44100 Hz file: resampled (the fallback) to a continuous sine at the
#    right pitch; with resampling off, the rate is JackBus's only (aplay warns
#    it got 48000 Hz)
aplay -D jackbus "$logs/s441.wav" >"$logs/aplay-441.log" 2>&1 &
p=$!
wait_port '^aplay:out_1$' >/dev/null
sleep 0.3
timeout 10 jack_rec -f "$logs/rec-441.wav" -d 1.5 aplay:out_1 >/dev/null 2>&1
python3 -I "$check" "$logs/rec-441.wav" 440 >"$logs/rec-441.check" 2>&1 && r=0 || r=1
wait $p; rc=$?
if [ $r -eq 0 ] && [ $rc -eq 0 ] && ! grep -q 'rate is not accurate' "$logs/aplay-441.log"; then
    pass "aplay of 44100 Hz: resampled at its rate (no 'rate is not accurate'), exit 0: $(cut -d' ' -f2- "$logs/rec-441.check")"
else
    flunk "aplay of 44100 Hz, resampled (rc $rc)" "$logs/rec-441.check" "$logs/aplay-441.log"
fi
JACKBUS_ALSA_RESAMPLE=0 timeout 10 aplay -D jackbus "$logs/s441.wav" >"$logs/aplay-441-nors.log" 2>&1
if grep -q 'rate is not accurate (requested = 44100Hz, got = 48000Hz)' "$logs/aplay-441-nors.log"; then
    pass "resampling off: JackBus's rate only (aplay: 'rate is not accurate')"
else
    flunk "resampling off" "$logs/aplay-441-nors.log"
fi

# 3. arecord of a node's tone, S16 and float
"$JACK_BUILD/example-clients/jackbus_tone" -n tone -f 440 >"$logs/tone.log" 2>&1 &
tp=$!
wait_port '^tone:out$' >/dev/null
for f in S16_LE FLOAT_LE S16_LE@44100; do
    rate=48000
    case $f in *@*) rate=${f#*@}; f=${f%@*} ;; esac
    JACKBUS_ALSA_AUTOCONNECT=0 arecord -D jackbus -f $f -r $rate -c 2 -d 2 "$logs/arec-$f.wav" >"$logs/arecord-$f.log" 2>&1 &
    p=$!
    wait_port '^arecord:in_2$' >/dev/null
    jack_connect tone:out arecord:in_1; jack_connect tone:out arecord:in_2
    wait $p; rc=$?
    if [ $f = FLOAT_LE ]; then
        # sine_check reads integer WAV: the float file's samples as S16 via sox-free python
        python3 -I - "$logs/arec-$f.wav" "$logs/arec-$f.s16.wav" <<'EOP'
import struct, sys, wave
raw = open(sys.argv[1], "rb").read()
i = raw.index(b"data") + 8
fl = struct.unpack("<%df" % ((len(raw) - i) // 4), raw[i:i + (len(raw) - i) // 4 * 4])
w = wave.open(sys.argv[2], "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, v)) * 32767)) for v in fl))
EOP
        file=$logs/arec-$f.s16.wav
    else
        file=$logs/arec-$f.wav
    fi
    if [ $rc -eq 0 ] && python3 -I "$check" "$file" 440 >"$logs/arec-$f.check" 2>&1; then
        pass "arecord $f at $rate of a node's tone: $(cut -d' ' -f2- "$logs/arec-$f.check")"
    else
        flunk "arecord $f (rc $rc)" "$logs/arec-$f.check" "$logs/arecord-$f.log"
    fi
done
stop $tp

# 4. sample-accurate timing
timeout 20 "$out/alsa-sync" 4 >"$logs/sync.log" 2>&1
if grep -q '^PASS' "$logs/sync.log"; then
    pass "timing: $(grep -o 'JACK frames - played: .*' "$logs/sync.log")"
else
    flunk "timing" "$logs/sync.log"
fi

# 5. an application's writing thread late (300 ms, 100 ms buffered; its
#    process runs on): the underrun is reported to it (EPIPE), it recovers
timeout 20 "$out/alsa-sync" xrun >"$logs/xrun.log" 2>&1
if grep -q '^PASS' "$logs/xrun.log"; then
    pass "underrun: $(grep -o 'alsa-sync xrun: .*' "$logs/xrun.log")"
else
    flunk "underrun reporting" "$logs/xrun.log"
fi

# 6. an application killed: its client goes, the server goes on
aplay -D jackbus "$logs/long.wav" >"$logs/aplay-kill.log" 2>&1 &
p=$!
wait_port '^aplay:out_1$' >/dev/null
kill -KILL $p; wait $p 2>/dev/null
i=0; while [ $i -lt 30 ] && jack_lsp 2>/dev/null | grep -q '^aplay:'; do sleep 0.1; i=$((i + 1)); done
if ! jack_lsp 2>/dev/null | grep -q '^aplay:' && jack_lsp 2>/dev/null | grep -q '^system:playback_1$'; then
    pass "aplay killed: its client removed within $((i * 100)) ms, the server goes on"
else
    flunk "client of a killed aplay" "$logs/jackd.log"
fi

# 7. the node's realtime thread
rt_cycles() {
    rt=$(grep -E 'sched_setscheduler\([0-9]+, SCHED_FIFO' "$1" | head -1 | awk '{print $1}')
    [ -n "$rt" ] || return 1
    awk -v t="$rt" '$1 == t' "$1" | sed -n '/sched_setscheduler(/,$p' | sed '1d' | sed '/<\.\.\. sched_setscheduler resumed>/d' | sed '/ --- SIG/,$d'
}
if command -v strace >/dev/null; then
    timeout 30 strace -f -qq -o "$logs/rt.strace" aplay -D jackbus "$logs/s48.wav" >/dev/null 2>&1
    if rt_cycles "$logs/rt.strace" >"$logs/rt.loop" && [ -s "$logs/rt.loop" ]; then
        calls=$(sed -E 's/^[0-9]+ +//; s/\(.*//; s/^<\.\.\. ([a-z_0-9]+) resumed>.*/\1/' "$logs/rt.loop" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}')
        others=$(grep -vcE '^[0-9]+ +(futex\(|<\.\.\. futex resumed>)' "$logs/rt.loop")
        [ "$others" -eq 0 ] && pass "aplay's node realtime thread: no system call but futex ($calls)" \
            || flunk "system calls in the realtime thread ($calls)" "$logs/rt.loop"
    elif [ "${JACKBUS_ALSA_ASYNC:-0}" = 1 ]; then
        # async: JackBus moves the stream; the application has no realtime thread of JackBus's
        pass "aplay async: no realtime thread in the application"
    else
        flunk "no realtime thread traced" "$logs/rt.strace"
    fi
fi

# 7b. the output setting: a second card (as a bridged one) gets the stream
fc=$JACK_BUILD/pa-shim/fake-card
if [ -x "$fc" ]; then
    "$fc" >/dev/null 2>&1 &
    fp=$!
    wait_port '^fakecard:playback_1$' >/dev/null
    JACKBUS_OUTPUT=fakecard aplay -D jackbus "$logs/s48.wav" >"$logs/out-fake.log" 2>&1 &
    p=$!
    wait_port '^aplay:out_1$' >/dev/null
    sleep 0.2
    conn=$(jack_lsp -c aplay:out_1 2>/dev/null | sed -n 2p | tr -d ' ')
    wait $p
    stop $fp
    if [ "$conn" = fakecard:playback_1 ]; then
        pass "output = fakecard: aplay to $conn"
    else
        flunk "output setting (connected to '$conn')" "$logs/out-fake.log"
    fi
else
    echo "SKIP the output setting (make -C nspa/pa-shim test-apps)"
fi

# 8. with a fallback (as the default device PatchBox sets), while JackBus
#    runs: JackBus
aplay -D jbdefault "$logs/s48.wav" >"$logs/fb-up.log" 2>&1 &
p=$!
port=$(wait_port '^aplay:out_1$')
wait $p; rc=$?
if [ -n "$port" ] && [ $rc -eq 0 ]; then
    pass "a device with a fallback, JackBus running: on JackBus (aplay:out_1), exit 0"
else
    flunk "fallback device with JackBus running (rc $rc)" "$logs/fb-up.log"
fi

stop $sp

# 9. JackBus not running: the fallback (null here, pipewire for the default
#    device), quietly; no fallback: an error; a fallback back to the device
#    itself: an error, not a loop
JACKBUS_ALSA_VERBOSE=1 timeout 20 aplay -D jbdefault "$logs/s48.wav" >"$logs/fb-down.log" 2>&1; rc1=$?
timeout 20 aplay -D jackbus "$logs/s48.wav" >"$logs/fb-none.log" 2>&1; rc2=$?
timeout 20 aplay -D jbloop "$logs/s48.wav" >"$logs/fb-loop.log" 2>&1; rc3=$?
if [ $rc1 -eq 0 ] && grep -q "jackbus-alsa: JackBus isn't running: null (opened)" "$logs/fb-down.log" &&
   ! grep -qi 'cannot connect\|jackbus socket' "$logs/fb-down.log" &&
   [ $rc2 -ne 0 ] && [ $rc2 -ne 124 ] && [ $rc3 -ne 0 ] && [ $rc3 -ne 124 ]; then
    pass "JackBus not running: the fallback opened, nothing from libjack; without one an error (exit $rc2); a fallback to itself an error (exit $rc3), no loop"
else
    flunk "JackBus not running (rc $rc1 $rc2 $rc3)" "$logs/fb-down.log" "$logs/fb-none.log" "$logs/fb-loop.log"
fi
grep -qE 'Segmentation|Assertion|terminate called' "$logs/jackd.log" && flunk "server log" "$logs/jackd.log"
echo "logs: $logs"
[ $fail -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
