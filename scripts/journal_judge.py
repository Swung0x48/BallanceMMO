#!/usr/bin/env python3
"""Judge one finished session from its black boxes alone.

The harness judge (arena_map_test.py --report-only) needs the harness's own
logs; a retail play session leaves nothing but the journals (.bmjs / .bmjc,
.bmjr before journal version 10) behind,
often only the clients'.  This reads those, and only those, and prints one
judgment table (and, with --json, writes it): every row a check, a verdict
(PASS / FAIL / REPORTED / N/A) and the numbers behind it.

    1. headers     the files describe one session (session id, level, seed,
                   anchor, world options, placement, slots); build id, input
                   delay and journal version are reported.  The journal holds
                   no protocol number: the journal version stands in for it.
    2. replay      the server journal replays tick for tick in the SimTool
                   (--simtool, --root, --map); N/A without them.
    3. bodies      each client against the server, body by body, at every tick
                   both checkpointed: FINAL rows when the client wrote them
                   (every version 10 client journal; before it only with the
                   headless --final-checkpoints), else the live LOCAL rows; a
                   version 10 client journal without them fails.
                   The first tick any shared body is off by more than the
                   tolerance, the worst body, the own ball against the
                   harness's "own ball 0.0000", the bodies only one side has,
                   the drift per window.  A ball in its airship span (BALA:
                   per world, never compared) is left out, as the harness does.
                   Live LOCAL rows hold the predictions a later rewind
                   replaced (session-client-core-design, step D0), so on them
                   these are REPORTED; on FINAL rows they are PASS / FAIL.
                   PE_Balloon and PE_Box_slide (resync_rules.hpp's keep-local
                   bodies, the user's "fix everything except the balloon") are
                   left out (--exclude) and reported on their own.
    4. health      resyncs (a test-requested one and a join import are
                   reported, any other fails, as arena_map_test.verdict has
                   it), corrections by kind (hard fails), the server's
                   substituted ticks per player (verdict's starvation rule),
                   each client's own fresh / substituted ticks, WAIT changes,
                   the end note, tick ranges and journal damage.
    5. events      the lifecycle events each client journalled for its own
                   player against the events the server applied (player,
                   type, origin tick, ball type); a miss on a tick the other
                   side's journal covers fails.  The server's own leaver
                   teardown (its NOTEs say so) is not the client's to have.

Without a server journal (client only) 1 and 4 run, and 3 against the server
snapshots the client received (RECEIVED rows), REPORTED; the rest is N/A.
Client journals are grouped into sessions by session id, level, anchor,
shared members and overlapping wall-clock spans, and each group is judged on
its own.

The thresholds are the project's, not new ones: the tolerance is
first_divergence.py's 0.05 m (any axis), the own ball's 0.00005 m and the
48-tick reach are arena_map_test.py's ZERO_M and REACH, the end cut (a
client's last 60 ticks are its exit's) and the starvation rule
(> max(20, 1 % of the ticks), or a run of more than 100) are
arena_map_test.event_report's and verdict()'s, the drift window is
server_client_drift.py's 500 ticks.

usage: python -X utf8 -I scripts/journal_judge.py RUN_DIR [RUN_DIR ...]
       python -X utf8 -I scripts/journal_judge.py --server server.bmjs --clients c0.bmjc c1.bmjc ...
           [--json OUT] [--tolerance 0.05]
           [--simtool BallanceMMOSimTool.exe --root GAME_DIR [--map MAP.nmo]] [--replay-dir DIR]
       python -X utf8 -I scripts/journal_judge.py --self-test

A RUN_DIR's journals (*.bmjs, *.bmjc, *.bmjr; top level only) are sorted by their header into
the server's and the clients'.  Without --map, a server whose start note names
a custom map is replayed with <root>/ModLoader/Maps/<that map> when it exists.
Exit code 1 when any row FAILs, 0 otherwise.
"""
import argparse
import collections
import json
import math
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_trace as jt
import first_divergence as fd

PASS, FAIL, REPORTED, NA = 'PASS', 'FAIL', 'REPORTED', 'N/A'

TOLERANCE = 0.05       # first_divergence.py --tolerance default (metres, any axis)
ZERO_M = 0.00005       # arena_map_test.ZERO_M: "own ball 0.0000" (euclidean)
REACH = 48             # arena_map_test.REACH: the rollback reach in ticks
END_CUT = 60           # arena_map_test.event_report: a client's span ends 60 ticks before its last tick
DRIFT_WINDOW = 500     # server_client_drift.py --window default
REPLAY_TIMEOUT = 1800  # arena_map_test.replay
SESSION_GAP_MS = 60000 # client-only grouping: wall-clock spans this close count as overlapping
# resync_rules.hpp keep_local_body_prefixes: the balloon and the slide jointed to it, the user's
# "fix everything except the balloon" (lockstep-without-snapshots.md: exception c)
DEFAULT_EXCLUDE = ['PE_Balloon', 'PE_Box_slide']
FINAL_BY_DEFAULT = 10  # journal.hpp: the version from which every client journal has FINAL rows

