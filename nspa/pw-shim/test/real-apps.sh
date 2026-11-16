#!/bin/sh
# Real PipeWire clients, unmodified, through jackbus-pw on an isolated JackBus
# (dummy driver, 48000 Hz, 256 frames): the system's pw-play, pw-record,
# pw-midiplay, mpv and fluidsynth, and test programs on SDL3 and OpenAL Soft
# (libraries that load libpipewire themselves), plus a MIDI pw_filter.
#
#   nspa/pw-shim/test/real-apps.sh
#
# Audio is recorded from each application's JACK port and checked as a
# continuous 440 Hz sine (nspa/tests/tools/sine_check.py). A missing program
# is skipped.
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$tree/nspa/run-isolated.sh" "$0" "$@"
fi
out=$JACK_BUILD/pw-shim
L=$here/../jackbus-pw
logs=$(mktemp -d /tmp/jbpw-real.XXXXXX)
check=$tree/nspa/tests/tools/sine_check.py
[ -e "$out/lib/libpipewire-0.3.so.0" ] || { echo "missing the shim: make -C nspa/pw-shim all test-apps"; exit 2; }

# if the shim were bypassed, the real libpipewire must find no daemon
XDG_RUNTIME_DIR=$logs/run; mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
PIPEWIRE_REMOTE=jackbus-pw-test-no-daemon JACK_DEFAULT_SERVER=jbreal
export XDG_RUNTIME_DIR PIPEWIRE_REMOTE JACK_DEFAULT_SERVER
# settings of the test's own (not the user's ~/.config/jackbus/compat.conf)
export JACKBUS_COMPAT_CONF=/nonexistent/jackbus-test-compat.conf
fail=0
pass() { echo "PASS $1"; }
flunk() { echo "FAIL $1"; shift; for l in "$@"; do [ -f "$l" ] && grep -v '^jackbus-pw: Jack' "$l" | tail -10 | sed 's/^/    /'; done; fail=1; }
have() { command -v "$1" >/dev/null 2>&1 || [ -x "$1" ]; }
wait_port() {  # regex
    i=0; while [ $i -lt 100 ] && ! jack_lsp 2>/dev/null | grep -qE "$1"; do sleep 0.1; i=$((i + 1)); done
    jack_lsp 2>/dev/null | grep -E "$1" | head -1
}
sine_of() {  # port name -> PASS line or failure
    sleep 0.3
    timeout 10 jack_rec -f "$logs/$2.wav" -d 1.5 "$1" >/dev/null 2>&1
    python3 -I "$check" "$logs/$2.wav" 440 >"$logs/$2.check" 2>&1
}
stop() {  # pid
    kill -TERM "$1" 2>/dev/null
    i=0; while kill -0 "$1" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    kill -0 "$1" 2>/dev/null && kill -KILL "$1"
    wait "$1" 2>/dev/null
}

"$JACK_BUILD/jackd" -n jbreal -R -P 70 -d dummy -r 48000 -p 256 >"$logs/jackd.log" 2>&1 &
sp=$!
i=0; while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbreal_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
python3 -I "$here/mkwav.py" "$logs/s441.wav" 44100 4
python3 -I "$here/mkwav.py" "$logs/s48.wav" 48000 4
python3 -I "$here/mkmid.py" "$logs/notes.mid"

