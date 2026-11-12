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

eval "$(dbus-launch --sh-syntax 2>/dev/null)" 2>/dev/null || true
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    addr_file="$work/bus.addr"
    dbus-daemon --session --fork --print-address=3 --print-pid=4 3>"$addr_file" 4>"$work/bus.pid" || { echo "SKIP jackdbus: no dbus-daemon"; exit 0; }
    DBUS_SESSION_BUS_ADDRESS=$(cat "$addr_file")
fi
export DBUS_SESSION_BUS_ADDRESS

call() {
    dbus-send --session --print-reply --dest=org.jackaudio.service /org/jackaudio/Controller "$@" >/dev/null 2>&1
}

start_jackdbus() {
    "$jackdbus" auto >/dev/null 2>&1 &
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
[ -n "${DBUS_SESSION_BUS_PID:-}" ] && kill "$DBUS_SESSION_BUS_PID" 2>/dev/null
rm -rf "$work"
exit $fail
