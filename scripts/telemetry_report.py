#!/usr/bin/env python3
"""Per-player summary of the members' SessionTelemetry (protocol 38, step A6), from the server's journals.

usage: telemetry_report.py SERVER_JOURNAL_OR_DIR [...] [--session N]

Each record covers one member's last ~10 s (the server's CLOCK `telemetry` records, step JNL: the message's fields
as received, tenths of ms where a time).  Per (session, player): the windows, the frames, the frame interval's
median p99 / worst p999 / worst max, the frames over 33 / 50 / 100 ms per minute, the core's own work p99 (median)
and max, rewinds a second, replayed ticks per rewind, resyncs, holds, starved reports, the worst behind and the
median ping.  A journal written before step JNL has no telemetry records (its server log's verbose lines were the
only copy: .local-tools/work/JNL/old/telemetry_report_log.py reads those)."""
import argparse
import os
import pathlib
import statistics
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import session_facts  # noqa: E402

HOSTS = {1: 'retail', 2: 'headless'}
TENTHS = ('interval_p50', 'interval_p99', 'interval_p999', 'interval_max', 'work_p99', 'work_max', 'behind_max',
          'inbound_p50', 'inbound_p99', 'inbound_max')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('journals', nargs='+')
    ap.add_argument('--session', type=int)
    a = ap.parse_args()
    paths = []
    for item in a.journals:
        p = pathlib.Path(item)
        paths += sorted(p.rglob('*.bmjs')) if p.is_dir() else [p]
    rows = defaultdict(list)
    names = {}
    untyped = 0
    for path in paths:
        facts = session_facts.Facts(path)
        if not facts.recorded('telemetry'):
            untyped += 1
            continue
        session = facts.header['session']
        if a.session is not None and session != a.session:
            continue
        players = {}
        for tag, payload in session_facts.jt.records(path):
            if tag == session_facts.jt.TAG_PLAYER:
                p = session_facts.jt.JournalReader._player(session_facts.jt._Cursor(payload))
                if p['added'] and p['name']:
                    players[p['id']] = p['name']
        for r in facts.stats('telemetry'):
            v = dict(r['values'])
            for k in TENTHS:
                v[k] = v[k] / 10.0
            key = (session, v['player'])
            rows[key].append(v)
            names[key] = '%s (%s)' % (players.get(v['player'], 'p%d' % v['player']), HOSTS.get(v['host'], 'unknown'))
    if untyped:
        print('%d journal(s) written before step JNL: no telemetry recorded' % untyped)
    if not rows:
        print('no telemetry records')
        return
    print('%-8s %-24s %4s %7s | %6s %6s %6s | %6s %6s %6s | %6s %6s | %6s %6s | %3s %3s %4s %5s | %5s %5s | %5s' % (
        'session', 'player', 'win', 'frames', 'p99', 'p999', 'max', '>33/m', '>50/m', '>100/m', 'work99', 'wmax',
        'rew/s', 'depth', 'rsy', 'hld', 'strv', 'behnd', 'in50', 'in99', 'ping'))
    for key in sorted(rows):
        r = rows[key]
        ticks = sum(x['ticks'] for x in r)
        minutes = ticks / 66.0 / 60.0 or 1.0
        rewinds = sum(x['rewinds'] for x in r)
        print('%-8d %-24s %4d %7d | %6.1f %6.1f %6.1f | %6.1f %6.1f %6.1f | %6.1f %6.1f | %6.1f %6.1f | %3d %3d %4d %5.1f | %5.1f %5.1f | %5.0f' % (
            key[0], names[key][:24], len(r), sum(x['frames'] for x in r),
            statistics.median(x['interval_p99'] for x in r), max(x['interval_p999'] for x in r),
            max(x['interval_max'] for x in r),
            sum(x['over_33'] for x in r) / minutes, sum(x['over_50'] for x in r) / minutes,
            sum(x['over_100'] for x in r) / minutes,
            statistics.median(x['work_p99'] for x in r), max(x['work_max'] for x in r),
            rewinds / (ticks / 66.0) if ticks else 0.0, sum(x['replayed'] for x in r) / rewinds if rewinds else 0.0,
            sum(x['resyncs'] for x in r), sum(x['holds'] for x in r), sum(x['starved'] for x in r),
            max(x['behind_max'] for x in r), statistics.median(x['inbound_p50'] for x in r),
            statistics.median(x['inbound_p99'] for x in r), statistics.median(x['ping_ms'] for x in r)))


if __name__ == '__main__':
    main()