# 1. pw-play (an async stream: no RT_PROCESS): a 48000 Hz file direct; a
#    44100 Hz one resampled (the fallback), refused with the reason with -R
if have pw-play; then
    timeout 10 "$L" -v -R pw-play "$logs/s441.wav" >"$logs/play-refused.log" 2>&1; rc=$?
    if [ $rc -ne 0 ] && grep -q 'rate 44100 Hz asked, JackBus runs at 48000 Hz (resampling is off' "$logs/play-refused.log"; then
        pass "pw-play -R s441.wav: refused, with the reason, exit $rc"
    else
        flunk "pw-play -R s441.wav not refused (rc $rc)" "$logs/play-refused.log"
    fi
    for f in s441 s48; do
        env "$L" -v pw-play "$logs/$f.wav" >"$logs/play-$f.log" 2>&1 &
        p=$!
        port=$(wait_port '^pw-play:FL$')
        conn=$(jack_lsp -c pw-play:FL 2>/dev/null | sed -n 2p | tr -d ' ')
        sine_of "$port" "play-$f"; r=$?
        wait $p; rc=$?
        how=direct; grep -q 'resampled' "$logs/play-$f.log" && how=resampled
        if [ $r -eq 0 ] && [ $rc -eq 0 ] && grep -q 'process on its loop (async)' "$logs/play-$f.log" &&
           grep -q 'streaming -> unconnected' "$logs/play-$f.log"; then
            pass "pw-play $f.wav: async, $how, to $conn, drained, exit 0: $(cut -d' ' -f2- "$logs/play-$f.check")"
        else
            flunk "pw-play $f.wav (rc $rc)" "$logs/play-$f.check" "$logs/play-$f.log"
        fi
    done
fi

# 2. pw-record (async capture), fed a JackBus node's tone
if have pw-record; then
    "$JACK_BUILD/example-clients/jackbus_tone" -n tone -f 440 >"$logs/tone.log" 2>&1 &
    tp=$!
    JACKBUS_PW_AUTOCONNECT=0 "$L" -v pw-record "$logs/rec.wav" >"$logs/record.log" 2>&1 &
    p=$!
    wait_port '^pw-record:FR$' >/dev/null
    wait_port '^tone:out$' >/dev/null
    jack_connect tone:out pw-record:FL; jack_connect tone:out pw-record:FR
    sleep 2.5
    kill -INT $p; wait $p
    if python3 -I "$check" "$logs/rec.wav" 440 >"$logs/rec.check" 2>&1; then
        pass "pw-record: async capture of a tone: $(cut -d' ' -f2- "$logs/rec.check")"
    else
        flunk "pw-record" "$logs/rec.check" "$logs/record.log"
    fi
    stop $tp
fi

# 3. pw-midiplay (an async MIDI stream) to jack_midi_dump: all 8 notes
if have pw-midiplay && have jack_midi_dump; then
    jack_midi_dump >"$logs/dump.log" 2>&1 &
    dp=$!
    JACKBUS_PW_AUTOCONNECT=0 "$L" -v pw-midiplay "$logs/notes.mid" >"$logs/midiplay.log" 2>&1 &
    p=$!
    mport=$(wait_port 'pw-midiplay:midi_out$')
    dport=$(wait_port '^midi-monitor:input$')
    jack_connect "$mport" "$dport"
    wait $p; rc=$?
    sleep 0.3; stop $dp
    ons=$(grep -c 'note on' "$logs/dump.log")
    pitches=$(grep 'note on' "$logs/dump.log" | sed 's/.*pitch *\([0-9]*\).*/\1/' | tr '\n' ' ')
    if [ "$ons" = 8 ] && [ "$pitches" = "60 61 62 63 64 65 66 67 " ] && [ $rc -eq 0 ]; then
        pass "pw-midiplay: async MIDI, 8 notes in order ($pitches), exit 0"
    else
        flunk "pw-midiplay: $ons note-ons ($pitches), rc $rc" "$logs/dump.log" "$logs/midiplay.log"
    fi
fi