EVENT_SUPERSEDED, EVENT_REJECTED = 2, 4   # entity/session.hpp EVENT_STATE_*

RE_AIRSHIP = re.compile(r'^airship: (?:p(\d+)|(own ball)) in the airship zone at tick (\d+)')
RE_LEAVER = re.compile(r'^leaver: p(\d+) Unphysicalize relayed for tick (\d+)')
RE_LEAVER_TRAFO = re.compile(r'^trafo: server \w+ reset of p(\d+)(?:L\d+)? for tick (\d+) \(leaver\)')
RE_RESYNC_REQUEST = re.compile(r'^resync: requested \((.*)\)$')
RE_CUSTOM_MAP = re.compile(r'custom map (\S+\.nmo)')
RE_SUMMARY_TICKS = re.compile(r'ticks=(\d+) matched=(\d+) first_divergence=(-?\d+)')


class Table(object):
    """The judgment: rows of (scope, check, verdict, detail)."""

    def __init__(self, name):
        self.name = name
        self.rows = []

    def add(self, scope, check, verdict, detail=''):
        self.rows.append({'scope': scope, 'check': check, 'verdict': verdict, 'detail': detail})

    def verdict(self):
        return FAIL if any(r['verdict'] == FAIL for r in self.rows) else PASS

    def print(self):
        print('=== %s: %s' % (self.name, self.verdict()))
        width = max([len(r['scope']) for r in self.rows] + [5])
        cwidth = max([len(r['check']) for r in self.rows] + [5])
        for r in self.rows:
            print('  %-*s  %-*s  %-8s  %s' % (width, r['scope'], cwidth, r['check'], r['verdict'], r['detail']))


# -- loading -----------------------------------------------------------------

def load(path):
    """A journal, with a short name."""
    journal = jt.read_journal(str(path))
    journal.name = pathlib.Path(path).name
    return journal


def last_tick(journal):
    return journal.tick_range()[1] or 0


def member_ids(journal):
    return {p['id'] for p in journal.players()}


def wall_span(journal):
    """(first, last) wall-clock ms of the file: the header's time plus the last TICK's ms."""
    last = journal.last_record['ms'] if journal.last_record else 0
    return journal.header['utc_ms'], journal.header['utc_ms'] + last


def group_sessions(clients):
    """Client journals of one session together: same session id, level and
    anchor, a member in common and wall-clock spans that overlap."""
    groups = []
    for journal in sorted(clients, key=lambda j: j.header['utc_ms']):
        h = journal.header
        lo, hi = wall_span(journal)
        for group in groups:
            g = group[0].header
            same = (g['session'], g['level'], g['anchor_hash']) == (h['session'], h['level'], h['anchor_hash'])
            glo = min(wall_span(j)[0] for j in group)
            ghi = max(wall_span(j)[1] for j in group)
            members = set().union(*(member_ids(j) for j in group))
            if same and members & member_ids(journal) and lo <= ghi + SESSION_GAP_MS and glo <= hi + SESSION_GAP_MS:
                group.append(journal)
                break
        else:
            groups.append([journal])
    return groups


# -- 1. headers --------------------------------------------------------------

MUST_MATCH = ('session', 'level', 'seed', 'spawn_impulse', 'anchor_hash', 'anchor_surfaces')
SINCE_VERSION = (('world_options', 5), ('mechanism_slots', 8))   # absent (read as 0) before that version
REPORT_ONLY = ('build_id', 'input_delay', 'checkpoint_ticks', 'file_version')


def header_value(journal, field):
    return journal.version if field == 'file_version' else journal.header[field]


