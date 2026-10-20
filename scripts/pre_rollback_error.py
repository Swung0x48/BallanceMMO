"""Per-body error of a client against the server BEFORE the client's first rollback.

usage: python scripts/pre_rollback_error.py <server.bmjr> <client.bmjr> [client.log]

The first rollback comes from the client journal's CORRECTION records (kind 1),
which the headless and the retail client both write; the log is only a fallback
for a journal without them.

The window a mechanism measurement is clean in: after the first rollback the
client's bodies carry the restore's own residual (docs/lockstep-without-snapshots.md),
before it they carry nothing but the two sides' simulation of the same inputs
and the same activation tick.  The server's FULL checkpoints against the
client's LOCAL ones, body by body, worst over the window; plus the tick each
side first checkpoints a mechanism (a P_ body), which is where the activation
tick shows up.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_trace as jt
import first_divergence as fd


def first_seen(world):
    """body -> the first checkpoint tick it is in (a sector's bodies appear
    when its activation physicalizes them, so this is where the activation tick
    shows up)."""
    out = {}
    for tick in sorted(world):
        for key in world[tick]:
            if key[0] == 'body':
                out.setdefault(key[1], tick)
    return out


def main():
    server = fd.worlds(jt.read_journal(sys.argv[1]), want_local=False)
    client_journal = jt.read_journal(sys.argv[2])
    client = fd.worlds(client_journal, want_local=True)
    # A rewind the engine takes for an event (a remote birth) is not a
    # CORRECTION record, so the log, when given, can only make the window
    # shorter: the earliest of both.
    rollbacks = [c['tick'] for c in client_journal.corrections() if c['kind'] == 1]
    if len(sys.argv) > 3:
        log = pathlib.Path(sys.argv[3]).read_text(encoding='utf8', errors='replace')
        rollbacks += [int(m) for m in re.findall(r'rollback to tick (\d+)', log)]
    first_rb = min(rollbacks) if rollbacks else None
    limit = first_rb if first_rb is not None else (max(client) + 1 if client else 0)
    shared = [t for t in sorted(set(server) & set(client)) if t < limit]
    print('first rollback: %s; window: ticks < %d (%d shared checkpoint ticks)' % (first_rb, limit, len(shared)))
    a, b = first_seen(server), first_seen(client)
    both = sorted(set(a) & set(b))
    late = [n for n in both if a[n] != b[n]]
    newest = max((a[n] for n in both), default=None)
    print('appearance: %d bodies on both sides, the last to appear at server tick %s; %d appear at different ticks%s'
          % (len(both), newest, len(late),
             (': ' + ', '.join('%s %d/%d' % (n, a[n], b[n]) for n in late[:8])) if late else ''))
    rows = {}
    for tick in shared:
        a, b = server[tick], client[tick]
        for key in set(a) & set(b):
            d = max(abs(a[key][k] - b[key][k]) for k in range(3))
            name = fd.show(key)
            worst, at = rows.get(name, (-1.0, None))
            if d > worst:
                rows[name] = (d, tick)
    for name, (worst, at) in sorted(rows.items(), key=lambda kv: (-kv[1][0], kv[0])):
        print('  %-28s worst %.5f m%s' % (name, worst, (' at tick %d' % at) if worst > 0 else ''))


if __name__ == '__main__':
    main()
