#!/bin/sh
# A batch of measured runs, inside nspa/run-isolated.sh (through with-muted.sh, and
# nspa/tests/bridge/with-cards.sh Loopback when a run has a bridge):
#   batch.sh <plan> <out dir>
# plan: one run per line, "<label> <build dir> <period> <dma> <bridge> [-s us] [-S] group..."
# (plan.py writes one, in a balanced order). Each run goes to <out>/<n>/; analyze.py's summary
# of each, with its label, is appended to <out>/results.jsonl.
set -u
here=$(cd "$(dirname "$0")" && pwd)
plan=$1 out=$2
mkdir -p "$out"
root=$(cd "$here/../../.." && pwd)
cc -O2 -Wall -I"$root/common" -o "$out/cycle_bench" "$here/cycle_bench.c" -L"$JACK_BUILD/common" -ljack || exit 1
export NSPA_PERF_BENCH="$out/cycle_bench"
n=0
total=$(grep -c . "$plan")
fails=0
while read -r label build rest; do
    [ -n "$label" ] || continue
    n=$((n + 1))
    d=$(printf '%s/%04d' "$out" $n)
    # shellcheck disable=SC2086
    if "$here/run-one.sh" "$build" "$d" $rest > "$d.log" 2>&1; then
        python3 -I "$here/analyze.py" "$d" --json | python3 -I -c '
import json, sys
r = json.load(sys.stdin)
r["label"] = sys.argv[1]
r["config"] = sys.argv[2]
r["qos"] = [l.split()[1] for l in open(sys.argv[3] + "/run.txt") if l.startswith("qos_")]
print(json.dumps(r))' "$label" "$rest" "$d" >> "$out/results.jsonl"
        echo "[$n/$total] $label $rest: ok"
    else
        fails=$((fails + 1))
        echo "[$n/$total] $label $rest: FAILED (see $d.log)"
    fi
done < "$plan"
echo "batch done: $n runs, $fails failed"
