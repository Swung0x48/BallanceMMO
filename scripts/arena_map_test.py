"""Local physics-server test on a custom map (the BMMO Mechanism Arena).

Runs, per sector: a local BallanceMMOServer with physics enabled, N headless
BallanceMMOSessionClients that create a room on the map's level slot, ready up
and play a physics session for a while.  Each run is then checked three ways:

  * every client exited 0 and reported no hard corrections / rollback mismatches,
  * the server's own journal replays bit-for-bit in BallanceMMOSimTool,
  * the mechanisms of that sector actually got physicalized (bodies > floors).

`--start-sector` is what makes the coverage complete: Ballance only runs the
mechanisms of the sector the ball is in, so a map whose mechanisms are spread
over four sectors needs one session per sector to exercise all of them.

usage:
  python scripts/arena_map_test.py --bin build-agent/BallanceMMOServer \\
      --root "C:/Users/geekerwan/Desktop/Ballance-Agent" --level 13
"""
import argparse
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

DIK_UP, DIK_LEFT, DIK_RIGHT = 200, 203, 205


def write_drive_record(path, level, ticks, player, steer=False):
    """An input record that rolls the ball down the arena, into the mechanisms.

    The arena runs along +X and the ball spawns facing it, so just holding "up"
    walks the ball along the bay centre line, one mechanism after another.
    Steering is off by default on purpose: a pre-recorded steer has no feedback,
    so any lateral input accumulates and the ball ends up running past the bays
    15-20 units off centre, missing the very mechanisms the run is meant to hit.
    """
    with open(path, 'wb') as rec:
        rec.write(struct.pack('<4sIdi64s32s4x', b'BMRC', 1, 66., level, b'', b''))
        for tick in range(ticks):
            keys = bytearray(256)
            if tick >= 120:
                keys[DIK_UP] = 1
                if steer:
                    phase = (tick + player * 97) % 400
                    if phase < 30:
                        keys[DIK_LEFT] = 1
                    elif 200 <= phase < 230:
                        keys[DIK_RIGHT] = 1
            rec.write(keys + bytes(24))


def status_metrics(line):
    """Pull the interesting counters out of a client `status:` line."""
    out = {}
    for key in ('mism', 'hard', 'mech_hard', 'mech_max_err', 'max_err',
                'unmatched', 'starved', 'rebases', 'tick', 'frames'):
        m = re.search(r'\b' + key + r'=([0-9.]+)', line)
        if m:
            out[key] = float(m.group(1)) if '.' in m.group(1) else int(m.group(1))
    m = re.search(r'resyncs=(\d+)/(\d+)', line)
    if m:
        out['resyncs'] = int(m.group(1))
    # mechanisms=<tracked>/<known>: how many of the sector's mechanisms the
    # session is actually following, i.e. whether the sector woke up at all
    m = re.search(r'mechanisms=(\d+)/(\d+)', line)
    if m:
        out['mechanisms'] = f'{m.group(1)}/{m.group(2)}'
    return out


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
        '  journal_checkpoint_ticks: 660\n'
        'auto_flush_log: true\n'
    )
    (case_dir / 'config.yml').write_text(config, encoding='utf8')

    bindir = pathlib.Path(args.bin).resolve()
    handles, children = [], []
    clients = []
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
                write_drive_record(record, args.level, args.seconds * 66 + 200, i, args.steer)
                cmd += ['--record', str(record)]
            cmd += ['--host', '--expect', str(args.players)] if i == 0 else ['--join-first']
            p = subprocess.Popen(cmd, cwd=client_dir, stdin=subprocess.DEVNULL,
                                 stdout=cf, stderr=subprocess.STDOUT, creationflags=FLAGS)
            clients.append(p)
            children.append(p)
            time.sleep(0.4)

        for p in clients:
            p.wait(timeout=args.seconds + 120)
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
            'final_status': statuses[-1] if statuses else '',
        })
    server_text = (case_dir / 'server.log').read_text(errors='replace')
    result['server_session_lines'] = [l for l in server_text.splitlines()
                                      if 'session' in l.lower() and
                                      ('physicalized' in l or 'bodies' in l or
                                       'started' in l or 'ended' in l or 'correction' in l)]
    result['journal'] = sorted(str(p) for p in journals.glob('*.bmjr')) if journals.exists() else []
    return result


def replay(args, journal, out_dir):
    """The gold-standard check: the server's journal must replay bit for bit."""
    exe = pathlib.Path(args.bin).resolve() / f'BallanceMMOSimTool{EXE}'
    proc = subprocess.run([str(exe), '--root', args.root, '--replay-session', journal,
                           '--report-every', '2000'],
                          capture_output=True, text=True, cwd=out_dir, timeout=900)
    summary = [l for l in (proc.stdout + proc.stderr).splitlines() if l.startswith('summary:')]
    return {'exit_code': proc.returncode,
            'summary': summary[-1] if summary else '',
            'tail': (proc.stdout + proc.stderr)[-1500:]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=str(REPO / 'build-agent/BallanceMMOServer'))
    ap.add_argument('--root', required=True, help='Ballance data directory')
    ap.add_argument('--level', type=int, default=13)
    ap.add_argument('--sectors', default='1,2,3,4')
    ap.add_argument('--players', type=int, default=2)
    ap.add_argument('--seconds', type=int, default=35)
    ap.add_argument('--input-delay', type=int, default=6)
    ap.add_argument('--spawn-impulse', type=float, default=3.0)
    ap.add_argument('--port', type=int, default=32990)
    ap.add_argument('--drive', action='store_true',
                    help='feed the clients an input record that rolls the ball through the bays')
    ap.add_argument('--steer', action='store_true',
                    help='add alternating left/right to the drive record (drifts the ball off the bays)')
    ap.add_argument('--out', default=str(REPO / 'maps/arena-test'))
    ap.add_argument('--label', default=time.strftime('%Y%m%d%H%M%S'))
    args = ap.parse_args()

    base = pathlib.Path(args.out) / args.label
    if base.exists():
        shutil.rmtree(base)
    base.mkdir(parents=True)

    results = []
    base_port = args.port
    for n, sector in enumerate(int(s) for s in args.sectors.split(',')):
        args.port = base_port + n
        print(f'=== sector {sector} (port {args.port}) ===', flush=True)
        case = run_case(args, sector, base / f'sector{sector}')
        for j in case['journal']:
            case.setdefault('replays', []).append(replay(args, j, base))
        results.append(case)
        for i, c in enumerate(case['clients']):
            print(f'  client{i} rc={c["exit_code"]} {c["metrics"]}', flush=True)
        for r in case.get('replays', []):
            print(f'  replay rc={r["exit_code"]} {r["summary"]}', flush=True)
        (base / 'summary.json').write_text(json.dumps(results, indent=2), encoding='utf8')

    print('\nwrote ' + str(base / 'summary.json'))


if __name__ == '__main__':
    main()