# 4. a MIDI pw_filter (RT): jack_midiseq -> filter -> jack_midi_dump, a semitone
#    up; and the data loop: a blocking invoke runs in the process thread
if have jack_midiseq && have jack_midi_dump; then
    jack_midi_dump >"$logs/dump2.log" 2>&1 &
    dp=$!
    jack_midiseq seq 24000 0 60 6000 12000 63 6000 >"$logs/seq.log" 2>&1 &
    qp=$!
    JACKBUS_PW_CLIENT_NAME=midifilter "$L" -v "$out/pw-midifilter" 3 >"$logs/midifilter.log" 2>&1 &
    p=$!
    wait_port '^midifilter:midi_out$' >/dev/null
    wait_port '^seq:out$' >/dev/null
    wait_port '^midi-monitor:input$' >/dev/null
    jack_connect seq:out midifilter:midi_in; jack_connect midifilter:midi_out midi-monitor:input
    wait $p; rc=$?
    stop $qp; stop $dp
    up=$(grep 'note on' "$logs/dump2.log" | grep -cE 'pitch *(61|64),')
    other=$(grep 'note on' "$logs/dump2.log" | grep -vcE 'pitch *(61|64),')
    if [ "$up" -ge 4 ] && [ "$other" = 0 ] && grep -q 'ran in the process thread: yes' "$logs/midifilter.log" && [ $rc -eq 0 ]; then
        pass "pw_filter MIDI: $up notes through, each a semitone up; $(grep -o 'invoke: .*' "$logs/midifilter.log")"
    else
        flunk "pw_filter MIDI ($up up, $other other, rc $rc)" "$logs/dump2.log" "$logs/midifilter.log"
    fi
fi

# 5. mpv (an RT stream; a 48000 Hz file: the direct route)
if have mpv; then
    "$L" -v mpv --no-config --no-video --ao=pipewire --length=2.5 "$logs/s48.wav" >"$logs/mpv.log" 2>&1 &
    p=$!
    port=$(wait_port '^mpv:FL$')
    sine_of "$port" mpv; r=$?
    wait $p; rc=$?
    if [ $r -eq 0 ] && [ $rc -eq 0 ] && ! grep -q 'Could not wait' "$logs/mpv.log" && ! grep -q 'resampled' "$logs/mpv.log"; then
        pass "mpv: $(grep -o 'process in the JACK thread' "$logs/mpv.log" | head -1), direct, exit 0: $(cut -d' ' -f2- "$logs/mpv.check")"
    else
        flunk "mpv (rc $rc)" "$logs/mpv.check" "$logs/mpv.log"
    fi
fi

# 6. SDL3: its PipeWire backend chosen by itself, JackBus's devices listed
if [ -x "$out/sdl3-tone" ]; then
    "$L" -v "$out/sdl3-tone" >"$logs/sdl.log" 2>&1 &
    p=$!
    port=$(wait_port '^sdl3-tone:FL$')
    sine_of "$port" sdl; r=$?
    wait $p; rc=$?
    if [ $r -eq 0 ] && [ $rc -eq 0 ] && grep -q '^driver: pipewire' "$logs/sdl.log" &&
       grep -q 'playback device: system playback (JackBus) (48000 Hz' "$logs/sdl.log" && ! grep -q 'resampled' "$logs/sdl.log"; then
        pass "SDL3: driver pipewire, device \"system playback (JackBus)\" at 48000 Hz, opened at it (direct): $(cut -d' ' -f2- "$logs/sdl.check")"
    else
        flunk "SDL3 (rc $rc)" "$logs/sdl.check" "$logs/sdl.log"
    fi
fi

# 7. OpenAL Soft: needs audio nodes in the registry; mixes at the device's rate
if [ -x "$out/openal-tone" ]; then
    ALSOFT_DRIVERS=pipewire ALSOFT_LOGLEVEL=2 "$L" -v "$out/openal-tone" >"$logs/al.log" 2>&1 &
    p=$!
    port=$(wait_port '^openal-tone:FL$')
    sleep 0.3
    sine_of "$port" al; r=$?
    wait $p; rc=$?
    if [ $r -eq 0 ] && [ $rc -eq 0 ] && grep -q 'opened: system playback (JackBus) at 48000 Hz' "$logs/al.log" &&
       ! grep -q 'Timeout getting' "$logs/al.log" && ! grep -q 'resampled' "$logs/al.log"; then
        pass "OpenAL: device \"system playback (JackBus)\" at 48000 Hz, direct: $(cut -d' ' -f2- "$logs/al.check")"
    else
        flunk "OpenAL (rc $rc)" "$logs/al.check" "$logs/al.log"
    fi