def check_headers(table, reference, others):
    """Each file against the reference (the server, else the first client)."""
    h = reference.header
    table.add(reference.name, 'header', REPORTED,
              'session %d level %d build %s journal v%d world_options %08x placement %016x slots %d'
              % (h['session'], h['level'], h['build_id'], reference.version, h['world_options'],
                 h['anchor_placement'], h['mechanism_slots']))
    for journal in others:
        bad, noted = [], []
        for field in MUST_MATCH:
            if journal.header[field] != reference.header[field]:
                bad.append('%s %s vs %s' % (field, journal.header[field], reference.header[field]))
        for field, version in SINCE_VERSION:
            both = min(journal.version, reference.version) >= version
            if both and journal.header[field] != reference.header[field]:
                bad.append('%s %s vs %s' % (field, journal.header[field], reference.header[field]))
        # 0 is "not computed" (a client host without CAP_PLACEMENT, an old file)
        a, b = journal.header['anchor_placement'], reference.header['anchor_placement']
        if a and b and a != b:
            bad.append('anchor_placement %016x vs %016x' % (a, b))
        for field in REPORT_ONLY:
            a, b = header_value(journal, field), header_value(reference, field)
            if a != b:
                noted.append('%s %s vs %s' % (field, a, b))
        detail = '; '.join(bad + noted) or 'matches %s' % reference.name
        table.add(journal.name, 'header vs %s' % reference.name, FAIL if bad else PASS, detail)


# -- 2. replay ---------------------------------------------------------------

def custom_map(server):
    for note in server.notes:
        if note['text'].startswith('start:'):
            m = RE_CUSTOM_MAP.search(note['text'])
            if m:
                return m.group(1)
    return ''


def check_replay(table, server, args):
    if server is None:
        table.add('server', 'replay', NA, 'no server journal')
        return
    if not args.simtool:
        table.add('server', 'replay', NA, 'no --simtool given')
        return
    if not pathlib.Path(args.simtool).exists() or not args.root:
        table.add('server', 'replay', NA, 'SimTool %s missing or no --root' % args.simtool)
        return
    map_path = args.map
    wanted = custom_map(server)
    if not map_path and wanted:
        candidate = pathlib.Path(args.root) / 'ModLoader' / 'Maps' / wanted
        if not candidate.exists():
            table.add('server', 'replay', NA, 'the session ran custom map %s: give --map' % wanted)
            return
        map_path = str(candidate)
    # absolute: the replay runs in its own working directory
    cmd = [str(pathlib.Path(args.simtool).resolve()), '--root', args.root,
           '--replay-session', str(pathlib.Path(server.path).resolve())]
    cmd += ['--map', map_path] if map_path else []
    cmd += ['--report-every', '5000']
    with tempfile.TemporaryDirectory(prefix='journal_judge_') as scratch:
        out_dir = args.replay_dir or scratch
        pathlib.Path(out_dir).mkdir(parents=True, exist_ok=True)
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, cwd=out_dir, timeout=REPLAY_TIMEOUT)
        except subprocess.TimeoutExpired:
            table.add('server', 'replay', FAIL, 'timed out after %d s' % REPLAY_TIMEOUT)
            return
    summary = [l for l in (proc.stdout + proc.stderr).splitlines() if l.startswith('summary:')]
    line = summary[-1] if summary else ''
    m = RE_SUMMARY_TICKS.search(line)
    # arena_map_test.verdict: exit 0 and first_divergence=-1
    ok = proc.returncode == 0 and m is not None and m.group(3) == '-1'
    table.add('server', 'replay', PASS if ok else FAIL,
              'exit %d %s%s' % (proc.returncode, line or 'no summary line',
                                ' (map %s)' % pathlib.Path(map_path).name if map_path else ''))


# -- 3. bodies ---------------------------------------------------------------

def airship_spans(journal, events_journal=None):
    """player -> [(zone tick, next birth tick)): the ball's airship span (BALA),
    from the journal's airship NOTEs to the player's next Physicalize, as
    arena_map_test.airship_spans has it from the server log."""
    own = journal.header['own_player']
    zones = collections.defaultdict(set)
    for note in journal.notes:
        m = RE_AIRSHIP.match(note['text'])
        if m:
            zones[int(m.group(1)) if m.group(1) else own].add(int(m.group(3)))
    births = collections.defaultdict(list)
    for e in (events_journal or journal).events():
        if e['type'] == 0:
            births[e['id']].append(e['tick'])
    spans = {}
    for player, ticks in zones.items():
        spans[player] = [(z, min([b for b in births[player] if b > z] or [1 << 31])) for z in sorted(ticks)]
    return spans


def in_spans(spans, tick):
    return any(lo <= tick < hi for lo, hi in spans)


def join_import_end(journal):
    """The applied tick of a join's import (a late joiner's or a rejoin's
    first state: a resync from old tick 0, arena_map_test RE_JOIN_IMPORT),
    else None."""
    for record in resyncs(journal):
        if record['tick'] + record['delta'] == 0:
            return record['applied_at']
    return None


def trim(world, spans, lo, hi):
    """The rows in [lo, hi] without the balls in their airship spans; also the
    number of ball rows left out."""
    out, dropped = {}, 0
    for tick, row in world.items():
        if not lo <= tick <= hi:
            continue
        kept = {k: v for k, v in row.items() if not (k[0] == 'ball' and in_spans(spans.get(k[1], ()), tick))}
        dropped += len(row) - len(kept)
        out[tick] = kept
    return out, dropped


