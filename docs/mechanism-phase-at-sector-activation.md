# A mechanism starts out of phase for the player who enters a sector second

> **Status (2026-10-20): fixed.** Every side now runs the same union of
> sectors from the relayed, tick-stamped Sector events, with a lead sized for
> the relay (`session/sector_union.hpp`, physics protocol 3), and nothing is
> deactivated anywhere. Four driving clients at 150 ms each way: every sector
> starts on the same tick on the server and all four clients, and the trailing
> clients' mechanism error is 0.00000 m where it was 0.6-2.7 m here. See
> docs/lockstep-without-snapshots.md, "The sector union, as built". The rest
> of this file is the finding as it was made.


Found 2026-10-19 on `maps/BMMO_Mechanism_Arena.nmo` (see
[maps/README.md](../maps/README.md)), reproducible on demand with
`scripts/arena_map_test.py`. It is a client-side artefact, not a server
correctness one: every server journal in every run below replays bit for bit,
with no hard corrections and no resyncs.

> **The numbers in this document are the second set.** The first measurement
> was taken on a version of the arena whose mechanisms interpenetrated the
> road, so they settled against a static body and slept abnormally. Everything
> derived from that — including a 3.3 m figure and an apparent before/after for
> the server-side change below — did not survive remeasurement on the corrected
> map. Section "What the fix is worth" says what that cost.

## What it looks like

Two players drive down the arena, one ahead of the other. At **every** sector
boundary, about 6 ticks after the *trailing* player crosses it, that player's
client takes one rollback on a mechanism of the sector it has just entered:

| sector | leader crosses | server activates | trailing crosses | lag | correction |
| --- | --- | --- | --- | --- | --- |
| 2 | 768 | 776 | 832 | 56 | `P_Box_MF` 2.747 m at 838 |
| 3 | 1213 | 1221 | 1326 | 105 | `P_Modul_26_Sack` 2.017 m at 1332 |
| 4 | 1657 | 1665 | 1819 | 154 | `P_Modul_08_Schaukel` 1.838 m at 1826 |

Three activations, three corrections, each one 6-7 ticks behind its activation.
`P_Box_MF` is the crate parked on the see-saw, so the first row is a loose body
that had been pushed somewhere else entirely on the server while the trailing
client still had it at rest.

## Why it happens

The two sides use different rules for which sectors run. The retail
`Gameplay_SectorManager` knows one ball, so each client runs only the sector
*its own* ball is in. The server has many balls, so
`physics_world::update_sectors` runs the **union** of its players' sectors —
"every one of them, and only those". The moment one player is a checkpoint
ahead, the server is simulating a sector the trailing client is not, and has
been for as long as the gap: 56, then 105, then 154 ticks in the run above.

When that client finally crosses, its copy of the sector's mechanisms starts
from their IC pose, while the server's have been running (and being shoved
around by the leading ball) all along. The client predicts its fresh bodies
forward, the error opens, and the rollback engine closes it in a tick.

## The evidence that it is the activation lag

A solo run, same map, same drive, same checkpoints — one player instead of two:

| run | sectors entered | worst mechanism rollback |
| --- | --- | --- |
| 2 players | 1, 2, 3, 4 | **2.747 m** |
| 1 player | 1, 2, 3, 4 | **none above 0.3 m** (whole-run max error 0.086 m) |

The solo client crosses the same three checkpoints at 817, 1311 and 1804 and
never diverges, because the only sector the server runs is the one that client
is in. Nothing else about the two runs differs.

## A server-side fix was tried, measured, and reverted

`physics_world::apply_event` marks `body_set_changed_` — which makes the next
snapshot a full one — on every event except `Sector`. For the *first* player
into a sector that is covered anyway, because `update_sectors()` changes the
union and sets the flag itself; for the second the union is unchanged,
`update_sectors()` returns early on its `desired == active_sectors_` guard, and
nothing asks for a full snapshot. That would matter if the server's copies of
that sector's mechanisms had gone to sleep, because `collect_bodies` puts only
*simulated* bodies in a delta snapshot: the trailing client would then have no
rows at all for the bodies it is getting wrong.

So the obvious one-liner is to mark the flag there too. **It has no measured
effect.** An A/B with the line present and absent, on deterministic runs with
identical crossing ticks, gives byte-identical mechanism errors:

