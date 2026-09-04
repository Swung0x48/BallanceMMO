"""Decide whether a frame's hash mismatch is a state difference or an order one.

The world hash folds each movable core in the order CKIpionManager keeps them,
so two engines holding bit-identical bodies still disagree if the list is
ordered differently.  The recording's sidecar prints every core's state as
exact hex floats but sorted by name, which hides the order.

This rebuilds the pose hash from those exact values and searches the
permutations for the one that reproduces the hash the client recorded.  A hit
proves the states are identical and only the order differs, and names the
client's order.

usage: order_solve.py <client .bmrc.txt> <frame> <expected pose hash hex>
"""
import itertools
import re
import struct
import sys

FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MASK = (1 << 64) - 1


def feed(value, data):
    for byte in data:
        value = ((value ^ byte) * FNV_PRIME) & MASK
    return value


def parse_block(path, frame):
    """-> [(name, packed bytes for feed_core_pose)] in file order"""
    cores = []
    active = False
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("exact frame"):
                active = line.startswith("exact frame%d " % frame)
                continue
            if not active or " st=" not in line:
                continue
            name = line.split(" ", 1)[0]
            fields = dict(re.findall(r"(\w+)=([^\s]+)", line.strip()))

            def nums(key):
                return [float.fromhex(v) for v in fields[key].split(",")]

            packed = b""
            packed += struct.pack("<3d", *nums("pos"))
            packed += struct.pack("<4d", *nums("ql"))
            packed += struct.pack("<4d", *nums("qn"))
            for key in ("v", "w", "dv", "dw", "dpsi"):
                packed += struct.pack("<3f", *nums(key))
            packed += struct.pack("<B", int(fields["st"]))
            # i_delta_time is a float in the hash but printed widened to double
            packed += struct.pack("<f", struct.unpack("<f", struct.pack("<f", float.fromhex(fields["idt"])))[0])
            cores.append((name, packed))
    return cores


path, frame, expected = sys.argv[1], int(sys.argv[2]), int(sys.argv[3], 16)
cores = parse_block(path, frame)
if not cores:
    raise SystemExit("no exact dump for frame %d in %s" % (frame, path))
print("frame %d: %d cores in the recording" % (frame, len(cores)))

# hash each core once; the fold is sequential so a permutation just reorders
# which packed blob is folded next
if len(cores) > 9:
    raise SystemExit("%d cores is too many to permute exhaustively" % len(cores))

for order in itertools.permutations(range(len(cores))):
    value = FNV_OFFSET
    for index in order:
        value = feed(value, cores[index][1])
    if value == expected:
        print("MATCH: the recorded pose hash is these exact states folded in this order")
        for rank, index in enumerate(order):
            print("  %d %s" % (rank, cores[index][0]))
        break
else:
    print("no permutation of these states reproduces %016x" % expected)
    print("so the states themselves differ, not merely their order")
