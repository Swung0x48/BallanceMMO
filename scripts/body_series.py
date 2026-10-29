#!/usr/bin/env python3
"""One body's position, server against clients, tick by tick.

The question world agreement's checks ask of a journal set (world-agreement
design 7.3 H1/H3/J1, step M0): does this body - a mechanism such as
P_Modul_01_Pusher, or a player's ball keyed by its owner - sit where the
server has it in every client's own world, and if not, from when, how far
and for how long?  first_divergence answers "which body first"; this answers
"how does that one body's error run".

The server journal's FULL checkpoints and each client's LOCAL checkpoints are
read (first_divergence.worlds, so the keys are the same: a map body by its
name, a ball by its owner).  The error is the Euclidean distance between the
server's and the client's position at a tick both checkpointed.

usage:
  python scripts/body_series.py SERVER.bmjr CLIENT.bmjr [CLIENT.bmjr ...] --body NAME
         [--ticks 620,624,632] [--from T] [--to T] [--changes] [--summary]
  python scripts/body_series.py --selftest

  --body NAME     a map body whose name starts with NAME (every match is its
                  own series), or ball:OWNER for the ball of player OWNER (its
                  own ball in the owner's journal, its mirror in the others)
  --ticks LIST    the table at these ticks only
  --from/--to     the table over [from, to] (every tick the server checkpointed)
  --changes       only the rows where some client's band changes
                  (0, < 1 mm, < 5 cm, >= 5 cm): the shape of a long run
  --summary       per client and body: max error and its tick, the first and
                  last tick with an error, the error ticks counted, and the
                  first exact tick after the last error

A cell reads 0 when the positions are bit-equal, '-' when that journal has no
checkpoint at the tick, 'absent' when it has one without the body.

The numbers of world-agreement design 1.1.3 (lp1_join, the Pusher woken by
client 2's ball first) come out of
  body_series.py maps/arena-test/lp1_join/server.bmjr \\
      maps/arena-test/lp1_join/sector1/client{0,1,2}.bmjr \\
      --body P_Modul_01_Pusher --ticks 620,624,632,636,660,688
"""
import argparse
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

BANDS = (0.0, 1e-3, 0.05)   # exact, < 1 mm, < 5 cm, beyond


def error(a, b):
    """Euclidean distance between two positions (three floats)."""
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


def band(e):
    if e is None:
        return None
    if e == 0.0:
        return 0
    return 1 if e < BANDS[1] else (2 if e < BANDS[2] else 3)


def select_keys(server, name):
    """The keys of `server` (tick -> {key: pos}) that NAME selects, in order."""
    keys = set()
    for row in server.values():
        for k in row:
            if name.startswith('ball:'):
                if k[0] == 'ball' and str(k[1]) == name[5:]:
                    keys.add(k)
            elif k[0] == 'body' and str(k[1]).startswith(name):
                keys.add(k)
    return sorted(keys, key=lambda k: (k[0], str(k[1])))


def cell(server, client, tick, key):
    """(error or None, why) of one client at one tick: why is '' for an error,
    '-' for no checkpoint at the tick, 'absent' for a checkpoint without the
    body; None when the server has no row for the body there."""
    srow = server.get(tick)
    if srow is None or key not in srow:
        return None, None
    crow = client.get(tick)
    if crow is None:
        return None, '-'
    if key not in crow:
        return None, 'absent'
    return error(srow[key], crow[key]), ''


def series_ticks(server, key, ticks=None, tfrom=None, tto=None):
    if ticks:
        return [t for t in ticks if t in server and key in server[t]]
    return [t for t in sorted(server) if key in server[t]
            and (tfrom is None or t >= tfrom) and (tto is None or t <= tto)]


def table(server, clients, key, ticks, changes=False):
    """Rows (tick, server position, [cell text per client])."""
    rows, last = [], None
    for t in ticks:
        cells, bands = [], []
        for c in clients:
            e, why = cell(server, c, t, key)
            if why is None:
                continue
            cells.append(why if why else ('0' if e == 0.0 else '%.4f' % e))
            bands.append(band(e) if not why else why)
        if changes and bands == last:
            continue
        last = bands
        rows.append((t, server[t][key], cells))
    return rows


def summary(server, client, key, ticks):
    """max error, its tick, the first and last tick with an error, how many
    ticks had one, and the first exact tick after the last error."""
    out = {'max': 0.0, 'max_tick': None, 'first': None, 'last': None, 'count': 0, 'exact_again': None,
           'compared': 0}
    for t in ticks:
        e, why = cell(server, client, t, key)
        if why != '':
            continue
        out['compared'] += 1
        if e > 0.0:
            out['count'] += 1
            if out['first'] is None:
                out['first'] = t
            out['last'] = t
            out['exact_again'] = None
            if e > out['max']:
                out['max'], out['max_tick'] = e, t
        elif out['last'] is not None and out['exact_again'] is None:
            out['exact_again'] = t
    return out


