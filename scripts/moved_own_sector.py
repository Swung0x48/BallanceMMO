#!/usr/bin/env python3
"""Did the server ever move a player's own Sector event?  (resync design 7.5, 8 T0)

The server applies an own Sector event at max(origin, N) and echoes it MOVED
when that is later than the owner's stamp.  Until step B1 the owner keeps the
sector at its stamp while every other world runs it at the applied tick, so a
capture with such a move changes under B1; the 16 reference captures are
expected to have none.  This answers that from the journals (step JNL2: logs
are for people):

- the server journal's s5 records (step JNL), one per accepted event, with its
  origin and applied tick: applied != origin is a move, and covers every
  client build, a lost echo included (a record flagged superseded is counted
  apart: its owner hears it as superseded);
- the client journals' status records: the own echoes each client counted
  (acked / moved / superseded / rejected / missing), all types together.

The other event types are counted beside Sector (an own Physicalize /
Unphysicalize moved, a BodyRevived re-stamped).  NONE needs a server journal
that records s5 (step JNL on): with none read - no server journal, or only
older ones (.bmjr, or a SCHEMA without s5) - the answer is UNKNOWN, exit 1.

usage: python scripts/moved_own_sector.py [PATH ...] [--list]
  PATH is a journal or a directory searched for *.bmjs / *.bmjc / *.bmjr
  (default maps/arena-test).  --list prints every s5 record that is not on
  time, of any type.  The last line is MOVED-OWN-SECTOR: NONE | FOUND N |
  UNKNOWN (no s5 records).
"""
import argparse
import collections
import os
import pathlib
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import journal_trace as jt  # noqa: E402
import session_facts as sf  # noqa: E402

ECHO_FIELDS = ('acked', 'moved', 'superseded', 'rejected', 'missing')


def journals(paths):
    for p in map(pathlib.Path, paths):
        if p.is_dir():
            yield from sorted(f for f in p.rglob('*') if f.suffix in ('.bmjs', '.bmjc', '.bmjr'))
        else:
            yield p


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument('paths', nargs='*', default=[str(root / 'maps' / 'arena-test')])
    ap.add_argument('--list', action='store_true', help='print every s5 record that is not on time')
    args = ap.parse_args()

    servers = clients = s5_files = echo_files = 0
    echoes = collections.Counter()      # verdict -> count, over the clients' last status records
    relays = collections.Counter()      # (type, 'moved'|'on time'|'superseded') over the s5 records
    found = []
    for path in journals(args.paths):
        facts = sf.Facts(path)
        if facts.header is None or facts.header['kind'] != jt.KIND_SERVER:
            clients += 1
            status = facts.stats('status')
            if status:
                echo_files += 1
                for k in ECHO_FIELDS:
                    echoes[k] += status[-1]['values']['echo_' + k]
            continue
        servers += 1
        s5 = facts.stats('s5')
        s5_files += facts.recorded('s5')
        for r in s5:
            v = r['values']
            if v['flag'] == 'superseded':
                relays[(v['type'], 'superseded')] += 1
            else:
                moved = v['applied'] != v['origin']
                relays[(v['type'], 'moved' if moved else 'on time')] += 1
                if moved and v['type'] == 'Sector':
                    found.append((path, 'server: player %d stamped %d, applied %d' % (v['player'], v['origin'],
                                                                                     v['applied'])))
                if not moved:
                    continue
            if args.list:
                print('%s: tick %d s5 player=%d type=%s origin=%d applied=%d %s' % (
                    path, r['tick'], v['player'], v['type'], v['origin'], v['applied'], v['flag']))

    print('%d server journals read, %d recording s5; %d client journals, %d with a status record'
          % (servers, s5_files, clients, echo_files))
    print('own echoes (clients): %s' % (', '.join('%s=%d' % (k, echoes[k]) for k in ECHO_FIELDS) if echo_files
                                        else 'none'))
    print('S5 relays (server):   %s' % (', '.join('%s %s=%d' % (k, v, n) for (k, v), n in sorted(relays.items()))
                                         or 'none'))
    for path, what in found:
        print('MOVED own Sector: %s: %s' % (path, what))
    if not found and not s5_files:
        print('MOVED-OWN-SECTOR: UNKNOWN (no s5 records)')
        return 1
    print('MOVED-OWN-SECTOR: %s' % ('FOUND %d' % len(found) if found else 'NONE'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
