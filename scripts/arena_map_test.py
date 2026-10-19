"""Local physics-server test on a custom map (the BMMO Mechanism Arena).

Runs, per sector: a local BallanceMMOServer with physics enabled and N headless
BallanceMMOSessionClients that create a room on the map's level slot, ready up
and play a physics session, driven by generated input records.  Each run is
then checked four ways:

  * every client exited 0 and reported no hard corrections, no resyncs and no
    rollback mismatches it could not resolve,
  * the server's own journal replays bit-for-bit in BallanceMMOSimTool,
  * the mechanisms of that sector were all tracked (`mechanisms=N/N` with a
    non-zero second number - zero corrections on its own is NOT a pass, it is
    also what a client with no local mechanism bodies reports),
  * the run actually produced the interactions it is meant to test: balls
    against balls, balls against mechanisms, and mechanisms against the loose
    bodies parked on them.

The drive records are staggered on purpose.  Every player rolls down the same
lane, but each starts later than the last and the leader brakes periodically,
so the field concertinas and the balls repeatedly run into each other - that is
what produces peer-ball prediction work, which is the part of the session code
a single ball never exercises.

`--start-sector` is what makes the mechanism coverage complete: Ballance only
runs the mechanisms of the sector the ball is in, so a map whose mechanisms are
spread over four sectors needs one session per sector.

usage:
  python scripts/arena_map_test.py --bin build-agent/BallanceMMOServer \\
      --root "C:/Users/geekerwan/Desktop/Ballance-Agent" --level 13 \\
      --players 4 --seconds 60 --drive
"""
import argparse
import collections
import json
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
FLAGS = subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0
EXE = '.exe' if sys.platform == 'win32' else ''

DIK_UP, DIK_DOWN, DIK_LEFT, DIK_RIGHT = 200, 208, 203, 205

# the arena's own geometry, from maps/build_arena_map.py.  The driving lane and
# the mechanism row are different lines: the mechanisms stand on the road, so a
# lane through them would just be a wall.
BAY_SPAN = 40.0
PLAY_Y = 15.0
MECH_Y = 43.0
BAYS_PER_SECTOR = 6
N_BAYS = 23


def write_drive_record(path, level, ticks, player, steer=False, stop_tick=0):
    """An input record that rolls the ball down the arena, into the mechanisms.

    Holding "up" walks the ball along the bay centre line, one mechanism after
    another.  Steering is off by default on purpose: a pre-recorded steer has
    no feedback, so any lateral input accumulates and the ball ends up running
    15-20 units off centre, missing the very mechanisms the run is meant to
    hit.

    The per-player stagger is what makes the balls meet.  Player i waits
    120 + 40*i ticks before setting off, and the leader brakes briefly every
    500 ticks, so the field concertinas and the balls run into each other
    repeatedly - without stopping anyone getting down the arena.  Braking hard
    or often does produce collisions, but it also leaves every ball parked in
    the first sector, which is worse: then nothing reaches a mechanism.
    """
    with open(path, 'wb') as rec:
        rec.write(struct.pack('<4sIdi64s32s4x', b'BMRC', 1, 66., level, b'', b''))
        start = 120 + 40 * player
        for tick in range(ticks):
            keys = bytearray(256)
            # stop_tick releases the controls: a ball then coasts to a stop
            # where it is, which is how a mechanism that acts on a *resting*
            # ball gets tested - a ventilator cannot lift a ball that crosses
            # its column at 15 units a second.
            if stop_tick and tick >= stop_tick:
                pass
            elif tick >= start:
                braking = player == 0 and (tick - start) % 500 < 25
                keys[DIK_DOWN if braking else DIK_UP] = 1
                if steer:
                    swing = (tick + player * 97) % 400
                    if swing < 30:
                        keys[DIK_LEFT] = 1
                    elif 200 <= swing < 230:
                        keys[DIK_RIGHT] = 1
            rec.write(keys + bytes(24))


def status_metrics(line):
    """Pull the interesting counters out of a client `status:` line."""
    out = {}
    for key in ('mism', 'hard', 'mech_hard', 'mech_max_err', 'max_err',
                'unmatched', 'starved', 'rebases', 'tick', 'frames', 'resim'):
        m = re.search(r'\b' + key + r'=([0-9.]+)', line)
        if m:
            out[key] = float(m.group(1)) if '.' in m.group(1) else int(m.group(1))
    m = re.search(r'resyncs=(\d+)/(\d+)', line)
    if m:
        out['resyncs'] = int(m.group(1))
    # mechanisms=<known>/<resolved>: the second number is the one that means
    # the client really has local mechanism bodies (level-reset postmortem).
    m = re.search(r'mechanisms=(\d+)/(\d+)', line)
    if m:
        out['mechanisms'] = f'{m.group(1)}/{m.group(2)}'
    return out


