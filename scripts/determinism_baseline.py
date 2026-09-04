"""Run a fixed corpus of client recordings on one headless engine build and
write a per-frame fingerprint of the engine's own state.

The fingerprints are what get compared between platforms and across engine
changes: identical fingerprints mean two builds simulate identically, and the
per-case "summary" line separately records where each build parts company with
the retail client that produced the recording.

Run it once per platform, then compare with compare_platforms.py.  Keep a copy
of the output directory before an engine change so the two can be diffed.

  python scripts/determinism_baseline.py x64 --records <dir> --game <dir>
  python scripts/determinism_baseline.py linux --records <dir> --game <dir>
  python scripts/determinism_baseline.py android --records <dir>

Recordings are not kept in the repository (they are tens of megabytes of
binary); point --records at wherever the mech_L*.bmrc sweep and the gameplay
and explosion recordings live.  Cases whose recording is missing are skipped
with a note rather than silently dropped.
"""
import argparse
import os
import re
import subprocess
import sys

# level -> [(sector, replay frame)].  The client reported the frame at which it
# activated each sector while recording; the engine runs the activation one
# frame later, which is what these numbers already are.
SECTORS = {
    1:  [(2, 2859), (3, 3137), (4, 3416)],
    2:  [(2, 2857), (3, 3136), (4, 3415), (5, 3694)],
    3:  [(2, 2853), (3, 3132), (4, 3412), (5, 3691)],
    4:  [(2, 2855), (3, 3134), (4, 3412), (5, 3691)],
    5:  [(2, 2847), (3, 3126), (4, 3405), (5, 3684)],
    6:  [(2, 2853), (3, 3132), (4, 3411), (5, 3690)],
    7:  [(2, 2858), (3, 3138), (4, 3418), (5, 3697)],
    8:  [(2, 2853), (3, 3131), (4, 3410), (5, 3689)],
    9:  [(2, 2858), (3, 3137), (4, 3416), (5, 3694)],
    10: [(2, 2852), (3, 3131), (4, 3410), (5, 3689)],
    11: [(2, 2848), (3, 3127), (4, 3406), (5, 3685), (6, 3964)],
    12: [(2, 2820), (3, 3099), (4, 3377), (5, 3656), (6, 3935), (7, 4214), (8, 4493)],
    13: [(2, 2510), (3, 2756), (4, 3000), (5, 3244)],
}

CASES = {}
for _lvl, _acts in SECTORS.items():
    _extra = []
    for _sector, _frame in _acts:
        _extra += ["--sector", str(_sector), str(_frame)]
    CASES["mech_L%02d" % _lvl] = ("mech_L%02d.bmrc" % _lvl, _extra)
CASES["gameplay_m3b"] = ("rec_m3b.bmrc", [])
CASES["explode_wood"] = ("fix_wood.bmrc", ["--explode", "wood", "3045"])
CASES["explode_stone"] = ("fix_stone.bmrc", ["--explode", "stone", "3045"])
CASES["explode_paper"] = ("fix_paper.bmrc", ["--explode", "paper", "3044"])

REPORT = re.compile(
    r"frame=(\d+) (ok|MISMATCH) expected=([0-9a-f]+) actual=([0-9a-f]+) "
    r"cores=(-?\d+)/(-?\d+) ivp_time=([-\d.e+]+)/([-\d.e+]+) seed=(-?\d+) mc=(-?\d+) "
    r"psi=([-\d.e+]+)/([-\d.e+]+) pose=(\w+)")


def fingerprint(text):
    """One line per frame: engine world hash, live cores, IVP clock, IVP seed,
    movement-check counter, next PSI time, and whether the pose matched the
    client's.  Everything here is the engine's own state, not a comparison."""
    return "".join(
        "%s %s %s %s %s %s %s %s\n" % (m.group(1), m.group(4), m.group(6), m.group(8),
                                       m.group(9), m.group(10), m.group(12), m.group(13))
        for m in REPORT.finditer(text))


