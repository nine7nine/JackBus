#!/bin/sh
# jack2-nspa real-hardware tests (ALSA). Not part of nspa/tests/run.sh.
#
#   nspa/tests/hw/run-hw.sh [device=hw:PCH]
#
# If a jackdbus server is running it is stopped first (jack_control stop) and
# started again at the end. The tests run in a user namespace mapped to your
# own uid, with a private tmpfs on /dev/shm: /dev/snd access (logind ACL), the
# session D-Bus (device reservation) and your RT/memlock limits still work,
# but the test servers cannot see or touch the host's JACK shm and sockets.
# Every test runs against the fork's jackd (with the fork's libjack) and
# against /usr/bin/jackd (with /usr/lib/libjack).
#
# Environment: NSPA_BUILD_DIR (default ./build), NSPA_HW_OUT (default
# /tmp/jack2-nspa-hw), NSPA_HW_SERVERS (default "fork system").
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
dev="${1:-hw:PCH}"
out="${NSPA_HW_OUT:-/tmp/jack2-nspa-hw}"
build="${NSPA_BUILD_DIR:-$root/build}"

if [ "${NSPA_HW_INNER:-0}" != 1 ]; then
    mkdir -p "$out"
    fl="$build/common"
    cc -O2 -Wall -I"$root/common" -o "$out/stress" "$root/nspa/tests/stress.c" -L"$fl" -ljack -lpthread || exit 1
    cc -O2 -Wall -I"$root/common" -o "$out/xrun_timer" "$root/nspa/tests/xrun_timer.c" -L"$fl" -ljack -lpthread || exit 1
    cc -O2 -Wall -I"$root/common" -o "$out/capture_stale" "$here/capture_stale.c" -L"$fl" -ljack || exit 1
    cc -O2 -Wall -I"$root/common" -o "$out/hwutil" "$here/hwutil.c" -L"$fl" -ljack || exit 1
    cc -O2 -Wall -o "$out/seq_ports" "$here/seq_ports.c" -lasound || exit 1

    was_running=0
    if command -v jack_control >/dev/null && jack_control status 2>/dev/null | grep -q '^started'; then
        was_running=1
        echo "stopping the running jackdbus server (jack_control stop)"
        jack_control stop >/dev/null || { echo "jack_control stop failed" >&2; exit 1; }
    fi
    restore() {
        if [ $was_running = 1 ]; then
            echo "restarting the jackdbus server (jack_control start)"
            jack_control start >/dev/null
            sleep 1
            jack_control status | tail -1
        fi
    }
    trap restore EXIT
    trap 'exit 130' INT TERM

    uid=$(id -u) gid=$(id -g)
    # own PID namespace: every test process dies with the run
    unshare --user --map-user="$uid" --map-group="$gid" --keep-caps --mount --ipc --pid --fork --mount-proc \
        sh -c 'mount -t tmpfs -o size=1g,mode=1777 nspa-hw-shm /dev/shm && exec setpriv --inh-caps=-all --ambient-caps=-all --bounding-set=-all env NSPA_HW_INNER=1 "$@"' \
        sh "$0" "$@"
    exit $?
fi

# ---- inside the namespace ----
ulimit -c 0
export STRESS_FT_SLACK_PCT=25
unset NOTIFY_SOCKET JACK_DEFAULT_SERVER JACK_PROMISCUOUS_SERVER JACK_DRIVER_DIR LD_LIBRARY_PATH
fail=0
card="${dev#hw:}"
SERVER_PID=
trap '[ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null && stop_server' EXIT

device_free() {
    for s in /proc/asound/"$card"/pcm*/sub*/status; do
        [ -e "$s" ] || continue
        grep -q closed "$s" || return 1
    done
    return 0
}

# start_server <fork|system> <log> <jackd driver args...>; sets SERVER_PID
start_server() {
    which=$1 log=$2; shift 2
    i=0
    while ! device_free && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    if ! device_free; then echo "  $dev is busy" >&2; return 1; fi
    if [ "$which" = fork ]; then
        env LD_LIBRARY_PATH="$build/common" JACK_DRIVER_DIR="$build" \
            "$build/jackd" -R -P 89 ${SERVER_OPTS:-} -d alsa -d "$dev" "$@" >"$log" 2>&1 &
    else
        /usr/bin/jackd -R -P 89 ${SERVER_OPTS:-} -d alsa -d "$dev" "$@" >"$log" 2>&1 &
    fi
    SERVER_PID=$!
    if ! client "$which" "$out/hwutil" wait 10; then
        echo "  server did not come up, see $log" >&2
        stop_server
        return 1
    fi
}

stop_server() {
    kill -TERM "$SERVER_PID" 2>/dev/null
    i=0
    while kill -0 "$SERVER_PID" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "  server did not exit on SIGTERM, killing" >&2
        kill -KILL "$SERVER_PID" 2>/dev/null
    fi
    wait "$SERVER_PID" 2>/dev/null
}

