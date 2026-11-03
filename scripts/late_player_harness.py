"""Revision-3 scenario recipes and diagnostics used by arena_map_test.py.

No network simulation policy lives here. Faults use the harness's live-tick
milestones; a server controller only starts/stops the selected test server.
"""
import json
import math
import pathlib
import re
import statistics
import subprocess
import sys

ARMS = ('A0', 'A0J', 'A0H', 'A1', 'A2', 'A3', 'A3X', 'A3L', 'A3M', 'A4',
        'A5', 'A5U', 'A5S', 'A5U2', 'A5U3', 'A5S2', 'A5UB', 'A5D', 'A5P',
        'A6', 'A6M', 'A6B', 'A7', 'A8', 'A9', 'A9C', 'A9H8', 'A9H20', 'A10', 'A10X', 'A11', 'A11U')
FAMILIES = {'A0H': 'control', 'A3M': 'minority', 'A5U2': 'lead', 'A5U3': 'lead',
            'A5S2': 'lead', 'A5UB': 'lead', 'A5P': 'pair', 'A9H8': 'clock', 'A9H20': 'clock',
            'A10': 'shared', 'A10X': 'shared', 'A11': 'burst', 'A11U': 'lead'}

def recipe(name, variant='lane', hold='300'):
    name = name.upper()
    if name not in ARMS:
        raise ValueError('unknown scenario ' + name)
    n = {'A5U2': 2, 'A5S2': 2, 'A5U3': 3, 'A11': 1, 'A11U': 1}.get(name, 4)
    p = min(2, n - 1)
    out = {'players': n, 'latency_ms': 150, 'links': [], 'events': [], 'client_args': [],
           'join_late': [], 'clock_skew': '', 'sectors': '4' if variant == 'swing' else '1'}
    def event(client, kind, ms=0, tick=1500, extra=''):
        out['events'].append('%d:%d:%s=%s%s' % (client, tick, kind, ms, extra))
    if name in ('A0', 'A0J', 'A0H', 'A4'):
        event(p, 'none')
        if name == 'A0J': out['links'] = ['%d:jitter=45' % i for i in range(n)]
        if name == 'A0H': out['links'] = ['%d:lat=%d' % (i, d) for i, d in enumerate((50, 150, 150, 250))]
        if name == 'A4': out['links'] = ['%d:loss=10,burst=8' % p]
    elif name in ('A1', 'A2', 'A3', 'A3X', 'A3L', 'A11'):
        ms = {'A1': 300, 'A2': 600, 'A3': 200, 'A3X': 200, 'A3L': 1000, 'A11': 300}[name]
        event(p, 'blackout', ms, extra=':dir=' + ('up' if name in ('A1', 'A2', 'A11') else 'both'))
        if name == 'A3X': out['latency_ms'] = 50
    elif name in ('A5', 'A5U', 'A5S', 'A5U2', 'A5U3', 'A5S2', 'A5UB', 'A5D', 'A5P', 'A3M', 'A10', 'A10X', 'A11U'):
        targets = [2, 3] if name == 'A5P' else ([1, 2, 3] if name == 'A3M' else (list(range(n)) if name in ('A10', 'A10X') else [p]))
        ms = 100 if name in ('A5S', 'A5S2') else (450 if name == 'A10X' else 300)
        direction = 'both' if name in ('A5', 'A5S', 'A5S2') else 'up'
        for client in targets:
            event(client, 'step', ms, extra=':dir=' + direction + (':until=2700' if name == 'A5D' else ''))
        if name == 'A5UB': event(0, 'spike', 300, 2100, ':dir=up:dur=1000')
    elif name in ('A6', 'A6M', 'A6B'):
        out['client_args'] = ['%d:--hold-event %s:2:%s' % (p, 'pair' if name == 'A6B' else 'physicalize', hold)]
        if name == 'A6M': out['latency_ms'] = 50
    elif name == 'A7':
        event(2, 'blackout', 300, extra=':dir=up')
        event(3, 'blackout', 300, 1513, ':dir=up')
    elif name == 'A8':
        out['join_late'], out['links'] = ['3:1500'], ['3:lat=250']
    elif name == 'A9': event(p, 'spike', 300, extra=':dir=up:dur=1000')
    elif name == 'A9C': event(p, 'stall', 500)
    elif name in ('A9H8', 'A9H20'):
        ms = 8000 if name == 'A9H8' else 20000
        out['clock_skew'] = '300:%d:7.5' % ms
        for client in range(n): event(client, 'none', 0, 300, ':dur=%d' % (2 * ms))
    return out

