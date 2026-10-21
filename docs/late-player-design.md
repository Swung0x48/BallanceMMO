# The late player: corrections, a per-player lead, and no lifecycle barrier

Design for three changes to the physics session (protocol 7): the server
tells a late player which of its inputs it substituted, a player whose
lateness persists gets a longer lead of its own, and the one-second
Physicalize barrier goes. Written against `collision-overhaul` at 0dd381a.
Line numbers are from that revision. Revision 2 answers two reviews; §8 lists
what each finding changed.

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

### 1.4 Retail mod (`BallanceMMOClient/session`)

The retail mod has no input rewind yet: `rewind_to` is never called.

**Applied now.** `physics_session_apply_queues` (physics_session_client.cpp:
2187-2228) parses v3 and reads the held section the same way:

- epoch check, dedupe, and resolution against a sent ring;
- amends `s.own_inputs` (physics_session.hpp:174-175) and, through
  `amend_recorded_input`, the record;
- journals the frame as SUBSTITUTED and counts it.

Any snapshot rollback anchored inside the held run (`on_snapshot`, 3131-3171)
then replays the server's input instead of the player's. The starvation
detector (3185-3211) reads `acked` from the relay header instead of from
snapshots, so it also works with snapshots off. A held tick past the reach, or
a `held_total` gap, requests a resync exactly as the headless client does.

**Deferred to the input-rewind card.** The rewind itself:

- factor the `input_at` lambda out of `physics_session_rollback` (3152-3167);
- call `rewind_to(T-1)` from apply_queues;
- keep `too_far` from turning an own rewind into a resync (2073-2074).

Until that card lands, a retail player with snapshots off is corrected only
by a resync.

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

### 2.1 Measurement on the server

No arrival time exists today. `handle_session_input` (server.cpp:1309-1313)
captures `steady_clock::now()` and passes it through
`runner_->submit_input` (session_runner.cpp:366-375) into the posted lambda.

**Samples are taken per datagram, from its newest frame.** Every uplink
datagram of player p gives one sample, `s = (deadline(t_new) - arrival) /
tick`, where `t_new` is the newest frame tick it carries. A datagram whose
`t_new` is not newer than one already sampled is ignored (a duplicate or a
reordered datagram). This measures the path and the client's send schedule.
Loss gives no sample at all, and a frame recovered from the redundancy window
is never a sample. So loss cannot look like latency, and no sample waits for
a "lost" classification. The meter is `lateness_meter` (new,
`BallanceMMOCommon/include/session/lateness.hpp`, pure and unit testable).

| quantity | value |
|---|---|
| reference | `r10_p`, `r01_p` = q10 and q01 of the samples over the player's first four windows of 66 samples after an assignment, taken only from windows where at most 10 % of samples fall after the deadline. Afterwards it is **frozen**: it is never re-measured downward, it moves only by an acked delta (`r += applied`), and it is measured anew only after the next assignment. |
| reference that cannot be set | after 8 windows without a qualifying one (a member structurally late from its start), `r10_p` and `r01_p` are set to the median of the other members' references, and the raise rule below applies against them |
| miss | a sample below `r01_p - 1`: that frame arrived after the player's patience (§2.6) |
| raise | in the trailing 66 samples **more than half are misses**, at every stepped tick for **132 consecutive ticks**. Then `k = ceil(r10_p - q10)` over those 66, at least 2. |
| lower | `q05 > r10_p + 6` over the trailing 66 samples, at every tick for 660 consecutive ticks; `k = -floor(q05 - r10_p - 1)`, and the player's total offset never goes below 0 |
| cooldown | no decision until `E + |k|·2 + 132` ticks after an adjustment; samples before E are discarded |

**What "persistent" means.** A window has a majority of misses only after 33
late samples. So the raise needs at least 131 consecutive ticks of lateness,
about two seconds. A one-second delay spike, a CPU stall with its ≤ 33-frame
catch-up, or a blackout (which gives no samples) can never raise the lead,
wherever it falls relative to a window boundary. Arms A9 and A9c (§5.2) check
this.

A raise of `r10_p - q10` puts the player's arrivals back where its reference
had them, not at the deadline. The lower threshold is wider and slower than
the raise, and together with the cooldown that hysteresis keeps the lead from
oscillating.

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
  its patience within the cooldown allows one more raise, with k measured
  again. If the arrivals are still not restored after that, the server stops
  deciding for that player until its next assignment and logs it once.
