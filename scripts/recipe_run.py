#!/usr/bin/env python3
"""Run a recipe, every life at its ticks (docs/canned-tests-plan.md, step T2).

    recipe_run.py RECIPE.bmrp --label LABEL [arena_map_test.py arguments: --bin, --root, --latency-ms, ...]

A recipe holds the ticks the original's server gave each life: its join, its start (the base) and its leave. The
server decides them from when its messages arrive, so each re-driven client is timed: its Join, its SessionReady (its
level load waits, as the original's player's machine did) and its leave - the recipe's calibration, calibration.json in
the .bmrp. The server is a black box here: its ticks go on a fixed grid of the session clock (66 a second), so a
message sent d ms later lands d * 66 / 1000 ticks later; a run's misses are measured and the send times moved by them.

- A recipe whose players are all there from the first tick to the end: one run, nothing to time.
- With a calibration in the recipe: one full run, its lives checked; a miss moves the times by what was measured and
  the run is made again (the stored calibration is replaced).
- Without one: short passes from recipe_estimate's times (each ends a second past the last life's event) until every
  life lands, the calibration stored in the .bmrp, then the full run, checked.
A SessionReady goes when its life anchors (the client logs that moment), never before its hold: the next hold is that
moment plus the ticks missed. Any other time that did not move its tick doubles its step on the next pass.
The calibration is not an input: it belongs to the build and the run's parameters; a miss makes it again.
"""

import argparse
import glob
import json
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_recipe  # noqa: E402
import journal_trace as jt  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent
TICK_MS = 1000.0 / 66.0
MAX_TRIES = 8


def targets(manifest, first):
    """join order -> [(join, start, leave or None)], absolute ticks."""
    out = {}
    for p in manifest['players']:
        out[p['join_order']] = [(first + l['join_tick'], first + l['start_tick'],
                                 first + l['leave_tick'] if l['leave_tick'] is not None else None) for l in p['lives']]
    return out


def measured(journal_path):
    """(join order -> [(join, start, leave or None)], the journal's first tick, its last tick) of a run."""
    j = jt.read_journal(journal_path)
    last = j.ticks[-1].tick if j.ticks else 0
    seen = []
    for r in list(j.initial_players) + [r for g in j.ticks for r in g.players]:
        if r['added'] and r['id'] not in [i for i, _ in seen]:
            seen.append((r['id'], r['join_order']))
    out = {}
    for pid, order in seen:
        applied = journal_recipe.expand(j, pid)
        out[order] = [(a, journal_recipe.start_of(j, pid, a, l, applied), l)
                      for a, l in journal_recipe.lives_of(j, pid, last)]
    return out, j.header['first_tick'], last


def misses(want, got, until):
    """[(order, life, what, target, measured)] for every event the run reached (<= until); a target leave of None
    (the original played to its end) takes no measured leave into account."""
    out = []
    for order, lives in sorted(want.items()):
        have = got.get(order, [])
        for k, (join, start, leave) in enumerate(lives):
            g = have[k] if k < len(have) else (None, None, None)
            for what, t, m in (('join', join, g[0]), ('start', start, g[1]), ('leave', leave, g[2])):
                if t is None or t > until:
                    continue
                if m != t:
                    out.append((order, k, what, t, m))
    return out


def anchored(label):
    """(client index, life) -> the session clock (ms of the run) at which that life anchored - its SessionReady went
    then - from the clients' logs."""
    out = {}
    for log in glob.glob(str(REPO / 'maps' / 'arena-test' / label / 'sector1' / 'client*.log')):
        i = int(re.search(r'client(\d+)\.log$', log).group(1))
        for m in re.finditer(r'--ready-at-ms: life (\d+) anchored at ([\d.]+) ms', pathlib.Path(log).read_text(errors='replace')):
            out[(i, int(m.group(1)) - 1)] = float(m.group(2))
    return out


def correct(calibration, want, got, first, memory, ready_sent):
    """The calibration moved by what the run measured; False when an event is missing (cannot be measured).

    A send time moves by the ticks it missed by. A SessionReady goes when its life anchors, never before its hold
    (`ready_sent`, from the client's log): from there on the server's base moves with it a tick per tick, so the
    next hold is that moment plus the ticks missed. Any other time that did not move its tick doubles its step on
    the next pass (`memory`: the last measurement and step)."""
    def moved(key, ms, target, measured):
        last, step = memory.get(key, (None, 1))
        step = step * 2 if last == measured else 1
        memory[key] = (measured, step)
        return int(round(ms + (target - measured) * TICK_MS * step))

    for i, entry in enumerate(calibration['players']):
        lives, have = want[i], got.get(i, [])
        for k, life in enumerate(entry['lives']):
            if k >= len(have):
                return False
            join, start, leave = lives[k]
            mj, ms, ml = have[k]
            late = k > 0 or join != first
            if k == 0 and entry['join_ms'] and mj != join:
                entry['join_ms'] = max(1, moved((i, k, 'join'), entry['join_ms'], join, mj))
            if late and life['ready_ms'] and ms != start:
                sent = max(life['ready_ms'], ready_sent.get((i, k), life['ready_ms']))
                life['ready_ms'] = max(0, int(round(sent + (start - ms) * TICK_MS)))
            if leave is not None and life['leave_tick'] is not None:
                if ml is None:
                    return False
                shift = leave - ml
                life['leave_tick'] += shift
                if life['rejoin_delay_ms'] is not None and k + 1 < len(have):
                    target, got_join = lives[k + 1][0], have[k + 1][0] + shift
                    if got_join != target:
                        life['rejoin_delay_ms'] = max(0, moved((i, k, 'rejoin'), life['rejoin_delay_ms'], target, got_join))
    return True


