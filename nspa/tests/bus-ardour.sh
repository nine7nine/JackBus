#!/bin/sh
# Ardour (headless: hardour) over the JackBus socket: a real DAW, unmodified,
# its JACK backend loading this tree's libjack with JACKBUS_TRANSPORT=1.
# Dummy driver; a scratch HOME, so Ardour's own configuration is never touched.
#
#   nspa/run-isolated.sh nspa/tests/bus-ardour.sh
#
# Checks: hardour loads a new session and runs as a JackBus client (by its
# maps); its master bus reaches system:playback; a JackBus node's tone fed into
# the master comes out as a clean, continuous sine of the same frequency (the
# master only applies gain); buffer-size changes while it runs; it exits on
# SIGINT.
set -u
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    echo "refusing to run outside nspa/run-isolated.sh" >&2
    exit 2
fi
here="$(cd "$(dirname "$0")" && pwd)"
out="${NSPA_TEST_OUT:-/tmp/JackBus-tests}/bus-ardour"
hardour=$(ls /usr/lib/ardour9/hardour-* 2>/dev/null | head -1)
newsession=/usr/lib/ardour9/utils/ardour9-new_session
if [ -z "$hardour" ] || [ ! -x "$newsession" ]; then
    echo "SKIP bus-ardour: Ardour 9 (hardour) not installed"
    exit 0
fi
rm -rf "$out"; mkdir -p "$out/home"
unset JACKBUS_TRANSPORT
export JACK_DEFAULT_SERVER=jbardour
fail=0
pass() { echo "PASS $1"; }
flunk() { echo "FAIL $1"; shift; for l in "$@"; do [ -f "$l" ] && tail -15 "$l" | sed 's/^/    /'; done; fail=1; }
lsp() { env -u JACKBUS_TRANSPORT timeout 10 jack_lsp "$@" 2>/dev/null; }

# Ardour's environment (as its wrappers set it), a scratch home, and this tree's
# libjack first: its JACK backend dlopens libjack.so.0. Programs are started
# through env itself (never a shell function in the background: its $! would be
# a subshell's)
HOME_ARD="$out/home"
ARD_ENV="HOME=$HOME_ARD XDG_CONFIG_HOME=$HOME_ARD/.config XDG_CACHE_HOME=$HOME_ARD/.cache XDG_DATA_HOME=$HOME_ARD/.local/share"
ARD_ENV="$ARD_ENV ARDOUR_DATA_PATH=/usr/share/ardour9 ARDOUR_CONFIG_PATH=/etc/ardour9 ARDOUR_DLL_PATH=/usr/lib/ardour9"
ARD_ENV="$ARD_ENV LD_LIBRARY_PATH=$JACK_BUILD/common:/usr/lib/ardour9"

rm -f /dev/shm/jack_jbardour_*
"$JACK_BUILD/jackd" -n jbardour -R -P 70 -d dummy -r 48000 -p 256 >"$out/jackd.log" 2>&1 &
server_pid=$!
i=0
while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbardour_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done

# (the tool may abort while exiting, after writing the session: the session counts)
env $ARD_ENV timeout 60 "$newsession" -s 48000 -m 2 "$out/session" busdaw >"$out/newsession.log" 2>&1
if [ ! -f "$out/session/busdaw.ardour" ]; then
    flunk "a new Ardour session" "$out/newsession.log"
fi

env $ARD_ENV JACKBUS_TRANSPORT=1 stdbuf -oL "$hardour" -c ardour "$out/session" busdaw >"$out/hardour.log" 2>&1 &
ard=$!
i=0
while [ $i -lt 300 ] && ! lsp | grep -q '^ardour:'; do sleep 0.1; i=$((i + 1)); done
if lsp | grep -q '^ardour:'; then
    if ! grep -q 'jack-shm-registry' "/proc/$ard/maps" && grep -q 'jack_alive\.' "/proc/$ard/maps"; then
        pass "hardour runs as a JackBus client ($(lsp | grep -c '^ardour:') ports)"
    else
        flunk "hardour is not a JackBus client by its maps" "$out/hardour.log"
    fi
else
    flunk "hardour did not come up" "$out/hardour.log" "$out/newsession.log"
fi

# the master bus (registered once the session is loaded) to the system playback,
# and a tone through it
i=0
while [ $i -lt 300 ] && ! lsp | grep -q '^ardour:Master/audio_out'; do sleep 0.1; i=$((i + 1)); done
lsp >"$out/ports"
master_out=$(grep -E '^ardour:Master/audio_out' "$out/ports" | head -1)
master_in=$(grep -E '^ardour:Master/audio_in' "$out/ports" | head -1)
if [ -n "$master_out" ] && lsp -c "$master_out" | sed -n '2,$p' | grep -q 'system:playback'; then
    pass "Ardour's master ($master_out) reaches system:playback"
else
    flunk "master -> system:playback ('$master_out')" "$out/ports"
fi
if [ -z "$master_in" ]; then
    flunk "no master input port" "$out/ports"
elif [ -x "$JACK_BUILD/example-clients/jackbus_tone" ]; then
    "$JACK_BUILD/example-clients/jackbus_tone" -n tone -f 440 -c >"$out/tone.log" 2>&1 & tp=$!
    i=0; while [ $i -lt 50 ] && ! lsp | grep -q '^tone:out'; do sleep 0.1; i=$((i + 1)); done
    env -u JACKBUS_TRANSPORT timeout 10 jack_connect tone:out "$master_in"
    sleep 0.5
    env JACKBUS_TRANSPORT=1 timeout 20 jack_rec -f "$out/master.wav" -d 2 "$master_out" >"$out/rec.log" 2>&1
    if python3 -I "$here/tools/sine_check.py" "$out/master.wav" 440 >"$out/master-check" 2>&1; then
        pass "a JackBus tone through Ardour's master: $(cut -d' ' -f2- "$out/master-check")"
    else
        flunk "tone through Ardour's master" "$out/master-check" "$out/rec.log"
    fi
    kill -TERM $tp; wait $tp 2>/dev/null
fi

# a buffer-size change under it
env JACKBUS_TRANSPORT=1 timeout 10 jack_bufsize 512 >/dev/null 2>&1
sleep 1
env JACKBUS_TRANSPORT=1 timeout 10 jack_bufsize 256 >/dev/null 2>&1
sleep 1
kill -0 $ard 2>/dev/null && lsp | grep -q '^ardour:' && pass "hardour survives buffer-size changes 256 -> 512 -> 256" \
    || flunk "hardour after buffer-size changes" "$out/hardour.log"

kill -INT $ard
i=0; while kill -0 $ard 2>/dev/null && [ $i -lt 150 ]; do sleep 0.1; i=$((i + 1)); done
if kill -0 $ard 2>/dev/null; then
    kill -KILL $ard; flunk "hardour did not exit on SIGINT" "$out/hardour.log"
else
    wait $ard 2>/dev/null; rc=$?
    if [ $rc -eq 0 ] || [ $rc -eq 130 ]; then
        pass "hardour exited on SIGINT (status $rc)"
    else
        flunk "hardour's exit status $rc after SIGINT" "$out/hardour.log"
    fi
fi
kill -TERM "$server_pid"; wait "$server_pid" 2>/dev/null
grep -qE 'Segmentation|Assertion|terminate called' "$out/jackd.log" && flunk "server log" "$out/jackd.log"

echo "logs: $out"
[ $fail -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
