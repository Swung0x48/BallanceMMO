"""Compare the per-frame engine fingerprints determinism_baseline.py wrote.

Two modes, both bit-exact:

  compare_platforms.py --dir baseline x64 x86 linux android
      one build per platform, all against the first tag

  compare_platforms.py --dir baseline --against baseline_before x64
      the same platform before and after a change, to see what moved

Anything other than "identical" names the first frame that differs and prints
both sides of it.
"""
import argparse
import os
import sys


def load(directory, tag, case):
    path = "%s/%s_%s.txt" % (directory, tag, case)
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as handle:
        return handle.read().splitlines()


def compare(name, reference, other, label):
    if other is None:
        return "%s=absent" % label, False
    if other == reference:
        return "%s=identical" % label, True
    for i in range(min(len(reference), len(other))):
        if reference[i] != other[i]:
            print("    frame %d\n      %s\n      %s" % (i, reference[i], other[i]))
            return "%s=DIFFERS@%d" % (label, i), False
    return "%s=length %d vs %d" % (label, len(reference), len(other)), False


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dir", default="baseline", help="directory of fingerprints")
    parser.add_argument("--against", help="a second directory, to compare the same tags across a change")
    parser.add_argument("tags", nargs="+", help="platform tags; the first is the reference")
    args = parser.parse_args()

    reference_tag = args.tags[0]
    cases = sorted({f[len(reference_tag) + 1:-4] for f in os.listdir(args.dir)
                    if f.startswith(reference_tag + "_") and f.endswith(".txt") and "summary" not in f})
    if not cases:
        print("no fingerprints for %s in %s" % (reference_tag, args.dir))
        return 1

    mismatches = 0
    for case in cases:
        reference = load(args.dir, reference_tag, case)
        row = ["%-14s %s=%d frames" % (case, reference_tag, len(reference))]
        if args.against:
            for tag in args.tags:
                text, ok = compare(case, load(args.dir, tag, case) or [],
                                   load(args.against, tag, case), "%s(before)" % tag)
                row.append(text)
                mismatches += not ok
        else:
            for tag in args.tags[1:]:
                text, ok = compare(case, reference, load(args.dir, tag, case), tag)
                row.append(text)
                mismatches += not ok
        print("  ".join(row))

    print("\ncases: %d, mismatches: %d" % (len(cases), mismatches))
    return 0


if __name__ == "__main__":
    sys.exit(main())
