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
# Every playback output of the card is set to 0 and muted for the whole run
# and restored afterwards. PipeWire takes the card whenever JACK releases it
# and re-applies its own volumes, so a watchdog re-mutes on every mixer change
# (and polls), and each test server checks before and after it starts.
# NSPA_HW_MUTE_SELFTEST=1 checks only the watchdog (no JACK).
#
# Environment: NSPA_BUILD_DIR (default ./build), NSPA_HW_OUT (default
# /tmp/jack2-nspa-hw), NSPA_HW_SERVERS (default "fork system"), NSPA_HW_LOW
# (low-latency configurations, default "64x2 64x3 128x2"), NSPA_HW_ONLY
# (subset of: load capture seq lowlat sync midi kill; default all).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
dev="${1:-hw:PCH}"
out="${NSPA_HW_OUT:-/tmp/jack2-nspa-hw}"
build="${NSPA_BUILD_DIR:-$root/build}"
card="${dev#hw:}"; card="${card%%,*}"

# All playback outputs of the card stay at zero and muted for the whole run
# (xruns and server start/stop click). Shared by the outer script, the mute
# watchdog and the namespace part. Capture controls are left alone (the
# capture test needs the input).
playback_controls() {
    amixer -c "$card" scontrols | sed -n "s/^Simple mixer control '\(.*\)',\([0-9]*\)$/\1,\2/p" |
    while IFS= read -r ctl; do
        caps=$(amixer -c "$card" sget "$ctl" | sed -n 's/^ *Capabilities: //p')
        case " $caps " in *" pvolume"*|*" pswitch"*) echo "$ctl" ;; esac
    done
}
# PLAYBACK_CONTROLS: one control per line (names may contain spaces);
# MUTE_COMMANDS: the amixer -s lines that zero and mute them. Both exported,
# so the check and the mute are one amixer process each.
mute_commands() {
    printf '%s\n' "$PLAYBACK_CONTROLS" | while IFS= read -r ctl; do
        [ -n "$ctl" ] || continue
        caps=$(amixer -c "$card" sget "$ctl" | sed -n 's/^ *Capabilities: //p')
        name=${ctl%,*} idx=${ctl##*,}
        case " $caps " in *" pvolume"*) echo "sset '$name',$idx 0%" ;; esac
        case " $caps " in *" pswitch"*) echo "sset '$name',$idx mute" ;; esac
    done
}
mute_all() {
    printf '%s\n' "$MUTE_COMMANDS" | amixer -q -c "$card" -s >/dev/null 2>&1
}
loud_controls() {  # prints the playback controls that are not at 0 and muted
    amixer -c "$card" scontents | awk '
        BEGIN { n = split(ENVIRON["PLAYBACK_CONTROLS"], a, "\n"); for (i = 1; i <= n; i++) if (a[i] != "") want[a[i]] = 1 }
        /^Simple mixer control / { name = $0; sub(/^Simple mixer control \047/, "", name); sub(/\047,/, ",", name); next }
        /Playback/ && !/Playback channels|Limits/ {
            l = $0; sub(/Capture.*/, "", l)
            if ((l ~ /\[on\]/ || l ~ /Playback [1-9]/) && (name in want) && !(name in seen)) { seen[name] = 1; printf "[%s] ", name }
        }'
}
# ensure_muted <who>: mute if anything is audible; non-zero if that still
# fails after 10 tries 50 ms apart (PipeWire may be setting its volumes at
# the same moment)
ensure_muted() {
    l=$(loud_controls)
    [ -z "$l" ] && return 0
    tries=0
    while [ -n "$l" ] && [ $tries -lt 10 ]; do
        mute_all
        echo "$(date +%T.%N | cut -c1-12) $1: re-muted $l" >>"$out/mute-watchdog.log"
        tries=$((tries + 1))
        sleep 0.05
        l=$(loud_controls)
    done
    [ -z "$l" ]
}

