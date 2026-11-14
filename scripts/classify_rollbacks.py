"""Which body each rewind a client took named, and why, from its journal (step JNL2: the rewind records).

The networked measurement's two columns: rewinds whose body was a player ball (prediction at the round trip, the
ball story) and rewinds whose body was a mechanism (the mechanism story: sleep, scripted phase, sector setup).  A
treatment that helps the mechanisms must move the second column and leave the first alone.  Then every cause the
records carry (session/journal_stats.hpp rewind_cause), the forced ones (--force-rollback) apart.

Before step JNL2 this read the client log's snapshot-correction lines ('rollback to tick T (B off by E m)'), which
no run with snapshots off prints: the rewind records (step JNL) carry the body and the cause of every rewind.

usage: python scripts/classify_rollbacks.py CLIENT_JOURNAL_OR_DIR [...]
  a directory: every client*.bmjc under it.  A journal from before step JNL has no rewind record: said, not 0.
"""
import collections
import os
import pathlib
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import session_facts as sf  # noqa: E402

CAUSES = ('input', 'own_input', 'calibration', 'forced', 'sector', 'spawn_forget', 'lifecycle', 'own_move', 'resync')
FORCED = CAUSES.index('forced')


def is_ball(body):
    return body.startswith(('Ball_', 'bmmo_ball', 'Peer_'))


def classify(path):
    """(balls, mechanisms by body, forced, causes), or None for a journal without rewind records."""
    facts = sf.Facts(path)
    if not facts.recorded('rewind'):
        return None
    balls, forced = 0, 0
    mechanisms, causes = collections.Counter(), collections.Counter()
    for r in facts.stats('rewind'):
        v = r['values']
        cause = v['cause']
        causes[CAUSES[cause] if 0 <= cause < len(CAUSES) else str(cause)] += 1
        if cause == FORCED:
            forced += 1
        elif v['body']:
            if is_ball(v['body']):
                balls += 1
            else:
                mechanisms[v['body']] += 1
    return balls, mechanisms, forced, causes


def journals(paths):
    for p in map(pathlib.Path, paths):
        yield from sorted(p.rglob('client*.bmjc')) if p.is_dir() else [p]


def main(argv):
    for path in journals(argv):
        got = classify(path)
        if got is None:
            print('%s: no rewind records (a journal from before step JNL)' % path)
            continue
        balls, mechanisms, forced, causes = got
        worst = ', '.join('%s %d' % kv for kv in mechanisms.most_common(5))
        print('%s: ball %d, mechanism %d%s  [%s]  causes %s' % (
            path, balls, sum(mechanisms.values()), ' (+%d forced)' % forced if forced else '', worst,
            ', '.join('%s %d' % kv for kv in sorted(causes.items())) or 'none'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
