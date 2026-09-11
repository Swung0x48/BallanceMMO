"""Two-retail-client physics session harness (local or remote server + two Player.exe copies + command channel).

Machine-specific paths (the two game copies, the build trees) are the constants below; adjust them.
Journals, logs and transcripts of a run are collected under ./retail/<label>/ next to this script.
Processes started from an agent sandbox may be killed ~10 min after launch: keep a run under that.

usage:
  python retail_test.py install            copy the built mod + physics_RT.dll into the two game copies (backs up the current files)
  python retail_test.py restore            put the backed-up files back
  python retail_test.py start [--port P]   start the local server and both clients (hidden), connect, load level 11
  python retail_test.py cmd <ids> <cmd...> send commands (ids like 1,2) and print the responses
  python retail_test.py session            room create/join/ready/start physics on the running clients
  python retail_test.py stop               close room, quit clients, stop server, collect journals + logs into ./retail/<label>/
"""
import json, os, pathlib, shutil, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent / 'retail'
REPO = pathlib.Path('D:/repos/BallanceMMO')
SERVER_DIR = REPO / 'build/physics-e2e-20260909/windows-baseline/x64-server/out/BallanceMMOServer'
CLIENT_BUILD = REPO / 'build/physics-e2e-20260909/windows-baseline/x86-client/out'
COPIES = {1: pathlib.Path('C:/Users/geekerwan/Downloads/Ballance-MMOTest'),
          2: pathlib.Path('C:/Users/geekerwan/Downloads/Ballance-MMOTest - Copy')}
DATA = str(COPIES[1]).replace('\\', '/')
FLAGS = subprocess.CREATE_NO_WINDOW

def client_dir(i):
    d = ROOT / f'client-{i}'
    d.mkdir(parents=True, exist_ok=True)
    return d

def send(i, commands, timeout=20):
    root = client_dir(i)
    path = root / 'command.txt'
    out = root / 'command.txt.out'
    if path.exists():
        raise RuntimeError('pending command: ' + str(path))
    if out.exists():
        out.unlink()
    pending = root / 'command.pending'
    pending.write_text('\n'.join(commands) + '\n', encoding='utf8')
    start = time.monotonic()
    pending.replace(path)
    while time.monotonic() - start < timeout:
        if out.exists():
            time.sleep(0.05)
            response = out.read_text(encoding='utf8', errors='replace').strip()
            out.unlink()
            with (root / 'transcript.jsonl').open('a', encoding='utf8') as f:
                f.write(json.dumps({'t': time.time(), 'i': i, 'commands': commands, 'response': response}, ensure_ascii=False) + '\n')
            return response
        time.sleep(0.02)
    raise TimeoutError(str(path))

def install():
    for i, copy in COPIES.items():
        backup = ROOT / f'backup-{i}'
        backup.mkdir(parents=True, exist_ok=True)
        for rel in ['ModLoader/Mods/BallanceMMOClient.bmodp', 'BuildingBlocks/physics_RT.dll']:
            src = copy / rel
            dst = backup / pathlib.Path(rel).name
            if src.exists() and not dst.exists():
                shutil.copy2(src, dst)
        shutil.copy2(CLIENT_BUILD / 'BallanceMMOClient/BallanceMMOClient.bmodp', copy / 'ModLoader/Mods/BallanceMMOClient.bmodp')
        shutil.copy2(CLIENT_BUILD / 'BuildingBlocks/physics_RT.dll', copy / 'BuildingBlocks/physics_RT.dll')
        print('installed into', copy)

def restore():
    for i, copy in COPIES.items():
        backup = ROOT / f'backup-{i}'
        for rel in ['ModLoader/Mods/BallanceMMOClient.bmodp', 'BuildingBlocks/physics_RT.dll']:
            src = backup / pathlib.Path(rel).name
            if src.exists():
                shutil.copy2(src, copy / rel)
                print('restored', copy / rel)

