#!/usr/bin/env python3
"""The bridges' trouble during the measured runs of a batch, per build and card.

  bridge_glitches.py <batch out dir>

A bridge logs its card xruns, capture underruns and playback frames missed (played as silence)
the first time any happens, then at most once a minute: a line before the server's stop signal
means something went wrong while the run was measured (the stop itself makes the card thread
miss frames: those lines come after the signal and are not counted). Also counts the bridges'
state changes before the stop besides the first lock (placed again, lost, back).
"""
import os
import re
import sys
from collections import defaultdict

out = sys.argv[1]
plan = [l.split() for l in open(os.path.join(out, 'plan.txt')) if l.strip()]
glitch = re.compile(r'^bridge (\S+): (\d+) card xruns?, (\d+) capture underruns?, (\d+) playback frames missed')
runs = defaultdict(int)
bad_runs = defaultdict(int)
totals = defaultdict(lambda: [0, 0, 0])
other = defaultdict(int)
for n, words in enumerate(plan, 1):
    d = os.path.join(out, '%04d' % n)
    log = os.path.join(d, 'jackd.log')
    if not os.path.exists(os.path.join(d, 'bench.txt')) or not os.path.exists(log):
        continue
    label = words[0]
    runs[label] += 1
    hit = False
    locked = defaultdict(int)
    for line in open(log, errors='replace'):
        if 'caught signal' in line:
            break
        m = glitch.match(line)
        if m:
            card = m.group(1)
            t = totals[(label, card)]
            for i in range(3):
                t[i] += int(m.group(2 + i))
            hit = True
            continue
        m = re.match(r'^bridge (\S+): (.*)', line)
        if m:
            card, what = m.groups()
            if what.startswith('locked'):
                locked[card] += 1
                if locked[card] > 1:
                    other[(label, card, 'locked again')] += 1
            elif not (what.startswith('hw:') or 'as measured' in what or 'Hz' in what):
                other[(label, card, what.split(',')[0][:40])] += 1
    if hit:
        bad_runs[label] += 1

for label in sorted(runs):
    print('%s: %d runs, %d with bridge trouble while measured' % (label, runs[label], bad_runs[label]))
    for (l, card), (x, u, m) in sorted(totals.items()):
        if l == label:
            print('  %-10s card xruns %d, capture underruns %d, playback frames missed %d' % (card, x, u, m))
    for (l, card, what), c in sorted(other.items()):
        if l == label:
            print('  %-10s %s: %d' % (card, what, c))
