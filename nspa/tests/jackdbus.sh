#!/bin/sh
# jack2-nspa: jackdbus lifecycle tests. Must run inside nspa/run-isolated.sh.
# Uses a private session bus (the namespace has its own network, so its own
# abstract sockets) and a temporary HOME, so the real D-Bus session and the
# user's ~/.config/jack and ~/.log/jack are never touched.
#
#   1. StartServer over D-Bus (dummy driver), then SIGTERM: jackdbus must stop
#      the server and exit promptly, leaving no shm segments behind.
#   2. SIGSEGV: the crash handler must write a trace to the log and the
#      process must die from the signal (core-dump action), not hang.
# Usage: jackdbus.sh <jackdbus-binary> [informational]
set -u
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    echo "refusing to run outside nspa/run-isolated.sh" >&2
    exit 2
fi
jackdbus=$1
gating=${2:-gating}
work=$(mktemp -d /tmp/nspa-jackdbus.XXXXXX)
export HOME="$work/home" XDG_CONFIG_HOME="$work/home/.config"
mkdir -p "$HOME"
log="$HOME/.log/jack/jackdbus.log"
fail=0

# Private bus WITHOUT service directories: nothing can be auto-activated (the
# standard session config would start /usr/bin/jackdbus, i.e. the installed
# build, if ours is not answering).
cat >"$work/bus.conf" <<'CONF'
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
addr_file="$work/bus.addr"
dbus-daemon --config-file="$work/bus.conf" --fork --print-address=3 --print-pid=4 3>"$addr_file" 4>"$work/bus.pid" \
    || { echo "SKIP jackdbus: no dbus-daemon"; exit 0; }
DBUS_SESSION_BUS_ADDRESS=$(cat "$addr_file")
export DBUS_SESSION_BUS_ADDRESS

call() {
    dbus-send --session --print-reply --dest=org.jackaudio.service /org/jackaudio/Controller "$@" >/dev/null 2>&1
}

start_jackdbus() {
    "$jackdbus" auto >>"$work/jackdbus.stderr" 2>&1 &
    JD_PID=$!
    i=0
    while ! call org.jackaudio.JackControl.IsStarted; do
        i=$((i + 1)); [ $i -gt 50 ] && return 1
        kill -0 $JD_PID 2>/dev/null || return 1
        sleep 0.1
    done
    return 0
}

wait_exit() {  # wait_exit <pid> <tenths>; sets st to the exit status, or "alive"
    i=0
    while kill -0 "$1" 2>/dev/null && [ $i -lt "$2" ]; do sleep 0.1; i=$((i + 1)); done
    if kill -0 "$1" 2>/dev/null; then st=alive; else wait "$1" 2>/dev/null; st=$?; fi
}

result() {  # result <name> <ok:0|1> <message>
    if [ "$2" = 0 ]; then echo "PASS $1: $3"
    elif [ "$gating" = gating ]; then echo "FAIL $1: $3"; fail=1
    else echo "INFO $1 (expected to fail on unfixed builds): $3"; fi
}

# 1. SIGTERM with a running server
if start_jackdbus; then
    call org.jackaudio.Configure.SetParameterValue array:string:engine,driver variant:string:dummy
    call org.jackaudio.Configure.SetParameterValue array:string:engine,realtime variant:boolean:true
    call org.jackaudio.Configure.SetParameterValue array:string:driver,period variant:uint32:128
    if call org.jackaudio.JackControl.StartServer; then
        segs=$(ls /dev/shm | grep -cE '^jack-[0-9]+-[0-9]+$')
        kill -TERM $JD_PID
        wait_exit $JD_PID 50
        left=$(ls /dev/shm | grep -cE '^jack-[0-9]+-[0-9]+$')
        if [ "$st" = alive ]; then
            kill -KILL $JD_PID; wait $JD_PID 2>/dev/null
            result jackdbus-sigterm 1 "still running 5 s after SIGTERM (server had $segs segments)"
        elif [ "$left" != 0 ]; then
            result jackdbus-sigterm 1 "exited ($st) but left $left shm segments"
        else
            result jackdbus-sigterm 0 "exited ($st), server stopped, segments removed"
        fi
    else
        kill -KILL $JD_PID; wait $JD_PID 2>/dev/null
        echo "FAIL jackdbus-sigterm: StartServer failed"; tail -5 "$log"; fail=1
    fi
