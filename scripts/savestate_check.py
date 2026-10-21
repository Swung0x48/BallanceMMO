"""The IVP savestate's robustness matrix (docs/ivp-savestate-design.md, "Tests that prove it").

Two parts.

The matrix (default).  BallanceMMOSimTool --savestate-check over scenes x seeds x depth caps, physics-only
and --frames, plus the --legacy-restore negative control.  Every positive cell must PASS: chained, seeded
rollbacks reproduce the straight run's savestate blobs byte for byte (restore identity before any physics,
replay identity after every re-stepped tick, the world hash and the exact core dump at the end of every
round).  Every negative-control cell must FAIL: the old per-body restore keeps the live world's collision
bookkeeping, and a check that cannot see that proves nothing.  A scene with no contact point or friction
system in its window is "vacuous" (SimTool exit 2); for the core scenes that is a failure of the harness, for
the --full extras it is reported and tolerated.

The SimTool command of every cell is in its log and printed by --list; the core scenes by hand (ROOT is
--root, the arena installed in slot 13 as in maps/README.md):

  arena-spawn4   BallanceMMOSimTool --root ROOT --savestate-check --level 13 --spawn-test 4
                 (four wood balls - --spawn-ball's default - born at one point on the start plate:
                  the coincident spawn)
  arena-drop41   BallanceMMOSimTool --root ROOT --savestate-check --level 13 --drop P_Modul_41 1
                     --drop-at 120 4 43 --drop-sector 1
                 (a wood ball dropped onto bay 3, the tilting block carrying the P_Ball_Wood prop)
  arena-sector2  BallanceMMOSimTool --root ROOT --savestate-check --level 13 --drop-sector 2
                 (sector 2 started by the union: the plate row P_Modul_29 with the paper prop on it, the
                  see-saw P_Modul_30, the stone prop and the box on the road)

each with [--frames] (whole engine frames, the scripts carried by the script replay) or [--legacy-restore]
(the negative control), and --ticks / --rounds / --max-depth / --seed / --savestate-every /
--savestate-trace as below.

--journals.  The end-to-end half: scripts/arena_map_test.py runs one solo session against an EXTERNAL server
(--server HOST:PORT) as a control, then once more per --pairs EVERY:DEPTH with the headless client's
--force-rollback EVERY:DEPTH, and each arm's client journal is compared with the control's by
scripts/worst_bodies.py: every body must be 0.0 m off, never above 1e-6 m, and the arm must really have
taken forced rollbacks.  arena_map_test.py's own verdict (written for multi-player runs) is printed but not
judged here.

usage:
  python scripts/savestate_check.py --quick
  python scripts/savestate_check.py [--full] [--seeds 1,2,3] [--depths 48,8] [--modes physics,frames]
  python scripts/savestate_check.py --journals --server 127.0.0.1:26677 [--pairs 50:12,97:24,131:47]

Exit status: 0 when every cell came out as expected, 1 otherwise, 2 on a usage error.
"""
import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
FLAGS = subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0
EXE = '.exe' if sys.platform == 'win32' else ''
DEFAULT_ROOT = 'C:/Users/geekerwan/Desktop/Ballance-Agent'


class Scene:
    def __init__(self, name, switches, core, about):
        self.name = name
        self.switches = switches
        self.core = core      # in --quick and the default matrix; vacuous = failure
        self.about = about


SCENES = [
    Scene('arena-spawn4', ['--level', '13', '--spawn-test', '4'], True,
          'four wood balls born at one point on the arena start plate (the coincident spawn)'),
    Scene('arena-drop41', ['--level', '13', '--drop', 'P_Modul_41', '1', '--drop-at', '120', '4', '43',
                           '--drop-sector', '1'], True,
          'a wood ball dropped onto bay 3: the tilting block P_Modul_41 with the P_Ball_Wood prop on it'),
    Scene('arena-sector2', ['--level', '13', '--drop-sector', '2'], True,
          'sector 2 started by the union: plate row + paper prop, see-saw, stone prop and box on the road'),
    # --full only.  Prop names follow the arena's P_<type>_MF bodies; the retail levels are the ones the
    # design names (Level 8's see-saw P_Modul_30_Wippe, Level 3's Dreharme), sector 1 only, so either can
    # be vacuous - reported, not failed.
    Scene('arena-drop-prop', ['--level', '13', '--drop', 'P_Modul_29', '2', '--drop-at', '360', '4', '43',
                              '--drop-sector', '2', '--drop-prop', 'P_Ball_Stone_MF',
                              '--drop-player-at', '348', '8', '43'], False,
          'a stone ball beside the plate row, the stone prop beamed onto the row from above'),
    Scene('level8-sector1', ['--level', '8'], False, 'retail Level 8, sector 1'),
    Scene('level3-sector1', ['--level', '3'], False, 'retail Level 3, sector 1'),
]


