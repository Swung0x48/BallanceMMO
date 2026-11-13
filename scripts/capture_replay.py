"""T3 (docs/canned-tests-plan.md): replay every process of a captured run alone and check it gives its live run
back - the server's journal, each client's journal and send trace, byte for byte.

    python scripts/capture_replay.py CASE_DIR [--bin DIR] [--server-exe NAME] [--only server|clientN ...]

CASE_DIR is a harness case run with --capture (maps/arena-test/LABEL/sectorN): server.bmic, clientN.bmic,
clientN.send, clientN.bmjc, config.yml and the server's journal there.  A replay runs the same binary with no
network and no pacing (--replay-inbound), real-time and coordinated runs alike, and writes into CASE_DIR/replay.
The game data under the capture's --root must be the live run's (a changed level is a divergence too).
Exit 0 when every process gave its run back.
"""
import argparse
import os
import pathlib
import re
import struct
import subprocess
import sys

W = pathlib.Path(__file__).resolve().parent.parent
EXE = '.exe' if os.name == 'nt' else ''


def command_line(capture):
    with open(capture, 'rb') as f:
        head = f.read(20)
        if head[:4] != b'BMIC':
            raise SystemExit('%s is not a capture' % capture)
        return f.read(struct.unpack_from('<I', head, 16)[0]).decode('utf-8')


def first_difference(a, b):
    x, y = a.read_bytes(), b.read_bytes()
    if x == y:
        return None
    n = next((i for i in range(min(len(x), len(y))) if x[i] != y[i]), min(len(x), len(y)))
    return 'differs at byte %d (%d vs %d bytes)' % (n, len(x), len(y))


def newest(d, pattern):
    files = sorted(pathlib.Path(d).glob(pattern), key=lambda p: p.stat().st_mtime)
    return files[-1] if files else None


def replay_server(case, out, bindir, exe, timeout):
    live_dir = pathlib.Path(re.search(r'^\s*journal_dir:\s*(.+?)\s*$', (case / 'config.yml').read_text(), re.M)[1])
    live = newest(live_dir, '*.bmjs')
    journals = out / 'journals'
    journals.mkdir(parents=True, exist_ok=True)
    config = re.sub(r'^(\s*journal_dir:).*$', lambda m: '%s %s' % (m[1], journals.as_posix()),
                    (case / 'config.yml').read_text(), flags=re.M)
    # Step SLQ: the replay's log verbose (a capture made before the key has none).
    config = re.sub(r'^log_verbose:.*\n?', '', config, flags=re.M)
    config = config.rstrip('\n') + '\nlog_verbose: true\n'
    (out / 'config.yml').write_text(config)
    if (case / 'login_data.yml').exists():
        (out / 'login_data.yml').write_bytes((case / 'login_data.yml').read_bytes())
    with open(out / 'server.log', 'wb') as log:
        rc = subprocess.call([str(bindir / (exe + EXE)), '--port', '26996', '--replay-inbound', str(case / 'server.bmic')],
                             cwd=out, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    got = newest(journals, '*.bmjs')
    if not live or not got:
        return False, 'rc %d; journal live %s, replay %s' % (rc, live, got)
    diff = first_difference(live, got)
    return diff is None, 'rc %d; journal %s' % (rc, diff or 'IDENTICAL (%d bytes)' % live.stat().st_size)


# The live run's arguments a replay drops: the network, its outputs, the coordinator's link events.
DROP = {'--server': 1, '--journal': 1, '--capture-inbound': 1, '--replay-inbound': 1, '--trace-sends': 1,
        '--coordinator': 1, '--link-event': 1}


def replay_client(case, name, out, bindir, timeout):
    words = command_line(case / (name + '.bmic')).split(' ')[1:]
    args, i = [], 0
    while i < len(words):
        if words[i] in DROP:
            i += 1 + DROP[words[i]]
            continue
        args.append(words[i])
        i += 1
    journal, sends = out / (name + '.bmjc'), out / (name + '.send')
    cwd = out / name
    cwd.mkdir(parents=True, exist_ok=True)
    cmd = [str(bindir / ('BallanceMMOSessionClient' + EXE)), *args, '--replay-inbound', str(case / (name + '.bmic')),
           '--journal', str(journal), '--trace-sends', str(sends)]
    with open(out / (name + '.log'), 'wb') as log:
        rc = subprocess.call(cmd, cwd=cwd, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    notes, ok = [], True
    for live, got, what in ((case / (name + '.bmjc'), journal, 'journal'), (case / (name + '.send'), sends, 'sends')):
        if not live.exists():
            notes.append('%s: no live file' % what)
            continue
        if not got.exists():
            ok = False
            notes.append('%s: the replay wrote none' % what)
            continue
        diff = first_difference(live, got)
        ok = ok and diff is None
        notes.append('%s %s' % (what, diff or 'IDENTICAL (%d bytes)' % live.stat().st_size))
    diverged = [l for l in (out / (name + '.log')).read_text(errors='replace').splitlines() if 'diverged' in l]
    if diverged:
        ok = False
        notes.append(diverged[0][:200])
    return ok and rc in (0, 2), 'rc %d; %s' % (rc, '; '.join(notes))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('case')
    ap.add_argument('--bin', default=str(W / 'build-agent/BallanceMMOServer'))
    ap.add_argument('--server-exe', default='', help='default: the BallanceMMOServerT1 copy when there is one '
                    '(page heap may be on for BallanceMMOServer), else BallanceMMOServer')
    ap.add_argument('--only', nargs='*', default=None)
    ap.add_argument('--timeout', type=int, default=900)
    args = ap.parse_args()
    case, bindir = pathlib.Path(args.case).resolve(), pathlib.Path(args.bin).resolve()
    exe = args.server_exe or ('BallanceMMOServerT1' if (bindir / ('BallanceMMOServerT1' + EXE)).exists() else 'BallanceMMOServer')
    out = case / 'replay'
    out.mkdir(exist_ok=True)
    names = sorted(p.stem for p in case.glob('*.bmic'))
    if args.only is not None:
        names = [n for n in names if n in args.only]
    if not names:
        print('%s: no captures (a run with --capture?)' % case)
        return 1
    bad = 0
    for name in names:
        ok, text = (replay_server(case, out, bindir, exe, args.timeout) if name == 'server'
                    else replay_client(case, name, out, bindir, args.timeout))
        bad += not ok
        print('%-8s %s  %s' % (name, 'SAME' if ok else 'DIFFERS', text), flush=True)
    print('%d of %d processes gave their run back' % (len(names) - bad, len(names)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
