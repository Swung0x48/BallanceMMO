"""Dump a tick window of a client journal: own probe, RECEIVED rows (server), LOCAL rows, corrections.

usage: python dump_window.py <client.bmjr> <server.bmjr> <from_tick> <to_tick> [name-substring ...]
Ball rows are named p<owner>; the own player's ball is named OWN.
"""
import sys, os, math
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))  # journal_trace lives next to this script
import journal_trace as jt

client_path, server_path, lo, hi = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
filters = sys.argv[5:]

client = jt.read_journal(client_path)
server = jt.read_journal(server_path)
own = client.header["own_player"]

# dictionary: owner -> name from any checkpoint that carries names
names = {}
for j in (client, server):
    for g in j.by_tick.values():
        for rec in g.checkpoints:
            for b in rec["bodies"]:
                if b["name"]:
                    names[(b["kind"], b["owner"])] = b["name"]

def nm(b):
    if b["kind"] == 0:
        return "OWN" if b["owner"] == own else "p%d" % b["owner"]
    return b["name"] or names.get((b["kind"], b["owner"]), "?%d/%d" % (b["kind"], b["owner"]))

def want(name):
    return not filters or any(f in name for f in filters)

def fmt(b):
    p = b["position"]; v = b["linear"]; w = b["angular"]
    speed = math.sqrt(sum(x * x for x in v))
    return "%-22s pos=(%9.4f,%9.4f,%9.4f) v=(%8.3f,%8.3f,%8.3f)|%6.3f w=(%6.2f,%6.2f,%6.2f)%s" % (
        nm(b), p[0], p[1], p[2], v[0], v[1], v[2], speed, w[0], w[1], w[2], "" if b["flags"] & 1 else " z")

for t in range(lo, hi + 1):
    cg = client.by_tick.get(t)
    sg = server.by_tick.get(t)
    if cg is None and sg is None:
        continue
    print("=== tick %d" % t)
    if sg is not None:
        tr = sg.record
        if tr:
            print("  S probe %-28s pos=(%9.4f,%9.4f,%9.4f) v=(%7.3f,%7.3f,%7.3f) cores=%d" % (
                tr["probe_name"], *tr["probe_position"], *tr["probe_speed"], tr["cores"]))
        for ev in sg.events:
            print("  S event p%d %s name=%s" % (ev["id"], jt.EVENT_TYPES.get(ev["type"], ev["type"]), ev.get("name", "")))
        for rec in sg.checkpoints:
            label = jt._checkpoint_flags(rec["flags"])
            for b in rec["bodies"]:
                if want(nm(b)):
                    print("  S %-14s %s" % (label, fmt(b)))
    if cg is not None:
        tr = cg.record
        if tr:
            print("  C probe %-28s pos=(%9.4f,%9.4f,%9.4f) v=(%7.3f,%7.3f,%7.3f) cores=%d" % (
                tr["probe_name"], *tr["probe_position"], *tr["probe_speed"], tr["cores"]))
        for rec in cg.checkpoints:
            label = jt._checkpoint_flags(rec["flags"])
            for b in rec["bodies"]:
                if want(nm(b)):
                    print("  C %-14s %s" % (label, fmt(b)))
        for c in cg.corrections:
            print("  C corr kind=%d local=%d entity=%s err=%.4f dv=%.3f local=(%.3f,%.3f,%.3f) server=(%.3f,%.3f,%.3f)" % (
                c["kind"], c["local_tick"], c["entity"], c["error_m"], c["velocity_error"],
                *c["local_position"], *c["server_position"]))
        for n in cg.notes:
            print("  C note %s" % n["text"])
        for ev in cg.events:
            print("  C event p%d %s name=%s" % (ev["id"], jt.EVENT_TYPES.get(ev["type"], ev["type"]), ev.get("name", "")))