class Cell:
    def __init__(self, scene, mode, restore, seed, cap, expect):
        self.scene = scene
        self.mode = mode          # 'physics' | 'frames'
        self.restore = restore    # 'savestate' | 'legacy'
        self.seed = seed
        self.cap = cap
        self.expect = expect      # 'PASS' | 'FAIL'
        self.result = None
        self.rc = None
        self.summary = {}
        self.first_failure = ''
        self.seconds = 0.0
        self.log = None

    def first_failure_field(self):
        # 'FAIL round=r anchor=a depth=d tick=t: <field>: <detail>' -> '<field>'
        m = re.match(r'FAIL round=\d+ anchor=\d+ depth=\d+ tick=\d+: ([^:]+)', self.first_failure)
        return m.group(1).strip() if m else ''

    @property
    def name(self):
        return f'{self.scene.name}.{self.mode}.{self.restore}.s{self.seed}.d{self.cap}'

    def ok(self):
        if self.restore == 'legacy':
            # the negative control proves something only when it fails where the
            # old per-body restore is known to fall short: a fresh capture after
            # the restore differs from the anchor's blob (the collision state a
            # pose write cannot put back), before any step is replayed
            return self.result == 'FAIL' and self.first_failure_field() == 'restore identity'
        if self.result == self.expect:
            return True
        # the --full extras may simply hold no contact in their window
        return self.result == 'VACUOUS' and self.expect == 'PASS' and not self.scene.core


def simtool(args):
    return pathlib.Path(args.bin).resolve() / f'BallanceMMOSimTool{EXE}'


def simtool_command(args, cell, trace):
    cmd = [str(simtool(args)), '--root', args.root, '--savestate-check', *cell.scene.switches,
           '--ticks', str(args.ticks), '--rounds', str(args.rounds), '--max-depth', str(cell.cap),
           '--seed', str(cell.seed), '--savestate-every', str(args.every)]
    if trace is not None:
        cmd += ['--savestate-trace', str(trace)]
    if cell.mode == 'frames':
        cmd.append('--frames')
    if cell.restore == 'legacy':
        # the first failure is the whole point of the control
        cmd += ['--legacy-restore', '--stop-on-divergence']
    elif args.stop_on_divergence:
        cmd.append('--stop-on-divergence')
    cmd += args.simtool_arg
    return cmd


RE_SUMMARY = re.compile(r'^summary: savestate-check (.*)$')
RE_VERDICT = re.compile(r'^(PASS|FAIL) rounds=(\d+) failed=(\d+)\s*$')


def run_cell(args, cell, out):
    trace = out / f'{cell.name}.trace'
    try:
        trace.unlink()            # the SimTool appends
    except FileNotFoundError:
        pass
    cmd = simtool_command(args, cell, trace)
    cell.log = out / f'{cell.name}.log'
    started = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=args.timeout,
                              creationflags=FLAGS)
        cell.rc, text = proc.returncode, proc.stdout + '\n--- stderr ---\n' + proc.stderr
    except subprocess.TimeoutExpired as timeout:
        def as_text(data):
            if data is None:
                return ''
            return data if isinstance(data, str) else data.decode(errors='replace')
        cell.rc, text = None, as_text(timeout.stdout) + '\n--- stderr ---\n' + as_text(timeout.stderr)
    cell.seconds = time.monotonic() - started
    cell.log.write_text(' '.join(cmd) + '\n\n' + text, encoding='utf8')
    verdict, vacuous = None, False
    for line in text.splitlines():
        m = RE_SUMMARY.match(line)
        if m:
            cell.summary = dict(kv.split('=', 1) for kv in m.group(1).split() if '=' in kv)
        m = RE_VERDICT.match(line)
        if m:
            verdict = m.group(1)
        if line.startswith('vacuous scene'):
            vacuous = True
        if line.startswith('FAIL round=') and not cell.first_failure:
            cell.first_failure = line
    if cell.rc == 0 and verdict == 'PASS':
        cell.result = 'PASS'
    elif cell.rc == 3 and verdict == 'FAIL':
        cell.result = 'FAIL'
    elif cell.rc == 2 and vacuous:
        cell.result = 'VACUOUS'
    elif cell.rc is None:
        cell.result = 'TIMEOUT'
    else:
        cell.result = f'ERROR({cell.rc})'
    return cell


