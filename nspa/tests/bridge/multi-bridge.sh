#!/bin/sh
# Several cards bridged at once against one master (nspa/tests/bridge/multi_test.cpp): no JACK
# server, no sound. A running jackdbus server is stopped and started again afterwards; each
# card's device reservation is held (PipeWire leaves them alone), every playback control of
# every card is set to 0 and muted for the run and restored afterwards, and the bridge plays
# silence.
#
#   nspa/tests/bridge/multi-bridge.sh <seconds> <master period> <card period> <periods> PCH U44 Headset ...
#   (a card as NAME@irq / NAME@timer / NAME@auto: its wake mode)
set -u
secs=$1; n=$2; cperiod=$3; periods=$4; shift 4
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
bin=$(mktemp /tmp/nspa-bridge-multi.XXXXXX)
g++ -std=gnu++11 -faligned-new -O2 -march=x86-64 -I"$root/common" "$root/common/JackBridgeCore.cpp" \
    "$root/common/JackBridgeStream.cpp" "$root/common/JackBridgeResampler.cpp" \
    "$root/common/JackBridgeAlsa.cpp" "$root/nspa/tests/bridge/multi_test.cpp" \
    -lasound -lpthread -o "$bin" || exit 1

was_started=0
if jack_control status 2>/dev/null | grep -q '^started'; then
    was_started=1
    jack_control stop >/dev/null 2>&1 || { echo "jack_control stop failed"; exit 1; }
fi
work=$(mktemp -d /tmp/nspa-bridge-multi.XXXXXX)
pids=""
devs=""
for arg in "$@"; do
    card=${arg%%@*}      # NAME[@auto|@timer|@irq]
    idx=$(readlink "/proc/asound/$card" 2>/dev/null | sed 's/^card//')
    [ -n "$idx" ] || { echo "no card $card"; continue; }
    pw-reserve -n "Audio$idx" -a nspa-bridge-test -p 1000 -r >/dev/null 2>&1 &
    pids="$pids $!"
    if [ -n "$(amixer -c "$idx" scontrols 2>/dev/null)" ] && alsactl -f "$work/$idx.state" store "$idx"; then
        amixer -c "$idx" scontrols | sed -n "s/^Simple mixer control '\(.*\)',\([0-9]*\)$/\1,\2/p" | while IFS= read -r c; do
            caps=$(amixer -c "$idx" sget "$c" | sed -n 's/^ *Capabilities: //p')
            case " $caps " in *" pvolume"*) amixer -q -c "$idx" sset "$c" 0% ;; esac
            case " $caps " in *" pswitch"*) amixer -q -c "$idx" sset "$c" mute ;; esac
        done
    fi
    devs="$devs hw:$arg"
done
cleanup() {
    for f in "$work"/*.state; do
        [ -f "$f" ] && alsactl -f "$f" restore "$(basename "$f" .state)" >/dev/null 2>&1
    done
    for p in $pids; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done
    rm -rf "$work" "$bin"
    if [ "$was_started" = 1 ]; then
        for i in 1 2 3; do jack_control start >/dev/null 2>&1 && break; sleep 1; done
        jack_control status 2>/dev/null | grep -q '^started' || echo "WARNING: JACK server did not start again"
    fi
}
trap cleanup EXIT INT TERM
sleep 1   # PipeWire lets the cards go
"$bin" "$secs" "$n" "$cperiod" "$periods" $devs
