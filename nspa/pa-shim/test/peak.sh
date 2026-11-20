#!/bin/sh
# A level meter through jackbus-pa (pavucontrol's, KDE's): a record stream
# with PA_STREAM_PEAK_DETECT at 25 Hz gets 25 values a second, each the
# largest magnitude of the signal in its 1/25 s: a tone of peak 0.2 reads
# 0.2; two (in phase) into its one port read their average, 0.2, as
# PulseAudio's downmix. The same on PulseAudio's GLib main loop
# (libpulse-mainloop-glib, pavucontrol's and Plasma's), whose timers are the
# wall clock's: the meter takes a few % of a CPU, not all of one (a timer
# given a monotonic time is due at once, the loop never sleeping). On an
# isolated JackBus (dummy driver).
#
#   make -C nspa/pa-shim && nspa/pa-shim/test/peak.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec "$tree/nspa/run-isolated.sh" "$0" "$@"
fi
L=$here/../jackbus-pa
logs=$(mktemp -d /tmp/jbpa-peak.XXXXXX)
cc -O2 -o "$logs/pa-peak" "$here/pa-peak.c" $(pkg-config --cflags --libs libpulse libpulse-mainloop-glib glib-2.0) ||
    { echo "can't build pa-peak"; exit 2; }
export PULSE_SERVER=unix:/jackbus-pa-test/no-server JACK_DEFAULT_SERVER=jbpeak JACKBUS_COMPAT_CONF=$logs/compat.conf
export JACKBUS_PA_AUTOCONNECT=0
: >"$logs/compat.conf"
fail=0
"$JACK_BUILD/jackd" -n jbpeak -R -P 70 -d dummy -r 48000 -p 256 >"$logs/jackd.log" 2>&1 &
sp=$!
i=0; while [ $i -lt 50 ] && ! ls /dev/shm/jack_jbpeak_*_0 >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
for t in tone tone2; do "$JACK_BUILD/example-clients/jackbus_tone" -n $t -f 440 -c >"$logs/$t.log" 2>&1 & done
i=0; while [ $i -lt 50 ] && [ "$(jack_lsp 2>/dev/null | grep -c '^tone2*:out$')" -lt 2 ]; do sleep 0.1; i=$((i + 1)); done

"$L" "$logs/pa-peak" 5 >"$logs/meter.out" 2>"$logs/meter.err" &
mp=$!
i=0; while [ $i -lt 100 ] && ! grep -q '^ready' "$logs/meter.out"; do sleep 0.1; i=$((i + 1)); done
port=$(jack_lsp 2>/dev/null | grep ':in_MONO$' | head -1)
jack_connect tone:out "$port"
sleep 2.2                                   # seconds 1-2: one tone
jack_connect tone2:out "$port"              # seconds 3-5: two
wait $mp
one=$(sed -n 3p "$logs/meter.out"); two=$(sed -n 6p "$logs/meter.out")
check() {  # "peaks <n> <max>"
    set -- $1
    python3 -I -c "import sys; n, v = int(sys.argv[1]), float(sys.argv[2]); sys.exit(0 if 23 <= n <= 27 and abs(v - 0.2) < 0.005 else 1)" "$2" "$3"
}
if check "$one" && check "$two"; then
    echo "PASS a peak-detect stream (25 Hz): one tone '$one', two into one port '$two'"
else
    echo "FAIL peak detect: one '$one', two '$two' ($(head -1 "$logs/meter.out"))"
    tail -5 "$logs/meter.err" | sed 's/^/    /'
    fail=1
fi

# on the GLib main loop: the values, and the CPU it takes
"$L" "$logs/pa-peak" 3 glib >"$logs/glib.out" 2>"$logs/glib.err" &
mp=$!
i=0; while [ $i -lt 100 ] && ! grep -q '^ready' "$logs/glib.out"; do sleep 0.1; i=$((i + 1)); done
port=$(jack_lsp 2>/dev/null | grep ':in_MONO$' | head -1)
jack_connect tone:out "$port"
wait $mp
last=$(grep '^peaks' "$logs/glib.out" | tail -1); cpu=$(sed -n 's/^cpu //p' "$logs/glib.out")
if check "$last" && [ -n "$cpu" ] && [ "$cpu" -lt 30 ]; then
    echo "PASS on PulseAudio's GLib main loop: '$last', $cpu % of a CPU"
else
    echo "FAIL on the GLib main loop: '$last', cpu '$cpu' %"
    tail -5 "$logs/glib.err" | sed 's/^/    /'
    fail=1
fi
kill $(jobs -p) 2>/dev/null; wait 2>/dev/null
[ $fail = 0 ] && rm -rf "$logs" || echo "logs: $logs"
exit $fail
