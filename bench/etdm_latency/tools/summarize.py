#!/usr/bin/env python3
# Copyright (c) 2026 MediaTek Inc.
# SPDX-License-Identifier: Apache-2.0
"""Summarise BENCH lines from Zephyr and Linux logs into one comparison.

A run is stable if it had laps and no late block, overrun, lost lap or marker
reinjection.  For every target (os, cpu, cores), channel count, variant and
block size, the stable run with the smallest lead is the operating point; its
loop latency is reported in frames and microseconds at 48 kHz.

  summarize.py logs/*.log            # markdown table on stdout
  summarize.py --overall logs/*.log  # lowest stable lap per target, channels, variant
  summarize.py --csv all.csv logs/*.log
"""
import argparse
import csv
import re
import sys
from collections import defaultdict

RATE = 48000
FIELD = re.compile(r'(\w+)=(\S+)')
NUM = {'cores', 'ch', 'block', 'lead', 'ms', 'laps', 'lap_min', 'lap_max', 'lost', 'injects',
       'late', 'overrun', 'proc_avg_ns', 'proc_max_ns', 'hw_gap'}


def parse(paths):
    runs = []
    for p in paths:
        with open(p, errors='replace') as f:
            for line in f:
                i = line.find('BENCH os=')
                if i < 0:
                    continue
                r = dict(FIELD.findall(line[i:]))
                for k in NUM:
                    if k in r:
                        r[k] = int(r[k])
                r['lap_avg'] = float(r.get('lap_avg', 0))
                r['load'] = float(r.get('load', '0').rstrip('%'))
                runs.append(r)
    return runs


def stable(r):
    return (r['laps'] > 0 and r['late'] == 0 and r['overrun'] == 0 and r['lost'] == 0
            and r['injects'] == 0)


def target_name(r):
    cores = r['cores']
    mem = r.get('mem')
    return (f"{r['os']} {r['cpu']}" + (f' x{cores}' if cores > 1 else '')
            + (f' {mem}' if mem else ''))


def overall(runs):
    best = {}
    for r in runs:
        if not stable(r):
            continue
        key = (r['ch'], r['var'], target_name(r))
        if key not in best or r['lap_avg'] < best[key]['lap_avg']:
            best[key] = r
    print('| ch | variant | target | lowest lap (fr) | (ms) | jitter (fr) | block | lead '
          '| proc avg (us) | load |')
    print('|---|---|---|---|---|---|---|---|---|---|')
    for key in sorted(best):
        r = best[key]
        print(f"| {key[0]} | {key[1]} | {key[2]} | {r['lap_avg']:.1f} "
              f"| {r['lap_avg'] * 1000 / RATE:.2f} | {r['lap_max'] - r['lap_min']} "
              f"| {r['block']} | {r['lead']} | {r['proc_avg_ns'] / 1000:.1f} "
              f"| {r['load']:.1f}% |")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--csv', help='also write every run to this CSV file')
    ap.add_argument('--overall', action='store_true',
                    help='lowest stable lap per target, channels and variant, over all blocks')
    args = ap.parse_args()

    runs = parse(args.logs)
    if not runs:
        print('no BENCH lines found', file=sys.stderr)
        return 1
    if args.csv:
        keys = sorted({k for r in runs for k in r})
        with open(args.csv, 'w', newline='') as f:
            w = csv.DictWriter(f, keys)
            w.writeheader()
            w.writerows(runs)

    if args.overall:
        return overall(runs)

    best = {}
    tried = defaultdict(int)
    for r in runs:
        key = (r['os'], r['cpu'], r['cores'], r.get('mem', '-'), r['ch'], r['var'], r['block'])
        tried[key] += 1
        if stable(r) and (key not in best or r['lead'] < best[key]['lead']):
            best[key] = r

    print('| target | ch | variant | block | min stable lead | lap avg (fr) | lap avg (us) '
          '| lap max (us) | proc avg (us) | proc max (us) | load | hw gap (fr) |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|')
    for key in sorted(tried, key=lambda k: (k[4], k[5], k[6], k[0], k[1], k[2], k[3])):
        os_, cpu, cores, mem, ch, var, block = key
        target = f'{os_} {cpu}' + (f' x{cores}' if cores > 1 else '') + (
            f' {mem}' if mem != '-' else '')
        r = best.get(key)
        if r is None:
            print(f'| {target} | {ch} | {var} | {block} | none stable | | | | | | | |')
            continue
        gap = r.get('hw_gap', '')
        print(f"| {target} | {ch} | {var} | {block} | {r['lead']} | {r['lap_avg']:.1f} "
              f"| {r['lap_avg'] * 1e6 / RATE:.0f} | {r['lap_max'] * 1e6 / RATE:.0f} "
              f"| {r['proc_avg_ns'] / 1000:.1f} | {r['proc_max_ns'] / 1000:.1f} "
              f"| {r['load']:.1f}% | {gap} |")
    return 0


if __name__ == '__main__':
    sys.exit(main())