else
    echo "FAIL jackdbus-sigterm: jackdbus did not come up"; fail=1
fi
rm -f /dev/shm/jack-* /dev/shm/jack_* 2>/dev/null

# 1b. failed SwitchMaster must leave jackdbus usable (server stopped, can start again)
if start_jackdbus; then
    call org.jackaudio.Configure.SetParameterValue array:string:engine,driver variant:string:dummy
    if call org.jackaudio.JackControl.StartServer; then
        call org.jackaudio.Configure.SetParameterValue array:string:engine,driver variant:string:alsa
        call org.jackaudio.Configure.SetParameterValue array:string:driver,device variant:string:hw:NSPA_TEST_NONEXISTENT
        if call org.jackaudio.JackControl.SwitchMaster; then
            result jackdbus-switch-fail 1 "SwitchMaster to a nonexistent device succeeded?"
        else
            started=$(dbus-send --session --print-reply --dest=org.jackaudio.service /org/jackaudio/Controller \
                org.jackaudio.JackControl.IsStarted 2>/dev/null | grep -o 'boolean [a-z]*' | awk '{print $2}')
            call org.jackaudio.Configure.SetParameterValue array:string:engine,driver variant:string:dummy
            if [ "$started" = true ]; then
                call org.jackaudio.JackControl.StopServer
            fi
            if call org.jackaudio.JackControl.StartServer; then
                result jackdbus-switch-fail 0 "failed switch stopped the server (started=$started), StartServer works again"
                call org.jackaudio.JackControl.StopServer
            else
                result jackdbus-switch-fail 1 "wedged after failed switch (started=$started, StartServer fails)"
            fi
        fi
    else
        echo "FAIL jackdbus-switch-fail: initial StartServer failed"; fail=1
    fi
    kill -TERM $JD_PID; wait_exit $JD_PID 50; [ "$st" = alive ] && kill -KILL $JD_PID
else
    echo "FAIL jackdbus-switch-fail: jackdbus did not come up"; fail=1
fi
rm -f /dev/shm/jack-* /dev/shm/jack_* 2>/dev/null
rm -rf "$HOME/.config/jack"

# 1c. patchbay under concurrent graph changes (server threads) and D-Bus calls
if start_jackdbus; then
    call org.jackaudio.Configure.SetParameterValue array:string:engine,driver variant:string:dummy
    if call org.jackaudio.JackControl.StartServer; then
        stress="${NSPA_TEST_OUT:-/tmp/jack2-nspa-tests}/stress64"
        LD_LIBRARY_PATH="$(dirname "$jackdbus")/../common" timeout 120 "$stress" 4 150 >/dev/null 2>&1 &
        spid=$!
        i=0; dbus_fail=0
        while kill -0 $spid 2>/dev/null && [ $i -lt 300 ]; do
            call org.jackaudio.JackPatchbay.GetGraph uint64:0 || dbus_fail=$((dbus_fail + 1))
            call org.jackaudio.JackPatchbay.ConnectPortsByName string:system string:capture_1 string:stress0 string:in0
            call org.jackaudio.JackPatchbay.DisconnectPortsByName string:system string:capture_1 string:stress0 string:in0
            i=$((i + 1))
        done
        wait $spid
        if ! kill -0 $JD_PID 2>/dev/null; then
            result jackdbus-patchbay 1 "jackdbus died during concurrent patchbay activity"
        elif ! call org.jackaudio.JackPatchbay.GetGraph uint64:0; then
            result jackdbus-patchbay 1 "jackdbus stopped answering after concurrent patchbay activity"
        else
            result jackdbus-patchbay 0 "$i rounds of GetGraph/Connect/Disconnect during client churn ($dbus_fail failed GetGraph)"
        fi
        call org.jackaudio.JackControl.StopServer
    else
        echo "FAIL jackdbus-patchbay: StartServer failed"; fail=1
    fi
    kill -TERM $JD_PID; wait_exit $JD_PID 50; [ "$st" = alive ] && kill -KILL $JD_PID
else
    echo "FAIL jackdbus-patchbay: jackdbus did not come up"; fail=1
fi
rm -f /dev/shm/jack-* /dev/shm/jack_* 2>/dev/null

