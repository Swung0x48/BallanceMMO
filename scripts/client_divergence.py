"""Two CLIENT journals compared body by body, split into balls and mechanisms.

first_divergence.py answers "client vs the server"; this answers "the same
client with and without a treatment", which is what the forced-rollback probe
needs: the control and the arm differ in nothing but the rollbacks.

usage: python scripts/client_divergence.py <control.bmjr> <arm.bmjr> [...]
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_trace as jt
import first_divergence as fd


def load(path):
    return fd.worlds(jt.read_journal(str(path)), want_local=True)


def compare(control, arm, pick):
    """mean / p90 / max over ticks of the worst body error, for the bodies pick()."""
    per_tick = []
    first = None
    for tick in sorted(set(control) & set(arm)):
        worst = 0.0
        for key, pos in control[tick].items():
            if not pick(key) or key not in arm[tick]:
                continue
            other = arm[tick][key]
            err = max(abs(a - b) for a, b in zip(pos, other))
            worst = max(worst, err)
            if err > 1e-9 and first is None:
                first = (tick, key, err)
        per_tick.append(worst)
    if not per_tick:
        return None
    per_tick.sort()
    return (sum(per_tick) / len(per_tick), per_tick[int(len(per_tick) * 0.9)], per_tick[-1],
            len(per_tick), first)


control_path = sys.argv[1]
control = load(control_path)
print('control: %s (%d checkpointed ticks)' % (pathlib.Path(control_path).parent.parent.name, len(control)))
print()
print('%-10s %-11s %9s %9s %9s   %s' % ('arm', 'bodies', 'mean', 'p90', 'max', 'first difference'))
for path in sys.argv[2:]:
    arm = load(path)
    label = pathlib.Path(path).parent.parent.name
    for name, pick in (('ball', lambda k: k[0] == 'ball'),
                       ('mechanisms', lambda k: k[0] == 'body')):
        got = compare(control, arm, pick)
        if not got:
            print('%-10s %-11s  no common checkpoints' % (label, name))
            continue
        mean, p90, mx, n, first = got
        where = 'none' if not first else 'tick %d %s %.2e m' % (first[0], first[1][1], first[2])
        print('%-10s %-11s %9.4f %9.4f %9.4f   %s' % (label, name, mean, p90, mx, where))
