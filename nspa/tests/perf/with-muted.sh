#!/bin/sh
# Run a command with every playback control of a card (default PCH, the laptop's speakers) at 0
# and muted for the whole run, as nspa/tests/hw/run-hw.sh does, then restore the mixer. A
# watchdog re-mutes on every mixer event of the card (and polls every 250 ms): PipeWire or a
# server opening the card can apply its own volumes. The live server is left running.
#   nspa/tests/perf/with-muted.sh [-c card] <command> [args...]
set -u
card=${NSPA_MUTE_CARD:-PCH}
if [ "${1:-}" = -c ]; then card=$2; shift 2; fi

# the amixer -s lines that zero and mute every playback control
mute_commands() {
    amixer -c "$card" scontrols | sed -n "s/^Simple mixer control '\(.*\)',\([0-9]*\)$/\1,\2/p" |
    while IFS= read -r ctl; do
        caps=$(amixer -c "$card" sget "$ctl" | sed -n 's/^ *Capabilities: //p')
        name=${ctl%,*} idx=${ctl##*,}
        case " $caps " in *" pvolume"*) echo "sset '$name',$idx 0%" ;; esac
        case " $caps " in *" pswitch"*) echo "sset '$name',$idx mute" ;; esac
    done
}
loud() {  # the playback controls that are not at 0 and muted
    amixer -c "$card" scontents | awk '
        /^Simple mixer control / { name = $0; sub(/^Simple mixer control /, "", name); next }
        /Playback/ && !/Playback channels|Limits/ {
            l = $0; sub(/Capture.*/, "", l)
            if ((l ~ /\[on\]/ || l ~ /Playback [1-9]/) && !(name in seen)) { seen[name] = 1; printf "%s ", name }
        }'
}
mute() { printf '%s\n' "$NSPA_MUTE_COMMANDS" | amixer -q -c "$card" -s >/dev/null 2>&1; }
check() {
    l=$(loud)
    [ -z "$l" ] && return 0
    mute
    echo "$(date +%T.%N | cut -c1-12) $1: re-muted $l" >> "$NSPA_MUTE_LOG"
}

# the watchdog (this script again, in its own process group)
if [ "${NSPA_MUTE_WATCHDOG:-0}" = 1 ]; then
    alsactl monitor "hw:$card" 2>/dev/null | while read -r ev; do check event; done &
    while :; do check poll; sleep 0.25; done
fi

[ $# -gt 0 ] || { echo "usage: with-muted.sh [-c card] <command> [args...]" >&2; exit 2; }
work=$(mktemp -d /tmp/nspa-muted.XXXXXX)
NSPA_MUTE_LOG=$work/mute-watchdog.log
: > "$NSPA_MUTE_LOG"
alsactl -f "$work/mixer.state" store "$card" || { echo "cannot save the mixer of $card, not running" >&2; rm -rf "$work"; exit 1; }
NSPA_MUTE_COMMANDS=$(mute_commands)
[ -n "$NSPA_MUTE_COMMANDS" ] || { echo "no playback controls on $card, not running" >&2; rm -rf "$work"; exit 1; }
export NSPA_MUTE_LOG NSPA_MUTE_COMMANDS
mute
sleep 0.2
if [ -n "$(loud)" ]; then
    echo "cannot mute $card: $(loud)" >&2
    alsactl -f "$work/mixer.state" restore "$card"
    rm -rf "$work"
    exit 1
fi
NSPA_MUTE_WATCHDOG=1 setsid "$0" -c "$card" &
watchdog=$!
restore() {
    kill -TERM -- "-$watchdog" 2>/dev/null
    sleep 0.3
    n=$(grep -c re-muted "$NSPA_MUTE_LOG")
    [ "$n" = 0 ] || sed 's/^/  /' "$NSPA_MUTE_LOG"
    alsactl -f "$work/mixer.state" restore "$card" && echo "mixer of $card restored ($n re-mute(s) during the run)"
    rm -rf "$work"
}
trap restore EXIT
trap 'exit 130' INT TERM
echo "$card muted (every playback control at 0, watchdog running)"
"$@"