# 1d. a session manager takes an application's clients out of the graph before
# stopping it (org.jackaudio.JackPatchbay.DetachClient / DetachProcess): killed
# afterwards, its departure costs the graph nothing, whatever its libjack (here
# without its JackAliveTable slot, JACK_NO_ALIVE_SLOT=1, so only the detach helps)
kill_bin="${NSPA_TEST_OUT:-/tmp/jack2-nspa-tests}/client_kill"
if [ ! -x "$kill_bin" ]; then
    echo "SKIP jackdbus-detach: no client_kill"
elif start_jackdbus; then
    call org.jackaudio.Configure.SetParameterValue array:string:engine,driver variant:string:dummy
    call org.jackaudio.Configure.SetParameterValue array:string:driver,period variant:uint32:256
    if call org.jackaudio.JackControl.StartServer; then
        for mode in detachkill detachpid; do
            before=$(grep -c 'taken out of the graph on request' "$log")
            if JACK_NO_ALIVE_SLOT=1 NSPA_KILL_LIMITS="0 0 1" LD_LIBRARY_PATH="$(dirname "$jackdbus")/../common" \
               timeout 120 "$kill_bin" $mode 4 40 0 >"$work/$mode.log" 2>&1; then
                rc=0
            else
                rc=1
            fi
            n=$(( $(grep -c 'taken out of the graph on request' "$log") - before ))
            [ "$n" = 4 ] || rc=1
            result jackdbus-$mode $rc "$(tail -1 "$work/$mode.log" | sed 's/^PASS: //; s/^FAIL: //'); taken out on request $n of 4"
        done
        if dbus-send --session --print-reply --dest=org.jackaudio.service /org/jackaudio/Controller \
               org.jackaudio.JackPatchbay.DetachClient string:nspa_no_such_client >/dev/null 2>&1; then
            result jackdbus-detach-unknown 1 "DetachClient of an unknown client succeeded"
        else
            result jackdbus-detach-unknown 0 "DetachClient of an unknown client is an error"
        fi
        call org.jackaudio.JackControl.StopServer
        if call org.jackaudio.JackPatchbay.DetachProcess int64:1; then
            result jackdbus-detach-stopped 1 "DetachProcess with the server stopped succeeded"
        elif ! kill -0 $JD_PID 2>/dev/null; then
            result jackdbus-detach-stopped 1 "jackdbus died"
        else
            result jackdbus-detach-stopped 0 "DetachProcess with the server stopped is an error"
        fi
    else
        echo "FAIL jackdbus-detach: StartServer failed"; fail=1
    fi
    kill -TERM $JD_PID; wait_exit $JD_PID 50; [ "$st" = alive ] && kill -KILL $JD_PID
else
    echo "FAIL jackdbus-detach: jackdbus did not come up"; fail=1
fi
rm -f /dev/shm/jack-* /dev/shm/jack_* 2>/dev/null
rm -rf "$HOME/.config/jack"

# 1e. per-client cycle statistics (org.jackaudio.JackControl.GetClientTimings,
# ResetClientTimings): late cycles are counted whether or not anyone asks, cycle
# lengths only once asked for; the late client is found and measured, the client
# waiting for it and an independent one are not counted late, jackdbus' own client
# (not in the graph) is not listed, a reset or a new activation starts afresh,
# clients gone are no longer listed, and a stopped server is an error for Get only
timings_bin="${NSPA_TEST_OUT:-/tmp/jack2-nspa-tests}/client_timings"
timings() {  # one line per client: name late_cycles max_callback_usecs
    dbus-send --session --print-reply --dest=org.jackaudio.service /org/jackaudio/Controller \
        org.jackaudio.JackControl.GetClientTimings 2>/dev/null \
      | awk '/string "/ { n = $2; gsub(/"/, "", n); k = 0; next }
             /uint32/ { v[k++] = $2; if (k == 2) print n, v[0], v[1] }'
}
field() {  # field <timings> <client> <2|3>
    printf '%s\n' "$1" | awk -v c="$2" -v f="$3" '$1 == c { print $f }'
}
run_timings() {  # run_timings <args>: starts client_timings, waits for "ready"
    rm -f "$work/tfifo"; mkfifo "$work/tfifo"
    LD_LIBRARY_PATH="$(dirname "$jackdbus")/../common" "$timings_bin" "$@" <"$work/tfifo" >"$work/timings.out" 2>&1 &
    T_PID=$!
    exec 7>"$work/tfifo"
    i=0
    while ! grep -q '^ready' "$work/timings.out" && kill -0 $T_PID 2>/dev/null && [ $i -lt 200 ]; do
        sleep 0.1; i=$((i + 1))
    done
    grep -q '^ready' "$work/timings.out"
}
stop_timings() {
    exec 7>&-
    wait_exit $T_PID 50; [ "$st" = alive ] && kill -KILL $T_PID
}
if [ ! -x "$timings_bin" ]; then
    echo "SKIP jackdbus-timings: no client_timings"
