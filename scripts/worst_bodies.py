"""Which bodies actually diverge between two client journals, and when each first does.

The per-body companion to client_divergence.py, which reports only the worst
body per tick: this names them all, so a treatment that helps one kind of
body and hurts another cannot hide behind a single number.

usage: python scripts/worst_bodies.py <control.bmjr> <arm.bmjr>
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_trace as jt
import first_divergence as fd

control = fd.worlds(jt.read_journal(sys.argv[1]), want_local=True)
arm = fd.worlds(jt.read_journal(sys.argv[2]), want_local=True)

worst = {}
first = {}
for tick in sorted(set(control) & set(arm)):
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