def compare_bodies(authority, client, own, tolerance):
    """Everything check 3 reports, from one pass over the shared ticks."""
    first, shared_count, _ = fd.compare(authority, client, tolerance)
    shared = sorted(set(authority) & set(client))
    worst = (0.0, None, None)
    own_max, own_first, own_bad = 0.0, None, 0
    windows = collections.OrderedDict()
    only_authority, only_client = collections.Counter(), collections.Counter()
    mechanisms = set()
    for tick in shared:
        a, b = authority[tick], client[tick]
        for key in set(a) - set(b):
            only_authority[key] += 1
        for key in set(b) - set(a):
            only_client[key] += 1
        start = tick - tick % DRIFT_WINDOW
        for key in sorted(set(a) & set(b)):
            if key[0] == 'body':
                mechanisms.add(key)
            err = max(abs(a[key][k] - b[key][k]) for k in range(3))
            windows[start] = max(windows.get(start, 0.0), err)
            if err > worst[0]:
                worst = (err, key, tick)
            if key == ('ball', own):
                dist = math.dist(a[key], b[key])
                own_max = max(own_max, dist)
                if dist >= ZERO_M:
                    own_bad += 1
                    if own_first is None:
                        own_first = tick
    return {'first': first, 'shared': shared_count, 'ticks': (shared[0], shared[-1]) if shared else None,
            'worst': worst, 'own_max': own_max, 'own_first': own_first, 'own_bad': own_bad,
            'windows': windows, 'only_authority': only_authority, 'only_client': only_client,
            'mechanisms': len(mechanisms)}


def drift_trend(windows, tolerance):
    """growing: the last window is the run's worst and over the tolerance (the
    error never came back down); bounded otherwise."""
    values = list(windows.values())
    if len(values) < 2:
        return 'one window'
    return 'growing' if values[-1] > tolerance and values[-1] >= max(values) else 'bounded'


def top(counter, n=4):
    ranked = sorted(counter.items(), key=lambda kv: (-kv[1], kv[0]))[:n]   # a tie in a fixed order
    return ', '.join('%s x%d' % (fd.show(k), v) for k, v in ranked)


def resync_windows(journal):
    return [(r['tick'], r['applied_at']) for r in resyncs(journal)]


def split_excluded(world, exclude):
    """(the rows without the bodies `exclude` names, the rows of those bodies alone)."""
    kept = {t: {k: v for k, v in row.items() if not fd.excluded(k, exclude)} for t, row in world.items()}
    held = {t: {k: v for k, v in row.items() if fd.excluded(k, exclude)} for t, row in world.items()}
    return kept, held


