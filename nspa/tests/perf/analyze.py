#!/usr/bin/env python3
"""JackBus perf: one run's server cycle trace (JACK_CYCLE_TRACE) joined with the cycle bench's
records on the frame time.

  analyze.py <run dir> [--json] [--hist]

Per cycle (ns from the cycle's start: the dummy driver's wake-up, or ALSA's poll return):
  - the driver's parts, each from the mark before it (in time order: candidates reorder them);
  - first: the first bench client's callback start; last: the last one's end (critical path);
  - each client's start; hops (a client's start minus its chain predecessor's end); heads
    (a first client's start minus the start of the slaves' resume, where the freewheel driver
    wakes it), bridge readers apart.
From the clients' records in the trace (µs, the ones every libjack writes): per edge, the woken
client's AwakeAt minus the FinishedAt of its last predecessor (the one that woke it).
Distributions: n, mean, p50, p90, p99, p99.9, max.
"""
import json
import sys
from collections import defaultdict

import numpy as np

POINTS = None


def load_trace(path):
    global POINTS
    refs, cycles = {}, {}
    hdr = {}
    cur = None
    with open(path) as f:
        for line in f:
            w = line.split()
            if not w or w[0] == '#':
                continue
            k = w[0]
            if k == 'c':
                frames = int(w[1])
                t = [int(x) for x in w[5:]]
                cur = {'begin_us': int(w[2]), 'flags': int(w[3]), 'bridges': int(w[4]), 't': t, 'cl': {}}
                cycles[frames] = cur
            elif k == 't':
                vals = [None if x == '-' else int(x) for x in w[3:6]]
                cur['cl'][int(w[1])] = (int(w[2]), *vals)
            elif k == 'ref':
                refs[int(w[1])] = {'name': w[2], 'succ': [int(x) for x in w[3:]]}
            elif k == 'points':
                POINTS = w[1:]
            elif k == 'rate':
                it = iter(w)
                for a in it:
                    hdr[a] = next(it)
    return hdr, refs, cycles


def load_bench(path):
    clients, recs = {}, defaultdict(dict)
    hdr = {}
    with open(path) as f:
        for line in f:
            w = line.split()
            if not w or w[0] == '#':
                continue
            if w[0] == 'r':
                recs[int(w[1])][int(w[2])] = (int(w[3]), int(w[4]))
            elif w[0] == 'e':
                recs['edits'][len(recs['edits'])] = int(w[1])
            elif w[0] == 'client':
                d = dict(zip(w[3::2], w[4::2]))
                d['name'] = w[2]
                clients[int(w[1])] = d
            elif w[0] == 'rate':
                it = iter(w)
                for a in it:
                    hdr[a] = next(it)
    return hdr, clients, recs


def dist(v):
    if len(v) == 0:
        return None
    a = np.asarray(v, dtype=float)
    q = np.percentile(a, [50, 90, 99, 99.9])
    return {'n': int(a.size), 'mean': float(a.mean()), 'p50': float(q[0]), 'p90': float(q[1]),
            'p99': float(q[2]), 'p999': float(q[3]), 'max': float(a.max())}


