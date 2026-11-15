#!/bin/sh
# A sound card bridge inside a JACK server (the fork's jackd, isolated: nspa/run-isolated.sh): the
# dummy driver as master, the card bridged (--bridge), a client reading its first capture port
# and writing silence to its first playback port; xruns, the ports' latencies and flags, and the
# server's log. The card's device reservation is held and every playback control of the card is
# set to 0 and muted for the run (restored afterwards). Not the live server's card.
#   nspa/tests/bridge/server-bridge.sh <card id[,card id...]> [seconds=30] [master period=256] [bridge spec suffix, e.g. /64@timer]
# (several cards: all bridged, the client on the first; the HDA card: through nspa/tests/hw/with-card.sh)
set -u
cards=${1:?card id}; secs=${2:-30}; n=${3:-256}; suffix=${4:-}
card=${cards%%,*}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    work=$(mktemp -d /tmp/nspa-bridge-mixer.XXXXXX)
    # every playback control at 0 and muted
    mute() {
        amixer -c "$1" scontrols 2>/dev/null | sed -n "s/^Simple mixer control '\(.*\)',\([0-9]*\)$/\1,\2/p" | while IFS= read -r x; do
            caps=$(amixer -c "$1" sget "$x" | sed -n 's/^ *Capabilities: //p')
            case " $caps " in *" pvolume"*) amixer -q -c "$1" sset "$x" 0% ;; esac
            case " $caps " in *" pswitch"*) amixer -q -c "$1" sset "$x" mute ;; esac
        done
    }
    # the card's device reservation, so the desktop's sound server leaves it alone
    reserve() {
        pw-reserve -n "Audio$1" -a nspa-bridge-test -p 1000 -r >/dev/null 2>&1 &
        echo $! >> "$work/reserve.pids"
    }
    index() { readlink "/proc/asound/$1" 2>/dev/null | sed 's/^card//'; }
    for c in $(echo "$cards" | tr ',' ' '); do
        idx=$(index "$c")
        [ -n "$idx" ] || { echo "SKIP: no card $c"; exit 0; }
        reserve "$idx"
        echo "$idx" > "$work/$c.idx"
        # kept by card id: an unplugged card can come back with another index
        if [ -n "$(amixer -c "$c" scontrols 2>/dev/null)" ] && alsactl -f "$work/$c.state" store "$c"; then
            mute "$c"
        fi
    done
    # A card unplugged and plugged in again during the run comes back with its usual volumes
    # (udev restores them): reserved again if its index changed, muted again (twice: the restore
    # may come after the first)
    (
        while :; do
            for c in $(echo "$cards" | tr ',' ' '); do
                idx=$(index "$c")
                if [ -z "$idx" ]; then
                    rm -f "$work/$c.present"
                elif [ ! -e "$work/$c.present" ]; then
                    touch "$work/$c.present"
                    [ "$idx" = "$(cat "$work/$c.idx")" ] || { reserve "$idx"; echo "$idx" > "$work/$c.idx"; }
                    mute "$c"; sleep 2; mute "$c"
                fi
            done
            sleep 0.2
        done
    ) &
    watcher=$!
    trap 'kill $watcher 2>/dev/null; for f in "$work"/*.state; do [ -f "$f" ] && alsactl -f "$f" restore "$(basename "$f" .state)" >/dev/null 2>&1; done; [ -f "$work/reserve.pids" ] && kill $(cat "$work/reserve.pids") 2>/dev/null; rm -rf "$work"' EXIT INT TERM
    sleep 1
    "$root/nspa/run-isolated.sh" "$0" "$@"
    exit $?
fi
work=$(mktemp -d /tmp/nspa-bridge-server.XXXXXX)
# A private session bus (nothing on it can be activated): the bridges' device reservations
# (org.freedesktop.ReserveDevice1.Audio<n>) are taken there, never on the desktop's bus (the
# outer part holds the cards there)
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
if dbus-daemon --config-file="$work/bus.conf" --fork --print-address=3 --print-pid=4 3>"$work/bus.addr" 4>"$work/bus.pid" 2>/dev/null; then
    DBUS_SESSION_BUS_ADDRESS=$(cat "$work/bus.addr")
    export DBUS_SESSION_BUS_ADDRESS
fi
reserved() {
    [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ] || return 0
    dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>/dev/null \
        | sed -n 's/.*"org.freedesktop.ReserveDevice1.\(Audio[0-9]*\)".*/\1/p' | sort | tr '\n' ' '
}
cc -O2 -Wall ${NSPA_TEST_CFLAGS:-} -I"$root/common" -o "$work/client" "$here/bridge_client.c" -L"$JACK_BUILD/common" -ljack -lm || exit 1
export JACK_DEFAULT_SERVER="nspa-bridge-$$"
jackd=$JACK_BUILD/jackd; [ -x "$jackd" ] || jackd=$JACK_BUILD/linux/jackd
bridges=""
for c in $(echo "$cards" | tr ',' ' '); do bridges="$bridges --bridge hw:$c$suffix"; done
"$jackd" -n "$JACK_DEFAULT_SERVER" -R -P 70 $bridges -d dummy -r 48000 -p "$n" > "$work/jackd.log" 2>&1 &
jd=$!
i=0; until grep -q "bridge $card:\|Cannot bridge\|No bridge" "$work/jackd.log" 2>/dev/null || [ $i -gt 50 ]; do sleep 0.1; i=$((i+1)); done
sleep 1
echo "reserved while running: $(reserved)"
if [ "${NSPA_BRIDGE_ALL_CLIENTS:-0}" = 1 ]; then
    # a client on every card (an unplug test: each card's client should keep its connections)
    pids=""
    for c in $(echo "$cards" | tr ',' ' '); do
        JACK_CLIENT_NAME_SUFFIX="$c" "$work/client" "$c:capture_1" "$c:playback_1" "$secs" > "$work/client-$c.log" 2>&1 &
        pids="$pids $!"
        sleep 0.3
    done
    for p in $pids; do wait $p; done
else
    "$work/client" "$card:capture_1" "$card:playback_1" "$secs" > "$work/client.log" 2>&1
fi
kill $jd 2>/dev/null; wait $jd 2>/dev/null
echo "reserved after: $(reserved)"
[ -f "$work/bus.pid" ] && kill "$(cat "$work/bus.pid")" 2>/dev/null
grep -i "bridge\|error\|xrun\|audio card" "$work/jackd.log" | head -30
for f in "$work"/client*.log; do echo "== $(basename "$f")"; grep -v "^t=" "$f" | head -3; tail -2 "$f"; done
echo "logs: $work"