elif start_jackdbus; then
    call org.jackaudio.Configure.SetParameterValue array:string:engine,driver variant:string:dummy
    call org.jackaudio.Configure.SetParameterValue array:string:driver,period variant:uint32:256
    call org.jackaudio.Configure.SetParameterValue array:string:driver,rate variant:uint32:48000
    p_us=5333   # 256 frames at 48 kHz
    if call org.jackaudio.JackControl.StartServer; then
        # nobody has asked yet: late cycles counted, lengths not measured
        if run_timings 2 1.5; then
            t=$(timings)
            l=$(field "$t" nspa_late 2); m=$(field "$t" nspa_late 3)
            [ -n "$l" ] && [ "$l" -ge 2 ] && [ "$l" -le 4 ] && [ "$m" = 0 ] && rc=0 || rc=1
            result jackdbus-timings-unasked $rc "2 callbacks of 1.5 periods, before any request: nspa_late late $l, longest $m us"
        else
            result jackdbus-timings-unasked 1 "client_timings: $(cat "$work/timings.out")"
        fi
        stop_timings
        sleep 0.3

        if run_timings 3 1.5; then
            t=$(timings)
            l=$(field "$t" nspa_late 2); m=$(field "$t" nspa_late 3)
            a=$(field "$t" nspa_after 2); s=$(field "$t" nspa_steady 2)
            names=$(printf '%s\n' "$t" | awk '{ print $1 }' | sort | tr '\n' ' ')
            rc=0
            [ "$names" = "nspa_after nspa_late nspa_steady " ] || rc=1
            [ -n "$l" ] && [ "$l" -ge 3 ] && [ "$l" -le 6 ] || rc=1
            [ -n "$m" ] && [ "$m" -ge $((p_us * 135 / 100)) ] && [ "$m" -le $((p_us * 3)) ] || rc=1
            [ "$a" = 0 ] && [ "$s" = 0 ] || rc=1
            result jackdbus-timings $rc "3 callbacks of 1.5 periods: nspa_late late $l, longest $m us; nspa_after late $a, nspa_steady late $s; listed: $names"

            call org.jackaudio.JackControl.ResetClientTimings
            sleep 0.3
            t=$(timings)
            l=$(field "$t" nspa_late 2); m=$(field "$t" nspa_late 3)
            rc=0
            [ "$l" = 0 ] && [ -n "$m" ] && [ "$m" -lt $((p_us / 2)) ] || rc=1
            [ "$(printf '%s\n' "$t" | awk '$2 != 0' | wc -l)" = 0 ] || rc=1
            result jackdbus-timings-reset $rc "after ResetClientTimings: nspa_late late $l, longest $m us"
            stop_timings
            sleep 0.3
            t=$(timings)
            [ -z "$t" ] && rc=0 || rc=1
            result jackdbus-timings-gone $rc "listed once the clients closed: '$(printf '%s' "$t" | tr '\n' ' ')'"
        else
            result jackdbus-timings 1 "client_timings: $(cat "$work/timings.out")"
            stop_timings
        fi

        if run_timings 2 1.5 reactivate; then
            t=$(timings)
            l=$(field "$t" nspa_late 2); m=$(field "$t" nspa_late 3)
            [ "$l" = 0 ] && [ -n "$m" ] && [ "$m" -lt $((p_us / 2)) ] && rc=0 || rc=1
            result jackdbus-timings-reactivate $rc "late twice, then activated again: nspa_late late $l, longest $m us"
        else
            result jackdbus-timings-reactivate 1 "client_timings: $(cat "$work/timings.out")"
        fi
        stop_timings

        call org.jackaudio.JackControl.StopServer
        if call org.jackaudio.JackControl.GetClientTimings; then
            result jackdbus-timings-stopped 1 "GetClientTimings with the server stopped succeeded"
        elif ! call org.jackaudio.JackControl.ResetClientTimings || ! kill -0 $JD_PID 2>/dev/null; then
            result jackdbus-timings-stopped 1 "ResetClientTimings with the server stopped failed, or jackdbus died"
        else
            result jackdbus-timings-stopped 0 "GetClientTimings with the server stopped is an error, ResetClientTimings is not"
        fi
    else
        echo "FAIL jackdbus-timings: StartServer failed"; fail=1
    fi
    kill -TERM $JD_PID; wait_exit $JD_PID 50; [ "$st" = alive ] && kill -KILL $JD_PID
