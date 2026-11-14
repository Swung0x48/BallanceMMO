#!/usr/bin/env python3
"""Per-session and total tables of a public test from the server's journals (step JNL).

    pubtest_report.py SERVER_JOURNAL_OR_DIR [...] [--out FILE] [--players]

Reads .bmjs files only - never a log: logs are for people.  Each column comes
from a typed record where the journal has one (step JNL on), else from the
records every journal has (derived), else it says '-' and the legend says
why (not recorded before the step).  --players adds the per-player table:
it names players, so it stays on the machine that holds the data (only the
totals and the per-session rows leave it).

Columns: the session's level, start (UTC, the header's), minutes (the last
tick's ms), founding members (N0) and the peak, the input delay, the worst
round trip it was sized from (rtt), player-hours, resyncs (states sent) and
the tight ones (the same player again within 198 ticks), resync MB on the
wire, substituted ticks (S2: a member's INPUT without FRESH) and their runs
of 132 ticks or more, the step time p50/p99/max (us), lead raises, the end
reason.  Python 3.8+, journal_trace.py and session_facts.py beside it.
"""
import argparse
import collections
import datetime
import os
import pathlib
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import journal_trace as jt  # noqa: E402

_U32 = struct.Struct('<I')
_INPUT = struct.Struct('<IIBB')
RESYNC_NOTE = re.compile(r'^resync state for player (\d+): tick (\d+) base (\d+), (\d+) rows, (\d+) script bytes, '
                         r'(\d+) savestate bytes')
LEAD_NOTE = re.compile(r'^lead: p(\d+) ([+-]\d+) at ')
LATENESS_NOTE = re.compile(r'^lateness: (raise|lower) player=(\d+)')
LEAVER_NOTE = re.compile(r'^leaver: p(\d+) ')
# The end reasons (server.cpp end_physics_session) as printed: the ones that name a member are a category only, so
# no name leaves the machine; any reason not listed is 'other'.
END_REASONS = [('everyone left', 'everyone left'), ('nobody reported ready', 'nobody reported ready'),
               ('restarted by the host', 'restarted by the host'), ('room closed', 'room closed'),
               ('simulation failed: ', 'simulation failed'), ('world boot failed: ', 'world boot failed'),
               ('world mismatch for ', 'world mismatch'), (' runs physics protocol ', 'protocol mismatch'),
               (' runs engine ', 'engine mismatch')]


def end_category(text):
    return next((name for key, name in END_REASONS if text.startswith(key) or (key[0] == ' ' and key in text)),
                'other')


def pct(values, p):
    if not values:
        return None
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * p / 100.0))]


def session_of(path):
    """One server journal walked once: what the tables need."""
    s = {'path': str(path), 'schema': None, 'players': {}, 'adds': [], 'removes': [], 'resyncs': [], 'ends': [],
         'subst': collections.defaultdict(list), 'stats': collections.defaultdict(list), 'raises': [], 'leads': [],
         'tick_ms': {}, 'first_tick': None, 'last_tick': None, 'last_ms': 0, 'capped': False,
         'leaves': collections.defaultdict(list)}
    seen_tick = False
    for tag, payload in jt.records(path):
        if tag == jt.TAG_HEADER:
            s['header'] = jt.JournalReader._header(payload)
        elif tag == jt.TAG_SCHEMA:
            s['schema'] = jt.parse_schema(payload)
        elif tag in (jt.TAG_STAT, jt.TAG_CLOCK):
            r = jt.parse_stat(payload, s['schema'] or {}, tag == jt.TAG_CLOCK)
            s['stats'][r['name']].append(r)
        elif tag == jt.TAG_PLAYER:
            p = jt.JournalReader._player(jt._Cursor(payload))
            if p['added']:
                s['players'].setdefault(p['id'], p['name'])
                s['adds'].append((p['tick'], p['id'], not seen_tick and p['tick'] == s['header']['first_tick']))
            else:
                s['removes'].append((p['tick'], p['id']))
        elif tag == jt.TAG_INPUT:
            tick, player, _, flags = _INPUT.unpack_from(payload, 0)
            if not flags & jt.INPUT_FRESH:
                s['subst'][player].append(tick)
        elif tag == jt.TAG_TICK:
            tick = _U32.unpack_from(payload, 0)[0]
            ms = _U32.unpack_from(payload, 24)[0]
            seen_tick = True
            if s['first_tick'] is None:
                s['first_tick'] = tick
            s['last_tick'], s['last_ms'] = tick, ms
            if tick % 66 == 0:
                s['tick_ms'][tick] = ms
        elif tag == jt.TAG_NOTE:
            c = jt._Cursor(payload)
            tick, text = c.u32(), c.string()
            m = RESYNC_NOTE.match(text)
            if m:
                s['resyncs'].append((int(m[1]), int(m[3]), int(m[5]) + int(m[6])))
            m = LEAD_NOTE.match(text)
            if m:
                s['leads'].append((int(m[1]), int(m[2])))
            m = LATENESS_NOTE.match(text)
            if m and m[1] == 'raise':
                s['raises'].append(int(m[2]))
            m = LEAVER_NOTE.match(text)
            if m:
                s['leaves'][int(m[1])].append(tick)
            if text.startswith('end: '):
                s['ends'].append(text[5:])
            if text.startswith('cap: '):
                s['capped'] = True
    return s


