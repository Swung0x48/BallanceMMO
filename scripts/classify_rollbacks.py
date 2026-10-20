"""Which body breached on each rollback a client took, from its log.

The networked measurement's two columns: rollbacks whose breaching body was a
player ball (prediction at the round trip, the ball story) and rollbacks whose
breaching body was a mechanism (the mechanism story: sleep, scripted phase,
sector setup).  A treatment that helps the mechanisms must move the second
column and leave the first alone.

usage: python scripts/classify_rollbacks.py <client.log> [<client.log> ...]
"""
import collections
import re
import sys

LINE = re.compile(r'rollback to tick (\d+) \((.+?) off by ([0-9.]+) m\)')


def classify(path):
    balls = 0
    mechanisms = collections.Counter()
    forced = 0
    for line in open(path, encoding='utf8', errors='replace'):
        if 'rollback to tick' not in line:
            continue
        if '(forced)' in line:
            forced += 1
            continue
        m = LINE.search(line)
        if not m:
            continue
        body = m.group(2)
        if body.startswith('Ball_') or body.startswith('bmmo_ball') or body.startswith('Peer_'):
            balls += 1
        else:
            mechanisms[body] += 1
    return balls, mechanisms, forced


for path in sys.argv[1:]:
    balls, mechanisms, forced = classify(path)
    total = sum(mechanisms.values())
    worst = ', '.join('%s %d' % kv for kv in mechanisms.most_common(5))
    print('%s: ball %d, mechanism %d%s  [%s]' % (path, balls, total, ' (+%d forced)' % forced if forced else '', worst))
