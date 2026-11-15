#!/usr/bin/env python3
"""Per-cycle user-space counters of a run (run-one.sh with NSPA_PERF_STAT): the server's
threads and the bench's clients, summed, divided by the cycles the counting lasted.

  perfstat.py <run dir> [--json]

perf stat -x, --per-thread lines: <comm>-<tid>,<count>,<unit>,<event>,...
"""
import json
import sys
from collections import defaultdict


def parse(run):
    hdr = {}
    for line in open(run + '/run.txt'):
        w = line.split()
        if w and w[0] == 'period':
            hdr = dict(zip(w[0::2], w[1::2]))
    period = int(hdr.get('period', 0))
    sums = defaultdict(lambda: defaultdict(float))
    for line in open(run + '/perfstat.csv'):
        if not line.strip() or line.startswith('#'):
            continue
        f = line.strip().split(',')
        if len(f) < 4 or not f[1].replace('.', '').isdigit():
            continue
        comm = f[0].rsplit('-', 1)[0]
        group = 'clients' if comm.startswith('cycle_bench') else 'server'   # only those are counted
        sums[group][f[3]] += float(f[1])
    return period, sums


def main():
    run = sys.argv[1]
    period, sums = parse(run)
    import os
    secs = float(os.environ.get('NSPA_PERF_SECS', '5'))
    cycles = secs * 48000 / period if period else 1
    res = {g: {e: v / cycles for e, v in ev.items()} for g, ev in sums.items()}
    if '--json' in sys.argv:
        print(json.dumps(res))
    else:
        for g, ev in sorted(res.items()):
            print(g, ' '.join('%s=%.1f' % (e, v) for e, v in sorted(ev.items())))


if __name__ == '__main__':
    main()