# client <fork|system> <program> [args...]: run a client with the libjack that
# matches the server
client() {
    which=$1; shift
    env LD_LIBRARY_PATH="$(libpath "$which")" "$@"
}
libpath() { [ "$1" = fork ] && echo "$build/common"; }

alive() { kill -0 "$SERVER_PID" 2>/dev/null; }

result() {  # result <PASS|FAIL|INFO> <text>
    [ "$1" = FAIL ] && fail=1
    printf '%-4s %s\n' "$1" "$2"
}

# xruns while a command runs: xruns_during <which> <tag> <command...>
xruns_during() {
    which=$1 xtag=$2; shift 2
    # started with env (which execs), not with the client function: $! must
    # be hwutil itself, not a subshell
    env LD_LIBRARY_PATH="$(libpath "$which")" "$out/hwutil" xruns >"$out/$xtag.xruns" 2>&1 &
    mon=$!
    sleep 0.5
    "$@"
    rc=$?
    kill -TERM $mon; wait $mon
    XRUNS="$(cat "$out/$xtag.xruns")"
    return $rc
}

for which in ${NSPA_HW_SERVERS:-fork system}; do
    echo "== $which server on $dev"

    # 1. your settings: 48 kHz, 512x2, -X seq
    tag=$which-512
    if start_server "$which" "$out/$tag.server.log" -r 48000 -p 512 -n 2 -X seq; then
        res="$(client "$which" "$out/hwutil" xruns 20)"
        result INFO "$tag idle 20 s: $res"
        xruns_during "$which" "$tag-stress" client "$which" "$out/stress" 4 2000 >"$out/$tag.stress.log" 2>&1
        rc=$?
        [ $rc = 0 ] && r=PASS || r=FAIL
        result $r "$tag stress (4 clients, 2000 connect rounds): exit $rc; $XRUNS"
        client "$which" "$out/capture_stale" 20 >"$out/$tag.stale.log" 2>&1
        rc=$?
        [ $rc = 0 ] && r=PASS || r=FAIL
        result $r "$tag capture after reconnect: $(tail -1 "$out/$tag.stale.log")"

        before=$(client "$which" "$out/hwutil" midi-ports)
        "$out/seq_ports" 150 4 2>"$out/$tag.seq.log" &
        sp=$!
        sleep 2
        during=$(client "$which" "$out/hwutil" midi-ports 2>/dev/null)
        wait $sp
        sleep 1
        after=$(client "$which" "$out/hwutil" midi-ports 2>/dev/null)
        if alive; then
            result PASS "$tag -X seq burst of 150 sequencer ports: server alive; MIDI ports $before -> ${during:-?} -> ${after:-?}"
        else
            result FAIL "$tag -X seq burst of 150 sequencer ports: server died ($(tail -2 "$out/$tag.server.log" | tr '\n' ' '))"
        fi
        alive && stop_server
    fi

    # 2. -X seq with 100 sequencer ports existing at startup
    tag=$which-seqstart
    "$out/seq_ports" 100 15 2>"$out/$tag.seq.log" &
    sp=$!
    sleep 0.5
    if start_server "$which" "$out/$tag.server.log" -r 48000 -p 512 -n 2 -X seq; then
        sleep 2
        n=$(client "$which" "$out/hwutil" midi-ports 2>/dev/null)
        if alive && [ -n "$n" ]; then
            result PASS "$tag -X seq with 100 sequencer ports at startup: server alive, ${n} MIDI ports"
        else
            result FAIL "$tag -X seq with 100 sequencer ports at startup: server died ($(tail -2 "$out/$tag.server.log" | tr '\n' ' '))"
        fi
        alive && stop_server
    else
        result FAIL "$tag -X seq with 100 sequencer ports at startup: server did not start ($(tail -2 "$out/$tag.server.log" | tr '\n' ' '))"
    fi
    kill $sp 2>/dev/null; wait $sp 2>/dev/null

    # 3. low latency: 64x2 (1.3 ms)
    tag=$which-64
    if start_server "$which" "$out/$tag.server.log" -r 48000 -p 64 -n 2; then
        res="$(client "$which" "$out/hwutil" xruns 20)"
        result INFO "$tag idle 20 s: $res"
        xruns_during "$which" "$tag-stress" client "$which" "$out/stress" 4 2000 >"$out/$tag.stress.log" 2>&1
        rc=$?
        [ $rc = 0 ] && r=PASS || r=FAIL
        result $r "$tag stress (4 clients, 2000 connect rounds): exit $rc; $XRUNS"
        stop_server
    fi

    # 4. xrun recovery in sync mode
    tag=$which-sync
    SERVER_OPTS=-S
    if start_server "$which" "$out/$tag.server.log" -r 48000 -p 512 -n 2; then
        client "$which" "$out/xrun_timer" >"$out/$tag.log" 2>&1
        rc=$?
        [ $rc = 0 ] && r=PASS || r=FAIL
        result $r "$tag DLL after an ALSA xrun (-S): $(tail -1 "$out/$tag.log")"
        alive || result FAIL "$tag server died after the xrun"
        alive && stop_server
    fi
    SERVER_OPTS=
done
exit $fail
