"""Compare a client's LOCAL rows with the server's RECEIVED rows at identical ticks for one entity.

usage: python phase_check.py <client.bmjr> <entity-substring> [max_rows]
Prints per coincident tick: local pos/vel, server pos/vel, dp along v (metres and implied ms), dv, and
whether a correction happened within the previous 12 ticks.
"""
import sys, math
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))  # journal_trace lives next to this script
import journal_trace as jt

path, ent = sys.argv[1], sys.argv[2]
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 30
j = jt.read_journal(path)
names = {}
for g in j.by_tick.values():
    for rec in g.checkpoints:
        for b in rec["bodies"]:
            if b["name"]:
                names[(b["kind"], b["owner"])] = b["name"]
def nm(b):
    return b["name"] or names.get((b["kind"], b["owner"]), "")
corr_ticks = sorted(t for t, g in j.by_tick.items() for c in g.corrections if c["kind"] == 1)
shown = 0
for t in sorted(j.by_tick):
    g = j.by_tick[t]
    loc = srv = None
    for rec in g.checkpoints:
        for b in rec["bodies"]:
            if ent in nm(b):
                if rec["flags"] & 2: loc = b
                elif rec["flags"] & 4: srv = b
    if not loc or not srv:
        continue
    dp = [a - b for a, b in zip(loc["position"], srv["position"])]
    v = srv["linear"]
    speed = math.sqrt(sum(x * x for x in v))
    along = sum(a * b for a, b in zip(dp, v)) / speed if speed > 1e-6 else 0.0
    dv = math.sqrt(sum((a - b) ** 2 for a, b in zip(loc["linear"], srv["linear"])))
    recent = any(t - 12 <= c <= t for c in corr_ticks)
    print("tick %6d |v|=%.3f dp_along=%+.4f m (%+.1f ms) |dp|=%.4f dv=%.3f loc_v=(%.3f,%.3f,%.3f) srv_v=(%.3f,%.3f,%.3f)%s" % (
        t, speed, along, along / speed * 1000 if speed > 1e-6 else 0, math.sqrt(sum(x * x for x in dp)), dv,
        *loc["linear"], *srv["linear"], "  [corr<=12 ticks before]" if recent else ""))
    shown += 1
    if shown >= limit:
        break