- A raise is capped by the total lead cap (80 ticks, §2.5) and, until step
  14, by the Sector margin (§2.9). A capped raise is applied as far as the
  cap allows. The server does not take another decision at the cap.

So a client that cannot or will not apply the raise costs the server nothing
after one decision, and the others nothing at all.

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
- The starvation detector moves from 132 to 264 frames and is suppressed
  while an adjustment is outstanding, so the server's raise, which needs 132
  ticks, always lands first.

### 2.5 Leads, caps and reach

| item | today | change |
|---|---|---|
| start base, `session_start_tick_base` (late_tick.hpp:90-94) | the capped `late_tick_lead_ticks`, clamped to `44 - input_delay` | **unchanged.** The start base and the A0 baseline do not move. |
| first late join | `late_tick_lead_ticks(D, ping_peak)`, round trip capped at 12, lead at 34 (:40-41, 50-54) | a new `late_join_lead_ticks(D, rtt_ms) = D + ceil(rtt) + 2`, capped only at a sanity bound of 80. The joiner's frames then arrive on time from its first tick. |
| resync | the same capped formula from `ping_peak` (server.cpp:1098-1101) | re-anchor at `L_eff_p` = the lead of the player's last assignment + `lead_offset[p]`, capped at 80. That is the lead measured to work, in the same frame of reference the offset was measured in. |
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
lead is still raised by the full `k`: control of one's own ball comes first,
and without the raise every input of the player is substituted. Two things
then happen, both to the late client only:

- its reach grows with its measured lag, up to 120;
- whatever lies past even that reach is repaired the way it is today, by a
  snapshot or a resync of that player.

Whether the deeper reach fits the CPU is open question 1, measured in plan
step 12.

### 2.6 The session stops waiting for one player

Part one makes a late frame harmless to the server's state. It does not stop
the session from waiting until `deadline(T)` for it. That is cost 2 in §3.6:
up to 24 ticks of extra relay lag for everyone at 300 ms.

**Patience.**

`patience_p(T) = min(deadline(T), deadline(T) - (r01_p - 1)·tick)`.

It is anchored on the reference's q01 and not on q10. Anchored on q10, about
10 % of a stationary, jittery player's arrivals would be cut off where the
deadline would have taken them fresh. On q01 less one tick, the margin is
sized per player from its own spread, `r10_p - r01_p + 1` ticks above its
usual tail.

The early-step test becomes: every player is `settled(T)`, or `now >=
patience_p(T)`, or its patience is waived (below).

**The streak waiver.** Once p has been substituted at T, its patience is
waived from T+1 on. The step then waits only for the others, and for frames
of p that happen to be settled already. The waiver ends with the first
sample of p whose slack is at least `r01_p - 1`: an arrival inside its
patience. Patience then applies again from the next tick.

- During a blackout or persistent lateness, only the first tick of the
  episode waits for p. Without the waiver, every tick of the episode would
  wait `patience_p`, which lies later than the others' own step time by up
  to `(r10_p - r01_p) + 1` ticks plus p's structural lag behind them.
- The waiver does not end at p's first *fresh* frame. Take the member whose
  arrivals are structurally the session's last (the worst ping, which
  `input_delay` was sized for). With no patience its frames always arrive
  just after the step, so it would never be fresh again, and it would be
  substituted for good.

**The reference is frozen** (§2.1). Re-measured after every adjustment, a
raise that was capped, or a client that never applied one, would leave the
arrivals late. The reference would then move down, and patience would move
later for everyone, for good. Frozen, a raise that did not help leaves only p
missing.

**A member without statistics** (a late joiner in its first four windows) has
as its patience the latest patience among the members that do have
statistics, that is, the others' schedule. Only at session start, when nobody
has statistics, is patience the deadline, which is today's behaviour.

**Waking up.** The runner sleeps for `until_due` (the deadline) only
(session_runner.cpp:625, 633), and is woken by `post()` (:83). A blacked-out
player posts nothing, so the step would wait for the next arrival or the
20 ms cap. `sleep_for` becomes the minimum over unsettled, unwaived players
of `patience_p(T) - now`, besides `until_due`.

**Not adopted: capping every member's patience at the others' schedule.** For
a member with statistics, a hard cap `patience_p <= max_{q≠p} patience_q`
would substitute the structurally latest member on every tick. Its reference
comes from that same distribution, so the meter would never see a shift and
would never raise it. That member would stay substituted for good. The frozen
reference and the waiver bound the drift instead. The cap is kept only for
members without statistics.