# Mute watchdog (own session, killed with its process group). PipeWire takes
# the card whenever JACK releases it and restores its own route volumes when
# it opens it, i.e. it un-mutes the speakers between test servers. React to
# every control change of the card, and poll as a backstop.
if [ "${NSPA_HW_WATCHDOG:-0}" = 1 ]; then
    [ -n "${PLAYBACK_CONTROLS:-}" ] && [ -n "${MUTE_COMMANDS:-}" ] || exit 1
    alsactl monitor "hw:$card" 2>/dev/null | while read -r ev; do ensure_muted "watchdog (event)"; done &
    while :; do ensure_muted "watchdog (poll)"; sleep 0.25; done
fi

if [ "${NSPA_HW_INNER:-0}" != 1 ]; then
    mkdir -p "$out"
    fl="$build/common"
    cc -O2 -Wall -I"$root/common" -o "$out/stress" "$root/nspa/tests/stress.c" -L"$fl" -ljack -lpthread || exit 1
    cc -O2 -Wall -I"$root/common" -o "$out/xrun_timer" "$root/nspa/tests/xrun_timer.c" -L"$fl" -ljack -lpthread || exit 1
    cc -O2 -Wall -I"$root/common" -o "$out/capture_stale" "$here/capture_stale.c" -L"$fl" -ljack || exit 1
    cc -O2 -Wall -I"$root/common" -o "$out/hwutil" "$here/hwutil.c" -L"$fl" -ljack || exit 1
    cc -O2 -Wall -o "$out/seq_ports" "$here/seq_ports.c" -lasound || exit 1
    cc -O2 -Wall -I"$root/common" -o "$out/seq_loop" "$here/seq_loop.c" -L"$fl" -ljack -lasound -lpthread || exit 1
    cc -O2 -Wall -I"$root/common" -o "$out/client_kill" "$root/nspa/tests/client_kill.c" -L"$fl" -ljack || exit 1

    mixer_state="$out/mixer.state"
    alsactl -f "$mixer_state" store "$card" || { echo "cannot save the mixer state of $card, not running" >&2; exit 1; }
    PLAYBACK_CONTROLS=$(playback_controls)
    [ -n "$PLAYBACK_CONTROLS" ] || { echo "no playback controls found on $card, not running" >&2; exit 1; }
    export PLAYBACK_CONTROLS
    MUTE_COMMANDS=$(mute_commands)
    export MUTE_COMMANDS
    : >"$out/mute-watchdog.log"
    # WirePlumber holds the card between test servers; it should not persist
    # our muting as a route volume, but check
    wp_state="${XDG_STATE_HOME:-$HOME/.local/state}/wireplumber"
    wp_state_sum() { cat "$wp_state"/* 2>/dev/null | md5sum; }
    wp_before=$(wp_state_sum)
    was_running=0
    watchdog=
    restore() {
        if [ $was_running = 1 ]; then
            echo "restarting the jackdbus server (jack_control start)"
            jack_control start >/dev/null
            sleep 1
            jack_control status | tail -1
        fi
        # jackdbus holds the card again (PipeWire can't take it): stop the
        # watchdog, then restore the mixer
        if [ -n "$watchdog" ]; then
            kill -TERM -- "-$watchdog" 2>/dev/null
            sleep 0.3
        fi
        wp_after=$(wp_state_sum)
        [ "$wp_before" = "$wp_after" ] ||
            echo "WARNING: WirePlumber state in $wp_state changed during the run (it may have saved a muted route volume)"
        n=$(grep -c 're-muted' "$out/mute-watchdog.log" 2>/dev/null)
        echo "mute watchdog: ${n:-0} re-mute(s) during the run${n:+ (see $out/mute-watchdog.log)}"
        alsactl -f "$mixer_state" restore "$card" && echo "mixer state of $card restored"
    }
    trap restore EXIT
    trap 'exit 130' INT TERM
    mute_all
    # verify before anything opens the device
    loud=$(loud_controls)
    if [ -n "$loud" ]; then
        echo "could not mute the playback controls of $card: $loud" >&2
        exit 1
    fi
    echo "playback outputs of $card muted: $(printf '%s\n' "$PLAYBACK_CONTROLS" | tr '\n' ' ')"
    : >"$out/mute-watchdog.log"
    NSPA_HW_WATCHDOG=1 setsid sh "$0" "$@" </dev/null >/dev/null 2>&1 &
    watchdog=$!
    sleep 0.5
    kill -0 "$watchdog" 2>/dev/null || { echo "mute watchdog did not start" >&2; exit 1; }

    # NSPA_HW_MUTE_SELFTEST=1: check the watchdog only (no JACK, no tests).
    # Turns an S/PDIF switch on (inaudible) and times the re-mute.
    if [ "${NSPA_HW_MUTE_SELFTEST:-0}" = 1 ]; then
        ctl=$(printf '%s\n' "$PLAYBACK_CONTROLS" | grep -m1 '^IEC958')
        [ -n "$ctl" ] || { echo "selftest needs an IEC958 switch" >&2; exit 1; }
        for i in 1 2 3 4 5; do
            t0=$(date +%s%N)
            amixer -q -c "$card" sset "$ctl" unmute
            while [ -n "$(loud_controls)" ]; do :; done
            echo "selftest $i: $ctl turned on, re-muted after $(( ($(date +%s%N) - t0) / 1000000 )) ms"
            sleep 0.5
        done
        exit 0
    fi

    if command -v jack_control >/dev/null && jack_control status 2>/dev/null | grep -q '^started'; then
        was_running=1
        echo "stopping the running jackdbus server (jack_control stop)"
        jack_control stop >/dev/null || { echo "jack_control stop failed" >&2; exit 1; }
    fi

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
    # PipeWire takes the card between test servers and keeps it open while a
    # stream is connected; jackd's device reservation request makes it release
    # the card (as when jackdbus starts), so after a short wait start anyway
    i=0
    while ! device_free && [ $i -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
    device_free || echo "  $dev is open (PipeWire?); jackd will request it through the device reservation" >&2
    if ! ensure_muted "before $which server"; then
        echo "ABORT: cannot mute the playback outputs of $card" >&2; exit 1
    fi
    # STRACE_OUT: run the server under strace (the tracer must be the parent:
    # no ptrace capability in here); stop it with stop_traced_server
    if [ "$which" = fork ]; then
        env LD_LIBRARY_PATH="$build/common" JACK_DRIVER_DIR="$build" ${STRACE_OUT:+strace -f -qq -o $STRACE_OUT} \
            "$build/jackd" -R -P 89 ${SERVER_OPTS:-} -d alsa -d "$dev" "$@" >"$log" 2>&1 &
    else
        env ${STRACE_OUT:+strace -f -qq -o $STRACE_OUT} \
            /usr/bin/jackd -R -P 89 ${SERVER_OPTS:-} -d alsa -d "$dev" "$@" >"$log" 2>&1 &
    fi
    SERVER_PID=$!
    if ! client "$which" "$out/hwutil" wait 10 2>/dev/null; then
        echo "  server did not come up, see $log" >&2
        stop_server
        return 1
    fi
    if ! ensure_muted "after $which server start"; then
        echo "ABORT: cannot mute the playback outputs of $card" >&2; stop_server; exit 1
    fi
}

stop_traced_server() {  # SERVER_PID is strace: stop the traced jackd itself
    pkill -TERM -x jackd
    i=0
    while kill -0 "$SERVER_PID" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    kill -0 "$SERVER_PID" 2>/dev/null && { pkill -KILL -x jackd; kill -KILL "$SERVER_PID"; }
    wait "$SERVER_PID" 2>/dev/null
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
want() {
    [ -z "${NSPA_HW_ONLY:-}" ] && return 0
    case " $NSPA_HW_ONLY " in *" $1 "*) return 0 ;; esac
    return 1
}

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
    if { want load || want capture || want seq; } &&
       start_server "$which" "$out/$tag.server.log" -r 48000 -p 512 -n 2 -X seq; then
      if want load; then
        res="$(client "$which" "$out/hwutil" xruns 20)"
        result INFO "$tag idle 20 s: $res"
        xruns_during "$which" "$tag-stress" client "$which" "$out/stress" 4 2000 >"$out/$tag.stress.log" 2>&1
        rc=$?
        [ $rc = 0 ] && r=PASS || r=FAIL
        result $r "$tag stress (4 clients, 2000 connect rounds): exit $rc; $XRUNS"
      fi
      if want capture; then
        client "$which" "$out/capture_stale" 20 >"$out/$tag.stale.log" 2>&1
        case $? in 0) r=PASS ;; 3) r=INFO ;; *) r=FAIL ;; esac
        result $r "$tag capture buffer in the cycle a connection takes effect: $(tail -1 "$out/$tag.stale.log")"
      fi
      if want seq; then
        before=$(client "$which" "$out/hwutil" midi-ports 2>/dev/null)
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
      fi
        alive && stop_server
    fi

    # 2. -X seq with 150 sequencer ports existing at startup (the ring per
    # direction held 63 in 1.9.22)
    if want seq; then
    tag=$which-seqstart
    "$out/seq_ports" 150 15 2>"$out/$tag.seq.log" &
    sp=$!
    sleep 0.5
    if start_server "$which" "$out/$tag.server.log" -r 48000 -p 512 -n 2 -X seq; then
        sleep 2
        n=$(client "$which" "$out/hwutil" midi-ports 2>/dev/null)
        if alive && [ -n "$n" ]; then
            result PASS "$tag -X seq with 150 sequencer ports (75 per direction) at startup: server alive, ${n} MIDI ports"
        else
            result FAIL "$tag -X seq with 150 sequencer ports (75 per direction) at startup: server died ($(tail -2 "$out/$tag.server.log" | tr '\n' ' '))"
        fi
        alive && stop_server
    else
        result FAIL "$tag -X seq with 150 sequencer ports (75 per direction) at startup: server did not start ($(tail -2 "$out/$tag.server.log" | tr '\n' ' '))"
    fi
    kill $sp 2>/dev/null; wait $sp 2>/dev/null
    fi

    # 3. low latency (NSPA_HW_LOW: list of PERIODxNPERIODS), without and
    # with a PM QoS CPU latency request of 0 us (deep C-state exit latencies
    # are around 1 ms on laptops). The fork holds the request itself
    # (--cpu-dma-latency); for /usr/bin/jackd a helper process holds it.
    want lowlat &&
    for qos in off on; do
    for cfg in ${NSPA_HW_LOW:-64x2 64x3 128x2}; do
        tag=$which-$cfg
        holder=
        if [ $qos = on ]; then
            tag=$tag-pmqos0
            if [ "$which" = fork ]; then
                SERVER_OPTS="--cpu-dma-latency 0"
            else
                sh -c 'printf "\000\000\000\000"; exec sleep 3600' >/dev/cpu_dma_latency &
                holder=$!
            fi
        fi
        if start_server "$which" "$out/$tag.server.log" -r 48000 -p "${cfg%x*}" -n "${cfg#*x}"; then
            res="$(client "$which" "$out/hwutil" xruns 20)"
            result INFO "$tag idle 20 s: $res (cpu_dma_latency=$(od -An -tu4 /dev/cpu_dma_latency | tr -d ' '))"
            xruns_during "$which" "$tag-stress" client "$which" "$out/stress" 4 2000 >"$out/$tag.stress.log" 2>&1
            rc=$?
            # device xruns break the stress timing checks: compare, don't gate
            if alive; then
                result INFO "$tag stress (4 clients, 2000 connect rounds): exit $rc; $XRUNS; $(tail -1 "$out/$tag.stress.log")"
                stop_server
            else
                result FAIL "$tag stress: server died ($(tail -2 "$out/$tag.server.log" | tr '\n' ' '))"
            fi
        fi
        SERVER_OPTS=
        [ -n "$holder" ] && { kill $holder; wait $holder 2>/dev/null; }
    done
    done

    # 4. xrun recovery in sync mode
    tag=$which-sync
    SERVER_OPTS=-S
    if want sync && start_server "$which" "$out/$tag.server.log" -r 48000 -p 512 -n 2; then
        client "$which" "$out/xrun_timer" >"$out/$tag.log" 2>&1
        rc=$?
        [ $rc = 0 ] && r=PASS || r=FAIL
        result $r "$tag DLL after an ALSA xrun (-S): $(tail -1 "$out/$tag.log")"
        alive || result FAIL "$tag server died after the xrun"
        alive && stop_server
    fi
    SERVER_OPTS=

    # 5. -X seq: MIDI timing in both directions (seq_loop), and the system
    # calls of the idle driver thread (no MIDI flowing)
    if want midi; then
    for cfg in 512x2 256x2; do
        tag=$which-midi-$cfg
        if start_server "$which" "$out/$tag.server.log" -r 48000 -p "${cfg%x*}" -n "${cfg#*x}" -X seq; then
            client "$which" "$out/seq_loop" 200 >"$out/$tag.log" 2>&1
            rc=$?
            [ $rc = 0 ] && r=PASS || r=FAIL
            result $r "$tag MIDI through -X seq: $(grep -E 'ALSA -> JACK|JACK -> ALSA|xruns during' "$out/$tag.log" | sed 's/^ *//; s/  */ /g' | tr '\n' ';')"
            alive && stop_server
        fi
    done
    tag=$which-seq-idle
    STRACE_OUT="$out/$tag.strace"
    rm -f "$STRACE_OUT"
    if start_server "$which" "$out/$tag.server.log" -r 48000 -p 512 -n 2 -X seq; then
        sleep 6
        stop_traced_server
        # driver thread: the one polling the PCM most often
        t=$(grep -E ' (ppoll|poll)\(' "$STRACE_OUT" | awk '{print $1}' | sort | uniq -c | sort -rn | head -1 | awk '{print $2}')
        n=$(awk -v t="$t" '$1 == t' "$STRACE_OUT" | grep -cE ' (ppoll|poll)\(')
        per=$(awk -v t="$t" '$1 == t' "$STRACE_OUT" | sed -E 's/^[0-9]+ +//; s/\(.*//' | sort | uniq -c | sort -rn |
              awk -v n="$n" 'n > 0 && $1 / n >= 0.05 {printf "%s %.2f, ", $2, $1 / n}')
        seqio=$(awk -v t="$t" '$1 == t' "$STRACE_OUT" | grep -c 'SNDRV_SEQ_IOCTL_GET_QUEUE_STATUS')
        result INFO "$tag driver thread syscalls per cycle (512x2, -X seq, no MIDI, $n cycles): ${per%, }; seq queue-status ioctls $(awk -v a="$seqio" -v n="$n" 'BEGIN {printf "%.2f", n ? a / n : 0}')/cycle"
    fi
    STRACE_OUT=
    fi

    # 6. a client (40+40 ports, between two others) goes away while the card
    # runs: jack_client_close, SIGKILL with 256 MB locked (slow teardown),
    # SIGKILL while a child holds its sockets
    if want kill; then
    for cfg in 512x2 256x2; do
        tag=$which-kill-$cfg
        if start_server "$which" "$out/$tag.server.log" -r 48000 -p "${cfg%x*}" -n "${cfg#*x}"; then
            for m in "close 4 0" "kill 4 256" "killfork 3 0"; do
                set -- $m
                client "$which" "$out/client_kill" "$1" "$2" 40 "$3" >"$out/$tag-$1.log" 2>&1
                line="$(grep '^total' "$out/$tag-$1.log" | sed 's/^total: //')"
                if [ "$which" = fork ]; then
                    # close: nothing; gone without closing: one xrun per kill, a couple of cycles
                    case $1 in
                        close) lim_x=0 lim_c=0 ;;
                        *) lim_x=$(( $2 * 2 )) lim_c=4 ;;
                    esac
                    x=$(sed -n 's/^total: xruns \([0-9]*\).*/\1/p' "$out/$tag-$1.log")
                    c=$(sed -n 's/.*(max \([0-9]*\) = .*/\1/p' "$out/$tag-$1.log")
                    u=$(sed -n 's/.*unreg wrong in \([0-9]*\).*/\1/p' "$out/$tag-$1.log")
                    if [ -n "$x" ] && [ "$x" -le $lim_x ] && [ "${c:-99}" -le $lim_c ] && [ "${u:-1}" = 0 ]; then r=PASS; else r=FAIL; fi
                else
                    r=INFO
                fi
                result $r "$tag $1 ($2 trials, ${3} MB): $line"
            done
            # with the fork's libjack, each kill is found through the client's
            # slot (JackAliveTable) before its next cycle
            result INFO "$tag realtime thread ended logged $(grep -c 'realtime thread ended' "$out/$tag.server.log") times, process found exiting $(grep -c 'its process is' "$out/$tag.server.log") times"
            alive || result FAIL "$tag server died"
            alive && stop_server
        fi
    done
    fi
done
exit $fail
