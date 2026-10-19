# Re-simulation work at latency, and the blind-window tolerance

A client that runs ahead of the server rolls back when an authoritative
snapshot disagrees with what it recorded, and a rollback replays the whole
window of physics. At a 300 ms round trip that was costing nearly twice the
physics budget, and most of it was avoidable. This is what the cost is made
of, which part was avoidable, and what is left.

Measured on the mechanism arena (`maps/`), sector 2, four headless clients
driving for 40 s, server built and run in Arch WSL, latency simulated with the
harness's `--latency-ms` (which is **each way**, so `150` is a 300 ms round
trip — the worst the deployment expects).

## The cost has two factors, and only one of them is arguable

**Depth** is how many ticks a rollback replays: `local_tick - snapshot.tick`.
The snapshot of tick T leaves the server when it simulates T, which is
`input_delay` ticks after T's nominal time, and then takes a trip. So depth is
`input_delay + one trip`, and it is not a bug:

| round trip | session input delay | measured depth (p50 / p90 / max) |
|---|---|---|
| 0 ms | 6 ticks | 0 / 1 / 1 |
| 300 ms | 17 ticks | 20 / 21 / 22 |

Over loopback the snapshot of tick T reaches the client while it is still on
T, so the window is empty and a mismatch costs nothing to fix. At 300 ms it
is twenty ticks of physics every time.

The input delay is sized per session from the worst round trip among the
members (`bmmo::session::input_delay_for_ping`), and the server logs its
choice: `Room 1: input delay 17 ticks (258 ms) for a worst round trip of
314 ms`. It scales correctly — there is nothing to fix there, and shrinking it
would only trade re-simulation for starved inputs.

**Frequency** is how often a snapshot disagrees, and that is where the waste
was.

## What was tripping the rollbacks

`scripts/resim_report.py` splits a client journal's mismatches by subject and
by which half of the tolerance pair was breached. Four clients, 300 ms round
trip, before the change:

| subject | breach | mismatches | resim ticks | share |
|---|---|---|---|---|
| remote ball | velocity only | 242 | 4868 | **57.8%** |
| mechanism | position | 104 | 2131 | 25.3% |
| remote ball | position | 28 | 560 | 6.6% |
| own ball | position | 22 | 444 | 5.3% |
| mechanism | velocity only | 14 | 283 | 3.4% |
| own ball | velocity only | 7 | 141 | 1.7% |

The top row is the finding. **Nearly three in five mismatches were another
player's ball whose position agreed to within 5 cm and which breached on
velocity alone** — and each one replayed twenty ticks of the whole world.

That is not noise, it is the architecture telling the truth. The pair in
`rollback_thresholds` is sized, by its own comment, so that "a body 0.5 m/s off
reaches the tolerance inside 0.1 s". But a remote ball is predicted *without
the input that drove it* — that input arrives `input_delay` plus a trip later —
so at a 300 ms round trip the client is guessing for 0.3 s, three times the
horizon the pair was chosen for. Demanding 0.1 s accuracy of a 0.3 s guess
guarantees a breach, and the breach shows up in velocity long before position.

## The change

`rollback_thresholds` gained a blind window: the pair a body is judged by is
widened in proportion to how old its prediction is, in units of that same
tenth of a second (`blind_reference_ticks = 7`), clamped to `[1, 4]`.

* Never below 1, so a shallow window keeps exactly today's behaviour.
* Never above 4, so a pathological window cannot switch correction off.
* **The own ball is never widened.** Its prediction used the player's own
  exact inputs, so an error there is a real divergence rather than news that
  has not arrived yet. Remote balls and mechanisms are the blind ones.

At a 300 ms round trip the scale lands at 20/7 ≈ 2.9, so a remote ball is
judged by 0.14 m / 1.4 m/s instead of 0.05 m / 0.5 m/s.

## What it bought

Same case, before and after:

| | mismatches | resim ticks (all clients) | extra physics per tick |
|---|---|---|---|
| 0 ms, before | 148 | 12 | 0.001 |
| 0 ms, after | 148 | 32 | 0.004 |
| 300 ms, before | 417 | 8427 | 0.94 |
| 300 ms, after | 245 | 4977 | 0.55 |

**At 0 ms the mismatch decisions are identical** — the same 10/28/39/71 per
client — which is the floor at work; the resim difference is window-depth noise
between runs.

At 300 ms the velocity-only remote component is gone (242 mismatches → 1), and
the client does 55% extra physics instead of 94%. It is not the 3.5x the
breach counts alone predicted, because a drift that used to be caught early on
velocity now accumulates until it breaches the widened *position* tolerance:
remote-ball position breaches went 28 → 127. That is the correct behaviour —
the correction still happens, just once instead of repeatedly on the way.

## How it scales with the room

Per-client resim ticks over ~2250 ticks, 300 ms round trip:

| players | remotes | per-client mean, before | after |
|---|---|---|---|
| 2 | 1 | 1641 | 927 |
| 4 | 3 | 2107 | 1244 |
| 6 | 5 | — | 1593 |

A straight line through those puts the marginal cost of one more player at
**~166 resim ticks per client, down from ~233**. Extrapolated to 29 remotes
that is ~2.5 extra physics steps per tick instead of ~3.6.

Treat the extrapolation as an extrapolation: 10–30 real clients will not fit
on one test machine, and the per-client spread within a single run is wide
(the harness beams the clients in sequence, so the last one in always carries
the most).

## None of this was a correctness problem

Worth saying plainly, because the counters look alarming and the behaviour was
not. Across every case above — 0/300 ms, 2/4/6 players, sectors 1–4 — every
run had `hard=0`, `mech_hard=0`, `resyncs=0`, `starved=0`, and the server's own
journal replayed bit-for-bit (`matched=N/N first_divergence=-1`,
`checkpoint_mismatches=0`). The clients kept pace with the clock throughout.
The re-simulation was eating CPU headroom, not breaking the session.

## The knob that is left

A rollback can only happen when a snapshot arrives, so `physics.snapshot_interval`
bounds the frequency directly. With the change in, at a 300 ms round trip:

| snapshot_interval | resim ticks | worst body error seen |
|---|---|---|
| 2 | 4977 | 12.98 m |
| 4 | 3731 | 14.61 m |
| 8 | 2691 | 25.67 m |

Doubling the interval does not halve the work — the window per rollback gets
deeper and more snapshots breach — and the bodies drift further before anyone
corrects them. It is a real trade, not a free win, which is why the default
stays at 2. For a large room on weak clients it is the lever to reach for, and
it cuts the server's snapshot bandwidth by the same factor.

## Reproducing

```bash
# a 300 ms round trip (150 ms each way), four clients, one bay
python scripts/arena_map_test.py --bin build-agent/BallanceMMOServer \
    --root <game root> --level 13 --sectors 2 --players 4 --seconds 40 \
    --drive --beam-bays 9,9,9,9 --beam-spread 3 --latency-ms 150 \
    --checkpoint-ticks 2 --label lat

# where the work went
python scripts/resim_report.py "maps/arena-test/lat/sector2/client*.bmjr"
```
