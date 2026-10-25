#!/usr/bin/env python3
"""The sector placement's invariant check I1 (docs/sector-placement-design.md
7.3, step S3).

For every arm of a level, SimTool runs the drop scene twice with
--placement-trace: the control arm activates only the watched sector (player
1 there, the second player parked there too), the test arm lets the second
player enter the later sectors at the given world ticks (--second-move).  The
trace holds, after every world tick, one line per object hierarchy of the
watched sector's PH rows (bodies' full core state, joint handles and their
referentials, force blocks, proximity blocks, every script's activity, every
entity's world matrix; BallanceMMOServer/sim/placement_trace.hpp) and, except
where the arm's ball legitimately differs (L8-c), one line for player 1's ball
(--placement-trace-ball: the watched bodies' contact partner, 9 Q9).  An arm
PASSes iff the two traces are byte-identical and the runs did what the arm
asks: the test arm's passes reported and their sectors activated, the
control activated no other sector, every --drop-again done, no sector event
or tick failed, and the trace is not empty (otherwise the arm is an ERROR,
not a PASS).

    python scripts/placement_check.py 8            # the L8 arms, placement on
    python scripts/placement_check.py all          # every arm of the table
    python scripts/placement_check.py all --placement off   # V10: every arm must FAIL
    python scripts/placement_check.py all --repeat          # the trace reproduces (control twice)

Ticks are world ticks (the world's tick index: the drop scene's settle, 30 by
default, counts, so the ball is dropped before world tick 30).  With
--placement on the exit code is 0 iff every arm PASSes; with --placement off
(V10) it is 0 iff every arm FAILs, no earlier than its first later pass, and
on the row the arm names (`v10_row`: L8's s1 Wippe, design 8 row S3).
Traces are deleted after the comparison unless --keep.
"""

import argparse
import concurrent.futures
import os
import subprocess
import sys

ROOT = "C:/Users/geekerwan/Desktop/Ballance-Agent"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(REPO, "build-agent", "BallanceMMOServer", "BallanceMMOSimTool.exe")

# The scene of each arm (design 7.3's table).  `base` is the drop scene with
# the watched sector as --drop-sector; `moves` the test arm's later passes
# (sector, world tick); `extra` goes to both arms; `ball=False` leaves player
# 1's ball out of the trace (L8-c: the ball is on the s2 copy, which only the
# test arm places); `v10_row` is the hierarchy V10 must name.
L8_S1 = ["--level", "8", "--drop", "P_Modul_30", "1", "--drop-at", "75.202", "8.5", "0.031",
         "--drop-sector", "1", "--drop-second-sector", "1"]
L11 = ["--level", "11"]
ARMS = {
    "L8-a": dict(base=L8_S1, moves=[(2, 30), (3, 50), (4, 70)], v10_row="row=0 P_Modul_30_MF",
                 what="s1 see-saw, plank awake: a stone ball on it from the drop"),
    "L8-b": dict(base=L8_S1, moves=[(2, 150), (3, 170), (4, 190)], v10_row="row=0 P_Modul_30_MF",
                 extra=["--drop-again", "75.202", "12.5", "0.031", "300"],
                 what="s1 see-saw, plank asleep at the passes, a second drop on it at 300"),
    "L8-c": dict(base=L8_S1 + ["--drop-player-at", "402.572", "21.815", "-58.067"], moves=[(2, 0)], ball=False,
                 v10_row="row=0 P_Modul_30_MF",
                 what="s1 watched, the ball at the s2 see-saw site only (the remote wake)"),
    "L8-d": dict(base=["--level", "8", "--drop", "P_Dome", "1", "--drop-at", "637.863", "13.52", "-337.955",
                       "--drop-sector", "3", "--drop-second-sector", "3"], moves=[(4, 60)],
                 what="s3 watched, the ball on the shared dome r41/r52"),
    "L1-a": dict(base=["--level", "1", "--drop", "P_Trafo_Wood", "1", "--drop-at", "-163.793", "16.59", "-272.917",
                       "--drop-sector", "1", "--drop-second-sector", "1"], moves=[(3, 30), (4, 60)],
                 what="s1 watched, the ball near the s1 wood trafo (row 10); the r37/r38 pair"),
    "L11-a": dict(base=L11 + ["--drop", "P_Modul_25", "1", "--drop-at", "115.885", "-26.41", "-75.984",
                              "--drop-sector", "1", "--drop-second-sector", "1"], moves=[(2, 30), (3, 60), (4, 90)],
                  what="s1 watched, the ball on the s1 P_Modul_25 bridge"),
    "L11-b": dict(base=L11 + ["--drop", "P_Modul_26", "1", "--drop-at", "90.729", "-23.725", "-50.767",
                              "--drop-sector", "1", "--drop-second-sector", "1"], moves=[(3, 130)],
                  what="s1 watched, the sack swing (P_Modul_26_MF001, row 9)"),
    "L11-c": dict(base=L11 + ["--drop", "P_Modul_37", "1", "--drop-at", "429.639", "-46.793", "-83.437",
                              "--drop-sector", "2", "--drop-second-sector", "2"], moves=[(3, 60), (4, 90)],
                  what="s2 watched, the ball on the s2 P_Modul_37"),
}


