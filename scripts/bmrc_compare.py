#!/usr/bin/env python3
"""Compare two BMRC determinism records frame by frame.

Usage: bmrc_compare.py a.bmrc b.bmrc [--shift N]

Prints the first divergent frame, the number of matching frames, and whether
shifting one record by +-1 frame produces a better alignment (which indicates
an off-by-one anchor rather than real divergence).
"""
import struct
import sys

HEADER = struct.Struct("<4sIdi64s32s")   # 116 bytes, file has 120 with padding
HEADER_SIZE = 120
FRAME = struct.Struct("<256sQdiI")       # 280 bytes


def load(path):
    with open(path, "rb") as handle:
        data = handle.read()
    magic, version, tick_rate, level, sha, _ = HEADER.unpack_from(data, 0)
    if magic != b"BMRC" or version != 1:
        raise SystemExit("not a BMRC v1 file: " + path)
    frames = []
    offset = HEADER_SIZE
    while offset + FRAME.size <= len(data):
        keys, digest, ivp_time, cores, flags = FRAME.unpack_from(data, offset)
        frames.append((keys, digest, ivp_time, cores, flags))
        offset += FRAME.size
    return {"level": level, "sha": sha.split(b"\0", 1)[0].decode(), "frames": frames}


def compare(a, b, shift):
    matched = 0
    first = None
    count = 0
    for i in range(len(a)):
        j = i + shift
        if j < 0 or j >= len(b):
            continue
        count += 1
        if a[i][1] == b[j][1]:
            matched += 1
        elif first is None:
            first = i
    return matched, count, first


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    a = load(sys.argv[1])
    b = load(sys.argv[2])
    print("a: level={} sha={} frames={}".format(a["level"], a["sha"][:16], len(a["frames"])))
    print("b: level={} sha={} frames={}".format(b["level"], b["sha"][:16], len(b["frames"])))
    keys_equal = sum(1 for i in range(min(len(a["frames"]), len(b["frames"]))) if a["frames"][i][0] == b["frames"][i][0])
    print("identical key frames: {}/{}".format(keys_equal, min(len(a["frames"]), len(b["frames"]))))
    for shift in (0, 1, -1):
        matched, count, first = compare(a["frames"], b["frames"], shift)
        print("shift {:+d}: matched {}/{} first divergence {}".format(shift, matched, count, first))
    for i in range(0, min(len(a["frames"]), len(b["frames"])), max(1, min(len(a["frames"]), len(b["frames"])) // 12)):
        fa, fb = a["frames"][i], b["frames"][i]
        print("frame {:5d}: a={:016x} b={:016x} cores={}/{} ivp_time={:.6f}/{:.6f}".format(
            i, fa[1], fb[1], fa[3], fb[3], fa[2], fb[2]))


if __name__ == "__main__":
    main()