def analyze(run):
    thdr, refs, cycles = load_trace(run + '/trace.txt')
    bhdr, clients, recs = load_bench(run + '/bench.txt')
    P = {p: i for i, p in enumerate(POINTS)}
    name2idx = {c['name']: i for i, c in clients.items()}
    name2ref = {r['name']: ref for ref, r in refs.items()}
    # a first client fed by anything but the master's ports: a bridge's
    bridge_reader = {i: c['src'] != '-' and not c['src'].startswith('system:') for i, c in clients.items()}
    heads = [i for i, c in clients.items() if c['pred'] == '-' and c['src'] != '-']

    edits = list(recs.pop('edits', {}).values())
    frames_all = set(cycles)
    for i in clients:
        frames_all &= set(recs[i])
    frames = sorted(frames_all)
    # the first and last cycles of the window can be partial
    frames = frames[2:-2]

    parts = defaultdict(list)
    out = defaultdict(list)
    per_client = defaultdict(list)
    for fr in frames:
        cy = cycles[fr]
        t = cy['t']
        start = t[P['wake']] or t[P['entry']]
        marks = sorted((v, p) for p, v in zip(POINTS, t) if v and p not in ('entry', 'wake'))
        prev = start
        for v, p in marks:
            parts[p].append(v - prev)
            prev = v
        rs = t[P['read_slaves']]
        fw_start = max((v for v, p in marks if v < rs), default=start)
        ent = {i: recs[i][fr][0] - start for i in clients}
        ext = {i: recs[i][fr][1] - start for i in clients}
        out['first'].append(min(ent.values()))
        out['last'].append(max(ext.values()))
        out['driver'].append((t[P['end']] or prev) - start)
        out['to_slaves_resume'].append(fw_start - start)
        for i in clients:
            per_client[clients[i]['name']].append(ent[i])
            c = clients[i]
            if c['pred'] != '-':
                p = name2idx[c['pred']]
                out['hop'].append(recs[i][fr][0] - recs[p][fr][1])
            elif c['src'] != '-':
                key = 'head_bridge' if bridge_reader[i] else 'head'
                out[key + '_wake'].append(ent[i])
                out[key + '_from_fw'].append(recs[i][fr][0] - (fw_start))
            out['callback'].append(recs[i][fr][1] - recs[i][fr][0])

    # µs edges from the trace's client records: AwakeAt(succ) - FinishedAt(waking pred)
    preds = defaultdict(list)
    for ref, r in refs.items():
        for s in r['succ']:
            if s != ref:
                preds[s].append(ref)
    edge = defaultdict(list)
    for fr in frames:
        cl = cycles[fr]['cl']
        for s, ps in preds.items():
            if s not in cl or cl[s][2] is None:
                continue
            fins = [(cl[p][3], p) for p in ps if p in cl and cl[p][3] is not None]
            if not fins:
                continue
            fin, p = max(fins)
            awake = cl[s][2]
            if awake < fin or awake - fin > 100000:
                continue    # stale (not this cycle's)
            edge['%s>%s' % (refs[p]['name'], refs[s]['name'])].append(awake - fin)

    xr = sum(int(c['xruns']) for c in clients.values())
    res = {
        'run': run, 'period': int(thdr.get('period', 0)), 'cycles': len(frames), 'client_xruns': xr,
        'unfinished': sum(1 for fr in frames if cycles[fr]['flags'] & 2),
        'parts': {p: dist(v) for p, v in parts.items()},
        'stats': {k: dist(v) for k, v in out.items()},
        'clients': {k: dist(v) for k, v in per_client.items()},
        'edges_us': {k: dist(v) for k, v in edge.items()},
    }
    if edits:
        res['stats']['edit'] = dist(edits)
    return res


def fmt(d, scale=1e-3):
    if not d:
        return '-'
    return '%6.2f %6.2f %6.2f %7.2f %7.2f' % (d['p50'] * scale, d['p90'] * scale, d['p99'] * scale,
                                            d['p999'] * scale, d['max'] * scale)


def main():
    run = sys.argv[1]
    res = analyze(run)
    if '--json' in sys.argv:
        print(json.dumps(res))
        return
    print('%s: %d cycles, %d client xruns, %d unfinished' % (run, res['cycles'], res['client_xruns'], res['unfinished']))
    print('%-22s %6s %6s %6s %7s %7s   (us)' % ('', 'p50', 'p90', 'p99', 'p99.9', 'max'))
    for p in POINTS:
        if p in res['parts']:
            print('%-22s %s' % ('part ' + p, fmt(res['parts'][p])))
    for k, v in res['stats'].items():
        print('%-22s %s' % (k, fmt(v)))
    if '--clients' in sys.argv:
        for k, v in sorted(res['clients'].items()):
            print('%-22s %s' % ('start ' + k, fmt(v)))
    for k, v in sorted(res['edges_us'].items()):
        if '--edges' in sys.argv:
            print('%-22s %s' % ('edge ' + k, fmt(v, 1)))


if __name__ == '__main__':
    main()
