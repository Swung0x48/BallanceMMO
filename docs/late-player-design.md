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
(server.cpp:551-552, 1157-1176). (FULL steps SAX / SAXE: the SessionAssign
itself now sends sixteen copies, one per server main-loop pass,
`sim/assign_copies.hpp`; the SessionLead keeps three 40 ms apart as its own
`kLeadRepeats` / `kLeadRepeatSpacing`.)

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

**As built (step R12, the user's two decisions of 2026-10-27).** The margin
is 8 + the server's wait, the wait is on the wire, and the cap is estimated
at run time from the client's own rewind cost:
`reach = clamp(p99 + 8 + w, 48, cap)`, `cap = clamp(floor((B - restore_ms) /
replay_ms_per_tick), 48, 120)`, B = 3 ticks (`kReachHitchBudgetMs` =
3000 / 66 ms, the user's "3 帧，约 45 ms").
- **w, defined from the runner's step test** (`lateness.hpp`
  `server_wait_ticks`): a member's wait at a step is how much longer the
  test would have held that tick for its missing frame - patience off,
  `deadline(T) - the instant the runner stepped T` in ticks (the step's
  earliness; what REACH called the session's smallest slack, since a step
  with every frame in happens at the latest arrival); patience on,
  `to_deadline - lead` of that member, 0 while it is waived and another
  member's frame paces the tick; never negative (a step at or after the
  deadline - a substituted tick, a catch-up - waited for nothing). `run()`
  computes it per member from the same `patience_members` view the test
  just passed (`session_state::step_waits`); `step()` feeds each member's
  `server_wait_meter` and puts its value on `applied_input::wait`.
- **On the wire** (protocol 13, `entity/session.hpp`): the relay header
  ends with `wait u8` after `epoch` (`session_remote_input_msg`; the header
  is 26 bytes, `kRelayDatagramHeader`), the recipient's own: the largest
  wait of its last 4 windows of 60 server ticks, rounded up, at most 255.
  The largest, not a percentile: a notice's own datagram (stepped at the
  deadline) carries a wait of 0 for its tick, so the value must come from
  the steps around it; the 4 windows match the client's p99 span.
