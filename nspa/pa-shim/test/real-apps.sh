#!/bin/sh
# PulseAudio clients, unmodified, through jackbus-pa (libpulse on JackBus) on
# an isolated JackBus (dummy driver, 48000 Hz, 256 frames); no PulseAudio or
# PipeWire server anywhere (PULSE_SERVER points nowhere):
#  - paplay of a 48000 Hz file: the direct route; of a 44100 Hz one:
#    resampled (the fallback), and refused with -R (no resampling);
#  - parecord of a JackBus node's tone;
#  - mpv's pulse output, the whole file, no spurious underflow;
#  - SDL3's pulseaudio backend: JackBus's devices listed at its rate, opened
#    at it;
#  - pactl's introspection; every installed libpulse user finds all of its
#    functions in the shim;
#  - without the launcher (session-wide): PulseAudio's own libpulse with the
#    setting off; with it on, JackBus while it runs, PulseAudio's while not;
#  - Chrome and Firefox (headless, their sandboxes on, no launcher: the
#    switch on in their ~/.config): a Web Audio 440 Hz tone;
#  - the realtime thread of a stream (paplay): no system call but futex.
# Audio is recorded from each application's port and checked as a continuous
# 440 Hz sine. A missing program is skipped.
#
#   make -C nspa/pa-shim && nspa/pa-shim/test/real-apps.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$tree/nspa/run-isolated.sh" "$0" "$@"
fi
out=$JACK_BUILD/pa-shim
L=$here/../jackbus-pa
check=$tree/nspa/tests/tools/sine_check.py
logs=$(mktemp -d /tmp/jbpa-real.XXXXXX)
[ -e "$out/lib/libpulse.so.0" ] && [ -e "$out/lib/libjackbus-pa.so" ] || { echo "missing the shim: make -C nspa/pa-shim"; exit 2; }

