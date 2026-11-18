#!/bin/sh
# JackBus: sound card hotplug in jackdbus (the ALSA device list follows cards coming and
# going). Must run inside nspa/run-isolated.sh.
#
# A real card is "unplugged" and "plugged in" without touching hardware or needing root: in
# the namespace, /dev/snd becomes a tmpfs holding bind mounts of the real nodes; removing a
# card's nodes makes ALSA (and so jackdbus) stop seeing it, putting them back brings it
# back. jackdbus runs on a private session bus with a scratch HOME, as in jackdbus.sh.
#
# Checks: the list loses and regains the card with one Configure.ConstraintsChanged each time;
# a burst of changes gives one refresh; a card whose control node is not there yet is not
# listed until it is; unrelated files under /dev/snd change nothing; a running server (dummy
# driver) keeps running through it all with no xrun; jackdbus leaks no file descriptor over
# many cycles; no error in its log.
#
#   hotplug.sh <jackdbus-binary> [card=second card in /proc/asound] [cycles=20]
set -u
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    echo "refusing to run outside nspa/run-isolated.sh" >&2
    exit 2
fi
jackdbus=$1
cycles=${3:-20}
work=$(mktemp -d /tmp/nspa-hotplug.XXXXXX)
fail=0
result() { if [ "$2" = 0 ]; then echo "PASS $1: $3"; else echo "FAIL $1: $3"; fail=1; fi; }

# The card to unplug: given, or the last card ALSA has with audio (jackdbus lists PCM devices: a
# MIDI-only card, as a USB keyboard, is never in its list); not the first: keep one card listed.
card=${2:-}
if [ -z "$card" ]; then
    for c in $(sed -n 's/^ *\([0-9]*\) \[\([^ ]*\) *\].*/\2/p' /proc/asound/cards); do
        ls -d "/proc/asound/$c"/pcm* >/dev/null 2>&1 && card=$c
    done
fi
idx=$(readlink "/proc/asound/$card" 2>/dev/null | sed 's/^card//')
if [ -z "$idx" ] || [ "$(grep -c '^ *[0-9]* \[' /proc/asound/cards)" -lt 2 ]; then
    echo "SKIP hotplug: needs two sound cards (one to unplug, one to keep)"; exit 0
fi

# /dev/snd as a tmpfs of bind mounts
mkdir -p "$work/realsnd"
mount --bind /dev/snd "$work/realsnd" || { echo "SKIP hotplug: cannot bind /dev/snd"; exit 0; }
mount -t tmpfs -o mode=755 nspa-snd /dev/snd || { echo "SKIP hotplug: cannot mount over /dev/snd"; exit 0; }
plug_node() { : > "/dev/snd/$1" && mount --bind "$work/realsnd/$1" "/dev/snd/$1"; }
unplug_node() { umount "/dev/snd/$1" 2>/dev/null; rm -f "/dev/snd/$1"; }
card_nodes() { ls "$work/realsnd" | grep -E "^(pcm|midi|hw)C${idx}D|^controlC${idx}\$"; }
for n in $(ls "$work/realsnd"); do
    [ -c "$work/realsnd/$n" ] && plug_node "$n"
done
plug_card() {  # the kernel order: PCM and other nodes first, the control node last
    for nd in $(card_nodes | grep -v '^controlC'); do plug_node "$nd"; done
    plug_node "controlC$idx"
}
unplug_card() { for nd in $(card_nodes); do unplug_node "$nd"; done; }

export HOME="$work/home" XDG_CONFIG_HOME="$work/home/.config"
mkdir -p "$HOME"
log="$HOME/.log/jack/jackdbus.log"
cat >"$work/bus.conf" <<'CONF'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
dbus-daemon --config-file="$work/bus.conf" --fork --print-address=3 --print-pid=4 3>"$work/bus.addr" 4>"$work/bus.pid" \
    || { echo "SKIP hotplug: no dbus-daemon"; exit 0; }
DBUS_SESSION_BUS_ADDRESS=$(cat "$work/bus.addr"); export DBUS_SESSION_BUS_ADDRESS
call() { dbus-send --session --print-reply --dest=org.jackaudio.service /org/jackaudio/Controller "$@"; }
devices() {  # the device list jackdbus offers, one value per line
    call org.jackaudio.Configure.GetParameterConstraint array:string:drivers,alsa,device 2>/dev/null |
        sed -n 's/^ *variant *string "\(hw:[^"]*\)"$/\1/p'
}
has_card() { devices | grep -qx "hw:$card"; }
signals() { c=$(grep -c 'member=ConstraintsChanged' "$work/monitor.log" 2>/dev/null); echo "${c:-0}"; }
wait_signals() {  # wait_signals <count> <tenths>
    i=0; while [ "$(signals)" -lt "$1" ] && [ $i -lt "$2" ]; do sleep 0.1; i=$((i + 1)); done
    [ "$(signals)" -ge "$1" ]
}