fi

# 8. fluidsynth's PipeWire driver (no sound font: its stream runs, silent;
#    its shell reads a pipe that stays open for a few seconds)
if have fluidsynth; then
    sleep 4 | "$L" -v fluidsynth -a pipewire -n -q -r 48000 >"$logs/fs.log" 2>&1 &
    p=$!
    port=$(wait_port '^([Ff]luid[Ss]ynth):FL$')
    conn=$(jack_lsp -c "$port" 2>/dev/null | sed -n 2p | tr -d ' ')
    sleep 0.5
    stop $p; rc=$?
    if [ -n "$port" ] && [ "$conn" = system:playback_1 ] && ! grep -q 'Segmentation' "$logs/fs.log"; then
        pass "fluidsynth -a pipewire: $port to $conn, then a clean exit"
    else
        flunk "fluidsynth (port '$port', '$conn', rc $rc)" "$logs/fs.log"
    fi
fi

# 9. the registry and links, as libremidi (ossia score) uses them: its own
#    node and ports by node id, the default devices, link-factory by port and
#    by node, links going with their proxies
if [ -x "$out/pw-linktest" ]; then
    timeout 30 "$L" -v "$out/pw-linktest" >"$logs/linktest.log" 2>&1
    rc=$?
    grep -E '^(PASS|FAIL)' "$logs/linktest.log" | sed 's/^\(PASS\|FAIL\) /\1 pw-linktest: /'
    [ $rc -eq 0 ] && [ "$(grep -c '^PASS' "$logs/linktest.log")" = 4 ] || flunk "pw-linktest (rc $rc)" "$logs/linktest.log"
fi

# 10. ossia score (PipeWire is its default backend on Linux), unmodified, on
#     Qt's offscreen platform with a scratch home: its filter, its own node
#     found in the registry, its ports linked to the default devices through
#     link-factory. It needs a document to start its engine:
#     OSSIA_TEST_DOC=<file.score> (else skipped)
if have ossia-score && [ -n "${OSSIA_TEST_DOC:-}" ] && [ -f "${OSSIA_TEST_DOC:-}" ]; then
    oh=$logs/ossia-home
    mkdir -p "$oh/Documents/ossia/score/packages/default"
    echo '{ "version": 9999 }' >"$oh/Documents/ossia/score/packages/default/package.json"
    env HOME="$oh" XDG_CONFIG_HOME="$oh/.config" XDG_DATA_HOME="$oh/.local/share" XDG_CACHE_HOME="$oh/.cache" \
        QT_QPA_PLATFORM=offscreen "$L" -v ossia-score --no-opengl --force-restore "$OSSIA_TEST_DOC" >"$logs/ossia.log" 2>&1 &
    p=$!
    i=0
    while [ $i -lt 600 ]; do
        jack_lsp -c ossia-score:out_0 2>/dev/null | grep -q 'system:playback_1' &&
            jack_lsp -c ossia-score:in_0 2>/dev/null | grep -q 'system:capture_1' && break
        sleep 0.1; i=$((i + 1))
    done
    conns=$(jack_lsp -c 2>/dev/null | grep -A1 '^ossia-score:' | grep -c 'system:')
    kill -TERM $p 2>/dev/null
    i=0; while kill -0 $p 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    kill -KILL $p 2>/dev/null; wait $p 2>/dev/null
    if [ "$conns" -ge 4 ] && grep -q 'link-factory: ossia-score:out_0 -> system:playback_1' "$logs/ossia.log"; then
        pass "ossia score: its PipeWire engine on JackBus ($(grep -o 'forced to JackBus: [^,]*, [^,]*' "$logs/ossia.log" | head -1)), linked to the defaults through link-factory"
    else
        flunk "ossia score ($conns connections)" "$logs/ossia.log"
    fi