def matrix_cells(args):
    names = {n for n in args.scenes.split(',') if n} if args.scenes else None
    if args.quick:
        core = [s for s in SCENES if s.core]
        cells = [Cell(s, 'physics', 'savestate', 1, args.quick_depth, 'PASS') for s in core]
        # one whole-frame cell on the mechanisms, one negative control
        sector = next(s for s in core if s.name == 'arena-sector2')
        cells.append(Cell(sector, 'frames', 'savestate', 1, args.quick_depth, 'PASS'))
        cells.append(Cell(core[0], 'physics', 'legacy', 1, args.quick_depth, 'FAIL'))
        if names is not None:
            cells = [c for c in cells if c.scene.name in names]
        return cells
    scenes = [s for s in SCENES if s.core or args.full]
    if names is not None:
        scenes = [s for s in SCENES if s.name in names]
    seeds = [int(x) for x in args.seeds.split(',') if x.strip()]
    caps = [int(x) for x in args.depths.split(',') if x.strip()]
    modes = [m for m in args.modes.split(',') if m.strip()]
    cells = []
    for scene in scenes:
        for mode in modes:
            for seed in seeds:
                for cap in caps:
                    cells.append(Cell(scene, mode, 'savestate', seed, cap, 'PASS'))
    if not args.no_legacy:
        for scene in scenes:
            if scene.core:
                cells.append(Cell(scene, 'physics', 'legacy', seeds[0], caps[0], 'FAIL'))
    return cells


def print_table(cells):
    head = ('cell', 'result', 'want', 'ok', 'rounds', 'failed', 'blob_bytes', 'contacts_min', 'systems_min',
            'collisions_min', 'secs')
    rows = []
    for c in cells:
        s = c.summary
        rows.append((c.name, c.result or '-', c.expect, 'ok' if c.ok() else 'UNEXPECTED',
                     s.get('rounds', '-'), s.get('failed', '-'), s.get('blob_bytes', '-'),
                     s.get('contacts_min', '-'), s.get('systems_min', '-'), s.get('collisions_min', '-'),
                     f'{c.seconds:.0f}'))
    widths = [max(len(str(r[i])) for r in rows + [head]) for i in range(len(head))]
    fmt = '  '.join('{:<%d}' % w for w in widths)
    print(fmt.format(*head))
    for r in rows:
        print(fmt.format(*[str(x) for x in r]))
    for c in cells:
        if c.first_failure and (not c.ok() or c.restore == 'legacy'):
            print(f'  {c.name}: {c.first_failure[:300]}')
        if c.restore == 'legacy' and c.result == 'FAIL' and not c.ok():
            print(f'  {c.name}: the negative control failed at "{c.first_failure_field()}", '
                  f'not at the restore identity it is meant to fail at')
        if not c.ok():
            print(f'  {c.name}: see {c.log}')


def run_matrix(args):
    if not simtool(args).exists():
        print(f'no SimTool at {simtool(args)} (--bin)', file=sys.stderr)
        return 2
    cells = matrix_cells(args)
    if not cells:
        print('no cell selected', file=sys.stderr)
        return 2
    out = pathlib.Path(args.out) if args.out else pathlib.Path(args.bin).resolve() / 'savestate-check'
    out = out / args.label
    if args.list:
        for c in cells:
            print(f'{c.name} (expect {c.expect}): {" ".join(simtool_command(args, c, None))}')
        return 0
    out.mkdir(parents=True, exist_ok=True)
    print(f'{len(cells)} cells, {args.jobs} at a time, logs in {out}', flush=True)
    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = [pool.submit(run_cell, args, c, out) for c in cells]
        for future in concurrent.futures.as_completed(futures):
            c = future.result()
            print(f'  {c.name}: {c.result} ({"ok" if c.ok() else "UNEXPECTED"}, {c.seconds:.0f} s)', flush=True)
    print(f'\nmatrix finished in {time.monotonic() - started:.0f} s\n')
    print_table(cells)
    bad = [c for c in cells if not c.ok()]
    print(f'\n{len(cells) - len(bad)}/{len(cells)} cells as expected')
    return 1 if bad else 0


