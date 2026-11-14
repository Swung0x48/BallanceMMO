"""How far each client's own world drifts from the server's over a session.

first_divergence.py names the first body to disagree; this one shows whether
the disagreement stays bounded or grows, which is the question a run without
snapshots asks: with no authoritative pose ever arriving, the client's world is
the inputs plus the rollbacks the relayed inputs trigger, and nothing else pulls
it back.  Per client, per window of ticks: the largest error over any body, the
body it was on, and the mean error over the client's own ball (its own input
stream, no prediction in it), over the other players' balls (predicted at the
round trip in the live record) and over the mechanisms.

usage: python scripts/server_client_drift.py <server.bmjr> <client.bmjr> [...] [--window 500]
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_trace as jt
import first_divergence as fd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('server')
    ap.add_argument('clients', nargs='+')
    ap.add_argument('--window', type=int, default=500)
    args = ap.parse_args()
    server = fd.worlds(jt.read_journal(args.server), want_local=False)
    for path in args.clients:
        journal = jt.read_journal(path)
        client = fd.worlds(journal, want_local=True)
        shared = sorted(set(server) & set(client))
        if not shared:
            print('%s: no tick in common with the server' % path)
            continue
        own = journal.header['own_player']   # the journal's own id (step JNL2: no log)
        print('%s: %d ticks in common (%d..%d)%s' % (pathlib.Path(path).name, len(shared), shared[0], shared[-1],
                                                     ', own ball %u' % own if own else ''))
        print('  %-12s %12s %-28s %12s %12s %12s' % ('ticks', 'max err (m)', 'on', 'mean own', 'mean others', 'mean mech'))
        start = shared[0] - shared[0] % args.window
        while start <= shared[-1]:
            end = start + args.window
            worst, worst_name = 0.0, '-'
            owns, others, mechs = [], [], []
            for tick in shared:
                if tick < start or tick >= end:
                    continue
                a, b = server[tick], client[tick]
                for key in sorted(set(a) & set(b)):   # sorted: a tie names the same body every run
                    err = max(abs(x - y) for x, y in zip(a[key], b[key]))
                    if key[0] == 'ball':
                        (owns if key[1] == own else others).append(err)
                    else:
                        mechs.append(err)
                    if err > worst:
                        worst, worst_name = err, fd.show(key)
            if owns or others or mechs:
                mean = lambda v: sum(v) / len(v) if v else 0.0
                print('  %-12s %12.4f %-28s %12.4f %12.4f %12.4f' % (
                    '%d-%d' % (start, end - 1), worst, worst_name, mean(owns), mean(others), mean(mechs)))
            start = end


if __name__ == '__main__':
    main()
