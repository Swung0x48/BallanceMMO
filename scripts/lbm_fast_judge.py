"""The fast LBM layer's judge (docs/canned-tests-plan.md, step LF): one block per twin of every arm - the
harness's criteria (counts, every fail), the server journal's replay, and journal_judge's FINAL comparison of
every client's world with the server's.  The raw criteria are read as LBM6's were: against the real-time run's
rows for the same arm, not as a pass/fail of their own.

    python scripts/lbm_fast_judge.py [--root GAME] [--simtool PATH] [--on-only | --twins] [--rows-out FILE]
        [--rows-ref FILE] [--json-out FILE] LABEL...   (LABEL_off / LABEL_on in maps/arena-test)

The pass (exit 0): every run FINAL exact (and its log checks clean), every expected run there (--on-only: each
LABEL_on, --twins: LABEL_off and LABEL_on; neither: what exists), and with --rows-ref the rows equal to that
file's - a previous run of the same mode, its --rows-out.  On-only runs have no off twin: their 'relay lag in D'
rows are n/a, counted per run below as skipped.

The rows (step JNL2, the user 2026-11-14: a criterion's row carries what it measured): per run, the label's prefix
cut, its verdict line, then one 'crit' line per criterion (who | criterion | STATUS | the whole detail), one
'value' line per measured value of the event report the criteria read (every client's phase figures and row
fields, the server's and the S1 meter's, at full precision; a value over VALUE_MAX characters as its length and
sha256), and journal_judge's FINAL lines.  Under the coordinator's virtual time every one of them is the same run
to run, so a value that moves - in a row that fails or has no control too - differs from the reference.  Left out:
RAW_KEYS, the S1 quorum's seen-at and step-lag-ms (steady-clock reads: session_runner.cpp's arrival_queue and
step_lag_ms, class 2), and the server journal's path.
"""
import argparse
import difflib
import hashlib
import json
import pathlib
import re
import subprocess
import sys

W = pathlib.Path(__file__).resolve().parent.parent
A = W / 'maps/arena-test'
RAW_KEYS = ('seen-at', 'step-lag-ms', 'server_journal')
VALUE_MAX = 2000
CUT_DIFF = re.compile(r'(?:name set|random hook) verify: DIFFERS[^\n]{0,120}'
                      r'|random hook(?: verify)?: the other \d+ managers DIFFER in order')


def without_raw(x):
    if isinstance(x, dict):
        return {k: without_raw(v) for k, v in x.items() if k not in RAW_KEYS}
    if isinstance(x, list):
        return [without_raw(v) for v in x]
    return x


def value_text(v):
    text = json.dumps(v, sort_keys=True, separators=(',', ':'))
    if len(text) <= VALUE_MAX:
        return text
    return 'n=%d sha256=%s' % (len(text), hashlib.sha256(text.encode('utf-8')).hexdigest()[:24])


def value_lines(report):
    """The event report's values as 'path = value' texts, the raw clock keys left out."""
    report = without_raw(report)
    out = []
    for key, v in sorted(report.items()):
        if key == 'clients':
            for row in v:
                who = 'client%s' % row.get('index')
                for k, x in sorted(row.items()):
                    if k == 'phases':
                        out += ['%s.phases.%s = %s' % (who, p, value_text(f)) for p, f in sorted(x.items())]
                    else:
                        out.append('%s.%s = %s' % (who, k, value_text(x)))
        elif key in ('server', 's1') and isinstance(v, dict):
            for k, x in sorted(v.items()):
                if isinstance(x, list) and len(json.dumps(x, sort_keys=True, separators=(',', ':'))) > VALUE_MAX:
                    out += ['%s.%s[%d] = %s' % (key, k, i, value_text(y)) for i, y in enumerate(x)]
                else:
                    out.append('%s.%s = %s' % (key, k, value_text(x)))
        else:
            out.append('%s = %s' % (key, value_text(v)))
    return out


