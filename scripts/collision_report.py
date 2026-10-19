"""Measure what actually touched what in a physics session.

The session counters say whether the two sides agreed; they do not say whether
anything happened. This reads a journal's body checkpoints - which carry every
body's position - and reports the contacts directly:

  * ball against ball: two player balls are radius 2, so centres closer than
    4.2 m are in contact. Reported per pair, with the closest approach and how
    many sampled ticks were in contact.
  * ball against mechanism: the closest any ball came to each mechanism body,
    and whether that body was moving while a ball was near it - a mechanism
    that only ever moves on its own script is not evidence of interaction.

    Read that second one as a lower bound, not a contact test. A journal
    carries body *origins*, and a mechanism mounted at its retail height has
    its origin up to 5 units below the road (15.8 for P_Modul_03), so a ball
    rolling right over it still reads as 5-7 m away. Comparing the number
    across two maps whose mount heights differ measures the mount heights, not
    the contact. The geometry-independent signal is the harness's
    `mechanism_bodies_corrected`: a body the client predicted and the server
    had an opinion about.

Run the session with a small journal_checkpoint_ticks (the harness's
--checkpoint-ticks 2); at the default 660 the sampling is far too coarse to
catch a contact.

usage: python scripts/collision_report.py <journal.bmjr> [--contact 4.2]
"""
import argparse
import collections
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_trace as jt

KIND_BALL = 0


def dist(a, b):
    return math.sqrt(sum((a[k] - b[k]) ** 2 for k in range(3)))


def speed(v):
    return math.sqrt(sum(c * c for c in v))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('journal')
    ap.add_argument('--contact', type=float, default=4.2,
                    help='ball-to-ball centre distance counted as contact (two radius-2 balls)')
    ap.add_argument('--near', type=float, default=6.0,
                    help='ball-to-mechanism centre distance counted as "near"')
    ap.add_argument('--moving', type=float, default=0.05,
                    help='speed above which a mechanism body counts as moving')
    args = ap.parse_args()

    journal = jt.read_journal(args.journal)

    samples = 0
    pair_min = {}
    pair_hits = collections.Counter()
    mech_min = {}
    lift = {}
    mech_live = collections.Counter()   # near a ball AND moving
    ball_names = {}

    for tick in sorted(journal.by_tick):
        for rec in journal.by_tick[tick].checkpoints:
            balls, mechs = [], []
            for b in rec['bodies']:
                if b['kind'] == KIND_BALL:
                    label = b['name'] or f'p{b["owner"]}'
                    ball_names.setdefault(b['owner'], label)
                    balls.append((label, b))
                else:
                    if b['name']:
                        mechs.append((b['name'], b))
            if len(balls) < 1:
                continue
            samples += 1
            for label, b in balls:
                y = b['position'][1]
                kind = label.split('_BMMO_')[0] if '_BMMO_' in label else label
                entry = lift.setdefault(label, [y, b['position'][0], set()])
                entry[2].add(kind)
                if y > entry[0]:
                    entry[0], entry[1] = y, b['position'][0]
            for i in range(len(balls)):
                for j in range(i + 1, len(balls)):
                    (na, ba), (nb, bb) = balls[i], balls[j]
                    key = tuple(sorted((na, nb)))
                    d = dist(ba['position'], bb['position'])
                    if key not in pair_min or d < pair_min[key]:
                        pair_min[key] = d
                    if d < args.contact:
                        pair_hits[key] += 1
            for mname, mb in mechs:
                nearest = min(dist(mb['position'], b['position']) for _, b in balls)
                if mname not in mech_min or nearest < mech_min[mname]:
                    mech_min[mname] = nearest
                if nearest < args.near and speed(mb['linear']) > args.moving:
                    mech_live[mname] += 1

    print(f'{pathlib.Path(args.journal).name}: {samples} sampled checkpoint ticks, '
          f'{len(ball_names)} balls, {len(mech_min)} mechanism bodies')

    print('\nball against ball (contact < %.1f m):' % args.contact)
    if not pair_min:
        print('  no tick had two balls at once')
    for key in sorted(pair_min, key=lambda k: pair_min[k]):
        hits = pair_hits[key]
        mark = 'CONTACT' if hits else '       '
        print(f'  {mark}  {key[0]} <-> {key[1]}: closest {pair_min[key]:.2f} m, '
              f'{hits} ticks in contact')
    print(f'  pairs that made contact: {sum(1 for k in pair_min if pair_hits[k])}/{len(pair_min)}')

    print('\nball height (y is up; a ball resting on the road sits at 2.0):')
    for name in sorted(lift, key=lambda n: -lift[n][0]):
        top, at_x, types = lift[name]
        print(f'  {name}: highest {top:.2f} at x={at_x:.0f}'
              + (f'; ball types seen: {", ".join(sorted(types))}' if types else ''))

    print('\nball against mechanism (closest approach, and ticks moving while a ball was within %.0f m):'
          % args.near)
    for mname in sorted(mech_min, key=lambda m: mech_min[m]):
        live = mech_live[mname]
        mark = 'DISTURBED' if live else '         '
        print(f'  {mark}  {mname}: closest {mech_min[mname]:.2f} m, {live} ticks')
    print(f'  mechanism bodies moving near a ball: {sum(1 for m in mech_min if mech_live[m])}/{len(mech_min)}')


if __name__ == '__main__':
    main()
