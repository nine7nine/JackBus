#!/bin/sh
# Why do JACK's period wakeups on an Intel HDA card come late? Records, for 20 s, the card's
# interrupts, the HDA driver's deferred period work (azx_irq_pending_work: the "IRQ timing
# workaround", a normal-priority kworker that loops on msleep(1)), and the wakeups of a
# 128-frame test stream (nspa/tests/hw/underrun_probe.c, writing zeros, every output muted).
#
#   sudo nspa/tests/hw/trace-hda-wakes.sh [card=PCH] [period=128]
#
# Needs root only for the kernel tracepoints. Everything else runs as the user who called sudo:
# with-card.sh takes the card from that user's jackdbus server and starts it again afterwards;
# run-underrun.sh mutes and restores the card's mixer. Writes, owned by that user:
#   /tmp/hda-trace/perf.data, /tmp/hda-trace/events.txt (perf script), the probe's log.
set -eu
[ "$(id -u)" = 0 ] || { echo "run it with sudo (the tracepoints need root)"; exit 1; }
user=${SUDO_USER:?run it with sudo from your user}
uid=$(id -u "$user")
here=$(cd "$(dirname "$0")" && pwd)
card=${1:-PCH}
period=${2:-128}
out=/tmp/hda-trace
rm -rf "$out"; mkdir -p "$out"; chown "$user" "$out"
irq=$(awk -v c="$card" '/snd_hda_intel/ { sub(":", "", $1); print $1; exit }' /proc/interrupts)

as_user() {
    sudo -u "$user" env HOME="$(getent passwd "$user" | cut -d: -f6)" XDG_RUNTIME_DIR="/run/user/$uid" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" PATH="$PATH" "$@"
}

# The test stream: 25 s of natural wakeups (no injected lateness), spare mode (it never stops).
as_user env UNDERRUN_OUT="$out/probe" UNDERRUN_CONFIGS="spare:2" UNDERRUN_PERIODS="$period" \
    UNDERRUN_LATE= UNDERRUN_SECONDS=25 \
    "$here/with-card.sh" "hw:$card" "$here/run-underrun.sh" "hw:$card" > "$out/run.log" 2>&1 &
runner=$!
sleep 3   # the server stopped and the probe running

# The HDA driver's verdict on every interrupt: azx_position_ok returns 1 (update the period), 0
# (defer to the worker) or -1 (drop as "too early"). Probes written to tracefs (no debug info
# needed): the entry with the stream (azx_dev) and the return value; removed at the end.
T=/sys/kernel/tracing; [ -d "$T/events" ] || T=/sys/kernel/debug/tracing
KP="$T/kprobe_events"
pcm_events=""
# simplest definitions that this kernel accepts: with the stream argument, then without
for entry in 'p:nspa/pos_in snd_hda_intel:azx_position_ok dev=%si:x64' \
             'p:nspa/pos_in azx_position_ok dev=%si:x64' \
             'p:nspa/pos_in azx_position_ok'; do
    if echo "$entry" >> "$KP" 2>>"$out/perf.log"; then
        pcm_events="-e nspa:pos_in"; break
    fi
done
for ret in 'r:nspa/pos_ret snd_hda_intel:azx_position_ok ret=$retval:s32' \
           'r:nspa/pos_ret azx_position_ok ret=$retval:s32' \
           'r:nspa/pos_ret azx_position_ok ret=%ax:s32'; do
    if echo "$ret" >> "$KP" 2>>"$out/perf.log"; then
        pcm_events="$pcm_events -e nspa:pos_ret"; break
    fi
done
cat "$T/error_log" >> "$out/perf.log" 2>/dev/null || true
echo "HDA events: ${pcm_events:-none (kprobes refused: see perf.log)}" > "$out/tracepoints.txt"

# shellcheck disable=SC2086
perf record -o "$out/perf.data" -a $pcm_events \
    -e irq:irq_handler_entry --filter "irq == ${irq:-168}" \
    -e irq:irq_handler_exit --filter "irq == ${irq:-168}" \
    -e workqueue:workqueue_queue_work \
    -e workqueue:workqueue_execute_start \
    -e workqueue:workqueue_execute_end \
    -e sched:sched_wakeup --filter 'comm == "underrun_probe"' \
    -- sleep 20 >> "$out/perf.log" 2>&1 || echo "perf record failed: see $out/perf.log"
wait "$runner" || true
echo '-:nspa/pos_in' >> "$KP" 2>/dev/null || true
echo '-:nspa/pos_ret' >> "$KP" 2>/dev/null || true

perf script -i "$out/perf.data" -F comm,tid,cpu,time,event,trace 2>/dev/null \
    | grep -E "irq_handler|azx_|underrun_probe|snd_|pcmC|nspa:pos_" > "$out/events.txt" || true
chown -R "$user" "$out"
echo "done: $(wc -l < "$out/events.txt") events in $out/events.txt; JACK: $(as_user jack_control status 2>/dev/null | tail -1)"
