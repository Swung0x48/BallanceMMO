#!/usr/bin/env python3
"""Which body stopped agreeing with the server first, and when?

A session that drifts gives you a hash that differs and nothing else. This
compares a client's own recorded world against the server's, body by body, at
every tick both of them checkpointed, and reports the first body to go outside
a tolerance - which is the question you actually need answered before you can
reason about a cause.

Both sides have to be dense enough to bisect: run the harness with
`--checkpoint-ticks 2`, which now sets the cadence for the server's journal
*and* each client's (the client's own default is one checkpoint every ten
seconds, a black box rather than a trace).

The server journal's FULL checkpoints carry every movable body with its name;
a client's LOCAL checkpoints carry its own world the same way. Names are the
identity on both sides. A body only one side has is reported separately - that
is a body-set disagreement, which is a different fault from drift.

usage: python scripts/first_divergence.py <server.bmjr> <client.bmjr> [...]
                                          [--tolerance 0.05]
"""
import argparse
import glob
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_trace as jt

LOCAL = 2       # JOURNAL_CHECKPOINT_LOCAL
RECEIVED = 4    # JOURNAL_CHECKPOINT_RECEIVED


def worlds(journal, want_local, prefix=''):
    """tick -> {name: position}, from the checkpoints of the kind asked for.

    `prefix` restricts it to bodies whose name starts with it, which is how you
    compare two CLIENTS: a player's own ball is named `Ball_<type>` locally on
    every client, so the same name means a different physical ball on each one
    and comparing by name is meaningless for them.  The map's own bodies - the
    mechanisms and props, `P_*` - carry the same name everywhere and are the
    shared world worth comparing.
    """
    out = {}
    for cp in journal.checkpoints():
        if not cp['bodies']:
            continue
        is_local = bool(cp['flags'] & LOCAL)
        if is_local != want_local:
            continue
        row = out.setdefault(cp['tick'], {})
        for b in cp['bodies']:
            if b['name'] and b['name'].startswith(prefix):
                row[b['name']] = b['position']
    return out


def compare(server, client, tolerance):
    shared = sorted(set(server) & set(client))
    if not shared:
        return None, 0, []
    first = None
    missing = []
    for tick in shared:
        a, b = server[tick], client[tick]
        only = (set(a) - set(b)) | (set(b) - set(a))
        if only:
            missing.append((tick, sorted(only)))
        worst_name, worst = None, 0.0
        for name in set(a) & set(b):
            d = max(abs(a[name][k] - b[name][k]) for k in range(3))
            if d > worst:
                worst, worst_name = d, name
        if worst > tolerance and first is None:
            first = (tick, worst_name, worst)
    return first, len(shared), missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('server')
    ap.add_argument('clients', nargs='+')
    ap.add_argument('--tolerance', type=float, default=0.05,
                    help='metres on any axis before a body counts as diverged')
    ap.add_argument('--prefix', default='',
                    help="only bodies whose name starts with this; use P_ to compare the "
                         "map's own bodies, whose names mean the same thing on every side")
    args = ap.parse_args()

    server = worlds(jt.read_journal(args.server), want_local=False, prefix=args.prefix)
    print('server %s: %d full checkpoints' % (pathlib.Path(args.server).name, len(server)))
    if not server:
        print('  no FULL checkpoints - is this a server journal?')
        return

    paths = []
    for pattern in args.clients:
        paths.extend(sorted(pathlib.Path(p) for p in glob.glob(pattern)) or [pathlib.Path(pattern)])
    for path in paths:
        client = worlds(jt.read_journal(str(path)), want_local=True, prefix=args.prefix)
        first, shared, missing = compare(server, client, args.tolerance)
        print('%s: %d local checkpoints, %d ticks comparable' % (path.name, len(client), shared))
        if not shared:
            print('    nothing to compare - run with --checkpoint-ticks 2')
            continue
        if first is None:
            print('    agrees with the server within %.3f m throughout' % args.tolerance)
        else:
            print('    first divergence: tick %d, %s off by %.4f m' % first)
        if missing:
            tick, names = missing[0]
            print('    body set first differs at tick %d: %s (%d such ticks)'
                  % (tick, ', '.join(names[:6]), len(missing)))


if __name__ == '__main__':
    main()
