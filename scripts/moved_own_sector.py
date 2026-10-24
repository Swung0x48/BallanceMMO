#!/usr/bin/env python3
"""Did the server ever move a player's own Sector event?  (resync design 7.5, 8 T0)

The server applies an own Sector event at max(origin, N) and echoes it MOVED
when that is later than the owner's stamp.  Until step B1 the owner keeps the
sector at its stamp while every other world runs it at the applied tick, so a
capture with such a move changes under B1; the 16 reference captures are
expected to have none.  This answers that from the logs, two ways:

- the client core's own-echo line (client_core_lifecycle.cpp process_own_echo):
    own sector stamped tick T echoed moved by the server (applied A, ...)
  in the headless client's log and the retail mod's core-on log.  The legacy
  retail session code prints no echo line;
- the server's relay line (server.cpp, the S5 line), one per accepted event:
    S5 event player=P type=Sector origin=T applied=A ...
  with A != T, which covers every client build, a lost echo included (a line
  ending " superseded" is counted apart: its owner hears it as superseded).

Both formats are checked against the other event types in the same logs (an
own Physicalize/Unphysicalize moved, a BodyRevived re-stamped), so a format
change that made the grep match nothing would show as zero format hits.

usage: python scripts/moved_own_sector.py [PATH ...] [--list]
  PATH is a log file or a directory searched for *.log (default
  maps/arena-test); a file named explicitly is read whatever its name.  --list prints every own-echo and S5 line that is not an
  on-time ack, of any type.  The last line is MOVED-OWN-SECTOR: NONE | FOUND.
"""
import argparse
import collections
import pathlib
import re

ECHO = re.compile(r'own (\w+) stamped tick (\d+) echoed (moved|superseded|rejected) by the server \(applied (\d+)',
                  re.IGNORECASE)
S5 = re.compile(r'S5 event player=(\d+) type=(\w+) origin=(\d+) applied=(\d+)')


def logs(paths):
    for p in paths:
        p = pathlib.Path(p)
        if p.is_dir():
            yield from sorted(p.rglob('*.log'))
        else:
            yield p


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument('paths', nargs='*', default=[str(root / 'maps' / 'arena-test')])
    ap.add_argument('--list', action='store_true', help='print every non-ack own echo and re-stamped S5 line')
    args = ap.parse_args()

    files = 0
    echo_files = 0      # logs that print own-echo lines at all
    s5_files = 0        # server logs with S5 lines
    echoes = collections.Counter()      # (type, verdict) over the client lines
    relays = collections.Counter()      # (type, 'moved'|'on time') over the S5 lines
    found = []
    for path in logs(args.paths):
        files += 1
        has_echo = has_s5 = False
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                if 'echoed' in line:
                    m = ECHO.search(line)
                    if m:
                        has_echo = True
                        kind, verdict = m.group(1).lower(), m.group(3).lower()
                        echoes[(kind, verdict)] += 1
                        if args.list:
                            print('%s: %s' % (path, line.strip()))
                        if kind == 'sector' and verdict == 'moved':
                            found.append((path, 'own echo: stamped %s, applied %s' % (m.group(2), m.group(4))))
                        continue
                if 'S5 event' in line:
                    m = S5.search(line)
                    if m:
                        has_s5 = True
                        kind = m.group(2)
                        if line.rstrip().endswith(' superseded'):
                            # the owner hears this one as superseded, not moved
                            relays[(kind, 'superseded')] += 1
                            if args.list:
                                print('%s: %s' % (path, line.strip()))
                            continue
                        moved = m.group(3) != m.group(4)
                        relays[(kind, 'moved' if moved else 'on time')] += 1
                        if moved and args.list:
                            print('%s: %s' % (path, line.strip()))
                        if moved and kind == 'Sector':
                            found.append((path, 'server: player %s stamped %s, applied %s'
                                          % (m.group(1), m.group(3), m.group(4))))
        echo_files += has_echo
        s5_files += has_s5

    print('%d logs read: %d with own-echo lines, %d with S5 lines' % (files, echo_files, s5_files))
    print('own echoes (client):  %s' % (', '.join('%s %s=%d' % (k, v, n) for (k, v), n in sorted(echoes.items()))
                                         or 'none'))
    print('S5 relays (server):   %s' % (', '.join('%s %s=%d' % (k, v, n) for (k, v), n in sorted(relays.items()))
                                         or 'none'))
    for path, what in found:
        print('MOVED own Sector: %s: %s' % (path, what))
    print('MOVED-OWN-SECTOR: %s' % ('FOUND %d' % len(found) if found else 'NONE'))


if __name__ == '__main__':
    main()
