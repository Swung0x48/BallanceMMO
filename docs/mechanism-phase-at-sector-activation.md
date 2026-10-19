# A mechanism starts out of phase for the player who enters a sector second

Found 2026-10-19 on `maps/BMMO_Mechanism_Arena.nmo` (see
[maps/README.md](../maps/README.md)), reproducible on demand with
`scripts/arena_map_test.py`. It is a client-side defect, not a server
correctness one: every server journal in every run below replays bit for bit.

## What it looks like

In a two-player session, about a second after the *second* player crosses a
checkpoint, that player's client takes one large rollback on a mechanism in the
sector they just entered:

```
mismatch at tick 1167 (local 1168): P_Modul_29_Platte01 dp=0.4501 local=(354.014,-1.144,30.211) server=(354.264,-0.770,30.211)z
                                    P_Modul_29_Platte02 dp=1.3741 local=(355.630,-2.117,30.211) server=(355.903,-0.770,30.211)z
                                    P_Modul_29_Platte03 dp=2.2352 local=(357.322,-2.994,30.211) server=(357.542,-0.770,30.211)z
rollback to tick 1167 (P_Modul_29_Platte05 off by 3.328804 m), re-simulated 1 ticks
```

The error grows along the plate row, and the trailing `z` says the server's
copies are asleep while the client's are still moving: this is a **phase**
difference, not a position error. The client's row is mid-sag; the server's has
already settled. Afterwards the same bodies sit at 0.014-0.046 m, so one
rollback ends it and `mech_hard` stays 0.

## Why it happens

Three things compound, and all three are needed:

1. **The two sides use different rules for which sectors run.** The retail
   `Gameplay_SectorManager` knows one ball, so each client runs only the sector
   *its own* ball is in. The server has many balls, so
   `physics_world::update_sectors` runs the **union** of its players' sectors
   ("every one of them, and only those"). The moment one player is a checkpoint
   ahead of another, the server is simulating a sector that the trailing client
   is not.

2. **The trailing client then starts that sector's mechanisms from their IC
   pose**, tens of ticks after the server started them. The journal shows every
   mechanism waking twice, once per client, at different ticks:

   | body | client1 | client0 | gap |
   | --- | --- | --- | --- |
   | `P_Modul_01_Pusher` | 759 | 780 | 21 |
   | sector 2 entered | 1059 | 1099 | 40 |
   | `P_Modul_25_Bridge` | 1171 | 1224 | 53 |
   | `P_Modul_29_Platte04` | 1200 | 1237 | 37 |
   | `P_Modul_30_Wippe` | 1340 | 1406 | 66 |

3. **Nothing corrects it for up to 98 ticks.** By the time the trailing client
   joins, the server's copies have settled and gone to sleep, and
   `physics_world::collect_bodies` puts only *simulated* bodies in a delta
   snapshot. So the trailing client has no server rows for exactly the bodies
   it is getting wrong, and keeps predicting alone until the next periodic full
   snapshot.

Point 3 splits the problem in two, and how bad it gets depends on which regime
the trailing client lands in:

- **The server's copies are already asleep** when the trailing client joins
  (its gap is longer than the mechanism's settle). The client then has *no*
  server rows at all for those bodies and runs blind until the next full
  snapshot. This is the expensive one, and it is a plain omission:
  in `physics_world::apply_event`, every other event marks
  `body_set_changed_` — which makes the next snapshot a full one — and the
  `Sector` event did not. For the *first* player to enter a sector that did
  not matter, because `update_sectors()` changes the union and sets the flag
  itself. For the second player the union is unchanged, `update_sectors()`
  returns early at its `desired == active_sectors_` guard, and nothing asks
  for a full snapshot at the one tick the trailing client most needs one.
- **The server's copies are still awake.** The trailing client does get rows
  every snapshot, but it has just created its own bodies at their IC pose and
  predicts them forward, so it is genuinely N ticks behind in the settle. The
  error grows until it crosses the rollback threshold and is then fixed in one
  tick. Nothing is missing here; the client is simply predicting a mechanism
  it has no business predicting yet.

## The evidence that it is the activation lag

Four runs of the same map, differing only in which sector the clients start in,
plus a solo control. The error tracks the gap between the two clients' sector
activations and nothing else:

| run | client0 enters sector 2 | client1 | gap | max `P_Modul_29` error |
| --- | --- | --- | --- | --- |
| `--start-sector 1` | 1099 | 1059 | 40 | 3.329 m |
| `--start-sector 3` | 989 | 965 | 24 | 2.965 m |
| `--start-sector 4` | 989 | 965 | 24 | 2.965 m |
| `--start-sector 2` | 8 | 8 | **0** | **0.093 m** |
| solo, 1 player | 1062 | — | **n/a** | **no rollback at all** |

The solo control is the decisive one: one player crossing the same checkpoint
mid-session, the same mechanism, the same map — and the client never diverges,
because the only sector the server runs is the one that client is in.

## The fix, and what it does and does not do

One line, in the `Sector` handler of `physics_world::apply_event`: mark
`body_set_changed_`, so any player's sector change forces a full snapshot on
the next send. The leading player's crossing already got one through
`update_sectors()`; the trailing player's now gets one too, carrying the
sleeping mechanism bodies it needs. The cost is one full snapshot per
checkpoint crossing per player.

The two runs below are deterministic — identical activation ticks before and
after, so this is a like-for-like comparison:

| run | gap | server activates | regime | before | after |
| --- | --- | --- | --- | --- | --- |
| `--start-sector 1` | 40 | 1067 | asleep by the time client0 joins at 1099 | 3.329 m | **0.046 m** |
| `--start-sector 3` | 24 | 973 | still awake when client0 joins at 989 | 2.965 m | 2.965 m |

That split is itself the confirmation: the change removes the asleep-body
regime completely and leaves the awake one byte-for-byte identical, which is
exactly what the diagnosis predicts. All four sectors still pass the harness
(mechanisms tracked N/N, `hard=0`, `resyncs=0`, journals bit-exact).

## What is left

**The awake regime is not fixed**, and cannot be from the server side: the
trailing client has the server's rows and is choosing to predict over them. The
fix belongs in the client's correction policy — when a sector activation
creates a mechanism body locally, that body should be *seeded* from the next
server snapshot rather than predicted forward from its IC pose, until one
comparison has matched. That is a change to the mechanism tracking the
Option A postmortem and the 9.28 joint work both warn is delicate, so it is
left as a separate piece of work rather than bolted on here. The residual cost
today is about 0.36 s of a visibly wrong mechanism for the trailing player,
ended by a single one-tick rollback, with no hard correction.

**Untested:** the arena puts `P_Modul_29` 140 units past the checkpoint, so the
ball is never standing on the mechanism when it snaps — the artefact here is
visual only. On a map where a mechanism sits at a checkpoint, a trailing player
could have it move under their ball. If that needs testing, the session
client's `--beam X Y Z TICK` can put a ball on the mechanism at a chosen tick.
