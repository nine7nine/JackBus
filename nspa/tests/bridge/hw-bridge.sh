#!/bin/sh
# The sound card bridge on a real card, measured against the system clock (no JACK server): how
# it locks, the card's drift, the delay's error, xruns and resyncs. The bridge plays silence, and
# every playback control of the card is set to 0 and muted for the run (restored afterwards).
# The card's device reservation is held so PipeWire leaves it alone; the JACK server is not
# touched, so use a card it does not use.
#
#   [BRIDGE_MODE=capture|playback] nspa/tests/bridge/hw-bridge.sh <card id> [seconds=60] [master period=256] [card period=128] [periods=2]
set -u
card=${1:?card id, e.g. U44}; secs=${2:-60}; n=${3:-256}; cperiod=${4:-128}; periods=${5:-2}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
idx=$(readlink "/proc/asound/$card" 2>/dev/null | sed 's/^card//')
[ -n "$idx" ] || { echo "SKIP: no card $card"; exit 0; }
if jack_control status 2>/dev/null | grep -q '^started' && jack_control dp 2>/dev/null | grep -q "hw:$card\b"; then
    echo "SKIP: the running JACK server uses $card (nspa/tests/hw/with-card.sh hw:$card $0 ...)"; exit 0
fi

bin=$(mktemp /tmp/nspa-bridge-hw.XXXXXX)
g++ -std=gnu++11 -faligned-new -O2 -march=x86-64 -I"$root/common" "$root/common/JackBridgeCore.cpp" \
    "$root/common/JackBridgeStream.cpp" "$root/common/JackBridgeResampler.cpp" \
    "$root/common/JackBridgeAlsa.cpp" "$root/nspa/tests/bridge/aloop_test.cpp" \
    -lasound -lpthread -o "$bin" || exit 1

pw-reserve -n "Audio$idx" -a nspa-bridge-test -p 1000 -r >/dev/null 2>&1 &
reserver=$!
state=$(mktemp /tmp/nspa-bridge-mixer.XXXXXX)
saved=0
if [ -n "$(amixer -c "$idx" scontrols 2>/dev/null)" ]; then
    alsactl -f "$state" store "$idx" && saved=1
fi
cleanup() {
    [ "$saved" = 1 ] && alsactl -f "$state" restore "$idx" >/dev/null 2>&1
    kill "$reserver" 2>/dev/null; wait "$reserver" 2>/dev/null
    rm -f "$state" "$bin"
}
trap cleanup EXIT INT TERM
if [ "$saved" = 1 ]; then
    amixer -c "$idx" scontrols | sed -n "s/^Simple mixer control '\(.*\)',\([0-9]*\)$/\1,\2/p" | while IFS= read -r c; do
        caps=$(amixer -c "$idx" sget "$c" | sed -n 's/^ *Capabilities: //p')
        case " $caps " in *" pvolume"*) amixer -q -c "$idx" sset "$c" 0% ;; esac
        case " $caps " in *" pswitch"*) amixer -q -c "$idx" sset "$c" mute ;; esac
    done
fi
i=0
while [ -n "$(fuser /dev/snd/pcmC"$idx"D*p /dev/snd/pcmC"$idx"D*c 2>/dev/null)" ]; do
    i=$((i + 1)); [ $i -gt 50 ] && { echo "hw:$card still in use"; exit 1; }
    sleep 0.2
done
BRIDGE_MODE="${BRIDGE_MODE:-}" BRIDGE_CAPTURE="hw:$card" BRIDGE_PLAYBACK="hw:$card" BRIDGE_SILENT=1 "$bin" "$secs" 0 0 "$n" "$cperiod" "$periods"
