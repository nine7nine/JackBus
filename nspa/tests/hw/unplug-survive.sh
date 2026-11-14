#!/bin/sh
# A JACK server whose card is unplugged while it runs, and plugged back (hotplug H2/H3).
# Isolated: the fork's jackdbus on a private bus with a scratch HOME, in nspa/run-isolated.sh.
# The card is unplugged by hand (or any other way): the script waits for it to go and come back.
#
#   nspa/tests/hw/unplug-survive.sh [card=U44] [period=256]
#
# A client (survivor_client) connected to system:capture_1 and system:playback_1 writes zeros.
# Checks: while the card is gone the client keeps getting cycles and no shutdown, its
# connections stay (to the dummy driver's ports), jackdbus says so; once the card is back the
# server runs on it again, the connections are on it, cycles go on; jackdbus leaks no fds.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$root/nspa/run-isolated.sh" "$0" "$@"
fi
card=${1:-U44}
period=${2:-256}
work=$(mktemp -d /tmp/nspa-unplug.XXXXXX)
fail=0
result() { if [ "$2" = 0 ]; then echo "PASS $1: $3"; else echo "FAIL $1: $3"; fail=1; fi; }
[ -e "/proc/asound/$card" ] || { echo "SKIP: no card $card"; exit 0; }

cc -O2 -Wall ${NSPA_TEST_CFLAGS:-} -I"$root/common" -o "$work/survivor" "$root/nspa/tests/survivor_client.c" -L"$JACK_BUILD/common" -ljack || exit 1
export HOME="$work/home" XDG_CONFIG_HOME="$work/home/.config"; mkdir -p "$HOME"
log="$HOME/.log/jack/jackdbus.log"
cat >"$work/bus.conf" <<'CONF'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy></busconfig>
CONF
dbus-daemon --config-file="$work/bus.conf" --fork --print-address=3 --print-pid=4 3>"$work/bus.addr" 4>"$work/bus.pid" || exit 1
DBUS_SESSION_BUS_ADDRESS=$(cat "$work/bus.addr"); export DBUS_SESSION_BUS_ADDRESS
call() { dbus-send --session --print-reply --dest=org.jackaudio.service /org/jackaudio/Controller "$@"; }
set_p() { call org.jackaudio.Configure.SetParameterValue "array:string:$1" "$2" >/dev/null; }

"$JACK_BUILD/dbus/jackdbus" auto >"$work/jackdbus.stderr" 2>&1 &
jd=$!
i=0; until call org.jackaudio.JackControl.IsStarted >/dev/null 2>&1; do i=$((i+1)); [ $i -gt 50 ] && exit 1; sleep 0.1; done
trap 'call org.jackaudio.JackControl.Exit >/dev/null 2>&1; sleep 0.5; kill $jd $cl 2>/dev/null; kill "$(cat "$work/bus.pid")" 2>/dev/null' EXIT
set_p engine,driver variant:string:alsa
set_p engine,realtime variant:boolean:false
set_p driver,device "variant:string:hw:$card"
set_p driver,period "variant:uint32:$period"
set_p driver,nperiods variant:uint32:2
call org.jackaudio.JackControl.StartServer >/dev/null 2>&1
call org.jackaudio.JackControl.IsStarted 2>/dev/null | grep -q "boolean true"; result "started on hw:$card" $? "period $period"

"$work/survivor" 600 > "$work/client.log" 2>&1 &
cl=$!
sleep 2
fds0=$(ls /proc/$jd/fd | wc -l)
last() { tail -1 "$work/client.log"; }
echo ">>> unplug $card now (waiting up to 3 min)"
i=0; while [ -e "/proc/asound/$card" ] && [ $i -lt 1800 ]; do sleep 0.1; i=$((i+1)); done
[ -e "/proc/asound/$card" ] && { echo "FAIL: $card was not unplugged"; exit 1; }
sleep 3
gone=$(last)
grep -q "keeps running on the dummy driver" "$log"; result "dummy took over" $? "$(grep -m1 'keeps running' "$log" | cut -c27-)"
echo "$gone" | grep -q "shutdown=0" && [ "$(echo "$gone" | sed 's/.*rate=\([0-9]*\).*/\1/')" -gt 10 ]; result "client ran on" $? "$gone"
echo "$gone" | grep -q "in=1 out=1"; result "connections kept" $? "$gone"
echo ">>> plug $card back in (waiting up to 3 min)"
i=0; while [ ! -e "/proc/asound/$card" ] && [ $i -lt 1800 ]; do sleep 0.1; i=$((i+1)); done
i=0; until grep -q "is back: the server runs" "$log" || [ $i -gt 400 ]; do sleep 0.1; i=$((i+1)); done
sleep 2
back=$(last)
grep -q "is back: the server runs" "$log"; result "back on the card" $? "$(grep -m1 'is back: the server runs' "$log" | cut -c27-)"
echo "$back" | grep -q "shutdown=0" && echo "$back" | grep -q "in=1 out=1" && [ "$(echo "$back" | sed 's/.*rate=\([0-9]*\).*/\1/')" -gt 10 ]
result "client ran on, connected" $? "$back"
xr=$(call org.jackaudio.JackControl.GetXruns 2>/dev/null | sed -n 's/.*uint32 //p')
echo "INFO xruns reported over the unplug and the return: ${xr:-?}"
fds1=$(ls /proc/$jd/fd | wc -l)
[ "$fds1" -le "$fds0" ]; result "fds" $? "$fds0 -> $fds1"
grep -c "connection(s) to its ports restored" "$log" | grep -q "^[1-9]"; result "connections restored by the server" $? "$(grep -c 'restored' "$log") switch(es) restored connections"
echo "--- jackdbus log (nspa lines):"; grep -E "dummy driver|is back|restored|device is gone|could not" "$log" | cut -c27- | tail -8
exit $fail
