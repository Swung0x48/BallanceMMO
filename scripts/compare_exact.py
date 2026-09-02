"""Compare the 'exact t=' dumps of the server log and the client ModLoader log.

usage: compare_exact.py <server.log> <client.log> [max_diffs] [own player id]
With an own id, only that player's server ball (Ball_X_BMMO_<id+1>) is kept and
renamed to the client's name; other players' balls are dropped on both sides.
"""
import re
import sys

server_log, client_log = sys.argv[1:3]
max_diffs = int(sys.argv[3]) if len(sys.argv) > 3 else 40
own_id = int(sys.argv[4]) if len(sys.argv) > 4 else None

pat = re.compile(r'exact t=(\d+) (.*)$')
bmmo = re.compile(r'_BMMO_\d+')


def parse(path):
    out = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\r\n')
            m = pat.search(line)
            if not m:
                continue
            tick = int(m.group(1))
            raw_name = m.group(2).split(' ', 1)[0]
            if own_id is not None:
                mm = re.match(r'(.*)_BMMO_(\d+)$', raw_name)
                if mm and int(mm.group(2)) != own_id + 1:
                    continue   # another player's ball
            rest = bmmo.sub('', m.group(2))
            name = rest.split(' ', 1)[0]
            if not name or '_BMMO_' in name:
                continue
            out[(tick, name)] = rest
    return out


s = parse(server_log)
c = parse(client_log)
ticks = sorted({t for t, _ in s} | {t for t, _ in c})
diffs = 0
for t in ticks:
    s_names = {n for tt, n in s if tt == t}
    c_names = {n for tt, n in c if tt == t}
    if s_names != c_names:
        print(f't={t}: object sets differ: only server={sorted(s_names - c_names)} only client={sorted(c_names - s_names)}')
        diffs += 1
    for n in sorted(s_names & c_names):
        a, b = s[(t, n)], c[(t, n)]
        if a == b:
            continue
        fa, fb = a.split(' '), b.split(' ')
        changed = [(x, y) for x, y in zip(fa, fb) if x != y]
        print(f't={t} {n}: ' + ' | '.join(f'S {x} C {y}' for x, y in changed[:6]))
        diffs += 1
    if diffs >= max_diffs:
        print('... (stopping)')
        break
if diffs == 0:
    print(f'identical over {len(ticks)} ticks ({len(s)} server / {len(c)} client entries)')
