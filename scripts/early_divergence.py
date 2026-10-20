"""Restore fidelity measured BEFORE chaos saturates the metric.

usage: python scripts/early_divergence.py <max-tick> <control.bmjr> <arm.bmjr> [...]

The session mean is worthless here: one bistable prop (P_Ball_Paper_MF either
stays on the map or does not) pins p90 and max to the same two numbers in every
arm.  So measure in an early tick window, where the error is still the restore's
own and nothing has had time to amplify it, and report per body.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_trace as jt
import first_divergence as fd

LIMIT = int(sys.argv[1])
control = fd.worlds(jt.read_journal(sys.argv[2]), want_local=True)

print('window: ticks <= %d' % LIMIT)
print('%-26s' % 'body', end='')
labels = []
for path in sys.argv[3:]:
    label = path.split('/')[-3]
    labels.append(label)
    print('%13s' % label, end='')
print()

arms = [fd.worlds(jt.read_journal(p), want_local=True) for p in sys.argv[3:]]
names = set()
for tick, bodies in control.items():
    if tick <= LIMIT:
        names.update(k[1] if k[0] == 'body' else 'ball:%s' % k[1] for k in bodies)

rows = []
for name in sorted(names):
    cells = []
    for arm in arms:
        worst = 0.0
        for tick in sorted(set(control) & set(arm)):
            if tick > LIMIT:
                continue
            for key, pos in control[tick].items():
                label = key[1] if key[0] == 'body' else 'ball:%s' % key[1]
                if label != name or key not in arm[tick]:
                    continue
                worst = max(worst, max(abs(a - b) for a, b in zip(pos, arm[tick][key])))
        cells.append(worst)
    rows.append((max(cells), name, cells))

for _, name, cells in sorted(rows, reverse=True):
    print('%-26s' % name, end='')
    for c in cells:
        print('%13s' % ('%.3e' % c if c else '0'), end='')
    print()
