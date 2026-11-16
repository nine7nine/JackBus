#!/bin/sh
# Device volumes through jackbus-pa: a card's own mixer. On an isolated
# JackBus (dummy driver), a second card (fake-card) whose ports name an ALSA
# device as the ALSA driver's do ("alsa_pcm:hw:jbtest:in1"); that card's
# mixer is a test plugin (fake-mixer.c: an HDA card's Master and Capture,
# its values in a file), put in place of every card's control device by the
# test's own ALSA configuration: no real card's mixer is opened.
#  - the sink's volume is Master's, as alsamixer shows it (its mapping);
#  - setting it sets Master; +5 % / -5 % steps always move it; balance;
#    over 100 %: 100 %; mute is the switch;
#  - the source's volume is Capture's, 100 % its 0 dB (unity), gain beyond;
#  - a change made by another process (amixer, another client) reaches a
#    subscriber as a sink change;
#  - a device of no card keeps a fixed volume, a request accepted.
#
#   make -C nspa/pa-shim all test-apps && nspa/pa-shim/test/volume.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$tree/nspa/run-isolated.sh" "$0" "$@"
fi
out=$JACK_BUILD/pa-shim
L=$here/../jackbus-pa
logs=$(mktemp -d /tmp/jbpa-vol.XXXXXX)
for f in lib/libpulse.so.0 lib/libjackbus-pa.so fake-card libasound_module_ctl_jbtest.so; do
    [ -e "$out/$f" ] || { echo "missing $f: make -C nspa/pa-shim all test-apps"; exit 2; }
done
command -v pactl >/dev/null || { echo "SKIP: no pactl"; exit 0; }

PULSE_SERVER=unix:/jackbus-pa-test/no-server JACK_DEFAULT_SERVER=jbvol JACKBUS_COMPAT_CONF=$logs/compat.conf
ALSA_CONFIG_PATH=$logs/alsa.conf
export PULSE_SERVER JACK_DEFAULT_SERVER JACKBUS_COMPAT_CONF ALSA_CONFIG_PATH
: >"$logs/compat.conf"
cat >"$ALSA_CONFIG_PATH" <<EOF
ctl.hw {
    @args [ CARD ]
    @args.CARD { type string }
    type jbtest
    card \$CARD
    dir "$logs"
}
ctl_type.jbtest { lib "$out/libasound_module_ctl_jbtest.so" }
EOF
# Master 0 dB both sides, on; Capture 40 (+12.75 dB), on
echo "86 86 1 1 40 40 1 1" >"$logs/jbtest"

fail=0
pass() { echo "PASS $1"; }
flunk() { echo "FAIL $1"; shift; for l in "$@"; do [ -f "$l" ] && tail -8 "$l" | sed 's/^/    /'; done; fail=1; }
st() { cut -d' ' -f"$1" "$logs/jbtest"; }      # a field of the mixer's state
pa() { "$L" pactl "$@" 2>>"$logs/pactl.log"; }
pct() { pa "get-$1-volume" "$2" | grep -o '[0-9]*%' | tr -d % | tr '\n' ' ' | sed 's/ $//'; }
# alsamixer's percentage of a raw value (-64.50 dB + 0.75 dB a step, 0 dB max)
expect() { python3 -I -c "
import math, sys
r, lo, step, hi = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
db = lo + r * step
n = 10 ** ((db - hi) / 6000); nmin = 10 ** ((lo - hi) / 6000)
print(round((n - nmin) / (1 - nmin) * 100))" "$@"; }

"$JACK_BUILD/jackd" -n jbvol -R -P 70 -d dummy -r 48000 -p 256 >"$logs/jackd.log" 2>&1 &
sp=$!
i=0; while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbvol_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
"$out/fake-card" fakecard hw:jbtest >"$logs/fake-card.log" 2>&1 &
fc=$!
i=0; while [ $i -lt 50 ] && ! grep -q ready "$logs/fake-card.log"; do sleep 0.1; i=$((i + 1)); done
S=jackbus.fakecard.playback
C=jackbus.fakecard.capture

# 1. the sink's volume is Master's
v=$(pct sink $S)
flags=$(pa list sinks | sed -n "/Name: $S/,/Flags/p" | grep Flags)
if [ "$v" = "100 100" ] && echo "$flags" | grep -q HW_VOLUME_CTRL && echo "$flags" | grep -q HW_MUTE_CTRL; then
    pass "sink $S: Master's volume (100 %), hardware volume and mute"
else
    flunk "sink volume '$v', '$flags'" "$logs/pactl.log"
fi
echo "43 43 1 1 40 40 1 1" >"$logs/jbtest"
v=$(pct sink $S); e=$(expect 43 -6450 75 0)
[ "$v" = "$e $e" ] && pass "Master at raw 43: $v % (alsamixer's $e %)" || flunk "Master at 43: '$v', want $e" "$logs/pactl.log"

# 2. setting it sets Master (and reads back as set)
pa set-sink-volume $S 50%; r=$(st 1)
v=$(pct sink $S)
if [ "$(st 1)" = "$(st 2)" ] && [ "$r" -gt 43 ] && [ "${v% *}" -ge 49 ] && [ "${v% *}" -le 51 ]; then
    pass "set 50 %: Master raw $r both sides, reads $v %"
else
    flunk "set 50 %: raw $(st 1)/$(st 2), reads '$v'" "$logs/pactl.log"
fi

# 3. volume keys: each 5 % step moves Master, up and down
ok=1; prev=$(st 1); seq=""
for d in +5% +5% +5% -5% -5% -5% -5%; do
    pa set-sink-volume $S $d; now=$(st 1); seq="$seq $now"
    case $d in +*) [ "$now" -gt "$prev" ] || ok=0 ;; -*) [ "$now" -lt "$prev" ] || ok=0 ;; esac
    prev=$now