**Behind a flag until the lead works.** Patience moves the cut-off from the
deadline to the player's own tail. A player whose +300 ms frames still beat
the deadline today (slack about 34) would be substituted for as long as the
lateness lasts, until part two raises its lead. So `physics_patience` stays
off in deployments until plan step 11 lands (§6).

### 2.7 Lowering the lead again

Lowering exists because the extra lead costs the player: deeper rollbacks,
more CPU, remote balls shown later. The lower policy is in §2.1. It moves no
tick range (`held_from = 0`, `E = N`), the client slews at 0.9x, and the
reference moves by the acked delta. A lowered player's arrivals still beat
its patience, because the threshold leaves `r10_p + 1` of slack.

### 2.8 Resync, rebase and late join

- A resync re-anchors at `L_eff_p` (§2.5). It resets that player's meter
  samples, `held_total` and epoch, and keeps `lead_offset[p]`. The reference
  is measured anew.
- A SessionLead whose `epoch` is older than the client's base is dropped.
- A client rebase (more than `33 + |slew|` behind) still asks for a resync.
  The CPU stall that causes it is the client's own.
- A late joiner starts at `late_join_lead_ticks` with offset 0, and is
  measured like anyone else once it has statistics.

### 2.9 Why the others are unaffected

The changes touch nothing the others see:

- `input_delay`, `sector_lead`, the scheduler, the start base and every other
  buffer are untouched;
- the reset is per buffer, and `settled()` keeps it out of the step timing;
- patience costs the others at most the first tick of an episode, and only a
  frozen reference can move it;
- the relay of the late player's frames to the others is what it was: the
  applied frames;
- the late player's own Sector stamps land `k` ticks *further* ahead for
  everyone else, which only adds margin.

The others' Sector events, in turn, reach the late client `k` ticks later on
its clock, so they are late *there*. Today the headless client does not
rewind for a late remote Sector (session_client.cpp:3015-3017). It starts the
sector a frame late, which leaves that client's mechanisms out of phase.
Plan step 14 builds the rewind for it. Until then, `k` is capped at the late
client's measured Sector margin.

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
so a substituted birth frame is a re-stamped birth. Patience anchored on
`r01 - 1` cuts off about 1 % of a player's arrivals at most, and births are a
few per minute. Arms A0 and A0j require 0 moved births and substitutions
within HEAD + 0.5 % of ticks (§5.2).

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
| 2 | one missing frame puts the session on deadlines | removed: patience per player with the streak waiver (§2.6), and `settled()` for reset ranges (§2.3) |
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
| 2 | client cannot apply it, or the raise is capped | no SessionLead without `CAP_LEAD`; at most two raises without restored arrivals, then none; frozen reference, so the others' patience never moves |
| 2 | oscillation | hysteresis (raise 2 s, lower 10 s, different thresholds), cooldown, frozen reference |
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

Client flags (session_client.cpp):

- C6 `--hold-event TYPE:N:MS|drop`, which gates `send_event` and both repeat
  paths;
- C7 `--stall-ms T:MS`, a CPU stall at tick T.

Counters to add in code:

- client: C1 (a `window:` line every 60 ticks), C2-C5 (`subst=`, `lead_adj=`,
  the rewind reason, the SUBSTITUTED journal flag);
- server: S1-S5, which are per-player substituted/runs/stale/resets/lead
  lines with the meter's samples and decisions, a relayed held-range line with
  source ticks, a lead/reset note, a step-without-ball line, and the event
  intake→send delay.

### 5.2 Arms and pass criteria

Every arm has 4 clients, 150 ms each way on all, snapshots effectively off
(`run L 100000 100000 4.2`), 60 s, `--drive`, checkpoints every 2 ticks,
the event on client 2 (not the host) at T = 1500. Lane and swing variants are
used as in C. A0 (the control) is run twice, and its spread is the noise
band. Every arm is run at HEAD first.