def fmt_pos(p):
    return '(%.3f,%.3f,%.3f)' % tuple(p)


def run(args):
    import journal_trace as jt
    import first_divergence as fd
    server = fd.worlds(jt.read_journal(args.server), want_local=False)
    names = [pathlib.Path(p).stem for p in args.clients]
    clients = [fd.worlds(jt.read_journal(p), want_local=True) for p in args.clients]
    ticks = [int(x) for x in args.ticks.split(',') if x.strip()] if args.ticks else None
    keys = select_keys(server, args.body)
    if not keys:
        print('no body in the server journal matches %r' % args.body)
        return 1
    for key in keys:
        span = series_ticks(server, key, ticks, args.tfrom, args.tto)
        print('== %s %s (%d server checkpoints)' % (key[0], key[1], len(span)))
        print('tick\tserver\t' + '\t'.join(names))
        for t, pos, cells in table(server, clients, key, span, args.changes):
            print('%d\t%s\t%s' % (t, fmt_pos(pos), '\t'.join(cells)))
        if args.summary:
            for n, c in zip(names, clients):
                s = summary(server, c, key, span)
                if not s['count']:
                    print('  %s: exact at all %d compared ticks' % (n, s['compared']))
                    continue
                print('  %s: max %.4f m at %d; error from %d to %d (%d of %d compared ticks); exact again at %s'
                      % (n, s['max'], s['max_tick'], s['first'], s['last'], s['count'], s['compared'],
                         s['exact_again'] if s['exact_again'] is not None else 'never'))
    return 0


def selftest():
    failures = []

    def check(what, ok, detail=''):
        print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, (' - ' + detail) if detail else ''))
        if not ok:
            failures.append(what)

    P = ('body', 'P_Modul_01_Pusher')
    B = ('ball', 7)
    server = {620: {P: (1.0, 2.0, 3.0), B: (0.0, 0.0, 0.0)},
              624: {P: (1.0, 1.91, 3.0), B: (1.0, 0.0, 0.0)},
              632: {P: (1.0, 1.9, 3.0)},
              636: {P: (1.0, 1.9, 3.0)}}
    late = {620: {P: (1.0, 2.0, 3.0), B: (0.0, 0.0, 0.0)},
            624: {P: (1.0, 2.0, 3.0), B: (1.0, 0.0, 0.0)},
            632: {P: (1.0, 1.9005, 3.0)},
            636: {P: (1.0, 1.9, 3.0)}}
    gaps = {620: {B: (0.0, 0.0, 0.0)}, 632: {P: (1.0, 1.9, 3.0)}}
    check('error is Euclidean', abs(error((0, 0, 0), (0.03, 0.04, 0)) - 0.05) < 1e-12)
    check('bands', [band(x) for x in (0.0, 5e-4, 0.01, 0.09, None)] == [0, 1, 2, 3, None])
    check('select by name prefix and by owner',
          select_keys(server, 'P_Modul_01') == [P] and select_keys(server, 'ball:7') == [B]
          and select_keys(server, 'ball:8') == [] and select_keys(server, 'P_Modul_02') == [])
    rows = table(server, [late, gaps], P, series_ticks(server, P))
    check('table cells: exact, error, no checkpoint, absent',
          [r[2] for r in rows] == [['0', 'absent'], ['0.0900', '-'], ['0.0005', '0'], ['0', '-']], str(rows))
    rows = table(server, [late], P, series_ticks(server, P), changes=True)
    check('--changes keeps the band changes only', [r[0] for r in rows] == [620, 624, 632, 636], str(rows))
    rows = table(server, [late], P, series_ticks(server, P, ticks=[624, 999]))
    check('--ticks keeps the asked ticks the server has', [r[0] for r in rows] == [624], str(rows))
    check('--from/--to', series_ticks(server, P, tfrom=624, tto=632) == [624, 632])
    s = summary(server, late, P, series_ticks(server, P))
    check('summary', s['max_tick'] == 624 and abs(s['max'] - 0.09) < 1e-9 and s['first'] == 624 and s['last'] == 632
          and s['count'] == 2 and s['exact_again'] == 636 and s['compared'] == 4, str(s))
    s = summary(server, server, P, series_ticks(server, P))
    check('summary of an exact client', s['count'] == 0 and s['compared'] == 4 and s['exact_again'] is None, str(s))
    print('selftest: %s' % ('PASS' if not failures else 'FAIL - ' + ', '.join(failures)))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('server', nargs='?')
    ap.add_argument('clients', nargs='*')
    ap.add_argument('--body', default='')
    ap.add_argument('--ticks', default='')
    ap.add_argument('--from', dest='tfrom', type=int, default=None)
    ap.add_argument('--to', dest='tto', type=int, default=None)
    ap.add_argument('--changes', action='store_true')
    ap.add_argument('--summary', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.server or not args.clients or not args.body:
        ap.error('SERVER.bmjr, at least one CLIENT.bmjr and --body are required')
    return run(args)


if __name__ == '__main__':
    sys.exit(main())