def run(harness, recipe, label, calibration_file=None, until=None):
    cmd = [sys.executable, '-X', 'utf8', '-I', str(REPO / 'scripts' / 'arena_map_test.py'), *harness,
           '--coordinated', '--recipe', str(recipe), '--label', label]
    if calibration_file:
        cmd += ['--recipe-calibration', str(calibration_file)]
    if until:
        cmd += ['--recipe-until-tick', str(until)]
    out = REPO / 'maps' / 'arena-test' / (label + '.harness.out')
    with open(out, 'wb') as f:
        rc = subprocess.call(cmd, cwd=str(REPO), stdout=f, stderr=subprocess.STDOUT)
    text = out.read_text(encoding='utf-8', errors='replace')
    for line in text.splitlines():
        if 'VERDICT' in line or 'coordinator: done' in line:
            print('   ' + line.strip()[:160], flush=True)
    journals = glob.glob(str(REPO / 'maps' / 'arena-test' / label / 'sector1' / 'journals' / '*.bmjs'))
    if not journals:
        raise SystemExit('%s: no server journal (harness rc %d, see %s)' % (label, rc, out))
    return journals[0], rc


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('recipe')
    ap.add_argument('--label', required=True, help='the runs: LABEL (the full one), LABEL_c1, LABEL_c2, ... (passes)')
    args, harness = ap.parse_known_args()
    recipe = pathlib.Path(args.recipe).resolve()
    manifest, stored = journal_recipe.read_recipe(recipe)
    want = targets(manifest, manifest['session_first_tick'])
    timed = any(len(l) > 1 or l[0][0] != manifest['session_first_tick'] or l[0][2] is not None for l in want.values())
    if not timed:
        print('recipe: every player from the first tick to the end - one run, nothing to time', flush=True)
        run(harness, recipe, args.label)
        return 0
    last_event = max(t for lives in want.values() for life in lives for t in life if t is not None)
    def pass_(label, calibration, until=None):
        tmp = REPO / 'maps' / 'arena-test' / (label + '.calibration.json')   # what this run is given
        tmp.write_text(json.dumps(calibration, indent=2), encoding='utf-8')
        journal, _ = run(harness, recipe, label, tmp, until)
        got, first, last = measured(journal)
        sent = anchored(label)
        if first != manifest['session_first_tick']:
            raise SystemExit('%s: the session starts at tick %d, the recipe at %d (--recipe-first-tick)'
                             % (label, first, manifest['session_first_tick']))
        wrong = misses(want, got, first + until if until else last)
        for order, k, what, t, m in wrong:
            print('   join order %d, life %d: %s at %s, the recipe %d' % (order, k + 1, what, m, t), flush=True)
        return got, wrong, sent

    memory = {}
    if stored:
        calibration, tries = stored, 0
        print('recipe: the stored calibration, one full run', flush=True)
        while True:
            label = args.label if tries == 0 else '%s_r%d' % (args.label, tries)
            got, wrong, sent = pass_(label, calibration)
            if not wrong:
                print('recipe: every life at its ticks (%s)' % label, flush=True)
                return 0
            tries += 1
            if tries >= MAX_TRIES or not correct(calibration, want, got, manifest['session_first_tick'], memory, sent):
                raise SystemExit('recipe: the lives do not land after %d runs' % tries)
            journal_recipe.write_calibration(recipe, calibration)
            print('recipe: the calibration moved by what was measured; again', flush=True)

    # No calibration yet: short passes until every life lands.
    latency = 0.0
    for i, a in enumerate(harness):
        if a == '--latency-ms' and i + 1 < len(harness):
            latency = float(harness[i + 1])
    calibration = journal_recipe.recipe_estimate(manifest, 3500, latency)
    until = last_event - manifest['session_first_tick'] + 66
    for n in range(1, MAX_TRIES + 1):
        print('recipe: calibration pass %d (to session tick %d)' % (n, until), flush=True)
        got, wrong, sent = pass_('%s_c%d' % (args.label, n), calibration, until)
        if not wrong:
            break
        if not correct(calibration, want, got, manifest['session_first_tick'], memory, sent):
            raise SystemExit('recipe: a life never came within the pass; nothing to measure')
    else:
        raise SystemExit('recipe: the lives do not land after %d passes' % MAX_TRIES)
    journal_recipe.write_calibration(recipe, calibration)
    print('recipe: calibrated (stored in %s); the full run' % recipe, flush=True)
    got, wrong, _ = pass_(args.label, calibration)
    print('recipe: %s' % ('every life at its ticks' if not wrong else 'MISSED in the full run'), flush=True)
    return 0 if not wrong else 1


if __name__ == '__main__':
    sys.exit(main())