done
[ $ok = 1 ] && pass "5 % steps up and down each move Master (raw$seq)" || flunk "steps: raw$seq" "$logs/pactl.log"

# 4. balance; over 100 %; 0 %
pa set-sink-volume $S 40% 60%
[ "$(st 1)" -lt "$(st 2)" ] && pass "balance 40 % / 60 %: Master $(st 1) / $(st 2)" || flunk "balance: $(st 1) / $(st 2)"
pa set-sink-volume $S 150%; v=$(pct sink $S)
[ "$(st 1) $(st 2)" = "86 86" ] && [ "$v" = "100 100" ] && pass "150 %: Master at its maximum, reads 100 %" || flunk "150 %: $(st 1) $(st 2), '$v'"
pa set-sink-volume $S 0%; v=$(pct sink $S)
[ "$(st 1) $(st 2)" = "0 0" ] && [ "$v" = "0 0" ] && pass "0 %: Master at its minimum, reads 0 %" || flunk "0 %: $(st 1) $(st 2), '$v'"
pa set-sink-volume $S 70%

# 5. mute is the switch
pa set-sink-mute $S 1; m1=$(pa get-sink-mute $S); s1="$(st 3) $(st 4)"
pa set-sink-mute $S toggle; m2=$(pa get-sink-mute $S); s2="$(st 3) $(st 4)"
if [ "$s1" = "0 0" ] && [ "$m1" = "Mute: yes" ] && [ "$s2" = "1 1" ] && [ "$m2" = "Mute: no" ]; then
    pass "mute: the Master switch (off, then on by toggle)"
else
    flunk "mute: '$s1' '$m1', '$s2' '$m2'" "$logs/pactl.log"
fi

# 6. the source's volume is Capture's: 100 % is 0 dB (unity), its gain
# above (to +30 dB) beyond 100 %
v0=$(pct source $C); pa set-source-volume $C 30%; v=$(pct source $C); r30=$(st 5)
pa set-source-volume $C 100%; r100=$(st 5); v100=$(pct source $C)
pa set-source-volume $C 150%; r150=$(st 5); v150=$(pct source $C)
pa set-source-mute $C 1; cm="$(st 7) $(st 8)"; pa set-source-mute $C 0
# raw 40 is +12.75 dB: 163 %; raw 23 is 0 dB; 150 % is +10.6 dB (raw 37 or 38);
# 30 %: within a step (0.75 dB, about 4 % here), rounded the way it moves
if [ "$v0" = "163 163" ] && [ "${v% *}" -ge 25 ] && [ "${v% *}" -le 31 ] && [ "$r30" -lt 23 ] &&
   [ "$r100" = 23 ] && [ "$v100" = "100 100" ] && [ "$r150" -ge 37 ] && [ "$r150" -le 38 ] &&
   [ "${v150% *}" -ge 149 ] && [ "${v150% *}" -le 155 ] && [ "$cm" = "0 0" ]; then
    pass "source $C: Capture's volume, 100 % = 0 dB (raw 23); +12.75 dB reads $v0 %; 30 %: raw $r30; 150 %: raw $r150 ($v150 %); mute its switch"
else
    flunk "source: '$v0'; 30%: '$v' raw $r30; 100%: raw $r100 '$v100'; 150%: raw $r150 '$v150'; mute '$cm'" "$logs/pactl.log"
fi

# 7. changes made elsewhere reach a subscriber
"$L" pactl subscribe >"$logs/sub.log" 2>&1 &
sub=$!
sleep 1
idx=$(pa list short sinks | awk -v n=$S '$2 == n { print $1 }')
amixer -q -D hw:jbtest sset Master 20% 2>>"$logs/amixer.log"
sleep 0.5
a=$(grep -c "'change' on sink #$idx" "$logs/sub.log")
pa set-sink-volume $S 60%
sleep 0.5
b=$(grep -c "'change' on sink #$idx" "$logs/sub.log")
kill $sub 2>/dev/null; wait $sub 2>/dev/null
if [ "$a" -ge 1 ] && [ "$b" -gt "$a" ]; then
    pass "a subscriber hears of amixer's change and another client's ($b sink #$idx change events)"
else
    flunk "subscriber: $a after amixer, $b after a client" "$logs/sub.log" "$logs/amixer.log"
fi

# 8. a device of no card: a fixed volume, a request accepted
pa set-sink-volume jackbus.system.playback 30%; rc=$?
v=$(pct sink jackbus.system.playback)
flags=$(pa list sinks | sed -n "/Name: jackbus.system.playback/,/Flags/p" | grep Flags)
if [ $rc = 0 ] && [ "$v" = "100 100" ] && ! echo "$flags" | grep -q HW_VOLUME_CTRL; then
    pass "jackbus.system.playback (no card): 100 %, a request accepted, not applied"
else
    flunk "no card: rc $rc, '$v', '$flags'" "$logs/pactl.log"
fi

kill $fc 2>/dev/null
kill $sp 2>/dev/null; wait $sp 2>/dev/null
if [ $fail = 0 ]; then echo "ALL PASS"; rm -rf "$logs"; else echo "logs: $logs"; exit 1; fi