dbus-monitor --session "type='signal',interface='org.jackaudio.Configure'" > "$work/monitor.log" 2>&1 &
mon=$!
"$jackdbus" auto >>"$work/jackdbus.stderr" 2>&1 &
jd=$!
i=0; until call org.jackaudio.JackControl.IsStarted >/dev/null 2>&1; do
    i=$((i + 1)); [ $i -gt 50 ] && { echo "FAIL hotplug: jackdbus did not answer"; kill $mon $jd; exit 1; }
    sleep 0.1
done
cleanup() { call org.jackaudio.JackControl.Exit >/dev/null 2>&1; sleep 0.3; kill $jd $mon 2>/dev/null; kill "$(cat "$work/bus.pid")" 2>/dev/null; }
trap cleanup EXIT

errs0=$(grep -c "ERROR" "$log" 2>/dev/null); errs0=${errs0:-0}   # start-up messages are not ours
has_card; result "listed at start" $? "hw:$card in jackdbus's ALSA device list"

# a running server through all of it: the dummy driver, nothing to do with the cards
call org.jackaudio.Configure.SetParameterValue array:string:engine,driver variant:string:dummy >/dev/null 2>&1
call org.jackaudio.Configure.SetParameterValue array:string:engine,realtime variant:boolean:false >/dev/null 2>&1
call org.jackaudio.JackControl.StartServer >/dev/null 2>&1
call org.jackaudio.JackControl.IsStarted 2>/dev/null | grep -q "boolean true"; result "server started" $? "dummy driver"

fds0=$(ls /proc/$jd/fd | wc -l)
s0=$(signals)

# unplug, plug
t0=$(date +%s%N); unplug_card
wait_signals $((s0 + 1)) 40; ok=$?; t1=$(date +%s%N)
! has_card && [ $ok = 0 ]; result "unplugged" $? "hw:$card gone, signal after $(( (t1 - t0) / 1000000 )) ms"
grep -A3 'member=ConstraintsChanged' "$work/monitor.log" | grep -q '"alsa"'; result "signal names the container" $? "drivers/alsa"
plug_card
wait_signals $((s0 + 2)) 40; ok=$?
has_card && [ $ok = 0 ]; result "plugged in" $? "hw:$card back"

# a burst: three changes within 0.2 s give one refresh
s=$(signals)
unplug_card; sleep 0.05; plug_card; sleep 0.05; unplug_card
wait_signals $((s + 1)) 40; sleep 1
[ "$(signals)" = $((s + 1)) ] && ! has_card; result "burst" $? "$(( $(signals) - s )) signal(s) for 3 changes, card absent at the end"
plug_card; wait_signals $((s + 2)) 40

# a card without its control node is not listed until the node is there
s=$(signals)
unplug_card; wait_signals $((s + 1)) 40
for nd in $(card_nodes | grep -v '^controlC'); do plug_node "$nd"; done
sleep 1.5
[ "$(signals)" = $((s + 1)) ] && ! has_card; ok1=$?
plug_node "controlC$idx"; wait_signals $((s + 2)) 40; ok2=$?
[ $ok1 = 0 ] && [ $ok2 = 0 ] && has_card; result "waits for the control node" $? "not listed with PCM nodes only; listed once controlC$idx appeared"

# unrelated changes under /dev/snd: nothing
s=$(signals)
: > /dev/snd/nspa-unrelated; sleep 0.1; rm -f /dev/snd/nspa-unrelated; sleep 1.2
[ "$(signals)" = "$s" ]; result "unrelated files" $? "no refresh"

# many cycles: descriptors stay put
s=$(signals)
cyc=0; while [ $cyc -lt "$cycles" ]; do
    unplug_card; wait_signals $((s + 2 * cyc + 1)) 40
    plug_card; wait_signals $((s + 2 * cyc + 2)) 40
    cyc=$((cyc + 1))
done
fds1=$(ls /proc/$jd/fd | wc -l)
[ "$(signals)" = $((s + 2 * cycles)) ] && [ "$fds1" = "$fds0" ] && has_card
result "$cycles cycles" $? "$(( $(signals) - s )) signals for $((2 * cycles)) changes, jackdbus fds $fds0 -> $fds1"

call org.jackaudio.JackControl.IsStarted 2>/dev/null | grep -q "boolean true"; ok1=$?
xr=$(call org.jackaudio.JackControl.GetXruns 2>/dev/null | sed -n 's/.*uint32 //p')
[ $ok1 = 0 ] && [ "${xr:-1}" = 0 ]; result "server unaffected" $? "still started, ${xr:-?} xruns"

errs=$(grep -c "ERROR" "$log" 2>/dev/null); errs=$(( ${errs:-0} - errs0 ))
[ "$errs" = 0 ]; result "log" $? "$errs ERROR lines after start; $(grep -c 'Sound cards changed' "$log" 2>/dev/null) refreshes logged"

exit $fail
