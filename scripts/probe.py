import argparse, json, os, pathlib, shutil, struct, subprocess, time, re
import heapq, select, socket, threading

ROOT = pathlib.Path(__file__).resolve().parent / 'probe'
REPO = pathlib.Path('D:/repos/BallanceMMO')
DATA = 'C:/Users/geekerwan/Downloads/Ballance-MMOTest'
FLAGS = subprocess.CREATE_NO_WINDOW

class DelayProxy:
    def __init__(self, listen_port, server_port, latency_ms):
        self.front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.front.bind(('127.0.0.1', listen_port))
        self.back = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.back.bind(('127.0.0.1', 0))
        self.target = ('127.0.0.1', server_port)
        self.client = None
        self.delay = latency_ms / 1000
        self.running = True
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()
    def run(self):
        queue, seq = [], 0
        while self.running:
            due = max(0, queue[0][0] - time.monotonic()) if queue else .01
            for sock in select.select([self.front, self.back], [], [], min(.01, due))[0]:
                try:
                    data, addr = sock.recvfrom(65535)
                except ConnectionResetError:
                    # Windows reports an ICMP port-unreachable after a client exits.
                    continue
                if sock is self.front:
                    self.client = addr
                    out, dest = self.back, self.target
                elif self.client is not None:
                    out, dest = self.front, self.client
                else: continue
                seq += 1
                heapq.heappush(queue, (time.monotonic() + self.delay, seq, out, dest, data))
            while queue and queue[0][0] <= time.monotonic():
                _, _, out, dest, data = heapq.heappop(queue)
                out.sendto(data, dest)
    def close(self):
        self.running = False
        self.thread.join(timeout=1)
        self.front.close(); self.back.close()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', default=str(REPO/'build/physics-e2e-20260909/windows-baseline/x64-server/out/BallanceMMOServer'))
    ap.add_argument('--client-exe')
    ap.add_argument('--label', default='baseline')
    ap.add_argument('--seconds', default=40, type=int)
    ap.add_argument('--cases', default='8:2:idle,8:4:idle,11:2:idle,11:4:idle')
    ap.add_argument('--base-port', default=32780, type=int)
    ap.add_argument('--latency-ms', default=0, type=float)
    args = ap.parse_args()
    basedir = ROOT/args.label
    basedir.mkdir(exist_ok=True)
    bindir = basedir/'bin'
    bindir.mkdir(exist_ok=True)
    source = pathlib.Path(args.source)
    for name in ['BallanceMMOServer.exe', 'BallanceMMOSessionClient.exe']:
        origin = pathlib.Path(args.client_exe) if args.client_exe and name == 'BallanceMMOSessionClient.exe' else source/name
        shutil.copy2(origin, bindir/name)
    for dll in source.glob('*.dll'):
        shutil.copy2(dll, bindir/dll.name)
    summary = []
    for index, case in enumerate(args.cases.split(',')):
        spec = case.split('@')
        level, players, mode = spec[0].split(':')
        level, players = int(level), int(players)
        beams, start_sector = [], None
        for extra in spec[1:]:
            key, _, value = extra.partition('=')
            if key == 'beam':
                for b in value.split(';'):
                    x, y, z, t = b.split(',')
                    beams.append((float(x), float(y), float(z), int(t)))
            elif key == 'sector':
                start_sector = int(value)
        case = spec[0]
        port = args.base_port + index
        case_dir = basedir/f'L{level}_{players}p_{mode}' if not (beams or start_sector) else basedir/f'L{level}_{players}p_{mode}_beam{index}'
        case_dir.mkdir(exist_ok=False)
        config = ('rooms_enabled: true\nmaximum_members: 8\nphysics:\n'
                  f'  enabled: true\n  game_root: {DATA}\n'
                  '  snapshot_interval: 2\n  input_delay: 6\n  maximum_physics_rooms: 1\n'
                  '  event_rate_limit: 20\n  spawn_impulse: 3\n'
                  f'  journal_dir: {str(case_dir / "journals").replace(chr(92), "/")}\n'
                  '  journal_max_mb: 64\n  journal_checkpoint_ticks: 660\nauto_flush_log: true\n')
        (case_dir/'config.yml').write_text(config)
        handles, children, proxies = [], [], []
        print(f'START {args.label} {case} port={port}', flush=True)
        try:
            sf = open(case_dir/'server.log', 'wb'); handles.append(sf)
            server = subprocess.Popen([str(bindir/'BallanceMMOServer.exe'), '--port', str(port)], cwd=case_dir,
                                      stdin=subprocess.PIPE, stdout=sf, stderr=subprocess.STDOUT, creationflags=FLAGS)
            children.append(server)
            time.sleep(1)
            clients = []
            for i in range(players):
                client_port = port
                if args.latency_ms:
                    client_port = args.base_port + 100 + index * 8 + i
                    proxies.append(DelayProxy(client_port, port, args.latency_ms))
                client_dir = case_dir/f'client{i}'
                client_dir.mkdir()
                logpath = case_dir/f'client{i}.log'
                cf = open(logpath, 'wb'); handles.append(cf)
                cmd = [str(bindir/'BallanceMMOSessionClient.exe'), '--root', DATA,
                       '--server', f'127.0.0.1:{client_port}', '--name', f'Probe{i}', '--level', str(level),
                       '--seconds', str(args.seconds), '--trace', '--journal', str(case_dir/f'client{i}.bmjr')]
                if start_sector: cmd += ['--start-sector', str(start_sector)]
                # beams: one per client (cycled), applied at the given session tick
                if beams:
                    bx, by, bz, bt = beams[i % len(beams)]
                    cmd += ['--beam', repr(bx), repr(by), repr(bz), str(bt)]
                if i == 0: cmd += ['--host', '--expect', str(players)]
                else: cmd += ['--join-first']
                if mode == 'edges':
                    record = case_dir/f'input{i}.bmrc'
                    with record.open('wb') as rec:
                        rec.write(struct.pack('<4sIdi64s32s4x', b'BMRC', 1, 66., level, b'', b''))
                        for tick in range(args.seconds * 66):
                            keys = bytearray(256)
                            # Brief alternating left/right key edges, preserving proximity to spawn.
                            if tick >= 120 and (tick + i * 5) % 132 < 8:
                                keys[203 if (tick // 132) % 2 == 0 else 205] = 1
                            rec.write(keys + bytes(24))
                    cmd += ['--record', str(record)]
                p = subprocess.Popen(cmd, cwd=client_dir, stdin=subprocess.DEVNULL, stdout=cf,
                                     stderr=subprocess.STDOUT, creationflags=FLAGS)
                clients.append(p); children.append(p)
                time.sleep(.35)
            for p in clients: p.wait(timeout=args.seconds + 30)
            server.stdin.write(b'sessions\nstop\n'); server.stdin.flush()
            try: server.wait(timeout=8)
            except subprocess.TimeoutExpired: server.terminate(); server.wait(timeout=5)
        finally:
            for p in children:
                if p.poll() is None: p.terminate()
            for h in handles: h.close()
            for p in proxies: p.close()
        item = {'case': case, 'directory': str(case_dir), 'clients': []}
        for i, p in enumerate(clients):
            lines = (case_dir/f'client{i}.log').read_text(errors='replace').splitlines()
            statuses = [s for s in lines if s.startswith('status:')]
            final = statuses[-1] if statuses else ''
            item['clients'].append({'client': i, 'exit_code': p.returncode, 'final_status': final})
            print(f'END {case} client={i} rc={p.returncode} {final}', flush=True)
        summary.append(item)
        (basedir/'summary.json').write_text(json.dumps(summary, indent=2))

if __name__ == '__main__': main()