# settings of the test's own (not the user's ~/.config/jackbus/compat.conf)
PULSE_SERVER=unix:/jackbus-pa-test/no-server JACK_DEFAULT_SERVER=jbpa JACKBUS_COMPAT_CONF=$logs/compat.conf
export PULSE_SERVER JACK_DEFAULT_SERVER JACKBUS_COMPAT_CONF
fail=0
pass() { echo "PASS $1"; }
flunk() { echo "FAIL $1"; shift; for l in "$@"; do [ -f "$l" ] && grep -v '^jackbus-pa: Jack' "$l" | tail -8 | sed 's/^/    /'; done; fail=1; }
have() { command -v "$1" >/dev/null 2>&1 || [ -x "$1" ]; }
app_port() {  # the first playback port of a client other than system
    i=0; while [ $i -lt "${2:-100}" ] && ! jack_lsp 2>/dev/null | grep -v '^system' | grep -qE ':(FL|MONO)$'; do sleep 0.1; i=$((i + 1)); done
    jack_lsp 2>/dev/null | grep -v '^system' | grep -E ':(FL|MONO)$' | head -1
}
sine_of() {  # port label
    sleep 0.4
    timeout 10 jack_rec -f "$logs/$2.wav" -d 1.2 "$1" >/dev/null 2>&1
    python3 -I "$check" "$logs/$2.wav" 440 >"$logs/$2.check" 2>&1
}
stop() {
    kill -TERM "$1" 2>/dev/null
    i=0; while kill -0 "$1" 2>/dev/null && [ $i -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
    kill -0 "$1" 2>/dev/null && kill -KILL "$1"
    wait "$1" 2>/dev/null
}

"$JACK_BUILD/jackd" -n jbpa -R -P 70 -d dummy -r 48000 -p 256 >"$logs/jackd.log" 2>&1 &
sp=$!
i=0; while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbpa_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
python3 -I "$tree/nspa/pw-shim/test/mkwav.py" "$logs/s48.wav" 48000 3
python3 -I "$tree/nspa/pw-shim/test/mkwav.py" "$logs/s441.wav" 44100 3

# 1. paplay: direct; resampled (the fallback); refused with -R
if have paplay; then
    for f in s48 s441; do
        "$L" -v paplay "$logs/$f.wav" >"$logs/paplay-$f.log" 2>&1 &
        p=$!
        port=$(app_port)
        conn=$(jack_lsp -c "$port" 2>/dev/null | sed -n 2p | tr -d ' ')
        sine_of "$port" "paplay-$f"; r=$?
        wait $p; rc=$?
        how=direct; grep -q 'resampled' "$logs/paplay-$f.log" && how=resampled
        if [ $r -eq 0 ] && [ $rc -eq 0 ] && [ "$conn" = system:playback_1 ]; then
            pass "paplay $f.wav: $how, to $conn by itself, drained, exit 0: $(cut -d' ' -f2- "$logs/paplay-$f.check")"
        else
            flunk "paplay $f.wav (rc $rc, to '$conn')" "$logs/paplay-$f.check" "$logs/paplay-$f.log"
        fi
    done
    timeout 10 "$L" -R paplay "$logs/s441.wav" >"$logs/paplay-R.log" 2>&1; rc=$?
    if [ $rc -ne 0 ] && grep -q 'rate 44100 Hz asked, JackBus runs at 48000 Hz (resampling is off' "$logs/paplay-R.log"; then
        pass "paplay -R of 44100 Hz: refused, with the reason (exit $rc)"
    else
        flunk "paplay -R not refused (rc $rc)" "$logs/paplay-R.log"
    fi
fi

# 2. parecord of a node's tone
if have parecord; then
    "$JACK_BUILD/example-clients/jackbus_tone" -n tone -f 440 >"$logs/tone.log" 2>&1 &
    tp=$!
    JACKBUS_PA_AUTOCONNECT=0 "$L" -v parecord --rate=48000 --channels=2 --format=s16le "$logs/rec.wav" >"$logs/parecord.log" 2>&1 &
    p=$!
    i=0; while [ $i -lt 100 ] && ! jack_lsp 2>/dev/null | grep -q ':in_FR$'; do sleep 0.1; i=$((i + 1)); done
    for ch in FL FR; do jack_connect tone:out "$(jack_lsp | grep ":in_$ch\$" | head -1)"; done
    sleep 2.5
    kill -INT $p; wait $p; rc=$?
    if [ $rc -eq 0 ] && python3 -I "$check" "$logs/rec.wav" 440 >"$logs/rec.check" 2>&1; then
        pass "parecord of a node's tone: $(cut -d' ' -f2- "$logs/rec.check")"
    else
        flunk "parecord (rc $rc)" "$logs/rec.check" "$logs/parecord.log"
    fi
    stop $tp
fi

# 3. mpv's pulse output: the whole file, no spurious underflow
if have mpv; then
    "$L" -v mpv --no-config --no-video --no-terminal --ao=pulse "$logs/s48.wav" >"$logs/mpv.log" 2>&1 &
    p=$!
    port=$(app_port)
    sine_of "$port" mpv; r=$?
    wait $p; rc=$?
    under=$(grep -c 'underflow (' "$logs/mpv.log")
    if [ $r -eq 0 ] && [ $rc -eq 0 ] && [ "$under" -le 1 ]; then
        pass "mpv --ao=pulse: the whole file ($under underflow, at its end), exit 0: $(cut -d' ' -f2- "$logs/mpv.check")"
    else
        flunk "mpv --ao=pulse (rc $rc, $under underflows)" "$logs/mpv.check" "$logs/mpv.log"
    fi
fi

# 4. SDL3's pulseaudio backend
if [ -x "$JACK_BUILD/pw-shim/sdl3-tone" ]; then
    SDL_AUDIO_DRIVER=pulseaudio "$L" -v "$JACK_BUILD/pw-shim/sdl3-tone" >"$logs/sdl.log" 2>&1 &
    p=$!
    port=$(app_port)
    sine_of "$port" sdl; r=$?
    wait $p; rc=$?
    if [ $r -eq 0 ] && [ $rc -eq 0 ] && grep -q '^driver: pulseaudio' "$logs/sdl.log" &&
       grep -q 'playback device: system playback (JackBus) (48000 Hz' "$logs/sdl.log" && ! grep -q resampled "$logs/sdl.log"; then
        pass "SDL3 pulseaudio: \"system playback (JackBus)\" at 48000 Hz, direct: $(cut -d' ' -f2- "$logs/sdl.check")"
    else
        flunk "SDL3 pulseaudio (rc $rc)" "$logs/sdl.check" "$logs/sdl.log"
    fi
else
    echo "SKIP SDL3 (make -C nspa/pw-shim test-apps)"
fi

# 5. pactl: introspection as a volume control sees it; what JackBus doesn't
#    have (cards, modules, the sample cache) answers empty or not supported
if have pactl; then
    P() { "$L" pactl "$@" 2>&1; }
    info=$(P info); sinks=$(P list short sinks); sources=$(P list short sources)
    cards=$(P list cards; echo "rc=$?"); mods=$(P list modules; echo "rc=$?"); clients=$(P list clients)
    stat=$(P stat; echo "rc=$?"); load=$(P load-module module-null-sink; echo "rc=$?")
    sample=$(P play-sample bell; echo "rc=$?")
    if echo "$info" | grep -q 'Server Name: PulseAudio (on JackBus)' &&
       echo "$info" | grep -q 'Default Sink: jackbus.system.playback' &&
       echo "$sinks" | grep -q 'jackbus.system.playback' && echo "$sources" | grep -q 'jackbus.system.capture' &&
       [ "$cards" = rc=0 ] && [ "$mods" = rc=0 ] && echo "$clients" | grep -q 'Client #1' &&
       echo "$stat" | grep -q 'rc=0' && echo "$load" | grep -q 'Not supported' && echo "$load" | grep -q 'rc=1' &&
       echo "$sample" | grep -q 'rc=1'; then
        pass "pactl: info, sinks, sources, clients; no cards or modules; load-module and play-sample refused"
    else
        printf '%s\n' "$info" "$sinks" "$sources" "$cards" "$mods" "$clients" "$stat" "$load" "$sample" >"$logs/pactl.log"
        flunk "pactl" "$logs/pactl.log"
    fi
fi

# 6. the session-wide way: the package puts the dispatcher on the dynamic
#    linker's path (here: the library path), so every program loading
#    libpulse gets it. Setting off (the default): PulseAudio's libpulse, as
#    without it. "pulse = yes" (PatchBox's switch): JackBus while it runs,
#    PulseAudio's while it doesn't. PipeWire's daemons: PulseAudio's. The
#    launcher's JACKBUS_PA_MODE=jackbus reaches a program whose library
#    path was replaced (as teams-sdl-cef does for its media host)
if have pactl; then
    auto() { env LD_LIBRARY_PATH="$out/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" JACKBUS_PA_VERBOSE=1 "$@" 2>&1; }
    off=$(auto pactl info)
    echo "pulse = yes" >"$logs/compat.conf"
    up=$(auto pactl info)
    down=$(JACK_DEFAULT_SERVER=jbpa-none auto pactl info)
    pwd_=$(auto bash -c 'exec -a pipewire pactl info')
    rm -f "$logs/compat.conf"
    forced=$(auto env LD_LIBRARY_PATH="$out/lib:$JACK_BUILD/common" JACKBUS_PA_MODE=jackbus pactl info)
    if echo "$off" | grep -q "jackbus-pa: PulseAudio's libpulse (the setting is off)" && ! echo "$off" | grep -q 'on JackBus' &&
       echo "$up" | grep -q 'jackbus-pa: JackBus (JackBus runs)' && echo "$up" | grep -q 'Server Name: PulseAudio (on JackBus)' &&
       echo "$down" | grep -q "jackbus-pa: PulseAudio's libpulse (JackBus isn't running)" &&
       echo "$down" | grep -q 'Connection failure: Connection refused' && ! echo "$down" | grep -q 'on JackBus' &&
       echo "$pwd_" | grep -q "jackbus-pa: PulseAudio's libpulse (a PipeWire daemon)" &&
       echo "$forced" | grep -q 'Server Name: PulseAudio (on JackBus)'; then
        pass "session-wide: setting off PulseAudio's; on: JackBus while it runs, PulseAudio's while not; PipeWire daemons PulseAudio's; JACKBUS_PA_MODE=jackbus wins"
    else
        printf '%s\n--\n' "$off" "$up" "$down" "$pwd_" "$forced" >"$logs/auto.log"
        flunk "session-wide dispatch" "$logs/auto.log"
    fi
fi

# 7. every installed program and library that links libpulse finds each of
#    its pa_ functions (Arch links with immediate binding: one missing and it
#    doesn't load; KDE's volume control and libcanberra are among them)
defs=$logs/defined.txt
for l in "$out/lib/libjackbus-pa.so" /usr/lib/pulseaudio/libpulsecommon-*.so /usr/lib/libpulse-simple.so.0 \
         /usr/lib/libpulse-mainloop-glib.so.0; do
    [ -e "$l" ] && nm -D --defined-only "$l"
done | awk '{print $3}' | sed 's/@.*//' | sort -u >"$defs"
users=0; lacking=
for f in $(find /usr/lib /usr/bin /opt -maxdepth 4 -type f \( -name '*.so*' -o -perm -u+x \) 2>/dev/null); do
    readelf -d "$f" 2>/dev/null | grep -qE 'NEEDED.*\[libpulse(-simple|-mainloop-glib)?\.so\.0\]' || continue
    users=$((users + 1))
    m=$(nm -D --undefined-only "$f" 2>/dev/null | awk '{print $2}' | sed 's/@.*//' | grep '^pa_' | sort -u | comm -23 - "$defs" | tr '\n' ' ')
    [ -n "$m" ] && lacking="$lacking
    $f: $m"
done
if [ -z "$lacking" ]; then
    pass "every installed libpulse user ($users programs and libraries) finds all its functions"
else
    echo "$lacking" >"$logs/lacking.txt"
    flunk "libpulse functions missing" "$logs/lacking.txt"
fi

# 8. the browsers, headless: a Web Audio tone
cat >"$logs/tone.html" <<'EOH'
<html><body><script>var c=new AudioContext();var o=c.createOscillator();o.frequency.value=440;var g=c.createGain();g.gain.value=0.5;o.connect(g);g.connect(c.destination);o.start();</script></body></html>
EOH
# the session-wide way, as a desktop has it: no launcher, the dispatcher on
# the library path, the switch on in ~/.config of their HOME
bhome=$logs/home
mkdir -p "$bhome/.config/jackbus"
echo "pulse = yes" >"$bhome/.config/jackbus/compat.conf"
browser() {  # label profile-dir command...
    label=$1; prof=$2; shift 2
    env -u JACKBUS_COMPAT_CONF HOME="$bhome" JACKBUS_PA_VERBOSE=1 \
        LD_LIBRARY_PATH="$out/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$@" >"$logs/$label.log" 2>&1 &
    p=$!
    port=$(app_port 300)
    if [ -n "$port" ]; then sine_of "$port" "$label"; r=$?; else r=1; fi
    kill -TERM $p 2>/dev/null; sleep 1.5
    pkill -KILL -f "$prof" 2>/dev/null; wait $p 2>/dev/null
    if [ $r -eq 0 ] && ! grep -q resampled "$logs/$label.log"; then
        pass "$label (no launcher: the switch on, sandboxed): a Web Audio tone, client \"${port%%:*}\", direct: $(cut -d' ' -f2- "$logs/$label.check")"
    else
        flunk "$label (port '$port')" "$logs/$label.check" "$logs/$label.log"
    fi
}
chrome=$(command -v google-chrome-stable || command -v chromium || ls /opt/google/chrome/chrome 2>/dev/null)
if [ -n "$chrome" ]; then
    mkdir -p "$logs/chrome-profile"
    # Chrome won't run as root with its sandbox: an ordinary user (a nested
    # user namespace), the sandbox on
    browser Chrome "$logs/chrome-profile" unshare --user --map-user=1000 --map-group=1000 -- \
        "$chrome" --headless=new --no-first-run \
        --disable-gpu --user-data-dir="$logs/chrome-profile" --autoplay-policy=no-user-gesture-required "file://$logs/tone.html"
else
    echo "SKIP Chrome (not installed)"
fi
firefox=$(ls /usr/lib/firefox/firefox 2>/dev/null)
if [ -n "$firefox" ]; then
    mkdir -p "$logs/ff-profile"
    printf '%s\n' 'user_pref("media.autoplay.default", 0);' 'user_pref("media.autoplay.blocking_policy", 0);' \
        'user_pref("browser.shell.checkDefaultBrowser", false);' 'user_pref("app.update.enabled", false);' \
        'user_pref("datareporting.policy.dataSubmissionEnabled", false);' >"$logs/ff-profile/user.js"
    browser Firefox "$logs/ff-profile" "$firefox" --headless --no-remote --profile "$logs/ff-profile" \
        "file://$logs/tone.html"
else
    echo "SKIP Firefox (not installed)"
fi

# 9. a stream's realtime thread
rt_cycles() {
    rt=$(grep -E 'sched_setscheduler\([0-9]+, SCHED_FIFO' "$1" | head -1 | awk '{print $1}')
    [ -n "$rt" ] || return 1
    awk -v t="$rt" '$1 == t' "$1" | sed -n '/sched_setscheduler(/,$p' | sed '1d' | sed '/<\.\.\. sched_setscheduler resumed>/d' | sed '/ --- SIG/,$d'
}
if command -v strace >/dev/null && have paplay; then
    timeout 30 strace -f -qq -o "$logs/rt.strace" "$L" paplay "$logs/s48.wav" >/dev/null 2>&1
    if rt_cycles "$logs/rt.strace" >"$logs/rt.loop" && [ -s "$logs/rt.loop" ]; then
        calls=$(sed -E 's/^[0-9]+ +//; s/\(.*//; s/^<\.\.\. ([a-z_0-9]+) resumed>.*/\1/' "$logs/rt.loop" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}')
        others=$(grep -vcE '^[0-9]+ +(futex\(|<\.\.\. futex resumed>)' "$logs/rt.loop")
        [ "$others" -eq 0 ] && pass "paplay's realtime thread: no system call but futex ($calls)" \
            || flunk "system calls in the realtime thread ($calls)" "$logs/rt.loop"
    else
        flunk "no realtime thread traced" "$logs/rt.strace"
    fi
fi

stop $sp
grep -qE 'Segmentation|Assertion|terminate called' "$logs/jackd.log" && flunk "server log" "$logs/jackd.log"
echo "logs: $logs"
[ $fail -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
