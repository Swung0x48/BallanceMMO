"""Compare two BMRC v2 tick records frame by frame.

    bmrc_diff.py a.bmrc b.bmrc [--show N]

Prints the first frame whose world hash differs, whether the pose hash and
collision-surface signature differ there, the probe core deltas, and a
summary count.  Exit code 0 when every frame matches, 3 otherwise.
"""
import struct
import sys

HEADER = struct.Struct("<4sIdi64s32s")           # 120 bytes incl. tail padding (dI d i ...)
FRAME = struct.Struct("<256sQdiIQ3d3f3fQ32s")     # 376 bytes


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, tick_rate, level, sha, _ = HEADER.unpack_from(data, 0)
    if magic != b"BMRC" or version != 2:
        raise SystemExit("{}: not a BMRC v2 record (version {})".format(path, version))
    frames = []
    offset = HEADER.size
    while offset + FRAME.size <= len(data):
        keys, h, t, cores, flags, surfaces, px, py, pz, vx, vy, vz, wx, wy, wz, pose, name = FRAME.unpack_from(data, offset)
        frames.append(dict(keys=keys, hash=h, ivp_time=t, cores=cores, flags=flags, surfaces=surfaces,
                           pos=(px, py, pz), speed=(vx, vy, vz), rot=(wx, wy, wz), pose=pose,
                           name=name.split(b"\0", 1)[0].decode(errors="replace")))
        offset += FRAME.size
    return level, sha.split(b"\0", 1)[0].decode(errors="replace"), frames


def main():
    show = 3
    args = []
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        if argv[i] == "--show":
            show = int(argv[i + 1])
            i += 2
            continue
        args.append(argv[i])
        i += 1
    if len(args) != 2:
        raise SystemExit(__doc__)
    la, sa, fa = load(args[0])
    lb, sb, fb = load(args[1])
    n = min(len(fa), len(fb))
    print("a: level={} frames={} physics={}".format(la, len(fa), sa[:16]))
    print("b: level={} frames={} physics={}".format(lb, len(fb), sb[:16]))
    first = -1
    matched = 0
    for i in range(n):
        if fa[i]["hash"] == fb[i]["hash"]:
            matched += 1
        elif first < 0:
            first = i
    if first < 0:
        print("identical: {} frames".format(n))
        sys.exit(0)
    print("first divergence: frame {} (matched {} of {})".format(first, matched, n))
    for i in range(first, min(n, first + show)):
        a, b = fa[i], fb[i]
        dpos = tuple(y - x for x, y in zip(a["pos"], b["pos"]))
        dspeed = tuple(y - x for x, y in zip(a["speed"], b["speed"]))
        print("frame {}: hash {:016x}/{:016x} pose {} surfaces {} cores {}/{} ivp_time {:.6f}/{:.6f} probe {}/{} dpos {} dspeed {}".format(
            i, a["hash"], b["hash"], "same" if a["pose"] == b["pose"] else "DIFF",
            "same" if a["surfaces"] == b["surfaces"] else "DIFF", a["cores"], b["cores"],
            a["ivp_time"], b["ivp_time"], a["name"], b["name"],
            tuple("{:.3g}".format(d) for d in dpos), tuple("{:.3g}".format(d) for d in dspeed)))
    sys.exit(3)


if __name__ == "__main__":
    main()