def run_tool(args, name, scene, trace, ball):
    cmd = [args.tool, "--root", args.root] + scene + [
        "--ticks", str(args.ticks), "--report-every", str(args.report_every),
        "--watch-entity", "Ball_Stone_BMMO_1", "--placement", args.placement, "--placement-trace", trace]
    if ball:
        cmd.append("--placement-trace-ball")
    with open(os.path.join(args.out, name + ".txt"), "w", encoding="utf-8") as out, \
            open(os.path.join(args.out, name + ".err"), "w", encoding="utf-8") as err:
        rc = subprocess.call(cmd, stdout=out, stderr=err)
    return rc, cmd


def split_line(line):
    """'t=.. row=.. root | entity ... || block ...' -> the segments."""
    parts = []
    for chunk in line.rstrip("\n").split(" || "):
        parts.extend(chunk.split(" | "))
    return parts


def segment_name(segment):
    """An entity segment's entity, a block segment's 'script#n block', a
    script's activity segment 'script (active)'."""
    if segment.endswith((" a=0", " a=1")):
        return segment[:-4] + " (active)"
    if "#" in segment.split(":", 1)[0]:
        cut = min([segment.find(m) for m in (" h=", " exists=", " <", " B=") if segment.find(m) >= 0] or [len(segment)])
        return segment[:cut]
    return segment.split(":", 1)[0]


def first_field(a, b):
    """The first differing segment of two trace lines, and in it the first
    differing token: (segment name, token a, token b)."""
    pa, pb = split_line(a), split_line(b)
    for k in range(max(len(pa), len(pb))):
        sa = pa[k] if k < len(pa) else "<none>"
        sb = pb[k] if k < len(pb) else "<none>"
        if sa == sb:
            continue
        name = segment_name(sa if k < len(pa) else sb)
        ta, tb = sa.split(), sb.split()
        for j in range(max(len(ta), len(tb))):
            xa = ta[j] if j < len(ta) else "<none>"
            xb = tb[j] if j < len(tb) else "<none>"
            if xa != xb:
                return name, xa[:160], xb[:160]
        return name, sa[:160], sb[:160]
    return "<line>", a[:160], b[:160]


def compare(control, test):
    """None if identical, else (line number, tick, control line, test line)."""
    with open(control, "r", encoding="utf-8", errors="replace") as fa, \
            open(test, "r", encoding="utf-8", errors="replace") as fb:
        n = 0
        while True:
            a, b = fa.readline(), fb.readline()
            n += 1
            if a == b:
                if not a:
                    return None, n - 1
                continue
            line = a or b
            tick = line.split(" ", 1)[0][2:] if line.startswith("t=") else "?"
            return (n, tick, a, b), n - 1


