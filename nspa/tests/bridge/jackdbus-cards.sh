#!/bin/sh
# Real cards bridged through jackdbus (the fork's, on a private bus, isolated; the cards held and
# muted by with-cards.sh): the "bridges" parameter at start, a client on each card (its first
# capture port in, silence out to its first playback port), then the master's buffer size
# changed (128, 512, back to 256) and its sample rate (a new dummy master at 44100 Hz, then
# 48000 again). After each step every bridge must lock again; the clients count xruns and show
# whether their card's signal still arrives (its connections kept). Not the live server's cards.
#   nspa/tests/bridge/jackdbus-cards.sh <card id[,card id...]>
set -u
cards=${1:?card id}
here=$(cd "$(dirname "$0")" && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$here/with-cards.sh" "$cards" "$0" "$@"
fi
work=$(mktemp -d /tmp/nspa-jackdbus-cards.XXXXXX)
export HOME="$work/home" XDG_CONFIG_HOME="$work/home/.config"
mkdir -p "$HOME"
log="$HOME/.log/jack/jackdbus.log"
fail=0
sed -n '/^<!DOCTYPE/,/^<\/busconfig>/p' "$here/jackdbus-bridge.sh" > "$work/bus.conf"
dbus-daemon --config-file="$work/bus.conf" --fork --print-address=3 --print-pid=4 3>"$work/bus.addr" 4>"$work/bus.pid" \
    || { echo "SKIP jackdbus-cards: no dbus-daemon"; exit 0; }
DBUS_SESSION_BUS_ADDRESS=$(cat "$work/bus.addr")
export DBUS_SESSION_BUS_ADDRESS
cc -O2 -Wall -I"$JACK_BUILD/.." -o "$work/client" "$here/bridge_client.c" -L"$JACK_BUILD/common" -ljack -lm || exit 1

call() { dbus-send --session --print-reply --dest=org.jackaudio.service /org/jackaudio/Controller "$@" >/dev/null 2>&1; }
gd() { gdbus call --session --dest org.jackaudio.service --object-path /org/jackaudio/Controller --method "$@" 2>&1; }
setp() { call org.jackaudio.Configure.SetParameterValue "array:string:$1" "$2"; }
bridges() { gd org.jackaudio.JackControl.GetBridges; }
field() {
    bridges | tr '(' '\n' | grep "'$1', '" | head -1 | tr -d "[]')" | sed 's/, /\n/g' | sed -n "${2}p" | sed 's/^ *//; s/^uint[0-9]* //; s/,$//'
}
wait_all_locked() {  # wait_all_locked <seconds>
    t0=$(date +%s%N)
    for c in $(echo "$cards" | tr ',' ' '); do
        i=0
        until bridges | grep -q "'$c', 'running', '\(locked\|none\)', '\(locked\|none\)'"; do
            i=$((i + 1)); [ $i -gt $(($1 * 10)) ] && return 1
            sleep 0.1
        done
    done
    waited=$(( ($(date +%s%N) - t0) / 1000000 ))
}
step() {  # step <name>: every bridge locked again
    if wait_all_locked 30; then
        rates=""
        for c in $(echo "$cards" | tr ',' ' '); do rates="$rates $c=$(field "$c" 6)Hz/$(printf '%.1f' "$(field "$c" 13)")ppm"; done
        echo "PASS jackdbus-cards-$1: all locked (${waited} ms);$rates"
    else
        echo "FAIL jackdbus-cards-$1: $(bridges)"; fail=1
    fi
}

"$JACK_BUILD/dbus/jackdbus" auto >>"$work/jackdbus.stderr" 2>&1 &
jd=$!
i=0; until call org.jackaudio.JackControl.IsStarted || [ $i -gt 50 ]; do sleep 0.1; i=$((i + 1)); done
setp engine,driver variant:string:dummy
setp engine,realtime variant:boolean:true
setp driver,rate variant:uint32:48000
setp driver,period variant:uint32:256
setp engine,bridges "variant:string:$(echo "$cards" | tr ',' ' ')"
call org.jackaudio.JackControl.StartServer || { echo "FAIL jackdbus-cards: StartServer"; tail -20 "$log"; fail=1; }
step start

secs=75
pids=""
for c in $(echo "$cards" | tr ',' ' '); do
    JACK_CLIENT_NAME_SUFFIX="$c" "$work/client" "$c:capture_1" "$c:playback_1" $secs > "$work/client-$c.log" 2>&1 &
    pids="$pids $!"
done
sleep 5
call org.jackaudio.JackControl.SetBufferSize uint32:128; sleep 1; step buffer-128
call org.jackaudio.JackControl.SetBufferSize uint32:512; sleep 1; step buffer-512
call org.jackaudio.JackControl.SetBufferSize uint32:256; sleep 1; step buffer-256
setp driver,rate variant:uint32:44100
call org.jackaudio.JackControl.SwitchMaster; sleep 1; step rate-44100
sleep 5
setp driver,rate variant:uint32:48000
call org.jackaudio.JackControl.SwitchMaster; sleep 1; step rate-48000
for p in $pids; do wait $p; done

for c in $(echo "$cards" | tr ',' ' '); do
    f="$work/client-$c.log"
    xr=$(sed -n 's/.*xruns=\([0-9]*\).*/\1/p' "$f" | sort -n | tail -1)
    silent=$(grep -c 'peak=-200' "$f")
    last=$(grep '^t=' "$f" | tail -5 | sed -n 's/.*peak=\(.*\)/\1/p' | tr '\n' ' ')
    echo "INFO jackdbus-cards-$c: client xruns ${xr:-?}, silent seconds $silent of $(grep -c '^t=' "$f"), last peaks $last"
    case "$last" in *-200.0*-200.0*-200.0*) echo "FAIL jackdbus-cards-$c: no signal at the end (connections lost?)"; fail=1 ;; esac
done
echo "INFO jackdbus-cards: master xruns $(gd org.jackaudio.JackControl.GetXruns | sed -n 's/.*uint32 \([0-9]*\).*/\1/p'); $(bridges)"
grep "bridge " "$log" | cut -c26- > "$work/bridge.log"
call org.jackaudio.JackControl.StopServer
kill -TERM $jd; wait $jd 2>/dev/null
kill "$(cat "$work/bus.pid")" 2>/dev/null
cat "$work/bridge.log"
if [ $fail = 0 ]; then rm -rf "$work"; else echo "logs: $work"; fi
exit $fail
