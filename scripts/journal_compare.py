#!/usr/bin/env python3
"""Compare session journals record by record (step JNL).

    journal_compare.py A B [--mode run|replay|legacy] [--checkpoint-sets] [--quiet]

A and B are two journals, or two directories whose journals (*.bmjs, *.bmjc,
*.bmjr, at any depth) are paired by their path under each.  The records are
compared decompressed, so one differing record names itself instead of
changing every compressed byte after it:

  run     two runs of one build (or of two builds of one tree) on the
          virtual clock (coordinated): CLOCK records (raw clock reads, clock
          sites, link status) skipped, the build id's commit masked, a
          pre-step server resync note's times dropped.  Two real-clock runs
          differ in the header's utc_ms and the TICK records' ms anyway.
  replay  a capture replay against its live run: only the raw clock and link
          records skipped - a clock site's value must replay exactly.
  legacy  a journal against one written before step JNL: the SCHEMA, STAT
          and CLOCK records skipped as well, so the rest is proven unchanged.

--checkpoint-sets compares a CHECKPOINT's bodies as a set (a retail capture
replay numbers the game's objects in another order).

Prints one line per pair and a total; exit code 1 when any pair differs or
is missing, or when there is nothing to compare.  Python 3.8+,
journal_trace.py beside it.
"""
import argparse
import os
import pathlib
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import journal_trace as jt  # noqa: E402

EXTENSIONS = (jt.SERVER_EXT, jt.CLIENT_EXT, jt.LEGACY_EXT)


def journals(root):
    root = pathlib.Path(root)
    return sorted(p.relative_to(root) for p in root.rglob("*") if p.suffix in EXTENSIONS and p.is_file())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--mode", choices=jt.COMPARE_MODES, default="run")
    parser.add_argument("--checkpoint-sets", action="store_true", help="a checkpoint's bodies as a set (retail replays)")
    parser.add_argument("--quiet", action="store_true", help="print the differing pairs and the total only")
    args = parser.parse_args(argv)
    if os.path.isdir(args.a):
        pairs = [(pathlib.Path(args.a) / rel, pathlib.Path(args.b) / rel, str(rel)) for rel in journals(args.a)]
        extra = sorted(set(map(str, journals(args.b))) - set(rel for _, _, rel in pairs)) if os.path.isdir(args.b) else []
    else:
        pairs, extra = [(pathlib.Path(args.a), pathlib.Path(args.b), args.a)], []
    equal = 0
    problems = []
    for a, b, label in pairs:
        if not b.exists():
            problems.append("%s: missing in %s" % (label, args.b))
            continue
        why = jt.compare(a, b, args.mode, args.checkpoint_sets)
        if why is None:
            equal += 1
            if not args.quiet:
                print("%s: equal" % label)
        else:
            problems.append("%s: %s" % (label, why))
    problems += ["%s: only in %s" % (rel, args.b) for rel in extra]
    for line in problems:
        print(line)
    print("%s mode: %d journals, equal %d, different or missing %d" % (args.mode, len(pairs) + len(extra), equal,
                                                                       len(problems)))
    if not pairs and not extra:
        print("nothing compared: no journals under %s" % args.a)
    return 1 if problems or not (pairs or extra) else 0


if __name__ == "__main__":
    sys.exit(main())
