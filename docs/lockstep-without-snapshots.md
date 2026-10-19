# Running a room without snapshots

The goal: lockstep on the relayed input stream plus local prediction, with
snapshots demoted to a fallback — join, resync, desync recovery — rather than
the thing that keeps the clients in step every other tick. This records how far
that is from working, measured rather than reasoned about, and what the one
remaining blocker is.

## The determinism half already works

`BallanceMMOSimTool --replay-session` rebuilds the world a room ran from its
journal — members, every applied input frame, every lifecycle event, no
snapshots at all — and matches each tick's fingerprint. That is lockstep,
running offline, and it has been bit-exact across Windows x86/x64, Linux and
ARM64. So "can the physics agree?" is settled; the question is only whether the
network can hand every client the same input and event stream.

## The measurement

Mechanism arena, sector 2, four headless clients driving 40 s, server in Arch
WSL, 300 ms round trip, and the snapshot path turned down as far as it goes:

```
physics:
  snapshot_interval: 600
  full_snapshot_interval: 600
```

That is genuinely off: each client received **9–11 snapshots** over ~2 100
ticks, against ~1 100 at the default interval of 2. (`full_snapshot_interval`
had to be made configurable for this — the delta interval never governed it.
The cadence still forces a full on the first tick and on every body-set change,
which is the fallback behaviour working as intended.)

The verdict, from the clients' own per-tick world hashes
(`journal_trace.py --diff`, which for two client journals is the only evidence
there is — neither is authoritative):

```
hash: matched=204/2077  first_divergence=231
```

The journals run ticks 27–2103, and 230 − 27 + 1 = **exactly 204**. So:

> **Every one of the 204 ticks before the balls are born matched bit for bit.
> Zero of the 1 873 ticks after matched.**

The mechanisms-only world is already in perfect lockstep with no snapshots at
all. Ball birth is where it breaks, and it never recovers.

## Why: the body set does not agree tick for tick

The journal's `cores=` field counts bodies. Both clients, identically:

| tick | cores | what is in the world |
|---|---|---|
| ≤ 230 | 10 | the sector's mechanisms |
| **231** | **11** | the mechanisms **and this client's own ball** |
| **252** | **14** | the other three balls appear |

21 ticks = the session's input delay (17 at this latency) plus one trip.

A client physicalizes **its own** ball locally, at the tick its own scripts
create it. It learns of the other three from relayed `Physicalize` events, which
it applies at *its current tick* — a round trip later. So for ticks 231–251
every client is simulating a world with one ball in it, and **which** ball
differs per client. The hashes cannot match, and by tick 252, when all four
balls exist everywhere, each ball has had 21 ticks of physics on its owner and
none anywhere else.

This is not a bug in anything. It is the architecture: body creation happens at
the receiver's current tick, and the receivers are at different ticks. The
snapshot-plus-rollback path is what retroactively repairs it — `rollback.hpp`
has `birth_record` and `repose_tick` for exactly the case of a body born inside
a replayed window. Which is precisely why snapshots cannot be demoted yet: they
are not a safety net here, they are the mechanism.

## Two things ruled out along the way

Worth recording, because both looked like the answer and neither was.

**Not the wire precision.** `session_event_msg` carries the birth pose as
`float[3]` position and `float[9]` rotation, while `body_state` carries every
later pose as `double[3]` + `double[4]`. A newborn ball really is the one body
whose handover is lower precision than everything after it, and it is worth
tidying — but the divergence at the birth tick measures **4.5 cm**, three orders
of magnitude too large for float32 rounding at these coordinates.

**Not the event ordering.** The server applies every due event before its
physics step, sorted by join order, and `session_runner::step` calls that
ordering "part of the determinism contract now" because two balls created in
one tick are created in that order. The headless client was applying events on
packet arrival instead — at an arbitrary point relative to its own step and in
network arrival order. That is a real latent bug and it is now fixed
(`drain_due_events`), but fixing it moved the first divergence by exactly
nothing: still 231, still `matched=204`.

## What is actually needed

Lifecycle events have to be applied at an **absolute tick every participant
agrees on**, instead of at each receiver's current tick. The shape:

* the server picks `apply_tick = max(stamp, now) + input_delay + margin` and
  relays it as a field of its own, distinct from the reported stamp (the spawn
  impulse direction derives from the stamp — `spawn_direction_index` — and both
  sides must keep deriving it from the same number);
* every participant applies the event at `apply_tick`, before that tick's step;
* **including the owner**, which means a player's own ball appears about a round
  trip after they earned it.

That last point is the real cost, and it is the same trade as the per-session
input delay: the consistency is bought with latency, and the latency is borne
where it belongs. A third of a second before your ball appears is noticeable;
whether that is acceptable is a product decision, not a technical one.

The server already has the horizon concept — `waiting_for_lifecycle` gives a
pending Physicalize until `tick + input_delay + 1` to show up — so the numbers
are not new, only their authority is.

## What was changed to get here

* The relayed input stream is now **reliable**. It was `UnreliableNoDelay`, one
  tick per message, no redundancy, so it survived zero packet loss; a snapshot
  every other tick was hiding that. A redundancy window was the wrong fix: the
  relay is already O(N²) in the room (N clients × N−1 frames × 66/s, 39 bytes a
  frame, so 83 KB/s per client at 30 players) and a window multiplies it.
* The headless client no longer drops a reordered input frame before recording
  it for the rollback, and no longer applies lifecycle events out of order.
* `physics.full_snapshot_interval` is configurable.
* The harness's latency proxy can drop datagrams (`--loss-pct`), with
  accounting — loopback does not drop, so an unreliable path passes a loss test
  by accident.

## Reproducing

```bash
# server config: snapshot_interval 600, full_snapshot_interval 600
python scripts/arena_map_test.py --bin build-agent/BallanceMMOServer \
    --root <game root> --level 13 --sectors 2 --players 4 --seconds 40 \
    --drive --beam-bays 9,9,9,9 --beam-spread 3 --latency-ms 150 \
    --checkpoint-ticks 2 --label snapoff

# the only evidence that counts: the clients' own per-tick hashes
python scripts/journal_trace.py maps/arena-test/snapoff/sector2/client0.bmjr \
                               maps/arena-test/snapoff/sector2/client1.bmjr --diff
```

## Not covered

Every run here was **single-sector**, with all four players in one bay, so the
server's sector union equalled each client's own set. A multi-sector room has a
second, independent problem: the server runs the union of the players' sectors
while each client runs only its own, so the clients are not simulating the same
world at all. That has to be settled before any of this generalises — but it is
not on the critical path, because a single-sector room already diverges at the
birth tick.
