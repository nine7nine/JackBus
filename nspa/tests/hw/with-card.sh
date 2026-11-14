#!/bin/sh
# Run a command with an ALSA card taken from the live JACK server, then give it back.
#
#   nspa/tests/hw/with-card.sh hw:PCH command [args...]
#
# Stops a running jackdbus server (jack_control stop), holds the card's device reservation
# (org.freedesktop.ReserveDevice1.AudioN, through pw-reserve) so PipeWire/WirePlumber can't open
# it meanwhile, waits until nothing has its PCMs open, runs the command, releases the reservation
# and starts the server again if it was running. The command itself is responsible for muting
# (run-underrun.sh mutes every playback control and writes zeros only).
set -u
dev="$1"; shift
card="${dev#hw:}"; card="${card%%,*}"
[ -e "/proc/asound/$card" ] || { echo "no card $card"; exit 1; }
idx=$(readlink "/proc/asound/$card" | sed 's/^card//')

was_started=0
if jack_control status 2>/dev/null | grep -q '^started'; then
    was_started=1
    jack_control stop >/dev/null 2>&1 || { echo "jack_control stop failed"; exit 1; }
    echo "JACK server stopped (will be started again)"
fi

pw-reserve -n "Audio$idx" -a nspa-hw-test -p 1000 -r >/dev/null 2>&1 &
reserver=$!
give_back() {
    kill "$reserver" 2>/dev/null; wait "$reserver" 2>/dev/null
    if [ "$was_started" = 1 ]; then
        for i in 1 2 3; do jack_control start >/dev/null 2>&1 && break; sleep 1; done
        jack_control status 2>/dev/null | grep -q '^started' && echo "JACK server started again" || echo "WARNING: JACK server did not start again"
    fi
}
trap give_back EXIT INT TERM

i=0
while [ -n "$(fuser /dev/snd/pcmC"${idx}"D*p /dev/snd/pcmC"${idx}"D*c 2>/dev/null)" ]; do
    i=$((i + 1)); [ $i -gt 50 ] && { echo "$dev still in use after 10 s: $(fuser /dev/snd/pcmC"${idx}"D* 2>&1)"; exit 1; }
    sleep 0.2
done
"$@"