def check_bodies(table, server, client, tolerance, end_cut, exclude):
    """Check 3 for one client: against the server's FULL rows, or (client
    only) against the snapshots it received."""
    scope = client.name
    final = any(cp['flags'] & fd.FINAL for cp in client.checkpoints())
    rows = fd.worlds(client, want_local=True, final=final)
    if server is not None:
        authority = fd.worlds(server, want_local=False)
        spans = airship_spans(server)
        against = 'the server FULL rows'
    else:
        # Every non-LOCAL row a client writes is the server's state as it
        # arrived: a snapshot (RECEIVED) or a resync state (RESYNC).
        authority = fd.worlds(client, want_local=False)
        spans = airship_spans(client)
        against = 'its RECEIVED/RESYNC rows (no server journal)'
    imported = join_import_end(client)
    lo = imported + 1 if imported is not None else -(1 << 31)
    hi = last_tick(client) - end_cut
    authority, _ = trim(authority, spans, lo, hi)
    rows, dropped = trim(rows, spans, lo, hi)
    authority, held_authority = split_excluded(authority, exclude)
    rows, held_rows = split_excluded(rows, exclude)
    kind = 'FINAL' if final else 'LOCAL'
    if not final and client.version >= FINAL_BY_DEFAULT:
        table.add(scope, 'FINAL rows', FAIL, 'none in a version %d client journal (written by default since '
                  'version %d: turned off, or no rollback history)' % (client.version, FINAL_BY_DEFAULT))
    r = compare_bodies(authority, rows, client.header['own_player'], tolerance)
    judged = final and server is not None
    detail = '%s rows (%d) vs %s: %d common ticks%s; %d airship ball rows left out%s' % (
        kind, len(rows), against, r['shared'], ' %d..%d' % r['ticks'] if r['ticks'] else '', dropped,
        '; rows to %d (the join import) left out' % imported if imported is not None else '')
    table.add(scope, 'rows compared', REPORTED if r['shared'] else NA, detail)
    if not r['shared']:
        return
    verdict = (FAIL if r['first'] else PASS) if judged else REPORTED
    if r['first']:
        text = 'first > %.3f m at tick %d: %s off by %.4f m' % ((tolerance,) + r['first'])
    else:
        text = 'every shared body within %.3f m' % tolerance
    err, key, tick = r['worst']
    text += '; worst %s %.4f m at tick %d' % (fd.show(key), err, tick) if key else ''
    if not judged:
        text += ' (live rows hold predictions)' if server is not None else ''
    table.add(scope, 'first divergence', verdict, text)
    if r['own_first'] is None:
        text = 'own ball within %.5f m throughout (max %.6f m)' % (ZERO_M, r['own_max'])
    else:
        text = 'own ball max %.4f m, %d rows >= %.5f m, first at tick %d' % (
            r['own_max'], r['own_bad'], ZERO_M, r['own_first'])
        inside = [w for w in resync_windows(client) if w[0] <= r['own_first'] <= w[1]]
        if inside:
            text += ' (inside the resync re-step %d..%d)' % inside[0]
    table.add(scope, 'own ball exact', ((FAIL if r['own_first'] is not None else PASS) if judged else REPORTED),
              text)
    if server is not None:
        text = '%d ticks; server only: %s; client only: %s' % (
            len(set(t for t in authority if t in rows and set(authority[t]) != set(rows[t]))),
            top(r['only_authority']) or '-', top(r['only_client']) or '-')
        table.add(scope, 'body set', REPORTED, text)
        # verdict(): a client with no mechanism body reports no mechanism
        # error at all, which reads as perfect sync (9.28 postmortem).
        server_has = any(k[0] == 'body' for row in authority.values() for k in row)
        table.add(scope, 'mechanisms compared', FAIL if server_has and not r['mechanisms'] else PASS,
                  '%d mechanism bodies in common' % r['mechanisms'])
    if exclude:
        h = compare_bodies(held_authority, held_rows, None, tolerance)
        err, key, tick = h['worst']
        table.add(scope, 'excluded bodies', REPORTED, '%s: %s' % (
            ', '.join(exclude), 'worst %s %.4f m at tick %d' % (fd.show(key), err, tick) if key else 'none compared'))
    trend = drift_trend(r['windows'], tolerance)
    text = '%s; worst per %d ticks: %s' % (trend, DRIFT_WINDOW,
                                          ' '.join('%d:%.3f' % (s, v) for s, v in r['windows'].items()))
    table.add(scope, 'drift', REPORTED, text)


# -- 4. health ---------------------------------------------------------------

def resyncs(journal):
    return [r for g in journal.ticks for r in g.resyncs]


def runs_of(ticks):
    """Sorted ticks -> [[first, last], ...] of consecutive ticks."""
    runs = []
    for t in sorted(ticks):
        if runs and t == runs[-1][1] + 1:
            runs[-1][1] = t
        else:
            runs.append([t, t])
    return runs


def show_runs(runs, n=4):
    text = ' '.join('%d-%d' % (a, b) if a != b else str(a) for a, b in runs[:n])
    return text + (' ... (+%d)' % (len(runs) - n) if len(runs) > n else '')


def end_note(journal):
    ends = [n['text'] for n in journal.notes if n['text'].startswith('end:')]
    return ends[-1] if ends else None


def check_file(table, journal):
    """Tick range, the end note, damage."""
    lo, hi = journal.tick_range()
    end = end_note(journal)
    damaged = journal.bytes_dropped or 'does not parse' in journal.warning
    detail = 'ticks %s..%s, %s%s' % (lo, hi, end or 'no end note',
                                     ('; ' + journal.warning) if journal.warning else '')
    table.add(journal.name, 'black box', FAIL if damaged or end is None else PASS, detail)


def check_resyncs(table, journal):
    requests = [(g.tick, RE_RESYNC_REQUEST.match(n['text'])) for g in journal.ticks for n in g.notes]
    requests = [(t, m.group(1)) for t, m in requests if m]
    imported = join_import_end(journal)
    parts, verdict = [], PASS
    for r in resyncs(journal):
        reason = [text for t, text in requests if t <= r['tick']]
        if imported is not None and r['applied_at'] == imported:
            why = 'join import'
        else:
            why = reason[-1] if reason else 'no request note'
        if why == 'join import' or why.startswith('test'):
            verdict = verdict if verdict == FAIL else REPORTED
        else:
            verdict = FAIL   # arena_map_test.verdict: "resyncs" is a problem
        parts.append('S=%d base %d delta %d applied %d %s (%s)' % (
            r['tick'], r['base'], r['delta'], r['applied_at'], jt.RESYNC_OUTCOMES.get(r['outcome'], r['outcome']),
            why))
    table.add(journal.name, 'resyncs', verdict, '; '.join(parts) or 'none')


