# The late player: corrections, a per-player lead, and no lifecycle barrier

Design for three changes to the physics session (protocol 7): the server
tells a late player which of its inputs it substituted, a player whose
lateness persists gets a longer lead of its own, and the one-second
Physicalize barrier goes. Written against `collision-overhaul` at 0dd381a.
Line numbers are from that revision. Revision 2 answers two reviews; §8 lists
what each finding changed. Revision 3 replaces part two's measurement (the
meter, patience and lowering: §2.1, §2.6, §2.7, and the Sector margin's
samples in §2.9) after the lp11 runs; its own line numbers are from 177c337
and are marked so. §8.2 records it.

## 0. Principles and the problem

**Principles.** A short burst or a latency spike at one player costs that
player only. The server does not change its state for the late player. It
applies what it has, and the late player's client is brought to the server's
state. The other members of the session are not affected at all. If the
lateness persists, the late client's time offset to the server is corrected:
that client runs further ahead, the server resets that player's input buffer,
and the session-wide `input_delay` and the other players' leads stay where
they are. Nothing stops the whole session for one player, so the one-second
barrier has to go. The server stays authoritative and never depends on
snapshots, and IVP is not touched.

**The problem.** A late frame never stalls the session. At `deadline(T)`
(timeline.hpp:193-195) the runner steps anyway (session_runner.cpp:612-621).
`input_buffer::take()` (timeline.hpp:130-142) then reuses the frame of the
player's last *fresh* tick, byte for byte, or a zero frame if none has arrived
yet. The frame that arrives later is dropped as stale (timeline.hpp:100-101).
It is never applied, journaled or relayed, and **the late player's own client
is never told**. The others get the substitute through the relay, so their
prediction of the late ball stays right. The late client, though, keeps the
ball it simulated with its own keys. Only a snapshot repairs that (own
thresholds 0.05 m / 0.5 m/s), and with snapshots off nothing does. The ball,
and everything it touched, diverges for good.

While a frame is missing, the session also leaves its early schedule and steps
on deadlines. At a 300 ms round trip that is `S = B + D - rtt = 28 + 16 - 20 =
24` ticks later than usual (late_tick.hpp:90-94, timeline.hpp:48-57). The
others' relay lag, already 32-44 of the 48-tick reach at 150 ms each way, then
goes past the reach.

Nothing adapts at runtime. `input_delay` is fixed at the start
(server.cpp:761, 773). A client's lead moves only by a CPU-stall rebase or a
resync, and a resync is a snapshot teleport.

The lifecycle barrier is worse (session_runner.cpp:431-471, called at 618). A
present frame flagged `INPUT_FLAG_PHYSICALIZED` with no ball and no queued
Physicalize halts **every** player for up to a second. The catch-up afterwards
runs 10 ticks per pass (session_runner.hpp:42), so the stalled ticks' relay
reaches every client about 66 ticks late, past the reach. An event that does
arrive late is applied "late by N" at the current tick (session_runner.cpp:
541-545), while every client applied it at its stamp. That is a silent
divergence.

## 1. Part one: substituted frames relayed back to their owner

### 1.1 Wire format: `session_remote_input_msg` v3, protocol 7

The datagram (session_remote_input_msg.hpp:21-28) gains three header fields
and one trailing section. The others' entries keep the v2 run format
unchanged: the others need nothing new.

| field | where | meaning |
|---|---|---|
| `acked` u32 | header, after `count` | the recipient's last tick the server applied FRESH (`s.acked`, session_runner.cpp:485) |
| `held_total` u32 | header | how many of the recipient's ticks **at or after `epoch`** the server has substituted |
| `epoch` u32 | header | the recipient's assigned base (the `first_tick` of its last SessionAssign) |
| held section | after the entries | `nheld` u8 (0..32), then one record per substituted tick of the recipient within the last `kHeldNoticeTicks = 32` ticks |

A held record is `back` u8 and `source` u32:

- `back` bits 0-4: `msg.tick - T`, 0..31. Bit 7: an inline frame follows
  instead of `source` (the v2 frame encoding, camera always present). Bits
  5-6 must be 0.
- `source`: the tick whose frame, **as the client sent it**, the server
  applied at T (§1.2), or `0xFFFFFFFF` for the default frame (nothing has
  arrived yet).
- The server writes the frame inline only when `T - source >= 96`, or when
  `source` is before `epoch` (the client clears its own input history on
  every re-anchor). *Adapted in step 3:* a reference is acted on while
  `current - (T-1) <= reach`, so the client's sent ring must reach back
  `reach + 94` ticks; `kSentInputRing` = 256 (input_relay.hpp) covers the
  deepest reach of 120 (a 128-tick ring covered a reach of 33 only).

A record is 5 B, so 32 of them and the count are 161 B. The section goes
into the first of the recipient's datagrams with room, or into one of its own
(25 + 161 B). `kRelayDatagramHeader` (input_relay.hpp:55) grows from 13 to 25.
The reader rejects `nheld > 32`, a `back` with bits 5-6 set, and a v6 peer.
`PHYSICS_PROTOCOL_VERSION` goes to 7 (entity/session.hpp:35), and
docs/rooms-and-sessions-protocol.md 2.2 gets the new rows.

**Why references and not frames.** The substitute is no longer constant
across a run (§1.2). Inline frames with a turning camera are 41 B each, so 32
of them exceed the 1100 B budget, and `build_datagrams` (input_relay.hpp:
128-132) would trim the oldest frames, which are the held ones. A reference
to a frame the client itself sent is 5 B and exact: the server applied the
bytes the client sent for `source`.

**When every frame was fresh, nothing is sent for the owner.** `nheld = 0`,
only the header. The datagram itself is now sent every tick to every member,
even when it has no entries (server.cpp:965 skips that case today). A solo
player then also gets its notices and an ack that does not depend on
snapshots.

### 1.2 Server bookkeeping

**The substitute.** `take()` returns `last_`, which only a fresh take updates
(timeline.hpp:130-142). Under persistent lateness every frame arrives stale,
so the server would drive the ball for seconds with the last frame from
before the shift, and every notice would rubber-band the player onto a ball
that ignored their keys. In v3 the substitute is the **newest arrived frame**
of the player: the frame with the highest tick at or below the cursor that
has arrived, fresh or stale. `submit()` records a stale arrival whose tick is
above the current substitute's. `take(tick, fresh, source)` returns the frame
and the substitute's tick.

The choice stays the server's. It is applied, journaled (flag 0, session_
runner.cpp:490-491) and relayed like any other frame, so the others and the
journal replay are unchanged. The corrections the late client gets shrink
from the whole lateness to the delay. During a blackout nothing arrives, and
the substitute is the last frame before it, as today.

The applied stream the others see now follows the late player's keys,
delayed. It changes as often as an on-time player's stream does, so the
others' predictions miss no more often than in the control. The criterion
"input rewinds naming ball 2 ≤ control" (§5.2) checks this.

`reset_hold(E)` (§2.3) freezes the substitute for every tick below E, so a
stale arrival inside [N, E) changes nothing there.

**The relay.** `applied` becomes a vector of `applied_input { uint32_t player;
input_frame frame; bool held; uint32_t source; }` built in `step()`
(session_runner.cpp:483-491). The `on_inputs` callback (session_runner.hpp:
93-94, registered at server.cpp:689-692) passes it through. It now fires on
every step, the solo case included (session_runner.cpp:493 requires
`size() > 1` today, and server.cpp:937 returns early for `< 2`).

`relay_ring::tick_entry` (input_relay.hpp:144-147) stores the same struct.
The ring keeps `max(window, kHeldNoticeTicks)` ticks. `trim()` uses that
depth, and the others' runs are still cut at `window`. `rebuild_runs()`
(:149-172) also collects, per player, its held ticks inside the notice depth.
`build_datagrams()` appends the recipient's held section and keeps the rest
of the recipient filter as it is.

`held_total` and `epoch` are kept per member in `physics_session_state`.
`held_total` is incremented from `applied` only for ticks at or after the
member's assigned base. A resync or late join resets the cursor to the base
(runner :342, timeline.hpp:160-166), and every tick between the current tick
and the base is then applied held. Those ticks precede the client's own
numbering and are never counted. Both fields are reset in
`send_session_assign`.

A substitution at T rides the recipient's datagrams for T .. T+31 (the notice
depth), and the others' datagrams for T .. T+window-1 (`relay_window` 8,
config_manager.cpp:78-81). The notice depth is longer than the relay window
because the late player's own downlink is the link most likely to be bad.

The server journal needs nothing new: it already records the applied frame
with flag 0. The source tick is diagnostic only and goes into the S2 lines
(§5.1).

### 1.3 Headless client (`sim/session_client.cpp`)

**Detect.** The relay loop (767-818) reads the held section after the
entries. A datagram whose `epoch` is not the client's own base contributes
only `acked`. For each record at tick T the client:

- deduplicates by tick (`held_seen_`, bounded like `relay_seen_`);
- resolves the frame. Inline and default records are taken as they are.
  Otherwise the frame comes from `sent_inputs_[source]`, a new
  `kSentInputRing`-tick (256) ring
  of the bytes each uplink datagram carried, which is never amended. It cannot
  be `own_inputs_`: a source tick can itself have been substituted, and
  `own_inputs_` is amended below. A source the ring no longer holds counts
  under `subst_late_` and leads to a resync;
- stores the frame in `own_inputs_[T]` **always, with or without a body**, so
  that a replay which re-makes the own body (§3.3) drives it with the
  server's frame, and so that `recorded_input_at` (2834-2846) agrees;
- journals `INPUT(T, own_id_, frame, JOURNAL_INPUT_SUBSTITUTED = 4)` (a new
  flag in journal.hpp:82-83 and journal_trace.py);
- counts `subst_recv_`, and adds T to `held_learned_` (a set of ticks at or
  after the base).

**Override.** A replay feeds the own ball from the **record**, not from
`own_inputs_` (rollback.hpp:1078-1096). `feed(own, false)` reads
`recorded->inputs[own_entity]` and falls back to `input_at` only when the
record has no entry. Overriding `own_inputs_` alone therefore changes nothing
for a tick that had a body. The record is read with the existing
`rollback_history::recorded_input(entity, tick, out)` (rollback.hpp:1403).
Two small calls are added:

- `std::string recorded_own_entity(uint32_t tick) const`: the record's
  `tracked.own_entity`, which a trafo may have renamed since, so it is not
  necessarily `own_nav_entity_`;
- `bool amend_recorded_input(const std::string& entity, uint32_t tick, const
  input_frame& f)`.

The client compares the held frame with the recorded one through
`same_frame()` (2433):

| result | action |
|---|---|
| no own entity at T (no ball) | count `subst_bodiless_`. The frame is in `own_inputs_` already. |
| equal | count `subst_same_`. No rewind. |
| different | amend the record and request a rewind |

**Rewind.** The client copies the gate from `note_input_correction`
(2469-2473): `current_tick() - (T-1) <= max_resim_ticks && knows_tick(T-1)`.
When the gate passes, it sets the existing `input_rewind_pending_` /
`input_rewind_tick_` (2509-2513, earliest wins) with a reason flag "own input
substituted at tick T". `replay_corrected_inputs()` (2713-2733) runs once at
the start of the next frame (1667), before `apply_held_lifecycle` (1673). So
every held tick of one datagram, and every remote correction of the same
frame, share a single replay from the earliest tick. The replay re-captures
each record with what it fed (rollback.hpp:1275, 1318-1319), so the amendment
survives later replays.

**The own feed during the replay** is the usual one. Polling is off
(rollback.hpp:1055-1063) and the own ball is driven with `nav_input` from the
amended record. Held ticks get the server's frames, and the other ticks get
the client's own fresh frames, which is what the server applied.

**Late gate.** A held tick past the reach is counted in `subst_late_`, and the
client asks for a resync. A resync costs this player only (§3.6, cost 8).

**Completeness.** The client counts the ticks of `held_learned_` at or below
`msg.tick`. Ticks announced by a SessionLead (§2.4) enter the set when
`msg.tick` passes them, so a reset range does not read as a gap. If
`held_total` exceeds that count for more than 64 ticks after the datagram
that showed it, a notice was lost past its 32-tick depth. That is counted
under `subst_late_` and also leads to a resync.

**Counters.** `subst=recv/diff/same/bodiless/rew/late` and `acked` join
`report()` (3859-3863). `acked` from the header replaces the snapshot-only
`starved/worst/lead` inputs (2093-2101), so those counters work with
snapshots off.

*Adapted in step 4:*

- A record whose tick the client has not simulated yet (it runs behind the
  server's cursor) is kept until that tick's live frame is recorded, then
  compared and replayed from T-1 like any other. Stored at arrival, the live
  frame would overwrite it, and its FRESH journal record would follow the
  SUBSTITUTED one. The forced live frame of §2.4 is step 11.
- A base record is stored and journaled too, but never amends a record or
  rewinds. After a resync's full snapshot the floor is `current + 1` (the
  base, if the snapshot lands before the first step of the new numbering),
  not the snapshot's tick. The snapshot replaced the world at the current
  tick and the history went with it, so a held tick in (snapshot, current + 1]
  would otherwise read as late and answer the resync with another one.
- `held_seen_` is cleared with every assignment, like the sent ring.
- The one replay per frame is counted in each class it carries: under
  `subst rew` and the window's `substrew=count/depth` when an own correction
  is among the frame's (earliest or not), under `inrew` when a remote's is.
  An own replay that `rewind_to` refuses asks for a resync and is counted in
  the status line's `subst_missed`, not under `inrew`'s missed. It is not
  counted late, because the record was inside the reach when it arrived.
- A pending replay does not survive a renumbering or a resync's snapshot: its
  anchor went with the history, and its refusal would answer the resync with
  another.
- A lost-notice gap is counted and credited always (later datagrams do not
  count it again), but asks for a resync only under the rule a received
  record follows: with `--rollback`, above the floor, and not while a resync
  is already waiting.
- A resync this section asks for (late, unresolved, lost, refused) is owed
  when the client's 2 s limiter holds it back, and asked again once it allows,
  unless an assignment with a newer base answers it first. While a request is
  unanswered (up to 10 s) no other goes out: a blackout can hold the answer
  past 2 s, and its full snapshot covers every reason found meanwhile.