def start(port, trace=False, client_trace=None, server_trace=None, remote=None):
    if client_trace is None: client_trace = trace
    if server_trace is None: server_trace = trace
    ROOT.mkdir(parents=True, exist_ok=True)
    sdir = ROOT / 'server'
    sdir.mkdir(exist_ok=True)
    (sdir / 'journals').mkdir(exist_ok=True)
    if remote:
        (sdir / 'pid.txt').write_text('0')
        (sdir / 'remote.txt').write_text(remote)
    elif (sdir / 'remote.txt').exists():
        (sdir / 'remote.txt').unlink()
    config = ('enable_op_privileges: true\nrestart_level_after_countdown: true\nforce_restart_after_countdown: false\n'
              'rooms_enabled: true\nmaximum_rooms: 64\nmaximum_members: 8\nphysics:\n'
              f'  enabled: true\n  game_root: {DATA}\n  snapshot_interval: 2\n  input_delay: 10\n'
              '  maximum_physics_rooms: 1\n  debug_trace: ' + ('true' if server_trace else 'false') + '\n  event_rate_limit: 100\n  spawn_impulse: 3\n'
              '  journal_dir: journals\n  journal_max_mb: 256\n  journal_checkpoint_ticks: 660\n  allowed_mods: {}\n'
              'logging_level: important\nauto_flush_log: true\n')
    (sdir / 'config.yml').write_text(config)
    # The server treats EOF on its console as "stop", so it must not inherit a
    # pipe this process closes: scripts/run_server.py keeps the pipe open for
    # as long as it lives and forwards lines from a command file.
    if (sdir / 'server.log').exists():
        (sdir / 'server.log').unlink()
    cmdfile = sdir / 'server-commands.txt'
    if remote:
        print('remote server', remote)
    else:
        _start_local_server(sdir, cmdfile, port)
    _start_clients(port, client_trace, remote)


def _start_local_server(sdir, cmdfile, port):
    if cmdfile.exists():
        cmdfile.unlink()
    (sdir / 'config.yml').write_text((sdir / 'config.yml').read_text())
    launcher = subprocess.Popen([sys.executable, str(pathlib.Path(__file__).resolve().parent / 'run_server_local.py'),
                                 str(SERVER_DIR / 'BallanceMMOServer.exe') + ' --port ' + str(port),
                                 str(sdir), str(sdir / 'server.log'), str(cmdfile)],
                                cwd=sdir, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)
    (sdir / 'pid.txt').write_text(str(launcher.pid))
    print('server launcher pid', launcher.pid, 'port', port)
    time.sleep(2.5)