def differing_rows(control, test):
    """Every hierarchy (row) whose lines differ: [(head, first tick, the
    segments differing in that first line)], in the order first seen."""
    rows = {}
    with open(control, "r", encoding="utf-8", errors="replace") as fa, \
            open(test, "r", encoding="utf-8", errors="replace") as fb:
        while True:
            a, b = fa.readline(), fb.readline()
            if not a and not b:
                break
            if a == b:
                continue
            line = a or b
            tick, _, rest = line.partition(" ")
            head = rest.split(" | ", 1)[0].strip()
            if head in rows:
                continue
            pa, pb = split_line(a), split_line(b)
            names = []
            for k in range(1, max(len(pa), len(pb))):
                sa = pa[k] if k < len(pa) else ""
                sb = pb[k] if k < len(pb) else ""
                if sa != sb:
                    names.append(segment_name(sa or sb))
            rows[head] = (tick[2:], names)
    return [(h, t, n) for h, (t, n) in rows.items()]


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def arg_value(scene, name):
    return int(scene[scene.index(name) + 1]) if name in scene else 0


def run_problems(args, spec, scene, tag, moves):
    """What a run did not do that the arm asks for: [] when it did."""
    problems = []
    watched = arg_value(scene, "--drop-sector")
    for side, passes in (("control", []), ("test", moves)):
        out = read_text(os.path.join(args.out, "%s_%s.txt" % (tag, side)))
        err = read_text(os.path.join(args.out, "%s_%s.err" % (tag, side)))
        for bad in ("sector event failed", "tick failed"):
            if bad in err:
                problems.append("%s: '%s' in its stderr" % (side, bad))
        activated = set()
        for line in err.splitlines():
            if line.startswith("world: sector +"):
                activated.add(int(line[len("world: sector +"):].split(" ", 1)[0]))
        for sector, tick in passes:
            if "tick=%d player 2 moved to sector %d\n" % (tick, sector) not in out:
                problems.append("%s: the pass %d@%d was not reported" % (side, sector, tick))
            if sector not in activated:
                problems.append("%s: sector %d was never activated (no 'world: sector +%d')" % (side, sector, sector))
        allowed = {watched} | {sector for sector, _ in passes}
        if activated - allowed:
            problems.append("%s: sectors %s activated, only %s expected" % (
                side, sorted(activated - allowed), sorted(allowed)))
        for k, value in enumerate(scene):
            if value == "--drop-again" and "tick=%s ball " % scene[k + 4] not in out:
                problems.append("%s: --drop-again at %s not done" % (side, scene[k + 4]))
        summary = next((l for l in out.splitlines() if l.startswith("placement trace: sector")), "")
        try:
            lines = int(summary.split(", ", 1)[1].split(" ", 1)[0])
            hierarchies = int(summary.split("last tick: ", 1)[1].split(" ", 1)[0])
        except (IndexError, ValueError):
            lines, hierarchies = 0, 0
        if lines <= 0 or hierarchies <= 0:
            problems.append("%s: empty trace (%s)" % (side, summary or "no trace summary"))
    return problems