def check_corrections(table, journal):
    kinds = collections.Counter(jt.CORRECTION_KINDS.get(c['kind'], str(c['kind'])) for c in journal.corrections())
    unconfirmed = sum(1 for cp in journal.checkpoints()
                      if cp['flags'] & fd.FINAL and cp['flags'] & fd.UNCONFIRMED)
    final = sum(1 for cp in journal.checkpoints() if cp['flags'] & fd.FINAL)
    detail = ', '.join('%s %d' % kv for kv in sorted(kinds.items())) or 'none'
    if final:
        detail += '; FINAL rows %d, UNCONFIRMED %d' % (final, unconfirmed)
    # arena_map_test.verdict: hard corrections are a problem
    table.add(journal.name, 'corrections', FAIL if kinds.get('hard') else REPORTED, detail)


def own_inputs(journal):
    """(fresh ticks, substituted ticks) of the client's own player; the later
    record of a tick wins (journal.hpp: SUBSTITUTED follows FRESH)."""
    own = journal.header['own_player']
    flags = {}
    for g in journal.ticks:
        for record in g.inputs:
            if record['id'] == own:
                flags[g.tick] = record['flags']
    substituted = [t for t, f in flags.items() if f & jt.INPUT_SUBSTITUTED]
    fresh = [t for t, f in flags.items() if f & jt.INPUT_FRESH and not f & jt.INPUT_SUBSTITUTED]
    return fresh, substituted


def check_client_inputs(table, journal):
    fresh, substituted = own_inputs(journal)
    waits = [(g.tick, w['wait']) for g in journal.ticks for w in g.waits
             if w['player'] == journal.header['own_player']]
    table.add(journal.name, 'own input', REPORTED, 'fresh %d, substituted %d%s' % (
        len(fresh), len(substituted), (' (%s)' % show_runs(runs_of(substituted))) if substituted else ''))
    table.add(journal.name, 'waits', REPORTED, '%d changes%s' % (
        len(waits), (': ' + ' '.join('%d:%d' % w for w in waits[:8])) if waits else ''))


def check_server_inputs(table, server, clients, names):
    """verdict(): starved > max(20, 1 % of the ticks) is starvation, a run of
    more than 100 ticks without a fresh frame is a client running behind.
    Counted inside the ticks the player's own journals cover when there are
    any (event_report counts from the client's base: the ticks before it, a
    rejoin's gap among them, are held by construction)."""
    spans = collections.defaultdict(list)
    for journal in clients:
        spans[journal.header['own_player']].append(coverage(journal))
    per_player = collections.defaultdict(dict)
    for g in server.ticks:
        for record in g.inputs:
            mine = spans.get(record['id'])
            if not mine or any(lo <= g.tick <= hi for lo, hi in mine):
                per_player[record['id']][g.tick] = record['flags']
    for player, flags in sorted(per_player.items()):
        substituted = [t for t, f in flags.items() if not f & jt.INPUT_FRESH]
        runs = runs_of(substituted)
        worst = max([b - a + 1 for a, b in runs] or [0])
        bad = len(substituted) > max(20, 0.01 * len(flags)) or worst > 100
        table.add('server', 'inputs p%d%s' % (player, names.get(player, '')), FAIL if bad else PASS,
                  'fresh %d, substituted %d, worst run %d%s' % (
                      len(flags) - len(substituted), len(substituted), worst,
                      (' (%s)' % show_runs(runs)) if runs else ''))
    waits = sum(len(g.waits) for g in server.ticks)
    resync_notes = [n['text'] for n in server.notes if n['text'].startswith('resync state for player')]
    table.add('server', 'waits / resync states', REPORTED, '%d WAIT records, %d resync states sent' % (
        waits, len(resync_notes)))


# -- 5. events ---------------------------------------------------------------

def event_key(e):
    """journal.hpp journal_keep_last_events: the event without its applied tick."""
    return (e['id'], e['origin_tick'], e['type'], e['ball_type'], e['sector'], e['name'],
            e['flags'] if e['type'] == 5 else 0)


def own_events(journal):
    """The client's own events as the server had them: the last record of
    each event, without the ones the server superseded or rejected."""
    own = journal.header['own_player']
    last = {}
    for e in journal.events():     # tick order; an echo follows at its applied tick
        if e['id'] == own:
            last[event_key(e)] = e
    return [e for e in last.values() if not e['state'] & (EVENT_SUPERSEDED | EVENT_REJECTED)]


