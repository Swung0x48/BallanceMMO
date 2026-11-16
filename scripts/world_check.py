#!/usr/bin/env python3
"""The fork detection's row terms in Python (step FD1).

A mirror of BallanceMMOCommon/include/session/world_check.hpp: row_term,
fold_rows, terms_fold and position_fold, over rows in journal_trace.py's form
({"kind", "owner", "name", "position", "rotation", "linear", "angular",
"flags"}). An analyzer folds a journal's FINAL / FULL rows with it to find the
classes a report differed in, or a replay's per-tick folds to place a fork to
the tick (the fork detection design, 1.1, 6.3a).

--self-test checks the golden values the unit test WorldCheck.GoldenAndRules
holds (BallanceMMOServer/tests/world_check_test.cpp): equal values mean the
three targets and this script hash the same bytes.

usage: python -I scripts/world_check.py --self-test
"""
import struct
import sys

FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MASK = (1 << 64) - 1
FOLD = 0x9E3779B97F4A7C15     # world_hash.hpp kFoldMultiplier
BUCKETS = 8
KEEP_LOCAL = ("PE_Balloon", "PE_Box_slide")   # resync_rules.hpp keep_local_body_prefixes
FLAGS = 1 | 2                 # BODY_FLAG_SIMULATED | BODY_FLAG_COLLISION_ENABLED
BALL, MECHANISM = 0, 1


def fnv1a64(data, h=FNV_OFFSET):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK
    return h


def keep_local(name):
    return any(name.startswith(p) for p in KEEP_LOCAL)


def _key(kind, owner, name):
    if kind == BALL:
        return struct.pack("<BI", 0, owner)
    raw = name.encode("utf-8")[:0xFFFF]   # journal_trace.py decodes names as UTF-8
    return struct.pack("<BH", 1, len(raw)) + raw


def row_term(row):
    data = (_key(row["kind"], row["owner"], row.get("name", ""))
            + struct.pack("<3d", *row["position"]) + struct.pack("<4d", *row["rotation"])
            + struct.pack("<3f", *row["linear"]) + struct.pack("<3f", *row["angular"])
            + struct.pack("<B", row["flags"] & FLAGS))
    return (fnv1a64(data) * FOLD) & MASK


def bucket_of(name):
    return fnv1a64(name.encode("utf-8")) & (BUCKETS - 1)


def fold_rows(rows, latched=()):
    """{'rows', 'unkeyed', 'dup', 'balls': {owner: term}, 'buckets': [8]} by world_check.hpp's rules."""
    balls, counts, buckets = {}, {}, [0] * BUCKETS
    hashed = unkeyed = 0
    for row in rows:
        name = row.get("name", "")
        if keep_local(name):
            continue
        term = row_term(row)
        if row["kind"] == MECHANISM:
            b = bucket_of(name)
            buckets[b] = (buckets[b] + term) & MASK
            hashed += 1
            continue
        if row["owner"] == 0:
            unkeyed += 1
            continue
        if row["owner"] in latched:
            continue
        hashed += 1
        balls[row["owner"]] = (balls.get(row["owner"], 0) + term) & MASK
        counts[row["owner"]] = counts.get(row["owner"], 0) + 1
    return {"rows": min(hashed, 0xFFFF), "unkeyed": min(unkeyed, 0xFF),
            "dup": min(sum(1 for n in counts.values() if n > 1), 0xFF),
            "balls": dict(sorted(balls.items())), "buckets": buckets}


def terms_fold(t):
    data = struct.pack("<HBB", t["rows"], t["unkeyed"], t["dup"])
    for owner, term in t["balls"].items():
        data += struct.pack("<IQ", owner, term)
    data += struct.pack("<8Q", *t["buckets"])
    return fnv1a64(data)


def position_fold(rows):
    """The per-tick fold: the mechanisms half << 32 | the balls half (keyed balls only)."""
    mechanisms = balls = 0
    for row in rows:
        name = row.get("name", "")
        if keep_local(name) or (row["kind"] == BALL and row["owner"] == 0):
            continue
        term = (fnv1a64(_key(row["kind"], row["owner"], name) + struct.pack("<3d", *row["position"])) * FOLD) & MASK
        if row["kind"] == BALL:
            balls = (balls + term) & MASK
        else:
            mechanisms = (mechanisms + term) & MASK
    return (mechanisms & 0xFFFFFFFF00000000) | (balls >> 32)


def _row(kind, owner, name, x, flags=3):
    return {"kind": kind, "owner": owner, "name": name, "position": [x, -2.25, 3.0],
            "rotation": [0.0, 0.0, 0.5, 0.75], "linear": [0.5, 0.0, -1.0], "angular": [0.0, 0.25, 0.0],
            "flags": flags}


def golden_rows():
    """The unit test's row set (world_check_test.cpp golden_rows)."""
    return [_row(BALL, 7, "Ball_Wood", 1.5),
            _row(BALL, 3, "Ball_Paper_BMMO_3", 4.0),
            _row(BALL, 3, "Ball_Stone_BMMO_3", 8.0),
            _row(BALL, 9, "Ball_Wood_BMMO_9", 16.0),
            _row(BALL, 0, "Ball_Wood_BMMO_x", 32.0),
            _row(MECHANISM, 0, "P_Modul_18_MF002", 64.0, 1),
            _row(MECHANISM, 1, "A01_Floor_01", 128.0),
            _row(MECHANISM, 2, "PE_Balloon_01", 256.0)]


def golden():
    rows = golden_rows()
    t = fold_rows(rows, latched=(9,))
    return {"term_ball7": row_term(rows[0]), "term_mech": row_term(rows[5]), "rows": t["rows"],
            "unkeyed": t["unkeyed"], "dup": t["dup"], "ball3": t["balls"][3], "ball7": t["balls"][7],
            "bucket_P_Modul_18_MF002": bucket_of("P_Modul_18_MF002"), "bucket_A01_Floor_01": bucket_of("A01_Floor_01"),
            "fold": terms_fold(t), "position_fold": position_fold(rows)}


# The constants world_check_test.cpp asserts (printed by --print-golden from this mirror).
GOLDEN = {
    "term_ball7": 0x31a434fb392c477e, "term_mech": 0x7b7c2f10981cc541, "rows": 5, "unkeyed": 1, "dup": 1,
    "ball3": 0xb58918358b633052, "ball7": 0x31a434fb392c477e, "bucket_P_Modul_18_MF002": 3, "bucket_A01_Floor_01": 0,
    "fold": 0xc613d8425945a175, "position_fold": 0x1c69ed6087231e41,
}


def main(argv):
    if argv[1:] == ["--print-golden"]:
        for k, v in golden().items():
            print("%s = 0x%016x" % (k, v) if v > 0xFFFF else "%s = %d" % (k, v))
        return 0
    if argv[1:] != ["--self-test"]:
        print(__doc__)
        return 2
    got = golden()
    bad = [k for k in GOLDEN if got[k] != GOLDEN[k]]
    if bad or set(GOLDEN) != set(got):
        for k in sorted(set(GOLDEN) | set(got)):
            print("%-24s got %s want %s" % (k, got.get(k), GOLDEN.get(k)))
        print("world_check self-test FAILED")
        return 1
    print("world_check self-test PASS (%d values)" % len(GOLDEN))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