def judge(case, root, simtool):
    sector = next(iter(sorted(case.glob('sector*'))), case / 'sector1')   # the swing variant runs sector 4
    out = {'label': case.name}
    try:
        summary = json.loads((case / 'summary.json').read_text(encoding='utf-8'))[0]
    except (OSError, ValueError, IndexError) as e:
        out['error'] = 'no summary: %s' % e
        return out
    crit = summary.get('criteria') or []
    out['criteria'] = {s: sum(1 for c in crit if c['status'] == s) for s in ('pass', 'fail', 'masked', 'n/a')}
    out['crit_rows'] = ['%s | %s | %s | %s' % (c['who'], c['criterion'], c['status'].upper(), c['detail']) for c in crit]
    out['values'] = value_lines(summary['event_report']) if summary.get('event_report') else ['no event report']
    out['failures'] = ['%s %s: %s' % (c['who'], c['criterion'], c['detail'][:160])
                       for c in crit if c['status'] in ('fail', 'masked')]
    out['no_twin'] = sum(1 for c in crit if c['status'] == 'n/a' and c['detail'].startswith('n/a: no off twin'))
    out['replay'] = [r['summary'].replace('summary: ', '') for r in summary.get('replays') or []]
    # Step SLQ: a server log that is not verbose leaves the server-read rows
    # empty on both twins (equal, so silent): the twin fails.
    if summary.get('server_log_level'):
        out['server_log_level'] = summary['server_log_level']
    # Step JNL, while the proof lasts: the criteria read from the journals must equal the ones read from the
    # logs (arena_map_test's criteria_source_check); a twin whose check says otherwise fails.  No key: not
    # checked (--no-log-source-check, or no event report), said, not failed.
    out['criteria_source_check'] = summary.get('criteria_source_check')
    # Step DWB: a work cut's verify difference (BMMO_WORK_CUTS=verify) or a manager table whose order the load hook
    # moved, in any engine's log, fails the twin.
    out['cut_differences'] = sorted({m.group(0) for f in sector.glob('*.log')
                                     for m in CUT_DIFF.finditer(f.read_text(encoding='utf-8', errors='replace'))})
    servers = sorted((sector / 'journals').glob('*.bmjs'))
    clients = sorted(sector.glob('client*.bmjc'))
    if servers and clients:
        p = subprocess.run([sys.executable, '-X', 'utf8', '-I', str(W / 'scripts/journal_judge.py'), '--server', str(servers[-1]),
                            '--clients', *map(str, clients), '--simtool', simtool, '--root', root],
                           capture_output=True, text=True, encoding='utf-8', errors='replace')
        out['final'] = [re.sub(r'\s+', ' ', l.strip()) for l in p.stdout.splitlines() if 'bodies exact' in l]
        out['final_exact'] = bool(out['final']) and all(' PASS ' in l for l in out['final'])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='C:/Users/geekerwan/Desktop/Ballance-Agent')
    ap.add_argument('--simtool', default=str(W / 'build-agent/BallanceMMOServer/BallanceMMOSimTool.exe'))
    group = ap.add_mutually_exclusive_group()
    group.add_argument('--on-only', action='store_true', help='each LABEL_on must be there (lbm_fast.sh by default)')
    group.add_argument('--twins', action='store_true', help='each LABEL_off and LABEL_on must be there')
    ap.add_argument('--rows-out', help='write the criteria rows here')
    ap.add_argument('--rows-ref', help='the rows must equal this file (a previous --rows-out of the same mode)')
    ap.add_argument('--json-out', help='the judged runs as JSON (default maps/arena-test/PREFIX-lbm-fast.json)')
    ap.add_argument('labels', nargs='+')
    args = ap.parse_args()
    prefix = args.labels[0].split('_')[0]
    rows, lines = [], []
    for label in args.labels:
        for mode in ('off', 'on'):
            case = A / ('%s_%s' % (label, mode))
            if not case.exists():
                if args.twins or (args.on_only and mode == 'on'):
                    rows.append({'label': case.name, 'error': 'missing'})
                    print('%-28s FAIL missing: %s' % (case.name, case), flush=True)
                continue
            r = judge(case, args.root, args.simtool)
            rows.append(r)
            c = r.get('criteria', {})
            line = '%-28s FINAL %s | criteria pass %s fail %s n/a %s | replay %s' % (
                r['label'], {True: 'EXACT', False: 'DIFFERENT'}.get(r.get('final_exact'), '?'),
                c.get('pass'), c.get('fail'), c.get('n/a'), '; '.join(r.get('replay', [])) or r.get('error', ''))
            print(line, flush=True)
            run = r['label'][len(prefix) + 1:]
            lines.append(' '.join(line[len(prefix) + 1:].split()))
            lines += ['%s crit %s' % (run, c) for c in r.get('crit_rows', [])]
            lines += ['%s value %s' % (run, v) for v in r.get('values', [])]
            lines += ['%s final %s' % (run, f) for f in r.get('final', [])]
            if args.on_only and r.get('no_twin'):
                print('    skipped (no off twin, n/a): %d "relay lag in D" rows' % r['no_twin'], flush=True)
            for f in r.get('failures', []):
                print('    fail ' + f, flush=True)
            if r.get('server_log_level'):
                print('    FAIL (not counted exact) ' + r['server_log_level'], flush=True)
            check = r.get('criteria_source_check')
            if check is None:
                print('    criteria from the journals: not checked against the logs', flush=True)
            elif check != 'equal':
                print('    FAIL (not counted exact) criteria from the journals vs the logs: ' + check, flush=True)
            for d in r.get('cut_differences', []):
                print('    FAIL (not counted exact) ' + d[:170], flush=True)
            for l in r.get('final', []):
                if ' PASS ' not in l:
                    print('    ' + l[:170], flush=True)
    exact = sum(1 for r in rows if r.get('final_exact') and not r.get('server_log_level')
                and r.get('criteria_source_check') in (None, 'equal') and not r.get('cut_differences'))
    rows_ok = True
    if args.rows_out:
        pathlib.Path(args.rows_out).write_bytes(''.join(l + '\n' for l in lines).encode('utf-8'))
    if args.rows_ref:
        ref = pathlib.Path(args.rows_ref).read_text(encoding='utf-8').splitlines()
        rows_ok = ref == lines
        print('rows (%d lines: verdicts, criteria, values, FINAL) %s %s' % (
            len(lines), 'equal to' if rows_ok else 'DIFFER from', args.rows_ref), flush=True)
        diff = [l for l in difflib.unified_diff(ref, lines, 'ref', 'this', n=0, lineterm='')
                if not l.startswith(('---', '+++', '@@'))]
        for l in diff[:60]:
            print('    %s %s' % ('ref ' if l[0] == '-' else 'this', l[1:][:300]), flush=True)
        if len(diff) > 60:
            print('    ... %d more differing lines' % (len(diff) - 60), flush=True)
    print('%d of %d %s FINAL exact' % (exact, len(rows), 'on runs' if args.on_only else 'twins'))
    pathlib.Path(args.json_out or A / ('%s-lbm-fast.json' % prefix)).write_text(json.dumps(rows, indent=2),
                                                                                  encoding='utf-8')
    return 0 if exact == len(rows) and rows and rows_ok else 1


if __name__ == '__main__':
    sys.exit(main())