def run_local(exe, game, record, extra):
    done = subprocess.run([exe, "--root", game, "--replay", record, "--report-every", "1"] + extra,
                          capture_output=True, text=True, errors="replace")
    return done.stdout + done.stderr


def run_wsl(exe, game, record, extra):
    remote_out = "/tmp/bmmo_baseline_out.txt"
    args = [exe, "--root", game, "--replay", record, "--report-every", "1"] + extra
    quoted = " ".join("'%s'" % a if " " in a else a for a in args)
    # write on the Linux side; piping large output back through wsl is unreliable
    subprocess.run(["wsl", "-e", "bash", "-c", "%s > %s 2>&1" % (quoted, remote_out)],
                   capture_output=True, text=True)
    got = subprocess.run(["wsl", "-e", "bash", "-c", "cat " + remote_out],
                         capture_output=True, text=True, errors="replace")
    return got.stdout.replace("\r", "")


def run_android(device_root, local_record, extra, scratch):
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    name = os.path.basename(local_record)
    subprocess.run(["adb", "shell", "mkdir", "-p", device_root + "/rec"], capture_output=True, env=env)
    subprocess.run(["adb", "push", local_record.replace("/", "\\"), device_root + "/rec/" + name],
                   capture_output=True, text=True, env=env)
    args = ["./BallanceMMOSimTool", "--root", ".", "--replay", "rec/" + name, "--report-every", "1"]
    remote_out = device_root + "/baseline_out.txt"
    subprocess.run(["adb", "shell", "cd %s && %s > %s 2>&1"
                    % (device_root, " ".join(args + extra), remote_out)],
                   capture_output=True, text=True, env=env)
    local = os.path.join(scratch, "android_out.txt")
    subprocess.run(["adb", "pull", remote_out, local.replace("/", "\\")], capture_output=True, env=env)
    return open(local, encoding="utf-8", errors="replace").read().replace("\r", "")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("tag", help="platform label, also the output file prefix")
    parser.add_argument("--engine", help="headless SimTool to run (a WSL path for linux)")
    parser.add_argument("--records", required=True, help="directory holding the .bmrc recordings")
    parser.add_argument("--game", help="game data root (not needed for android)")
    parser.add_argument("--out", default="baseline", help="directory to write fingerprints into")
    parser.add_argument("--device-root", default="/data/local/tmp/bmmo",
                        help="android only: where the game data and SimTool live on the device")
    parser.add_argument("--platform", choices=["local", "wsl", "android"],
                        help="how to run the engine (default: guessed from the tag)")
    parser.add_argument("cases", nargs="*", help="case names to run (default: all)")
    args = parser.parse_args()

    kind = args.platform or ("android" if "android" in args.tag
                             else "wsl" if "linux" in args.tag else "local")
    if kind != "android" and not (args.engine and args.game):
        parser.error("--engine and --game are required unless the platform is android")

    os.makedirs(args.out, exist_ok=True)
    summaries = []
    for name in (args.cases or sorted(CASES)):
        record, extra = CASES[name]
        local_record = os.path.join(args.records, record)
        if not os.path.exists(local_record):
            print("%-14s %-8s recording missing, skipped" % (name, args.tag), flush=True)
            continue
        if kind == "android":
            text = run_android(args.device_root, local_record.replace("\\", "/"), extra, args.out)
        elif kind == "wsl":
            text = run_wsl(args.engine, args.game, local_record.replace("\\", "/"), extra)
        else:
            text = run_local(args.engine, args.game, local_record, extra)

        marks = fingerprint(text)
        with open("%s/%s_%s.txt" % (args.out, args.tag, name), "w", encoding="utf-8") as out:
            out.write(marks)
        summary = next((l.strip() for l in text.splitlines() if l.startswith("summary:")), "?")
        line = "%-14s %-8s frames=%-6d %s" % (name, args.tag, marks.count("\n"), summary)
        summaries.append(line)
        print(line, flush=True)

    path = "%s/%s_summary.txt" % (args.out, args.tag)
    with open(path, "w", encoding="utf-8") as out:
        out.write("\n".join(summaries) + "\n")
    print("wrote", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
