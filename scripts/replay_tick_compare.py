#!/usr/bin/env python3
"""A SimTool replay's own journal (--replay-session ... --write-journal,
server kind) against the room's server journal, tick by tick.

    replay_tick_compare.py SERVER.bmjr REPLAY.bmjr [--from T] [--to T]

SimTool never compares a client journal's TICK hashes (they are of the
client's own world), so this is how a client journal's replay through a
RESYNC record (resync design, step T1) is shown exact: the replay writes its
own journal, and its TICK records are compared with the server's.

Per common tick with a TICK record in both: hash and pose equal or not.
Prints the ranges of equal / differing ticks, the first differing tick at
or after --from, and the FULL checkpoints' bodies compared by name (balls by
owner) at the common checkpoint ticks.  Exit status 0 when every common tick
in --from..--to is identical and no FULL checkpoint there differs, else 1
(--to leaves out a client journal's tail past its last relayed frames).
"""
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_trace  # noqa: E402


def ticks_of(j):
    out = {}
    for g in j.ticks:
        if g.has_tick and g.record is not None:
            out[g.tick] = g.record
    return out


def checkpoints_of(j):
    out = {}
    for g in j.ticks:
        for cp in g.checkpoints:
            if cp["flags"] & journal_trace.CHECKPOINT_FULL and not (cp["flags"] & journal_trace.CHECKPOINT_RECEIVED):
                out[g.tick] = cp
    return out


def key(body):
    if body["kind"] == 0:
        return ("ball", body["owner"])
    return ("mech", body["name"])


def main():
    args = sys.argv[1:]
    start = 0
    if "--from" in args:
        i = args.index("--from")
        start = int(args[i + 1])
        del args[i:i + 2]
    end = None
    if "--to" in args:
        i = args.index("--to")
        end = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        return 2
    server = journal_trace.read_journal(args[0])
    replay = journal_trace.read_journal(args[1])
    st, rt = ticks_of(server), ticks_of(replay)
    common = sorted(set(st) & set(rt))
    ranges = []
    first_diff = None
    for t in common:
        same = st[t]["hash"] == rt[t]["hash"] and st[t]["pose"] == rt[t]["pose"]
        if t >= start and (end is None or t <= end) and not same and first_diff is None:
            first_diff = t
        if ranges and ranges[-1][2] == same and ranges[-1][1] == t - 1:
            ranges[-1][1] = t
        else:
            ranges.append([t, t, same])
    print("common TICK records: %d (%d..%d)" % (len(common), common[0] if common else -1, common[-1] if common else -1))
    for a, b, same in ranges:
        print("  %6d..%-6d %s (%d)" % (a, b, "SAME" if same else "DIFF", b - a + 1))
    after = [t for t in common if t >= start and (end is None or t <= end)]
    nsame = sum(1 for t in after if st[t]["hash"] == rt[t]["hash"] and st[t]["pose"] == rt[t]["pose"])
    print("from %d to %s: %d/%d ticks identical (hash and pose); first difference: %s" % (start, end, nsame, len(after), first_diff))
    sc, rc = checkpoints_of(server), checkpoints_of(replay)
    cc = [t for t in sorted(set(sc) & set(rc)) if t >= start and (end is None or t <= end)]
    worst = 0.0
    worst_at = None
    bad = 0
    for t in cc:
        a = {key(b): b for b in sc[t]["bodies"]}
        b = {key(x): x for x in rc[t]["bodies"]}
        if set(a) != set(b):
            bad += 1
            continue
        for k in a:
            d = math.sqrt(sum((a[k]["position"][i] - b[k]["position"][i]) ** 2 for i in range(3)))
            if d > worst:
                worst, worst_at = d, (t, k)
            if a[k]["position"] != b[k]["position"] or a[k]["rotation"] != b[k]["rotation"] \
                    or a[k]["linear"] != b[k]["linear"] or a[k]["angular"] != b[k]["angular"]:
                bad += 1
                break
    print("from %d: FULL checkpoints in common %d, differing %d, worst %.9f m at %s" % (start, len(cc), bad, worst, worst_at))
    return 0 if after and first_diff is None and bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