def _start_clients(port, client_trace, remote):
    for i, copy in COPIES.items():
        d = client_dir(i)
        for name in ['command.txt', 'command.txt.out']:
            p = d / name
            if p.exists():
                p.unlink()
        env = dict(os.environ)
        env['BMMO_COMMAND_FILE'] = str(d / 'command.txt')
        env['BMMO_COMMAND_PIPE'] = ''
        env['BMMO_PHYSICS_STDOUT'] = str(d / 'physics-stdout.log')
        if '--perturb' in sys.argv:
            env['BMMO_SIM_ALLOC_PERTURB'] = '1'
        for stale in ['Bin/Player.log', 'ModLoader/ModLoader.log']:
            p = copy / stale
            if p.exists():
                p.unlink()
        p = subprocess.Popen([str(copy / 'Bin/Player.exe')], cwd=copy / 'Bin', env=env,
                             creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        (d / 'pid.txt').write_text(str(p.pid))
        print('client', i, 'pid', p.pid)
        time.sleep(1.0)
    print('waiting for the clients to reach the menu...')
    for attempt in range(60):
        time.sleep(1.0)
        try:
            r1 = send(1, ['ping'], timeout=3)
            r2 = send(2, ['ping'], timeout=3)
            print(r1, '|', r2)
            break
        except (TimeoutError, RuntimeError):
            for i in (1, 2):
                p = client_dir(i) / 'command.txt'
                if p.exists():
                    p.unlink()
    # The mod fills its nickname a few seconds after the menu is up; connecting
    # earlier logs in with an empty name and the server refuses it.
    time.sleep(10)
    for i in (1, 2):
        print(i, send(i, ['journal on', 'session trace ' + ('on' if client_trace else 'off'), 'mmo connect ' + (remote if remote else f'127.0.0.1:{port}')]))
    time.sleep(3)
    for i in (1, 2):
        print(i, send(i, ['level 11']))
        if i == 1 and '--stagger' in sys.argv:
            time.sleep(float(sys.argv[sys.argv.index('--stagger') + 1]))
    time.sleep(12)
    for i in (1, 2):
        print(i, send(i, ['status']))

def session():
    print(1, send(1, ['mmo room create RetailTest']))
    time.sleep(1.0)
    print(2, send(2, ['mmo room join 1']))
    time.sleep(0.5)
    print(2, send(2, ['mmo room ready']))
    print(1, send(1, ['mmo room ready']))
    print(1, send(1, ['mmo room start physics']))
    time.sleep(8)
    for i in (1, 2):
        print(i, send(i, ['status', 'session']))

def stop(label):
    try:
        print(1, send(1, ['keys clear', 'journal mark test_finished', 'session', 'mmo room close']))
    except Exception as e:
        print('close failed', e)
    for i in (1, 2):
        try:
            print(i, send(i, ['journal off', 'mmo disconnect', 'quit'], timeout=10))
        except Exception as e:
            print('quit failed', i, e)
    time.sleep(2)
    sdir = ROOT / 'server'
    remote = (sdir / 'remote.txt').exists()
    try:
        if remote:
            raise RuntimeError('remote server: nothing to stop')
        (sdir / 'server-commands.txt').write_text('stop\n')
        time.sleep(3)
        pid = int((sdir / 'pid.txt').read_text())
        subprocess.run(['taskkill', '/PID', str(pid), '/F', '/T'], capture_output=True)
    except Exception as e:
        print('server stop', e)
    dest = ROOT / label
    dest.mkdir(parents=True, exist_ok=True)
    if not remote:
        for f in (sdir / 'journals').glob('*.bmjr'):
            shutil.copy2(f, dest / f.name)
        shutil.copy2(sdir / 'server.log', dest / 'server.log')
    for i, copy in COPIES.items():
        jd = copy / 'ModLoader/BMMOJournals'
        newest = sorted(jd.glob('*.bmjr'), key=lambda p: p.stat().st_mtime)[-1:] if jd.exists() else []
        for f in newest:
            shutil.copy2(f, dest / f'client{i}_{f.name}')
        log = copy / 'ModLoader/ModLoader.log'
        if log.exists():
            shutil.copy2(log, dest / f'client{i}_ModLoader.log')
        t = client_dir(i) / 'transcript.jsonl'
        if t.exists():
            shutil.copy2(t, dest / f'client{i}_transcript.jsonl')
    print('collected into', dest)

if __name__ == '__main__':
    what = sys.argv[1]
    if what == 'install':
        install()
    elif what == 'restore':
        restore()
    elif what == 'start':
        port = 26688
        if '--port' in sys.argv:
            port = int(sys.argv[sys.argv.index('--port') + 1])
        start(port, remote=(sys.argv[sys.argv.index('--server') + 1] if '--server' in sys.argv else None),
              trace='--trace' in sys.argv,
              client_trace=True if '--client-trace' in sys.argv else None,
              server_trace=True if '--server-trace' in sys.argv else None)
    elif what == 'cmd':
        ids = [int(x) for x in sys.argv[2].split(',')]
        for i in ids:
            print(i, send(i, sys.argv[3:]))
    elif what == 'session':
        session()
    elif what == 'stop':
        stop(sys.argv[2] if len(sys.argv) > 2 else 'run')
