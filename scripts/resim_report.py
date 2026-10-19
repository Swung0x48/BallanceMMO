#!/usr/bin/env python3
"""Where does a client's re-simulation work come from?

Every rollback replays `local_tick - tick` ticks of physics, so a client's
spare CPU goes on two numbers: how DEEP the window is when a snapshot
disagrees, and how OFTEN one does.  Both are in the client's own journal
(CORRECTION records, journal_correction), and this splits them up:

  * the depth, against the session's input delay from the journal header.
    Depth is structural - the snapshot of tick T reaches the client
    input_delay + one trip after T - so it is the number you cannot argue
    with, only measure.

  * the frequency, by subject and by which half of the tolerance pair was
    breached.  The subject split is the one that matters: the client predicted
    its OWN ball from the player's own exact inputs, so an error there is a
    real divergence, while a remote ball or a mechanism was predicted without
    the inputs that drove it - those arrive input_delay + a trip later - so an
    error there is mostly news that has not come yet.

That second table is what sized rollback_thresholds' blind window.  Measured
over a 4-player session at a 300 ms round trip, 242 of 417 mismatches were
remote balls whose position agreed to within 5 cm and which breached on
velocity alone, and they carried 58% of all the re-simulation work - the one
component that grows with the player count.  Run it again after changing a
threshold and the same table says what the change bought.

Run the session with --checkpoint-ticks small enough to be useful, and point
this at the CLIENT journals (the server does not roll back).

usage: python scripts/resim_report.py <client.bmjr> [...] [--position 0.05]
                                      [--velocity 0.5]
"""
import argparse
import collections
import glob
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_trace as jt

MISMATCH = 0          # journal_correction.kind, see rollback.hpp
DEPTH_BANDS = (0.5, 0.9, 1.0)


def classify(entity):
    """Which prediction produced this body: ours alone, or someone else's."""
    if not entity:
        return 'unnamed'
    if '_BMMO_' in entity:
        return 'remote ball'
    if entity.startswith('Ball_'):
        return 'own ball'
    return 'mechanism'


def report(path, position, velocity):
    journal = jt.read_journal(str(path))
    rows = [c for c in journal.corrections() if c['kind'] == MISMATCH]
    print('== %s  (session input delay %u ticks)'
          % (path.name, journal.header['input_delay']))
    if not rows:
        print('   no mismatches')
        return collections.Counter(), collections.Counter()

    depths = sorted(max(0, c['local_tick'] - c['tick']) for c in rows)
    marks = ', '.join('p%d %d' % (p * 100, depths[min(len(depths) - 1, int(len(depths) * p))])
                      for p in DEPTH_BANDS)
    print('   %d mismatches, %d resim ticks; window depth %s'
          % (len(rows), sum(depths), marks))

    count, work = collections.Counter(), collections.Counter()
    for c in rows:
        depth = max(0, c['local_tick'] - c['tick'])
        breach = ('position' if c['error_m'] > position
                  else 'velocity only' if c['velocity_error'] > velocity
                  else 'within pair')
        key = (classify(c['entity']), breach)
        count[key] += 1
        work[key] += depth
    total = sum(work.values())
    for key in sorted(work, key=lambda k: -work[k]):
        print('     %-12s %-13s %4d mismatches, %6d resim ticks (%4.1f%%)'
              % (key[0], key[1], count[key], work[key], 100.0 * work[key] / max(1, total)))
    return count, work


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('journals', nargs='+')
    ap.add_argument('--position', type=float, default=0.05,
                    help='position tolerance the session ran with, in metres')
    ap.add_argument('--velocity', type=float, default=0.5,
                    help='velocity tolerance the session ran with, in m/s')
    args = ap.parse_args()

    paths = []
    for pattern in args.journals:
        paths.extend(sorted(pathlib.Path(p) for p in glob.glob(pattern)) or [pathlib.Path(pattern)])
    grand_count, grand_work = collections.Counter(), collections.Counter()
    for path in paths:
        count, work = report(path, args.position, args.velocity)
        grand_count.update(count)
        grand_work.update(work)

    if len(paths) > 1:
        print('\n== every client')
        total = sum(grand_work.values())
        for key in sorted(grand_work, key=lambda k: -grand_work[k]):
            print('   %-12s %-13s %5d mismatches, %7d resim ticks (%4.1f%%)'
                  % (key[0], key[1], grand_count[key], grand_work[key],
                     100.0 * grand_work[key] / max(1, total)))
        print('   %d resim ticks in all' % total)


if __name__ == '__main__':
    main()
