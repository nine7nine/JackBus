#!/bin/sh
# Run a test with sound cards held for it, isolated (nspa/run-isolated.sh): each card's device
# reservation is held (org.freedesktop.ReserveDevice1.Audio<n>, so the desktop's sound server
# leaves it alone), and every playback control of each card is at 0 and muted for the run.
# A card unplugged and plugged in again meanwhile is muted again (its volumes come back with
# it) and reserved again if its index changed. Afterwards the mixers are restored (by card id)
# and the reservations released. Not for the live server's card.
#   nspa/tests/bridge/with-cards.sh <card id[,card id...]> <command> [args...]
set -u
cards=${1:?card id}; shift
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
work=$(mktemp -d /tmp/nspa-bridge-mixer.XXXXXX)
watcher=""
# whatever happens (a card missing half way included): mixers restored, reservations released
trap '[ -n "$watcher" ] && kill $watcher 2>/dev/null; for f in "$work"/*.state; do [ -f "$f" ] && alsactl -f "$f" restore "$(basename "$f" .state)" >/dev/null 2>&1; done; [ -f "$work/reserve.pids" ] && kill $(cat "$work/reserve.pids") 2>/dev/null; rm -rf "$work"' EXIT INT TERM
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
sleep 1
"$root/nspa/run-isolated.sh" "$@"
