#!/usr/bin/env python3
"""Tables from batch.sh's results.jsonl: per configuration, each build's runs.

  compare.py <results.jsonl> [--metrics first,last,hop,...] [--base A] [--quantiles p50,p99]

For each configuration and metric: the median over runs of each run's quantile (p50, p99, ...),
per build, and for two builds the paired comparison: runs are paired in the order they ran
(the plan alternates AB/BA), "B<A" counts the pairs where the second build was lower.
Metrics: the 'stats' keys (first, last, hop, head_wake, head_from_fw, head_bridge_wake, driver,
to_slaves_resume, callback), 'part.<point>' for the driver's parts, and 'xruns'.
"""
import json
import statistics
import sys
from collections import defaultdict, OrderedDict


def get(r, metric, q):
    if metric == 'xruns':
        return r['client_xruns']
    if metric.startswith('part.'):
        d = r['parts'].get(metric[5:])
    else:
        d = r['stats'].get(metric)
    return d[q] / 1000.0 if d else None


def main():
    path = sys.argv[1]
    args = dict(a.split('=', 1) for a in sys.argv[2:] if '=' in a)
    metrics = args.get('--metrics', 'first,last,hop,head_wake,head_bridge_wake,to_slaves_resume').split(',')
    qs = args.get('--quantiles', 'p50,p99').split(',')
    runs = [json.loads(l) for l in open(path) if l.strip()]
    labels = list(OrderedDict.fromkeys(r['label'] for r in runs))
    if '--base' in args:
        labels.remove(args['--base'])
        labels.insert(0, args['--base'])
    by = defaultdict(lambda: defaultdict(list))
    for r in runs:
        cfg = r['config'].replace(' -s 0', '')
        by[cfg][r['label']].append(r)
    qos_bad = [r for r in runs if len(set(r.get('qos', []))) > 1]
    if qos_bad:
        print('note: the system PM QoS value changed during %d run(s)' % len(qos_bad))

    def key(cfg):
        w = cfg.split()
        dma = -1 if w[1] == 'off' else int(w[1])     # off, then the larger holds first
        return (int(w[0]), -dma if dma >= 0 else -10**9, w[2:])
    for cfg in sorted(by, key=key):
        groups = by[cfg]
        print('\n== %s   (runs: %s)' % (cfg, ', '.join('%s %d' % (l, len(groups[l])) for l in labels if l in groups)))
        head = '%-22s' % 'us' + ''.join('  %-15s' % (l + ' ' + '/'.join(qs)) for l in labels if l in groups)
        two = len([l for l in labels if l in groups]) == 2
        if two:
            head += '  pairs B<A (' + '/'.join(qs) + ')'
        print(head)
        for m in metrics + ['xruns']:
            cells = []
            vals = {}
            for l in labels:
                if l not in groups:
                    continue
                per_q = []
                for q in qs:
                    v = [get(r, m, q) for r in groups[l]]
                    v = [x for x in v if x is not None]
                    per_q.append(v)
                vals[l] = per_q
                if not per_q[0]:
                    cells.append('  %-15s' % '-')
                else:
                    cells.append('  %-15s' % '/'.join('%.2f' % statistics.median(v) if m != 'xruns' else '%d' % sum(v) for v in per_q))
            if all(not vals[l][0] for l in vals):
                continue
            line = '%-22s' % m + ''.join(cells)
            if two and m != 'xruns':
                a, b = [l for l in labels if l in groups]
                wins = []
                for qi in range(len(qs)):
                    pa, pb = vals[a][qi], vals[b][qi]
                    n = min(len(pa), len(pb))
                    wins.append('%d/%d' % (sum(1 for i in range(n) if pb[i] < pa[i]), n))
                line += '  ' + ' '.join(wins)
            print(line)


if __name__ == '__main__':
    main()
