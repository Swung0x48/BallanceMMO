#!/usr/bin/env python3
"""Per-player summary of the SessionTelemetry lines in a server log (protocol 38, step A6).

usage: telemetry_report.py SERVER_LOG [--session N]

Each line covers one member's last ~10 s.  Per (session, player): the windows, the frames, the frame interval's
median p99 / worst p999 / worst max, the frames over 33 / 50 / 100 ms per minute, the core's own work p99 (median)
and max, rewinds a second, replayed ticks per rewind, resyncs, holds, starved reports, the worst behind and the
median ping."""
import argparse
import re
import statistics
from collections import defaultdict

LINE = re.compile(
    r'Physics session (\d+): telemetry (.*) \(#(\d+), (\w+)\): tick=(\d+) ticks=(\d+) frames=(\d+) interval '
    r'p50=([\d.]+) p99=([\d.]+) p999=([\d.]+) max=([\d.]+) ms over33=(\d+) over50=(\d+) over100=(\d+) work '
    r'p99=([\d.]+) max=([\d.]+) ms rewinds=(\d+) replayed=(\d+) resyncs=(\d+) holds=(\d+) starved=(\d+) '
    r'behind=([\d.]+) ping=(-?\d+) ms')
KEYS = ('tick', 'ticks', 'frames', 'p50', 'p99', 'p999', 'max', 'over33', 'over50', 'over100', 'work_p99',
        'work_max', 'rewinds', 'replayed', 'resyncs', 'holds', 'starved', 'behind', 'ping')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--session', type=int)
    a = ap.parse_args()
    rows = defaultdict(list)
    names = {}
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = LINE.search(line)
        if not m:
            continue
        session, name, player, host = int(m.group(1)), m.group(2), int(m.group(3)), m.group(4)
        if a.session is not None and session != a.session:
            continue
        vals = dict(zip(KEYS, (float(x) for x in m.groups()[4:])))
        rows[(session, player)].append(vals)
        names[(session, player)] = '%s (%s)' % (name, host)
    if not rows:
        print('no telemetry lines')
        return
    print('%-8s %-24s %4s %7s | %6s %6s %6s | %6s %6s %6s | %6s %6s | %6s %6s | %3s %3s %4s %5s %5s' % (
        'session', 'player', 'win', 'frames', 'p99', 'p999', 'max', '>33/m', '>50/m', '>100/m', 'work99', 'wmax',
        'rew/s', 'depth', 'rsy', 'hld', 'strv', 'behnd', 'ping'))
    for key in sorted(rows):
        r = rows[key]
        ticks = sum(x['ticks'] for x in r)
        minutes = ticks / 66.0 / 60.0 or 1.0
        rewinds = sum(x['rewinds'] for x in r)
        print('%-8d %-24s %4d %7d | %6.1f %6.1f %6.1f | %6.1f %6.1f %6.1f | %6.1f %6.1f | %6.1f %6.1f | %3d %3d %4d %5.1f %5.0f' % (
            key[0], names[key][:24], len(r), sum(x['frames'] for x in r),
            statistics.median(x['p99'] for x in r), max(x['p999'] for x in r), max(x['max'] for x in r),
            sum(x['over33'] for x in r) / minutes, sum(x['over50'] for x in r) / minutes,
            sum(x['over100'] for x in r) / minutes,
            statistics.median(x['work_p99'] for x in r), max(x['work_max'] for x in r),
            rewinds / (ticks / 66.0) if ticks else 0.0, sum(x['replayed'] for x in r) / rewinds if rewinds else 0.0,
            sum(x['resyncs'] for x in r), sum(x['holds'] for x in r), sum(x['starved'] for x in r),
            max(x['behind'] for x in r), statistics.median(x['ping'] for x in r)))


if __name__ == '__main__':
    main()
