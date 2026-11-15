#!/bin/sh
# Run a command in a private user/mount/IPC/net namespace with its own tmpfs
# on /dev/shm, so test JACK servers/clients can never touch the host's JACK
# shm registry, segments, SysV registry lock, sockets or session D-Bus.
#
# Usage: nspa/run-isolated.sh <command> [args...]
# Inside: JACK_BUILD (fork build dir), LD_LIBRARY_PATH (fork libs, unless
# NSPA_SYSTEM_LIBJACK=1), JACK_DRIVER_DIR are set.
set -eu
here="$(cd "$(dirname "$0")/.." && pwd)"

if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    exec unshare --user --map-root-user --mount --ipc --net \
        env NSPA_ISOLATED=1 "$0" "$@"
fi

mount -t tmpfs -o size=1g,mode=1777 nspa-shm /dev/shm
# A private runtime directory too: a test server's JackBus socket
# ($XDG_RUNTIME_DIR/jackbus) must never land in the user's real one, and tests
# must not reach the session's PipeWire or D-Bus sockets there
if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -d "$XDG_RUNTIME_DIR" ]; then
    mount -t tmpfs -o size=64m,mode=0700 nspa-run "$XDG_RUNTIME_DIR"
else
    XDG_RUNTIME_DIR=/dev/shm/nspa-run
    mkdir -m 0700 "$XDG_RUNTIME_DIR"
fi
export XDG_RUNTIME_DIR
ulimit -c 0  # crashes show up as exit status; no core files in the tree
unset DBUS_SESSION_BUS_ADDRESS NOTIFY_SOCKET JACK_DEFAULT_SERVER JACK_PROMISCUOUS_SERVER
export JACK_BUILD="${NSPA_BUILD_DIR:-$here/build}"
export JACK_DRIVER_DIR="$JACK_BUILD"
if [ "${NSPA_SYSTEM_LIBJACK:-0}" != 1 ]; then
    export LD_LIBRARY_PATH="$JACK_BUILD/common${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
exec "$@"