| arm | event |
|---|---|
| A0 / A0j | control; ±3 ticks of jitter on every link, whole run |
| A1 / A2 | uplink blackout 300 / 600 ms |
| A3 / A3L | both-way blackout 200 / 1000 ms (A3L past the reach: the late client is reported only) |
| A3x | A3 at 50 ms each way, where the notices stay inside the reach |
| A4 | client 2 at 10 % loss, bursts of 8, whole run |
| A5 / A5u / A5s | +300 ms both ways / uplink only / +100 ms both ways, persistent |
| A5d | +300 ms uplink at 1500, back at 2700 |
| A6 | `--hold-event physicalize:2:{300,1500,drop}` |
| A6m | hold 300 ms at 50 ms each way, barrier off: the move inside the reach |
| A6b | barrier off, 300 ms: hold a Physicalize and the following Unphysicalize together; and two same-type trafos (wood → stone → wood) in one hold |
| A7 | clients 2 and 3, uplink blackouts of 300 ms, overlapping by 100 ms |
| A8 | client 4 joins late at T = 1500 on a 250 ms each way link |
| A9 / A9c | a +300 ms uplink delay spike of 1 s (late, not lost) / a 500 ms CPU stall on client 2 |

**The other clients, every arm:**

- own-ball error 0.0000 m in B, D and A;
- mechanism error within the control band in B, D and A;
- in D, rollbacks per class within the control band, and input rewinds that
  name the late ball no more often than in the control;
- relay lag, overall and per source player: p99 in D no higher than the
  control band's upper edge (one-sided), and above it only in the first two
  ticks of the episode;
- relay_late Δ 0, lead_adj 0, resyncs 0;
- their mirror of the late ball from T_off+60 on ≤ 0.001 m;
- the server journal replays exactly.

**A0 and A0j:** substituted ticks per player ≤ HEAD + 0.5 % of the ticks,
moved births 0, lead_adj 0.

**Late client, bursts (A1, A2, A3x, A4, A7, A9):**

- the server's substituted ticks lie only inside [T_on, T_off + 2];
- `subst recv` equals the server's count, and `subst late` is 0;
- own ball 0.0000 from T_off+48 on;
- no snap-class rollback and no RECEIVED checkpoint in [T_on, T_off+48]
  (otherwise the run is marked "masked" and rerun);
- lead_adj 0.

**A3** (150 ms): the earliest notices arrive at relay lag + 13 = 45-57 ticks,
past the reach. So `subst recv + subst late` equals the server's count,
there is exactly one resync if `subst late > 0`, and the own ball is 0.0000
48 ticks after it.

**A9c:** lead_adj 0. The stall's ticks are substituted and corrected, or the
client rebases and resyncs itself. Either way, the others pass.

**A5u and A5s:**

- exactly one lead_adj, with K in [ceil(up_ms/15), +4];
- exactly one buffer reset, whose E (S3 note) satisfies E ≥ the first
  on-time frame after the slew;
- substitutions only in [T_on, E];
- own ball 0.0000 from E+48;
- resyncs 0;
- the session's input delay printed once;
- the others: lead_adj 0, relay lag in A within the control band.

**A5 (both ways)** is reported. At this base the late client's relay lag
exceeds even its deeper reach (§2.5), and its relay_late is the measured cost
to that player.

**A5d:** one raise and one lower, and the others are unaffected throughout.

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

**A8:** the joiner's lead is `D + ceil(rtt) + 2`, as printed. Its
`subst late` is 0 after its first four windows. The others' relay lag stays
within the band during those windows (the patience cap for members without
statistics).

## 6. Implementation plan

Each step is one implement-and-review workflow, and is closed by its
measurement. Flags `physics_patience` and `physics_lead_adjust` exist from
steps 9 and 10 and stay off in deployments until step 11 closes.

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

## 7. Open questions

1. **The deeper per-client reach.** Can a late client afford a reach of up to
   120 ticks? Step 12 measures its CPU. If it cannot, the cap on `k` is that
   client's CPU budget, and the rest of its lateness stays substituted and
   corrected by part one. At 150 ms each way the relay lag already takes 32-44
   of 48, which is why A5 is reported and not passed.
2. **Session-wide sizing.** Should `input_delay` and the start base be sized
   from a robust statistic rather than the worst ping, now that part two can
   lift one slow member on its own? The principles freeze them during a
   session, not how they are chosen.
3. **The patience tail under loss.** Under 10 % loss a frame's first arrival
   can be the second datagram carrying it (+1 tick). Is `r01 - 1` enough
   then? A4 decides; the birth stamp lead (§3.4) is the fallback.
4. **BodyRevived.** Does it still do anything under the sector union, or can
   it be retired?
5. **Retail input rewind.** Parts one and three on retail are only partial
   until the input-rewind card lands. Whether the retail scripts tolerate a
   multi-tick own absence is measured in step 21.

## 8. Review record (revision 2)

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