def match_key(e):
    return (e['id'], e['type'], e['origin_tick'], e['ball_type'])


def show_event(key):
    return '%s@%d' % (jt.EVENT_TYPES.get(key[1], key[1]), key[2])


def leaver_teardown(server):
    """(player, type, origin) the server made itself for a leaving member: on
    neither side's account (a rejoin's next life journals its old life's)."""
    out = set()
    for note in server.notes:
        m = RE_LEAVER.match(note['text'])
        if m:
            out.add((int(m.group(1)), 1, int(m.group(2))))
        m = RE_LEAVER_TRAFO.match(note['text'])
        if m:
            out.add((int(m.group(1)), 5, int(m.group(2))))
    return out


def coverage(journal):
    """The ticks a client journal saw: its first LOCAL row to its last tick."""
    local = [cp['tick'] for cp in journal.checkpoints() if cp['flags'] & fd.LOCAL]
    return (min(local) if local else journal.tick_range()[0] or 0), last_tick(journal)


def check_events(table, server, clients):
    teardown = leaver_teardown(server)
    s_lo, s_hi = server.tick_range()
    by_player = collections.defaultdict(list)
    for journal in clients:
        by_player[journal.header['own_player']].append(journal)
    for player, journals in sorted(by_player.items()):
        scope = 'p%d (%s)' % (player, ', '.join(j.name for j in journals))
        mine = collections.Counter(match_key(e) for j in journals for e in own_events(j)
                                   if (player, e['type'], e['origin_tick']) not in teardown)
        theirs = collections.Counter(match_key(e) for e in server.events() if e['id'] == player
                                     and (player, e['type'], e['origin_tick']) not in teardown)
        made = sum(1 for e in server.events() if e['id'] == player
                   and (player, e['type'], e['origin_tick']) in teardown)
        spans = [coverage(j) for j in journals]
        lost = [k for k in (mine - theirs).elements() if s_lo <= k[2] <= s_hi]
        never = [k for k in (theirs - mine).elements() if any(lo <= k[2] <= hi for lo, hi in spans)]
        uncovered = sum((mine - theirs).values()) + sum((theirs - mine).values()) - len(lost) - len(never)
        detail = 'matched %d' % sum((mine & theirs).values())
        if lost:
            detail += '; the server never applied %s' % ' '.join(show_event(k) for k in lost[:6])
        if never:
            detail += '; the client never had %s' % ' '.join(show_event(k) for k in never[:6])
        if uncovered:
            detail += '; %d outside the other side\'s ticks' % uncovered
        if made:
            detail += '; %d leaver teardown events of the server\'s own' % made
        table.add(scope, 'own events vs server', FAIL if lost or never else PASS, detail)
    for player in sorted(member_ids(server) - set(by_player)):
        table.add('p%d' % player, 'own events vs server', NA, 'no client journal of this player')


# -- a run ---------------------------------------------------------------------

def judge(name, server, clients, args):
    table = Table(name)
    if server is not None:
        check_headers(table, server, clients)
    elif clients:
        check_headers(table, clients[0], clients[1:])
    check_replay(table, server, args)
    for journal in ([server] if server is not None else []) + clients:
        check_file(table, journal)
    if server is not None:
        names = {p['id']: ' "%s"' % p['name'] for p in server.players() if p['added'] and p['name']}
        check_server_inputs(table, server, clients, names)
    for journal in clients:
        check_bodies(table, server, journal, args.tolerance, args.end_cut, args.exclude)
        check_resyncs(table, journal)
        check_corrections(table, journal)
        check_client_inputs(table, journal)
    if server is not None:
        check_events(table, server, clients)
    else:
        table.add('-', 'own events vs server', NA, 'no server journal')
    return table


def runs_from_args(args):
    """[(name, server or None, [clients])] for every run asked for."""
    out = []
    if args.server or args.clients:
        server = load(args.server) if args.server else None
        clients = [load(p) for p in args.clients or []]
        out.extend(split(pathlib.Path(args.server or args.clients[0]).parent.name, server, clients))
    for directory in args.run_dir:
        journals = [load(p) for p in jt.journal_files(directory)]
        servers = [j for j in journals if j.is_server]
        if len(servers) > 1:
            sys.exit('%s: %d server journals; name one with --server' % (directory, len(servers)))
        out.extend(split(pathlib.Path(directory).name, servers[0] if servers else None,
                         [j for j in journals if not j.is_server]))
    return out