- A datagram of another epoch still counts for `acked`/starvation (its tick
  and acked are the server's own numbers), and nothing else. Since `starved`
  and `worst` now count every substitution with snapshots off too, the
  harness's verdict reads them as a note, not a problem, on an `--event`'s
  clients.
- The server's resync log line names the newest tick the old epoch's
  `held_total` counted; the ticks after it up to the new base are applied
  held under no numbering the client has, and the harness's per-client server
  count leaves them out (`subst_unnumbered`).

*Adapted in step 4b:* the replay no longer runs on the next frame for every
own correction. Measured on the lane (lp4_a0/a1), every client stalls on the
sector activation around tick 2675, its frames reach the server late, and one
notice per server tick cost one replay of about 42 ticks per frame across the
activation, which lengthened the stall and the episode until five of seven
runs rebased into a resync. So:

- the record is amended at once as before, but a pending replay that carries
  only own corrections runs at most once per `kOwnReplayCoalesceFrames` = 6
  frames (session/own_correction.hpp `own_replay_pacer`), and always on the
  last frame its earliest tick's anchor is still inside the reach; it starts
  at the earliest amended tick, so it carries every one. A remote's
  correction is never held back and takes a waiting own one along;
- it waits only while the notices keep coming: once the newest relay header
  of our epoch says the server had our frame for the tick it simulated
  (`acked >= tick`), the waiting replay runs on that frame, so the own ball
  converges as early as before the coalescing and not at the reach's edge
  (a pure rate limit would have let the last replay of an episode run as
  late as T_off+47, on the A1 criterion's boundary);
- a snapshot rollback, lifecycle rewind or forced rewind that already
  carried a waiting own-only replay drops it (`replay_subsumes`: the rewind
  started at or before the replay's anchor and fed the amended records; a
  snapshot rollback that wrote our own ball's row from any anchor, since its
  rows are the server's world, which ran on the substituted frames). Run
  after it, the replay would step the same ticks again, and one anchored
  before a snapshot's tick would also restore our prediction over that
  snapshot's rows;
- a `diff` the replay cannot see is amended and not replayed
  (`replay_sees_difference`: the replay feeds only the leaf keys, NAV_ACTIVE
  and, while a leaf key is down, the camera); counted in `diff`;
- counted on the status line's `subst_coalesce=` and the window line's
  `substco=` as deferred frames / unseen diffs / waiting replays another
  rewind carried / amendments that needed no own replay of their own; the
  harness prints the last per phase (`co`).

### 1.4 Retail mod (`BallanceMMOClient/session`)

*Corrected at step R4 of `session-client-core-design.md` (§6.2).* The text
this section held until then ("Applied now": the held section parsed,
resolved against a sent ring, the own record amended and journaled
SUBSTITUTED; the starvation detector reading the relay header's `acked`)
described code the retail mod never had. What the retail mod does:

**Session core on (the shared core; `session core on`, R1-R6).** The retail
host (`session/core_host.cpp`) hosts the shared session-client core, which
is the headless client's implementation of §1.3 verbatim
(`BallanceMMOCommon/src/session/client_core_held.cpp`:
`note_held_section`, `settle_held_tick`, `correct_own_tick`,
`take_held_ahead`, `check_held_complete`; the replay in
`client_core_frame.cpp` `replay_corrected_inputs` with the
`own_replay_pacer`). From R4 the retail `core_config` sets
`own_corrections`: a held notice of an own tick inside the reach amends
the tick's record and the own ball is replayed from it; a notice past the
reach, a source the sent ring no longer holds, a notice every copy of which
was lost (`check_held_complete`) or an own replay `rewind_to` refuses asks
for a resync, exactly as on the headless client. The retail replay drives
the own ball from the amended record through the same navigation input as
every other retail replay (R2). The counters are the core's
(`subst=recv/diff/same/bodiless/rew/late`, `subst_skip`, `subst_missed`,
`subst_coalesce`, `acked`, `held_total` on the status line; the window
line's `subst`/`substrew`/`substco`). The host's transitional
input-starvation detector (R1-R3, over the snapshots' `acked_input_tick`)
is deleted at R4, and with it the core's `acked_input()` reader that only
it used: what it guessed at from a cursor that stopped moving, the held
notices now name tick by tick, with snapshots off too.

**Session core off (the legacy path, deleted at R7).** Unchanged and given
nothing new (the retail BAR): `physics_session_apply_queues`
(physics_session_client.cpp) parses v3, keeps the newest header of its
epoch (`acked`, `held_total`) and counts the held records (`subst=recv/
dup/stale_epoch`), and nothing else: no frame is resolved, stored, amended
or journaled SUBSTITUTED, and nothing rewinds. A legacy retail player with
snapshots off is corrected only by a resync. Its starvation detector
(`physics_session_check_input_starvation`) still reads the snapshots'
`acked_input_tick`, so with snapshots off it can judge only within 66
frames of the full snapshots a body-set change still sends.

### 1.5 What the player experiences

During a burst the late player's ball still answers their keys, because it is
predicted locally. When the notices arrive, the ball is put where the server
moved it. During a blackout the server had nothing new, so the correction is
what the held input did differently. Under lateness without loss, the server
applied the player's own keys a few ticks late, and the correction is that
delay. If the input did not change, there is no correction at all. The
others see what they see today: the late ball moves on the applied frames,
and they get them through the relay like any other frames.

## 2. Part two: a per-player lead

**Revision 3: what revision 2 measured, and why its rules were replaced.**
Revision 2's meter and patience were built in plan steps 7, 9 and 10
(0b5de5f, 53d8ecc, b134bcb) and measured in the lp11 runs: four headless
clients, 150 ms each way, lane, snapshots off, the event on client 2
(`maps/arena-test/lp11_*`). Revision 2 judged every member against a reference
frozen from its own first four windows: r01 ≈ 20.2 and r10 ≈ 21.0 ticks of
slack, measured over ticks 27-290. With both flags on:

- The control A0 substituted 11.4-15.7 % of every client's ticks; with the
  flags off (lp11_a0_off) it substituted 0-1 ticks. A0 also raised three
  healthy clients by 9-11 ticks. A0j substituted 10.6-18.3 %, A1 11.7-16.2 %.
- The cause is that the members' slack dips **together**. All four dip to
  3-11 ticks around the deaths and respawns (ticks 2302-2615 in the lane).
  Three of them dip at the key edges of client 0's recorded drive
  (arena_map_test.py:428 brakes for 25 ticks every 500, around 1169, 1670 and
  2169), where the other three replay about 20 ticks together. The dip around
  tick 2640 is not a sector activation: there is no Sector event after 1807.
- Patience at `deadline - (r01 - 1)` cut everyone in such a dip. The waiver,
  which ended only on a sample with slack ≥ r01 - 1, then did about 90 % of
  the cutting: `cut=888 (patience 33, waiver 855)` in the lp11_a0 server log.
  The meter compared each member with its own frozen past, so it read the
  shared dip as every member's own lateness.
- In A9c the WSL server's clock ran about 7-8 % fast against the Windows
  clients for about 8 s. Every member's frames fell past the deadline
  together: slack -23.4 for all four at tick 660, substituted on every tick
  from 245 to 760. The reference froze on that (r01 3.85 at 576), and all four
  were raised by +30 at tick 707.
- Under a persistent +300 ms uplink on client 2 (lp11_a5u), the meter decided
  k = 20. The Sector margin capped it at 14 (§2.9). With both flags off
  (lp11_a5u_off), the same arm resynced all four clients, because the relay
  went past the reach.

Revision 3 keeps the per-datagram sampling, the persistence rule, the gates and
the whole SessionLead path. It changes three things:

- A member's lateness is measured **against the other members at the same
  tick**, so anything they share cancels. An early member counts as on time:
  it is never a reference earlier than on time, and a raised member that is
  still late counts with its lateness, not with its raise.
- Each member keeps one frozen number, its **baseline**: where its frames
  normally fall. Members on one start base differ structurally by their round
  trips, and the baseline absorbs that.
- An **absolute rule** is kept for frames at the deadline's edge. When every
  member is late together, nothing relative can see it; when a majority shares
  it, it waits ten seconds before it raises anyone, so a fault of the server's
  own clock (lp11_a9c) raises nobody.

The per-tick numbers quoted below come from replaying the lp11 runs: arrival
times are reconstructed from the client journals (a client's TICK record for T
plus a constant 160 ms per run, calibrated against the S1 lines to ±0.1 tick
at q50). The replay is open loop, and proxy jitter and loss are not in it, so
plan steps R1-R3 (§6.1) measure the rules in the runner.

### 2.1 Measurement on the server

**Arrivals.** `handle_session_input` passes the GNS receive time
(177c337: server.cpp:1655-1664) to `submit_input` (session_runner.cpp:471-523).
The runner keeps an arrival ring per member, 256 ticks deep: {tick, `at`,
`seen`, `as_newest`}.

- `at` is the first arrival, at GNS's receive time.
- `seen` is the runner's clock when the command that carried the frame ran.
  The main loop polls every 15 ms and the runner drains commands only between
  passes (session_runner.cpp:1044-1121), so `seen` is up to about a tick
  later, and after a main-loop stall up to the 10 s age clamp later.
- `as_newest` marks a frame whose first arrival was in the datagram where it
  was the newest frame.

The ring is written in `submit_input` before `buffer.submit` (:481) for every
frame of the datagram whose slot does not hold that tick yet, stale frames
included, and never for the `kMaxLookahead` garbage (:489-490). It is created
with the input buffer (:240, :295), cleared at `player_ready` (:386-398) and
erased at `remove_player` (:316-319). For a tick T,
`slack_p(T) = (deadline(T) - at) / tick`. The meter measures on `at`.
Patience (§2.6) measures on `seen`, because the step test can only take a
frame it has seen.

**The baseline.** `A_p` is the median slack of p's first 264 newest-frame
samples after its **first** assignment (four windows of 66), floored at 3
ticks (`kBaselineFloorTicks`, revision 2's deadline reference r10). It is
frozen:

- An acknowledged lead delta does not move it. A raise restores the arrivals
  to the baseline, so moving the baseline by the delta would read the
  restored arrivals as late.
- A resync keeps it, shifted only by a lead the re-anchor did not restore
  (§2.8).

A member without a baseline is judged by nobody and is nobody's reference.

**The quantities**, at tick T, in ticks:

| name | definition | meaning |
|---|---|---|
| `n_p(T)` | `A_p - slack_p(T)` | how much later than usual p's frame came, at p's current lead |
| `r_q(T)` | `max(0, n_q(T))` | q as a reference: its lateness, with an early frame counted as on time |
| `U_p(T)` | the upper median of `r_q(T)` over the other members q that have a baseline and are not exempt at T (in a lead reset's held range, or below their cursor): the j-th smallest of `n_o`, `j = floor(n_o/2) + 1` | the others' common lateness |
| `V_p(T)` | the smallest `r_q(T)` over the same members | the least late of the others |
| `L_p(T)` | `n_p(T) - U_p(T)` | **p's lateness relative to the others** |
| `L'_p(T)` | `n_p(T) - V_p(T)` | p's lateness relative to the least late other |

**When they are computed.** In tick order, from the ring, once T has been
stepped and p's frame for T is in. That is at T's step when the frame was
already there, and at its arrival when T was stepped without it. A member q
whose frame is still missing then counts with the lower bound of its
lateness, `A_q - (deadline(T) - now)/tick`, because its frame arrives no
earlier than now. Nothing is final earlier: n normalises each arrival by its
member's baseline, so the order in n is not the order of arrival. In A0h the
250 ms member arrives last but, with a baseline about 13 ticks below the
others', can have the smallest n. Two things are excluded:

- A sample is taken only from p's `as_newest` frames. Revision 2's rule
  stands: loss is not latency, and a frame recovered from the redundancy
  window is never a sample.
- A tick at which no other member has a baseline and is not exempt gives no
  relative sample.

In tick order, lateness.hpp:215-216's "newer than the last sampled tick" test
drops nothing. Revision 3 computed L as soon as j frames were in, so the ticks
whose reference completed late were dropped, and those are exactly the ticks
where the others were late. A sample whose T lies below the meter's
`discard_below_` is still dropped.

**Why each piece.**

- **What the others share cancels.** A dip all members share (the deaths and
  respawns) moves U and V with p. So does one most of them share: in the
  brake-edge dips the three replaying members are each other's reference.
- **The clamp at 0.** An early member is never a reference earlier than on
  time. Three kinds of member arrive early against their baselines:
  - a raised member whose path recovered (`n = -k`);
  - a member whose path improved after its baseline was measured;
  - a late joiner, whose lead `D + ceil(rtt) + 2` gives slack `2D + 2` whatever
    its round trip (its baseline absorbs that, so only an improvement after
    it counts).

  Without the clamp, in a two-member session such a member would make its
  partner look late by the improvement, cut it, and raise it.
- **No offset in the reference.** Revision 3 used `g_q = n_q + o_q`, q's
  lateness with its acknowledged raises added back, as the reference. That
  is wrong after a raise:
  - A raised member's frames arrive at `n`, not at `g`. A5u's client 2 after
    a capped raise of 14 is at `n ≈ 6` and `g ≈ 20`, and A5s's after its +7
    is at `n ≈ 0` and `g ≈ 7`.
  - Every healthy member's reference moved by the raise. Its patience fell
    to the deadline, and with two or three members its own lateness went
    unseen up to `20 + M` ticks.
  - In the window between E and the acknowledgment, `o` was not counted yet
    and a slewing member read `g = -k`.

  With `r = max(0, n)`, a raised member counts with what its frames do.
- **The upper median.** U is a majority's lateness: one late member never
  moves it, and a late pair in a four-member session is still seen. With
  `n_o` = 1 or 2 it is the later of the others, so in a three-member session
  a late pair is its own reference in L. A late majority (two of three, three
  of four) reads `L ≈ 0`.
- **The least late other.** `L'` compares p with the least late other member,
  which is on time at worst. It sees any lateness that not every member
  shares, including a late majority's, but a dip shared by all but one member
  reads the same way. So its rule has a persistence of 660 ticks, not 132. In
  the lp11 replays (a0_off, and a0j, a1, a4, a9 to tick 2700), the longest
  majority of `L'` misses that any member held was 93 ticks, around the
  sector-3 crossings in a9; the brake-edge dips held 22-55.
- **The baseline.** Without it, structural differences read as lateness. A
  start member's slack is `base + D - raw rtt`, with one base for the whole
  session (late_tick.hpp:124-126, 130-134). So a 30 ms member and a 200 ms
  member differ by 11 ticks for good, and a late joiner sits 12-15 ticks
  before the start members. With L on raw arrivals, the later member would be
  cut from the session's first tick and raised after about 3 s. The lp11 arms
  are symmetric and cannot show this; A0j's clock anchoring already offsets
  two members by 2.1 ticks, and arm A0h (§5.2) covers it. The baseline does
  not bring back revision 2's failure:
  - It is a median, not a tail.
  - Start members measure theirs over the same ticks, so a dip in the
    measurement window biases all of them together and cancels in L.
  - A joiner measures alone, and a median over 264 samples absorbs any dip
    shorter than 132 ticks.

**The rules.** They share the margin `M = 4` ticks (`kQuorumMarginTicks`) with
patience (§2.6), where its sizing is given.

| quantity | value |
|---|---|
| relative miss | `L_p > M`: p came more than M ticks later than the others, against its baseline |
| minority miss | `L'_p > M` |
| absolute miss | a per-datagram newest-frame sample with slack below `θ = 3` ticks (`kAbsoluteMissTicks`). As built: `s = (deadline(t_new) - arrival) / tick`, one per datagram, newer ticks only. The threshold moves from 0 to 3. |
| raise | One of three majorities over its trailing 66 samples holds at every fresh tick for its persistence: relative, 132 ticks; minority, 660 ticks (`kMinorityPersistTicks`); absolute, 132 ticks, or 660 (`kSharedPersistTicks`) while the absolute majority is **shared**, meaning at least two counted members, and more than half of them, hold one at that tick. k: relative `max(2, ceil(q90(L)))`; minority `max(2, ceil(q90(L')))`; absolute `ceil(q50(n))` over the trailing 132 samples (660 when shared). When several hold, the largest. |
| lower | `lead_offset > 0`, the relative window full, `q95(L) < -4` (`kLowerMarginTicks`) at every fresh tick for 660 consecutive ticks, and no absolute majority. `k = -min(lead_offset, floor(-q95(L)) - 1, floor(q10(slack)) - 3)`; nothing when that is not negative. |
| cooldown, gates | Unchanged: no decision until `E + |k|·2 + 132` after an acknowledgment; samples below E discarded; one decision outstanding; at most two raises that do not restore the arrivals; the total cap latches; the Sector bound defers (§2.2, §2.9). |
| solo | no relative samples, so only the absolute rule applies, at 132 ticks |

**The raise's size.**

- **Relative.** `ceil(q90(L))` puts 90 % of p's frames back at or before the
  others', against its baseline. Client 2 in lp11_a5u reads L p50 19.73 and
  p90 19.80 after the +300 ms uplink, which gives k = 20.
- **Absolute.** `ceil(q50(n))` brings p's median arrival back to its
  baseline, which is where the session's sizing put it. The trigger and the
  target are then on one quantity: q50 of slack goes back to `A_p ≥ 3`, so
  the trigger cannot hold again. Revision 3 triggered on `q50 < 0` and aimed
  at `q10 = 3`, so a member could stay substituted on up to half its ticks
  without a raise. Its q10 target was also unstable:
  - The test was the lp11_a0_off replay with 19.8 ticks added to every
    uplink from tick 1500. There q10 over 66 samples read 1 to 12 depending
    on whether the window held a shared dip.
  - `q50(n)` over 132 or 660 samples read 19.8-20.0 for every member at any
    decision tick.
  - The same replay gives 16.7-22.5 % of every member's frames past the
    deadline before the raise.

  On lp11_a9c's fault the absolute rule gives about 45 (`22 + 23`), but the
  shared persistence takes no decision for a fault under 10 s.
- **Several rules.** A member that is both a single late member and past the
  deadline's edge gets the larger k.

**What "persistent" means.** A majority takes 34 late samples, so a raise
needs at least 131 consecutive ticks of lateness, about two seconds. A
one-second delay spike (66 samples at L ≈ 20, a majority for about 66 ticks),
a CPU stall with its ≤ 33-frame catch-up, or a blackout (no samples) can never
raise the lead. A shared dip raises nobody: L and L' stay near 0, and slack
stays at or above 3. The lane's dips bottom out at 3.0-4.7 ticks of slack. In
the replays of lp11_a0_off, a0j, a1, a4 and a9, no member's trailing-66
median of slack went below 3 even once. Two things take 660 ticks:

- **A shift shared by a majority.** lp11_a9c's fault held every member past
  the deadline from tick 245 to 760, 515 ticks. Under the shared rule it
  raises nobody. A fault longer than 10 s raises every member once, and the
  lower takes it back after the fault (`n = -k`, `L = -k`).
- **A late majority** (the minority rule). It is waited for until then
  (§2.6).

**Not adopted.**

- **The others' latest arrival as the meter's reference**: two members late
  together would mask each other for good.
- **No baseline**, the proposal: it fails on structural round-trip
  differences, as shown above.
- **The pooled inter-member spread as M**: one persistently late member widens
  it for everyone.
- **Revision 3's `g = n + o` reference**: it moves every other member's
  reference by one member's raise (above).
- **The absolute rule without the shared persistence.** Revision 3 raised
  every member by up to 14 for a host-clock fault. Before step 12, that puts
  every member's relay lag at 46-58 against a reach of 48.
### 2.2 The message: `SessionLead` (new opcode)

SessionAssign cannot carry this. A newer `first_tick` means a renumbering, a
history wipe and a snapshot resync (session_client.cpp:664-695; retail
physics_session_client.cpp:234-250). A new reliable message is needed, sent
with three unreliable copies 40 ms apart through the `assign_repeats` pattern
(server.cpp:551-552, 1157-1176):

| field | meaning |
|---|---|
| `session` u32, `player` u32 | the recipient (kept so journals can hold the bytes verbatim) |
| `seq` u16 | dedupe and ordering per player |
| `delta` i16 | ticks: positive = run further ahead |
| `held_from` u32 = N | the first tick the reset substitutes (0 = no range) |
| `effective` u32 = E | from this tick, the server expects the new schedule |
| `held` input_frame, `held_source` u32 | the substitute for [N, E) and its source tick |
| `epoch` u32 | the assignment base this refers to; a client with a newer base drops it |

The client answers with `SessionLeadAck { session, seq, applied i16 }`,
reliable. The server keeps `lead_offset[player] += applied`.

**Who gets one, and how often.**

- `SessionReady` gains `caps` u32. A client gets SessionLead only with
  `CAP_LEAD` set. Retail sets it in plan step 15, and the headless client in
  step 11.
- No new decision is taken for a player while its previous `seq` is unacked.
- A raise that is acked but does not bring the player's samples back inside
  its patience (revision 3: below the miss threshold, §2.1) within the
  cooldown allows one more raise, with k measured again. If the arrivals are still not restored after that, the server stops
  deciding for that player until its next assignment and logs it once.
- A raise is capped by the total lead cap (80 ticks, §2.5) and, until step
  14, by the Sector bound (§2.9). Revision 3 applies that bound only while
  some sector of the level is not yet in the union. A capped raise is
  applied as far as the cap allows. The server takes no further decision at
  the total cap.

So a client that cannot or will not apply the raise costs the server nothing
after one decision, and the others nothing at all.

**As built (step 10).** `seq` is u32: it is the meter's decision id, unique
per player for the session's life, so it never wraps and the ack names the
decision directly. The Sector margin is measured on the server: when it takes
another member's Sector event it relays it at once, so the event reaches p at
about p's `c_est` (§2.3), and the margin is the stamp less that; the smallest of
the last 8 binds, `k <= margin - 1`, and with nothing measured there is no
Sector cap. Only an event that reached the server on time (stamp at or after
the server's next tick) is a sample: a later one is late for every member
whatever their leads, the sender's problem, and would otherwise cap everyone
else's raises at nothing. An acked delta shifts p's samples by `-applied`
(its clock moved, the same events now land that much closer), so a second
raise is checked against the margin the first left. When the total cap binds,
the capped raise is acked like any other and then no further raise is taken
this assignment, until an acked lower takes the player below the cap (one
capped to nothing is declined and latches the same way). When the Sector
margin binds nothing latches: the margin moves with the path and the others'
crossings, so the next raise is capped anew, and one capped to nothing is
deferred, the meter taking no decision for 660 ticks. The ack's `applied` is
bounded by the delta sent (between 0 and k, on k's side), on the server and
in the meter: the offset is what a resync anchors at, and it must not be
the client's to set. The per-player path (the late join's uncapped lead,
from the current round trip and not the peak, and the resync at `L_eff`)
needs `physics.lead_adjust` AND the client's `CAP_LEAD`: a client that cannot
apply a lead cannot grow its reach either (steps 12, 15), so it keeps the
capped leads the 48-tick reach was sized for, and the flag off, or a session
of clients without `CAP_LEAD`, is the step-9 server exactly. A resync that
lands while a raise's range [N, E) is still ahead cuts the hold back to the
new base (or the first untaken tick): the client on the new base dropped the
SessionLead by its epoch, and its real frames for [base, E) must count.
Without patience, only a `reset_hold` range counts as settled in the step
test (a resync's re-anchored cursor does not), behind the same pacing floor
as §2.6. *Adapted in step 20:* the ticks a re-anchor skips (a resync's or a
late joiner's base, `input_buffer::reanchored`) count too, behind the same
floor. Waiting for their deadlines stalled the whole session: in
lp19b_a6_1500 a resync at server tick 2618 with base 2649 put every member
22 ticks (the TICK gap of 337 ms) behind for those 31 ticks, the others'
relay lag went 20 → 42 and the resynced owner's to 48-49, its reach (§2.8). (Revision 3, step R5: the Sector bound is kept per pair of members,
from every datagram and every on-time event of each sender. It binds only
senders that are on time, and only while the level has a sector nobody has
entered yet (§2.9).)

### 2.3 The server's reset of that player's input buffer

The decision is taken on the simulation thread, at a tick where the raise
rule holds:

- `N` is the session's next tick.
- **E covers the slew.** Write `c` for the client's tick when the message
  reaches it. Its frames are late by k, so `c ≈ N + rtt - k`. At 2x, the
  client gains one tick per tick period, so it needs k periods to gain k
  ticks, and it produces ticks c+1 .. c+2k on the way. Each of those frames
  is still late, by k minus the gain so far. Therefore `E = c_est + 2k + 2 ≈
  N + rtt + k + 2`, where `c_est = t_newest + (N - n_arr) + ceil(rtt_p)`,
  `t_newest` is the newest uplink tick of p received, and `n_arr` is the
  session tick at its arrival. `rtt_p` is the player's current `m_nPing`,
  posted by the main loop once a second through a new `set_player_rtt`.
  `ping_peak_ms` only ever grows and is the wrong input here.
- `inputs[p].reset_hold(E)` drops the queued frames below E, moves the cursor
  to E (timeline.hpp:160-166), and freezes the substitute for ticks below E
  to the one it holds now.

From then on every tick in [N, E) is substituted with that one frame. A
straggler below E is stale (:100-101) and no longer changes the substitute.
This lets the message carry the held frame for the whole range: it is final
at decision time.

The range [N, E) is exactly the stretch whose frames would be late anyway: the
ticks the server steps before the slewed frames start arriving. The reset
turns a ragged fresh/held mixture into one held run, which costs one rewind
instead of many.

The reset must not make the session wait. Today the early step requires
`has(tick)` for every buffer (session_runner.cpp:614-617). A reset player has
no frames for [N, E), so the session would run on deadlines for the whole
range. A new `input_buffer::settled(tick) = has(tick) || tick < next_tick_`
replaces `has` in that loop.

The journal records the range's frames with flag 0, as it does now. A note
says `lead: pP +k at N, effective E; buffer reset dropped D frames` (S3). The
reset changes nothing in the world, so `sim_tool` replays the journal
bit-exact without knowing about it.

### 2.4 The client's application

**On receipt** at current tick `c`:

1. Amend every tick of [N, E) that is ≤ `c` with `held` (§1.3), store it in
   `own_inputs_`, and request one rewind from the earliest differing tick.
2. For ticks in (c, E), set `forced_own_[t] = held`. The live frame of such a
   tick turns own polling off, drives the own ball with `nav_input(held)`
   (the replay's own mechanism, rollback.hpp:1055-1063), records and journals
   `held` as SUBSTITUTED, and sends it (the server drops it as stale). These
   are the ticks the slew produces (about 2k + 2). The player loses control
   for them, over about k + 2 tick periods of wall time, and no second
   correction follows when their notices arrive. If `c ≥ E` (the round trip
   was over-estimated), nothing is forced.
3. Slew the clock: `lead_slew_ += delta`. For a raise, each paced frame moves
   `origin_` by one tick, so the pacer runs two frames per slot. A lower
   moves it by one tick every tenth frame, so the pacer runs at 0.9x. A
   lowering is never urgent, and 0.5x would show as slow motion.
4. Ack with the applied delta.

**Why not step the forced ticks at once.** The forced ticks are the ticks the
slew produces. Stepping all of (c, E) at once would move the client 2k + 2
ticks ahead for a delta of k. It overshoots by k + 2 and would need a slow
slew afterwards.

**Nothing is skipped or renumbered.** Every tick is still stepped, recorded
(`rollback_.record`, 1950), journaled (1904) and sent in its own datagram with
the 8-frame history (1953-1960). A jump `k` larger than `MAX_INPUT_FRAMES` is
therefore harmless: no tick is ever produced without a datagram of its own.

Renumbering (`tick_base_ += k`) is not viable. It breaks design 9.25 phase
alignment (1539-1547), and it leaves a k-tick hole in `own_inputs_`, the
records and the uplink.

The pacer's rebase threshold (1573, `+ 33`) becomes `33 + |lead_slew_|`, so a
slew does not read as a stall. Event repeats are sent once per frame
(1964-1966). Their spacing compresses at 2x, which is harmless because the
reliable copy stands behind them.

**Retail.**

- `fixed_tick_driver` gets `slew(int ticks)`, applied in `on_process`
  (fixed_tick.cpp:53-92) the same way. The `SkipRenderForNextTick` catch-up
  does the 2x.
- The rebase threshold `kMaxCatchUpTicks` (fixed_tick.hpp:50) gets the same
  `+ |slew|`.
- (As built, core design step R5: the core host's detector was deleted at
  R4, so the rule below has no core-path subject; the legacy path keeps its
  132 frames and sends no `CAP_LEAD`. The slew is `fixed_tick_driver::slew`
  driven by the core's `slew_step` through `core_host_pacer`, with no field
  added to the driver: session-client-core-design.md, "As built (step R5)".)
- The starvation detector moves from 132 to 264 frames and is suppressed
  while an adjustment is outstanding, so the server's raise, which needs 132
  ticks, always lands first.

**As built (step 11, headless).** The arithmetic is in
`session/lead_slew.hpp`. The ack goes out when the slew ends, not on
receipt, so `applied` is what moved; the server's gates only count on the ack.
A raise gains a tick only while the pacer is less than one frame behind, so a
slow CPU slews more slowly instead of reading as a stall. A rebase does not end
the slew: the ticks it moved are already stepped, and it goes on moving the
new schedule. A resync request ends it and acks the whole delta first, on the
same reliable channel. The re-anchor at `L_eff` then applies all of it,
because the assignment's clock replaces the slewed one. An ack of only the
part moved would leave the client short of the lead the server decided. The
slew starts before [N, c] is amended, so a resync that an amended tick past the
reach asks for carries the decision. A SessionLead that arrives while the
client's own resync is in flight is void, because the assignment that answers
the resync clears it at the server. Its range is still applied, but there is no
slew and no ack. The forced range is capped at the 256-tick ring. The ticks past
the cap are neither forced nor learned, so their notices settle them. The
status line prints the offset as `lead_offset=`, because its `lead=` is the
measured lead. The window line prints `lead_adj= lead= slew=`.

### 2.5 Leads, caps and reach

| item | today | change |
|---|---|---|
| start base, `session_start_tick_base` (late_tick.hpp:90-94) | the capped `late_tick_lead_ticks`, clamped to `44 - input_delay` | **unchanged.** The start base and the A0 baseline do not move. |
| first late join | `late_tick_lead_ticks(D, ping_peak)`, round trip capped at 12, lead at 34 (:40-41, 50-54) | a new `late_join_lead_ticks(D, rtt_ms) = D + ceil(rtt) + 2`, capped only at a sanity bound of 80. The joiner's frames then arrive on time from its first tick. |
| resync | the same capped formula from `ping_peak` (server.cpp:1098-1101) | re-anchor at `L_eff_p` = the lead of the player's first assignment (the start base, or the late join's lead) + `lead_offset[p]`, capped at 80. That is the lead measured to work, in the same frame of reference the offset was measured in. (As built, step 10: "first", not "last" - a resync's own lead is already `L_eff`, so adding the offset to it would count the offset twice. Only for a client that declared `CAP_LEAD`; for a start member that means a resync re-anchors at the start base plus its offset, which is clamped to `44 - D`, instead of the 34-capped late-tick lead it got before.) |
| `max_resim_ticks` 48, history 96 (rollback.hpp:317-318) | one reach for everyone | per client: `max_resim = clamp(p99 relay lag over the last 4 windows + 8, 48, 120)`. It starts from `L_assigned + ceil(rtt/2) + 8`. History becomes 128 once `max_resim > 48`. The deeper re-simulation is that player's CPU. |
| pacer 33 (session_client.cpp:1573; fixed_tick.hpp:50) | rebase means resync | `+ |slew|` |
| `kRelayRing` 128, `own_inputs_` 128, `sent_inputs_` 256 (`kSentInputRing`), retail `kInputRing` | | Sufficient up to a reach of 120. Stated, not changed, except the sent ring: a held reference reaches `reach + 94` ticks back (§1.1). |

The reach is keyed on what the client measures, its relay lag (lead +
downlink), and not on `lead_offset`. Keyed on the offset, a late joiner at
400 ms (offset 0, lead about 42) would have a relay lag of about 56 against a
reach of 48 from its first tick. Every correction would be relay_late, and
its mirrors would diverge silently.

**The reach is the binding constraint, and it is the late player's.** At 150
ms each way the relay lag is already 32-44 ticks (C's runs). A raise of `k`
adds `k` to everything the late client hears. A +300 ms uplink step therefore
needs about 20 ticks of lead and puts that client's relay lag at 52-64. The
lead is raised by the full `k` as far as the Sector bound allows: control of
one's own ball comes first, and without the raise every input of the player
is substituted. The Sector bound applies only until every sector of the level
is in the union (§2.9). In the lane that is tick 1713, so A5u's 14 of 20 is
followed by the remaining 6 at the next decision. Until step 12, the full 20
puts client 2's relay lag past the 48-tick reach. Its relay_late and any
resync of it are its own cost, reported and not failed in A5u. Two things
then happen, both to the late client only:

- its reach grows with its measured lag, up to 120;
- whatever lies past even that reach is repaired the way it is today, by a
  snapshot or a resync of that player.

Whether the deeper reach fits the CPU is open question 1, measured in plan
step 12.

**As built (step 12 = roadmap step REACH, 2026-10-27).** The rule is in the
shared session-client core, so both hosts run it (core design §0: one
implementation), not in `session_client.cpp`: `rollback.hpp`
(`kReachFloorTicks` 48, `kReachCapTicks` 120 = `kHeldMaxReachTicks`,
`kReachMarginTicks` 8, `kReachHistoryTicks` 128, `reach_meter`,
`rollback_engine::set_reach`), `client_core_held.cpp` (`note_held_section`
samples), `client_core_resync.cpp` (`sample_reach`, `apply_reach`, a
`reach: N ticks at tick T (...)` line per change, 32 at most),
`client_core.cpp` (`begin`: every session starts at 48 / 96, or at the pinned
reach, set without the log line, whose tick would still be the previous
session's), the status line's `reach=now/deepest/history`; the headless client's
`--reach N` pins it (the measurement's arms; 0 = per client, the default)
and prints a `frame_time:` line (each `frame()`'s CPU time, steady clock).
Every gate that read `max_resim_ticks` follows it unchanged: the input, own
and lifecycle rewinds, a late Sector stamp, a snapshot's depth, a held
notice (`window_rewind_reach`). Adapted where the code differs from the row:
- **The sample is the header lag**, `current - msg.tick` at the first arrival
  of each server tick's datagram (the number `input: lead=` already
  averaged), not the remote frames' first-arrival lag: a notice of our tick T
  rides a datagram of server tick >= T, so this is the own notices' lag; it
  needs no remote (a solo client measures it too), and it equals the remote
  frames' lag within a tick, because the server relays a frame when it steps
  its tick.
- **No separate starting value.** The reach is re-derived at every sample
  over the current window and the three before it (a window is the
  `window:` line's 60 ticks), from the first sample on; `L_assigned +
  ceil(rtt/2) + 8` is that lag, measured. The core has neither number
  (`SessionAssign` carries `first_tick` only).
- **The history grows to 128 once the reach passes 48 and stays there for
  the session**, so a pending resync's or a rewind's anchor never leaves it
  when the reach falls back (the resync reach, `history - 1`, goes 95 ->
  127 with it). A replay at reach 120 with forced 48-tick rewinds costs the
  same as at reach 48 (below): the 32 extra records are memory only.
- **The server's Sector bound stays at 48** (`lead.hpp`
  `kLeadSectorReachTicks`, step B2): no client's reach is below it, and the
  server does not know a client's reach (no protocol change). It is now the
  floor of the reach, not `max_resim_ticks`: `lead_test` asserts
  `kLeadSectorReachTicks == kReachFloorTicks` at compile time.

Verified (quick tier): `BallanceMMOMessageTests` 542/542 (new:
`RollbackEngine.PerClientReach120RewindsDeepBitExact` - the lockstep claim at
depths 49, 80 and 120, every 7 and 20 ticks, bit-exact with reach 120 and
every one refused at 48, 121 refused at 120; `ReachMeterIsTheRelayLagP99OverFourWindows`;
`SessionClientCoreRelay.PerClientReachFollowsTheRelayLag`;
`HeldNoticeInsideThePerClientReachIsNoResync` - a notice 51 deep resyncs at
48 and is taken at a measured 60); GCC syntax 29/29; `wire_check.sh rSP11
rREACH` ALL SAME (12/12 sends, 12/12 journals IDENTICAL) although 6 of the
12 replays raised their reach to 49-50 with history 128 (resync_snapoff
c0-c3, swing_snapoff c2-c3: nothing in those captures falls in (48, 50]);
core_ref_s6/solo1 replayed IDENTICAL; one live run of RC1d's 150 ms recipe
(`resync_at150_down`, snapshots off, `maps/arena-test/reach_live2`): every
client's reach 49-51 with history 128 (relay lag p99 41-43), client 0's 63
notices all inside the reach (55 the same frame, 6 bodiless, 2 replayed at
depth 43; none past it), its one resync the recipe's `--resync-at`, the server
journal replays 3418/3418. That recipe is the resync one, not the row's;
the review's fix-up ran the stall recipe once (late-player 5.1 C7, client 0
`--stall-ms 600:450`, under the pacer's 33-frame rebase; 4 clients, 150 ms,
snapshots off, driven; `maps/arena-test/reach_stall`): the server waited
and substituted client 0's ticks 601..608 (`S2 held ... n=8`), and all 8
notices reached client 0 while it was still stalled at 600, ahead of their
ticks (`take_held_ahead`: subst=8/0/8/0/0/0, starved=8, no rewind, no
resync, nothing past the reach); the post-stall burst lifted every client's
reach to 50 for four windows (header lag p99 42, normally 22); verdict PASS,
the server journal replays 3516/3516. **A client's own CPU stall cannot put
a notice behind its tick**: the client is not stepping while the server
substitutes. A deep notice needs the server (or the downlink) to stall
while the client steps on, as in the trial; the harness has no server-stall
knob, so the (48, reach] band is shown by the unit test alone. The
fix-up's rerun: x64 BUILD-OK, x86 CLIENT-BUILD-OK, 542/542, GCC 29/29,
`wire_check.sh rSP11 rREACH2` ALL SAME.

**Open question 1, measured** (`work/REACH/cpu_reach.sh`: core_ref_s6
resync_snapoff client 0 - 150 ms, snapshots off, trafo inputs off - replayed
unpaced on the x64 headless client, a forced rewind of the reach's depth
every 30 ticks so the p99 is a rewind frame): no forced rewind p99 11.3 ms
(max 18.5); 48 deep p99 21.4 ms (max 32.0, longest rewind 25 ms); 120 deep
p99 50.9 ms (max 61.9, longest rewind 58 ms); reach 120 with 48-deep
rewinds 21.6 ms. A replayed tick costs about 0.45 ms here (rewind-cost
design §1: 0.41-0.47 ms, 1.23-1.27 ms with the trafo inputs on, which would
make a 120-deep rewind about 150 ms). One 120-deep rewind is a hitch of about
four frames (ten with the trafo inputs), under the pacer's 33-frame rebase,
but that a live client absorbs it was not shown (no live run reached the
band); a server stall's burst brings deep notices over several frames, and
`replay_corrected_inputs` rewinds once per frame, so it is several frames
over budget in a row. A rewind per frame deeper than about 33 ticks is past
the 15.15 ms budget at any reach (48 deep is already 21 ms) - the CPU cliff
of the rewind-cost track, which a deeper reach extends to the corrections it
no longer refuses. Not measured: the retail x86 client's cost, the trafo-on
cost (estimated from §1 of the rewind-cost design), and the x86 memory of
history 128 with savestate records (32 more blobs in a 32-bit process).

**Not settled: the cap is the user's choice** (open question 1, the user's
rule of 2026-10-27). 120 is past one frame's budget by about 4x (x64, trafo
off), about 10x estimated with the trafo inputs. (i) **Cap 120 as designed**
(as built): every notice up to 120 deep is a rewind, a deep one a 58 ms
(x64) to ~150 ms (trafo, estimated) hitch, several in a row after a server
stall. (ii) **A lower cap, e.g. 72**: about 36 ms per rewind on x64
(interpolated: 25 ms at 48, 58 ms at 120, 0.46 ms per tick + about 3 ms of
restore), covers the trial's 61-64 deep notices; deeper
ones resync. (iii) **A cap tied to the trafo inputs** (120 off, lower on),
the cost being 2.7x per tick with them. The code takes any of them by
changing `kReachCapTicks` (and `kHeldMaxReachTicks`, asserted equal).

**The trial's resyncs** (`.local-tools/work/TRIAL1/rounds/round1-holdlast`):
client 1 stopped reaching the server at 20485 because both game processes
hitched about 17 / 13 ticks (client 1 saw client 2's frames at lag 4
instead of 21, client 2 saw client 1's at 24 instead of 37, and the
60-tick windows took 1.03 / 0.96 s; the server kept its 15 ms steps from
20486 after waiting its full 99 ms for 20485) while client 1 had only 6.7
ticks of slack at the server - the resync-base deadline phase that RL1 fixed
(its rebase resync at 1851 had dropped it from 21.8). Those 52 notices came
inside the reach. The three resyncs followed server stalls: the server's
journal shows 646 ms between steps 20584 and 20585 and 595 ms between 20770
and 20771 (more than any deadline wait, at most the largest slack, 22 ticks
= 336 ms), during which both clients received nothing (`window:` relay 29 /
48, then 92 with 47 / 39 late). Notices for 20585 and 20771 / 20778 reached
their owners 64, 61 and 61 deep. With this rule (`work/REACH/trial_reach.py`,
the windows' lag as the sample): client 1 at 20648 had p99 26 -> reach 48 <
64, **still a resync**; client 1 at 20831 had p99 61 (the first stall's
burst still in its last 4 windows) -> 69 >= 61, **avoided**; client 2 at
20838 p99 77 -> 85 >= 61, **avoided**. RC1d's `resync_at150_down` d0 cell
(notices 49 deep, p99 47 -> 55): **avoided**. What stalled the server is not
established (the journal flushes every checkpoint, every 2 ticks here, to
`/mnt/d` from WSL; unmeasured).

**Found: the margin of 8 does not cover the server's wait.** A notice of
our substituted tick T leaves with the server's step of T, which waits for
our missing frame until the deadline, while the datagrams we sample leave at
the normal step, the arrival of the latest member's frame. So a notice is
`h + w + 1` deep, h the header lag and w that member's slack at the server
(the smallest slack of the session): about 21 ticks at the arena with
RL1's slack of 22, 6.7 in the trial (client 1), 6 in RC1d's cell. The rule
covers it when `w + 1 <= 8 + (p99 - h)`: with small slacks, or after a burst
has lifted the p99. It does not for the case this step was written for: R5's
run-end resync (core design, "As built (step R5)": relay lag 41-43, the
notice 64 deep) gives p99 43 -> reach 51 < 64. Not changed here; it is the
user's choice (roadmap §2, the user's rule of 2026-10-27).

### 2.6 The session stops waiting for one player

Part one makes a late frame harmless to the server's state. It does not stop
the session from waiting until `deadline(T)` for it. That is cost 2 in §3.6:
up to 24 ticks of extra relay lag for everyone at 300 ms.

**Patience, revision 3: a quorum timer.** For tick T, the **counted** members
are those that are ready, have a baseline (§2.1), are not exempt at T (in a
held range, or below their cursor) and are not waived. p is waited for while
its lateness against the counted others stays within M. Patience runs on
`seen` (§2.1): `ns_q(T) = A_q - (deadline(T) - seen_q)/tick`.

`patience_p(T) = min(deadline(T), deadline(T) - (A_p - R_p(T) - M)·tick)`

`R_p(T)` comes from one of two timers:

- **The last member.** `R_p(T) = max(0, max ns_q(T))` over the other counted
  members. It exists once all of their frames for T are seen. With equal
  baselines this is "M ticks after the latest of the others was seen". Only
  the member still missing when everyone else is in can be cut by it.
- **A minority late together** (four or more counted members).
  `R'_p(T) = max(0, the m-th smallest ns)` among the counted members whose
  frames are seen, `m = ceil(n/2)`. It exists once m of them are seen, and it
  takes the margin `M2 = 10` (`kQuorumWideMarginTicks`) instead of M. Of the
  two patiences, the earlier holds.

The clamp is §2.1's. A reference never lies before on time, and a raised
member that is still late counts with its own lateness. Revision 3's `g`
put the healthy members' patience at the deadline for as long as the raised
member stayed late. That cost them up to M2 at the next burst of one of them
(four members), or up to the deadline (two or three).

p has no patience before the deadline (it waits the deadline, today's rule) in
three cases: p has no baseline, no counted member's frame for T is in, or
neither timer exists yet (a majority is missing).

**Why `seen`.** On `at` the timer would start at the others' backdated
arrivals, while p's frame counts only once it is seen. M would then act as M
less p's poll delay: in the replay, M = 3 cuts 85 against 58 at M = 4. On
`seen` the others' poll delays move the timer with p's. A main-loop stall
delays everyone's `seen` together, so it cuts nobody. The runner's wake-up
can be up to a Windows timer period (15.6 ms) late, which only makes patience
later. The S1 quorum line prints p50/p99 of `seen - at` and of the wake-up
against the patience it slept for.

**The step test** (patience on). The tick steps when every member has its
frame, or is exempt, or is waived, or is counted with `now ≥ patience_p(T)`;
or when the tick is due. The pacing floor stays: an exempt or waived member
is exempt only while some member's real frame for T is here (177c337:
lateness.hpp:691-705). With none, the tick steps at the deadline. Members
without a baseline wait for the deadline, so the first 264 samples of a
session (about 4 s) run today's rule. A late joiner's uncapped lead puts its
frames early anyway (A8). The runner sleeps until the earliest patience among
the blocked members (session_runner.cpp:1074-1081, 1111-1114, as built). Only
`wait_ticks`' meaning changes.

**What the others pay.**

- At most M (60 ms) on the first tick of one member's episode.
- At most M2 (150 ms) when two or more of at least four members go late
  together.
- Nothing after that first tick, because of the waiver.
- Nothing more because another member was raised: a raised member counts
  with its own lateness (above).
- A late majority (two of three, three of four, or more) is waited for on
  every tick until the minority rule raises it. Its decision comes about 726
  ticks (66 + 660) after the shift and its E about 60 ticks later, 12 s in
  all, and then nothing. Until then the others pay
  the majority's lateness, about 20 ticks of relay lag at +300 ms (arm
  A3m). This cost is accepted: nothing stops the whole session for **one**
  player, and a majority is not one player. A timer that cut the majority
  instead would substitute three players' inputs for those 12 s to spare one
  player 20 ticks of relay lag.

A member structurally later than the others is not "late": its baseline holds
it, and the others wait for it exactly as in the flags-off session. That is
the session's own sizing (`input_delay` from the worst ping), and the
principles freeze it.

**The streak waiver** starts as built, at a substitution of a frame the step
had to do without (session_runner.cpp:654). It ends on the first frame of p
the step test would have waited for. That is a frame seen before
`patience_p(T)`, computed for T after the fact, with p counted, from both
timers. When p has no counted reference at T (solo, a partner without a
baseline, every other member exempt), patience is the deadline, so the
waiver ends on a frame seen with slack ≥ 0. That is lateness.hpp:769-773's
rule with lead 0. Without that case, a waived member beside a member that is
still measuring its baseline would be cut on every tick for about 4 s while
on time, because the pacing floor lets the session step on the other member's
frames. The member whose arrivals are structurally the last is still never
stuck: with its baseline it is within M of the others. Without the waiver,
every tick of a blackout or of a persistent lateness would cost the others M.

Revision 3 ended the waiver on `L_p ≤ M`, with U the upper median. That is
not the patience the step test applies. On lp11_a0_off it cuts 81 (0.58 %)
against 61 (0.44 %) for the end above (`rv4_w.py`). There is no hysteresis.
Ending the waiver one or two ticks inside the patience instead saves the
others nothing: ending it at M, M - 1 and M - 2 gives 13, 13 and 12 episodes
on lp11_a0_off, and 22, 24 and 26 on a0j. It only adds the member's own cuts:
61, 65 and 67 on lp11_a0_off, and 95, 108 and 117 on a0j.

**Sizing M and the quorum.** The replay of lp11_a0_off covers ticks
27-3496, 13 880 member-ticks, with the waiver. For comparison, revision 2's
flags-on control actually substituted 1509 ticks (13.6 %) in lp11_a0, ticks
27-2800.

| rule | M = 3 | M = 4 | M = 5 |
|---|---|---|---|
| the last member's timer (latest of the others + M) | 85 (0.61 %) | 58 (0.42 %) | 46 (0.33 %) |
| plus the minority timer (2nd arrival + 10) | | 61 (0.44 %) | |
| lower median of all four + M (the code map's t_q) | 159 (1.15 %) | 106 (0.76 %) | |
| timer from the first arrival, restarted on each (the proposal as written) | 1045 (7.5 %) | 861 (6.2 %) | |
| revision 2's patience and waiver | 1506 (10.9 %) at its own r01 | | |

Notes on the table:

- **What the 58 cuts at M = 4 are.** 26 of them are one stall of client 2 alone
  (ticks 3215-3240, up to 9.7 ticks behind the others, slack down to -0.4).
  That stall is its own: the flags-off session made the others pay for it.
- **The clamp.** It changes nothing in these runs, because nobody is early
  against its baseline: with and without it, `rv4_check.py` cuts the same
  ticks.
- **Jitter.** A0j's proxy jitter was modelled: uniform 0-45 ms per datagram,
  first arrival over the 8-frame redundancy, and A0j's anchoring offsets of
  0/2.1/1.1/2.1 ticks on raw arrivals, without baselines. That gives 54
  (0.39 %) at M = 4. The redundancy compresses the jitter to about one tick.
- **Spread out of the dips.** With nobody dipped, the gap between the latest
  arrival and the second-latest has p95 0.13-0.33 and p99 1.7 ticks, max 3.9.
  Inside the shared dips the gap reaches 5-11 ticks.
- **Why M is a constant.** A spread measured over recent windows sits at its
  floor and cannot foresee those gaps. The S1 quorum line (step R1) reports
  the spread so that the constant can be revisited.
- **Visibility.** The table is on backdated arrivals. Step R1 measures the
  would-cut live, on `seen`, and that is what closes it.

**Not adopted.**

- **The timer from the first arrival** (the proposal as written). It cut all
  three replaying members at every brake edge, because client 0 alone
  arrives 10 or more ticks earlier (above).
- **The lower median as the only reference** (the code map). It doubles the
  steady-state cuts, which is the price of seeing a late pair. The minority
  timer sees the pair at M2 for 3 more cuts in lp11_a0_off (61 against 58).
- **A latest-arrival timer that restarts on every arrival.** It is the last
  member's timer with the pair masking each other, and the minority timer
  bounds that case.
- **Revision 2's frozen absolute patience and its statistics-less cap.** It is
  replaced by the baseline and the others' lateness.
- **Waiver hysteresis** (above).

**Behind a flag until the lead works.** A player whose +300 ms frames still
beat the deadline today (slack about 2.4) is cut on every tick while the
lateness lasts, until part two raises its lead. So `physics_patience` stays
off in deployments until step R5 closes (§6.1).

Patience also makes the late player's lifecycle and Sector events late at the
server where the deadline would have taken them: a tick stepped early, or
under the waiver, is gone before an event stamped for it arrives. The server
applies such an event at the current tick and the clients at the stamp, which
is the divergence of §0 that only the re-stamp (plan step 16) removes. Until
then the S1 patience line counts them (`late-events=`) and the journal notes
each one. The flag-on closing runs of A1, A2, A4 and A7 must report that count
next to the others' criteria.

### 2.7 Lowering the lead again

Lowering exists because the extra lead costs the player: deeper rollbacks,
more CPU, remote balls shown later. A raised member whose path recovered
arrives early against its baseline, `L_p ≈ -k`. The rule (§2.1) lowers it once
q95 of L has stayed below -4 for 660 ticks, by `floor(-q95(L)) - 1`, never
below offset 0. The lower moves no tick range (`held_from = 0`, `E = N`), and
the client slews at 0.9x.

**No oscillation.**

- After a raise, q90 of L sits near 0, so q95 is far above -4: no lower.
- After a lower, q95 of L is near -1, so its frames still precede the others'
  and no relative miss follows.
- A lower never takes p's slack below the absolute threshold: its size is
  bounded by `floor(q10(slack)) - 3`, and no lower is taken while an absolute
  majority holds. A sustained shift of the server's clock, or a shared shift
  after an absolute raise, can therefore not alternate a relative lower with an
  absolute raise.
- After an absolute raise for a shift every member shares (A10, A9h), every
  member is at `n ≈ 0`, `U ≈ 0` and `L ≈ 0`: no lower while the shift lasts.
  When it ends, `n = -k` for all, `U = 0` (the clamp), `L = -k`, and each is
  lowered once.
- A member that is early from its start, a late joiner or a LAN host, has no
  offset and is never lowered.

In A5d (the uplink back at 2700) the raised client 2 reads L ≈ -20 and is
lowered by 19 at the earliest 660 ticks later; with the Sector bound of
revision 3 it had only 14 to give back.

### 2.8 Resync, rebase and late join

- *As built (step 20):* the second owner resync of lp19b_a6_1500 (D = 17,
  150 ms each way) had two causes. The session stalled 22 ticks on the
  deadlines of the 31 ticks the first resync skipped (§2.2, now fixed with
  `input_buffer::reanchored`), which put the owner's relay lag at 48-49; its
  deep replays then sent its own frames late (2747..2755 substituted, the
  CPU shared by four clients), and their notices arrived past the reach.
  Separately, off the per-player path a resync now re-anchors at the start
  base's lead (`resync_lead_ticks` = `session_start_tick_base`, the
  late-tick lead clamped to `44 - D`), 27 instead of 31 there: the owner's
  steady lag after the resync was 27-28 against the start members' 20, and
  the clamp brings it back to about 23. A late joiner keeps the unclamped
  late-tick lead: it has no earlier lag to come back to, its steady lag at
  31 is about 27 (inside the reach once the stall is gone), and no arm of
  this step measures it (A8 would).
- A resync re-anchors at `L_eff_p` (§2.5). It resets that player's meter
  samples, `held_total` and epoch, and keeps `lead_offset[p]` **and the
  baseline**: re-anchoring at `L_eff` restores the lead the baseline was
  measured against plus the offset, so p's frames fall where they fell. Only
  when the re-anchor's lead differs from `assigned_lead + lead_offset` (L_eff
  clamped at `44 - D` or 80, or a client without CAP_LEAD that gets the
  capped late-tick lead) does `A_p` move, by the difference. Re-measuring
  would have absorbed whatever lateness p had at the resync:
  - lp11_a5u resynced client 2 at 2820 with the uplink still on, and its
    residual of 6 would have become everyone's wait for the rest of the run;
  - lp11_a5d resynced it at 2817, after the uplink came back at 2700, and a
    baseline measured at the raised lead would have left the +14 for good.

  The baseline is measured only at a member's first assignment. A resync's
  anchoring error (A0j: up to 2.1 ticks) stays inside M.
- *As built (step RL1, 2026-10-27): the re-anchor's lead is counted on the
  fixed deadline schedule, not over the server's current tick.* The rule
  above rests on a premise that is false as built: that a lead over
  `current_tick` puts p's frames where they fell. The deadlines are fixed
  at the start (`tick_scheduler::start`, session_runner.cpp; deadline(t) =
  start + (t - first + D) periods) and nothing re-anchors them, while the
  server steps at the latest member's arrival capped at the deadline, so
  the current tick C sits anywhere from D ticks behind the schedule (the
  server waiting on a member's deadlines) to a few ticks ahead of it. A base
  of C + lead gave p's frames `lead + e(C) - path` ticks before their
  deadlines (e = deadline(C) - step(C)) instead of the start's
  `start_base + D - path`. A stall-triggered resync is handled while the
  server waits on that very member (e about 0), so p's frames came about
  D ticks short (about 6 instead of 22), the server stepped at p's arrivals
  from then on and relayed every member's frames about 15 ticks later for
  the rest of the session (the user's trial: relay lag 21 -> 36-37 on the
  other client; headless `core_ref_rc1/resync_snapoff`: c0, c1 and c3 see
  every remote 20 -> 36-37 after c2's pause-triggered resync at 1545, the
  server's S1 q50 for c2 22.23 -> 6.05). A resync without a stall landed
  about 5 ticks early instead (`resync_at150` c0, e = 21.7: q50 26.77, its
  own relay lag 25). And the code does not keep the baseline: every
  assignment re-measures the reference (`lateness.meter.reset`,
  session_runner.cpp `player_ready`), so the 6 became p's new baseline and
  nothing re-paced it.
  Fix (F1 of `.local-tools/work/TRIAL1/analysis/resync_lag/result1.md`):
  every re-anchor - the resync off the per-player path
  (`resync_tick_base`), the resync at `L_eff`, and the late join - takes
  `base = max(tick_at(now, lead), current_tick + 2)`
  (`session_runner::reanchor_tick`, `sim/late_tick.hpp`
  `reanchor_base_tick`; `tick_scheduler::tick_at` on a copy of the session's
  scheduler as started, kept in the runner's cross-thread view). `lead` now
  means the start's budget less the input delay: deadline(base) - now =
  lead + D periods whatever the server's pace, as the start base's
  deadline is start_base + D periods after the all-ready moment (design
  9.26). The floor keeps base - 1, the tick whose state the exact resync
  answers with, ahead of the runner's next tick; a runner that bursts past
  it before the command arrives is the base-missed path, as before (for a
  member with CAP_RESYNC_STATE only; see RL1b below). It
  binds only when the server runs more than lead - 2 ticks ahead of its
  schedule (a path under 2 ticks) and then gives more budget, never less.
  Before the session ticks the base is C + lead, the old rule (corrected at
  RL1b: that branch is unreachable, and the real pre-start window was
  clamped). A3's exact
  resync is unchanged: the state at S is the same, only the choice of S
  moves. No protocol change (the base travels in the existing assignment).
  Measured (snapshots off, 150 ms each way, D = 17, 4 clients, the
  `core_ref_rc1` recipes, maps/arena-test/rl1/): resync_snapoff - base
  1545 + 43 = 1588 (was + 27), its deadline 660 ms after the server's step
  of 1545 (was 417); the other clients' relay lag of every remote 20-21
  throughout (was 36-37 after the resync); the server's earliness
  20.6-21.4 after it (was 5.2-5.6); c2's S1 q50 22.14-22.16 (was
  6.04-6.08); c2's own lag 20-21. resync_at150 (e = 21.9 at the resync) -
  base 860 + 22 = 882 (was + 27), c0's S1 q50 22.30-22.37 (was
  26.55-26.77), its relay lag 21 (was 25). Both servers' journals replay
  exactly. Not run: the late-join arm, lead_adjust / patience on with a
  stall, and the LAN case (D at its floor of 6, where the old shortfall
  was about 6 ticks), deferred to the stage close-out
  (`docs/roadmap.md` §4).
- *As built (step RL1b, 2026-10-27): the follow-ups of RL1's review
  (`.local-tools/work/RL1/review_retry/`, no blocker).* What RL1's rule
  costs and where it does not hold, and three small code changes:
  - **The exact resync's re-step depth.** The client re-steps from base - 1
    to its own tick when the state arrives, which is how far ahead of the
    server's stepping the resynced member runs: resync_snapoff 12 -> 28
    ticks (`maps/arena-test/rl1/resync_snapoff` c2, 1587 -> 1615, against
    `core_ref_rc1`'s 1571 -> 1583), resync_at150 34 -> 29 (881 -> 910,
    against 886 -> 920). The 12 was the defect's shrunk budget (the member
    6 ticks ahead of its deadlines instead of 22), not a baseline; 28-29 is
    what a member at the start's budget re-steps, and the pre-RL1 at150
    already re-stepped 34. The client's reach guard
    (client_core_resync.cpp, the re-step refused past `resync_reach()`, and
    `poll_resync_hold`) bounds it as before; the lead fed to the rule is
    still clamped to 44 - D. Deferred check (d), the retail two-copy stall
    resync, logs the re-step depth and its apply time as an observation.
  - **The held window is lead + D - e(C), unbounded by a backlog.**
    `input_buffer::reset(base)` discards the member's frames below the base,
    and the ticks [C, base) are applied held. With the base on the schedule
    that window is lead + D - e(C) ticks (e = deadline(C) - step(C)): lead +
    D when the server steps at that member's deadlines (e near 0, the stall
    case: 1545 -> 1588 in resync_snapoff), a little under lead when it is
    arrival-paced (e near D + the path's slack), lead + D + B when it is B
    ticks behind its deadlines (a backlog: the trial's earliness of -23 at
    20580; or a resync while it waits on a different member). The old rule's
    window was lead. Frames below the base of a member
    whose numbering was intact were on time and are now discarded, and A3
    snaps its own ball onto the held trajectory. This is the price of the
    session's phase; capping the base would bring RL1's defect back.
    Deferred check (b) also measures the resynced member's own correction
    in a non-stall resync while the server waits or lags.
  - **The late joiner off the per-player path** is anchored at
    `late_tick_lead_ticks`, which is not clamped to 44 - D like the start
    base (`session_start_tick_base`): at D 17 and a peak round trip of
    180 ms or more it is 31 against the start members' 27, so the joiner's
    frames land up to about 4 ticks early and its relay lag sits about 4
    above the others' until a resync. Not a regression (the old rule gave
    31 + e(C), about +9 when arrival-paced); "every re-anchor gets the
    start's budget" holds for the resyncs, not for that arm. Left as is;
    deferred check (a) measures it.
  - **The base-missed fallback is the state path's only.**
    `on_resync_base_missed` re-answers a member whose base the runner
    stepped first only when it applies the resync state (CAP_RESYNC_STATE,
    the session core). A legacy member gets a full snapshot and nothing
    re-assigns it; its first frames would be counted stale and held. Code:
    its floor is current_tick + 3 (`kReanchorBaseLegacyMarginTicks`, the
    smallest lead `late_tick_lead_ticks` ever gave, so at least the margin
    it had before RL1, where a legacy resync usually got more,
    C + resync_lead_ticks), the state path's stays + 2 (`reanchor_margin_ticks`;
    `reanchor_tick` and `resync_tick_base` take the member's flag, the late
    join the joiner's caps).
  - **Before the schedule's start.** `tick_scheduler::tick_at` clamps every
    instant before the start (the first start_base periods after the last
    SessionReady, design 9.26) to first + lead, so a re-anchor there got up
    to start_base periods more (a joiner 100 ms after the all-ready moment
    about 20 ticks more at start_base 27). Code: the schedule's tick is
    taken SIGNED and floored (`sim/late_tick.hpp` `schedule_tick_at`,
    `reanchor_tick_on_schedule`; equal to `tick_at` from the start on), so
    deadline(base) - now is lead + D periods there too. The branch "before
    the session ticks: C + lead" stays only for a session with no schedule,
    which the server never re-anchors (resync and late join both need
    `running`). `timeline.hpp` is not changed (shared): the runner's view
    keeps the schedule's start and first tick instead of a scheduler copy.
  - **One start site.** Both start paths (the last unready member left,
    the last member became ready) go through `session_runner::start_ticking`,
    which starts the scheduler and stores the schedule in the view, so
    neither can drop it. No world-less `session_runner` fixture exists (a
    start needs a booted world), so the runner's own call is not
    unit-tested; `tests/lead_test.cpp` `ReanchorOnSchedule` tests the rule
    the runner calls - equal to `reanchor_base_tick(tick_at(now, lead),
    next_tick)` from the start on, the start's budget before it, the
    signed floor, the 3-tick legacy floor.
  - Stale comments (late_tick.hpp's header, server.cpp's late-join,
    resync and base-missed comments) now say the base is counted on the
    schedule, floored at the current tick + 2.
- The lower's gate is `lead_offset > 0`, which a resync keeps.
- A SessionLead whose `epoch` is older than the client's base is dropped.
- A client rebase (more than `33 + |slew|` behind) still asks for a resync.
  The CPU stall that causes it is the client's own.
- A late joiner starts at `late_join_lead_ticks` with offset 0. Its frames
  arrive early, and it is judged like anyone else once its baseline is set
  (264 samples). Until then it is waited for until the deadline and is nobody's
  reference, and a waived member beside it ends its waiver on the absolute
  rule (§2.6).

### 2.9 Why the others are unaffected

The changes touch nothing the others see:

- `input_delay`, `sector_lead`, the scheduler, the start base and every other
  buffer are untouched;
- the reset is per buffer, and `settled()` keeps it out of the step timing;
- the quorum (§2.6) costs the others at most M on the first tick of one
  member's episode (M2 when a minority of two or more of at least four is late
  together), and nothing a single member does moves it. The reference is the
  others' lateness clamped at on time, and each member's own frozen baseline,
  and neither moves when a member is raised;
- the relay of the late player's frames to the others is what it was: the
  applied frames;
- the late player's own Sector stamps land `k` ticks *further* ahead for
  everyone else once it is raised, which only adds margin.

**The Sector margin, re-derived (revision 3).** Another member q stamps a
Sector event `S` ticks ahead of its clock (`S = D + 1 +
input_delay_for_ping(worst rtt)`, timeline.hpp:79-81; 35 in lp11_a5u). The
event takes q's uplink to the server, which relays it at intake
(177c337: server.cpp:1816), and p's downlink. It reaches p when p's clock reads
`c_q + up_q + down_p + (c_p - c_q)`. So p's margin against q is

`margin_pq = S - up_q - down_p + (c_q - c_p)`

in ticks. A raise of p by k takes k off every `margin_pq`. The headless client
does not rewind for a late remote Sector (session_client.cpp:3015-3017): it
starts the sector a frame late, and that client's mechanisms stay out of phase
for the rest of the session. So until plan step 14, `k ≤ margin_pq - 1` for
the senders q that bind (below).

- **What the margin protects: the first crossing of a sector.** The union only
  grows. A Sector event for a sector the union already holds is a no-op on
  every side (sector_union.hpp:68-70: `request` inserts nothing, and `tick`
  starts nothing). Only an event that adds a sector can be late in a way that
  matters. The arena has four sectors, and in lp11_a5u the union is complete
  at tick 1713 (server.log:126). The deferrals at 1918 and 2579
  (server.log:141, 204) protected nothing, and neither did revision 3's "L ≈
  6 for as long as the uplink lasts". So the bound applies only while some
  sector of the level has not been requested at a stamp at or below the
  session's next tick N. Once every one has, every event still in flight is
  a no-op. The world counts the level's sectors at boot
  (`level_sector_count()`, from the level's `Sector_NN` groups, one per
  sector, as bbp_ng writes them: build_arena_map.py:535). When it cannot
  tell (0), the bound always applies.
- **The 14 in lp11_a5u is the true limit before the union completes.** Before
  the shift, `margin = 35 - 10 - 10 + 0 = 15`. A delay on p's uplink does not
  enter the formula: p's clock does not move, and the others' events never
  travel p's uplink. So `k ≤ 14` against the 20 that client 2 needs, while
  the union lacks sector 4 (stamped 1713; the decision was at 1685). The
  full 20 would deliver another member's first crossing 5-6 ticks after
  client 2 simulated its stamp. After 1713 the remaining 6 go out at the
  next decision (about 1918).
- **The estimate is right.** The server estimates p's clock as `c_est = t_newest
  + (N - n_arr) + ceil(rtt_p)` (lead.hpp:56-60). An uplink delay cancels in it:
  the newest frame is older by `up`, and the round trip is longer by `up`.
  This is exact while GNS's rtt is `up_p + down_p` (601-605 ms for client 2,
  which fits). Its biases are all small, and two of them make the margin
  smaller than it is (conservative):
  - the ceil, under a tick, conservative;
  - the time between the relay at intake and the runner reading its tick,
    conservative;
  - the session-tick term `(N - n_arr)`, exact while the session steps on its
    schedule.
- **The samples were wrong.** Revision 2 sampled p's margin from every other
  member's Sector events, pooled, the worst of the last 8 (lead.hpp:71-95,
  153-159, 211-212). That makes p's bound a function of whichever sender was
  worst:
  - In lp11_a5u, client 2's own sector-4 event was stamped for 1713 before its
    raise was acknowledged. It reached the others late (`client0.log:1086`:
    "LATE: the server started it already"), but it was on time at the server,
    so it was sampled. The healthy members read margins of -8 and -9 from
    then on (server.log:143-145), and their (reference-bug) would-raises at
    2784-2807 were deferred by it.
  - Before any shift, event samples sit below the path model's 15 for some
    receivers. At tick 1320 (the sector-2 and sector-3 crossings), one
    member reads 9 in lp11_a9, two read 12 in a5s, 13 and 12 in a5d, and 13
    in a4. Others read 15-16, and a9c reads 17-20. Revision 3 attributed a
    "margin 1 at tick 1320" to lp11_a5u; there is none: a5u reads 15 for all
    four at 1320 (server.log:105-108). The 1 at 1980 (server.log:146) is client
    2's own margin after its acknowledged 14 (`15 - 14`). The 1, 3 and 5 at
    3300 in lp11_a0, a0j, a4 and a9 are healthy members' margins after their
    own (reference-bug) raises.

  Revision 3 (step R5) keeps the bound **per pair** (p receiving, q sending):
  - **Path samples.** On every datagram of q, for every other member p:
    `t_newest_q + S - c_est_p`, the margin of a crossing of q at that
    datagram's tick, relayed now. There is no on-time filter. A sender in its
    own catch-up sends older newest ticks, and a crossing it stamps then is
    exactly that late, so the path sample shows what its event would.
    Revision 3's filter (`L_q ≤ M`) dropped a raised sender that is still
    late as a sender, although its events reach the others at about
    `35 - 30 + 14 - 10 = 9`.
  - **Event samples.** Each on-time Sector event of q (as built,
    lead.hpp:153-159) is also a sample of `margin_pq`, kept until R5's
    per-pair lines explain the 9-13 lows above (open question 9).
  - The window per pair is the minimum over the trailing 660 ticks, kept as
    ten 66-tick buckets.
  - **Binding.** p's bound is `min over q of (min_pq - 1)` over the senders
    whose `min_pq ≥ 1`. A sender at or below 0 is late for p whatever p
    does: its first crossings reach p late today, the raise changes how late
    and not whether, and only that sender's own raise (or step 14) fixes it.
    It is reported (S1: `sector-late=q`) and binds nothing. Otherwise one
    late sender would cap everyone else's raises at nothing.
  - **No samples at all** with two or more members defers the raise.
    lead.hpp:211's "unknown means uncapped" goes.
  - **On an acknowledgment of p**, every pair with p on either side is
    cleared; the cooldown (`E + 2k + 132`) refills it with at least 132
    datagrams per sender. Path samples are re-measured continuously, so
    revision 2's shift of the stored samples (lead.hpp:85-87, 291) would
    count a sample taken after E twice.
  - The runner needs S: `create_session` takes `s.sector_lead`
    (server.cpp:835).

  In the scenario the per-pair bound exists for, client 2 has been raised by
  14 and its events reach the others at 9. If client 0 then takes a +200 ms
  uplink and needs 13, it gets 8: the pooled path samples of revision 3 would
  have allowed 13, and client 2's next first crossing would reach client 0 at
  -4.
- **What lifts the bound for good: step 14.** On a late remote Sector, step 14
  rewinds the client to the stamp minus 1 and re-stamps the union. The bound
  then becomes p's own reach, `k - (margin_pq - 1) ≤` p's rewind depth (step
  12: `max_resim` up to 120, history 128), and it stays at lead.hpp:211-212
  as a reach bound that costs p only. (As built: resync design step B2,
  `lead_sector_bound`, gated on `CAP_RESYNC_STATE`, with the reach at 48 =
  `max_resim_ticks` until step 12; `resync-and-sector-timing-design.md`,
  "As built (step B2)".) Raising S instead would move every
  member's activation, so the principles rule it out.
- **What the bound costs until then.** Only while a sector is still unentered,
  and only p. A member raised as far as the bound allows stays at `L ≈ k -
  bound`, is cut on most ticks, and is corrected by part one: its inputs take
  effect late, with constant own replays. The meter asks again after its
  cooldown, or 660 ticks after a raise the bound left nothing of, and the
  rest goes out once the union is complete. Neither the others nor the
  server's state pay for it.
- **The other direction.** Before p is raised (at least 66 + 132 ticks after the
  shift), p's own first crossings reach the others late:
  `margin_qp = S - up_p - down_q = 35 - 30 - 10 = -5` in lp11_a5u. That is a
  cost to the others that only step 14 on the receivers removes. Step 16's
  re-stamp does not: the events are on time at the server. A late first
  crossing of p is also the one case in which p's own lateness reaches the
  others; it ends with the union.

## 3. Part three: the barrier goes; late events are re-stamped

Report B's recommendation is adopted: options (a) and (b) together. One change
is made to (a): the re-stamp is decided where the event is **accepted**, not
where it is applied. That way an on-time event is relayed as early as it is
today.

### 3.1 The server's re-stamp rule

`submit_event` posts to the simulation thread (session_runner.cpp:377-387).
There, with the session's next tick `N` exact (no cross-thread view
involved), the runner fixes:

**`applied = max(stamp, N)`**, and stores it in `pending_event` beside the
original stamp, the **origin**.

Since `applied ≥ N`, every event is applied at exactly its `applied` tick.
`step()` keys "due" on `applied` (:504). It sorts a tick's events by join rank,
then by **origin**, then by type rank (:512-517). The Unphysicalize guard and
`last_physicalize_tick` compare **origins** (:520-533). Otherwise a death
stamped after a re-stamped birth would be judged superseded, and the ball
would live on.

**Every receiver uses the origin the same way.** The server's order is only
deterministic if every client keys, sorts and guards on the same fields. The
event message carries `origin_tick` beside `tick = applied`, and:

| where | today keyed on `tick` | v7 |
|---|---|---|
| dedupe key `lifecycle_event_key` / `lifecycle_key_of` (lifecycle.hpp:100-135) | (tick, player, type, ball type, sector, name) | `tick` is the **origin**, `applied` is added, at session_client.cpp:750, physics_session_client.cpp:2176 and the server intake (server.cpp:1327) |
| order (session_client.cpp:2883-2888; physics_session_client.cpp:2179-2184) | (join, tick, rank) / (tick, join, rank) | (join, applied, origin, rank) on the headless client; (applied, join, origin, rank) on retail, its existing batch order |
| guard (session_client.cpp:3023-3024; physics_session_client.cpp:2253-2264) | stamps | origins, both for "older than the last lifecycle event" and for "Unphysicalize of a replaced body" |
| ghost birth for late joiners (server.cpp:1471-1478, `last_physicalize`) | stamp | origin, updated at acceptance on the simulation thread |
| client-journal replay (sim_tool.cpp:2930-2938) | join rank within a tick | (join, origin, rank) from the v3 EVENT record |

Two cases that only the origin decides. First, one uplink outage covers P
(origin 100) and U (origin 110), and both are applied at 130. Ordered by
origin, P then U, the ball dies everywhere. Ordered by `tick` and rank, a
remote would run U first (a no-op), then P, and then drop any later U as
superseded, leaving a ball the server does not have. Second, two same-type
births in one outage (trafo wood → stone → wood) no longer collide on one
dedupe key, the owner's echo included.

**A birth overtaken by a newer event is superseded at acceptance.** The
runner keeps, per player, the (origin, applied) of the Physicalize and
Unphysicalize events accepted in the last 128 ticks. A Physicalize whose
origin is older than an accepted P or U, but whose `applied` would fall after
that event's `applied`, is superseded: it is never queued, never relayed to
the others, and the owner gets a SUPERSEDED echo (§3.2). This covers:

- a birth whose every copy was lost and whose reliable retransmit landed
  after an on-time death (U at 110, P at 128). Applied, it would leave a
  ghost ball in the server and on every remote that nothing ever takes out;
- a birth that loses its piggyback datagrams and waits for a retransmit
  while a later beam or death is accepted first.

The applied comparison matters. An older birth that still lands before the
newer event (U accepted on time for 110, P accepted at N = 98 for 100) is the
ordinary out-of-order arrival of protocol 6 and is applied. An Unphysicalize
overtaken by a newer Physicalize stays with the existing guard, on origins.

`e.event.tick = applied` (replacing :545), so the spawn impulse index comes
from the applied tick (physics_world.cpp:829-831). The "late by N" log stays as
a diagnostic.

**The relay moves to acceptance.** A new callback, `on_event_accepted(
session, player, origin, applied, event)`, fires from the posted lambda. The
server serializes and relays the event from there, with `tick = applied` and
`origin_tick` (session_event_msg, protocol 7). It goes to **every** member,
the owner included, reliable no-Nagle plus the 6 repeats. The intake relay
(server.cpp:1464-1493) goes away. `last_physicalize` and `sector_history`
(1470-1480) are updated at acceptance. For an on-time event (`applied ==
origin`) the owner's echo is a plain ack.

The posted command normally runs within a few milliseconds. It can wait
behind a catch-up pass, though, of up to 10 steps for every session
(session_runner.cpp:607-623), and that eats into `sector_lead`
(timeline.hpp:79-81). So the server logs the intake→send delay per event (an
S-line), and plan step 17 is closed only if its p99 is at most 2 ticks. If it
is not, on-time events keep the intake relay, and only re-stamped events
relay from the simulation thread.

### 3.2 Removals and the other server-side fixes

The barrier itself is deleted: `waiting_for_lifecycle` and its call
(session_runner.cpp:431-471, 618), `barrier_*` and `lifecycle_missing`
(session_runner.hpp:205-213) and `physicalize_pending` (lifecycle.hpp:26-44).
The world never reads `INPUT_FLAG_PHYSICALIZED` (physics_world.cpp:729-746),
so stepping a flagged frame without a ball is safe. When that happens, a
capped log line and a journal note record it (S4). Until plan step 20 the
barrier sits behind a runner config `lifecycle_barrier` (default on), so the
move arms can be run without it before the code is deleted.

*As built (step 20):* the barrier, `physicalize_pending` and the config are
deleted. S4 is one line per run of such ticks (`S4 bodiless player= ticks=A..B
n=`) when the run ends, and one more marked `open` at 66 ticks, capped at 50
lines per session, each with a journal NOTE; a line per tick would repeat at
every birth, whose stamp trails its first flagged frame by up to
`input_delay + 1` ticks. The owner's deadline for a missing echo (§3.3,
`kOwnEchoDeadlineTicks`) reads the server's tick from the relay header as
well as the snapshots, so with snapshots off a Physicalize that never reached
the server no longer leaves its owner a ball for good. The deadline counts
only once the header's `acked` shows the server applied a frame of the owner
at or past the birth's origin, and 66 ticks after that was first seen
(`own_echo_deadline`): the server's tick moves with the downlink, and with
the uplink out longer than the deadline the reliable copy is still queued at
the owner, whose give-up would be followed by a second resync when the copy
is echoed as made. Runs of at most `input_delay + 1` ticks (every birth makes
one) are counted in `describe` (`bodiless=runs/open/short`) and not logged,
so the cap is spent on the long ones; the `open` line is given once at or
past 66 ticks. This is the headless client only: the retail mod still has no
deadline of its own and keeps a ball the server never made, with snapshots
off, until plan step 24 (§3.5).

Other server-side fixes come with it:

- **The leaver's Unphysicalize.** It is produced by `remove_player` on the
  simulation thread at the runner's own tick, through `on_event_accepted`,
  instead of being stamped from the network thread's view (server.cpp:1077).
- **A lifecycle budget instead of the rate limit.** Physicalize, Unphysicalize
  and Sector no longer count against `physics_event_rate_limit`
  (server.cpp:1340-1355), which can drop a Physicalize. Each player has its own
  budget per type instead: 2 Physicalize and 2 Unphysicalize per 8 ticks of
  origin, and one Sector per sector. The budget is enough for a beam pair and
  a trafo, and it still bounds a spamming client's body creation. An event
  over the budget gets a REJECTED echo.
- **BodyRevived** is deduplicated (1430-1439) *before* it is counted
  (1342).
- **REJECTED and SUPERSEDED echoes.** A malformed recipe (1380), an event
  over the budget, and a superseded birth (§3.1) are answered to the owner
  only, with a flag bit in the event message. The owner removes the birth
  exactly as it moves one (§3.3, with `applied` = never), and then matches the
  server: no ball. None of these stalls anyone any more.

### 3.3 The owner moves its own birth inside its history (headless)

**`own_life_` replaces `own_beam_` and `own_capture_`.**
`capture_own_physicalize` (1474-1528) keeps its capture after the report
(2142 clears it today). `report_physicalize` (2123-2150) turns the capture
into an `own_life_` record with:

- kind (birth, death or beam), origin T, applied tick, entity, generation;
- the pose, post-offset and pre-step;
- the recipe from `fill_retail_recipe`, the group, and the spawn flag;
- a state: `sent`, `acked`, `moved`, `rejected` or `superseded`.

The beam machinery (1313-1374) becomes the `beam` kind of the same record.
A record retires when `!knows_tick(min(T, applied) - 1)`, the pattern at 1373.

**Echo with `applied = T' > T`.**

1. If `current - (T-1) > reach` or `!knows_tick(T-1)`, the client cannot
   rewind that far. It requests a resync (§3.5).
2. Otherwise it marks the record `moved` and requests a lifecycle rewind to
   T-1, with the reason "own birth moved T → T'".
3. **At the restore**, the own body made live at T is destroyed.
   `destroy_body` accepts the own entity when the record says moved
   (3476 refuses it today). The client mutes the event log, shuts the own
   navigation replica down first (as at 1327), and puts the entity back at
   the captured pose.
4. **From T to T'-1** there is no own body. The server had none either: its
   `set_input` reached a player with no ball. The records of those ticks are
   re-captured without an own entity.
5. **At T'**, in `drain_pending_births` at the owner's join rank (as
   `apply_own_beam` is placed in 2911-2926), the client:
   - makes the body at the captured pose with the recipe and the group;
   - re-attaches navigation and bumps the generation;
   - kicks with `spawn_direction_index(seed, join_order, T')` if the spawn
     flag is set.

   Every later replay crossing T' does the same until the record retires,
   exactly as the own beam does today. The re-made body is fed from the
   record where it has one, and otherwise from `own_inputs_`, which holds
   the server's frames for every held tick, with or without a body (§1.3).

**Spawn impulse and spacing.** The impulse index comes from T' on every side:
the server (§3.1), the remotes (they only ever apply at T'), and the owner's
re-made birth. The live kick at T (1522) dies with the body. The birth cell
offset depends on the join order and the resetpoint at capture only
(1504-1519), not on a tick. It is already part of the captured pose, so
nothing is recomputed.

**What the player sees.** The echo always arrives after the client has passed
T', because the client runs ahead. So the move shows as one jump at the
replay: the ball has lost T'-T ticks of motion. The rewind depth is `(T'-T)
+ relay lag`. At 150 ms each way the relay lag alone is 32-44, so a move
fits the 48-tick reach only for `T'-T` of 4 to 16 ticks. Larger moves resync
the owner, which is the owner's cost alone. That is why the move arms run at
50 ms each way (§5.2).

**The Unphysicalize side** is the mirror, and its own step (plan step 22). A
death moved D → D' rewinds to D-1. The replay re-makes the own body from the
D-1 record (`insert_birth` for the own entity, recipe from the `own_life_`
birth record, which is therefore kept while the ball lives). It drives the
body from `own_inputs_`, which holds the frames the client kept sending after
the death, amended to the server's wherever the server substituted. It
destroys the body at D'.

A death superseded at the server, because the respawn overtook it, is echoed
SUPERSEDED. The owner then treats it as moved to the applied tick of the
replacing Physicalize.

The mirror is required, even though a dead ball is below the level and
touches nothing. The body set shapes IVP's overlap tree, and the arena runs
showed that tree's identity matters for bit-exactness.

**An own Sector moved** T → T' stamps the union at T' and rewinds to T-1. The
replay re-issues the activation (the machinery from
docs/mechanism-phase-at-sector-activation.md and the 2026-10-22 solo results),
once plan step 13 has proven it for a late stamp. Own Sector events are
stamped `sector_lead` ahead, so a re-stamp needs lateness beyond that margin
and is rare. Until plan step 23 lands, the owner resyncs instead.

**BodyRevived** stays server-only (physics_world.cpp:878-891: a wake of a body
the server's own scripts also run), re-stamped and not echoed. Whether it is
still needed under the sector union is open question 4.

### 3.4 The remote side, and option (b)

**Remotes** receive the applied tick and the origin. Their late-birth path
(headless 2985-3006, `pending_births_`, rewind to applied-1; retail
on-arrival 2247-2265) works unchanged on `tick = applied`. Dedupe, order and
the guards use the origin (§3.1 table).

**Option (b): the event rides the uplink.** `session_input_msg` v3 appends
`nevents` u8 and then serialized lifecycle payloads.

- The client includes each of its Physicalize, Unphysicalize and Sector
  events in every input datagram from its send until `stamp + 7`.
- BodyRevived stays reliable plus repeats.
- The unreliable uplink repeats of those three types go. The reliable copy
  stays, for an outage longer than the window.
- `handle_session_input` runs the piggybacked events through the intake
  (dedupe at 1327) **before** submitting the frames, so a present frame T
  always comes with its event.
- With 8 frames of 42 B and a beam pair of about 300 B, a datagram stays
  under 1100 B. A datagram over budget drops its oldest piggybacked event.

What (b) removes is B's case A (the event lost while the frame arrived, about
1 % of births at 10 % loss) and case D (trafo). What remains moves only when
the frames are late too.

**Patience makes that rarer, not more common.** A birth rides with its frame,
so a substituted birth frame is a re-stamped birth. The revision 3 quorum
(§2.6) cuts off about 0.4 % of a player's arrivals in the lp11 replay, and
births are a few per minute. Arms A0, A0j and A0h require 0 moved births and
substitutions within the flags-off twin + 0.5 % of ticks (§5.2).

**A birth stamp lead was considered and is not adopted.** A birth would be
stamped a few ticks ahead, as with `sector_lead`. That delays the owner's own
ball by those ticks in the retail game, where the scripts expect it at once.
If A0j fails, this is the next lever.

### 3.5 Retail mod

- **Remote events at T'** work as today; `event.tick` drives the impulse
  (2333). Dedupe, order and guard use the origin (§3.1).
- **Own echo, on time:** an ack.
- **Own echo, moved, rejected or superseded:** the mod has no rewind, so the
  move is deferred to the input-rewind card. With snapshots on, the snapshot
  rollback reconciles the own ball, since the server's ball exists from T'.
  With snapshots off, the mod requests a resync of itself.
- **"Server has no ball for us"** (2565-2601) keeps its snapshot-driven
  re-report as a backstop. The REJECTED echo makes it immediate.

### 3.6 Every whole-session cost found in report B

| # | cost | decision |
|---|---|---|
| 1 | lifecycle barrier | removed (§3.2) |
| 2 | one missing frame puts the session on deadlines | removed: patience per player with the streak waiver (§2.6), and `settled()` for reset ranges (§2.3); without patience, a lead reset's held range and, since step 20, the ticks a resync's or late joiner's base skips (`reanchored`, §2.2) |
| 3 | 10 ticks per pass, one simulation thread for all sessions | **kept.** Without the barrier a session falls behind only when the server's CPU does, which is the server's cost and not a player's. The cap is fairness between sessions. |
| 4 | start barrier, 8 s | **kept.** It acts before play, while every member is loading, and it is bounded. |
| 5 | `input_delay`, `sector_lead`, start base from the worst ping | **kept,** as the principles require. Sizing them from a robust statistic and letting part two absorb the outlier is open question 2. |
| 6 | late events applied "late by N" | removed: re-stamp at acceptance plus echo (§3.1, §3.3) |
| 7 | rejection stalls everyone 66 ticks; the rate limit can drop a Physicalize | removed: no barrier, a lifecycle budget per player, BodyRevived deduped first, REJECTED echo |
| 8 | a resync or late join sends a full snapshot to every member (server.cpp:1282, 1304) | removed: `request_full_snapshot(session, player)` sends that full snapshot to that member only; the others' cadence is untouched. Full snapshots on a body-set change stay, because those are session state, not one player's. |
| 9 | the leaver's Unphysicalize stamped from the network thread's view | removed: produced by the runner (§3.2) |
| 10 | snapshot fallback broadcast | **kept.** It is a deployment setting for everyone; the per-player need is covered by #8. |

## 4. Failure modes and determinism

**Server journal.** It still replays bit-exact. Every applied frame is
recorded as today (fresh or flag 0), so the substitute's content,
resets and patience are all timing and choices that the record already
absorbs. Events are filed under their applied tick with `event.tick =
applied`, which is what the world consumed. A superseded birth is never
applied and is recorded as a NOTE only. The EVENT record gains `origin_tick`,
so `kJournalVersion` goes 2 → 3 (journal.hpp:68). The server's own replay
does not need it, because the file is in apply order. `journal_trace.py`
reports lateness from it, and the client journal's replay orders by it. Lead
adjustments, resets, steps without a ball, rejections and supersessions are
NOTE records.

**Client journal.** It gains:

- `INPUT(own, SUBSTITUTED)` for every held tick; the reader groups by tick
  and the later record wins (journal.hpp:863-865, sim_tool.cpp:2914-2917);
- the echoed own events, carrying their origin and their state;
- lead notes.

`sim_tool` keeps the last record per (player, origin, type) and orders a
tick's events by (join, origin, rank), so a client journal replays the
server's world (plan step 19).

| part | failure | handling |
|---|---|---|
| 1 | every copy of a notice lost (32 ticks of downlink) | the `held_total` gap leads to a resync of that player; counted in `subst_late` |
| 1 | notice past the reach | resync of that player (snapshot to that player only); counted |
| 1 | a SessionAssign processed late, or notices from before it | `epoch` in the header; `held_total` counts only ticks at or after the base, so the re-anchor's held stretch is never a gap |
| 1 | the source tick was itself substituted | resolved from `sent_inputs_`, the bytes as sent, never amended |
| 1 | trafo between T and the notice | the amendment uses the record's own entity at T |
| 1 | no body at T (death, move) | the frame is stored in `own_inputs_` anyway; a replay that re-makes the body uses it |
| 1 | determinism | the replay drives the own ball through `nav_input` from the record, the path every existing input rewind already proves exact; the server drives its clone with the same bytes |
| 2 | SessionLead lost | reliable plus 3 copies; until it lands, frames ≥ E are simply late and part one covers them; no new decision while unacked |
| 2 | client cannot apply it, or the raise is capped | no SessionLead without `CAP_LEAD`; at most two raises without restored arrivals, then none; the frozen baseline and the quorum on the others' lateness clamped at on time (revision 3), so neither one member's lateness nor its raise moves the others' patience |
| 2 | oscillation | hysteresis (raise 2 s, lower 10 s, different thresholds: L > 4 against q95(L) < -4); a lower never taken under an absolute majority and never below slack 3 (§2.7); the absolute raise aimed at q50, the quantity its trigger reads; cooldown; frozen baseline |
| 2 | a shift every member shares (dips at deaths and respawns, the host clock) | relative lateness cancels it; the absolute rule (median slack below 3) raises for it only after 660 ticks when a majority shares it, so a host-clock fault under 10 s raises nobody (revision 3, §2.1; arm A9h) |
| 2 | loss, a delay spike or a CPU stall mistaken for persistence | per-datagram samples (loss gives none), and a majority of misses over 132 consecutive ticks (a spike under about 2 s cannot qualify) |
| 2 | CPU stall during a slew | rebase threshold `33 + |slew|`; a real stall still resyncs |
| 2 | resync during an adjustment | `epoch` drops stale messages; resync re-anchors at `L_eff_p` |
| 2 | determinism | the reset changes no world state; the client still steps every tick; the forced own input is recorded and journaled as what the server applied |
| 3 | birth and death re-stamped into one tick | every receiver orders by origin, so birth then death, and guards on origins, so the death is applied; dedupe keys carry the origin |
| 3 | late birth after an on-time death, or overtaken by a later beam | superseded at acceptance; SUPERSEDED echo to the owner; never relayed |
| 3 | echo past the owner's reach | resync of the owner |
| 3 | own record retired before the echo | records retire only when `!knows_tick(min(T, applied)-1)`; by then the echo is past the reach anyway, and §3.3 step 1 applies |
| 3 | multi-tick own absence (T..T'-1) confusing the retail scripts | to be measured in plan step 21; the server already runs these ticks without the ball |
| 3 | determinism | one rule (`max(stamp, N)`) taken on the thread that owns N; every receiver sees one applied tick per event and one origin order; the impulse comes from the applied tick everywhere |
| 3 | a resync with snapshots off is not exact (step 20) | the server sends its state at its tick S and the client applies it at its base B > S, its history gone; the server steps S..B-1 on substitutes the client is never told about, and with snapshots off nothing corrects the gap: the owner's ball stays 10-50 m off after a resync past the reach (lp4_a3, lp19b). The A6 1500 criterion "own ball 0.0000 48 ticks after the resync" fails by construction until plan step 25; the drop arm's own error can too once the owner's ball is back |

## 5. Verification

### 5.1 Harness additions (`scripts/arena_map_test.py`)

Today one link setting is shared by every client and by both directions
(DelayProxy 55-137; 327-329). The additions:

- **H1 Links.** A per-client link, `--link N:lat=,loss=,burst=,jitter=`, with
  a proxy per client kept by index.
- **H2 Events.** An event on one client: `--event-client N --event-tick T`
  with `--step-ms` / `--spike-ms` / `--blackout-ms` and `--event-dir
  up|down|both`. The event is armed at the client's tick from its `tick base
  … assigned` / `resynced` log line (session_client.cpp:727, 693). Loss draws
  stay seeded, so a control and an event run lose the same datagrams.
- **H3 Per-client arguments.** `--client-arg-for N:"…"`.
- **H4 Late join.** `--join-late N:T`: client N starts at tick T of the host.
- **H5 Logs.** `--server-log`, so the external WSL server's log and journal
  are read.
- **H6 Event report.** An `event_report()` per client and phase (B = before,
  D = during the event, A = after). It reports:
  - substituted ticks from the server journal, and `subst=`;
  - own, other and mechanism error, with 30-tick windows;
  - rollbacks by class (input, life, snap, forced, subst);
  - relay lag max and p99, overall and per source player, and relay_late Δ;
  - snapshots applied, `lead_adj`, resyncs, moved births;
  - the server's TICK gaps and late events.
- **H7 Journal replay.** `sim_tool` replays the server journal in **every**
  arm, not only in the lifecycle steps.
- **H8 Verdict.** Per criterion: pass, fail, or masked.
- **H9 Host clock** (revision 3). A run in which every member's server-side
  slack falls below 0 together while the client journals keep 15.15 ms per
  tick is labelled `host clock`, with the fault's span (lp11_a9c: ticks
  245-760). It is not rerun. The span is judged by A9h's criteria, and the
  arm's own criteria are judged outside it and reported inside it.
- **H10 Twins** (revision 3). Every arm runs flags-off and flags-on, and the
  report compares them.
- **H11 Server clock skew** (revision 3). A test-only server option
  `physics.test_clock_skew = T:MS:PCT`, off by default, offsets the runner's
  scheduler clock: from tick T it runs PCT % fast for MS, then returns to the
  true clock at the same rate. It moves the deadlines only, not the
  client-to-client paths, as lp11_a9c's fault did.
- **H12 Session size** (revision 3). `--clients N` per arm (1 to 4).

Client flags (session_client.cpp):

- C6 `--hold-event TYPE:N:MS|drop`, which gates `send_event` and both repeat
  paths;
- C7 `--stall-ms T:MS`, a CPU stall at tick T;
- C8 `--resync-at T`, a resync request at tick T (revision 3: the baseline
  kept through a resync).

Counters to add in code:

- client: C1 (a `window:` line every 60 ticks), C2-C5 (`subst=`, `lead_adj=`,
  the rewind reason, the SUBSTITUTED journal flag);
- server: S1-S5, which are per-player substituted/runs/stale/resets/lead
  lines with the meter's samples and decisions, a relayed held-range line with
  source ticks, a lead/reset note, a step-without-ball line, and the event
  intake→send delay.

### 5.2 Arms and pass criteria

**Setup, every arm.**

- 4 clients unless the arm names another count (H12), 150 ms each way on all,
  snapshots effectively off (`run L 100000 100000 4.2`), 60 s, `--drive`,
  checkpoints every 2 ticks.
- The event is on client 2 (not the host) at T = 1500.
- Lane and swing variants are used as in C.

**Twins and validity (revision 3).**

- Every arm is run as a twin: both flags off (its **control**) and the flags
  under test. A0 off is run twice, and its spread is the noise band.
- A run H9 labels `host clock` is judged over the fault's span by A9h's
  criteria, and by its own arm's criteria outside it.
- The control twin does not always bound anything. lp11_a5u_off resynced all
  four clients, because its relay went past the reach. So wherever a
  criterion says "no higher than the control twin and the A0-off band's upper
  edge", both bounds are checked separately, and the band edge is the one
  that binds.

| arm | event |
|---|---|
| A0 / A0j | control; ±3 ticks of jitter on every link, whole run |
| A0h | heterogeneous links, whole run: clients 0-3 at 50 / 150 / 150 / 250 ms each way (revision 3: structural round-trip differences) |
| A1 / A2 | uplink blackout 300 / 600 ms |
| A3 / A3L | both-way blackout 200 / 1000 ms (A3L past the reach: the late client is reported only) |
| A3x | A3 at 50 ms each way, where the notices stay inside the reach |
| A3m | clients 1, 2 and 3 +300 ms uplink at 1500, persistent (revision 3: a late majority) |
| A4 | client 2 at 10 % loss, bursts of 8, whole run |
| A5 / A5u / A5s | +300 ms both ways / uplink only / +100 ms both ways, persistent |
| A5u2 / A5u3 / A5s2 | A5u with 2 and with 3 clients; A5s with 2 clients (revision 3: session size, the window between E and the acknowledgment) |
| A5uB | A5u, plus A9's spike on client 0 at tick 2100, after client 2's raise (revision 3: a burst after a raise) |
| A5d | +300 ms uplink at 1500, back at 2700; once more with C8 `--resync-at 2900` on client 2 |
| A5p | clients 2 and 3 together, +300 ms uplink, persistent (revision 3: a late pair) |
| A6 | `--hold-event physicalize:2:{300,1500,drop}` |
| A6m | hold 300 ms at 50 ms each way, barrier off: the move inside the reach |
| A6b | barrier off, 300 ms: hold a Physicalize and the following Unphysicalize together; and two same-type trafos (wood → stone → wood) in one hold |
| A7 | clients 2 and 3, uplink blackouts of 300 ms, overlapping by 100 ms |
| A8 | client 4 joins late at T = 1500 on a 250 ms each way link |
| A9 / A9c | a +300 ms uplink delay spike of 1 s (late, not lost) / a 500 ms CPU stall on client 2 |
| A9h8 / A9h20 | H11: the server's clock 7.5 % fast from tick 300 for 8 s / 20 s, then back (revision 3: lp11_a9c's fault, injected) |
| A10 / A10x | every client +300 ms / +450 ms uplink at 1500, persistent (revision 3: a shift shared by all) |
| A11 / A11u | one client alone: A1's blackout / A5u's uplink (revision 3: the absolute rule alone) |

**The other clients, every arm** (the unaffected members; in A3m, A5p, A7,
A9h, A10 and A10x there are fewer or none):

- own-ball error 0.0000 m in B, D and A;
- mechanism error within the control band in B, D and A;
- in D, rollbacks per class within the control band, and input rewinds that
  name the late ball no more often than in the control;
- relay lag, overall and per source player: p99 in D no higher than the
  arm's own control twin and the A0-off band's upper edge (one-sided, both
  checked), and above it only in the first two ticks of an episode (the
  quorum's M, or M2 in A5p);
- substituted ticks within the A0-off band + 0.5 % of their ticks (one-sided):
  the late member's episode never cuts them;
- relay_late Δ 0, lead_adj 0, resyncs 0;
- their mirror of the late ball from T_off+60 on ≤ 0.001 m;
- the server journal replays exactly.

**A0, A0j, A0h (flags on against their off twins):**

- substituted ticks in total ≤ the twin + 0.5 % of member-ticks. The replay
  gives 0.42-0.44 % at M = 4, with one 26-tick stall of client 2 in it;
- per member ≤ the twin + 1 %; every cut episode listed with its L and slack,
  so a stall of one client alone shows as that client's;
- would-raise 0 on all three rules, lead_adj 0 (nobody raised), moved births 0;
- S1: no common-dip episode in which three or four members are cut at once;
- A0h in addition: each member's baseline equals its control slack q50
  within one tick, so the 250 ms member's baseline sits about 13 ticks below
  the 50 ms member's, and nobody is cut more than in A0.

**Late client, bursts (A1, A2, A3x, A4, A7, A9):**

- the server's substituted ticks lie only inside [T_on, T_off + 2] (in A4,
  inside the loss bursts' own spans + 2);
- `subst recv` equals the server's count, and `subst late` is 0;
- own ball 0.0000 from T_off+48 on;
- no snap-class rollback and no RECEIVED checkpoint in [T_on, T_off+48]
  (otherwise the run is marked "masked" and rerun);
- would-raise 0, lead_adj 0.

**A3** (150 ms): the earliest notices arrive at relay lag + 13 = 45-57 ticks,
past the reach. So `subst recv + subst late` equals the server's count,
there is exactly one resync if `subst late > 0`, and the own ball is 0.0000
48 ticks after it.

**A9c:** nobody raised (would-raise 0, lead_adj 0). The stall's ticks are
substituted and corrected, or the client rebases and resyncs itself. Either
way, the others pass. A host-clock span H9 finds in the run is judged as A9h.

**A9h8:** nobody raised. The shared absolute majority lasts under 660 ticks,
as lp11_a9c's 515 did. Every member is substituted only inside the fault's
span + 2, and the late-client burst criteria hold for every member with that
span as [T_on, T_off].

**A9h20:**

- every member is raised exactly once, by the shared absolute rule, about 726
  ticks after the fault's start, with the k it prints (`ceil(q50(n))`, capped
  only by the total cap and the Sector bound);
- each is lowered exactly once after the fault, within cooldown + 660 + 66
  ticks of its end;
- no second raise, and no raise after a lower;
- until step 12, relay_late and resyncs are reported, not failed: the raise
  puts every member's relay lag past the reach.

**A11:** no raise; the client's substitutions lie only inside the blackout.
**A11u:** one raise by the absolute rule, about 132 + 66 ticks after 1500,
with k in [20, 24]. A member alone has no Sector bound.

**A5u and A5s, and A5u2, A5u3, A5s2:**

- The meter's decided k lies in [ceil(up_ms/15), +4] = [20, 24] for A5u and
  [7, 11] for A5s.
- The raises sent to client 2 sum to a k in that range, in at most two
  SessionLeads:
  - A raise decided while some sector of the level has not been entered is
    sent as far as the Sector bound allows. In the lane at 150 ms each way
    that is 14 of A5u's 20, decided at about 1686, before sector 4's stamp
    at 1713.
  - The rest goes at the next decision, after the cooldown.
  - In A5s the bound is about 7 (15 less client 2's +7 downlink), so A5s's
    7 goes at once.
- give-up is not reached;
- one buffer reset per raise, whose E (S3 note) satisfies E ≥ the first
  on-time frame after its slew;
- client 2's substitutions only in [T_on, E of the last raise];
- own ball 0.0000 from that E + 48 on, outside a resync's own 48 ticks;
- the others: resyncs 0. Client 2's relay_late and resyncs are reported, not
  failed, until step 12: at +20 its relay lag is 52-64 against a reach of 48;
- the S1 Sector lines of the three others:
  - their bound from each other stays at 14 (15 - 1) for the whole run;
  - client 2 as a sender reads about -5 until its first raise. It is
    reported as `sector-late` and binds nothing.
  - After the raises it reads `-5 + k`;
  - after the union is complete the line says `union=4/4` and no bound
    applies;
- the session's input delay printed once;
- the others: lead_adj 0, relay lag and substitutions as in "every arm".
- A5u2 and A5u3 in addition: after client 2's raise, the healthy members'
  S1 quorum reference is 0 ± 1. Their patience is their baseline + M, not
  the deadline.
- A5s2 in addition: nobody but client 2 is cut between E and the
  acknowledgment.

**A5uB:** client 0's 1 s spike at 2100 costs clients 1 and 3 at most M, at its
first tick only (relay lag above the band only there). Client 0 is cut only
inside the spike and is not raised, and client 2's criteria are A5u's.

**A5 (both ways)** is reported. At this base the late client's relay lag
exceeds even its deeper reach (§2.5), and its relay_late is the measured cost
to that player.

**A5d:**

- The raises are as in A5u.
- There is one lower, at least 660 ticks after 2700, by 19 ± 1 when the full
  20 was sent.
- The others are unaffected throughout.
- The variant with `--resync-at 2900` lowers the same, because the resync
  keeps the baseline and the offset (§2.8).

**A5p:**

- clients 2 and 3 are each raised once, by the relative rule: against the
  upper median of the others, each reads L ≈ 20;
- the two healthy members are delayed only at the episode's first tick, by at
  most M2;
- the healthy members' substitutions and relay lag are as in "every arm".

**A3m:**

- Clients 1, 2 and 3 are each raised once, by the minority rule, about 726
  ticks after 1500, with k in [20, 24]. By then the union is complete in the
  lane, so there is no bound.
- No member is raised twice.
- Client 0 is never raised and never cut.
- Client 0's relay lag carries about 20 extra ticks from 1500 until the raises'
  E. That is §2.6's accepted cost. It is back in the A0-off band after that
  E, and its resyncs are 0.

**A10:**

- Every member is raised exactly once, by the shared absolute rule, about 726
  ticks after 1500, with k in [20, 22]. The replay gives `q50(n)` = 19.8-20.0.
- Before the raise, 16-23 % of every member's frames are past the deadline in
  the replay. They are substituted and corrected, and this is reported.
- Nobody is cut by the quorum beyond the A0 criterion.
- Every pair's Sector margin is about -5 before the raises (reported as
  `sector-late`) and about 15 after.
- Until step 12, relay_late is reported.

**A10x:** as A10, with k in [30, 32]. Every frame is past the deadline until
the raise. After it, every relay lag is past the reach until step 12, so A10x
is reported, not passed, until then.

**A6:**

- At HEAD, expected: a TICK gap of about min(hold, 1000) ms, catch-up of 10
  per pass, the 465 log line, and the others' lag ≥ 60 with relay_late > 0.
- After the change, the largest TICK gap near the hold is ≤ control + 15 ms,
  there is no barrier line, and the others match the control.
- At 150 ms each way the owner's move is past its reach (§3.3). So the owner
  has exactly one resync, and `respawn_offset_any` +60 ≤ 0.001 m holds for
  every mirror.
- The drop variant: the owner matches the server (no ball, after the REJECTED
  or missing echo) within 48 ticks, and nobody stalls.

**A6m:** the owner is at 0.0000 m from T'+48, with no resync.

**A6b:** every mirror and the owner are ≤ 0.001 m from the server journal's
bodies, with the same body set.

**A8:** the joiner's lead is `D + ceil(rtt) + 2`, as printed, and its frames
arrive before the start members' (its baseline is above theirs). Its `subst
late` is 0. Nobody is raised, the joiner included: it is early, and it has no
offset to lower. The others' relay lag stays within the band while the
joiner's baseline is measured (it is waited for until the deadline meanwhile,
and arrives early).

## 6. Implementation plan

Each step is one implement-and-review workflow, and is closed by its
measurement. Flags `physics_patience` and `physics_lead_adjust` exist from
steps 9 and 10 and stay off in deployments until step R5 closes (§6.1).
Steps 7-11 are built; the rules their rows name for the meter and patience
(reference, `r01 - 1`, the statistics-less cap) are revision 2's and are
replaced by §6.1, which runs before step 12.

| # | step | files | tests | close |
|---|---|---|---|---|
| 1 | Harness | scripts/arena_map_test.py (H1-H8); sim/session_client.cpp (C6 `--hold-event`, C7 `--stall-ms`) | harness self-check: per-client link visible in per-client relay lag; hold and stall logged | an A1 dry run at HEAD produces a full event_report |
| 2 | HEAD baselines | none (runs) | none | A0 ×2, A0j, A1-A9c at HEAD recorded as the before numbers |
| 3 | Relay v3, both ends | timeline.hpp (newest-arrived substitute, source tick); input_relay.hpp; session_remote_input_msg.hpp; entity/session.hpp (protocol 7); session_runner.{hpp,cpp} (`applied_input`, solo `on_inputs`); server.cpp (header, held section, header-only datagrams, epoch/held_total); session_client.cpp and physics_session_client.cpp (parse v3, count the held section, no correction yet); docs/rooms-and-sessions-protocol.md | input_relay_test (held section span, placement, budget, inline vs reference); session_messages_test (round trip, v6 rejection); session_logic_test (substitute = newest arrival, stale included) | A1: the server's S2 count equals the held records sent; A0 others unchanged; server journal replays exact |
| 4 | Headless own correction | rollback.hpp (`recorded_own_entity`, `amend_recorded_input`); session_client.cpp (`sent_inputs_`, own branch, store-always, gate, rewind reason, SUBSTITUTED, epoch, completeness, `subst=`, `window:`, `acked`); journal.hpp; journal_trace.py | rollback_engine_test: an amended record drives the replay | late-client burst criteria in A1, A2, A3x, A4; the A3 resync assertion; A0 unchanged |
| 5 | Targeted snapshots | session_runner.{hpp,cpp}; server.cpp (`request_full_snapshot(session, player)`) | session_logic_test | A3L: the others' snapshots-applied Δ 0; the late client recovers |
| 6 | Retail part one | physics_session_client.cpp; physics_session.hpp; session_journal_client.cpp | session_messages_test (v3 parse) | a retail client in A1 logs `subst recv` equal to the server's count, no resync |
| 7 | Lateness meter, observe only | session/lateness.hpp (new); server.cpp (arrival time); session_runner.cpp (samples, S1 lines) | new lateness_test: synthetic series raise once for a 2.5 s shift; never for a 1 s spike, a 33-frame stall, the A4 loss pattern or a blackout; frozen reference; lower; cooldown | would-raise exactly once in A5u (K in range); zero in A0, A0j, A1, A2, A4, A9, A9c |
| 8 | Intake→send delay line | server.cpp (S5) | none | S5 recorded in A0 and A4 (input to step 17's gate) |
| 9 | Patience (flag) | timeline.hpp (`settled`); lateness.hpp (patience, waiver, statistics-less cap); session_runner.cpp (step test, wake-up) | session_logic_test: waiver on and off, a structurally last member never stuck, wake at patience | flag on: the others' criteria in A1, A2, A4, A7; the A0/A0j substitution criterion |
| 10 | SessionLead, server (flag) | new session_lead_msg.hpp; session_ready_msg.hpp (`caps`); timeline.hpp (`reset_hold`); session_runner.cpp (decision, E, gates); server.cpp (send, ack, `lead_offset`, `L_eff`, `set_player_rtt`); late_tick.hpp (`late_join_lead_ticks`; start base untouched) | runner test: a reset mid-run (constant held range, no deadline wait, journal replays exact); late_tick test: start base unchanged for 0..1000 ms; no decision without CAP_LEAD or while unacked | flag off: A0 identical to step 9; flag on with clients lacking CAP_LEAD: one logged decision and nothing sent |
| 11 | SessionLead, headless client | session_client.cpp (CAP_LEAD, amend [N, c], forced (c, E), 2x and 0.9x slew, rebase `33 + |slew|`, ack) | session_logic_test for the slew arithmetic | both flags on: A5u and A5s criteria; A9, A9c lead_adj 0; A0 unchanged |
| 12 | Per-client reach | rollback.hpp (runtime `max_resim`, history 128); session_client.cpp (relay-lag p99) | rollback_engine_test at reach 120 | A5: the late client's relay_late vs step 11; frame time p99 at reach 48 and 120 recorded (open question 1) |
| 13 | Late remote Sector: prove the facility | sim_tool.cpp; session_client.cpp (activation re-issue for a late remote stamp) | SimTool solo: a forced late remote stamp, replayed, gives mechanisms exact vs an on-time run | that SimTool comparison exact |
| 14 | Late remote Sector: use it | session_client.cpp (rewind to stamp-1, union re-stamped); session_runner.cpp (lift the Sector-margin cap on k) | none beyond step 13 | A5u with sectors 4: client 2's and the others' mechanism error ≤ control |
| 15 | Retail part two | fixed_tick.{hpp,cpp} (`slew`); physics_session_client.cpp (SessionLead, CAP_LEAD, starvation 264) | none (no retail unit target) | a retail client in A5u adjusts once, no resync |
| 16 | Re-stamp at acceptance, origin everywhere | session_runner.{hpp,cpp} (`applied`/origin in `pending_event`, superseded rule, `lifecycle_barrier` config); session_event_msg.hpp (`origin_tick`, state bits); lifecycle.hpp (key with origin); server.cpp (`last_physicalize` on origin); session_client.cpp, physics_session_client.cpp (key, order, guard; own echo ignored); sim_tool.cpp (order); journal.hpp v3 | session_lifecycle_test: the P+U one-tick case, same-type trafos, late birth after on-time death, overtaken birth, on-time out-of-order pair still applied | A6 300 (barrier on): no late-by-N divergence among the others; A6b (barrier off): every mirror ≤ 0.001 m vs the server journal; server journal replays exact |
| 17 | Relay at acceptance | session_runner.{hpp,cpp} (`on_event_accepted`); server.cpp (intake relay removed, leaver's Unphysicalize from the runner) | runner test for the leaver's tick | A0, A4: S5 p99 ≤ 2 ticks (otherwise the intake relay stays for on-time events); remote Sector lateness not above HEAD |
| 18 | Lifecycle budget and echoes | server.cpp (budget, BodyRevived dedupe first, REJECTED/SUPERSEDED to the owner) | intake unit test: budget, dedupe order | a spam arm (10 Physicalize in 8 ticks): the excess REJECTED, nobody stalls |
| 19 | Owner birth move (headless) | session_client.cpp (`own_life_`, moved/rejected/superseded, own `destroy_body`, remake at T'); sim_tool.cpp (last record per (player, origin, type)) | SimTool solo forced move | A6m: owner 0.0000 m from T'+48, no resync; A6 1500 (barrier off): exactly one owner resync; the client journal replays to the server's world |
| 20 | Barrier removal | session_runner.{hpp,cpp}; lifecycle.hpp (`physicalize_pending` and the config deleted, S4) | session_lifecycle_test updated | every A6 criterion; A0 unchanged |
| 21 | Uplink piggyback (option b) | session_input_msg.hpp v3; session_client.cpp, physics_session_client.cpp (send); server.cpp (intake order, repeats dropped) | session_messages_test (events in the input datagram, budget) | A4: 0 moved births from lost copies; datagrams under budget; A0 moved births 0; multi-tick own absence on retail measured |
| 22 | Owner death move (headless) | session_client.cpp | SimTool forced death move | an A6m variant holding an Unphysicalize: owner exact |
| 23 | Own Sector move (headless) | session_client.cpp (on step 13's facility) | SimTool forced own Sector move | an A6m variant holding a Sector event: the owner's mechanism error ≤ control |
| 24 | Retail part three | physics_session_client.cpp (origin echoes; moved/rejected/superseded → self-resync with snapshots off; REJECTED fast path) | none | A6 drop with a retail late client: no stall, the retail client converges |
| 25 | Exact resync (added by the step 20 review) | server.cpp, session_runner.{hpp,cpp} (the resynced member's full snapshot taken when the server steps B-1, to that member only, with the held notices for S..B-1); session_client.cpp (the snapshot as a rollback anchor at B-1, its own and the relayed frames replayed from B) | session_logic_test: the snapshot's tick and target | A6 1500: own ball 0.0000 48 ticks after the resync with snapshots off; the others untouched (needs step 5's per-player full snapshot, §3.6 #8) |

### 6.1 Revision 3: the follow-up to steps 7-11, in order

Steps 7-11 are built (0b5de5f, 53d8ecc, b134bcb, 177c337). Their meter,
patience and Sector samples are replaced by the steps below. Each step is one
implement-and-review pass, closed by its measurement. All of them come before
step 12, and each builds and runs on its own. Line numbers are 177c337's.
Both flags stay off in deployments until R5 closes.

**R0. Harness: twins, new arms, host clock, clock skew.**

- Files:
  - scripts/arena_map_test.py:
    - H9 labels the host clock with its span;
    - H10 runs every arm as a flags-off/flags-on twin;
    - H12 sets the client count per arm;
    - the arms A0h, A3m, A5u2, A5u3, A5s2, A5uB, A5p, A9h8, A9h20, A10, A10x,
      A11 and A11u are added (§5.2);
    - the S1 quorum, lateness and Sector lines of R1-R5 are parsed into the
      report.
  - server.cpp and session_runner.{hpp,cpp}: H11 `physics.test_clock_skew`,
    off by default, applied to the scheduler's clock only.
  - sim/session_client.cpp: C8 `--resync-at`.
- Tests:
  - the harness self-check: H9 labels lp11_a9c's span 245-760 and nothing in
    lp11_a0_off;
  - session_logic_test: the skewed clock moves deadlines only inside its
    span, and is the identity when off.
- Close: dry runs of A0h, A5p and A9h8 at HEAD produce full reports, and H9
  labels A9h8's span.

**R1. The arrival ring and the baseline, observe only.**

- lateness.hpp:
  - `arrival_ring`: 256 slots of {tick, at, seen, as_newest}, first arrival
    only;
  - `lateness_baseline`: the median of the first 264 newest-frame samples
    after the first assignment, floored at 3 and frozen, with
    `rebase(delta)` for a re-anchor's lead difference.
- session_runner.{hpp,cpp}: a ring and a baseline per member in
  `session_state` (session_runner.hpp:232-279).
  - The ring is written in `submit_input` before `buffer.submit` (:481), with
    `seen` taken in the posted command, and not for the lookahead garbage
    (:489-490).
  - It is created at :240 and :295 and erased at `remove_player` (:316-319).
    `player_ready` (:386-398) clears the ring, but clears the baseline only
    at the first assignment.
- Online, at each evaluation of the run loop's step test (:1069-1073), the
  runner computes what the quorum would decide on `seen`, with both timers
  and the clamp, and after each step what the waiver would have done. It acts
  on none of it.
- A new S1 quorum line every 660 ticks:
  - counted members;
  - the gap between the latest and the second-latest normalized arrival
    (p50/p95/p99/max);
  - would-cut counts per timer (M = 4, M2 = 10);
  - p50/p99 of `seen - at`, and of the wake-up against the patience it slept
    for;
  - per member, its baseline and the q50/q90 of n.
- An optional `physics.arrival_trace` prints one line per tick with every
  member's slack. It is off by default.
- Tests, lateness_test:
  - the ring: first arrival kept, `seen ≥ at`, stale frames recorded, wrap at
    256, `as_newest`;
  - the baseline: median, floor, frozen through an acked raise, kept through
    a resync and moved by `rebase`, measured only at the first assignment.
- Close: A0 off, A0h off and A0j off.
  - The live would-cut is ≤ 0.5 % of member-ticks in A0; the replay gives
    58-61 on `at`.
  - The gap's p99 is ≤ 2 ticks outside the deaths and respawns.
  - A0h's baselines equal the members' control slack q50 within one tick,
    and its would-cut is within A0's + 0.2 %.
  - `seen - at` is recorded.

**R2. The new meter beside revision 2's, observe only.**

- lateness.hpp: a `relative_meter` with three trailing windows (relative,
  minority, absolute at θ = 3).
  - The runner passes the shared flag per tick.
  - Persistence is 132 or 660 ticks, k is per §2.1, and the lower follows
    §2.1 with its slack bound.
  - Revision 2's meter stays and still drives every decision. Its reference
    code, and `patience_member_of` (lateness.hpp:621-633) that reads it, are
    untouched, so the tree builds and both flags behave as at 177c337.
- session_runner.cpp:
  - L and L' are computed per member in tick order, from the ring, after
    each step and at a cut frame's arrival (§2.1). They feed the new meter.
  - "Shared" is computed each tick from the members' absolute majorities.
  - The S1 lateness line gains the new meter's state: the baseline; q50/q90
    of L and L'; q10/q50 of slack; the three miss counts; and
    `would-raise2` with its rule and k.
- Tests, lateness_test, on a multi-member `run()` that feeds arrivals on a
  shared clock:
  - (a) a dip shared by four members (-15 ticks for 400 ticks, spread ≤ 2):
    no raise;
  - (b) the brake-edge pattern (three members 15 ticks late for 25 ticks
    every 500): no raise;
  - (c) one member +20 for good: one relative raise, k = 20;
  - (d) two of four +20 together: both raised by the relative rule;
  - (e) three of four +20: no relative raise; all three raised by the
    minority rule after 660 ticks;
  - (f) solo +20: one absolute raise after 132 ticks, k = 20;
  - (g) members on 50/150/250 ms from the start: no miss, no raise;
  - (h) two members, one a late joiner 12 ticks early: no raise for either;
  - (i) a raised member whose path recovers: its partner in a two-member
    session reads L ≈ 0, and it is lowered after 660 ticks, never below
    offset 0;
  - (j) three members, one raised and still late by 6: a healthy member late
    by M + 1 against its baseline is a relative miss;
  - (k) two members, one slewing between E and its acknowledgment: its
    partner's L stays at 0;
  - (l) a shared shift into the absolute band (all +20): nothing before 660
    ticks, then every member raised once, k = 20. The same shift for 515
    ticks raises nobody;
  - (m) after (l), the shift ends: each member lowered once. With the shift
    sustained, no lower;
  - (n) a resync keeps the baseline and the offset, and the lower still
    follows the return;
  - (o) a member whose path improved by 7 after its baseline: its partner
    reads L = 0.
- Close, flags off:
  - would-raise2 exactly once in A5u (k in [20, 24]) and in A11u;
  - once each for clients 2 and 3 in A5p, and for clients 1-3 in A3m
    (minority);
  - once each for every member in A10, A10x and A9h20 (shared);
  - zero in A0, A0h, A0j, A1, A2, A4, A9, A9c, A9h8 and A11.

**R3. Patience as the quorum, and the waiver (`physics.patience`).**

- lateness.hpp:
  - `patience_member` becomes {has, settled, held, waived, counted, baseline,
    ns when seen}. `patience_lead_ticks`, `patience_assign_leads`,
    `patience_member_of` and the statistics fields go (:587-633), so
    patience no longer reads revision 2's reference.
  - `patience_step_test(members, due, to_deadline, M, M2)` replaces
    :706-743. The patience-off branch (:709-722) stays exactly as it is.
  - Per member, the lead in ticks before the deadline is `A_p - R - M` (or
    `- M2`), clamped at 0, with R clamped at 0. The `to_deadline <= lead`
    structure and the pacing floor carry over.
  - `patience_waiver::sample` (:769-773) ends the waiver on a frame seen
    before `patience_p(T)` (both timers, p counted). Without a reference, it
    ends on a frame with slack ≥ 0.
- session_runner.cpp:
  - `patience_members` (:964-986) fills `ns` and `counted` from the ring and
    the baselines, on `seen`;
  - `patience_lead` (:988-996) goes;
  - the waiver call (:516-521) moves to R1's after-the-fact evaluation;
  - the run loop passes M and M2 (:1069-1073). The wake-up (:1074-1081,
    1111-1114) is unchanged, and so is the cut bookkeeping (:491-506,
    1084-1101);
  - `report_patience` and `log_patience_on` (:1007-1040) print M, M2,
    counted, the timer that cut, and the cut counts by timer.
- Tests, patience_test:
  - deleted: LeadIsR01LessTheMarginAndNeverPastTheDeadline,
    StatisticsLessMemberGetsTheLatestPatienceAmongTheOthers,
    MemberOfMapsTheMeterLikeTheRunner,
    WithoutStatisticsTheFirstMissWaitsForTheDeadline and
    LateAssignedMemberIsCappedAtTheOthersScheduleUntilItsReference;
  - rewritten for the quorum: StepTestWaitsUntilTheEarliestPatience,
    WaiverStartsAtASubstitutionAndEndsInsidePatience,
    StructurallyLastMemberIsNeverStuckSubstituted,
    BlackoutWaitsOnceAtPatienceWithTheWaiverAndEveryTickWithout,
    SoloBlackoutNeverStepsBeforePatience and
    SharedOutageStepsAtTheLatestPatience (both now step at the deadline);
  - kept: SettledIsArrivedOrBelowTheCursor,
    StepTestWithPatienceOffIsTheDeadlineRule,
    StepTestNeedsARealFrameToSkipAWaivedOrReanchoredMember and
    SteadyLinksAreNeverCutOff;
  - `run_sim` (~49-150) records arrivals in the ring, with a `seen` delay.
- New patience tests:
  - a shared dip cuts nobody and starts no waiver;
  - the brake-edge trio within 4 ticks of each other is never cut;
  - one member +20: cut from the first tick, and the others' step times
    unchanged to 1e-6 after the first tick;
  - a pair +12 of four: cut at the minority timer once, then waived;
  - a member with no baseline waits for the deadline;
  - a member structurally 11 ticks late (its baseline) is never cut;
  - the waiver ends on the first frame inside patience while its slack is
    low;
  - a waived member beside a member without a baseline ends its waiver at
    slack ≥ 0;
  - a member raised by 20 and still on its slow path (n ≈ 0): a burst at a
    healthy member costs the others at most M;
  - a uniform `seen` delay of one tick cuts nothing that `at` would not.
- Close, patience on and lead off:
  - A0, A0h and A0j: substitutions within the twin + 0.5 % in total and
    + 1 % per member.
  - A1, A2, A4, A7, A9 and A5uB's spike: the others' criteria, with the late
    client's cuts only inside its episode.
  - A3m: client 0 is never cut.
  - The S1 cut counts by timer and `seen - at` are recorded.

**R4. Lead decisions on the new meter (`physics.lead_adjust`).**

- lateness.hpp: the new meter takes over `on_tick` and the decisions.
  - Revision 2's reference goes: the `kReference*` constants,
    `kQualifyingMaxLate` and the deadline reference constants (:164-166,
    182, 191-192).
  - So do `window_`, `reference_samples_`, `close_window`, `set_reference`,
    `adopt_reference`, `adopt_deadline_reference`, `wants_fallback`,
    `wants_deadline_reference` and `reference_source` (:229-232, 380-401,
    406, 459-474, 486-494), and `lateness_median` and
    `lateness_settle_reference` (:89-94, 560-570).
  - The `measuring` and `wants_fallback` states become `baseline`.
- session_runner.cpp:
  - in `observe_lateness` (:798-), the fallback block (:810-820) and the
    reference line (:821-828) go;
  - `player_ready` (:386) keeps the baseline and the offset at a resync, and
    rebases the baseline by the lead difference;
  - server.cpp passes the assigned lead with the assignment.
- `apply_lead_decision` and `lead_decide` are unchanged. The Sector bound is
  still revision 2's until R5.
- Tests:
  - lateness_test: rewrite or delete the reference tests (`measured_meter`,
    ReferenceFromFourQualifyingWindows,
    ReferenceIsFrozenUntilTheNextAssignment,
    StructurallyLateMemberTakesTheOthersMedian,
    PersistentShiftRaisesExactlyOnce, TwoAndAHalfSecondShiftRaisesExactlyOnce,
    AcknowledgedRaiseRestoresAndALowerFollowsTheReturn,
    LowerNeverTakesTheOffsetBelowZero, NoLowerWithoutAnOffset,
    ResetKeepsOnlyTheOffset). The spike, burst, 33-frame stall, A4 loss,
    blackout and streak tests keep their meaning.
  - lead_test: `meter_rig` (~425-455) feeds arrivals instead of slack, which
    touches 493-614 and 837-911. The three `LatenessReference` tests (648,
    679, 692) go, and the solo test is rewritten as the absolute rule.
- Close, both flags on:
  - lead_adj 0 in A0, A0h, A0j, A1, A2, A4, A9, A9c, A9h8 and A11;
  - A5u and A5s raise client 2 once, sent `min(decided, revision 2's
    bound)`;
  - A11u raises once, by k in [20, 24];
  - A5d lowers once, in both variants.

**R5. The Sector bound per pair, lifted with the union (`physics.lead_adjust`).**

- lead.hpp:
  - `lead_sector_margin` keeps, per sender q, ten 66-tick bucket minima
    (660 ticks).
  - `lead_sector_path_sample(p, q, t_newest_q, S, now_tick)` is new, with
    `c_est` unchanged (:56-60, 120-124).
  - `lead_sector_sample` (:153-159) feeds the same per-pair window, from
    on-time events only, as built.
  - `shift` (:85-87) and its call (:291) go. An acknowledgment of p clears
    every pair with p on either side.
  - `lead_decide` (:210-212): the bound is the minimum of `min_pq - 1` over
    the senders with `min_pq ≥ 1`. With two or more members and no samples
    at all, the raise is deferred. Once the union is complete there is no
    bound.
- physics_world.{hpp,cpp}: `level_sector_count()`, from the level's
  `Sector_NN` groups at boot.
- session_runner:
  - the union is complete when every level sector is requested at a stamp
    ≤ N;
  - `create_session` takes S (`s.sector_lead`, server.cpp:835);
  - `submit_input` takes a path sample for every other member from each
    datagram (no filter);
  - `submit_event`'s sampling (:532-542) feeds the pair;
  - the S1 lead line prints the per-sender minima, `sector-late=` and
    `union=n/N`.
- session_client.cpp: the `window:` line prints each remote Sector event's
  actual margin at receipt (its stamp less the own tick), per sender (open
  question 9).
- Tests, lead_test:
  - `margin = S - up_q - down_p` on a synthetic two-path rig, independent of
    p's uplink;
  - a sender in catch-up lowers its pairs' samples;
  - a sender at or below 0 binds nothing and is reported;
  - a raised sender at 9 bounds another member's later raise at 8;
  - an acknowledgment clears the pairs;
  - with no samples the raise is deferred;
  - with the union complete there is no bound;
  - `LeadSector.AnEventLateAtTheServerIsNoSample` (960) is kept.
- Close, both flags on: the §5.2 matrix against the twins. The arms are A0,
  A0h, A0j, A1, A2, A3m, A4, A5u, A5u2, A5u3, A5uB, A5s, A5s2, A5d, A5p,
  A7, A9, A9c, A9h8, A9h20, A10, A10x, A11 and A11u.
  - A5u: 14 and then the rest, and the others' bound at 14 from each other.
  - Relay_late of a raised member is reported until step 12.

Then both flags may go on in deployments (open question 8).

After R5, step 12 gives the raised members the reach their lead needs (A5u,
A10, A9h20), and step 14 removes the bound on raises decided before a level's
last sector is entered.

## 7. Open questions

1. **The deeper per-client reach.** Can a late client afford a reach of up to
   120 ticks? Step 12 measures its CPU. If it cannot, the cap on `k` is that
   client's CPU budget, and the rest of its lateness stays substituted and
   corrected by part one. At 150 ms each way the relay lag already takes 32-44
   of 48, which is why A5 is reported and not passed. (Measured at step 12,
   §2.5 "As built (step 12)": a 120-deep rewind is a 51-62 ms frame on the
   x64 headless client, 0.45 ms per replayed tick, about 150 ms with the
   trafo inputs on; a 48-deep one 21-32 ms. The cap - 120, a lower one such
   as 72, or one tied to the trafo inputs - is open, the user's choice;
   retail x86 and the trafo-on cost are not measured.)
2. **Session-wide sizing.** Should `input_delay` and the start base be sized
   from a robust statistic rather than the worst ping, now that part two can
   lift one slow member on its own? The principles freeze them during a
   session, not how they are chosen.
3. **The quorum under loss.** Under loss, a frame's first arrival can be a
   later datagram's redundancy copy: one tick late per datagram lost before
   it. With M = 4, a burst of five or more lost datagrams cuts the frames it
   delays. That cut is the lossy member's alone, and the meter never sees it
   (no `as_newest` sample). A4 measures how many; the birth stamp lead (§3.4)
   is the fallback for its events.
4. **BodyRevived.** Does it still do anything under the sector union, or can
   it be retired?
5. **Retail input rewind.** Parts one and three on retail are only partial
   until the input-rewind card lands. Whether the retail scripts tolerate a
   multi-tick own absence is measured in step 21.
6. **A shift every member shares.** Revision 3 raises every member for it,
   after 660 ticks of a shared absolute majority (A10, A10x, A9h20), and
   nobody for a shorter one (lp11_a9c, A9h8). Re-anchoring the server's
   deadline schedule by the shared shift would restore every member's frames
   at once. It would send no SessionLead and force no range, and it would
   change neither `input_delay` nor any member's relative lead. Is that within
   the principles? A shared shift is not one player. Decide after A9h20 and
   A10.
7. **M as a constant.** M = 4 and M2 = 10 come from the lp11 replay, at 150 ms
   with uniform jitter compressed by the redundancy, on backdated arrivals.
   R1's live would-cut on `seen` re-measures them; revisit with deployment
   logs.
8. **The reach before step 12.** With the Sector bound lifted once the union
   is complete, A5u's full raise puts client 2's relay lag at 52-64, past the
   48 reach. A10, A10x and A9h20 do the same for every member. Their
   relay_late is reported, not failed, until step 12. Should the deployment
   gate after R5 wait for step 12, or should a raise be bounded by p's reach
   until then? A bound would leave p partly substituted instead of partly
   relay_late; both costs are p's alone. Step 14 now matters only for raises
   decided before a level's last sector is entered, so it stays after 12.
9. **Sector event lows.** Before any shift, some receivers' event samples sit
   2-6 ticks below the path model's 15: lp11_a9 reads 9, a5s 12 and 12, a5d
   13 and 12, and a4 13, at tick 1320. They are per receiver, not per sender:
   in lp11_a9 the other three receivers of the same crossings read 14, 14 and
   16. Two causes fit: the server's `c_est` overestimating that receiver (a
   ping spike, which is conservative), or a real late arrival. R5 keeps the
   event samples as a bound, and prints per pair the path minimum, the event
   minimum and the client's actual margin at receipt. If the actual margins
   sit at the model's, the event samples can go.

## 8. Review record

### 8.1 Revision 2

| finding | disposition |
|---|---|
| R1 B1: receivers key, sort and guard on `tick` | changed: origin in every key, order and guard, and in `last_physicalize` and the sim_tool order (§3.1 table); arm A6b |
| R1 S1, R2 overtaking: a late birth after a newer event | changed: superseded at acceptance when its applied tick would fall after a newer-origin event's; an on-time out-of-order pair is still applied (§3.1) |
| R1 S2, R2 horizon E | changed: `E = c_est + 2k + 2 ≈ N + rtt + k + 2`. R2's claim that (c, E) is empty is refuted: for the frames to be late, the lead is below the uplink trip, so `c - N = rtt - k`, not the normal 32-44. R1's "step the forced ticks at once" is refuted: it overshoots by k + 2 (§2.4). |
| R1 S3, R2 joiner reach | changed: start base untouched; uncapped first-join lead; resync at `L_eff_p`; reach from the measured relay lag (§2.5) |
| R1 S4: `held_total` across an assignment | changed: `epoch` in the header; counted only at or after the base |
| R1 S5: notice depth | changed: held section of 5 B references to the client's own sent frames (§1.1) |
| R1 S6: held frame lost without a body | changed: always stored in `own_inputs_` |
| R1 S7, R2 detector | changed: per-datagram newest-frame samples; a majority of misses for 132 consecutive ticks. R2's "decide 8 + jitter after the boundary" is not needed, since no sample waits for a loss classification. |
| R1 S8, R2 steady state | changed: patience on `r01 - 1`; A0j; moved births 0 in A0 and A0j; birth stamp lead considered and not adopted |
| R2 patience costs the others | changed: the streak waiver, ending at the first in-patience sample rather than at a fresh frame (§2.6); one-sided criteria |
| R2 reference drift | changed in part: the reference is frozen, and members without statistics get the others' schedule. The hard cap for members with statistics is refuted (§2.6). |
| R2 substitute content | changed: newest arrival, stale included, frozen by `reset_hold` |
| R2 runner wake-up | changed: sleep until the earliest patience |
| R2 client never applies / capped k | changed: `CAP_LEAD`, unacked gate, at most two unrestored raises |
| R2 infeasible criteria | changed: A3 resync assertion and A3x; the move closes on A6m at 50 ms with the barrier off |
| R1 N1 | changed: S5 line; step 17 gated on it |
| plan (both) | changed: step 3 covers both clients' parse; patience and SessionLead behind flags; step 1 split from the baselines; the re-stamp in three steps (16-18); reach its own step (12); the Sector facility proven before use (13-14); journal ordering in step 19; arms A0j, A6b, A6m, A7, A8, A9, A9c |
| R2 nits | changed: `recorded_input` reused; a lifecycle budget instead of an exemption; 0.9x lowering; SessionLead ticks counted as learned |

### 8.2 Revision 3

| proposal or finding | disposition |
|---|---|
| (1) patience relative to the others, a quorum timer restarted on every arrival, M 2-3 | changed: the timer starts only for the last missing member (latest of the others + M), plus a minority timer (m-th arrival + M2) for four or more members. Started on any first arrival, the timer cut 7.5 % of lp11_a0_off at M = 3, because the brake-edge trio arrives 10+ ticks after client 0. M = 4 (0.42 % against 0.61 % at M = 3); M2 = 10. A measured spread is not adopted (it sits at its floor out of the dips). |
| (2) the meter on relative lateness, no statistics | changed: relative lateness against the upper median of the others, plus one frozen median baseline per member. Without it, structural round-trip differences (one start base: slack = base + D - rtt) and early late joiners read as lateness. The baseline is a median and cancels in L, so the shared-dip failure does not return. The absolute rule (slack < 0) is kept for a shift every member shares (lp11_a9c). |
| (3) the waiver ends within M of the others | adopted: it ends on `L_p ≤ M` |
| (4) the Sector-margin cap | the 14 of A5u re-derived as the true limit (`S - up_q - down_p = 15`; an uplink delay cancels in `c_est`). The samples are replaced: other members' events bound p by the sender's timing (the -8/-9 from client 2's late event, the margin 1 at tick 1320). They become path samples from on-time members' datagrams (R4). Step 14 lifts the cap, and the residual until then is p's alone. |
| A: no dip is a sector activation; the three-of-four dips are the brake edges | adopted in the text (part two's opening) |
| A: lp11_a9c's shared lateness is a host clock fault | adopted: H9 marks such a run masked; open question 6 for the remedy |
| B: `t_q` = lower median, M from each member's own jitter | not adopted: the lower median doubles the steady-state cuts (0.76 % against 0.42 % at M = 4); the baseline makes a member's own spread a matter of M alone |
| B: skip a Sector sample from a late sender | adopted in a stronger form: path samples only from on-time senders, no event samples |

### 8.3 Revision 3, review

| finding | disposition |
|---|---|
| R1 B1, R2 1: the reference `g = n + o` moves every member's reference by one member's raise; after A5u's raise the healthy members' patience is the deadline and their own lateness goes unseen | changed: the reference is `r = max(0, n)` (§2.1), with U, R and R' over it; arms A5u2, A5u3, A5uB and tests (j), (k) in R2, and the burst-after-raise test in R3 |
| R1 S1: absolute raises and relative lowers oscillate (A10x, a server-clock skew) | changed: with `r` the shared shift reads `L ≈ 0`, and a lower is bounded by `floor(q10(slack)) - 3` and never taken under an absolute majority (§2.7) |
| R1 S2: a member whose path improved makes its partner late | changed: the clamp (test (o)) |
| R1 S3, R2 7: "the j-th smallest is final once j are in" is false with differing baselines, and the "newer than the last sample" test then drops late-reference ticks | changed: L and L' are computed in tick order once T is stepped and p's frame is in, with missing members at their lower bound (§2.1) |
| R1 S4, R2 8: the waiver never ends without a reference | changed: without a counted reference it ends on slack ≥ 0 (§2.6) |
| R1 S5: the Sector cap protects only a sector's first crossing | changed: no bound once every level sector is requested at a stamp ≤ N (§2.9). A5u gets its remaining 6 at the next decision, and open question 8 is reworded |
| R1 S6: there is no "margin 1 at tick 1320"; event lows of 9-13 exist; the on-time filter drops a capped sender | changed: the claim is corrected. The 1 at 1980 is client 2's own margin after its 14, and the 1/3/5 at 3300 in lp11_a0, a0j, a4 and a9 are margins after raises. Event samples stay as a bound per pair; the filter goes; a sender binds while its margin is ≥ 1 (§2.9, open question 9) |
| R1 S7: a late majority is held every tick, not once | changed: the minority rule (`L'` against the least late other, clamped, 660 ticks) raises it, and the wait until then is stated as an accepted cost (§2.6); arm A3m. In the replays the longest `L'` majority is 93 ticks against 660 |
| R1 S8: A10's "slack about 2" is false; the trigger `q50 < 0` and the target `q10 = 3` disagree | changed: the absolute miss is slack < 3, and the raise is `ceil(q50(n))`, so the trigger and the target are one quantity. In the replay it gives 19.8-20.0 where the q10 target read 1-12. A10's criteria are rewritten |
| R1 S9, R2 5: masking hides the host-clock case; no solo or 2- or 3-member arms | changed: H9 labels and judges instead of masking; H11 injects the skew (A9h8, A9h20); shared absolute majorities wait 660 ticks, so lp11_a9c's 515-tick fault raises nobody; arms A11, A11u, A5u2, A5u3 and A5s2 |
| R1 N1: waiver hysteresis | refuted: in the replay, ending at M - 1 or M - 2 does not reduce the episodes (13/13/12 on lp11_a0_off, 22/24/26 on a0j) and adds the member's own cuts (61 → 65 → 67, 95 → 108 → 117) |
| R2 2: a resync re-measures the baseline and resets `o` | changed: a resync keeps the baseline and the offset, and the baseline moves only by a lead the re-anchor did not restore (§2.8); the lower is gated on `lead_offset`; the A5d variant with C8 `--resync-at`. (Corrected at step RL1, 2026-10-27: a lead over the current tick does not restore where p's frames fell - the server's pace moves C against the fixed deadlines - and the code re-measures the reference at every assignment; the re-anchor now takes its lead on the fixed schedule, `tick_at(now, lead)`, so p's frames get the start's budget again: resync_snapoff 6.05 -> 22.15, resync_at150 26.77 -> 22.3; §2.8, "As built (step RL1)".) |
| R2 3: arrivals are backdated, and the step test sees frames later | changed: the ring keeps `seen`; patience runs on `seen`; R1 measures the would-cut live; the S1 line prints `seen - at` and the wake-up's lateness |
| R2 4: path samples with an on-time filter, pooled, shifted on the ack, absent at the start | changed: per pair, from every datagram, a minimum over 660 ticks, cleared on an ack, deferring with no samples (§2.9, R5) |
| R2 6: R2 deleted the reference while step 9's patience still read it | changed: R2 is observe-only beside revision 2's meter; R3 removes patience's use of the reference; R4 deletes it. Each step builds on its own |
| R2 control-twin gap | changed: both bounds checked separately; the A0-off band edge binds (§5.2) |
| (own) revision 3's waiver end on `L ≤ M` was not the patience the replay used | changed: the waiver ends on a frame inside the step test's patience. On lp11_a0_off the `L ≤ M` end cuts 81 (0.58 %, past the 0.5 % criterion) against 61 |
| (own) the full raise after the union completes puts A5u's client 2 past the 48 reach before step 12 | stated: reported, not failed, until step 12; open question 8 |

Scripts for this record: `rv4_check.py`, `rv4_hyst.py`, `rv4_ep.py`,
`rv4_w.py`, `rv4_a10.py`, `rv4_a10b.py` and `rv4_a10c.py`, in the session's
scratchpad.
