#!/bin/sh
# jackbus-mixer: a card's mixer state saved and put back, on JackBus's test
# control plugin (fake-mixer.c, "jbtest": an HDA card's Master and Capture,
# their values in a file) put in place of every card's control device by the
# test's ALSA configuration: no real card's mixer is touched.
#  - save writes the card's controls to ~/.config/jackbus/mixer/<id>.state;
#  - changed elsewhere (another process), restore puts them back, every value;
#  - restore with nothing saved: says so, changes nothing;
#  - the ALSA driver opening the card (a null PCM in place of its audio
#    device: no hardware) puts the saved state back.
#
#   make -C nspa/pa-shim test-apps && nspa/tests/mixer-state.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$tree/nspa/run-isolated.sh" "$0" "$@"
fi
plugin=$JACK_BUILD/pa-shim/libasound_module_ctl_jbtest.so
tool=$JACK_BUILD/jackbus-mixer
[ -e "$plugin" ] && [ -x "$tool" ] || { echo "missing $plugin or $tool"; exit 2; }
logs=$(mktemp -d /tmp/jbmixer.XXXXXX)
export XDG_CONFIG_HOME=$logs/config ALSA_CONFIG_PATH=$logs/alsa.conf
cat >"$ALSA_CONFIG_PATH" <<EOC
ctl.hw {
    @args [ CARD ]
    @args.CARD { type string }
    type jbtest
    card \$CARD
    dir "$logs"
}
ctl_type.jbtest { lib "$plugin" }
pcm.hw {
    @args [ CARD DEV SUBDEV ]
    @args.CARD { type string }
    @args.DEV { type integer default 0 }
    @args.SUBDEV { type integer default -1 }
    type null
}
EOC
card=$(sed -n 's/^ *\([0-9][0-9]*\) \[.*/\1/p' /proc/asound/cards | head -1)
[ -n "$card" ] || { echo "SKIP mixer-state: no sound card listed (the plugin stands in for one)"; exit 0; }
st=$logs/$card
fail=0
pass() { echo "PASS $1"; }
flunk() { echo "FAIL $1"; shift; for l in "$@"; do [ -f "$l" ] && tail -5 "$l" | sed 's/^/    /'; done; fail=1; }

echo "86 86 1 1 40 40 1 1" >"$st"
"$tool" restore "$card" >"$logs/r0.log" 2>&1; rc=$?
[ $rc = 1 ] && [ "$(cat "$st")" = "86 86 1 1 40 40 1 1" ] && grep -q 'nothing saved' "$logs/r0.log" &&
    pass "restore with nothing saved: says so, changes nothing" || flunk "restore with nothing saved: exit $rc, state $(cat "$st")" "$logs/r0.log"

echo "60 70 1 0 23 23 0 1" >"$st"
"$tool" save "$card" >"$logs/save.log" 2>&1; rc=$?
f=$("$tool" path "$card")
[ $rc = 0 ] && [ -s "$f" ] && grep -q '^Master|0|pvol|0:60 1:70$' "$f" && grep -q '^Master|0|psw|0:1 1:0$' "$f" &&
    grep -q '^Capture|0|cvol|0:23 1:23$' "$f" &&
    pass "save: $(grep -vc '^#' "$f") lines in $(basename "$f") (Master 60/70, its switch on/off, Capture 23)" ||
    flunk "save: exit $rc" "$logs/save.log" "$f"

echo "20 30 0 0 63 63 1 1" >"$st"       # another process changed it all
"$tool" restore "$card" >"$logs/r1.log" 2>&1; rc=$?
[ $rc = 0 ] && [ "$(cat "$st")" = "60 70 1 0 23 23 0 1" ] &&
    pass "restore after a change elsewhere: every value back ($(cat "$logs/r1.log"))" ||
    flunk "restore: exit $rc, state $(cat "$st") (want 60 70 1 0 23 23 0 1)" "$logs/r1.log"

echo "20 30 0 0 63 63 1 1" >"$st"
JACK_NO_AUDIO_RESERVATION=1 JACK_DEFAULT_SERVER=jbmixer timeout 6 "$JACK_BUILD/jackd" -n jbmixer \
    -d alsa -d "hw:$card" -r 48000 -p 256 -n 2 >"$logs/jackd.log" 2>&1
[ "$(cat "$st")" = "60 70 1 0 23 23 0 1" ] && grep -q "mixer as saved" "$logs/jackd.log" &&
    pass "the ALSA driver opening the card: the saved state back ($(grep -o "card .*mixer as saved.*" "$logs/jackd.log"))" ||
    flunk "the driver's open: state $(cat "$st")" "$logs/jackd.log"

[ $fail = 0 ] && { echo "ALL PASS"; rm -rf "$logs"; } || echo "logs: $logs"
exit $fail
