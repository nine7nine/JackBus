#!/bin/sh
# Sound card bridges through jackdbus, on snd-aloop (nothing is played on a real card):
#   - the "bridges" engine parameter: checked, added when the server starts, applied at once
#     while it runs, kept with the settings;
#   - AddBridge / RemoveBridge / GetBridges;
#   - the master's buffer size changed (SetBufferSize), and its sample rate (SwitchMaster to the
#     dummy driver at another rate): the bridges lock again, the card at the new rate.
# A private session bus and a temporary HOME (the user's settings and session bus are never
# touched). Must run inside nspa/run-isolated.sh.
#   nspa/run-isolated.sh nspa/tests/bridge/jackdbus-bridge.sh
set -u
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    echo "refusing to run outside nspa/run-isolated.sh" >&2
    exit 2
fi
if [ ! -e /proc/asound/Loopback ]; then
    echo "SKIP jackdbus-bridge: no snd-aloop card (modprobe snd-aloop)"
    exit 0
fi
jackdbus=$JACK_BUILD/dbus/jackdbus
work=$(mktemp -d /tmp/nspa-jackdbus-bridge.XXXXXX)
export HOME="$work/home" XDG_CONFIG_HOME="$work/home/.config"
mkdir -p "$HOME"
log="$HOME/.log/jack/jackdbus.log"
fail=0

cat > "$work/bus.conf" <<'CONF'
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
    || { echo "SKIP jackdbus-bridge: no dbus-daemon"; exit 0; }
DBUS_SESSION_BUS_ADDRESS=$(cat "$work/bus.addr")
export DBUS_SESSION_BUS_ADDRESS

call() {
    dbus-send --session --print-reply --dest=org.jackaudio.service /org/jackaudio/Controller "$@" >/dev/null 2>&1
}
gd() {
    gdbus call --session --dest org.jackaudio.service --object-path /org/jackaudio/Controller --method "$@" 2>&1
}
setp() {  # setp <address,comma,separated> <dbus-send variant>
    call org.jackaudio.Configure.SetParameterValue "array:string:$1" "$2"
}
getp() {
    gd org.jackaudio.Configure.GetParameterValue "['$1', '$2']" | sed -n "s/.*<'\([^']*\)'>.*/\1/p"
}
bridges() {
    gd org.jackaudio.JackControl.GetBridges
}
# wait_locked <client name> <seconds>: true once its card runs and both directions are locked
wait_locked() {
    i=0
    while [ $i -lt $(($2 * 10)) ]; do
        bridges | grep -q "'$1', 'running', 'locked', 'locked'" && return 0
        sleep 0.1; i=$((i + 1))
    done
    return 1
}
start_jackdbus() {
    "$jackdbus" auto >>"$work/jackdbus.stderr" 2>&1 &
    JD_PID=$!
    i=0
    while ! call org.jackaudio.JackControl.IsStarted; do
        i=$((i + 1)); [ $i -gt 50 ] && return 1
        kill -0 $JD_PID 2>/dev/null || return 1
        sleep 0.1
    done
}
stop_jackdbus() {
    kill -TERM $JD_PID 2>/dev/null
    i=0; while kill -0 $JD_PID 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    kill -KILL $JD_PID 2>/dev/null; wait $JD_PID 2>/dev/null
}
result() {  # result <name> <ok:0|1> <message>
    if [ "$1" != "" ] && [ "$2" = 0 ]; then echo "PASS jackdbus-bridge-$1: $3"
    else echo "FAIL jackdbus-bridge-$1: $3"; fail=1; fi
}
field() {  # field <client name> <n>: the n-th field of that bridge in GetBridges (1: spec)
    # (fields are separated by ", "; a spec may hold a bare comma: hw:Loopback,1)
    bridges | tr '(' '\n' | grep "'$1', '" | head -1 | tr -d "[]')" | sed 's/, /\n/g' | sed -n "${2}p" | sed 's/^ *//; s/^uint[0-9]* //; s/,$//'
}

if ! start_jackdbus; then
    echo "FAIL jackdbus-bridge: jackdbus did not come up"; cat "$work/jackdbus.stderr"
    kill "$(cat "$work/bus.pid")" 2>/dev/null; rm -rf "$work"; exit 1
fi
setp engine,driver variant:string:dummy
setp engine,realtime variant:boolean:true
setp driver,rate variant:uint32:48000
setp driver,period variant:uint32:256

# 1. specs are checked: a bad one leaves the list as it was
setp engine,bridges "variant:string:hw:Loopback/12x"
v=$(getp engine bridges)
r=$(gd org.jackaudio.JackControl.AddBridge "'Loopback/0/1@never'")
if [ -z "$v" ] && echo "$r" | grep -q "not a bridge"; then
    result spec-check 0 "bad specs refused (parameter and AddBridge)"
else
    result spec-check 1 "bridges='$v', AddBridge said: $r"
fi

# 2. listed while stopped: bridged when the server starts
setp engine,bridges "variant:string:Loopback"
if call org.jackaudio.JackControl.StartServer && wait_locked Loopback 15; then
    result start 0 "Loopback bridged at start: $(field Loopback 6) Hz, latency $(field Loopback 11)/$(field Loopback 12)"
else
    result start 1 "not locked: $(bridges)"; tail -20 "$log"
