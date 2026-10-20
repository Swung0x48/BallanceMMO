"""Where two CLIENT journals disagree about whether a body is asleep.

The rollback restores a body's pose and velocity (and, since v11, its queued
impulses and both PSI quaternions) but nothing of IVP's freeze bookkeeping:
the core's calm references, its simulation unit's per-unit check countdown
(engine change #5), the movement state itself.  If that is what a replay
misses, the first thing to move after a forced rewind is not a position but a
sleep decision: a body that the control run keeps awake falls asleep in the
arm (or the other way round), and only then does its position drift.

Compares the LOCAL checkpoints of a control journal and an arm journal (the
same solo session with and without forced rollbacks), body by body: the first
tick where BODY_FLAG_SIMULATED differs, how many checkpoint ticks differ in
all, and the position error at the first such tick and the worst one.

usage: python scripts/sleep_divergence.py <control.bmjr> <arm.bmjr> [first_tick]
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_trace as jt
import first_divergence as fd

SIMULATED = 1   # session::BODY_FLAG_SIMULATED


def sleep_worlds(journal, want_local=True):
    """tick -> {key: (simulated, position)} from the checkpoints asked for."""
    out = {}
    for cp in journal.checkpoints():
        if not cp['bodies']:
            continue
        if bool(cp['flags'] & fd.LOCAL) != want_local:
            continue
        row = out.setdefault(cp['tick'], {})
        for b in cp['bodies']:
            key = fd.key_of(b)
            if key[0] == 'body' and not b['name']:
                continue
            if key[0] == 'ball' and not b['owner']:
                continue
            row[key] = (bool(b['flags'] & SIMULATED), b['position'])
    return out


def main():
    control = sleep_worlds(jt.read_journal(sys.argv[1]))
    arm = sleep_worlds(jt.read_journal(sys.argv[2]))
    from_tick = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    shared = [t for t in sorted(set(control) & set(arm)) if t >= from_tick]
    if not shared:
        print('no common checkpoint ticks')
        return 2
    first_sleep = {}     # key -> (tick, control flag, arm flag, position error then)
    sleep_ticks = {}     # key -> number of checkpoint ticks with differing flags
    first_pos = {}       # key -> first tick with a position error > 1e-4
    worst_pos = {}       # key -> (tick, error)
    for tick in shared:
        a, b = control[tick], arm[tick]
        for key in sorted(set(a) & set(b)):
            (sa, pa), (sb, pb) = a[key], b[key]
            err = max(abs(pa[k] - pb[k]) for k in range(3))
            if sa != sb:
                sleep_ticks[key] = sleep_ticks.get(key, 0) + 1
                if key not in first_sleep:
                    first_sleep[key] = (tick, sa, sb, err)
            if err > 1e-4 and key not in first_pos:
                first_pos[key] = tick
            if err > worst_pos.get(key, (0, 0.0))[1]:
                worst_pos[key] = (tick, err)
    print('common checkpoint ticks: %d (from %d to %d)' % (len(shared), shared[0], shared[-1]))
    print('bodies whose sleep flag ever differs: %d' % len(first_sleep))
    print('%-32s %8s %8s %8s %9s  %10s  %s' % ('body', 'sleepdiff@', 'ctl', 'arm', 'diffs', 'err then', 'first pos err@ / worst'))
    rows = sorted(first_sleep.items(), key=lambda kv: kv[1][0])
    for key, (tick, sa, sb, err) in rows:
        w = worst_pos.get(key, (0, 0.0))
        print('%-32s %8d %8s %8s %9d  %10.5f  %s / %.5f@%d' % (
            fd.show(key)[:32], tick, 'awake' if sa else 'asleep', 'awake' if sb else 'asleep',
            sleep_ticks[key], err, str(first_pos.get(key, '-')), w[1], w[0]))
    print('--- bodies with a position error but no sleep difference:')
    for key, tick in sorted(first_pos.items(), key=lambda kv: kv[1]):
        if key in first_sleep:
            continue
        w = worst_pos[key]
        print('%-32s first pos err@%d  worst %.5f@%d' % (fd.show(key)[:32], tick, w[1], w[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