- **Journal v7** (`journal.hpp`): tag 9 WAIT `u32 tick, u32 player, u8
  wait`, written by the server when a member's header value changes and by
  the core when the newest received header's value changes; read into
  `journal_tick::waits`, `journal_trace.py` reads it, `e0_compare.py
  --live` leaves it out (the server's step timing, like a RECEIVED
  checkpoint's place). Nothing replays it.
- **The core** (both hosts, one copy): `note_held_section` takes the
  newest header's wait (any epoch) before the lag sample; `sample_reach`
  asks `reach_meter::reach(wait, cap)`; the `reach:` line names the p99,
  the margin, the wait, the cap and its averages; the status line's
  `reach=now/deepest/history` is followed by `cap=cap/restore ms/replay ms
  per tick/rewinds wait=w`. The estimator is `rollback.hpp`
  `rewind_cost_meter`, fed by `restore_and_replay`'s timer (every rewind
  that replayed a tick: input, own, lifecycle, snapshot, forced, a
  resync's re-step): the restore an exponential average (weight 1/16), the
  replay per tick as decayed sums of ms and of ticks (a 2-tick rewind
  counts less than a 48-tick one). **Before the first sample the cap is
  48**: without a measurement no deeper claim than every client made
  before step 12 (and a client that never rewinds needs no deeper reach);
  the first rewind, of any depth, is a sample. A session starts its meter
  and its wait over (its level's world is what its rewinds cost). The
  history becomes 128 when the reach passes 48, as at step 12. The
  headless `--reach N` still pins the reach (cap and wait ignored).

Verified (quick tier, 16.5 minutes of wall time with the reference
re-capture, about 1.5 over the 15): x64 BUILD-OK, x86 CLIENT-BUILD-OK (the
client target's 29 objects cleaned first: struct layouts changed), Arch
ARCH-BUILD-OK, GCC syntax 29/29, `BallanceMMOMessageTests` 548/548 (new:
`ServerWait.TheStepTestsHoldForAMissingFrame`,
`ServerWait.TheHeaderCarriesTheLargestOfFourWindows`,
`RollbackEngine.ReachAddsTheServerWaitUpToTheCap`,
`RollbackEngine.RewindCostCapFromTheClientsOwnRewinds` - a slower machine's
samples give 69 against 92, the trafo-on cost 48 -,
`SessionJournal.V7WaitRecord`,
`SessionClientCoreRelay.TheServerWaitWidensTheReachUpToTheCap`; and every
rewind of the 120-deep lockstep test is a cost sample). The stall recipe
(REACH's `reach_stall.sh` on rR12, 4 clients, 150 ms, snapshots off, client
0 `--stall-ms 600:450`, `maps/arena-test/r12_stall`): the server's journal
has each member's wait 22 at 27, 23 from 118 to the end of play (0 once the
clients left); every client read 22 then 23; the reach was 52-53 (p99 21-22
+ 8 + 23) and 73 for four windows after the stall's burst (p99 42), where
step 12 gave 48 and 50; client 0's 8 substituted ticks were all taken
ahead (subst 8/0/8), no notice past the reach, no resync, no rebase; caps
105-120 (restore 0.4-1.0 ms, 0.23-0.42 ms a tick); the server journal
replays 3528/3528. The retail two-copy run on L11 (`work/R12/r12_retail.py`,
the core on by default, placement on, snapshots off, `net lag 150` both
ways on both, the key record, client 1 `session force-rollback 30:48`,
`scripts/retail/r12_retail`): both clients assigned in 11.0 s, wait 22 on
the wire (21 at 28, 22 from 38; 23..31 in the last ticks after `net lag 0`,
the slack growing by the lag), reach 52-53 with history 128 from tick 91,
**the estimator's cap on retail x86 85-94** (client 1, 167 rewinds incl.
143 forced 48 deep: 94, restore 0.64 ms, 0.476 ms a replayed tick; client
2, 39 natural rewinds: 90, 0.64 ms, 0.493 ms), no resync, 15/15
mechanisms; **the retail x86 memory at history 128**: Player.exe private
bytes 150.3 / 152.2 MB before the session, 186.6-192.5 MB during it (peak
working set 193.5 MB), flat over the 60 s - the whole session state,
history included, is under 43 MB, so the 32 extra records are at most about
11 MB of a 32-bit process; the server journal replays 4353/4353 and both
client journals read as v7 with their WAIT records. The protocol bump makes
every `core_ref_s6` capture refuse: the set is re-captured as
`maps/arena-test/core_ref_s7` (its README; the same five arms and recipes;
`wire_check.sh` defaults to it) and `wire_check.sh - rR12 rR12w` is ALL SAME
(12/12 sends, 12/12 journals IDENTICAL); `solo1` replayed BYTE-IDENTICAL,
`solo1` vs `solo2` `--live` IDENTICAL.

Found: **the estimator over-states the per-tick cost of shallow rewinds
that make bodies.** In `core_ref_s7/l8_cross` (0 ms) every client's 7-9
rewinds were lifecycle rewinds whose replay stage is mostly births'
bookkeeping: 2.6-3.3 ms "a tick", cap 48 (one client 70). It changed
nothing there (p99 + 8 + 13 is under 48) and deeper rewinds outweigh them
(the replay is weighted by ticks), but a session whose first rewinds are a
few shallow births keeps a low cap until deeper ones come. Not changed
here: excluding the births' share from the replay stage is a design choice
(the cost is real, it is the per-tick model that does not fit it); queued
under roadmap §4 item 2 with the step's deferred checks.

Review round (the step's review, two should-fix findings, both fixed):
- **The cap's samples are the flow's clock reads.** The cap decides
  `max_resim_ticks` (a rewind past it is refused, a held notice past the
  reach resyncs), so a cap measured with the replay's own clock made a
  capture replay exact only while the cap did not bind (the replays' caps
  differed from the live ones by a few ticks). Per the core design's 5.2
  (5) rule - every wall-clock read the flow makes goes through `now(site)` -
  `restore_and_replay`'s timer now reads three new sites for the meter
  (`clock_site.hpp` `rewind_start`, `rewind_replay`, `rewind_end`; the
  engine's `set_cost_clock`, which the core sets to the platform's `now`
  at every session start; the stats' `t_rest` / `t_rep` keep their own
  reads, so a replay still measures its own cost): the headless client
  captures them and a capture replay takes the captured ones; retail reads
  its clock. Nothing is journalled for it (the capture carries it). The
  three reads are part of the call order the core tests pin (eight
  `session_client_core_test` lists gained them). The captures change, so
  `core_ref_s7` was captured again with the final binaries (rR12b; the
  first R12 set, which the final binaries cannot replay, was set aside);
  every replay's cap, reach and `far` equal the live ones (12/12 status
  lines, every `reach:` line identical), `wire_check.sh - rR12b rR12bw`
  ALL SAME, `solo1` BYTE-IDENTICAL (5954 records), `solo1` vs `solo2`
  `--live` IDENTICAL.
- **`SimTool --replay-session --list` reads the WAIT record** (it stopped
  at the first one: "record 64 (tag 9) does not parse"): `r12_stall`'s
  client 0 and server journals list in full (dropped 0; 2 and 16 WAIT
  lines). A client's WAIT record is filed under the header's server tick
  (`journal.hpp` says so).
- **The cap before the first sample is 48, and its consequence**: the
  user's "even a first, unwarned stall is covered" holds once the client
  has rewound once. A client that has not rewound yet (a solo player, the
  first seconds of a session) keeps the reach at 48 whatever p99 + 8 + w
  asks; at about 200 ms (h about 28, w 23) its first stall's notice would be
  h + w + 1 = 52 deep and resync. Not changed: the pre-sample value was
  delegated to this step ("state which and why") and 48 is what every
  client claimed before step 12; a different starting value (for example
  the x64 measurement's 90) is the user's to choose.
- **The user's answers to R12's two queued points (2026-10-27).** The
  cap before a session's first rewind: **"保持 48 (Recommended)"** - it
  stays 48 (the floor) until the first rewind is measured. The births'
  share of the estimator: **"拆成固定开销 (Recommended)"** - built at step
  B5RC as R12B and not committed: its premise was measured wrong (below).

**As built (step B5RX, item R12D): R12B dropped; the headless-only savestate
diff's time left out of the rewind cost meter** (the user, 2026-10-28,
answering B5RC's stop: "按推荐继续" - option (b)). No CK2, physics_RT or IVP
change; no protocol, journal or capture-format change.
- **Why R12B is not built.** B5RC built it (`.local-tools/work/B5RC/r12b.patch`
  stays a patch) and found the premise wrong (its diagnostic,
  `work/B5RC/diag`, `cap_variants.py`): a birth tick at `l8_cross` costs
  0.45-0.55 ms against 0.39-0.44 ms for an ordinary replayed tick - the
  births are cheap. The 7.7-10.9 ms ticks that held the cap at 48 are a
  rewind's LAST replayed tick, where the engine's end-of-replay savestate
  check (`rollback.hpp`, the replay loop at `t == current_tick`) calls
  `world.diff_savestate`, which renders both blobs as text: 7.2-10.3 ms a
  call, at most 8 a session (`savestate_replay_logs_`). Only the headless
  client installs that hook (`session_client.cpp`, "Diagnostic (headless
  only: the mod has no blob diff)"; the engine keeps the live blob aside
  only when the hook is set), so a retail client never pays it and the
  headless meter was pessimistic against retail. The committed R12
  estimator stays.
- **As built** (`rollback.hpp`, `restore_and_replay`): the rewind timer
  gained `cost_ends()`, the `rewind_end` read taken once; the last replayed
  tick calls it right after its capture, so the meter's span is the restore
  and the replay. The scripts' end-of-replay check (both hosts run it) is
  made before that read and logged after the savestate line as before; the
  savestate check (the headless text diff) runs after it. A rewind that
  never reaches its last tick (frozen clock, too far, a failed step) still
  reads `rewind_end` in the timer's destructor. The same three cost-clock
  reads in the same order and no new site, so every existing capture
  replays strictly (below); the log lines are unchanged in text and order.
  What leaves the meter besides the diff: the record's store, the
  navigation re-poll, the rewind's log line and its correction record - a
  few microseconds, on both hosts alike. The stats' own `t_rest` / `t_rep`
  are unchanged (they still include the diff). Unit test
  `RollbackEngine.TheSavestateDiffIsLeftOutOfTheRewindCost` (a fake cost
  clock, 1 ms a step and 10 ms a diff: 1.0 ms a tick, the reads
  `rewind_start, rewind_replay, rewind_end`, the savestate line before the
  rewind's line; the pre-R12D order would give 3.5 ms a tick).
- **Harness tooling landed with it** (`inbound_capture.hpp`,
  `session_client.cpp`): B5RC's `--replay-free-cost-clock` without R12B's
  `rewind_tick` site - in a `--replay-inbound` replay the three rewind-cost
  sites read the replay's own clock and the capture's reads at those sites
  are passed over (counted in the replay's last line), so a build whose
  rewinds differ from the capture's (RC3's paired replays) replays to the
  end and the cap is the machine's own. Off by default: a strict replay is
  unchanged (`free_` 0: `next_pos()` is `pos_`). The review's nit landed:
  `static_assert(clock_site::count_ <= 64)` on the free-site mask.
- **Checked.** Strict: `core_ref_s7`'s three multi-client arms replayed by
  rB5RX with `--body-set-corrections` (the pre-RC3 control; RC3's default
  changes those arms by design, item RC3A of the rewind-cost design) against
  rB2SX's replays (`rB5RXw_old`): 12/12 sends SAME, 12/12 journals
  IDENTICAL (`rB5RX_ctl`); `solo1` / `solo2` against their live journals:
  SAME / IDENTICAL. The new cap (`l8_cross`, the same control arm with
  `--replay-free-cost-clock`, the replay's own clock, four replays at once,
  `rB5RX_capctl`; sends and journals still IDENTICAL to rB2SX's, the cap not
  binding): **73 / 92 / 71 / 70** (0.61 / 0.49 / 0.63 / 0.63 ms a tick,
  11 / 5 / 11 / 10 rewinds), against the captured R12 caps 48 / 82 / 48 /
  48 (3.31 / 0.54 / 2.68 / 2.76 ms a tick) and B5RC's "R12 without the
  diff" estimate 48 / 87 / 69 / 67 (its c0 48 came from a 9 ms stall outside
  any tick, absent from this replay). The savestate check still ran (3 / 0 /
  3 / 2 lines, as in rB2SX's replays).
- **The trade-off the user accepted** (B5RC's option (b)): the 7-10 ms
  hitch each of a headless session's first 8 such rewinds really costs is
  not budgeted by the headless cap; retail never pays it.

**As built (step LBR, FULL close-out, 2026-11-01): the reach on the reference
machine after the realtime cuts; open question 8 closed by measurement.** No
code change: no build, no protocol, journal or capture-format change. Measured
on HEAD ebad5f80, in which SRT3 (one rewind per frame) and SRT4 (sparse capture,
k = 8) are committed (realtime-design "As built (step SRT3)" / "(step SRT4)").
The x64 binary is `build-agent-rRTD` (byte-equal to `rSAXC`, the last code
commit; RTD changed docs only). The x86 build is SAXC's, as RTD installed it in
both copies (cmp-equal to the x86 out dir). The Arch server is RTD's build of
SAXC's tree (`ballanced-36c04e3d24c9+bmmo-6916f406899b`, the same code as
HEAD). Every performance figure follows decision 33: each client is pinned to
E-cores of its own (16-18, 19-21, 22-24, 25-27; retail 16-21 / 22-27), the
server runs in WSL and snapshots are off.

- **The audit: sparse records against the depth gates.**
  - *What the engine does.* `rewind_to` (`rollback.hpp`, `rewind_to`)
    compares the REQUESTED anchor's depth with the limit (`max_resim_ticks`,
    or the caller's `reach`). It then restores from `restore_base`, which is
    the anchor or the newest whole record before it, at most
    `kSparseCaptureMax - 1` = 7 ticks deeper. It replays from there.
  - *The cost meter already counts the restored record's ticks.* Its tick
    count is the `stats.resim_ticks` delta (`rewind_timer`; incremented once
    per replayed tick), and that delta includes the rebased extra ticks. Its ms
    per tick is therefore per tick actually replayed.
  - *Every core gate compares the requested anchor*:
    - the input and own replay (`client_core_frame.cpp`
      `replay_corrected_inputs`, `own_replay_pacer::due`);
    - `force_rewind`;
    - `sector_rewind_reachable`;
    - the held notice (`client_core_held.cpp`, `window_rewind_reach`);
    - the late relayed frame (`client_core_messages.cpp`);
    - the trafo input (`client_core_trafo.cpp`);
    - the lifecycle gates (`client_core_lifecycle.cpp`: `joins_life_batch`,
      `move_own_birth`, the forced own move).
  - **The requested anchor is the right depth, so nothing is built.**
    1. *The reach is a promise about the age of a correction, not about the
       restore.* Its floor is the server's Sector bound: `lead.hpp`
       `kLeadSectorReachTicks == kReachFloorTicks` (`lead_test.cpp`
       static_assert), so every client takes a Sector stamp up to 48 deep, and
       the server's raise arithmetic (`lead_sector_bound`) counts on that. A
       gate on the restored depth would refuse a stamp 42-48 deep whenever its
       anchor is a light record (7 of every 8 replayed ticks) and would resync.
       The need it covers, `p99 + 8 + w`, is also measured in requested depth.
    2. *The restored record is always in the history.* Two kinds of gate check
       for it:
       - The gates that can reach past the standard reach ask
         `rewindable()`, which looks for the restored record itself.
       - The lifecycle gates ask `knows_tick` within the standard reach. Their
         base is at most 48 + 7 = 55 ticks deep while the history is 96, and
         at most 120 + 7 = 127 once it is 128 (`kSparseCaptureMax =
         kReachHistoryTicks - kReachCapTicks`).
    3. *The rebased replay is exact.* SRT4's verify mode found 0 mismatches
       (403 + 224 + 154 + 12 anchors), and its wire check had 45/45 journals
       IDENTICAL. Gating on the restored depth would only refuse corrections
       that dense capture takes, so sparse capture would change behaviour.
  - *The one consequence (stated, not changed).* The cap formula reserves
    nothing for the extra ticks. A rewind requested at depth = cap replays up
    to cap + 7 ticks, so it can exceed B by up to 7 x the replay ms per tick:
    3.1 ms on headless arena E-cores (0.44 ms) and 3.4-4.0 ms on retail L11
    (0.49-0.57 ms), about 7-9 % of 45.45 ms. This decides no correction: whether
    a correction is taken depends on the requested depth alone. Every host and
    level below has the reach (53-55) 20-67 ticks under its cap (the verdict
    table), except headless L11 (cap 54-64) and headless L12 (the floor).
- **(1) Offline: the E-core captures replayed by HEAD.** Tool: `work/LBR/e_run.sh`,
  which is SRT4's `e_run.sh` with `core_ref_s14`'s pins, four clients at once,
  each on E-cores of its own, `--replay-free-cost-clock`. The census is
  `work/LBR/census.txt`. Every replay ran to its end (rc 0) and all 28
  journals are IDENTICAL to the live ones. Each row gives the cap as
  `cap (replay ms per tick)` per client c0-c3. "Before" is the status line of
  the binary that captured the arm (SRT's census build for `srt_E_*`, SRTJ's
  for `srtj_E_*`, CMAP's unpinned live run for `a5u_lead`):

    | arm | before | after (HEAD, E-cores) | restore after |
    |---|---|---|---|
    | `srt_E_l12x_on2` (L12, trafo on) | 48 x4 (1.71-1.82) | 48 x4 (0.93-0.99) | 2.20-2.30 ms |
    | `srtj_E_l12x_on2` | 48 x4 (1.06-1.11) | 48 x4 (0.93-0.99) | 2.22-2.31 ms |
    | `srtj_E_l12x_h` (human keys) | 48 x4 (1.01-1.06) | 48 x4 (0.99-1.02) | 2.80-3.07 ms |
    | `srtj_E_l11x` | 55 / 57 / 54 / 57 (0.74-0.79) | 58 / 56 / 54 / 59 (0.73-0.79) | 1.79-2.09 ms |
    | `srtj_E_l11x_h` | 62 / 60 / 57 / 63 (0.68-0.74) | 64 / 60 / 57 / 63 (0.67-0.75) | 2.33-2.50 ms |
    | `srt_E_l8x_on` | 48 x4 (0.98-1.04) | 76 / 77 / 80 / 77 (0.55-0.57) | 1.26-1.50 ms |
    | `core_ref_s14/a5u_lead` (arena) | 120 x4 (0.25-0.26, unpinned) | 102 / 101 / 102 / 104 (0.42-0.43) | 1.60-1.98 ms |

  Not replayed: the arena arms `srt_E_h1_on`, `srtj_E_h1_on` and
  `srtj_E_h1_h`. They pin the arena as `Level_13.NMO`, and since CMAP the
  slot holds the original again, so `core_ref_s14/a5u_lead` stands in for
  them. Rebased rewinds: from 1 (+1 tick) up to 85 (+325) per client, and 0
  unanchored.

  Reading: SRT3 / SRT4 lifted L8 off the floor and leave L11 at 54-64. The
  headless L12 trafo-on pile still costs 0.93-1.02 ms a replayed tick on
  E-cores, a cap of 42-46 by the formula, so it stays at the floor 48.
  Retail L12 measured 0.57-0.60 ms and caps 73 / 76 (SRT4's `srt4_l12`). The
  realtime-design §4 row d note ("cap=48 on every E-core client") now holds
  for headless L12 only.
- **(2) Live: `a5u_lead`'s recipe on E-cores** (`work/LBR/lbr_run.sh` is
  CMAP's `s14_run.sh` with `work/SRT/pin_clients.py`; store
  `maps/arena-test/lbr`). The recipe is 4 clients, 60 s, 150 ms, the arena as
  a custom map, `--drive --arm A5u --sectors 1 --keep-lane --event
  2:1500:step=300:dir=up`, and the server with trafo_inputs and
  placement_clones on and snapshots off. All four clients were pinned on both
  runs. Host load max 13.2 / 15.3 %.

    | run | c2's raise | c2 relay lag (p99 / max) | c2 reach lines | c2 cap (over the session -> end) | relay_late / far / resyncs / starve resyncs (all clients) | server journal |
    |---|---|---|---|---|---|---|
    | `a5u_lead` (patience and lead on) | k = +20, N 1616, applied at 1636, slew complete at 1676, lead offset +20 | 41-42 / 43 (22 before the raise) | 24 lines; 52-53 after the raise (p99 41-42 + 8 + wait 2-3) | 103 -> 120 -> 105 -> 97 (0.30-0.44 ms a tick, restore 1.2-2.2 ms) | 0 / 0 / 0 / 0 (c2 starve_run max 185: its event's substitutions before the raise) | 4097/4097 |
    | `a5u_twin` (both off) | none | 42 / 43, and the other three members are at 41-43 too (the server waits for c2's frames until the deadline, §2.6's shared cost) | 52-54 (the deepest 72 at 1473, the step's own tick - the harness's 1500 counts from base 27 - where the windows' lag rose to 40-41 while the four-window wait still read the 23 of the start) | 120 -> 98 (0.25-0.44) | 0 / 0 / 0 / 0 | 4038/4038 |

  Every client's final cap: 95 / 94 / 97 / 97 (lead on) and 96 / 97 / 98 / 98
  (the twin), with mechanisms 52/52 (c3 52/44; core_ref_s14's c3 52/43). The
  raised member's relay lag is 42, not §2.5's 52-64, because the base header
  lag at 150 ms is 22 today (REACH's "normally 22") where §2.5 assumed C's
  32-44.
- **(3) Retail x86, two copies on L11 on E-cores** (`work/LBR/lbr_retail.py`
  is R12's recipe on RTD's helpers; `scripts/retail/lbr_l11`). The recipe is
  `net lag 150` on both copies, the key record on both, and client 1 `session
  force-rollback 30:48`. Results:
  - Session: 62 s, ticks to 4417. The reach is 53-55 (p99 22-23 + 8 +
    wait 23), history 128.
  - **Cap 83-90 on client 1** (167 rewinds, 144 of them forced 48 deep;
    0.48-0.53 ms a replayed tick, restore 1.43-1.64 ms). **Cap 76-77 on
    client 2** (37 natural rewinds; 0.57 ms, restore 1.44-1.59 ms).
  - Resyncs 0, far 0, relay late 0, mechanisms 15/15. Sparse rebased
    121 + 492 ticks on client 1. Host load max 19.9 %.
  - Client 1's `forced=144/1` (`unmatched=1`) matches R12's `143/1`: one
    forced rewind before the history held its anchor. It is a property of the
    harness verb and pre-existing.
  - Before SRT3 / SRT4 (R12, 2026-10-27, unpinned) the caps were 94 / 90.
    The E-core pinning raises the per-tick cost, and the cuts more than offset
    it on L12 (SRT4).
- **Verdict.** The need is `p99 + 8 + w`, with p99 the raised member's relay
  lag (42 after a +20 raise) and w the server's wait, the smallest slack of
  the session. The wait depends on the session, so the verdict takes two
  values of w:
  - **w = 3.** This is the measured value in a raised session. With patience
    on (`a5u_lead`) every member reads wait 3 from tick 501 on. With both
    flags off (`a5u_twin`) the late member's frames use up the slack, and
    every member reads 3 from 1660. The need is 42 + 8 + 3 = 53.
  - **w = 23.** This is the wait when nobody is late and patience is off. It
    is the server's default (`physics_patience = false`, config_manager.hpp:92)
    and the retail run's (the reach lines read `p99 22-24 + 8 + wait 22-23`
    all session). No run had patience off and lead_adjust on, so whether the
    slack comes back to 23 after a raise is not measured. The conservative
    need therefore takes w = 23: 42 + 8 + 23 = 73. The twin's transient
    corroborates the combination: 72 = 41 + 8 + 23 at its step tick 1473,
    where the windows' lag had risen while the wait still read 23.

  | host | cap on E-cores | margin over 53 (w = 3) | margin over 73 (w = 23) |
  |---|---|---|---|
  | headless x64, arena (the A5u recipe), live | 94-120 | 41-67 | 21-47 |
  | headless x64, arena, offline | 101-104 | 48-51 | 28-31 |
  | retail x86, L11 | 76-90 | 23-37 | 3-17 |
  | retail x86, L12 (SRT4) | 73 / 76 | 20 / 23 | 0 / 3 |

  Every cap covers both needs, so no raise is bound by the reach. Both live
  runs and the retail run had relay_late 0 and resyncs 0. **Open question 8
  is closed** (§7).

  Retail L12 with patience off has no margin, so the decision for a raise
  the reach cannot cover is DB3 (b) (answered: keep the full raise and accept
  that client's relay_late / resync as its own cost). At that depth the
  rebased replay (cap + 7 ticks, see the audit) can exceed the 45.45 ms
  budget by up to 7 x 0.48-0.57 ms = 3.4-4.0 ms. That is a hitch; it does not
  decide whether a correction is taken. DB1's target (a), patience on, would
  put retail at the w = 3 row.

  Only one host sits at the floor: a headless x64 client on L12 with the
  trafo inputs, cap 48 on E-cores (0.93-1.02 ms a replayed tick; the formula
  gives 42-46). There the formula's reach for a raised member (53 with
  w = 3) is above the cap. The measured depths are below it: the raised
  relay lag is max 43, and a held notice is h + w + 1 = 42 + 3 + 1 = 46 deep.
  With w = 23 a held notice would be 66 deep and late. On that client this is
  DB3 (b)'s accepted cost. The deployed client is retail, and its cap there
  is 73-76.

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
- *As built (step H3, FULL, 2026-10-29): RL1's deferred live checks
  (a), (b), (c), (e).* No code change. The headless client of the tree
  committed as 00082de8 (`build-agent-rH2s`; H2's change is SimTool's
  only), the Arch server built from 12ccae76 (the same server code); every run
  snapshots off, trafo inputs and placement on, captures and journals under
  `maps/arena-test/h3_full/<run>`, the tables under `.local-tools/work/H3/`
  (`lag_*.txt` from TRIAL1's `resync_lag.py`); seven live runs (the
  seventh, `rl1b_waitother`, in the gate round), one at a time, host load
  <= 16.4 %; every server journal replays exactly. The plan's "signed
  schedule tick logged" is **partially met**: the server prints no such
  line, so the tick is derived here from the server journal - the deadline
  fitted on a held range, deadline(base) - step(C) against lead + D
  periods - and the server's S1 lateness q50 (the exact per-member measure)
  is the evidence. Queued (roadmap section 4 item 2): one log line in the
  server's re-anchor path (`session_runner::reanchor_tick`: the signed
  schedule tick, the floor, the base) for the next step that touches the
  server, so this check need not be fitted again.
  - **(a) the late joiner** (`rl1a_join150`: 150 ms, D 17, client 3
    `--join-late 3:1500`, patience and lead_adjust off): base 1659 assigned
    at runner tick 1612 (stepped at its deadline), deadline(1659) 716 ms =
    47.3 periods after that step, i.e. on the schedule with the joiner's
    `late_tick_lead_ticks` 31 + D 17 = 48 (the base computed within a period
    of the step). Its S1 lateness q50 26.00-26.01 against the start members'
    21.88-21.93 (+4.1 ticks: 31 against the start base 27, as RL1b said);
    its relay lag of every remote 24 (25 in its first window) against the
    start members' 20; the start members' relay lag of the joiner 20.
    "Joiner relay lag <= start members' + 4": met exactly at the bound on
    the steady windows (from 1800 on); the first window after the join
    (1500, the join's transient) reads 25, i.e. + 5.
  - **(b) lead_adjust and patience on, a stall-triggered resync**
    (`rl1b_stall_pl`, the resync_snapoff recipe, client 2 paused 1000 ms
    at 1500): base 1588 at runner tick 1566 (the `L_eff` path, offset 0, no
    lead decision in the run); client 2's lateness after 21.99-22.00
    against the others' 22.26-22.29 (-0.3); every relay lag 20 before and
    after; re-step 1587 -> 1615 = **28 ticks**, waited 0 frames; the frame
    that applied it took **28 ms** (13 ms over the period, made up by the
    next frame, +3 ms; t_rest/t_rep 63/696 ms over the whole run); resyncs
    1/1 on client 2, 0/0 elsewhere, one post-start S1 reset for it: no
    second resync. The deadline fit is poor with patience on (no held
    range at the deadline, D0 spread 43 ms), so the lateness, not the
    fitted deadline, is the measure.
  - **(b) the resynced member's own correction while the server lags**
    (`rl1b_lagresync_pl`, the same flags, `BMMO_SESSION_STALL=1490:400`,
    client 0 `--resync-at 1500`): the request was answered while the
    runner was 26 ticks behind its deadlines (the steps before the S1
    reset 26.6 / 25.6 ticks late); base 1522 at runner tick 1491, held
    window [1491, 1522) = 31 ticks (the client's numbering moved by 1:
    "delta +1", so its on-time frames of those ticks were discarded and
    applied held); re-step 1521 -> 1548, 27 ticks. **Own correction 0.532 m**
    (its live rows 1522-1548 0.532-0.534 m off the server's held trajectory;
    0.0000 from 1549, the apply, on). And a **1.5-tick shortfall**: client
    0's lateness 22.0 -> 20.54 for the rest of the run, and, the server now
    paced by it, every other member's relay lag 20 -> 22. Cause (journal
    wall clocks): the frame that handled the SessionAssign ran 37 ms (tick
    1523, 22 ms over the period) and the headless pacer's origin restarts
    with the numbering after the resync's anchor capture: the exact
    re-assign runs `anchor_resync(msg.first_tick, old_tick)`
    (`client_core.cpp` 846) before
    `hooks.numbering_restarted(clock_site::assign_resync)` (852), whose
    headless hook sets `origin_ = wall_now(site)` (`session_client.cpp`
    1797), so that overrun is never made up (the apply's own 22 ms frame
    at 1549 is made up at 1550, +7 ms): 22 ms = 1.45 periods. The cause is
    inferred from the journals' wall clocks (no diagnostic line was added);
    the seventh run below shows it again, smaller (a 13 ms overrun ->
    0.85 tick). **The headless host's pacer only:** the retail mod sets no
    `numbering_restarted` hook (`client_platform.hpp` 82-84: "retail:
    nothing (its fixed tick counts on its own)"; no hook anywhere in
    `BallanceMMOClient`), so a retail exact resync keeps its fixed tick's
    phase - whether it then loses or gains any phase live is unverified (no
    retail resync run in H3). So this is not a user decision but a harness
    fidelity fix: keep the old phase on an exact resync, as retail does
    (or take the origin at the assignment's arrival); until then it skews
    later headless measurements after a resync (every other member's relay
    lag + 1-2). Queued as a harness item, roadmap section 4 item 2.
  - **(b) the resynced member's own correction while the server waits on a
    different member** (`rl1b_waitother`, the gate round's seventh run:
    the resync_snapoff recipe - client 2 paused 1000 ms at 1500 - plus
    client 0 `--resync-at 1525`; patience and lead_adjust **off**, so the
    server waits on client 2 at its deadlines instead of cutting): the
    request was answered at runner tick 1504 while the runner held client
    2's ticks at their deadlines (S2 held 1501..1543; the earliness of the
    four steps before the S1 reset 0.0); base 1547 = C + 43, deadline(1547)
    651 ms = 43 periods after that step (lead 27 + D 17, on the schedule),
    held window [1504, 1547) = 43 ticks = lead + D - e(C) with e = 0, as
    RL1b predicted; client 0's numbering intact ("delta +0"); re-step
    1546 -> 1594, **48 ticks** (inside `resync_reach()` = history - 1 =
    95), waited 0 frames; the frame that applied it took 25 ms (+10,
    made up by the next, +4). **Own correction 0.0000 m** (client 0's own
    ball equal to the server's at every tick 1500-1600). Client 0's
    lateness 22.3 -> 21.47 (-0.85) and clients 1 and 3's relay lag of
    every remote 20 -> 21: the same pacer slip as above, its frame 1547
    (the one that handled the assignment) took 28 ms (+13 ms, 0.86
    period) and was never made up. Client 2's own stall-triggered resync
    (base 1587 at 1544) followed; resyncs 1/1 on clients 0 and 2, 0/0 on
    the others, no second resync for anyone; server journal 4062/4062
    (the harness's VERDICT reads FAIL - client 0's and client 2's resyncs,
    client 2's starvation 43 in its pause - the arm's expected verdict, as
    for core_ref_s12's resync_snapoff).
    So both variants of RL1b's deferral ("while the server waits or lags")
    are run: the wait costs the resynced member nothing on its own ball
    and puts its re-step at the start's budget plus the path (48), the
    backlog costs it the held window's correction (0.532 m above).
  - **(c) the LAN case** (`rl1c_lan_resync`, the resync_snapoff recipe at
    0 ms): D = 6, start base 8; base 1574 at runner tick 1561 (C + 13, the
    floor C + 2 not binding); client 2's lateness after 12.08 against the
    start members' 5.95-6.03: **no shortfall, but a 6-tick excess for the
    rest of the session** - its relay lag of every remote 6 against the
    start members' 0, i.e. a LAN player who resyncs once predicts 6 more
    ticks of every remote ball (more rewinds) until the session ends, and
    with lead_adjust off nothing lowers it. This contradicts, on a LAN,
    "every re-anchor gets the start's budget" above: the resync gets the
    rule's full budget (deadline(base) - now = lead 8 + D 6 = 14 periods,
    12.08 after a ~2-tick path), but **the LAN start members never get
    theirs**. Cause (journal wall clocks; `client_core.cpp` 584-641): the
    core sends SessionReady (584) *before* its anchor work - the journal
    open (596), the airship zone, the spawn ghost and one trafo piece set
    per member (`ensure_trafo_set`, 632-640) - and the hold is polled only
    after it. All four clients opened their journals within 2 ms (utc
    ...059) and played their first tick (8) 132 ms later (...191), their
    holds "released after 0 polls": on a LAN the start assignment is
    already waiting when that work ends. The server's deadline fit puts
    the all-ready moment at about ...078, so the start members' first
    frame comes ~113 ms (7.5 periods) after it and eats the start base's 8
    periods, leaving D. At 150 ms the 300 ms assignment round trip covers
    the same work (`rl1b_stall_pl`: "released after 110 polls"; journal
    open -> first tick 346-356 ms, all four first ticks at the same wall
    instant, i.e. bounded by the round trip, against 132 ms on the LAN,
    bounded by the work), so the start members keep the budget there
    (22.3 = 44 - two trips - ~2) and the resync matches them. The
    attribution to the anchor work is inferred from these wall clocks and
    the code order (no timestamped line brackets the work itself). Not fixed here; queued (roadmap section 4
    item 2) as "RL1 LAN excess" with the options: send SessionReady after
    the anchor's work (a shared-core change; the start members then get
    the full budget, the resync matches them, and LAN relay lag stays 0),
    or count the work into the LAN start base. The work grows with the
    member count (one piece set each). Re-step 1573 -> 1587, 14 ticks;
    resyncs 1/1, server journal 4574/4574. The same run had a **1.07 s server step at 2059**
    (`step time ... max 1069616 us`; TICK gap 1084 ms), the step that logs
    the first "airship: ... a ghost from here": every client then got 69
    ticks of relayed frames in one burst, 3 of them past the reach (the
    harness's FAIL; core_ref_s12's resync_snapoff fails its A0 verdict on
    the resync anyway). Live-only: a SimTool replay of that server journal
    runs ticks 2050-2070 at its normal pace (2070 ticks in 1.1 s), so the
    world's simulation is not slow there. Not established; the likeliest
    cause is B2SW's class (a write through WSL's 9P blocking the step): the
    server's log is still `server.log` on `/mnt/d` (`arch_server_lp.sh`),
    and the step logs that line. Queued (roadmap section 4 item 2).
  - **As built (step DX5, 2026-10-29): RL1-LAN fixed, the user's option 1
    ("选1": SessionReady after the anchor work).** `session_client_core::anchor`
    (`client_core.cpp` 511-656) now sends SessionReady last: the phase, the
    pacer's origin hook, the hold (its 10 s fault deadline still read here,
    `anchor_hold`), the "anchored" line, the journal open, the airship zone,
    the spawn ghost, one trafo piece set per open life, and only then the
    message (built and sent at 605-655; its fields and caps unchanged). Shared
    core, so headless and retail alike; no protocol or journal change.
    **Adapted:** the hold's deadline is not moved after the work - the
    headless inbound capture replays clock reads strictly in order by site
    (`inbound_capture.hpp` `take_clock`), so moving `anchor_hold` past
    `journal_utc` / `journal_start` would make every reference capture
    refuse; 10 s against ~0.1-0.3 s of work changes nothing. The journal
    still opens before the work (it holds what it held: header, start note,
    PLAYER records; nothing in the work writes to it or sends or polls), so
    a journal's header -> first tick span still contains the work and is not
    the measure of the fix (4 members 142 ms, 8 members 299-313 ms after,
    against 132 and 280-300 before). Unit test
    `SessionClientCoreFlow.AnchorSendsReadyAndArmsTheHold` pins the new call
    order (an airship zone call before `send:0`). Wire check over
    `core_ref_s12`, rDX4 vs rDX5: **ALL SAME** (36 send traces SAME, 36
    replayed journals IDENTICAL) - no send moved relative to another (no
    send between the old and the new place) and no clock read moved, so the
    plan's expected "send-order differences where SessionReady moved" do
    not occur. One consequence: the anchor work now counts against the
    server's start barrier (`kStartBarrierTimeout` = 8 s, armed when
    SessionStart is sent, `server.cpp` 1278-1281; `check_start_barriers`
    1964-1994 drops a member that has not reported ready by then), which
    before covered the level load only - a machine whose load ends near
    8 s and then builds its piece sets (headless ~35 ms each, retail
    unmeasured) can now miss it and is dropped with "did not report
    ready" (it stays in the room and can late-join); the FULL retail run
    is to log the retail work time. Live (Arch server rebuilt from this tree, snapshots off,
    patience and lead_adjust off, `.local-tools/work/DX5/`, stores
    `maps/arena-test/dx5_full/`):
    - `rl1c_lan_resync` (this section's (c) recipe at 0 ms, D = 6, start
      base 8): the start members' S1 lateness q50 **12.49-12.53** (H3:
      5.95-6.05) = lead 8 + D 6 less the ~1.5-tick path; the hold now waits
      ("released after 8-9 polls", H3: 0 polls - the assignment is no
      longer waiting when the work ends); client 2 (paused 1000 ms at 1500)
      resynced to base 1568 at runner tick 1555 (C + 13), lateness after
      **11.92-11.94** against the start members' 12.5 (H3: 12.08 against
      6.0); its relay lag of every remote **0** after the resync and the
      start members' relay lag of every remote 1 (H3: client 2's 6, the
      start members' 0), i.e. **no 6-tick excess**; the remaining ~0.6 tick
      is the headless pacer's resync slip (H3's harness item: the frame
      that handles the assignment restarts the origin), not the LAN start.
      That slip is also why the start members' lag of every remote went
      0 -> 1 from the 1500 window (server earliness 12.0 -> 11.4): with
      patience and lead_adjust off the server steps when its latest
      member's frame is in, which is now client 2 (11.9 against 12.5), so
      every relayed frame comes ~0.6 tick later; in H3 the start members
      (6.0) were the latest and the slip did not show. Clients step a
      tick 4-8 ms before the server does (H3: 10-16). Resyncs 1/1 on client
      2, 0 elsewhere; server journal 4585/4585; the harness's FAIL is the
      arm's designed event (client 2's resync and its 54 starved ticks in
      the pause), as for `core_ref_s12/resync_snapoff`.
    - `u9ctl` (H4's 8-member 0 ms control, 30 s, client 2 `--rejoin-at
      900`, no captures): **starved 0 on all eight clients in all four
      sectors** (H4: 1487-1514 of ~1500, every own frame substituted, and
      the harness stopped after sector 1 - whether H4's invocation also
      passed `--sectors 1` is not recorded; this one ran all four arena
      sectors, four 30 s sessions, 4.3 min; the client journals and
      `server.bmjr` in the store are sector 4's, `server.log` holds all
      four); every member's S1 lateness q50
      **12.49-12.52** (H4: -2.6..-4.5), the 4-member level of the run
      above; the clients step tick T 0.5-1.6 ticks *before* the server
      (H4: 2.6-4.5 ticks after); holds released after 8-31 polls (H4: 0).
      Every sector's server journal replays N/N; harness VERDICT PASS in
      all four. The 0 ms late joiner of the step's pass list is this run's
      rejoin (client 2's second life, a late join at 0 ms): base 945
      assigned at server tick 932 (C + 13), its first frame played 8 ms
      before the server stepped 945, lateness after 11.84 against the
      members' 12.5 (the same pacer slip) - inside its budget.
    - The piece sets' share (H4's question), derived, no new clock read
      (a new clock site would break every capture): before the move both
      runs were work-bound (holds released after 0 polls), header -> first
      tick 132 ms with 4 sets and 280-300 ms with 8, so ~35 ms a piece set
      and the airship zone plus spawn ghost a few ms. After the move the
      excess is gone in both (lateness 6.0 -> 12.5 with 4 members, -3.4 ->
      12.5 with 8), so the work, whatever its size, no longer eats the
      start base on a LAN.
    Deferred (live-run cap of the quick tier: two runs): `rl1b_stall_pl`
    (the 150 ms resync recipe, "not worse") - at 150 ms the assignment's
    round trip already covered the work (H3: "released after 110 polls"),
    and the move only delays the all-ready moment by the work, which every
    member's schedule follows; to be run in the FULL batch. The retail
    two-copy run (the x86 mod compiles with the change; the retail host
    calls `core.anchor` as before and holds after it, `core_host.cpp`
    1956-1975, so nothing host-side changed). F2-a's joiner budget (a
    joiner's own anchor work no longer counts against its base's budget)
    is DX6's to measure.
  - **(e) a late joiner in the first start_base periods after the
    all-ready moment: not run, unreachable with these binaries.** The
    window is start_base periods (27 = 409 ms at 150 ms, 8 = 121 ms on a
    LAN); a joiner is admitted only once the session runs and its
    SessionReady follows its own world boot - measured on HEAD (LAN,
    `f2_rejoin_B`): admit -> SessionReady 91-416 ms, -> first frame
    216-523 ms; at 150 ms two one-way trips (300 ms) come on top. A rejoin
    leaves at a tick at or after its base, so it is later still. The rule
    there is unit-tested (`lead_test.cpp`
    `ReanchorOnSchedule.BeforeTheScheduleStartTheBudgetIsStillTheStarts`,
    with the rest of `ReanchorOnSchedule*`: 13/13 pass on rH2s). A live
    check needs a joiner that sends its Join at the all-ready moment with
    its world already booted (no such harness switch). The runner's own
    call on that path is not unit-tested (no world-less `session_runner`
    fixture, RL1b above), so the unit test covers the rule, not the start
    path. Queued (roadmap section 4 item 2), not dropped: a headless
    harness switch for a pre-booted joiner (its world anchored and held
    before it sends Join, the Join sent at the all-ready moment), then
    this live check.
  - **As built (step DX9, FULL, 2026-10-30): H3's three harness items -
    the headless pacer's phase on a resync, a pre-booted late joiner and
    RL1-e live, the server's re-anchor log line.** Headless client, harness
    and server only; no shared (`BallanceMMOCommon`) file, no protocol or
    journal change, the retail mod untouched.
    - **The pacer keeps its phase over a resync's renumbering**
      (`session_client.cpp` `pacer_resync_renumbered`, called from the
      `numbering_restarted` hook at `clock_site::assign_resync`): frame k
      of the new numbering is due where frame `paced_frames_` + k of the
      old one was (`origin_ += paced_frames_` periods, `paced_frames_` = the
      last frame the origin's schedule ran), so the frame that handled the
      assignment no longer loses its overrun; the other four sites still
      restart the origin. The clock is still read at the site (the
      inbound capture takes reads in order), and the client logs what a
      restart would have cost ("pacer: the resync's renumbering keeps the
      frame phase (N frames on the old origin; restarting the origin here
      would have moved every later frame +x ticks)"). Both resync paths
      share the site, so the full-snapshot answer keeps the phase too (as
      the retail mod's fixed tick does for both); with no frame on the old
      origin it restarts as before - except when that origin is itself a
      kept one (the review's case: a newer SessionAssign arriving while
      the first resync's A3 hold still holds the world - RL1b's
      base-missed re-answer, or "resync hold ended by a newer
      assignment", `client_core.cpp` ~856, which calls no
      `resync_hold_ended`): the kept grid stays, and the origin moves by
      the whole periods that came due meanwhile (held slots, not owed,
      A3's rule), so frame 1 is the first slot of that grid after the
      assignment ("pacer: a newer resync assignment before any frame on
      the kept origin keeps that phase (N held slots not owed)"); a
      restart there would bring H3's 0.85-1.5 tick slip back. Host-only
      (no core change). Not met in any run or capture so far (no log of
      it in `dx9_full` or the reference replays). `client_platform.hpp`'s comment on the
      hook ("Headless: origin_ = wall_now(site)") is left as it was: a
      shared header, and the headless host's behaviour is this note's.
      **`--pacer-restart-on-resync`** brings back the old rule, for
      replaying a capture taken before DX9 (its pacer decisions after a
      resync are then the capture's; the replay's argument check passes
      it over, like `--replay-free-cost-clock`).
      Live, `rl1b_waitother` (this section's (b) recipe, the gate round's
      seventh run of H3: patience and lead_adjust off, client 2 paused
      1000 ms at 1500, client 0 `--resync-at 1525`; `maps/arena-test/
      dx9_full/rl1b_waitother`, `.local-tools/work/DX9/lag_rl1b_waitother.txt`):
      the two resyncs logged slips avoided of +0.96 (client 0, base 1548)
      and +0.49 ticks (client 2, base 1589). The resynced members' S1
      lateness q50 21.87-21.89 before, **22.85-22.86 and 22.53-22.56 after**
      (H3: 22.3 -> 21.47, a 0.85-tick shortfall); the others' 21.86 before
      and after; **clients 1 and 3's relay lag of every remote 20 before
      and after** (H3: 20 -> 21); server earliness 21.3-21.4 before and
      after (H3: 21.8 -> 20.9). So no slip. What remains is the other
      way: a resynced member now plays its frames 0.7-1.0 tick earlier
      than the start members, and its own relay lag of every remote is 21
      against their 20 (it alone predicts one more tick; nobody else
      pays). Cause (the rule, not the pacer): the base gives lead + D
      periods less the floor's fraction from the server's handling
      instant, and with the phase kept the base is played at the client's
      next slot after the assignment's arrival, so lateness after =
      lead + D - (path) - (fraction + slot wait), and fraction + slot wait
      is a constant of the client's phase (~0.33 or 1.33 periods here),
      while the start members also lose the start path's hold release.
      Not a slip and not this step's to change (the plan's fix is the
      retail-equivalent phase); recorded for the FULL batch's resync lag
      readings. Server journal 4061/4061; resyncs 1/1 on clients 0 and 2
      (the arm's designed events, VERDICT FAIL as in H3 and
      `core_ref_s12/resync_snapoff`).
    - **A pre-booted late joiner** (`session_client.cpp` `--join-cue
      FILE`; `scripts/arena_map_test.py` `--join-late N:cue`): the client
      loads `--level` right after its engine boots - `begin_session`'s
      level request and `load_step`'s loading ticks with the level-file
      counters recorded on first sight, up to where `load_step` would
      anchor (Gameplay_Ingame active) - and holds it (nothing ticks an
      idle client); connected, it remembers the room and sends its Join
      only once FILE exists; the SessionStart that follows takes the
      loaded level (`begin_session`: no reboot, no load; once only, a
      later life loads as usual; another level is loaded again), so the
      anchor is its first load step (the trafo reference is the anchor's
      own capture: no tick runs between). In a replay the cue counts as
      given (the capture's Join is its next send either way): the joiner's
      capture replays to its end marker. The harness starts a cue joiner
      with the others (outside the host's `--expect`), follows the
      server's log from its size at the case's start, and writes the cue
      at the runner's "all K players ready, ticking from" line (printed
      right after `start_ticking`, so the Join meets a running session: a
      late join, not a start member). Adapted from H3's wording ("its
      world anchored and held before it sends Join"): a world cannot
      anchor before its SessionStart (the seed, the options and the
      placement come in it); what is held is the loaded level, which is
      the boot's cost (admit -> SessionReady 91-416 ms on HEAD).
    - **RL1-e live** (`rl1e_cue`: clients 0-2 at 150 ms, D 17, start base
      27 = a 409 ms window, client 3 `--join-late 3:cue` on a 0 ms link -
      at 150 ms for the joiner too, Join + SessionStart + SessionReady are
      three one-way trips, 450 ms, past the window whatever the boot;
      patience and lead_adjust off, 40 s, sector 1;
      `maps/arena-test/dx9_full/rl1e_cue`): the server's line "re-anchor
      of Arena3 (late join): **-227.8 ms from the schedule start (before
      it), signed schedule tick 11 (first 27 -16)**, lead 19 -> 30, floor
      29 (next tick 27 + margin 2), base 30 (the schedule)" - RL1-e's
      window reached, and the signed rule gave base 30 where RL1's clamp
      (`tick_at`) would have given first + lead = 46, 16 ticks more. The
      joiner's S1 lateness q50 **32.95-33.35** = lead 19 + D 17 less ~3
      (the fraction, its own path and processing), against 49 the clamp
      would have left; the start members' 21.65-22.37; every start
      member's relay lag of the joiner 20, as of each other (the joiner
      does not pace the server); the joiner's relay lag of the 150 ms
      members 11 (one trip). FINAL rows (`.local-tools/work/DX9/
      final_replay.sh`, H4's `final_cmp.py`, the airship span and the
      balloon out): own, mirrors and bodies 0.0000 m on all four clients,
      the joiner included; server journal 3168/3168; resyncs 0; starved 0;
      VERDICT PASS (the harness's live-row "late ball mirror" 0.78-1.15 m
      on the start members is the prediction of a mirror at 150 ms, plan
      section 4 C1, as in H3's `rl1a_join150`, 0.85 m). **RL1-e: PASS.**
      The runner's start path is now exercised live as well (the unit
      tests cover the rule).
    - **The server's re-anchor line** (`session_runner::reanchor_tick`,
      parts from `sim/late_tick.hpp` `reanchor_on_schedule_parts`, whose
      base is `reanchor_tick_on_schedule`'s): "[session S] re-anchor of
      NAME (resync | resync, L_eff | late join): +-ms from the schedule
      start (before|after it), signed schedule tick T (first F +-k), lead
      L -> T+L, floor C+m (next tick C + margin m), base B (the schedule |
      the floor)". The resync path called the rule twice when the
      per-player lead applied (the first result was overwritten unread);
      it now calls it once (same base). Unit test
      `ReanchorOnSchedule.TheLogLinesPartsAreTheRules`.
    - **Checks** (quick tier, ~12 min of checks): x64 build BUILD-OK /
      DEPS-OK; unit tests 672/672; GCC syntax 31/31 (`server.cpp` and
      `session_runner.cpp` compile in the Arch server build, ARCH-BUILD-OK);
      the wire check over `core_ref_s12`, rDX8 vs rDX9: with
      `NEW_EXTRA=--pacer-restart-on-resync` **ALL SAME** (36 sends SAME, 36
      journals IDENTICAL); without it 33 SAME / IDENTICAL and the three
      resynced captures (`resync_snapoff` c2, `resync_at150` c0,
      `resync_at150_down` c0) diverge at the first pacer read after their
      resync's assignment ("expected a clock read at journal_ms, the
      capture has one at pace_due": the kept phase runs the frame the old
      origin made wait), their sends identical up to the new base - by
      design; those replays log the slips the reference runs took, +1.20,
      +0.41 and +0.81 ticks. Two live runs (above). Not run: the x86
      build (no shared or retail file changed). **From DX9 on, a wire
      check over `core_ref_s12` (or an older set) whose binaries keep the
      phase passes `--pacer-restart-on-resync` to them** -
      `EXTRA=--pacer-restart-on-resync` when both are DX9 or later,
      `NEW_EXTRA=` only when the old one predates DX9 (it has no such
      flag) - until a later REF step re-captures the set (done at FULL
      step REF13: `core_ref_s13` was captured under the DX9 rule and
      replays without the flag; `wire_check.sh` adds it only for a pre-DX9
      set); noted in
      `wire_check.sh`'s header and the set's README. Gate round: the
      review's two should-fix items - that tooling note, and the newer
      assignment before any frame (above) - built; x64 build, unit
      tests, GCC syntax and the wire check re-run (the live runs not:
      the new branch is not reached by them).
  - Also seen: the three start members of `rl1a_join150` were held 10-11
    ticks together twice (319-329, 633-643), before the join - a hiccup on
    the harness side (the clients or the latency proxy all three uplinks
    pass through), not the joiner's. The harness's own A8 criterion
    "joiner lead == the session's input delay (lead_adjust off)" reads
    PASS (17 vs 17).
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

*As built (step F3, the stage A close-out's lifecycle-event arrival margin,
LCM, 2026-10-29; diagnostic only, no protocol or simulation change):* the
S5 line ends with ` margin=M slack=S` for a member's event: `M` = origin -
the session's next tick `N` when the runner took it (>= 0 in time, the
exact quantity `applied = max(stamp, N)` acts on), `S` = (deadline(origin) -
arrival) in ticks (`lateness_slack_ticks`). The harness got `--proxy-log`
(one line per datagram) and `--loss-seed` (0 = the old draws). Measured on
REF12's `swing_snapoff` recipe (150 ms, 10 % loss, input delay 17) at seeds
0, 1 and 2 (`maps/arena-test/f3_lcm`, `.local-tools/work/F3/lcm_analyze.py`):
the events stamped local + 1 (Physicalize, Unphysicalize) arrive with
**M = 0..1 tick for whichever member paces the server** - per member per
run: seed 0 c0 / c1 / c3 0..1, c2 0..3; seed 1 c0 / c1 / c3 0..1, c2 -2..1;
seed 2 c2 0..1, c1 1..4, c0 3, c3 3..4 (seeds 0 and 1 have no single
pacing member: every member's events sit at 0..1; in seed 2 c2's uplink
paced the others) - while S is
about 22 ticks (one exception: seed 0 c3's Physicalize at 3385, M = 21, at
the session's end, the other members gone and the server back on its
deadline); Sector and Trafo (stamped with the 35-tick lead) arrive with
M = 33..40. S does not govern lateness: with patience off the runner steps a
tick as soon as every member's frame is in ("every buffer has its frame, or
the deadline passed", the step loop of `session_runner.cpp`), so the server
runs about 22 ticks ahead of its deadlines, paced by the slowest uplink, and
an event sent with frame L for stamp L + 1 is taken just before the server
could step L + 1. (`core_ref_s11`'s README inferred "about 6 ticks" from the
input slack; that is S's frame of reference, not M's.) One lost copy is
covered: the first unreliable repeat goes out in the next tick's input
datagram (GNS coalesces it with the frame: the 510-536-byte datagrams of the
proxy log), and the server cannot step L + 1 before that frame arrives. Two
losses in a row - the reliable copy (for a beam pair, either reliable
message: consistent with GNS's in-order reliable stream, a lost
Unphysicalize holds the Physicalize behind it - in seed 1 the passed
Physicalize was applied with the Unphysicalize at L + 3, and both went out
again together in one 207-byte retransmit 11 ticks later) and the L + 1
datagram - let frame L + 1 arrive in
the L + 2 datagram, which carries no repeat (spacing 2), so the next copy, at
L + 3, is taken at N = L + 3: late by 2. Seed 1 shows exactly that for c2's
beam pair (proxy2: the 66-byte Unphysicalize datagram lost, the 187-byte
Physicalize datagram passed, the 536-byte L + 1 datagram lost, the L + 2
input datagram passed; S5 `margin=-2`), then "own beam moved 701 -> 703 ...
resync": the signature of `core_ref_s11`'s c0. A lone late own Unphysicalize
is "kept where we stamped it" (the own world ends the ball up to 2 ticks
before the server does; with no resync and no correction - queued as its
own item with plan step 22 in roadmap section 4 item 8). Changing any of
this is plan section 3 D4, the user's decision. The options and what each
costs: (a) stamp own events at local + k: k = input delay (17) gives a
margin of about 17 but the own world must hold its own births and deaths
back 17 ticks (258 ms; own-body control, retail only at R6); a small k
gives a margin of about k for a k-tick hold-back; either is a stamp-rule
change. (b) repeats every tick instead of every other tick: the copy rides
every input datagram until frame L + 1 is in, same bytes, no protocol
change - but it was measured and rejected before (`lifecycle.hpp`'s
`kLifecycleEventRepeatSpacing`: six copies one tick apart all fell inside a
dozen-datagram burst and the reliable copy carried the event late again;
six copies span 6 ticks instead of 12), and F3 ran with `--loss-burst 1`,
which hides that cost; the server's relay shares the constants unless they
are split, and the wire check shows the send-trace difference by design.
(b') the hybrid ("earlier redundant repeats"): first copies at L + 1 and
L + 2, the rest at spacing 2 - closes the two-in-a-row gap measured here and
keeps the 12-tick burst span; one more copy per event, no protocol change.
(c) events on the input datagram (plan step 21) and / or a moved own pair
handled without a resync (plan step 22): Stage B item 8, protocol and core
work, retail's own death cannot move before R6. (d) accept it: about 1 % of
lone events and 2 % of beam pairs late at 10 % loss (2 late pairs in 5
measured runs, both resynced), far fewer at realistic loss; it conflicts
with the headline's resyncs 0 for the 10 %-loss arm and leaves the 2-tick
own-death gap.

*As built (step DX8, FULL, 2026-10-30): D4 = (b') hybrid repeats, measured
and kept; U5 (c).* The user's answer to D4 was "你决定"; the orchestrator
chose (b'). **The schedule**: the old copy ticks plus one, i.e. the copies
of an own event ride the input datagrams of L + 1, L + 2, L + 3, L + 5 ...
L + 11 (seven copies, span 11) - `lifecycle.hpp` `kOwnEventRepeats` 7,
`kOwnEventRepeatsDense` 3, `kOwnEventRepeatSpacing` 2, `event_repeats`'s new
`dense` count (the first `dense` copies on consecutive calls), pushed only by
the core's `put_event_on_wire` (`client_core.cpp`) - so it applies to every
own event the core sends (Sector, BodyRevived and Trafo too, not only the
Physicalize / Unphysicalize the plan row names: one more copy each,
harmless). Adapted from the plan's
wording ("first repeats at L + 1 and L + 2, then spacing 2", which also reads
as L + 1, L + 2, L + 4 ... L + 12): that reading drops L + 3, so a burst of
three from the reliable datagram (reliable, L + 1, L + 2 lost, L + 3 passed),
on time today, would be late; the superset keeps every send tick of the old
schedule, so no loss pattern, bursts included, makes an event later than
before. **Split as the plan allowed**: `kLifecycleEventRepeats` /
`kLifecycleEventRepeatSpacing` and `event_repeats::push`'s defaults are
unchanged, so the server's relay (the downlink is not the problem: relayed
depth 20-25 against a reach of 52-74) and the legacy retail path send
exactly as before. **Two send orders** (found here): an event sent after the
frame's input (the beam at 700 of F3's seed 1) is stamped L + 1 and its
copies were at L + 1, L + 3 ...; one sent at a frame's start, ahead of that
frame's input (a birth or a death the platform reports first, e.g. 864 of
DX8's burst-2 run, S5 `origin=864`), is stamped L and its copies were at L,
L + 2 ...: in both the tick after the first copy - the datagram that carries
the stamp's frame when the first one is lost - had none, and it has one now.
**Measured** (`.local-tools/work/DX8/lcm_model.py`, the physics-free model of
F3's rule: on time if the reliable datagram(s) passed or a copy rides the
first passed input datagram at or after the stamp; worst case, the sender
paces the server): (1) the proxy's own loss rule (`arena_map_test.py`
`LinkProxy`, per direction a burst of b started with q = p / (b - p (b - 1)),
p = 0.10) drawn over a synthetic uplink, 200 000 events per cell, late per
1000 events old -> new: burst 1 lone 9.1 -> 0.1, beam pairs 17.2-17.6 ->
0.15-0.2; burst 2 about 50 -> 2.7; burst 3 about 35 -> 2.4-3.7; burst 4
about 50 -> 26; bursts 6 and 8 about 50 -> 34-39 (both send orders; the new
schedule was never later than the old on any draw); (2) F3's three recorded
runs (`maps/arena-test/f3_lcm`, proxy logs, 50 events aligned): the model
reproduces exactly the one late event pair seen (seed 1 c2's beam at 700,
old late 2 = S5 margin -2), and scores it on time under the new schedule; no
other event late either way. **The model's one assumption** (the review's
finding): a copy shares the fate of its tick's input datagram. In the code
each copy is a send of its own (`flush_inputs` sends the due copies
unreliable-no-delay, then the input datagram), so it only holds where GNS
packs the two into one packet. The recorded proxy logs say it does: an
event counts as coalesced when at least four of its first five
copy-carrying input datagrams are 20 bytes or more above the median of the
copy-free ones, and 83 of the 86 scored events are (F3's three runs 49/50,
DX8's burst-2 run 18/18, burst-1 run 16/18; the other three unconfirmed,
not contradicted). Where a copy does go alone and is lost while its input
datagram passes, the event is late under both schedules, by 1 instead of
2: so the model's late counts are the gain under full coalescing (an upper
bound on it), and "never later than the old" holds on every draw either
way, the new copy ticks being a superset; (3) two live runs of F3's recipe with the DX8
build (`.local-tools/work/DX8/dx8_run.sh`, `maps/arena-test/dx8_lcm`):
burst 2 seed 0 - 0 late member events, resyncs 0/0 on all four, far 0,
VERDICT PASS, and one rescue on the wire: c0's lone death at 864 lost its
reliable datagram and the input datagram of 864, the 865 datagram (with the
new copy) passed, S5 `late=0 margin=0` - late 2 under the old schedule (the
silent own-death gap); burst 1 seed 1 - 0 late, resyncs 0/0, PASS (seed
numbers do not pair runs: the proxy draws one stream for both directions in
arrival order). Cost: one more copy per own event (28-150 bytes in the
L + 2 datagram). Not changed: a lone own Unphysicalize the server does
re-stamp late is still "kept where we stamped it" (roadmap item 8, plan
step 22); it is rarer now. **U5 (c)**: `rewind_for_lifecycle`'s miss
(`client_core_lifecycle.cpp`, a late event whose rewind the history cannot
reach) now asks for a resync ("a late lifecycle event beyond the reach",
owed), with the late Sector path's guards: not while a resync is pending
and not for a stamp at or before the numbering's first tick; the change
itself is still applied live meanwhile (as the Sector path does; the
resync's state replaces it). The log line says "applied live, resync" (no
longer "until a snapshot re-anchors it"). See world-agreement-gaps-design.md
"As built (step DX8)". **Checks**: unit tests 671/671; GCC syntax 31/31;
x86 client compile; the wire check rDX7 -> rDX8 over core_ref_s12 prints
DIFFERENCES by design - all 36 send traces differ only by the added copies
(`.local-tools/work/DX8/trace_diff.py`: removing one unreliable copy per own
event at its first copy + 1 gives the old trace exactly, 36/36 EXPLAINED,
3-16 copies per client) - and all 36 journals are IDENTICAL (no capture has
a late event outside the history, so U5 (c) never fires in the set). Not
run (QUICK budget; FULL batches): F3's live seed sweep old vs new (seeds
0/1/2, bursts 1 and > 1) - the keep rule was judged on the model, F3's
recorded runs and the two live runs above. That sweep (F3's recipe,
old and new build, seeds 0/1/2, bursts 1, 2 and 4, late member events AND
resyncs per run - U5 (c) adds resyncs that did not exist before, and only
the two runs above counted them: 0) is queued as an explicit item of the
close-out's remaining batches.

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
- **As built (FULL step R6P, 2026-11-01): retail part three.** The retail
  core host runs the headless answers: own body control (R6B) with
  `own_birth_move` on. A moved own birth is moved inside the history (the
  body out live, the rewind to T-1, the body made at T'); a superseded one
  is taken out of the history; a rejected / missing one (the user's
  Q-R6-NEVER (a)) is re-reported once at the ball's current pose and that
  re-report's echo answers it as a move T -> R'' (a resync past the reach,
  always for a missing one, whose deadline is 192 ticks), the body kept on
  retail (`own_life_take_out` false); a resync only past the reach. Measured
  on L11 (session-client-core-design.md "As built (step R6P)"): the A6m
  analogue moved 507 -> 527, 0 m and no resync; the A6 drop analogue (row
  24): no stall, one resync after the re-report's ack, the jump 2.73 m (was
  78.8 m), every FINAL row bit-equal after it; past the reach one resync.

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
| 3 | multi-tick own absence (T..T'-1) confusing the retail scripts | to be measured in plan step 21; the server already runs these ticks without the ball. **Measured (FULL step R6M, "As built (step R6M)"):** no script acts on the bodiless ball for any absence up to 48 ticks lived through, or for any absence inside a replay whose rewind fits the reach, on retail and headless; the only bound is the existing reach (a replayed move whose echo arrives at local tick T + K needs a rewind of depth K + 1 <= 48, so n <= K <= 47 and, with a real echo, n <= 47 - the relay lag; beyond it the existing resync) |
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
   retail x86 and the trafo-on cost are not measured.) **Resolved
   (2026-10-27, the user's decisions, built at step R12, §2.5 "As built
   (step R12)")**: the cap is estimated at run time by every client from
   its own rewind cost, `clamp(floor((B - restore_ms) / replay_ms_per_tick),
   48, 120)` with B = 3 ticks (about 45 ms), and the margin is 8 + the
   server's wait, which the server writes into every relay header
   (protocol 13). Measured: retail x86 85-94, x64 headless 104-120. Until
   a client's first rewind the cap is 48 (no measurement yet), so a first
   stall deeper than 48 before any rewind still resyncs (§2.5, review
   round).
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
   multi-tick own absence is measured in step 21. **Answered (FULL step
   R6M, "As built (step R6M)")**: they do, for every absence of 1-48 ticks
   lived through live, and for every absence inside a replay whose rewind
   fits the reach (depth = K + 1 <= 48 for an echo at T + K, so
   n <= K <= 47; with a real echo n <= 47 - the relay lag; measured at
   K = n and K > n); R6P needs no depth cap beyond the reach
   `move_own_birth` already checks.
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
   **Closed (FULL step LBR, 2026-11-01, §2.5 "As built (step LBR)")**:
   step 12 / R12 built the per-client reach. It was measured on the reference
   machine after the realtime cuts (SRT3, SRT4), every client on E-cores of
   its own, snapshots off:
   - **What the raise needs.** A5u's full raise is k = +20, and it puts the
     raised member's relay lag p99 at 41-42 (max 43), not 52-64. The base
     lag at 150 ms is 22 today. The need is p99 + 8 + w. With the wait
     measured in a raised session (w = 3), it is 53. With the default
     server's wait when nobody is late (patience off, w = 23), it is 73.
   - **Headless x64 cap.** The cap is 94-120 live and 101-104 offline on the
     arena.
   - **Retail x86 cap.** The cap is 76-90 on L11; SRT4 measured 73 / 76 on
     L12.
   - **Result.** Both live runs (lead on, and the both-off twin) and the
     retail run had relay_late 0 and resyncs 0.
   - **Answer.** No bound of the raise by the reach is needed, and the
     deployment gate need not wait. Every cap covers 73, but retail L12 with
     patience off has a margin of 0-3. The one host at the floor is a
     headless client on L12 with the trafo inputs (0.93-1.02 ms a replayed
     tick on E-cores). Its measured raised depths are below the floor of 48
     (relay lag 43; a held notice 46 deep with w = 3). Anything that does not
     fit there, and retail L12's zero margin, is DB3 (b)'s accepted cost. The
     deployed retail client is not at the floor there.
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

## As built (step B2SW, 2026-10-27): the trial's server stall

**As built (step B2SW, 2026-10-27): what stalled the trial's server, and a
server-side stall for the harness.**

- **The stall's cause (measured).** The trial's server journal
  (`TRIAL1/rounds/round1-holdlast/server`, `.local-tools/work/B2SW/
  step_gaps.py`): 36122 steps; gaps over 100 ms at 1794 (335), 1850 (232),
  10918 (327), 20584 (646), 20676 (153), 20770 (595), 20838 (326), 27587
  (339), 27655 (242), 35594 (102). The ones at or under 336 ms fit a
  deadline wait (the largest slack, 22 ticks); 646 and 595 do not. The server
  writes its journal on the step thread and flushes it at every checkpoint
  (every 2 ticks in the trial) to `journal_dir` =
  `/mnt/d/repos/BallanceMMO/maps/arena-test/wsl/journals`, a Windows drive
  seen through WSL's 9P file system. The journal's pattern on that file
  system (`flushbench.py`, 1800-byte writes every 30.3 ms for 300 s, run
  twice at once in the Arch WSL): `/mnt/d` p50 1.3 ms, p99 7.9, p999 16.4,
  **max 523 ms**, 3 writes over 50 ms (61, 179, 523 ms); the WSL ext4 home
  directory, same moment: max 0.7 ms. So a checkpoint flush can hold a step
  for half a second - the size of the two unexplained gaps. Then a server
  rule of its own turns the stall into substitutions: the runner's catch-up
  loop (`session_runner::run`) steps up to `max_catch_up_ticks` (10) due
  ticks before it drains its command queue again, and the members' frames
  for those ticks are queued there (submit_input posts them) although they
  arrived before their deadlines - every member's 9-10 ticks after the stall
  are substituted (the trial: "S2 held ... ticks=20585..20593 n=9 src=20584",
  "20771..20779 n=9 src=20770"; the harness: n=6-8 per stall, ≤ 10 always).
  Their notices then come back h + stall deep (the trial's 61-64). Not
  fixed: both halves have options (the user's, below).
- **The harness's server stall** (`session_runner`): the environment variable
  `BMMO_SESSION_STALL=TICK:MS[,TICK:MS...]`, read once at start (logged),
  holds the step of each TICK for MS ms after the step test passed it, inside
  the step's timing - a slow step. Unset in every deployment; the Arch server
  inherits it from `wsl -d Arch -- env BMMO_SESSION_STALL=... bash
  arch_server_lp.sh ...` (`.local-tools/work/B2SW/live.sh`, `STALL=`).
- **A notice past 48 taken without a resync, live**
  (`maps/arena-test/b2sw_stall4`: 2 clients, 150 ms, snapshots off, client 0
  `--brake-tap 3:7` so a repeated frame differs from its own; stalls 560 ms at
  1300 and 660 ms at 1700): the server substituted ticks 1302-1309 of both
  clients from 1301's frame; client 0's notice for its own tick 1303 arrived
  at local 1358, 56 deep - past the fixed 48 - while the burst of headers
  after the stall had lifted its reach 51 -> 86 ("reach: 86 ticks at tick
  1358 (relay lag: p99 57 ... + 8 + wait 21, cap 120)"), and it was taken by
  "rollback to tick 1302 (own input substituted at tick 1303), re-simulated
  56 ticks"; resyncs 0/0 on both clients; the server journal replays
  2324/2324. (420 ms stalls: rewinds of 47-48, inside the floor,
  `b2sw_stall3`.)
- **The user's choice** (reported): (A) the stall - (i) keep the journal off
  the 9P file system in the harness and the trial (an ext4 directory, copied
  out after the run; config only), (ii) write the journal from a writer
  thread so no file system can hold a step (the black box then loses up to
  the queued records on a crash instead of up to the last flush), (iii) flush
  less often (by time, not per checkpoint); (B) the catch-up substitutions -
  (i) drain the command queue before every catch-up step (a frame that
  arrived before the step is used; one that arrived after its deadline during
  the stall would be used too), (ii) the same but honouring each frame's
  arrival against its tick's deadline (exact to the deadline rule; the input
  buffer would carry arrival times), (iii) leave it (the per-client reach of
  R12 covers stalls up to about cap - h - 1 ticks).

## As built (step B2SX, 2026-10-27): the trial's server stall fixed

The user's choices (2026-10-27): (A)(i) the local server's journal off the
9P file system, config only, and (B)(ii) the catch-up drains the queue and
judges every frame by its arrival against its own tick's deadline.

- **(A)(i), the local test server** (scripts under `.local-tools`, which git
  excludes): `arch_server_lp.sh` points `physics.journal_dir` at WSL's ext4,
  `/home/swung/bmmo-journals/<label>`, while the server reads its
  configuration (config.yml then gets its `/mnt/d` value back, so every
  other starter is unchanged) and starts `arch_journal_mirror.sh SRC DST
  PID`, which every 0.25 s copies each journal the server no longer has open
  (`/proc/PID/fd`: a session's journal closes at its end) into
  `maps/arena-test/<label>/journals` (a `.part` name, then renamed; `cp -p`
  keeps the server's mtime); `arch_stop.sh`, and the next `arch_server_lp.sh`
  start, copy what is left (a killed server never closed a running
  session's journal). **Adapted**: the user's text said "copied back when
  the server stops"; the harnesses read a session's journal right after
  the session while the server keeps running (`arena_map_test.py`'s replay
  check, the `live.sh` scripts, `ab_template.sh`), so the copy is made as
  soon as the server closes the journal, and `scripts/arena_map_test.py`
  waits up to 20 s for an external server's journal that was not there (or
  has changed) since the case started - by name and mtime, so an earlier
  case's journal, seconds old, never stands in for this one's. No server
  option was needed (`journal_dir` is one). The deployed server is
  unchanged (its journal is on its Linux file system).
- **(B)(ii)** `input_buffer` (`session/timeline.hpp`) keeps each stored
  frame's arrival (`submit(first_tick, frames, arrival)`; the server's
  stamp is GameNetworkingSockets' receive time, `server.cpp`
  `handle_session_input`, as before), and `take(tick, fresh, source,
  cutoff)`: a catch-up step - one that runs more than a tick past its
  deadline - passes the tick's deadline as the cutoff, every other step
  `time_point::max()`. Against a cutoff, a frame that arrived by it is used
  however late the step runs; one that arrived after it is late exactly as
  at the deadline - counted stale, not applied at its tick, and the
  substitute only from the first later take whose cutoff is past its
  arrival (`late_by_arrival()` counts them). A straggler for a tick already
  taken waits in the same list for the next take, so a frame for 1300 that
  arrives during the stall is not the substitute of the catch-up ticks
  whose deadlines it missed; nothing reads the substitute between a submit
  and the next take (the lead decision runs after the step's takes).
  `session_runner::run` drains
  the command queue before every step, so the frames received while a step
  (or a stall) held the thread are in their buffers when their tick is
  taken; the sessions are walked by id (a drained command may end one). One
  copy of the rule, unit-tested (`InputBuffer.ArrivalJudgedAgainstEach
  TicksDeadlineAfterAStall`, `...LateFrameBecomesTheSubstituteOnceIt
  ArrivedAndUnstampedFramesAreOnTime`). A frame stored without an arrival
  is on time (every other caller). Outside a stall nothing changes: an
  ordinary step wakes a millisecond or two past its deadline and still
  takes a frame that came in that gap fresh, as before. (The implement
  round applied the deadline to every step, which made those frames late;
  the review found that a change the user had not chosen, so the cutoff is
  now the catch-up's alone.) With patience on (off everywhere deployed) a stalled tick is judged by
  its deadline, not by the earlier step patience would have made.
- **Live** (the one run, `maps/arena-test/b2sx_stall`, B2SW's stall4
  recipe: 2 clients, 150 ms, snapshots off, client 0 `--brake-tap 3:7`,
  stalls 560 ms at 1300 and 660 ms at 1700, level 13; `rB2SX` and the Arch
  server built from this tree): no substitution after either stall (B2SW's
  `b2sw_stall4`, same recipe: 1302..1309 and 1702..1709, n = 8 on both
  clients); starved 0 / 0 (16 / 16), rebases 0 / 0, resyncs 0 / 0, resim
  65 / 65 (187 / 9648). The server journal was written on ext4, copied back
  at the session's end and replayed by the harness 2778/2778 (the run's
  other three cases 2817, 2794, 2809, all matched). Step gaps
  (`B2SW/step_gaps.py`) over 100 ms: the two stalls (576, 661 ms) and the
  session end's deadline wait (336 ms; stall4 311 ms), nothing else. One
  35 s run shows the path; the trial's 9P stalls were 2 in 36122 steps, so
  their absence over a trial is not measured here. **The gate round**
  (the cutoff for catch-up steps only, the stragglers deferred; the same
  recipe, `maps/arena-test/b2sx_stallg`): the server journal of the stalled
  case has no substituted input in 1295-1330 or 1695-1730 (the before run,
  B2SW's stall4: 1302-1309 and 1702-1709 for both players), starved 0 / 0,
  rebases 0 / 0, resyncs 0 / 0, replays 2763/2763 (the other cases 2773,
  2765, 2763); step gaps over 100 ms: the two stalls (577, 678 ms) and the
  session end's wait (321 ms). The same round found the mirror recopying
  every journal every 0.25 s for as long as the server lived (`/mnt/d`
  keeps an mtime to the second, so `-nt` against a copy never turned
  false); it now compares whole seconds and the size.

## As built (step HLX, FULL close-out, 2026-10-31): a beam's Physicalize applied before its same-origin Unphysicalize replaces the body

**Symptom (FULL step HL, run 15, `.local-tools/work/HL/hl_result.txt`).**
In HL's `swing_s1` arm (four headless clients, 150 ms, 10 % loss,
`--loss-seed 1`, snapshots off) client 3's confirmed FINAL rows kept the
mirrors of clients 1 and 2 rolling down the lane for ticks 702..886 (178
rows, 541-545 m off) after the server beamed both balls at 701; mism 0,
resyncs 0 - the client never noticed. Deterministic from the stored capture.

**Cause.** Client 3 applied each beam's Physicalize (origin 701) in a
drain before its same-origin Unphysicalize arrived (journal order P, P, U,
U). `drain_due_events` puts a pair's U before its P only within one drain
(`lifecycle_apply_before`, §3.1); across drains the P came first.
`physicalize_remote` ended the old life only when the entity changed (a
trafo); a beam keeps the entity, and the backend's physicalize is a no-op on
a live body (`physics_state.cpp`, `if (physics->GetPhysicsObject(entity))
return true;`), so the old IVP body rolled on, in the live apply and in every
replay. The late U was then superseded by the protocol-6 guard (equal origin,
`lifecycle_unphysicalize_superseded`) and dropped, uncounted on any line. This
broke the rule protocol 6 already states (`lifecycle.hpp`: receivers must not
depend on copy order). The server's world had the same hole for the uplink
twin (`physics_world::apply_event`: an owner's P applied at its origin and
its U re-stamped later, then superseded by the runner's guard,
`session_runner.cpp`). Pre-existing since protocol 6 / 9; 2 of 84 same-origin
remote pairs at 10 % loss in HL's journals (`HL/pair_order.py`).

**The fix (option (e) of HL's review; the orchestrator ruled it settled by
the design; no protocol, journal or design-rule change).** A Physicalize
always makes the body at its pose:
- client core (`client_core_lifecycle.cpp`, `physicalize_remote`): when the
  remote is physicalized under the same entity and the event is not an LB
  remake, the old life ends first exactly as the pair's U would end it
  (navigation detached above, generation bumped, `unphysicalize_mirror`,
  `physicalized = false`), then the birth makes the new body and bumps the
  generation again - the same platform calls and the same two bumps as the
  in-order U, P. The late U is then superseded correctly. The reconcile
  needs nothing new: the superseded U is logged skipped, so a later rewind
  across the beam revives the A-life and queues the one window event (the
  P), which ends that life again in the replay (swing_s1 c3:
  "revived until tick 701; 1 window event(s) queued", rows exact).
- server world (`physics_world.cpp`, the Physicalize case): when the
  player's clone is the same and still has its body, it is taken out as the
  Unphysicalize case does (`shutdown_all`, `unphysicalize`,
  `physicalized = false`) before the clone is physicalized; logged
  "world: physicalize ... found its body alive: the old body replaced",
  counted (`births_replaced()`).
- the counters: the status line has `superseded=deaths/replaced` after
  `overtaken=` (the guard's dropped remote deaths, `events_superseded_`, and
  the births that replaced a living mirror, `births_replaced_live_`, counted
  once per event, not in the reconcile's re-applications - except a P-first
  birth no replay reached, which `settle_unreplayed_lives` applies live with
  `reconciled` cleared and so counts a second time; diagnostic only), the
  window line the same per window after `ownlife=`.

**Is (e) ever inexact?** Not for a beam: the P carries the whole
replacement, so it no longer matters whether or when its U arrives (a U that
never comes is the same case). Every world runs the same rule, so worlds
agree. The P senders were checked: a beam sends U then P of one origin
(`apply_requested_beam`), a respawn's P follows its death; the bare-P sender
`re_report_own_birth` (a birth the server never made) means "the body is
made here" too and is replaced alike on the server and every receiver. The
`own_body_backstop` re-report (`check_own_body`; deleted at FULL step R6V) fires only when the
server's snapshot has no row for the ball, i.e. the server has no body, so
the branch does not fire for it on the server, nor on a receiver that
followed the server (and it never fires with snapshots off).
**The one inexact ordering (review of HLX, queued under roadmap §4 item 5):**
a death and a LATER respawn of different origins - the server applies U(800)
and P(850) in order and relays both - at a receiver that lost every early
copy of U(800) and got P(850) first. HLX replaces the old body at 850; the
late U(800) is then dropped by `lifecycle_unphysicalize_superseded`
(`lifecycle.hpp`, origins only, 800 <= 850). That receiver alone keeps the
old body alive 800..850, where the server and every receiver that got the U
in time have none (it can collide with their balls). Before HLX it was
never removed at all (unbounded); HLX bounds it to the death-to-respawn
interval. The server-side twin (an owner's uplink reordered) agrees in every
world, because every receiver follows the server's applied order. Option
(a) (hold the P until its U) does not apply - it only pairs same-origin
events - and applying such a late U in a rewind at its stamp instead of
dropping it changes the protocol-6 supersede rule (design §3.1), a design
rule and so the user's choice. Not seen: `.local-tools/work/HLX/cross_origin.py`
(a remote U journalled after a P of a newer origin) finds 0 of 294 remote
Unphysicalize events in the client journals of `hl_full` (swing_s0,
swing_s1, l11_cross, hlxsmoke_s1) and `core_ref_s13`.
One state difference remains between the orders: the superseded U does not
set `remote_last_unphysicalize_`, which only the overtaken-birth rule reads
(an older P relayed after the pair). The server never relays an older P
whose applied tick falls after the pair (`lifecycle_acceptance::accept`
supersedes it); one applied before the pair is made in its window and
replaced at the pair's tick by the P (in order: taken out by the re-applied
death and made again by the P), so the body set is the same. Analysed, not
exercised by any capture.

**Verified (2026-10-31).** Unit test
`SessionClientCoreLifecycle.ABeamsPhysicalizeBeforeItsUnphysicalizeReplacesTheBody`
(U4 P4 U5 P5 in one drain vs P4 | P5 | U4 | U5 vs U4 | P4 | P5 | U5: the same
remotes, generations 3, one take-out and one make per body at 123, the
take-out first, the same per-body mirror operations; it fails without the
fix); 729/729 tests. HL's stored `swing_s1` capture replayed with the HLX
build (`HL/final_replay.sh rHLX hl_full hlx swing_s1`, `judge_arm.sh`): every
client's confirmed FINAL rows bit-equal to the server (own, mirror, bodies,
body set 0), client 3 `superseded=2/2`, both split pairs replaced. The
server's half (adapted: the row asked for a test; `physics_world` needs the
game engine and does not link into the message tests, so the runner's path
is checked by reading `session_runner.cpp` ~1357-1369 and by this
edited-journal replay instead): `swing_s1`'s server journal with client 1's
U at 701 left out
(`.local-tools/work/HLX/strip_event.py`: what the runner journals when it
supersedes a re-stamped U) replays 674/4024 matched, first divergence 701,
1242 checkpoint mismatches with the previous build (rRTR) and 4024/4024, 0
mismatches with HLX, which logs the replacement once; the unedited journal
4024/4024 with no replacement. The 13 `core_ref_s13` server journals replay
exact with HLX, no replacement. The wire check `rRTR rHLX` on `core_ref_s13`:
ALL SAME (44 sends SAME, 45 journals IDENTICAL), as predicted - no split pair
in any of its arms (`pair_order.py`). One live smoke run (swing recipe,
`--loss-seed 1`, Arch server rebuilt from this tree): a split pair occurred
again (client 3, 701), `superseded=1/1`, PASS, every client's confirmed FINAL
rows bit-equal to the server.

**On retail (FULL step RTD, 2026-11-01; `scripts/retail/rtd_hlx`,
`.local-tools/work/RTD/rtd_retail.py hlx`).** L11, two copies on SAXC's x86
build, the Arch server of HEAD (snapshots off, trafo_inputs and placement
on), `net lag 150` both ways on both copies, each pinned to E-cores of its
own: client 1 beamed in place eight times (`beam ... at T`, T = 419, 529,
..., 1189), with a blackout across six of the stamps on client 2's downlink
(150 / 250 / 350 / 200 / 300 / 400 ms from T + 6 + 2k) and across two on
client 1's uplink (250 / 350 ms from T - 4). `pair_order.py` on the client
journals: 8 same-origin remote pairs (client 2's view of client 1), **0
split** - no P,P,U,U order occurred, so `superseded=0/0`; HLX's unit test
and its `swing_s1` replay carry the split case. Every FINAL row of both
clients bit-equal to the server (own, mirror, 15/15 mechanisms), server
1486/1486. The 250 ms blackout at 537 also covered every repeat of the
beam stamped 529: the reliable copy arrived at client 2's local 587, past
the 48-tick reach - "a late lifecycle event beyond the reach", one resync
(608), rows bit-equal after it: the designed fallback (roadmap section 4,
the LCM / DX8 class), not HLX's.

## As built (step R6M, FULL close-out, 2026-11-01): plan step 21's measurement - how long retail tolerates its own ball absent

Measurement only, at HEAD 7a13ac5c; no code changed. Binaries: the x86 mod
of HEAD (incremental build, `CLIENT-BUILD-OK`, physics_RT id
`ballanced-36c04e3d24c9+bmmo-7a13ac5c6508`), the Arch server rebuilt from
HEAD with the same id (`ARCH-BUILD-OK`), HEAD's x64 SimTool and headless
client (`build-agent-rOVX`). Every live run snapshots off unless named;
trafo_inputs and placement_clones on. Tools and outputs:
`.local-tools/work/R6M/` (`off_arms.sh` + `off_judge.py` the SimTool arms,
`hl_arms.sh` the headless arms, `r6m_retail.py` the retail sessions,
`own_rows.py` the own ball around each birth, `hull_cmp_notime.py` OWNB's
hull comparison without the clock fields); the retail runs under
`scripts/retail/r6m_*`.

**The absence measured.** R6H's forced own move (`session force-own-move
<n> [after <k>]`, headless `--force-own-move N[:K]`) runs
`move_own_birth`'s capability path: the scripted body taken out at the
birth T, the records without it T..T'-1, the bridge's make at T' = T + n.
K = 0: the client lives through the absence (LIVE, the "live frame of T'"
branch of `move_own_birth`); K >= n: the absence lies inside the rewind's
replay (REPLAYED). K = n is the shortest rewind that contains the absence
(the make lands at the replay's end); a real echo arrives after the client
passed T' (§3.3), so its case is K > n: the rewind makes the ball at T'
inside the replay and then replays T'..now from records made with the
scripted body. Both were measured (the K > n arms headless only, added at
the step's gate round). The take-out and the make are the ones R6P will
use. The rewind's depth is K + 1 (its anchor is the birth's tick - 1,
`client_core_lifecycle.cpp` 3089-3101), so the bound is on K, not on n: a
move is replayed when K + 1 <= 48, whatever n <= K.

| arm | absences n | result |
|---|---|---|
| retail L1 solo, LIVE (spawn and respawns, `key up` held across) | 1, 2, 4, 8, 16, 32, 48 | all pass |
| retail L1 solo, REPLAYED (`after n`) | 1, 2, 4, 8, 16, 32 | all pass |
| retail L1 solo, REPLAYED | 48 | refused: `past the reach (depth 49)` (the reach is 48; a replayed n needs a rewind of n + 1) |
| retail L11 two copies, copy 2's trafo-made stone ball (507 -> 554), REPLAYED | 47 (rewind 48) | pass; copy 1 unaffected |
| headless arena (core_ref_s14/solo1 replayed offline from its capture), LIVE | 1, 2, 4, 8, 16, 32, 48 | all pass |
| headless, REPLAYED K = n | 1, 2, 4, 8, 16, 32, 47 | all pass; 48:48 refused past the reach |
| headless, REPLAYED K > n (n:K = 1:10, 4:20, 8:20, 16:40, 30:47, 1:47; `hl_arms_kgtn.sh`, `--final-checkpoints`) | 1, 4, 8, 16, 30 | all pass: made at T', rewind 11-48 ticks, no own FINAL row inside T..T'-1, no death / BallOff; the made ball's FINAL rows follow the control's at the same offset after the make (`hl_kgtn_judge.py`: 4 mm at +0, 0.11 m at +10, 0.30 m at +40, \|v\| +0.03..0.09 - the earlier force start below); 2:48 and 8:48 refused `past the reach (depth 49)` |
| SimTool arena `--ball-off` at the spawn and at the trafo birth | 1, 2, 4, 8, 16, 32, 48 | pass (the mid-life situations below) |

**The largest absence tolerated:** live 48 (every n measured); replayed,
any n whose rewind fits the reach - depth = (now - T) + 1 <= 48, i.e.
n <= K <= 47, and with a real echo K = n + the relay lag, so n <= 47 - lag.
The reach, not a script, is the bound. Every absence the reach admits
passes, so R6P needs no depth cap beyond the reach `move_own_birth`
already checks (its past-the-reach resync); risk row 3 and open question
5 are answered.

**What "pass" was, per retail arm.**

- The process survives (no dump; every session collected normally).
- Controllable within one tick of the make: the own ball's LOCAL rows
  (checkpoints every 2 ticks) accelerate from the first row after T'
  at the same rate as a scripted birth's, the key force starting 2 ticks
  earlier relative to the birth (below) (|v| +1.29..+1.44 per 2 ticks under the
  held key; `r6m_l1d` live 8/16/32/48: T' = 229/628/1043/1474). A replayed
  move shows as the one jump §3.3 predicts: the rows up to T' are the live
  life's, the next ones the made ball's from the birth pose (rep16: 620,
  then 622 back at (54.166, 17.562, -153.072) with the same acceleration).
- No death, trafo or checkpoint on the bodiless ball: no BallOff, Dead or
  Unphysicalize inside T..T'-1 in any part; each made ball's roll-off death
  came 185 ticks after its make (414-229, 813-628, 1228-1043), the
  control's 406-221 = 185.
- No OnPhysicalize / OnUnphysicalize for the bridge's take-out and make:
  one "own ball ... physicalized" report per scripted birth, none for the
  make (`core_host_on_physicalize` reports no own birth there).
- Camera: the screenshots 0.4 s after each birth (inside the live 48
  absence too: `r6m_l1d/live48_c1.bmp`) show the camera at the resetpoint
  with the entity at its birth pose, as the control's.
- IVP bookkeeping (`BMMO_HULL_PROBE=Ball` on copy 1) - PARTLY met: the
  made body's `created` line and first PSI equal a scripted birth's at the
  same pose
  (entries, overlap element, hull, mass; clock fields left out); from the
  second PSI on the hull's gradient fields and the velocity differ, and
  with those left out the rest (core, entries, overlap element) is equal on
  all 9 lines (`hull_cmp_nohull.py`): the made ball moves faster - the key
  force reaches it at T', where a scripted birth gets it at T + 2 (v_x 0.887
  one row earlier). On retail the made body is driven from T' by the core's
  navigation replica ("own ball ... driven by the navigation replica",
  `cbq=0`); in SimTool the SetPhysicsForce entry queued during the absence
  is applied at the make; the headless K > n arms show the same (FINAL
  rows |v| 0.632 against the control's 0.605 at the make's own row).
  Within one host the ball is controllable in both cases, but the made
  ball is not the scripted birth's ball shifted by n: its force starts 2
  ticks earlier relative to the birth. OPEN CHECK for R6D / R6P (R6D answered it by reading - the
  server's moved clone gets the force at T' too, from the same input frames: core design "As built (step
  R6D)"; R6P's A6m arm measures it): whether
  the server's moved birth at T' starts the key force at T' (as the
  client's make does) or at T' + 2 (as a scripted birth does). The forced
  move arms cannot show it - the server moved nothing in them - and if
  the two sides differ, a real moved birth starts with a 2-tick force
  offset between the worlds.
- Not measured: sound. A bridge-made own body lacks Sound_Manager's
  listener blocks (BEAMF-S, R6D item 1 / R6B); neither the screenshot nor
  the log shows it, so it stays R6B's check.

**SimTool, the arena record (`work/a4/r0_keys.bmrc`, the only key record on
disk), `--ball-off FROM FROM+n` at nine situations (the spawn 205, mid-roll
with the key held 400, a key change inside the absence 615, before the
checkpoint 775 and 786, before the stone trafo 912 and at its stone birth
1083 (beam 240 6 43 at 900), on the see-saw 1060 (beam 405 7 43 at 1000),
in the fan 515 (beam 80 6 43 at 500)), n = 1..48, each against its
control.** No crash, assert or "is not physicalized" warning in any of the
63 arms. Queued while the ball had no body: the outside producers 26418
(Physics WakeUp), 26414 and 26432 (SetPhysicsForce) of Gameplay_Ingame, a
fourth one 27403 (Physics WakeUp, Gameplay_Ingame, at the spawn), and at
the trafo birth four Sound_Manager blocks 6271, 6471, 6489, 6507 (Physics
Collision Detection); every one was consumed at the make (`sim=1`, the
queue empty). No trigger fired on the bodiless entity: the stone trafo did
not fire while the entity sat above it (the control fires at 917; with the
absence it fires 10 ticks after the make), the checkpoint at x = 220 was
not taken, the see-saw only froze (n = 48). At the spawn and the trafo
birth (the situations a moved birth produces) the made ball accelerated as
the control's. The mid-life situations are EG0's `--ball-off` model, not a
moved birth: the body comes back at rest and the held key's persistent
SetPhysicsForce went with the old body, so the ball stays still until the
key changes (`roll` n = 8: 0.01 m/s, asleep from 429 until the key edge at
620). A moved own birth never takes a body out mid-life (its absence starts
at the birth, where the script's force comes after it and is queued), and
the retail and headless REPLAYED arms drive the made ball from the
records; so this is the model's limit, not a failure of the R6P path.

**The own_body_backstop measurement (core design §7.1 decision 2; the backstop was deleted at FULL step R6V).**
`check_own_body` (client_core_messages.cpp:130) runs on every applied
non-resync snapshot; with snapshots off those are only the names-only full
snapshots of body-set changes (9-13 a session), while it needs 30
consecutive snapshots without the own row. `backstops=0` in every retail
session since DEP (`dep_smoke`, `rtr_*`, `r6h_*`, `ovx_r1/r2`,
`srtj_ctl_h`, every R6M session). With `session drop-birth` on copy 2's
beam Physicalize (the ball beamed onto the start plate at 340 and living
on): snapshots on (`r6m_snap2`, server 2/66) - "the server has no body for
our ball (1); Physicalize re-reported at tick 402", backstops=1; snapshots
off (`r6m_snapoff`) - backstops=0, and the server had no ball for that
player from 340 to the session's end (581 client checkpoint ticks without a
server row). A dropped birth is answered by the echo deadline instead
(check_own_body waits while a birth awaits its echo); a beam's Physicalize
is not. What to do with it is the user's decision (R6V builds it); the
options as measured: keep it as is (it fires only with snapshots on), let
the body-set change's full snapshot alone trigger the re-report with
snapshots off, or retire it (events are reliable and a rejection is
echoed).

**Adaptations.**
- The slot-13 hack the plan names is retired (step CMAP): the arena is a
  BML+ custom map; SimTool takes `--map`.
- The 9.11 recordings (`mech_L*.bmrc`) are not on disk and `--ball-off`
  exists only in `--replay` (not in `--drop`), so the offline arms ran on
  the arena record alone; L1-L13 offline are not covered (the retail L1 /
  L11 and headless arena arms run the same path live).
- `--describe-script` dumps a level's graph at load, not a state mid-run;
  the "cells that differ" check was made on what the scripts did instead
  (the event log, the PreSimulate queue, the ball identity, the triggers),
  per arm against its control.
- `force-own-move` needs a running core: armed before the room start it is
  refused ("no session core runs"), so the L11 copy-2 spawn arm did not
  run; the L1 arms cover live spawns.
- L1 gives four balls and the held key spends one every ~6 s, so the L1
  arms ran as four sessions of four births.
- Retail launches: 10 (8 informative; `r6m_l1b` stopped on a driver error
  after 57 s, `r6m_l1c` is the Level 1 reset below) against the
  plan's 4-6.

**Seen, outside this measurement: no respawn after a Level 1 reset in
`r6m_l1c` - RK3R's known limit, not a regression.** In `r6m_l1c` the
automation sent `restart` (the ESC menu's Reset Level path) 11 times in
one session. Each time it ran PauseLevel, PreResetLevel, PostResetLevel,
StartLevel and "input live from tick 401 (ingame script 0, paused 0)",
and then there was no own spawn and no BallNavActive for 25 s. The cause
is RK3R (FULL plan §1, decided 2026-10-29: not handled - the tutorial is
skipped only at the server's countdown restart): a Level 1 reset
re-activates Gameplay_Tutorial, and its opening waits for the player's
Enter before the camera and the ball come. The evidence:
- `esck_l1` (2026-10-31): `pausechain` reads `tutorial_script=0` before
  its reset and `tutorial_script=1` after it.
- `esck_l1` and `rt2_l1trace2` (2026-10-30): the ball came back only
  after a `key enter`. CamNavActive came at the press, and the spawn
  followed about 3 s later (`esck_l1` tick 1646, `rt2_l1trace2` tick 933).
- `rt2_l1` and `rt2_l1esc` (2026-10-30, before ESCX): the same
  no-respawn. Their only Enter after the reset came while the pause menu
  was open, and before ESCX the pause menu deactivates the tutorial.
- `r6m_l1c` sent no `key enter` at all (`r6m_retail.py` holds `key up`).
So the behaviour is the same on the builds before this step, and nothing
needs queueing beyond RK3R and its recording check RK3R-rec. The "ingame
script 0" in the "input live" line is read at StartLevel, before the reset
re-activates the scripts (`esck_l1`'s later `pausechain` reads
`ingame_script=1`); it is not the cause. No in-session restart on the
arena exists on disk to compare with: `cmap_reload_fix`'s `restart` ran
after its session 1 had ended, and its respawn at 212 is session 2's
start.