| | `P_Box_MF` | `P_Modul_26_Sack` | `P_Modul_08_Schaukel` | `P_Modul_34_Schiebestein` |
| --- | --- | --- | --- | --- |
| with the change | 2.747408 | 2.016606 | 1.838182 | 0.648123 |
| without it | 2.747408 | 2.016606 | 1.838182 | 0.648123 |

The reason is that the sleeping regime it addresses does not arise here: the
leading ball keeps the sector's mechanisms awake, so their rows are in every
delta snapshot and a forced full snapshot adds nothing. The earlier claim that
the change took 3.329 m down to 0.046 m came from the flawed map and does not
reproduce.

**It was reverted.** A change that buys nothing measurable is not worth the
full snapshot per checkpoint crossing per player that it costs, and leaving it
in would have left the next person reading a fix where there is only a plausible
story. The `Sector` case now carries a comment saying it was tried and what the
measurement was, so the idea does not have to be rediscovered — if a future
scenario *does* put the trailing client in the sleeping regime (a longer gap,
or a mechanism the leader never disturbs), this is the line to add back, with a
measurement that shows it helping.

## A client-side seed was tried, and is unresolved

The obvious completion is to stop the trailing client predicting a mechanism it
has only just created, and seed it from the server instead. The formulation
tried was: in `rollback.hpp`, treat a **sleeping** server row as news when
*our* copy of that body is still awake, instead of ignoring it while the two
poses still agree —

```cpp
const bool parked_by_server = recorded->simulated && !ref.simulated;
ref.authoritative = ref.breached || ref.simulated || parked_by_server;
```

On the flawed map it made things worse (3.329 m where the baseline was 0.046 m)
and it fails `RollbackEngine.SleepingRowInsideToleranceLeavesTheLocalSleepStateAlone`,
which encodes the existing rule deliberately. It was reverted.

**Both of those results need redoing.** The measurement was taken on the flawed
map, so it carries no more weight than the fix measurement it sat next to; only
the unit-test failure stands on its own, and that test is a design decision, not
an accident — the comment it guards explains that copying a sleeping row re-arms
a mechanism's freeze timer.

The formulation that matches the symptom better has not been tried at all: key
it on **body birth** rather than sleep state, and hold a mechanism body created
by a sector activation out of prediction entirely until one snapshot comparison
has matched it. That is a change to the mechanism tracking the Option A
postmortem and the 9.28 joint work both warn is delicate, and it needs its own
measurement run on the corrected map.

## Confirmed with a human in the loop

2026-10-19, a real game client (BMLPlus 0.3.12, the mod built from the same
commit as the server) joined three headless bots on the arena for a 229-second
session — 14 904 ticks, 22 full checkpoints. The player was join order 3 and
crossed every checkpoint *behind* the bots, which is exactly the trailing-client
case:

| the player's crossing | correction in their own journal | offset |
| --- | --- | --- |
| sector 2 at tick 1243 | `P_Box_MF` 1.304 m at 1252 | +9 ticks |
| sector 3 at tick 2828 | none above 0.07 m | — |
| sector 4 at tick 4017 | `P_Modul_08_Schaukel` 2.087 m at 4016 | at the crossing |

Two of the three crossings produced the signature; the third did not, which
fits the mechanism — the size depends on whether that sector's mechanism is
actually mid-motion when the trailing client starts it. Of the 1619 corrections
in the player's journal, those two are the **only** ones above 0.07 m.

The session is nonetheless completely consistent: the server's journal replays
`ticks=14904 matched=14904 first_divergence=-1`, with all 22 checkpoints
reporting 0 bodies differing. So the artefact is exactly what this document
says it is — a transient the rollback engine absorbs, not a divergence.

## What is left

- Re-try the client-side seed, birth-keyed, on the corrected map.
- **Untested:** the arena keeps its mechanisms off the driving lane, so a ball
  is never standing on one when it snaps — the artefact here is visual only. On
  a map where a mechanism sits where a player stands, a trailing player could
  have it move under their ball. The harness's `--beam-bays` can put a ball on
  a mechanism at a chosen tick if that needs testing.
- A separate oddity the arena turned up and then designed away: two instances
  of one element type give two physics bodies with the same name (the game
  names a body after its element type), and a run with two `P_Ball_Wood` 320 m
  apart produced a phantom `P_Ball_Wood_MF` correction of 320.004 m — the exact
  distance between them — although `mechanism_tracking.hpp`'s identity guard is
  meant to drop a pick beyond 20 m. Retail Level 8 has the same situation with
  two `P_Modul_30_Wippe`, so this is worth a look on its own.
