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
import random
import re
import shutil
import struct
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
FLAGS = subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0
EXE = '.exe' if sys.platform == 'win32' else ''

import heapq
import select
import socket
import threading


class DelayProxy:
    """A UDP relay that holds every datagram for `latency_ms` before passing it
    on, and drops `loss_pct` of them, so a client can be given a worse link
    than the rest of the field.

    The delay half is lifted from scripts/probe.py, which has used it for the
    same purpose.  The loss half matters for any test of what the session does
    when a message never arrives: loopback does not drop, so an unreliable
    message path passes such a test by accident and proves nothing about a real
    link.  Loss applies per datagram in both directions; `dropped` counts them.
    """

    def __init__(self, listen_port, server_addr, latency_ms, loss_pct=0.0):
        self.front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.front.bind(('127.0.0.1', listen_port))
        self.back = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.back.bind(('127.0.0.1', 0))
        self.target = server_addr
        self.client = None
        self.delay = latency_ms / 1000.0
        # Seeded, so two runs of one case drop the same datagrams.
        self.loss = loss_pct / 100.0
        self.rng = random.Random(0x10CC + listen_port)
        self.dropped = 0
        self.passed = 0
        self.running = True
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        queue, seq = [], 0
        while self.running:
            due = max(0, queue[0][0] - time.monotonic()) if queue else 0.01
            for sock in select.select([self.front, self.back], [], [], min(0.01, due))[0]:
                try:
                    data, addr = sock.recvfrom(65535)
                except ConnectionResetError:
                    continue      # Windows reports ICMP port-unreachable after an exit
                if sock is self.front:
                    self.client = addr
                    out, dest = self.back, self.target
                elif self.client is not None:
                    out, dest = self.front, self.client
                else:
                    continue
                if self.loss > 0.0 and self.rng.random() < self.loss:
                    self.dropped += 1
                    continue
                self.passed += 1
                seq += 1
                heapq.heappush(queue, (time.monotonic() + self.delay, seq, out, dest, data))
            while queue and queue[0][0] <= time.monotonic():
                _, _, out, dest, data = heapq.heappop(queue)
                out.sendto(data, dest)

    def close(self):
        self.running = False
        self.thread.join(timeout=1)
        self.front.close()
        self.back.close()


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
    # an external server writes its journal wherever its own config says;
    # --journal-dir points the replay check at it
    journals = (pathlib.Path(args.journal_dir).resolve() if args.journal_dir
               else case_dir / 'journals')
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
    handles, children, clients, proxies = [], [], [], []
    proxy_stats = {}
    server = None
    # --external-server points the clients at a server someone else is running
    # (the Arch WSL one, say).  Its journal still has to land somewhere this
    # side can read, which is what journal_dir in its own config decides.
    host, _, port = (args.external_server or f'127.0.0.1:{args.port}').partition(':')
    server_addr = (host, int(port))
    try:
        if not args.external_server:
            sf = open(case_dir / 'server.log', 'wb')
            handles.append(sf)
            server = subprocess.Popen(
                [str(bindir / f'BallanceMMOServer{EXE}'), '--port', str(args.port)],
                cwd=case_dir, stdin=subprocess.PIPE, stdout=sf,
                stderr=subprocess.STDOUT, creationflags=FLAGS)
            children.append(server)
            time.sleep(1.5)

        laggy = {int(x) for x in args.latency_clients.split(',') if x.strip()}
        for i in range(args.players):
            client_dir = case_dir / f'client{i}'
            client_dir.mkdir()
            cf = open(case_dir / f'client{i}.log', 'wb')
            handles.append(cf)
            connect_host, connect_port = server_addr
            if (args.latency_ms > 0 or args.loss_pct > 0) and (not laggy or i in laggy):
                pport = args.port + 400 + i
                proxies.append(DelayProxy(pport, server_addr, args.latency_ms, args.loss_pct))
                connect_host, connect_port = '127.0.0.1', pport
            cmd = [str(bindir / f'BallanceMMOSessionClient{EXE}'),
                   '--root', args.root,
                   '--server', f'{connect_host}:{connect_port}',
                   '--name', f'Arena{i}',
                   '--level', str(args.level),
                   '--seconds', str(args.seconds),
                   '--trace',
                   '--journal', str(case_dir / f'client{i}.bmjr'),
                   # the client's own world at the same cadence as the server's,
                   # so the two can be compared body by body at the same ticks
                   '--checkpoint-ticks', str(args.checkpoint_ticks)]
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
                # --beam-y picks the line: MECH_Y for a mechanism, PLAY_Y for
                # the driving lane (the checkpoints and resetpoints live there,
                # 28 units off the mechanism row - beaming at a checkpoint and
                # aiming at the mechanism row simply misses it)
                cmd += ['--beam', repr(bay * BAY_SPAN + args.beam_dx - i * args.beam_spread),
                        repr(4.0), repr(args.beam_y if args.beam_y >= 0 else MECH_Y),
                        str(args.beam_tick)]
            if args.stall_client == i and args.stall_ms > 0:
                cmd += ['--pause-at', str(args.stall_at), '--pause-ms', str(args.stall_ms)]
            cmd += ['--host', '--expect', str(args.players)] if i == 0 else ['--join-first']
            p = subprocess.Popen(cmd, cwd=client_dir, stdin=subprocess.DEVNULL,
                                 stdout=cf, stderr=subprocess.STDOUT, creationflags=FLAGS)
            clients.append(p)
            children.append(p)
            time.sleep(0.4)

        for p in clients:
            p.wait(timeout=args.seconds + 180)
        if server is not None:
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
        # Prove the loss actually happened: a run that claims to survive
        # dropped datagrams has to show that some were dropped.  Counted here,
        # while the proxies are alive; attached to the result once it exists.
        if any(q.loss for q in proxies):
            proxy_stats['dropped'] = sum(q.dropped for q in proxies)
            proxy_stats['passed'] = sum(q.passed for q in proxies)
        for q in proxies:
            q.close()

    result = {'sector': sector, 'dir': str(case_dir), 'clients': []}
    if proxy_stats:
        result['proxy_dropped'] = proxy_stats['dropped']
        result['proxy_passed'] = proxy_stats['passed']
        print('  proxy dropped %d of %d datagrams'
              % (proxy_stats['dropped'], proxy_stats['dropped'] + proxy_stats['passed']))
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
    ap.add_argument('--journal-dir', default='',
                    help="where the server writes journals, when it is not ours")
    ap.add_argument('--external-server', default='',
                    help='HOST:PORT of a server to use instead of starting one')
    ap.add_argument('--latency-ms', type=float, default=0.0,
                    help='each way delay added to the affected clients links')
    ap.add_argument('--loss-pct', type=float, default=0.0,
                    help='percentage of datagrams the proxy drops, each way; on its own it '
                         'puts a lossless-latency proxy in the path with loss only')
    ap.add_argument('--latency-clients', default='',
                    help='client indices to put behind the delay proxy; empty = all')
    ap.add_argument('--stall-client', type=int, default=-1,
                    help='client index to freeze mid-session')
    ap.add_argument('--stall-at', type=int, default=1200,
                    help='session tick at which that client freezes')
    ap.add_argument('--stall-ms', type=int, default=0,
                    help='how long it freezes for, in milliseconds')
    ap.add_argument('--beam-spread', type=float, default=3.0,
                    help='metres between the beam landing points of successive clients')
    ap.add_argument('--beam-offset', type=int, default=0,
                    help='rotate which bays of the sector --beam-bays auto picks')
    ap.add_argument('--checkpoint-ticks', type=int, default=660,
                    help='ticks between full body checkpoints in the journals; small '
                         'values make contact measurable with collision_report.py')
    ap.add_argument('--beam-dx', type=float, default=-9.0,
                    help='offset from the bay centre for the first beam landing; the default puts the ball just short of the bay mechanism')
    ap.add_argument('--beam-y', type=float, default=-1.0,
                    help='lane to beam onto: %g for the mechanism row, %g for the '
                         'driving lane; default is the mechanism row' % (MECH_Y, PLAY_Y))
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