def ms_at(s, tick):
    """The ms of `tick` (a TICK record each 66 ticks; interpolated at 15.15 ms a tick between)."""
    base = tick - tick % 66
    while base not in s['tick_ms'] and base > (s['first_tick'] or 0):
        base -= 66
    return s['tick_ms'].get(base, 0) + (tick - base) * 1000.0 / 66.0


def summarize(s):
    """The row of one session; every value with where it came from."""
    h = s['header']
    stats = s['stats']
    typed = s['schema'] is not None
    row = {'session': h['session'], 'level': h['level'], 'delay': h['input_delay'],
           'start': datetime.datetime.utcfromtimestamp(h['utc_ms'] / 1000.0).strftime('%m-%d %H:%M:%S'),
           'minutes': s['last_ms'] / 60000.0, 'capped': s['capped'], 'typed': typed}
    ready = stats.get('ready')
    founders = [p for t, p, f in s['adds'] if f]
    row['n0'] = ready[0]['values']['players'] if ready else len(founders)
    start_tick = ready[0]['values']['first_tick'] if ready else s['first_tick'] or 0
    # members over time: the founders from the first tick, then every add and remove
    changes = sorted([(t, +1) for t, p, f in s['adds'] if not f] + [(t, -1) for t, p in s['removes']])
    count, peak, area, prev = len(founders), len(founders), 0.0, ms_at(s, start_tick)
    for tick, d in changes:
        now = ms_at(s, tick)
        area += count * max(0.0, now - prev)
        prev, count = now, count + d
        peak = max(peak, count)
    area += count * max(0.0, s['last_ms'] - prev)
    row['peak'] = peak
    row['player_hours'] = area / 3600000.0
    link = stats.get('link')
    row['rtt'] = link[0]['values']['worst_ms'] if link else None
    states = [(r['values']['player'], r['values']['base']) for r in stats['resync_state']] if typed \
        else [(p, b) for p, b, _ in s['resyncs']]
    # Resyncs (the log's 'P resynced at tick B' lines): the requests as recorded, or (before step JNL) every
    # state sent but the first of a life that did not found the session (a late join's or a rejoin's import).
    founders_set = set(founders)
    adds = collections.defaultdict(list)
    for t, p, f in s['adds']:
        adds[p].append(t)
    imports = set()
    for player in adds:
        for life in adds[player]:
            if player in founders_set and life == s['header']['first_tick']:
                continue
            first = min([k for k, (p, b) in enumerate(states) if p == player and b >= life], default=None)
            if first is not None:
                imports.add(first)
    requests = stats.get('resync_request')
    # (from the step on: a member's SessionResync and the start handshake's - the server's own count, the end
    # line's 'X resyncs'; a base the runner stepped first is answered again, 'again', and not counted twice)
    resynced = [(r['values']['player'], r['values']['base']) for r in requests
                if r['values']['why'] in ('resync', 'start')] \
        if typed and requests is not None else [x for k, x in enumerate(states) if k not in imports]
    row['resyncs'] = len(resynced)
    row['subst_bound'] = not typed and bool(resynced)
    last, tight = {}, 0
    for player, base in resynced:
        if player in last and 0 <= base - last[player] <= 198:
            tight += 1
        last[player] = base
    row['tight'] = tight
    wire = stats.get('resync_wire')
    row['wire_mb'] = sum(r['values']['wire_bytes'] for r in wire) / 1e6 if wire is not None and typed else None
    # S2 (the log's 'S2 held' lines): a member's substituted ticks at or after its epoch - the session's first
    # tick for a founder, else (and after each resync) the base of its state - while it is a member (not from its
    # leave until it is added again), and (recorded from step JNL) not in the span a resync leaves unnumbered:
    # after the old epoch's last counted tick, before the new base.
    resync_bases = collections.defaultdict(list)
    for player, base in states:
        resync_bases[player].append(base)
    unnumbered = collections.defaultdict(list)
    for r in stats.get('resync_request', []):
        v = r['values']
        if v['why'] != 'join':
            unnumbered[v['player']].append((v['counted_through'], v['base']))
    def numbered(player, tick):
        # the life the tick is in (its add), its epoch (a founder's first tick, else a state's base in the life)
        life = max([a for a in adds[player] if a <= tick], default=None)
        if life is None:
            return False
        founder = player in founders_set and life == s['header']['first_tick']
        if not founder and not any(life <= b <= tick for b in resync_bases[player]):
            return False
        if any(life <= leave <= tick for leave in s['leaves'].get(player, [])):
            return False
        return not any(lo < tick < hi for lo, hi in unnumbered[player])

    runs = []
    for player, ticks in s['subst'].items():
        ticks = sorted(t for t in ticks if numbered(player, t))
        first = prev_t = None
        for t in ticks:
            if first is not None and t == prev_t + 1:
                prev_t = t
                continue
            if first is not None:
                runs.append((player, prev_t - first + 1))
            first = prev_t = t
        if first is not None:
            runs.append((player, prev_t - first + 1))
    row['subst'] = sum(n for _, n in runs)
    row['runs132'] = sum(1 for _, n in runs if n >= 132)
    row['run_lengths'] = [n for _, n in runs]
    step = stats.get('step')
    row['step'] = (pct([r['values']['p50_us'] for r in step], 50), max(r['values']['p99_us'] for r in step),
                   max(r['values']['max_us'] for r in step)) if step else None
    lateness = stats.get('lateness')
    row['raises'] = sum(1 for r in lateness if r['values']['raise']) if typed and lateness is not None else \
        len(s['raises']) + sum(1 for _, k in s['leads'] if k > 0)
    row['end'] = end_category(s['ends'][-1]) if s['ends'] else 'no end note (the server stopped first)'
    row['resync_players'] = collections.Counter(p for p, _ in resynced)
    return row


