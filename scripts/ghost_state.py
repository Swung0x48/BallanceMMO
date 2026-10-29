"""The spawn ghost per ball and tick, two worlds side by side, and the per-world
"no contact from an overlap" check (step F0, SPX's deferred tools).

  ghost_state.py SERVER.bmjr CLIENT.bmjr [--server-log S] [--client-log C]
                 [--from T] [--to T] [--every N] [--contacts-only] [--selftest]

Reads the server journal (FULL checkpoints) and one client journal (LOCAL
checkpoints), and each side's log for its ghost transitions:

  server  "player P's ball B is a spawn ghost|solid at tick T",
          "player P's ball B is in the airship zone at tick T"
  client  the same lines for the mirrors, "the own ball B is a spawn ghost|solid
          at tick T" and "the own ball B is in the airship zone at tick T" for
          its own ball (the own id from "own id N").

Prints one line per ball per common checkpoint tick:

  tick owner  server (x,y,z) state nearest | client (x,y,z) state nearest | dist agree

state is ghost / solid / airship / - (no line yet) as each world's log says at
that tick; nearest is the centre distance to the nearest other ball in that
world.  Then, per world, the contact check: at every solid transition the
nearest other ball at the checkpoints T-2..T+2, and over the run the closest
approach of two balls both solid in that world; a checkpoint under the ball
diameter (4 m) between two solid balls is a contact started from an overlap.

LIMITS (said, not hidden): the ghost bit is not in the journal - a body's
checkpoint flags carry only SIMULATED and COLLISION_ENABLED, and a spawn ghost
keeps its collision on (only ball-to-ball pairs are off), so the states come
from the logs.  A client's line is frame-observed, once per life: a ghost ->
solid decided again by a later rewind leaves no line, so a client state is the
first time its world showed it, not a verified final state (SPX's As built: c1
logs a mirror solid at 680 where the server and the owner say 681).  Putting
the ghost bit into the record would be a journal format change."""
import argparse
import math
import re
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import journal_trace as jt  # noqa: E402

DIAMETER = 4.0
RE_PLAYER = re.compile(r"player (\d+)'s ball \S+ is (a spawn ghost|solid|in the airship zone) at tick (\d+)")
RE_OWN = re.compile(r"the own ball \S+ is (a spawn ghost|solid|in the airship zone) at tick (\d+)")
RE_OWN_ID = re.compile(r'own id (\d+)')
STATE = {'a spawn ghost': 'ghost', 'solid': 'solid', 'in the airship zone': 'airship'}


def transitions(text, own_id=0):
    """[(tick, player, state)] from a log, in tick order (the own ball's lines
    under own_id)."""
    out = []
    for line in text.splitlines():
        m = RE_PLAYER.search(line)
        if m:
            out.append((int(m.group(3)), int(m.group(1)), STATE[m.group(2)]))
            continue
        m = RE_OWN.search(line)
        if m and own_id:
            out.append((int(m.group(2)), own_id, STATE[m.group(1)]))
    out.sort()
    return out


def state_at(trans, player, tick):
    """The last state the log gave the player at or before tick, '-' none."""
    st = '-'
    for t, p, s in trans:
        if t > tick:
            break
        if p == player:
            st = s
    return st


def balls(journal, want_local):
    """tick -> {owner: (x, y, z)} of the ball bodies at the side's checkpoints
    (LOCAL for a client, FULL for the server)."""
    out = {}
    for cp in journal.checkpoints():
        # Step D0: a FINAL row (headless --final-checkpoints) is not the live world.
        if bool(cp['flags'] & jt.CHECKPOINT_LOCAL) != want_local or not cp['bodies'] or cp['flags'] & jt.CHECKPOINT_FINAL:
            continue
        row = {b['owner']: tuple(b['position']) for b in cp['bodies'] if b['kind'] == 0 and b['owner']}
        out[cp['tick']] = row   # the last checkpoint of a tick wins (as first_divergence.worlds)
    return out


def nearest(row, owner):
    others = [math.dist(row[owner], q) for o, q in row.items() if o != owner]
    return min(others) if others else float('inf')


def contact_check(name, rows, trans):
    """The per-world check: nearest other ball at each solid transition, and
    the closest approach of two balls both solid in this world."""
    ticks = sorted(rows)
    lines = []
    for t, p, s in trans:
        if s != 'solid' or not ticks or not ticks[0] <= t <= ticks[-1]:
            continue
        near = ['%d:%.3f' % (u, nearest(rows[u], p)) for u in range(t - 2, t + 3) if u in rows and p in rows[u]]
        lines.append('  %s: solid %d p%d: nearest other ball %s' % (name, t, p, ' '.join(near) or '-'))
    worst, under = (float('inf'), None), 0
    for t in ticks:
        row = rows[t]
        solid = [o for o in row if state_at(trans, o, t) == 'solid']
        for i in range(len(solid)):
            for j in range(i + 1, len(solid)):
                d = math.dist(row[solid[i]], row[solid[j]])
                if d < worst[0]:
                    worst = (d, (t, solid[i], solid[j]))
                if d < DIAMETER - 1e-3:
                    under += 1
    lines.append('  %s: closest approach of two solid balls %.4f m at %s; checkpoints under %.1f m: %d -> %s' % (
        name, worst[0], worst[1], DIAMETER, under, 'PASS' if under == 0 else 'FAIL (a contact from an overlap)'))
    return lines, under