RE_CORRECTED = re.compile(r'(?:rollback to tick \d+ \(|mismatch at tick \d+ \(local \d+\): )'
                          r'([A-Za-z0-9_]+)')
RE_BODY = re.compile(r'\b(P_[A-Za-z0-9_]+|Ball_[A-Za-z]+(?:_BMMO_\d+)?)\b(?= dp=| off by)')


def interaction_report(case_dir, players):
    """Evidence that the run produced the interactions it is meant to test.

    Everything here is read back out of what the session itself recorded, so a
    quiet run cannot be mistaken for a thorough one."""
    mechanisms, peer_balls, own_balls = set(), set(), set()
    for i in range(players):
        log = case_dir / f'client{i}.log'
        if not log.exists():
            continue
        text = log.read_text(errors='replace')
        for name in RE_BODY.findall(text):
            if name.startswith('P_'):
                mechanisms.add(name)
            elif '_BMMO_' in name:
                peer_balls.add(name)
            else:
                own_balls.add(name)
    return {
        # mechanism bodies the clients predicted and the server corrected:
        # proof the mechanisms were simulated locally, not just received
        'mechanism_bodies_corrected': sorted(mechanisms),
        # peer balls corrected: only happens when a remote ball's predicted
        # path is disturbed, which is what a ball-to-ball collision does
        'peer_balls_corrected': sorted(peer_balls),
        'own_ball_types': sorted(own_balls),
    }


def journal_events(args, journal):
    """Ball-type changes and mechanism wakes, from the server's own journal."""
    exe = pathlib.Path(args.bin).resolve() / f'BallanceMMOSimTool{EXE}'
    proc = subprocess.run([str(exe), '--replay-session', journal, '--list'],
                          capture_output=True, text=True, timeout=600)
    text = proc.stdout + proc.stderr
    revived = collections.Counter(re.findall(r'BodyRevived .*name=(\S+)', text))
    ball_types = collections.Counter(re.findall(r'Physicalize ball=(\d+)', text))
    sectors = re.findall(r'Sector .*sector=(\d+)', text)
    return {'mechanisms_revived': dict(revived),
            'physicalize_by_ball_type': dict(ball_types),
            'distinct_sectors_entered': sorted(set(sectors))}


def run_case(args, sector, case_dir):
    case_dir.mkdir(parents=True, exist_ok=False)
    journals = case_dir / 'journals'
    config = (
        'rooms_enabled: true\n'
        'maximum_members: 8\n'
        'physics:\n'
        '  enabled: true\n'
        f'  game_root: {args.root}\n'
        '  snapshot_interval: 2\n'
        f'  input_delay: {args.input_delay}\n'
        '  maximum_physics_rooms: 1\n'
        '  event_rate_limit: 0\n'
        f'  spawn_impulse: {args.spawn_impulse}\n'
        f'  journal_dir: {str(journals).replace(chr(92), "/")}\n'
        '  journal_max_mb: 64\n'
        f'  journal_checkpoint_ticks: {args.checkpoint_ticks}\n'
        'auto_flush_log: true\n'
    )
    (case_dir / 'config.yml').write_text(config, encoding='utf8')

    bindir = pathlib.Path(args.bin).resolve()
    handles, children, clients = [], [], []
    try:
        sf = open(case_dir / 'server.log', 'wb')
        handles.append(sf)
        server = subprocess.Popen(
            [str(bindir / f'BallanceMMOServer{EXE}'), '--port', str(args.port)],
            cwd=case_dir, stdin=subprocess.PIPE, stdout=sf,
            stderr=subprocess.STDOUT, creationflags=FLAGS)
        children.append(server)
        time.sleep(1.5)

        for i in range(args.players):
            client_dir = case_dir / f'client{i}'
            client_dir.mkdir()
            cf = open(case_dir / f'client{i}.log', 'wb')
            handles.append(cf)
            cmd = [str(bindir / f'BallanceMMOSessionClient{EXE}'),
                   '--root', args.root,
                   '--server', f'127.0.0.1:{args.port}',
                   '--name', f'Arena{i}',
                   '--level', str(args.level),
                   '--seconds', str(args.seconds),
                   '--trace',
                   '--journal', str(case_dir / f'client{i}.bmjr')]
            if sector > 1:
                cmd += ['--start-sector', str(sector)]
            if args.drive:
                record = case_dir / f'input{i}.bmrc'
                write_drive_record(record, args.level, args.seconds * 66 + 200, i,
                                   args.steer, args.drive_stop)
                cmd += ['--record', str(record)]
            if args.beam_bays:
                # Put each ball just short of a mechanism of its own, so the run
                # does not depend on anyone driving the full length of the arena
                # to reach one.  Virtools axes: the arena runs along x, y is up,
                # and the mechanism row is z (BAY_SPAN and MECH_Y below mirror
                # maps/build_arena_map.py).
                if args.beam_bays == 'auto':
                    # the bays of the sector this run has active: a mechanism in
                    # any other sector is not physicalized, so beaming a ball at
                    # it would meet nothing.  --beam-offset rotates the choice so
                    # a second pass reaches the bays the first pass did not.
                    in_sector = list(range((sector - 1) * BAYS_PER_SECTOR,
                                           min(sector * BAYS_PER_SECTOR, N_BAYS)))
                    bays = [in_sector[(args.beam_offset + k) % len(in_sector)]
                            for k in range(args.players)]
                else:
                    bays = [int(b) for b in args.beam_bays.split(',')]
                bay = bays[i % len(bays)]
                # spread the landing points along the lane: without this two
                # clients sent to the same bay materialise inside each other
                cmd += ['--beam', repr(bay * BAY_SPAN - 9.0 - i * args.beam_spread),
                        repr(4.0), repr(MECH_Y), str(args.beam_tick)]
            cmd += ['--host', '--expect', str(args.players)] if i == 0 else ['--join-first']
            p = subprocess.Popen(cmd, cwd=client_dir, stdin=subprocess.DEVNULL,
                                 stdout=cf, stderr=subprocess.STDOUT, creationflags=FLAGS)
            clients.append(p)
            children.append(p)
            time.sleep(0.4)

        for p in clients:
            p.wait(timeout=args.seconds + 180)
        server.stdin.write(b'sessions\nstop\n')
        server.stdin.flush()
        try:
            server.wait(timeout=15)
        except subprocess.TimeoutExpired:
            server.terminate()
            server.wait(timeout=10)
    finally:
        for p in children:
            if p.poll() is None:
                p.terminate()
        for h in handles:
            h.close()

    result = {'sector': sector, 'dir': str(case_dir), 'clients': []}
    for i, p in enumerate(clients):
        text = (case_dir / f'client{i}.log').read_text(errors='replace')
        statuses = [s for s in text.splitlines() if s.startswith('status:')]
        result['clients'].append({
            'exit_code': p.returncode,
            'metrics': status_metrics(statuses[-1]) if statuses else None,
        })
    result['interaction'] = interaction_report(case_dir, args.players)
    result['journal'] = sorted(str(q) for q in journals.glob('*.bmjr')) if journals.exists() else []
    return result