fi

# 3. AddBridge while running: the card's other device, its own client name
name=$(gd org.jackaudio.JackControl.AddBridge "'hw:Loopback,1'" | sed -n "s/('\(.*\)',)/\1/p")
if [ "$name" = Loopback-1 ] && wait_locked Loopback-1 15 && [ "$(getp engine bridges)" = "Loopback hw:Loopback,1" ]; then
    result add 0 "AddBridge gave $name, locked; bridges='$(getp engine bridges)'"
else
    result add 1 "name='$name', bridges='$(getp engine bridges)': $(bridges)"
fi
r=$(gd org.jackaudio.JackControl.AddBridge "'hw:Loopback,0'")
echo "$r" | grep -q "bridged already" && result add-twice 0 "the same device twice is refused" \
    || result add-twice 1 "AddBridge of hw:Loopback,0 again said: $r"

# 4. the master's buffer size
if call org.jackaudio.JackControl.SetBufferSize uint32:512 && sleep 1 && wait_locked Loopback 15 && wait_locked Loopback-1 15; then
    result buffer-size 0 "512 frames: both locked again, latency $(field Loopback 11)/$(field Loopback 12)"
else
    result buffer-size 1 "$(bridges)"
fi

# 5. the master's sample rate (a new master at 44100 Hz). Both ends of an aloop cable run at
#    one rate while either is open: these two cannot leave 48000 Hz, so they stay there,
#    resampled to the master's rate
setp driver,rate variant:uint32:44100
if call org.jackaudio.JackControl.SwitchMaster && sleep 1 && wait_locked Loopback 15 && wait_locked Loopback-1 15 \
    && [ "$(field Loopback 6)" = 48000 ] && [ "$(field Loopback-1 6)" = 48000 ]; then
    result rate-tied 0 "44100 Hz master, cards tied at 48000 Hz: resampled, locked"
else
    result rate-tied 1 "$(bridges)"; tail -20 "$log"
fi

# 6. RemoveBridge, by client name
if call org.jackaudio.JackControl.RemoveBridge string:Loopback-1 && [ "$(getp engine bridges)" = Loopback ] \
    && ! bridges | grep -q "Loopback-1"; then
    result remove 0 "Loopback-1 removed; bridges='$(getp engine bridges)'"
else
    result remove 1 "bridges='$(getp engine bridges)': $(bridges)"
fi

# 6b. one bridge on the cable: its card follows the master's rate, both ways
setp driver,rate variant:uint32:48000
call org.jackaudio.JackControl.SwitchMaster
wait_locked Loopback 15
setp driver,rate variant:uint32:44100
if call org.jackaudio.JackControl.SwitchMaster && sleep 1 && wait_locked Loopback 15 && [ "$(field Loopback 6)" = 44100 ]; then
    result sample-rate 0 "44100 Hz master: the card at $(field Loopback 6) Hz, locked"
else
    result sample-rate 1 "$(bridges)"; tail -20 "$log"
fi
setp driver,rate variant:uint32:48000
if call org.jackaudio.JackControl.SwitchMaster && sleep 1 && wait_locked Loopback 15 && [ "$(field Loopback 6)" = 48000 ]; then
    result sample-rate-back 0 "back at 48000 Hz, locked"
else
    result sample-rate-back 1 "$(bridges)"
fi

# 7. stopped and started again
call org.jackaudio.JackControl.StopServer
if bridges | grep -q "'Loopback', '', 'stopped'" && call org.jackaudio.JackControl.StartServer && wait_locked Loopback 15; then
    result restart 0 "stopped: listed as stopped; started: bridged again"
else
    result restart 1 "$(bridges)"
fi

# 8. the list emptied while running: removed at once
setp engine,bridges "variant:string:"
sleep 0.5
if ! bridges | grep -q "Loopback" && [ -z "$(getp engine bridges)" ]; then
    result clear 0 "emptied: no bridge left"
else
    result clear 1 "$(bridges)"
fi

# 9. kept with the settings: jackdbus started again finds the list
setp engine,bridges "variant:string:Loopback/128/3"
call org.jackaudio.JackControl.StopServer
sleep 3     # jackdbus saves its settings two seconds after a change
stop_jackdbus
if start_jackdbus && [ "$(getp engine bridges)" = "Loopback/128/3" ] && call org.jackaudio.JackControl.StartServer \
    && wait_locked Loopback 15 && [ "$(field Loopback 8)" = 3 ]; then
    result saved 0 "bridges='Loopback/128/3' kept; bridged at start with 3 periods"
else
    result saved 1 "bridges='$(getp engine bridges)': $(bridges)"
fi
x=$(gd org.jackaudio.JackControl.GetXruns | sed -n 's/.*uint32 \([0-9]*\).*/\1/p')
echo "INFO jackdbus-bridge: master xruns $x; card xruns $(field Loopback 14), resyncs $(field Loopback 15), underruns $(field Loopback 16)"
call org.jackaudio.JackControl.StopServer
stop_jackdbus

grep -i "bridge" "$log" > "$work/bridge.log" 2>/dev/null
kill "$(cat "$work/bus.pid")" 2>/dev/null
if [ $fail = 0 ]; then
    rm -rf "$work"
else
    echo "logs: $work ($log)"
fi
exit $fail
