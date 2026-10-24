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

KIND_BALL = 0
LOCAL = 2       # JOURNAL_CHECKPOINT_LOCAL
RECEIVED = 4    # JOURNAL_CHECKPOINT_RECEIVED


def key_of(body):
    """The identity of a body that means the same thing on every side.

    A ball is its OWNER.  Names cannot do it: the server records a ball by
    owner and leaves the name empty, a client calls its own ball `Ball_<type>`
    whatever its id, and a mirror's `Ball_<type>_BMMO_<id>` has its trailing
    digits bumped by Virtools whenever CopyObject has to uniquify it, so the id
    in the name drifts.  A mechanism or prop is its name, which the map gives
    it and every side agrees on.
    """
    if body['kind'] == KIND_BALL:
        return ('ball', body['owner'])
    return ('body', body['name'])


def worlds(journal, want_local, prefix='', ownerless_by_name=False):
    """tick -> {key: position}, from the checkpoints of the kind asked for.

    `prefix` keeps only map bodies whose name starts with it (and drops the
    balls), for when you want the shared world alone.

    `ownerless_by_name` keys a kind-0 body with no owner by its name instead
    of dropping it: a journal written before step P recorded every "Ball_"
    body as a ball, the trafo explosion pieces (Ball_*_piece*) among them,
    and a legacy retail journal records even its balls with owner 0; their
    names are what identifies them within one client's runs.
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
            key = key_of(b)
            if prefix and not (key[0] == 'body' and str(key[1]).startswith(prefix)):
                continue
            if key[0] == 'body' and not b['name']:
                continue
            if key[0] == 'ball' and not b['owner']:
                if ownerless_by_name and b['name']:
                    key = ('body', b['name'])
                else:
                    continue   # a client that did not know the owner: not comparable
            row[key] = b['position']
    return out


def show(key):
    return 'ball of %u' % key[1] if key[0] == 'ball' else str(key[1])


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
            missing.append((tick, sorted(show(k) for k in only)))
        worst_name, worst = None, 0.0
        for name in set(a) & set(b):
            d = max(abs(a[name][k] - b[name][k]) for k in range(3))
            if d > worst:
                worst, worst_name = d, name
        if worst > tolerance and first is None:
            first = (tick, show(worst_name), worst)
    return first, len(shared), missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('server')
    ap.add_argument('clients', nargs='+')
    ap.add_argument('--tolerance', type=float, default=0.05,
                    help='metres on any axis before a body counts as diverged')
    ap.add_argument('--prefix', default='',
                    help="only MAP bodies whose name starts with this, dropping the balls; "
                         "use P_ to look at the shared world on its own")
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