else
    echo "SKIP ossia score (not installed, or no OSSIA_TEST_DOC document)"
fi

# 11. the JACK realtime thread of a shim application makes no system call but
#    JACK's futex, from its first cycle on: an async stream (ring copies only),
#    an RT stream being resampled (PipeWire's audio-src: 44100 Hz), a MIDI
#    filter with a data-loop invoke; a negative control must be seen
rt_cycles() {  # <strace file> -> the realtime thread's lines in its cycles
    rt=$(grep -E 'sched_setscheduler\([0-9]+, SCHED_FIFO' "$1" | head -1 | awk '{print $1}')
    [ -n "$rt" ] || return 1
    awk -v t="$rt" '$1 == t' "$1" | sed -n '/sched_setscheduler(/,$p' | sed '1d' | sed '/<\.\.\. sched_setscheduler resumed>/d' | sed '/ --- SIG/,$d'
}
rt_check() {  # name strace
    if rt_cycles "$2" >"$2.loop" && [ -s "$2.loop" ]; then
        calls=$(sed -E 's/^[0-9]+ +//; s/\(.*//; s/^<\.\.\. ([a-z_0-9]+) resumed>.*/\1/' "$2.loop" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}')
        others=$(grep -vcE '^[0-9]+ +(futex\(|<\.\.\. futex resumed>)' "$2.loop")
        if [ "$others" -eq 0 ]; then
            pass "$1: its realtime thread makes no system call but futex ($calls)"
        else
            flunk "$1: system calls in its realtime thread ($calls)" "$2.loop"
        fi
    else
        flunk "$1: no realtime thread traced" "$2"
    fi
}
if command -v strace >/dev/null; then
    timeout 30 strace -f -qq -o "$logs/rt-play.strace" "$L" pw-play "$logs/s48.wav" >/dev/null 2>&1
    rt_check "pw-play (async)" "$logs/rt-play.strace"
    if [ -x "$out/audio-src" ]; then
        timeout 30 strace -f -qq -o "$logs/rt-src.strace" "$L" timeout -s INT 3 "$out/audio-src" >/dev/null 2>&1
        rt_check "audio-src (RT, resampled 44100 -> 48000)" "$logs/rt-src.strace"
    fi
    if have jack_midiseq; then
        jack_midiseq seq 24000 0 60 6000 12000 63 6000 >/dev/null 2>&1 &
        qp=$!
        wait_port '^seq:out$' >/dev/null
        for ctl in "" 1; do
            ( i=0; while [ $i -lt 100 ] && ! jack_lsp 2>/dev/null | grep -q '^midifilter:midi_in$'; do sleep 0.1; i=$((i + 1)); done
              jack_connect seq:out midifilter:midi_in ) &
            cp=$!
            MIDIFILTER_RT_SYSCALL=$ctl JACKBUS_PW_CLIENT_NAME=midifilter timeout 30 \
                strace -f -qq -o "$logs/rt-midi$ctl.strace" "$L" "$out/pw-midifilter" 2 >"$logs/rt-midi$ctl.log" 2>&1
            wait $cp
        done
        stop $qp
        rt_check "a MIDI pw_filter with a data-loop invoke" "$logs/rt-midi.strace"
        if rt_cycles "$logs/rt-midi1.strace" | grep -q 'getppid('; then
            pass "the trace sees a system call made in a cycle (negative control)"
        else
            flunk "the trace missed a system call made in a cycle" "$logs/rt-midi1.log"
        fi
    fi
else
    echo "SKIP realtime system calls (no strace)"
fi

stop $sp
grep -qE 'Segmentation|Assertion|terminate called' "$logs/jackd.log" && flunk "server log" "$logs/jackd.log"
echo "logs: $logs"
[ $fail -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