def apply_scenario(args):
    if args.scenario:
        r = recipe(args.scenario, args.variant, args.hold)
        if args.players is not None and args.players != r['players']:
            raise ValueError('%s requires %d clients' % (args.scenario, r['players']))
        args.players = r['players']
        args.arm = args.scenario.upper()
        if args.latency_ms is None: args.latency_ms = r['latency_ms']
        if args.sectors is None: args.sectors = r['sectors']
        if args.checkpoint_ticks is None: args.checkpoint_ticks = 2
        args.drive = True
        if args.variant == 'lane': args.keep_lane = True
        elif not args.beam_bays: args.beam_bays = ','.join(['18'] * args.players)
        args.link = r['links'] + args.link
        args.event = r['events'] + args.event
        args.client_arg_for_specs = r['client_args'] + args.client_arg_for_specs
        args.join_late = r['join_late'] + args.join_late
        args.test_clock_skew = args.test_clock_skew or r['clock_skew']
    if args.players is None: args.players = 4
    if args.latency_ms is None: args.latency_ms = 0.0
    if args.sectors is None: args.sectors = '1,2,3,4'
    if args.checkpoint_ticks is None: args.checkpoint_ticks = 660
    if not 1 <= args.players <= 8: raise ValueError('--clients must be between 1 and 8')
    if args.test_clock_skew:
        parts = args.test_clock_skew.split(':')
        if len(parts) != 3: raise ValueError('--test-clock-skew expects T:MS:PCT')
        tick, ms, pct = int(parts[0]), int(parts[1]), float(parts[2])
        if not 0 <= tick <= 0xffffffff or not 0 < ms <= 600000 or not math.isfinite(pct) or not 0 < pct < 100:
            raise ValueError('invalid clock-skew span or rate')
        # Assignment instants deliberately stay on the true clock. These
        # standard fault recipes cannot join/re-anchor inside the skew span.
        if args.join_late or any('--resync-at' in value for value in args.client_arg_for_specs):
            raise ValueError('clock-skew scenarios must place joins/resyncs outside the fault; use separate runs')

def server_control(args, action, out):
    config = {'patience': args.patience == 'on', 'lead_adjust': args.lead_adjust == 'on',
              'test_clock_skew': args.test_clock_skew}
    with pathlib.Path(out).open('wb') as log:
        subprocess.run([sys.executable, '-X', 'utf8', '-I', args.server_control, action, json.dumps(config)],
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)

def twins(args, argv):
    if args.external_server and not args.server_control:
        raise ValueError('--twins with an external server requires --server-control to apply both flag configurations')
    script = pathlib.Path(__file__).with_name('arena_map_test.py')
    raw = [v for v in argv if v != '--twins']
    paths, statuses = [], []
    for mode in ('off', 'on'):
        label = args.label + '_' + mode
        path = pathlib.Path(args.out) / label
        if path.exists(): raise ValueError('twin output already exists: ' + str(path))
        p = mode if args.twin_flags in ('both', 'patience') else 'off'
        lead = mode if args.twin_flags in ('both', 'lead') else 'off'
        cmd = [sys.executable, '-X', 'utf8', '-I', str(script), *raw,
               '--label', label, '--patience', p, '--lead-adjust', lead]
        if mode == 'on': cmd += ['--control', str(paths[0])]
        statuses.append(subprocess.run(cmd, timeout=590).returncode)
        paths.append(path)
    off = json.loads((paths[0] / 'summary.json').read_text())
    on = json.loads((paths[1] / 'summary.json').read_text())
    comparison = {'scenario': args.scenario or args.arm, 'flags': args.twin_flags, 'exit_codes': statuses, 'sectors': []}
    for a, b in zip(off, on):
        comparison['sectors'].append({'sector': a['sector'], 'off': a['clients'], 'on': b['clients'],
                                      'criteria': b.get('criteria', [])})
    target = pathlib.Path(args.out) / (args.label + '_twins.json')
    target.write_text(json.dumps(comparison, indent=2), encoding='utf8')
    print('twins: ' + str(target), flush=True)
    return int(any(statuses))