def split(name, server, clients):
    """A run with a server is one session; client journals alone are grouped."""
    if server is not None:
        return [(name, server, clients)]
    groups = group_sessions(clients)
    if len(groups) == 1:
        return [(name, None, groups[0])]
    return [('%s session %d/%d' % (name, i + 1, len(groups)), None, group) for i, group in enumerate(groups)]


# -- self-test -----------------------------------------------------------------

def self_test():
    """journal_trace's synthetic pair, made consistent, then broken once."""
    with tempfile.TemporaryDirectory(prefix='journal_judge_selftest_') as scratch:
        return _self_test(pathlib.Path(scratch))


def _self_test(out_dir):
    s_header, s_plan = jt._synthetic_server()
    c_header, c_plan = jt._synthetic_client()
    c_header = dict(c_header, world_options=s_header['world_options'], mechanism_slots=s_header['mechanism_slots'])
    jt._write_plan(str(out_dir / 'server.bmjr'), s_header, s_plan)
    jt._write_plan(str(out_dir / 'client0.bmjr'), c_header, c_plan)
    jt._write_plan(str(out_dir / 'client1.bmjr'), dict(c_header, level=12), c_plan)
    # end_cut 0: the synthetic client's whole life is shorter than the cut
    args = argparse.Namespace(tolerance=TOLERANCE, end_cut=0, simtool='', root='', map='', replay_dir='', exclude=[])
    server = load(out_dir / 'server.bmjr')
    good, other = load(out_dir / 'client0.bmjr'), load(out_dir / 'client1.bmjr')
    table = judge('self-test', server, [good, other], args)

    def row(scope, check):
        return next(r for r in table.rows if r['scope'] == scope and r['check'] == check)

    checks = [
        ('header of a consistent client passes', row('client0.bmjr', 'header vs server.bmjr')['verdict'] == PASS),
        ('a level mismatch fails', row('client1.bmjr', 'header vs server.bmjr')['verdict'] == FAIL),
        ('the shared tick 102 is compared', '1 common ticks' in row('client0.bmjr', 'rows compared')['detail']),
        ('LOCAL rows are reported, not judged', row('client0.bmjr', 'first divergence')['verdict'] == REPORTED),
        ('a hard correction fails', row('client0.bmjr', 'corrections')['verdict'] == FAIL),
        ('the substituted own tick is counted', 'substituted 1' in row('client0.bmjr', 'own input')['detail']),
        ('player 2 starved on the server, under the threshold',
         row('server', 'inputs p2 "小球玩家"')['verdict'] == PASS),
        ('no replay without --simtool', row('server', 'replay')['verdict'] == NA),
    ]
    for what, ok in checks:
        print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    return 0 if all(ok for _, ok in checks) else 1


def main():
    jt.use_utf8_output()
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('run_dir', nargs='*', help='a directory of one run\'s .bmjr files')
    ap.add_argument('--server', help='the server journal')
    ap.add_argument('--clients', nargs='+', help='client journals')
    ap.add_argument('--json', help='write the tables here')
    ap.add_argument('--tolerance', type=float, default=TOLERANCE,
                    help='metres on any axis before a body counts as diverged (first_divergence.py default)')
    ap.add_argument('--exclude', action='append', default=None, metavar='PREFIX',
                    help='bodies (by name prefix) left out of the body checks and reported on their own; '
                         'default PE_Balloon and PE_Box_slide (resync_rules.hpp keep-local); --exclude "" for none')
    ap.add_argument('--end-cut', type=int, default=END_CUT,
                    help="ticks at a client's end left out of the body checks (its exit's)")
    ap.add_argument('--simtool', default='', help='BallanceMMOSimTool executable, for the replay check')
    ap.add_argument('--root', default='', help='the game directory the SimTool loads levels from')
    ap.add_argument('--map', default='', help='the custom map (.nmo) the session ran')
    ap.add_argument('--replay-dir', default='', help="the SimTool's working directory, kept (default: a temp dir, removed)")
    ap.add_argument('--self-test', action='store_true', help="judge journal_trace's synthetic journals")
    args = ap.parse_args()
    args.exclude = [p for p in (args.exclude if args.exclude is not None else DEFAULT_EXCLUDE) if p]
    if args.self_test:
        return self_test()
    if not args.run_dir and not args.server and not args.clients:
        ap.error('give a RUN_DIR, or --server / --clients')
    tables = [judge(name, server, clients, args) for name, server, clients in runs_from_args(args)]
    for table in tables:
        table.print()
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(
            [{'run': t.name, 'verdict': t.verdict(), 'rows': t.rows} for t in tables], indent=1), encoding='utf8')
    return 1 if any(t.verdict() == FAIL for t in tables) else 0


if __name__ == '__main__':
    sys.exit(main())
