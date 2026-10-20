"""Which bodies actually diverge between two client journals, and when each first does.

The per-body companion to client_divergence.py, which reports only the worst
body per tick: this names them all, so a treatment that helps one kind of
body and hurts another cannot hide behind a single number.

usage: python scripts/worst_bodies.py <control.bmjr> <arm.bmjr> [--from TICK] [--until TICK]

--from / --until restrict the comparison to a tick window, for a session whose
runs stop being comparable at some point (a death whose sector reset the two
runs take at different ticks, a ball that leaves the map in one of them).
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_trace as jt
import first_divergence as fd

ap = argparse.ArgumentParser()
ap.add_argument('control')
ap.add_argument('arm')
ap.add_argument('--from', dest='tick_from', type=int, default=0)
ap.add_argument('--until', dest='tick_until', type=int, default=1 << 31)
args = ap.parse_args()

control = fd.worlds(jt.read_journal(args.control), want_local=True)
arm = fd.worlds(jt.read_journal(args.arm), want_local=True)

worst = {}
first = {}
for tick in sorted(set(control) & set(arm)):
    if tick < args.tick_from or tick > args.tick_until:
        continue
    for key, pos in control[tick].items():
        if key not in arm[tick]:
            continue
        err = max(abs(a - b) for a, b in zip(pos, arm[tick][key]))
        name = key[1] if key[0] == 'body' else 'ball:%s' % key[1]
        if err > worst.get(name, -1):
            worst[name] = err
        if err > 1e-6 and name not in first:
            first[name] = (tick, err)

print('%-28s %12s   %s' % ('body', 'max err (m)', 'first > 1e-6 m'))
for name, err in sorted(worst.items(), key=lambda kv: -kv[1]):
    got = first.get(name)
    print('%-28s %12.4f   %s' % (name, err, 'tick %d (%.3e m)' % got if got else 'never'))