# ------------------------------------------------------------------ --journals

def parse_pairs(text):
    pairs = []
    for item in text.split(','):
        item = item.strip()
        if not item:
            continue
        every, _, depth = item.partition(':')
        pairs.append((int(every), int(depth) if depth else 12))
    return pairs


def arena_run(args, base, label, client_args):
    """One solo session through arena_map_test.py; returns what the comparison needs."""
    cmd = [sys.executable, str(REPO / 'scripts' / 'arena_map_test.py'),
           '--bin', str(pathlib.Path(args.bin).resolve()), '--root', args.root,
           '--level', str(args.level), '--sectors', str(args.sector), '--players', '1',
           '--seconds', str(args.seconds), '--external-server', args.server,
           '--out', str(base), '--label', label,
           '--checkpoint-ticks', str(args.checkpoint_ticks), '--gap', str(args.gap)]
    if args.drive:
        cmd.append('--drive')
    if args.journal_dir:
        cmd += ['--journal-dir', args.journal_dir]
    # the = form: argparse would take a bare "--force-rollback" for an option of its own
    for a in list(args.client_arg) + list(client_args):
        cmd.append('--client-arg=' + a)
    print(f'=== {label}: {" ".join(cmd)}', flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, errors='replace',
                          timeout=args.seconds + 1800, creationflags=FLAGS)
    base.mkdir(parents=True, exist_ok=True)
    (base / f'{label}.log').write_text(proc.stdout + '\n--- stderr ---\n' + proc.stderr, encoding='utf8')
    for line in proc.stdout.splitlines():
        if line.strip().startswith(('client0 rc=', 'replay rc=', 'VERDICT')):
            print('   ' + line.strip(), flush=True)
    case = base / label / f'sector{args.sector}'
    status = ''
    log = case / 'client0.log'
    if log.exists():
        statuses = [s for s in log.read_text(errors='replace').splitlines() if s.startswith('status:')]
        status = statuses[-1] if statuses else ''
    forced = re.search(r'\bforced=(\d+)/(\d+)', status)
    return {'rc': proc.returncode, 'journal': case / 'client0.bmjr', 'status': status,
            'forced': int(forced.group(1)) if forced else None,
            'forced_missed': int(forced.group(2)) if forced else None}


def exact_difference(control, arm):
    """The largest coordinate difference over every body and tick both journals checkpointed, unrounded
    (worst_bodies.py prints four decimals): (worst, body, tick, pairs compared), or None when the journal
    readers cannot be imported."""
    try:
        sys.path.insert(0, str(REPO / 'scripts'))
        import journal_trace as jt
        import first_divergence as fd
    except Exception as e:  # a reader that does not import must not hide the worst_bodies.py verdict
        print(f'  (no unrounded comparison: {e})')
        return None
    a = fd.worlds(jt.read_journal(str(control)), want_local=True)
    b = fd.worlds(jt.read_journal(str(arm)), want_local=True)
    worst, name, at, compared = 0.0, '-', -1, 0
    missing, extra, first_missing = 0, 0, None
    for tick in sorted(set(a) & set(b)):
        # The player's ball is keyed by a session-specific player id: the one
        # ball of each solo journal is the same body (worst_bodies.py pairs
        # them the same way).
        row_b = b[tick]
        balls_a = [k for k in a[tick] if k[0] == 'ball']
        balls_b = [k for k in row_b if k[0] == 'ball']
        if len(balls_a) == 1 and len(balls_b) == 1 and balls_a[0] != balls_b[0]:
            row_b = dict(row_b)
            row_b[balls_a[0]] = row_b.pop(balls_b[0])
        # Presence: a body the control has and the arm lacks (a sector that never
        # started, a body destroyed and never made again) is not "no difference".
        for key in a[tick]:
            if key not in row_b:
                missing += 1
                if first_missing is None:
                    first_missing = (tick, key[1] if key[0] == 'body' else 'ball:%s' % key[1])
        extra += sum(1 for key in row_b if key not in a[tick])
        for key, pos in a[tick].items():
            other = row_b.get(key)
            if other is None:
                continue
            compared += 1
            err = max(abs(x - y) for x, y in zip(pos, other))
            if err > worst:
                worst, at = err, tick
                name = key[1] if key[0] == 'body' else 'ball:%s' % key[1]
    return worst, name, at, compared, {'missing': missing, 'extra': extra, 'first_missing': first_missing}