def fmt(v, form='%s'):
    return '-' if v is None else form % v


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('inputs', nargs='+')
    parser.add_argument('--out')
    parser.add_argument('--players', action='store_true', help='the per-player table (names: stays local)')
    args = parser.parse_args(argv)
    paths = []
    for item in args.inputs:
        p = pathlib.Path(item)
        paths += sorted(p.rglob('*.bmjs')) if p.is_dir() else [p]
    rows, sessions = [], []
    for path in paths:
        s = session_of(path)
        sessions.append(s)
        rows.append(summarize(s))
    out = open(args.out, 'w', encoding='utf-8') if args.out else sys.stdout
    P = lambda *a: print(*a, file=out)  # noqa: E731
    P('SESSIONS (from the server journals; start UTC)')
    P('%3s %3s %14s %6s %4s %5s %4s %4s %6s %6s %5s %6s %6s %8s %13s %7s %5s %s' % (
        'sid', 'lvl', 'start', 'min', 'N0', 'peak', 'dly', 'rtt', 'pl-h', 'resync', 'tight', 'MB', 'subst', 'run>=132',
        'stp50/99', 'stpmax', 'raise', 'end'))
    tot = collections.Counter()
    for r in rows:
        stp = r['step']
        P('%3d %3d %14s %6.1f %4d %5d %4d %4s %6.2f %6d %5d %6s %6s %8s %13s %7s %5d %s%s' % (
            r['session'], r['level'], r['start'], r['minutes'], r['n0'], r['peak'], r['delay'], fmt(r['rtt']),
            r['player_hours'], r['resyncs'], r['tight'], fmt(r['wire_mb'], '%.1f'),
            ('<=%d' if r['subst_bound'] else '%d') % r['subst'], ('<=%d' if r['subst_bound'] else '%d') % r['runs132'],
            '%s/%s' % stp[:2] if stp else '-', fmt(stp[2] if stp else None), r['raises'], r['end'],
            ' (capped)' if r['capped'] else ''))
        for key in ('minutes', 'player_hours', 'resyncs', 'tight', 'subst', 'runs132'):
            tot[key] += r[key]
        tot['wire_mb'] += r['wire_mb'] or 0.0
    runs = [n for r in rows for n in r['run_lengths']]
    P('TOTAL sessions=%d minutes=%.1f player-hours=%.2f resyncs=%d tight=%d MB=%s subst_ticks=%d runs>=132=%d' % (
        len(rows), tot['minutes'], tot['player_hours'], tot['resyncs'], tot['tight'],
        '%.1f' % tot['wire_mb'] if any(r['wire_mb'] is not None for r in rows) else '-', tot['subst'], tot['runs132']))
    if runs:
        P('substitution run length (merged) n=%d p50=%s p90=%s p99=%s max=%s; >=66: %d; >=300: %d' % (
            len(runs), pct(runs, 50), pct(runs, 90), pct(runs, 99), max(runs), sum(1 for x in runs if x >= 66),
            sum(1 for x in runs if x >= 300)))
    untyped = sum(1 for r in rows if not r['typed'])
    P('legend: rtt, MB and stp* are recorded from step JNL on (link, resync_wire, step); %d of %d journals predate it '
      '(-). raise: the lateness records, or (before) the lead notes with +k and the observe-only lateness notes. '
      'subst / run>=132 "<=": a journal before the step cannot tell the ticks a resync leaves unnumbered (between '
      'the request and the new base, resync_request records them from the step on), so it counts them too: an '
      'upper bound. N0: the ready record, or (before) the founding PLAYER records. end: the reason\'s category '
      '(a reason that names a member is never printed).' % (untyped, len(rows)))
    if args.players:
        P('\nPER PLAYER')
        per = collections.defaultdict(collections.Counter)
        for s, r in zip(sessions, rows):
            for player, n in r['resync_players'].items():
                per[s['players'].get(player, 'p%d' % player)]['resyncs'] += n
        for name, c in sorted(per.items(), key=lambda kv: -kv[1]['resyncs']):
            P('%-16s resyncs %d' % (name[:16], c['resyncs']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