def parse_s1(text, session=None):
    groups = {'quorum': [], 'lateness': [], 'sector': [], 'arrival': [], 'clock-skew': [], 'decisions': []}
    for line in text.splitlines():
        sid = re.search(r'\[session (\d+)\]', line)
        if session is not None and sid and int(sid[1]) != session: continue
        m = re.search(r'\bS1 (quorum|lateness|sector|arrival|clock-skew|lead|would-raise2|would-lower2)(?:\s|$)(.*)', line)
        if not m: continue
        fields = dict(re.findall(r'([\w-]+)=([^\s;,]+)', m[2]))
        for k, value in list(fields.items()):
            try: fields[k] = int(value) if re.fullmatch(r'-?\d+', value) else float(value)
            except ValueError: pass
        if m[1] == 'clock-skew': fields['phase'] = m[2].strip().split()[0]
        kind = m[1]
        if kind in ('would-raise2', 'would-lower2'):
            fields['action'], kind = kind, 'decisions'
        if kind != 'lead': groups[kind].append(fields)
        if kind != 'sector' and any(k in fields for k in ('sector-late', 'union', 'sector-margin')):
            groups['sector'].append(fields)
    return {key: value or None for key, value in groups.items()}

def host_clock(s1, client_cadence, server_cadence=None):
    spans, begin, recover = [], None, None
    for row in s1.get('clock-skew') or []:
        if row.get('phase') == 'begin': begin, recover = row, None
        elif row.get('phase') == 'recover': recover = row.get('tick')
        elif row.get('phase') == 'end' and begin:
            spans.append({'first': begin['tick'], 'last': row['tick'], 'recover': recover,
                          'source': 'injected', 'percent': begin.get('percent'), 'ms': begin.get('ms')})
            begin = None
    if begin:
        spans.append({'first': begin['tick'], 'last': None, 'recover': recover, 'source': 'injected-incomplete'})
    # An un-injected host fault must be supported by all members' negative
    # slack at the same reporting tick and normal client tick cadence.
    grouped = {}
    for row in s1.get('lateness') or []:
        if 'tick' in row and 'player' in row and 'q50' in row:
            grouped.setdefault(row['tick'], {})[row['player']] = row['q50']
    normal = bool(client_cadence) and all(v is not None and 14 <= v <= 16.5 for v in client_cadence.values())
    common = [tick for tick, rows in grouped.items() if len(rows) == len(client_cadence) and all(v < 0 for v in rows.values())]
    # A shared network delay (A10) also makes all slack negative. Require
    # faster server tick cadence as independent evidence before blaming its clock.
    common_fast = [tick for tick in common if (server_cadence or {}).get(tick, math.inf) < 14.8]
    if not spans and normal:
        for tick in sorted(common_fast):
            if spans and tick - spans[-1]['last'] <= 660: spans[-1]['last'] = tick
            else: spans.append({'first': max(0, tick - 659), 'last': tick, 'source': 'observed', 'resolution_ticks': 660})
    return {'label': 'host clock' if spans else None, 'spans': spans,
            'client_tick_ms_p50': client_cadence, 'client_cadence_normal': normal, 'common_negative_ticks': common}

def cadence(records):
    gaps = [(b['ms'] - a['ms']) / (b['tick'] - a['tick']) for a, b in zip(records, records[1:])
            if b['tick'] > a['tick'] and b['ms'] >= a['ms']]
    return statistics.median(gaps) if gaps else None
