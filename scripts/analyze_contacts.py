"""Acceptance metrics for a client journal (before/after comparison).

usage: python analyze_contacts.py <client.bmjr> [server.bmjr]
Prints: rollback stats by kind and entity; own-ball corrections (count, p50/p95/max error, max dv);
mechanism LOCAL-vs-RECEIVED disagreement at coincident ticks (per mechanism p50/p95/max);
interpenetration samples (own ball within 3.9 m of a prop stone ball in LOCAL rows, or ball-vs-sack < 3.0 m);
own-ball speed spikes (LOCAL/probe speed > 12 m/s) and unmatched windows.
"""
import sys, math, statistics
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))  # journal_trace lives next to this script
import journal_trace as jt

def pct(xs, q):
    if not xs: return 0.0
    xs = sorted(xs)
    k = min(len(xs) - 1, int(round(q * (len(xs) - 1))))
    return xs[k]

client = jt.read_journal(sys.argv[1])
own = client.header["own_player"]
names = {}
for g in client.by_tick.values():
    for rec in g.checkpoints:
        for b in rec["bodies"]:
            if b["name"]:
                names[(b["kind"], b["owner"])] = b["name"]

def nm(b):
    if b["kind"] == 0:
        return "OWN" if b["owner"] == own else "p%d" % b["owner"]
    return b["name"] or names.get((b["kind"], b["owner"]), "?%d/%d" % (b["kind"], b["owner"]))

kinds = {}
by_entity = {}
own_err, own_dv = [], []
unmatched_ticks = 0
for t, g in client.by_tick.items():
    for c in g.corrections:
        kinds[c["kind"]] = kinds.get(c["kind"], 0) + 1
        if c["kind"] == 1:
            e = c["entity"].replace("_BMMO_", "#").split("#")[0]
            by_entity[e] = by_entity.get(e, 0) + 1
            if c["entity"] and "_Peer_" not in c["entity"] and "_BMMO_" not in c["entity"]:
                own_err.append(c["error_m"]); own_dv.append(c["velocity_error"])
        if c["kind"] == 7:
            unmatched_ticks += 1
print("ticks", client.tick_record_count, "corrections by kind", kinds)
print("rollbacks by entity", dict(sorted(by_entity.items(), key=lambda kv: -kv[1])))
print("own-ball rollbacks n=%d err p50=%.3f p95=%.3f max=%.3f  dv p50=%.2f p95=%.2f max=%.2f" % (
    len(own_err), pct(own_err, .5), pct(own_err, .95), max(own_err) if own_err else 0,
    pct(own_dv, .5), pct(own_dv, .95), max(own_dv) if own_dv else 0))

# mechanism LOCAL vs RECEIVED at the same tick
received = {}
for t, g in client.by_tick.items():
    for rec in g.checkpoints:
        if rec["flags"] & 4:   # RECEIVED
            received[t] = {nm(b): b for b in rec["bodies"]}
mech_err = {}
pen_samples = []
speed_spikes = 0
for t, g in client.by_tick.items():
    for rec in g.checkpoints:
        if not (rec["flags"] & 2):   # LOCAL
            continue
        local = {nm(b): b for b in rec["bodies"]}
        # interpenetration: own ball (kind 0) vs props
        ownrow = None
        for b in rec["bodies"]:
            if b["kind"] == 0 and b["name"].startswith("Ball_") and "_Peer_" not in b["name"] and "_BMMO_" not in b["name"]:
                ownrow = b
        if ownrow:
            sp = math.sqrt(sum(v * v for v in ownrow["linear"]))
            if sp > 12.0: speed_spikes += 1
            for n, b in local.items():
                if b["kind"] != 1: continue
                d = math.sqrt(sum((x - y) ** 2 for x, y in zip(ownrow["position"], b["position"])))
                if n.startswith("P_Ball_Stone") and d < 3.9:
                    pen_samples.append((t, n, d))
                if "Sack" in n and d < 3.0:
                    pen_samples.append((t, n, d))
        srv = received.get(t)
        if not srv: continue
        for n, b in local.items():
            if b["kind"] != 1 or n not in srv: continue
            d = math.sqrt(sum((x - y) ** 2 for x, y in zip(b["position"], srv[n]["position"])))
            mech_err.setdefault(n, []).append(d)
for n, xs in sorted(mech_err.items()):
    print("mech %-24s n=%4d p50=%.4f p95=%.4f max=%.4f" % (n, len(xs), pct(xs, .5), pct(xs, .95), max(xs)))
print("interpenetration samples:", len(pen_samples), pen_samples[:8])
print("own-ball speed spikes (>12 m/s) in LOCAL rows:", speed_spikes, " unmatched records:", unmatched_ticks)