def replay(args, journal, out_dir):
    """The gold-standard check: the server's journal must replay bit for bit."""
    exe = pathlib.Path(args.bin).resolve() / f'BallanceMMOSimTool{EXE}'
    proc = subprocess.run([str(exe), '--root', args.root, '--replay-session', journal,
                           '--report-every', '5000'],
                          capture_output=True, text=True, cwd=out_dir, timeout=1800)
    summary = [l for l in (proc.stdout + proc.stderr).splitlines() if l.startswith('summary:')]
    return {'exit_code': proc.returncode, 'summary': summary[-1] if summary else ''}


def verdict(case):
    """Pass only when the run was both clean and actually interactive."""
    problems, warnings = [], []
    for i, c in enumerate(case['clients']):
        m = c['metrics'] or {}
        if c['exit_code'] != 0:
            problems.append(f'client{i} exit {c["exit_code"]}')
        if m.get('hard') or m.get('mech_hard'):
            problems.append(f'client{i} hard corrections')
        if m.get('resyncs'):
            problems.append(f'client{i} resyncs')
        # A handful of starved ticks is the local box being busy, not a
        # protocol problem: the server repeats the last input, the session
        # recovers, and the journal still replays bit-exact.  Only a sustained
        # rate means the run is not worth trusting.
        starved, ticks = m.get('starved', 0), max(m.get('tick', 1), 1)
        if starved > max(20, 0.01 * ticks):
            problems.append(f'client{i} input starvation {starved}/{ticks}')
        elif starved:
            warnings.append(f'client{i} starved {starved}/{ticks} ticks')
        # mechanisms=<known>/<resolved>.  The invariant that matters is
        # resolved > 0: a client with no local mechanism bodies reports zero
        # mechanism corrections, which otherwise reads as perfect sync (see the
        # 9.28 level-reset postmortem).  known > resolved is NOT a failure - it
        # is the normal state whenever the players straddle a checkpoint, since
        # the server runs the union of their sectors and names mechanisms this
        # client has no body for.
        known, _, resolved = (m.get('mechanisms') or '0/0').partition('/')
        if resolved == '0':
            problems.append(f'client{i} resolved no mechanism bodies ({m.get("mechanisms")})')
    for r in case.get('replays', []):
        if r['exit_code'] != 0 or 'first_divergence=-1' not in r['summary']:
            problems.append('journal replay diverged')
    inter = case['interaction']
    if not inter['mechanism_bodies_corrected']:
        problems.append('no mechanism interaction observed')
    if len(case['clients']) > 1 and not inter['peer_balls_corrected']:
        problems.append('no ball-to-ball interaction observed')
    return problems, warnings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=str(REPO / 'build-agent/BallanceMMOServer'))
    ap.add_argument('--root', required=True, help='Ballance data directory')
    ap.add_argument('--level', type=int, default=13)
    ap.add_argument('--sectors', default='1,2,3,4')
    ap.add_argument('--players', type=int, default=4)
    ap.add_argument('--seconds', type=int, default=60)
    ap.add_argument('--input-delay', type=int, default=6)
    ap.add_argument('--spawn-impulse', type=float, default=3.0)
    ap.add_argument('--port', type=int, default=32990)
    ap.add_argument('--drive', action='store_true',
                    help='feed the clients staggered input records that roll the balls through the bays')
    ap.add_argument('--steer', action='store_true',
                    help='add alternating left/right (drifts the balls off the bays)')
    ap.add_argument('--beam-bays', default='',
                    help='"auto" (the bays of the active sector) or comma-separated bay indices; '
                         'client i is beamed just short of bay[i] '
                         'so it meets that mechanism without driving the whole arena')
    ap.add_argument('--drive-stop', type=int, default=0,
                    help='tick at which the clients stop driving and coast to a halt')
    ap.add_argument('--beam-spread', type=float, default=3.0,
                    help='metres between the beam landing points of successive clients')
    ap.add_argument('--beam-offset', type=int, default=0,
                    help='rotate which bays of the sector --beam-bays auto picks')
    ap.add_argument('--checkpoint-ticks', type=int, default=660,
                    help='ticks between full body checkpoints in the journals; small '
                         'values make contact measurable with collision_report.py')
    ap.add_argument('--beam-tick', type=int, default=700,
                    help='session tick at which --beam-bays fires')
    ap.add_argument('--out', default=str(REPO / 'maps/arena-test'))
    ap.add_argument('--gap', type=float, default=6.0,
                    help='seconds to wait between cases so the last session fully exits')
    ap.add_argument('--label', default=time.strftime('%Y%m%d%H%M%S'))
    args = ap.parse_args()

    base = pathlib.Path(args.out) / args.label
    if base.exists():
        shutil.rmtree(base)
    base.mkdir(parents=True)

    results, failed = [], 0
    base_port = args.port
    for n, sector in enumerate(int(s) for s in args.sectors.split(',')):
        args.port = base_port + n
        print(f'=== sector {sector}: {args.players} players, {args.seconds}s '
              f'(port {args.port}) ===', flush=True)
        case = run_case(args, sector, base / f'sector{sector}')
        for j in case['journal']:
            case.setdefault('replays', []).append(replay(args, j, base))
            case.setdefault('events', []).append(journal_events(args, j))
        problems, warnings = verdict(case)
        case['problems'], case['warnings'] = problems, warnings
        failed += bool(problems)
        results.append(case)

        for i, c in enumerate(case['clients']):
            print(f'  client{i} rc={c["exit_code"]} {c["metrics"]}', flush=True)
        for r in case.get('replays', []):
            print(f'  replay rc={r["exit_code"]} {r["summary"]}', flush=True)
        inter = case['interaction']
        print(f'  mechanisms touched ({len(inter["mechanism_bodies_corrected"])}): '
              f'{", ".join(inter["mechanism_bodies_corrected"]) or "none"}', flush=True)
        print(f'  peer balls corrected ({len(inter["peer_balls_corrected"])}): '
              f'{", ".join(inter["peer_balls_corrected"]) or "none"}', flush=True)
        for e in case.get('events', []):
            print(f'  ball types physicalized: {e["physicalize_by_ball_type"]}; '
                  f'sectors entered: {e["distinct_sectors_entered"]}; '
                  f'mechanisms revived: {len(e["mechanisms_revived"])}', flush=True)
        if warnings:
            print(f'  note: {"; ".join(warnings)}', flush=True)
        print(f'  VERDICT: {"PASS" if not problems else "FAIL - " + "; ".join(problems)}',
              flush=True)
        (base / 'summary.json').write_text(json.dumps(results, indent=2), encoding='utf8')
        # Let the previous session's processes actually go before the next
        # server binds.  One server plus N headless clients is N+1 full
        # Ballance engines stepping at 66 Hz; starting the next set while the
        # last is still tearing down starves the new clients of CPU, and a
        # client that cannot step its own physics (frames far below tick)
        # reports input starvation and re-anchors.  That is a host capacity
        # limit, not a protocol fault - the server's journal still replayed
        # bit-exact through it - but it makes the suite flap, so leave a gap.
        time.sleep(args.gap)

    print(f'\n{len(results) - failed}/{len(results)} sectors passed; wrote '
          + str(base / 'summary.json'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
