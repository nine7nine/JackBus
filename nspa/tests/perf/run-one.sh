#!/bin/sh
# One measured run, inside nspa/run-isolated.sh: a server of the given build (dummy master,
# optional snd-aloop bridge, optional cpu-dma-latency hold) with its cycle trace on, the cycle
# bench's graph, then the server stopped (its trace is written with the bench's clients still
# in it). Leaves <out>/trace.txt, bench.txt, jackd.log, run.txt (the run's facts).
#   run-one.sh <build dir> <out dir> <period> <dma: off|usecs> <bridge> [-s spin_us] [-S] group...
# bridge: 0 none, 1 snd-aloop (hw:Loopback), or card ids (U44,Headset: each bridged, hw:<id>)
# -S: the server in sync mode. NSPA_PERF_SECS (default 5) and NSPA_PERF_WARMUP (default 2, 7
# with a bridge: it locks in about 5 s) set the bench's times. NSPA_PERF_STAT=<perf events>:
# per-thread user-space counters of the server and the bench while it records (perfstat.csv).
# NSPA_PERF_EDIT_RATE=<per second>: graph edits while the bench records (cycle_bench -e).
set -u
if [ "${NSPA_ISOLATED:-0}" != 1 ]; then
    echo "refusing to run outside nspa/run-isolated.sh" >&2
    exit 2
fi
build=$1 out=$2 period=$3 dma=$4 bridge=$5
shift 5
spin=0 sync=""
while [ $# -gt 0 ]; do
    case "$1" in
        -s) spin=$2; shift 2 ;;
        -S) sync=-S; shift ;;
        *) break ;;
    esac
done
here=$(cd "$(dirname "$0")" && pwd)
secs=${NSPA_PERF_SECS:-5}
case "$bridge" in
    0) cards="" warmup=${NSPA_PERF_WARMUP:-2} ;;
    1) cards=Loopback warmup=${NSPA_PERF_WARMUP:-7} ;;
    *) cards=$(echo "$bridge" | tr ',' ' ') warmup=${NSPA_PERF_WARMUP:-10} ;;   # USB: ~5 s to lock
esac
mkdir -p "$out"
rm -f "$out"/trace.txt "$out"/bench.txt "$out"/run.txt

export LD_LIBRARY_PATH="$build/common" JACK_DRIVER_DIR="$build" JACK_NO_AUDIO_RESERVATION=1
export JACK_DEFAULT_SERVER="perf$$"
export JACK_CYCLE_TRACE="$out/trace.txt"
# the whole run, warm-up and bench included, plus a margin
export JACK_CYCLE_TRACE_CYCLES=$(( (warmup + secs + 4) * 48000 / period ))
bench=${NSPA_PERF_BENCH:-$out/../cycle_bench}

args="-n $JACK_DEFAULT_SERVER -R -P 70 $sync ${NSPA_PERF_JACKD_ARGS:-}"
[ "$dma" = off ] || args="$args --cpu-dma-latency $dma"
for c in $cards; do args="$args --bridge hw:$c"; done
# shellcheck disable=SC2086
"$build/jackd" $args -d dummy -r 48000 -p "$period" > "$out/jackd.log" 2>&1 &
jd=$!
i=0
until "$bench" -q 2>/dev/null || [ $i -gt 100 ]; do sleep 0.05; i=$((i + 1)); done
qos() { python3 -c "import struct; print(struct.unpack('i', open('/dev/cpu_dma_latency', 'rb').read(4))[0])" 2>/dev/null || echo "?"; }
lib=$(grep -o '/[^ ]*libjackserver.so[^ ]*' "/proc/$jd/maps" 2>/dev/null | head -1)
{
    echo "build $build"
    echo "libjackserver $lib"
    echo "period $period dma $dma bridge $bridge spin $spin sync ${sync:-0}"
    echo "groups $*"
    echo "qos_before $(qos)"
} > "$out/run.txt"
case "$lib" in
    "$build"/common/*) ;;
    *) echo "FAIL: jackd loaded '$lib', not $build's libjackserver" >&2; kill -INT $jd; wait $jd; exit 1 ;;
esac

edit=""
[ -n "${NSPA_PERF_EDIT_RATE:-}" ] && edit="-e $NSPA_PERF_EDIT_RATE"
# shellcheck disable=SC2086
"$bench" -d "$secs" -w "$warmup" -s "$spin" $edit -o "$out/bench.txt" "$@" > "$out/bench.log" 2>&1 &
bp=$!
pp=""
if [ -n "${NSPA_PERF_STAT:-}" ]; then
    # user-space counters of the server and every bench process while the bench records
    # (NSPA_PERF_STAT: the events, e.g. mem_load_l3_hit_retired.xsnp_hitm:u,cycles:u)
    sleep "$warmup"
    pids="$jd,$(pgrep -d, -P $bp),$bp"
    perf stat -x, --per-thread -e "$NSPA_PERF_STAT" -p "$pids" -o "$out/perfstat.csv" -- sleep "$secs" 2>/dev/null &
    pp=$!
fi
i=0
limit=$(( (warmup + secs + 15) * 10 ))
while [ ! -s "$out/bench.txt" ] && kill -0 $bp 2>/dev/null && [ $i -lt $limit ]; do sleep 0.1; i=$((i + 1)); done
[ -n "$pp" ] && wait $pp
echo "qos_after $(qos)" >> "$out/run.txt"
kill -INT $jd
wait $jd
status=$?
i=0
while kill -0 $bp 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
kill -9 $bp 2>/dev/null
wait $bp
bstatus=$?
echo "jackd_status $status bench_status $bstatus" >> "$out/run.txt"
grep -q "xrun\|XRun" "$out/jackd.log" && echo "server_xruns $(grep -c 'XRun\|xrun' "$out/jackd.log")" >> "$out/run.txt"
if [ ! -s "$out/bench.txt" ] || [ ! -s "$out/trace.txt" ]; then
    echo "FAIL: missing bench.txt or trace.txt in $out" >&2
    tail -5 "$out/jackd.log" "$out/bench.log" >&2
    exit 1
fi
exit 0
