#!/usr/bin/env python3
"""Write a batch plan (batch.sh): every configuration for every build, repeated, in a balanced
order. Each repetition visits the configurations in a new shuffled order; within a
configuration the builds alternate (AB, then BA at the next repetition, ...), so neither build
always runs first or under the same background.

  plan.py --builds A=dir[,B=dir] [--periods 32,64,128] [--dma off,1,0] [--bridge 0,1]
          [--topos 'chain:16;fan:8'] [--reps 2] [--seed 1] [--spin 0] [--sync]
"""
import argparse
import random

BRIDGE_GROUP = 'fan:2@Loopback:capture_>Loopback:playback_'

ap = argparse.ArgumentParser()
ap.add_argument('--builds', required=True)
ap.add_argument('--periods', default='32,64,128')
ap.add_argument('--dma', default='off,1,0')
ap.add_argument('--bridge', default='0,1', help='0 none, 1 snd-aloop, or a "+"-joined card list per value (U44+Headset)')
ap.add_argument('--topos', default='chain:16;fan:8')
ap.add_argument('--reps', type=int, default=2)
ap.add_argument('--seed', type=int, default=1)
ap.add_argument('--spin', default='0')
ap.add_argument('--sync', action='store_true')
a = ap.parse_args()
builds = [b.split('=', 1) for b in a.builds.split(',')]
configs = []
for p in a.periods.split(','):
    for d in a.dma.split(','):
        for br in a.bridge.split(','):
            for t in a.topos.split(';'):
                if br == '0':
                    bg = []
                elif br == '1':
                    bg = [BRIDGE_GROUP]
                else:   # a client reading and playing each bridged card
                    bg = ['fan:1@%s:capture_>%s:playback_' % (c, c) for c in br.split('+')]
                groups = t.split() + bg
                extra = ['-s', a.spin] + (['-S'] if a.sync else [])
                configs.append([p, d, br.replace('+', ',')] + extra + groups)
rng = random.Random(a.seed)
for r in range(a.reps):
    order = list(range(len(configs)))
    rng.shuffle(order)
    for k, ci in enumerate(order):
        bs = builds if (r + k) % 2 == 0 else builds[::-1]
        for label, d in bs:
            print(label, d, ' '.join(configs[ci]))