else
    echo "FAIL jackdbus-timings: jackdbus did not come up"; fail=1
fi
rm -f /dev/shm/jack-* /dev/shm/jack_* 2>/dev/null
rm -rf "$HOME/.config/jack"

# 1f. the server stops with clients connected, which leave as soon as they are told:
# what the server still has to say during its teardown (its own clients and ports going)
# must not be written to their closed sockets, nor to its own request channel once that
# is closed (1.9.22 logged three error lines per notification: 20 to 60 per stop with a
# patchbay connected; the build before the fix: 158 and 166 lines over these three stops).
pb_bin="${NSPA_TEST_OUT:-/tmp/jack2-nspa-tests}/patchbay_client"
if [ ! -x "$pb_bin" ]; then
    echo "SKIP jackdbus-stop-quiet: no patchbay_client"
elif start_jackdbus; then
    call org.jackaudio.Configure.SetParameterValue array:string:engine,driver variant:string:dummy
    call org.jackaudio.Configure.SetParameterValue array:string:driver,period variant:uint32:256
    before=$(grep -c 'Broken pipe\|Could not write notification\|ClientNotify fails' "$log")
    rc=0
    for round in 1 2 3; do
        if ! call org.jackaudio.JackControl.StartServer; then rc=1; break; fi
        rm -f "$work/pbfifo"; mkfifo "$work/pbfifo"; exec 7<>"$work/pbfifo"
        LD_LIBRARY_PATH="$(dirname "$jackdbus")/../common" "$pb_bin" patchbay <&7 >/dev/null 2>&1 & p1=$!
        LD_LIBRARY_PATH="$(dirname "$jackdbus")/../common" "$pb_bin" patchbayd <&7 >/dev/null 2>&1 & p2=$!
        p3=
        if [ -x "$timings_bin" ]; then  # ordinary realtime clients, which stay until killed
            LD_LIBRARY_PATH="$(dirname "$jackdbus")/../common" "$timings_bin" 0 1 <&7 >/dev/null 2>&1 & p3=$!
        fi
        sleep 1.5
        call org.jackaudio.JackControl.StopServer || rc=1
        sleep 0.3
        exec 7>&-
        kill $p1 $p2 $p3 2>/dev/null; wait $p1 $p2 $p3 2>/dev/null
    done
    n=$(( $(grep -c 'Broken pipe\|Could not write notification\|ClientNotify fails' "$log") - before ))
    [ "$n" = 0 ] || rc=1
    result jackdbus-stop-quiet $rc "3 stops with patchbay clients connected: $n notification error lines"
    kill -TERM $JD_PID; wait_exit $JD_PID 50; [ "$st" = alive ] && kill -KILL $JD_PID
else
    echo "FAIL jackdbus-stop-quiet: jackdbus did not come up"; fail=1
fi
rm -f /dev/shm/jack-* /dev/shm/jack_* 2>/dev/null
rm -rf "$HOME/.config/jack"

# 2. crash handler
if start_jackdbus; then
    kill -SEGV $JD_PID
    wait_exit $JD_PID 30
    if [ "$st" = alive ]; then
        kill -KILL $JD_PID; wait $JD_PID 2>/dev/null
        result jackdbus-crash 1 "hung after SIGSEGV"
    elif [ "$st" != 139 ] && [ "$st" != 11 ]; then
        result jackdbus-crash 1 "exit status $st, expected death by SIGSEGV (139)"
    elif ! grep -q 'end of stack trace' "$log"; then
        result jackdbus-crash 1 "no stack trace in log"
    else
        result jackdbus-crash 0 "died by SIGSEGV, trace in log ($(grep -c '' "$log") lines)"
    fi
else
    echo "FAIL jackdbus-crash: jackdbus did not come up"; fail=1
fi

[ -f "$work/bus.pid" ] && kill "$(cat "$work/bus.pid")" 2>/dev/null
[ "${NSPA_KEEP_WORK:-0}" = 1 ] && echo "work dir kept: $work" || rm -rf "$work"
exit $fail