def read_text(path):
    if not path:
        return ''
    return open(path, encoding='utf-8', errors='replace').read().replace('\0', '')


def selftest():
    text = ("[x] world: spawn ghost: player 7's ball Ball_Stone_BMMO_8 is a spawn ghost at tick 212\n"
            "own id 9\nspawn ghost: the own ball Ball_Stone is a spawn ghost at tick 212\n"
            "spawn ghost: the own ball Ball_Stone is solid at tick 300\n"
            "[x] world: spawn ghost: player 7's ball Ball_Stone_BMMO_8 is solid at tick 320\n"
            "airship: the own ball Ball_Stone is in the airship zone at tick 900: out of the sync from here\n")
    tr = transitions(text, own_id=9)
    ok = (tr == [(212, 7, 'ghost'), (212, 9, 'ghost'), (300, 9, 'solid'), (320, 7, 'solid'), (900, 9, 'airship')]
          and state_at(tr, 7, 319) == 'ghost' and state_at(tr, 7, 320) == 'solid' and state_at(tr, 9, 100) == '-')
    rows = {300: {7: (0.0, 0.0, 0.0), 9: (3.0, 0.0, 0.0)}, 322: {7: (0.0, 0.0, 0.0), 9: (3.0, 0.0, 0.0)},
            324: {7: (0.0, 0.0, 0.0), 9: (5.0, 0.0, 0.0)}}
    _, under = contact_check('t', rows, tr)
    ok = ok and under == 1
    print('selftest: %s' % ('PASS' if ok else 'FAIL %s %d' % (tr, under)))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('server', nargs='?')
    ap.add_argument('client', nargs='?')
    ap.add_argument('--server-log', default='')
    ap.add_argument('--client-log', default='')
    ap.add_argument('--from', dest='lo', type=int, default=0)
    ap.add_argument('--to', dest='hi', type=int, default=1 << 31)
    ap.add_argument('--every', type=int, default=1, help='print every N-th common checkpoint (the check reads all)')
    ap.add_argument('--contacts-only', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.server or not args.client:
        ap.error('SERVER and CLIENT journals are needed')
    sj, cj = jt.read_journal(args.server), jt.read_journal(args.client)
    own = cj.header.get('own_player', 0)
    stext, ctext = read_text(args.server_log), read_text(args.client_log)
    m = RE_OWN_ID.search(ctext)
    if m:
        own = int(m.group(1))
    s_tr, c_tr = transitions(stext), transitions(ctext, own_id=own)
    s_rows, c_rows = balls(sj, False), balls(cj, True)
    common = [t for t in sorted(set(s_rows) & set(c_rows)) if args.lo <= t <= args.hi]
    print('== ghost states: server %s (%d transitions), client %s (own %d, %d transitions); %d common checkpoints'
          % (args.server, len(s_tr), args.client, own, len(c_tr), len(common)))
    disagree = 0
    for k, t in enumerate(common):
        s_row, c_row = s_rows[t], c_rows[t]
        for owner in sorted(set(s_row) | set(c_row)):
            ss, cs = state_at(s_tr, owner, t), state_at(c_tr, owner, t)
            agree = ss == cs
            disagree += 0 if agree else 1
            if args.contacts_only or k % args.every:
                continue
            sp, cp = s_row.get(owner), c_row.get(owner)
            print('%6d %10d%s  S %s %-7s %7s | C %s %-7s %7s | %8s %s' % (
                t, owner, ' own' if owner == own else '    ',
                '(%.3f,%.3f,%.3f)' % sp if sp else '(absent)', ss,
                '%.3f' % nearest(s_row, owner) if sp else '-',
                '(%.3f,%.3f,%.3f)' % cp if cp else '(absent)', cs,
                '%.3f' % nearest(c_row, owner) if cp else '-',
                '%.4f' % math.dist(sp, cp) if sp and cp else '-', 'agree' if agree else 'DIFFER'))
    print('== ball-ticks whose logged state differs between the worlds: %d' % disagree)
    out, under_s = contact_check('server', {t: s_rows[t] for t in s_rows if args.lo <= t <= args.hi}, s_tr)
    out2, under_c = contact_check('client', {t: c_rows[t] for t in c_rows if args.lo <= t <= args.hi}, c_tr)
    print('\n'.join(out + out2))
    return 0 if under_s == 0 and under_c == 0 else 3


if __name__ == '__main__':
    sys.exit(main())