def summary_line(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("placement trace:"):
                    return line.strip()
    except OSError:
        pass
    return "(no trace summary)"


def run_arm(args, arm):
    spec = ARMS[arm]
    scene = list(spec["base"]) + list(spec.get("extra", []))
    moves = []
    for sector, tick in spec["moves"]:
        moves += ["--second-move", str(sector), str(tick)]
    tag = "%s_%s%s" % (arm, args.placement, "_repeat" if args.repeat else "")
    control = os.path.join(args.out, tag + "_control.trace")
    test = os.path.join(args.out, tag + "_test.trace")
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        ball = spec.get("ball", True)
        fc = pool.submit(run_tool, args, tag + "_control", scene, control, ball)
        ft = pool.submit(run_tool, args, tag + "_test", scene if args.repeat else scene + moves, test, ball)
        (rc_c, cmd_c), (rc_t, cmd_t) = fc.result(), ft.result()
    first_pass = min(t for _, t in spec["moves"])
    result = dict(arm=arm, rc=(rc_c, rc_t), first_pass=first_pass,
                  summary=summary_line(os.path.join(args.out, tag + "_test.txt")),
                  cmd=" ".join(cmd_t))
    if rc_c != 0 or rc_t != 0 or not os.path.exists(control) or not os.path.exists(test):
        result["verdict"] = "ERROR"
        result["why"] = ["rc=%s/%s or a trace missing" % (rc_c, rc_t)]
        return result
    problems = run_problems(args, spec, scene, tag, [] if args.repeat else spec["moves"])
    if problems:
        result["verdict"] = "ERROR"
        result["why"] = problems
        return result
    diff, lines = compare(control, test)
    result["lines"] = lines
    if diff is None:
        result["verdict"] = "PASS"
    else:
        n, tick, a, b = diff
        result["verdict"] = "FAIL"
        result["line"] = n
        result["tick"] = tick
        head = (a or b).split(" | ", 1)[0]
        result["where"] = head
        result["field"] = first_field(a, b) if a and b else ("<length>", "", "")
        result["rows"] = differing_rows(control, test)
    if not args.keep and result["verdict"] != "ERROR":
        for path in (control, test):
            try:
                os.remove(path)
            except OSError:
                pass
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("level", help="8, 1, 11 or all")
    p.add_argument("--placement", choices=["on", "off"], default="on")
    p.add_argument("--arm", action="append", help="only these arms (L8-a, ...)")
    p.add_argument("--ticks", type=int, default=900, help="drop-scene ticks after the settle (default 900)")
    p.add_argument("--report-every", type=int, default=100)
    p.add_argument("--tool", default=TOOL)
    p.add_argument("--root", default=ROOT)
    p.add_argument("--out", default=os.path.join(REPO, ".local-tools", "work", "placement_check"))
    p.add_argument("--jobs", type=int, default=2, help="arms run at once (each runs its two arms in parallel)")
    p.add_argument("--keep", action="store_true", help="keep the traces")
    p.add_argument("--repeat", action="store_true",
                   help="the trace's own reproducibility: run every arm's control twice; PASS iff identical")
    args = p.parse_args()
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    arms = [a for a in ARMS if args.level == "all" or a.startswith("L%s-" % args.level)]
    if args.arm:
        arms = [a for a in arms if a in args.arm]
    if not arms:
        print("no arm for level %s" % args.level)
        return 2
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        results = list(pool.map(lambda a: run_arm(args, a), arms))
    expect_fail = args.placement == "off" and not args.repeat
    ok = True
    for r in results:
        moves = ",".join("%d@%d" % m for m in ARMS[r["arm"]]["moves"])
        if r["verdict"] == "ERROR":
            print("%-6s placement=%s ERROR (see %s): %s" % (r["arm"], args.placement, args.out, "; ".join(r["why"])))
            ok = False
            continue
        if r["verdict"] == "PASS":
            print("%-6s placement=%s PASS: %d trace lines identical; %s; %s" % (
                r["arm"], args.placement, r["lines"], "control twice" if args.repeat else "passes " + moves,
                r["summary"]))
            ok = ok and not expect_fail
        else:
            name, va, vb = r["field"]
            early = r["tick"].isdigit() and int(r["tick"]) < r["first_pass"]
            print("%-6s placement=%s FAIL at line %d, tick %s (first later pass %d%s): %s; first difference in %s:"
                  " control %s / test %s" % (r["arm"], args.placement, r["line"], r["tick"], r["first_pass"],
                                            ", BEFORE it" if early else "", r["where"], name, va, vb))
            for head, tick, names in r["rows"]:
                print("         %s differs from tick %s: %s" % (head, tick, ", ".join(names[:8])
                                                                + (" ... (%d)" % len(names) if len(names) > 8 else "")))
            want = ARMS[r["arm"]].get("v10_row")
            named = not expect_fail or not want or any(head == want for head, _, _ in r["rows"])
            if not named:
                print("         V10 expects %s among the differing rows: it is not" % want)
            ok = ok and expect_fail and not early and named
    if args.repeat:
        verdict = "repeat %s: every arm's control trace reproduces" % ("PASS" if ok else "FAIL")
    elif expect_fail:
        verdict = "V10 %s: every arm fails at or after its first later pass, on the row it names" % (
            "PASS" if ok else "FAIL")
    else:
        verdict = "I1 %s: every arm's traces identical" % ("PASS" if ok else "FAIL")
    print(verdict)
    return 0 if ok else 3


if __name__ == "__main__":
    sys.exit(main())