def worst_bodies(control, arm):
    proc = subprocess.run([sys.executable, str(REPO / 'scripts' / 'worst_bodies.py'), str(control), str(arm)],
                          capture_output=True, text=True, errors='replace', timeout=1200, creationflags=FLAGS)
    rows = []
    for line in proc.stdout.splitlines()[1:]:
        m = re.match(r'^(\S+)\s+(\S+)\s+(.*)$', line.strip())
        if not m:
            continue
        try:
            err = float(m.group(2))
        except ValueError:
            continue
        rows.append((m.group(1), err, m.group(3).strip()))
    return proc.returncode, rows, proc.stdout + proc.stderr


def run_journals(args):
    if not args.server:
        print('--journals needs --server HOST:PORT (an external physics server)', file=sys.stderr)
        return 2
    pairs = parse_pairs(args.pairs)
    if not pairs:
        print('--pairs is empty', file=sys.stderr)
        return 2
    out = pathlib.Path(args.out) if args.out else pathlib.Path(args.bin).resolve() / 'savestate-check'
    base = out / args.label / 'journals'
    base.mkdir(parents=True, exist_ok=True)
    control = arena_run(args, base, 'control', [])
    problems = []
    if not control['journal'].exists():
        print(f'control: no client journal at {control["journal"]} (rc={control["rc"]})')
        return 1
    print(f'control: {control["journal"]} forced={control["forced"]}')
    if control['forced']:
        problems.append('the control took forced rollbacks')
    results = []
    for every, depth in pairs:
        label = f'forced-{every}x{depth}'
        # The server keeps the previous run's player until its connection
        # times out, and a second 'Arena0' is refused while it does.
        if args.settle_seconds > 0:
            print(f'settling {args.settle_seconds} s so the server drops the previous client', flush=True)
            time.sleep(args.settle_seconds)
        arm = arena_run(args, base, label, ['--force-rollback', f'{every}:{depth}'])
        if not arm['journal'].exists():
            problems.append(f'{label}: no client journal (rc={arm["rc"]})')
            results.append((label, None, [], arm, None))
            continue
        rc, rows, text = worst_bodies(control['journal'], arm['journal'])
        (base / f'{label}.worst_bodies.txt').write_text(text, encoding='utf8')
        unrounded = exact_difference(control['journal'], arm['journal'])
        results.append((label, rc, rows, arm, unrounded))
        if rc != 0 or not rows:
            problems.append(f'{label}: worst_bodies.py rc={rc}, {len(rows)} bodies compared')
            continue
        off = [r for r in rows if r[1] != 0.0 or r[2] != 'never']
        if off:
            problems.append(f'{label}: {len(off)} of {len(rows)} bodies off the control')
        if unrounded is None:
            problems.append(f'{label}: no unrounded comparison (the journal readers did not import)')
        elif unrounded[0] != 0.0:
            problems.append(f'{label}: {unrounded[1]} is {unrounded[0]:.3e} m off at tick {unrounded[2]}')
        if unrounded is not None and (unrounded[4]['missing'] or unrounded[4]['extra']):
            fm = unrounded[4]['first_missing']
            problems.append(f"{label}: presence differs - {unrounded[4]['missing']} control body-ticks missing from the arm"
                            f"{', first ' + fm[1] + ' at tick ' + str(fm[0]) if fm else ''}, {unrounded[4]['extra']} arm body-ticks absent from the control")
        if not arm['forced']:
            problems.append(f'{label}: the arm took no forced rollback (forced={arm["forced"]}), so it proves nothing')
    print()
    for label, rc, rows, arm, unrounded in results:
        if not rows:
            print(f'{label}: no comparison (rc={rc})')
            continue
        worst = max(rows, key=lambda r: r[1])
        exact = all(r[1] == 0.0 and r[2] == 'never' for r in rows) and (unrounded is None or unrounded[0] == 0.0)
        extra = '' if unrounded is None else f'; unrounded worst {unrounded[0]:.3e} m over {unrounded[3]} pairs'
        print(f'{label}: forced={arm["forced"]}/{arm["forced_missed"]} bodies={len(rows)} '
              f'worst {worst[0]} {worst[1]:.4f} m ({worst[2]}){extra} -> {"EXACT" if exact else "MISMATCH"}')
    for p in problems:
        print('  problem: ' + p)
    print('PASS' if not problems else 'FAIL')
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--root', default=DEFAULT_ROOT, help='Ballance game directory (arena in slot 13)')
    ap.add_argument('--bin', default=str(REPO / 'build-agent' / 'BallanceMMOServer'),
                    help='directory holding BallanceMMOSimTool (and the session client for --journals)')
    ap.add_argument('--out', default='', help='where logs go (default: <bin>/savestate-check)')
    ap.add_argument('--label', default=time.strftime('%Y%m%d%H%M%S'))
    # the matrix
    ap.add_argument('--quick', action='store_true',
                    help='2-minute smoke: the core scenes physics-only, one --frames cell, one negative control, '
                         'short windows')
    ap.add_argument('--full', action='store_true', help='add the extra scenes (vacuous tolerated)')
    ap.add_argument('--scenes', default='', help='comma-separated scene names to run (see --list)')
    ap.add_argument('--modes', default='physics,frames')
    ap.add_argument('--seeds', default='1,2,3')
    ap.add_argument('--depths', default='48,8', help='comma-separated --max-depth caps')
    ap.add_argument('--rounds', type=int, default=None, help='rounds per cell (50; --quick 12)')
    ap.add_argument('--ticks', type=int, default=None, help='straight-pass window per cell (200; --quick 96)')
    ap.add_argument('--quick-depth', type=int, default=24, help='the depth cap of the --quick cells')
    ap.add_argument('--every', type=int, default=1, help='--savestate-every')
    ap.add_argument('--no-legacy', action='store_true', help='skip the --legacy-restore negative control')
    ap.add_argument('--stop-on-divergence', action='store_true')
    ap.add_argument('--jobs', type=int, default=2, help='SimTool processes at a time')
    ap.add_argument('--timeout', type=int, default=3600, help='seconds per cell')
    ap.add_argument('--simtool-arg', action='append', default=[], help='extra SimTool argument; repeatable')
    ap.add_argument('--list', action='store_true', help='print every cell\'s command and exit')
    # --journals
    ap.add_argument('--journals', action='store_true', help='the end-to-end forced-rollback journal check')
    ap.add_argument('--server', default='', help='HOST:PORT of the external physics server')
    ap.add_argument('--journal-dir', default='', help="passed to arena_map_test.py: the server's journal_dir")
    ap.add_argument('--pairs', default='50:12,97:24,131:47', help='comma-separated EVERY:DEPTH pairs')
    ap.add_argument('--seconds', type=int, default=40)
    ap.add_argument('--level', type=int, default=13)
    ap.add_argument('--sector', type=int, default=1)
    ap.add_argument('--checkpoint-ticks', type=int, default=33,
                    help='journal checkpoint cadence: the bodies worst_bodies.py compares')
    ap.add_argument('--no-drive', dest='drive', action='store_false', help='no input record (the ball sits)')
    ap.add_argument('--client-arg', action='append', default=[],
                    help='extra session-client argument for every run, control included; repeatable')
    ap.add_argument('--gap', type=float, default=4.0, help='arena_map_test.py --gap')
    ap.add_argument('--settle-seconds', type=int, default=30,
                    help='wait between journal runs until the server has dropped the previous client')
    args = ap.parse_args()
    if args.rounds is None:
        args.rounds = 12 if args.quick else 50
    if args.ticks is None:
        args.ticks = 96 if args.quick else 200
    if args.journals:
        return run_journals(args)
    return run_matrix(args)


if __name__ == '__main__':
    sys.exit(main())
