# Rewind cost: the hold-last re-prediction, and the cost of each remaining rewind

Status: design, 2026-10-25. Step RC1 (the hold-last re-prediction) built and
committed with hold-last by the user's ruling of 2026-10-27 (§11; "As built
(step RC1)" at the end: its drift check, a FAIL under the step's rule at the
build, was settled by the pinned method of Measurement RC1d and the user's own
play, Measurement RC1e). RC6, the tail prediction refinement (R6 / R7), is not
adopted (§8). RC2 is not built (the user's choice), RC3 is built (B5RX),
RC4 is built (B6RC); RC5 is not built (the user left it to the
orchestrator on 2026-10-28, who chose option (c), §11); the close-out ran
with snapshots off at step RCX (the user's decision of 2026-10-28, §11),
stopped for the user (H4 passes, H6 does not, "As built (step RCX)"), and
is passed by the user's ruling on H6 of 2026-10-28: H6 is judged by its
absolute limits and `physics.trafo_inputs` is on by default since step
B9DX (§11). This document is roadmap §4 item 10 (the rewind-cost
optimization); its first step was pulled ahead of trafo T3 by the user's
decision of 2026-10-25 (§11).

Base: HEAD 49e2cc8. Line numbers are HEAD's plus RC1's own edits. The profile
this design rests on is step CPUPROF's ("The rewind-cost profile
(2026-10-25)", in `trafo-replication-design.md` §8, on the main line since
step TR3b merged T3; its roadmap ranking, item 10 "Profiled" on the parked
branch `wip/trafo-t3-merged`, is §1 here); its helpers
and outputs are `.local-tools/work/CPUPROF/` (`prof.py`, `ceil.py`,
`stale_tail.py`, `reasons.py`, `out/`) and its runs `maps/arena-test/cpuprof_*`.

Path key:

| key | file |
|---|---|
| IR | `BallanceMMOCommon/include/session/input_relay.hpp` |
| CF | `BallanceMMOCommon/src/session/client_core_frame.cpp` |
| CL | `BallanceMMOCommon/src/session/client_core_lifecycle.cpp` |
| CM | `BallanceMMOCommon/src/session/client_core_messages.cpp` |
| RB | `BallanceMMOCommon/include/session/rollback.hpp` |
| PSC | `BallanceMMOClient/session/physics_session_client.cpp` (legacy retail path) |

## 1. The problem

With the trafo as an input (trafo T3), H1 at 150 ms with snapshots off starves
0-26 own frames per client (control 0) during the ~1 s of rewinds after four
simultaneous trafos; with snapshots at interval 2 the snapshot rollbacks make
H4 / H6 resync on 3-4 clients. CPUPROF measured where a frame that falls
behind spends its time (H1 at 150 ms, option on, the frames with a rewind,
four clients):

- 94-95 % of such a frame is rewinds; it costs 37-43 ms (11.6-13.5 ms with the
  trafo option off) against a 15.15 ms budget; 1.09-1.22 rewinds per such
  frame, 23-28 ticks deep.
- A replayed tick costs 1.23-1.27 ms (0.41-0.47 off), a restore 4.7-5.4 ms
  (0.86-1.24 off). A capture costs about 400 us + 4 us per awake + 4.6 us per
  sleeping movable body; the IVP step 4.3 us per awake body only.
- **Why one rewind per frame, as deep as the relay lag.** Of the 308 input
  corrections that triggered H1's rewinds, 0 disagreed on the keys: 151 were a
  newborn remote ball's `flags 1 -> 5` (predicted PHYSICALIZED, relayed
  PHYSICALIZED | NAV_ACTIVE), 125 `no input recorded`, 32 `flags 0 -> 5`. In
  100 % of them the corrected tick was that ball's newest relayed tick, so the
  rewind was lag + 1 deep (20 ticks at 150 ms), one per frame for about the lag
  in frames after every change of any remote's input (a birth, a key change).
  In the CPU cliff (A5u at 150 ms, three respawns within 15 ticks) the same
  mechanism also runs on the late player's key changes, 37-40 ticks deep.
- **The cause.** The live predictor drives a mirror with its newest relayed
  frame (`remote.input`: CM:378-381, CL:553-560; `live_drive`, CL:463-474) -
  hold-last. A replay's feed (RB:1435-1452, `feed`) asks
  `recorded_input_at` (CF:473-501), which for a tick past the remote's newest
  relayed frame returned false - `relay_frame_or_earlier` (IR:345) returns
  null for the tail - and the feed then fell back to `recorded->inputs`, the
  prediction recorded live BEFORE the change arrived, re-simulated the tail
  with it and re-captured it as the record (RB:1721). So every later frame that
  repeats the newest one disagreed with the stale record
  (`note_input_correction`, CM:392-460: `recorded_input` + `same_frame`) and
  asked for its own rewind.

The ranked candidates and their measured ceilings (CPUPROF, H1 option on, as
a share of the frames with a rewind; in brackets of the whole window):

| | candidate | ceiling | kind |
|---|---|---|---|
| RC1 | the hold-last re-prediction (§2) | 63.9-66.3 % (43.0-49.3 %); option off 76.4-78.9 % (51.8-61.4 %); the cliff 57.1-66.2 % (52.1-64.1 %) | cuts the NUMBER of rewinds (85-87 % of H1's input rewinds, 68-75 % of the cliff's) |
| RC2 | an incremental capture that reuses sleeping bodies' state | 22.2-24.3 % (21.5-23.4 %); 19-22 % off, 27 % in the cliff | cuts the cost of each rewind |
| RC3 | no body-set full snapshot while snapshots are off | 8.7-10.7 % (6.5-7.9 %) | cost, but the snapshots are a correction today: own drift check |
| RC4 | the dormant-script encoding (trafo design 9.4) | 5.2-5.3 % (4.8 %) | cost |
| RC5 | an incremental restore of the blob (sleeping bodies) | 2.8-4.2 % (1.8-3.1 %) | cost |

Not removable without changing what is simulated: the awake pieces' IVP step
(15.6-17.8 %) and the replayed trafo scripts' frame (3-5 %). RC1 cuts the
rate; RC2, RC4 and RC5 the cost of each remaining rewind, so they compose
multiplicatively.

## 2. The hold-last re-prediction (step RC1)

### 2.1 The rule

In a replay, a remote's tick past its newest relayed frame is fed that newest
frame - the frame the live predictor drives the tail with - and the
re-captured record holds it, so a later arrival equal to it is no news:

| the tick, for a remote's ring of relayed frames | fed (before RC1) | fed (RC1) |
|---|---|---|
| a relayed frame | that frame | that frame |
| a hole: relayed frames on both sides | the nearest earlier frame | the nearest earlier frame (unchanged) |
| past the newest relayed frame (the tail) | the record's prediction (made before the newest frame arrived) | **the newest relayed frame** |
| before the first relayed frame, or an empty ring | the record's prediction, or nothing | the same (unchanged) |
| the own ball | its exact record | unchanged |

`relay_frame_or_newest` (IR:353-367) is the rule: `relay_frame_or_earlier`'s
frame, else for a tick at or past the ring's newest that newest frame, else
null. The legacy `relay_frame_or_earlier` is unchanged (PSC:3269 uses it; the
legacy retail path gets nothing new).

### 2.2 The site: one rule for every rewind kind

`recorded_input_at` (CF:473-501) is the `input_at` of every rewind the core
runs: the input rewind (`replay_corrected_inputs`, CF:73-81), the forced
rewind (CF:124-134), the sector rewind (CF:313-323), the lifecycle rewind
(CL:201-207, CL:1542-1543), the snapshot rollback (CM:153-158) and the exact
resync's re-step (`client_core_resync.cpp`:415-417). Its remote branch now
calls `relay_frame_or_newest`; nothing else changed. The engine
(`restore_and_replay`) is untouched: its feed already prefers `input_at` for a
remote and records what it fed (RB:1435-1452, RB:1721).

**One rule with the live predictor.** `live_drive` (CL:463-474) drives a live
tick with the ring's frame for that tick, else `remote.input`, the newest
relayed frame by tick (CM:378-381: an older, reordered frame never replaces
it; CL:553-560: the pending frames moved into a new mirror's ring set it to
their newest). Every frame stored in the ring updates `remote.input` when it
is the newest, and no path clears one without the other, so past the ring's
newest frame `remote.input` is the ring's newest frame: the two rules agree
on the tail. `SessionClientCoreFrame.TheReplayHoldsTheNewestRelayedFrameSoARepeatIsNoNews`
pins it: past the newest frame `recorded_input_at` returns `live_drive`'s
frame at every tick, before and after a later arrival. On a hole the two
differ by design (live: the newest; a replay: the nearest earlier) - a live
step meets a hole only when the client runs behind the relay.

### 2.3 Births

A tick before the remote's first relayed frame keeps what it has (the birth
state, or no input): that is the one rewind a birth needs. A remote born
inside the replay (not named by the records; RB:1520-1565) is fed through the
same `input_at` once `replay_remotes` names it, so its tail is the newest
relayed frame too; the `carried` map (RB:1425, 1553) still covers a newborn
whose ring is empty. The ring keeps frames of an earlier lifetime (CL:549-551),
so a newborn's tail before its own first frame is fed the newest frame of the
player, which is what the live predictor drives it with as well (CL:574).

### 2.4 The window, the reach, the counters

Untouched: the 48-tick reach and `window_rewind_reach`, `relay_late_`,
`relay_bodiless_`, `relay_birth_frozen_`, `feeds_next` (CM:398-438), the relay
window and its reassembly. `note_input_correction` still compares an arriving
frame with the record of its tick; what changes is the record: after a rewind
the tail holds the newest frame, and a repeat finds itself recorded.

### 2.5 What is recorded

The rollback records' `inputs` (RB:1721) hold the newest frame for the
re-predicted tail. Nothing new on the wire or in the journal; no protocol or
journal version. Client journals differ from a pre-RC1 build's at the
re-predicted ticks and after them (§7, check 4); the server is not touched.

## 3. The other candidates (later steps)

- **RC2, an incremental capture for sleeping bodies** (ceiling 22-24 %): a
  sleeping body's per-body reads and blob bytes are the same as the previous
  record's while it sleeps; reuse them, keyed by the body and its sleep
  generation. Exactness: the record must still restore bit-exact (the
  savestate checks of `ivp-savestate`), and a body woken by a write must not
  reuse the sleeping copy.
  **Not built, the user's choice 2026-10-28** (option (c) of step B5RC's
  stop: "按推荐继续"; "As built (step B5RX)", item RC2S, at the end): neither
  the per-body blob bytes nor a sleep generation exist, so RC2 as written
  needs a format change; the two forms kept for later are recorded there.
- **RC3, no body-set full snapshot while snapshots are off** (8.7-10.7 %):
  the lifecycle full snapshots a snapshots-off session still receives. They
  are today a correction the client applies, so dropping them is not a pure
  cost cut: it needs its own drift check (the same as RC1's, §7).
  **Built at step B5RX, snapshot-free by default** (the user, 2026-10-28:
  "不对，RC3要保留快照？现在应该尽可能假设快照不存在来做"): a snapshots-off
  session takes the body-set full snapshot for its names and rows only; the
  drifts it used to correct are gaps for step SFD ("As built (step B5RX)",
  item RC3A).
- **RC4, the dormant-script encoding** (trafo design 9.4; at most 5.2-5.3 %):
  the 32 trafo scripts' state, written compactly while they are dormant.
  **Built at step B6RC** ("As built (step B6RC)", item RC4): a trafo script
  at its set's built state is one byte of record v3, known from a ledger
  the trafo frame, the inputs and the restores keep.
- **RC5, an incremental restore** (2.8-4.2 %): skip writing a sleeping body
  whose blob entry equals the world's.
  **Not built** (stopped at step B6RC for a choice, "As built (step B6RC)",
  item RC5; the user left it to the orchestrator on 2026-10-28, who chose
  option (c), §11): the restore tears the whole world's collision state
  down and rebuilds it from world-wide numbers, so skipping a sleeping body
  is a selective teardown, and knowing it unchanged needs a wake counter
  (IVP) or a per-object blob (a protocol change) - RC2's cause.

## 4. Rollback interplay

RC1 changes only what a replay feeds a remote past its newest relayed frame;
the restore, the births' remakes, the script replay, the lifecycle and sector
handling, the resync's re-step and the records' bodies are as before. A
rewind for a real change of a remote's input still happens once (the frame
differs from the hold-last record); a rewind for a birth still happens once
(before the first relayed frame the old behaviour stands).

## 5. Wire, protocol and journal

None. Protocol 11 / journal v5 unchanged.

## 6. What must not change

The server (it never depends on a client's prediction: `git diff --stat`
names no server file); the own ball's feed; the legacy retail path
(`relay_frame_or_earlier`); a solo world (no remote: bit-identical journal).

## 7. Checks (step RC1, quick tier)

1. Unit tests: `RelayFrameOrNewest.TheTailTakesTheNewestFrameHolesAndTheStartAsBefore`
   (input_relay_test.cpp), `RollbackEngine.TheTailPastARemotesNewestRelayedFrameIsFedThatFrameAndRecordedSo`
   (rollback_engine_test.cpp: tail, hole, before the first frame, a remote
   born inside the replay), `SessionClientCoreFrame.TheReplayHoldsTheNewestRelayedFrameSoARepeatIsNoNews`
   (session_client_core_test.cpp: a repeat is no correction, a different
   frame is; the tail is `live_drive`'s frame).
2. GCC syntax check, x64 and x86 builds; the Arch server rebuilt from the tree.
3. Solo, snapshots off, against `core_ref_t3off/solo`: IDENTICAL.
4. The wire check against `core_ref_t3off` (old build against new): a
   multi-client journal may differ; every first difference must be a remote
   ball's row at a tick the new rule fed differently (past that remote's
   newest relayed frame as of the rewind that re-predicted it) or a consequence
   after such a tick, and a send may differ only after such a tick.
5. Live A/B, same batch, snapshots off, four clients, captures on, the same
   Arch server: the six arms of `core_ref_t3off`, H1 at 150 ms with the option
   off, the cliff. Input rewinds and replayed ticks, the worst lag, and **the
   drift check** (`.local-tools/work/RC1/prediction_error.py`: every remote
   ball at every LOCAL checkpoint against the server's row).
6. Every new capture replays byte-identical with the new build.
7. CPU parity (one-sided).
8. The headless reference set re-captured as `core_ref_rc1`.

## 8. Steps

| step | what | checks |
|---|---|---|
| RC1 | the hold-last re-prediction (§2) | §7; quick tier |
| RC2 | incremental capture for sleeping bodies - **not built, the user's choice 2026-10-28** (the blob has no per-body bytes and IVP no sleep generation; options (a) / (b) kept for later: "As built (step B5RX)", item RC2S) | the savestate exactness set, the wire check (ALL SAME: no behaviour change), CPU parity, H1 at 150 ms |
| RC3 | no body-set full snapshot while snapshots are off - **built at B5RX, the default on every host** (the user's correction of 2026-10-28; the drifts it exposes go to step SFD) | its own drift check, the snapshots-off arms |
| RC4 | the dormant-script encoding (trafo design 9.4) - **built at B6RC** | script replay exactness, the wire check |
| RC5 | incremental restore - **not built: stopped at B6RC, left to the orchestrator by the user (2026-10-28), who chose option (c), like RC2** (the restore's whole-world teardown and world-wide numbers: a selective teardown, a wake counter or a per-object blob; "As built (step B6RC)", item RC5) | the savestate checks, the wire check |
| RC6 | **not adopted at RC1** (the user's ruling of 2026-10-27, quoted in §11); R6 and R7 measured (RC1d, RC1e) and kept as a possible later refinement if human rapid-trigger play shows periods. (History: proposed by Measurement RC1d; ruled 2026-10-27 "R6 直接并入 RC1", then the R7 variant measured, Measurement RC1e: R7 not selected, the choice between R6, R7 and hold-last went back to the user, who chose hold-last.) What the refinement would be, as specified when it was proposed: the tail prediction refinement: hold-last replaced past a remote's newest relayed frame by **R6** (per-key pattern-repeat: each key bit's own period repeated, the other keys of the newest frame by R4's history-adaptive hold, k = 5; §9.2; R5, the whole-frame pattern-repeat, is the measured fallback choice), one function in IR next to `relay_frame_or_newest` (IR:353-367) called from both sites (`recorded_input_at`'s tail branch, CF:494; `live_drive`, CL:467-470); its history window fixed relative to the newest frame (the 128 ticks before it - not the ring's cap `kRelayRing`, IR:47, so a later change of the cap cannot change a prediction), no allocation on the path (the measured build's per-key `std::vector` goes), and the periods found once per arrival, not per tail tick | unit tests of the function (a 1:1 and a 1:3 lateral alternation, an UP/DOWN 1:1 and 2:1 alternation, a 1-tick DOWN every 10 ticks repeated, a direct LEFT -> RIGHT switch, a hold with and without complete holds, a 6-tick brake, flags as hold-last, holes, the start and a birth unchanged) and a session test pinning the two sites on the tail; the wire check against `core_ref_rc1` (every first difference attributed to a tail tick the rule fed differently); solo IDENTICAL; **the paired drift check with the own keys pinned** (`.local-tools/work/RC1d/gaps/pinned_replay.py`, every rule's replay of the same captures; R1 replays byte-identical on every cell) on the RC1d cells with these **required cells at both lags (resync_at150 and resync_at150_down)**: the native steer d0; the 1-tick lateral taps t1x1 (rapid-trigger 1:1 alternation) and t1x3; the brake cells d0_brake and t1x1_brake (a 6-tick DOWN in every 90 UP ticks) and d0_ud1x9 (a 1-tick DOWN in every 10: the 1-tick brake tap); the UP/DOWN fast switches d0_ud1x2 (UP UP DOWN) and d0_ud1x1 (UP/DOWN 1:1); the holds d5 / d10 / d15 and d10_brake (direct LEFT -> RIGHT switches: 405 in d10, Measurement RC1d). Criteria: (1) pooled mean no worse than hold-last's on every required cell and no client more than 5 % worse; (2) on the hold and brake cells (d0, d5, d10, d15, d0_brake, d10_brake) input rewinds per real change no more than hold-last's + 0.1, on the tap and UP/DOWN cells no more input rewinds than hold-last; (3) replayed ticks reported per required cell next to hold-last's. **As measured, R6 (and R5) fail (2) on resync_at150_down d0: 0.96 against 0.80 per change, replayed ticks +21 % (42893 -> 51716), all of it on the mirrors of the lowered player (Measurement RC1d (e)); every other required cell passes (largest excess +0.06).** RC6 is therefore not passable as measured unless the user accepts that cell or RC6 removes the excess. [Checked after the fix-up round, 2026-10-27, `gaps/record_model.py`: a physics-free model of the engine's records (a tick keeps the prediction made when it was last simulated, live or in the last rewind) reproduces the cell's rewinds on the lowered player's mirror (client 1: R1 232, R4 / R5 / R6 299; measured 231 / 296). The excess is all lateral: corrections where the rule had released a lateral key the player still held go 187 -> 266, the opposite kind 157 -> 136, UP / DOWN corrections 20 in every rule. So the excess is R4's fallback releasing a lateral key early at max(5, H - r) on the native steer's irregular long holds, and holding the forward / backward keys (named here before this check) would not remove it; a fix has to change the lateral release, not measured.] The live A/B (snapshots off) on the required cells, t1x1, d0_ud1x2 and d0_ud1x9 at least three sessions per rule at each lag (one session is not evidence of starvation either way); CPU parity, against the replayed ticks of (3) | quick tier |
| close-out | H4 and H6 of the trafo design §7.3, **with snapshots off** (the user, 2026-10-28: "改成关快照"; replaces decision (b) of 2026-10-25, which ran them with snapshots on; §11), against T3's snapshots-off CPU numbers as the baseline. **Run at step RCX: H4 passes, H6 fails, stopped for the user ("As built (step RCX)"); passed by the user's ruling of 2026-10-28 (option (a): H6 judged by its absolute limits, starved within the band and the server's p99 within the budget; H6's numbers stay recorded in RCX's note), `physics.trafo_inputs` on by default since B9DX (§11)** | quick tier (a stage-internal close-out, the user's 15-minute rule; the second pair and the retail run are deferred to the stage close-out) |

## 9. Open questions

1. **Should a birth predict NAV_ACTIVE?** The first relayed frame of every
   birth corrects `flags 1 -> 5` (or `0 -> 5`, or no input): one rewind per
   birth, lag + 1 deep, that RC1 keeps by design (§2.3). Predicting
   PHYSICALIZED | NAV_ACTIVE with no keys from a mirror's birth would save it
   when the owner's first frames carry no keys; it is a change of the live
   predictor too (both sides must stay one rule) and needs its own drift check.
2. **Oscillating inputs and a long tail** (found at RC1, "As built"). The
   `--keep-lane` drivers of the harness add LEFT / RIGHT every ~6 ticks
   (a bang-bang lane keeper). Holding one lateral key for a whole re-predicted
   tail of 40+ ticks (the down arm's relay lag) drives the mirror 2-5 m
   sideways by the present tick, where the pre-RC1 stale tail - a delayed copy
   of the oscillation - nets out; and every real change is a correction
   against the hold-last (37 real-change rewinds in the cliff's respawn window
   on client 2 against 10 before, which mostly hit the stale tail by
   coincidence). On the aggregate RC1 is better in every arm (§ As built);
   [Added at RC1d: that verdict rests on RC1's and RC1c's paired drift
   tables, which re-ran the harness steer on the predicted own ball and so
   drove the replayed own ball with keys the captured server never applied
   (Measurement RC1d, gap (b)). With the own keys pinned it holds for the
   input rewinds and the CPU (R1 2.6x / 3.4x fewer input rewinds than the
   stale tail on d0 at 20 / 42 ticks; the stale tail's live runs starved on
   every client), not for the drift at the 42-tick lag: the stale tail's
   error is 0.84 of hold-last's there (1.09 at 20 ticks), its live d0 runs
   had the lower mean on both arms (0.379 / 0.404 m at 20, 0.434 / 0.883 m
   at 42) and under UP/DOWN fast switches it is far better than hold-last
   (0.21-0.70). So the RC1 ruling is a ruling on rewinds, not on drift.]
   whether a decaying prediction (hold the forward / backward keys, drop the
   lateral ones after k ticks, in the live predictor and the replay alike) is
   worth its own step is open. Real players hold keys longer than the bot.
   (Contested by the user, §11 2026-10-25/26: top players tap within one
   tick, alternate with rapid-trigger keyboards and brake-tap; those cases
   are required.) **The rules measured at RC1d** (the tail = a tick past a
   remote's newest relayed frame, d = tick - newest >= 1; flags always as
   hold-last; holes, the start and the own ball unchanged; one function at
   both sites, the replay's `recorded_input_at` and the live `live_drive`;
   the arena's leaf bits 0 UP, 1 DOWN, 2 / 3 lateral):
   - R0 the old stale tail (`relay_frame_or_earlier`: the record's
     prediction made before the newest frame arrived; rK0's rule);
   - R1 hold-last (RC1): the newest frame;
   - R2 fixed lateral decay: UP / DOWN held, the lateral bits released when
     d > k (k = 10);
   - R3 run-adaptive: each key of the newest frame held while d <= r + k,
     r = its run of consecutive relayed ticks up to the newest (cap 128; k = 5);
   - R4 history-adaptive: each key held while d <= max(k, H - r), H the
     median of that key's last three complete holds in the ring (both edges
     relayed, contiguous ticks), hold-last for a key with none (k = 5);
   - R5 pattern-repeat: the smallest period p in 2..16 such that the
     contiguous relayed run ending at the newest is at least 4p ticks long,
     keys[t] == keys[t - p] over its newest 3p ticks and the newest period
     is not constant: tick newest + d takes the keys of newest - p + 1 +
     (d - 1) mod p; none found: R4 (k = 5).
   - R6 per-key pattern-repeat (built at RC1d's fix-up round, after the
     1-tick brake cell showed R4 releasing UP after max(k, H - r) where UP
     is interrupted by a 1-tick DOWN): R5's period search done for each key
     bit on its own (the bit periodic over the newest 3p ticks, not constant
     over its newest period); a bit with a period takes its own pattern, a
     key of the newest frame without one R4 (k = 5).
   - R7 (measured at RC1e, the user's "先试一个修侧向释放的变体", §11
     2026-10-27): R6 whose fallback, for a key of the newest frame with no
     period, holds the key for the whole tail once its run r >= H; hold-last
     for a key with no complete hold; else held while d <= max(5, H - r) -
     as `TAS/tas_screen.py`'s `predict()` codes it. Measurement RC1e: R7 is
     not selected (it removes most of R6's rewind excess but loses R6's drift
     on the cells it was built for).
   Results and the recommendation (R6 as step RC6, §8; R5 the measured
   alternative): "Measurement RC1d".
   [Ruled 2026-10-27 (§11): RC1 is committed with hold-last; R6 and R7 are
   not adopted. On the user's own play (Measurement RC1e) hold-last, R6 and
   R7 drift within about 2 % of each other while R6 costs +6-11 % input
   rewinds (pinned) and +24 % (modelled); R6 / R7 win only on exactly
   periodic tapping, which no human data has shown (the user's round had no
   period at all, `TRIAL1` round 1). The question stays open for a later
   refinement if human rapid-trigger play shows periods.]
3. Whether RC3's snapshots are still needed once RC1 and the world-agreement
   gaps are in (the snapshot fallback stays until the known drift sources are
   fixed: the user's decision of 2026-10-24).
   [Answered by the user, 2026-10-28, verbatim: "不对，RC3要保留快照？现在应该
   尽可能假设快照不存在来做" (no - RC3 keeps the snapshots? From now on work as
   far as possible as if snapshots did not exist). This supersedes, for this
   work, the 2026-10-24 rule: RC3 is on by default (step B5RX), and the drifts
   the body-set snapshots hid are gaps to root-cause and fix (step SFD), not
   reasons to keep the snapshots.]

## 11. Decisions

- **2026-10-25 (user), verbatim:** "先把"重放沿用远端旧预测尾巴"这个缺陷修掉（hold-last
  重预测，客户端本地，带自己的精确性和漂移检查），然后 T3 按原门槛 H1 (c) 重新过，过了就提交，再继续
  T4、T5、S5-S11。" RC1 comes before trafo T3; T3 is re-gated under its
  original H1 (c) at TR3b.
- **2026-10-25 (user), decision (b)** of the trafo design's fix-up round
  ("好的，就b"): T3 is judged with snapshots off; the CPU with snapshots on -
  H4 and H6 as the trafo design §7.3 states them - becomes this optimization's
  close-out (§8), against T3's snapshots-off CPU numbers as its baseline. The
  snapshot fallback itself stays.
- **2026-10-25/26 (user), verbatim, three messages** (they widened step RC1d
  to 1-tick taps, rapid-trigger alternation, fast switches and brake taps; they
  make those REQUIRED cases for the rule the user picks and for RC6's gate):
  1. "\"真实玩家按键时长比这个机器人长得多\"这一点不一定是真的。高级速通玩家很可能会快速切换按键来达到调转方向和急停等目的"
  2. "其实高端人类玩家可以做到1tick内的轻押的，而且很常见（例如在单轨/路面边缘上走，他们甚至会用rapid trigger键盘）。所以必须要考虑这种情况的。"
  3. "rd1d在干嘛，如果你在纠结的是要不要在实现里考虑这个1tick的情况，我可以告诉你肯定需要的"
- **2026-10-27 (user), verbatim, answering the RC1d questions** (recorded at
  step RC1e):
  1. the rule: "R6 直接并入 RC1" (R6 folded into RC1 as the RC6 row's clean
     implementation);
  2. the failing cell (resync_at150_down d0): "先试一个修侧向释放的变体", the
     option chosen: "只做一个针对侧向键过早松开的变体：先用物理无关的模型在 30 个格子上筛，再做钉住按键的配对重放。修不掉或在别处变差，就带着数字回来问你。"
     (-> the variant R7, Measurement RC1e: it got worse elsewhere, so the
     numbers go back to the user);
  3. the verification budget: "感觉我最多能接受15分钟的验证时间";
  4. the TAS files: "那个tas不是真人高手亲自打的。那些tas文件仅作为参考给你测试用，不要对它过拟合"
     (a reference only: never a tuning target or a required gate cell);
  5. the resume: "好，按缩短的验证步骤继续推进链".
- **2026-10-27 (user), verbatim, the RC1 ruling** (after Measurement RC1e put
  the choice between R6, R7 and hold-last back with the user). Asked which
  tail rule RC1 takes, the user asked which rule the round they had just
  played used (`.local-tools/work/TRIAL1`, round 1: hold-last on both retail
  copies, level 11) and answered: "刚才我打的那一轮用的是什么？先接受那个吧"
  (that one: hold-last); then: "Commit RC1 with hold-last, then fix RL1 right
  after, then continue the chain under the 15-minute rule." The evidence the
  user saw (Measurement RC1e): on the user's real play hold-last, R6 and R7
  drift within about 2 % of each other (pinned pooled mean R6 / R1 0.99 /
  0.98, R7 / R1 1.00 / 1.00 at 20 / 42 ticks), while R6 costs +6-11 % input
  rewinds pinned (864 / 809, 782 / 707) and +24 % in the physics-free model
  of the whole round (1842 / 1480); R6 / R7 win only on exactly periodic
  tapping (the bot cells t1x1, d0_ud*), which no human data has shown yet -
  the user's round had no period at all (R6 found none, even up to p = 40).
  -> RC1 is committed with hold-last; RC6 (R6 / R7) is not adopted (§8);
  RL1 (the lone-resync lag, `TRIAL1` round 1) follows RC1 directly.

- **2026-10-28 (user), verbatim, B5RC's three stops:** "按推荐继续" (go on as
  recommended): RC2 option (c), not built now; R12B not built, the
  headless-only savestate diff's time left out of the rewind cost meter
  (late-player design §2.5, "As built (step B5RX)"); and, correcting the
  recommendation for RC3 (option (c), the switch default on): "不对，RC3要保留快照？
  现在应该尽可能假设快照不存在来做" - RC3 is option (a), snapshot-free by
  default. Landed at step B5RX.
- **2026-10-28 (user), verbatim, the close-out's condition:** asked whether
  the close-out should follow RC3's snapshot-free default, the user
  answered "改成关快照" (switch it to snapshots off). This REPLACES decision
  (b) of 2026-10-25 above as far as the close-out goes: H4 / H6 of the trafo
  design §7.3 are run with snapshots off (the same condition as their
  baseline, T3's snapshots-off numbers), with RC3's default (no
  `--body-set-corrections`). Run at step RCX ("As built (step RCX)").
- **2026-10-28 (user), verbatim, the answers to the orchestrator's seven open
  decisions after RCX** (the whole message is quoted in the roadmap, §4
  item 3; these are this design's):
  - RCX's stop, answer 1: "打开" (turn it on) - option (a): H6 is judged by
    its absolute limits (starved within the control's band, the server's
    step p99 within the 15.15 ms budget, both met at RCX), the close-out is
    passed and `physics.trafo_inputs` is on by default (step B9DX, item
    TIO, `BallanceMMOServer/config_manager.hpp:91`). The trade-off the user
    accepted: a session with the trafo inputs costs every client about
    twice the rewind CPU (RCX's table); the x86 retail frame time of a trafo
    run stays deferred to the stage close-out.
  - RC5 (B6RC's stop), answer 4: "你决定" (you decide) - the orchestrator
    chose option (c): RC5 is not built, like RC2. The reason: the restore
    tears down and rebuilds the whole world's collision state from
    world-wide numbers (the shared overlap tree, the hull min-lists in
    recorded order, the event queue), so (a) needs RC2's per-object blob
    (a protocol bump) and a selective teardown - the track's largest
    exactness risk for at most 2.8-4.2 % - and (b) saves a small, unprofiled
    part of that ceiling at the risk of the `SetWorldMatrix` ulp; neither
    moves factor (1) of RCX's cause (trafos existing at all). Recorded at
    step B9DX (item RC5C).

## As built (step RC1)

**Committed (2026-10-27), with hold-last, by the user's ruling (§11).**

- **What RC1 is.** In every rewind the core runs, a remote's tick past its
  newest relayed frame is fed that newest frame - the live predictor's rule -
  and the re-captured record holds it, so a later relayed frame that repeats
  it is no correction (§2). The defect it fixes (CPUPROF's profile, §1): the
  replay fed that tail the stale prediction recorded before the newest frame
  arrived, so after every change of a remote's input (a birth, a key change)
  each later frame disagreed with the stale record and asked for its own
  rewind - one rewind per frame, lag + 1 deep, for about the lag. Client
  only: no server file, no wire or journal change (protocol 11 / journal v5),
  the legacy retail path untouched (`relay_frame_or_earlier`).
- **Measured** (the Results below; snapshots off, same batch, rK0 -> rRC1):
  input rewinds -83..-87 % on swing and on H1 at 150 ms (option off), -67..-77 %
  on the cliff, -67..-71 % on the resync arms (a5u_lead -70 %), -44 % at 50 ms; no schedule
  rebase and no rebase resync in any arm (the old build rebased and resynced
  on every client of five arms); CPU parity on resync_snapoff 20.3-20.8 s ->
  13.2 s (-36 %).
- **The drift question, settled by the pinned method.** The build's drift
  check (paired, unpinned) was a FAIL under the step's rule on some clients;
  Measurement RC1d showed that method invalid after the first own-key
  difference (gap (b): the replay re-ran the harness steer on the predicted
  own ball) and re-measured with the own keys pinned: hold-last has 2.6x /
  3.4x fewer input rewinds than the old stale tail at 20 / 42 ticks (R0 1852
  / 3614 against R1 705 / 1058 on d0), and the drift splits by lag (R0 / R1
  error 1.09 at 20 ticks - hold-last better - and 0.84 at 42, the bang-bang
  steer's artefact of §9.2). On the user's own retail play (Measurement
  RC1e, `TRIAL1` round 1, driven pinned on the arena) hold-last, R6 and R7
  drift within about 2 % of each other, and R6 costs +6-11 % input rewinds
  pinned and +24 % in the model.
- **The ruling** (verbatim in §11): "先接受那个吧" (the rule of the round the
  user played: hold-last) and "Commit RC1 with hold-last, then fix RL1 right
  after, then continue the chain under the 15-minute rule." RC6 not adopted
  (§8).
- **The commit's gate** (quick, 2026-10-27, 02:44:06-02:50:37, about 6.5
  minutes): the tree's session files equal `rc1_tree.patch` (PATCH-SAME, 9
  files), `map.hpp` clean, `client_core_lifecycle.cpp` no diff; x64 BUILD-OK,
  x86 CLIENT-BUILD-OK; unit tests 508/508; GCC syntax check 28/28 files ok;
  **the equivalence check** `wire_check.sh rRC1d rRC1 rRC1w` (REF
  `core_ref_rc1`, the measured hold-last build against the committed tree's
  slim copy): 36/36 sends SAME, 36/36 journals IDENTICAL, `WIRE-CHECK: ALL
  SAME` - the committed tree is the measured build; solo, snapshots off, one
  live run with the committed build against RC1's Arch server (sha256
  3cfff104...): `e0_compare.py --live` against `core_ref_rc1/solo/solo1`
  IDENTICAL (2383 common TICK ticks, 0 differ; LOCAL checkpoints 1192/1192;
  other records 5994/5994), the harness's server replay 0 checkpoint
  mismatches (`maps/arena-test/rc1_commit_solo/`). No live multi-client run:
  nothing changed since the measured build. `build-agent-rRC1` is now the
  committed tree's slim copy (the first attempt's copy replaced).
- The harness options `--keep-lane-dwell`, `--brake-tap` and
  `--keep-lane-tap` (`BallanceMMOServer/sim/session_client.cpp`, Measurements
  RC1c and RC1d) are committed with RC1: the measurements cite them. Their
  inertness when absent is shown by Measurement RC1d's
  `wire_check.sh rRC1c rRC1d rRC1dw` (the options' tree against RC1c's
  option-less binary, `WIRE-CHECK: ALL SAME`, see Measurement RC1d's harness
  block), not by the wire check above: both binaries of that check carry the
  option code, so it shows only that the committed tree is the measured
  build.
- **Method note** (for every later drift check): a paired drift check -
  the same capture replayed by two rules - is valid only with the own keys
  pinned (`.local-tools/work/RC1d/gaps/pinned_replay.py`); with the harness
  steer re-run on the predicted own ball the replayed client drives keys the
  captured server never applied.

**Code** (keep the edits small for TR3b's cherry-pick of the parked branch,
which also changes CF, RB, CM, the tests, the trafo design and the roadmap):

- IR:353-367: `relay_frame_or_newest` (new, next to `relay_frame_or_earlier`).
- CF:466-501: `recorded_input_at`'s remote branch calls it; its two comments.
- Tests: input_relay_test.cpp:223-242, rollback_engine_test.cpp:768-869,
  session_client_core_test.cpp:119-121 (the friend's `recorded_input_at`) and
  3989-4059.
- No change to RB, CM, CL, the server, the retail mod's own files, the wire
  or the journal. `git diff --stat` names only shared core files and tests.
- Adaptation: the design asked for one function or a test that pins the live
  predictor and the replay; `live_drive` keeps `remote.input` (changing its
  source would also move the live hole case) and the session test pins the
  two on the tail.

**Results** (2026-10-25; old = `build-agent-rK0`, new = `build-agent-rRC1`;
the Arch server rebuilt from this tree, journal build id
`ballanced-3e4289e95f25+bmmo-49e2cc8e0253-dirty`; snapshots off unless named):

- Unit tests 508/508; GCC syntax check every file ok; x64 BUILD-OK, x86
  CLIENT-BUILD-OK.
- Solo against `core_ref_t3off/solo/old1` and `old2`: IDENTICAL (1202/1202
  LOCAL checkpoints, 6046 other records), both new solo runs.
- Wire check `REF=core_ref_t3off wire_check.sh rK0 rRC1 rRC1w`: DIFFERENCES as
  expected (24/24 journals, 17/24 sends), **every one attributed**
  (`.local-tools/work/RC1/attrib_all.sh`, a temporary trace build
  `build-agent-rRC1d` - `diag_patch.py`, reverted - whose replays are
  IDENTICAL to rRC1's): the first differing LOCAL row is at tick 232-256, on
  remote balls whose tail the new rule fed differently from tick 221-233 on
  (the start's births); TICK records first differ at 232-255; the first
  differing send at 255-2973, always after the first changed tail tick.
  The traces are kept in `.local-tools/work/RC1/traces/<arm>/`; the replays
  themselves were deleted once read.
- Live A/B (one run per arm and binary, cpu sampler max total 11.7-20.4 %;
  `maps/arena-test/rc1ab_old/<arm>` and `core_ref_rc1/<arm>`), input rewinds
  per client and replayed ticks (live; paired = the same capture replayed by
  the other binary, `cross_replay.sh`):

  | arm | input rewinds old -> new | replayed ticks | paired (old capture / new capture) | worst lag old / new |
  |---|---|---|---|---|
  | swing, snapshots off | 154/273/269/274 -> 23/47/46/48 (-83 %) | -75 % | -83 % / -87 % | 12.5-15.4 / 6.9-7.7 |
  | swing_snapon | 134/260/252/253 -> 22/36/39/33 (-86 %) | -54 % | -83 % / -89 % | 17.0-33.6 / 4.5-22.7 |
  | a6m_snapoff (50 ms) | 243/319/316/273 -> 150/178/187/128 (-44 %) | -39 % | -44 % / -50 % | 4.8-12.8 / 3.7-6.6 |
  | resync_snapoff | 466/543/632/596 -> 161/160/199/142 (-70 %) | -58 % | -60 % / -64 % | 33.4-65.5 / 13.4-65.5 |
  | a5u_lead | 456/518/508/551 -> 152/162/174/130 (-70 %) | -64 % | -56 % / -66 % | 18.9-32.6 / 4.9-6.9 |
  | resync_at150 | 458/554/601/540 -> 175/189/198/143 (-67 %) | -62 % | -54 % / -62 % | 33.3-33.7 / 10.3-14.5 |
  | resync_at150_down | 666/1039/1162/1007 -> 153/341/350/287 (-71 %) | -28 % | -66 % / -70 % | 33.4-34.0 / 25.3-27.3 |
  | H1 at 150 ms, option off | 148/195/190/182 -> 19/29/31/24 (-84..-87 %) | -75 % | -88 % / -85 % | 12.1-22.9 / 5.0-8.3 |
  | cliff | 519/583/582/599 -> 170/168/189/138 (-67..-77 %) | -46 % | -57 % / -67 % | 33.4-33.7 / 15.9-32.1 |

  The worst lag is the pacer's (`.local-tools/work/RC1/pace_lag.py`, from
  the capture's clock reads; it gives 6.5-10.2 on CPUPROF's cliff run, which
  its diagnostic build measured as 6.6-10.3). Old: every client of the
  resync, resync_at150(_down), a5u and cliff arms rebased its schedule and
  resynced (as the `core_ref_t3off` live runs did); new: no rebase anywhere,
  and the only resyncs are the arms' own (the paused client of
  resync_snapoff, the `--resync-at` client of resync_at150, the lowered
  client of resync_at150_down: its forced resync and one "a substituted own
  frame arrived past the reach") and swing_snapon client 2's "own beam moved
  701 -> 702 ... an own pair, plan step 22, resync" at 10 % loss (the old
  binary replaying the same capture requests it too: pre-existing, plan step
  22). Starved: H1 0/0/0/0 (old 0/5/0/0), cliff 33/5/359/0 (old 614-1051 on
  all four; client 2 is the +300 ms uplink), a5u_lead non-raised clients
  154-216 (old 463-569, the `core_ref_t3off` live run 412-500), resync arms 0
  on the clients without an event (old 237-1505). far / unmatched 0 except
  swing at 10 % loss (far 1/0/0/3 snapshots off, old 1/0/0/1: too-late
  lifecycle snapshots refused, pre-existing). The H1 drop on client 2 (-84 %)
  is a point under the expected -85 %: the remaining rewinds are the births'
  first frames and the lifecycle rewinds (one per real change).
- **The drift check** (`prediction_error.py`, and `paired_drift.py` for the
  paired comparison up to the first differing reliable send, after which the
  captured server answers belong to the other client's events). Live O1 and N1
  are different sessions, and their difference is mostly the session (H1 off:
  rows over 2 m 548-603 old, 1298-1344 new live; the old binary replaying the
  new capture gives 1315-1359, the same). Paired, 72 client pairs, 228 428
  remote-ball rows: mean error new rule 1.616 m against 1.657 m; better in
  every arm's aggregate mean (swing 4.080 / 4.127 off and 3.717 / 3.775 on,
  a6m 0.145 / 0.149, resync 0.193 / 0.253, a5u 0.211 / 0.287, resync_at150
  0.333 / 0.340, resync_at150_down 0.402 / 0.429, H1 6.121 / 6.160, cliff
  0.257 / 0.323); rows over 2 m 8888 against 9541. **Not uniformly**: 10 of
  72 pairs have a mean more than 5 % worse, 4 a p99 (resync_at150 old capture
  client 1: 12.8 against 9.0 m; resync_at150_down new capture client 2: 5.4
  against 4.9 m), and rows over 2 m rise in the two resync_at150 arms (834
  against 761, 1270 against 1008). Cause, traced on resync_at150_down client
  1's mirror of the lowered player (`ball_track.py`): the `--keep-lane`
  driver's LEFT / RIGHT switch every ~6 ticks; with a relay lag of 42 ticks a
  re-predicted tail holds one lateral key for 40 ticks and the mirror ends
  2-5 m sideways at the present tick (z error growing 0.14 m per tick, sign
  alternating with the key held), where the old stale tail - a delayed copy of
  the oscillation - stayed within about 1 m (the old build's replay of that
  capture is kept: `maps/arena-test/core_ref_rc1/resync_at150_down/replay/x_old`).
  The tables: `.local-tools/work/RC1/paired/`, `sum/`, `ab_table.md`. §9.2.
  The per-pair distribution (fix-up round): 31 of 72 pairs more than 5 %
  better in mean against 10 worse, p99 better on 44 against worse on 6 (4 by
  more than 5 %: the two above, cliff new capture client 1 2.40 -> 2.55 m,
  cliff old capture client 0 0.99 -> 1.51 m), geometric-mean ratio new / old
  0.85; the same client and arm can flip between the two captures
  (resync_at150 client 1: 1.18 on the old capture, 0.49 on the new one).
  Seven of the ten worse means are below 0.25 m on both rules. **Verdict:** the
  aggregate improves, but the long-lag arms' tail (rows over 2 m, p99 on two
  clients) is worse for a deterministic, traced reason, not noise; under the
  step's rule ("a regression is a FAIL with its cause, not a waiver") RC1 is
  not committed until the user rules. Options: (a) accept - hold-last is the
  rule the user's decision names, the live predictor already holds the last
  frame on the same tail, and the old replay's advantage was a stale record
  that happened to average a bang-bang bot out - and queue §9.2's decaying
  lateral prediction as its own step; (b) build §9.2 first (it changes the
  live predictor too and needs its own drift check); (c) re-run the drift
  check with a driver that holds lateral keys like a player, to separate the
  bot's artefact from the rule.
- Notes from the reviews (no code change; CM's comment is left alone for
  TR3b's cherry-pick):
  - The "before the first relayed frame" boundary is per player, not per
    life: the ring keeps an earlier lifetime's frames (CL:549-551), so a ball
    born inside a replay is fed the previous life's newest frame on its tail
    (as `live_drive` does, CL:574; §2.3), and "the birth state / no input"
    holds only for an empty ring.
  - `feeds_next` (CM:418-420) still counts a frame as feeding later ticks only
    when a later frame exists; under hold-last the newest frame also feeds the
    tail. A newest frame that arrives for a bodiless or birth-frozen tick,
    with the body present at tick + 1 and no frame after it (a pause, a loss
    burst, a leave), is skipped as bodiless and the tail keeps its old
    prediction until the next arrival or another rewind replays it -
    self-correcting, and per the step's "`feeds_next` untouched".
  - The comment at CM:149-152 ("Only an exact hit counts, like the mod") was
    already untrue for holes and is now untrue for the tail as well.
  - The session test pins the live predictor and the replay on the tail
    after in-order arrivals; a reordered (older) frame arriving after the
    newest is not covered (the ring's newest is unchanged by it, so
    `remote.input` and the tail stay equal by construction).
- Every new live capture replays byte-identical with rRC1 (`wire_check.sh -
  rRC1 rc1self`, REF=core_ref_rc1, nine arms: 36/36 sends SAME, 36/36
  journals IDENTICAL; `e1_bytecheck.py`: BYTE-IDENTICAL-EXCEPT-BUILD-ID, 36
  pairs).
- CPU parity `cpu_parity.sh rK0 rRC1 resync_snapoff 3` (REF now defaults to
  `core_ref_t3off`; `core_ref_r3` is protocol 10 and refused): old
  20.34-20.79 s, new 13.15-13.17 s, PASS (-36 %).
- The reference set `maps/arena-test/core_ref_rc1/` (README there): the nine
  arms' new runs above and two solo runs; `wire_check.sh` defaults to it.
- **Measurement RC1c (2026-10-25)** - option (c) of the drift check: is the
  regression the bang-bang bot's artefact? A measurement; no session code
  changed, nothing here is a ruling. Tools: `.local-tools/work/RC1c/`
  (`dwell.py`, `dwell_pooled.py`, `dwell_summary.py`, `accel.py`,
  `rc1c_run.sh`, `analyze_cell.sh`, `table.py`, `own_keys_cmp.py`,
  `scan_bmrc.py`; tables `ab_table.md`, `paired_table.md`; `sum/`, `paired/`,
  `dwell/`); runs `maps/arena-test/rc1c_<arm>_d<N>_<old|new>/`.
  - **The harness option** (`BallanceMMOServer/sim/session_client.cpp`, the
    one file changed): `--keep-lane-dwell N` (arguments :321, parse :417,
    usage :350); the steer's decision moved unchanged into `keep_lane_key`
    (:2043-2067, returning the leaf, -1 in the deadband, `kLaneSteerOff` :342
    where the steer does not act) and its own region `keep_lane_dwell`
    (:2069-2095, state :3078-3079): every state of the steer (no key, LEFT,
    RIGHT) lasts N frames or more - a press is held N frames, a release
    stays up N frames, a direct LEFT -> RIGHT switch is a change of state like
    any other (adapted: the row names "pressed" and "released"; a direct
    switch is both); where the steer does not act (no drive key, no own body,
    no camera) the key is released and the next press is free. N = 0 or
    absent: `keep_lane_dwell` returns the wanted key, today's steer.
    Proved: `REF=core_ref_rc1 wire_check.sh rRC1 rRC1c rRC1cw`: 36/36 sends
    SAME, 36/36 journals IDENTICAL, `WIRE-CHECK: ALL SAME`; the own input
    rows (the steer's keys as sent) of the resync_at150_down, cliff and
    resync_at150 replays identical between rRC1 and rRC1c, 12/12
    (`own_keys_cmp.py`). Unit tests 508/508; the GCC check (every file ok)
    plus a GCC compile of `session_client.cpp` itself in the Arch tree (object
    only; the server binary untouched, sha256 3cfff104...); x64 BUILD-OK.
    The rerunnable form of that compile is `.local-tools/work/RC1c/gcc_sc.sh`
    (run in WSL Arch: it takes the object's compile command from
    `~/bmmo-build-arch` with `ninja -t commands` and runs it with
    -fsyntax-only; gcc rc=0 on the final file; the first version of the
    script lacked the game's include paths and failed - replaced at the
    review). The dwell counts `frame()` calls, one per tick in every run
    (the server journals show holds of exactly N); `--keep-lane-dwell`
    without `--keep-lane` is inert (the steer is never called) and says
    nothing - pass both.
  - **The old side** had to steer with the dwell too, and rK0 has no such
    option (its replay of a dwell capture would refuse the argument), so the
    old binary is `build-agent-rK0d`: this tree with `recorded_input_at`'s
    remote branch temporarily back on `relay_frame_or_earlier` (the only
    behavioural line of RC1), built once, the file restored byte for byte
    (sha256 checked) and the tree rebuilt. Plainly: that is a temporary edit
    of an RC1 file (`BallanceMMOCommon/src/session/client_core_frame.cpp`),
    reverted within seconds; the file is identical to RC1's
    (`client_core_frame.cpp.rc1tree`) and its diff equals `rc1_tree.patch`. `REF=rc1ab_old
    LABELS="resync_at150_down cliff resync_at150" wire_check.sh rK0 rK0d`:
    12/12 sends SAME, 12/12 journals IDENTICAL - rK0d is rK0 without the
    option.
  - **Part 1, the "real player's" key dwell.** `maps/arena-test/r0_keys_p0.bmrc`
    is **not a human's play**: it is byte-identical to
    `arena_map_test.write_drive_record(level 13, 2400 ticks, player 0, steer
    off)` (the harness's player-0 drive: idle 0-119, DOWN 120-144, then UP
    with a 25-tick DOWN brake every 500; R0 made it once for every R step,
    EG0 and A4 reused it as `r0_keys`). It has **no lateral key at all**;
    forward/back: UP holds median 475 (5 holds, 343-475), DOWN 25 (5),
    every gap 0 (direct UP <-> DOWN switches). The repository and the game
    copies hold no human key record of the arena: the only record written by
    the mod's `record start` (`Downloads/Ballance-MMOTest/Bin/rec_live.bmrc`,
    2026-09-04, BMRC v2, level 1, 2579 frames; who made it is not recorded,
    a hand or the `key down/up` verbs) has three holds - UP 204, LEFT 69, UP
    148 ticks - one lateral hold, not a distribution (and v2: the headless
    client reads v3 only, `tick_record.hpp:88`). The keep-lane bot (dwell 0)
    in RC1's captures, the server journals' applied frames (leaf bits 0 UP,
    1 DOWN, 2/3 lateral; the drive records `input*.bmrc` carry no steer,
    it is added in the client): resync_at150_down lateral holds n 243, median
    3, p10 1, p90 19, 91 % shorter than 20 ticks and 98 % than 42; gaps median
    5 (p90 29, 43 direct switches); cliff holds median 1 (p90 13), 94 % / 99 %
    under 20 / 42, gaps median 6; resync_at150 median 1 (p90 8), 97 % / 99 %,
    gaps median 8. The only long holds are the lowered player's (one
    RIGHT of 617 ticks against the slope). So the bot's pulses are 1-3
    ticks, not "about every 6 ticks". **A driver from a recorded .bmrc
    exists**: `--record FILE` (with `--record-by-tick` to align frame and
    session tick) is exactly that - the harness's own drive is a key record
    - and `--client-arg-for 'i:--record X'` would drive one client from any v3
    record; it was **not used** in part 3: there is no human lateral record
    to drive with (r0_keys_p0 has no lateral key; rec_live is v2 and level 1).
    With no human data the sweep has no "real player's median" point.
  - **Part 3, the runs** (snapshots off, 4 clients, captures on, RC1's Arch
    server `ballanced-3e4289e95f25+bmmo-49e2cc8e0253-dirty` sha256
    3cfff104... for all 14, RC1's recipes plus `--client-arg=--keep-lane-dwell
    --client-arg=N` on every client, old rK0d then new rRC1c per cell, one run
    at a time, cpu sampler max total 13.0-24.5 %; dwell 0 is RC1's own pair
    `rc1ab_old` / `core_ref_rc1`). In the new runs the server's applied
    lateral holds are N ticks (median N, min N but for steer-off edges); the
    old runs' server holds are shorter in places (min 1-15) because those
    clients starved and the server substituted their frames.
    Input rewinds per client, replayed ticks, worst lag (live; `ab_table.md`
    has every column, the paired replays xnew / xold included):

    | arm, dwell | input rewinds old -> new | replayed ticks old -> new | starved old -> new | resyncs / rebases old -> new | worst lag old -> new |
    |---|---|---|---|---|---|
    | resync_at150_down d0 (RC1) | 666/1039/1162/1007 -> 153/341/350/287 | 18147-20027 -> 11977-16518 | 1263-2318 -> 2355/15/17/11 | all / all -> c0 / none | 33.4-34.0 -> 25.3-27.3 |
    | resync_at150_down d15 | 1508/1664/1623/1637 -> 296/270/318/300 | 28209-29061 -> 10060-12744 | 1978-2576 -> 135/0/0/0 | all / c1-c3 -> c0 / none | 28.4-33.5 -> 8.6-16.4 |
    | resync_at150_down d30 | 1469/1512/1497/1517 -> 234/261/308/301 | 27548-27998 -> 8909-10557 | 1747-2549 -> 24/0/0/0 | all / all -> c0 / none | 33.5-34.5 -> 7.6-10.1 |
    | resync_at150_down d60 | 1135/1203/1236/1154 -> 167/188/200/196 | 23061-25959 -> 6623-7415 | 1267-2607 -> 44/0/0/0 | all / all -> c0 / none | 33.3-33.6 -> 8.4-11.0 |
    | cliff d0 (RC1) | 519/583/582/599 -> 170/168/189/138 | 9872-11915 -> 5643-6274 | 614-1051 -> 33/5/359/0 | all / all -> none | 33.4-33.7 -> 15.9-32.1 |
    | cliff d15 | 1415/1463/1548/1525 -> 296/349/359/341 | 22395-23679 -> 11085-14092 | 2148-2823 -> 0/0/904/0 | all / all -> none | 33.3-33.6 -> 10.9-18.2 |
    | cliff d30 | 1333/1393/1369/1330 -> 282/289/299/293 | 22295-23349 -> 10218-10873 | 2061-2662 -> 0/0/513/0 | all / all -> none | 33.3-34.3 -> 9.5-13.9 |
    | resync_at150 d0 (RC1) | 458/554/601/540 -> 175/189/198/143 | 10890-12856 -> 3777-5136 | 237-306 -> 0 | all / all -> c0 / none | 33.3-33.7 -> 10.3-14.5 |
    | resync_at150 d15 | 1667/1769/1725/1798 -> 313/326/367/336 | 26700-28466 -> 7659-8779 | 2078-2185 -> 0 | all / c1-c3 -> c0 / none | 27.2-33.9 -> 5.5-10.2 |
    | resync_at150 d30 | 1472/1529/1599/1535 -> 291/317/303/299 | 27648-28887 -> 6893-7980 | 1696-1836 -> 0 | all / c1-c3 -> c0 / none | 21.8-34.2 -> 6.0-9.5 |

    far and unmatched 0 in every run; the new runs' resyncs are the arms' own
    (c0's `--resync-at`); the cliff's starved client 2 is the +300 ms uplink.
    Mechanism maxima: P_Modul_34_Schiebestein 2.737-2.742 m on
    non-leaders (the known G-c signature, both rules); P_Modul_08_Schaukel
    3.43-3.85 m only in old-rule runs whose clients all resynced (RC1's old
    runs up to 4.4 m likewise); PE_Box_slide 3.20-3.33 m on resync_at150 d15
    new c1 / c3 - it is jointed to the balloon platform (keep-local,
    `resync_rules.hpp`), and on client 1 it parts from the server's at 2114
    when the server's box takes 1.16 m/s with no ball within 8 m: the
    balloon (exception c), as in RC1's own resync_at150 run (3.26 / 3.12 m,
    and the old rule's replay of it the same). Every harness server replay matched (0 checkpoint mismatches).
    **Under player-like holds the old rule's cost explodes** (1135-1798
    input rewinds per client, every client resyncs and starves 1267-2823
    ticks) and RC1 cuts the rewinds by 80-85 % (67-85 % at dwell 0): a held
    key repeats the same frame for the whole hold, and every repeat
    disagreed with the stale record (§1's mechanism); the bot's 1-3-tick
    pulses often matched the stale record by coincidence.
  - **The drift check, paired** (the same capture replayed by the other
    binary, up to the first differing reliable send; both captures pooled,
    8 client pairs per cell; per-client rows in `paired_table.md`):

    | arm | dwell | rows | mean old / new (m) | new / old | rows over 2 m old -> new | pairs worse / better in mean (> 5 %) | p99 worse / better | geo-mean ratio |
    |---|---|---|---|---|---|---|---|---|
    | resync_at150_down | 0 (RC1) | 21572 | 0.429 / 0.402 | 0.94 | 1008 -> 1270 (+26 %) | 1 / 7 | 1 / 4 | 0.77 |
    | resync_at150_down | 15 | 12199 | 0.806 / 0.830 | 1.03 | 1099 -> 1802 (+64 %) | 3 / 4 | 4 / 2 | 0.83 |
    | resync_at150_down | 30 | 13458 | 1.305 / 0.805 | 0.62 | 2915 -> 1655 (-43 %) | 0 / 8 | 0 / 2 | 0.51 |
    | resync_at150_down | 60 | 21229 | 1.501 / 0.706 | 0.47 | 5886 -> 2450 (-58 %) | 0 / 8 | 0 / 5 | 0.51 |
    | cliff | 0 (RC1) | 28585 | 0.323 / 0.257 | 0.80 | 1064 -> 783 (-26 %) | 1 / 5 | 2 / 3 | 0.78 |
    | cliff | 15 | 18037 | 0.577 / 0.610 | 1.06 | 896 -> 1369 (+53 %) | 2 / 4 | 3 / 2 | 0.92 |
    | cliff | 30 | 22030 | 0.818 / 0.709 | 0.87 | 2438 -> 2257 (-7 %) | 2 / 6 | 3 / 1 | 0.70 |
    | resync_at150 | 0 (RC1) | 26094 | 0.340 / 0.333 | 0.98 | 761 -> 834 (+10 %) | 3 / 5 | 1 / 3 | 0.87 |
    | resync_at150 | 15 | 18308 | 0.785 / 0.683 | 0.87 | 801 -> 896 (+12 %) | 1 / 6 | 2 / 1 | 0.80 |
    | resync_at150 | 30 | 18309 | 0.809 / 0.579 | 0.72 | 919 -> 875 (-5 %) | 1 / 7 | 1 / 2 | 0.54 |

    (The d0 rows recompute RC1's own tables with the same tool: e.g.
    resync_at150_down new capture 0.541 / 0.538, rows over 2 m 627 / 927, as
    in `core_ref_rc1/README.md`.) The live O1 / N1 numbers are not the
    comparison: the old-rule clients ran a schedule behind (starved, worst lag
    33-34) and show fewer predicted ticks at their checkpoints, so live old
    pred means (0.39-1.27 m) sit under the new ones (0.42-2.49 m), while the
    old rule replaying a new capture - the same on-time session - gives
    0.58-3.43 m (one client 11.0; `ab_table.md`, xold): the session, not
    the rule, as RC1
    found for H1. One run per cell (quick tier): a single cell can flip
    between its two captures (resync_at150_down d15: old capture 0.713 /
    0.692 and 295 -> 293 rows over 2 m, new capture 0.855 / 0.901 and 804 ->
    1509).
  - **Part 4, the answer.** (1) Hold-last stops being worse between a
    minimum lateral dwell of 15 and 30 ticks: at 15 it is worse in rows over
    2 m in all three arms (+12 %, +53 %, +64 %) and in the mean in two (1.03,
    1.06; resync_at150 0.87); at 30 it is better in the mean in all three
    (0.62, 0.87, 0.72; geometric-mean ratio 0.51-0.70; 21 of 24 pairs better,
    3 worse) and in rows over 2 m pooled in all three (-43 %, -7 %, -5 %); at
    60 (resync_at150_down) 0.47 and -58 %, all 8 pairs better. How firm
    that is (review of RC1c): only resync_at150_down at 30 and 60 is better
    on both captures (8 of 8 pairs in the mean each, rows over 2 m 510 ->
    297 and 2405 -> 1358 at 30); on cliff and resync_at150 at 30 the
    rows-over-2 m gains come from the new-rule captures alone - on the
    old-rule captures the same cells are worse (cliff 642 -> 778, +21 %, 2 of
    4 clients worse in the mean; resync_at150 651 -> 695, +7 %) - so those
    two gains are within the single-run spread between captures (the same
    kind of flip as resync_at150_down at 15); what is consistent on both
    captures there is the mean (cliff 0.473 / 0.478 and 1.615 / 1.242;
    resync_at150 0.711 / 0.668 and 0.970 / 0.433) and the geometric-mean
    ratio. So: the crossover is 15-30, firm on resync_at150_down; on cliff
    and resync_at150 dwell 30 is no worse on the mean but its rows-over-2 m
    gain is within single-run noise; the run that would settle it is O2 / N2
    at dwell 30 on cliff and resync_at150 (not run: quick tier). The crossover sits
    at the relay lag's order (20 ticks at 150 ms, 42 for the +300 ms
    clients): a hold the relay has shown is still held at the present when
    holds last about a lag; a 15-tick hold has ended before the present at
    either lag, and the bot's 1-3-tick pulses (dwell 0) are the extreme of
    that. (2) Whether real players' lateral holds lie above that cannot be
    measured from the repository: there is no human lateral key record
    (part 1); the one plausibly human hold is 69 ticks (n = 1). A real
    distribution needs a person to play: the mod's `/mmo auto record start
    <file> 13` on the arena (or any level) for a minute or two, then
    `dwell.py bmrc <file>` gives the share under 15 / 30 ticks, and the same
    file drives a headless client with `--record` (v3 since the mod writes
    v3). (3) The sideways error of a held key: on the arena a lateral key
    changes the ball's sideways speed by 0.105-0.119 m/s per tick (about
    7.6 m/s2; `accel.py` on the d30 / d60 server journals), and with no key
    the speed holds (mean change 0.000-0.002 m/s per tick: no lateral drag).
    A key held L ticks too long puts the mirror 0.115/66 * L^2/2 m off (0.35
    m at L = 20, 1.53 m at L = 42), twice that against the opposite key -
    whose slope at L = 42 is 0.146 m per tick, RC1's 0.14. That is a property
    of predicting any held key past its end, so of hold-last; §9.2's decaying
    lateral prediction (drop the lateral key after k ticks of tail) bounds the
    *speed* error to 0.115 k m/s, not the position: the speed carries on, so
    the error at the present is 0.115/66 * k * (L - k/2) m (k = 10: 0.65 m
    at L = 42 instead of 1.53 m), and it costs the mirror the same kind of
    error when the player is really still holding: 0.115/66 * (L - k)^2/2
    (0.89 m at k = 10, L = 42). Which k is right depends on how long holds
    last against L: the crossover above says hold-last is right for holds of
    30 ticks or more, so a k under the players' typical hold would hurt
    them; k should not be below the median human lateral hold, which is the
    missing number. (4) The options again: (a) accept RC1 as it is - for
    holds of 30 ticks and longer the numbers favour hold-last on the mean in
    every arm, and on rows over 2 m firmly only on resync_at150_down (cliff
    and resync_at150 at 30: within single-run noise, see (1); p99: better on
    5 pairs, worse on 4), the bot's 1-3-tick pulses
    and 15-tick holds are where it loses (rows over 2 m +10..+64 %), and it
    cuts the rewinds 80-85 % under holds; (b) §9.2 first - would help only
    holds shorter than the lag, bounds the speed error not the position
    (no lateral drag), and its k cannot be chosen without human dwell data;
    it changes the live predictor too and needs its own drift check; (c) is
    this measurement: done for the bot with dwells 15 / 30 / 60; what it
    cannot say without a human record is where real players' holds fall
    relative to the 15-30-tick crossover.
- **Measurement RC1d (2026-10-26; fix-up round 2026-10-27)** - the tail
  prediction rules under the cases the user made required (§11,
  2026-10-25/26: 1-tick taps, rapid-trigger alternation, fast switches,
  brake taps), against short holds: R0 (the old stale tail), R1 (hold-last),
  R2-R6 (§9.2), paired on the same captures. A measurement: no session code
  is changed in the tree, nothing here is a ruling. Tools
  `.local-tools/work/RC1d/` (`rule_patch.py`, `build_rule.sh`, `rc1d_run.sh`,
  `analyse_cell.py`, `tail_accuracy.py`; the third attempt's `gaps/`:
  `pinned_replay.py`, `pinned_all.sh`, `analyse_pinned.sh`, `common_cut.py`,
  `final_table.py` (-> `final_table.md`), `proxy_table.py`, `live_table.py`,
  `own_keys.py`, `proximity.py`, `bursts.py`, `diverged_check.py`; the
  fix-up round's `fixup/`: `rc1d_run_ud.sh`, `rule_patch6.py`,
  `build_rule6.sh`, `det_check.py`, `switches.py`, `births.py`,
  `per_client.py`, `cut_setters.py`, `doc_table.py`); runs
  `maps/arena-test/rc1d_<arm>_<cell>_<rule>/` with the pinned paired
  replays under `pinned/r<n>/` (attempt 2's unpinned ones under
  `replay/r<n>/`, its `tables.md`: superseded, see gap (b)).
  - **Harness options** (`BallanceMMOServer/sim/session_client.cpp`, new
    regions only): `--brake-tap N:P` (arguments :322-327, parse :433-440,
    usage :365, `brake_tap` :2129-2145, applied in `frame()` :2185, state
    :3155-3156): while the drive record holds UP, the last N of every P
    UP-frames press DOWN instead (UP released; N = 1 is a 1-tick brake, 1:2
    an UP/DOWN 1:1 alternation); `--keep-lane-tap N:M` (arguments :328-334,
    parse :441-448, usage :366, `keep_lane_tap` :2147-2169, one line in
    `keep_lane` :2069, state :3157-3158): while the steer wants a key it is
    pressed N frames, released M, repeating; a new wanted key restarts the
    cycle. In a release frame `keep_lane` puts back the drive record's value
    of that key; the arena's records hold no lateral keys, so a release
    releases (a record that held one would not). Both inert when absent:
    `wire_check.sh rRC1c rRC1d rRC1dw` (this tree with all three options
    against RC1c's binary) `WIRE-CHECK: ALL SAME` (36/36 sends SAME, 36/36
    journals IDENTICAL), re-run 2026-10-26.
  - **Builds**: `build-agent-rRC1d1` = `rRC1d` = this tree (hold-last);
    `rRC1d0` and `rRC1d2..5` = the tree plus `patches/rule<n>.patch`
    (temporary: `rc1d_tail_frame` in IR, called from CF's tail branch and
    from `live_drive` in CL - both sides one rule; R0 only switches CF back
    to `relay_frame_or_earlier`); the fix-up round's `rRC1d6` = the tree plus
    `patches/rule6.patch` (`fixup/rule_patch6.py`, `fixup/build_rule6.sh`).
    Each built once and the three files restored byte for byte: IR, CF and
    CL sha256 equal `pristine/` now (CL has no diff); `build-agent` was
    rebuilt from the restored tree after R6 (`fixup/build_restored.log`
    BUILD-OK). Every build log ends BUILD-OK; the seven client binaries
    differ. The rule builds do what their patches say: **the R1 replays of
    the R1 captures are byte-identical to the live journals and send the
    same bytes on every cell (120/120 client journals on 30 cells,
    `fixup/det_check.py`; the first round had checked 8 cells)**, the R5
    replays of two R5 live captures and the R6 replays of two R6 live
    captures likewise (8/8 each), and in every cell every other rule's first
    difference from the R1 journal lies at or after the first tick at which
    its python model (`tail_accuracy.py`) predicts a different tail (R0-R6,
    every client of every cell).
  - **Cells** (RC1c's recipes: snapshots off, 4 clients, trafo option off,
    RC1's Arch server `ballanced-3e4289e95f25+bmmo-49e2cc8e0253-dirty`; one
    R1 live run per cell): the arms resync_at150 (relay lag about 20 ticks)
    and resync_at150_down (client 0 lowered by 300 ms: up to 42); the
    steer's pattern d0 (the native bang-bang steer: lateral holds median
    1-2, p90 10-35), t1x1 (`--keep-lane-tap 1:1`, rapid-trigger 1:1
    alternation; its bursts last a median of 1 tick, 11 % of them 20 ticks
    or more), t1x3 (1:3), dN (`--keep-lane-dwell N`: holds of N or more),
    _brake (`--brake-tap 6:90`: a 6-tick DOWN in every 90 UP ticks), and,
    added at the fix-up round on the native steer, **d0_ud1x9**
    (`--brake-tap 1:10`: a 1-tick DOWN in every 10 UP ticks, the 1-tick
    brake tap), **d0_ud1x2** (`1:3`: UP UP DOWN, an UP/DOWN fast switch that
    still advances) and **d0_ud1x1** (`1:2`: UP/DOWN 1:1; the balls barely
    leave the start - no sector entered, `events=3/70/60` - so it measures
    the alternation on a ball nearly at rest). 30 cells: the 23 of
    `cells.txt`, resync_at150_down d15_brake (whose live run is complete -
    all four clients logged their final status and `exit code 0` - but whose
    harness raised `TimeoutExpired` because client 1's process had not
    exited 240 s after its last log line: a process-exit hang after the
    session, not investigated; attempt 2 moved the run by hand out of its
    working directory `t3x_rc1d_resync_at150_down_d15_brake_r1`, which no
    longer exists), and the six d0_ud cells. What the first round's cells
    did and did not hold (`fixup/switches.py` on the server journals):
    direct LEFT -> RIGHT switches 405 in d10 (the dwell's transitions; 5 in
    d0), direct UP <-> DOWN switches 294 in d0_brake (two per 6-tick
    brake), 1-tick DOWN presses 0 in every cell - the 1-tick brake and the
    UP/DOWN alternation were missing (review 2's finding 1, confirmed) and
    are the d0_ud cells.
  - **Gap (a), "replay diverged"** (attempt 2's tables: c0 at 955-1026
    steps, c1 / c3 at 3158-3298): `e0_replay.py` stops when the replayed
    client asks for a clock read the capture does not have. c0 is the
    `--resync-at s2+3` client: under another rule its own ball crosses into
    sector 2 a tick later (855 -> 856 on resync_at150 d5, R2), so its sector
    event (a reliable send: the cut) and its test resync move, and the
    replay leaves the captured stream at the resync's clock read. c1 / c3:
    the replay's own ball dies at another tick, the server never echoes that
    birth (`own birth moved 2834 -> never ... resync`) and the client asks
    for a resync. Every one of the 61 diverged replays (and the 2 of the
    pinned set, both R0 c0) ends AFTER its cut (`diverged_check.py`: last
    replayed tick 858-3200 against cuts 619-1226): no row is lost, nothing
    is invalidated; it is the expected end of a replay run past its cut,
    not a determinism break.
  - **Gap (b), the cause of the 20-tick 1:1 "loss": a flaw of the paired
    method, not of R5.** Attempt 2 had R2-R4 at 1.31-1.33 and R5 at 1.51
    on resync_at150 t1x1. On common rows (`common_cut.py`) the whole loss
    was client 1 (R1 0.301 m against R5 0.784 m; clients 0, 2, 3 better
    under every rule), one episode at ticks 2860-3274: R5's mirror of player
    3734335064 drifted 0.15 -> 6.7 m sideways for 150 ticks while the live
    journal's stayed under 1 m. That mirror rolls in contact with the own
    ball (centres 4.02 m apart), and the replay's OWN ball was 1.07-3.51 m
    off the server's own ball there (live 0.01-0.03 m). The cause: the
    paired replays re-ran the harness steer (`--keep-lane` and the pattern
    options), which reads the own ball's PREDICTED state; another remote
    prediction pushes the own ball differently, the steer presses other
    keys, and the replayed client drives its own ball with keys the captured
    server never applied - from tick 308 on under every rule but R1 (t1x1
    c1: 182 of 3412 own frames differ under R5, 60-124 under R0 / R2 / R4;
    `own_keys.py` on the FRESH rows). The cut (the first differing RELIABLE
    send) never saw it: own inputs are unreliable sends. So the paired rows
    after about tick 300 in attempt 2's `tables.md` (and its "rows" that
    differed between rules) compared a different session with the server's,
    not another prediction of the same one. **The fix**
    (`gaps/pinned_replay.py`): the replay runs with the steer and the
    pattern options off and `--record` / `--record-by-tick` holding, per
    tick, the arrow keys of the live journal's own FRESH input rows (a
    SUBSTITUTED row is the server's frame, not what was sent; leaf bit 2 =
    DIK 205, bit 3 = DIK 203); 700-800 record frames per client have no
    FRESH row and keep the drive record's keys, which the 120/120
    byte-identical R1 replays show to be harmless. With the own keys pinned
    every rule is judged on the same rows.
  - **What this does to RC1's own evidence (review 1's blocker, review 2's
    finding 3).** RC1's and RC1c's paired drift tables (`paired_drift.py`;
    RC1c's 0.94 for resync_at150_down dwell 0 above) were made the same
    unpinned way and carry the same flaw after the first own-key
    difference; RC1c's dwell captures are not re-measured (queued with
    RC6's checks), so neither RC1's nor RC1c's drift ratio should be read
    as evidence. The d0 cells re-measure RC1's question pinned, and the
    answer splits: **on input rewinds and CPU hold-last wins** (paired: R0
    needs 2.6x / 3.4x hold-last's input rewinds at 20 / 42 ticks, 1852 /
    705 and 3614 / 1058; live: R0's d0 runs had 2153 / 705 and 3874 / 1131
    input rewinds and starved on every client, 237-306 frames each at 20
    ticks and 1263-2318 at 42, where R1 starved 0 at 20 and on client 0
    alone at 42 (2355; 11-17 on the others)); **on drift it does not at the
    42-tick lag**: paired R0 / R1 1.09 at 20 ticks (hold-last better) but
    0.84 at 42 (the stale tail better, the bang-bang cause §9.2 traced), and
    both live d0 runs of R0 had the lower mean (0.379 against 0.404 m at 20;
    0.434 against 0.883 m at 42, rows over 2 m 789 against 2987 - single
    sessions); under UP/DOWN fast switches the stale tail is far better
    than hold-last (R0 / R1 0.21-0.70 on the d0_ud cells: a delayed copy of
    the alternation nets out, a held DOWN or UP does not). §9.2's "on the
    aggregate RC1 is better in every arm" holds for rewinds, not for drift;
    a ruling on RC1 is a ruling on rewinds and CPU.
  - **The paired table, own keys pinned, common rows** (`gaps/final_table.md`
    has every column; pooled over the four clients; R0-R6 = the rule's mean
    error over hold-last's on the same rows; rows over 2 m; input rewinds
    and "per change" = input rewinds per change of a remote's applied keys;
    replayed ticks = the ticks the input rewinds re-simulated):

    | lag | cell | rows | R1 mean (m) | R0 | R2 | R3 | R4 | R5 | R6 | over 2 m R1 / R5 / R6 | input rewinds R1 / R0 / R2 / R3 / R4 / R5 / R6 | per change R1 / R0 / R5 / R6 | replayed ticks R1 -> R5 / R6 |
    |---|---|---|---|---|---|---|---|---|---|---|---|---|---|
    | 20 | d0 | 17681 | 0.404 | 1.09 | 0.98 | 0.99 | 0.98 | 0.98 | 0.98 | 746 / 737 / 737 | 705 / 1852 / 673 / 702 / 682 / 678 / 678 | 0.84 / 2.21 / 0.81 / 0.81 | 18332 -> 17638 / 17638 |
    | 20 | t1x1 | 17550 | 0.280 | 1.10 | 0.96 | 0.95 | 0.95 | 0.93 | 0.93 | 457 / 443 / 443 | 1135 / 1723 / 1106 / 1126 / 1112 / 713 / 709 | 0.46 / 0.70 / 0.29 / 0.29 | 29029 -> 19598 / 19506 |
    | 20 | t1x3 | 17635 | 0.440 | 1.04 | 0.98 | 0.98 | 0.97 | 0.95 | 0.95 | 641 / 638 / 638 | 1258 / 1862 / 1210 / 1234 / 1226 / 795 / 785 | 0.62 / 0.92 / 0.39 / 0.39 | 31424 -> 20488 / 20268 |
    | 20 | d5 | 17531 | 0.429 | 0.99 | 0.88 | 0.84 | 0.82 | 0.82 | 0.82 | 549 / 493 / 493 | 1635 / 4302 / 1626 / 1622 / 1663 / 1661 / 1661 | 0.93 / 2.43 / 0.94 / 0.94 | 40049 -> 40611 / 40611 |
    | 20 | d10 | 17661 | 0.923 | 0.74 | 0.74 | 0.73 | 0.67 | 0.67 | 0.67 | 925 / 681 / 681 | 2155 / 6236 / 2173 / 2486 / 2076 / 2076 / 2076 | 0.85 / 2.45 / 0.81 / 0.81 | 52209 -> 50270 / 50270 |
    | 20 | d15 | 14258 | 0.611 | 1.12 | 0.69 | 0.79 | 0.71 | 0.71 | 0.71 | 523 / 350 / 350 | 1067 / 5730 / 1386 / 1504 / 1036 / 1036 / 1036 | 0.90 / 4.86 / 0.88 / 0.88 | 25628 -> 24962 / 24962 |
    | 20 | d0_brake | 18350 | 0.381 | 1.18 | 0.98 | 1.12 | 0.88 | 0.88 | 0.88 | 551 / 367 / 367 | 1468 / 3917 / 1460 / 1690 / 1550 / 1550 / 1550 | 0.87 / 2.32 / 0.92 / 0.92 | 35780 -> 37572 / 37572 |
    | 20 | t1x1_brake | 17834 | 0.392 | 1.11 | 0.98 | 1.13 | 0.87 | 0.86 | 0.86 | 540 / 368 / 367 | 2105 / 3946 / 2088 / 2295 / 2178 / 1909 / 1867 | 0.57 / 1.07 / 0.52 / 0.51 | 51340 -> 47396 / 46460 |
    | 20 | d5_brake | 18555 | 0.587 | 0.98 | 0.90 | 0.89 | 0.79 | 0.79 | 0.79 | 573 / 358 / 358 | 2654 / 5496 / 2678 / 2730 / 2702 / 2702 / 2702 | 0.70 / 1.45 / 0.72 / 0.72 | 62100 -> 63282 / 63282 |
    | 20 | d10_brake | 18506 | 0.824 | 0.80 | 0.78 | 0.79 | 0.67 | 0.67 | 0.67 | 875 / 360 / 360 | 2532 / 6030 / 2552 / 2730 / 2561 / 2561 / 2561 | 0.78 / 1.87 / 0.79 / 0.79 | 59659 -> 60317 / 60317 |
    | 20 | d15_brake | 18294 | 0.742 | 1.13 | 0.74 | 0.84 | 0.68 | 0.68 | 0.68 | 695 / 321 / 321 | 2164 / 6634 / 2317 / 2522 / 2161 / 2161 / 2161 | 0.81 / 2.47 / 0.81 / 0.81 | 49437 -> 49440 / 49440 |
    | 20 | d30_brake | 17881 | 0.722 | 1.74 | 0.95 | 1.14 | 0.70 | 0.70 | 0.70 | 1151 / 394 / 394 | 1669 / 6745 / 1926 / 2060 / 1763 / 1763 / 1763 | 0.82 / 3.32 / 0.87 / 0.87 | 38894 -> 41118 / 41118 |
    | 20 | d0_ud1x9 | 11158 | 0.524 | 0.57 | 0.98 | 1.15 | 1.26 | 0.56 | 0.26 | 559 / 203 / 166 | 1979 / 2402 / 1979 / 2168 / 1997 / 1235 / 636 | 0.37 / 0.44 / 0.23 / 0.12 | 46625 -> 29532 / 15681 |
    | 20 | d0_ud1x2 | 19590 | 0.615 | 0.34 | 0.99 | 0.54 | 0.46 | 0.28 | 0.26 | 195 / 215 / 215 | 6648 / 5434 / 6648 / 6648 / 6645 / 3127 / 2791 | 0.32 / 0.26 / 0.15 / 0.14 | 167631 -> 88453 / 80839 |
    | 20 | d0_ud1x1 | 19338 | 0.871 | 0.21 | 1.00 | 0.50 | 0.44 | 0.04 | 0.03 | 43 / 22 / 22 | 9403 / 5750 / 9403 / 9403 / 9403 / 779 / 518 | 0.24 / 0.15 / 0.02 / 0.01 | 212820 -> 17812 / 11988 |
    | 42 | d0 | 15120 | 0.939 | 0.84 | 0.68 | 0.71 | 0.71 | 0.71 | 0.71 | 2813 / 1339 / 1339 | 1058 / 3614 / 1260 / 1320 / 1262 / 1260 / 1260 | 0.80 / 2.75 / 0.96 / 0.96 | 42893 -> 51716 / 51716 |
    | 42 | t1x1 | 17875 | 0.413 | 1.13 | 0.96 | 0.95 | 0.95 | 0.94 | 0.94 | 775 / 730 / 730 | 1768 / 2509 / 1734 / 1755 / 1756 / 1355 / 1341 | 0.51 / 0.72 / 0.39 / 0.38 | 51608 -> 42213 / 41876 |
    | 42 | t1x3 | 14122 | 0.328 | 1.21 | 0.95 | 0.93 | 0.93 | 0.91 | 0.91 | 528 / 481 / 481 | 1164 / 1810 / 1132 / 1154 / 1142 / 747 / 739 | 0.61 / 0.96 / 0.39 / 0.39 | 35308 -> 25306 / 25083 |
    | 42 | d5 | 7666 | 0.445 | 1.07 | 0.72 | 0.69 | 0.72 | 0.72 | 0.72 | 529 / 273 / 273 | 674 / 2087 / 670 / 679 / 673 / 673 / 673 | 0.89 / 2.76 / 0.89 / 0.89 | 21438 -> 21577 / 21577 |
    | 42 | d10 | 14385 | 1.469 | 0.66 | 0.55 | 0.55 | 0.51 | 0.51 | 0.51 | 4428 / 994 / 994 | 1413 / 4577 / 1448 / 1701 / 1460 / 1460 / 1460 | 0.81 / 2.62 / 0.84 / 0.84 | 59877 -> 61898 / 61898 |
    | 42 | d15 | 17640 | 1.735 | 0.65 | 0.52 | 0.60 | 0.55 | 0.55 | 0.55 | 7025 / 1642 / 1642 | 1535 / 6138 / 1791 / 1915 / 1480 / 1480 / 1480 | 0.89 / 3.57 / 0.86 / 0.86 | 60136 -> 58324 / 58324 |
    | 42 | d0_brake | 18212 | 1.015 | 1.02 | 0.90 | 1.08 | 0.81 | 0.81 | 0.81 | 2434 / 1571 / 1571 | 1565 / 4018 / 1590 / 1850 / 1706 / 1703 / 1703 | 0.68 / 1.75 / 0.74 / 0.74 | 64130 -> 69315 / 69315 |
    | 42 | t1x1_brake | 7867 | 0.526 | 1.14 | 0.93 | 1.06 | 0.83 | 0.83 | 0.83 | 408 / 297 / 297 | 1331 / 2178 / 1326 / 1405 / 1358 / 1193 / 1175 | 0.55 / 0.91 / 0.50 / 0.49 | 36590 -> 33745 / 33351 |
    | 42 | d5_brake | 14889 | 1.147 | 1.04 | 0.83 | 0.93 | 0.76 | 0.76 | 0.76 | 2829 / 1817 / 1817 | 2212 / 4936 / 2267 / 2312 / 2364 / 2364 / 2364 | 0.78 / 1.74 / 0.83 / 0.83 | 77750 -> 83838 / 83838 |
    | 42 | d10_brake | 11634 | 1.425 | 0.74 | 0.66 | 0.71 | 0.58 | 0.58 | 0.58 | 3216 / 1060 / 1060 | 1273 / 3387 / 1289 / 1438 / 1318 / 1318 / 1318 | 0.71 / 1.88 / 0.73 / 0.73 | 49598 -> 51171 / 51171 |
    | 42 | d15_brake | 11717 | 1.657 | 0.77 | 0.65 | 0.73 | 0.60 | 0.60 | 0.60 | 3816 / 1356 / 1356 | 1336 / 4112 / 1415 / 1511 / 1388 / 1388 / 1388 | 0.80 / 2.45 / 0.83 / 0.83 | 46517 -> 48348 / 48348 |
    | 42 | d30_brake | 18181 | 2.140 | 1.15 | 0.71 | 0.91 | 0.65 | 0.65 | 0.65 | 7510 / 4725 / 4725 | 1691 / 7341 / 1975 / 2096 / 1814 / 1814 / 1814 | 0.83 / 3.60 / 0.89 / 0.89 | 69204 -> 74118 / 74118 |
    | 42 | d0_ud1x9 | 8691 | 0.544 | 0.70 | 0.97 | 1.15 | 1.30 | 0.72 | 0.56 | 382 / 161 / 153 | 1621 / 2167 / 1622 / 1770 / 1674 / 1208 / 914 | 0.41 / 0.55 / 0.31 / 0.23 | 43046 -> 32620 / 25195 |
    | 42 | d0_ud1x2 | 15798 | 1.350 | 0.26 | 0.99 | 0.60 | 0.59 | 0.20 | 0.14 | 1605 / 364 / 167 | 4901 / 4422 / 4903 / 4902 / 4902 / 2868 / 2259 | 0.28 / 0.25 / 0.16 / 0.13 | 138432 -> 84867 / 69198 |
    | 42 | d0_ud1x1 | 19382 | 2.027 | 0.31 | 0.99 | 0.61 | 0.58 | 0.11 | 0.10 | 3909 / 621 / 619 | 7595 / 5412 / 7595 / 7596 / 7596 / 2389 / 1455 | 0.20 / 0.14 / 0.06 / 0.04 | 216957 -> 62157 / 37658 |

    ("20" = resync_at150; "42" = resync_at150_down, whose lag is 42 ticks
    for the lowered player's mirrors and about 20 for the others. "Per
    change" counts every change of the four leaf bits, so a 1:1 tap is two
    changes; below 1 means several changes answered by one rewind.) Per
    client (120 client-cells, `fixup/per_client.py`): R5 and R6 are more
    than 5 % worse than R1 on none and more than 5 % better on 114 (worst
    0.99); R4 is worse on 8 (worst 1.44, resync_at150_down d0_ud1x9 c0),
    R3 on 22, R0 on 47 (better on 53), R2 on none (better on 67). **R4 alone
    fails the 1-tick brake** (1.26 / 1.30): with a 1-tick DOWN every 10
    ticks UP's complete holds are 9 long, so R4 releases UP after
    max(5, 9 - r) tail ticks and predicts no key at all for the rest of the
    lag (the proxy's share of tail ticks right drops to 0.25 against
    hold-last's 0.79) - the opposite of review 2's expected failure (a held DOWN): a
    1-tick DOWN that has been seen whole gives H = 1, held 5 ticks. R5
    hides that only where the whole frame is periodic; the native steer's
    irregular lateral keys break the whole-frame period often, R5 falls
    back to R4 and loses UP there (proxy 0.763 against R1's 0.786 at 20
    ticks); R6 finds UP's and DOWN's own period whatever the lateral keys
    do (proxy 0.930). The ratio on each rule's own rows (to its own cut)
    agrees with the common-rows ratio within 0.04 everywhere but
    resync_at150_down d0_ud1x9 (R6 0.56 on 8691 common rows, 0.75 on its
    own 12499; R5 0.72 / 0.68 on 9282) and resync_at150 d0_ud1x9 (R6 0.26 /
    0.25). **Who sets the short common rows** (`fixup/cut_setters.py`): R0
    on most (resync_at150_down d5: three clients cut at 967-972; d0 c0
    1408; d5_brake and t1x1_brake c0 888), R3 on four clients, R4 / R5 on
    resync_at150_down d10 c1 (968), R3 / R4 on resync_at150 d0_ud1x9 c1 / c3
    (946-956) and R4 / R5 / R6 on resync_at150_down d0_ud1x9 and d0_ud1x2 c0.
    The 968 cut is the own ball's reliable `BodyRevived` of
    P_Modul_25_Bridge, sent at 968 live and at 978 in the R4 / R5 replays:
    until tick 967 the own ball rolled in contact with the mirror
    Ball_Stone_BMMO_697530028; live the contact broke at 967 (mindist 0.50),
    in the R5 replay it held past 978, so the other prediction of that
    mirror moved the own ball (its keys pinned) and it woke the bridge ten
    ticks later - a physical consequence of the prediction, the cut doing
    its job.
  - **Gap (c), the proxy** (`tail_accuracy.py`, physics-free: every tail
    tick of every relayed arrival, the keys each rule predicts against the
    keys the server applied; `gaps/proxy_table.md`, k in {3, 5, 10, 15},
    all 30 cells): R5 / R6 have the most tail ticks right on every lateral
    tap cell (k = 3: t1x1 0.959 / 0.934, t1x3 0.967 / 0.960 at 20 / 42; R1
    0.943 / 0.919 and 0.946 / 0.945); R6 on every d0_ud cell (k = 5: 0.930
    / 0.683 on d0_ud1x9, 0.660 / 0.683 on d0_ud1x2, 0.962 / 0.939 on
    d0_ud1x1; R1 0.786 / 0.739, 0.574 / 0.534, 0.507 / 0.487; R4 0.11-0.26);
    R2 at k = 3 on short holds and brakes (d5 0.845 / 0.866); R4 = R5 = R6
    on the hold cells. k: on the hold and lateral tap cells k = 3 and 5
    differ by at most 0.009 and k = 5 is within 0.01 of the best; on the
    d0_ud cells a larger k is better for R5 / R6 (R6 d0_ud1x9 at 42 ticks
    0.683 at k = 5, 0.752 at k = 15), because k only acts in the R4
    fallback, where it delays the release of the forward key - the same
    weakness as above, which a larger k would trade against the hold cells
    (d5 0.839 -> 0.819 at k = 15). Key accuracy is not the error: the proxy
    ranks R2 first on d0_brake (0.850 at 20 ticks) where R4-R6 have the
    smaller error (0.88 against R2's 0.98); the physics weighs a wrong key by
    how long it acts before the present, the proxy does not.
  - **Births, counted** (`fixup/births.py`; review 2's nit: the first round
    only argued them): the birth rewinds themselves are the same in every
    rule on every cell (the first relayed frame corrects the birth state
    whatever the tail rule). The input rewinds in the 50 ticks after a
    birth are not: R4 / R5 / R6 have R1's or up to 25 % more on the hold and
    brake cells (resync_at150_down d0 146 -> 183, resync_at150 d30_brake
    124 -> 147, d15 131 -> 142, resync_at150_down d5 49 -> 49) and fewer on
    the tap and UP/DOWN cells (t1x1 338 -> 131 / 128, d0_ud1x1 148 -> 121 /
    62); R0 has 2-5x R1's. The first round's "births are untouched by every
    rule" was wrong for the window after the first relayed frame.
  - **Gap (d), the live runs** (`gaps/live_table.md`: per client input
    rewinds, resim ticks, starved, resyncs / rebases, the pacer's worst
    lag, the live prediction error; cpu sampler max total 10.1-26.2 %; R1 on
    all 30 cells, R5 on 26, R6 on d0_ud1x9 at both lags, R0 on d0 = RC1's
    own runs).
    **Each row is ONE session, and a rule's live run is a different session
    from another rule's: the starvation counts below are single sessions in
    both directions, not evidence for or against a rule; the paired table
    is the evidence, and repeated live runs per rule are part of RC6's
    gate.** What the single sessions show: R5's live mean error is lower in
    23 of 26 cells, higher on resync_at150 d0 (0.404 -> 0.413), on
    resync_at150_down t1x3 (0.362 -> 0.551) and on resync_at150_down
    d0_ud1x9 (0.655 -> 0.665; R6 0.261). Where the paired difference is
    large the live one follows it: on d0_ud1x2 and d0_ud1x1 R1's mirrors are
    0.62-2.03 m off on average and R5's 0.03-0.12 (d0_ud1x2 at 20 ticks R1
    0.615 m, 8593 input rewinds, 167631 resim ticks, 5862 frames starved -
    1387-1495 on every client, each with a rebase and a resync; R5 0.053 m,
    960, 21709, none; at 42 ticks R1 1.810 m with client 0's p99 at 44 m,
    6132 starved, R5 0.116 m, 239); d0_ud1x1 R1 9403 / 7595 input rewinds,
    R5 953 / 762, neither starved (balls at rest); d0_ud1x9 R1 3139 / 3460,
    R5 1487 / 1832, R6 846 / 1228 (starved at 42: 3698 / 696 / 278). Where
    the paired difference is small the sessions scatter both ways: on the
    lateral 1:1 tap R1 starved 288 frames at 20 ticks and R5 none, but on
    the 42-tick arm R5 starved more than R1 in some hold sessions (d5 549 ->
    1466, d10 1160 -> 1983, d10_brake 834 -> 2045, t1x1_brake 3047 -> 4358,
    t1x3 634 -> 2669) and less in others (d0 2398 -> 441, d30_brake 2407 ->
    457, t1x1 1808 -> 435). On the same captures R5's input rewinds are
    R1's -4 to +9 % on the 42-tick hold and brake cells (d0 +19 %) and its
    replayed ticks -3 to +8 % (d0 +21 %): a modest extra cost on long-lag holds
    that one session cannot separate from the session's own scatter; the
    reverse pairing of the R5 down t1x3 capture (replayed by R1, pinned) is
    worse under R1 on all four clients (mean 0.558 / 0.635 / 0.407 / 0.905
    against R5's 0.514 / 0.556 / 0.318 / 0.820, input rewinds +30-45 %).
    Resyncs and rebases: the arms' own (c0's `--resync-at`, the lowered
    client's forced one) plus every client's rebase and second resync in
    R1's d0_ud1x2 runs and client 0's in R1's 20-tick t1x1 and t1x1_brake
    runs; the 42-tick tap cells rebase and resync on some clients under
    both rules.
  - **(e) The answer.** (1) Best rule per cell against hold-last (common
    rows, pinned): R6, never worse than hold-last on any cell (largest ratio
    0.98) nor on any client by more than 5 %; identical to R5 on every hold
    and 6-tick brake cell (0.51-0.82, the same rewinds), level or better on
    the lateral taps (t1x1 0.93 / 0.94, t1x3 0.95 / 0.91, t1x1_brake 0.86 /
    0.83 at 20 / 42), and far better on the UP/DOWN cells: the 1-tick
    brake d0_ud1x9 0.26 / 0.56 (R5 0.56 / 0.72, R4 1.26 / 1.30, R0 0.57 /
    0.70), d0_ud1x2 0.26 / 0.14, d0_ud1x1 0.03 / 0.10. R2 is level with R6
    on 20-tick d15 (0.69 / 0.71) and better on 42-tick d0 (0.68 / 0.71) and
    d15 (0.52 / 0.55) but is hold-last on every UP/DOWN cell (0.97-1.00).
    The 1:1 loss of attempt 2 does not exist: it was the unpinned method.
    (2) One rewind per real change on holds and births: R1, R4, R5 and R6
    stay at 0.68-0.96 per change on every hold and brake cell; R5 / R6 are
    within +0.06 of R1 on all of them but resync_at150_down d0, **+0.16
    (0.80 -> 0.96; replayed ticks 42893 -> 51716, +21 %)**, all of it on
    the mirrors of the lowered player (client 1: input rewinds on player
    142184816's balls 231 -> 296, on the others 110 -> 106): at a 42-tick
    tail the native steer's long lateral holds (p90 10-35) outlast R4's
    release at max(5, H - r) with H the short median, and the arrival
    corrects it; at 20 ticks the same rule has fewer rewinds than R1
    (682 / 705). Replayed ticks on the 42-tick brake cells rise too (d0_brake
    +8 %, d5_brake +8 %, d30_brake +7 %). R2 and R3 rise above 1 on d15
    (1.17, 1.27), R3 on d30_brake; R0 needs 1.4-5.4 times R1's input
    rewinds on the hold and tap cells (fewer only on the UP/DOWN cells).
    Births: (Births, counted) above. (3) Under 1:1 lateral alternation,
    input rewinds per 100 frames: R1 8.2 (20 ticks) / 12.7 (42), R2-R4
    within 0.3 of it, R5 5.2 / 9.7, R6 5.1 / 9.6, R0 12.5 / 18.0; under
    UP/DOWN 1:1, R1 67.7 / 54.5, R5 5.6 / 17.2, R6 3.7 / 10.4 (replayed
    ticks 212820 -> 11988 and 216957 -> 37658). (4) The brake tap: the
    6-tick brake d0_brake R4-R6 0.88 / 0.81, R2 0.98 / 0.90, R3 1.12 / 1.08,
    R0 1.18 / 1.02; the 1-tick brake d0_ud1x9 as in (1). (5) k: k = 5 for
    R4-R6 on the hold and tap cells (the proxy and the paired runs agree);
    on the UP/DOWN cells a larger k helps the R4 fallback (gap (c)) - the
    fallback's treatment of the forward / backward keys is the open point,
    not k. (6) The variant R6 was built because the fix-up round's 1-tick
    brake cell pointed to a concrete fault of R5 (its whole-frame period
    broken by the irregular lateral steer, then R4 releasing UP); it removes
    that fault and changes nothing where R5 was already right.
  - **Recommendation, as the refinement step RC6 (§8; hold-last stays RC1's
    rule unless the user rules otherwise):** §2.1's tail row becomes "past
    the newest relayed frame: R6 - for each key bit, that bit's newest
    relayed period repeated when the contiguous relayed run ending at the
    newest frame shows one (p 2..16, at least 4p ticks of run, the bit
    repeating over its newest 3p ticks, not constant over its newest
    period), else, for a key of the newest frame, held while d <=
    max(5, H - r) (H the median of the key's last three complete relayed
    holds within the 128 ticks before the newest, r its current run),
    hold-last for a key with no complete hold; flags as hold-last". Sites:
    one function in IR next to `relay_frame_or_newest` (IR:353-367), called
    by `recorded_input_at`'s remote tail branch (CF:494) and by `live_drive`
    for a tick past the ring's newest frame (CL:467-470), so the replay and
    the live predictor stay one rule (§2.2); holes, the start, the birth
    state, the own ball, the wire and the journal unchanged. Its gate is
    §8's RC6 row, which as measured R6 does not pass on one cell
    (resync_at150_down d0, +0.16 per change): the user rules whether that
    is accepted or RC6 must remove it first. Cost: the measured build walks
    up to 64 ring entries per key for the periods and the ring once per held
    key for H, and allocates a `std::vector` per key per tail tick; RC6
    computes the periods once per arrival, bounds H's window, allocates
    nothing, and is judged by CPU parity against the replayed ticks it adds
    on the 42-tick holds (-3 to +8 %, d0 +21 %) and removes on the tap and
    UP/DOWN cells (-19 % to -94 %).
  - **What a rapid-trigger player would see** (from the paired numbers;
    the live sessions agree where the paired difference is large). Under
    RC1 alone the others' mirror of a tapper holds whatever the newest
    relayed tick had for the whole lag - a lateral key down (twice the real
    sideways push) or up (none), DOWN for the whole lag after a 1-tick
    brake, UP or DOWN for the whole lag in an UP/DOWN alternation - and the
    arriving frames keep contradicting that tail: a 1:1 lateral tapper costs
    each other client 8.2 input rewinds per 100 frames at 20 ticks (12.7 at
    42), 20-42 ticks deep; an UP/DOWN rapid-trigger player 42-68 per 100
    frames (UP UP DOWN to UP/DOWN 1:1; 138432-216957 replayed ticks per
    session where R6 replays 11988-80839), with his mirror 0.5-2.0 m off on
    average (p99 up to 31 m at 42 ticks); in single live sessions that load
    starved every client (UP UP DOWN at 20 ticks: 1387-1495 frames each, a
    rebase and a resync each). Under RC6 (R6) the mirror repeats the tapping
    per key: lateral tap error 5-9 % lower than hold-last's and input
    rewinds 24-38 % lower; 1-tick brakes 0.26 / 0.56 of hold-last's error
    with 68 % / 44 % fewer input rewinds; UP/DOWN alternation 0.03-0.26 of
    hold-last's error with 54-94 % fewer input rewinds. What RC6 cannot
    remove: the end of a burst is still news one lag late; a single
    isolated tap, or tapping without a period, is predicted by the R4
    fallback (which releases a held key, the forward one included, after
    max(5, H - r)); and the lowered (42-tick) clients still starve and
    rebase in some sessions of every rule - the relay lag and the rewind
    cost (RC2-RC5), not the prediction.
  - **Fix-up round (2026-10-27): the reviews' findings.** Review 1: the
    RC1-evidence blocker - done (above, and §9.2's added note); d0 into the
    RC6 gate with the rewind bound and the failing cell stated - done (§8);
    the live attribution - rewritten as single sessions both ways (gap
    (d)), the player claim on the paired numbers; R1 validated on 8 cells
    only - now 30 / 30 cells, 120 / 120 journals byte-identical; the common
    cut set by R0, the 968 event - named (paired table); the ring-cap
    dependence and the allocation - RC6's row fixes the window and forbids
    the allocation. Review 2: the missing 1-tick brake and UP/DOWN cells -
    confirmed and added (d0_ud1x9 / x2 / x1, both lags, R1 and R5 live, R6
    live on d0_ud1x9), they exposed R4's forward-key release and led to R6;
    "direct LEFT -> RIGHT switches were never measured" - refuted: d10 holds
    405 of them (d0 5); "R4 holds DOWN for the whole lag" - refuted: a
    whole 1-tick DOWN gives H = 1 and a 5-tick hold, R4's real fault is
    releasing UP; the ambiguous rewind criterion - the cells are listed in
    §8; the cost note - replayed-tick increases reported in (e) and the
    allocation named; births - counted; the `keep_lane` release edge case -
    stated under Harness options. Checks of the round: no tree file changed
    but this document (IR, CF, CL equal `pristine/`; the RC1 files' diff
    equals `rc1_tree.patch`); x64 BUILD-OK for rRC1d6 and for the restored
    `build-agent`, whose binary replays the `core_ref_rc1` set exactly as
    `rRC1d` does (`wire_check.sh rRC1d rRC1dfx rRC1dfxw`: `WIRE-CHECK: ALL
    SAME`; its bytes differ from `rRC1d`'s by the link layout only).

- **Measurement RC1e (2026-10-27): the variant R7 against R6 and hold-last.**
  The user ruled R6 into RC1 and, for the failing cell, a variant that only
  fixes the early lateral release (§11 2026-10-27). **R7** = R6 whose
  fallback, for a key of the newest frame with no period, holds the key for
  the whole tail once its run r >= H; hold-last for a key with no complete
  hold; else held while d <= max(5, H - r) - exactly `TAS/tas_screen.py`'s
  `predict()` (the definition; `.local-tools/work/RC1e/r7_equiv.py`: its R7
  counters on the two round-1 streams at lag 20 equal those of
  `tail_accuracy.predict_keys(7)`, R7-DEFINITION-EQUAL; the sentence of the
  step and the code agree). Verdict: **R7 is not selected - it fails the
  "no worse than R6" half of the pass rule on every pinned bot cell; the
  user decides (R6 as ruled, or otherwise).**
  - **Tools (nothing in the tree changed).** R7 in `RC1d/tail_accuracy.py`
    (`_horizon` rule 7, `predict_keys` rules 6 / 7, the variant list; its
    R0-R6 output on rc1d_resync_at150_down_d0 client 1 is identical to the
    pre-edit copy's, `RC1e/ta_pre.out` / `ta_new.out`), `K[7] = 5` in
    `RC1d/analyse_cell.py`; `RC1e/screen.py` (record_model.py's run for R1 /
    R6 / R7 per client journal, real changes from the server journal);
    `RC1e/rule_patch7.py` (made by `make_rule_patch7.py` from
    `fixup/rule_patch6.py` by five asserted replacements: rule 7 takes rule
    6's per-key period search and `keep = (rule == 7 && r >= h) || d <=
    max(k, h - r)`) and `build_rule7.sh`: `build-agent-rRC1d7`, x64
    BUILD-OK (32 s), the three session files restored (RESTORE-OK, sha256
    11a946a6 / f83760f0 / 446724be = `pristine/`), `build-agent` rebuilt
    from the restored tree (BUILD-OK), the RC1 files' diff = `rc1_tree.patch`
    (PATCH-SAME), `client_core_lifecycle.cpp` and `map.hpp` clean;
    `RC1e/real_records.py` / `real_run.sh` (the real-play drive),
    `RC1e/pinned_table.py`, `RC1e/lat_kinds.py`.
  - **(1) Physics-free screen** (`record_model.py`'s engine records on the R1
    live client journals of all 30 RC1d cells - the RC6 row's 12 required
    cells and d5_brake, d15_brake, d30_brake, at both lags (the last six
    added in the review fix-up, `RC1e/screen_extra.out`) - and on the user's
    two round-1 client journals - their real arrival
    pattern; rewinds per real change = modelled input rewinds / key changes
    of the client's remote players in the server journal):

    | cell | real changes | rewinds R1 / R6 / R7 | per change R1 / R6 / R7 | R7 - R6 | lateral released early R1 / R6 / R7 | lateral held late R1 / R6 / R7 |
    |---|---|---|---|---|---|---|
    | 20 d0 | 837 | 705 / 696 / 693 | 0.84 / 0.83 / 0.83 | -0.00 | 440 / 468 / 448 | 599 / 552 / 593 |
    | 20 t1x1 | 2473 | 1534 / 774 / 774 | 0.62 / 0.31 / 0.31 | +0.00 | 977 / 567 / 562 | 1053 / 524 / 549 |
    | 20 t1x3 | 2024 | 1485 / 816 / 816 | 0.73 / 0.40 / 0.40 | +0.00 | 947 / 497 / 496 | 1133 / 597 / 651 |
    | 20 d0_brake | 1688 | 1475 / 1609 / 1509 | 0.87 / 0.95 / 0.89 | -0.06 | 498 / 526 / 506 | 596 / 547 / 586 |
    | 20 t1x1_brake | 3691 | 2744 / 2138 / 2055 | 0.74 / 0.58 / 0.56 | -0.02 | 1262 / 847 / 845 | 1299 / 854 / 864 |
    | 20 d0_ud1x9 | 8606 | 7119 / 1094 / 1045 | 0.83 / 0.13 / 0.12 | -0.01 | 599 / 607 / 603 | 673 / 633 / 656 |
    | 20 d0_ud1x2 | 20665 | 17924 / 8558 / 8528 | 0.87 / 0.41 / 0.41 | -0.00 | 587 / 590 / 588 | 472 / 467 / 472 |
    | 20 d0_ud1x1 | 38476 | 28058 / 705 / 641 | 0.73 / 0.02 / 0.02 | -0.00 | 334 / 332 / 330 | 203 / 197 / 204 |
    | 20 d5 | 1767 | 1746 / 1842 / 1756 | 0.99 / 1.04 / 0.99 | -0.05 | 1601 / 1776 / 1611 | 1439 / 1208 / 1229 |
    | 20 d10 | 2548 | 2539 / 2448 / 2324 | 1.00 / 0.96 / 0.91 | -0.05 | 3199 / 3449 / 3246 | 1159 / 636 / 677 |
    | 20 d15 | 1487 | 1479 / 1466 / 1362 | 0.99 / 0.99 / 0.92 | -0.07 | 1806 / 1998 / 1840 | 572 / 332 / 353 |
    | 20 d10_brake | 3231 | 3131 / 3154 / 3043 | 0.97 / 0.98 / 0.94 | -0.03 | 3481 / 3647 / 3515 | 1023 / 695 / 713 |
    | 42 d0 | 1369 | 1169 / 1389 / 1275 | 0.85 / 1.01 / 0.93 | -0.08 | 891 / 1163 / 1018 | 839 / 772 / 815 |
    | 42 t1x1 | 3489 | 2447 / 1690 / 1676 | 0.70 / 0.48 / 0.48 | -0.00 | 1517 / 1101 / 1067 | 1835 / 1309 / 1368 |
    | 42 t1x3 | 2141 | 1661 / 959 / 959 | 0.78 / 0.45 / 0.45 | +0.00 | 1027 / 589 / 584 | 1343 / 845 / 867 |
    | 42 d0_brake | 2302 | 1949 / 2131 / 2005 | 0.85 / 0.93 / 0.87 | -0.05 | 984 / 1087 / 1027 | 1090 / 1034 / 1064 |
    | 42 t1x1_brake | 4756 | 3633 / 3211 / 3142 | 0.76 / 0.68 / 0.66 | -0.01 | 1998 / 1734 / 1707 | 2139 / 1806 / 1827 |
    | 42 d0_ud1x9 | 7628 | 6302 / 3412 / 3131 | 0.83 / 0.45 / 0.41 | -0.04 | 1162 / 1422 / 1209 | 1029 / 992 / 1014 |
    | 42 d0_ud1x2 | 20505 | 16226 / 8305 / 8287 | 0.79 / 0.41 / 0.40 | -0.00 | 1413 / 1433 / 1422 | 1023 / 992 / 1010 |
    | 42 d0_ud1x1 | 38607 | 22818 / 1719 / 1663 | 0.59 / 0.04 / 0.04 | -0.00 | 1255 / 1268 / 1256 | 742 / 721 / 754 |
    | 42 d5 | 1623 | 1587 / 1654 / 1610 | 0.98 / 1.02 / 0.99 | -0.03 | 1373 / 1483 / 1405 | 1309 / 1105 / 1127 |
    | 42 d10 | 2194 | 2176 / 2297 / 2140 | 0.99 / 1.05 / 0.98 | -0.07 | 2783 / 3188 / 2932 | 1041 / 677 / 695 |
    | 42 d15 | 1717 | 1700 / 1688 / 1567 | 0.99 / 0.98 / 0.91 | -0.07 | 2489 / 2805 / 2553 | 960 / 497 / 558 |
    | 42 d10_brake | 2885 | 2720 / 2902 / 2744 | 0.94 / 1.01 / 0.95 | -0.05 | 3295 / 3756 / 3451 | 1215 / 875 / 946 |
    | 20 d5_brake | 3779 | 3496 / 3586 / 3519 | 0.93 / 0.95 / 0.93 | -0.02 | 3319 / 3397 / 3337 | 2884 / 2252 / 2277 |
    | 20 d15_brake | 2684 | 2638 / 2650 / 2530 | 0.98 / 0.99 / 0.94 | -0.04 | 2373 / 2519 / 2422 | 763 / 439 / 438 |
    | 20 d30_brake | 2031 | 1996 / 2148 / 2014 | 0.98 / 1.06 / 0.99 | -0.07 | 2013 / 2141 / 2045 | 260 / 166 / 183 |
    | 42 d5_brake | 3558 | 3435 / 3662 / 3496 | 0.97 / 1.03 / 0.98 | -0.05 | 2325 / 2587 / 2394 | 2151 / 1993 / 2030 |
    | 42 d15_brake | 2640 | 2513 / 2565 / 2419 | 0.95 / 0.97 / 0.92 | -0.06 | 2524 / 2716 / 2637 | 1176 / 675 / 705 |
    | 42 d30_brake | 2040 | 1978 / 2124 / 1976 | 0.97 / 1.04 / 0.97 | -0.07 | 1573 / 1659 / 1628 | 317 / 232 / 232 |
    | **real play, round 1** (2 clients, ~36000 ticks each) | 1497 | 1480 / 1842 / 1599 | 0.99 / 1.23 / 1.07 | -0.16 | 570 / 877 / 686 | 515 / 483 / 500 |

    ("20" = resync_at150, "42" = resync_at150_down.) R7's modelled rewinds
    are at most R6's on every cell (R7 - R6 <= +0.00 per change: the screen
    bound holds), and it removes about half of R6's excess over hold-last on
    the 42-tick d0 (0.85 / 1.01 / 0.93). Other physics-free numbers: the
    round-1 server journal at constant lag (`TAS/journal_screen.py`), rewinds
    per 100 ticks R1 2.08, R6 2.61, R7 2.26 / 2.25 / 2.25 (lag 20 / 24 /
    42; 1.08x R1), tail ticks right R1 0.866 / 0.855 / 0.823, R6 0.863 /
    0.853 / 0.825, R7 0.869 / 0.858 / 0.829; the key-accuracy proxy on the
    two round-1 client journals (`tail_accuracy.py`, real arrivals, 1938075
    tail ticks): all keys right R1 0.8461, R6 0.8455, R7 0.8505, on the
    ticks where hold-last is wrong R6 0.0995, R7 0.0702. TAS reference line
    (`tas_screen.py --sampling any --lags 20`, reference only, decides
    nothing): rewinds per 100 ticks R1 5.19, R6 8.88 (1.71x), R7 5.84
    (1.12x); tail right 0.773 / 0.652 / 0.753.
  - **(2) Pinned paired replays** (`gaps/pinned_replay.py`, own keys pinned
    from the live journal's FRESH rows; R1 and R6 replays of RC1d reused,
    R7 run with `build-agent-rRC1d7`; common cut over R1 / R6 / R7, so the
    rows differ slightly from `final_table.md`'s seven-rule cut; R1 replays
    byte-identical to the live journals on all 24 (det True 24 / 24); a
    fresh R1 replay of resync_at150_down d0 client 1 with `rRC1d` is
    byte-identical to its live journal and to RC1d's R1 replay):

    | cell | rule | rows | pooled mean (m) | / R1 | / R6 | clients' mean (m) | worst client vs R6 | input rewinds | per real change | replayed ticks |
    |---|---|---|---|---|---|---|---|---|---|---|
    | 42 d0 | R1 | 17733 | 0.883 | 1.00 | 1.352 | 0.519 / 1.044 / 0.826 / 1.134 | +58.9 % | 1096 | 0.80 | 55380 |
    | 42 d0 | R6 | 17733 | 0.653 | 0.74 | 1.000 | 0.505 / 0.738 / 0.520 / 0.845 | - | 1297 | 0.95 | 64170 |
    | 42 d0 | R7 | 17733 | 0.723 | 0.82 | **1.107** | 0.518 / 0.832 / 0.612 / 0.924 | **+17.7 %** | 1198 | **0.88** | 59743 |
    | 20 t1x1 | R1 | 17550 | 0.280 | 1.00 | 1.071 | 0.403 / 0.301 / 0.140 / 0.282 | +18.8 % | 1135 | 0.46 | 29029 |
    | 20 t1x1 | R6 | 17550 | 0.262 | 0.93 | 1.000 | 0.382 / 0.286 / 0.118 / 0.267 | - | 709 | 0.29 | 19506 |
    | 20 t1x1 | R7 | 17550 | 0.267 | 0.95 | **1.020** (1.0202) | 0.388 / 0.291 / 0.126 / 0.271 | **+6.2 %** | 727 | 0.29 | 20007 |
    | 42 d0_ud1x9 | R1 | 12499 | 0.676 | 1.00 | 1.338 | 0.744 / 0.471 / 0.516 / 0.864 | +460 % | 2236 | 0.42 | 65940 |
    | 42 d0_ud1x9 | R6 | 12499 | 0.506 | 0.75 | 1.000 | 0.412 / 0.084 / 0.361 / 0.816 | - | 1454 | 0.27 | 45316 |
    | 42 d0_ud1x9 | R7 | 12499 | 0.471 | 0.70 | 0.932 | 0.414 / 0.095 / 0.343 / 0.736 | **+13.0 %** | 1372 | 0.26 | 42772 |
    | 20 d0_ud1x1 | R1 | 19338 | 0.871 | 1.00 | 32.2 | 0.915 / 0.889 / 0.833 / 0.847 | - | 9403 | 0.24 | 212820 |
    | 20 d0_ud1x1 | R6 | 19338 | 0.027 | 0.03 | 1.000 | 0.011 / 0.025 / 0.034 / 0.038 | - | 518 | 0.01 | 11988 |
    | 20 d0_ud1x1 | R7 | 19338 | 0.030 | 0.03 | **1.098** | 0.016 / 0.032 / 0.036 / 0.035 | **+41.1 %** (0.016 vs 0.011 m) | 457 | 0.01 | 10531 |
    | real 20 (each human 4160 ticks = about 69 s of the ~9-minute round, arena) | R1 | 17842 | 0.209 | 1.00 | 1.006 | 0.268 / 0.290 / 0.109 / 0.173 | - | 809 | 0.97 | 19349 |
    | real 20 | R6 | 17842 | 0.208 | 0.99 | 1.000 | 0.265 / 0.289 / 0.108 / 0.174 | - | 864 | 1.03 | 20473 |
    | real 20 | R7 | 17842 | 0.210 | 1.00 | 1.009 | 0.268 / 0.290 / 0.110 / 0.175 | +2.4 % | 821 | 0.98 | 19565 |
    | real 42 | R1 | 17160 | 0.257 | 1.00 | 1.023 | 0.454 / 0.298 / 0.109 / 0.176 | - | 707 | 0.97 | 19126 |
    | real 42 | R6 | 17160 | 0.251 | 0.98 | 1.000 | 0.435 / 0.295 / 0.106 / 0.176 | - | 782 | 1.08 | 20819 |
    | real 42 | R7 | 17160 | 0.257 | 1.00 | **1.022** | 0.451 / 0.297 / 0.110 / 0.177 | +3.8 % | 729 | 1.00 | 19637 |

  - **(3) The user's real play, driven.** On the arena, not level 11: the
    capture harness (`tr3fix/ref_run.sh` -> `arena_map_test.py`) runs the
    arena (`--level 13`, its map-hash slot and checkpoints), and a level-11
    headless run was not tried - it needs its own harness setup and would not
    fit the 15-minute gate. The two round-1 players' applied keys per tick (server journal, 4160 ticks
    from 120 before each player's first press: player 1367537785 ticks
    172-4331, 100 key changes; player 1853470835 ticks 1795-5954, 59) became
    v3 `.bmrc` records (`RC1e/real/player0/1.bmrc`, leaf bits to DIK 200 /
    208 / 205 / 203 as `pinned_replay.py`) driven with `--record-by-tick` by
    clients 0 and 1 on the arena, clients 2 and 3 on the native steer
    (`--keep-lane 15`), RC1d's arm recipes, 150 ms, snapshots off, captures
    on, the hold-last build live: `maps/arena-test/rc1e_real_resync_at150_r1`
    ("real 20") and `rc1e_real_resync_at150_down_r1` ("real 42": the down
    arm's lowered client 0, relayed lag max / p99 50 / 47, mirrors the human
    of client 1; clients 1-3 mirror the human of client 0). The rows pool
    every mirror of each client (humans and steers). The human keys steer an
    arena ball with a level-11 player's timing, so the drift is indicative;
    the key streams and arrival patterns are real. Both live sessions: rb
    148-258 per client, far 0, starved 0, resyncs 0, host CPU max 15.5 %;
    the harness VERDICT lines (no ball-to-ball interaction; client 0's frames
    past the 48-tick reach on the down arm = the queued "48 reach") are the
    harness's own and not this measurement's. Scope: each human stream
    is one 4160-tick window (about 69 s, 59-100 key changes) of a round of
    about 36000 ticks per player, at the harness's 150 ms, so the pinned
    real-play rows weigh against the physics-free model of the whole round
    (1.07 for R7 against 0.99), not instead of it. **Real play, R7 against
    hold-last:** pooled mean +0.3 % / -0.1 % (20 / 42), per client -0.6 to
    +0.8 %, input rewinds per real change 0.98 / 1.00 against 0.97 / 0.97
    (+0.01 / +0.03); modelled on the round-1 journals 1.07 against 0.99
    (+0.08); key accuracy 0.8505 against 0.8461. R6 against hold-last: -0.6 %
    / -2.3 % pooled, 1.03 / 1.08 per change (+0.06 / +0.10), modelled 1.23
    (+0.24). So on the user's play R7 is within the bounds against hold-last
    (no flag), and R6 costs up to +0.10 per change (pinned) or +0.24
    (modelled) for at most 2.3 % less error.
  - **(4) Why R7 loses R6's drift** (`RC1e/lat_kinds.py`, the kind of every
    wrong lateral key in every arrival's tail, tail_accuracy's arrivals, all
    four clients; a proxy: it counts (arrival x tail tick x key) triples, so a
    tick re-predicted by several arrivals counts several times - these are
    arrival-tail key-ticks, not the predictions the engine keeps; the
    engine-faithful model, `record_model.py` in (1), shows the same direction
    much smaller: lateral held-late corrections on the 42-tick d0 R6 772, R7
    815, +6 %, while the early releases fall 1163 -> 1018, so the number of
    corrections barely moves and the shift is in how long each phantom push
    lasts):
    resync_at150_down d0 - phantom (a lateral key predicted held that the
    player had released) R1 129482, R6 24466, R7 66033; missing (released
    while held) R1 132737, R6 190498, R7 164035; t1x1 - phantom 20772 / 8833
    / 15378, missing 21049 / 17865 / 16507; real 42 - phantom 32864 / 21427
    / 28240, missing 32631 / 39312 / 35969. R7 does what it was built for -
    fewer early releases, so fewer corrections - but every hold that ends a
    few ticks after reaching its median is now held for the whole 20-42-tick
    tail: on the 42-tick d0 the phantom key-ticks (proxy) rise 2.7x over
    R6's while the missing ones fall 14 %, and a phantom lateral key pushes the mirror
    sideways for the rest of the tail where a missing one only leaves out a
    push. R6's lower drift and its extra rewinds come from the same early
    release: a release that is wrong costs a correction, one that is right
    saves the phantom push. Only this one switch (hold once r >= H) was
    measured, as the user's option asked.
  - **(5) The pass rule (the user's ruling).** (a) resync_at150_down d0:
    input rewinds per real change R7 0.88 <= hold-last 0.80 + 0.1 - **passes**.
    (b) no worse than R6 elsewhere: the screen bound (rewinds per real
    change <= R6's + 0.1 on every screened cell) - **passes** (R7 - R6 <=
    +0.00 on all 30 cells and real play); the pinned bound (pooled mean <=
    1.02 x R6's and no client more than 5 % worse than under R6) - **fails**:
    42 d0 1.107 x (clients +2.6 / +12.8 / +17.7 / +9.4 %), 20 t1x1 1.0202 x
    (client 2 +6.2 %), 42 d0_ud1x9 0.932 x but client 1 +13.0 %, 20 d0_ud1x1
    1.098 x (client 0 +41 %, 0.016 against 0.011 m), real 42 1.022 x (real
    20 1.009 x passes). **R7 is not selected; passed = false.** The choice
    between R6 as ruled (its +0.15 per change on the 42-tick d0, +0.10 on
    the real play at 42, the lowest drift), R7 (rewinds near hold-last's,
    drift between hold-last's and R6's) or hold-last goes back to the user
    with these numbers.
  - **The gate** (quick, 15 minutes): verification wall time about 11.5
    minutes (02:16:33-02:27:59): the rule-7 build and the restore build, the
    screens, 16 + 24 pinned replays, two live captures of about 1.5 minutes
    each (the step asked for the two real-play captures; nothing else ran
    during them). No headless or shared file changed, so no wire check, GCC
    check or unit tests were run (nothing they can reach changed). Review
    fix-up round: the six brake cells the first screen left out (d5_brake,
    d15_brake, d30_brake at both lags) screened physics-free (40 s,
    02:34:28-02:35:08; R7 - R6 -0.02 to -0.07 per change, the screen bound
    holds on all 30), the rest docs only (no rebuild, no rerun); the verdict
    is unchanged.

## As built (step B5RX)

Step B5RX is the batch that lands the user's answers to step B5RC's three
stops (2026-10-28: "按推荐继续" - go on as recommended - and, for RC3, the
correction quoted under item RC3A). Base e0caa8a (protocol 13, journal v7;
no protocol or journal change). B5RC committed nothing; its code and
measurements are kept in `.local-tools/work/B5RC/` (`rc3.patch`,
`r12b.patch`, `all_code.patch`, `docs.patch`, the git ref
`refs/b5rc/rc3-code-v2`). This step changes nothing in CK2, physics_RT or
IVP and records no engine change.

**Item RC2S - RC2 not built (the user's choice, option (c)).** The design
(§3) reuses "a sleeping body's per-body reads and blob bytes ... keyed by
the body and its sleep generation"; neither exists in a reusable form
(B5RC, checked by the orchestrator):
- *The blob has no per-body bytes.* `capture_world`
  (`BallanceMMOCommon/src/physics/physics_savestate_capture.cpp`) numbers
  every collision checker (`add_collision`, :146-158: a checker's number is
  its encounter order), contact, friction system, simulation unit and
  overlap node at first encounter, in one walk over the whole world, and an
  object's record refers to them by those numbers (`ov_collisions`, the
  hull entries' collision, the exact / invalid / friction synapses, the
  core's `unit`, `ov_node`, a controller's friction system). A checker made
  or freed for any body earlier in the walk renumbers every later record,
  so a sleeping body's bytes change while it sleeps.
- *No sleep generation.* IVP keeps no wake or write counter; adding one is
  an IVP source change, which the engine-change discipline allows only for
  a proven discrepancy, not for a cost cut.
- *The blob is on the wire*: SessionResyncState carries it
  (`session_resync_state_msg.hpp`:37), so a format change is a protocol
  change.

The two options kept for later (CPUPROF's split: a sleeping body costs a
capture 4.5-4.7 us, 3.2 of it the blob, 1.3-1.4 the per-body reads):
(a) a per-object blob format with name-keyed references (checkers by
object pair and edge, contacts by system and pair), a sleeping object's
section reused while its live fields compare equal - at most 3.2 of the
4.6 us per sleeping body; a protocol bump (the travelling blob) and changes
to capture, restore, describe, diff and the validator; (b) a cache of the
rollback engine's per-body reads (`rollback_engine::capture`'s get_body /
extra / contacts) for a body asleep in the previous record and now, every
engine write (restore, set_body, recreate, destroy, rows) invalidating it -
at most 1.3-1.4 us, no format change; a script that moves a sleeping body
outside the engine's writes (a teleport through physics_RT) is the
exactness risk to rule out first. Docs only: no code.

**Item RC3A - RC3 snapshot-free by default (option (a), the user's
correction).** The user, 2026-10-28, verbatim: "不对，RC3要保留快照？现在应该
尽可能假设快照不存在来做" (no - RC3 keeps the snapshots? From now on work as far
as possible as if snapshots did not exist). It replaces the recommended
option (c) (the switch landed default on) and supersedes, for this work, the
2026-10-24 rule that the snapshot fallback stays until the known drift
sources are fixed.
- *As built* (B5RC's code, `refs/b5rc/rc3-code-v2`, with the default
  flipped): `client_core_messages.cpp` `apply_snapshot` - with the
  session's snapshots off (`snapshots_run()` false: SessionStart's interval
  byte 255) and `core_config::body_set_corrections` false, a full snapshot's
  names and rows go to the mechanism registry and the journal (the RECEIVED
  checkpoint) and the function returns before the rollback engine compares
  it: no compare, no rewind, the first four logged ("taken for its names
  only ... (step RC3)"), counted in `snapshots_uncorrected_`.
  `core_config::body_set_corrections` (`client_core.hpp`) defaults to
  **false**; the headless client's default is false and
  `--body-set-corrections` is the measured control arm (the pre-RC3
  behaviour; B5RC's `--no-body-set-corrections` spelling is gone); the
  retail mod sets it false explicitly in `core_host.cpp`'s config block
  ("every field set here"). The server, the legacy retail path (`session
  core off`) and a snapshots-on session are unchanged. Unit test
  `SessionClientCoreSnapshot.ASnapshotsOffSessionTakesABodySetSnapshotForItsNamesOnly`
  (the default false; interval 255 without corrections: no compare, no
  rewind, logged; 255 with the control switch and interval 2: rewinds as
  before).
- *The drifts this exposes* (handed to step SFD; B5RC's paired replays of
  `core_ref_s7`, control = the body-set corrections, RC3 = without them;
  identical up to the control's first snapshot correction; reproduced at
  B5RX with the B5RX build, `rB5RX_ctl` against `rB5RX_rc3f`: the same
  remote-ball and mechanism statistics; the per-plate means are B5RC's
  `rc3_where.py`): `resync_snapoff` c1 a Ball_Stone mirror (of 4278410334) 0.109 m
  at 1768 -> 21.3 m, remote balls mean 0.22 -> 0.95 m, p99 3.0 -> 14.8 m,
  and the client asks for a resync the live client never did (the replay
  stops there at 2974 of 3635 steps: "expected a clock read at
  resync_limiter"); `swing_snapoff` c3 remote-ball p99 5.0 -> 17.2 m (the
  control's 3.29 m correction of Ball_Stone_BMMO_3493379180 at 3163), c1
  p99 3.4 -> 4.5 m; `l8_cross` c0 / c2 / c3 the P_Modul_29 plates 0.03 ->
  1.4-1.8 m mean, worst 1.6 -> 3.6 m (P_Modul_29_Platte05 at 574; the
  control's 0.62 m correction of P_Modul_29_Platte02 at 561), mechanisms
  mean 0.005 -> 0.25 m on c0 / c3; c1 identical. Up to 17 % fewer replayed
  ticks (swing c0 1124 -> 996). Run outputs:
  `maps/arena-test/core_ref_s7/*/replay/rB5RC_{ctl,rc3}` (B5RC) and
  `rB5RX_ctl`, `rB5RX_rc3f` (B5RX; `rc3_drift.py` in `work/B5RC/`).
- *The wire check* (`wire_check.sh rB2SX rB5RX rB5RXw`, strict, over
  `core_ref_s7`; every arm there is snapshots off - the set has no
  snapshots-on arm, so the "snapshots-on arms ALL SAME" clause has nothing
  to run on; the unit test covers interval 2): DIFFERENCES, as expected,
  each attributed - the clients with no body-set correction are SAME /
  IDENTICAL (resync c0, c2; swing c2; l8 c1), and every other client's
  first journal difference is the old build's first body-set snapshot
  CORRECTION record, before which every common TICK hash is equal: resync
  c1 tick 1768 (Ball_Stone_BMMO_4278410335 0.109 m, 1763 common ticks),
  resync c3 2438 (Ball_Stone 4.12 m, 2434), swing c0 3163 (the snapshot of
  3163 compared and refused as past the reach at 3230 - the old build
  journals that correction, RC3 does not compare it; 3243 common), swing c1
  3163 (Ball_Stone_BMMO_3493379180 0.86 m, 3182), swing c3 3163 (the same
  body 3.29 m, 3180), l8 c0 / c2 / c3 561 (P_Modul_29_Platte02 0.62 m, 555).
  The strict replay then stops at the old build's snapshot rewind ("expected
  a clock read at run_limit, the capture has one at rewind_start"): the
  sends differ only from the first send after that rewind's live tick (c1
  1790 = 1768 + 21 + 1, c3 2461, swing c0 3270 after its 3227 balloon
  rewind, c1 3209, c3 3207, l8 563). The full-length RC3 sessions are the
  free-clock replays above.
- Solo, snapshots off (no remote, no body-set correction): `solo1` / `solo2`
  replayed strictly by rB5RX against their live journals: sends SAME,
  journals IDENTICAL (`rB5RX_solo`).
- *Two consequences found by the review (gate round)*. (1) The frozen-clock
  re-anchor (design 4.1, the core design's 7 Q1): only `on_snapshot` asked
  for it, and with no snapshot rewinding in a snapshots-off session nothing
  did - an input, lifecycle, sector or forced rewind on a stopped clock
  (`rollback.hpp` `restore_and_replay`, the `if (frozen)` branch: the bodies
  set, the history dropped, `true` returned) left the world on its anchor
  while the ticks ran on. Every core rewind outside a snapshot now goes
  through `session_client_core::rewind_world` (`client_core.cpp`), which
  calls `rewind_to` and asks for the resync (owed, so the 2 s limiter
  cannot lose it) when `stats().frozen` grew; the five call sites in
  `client_core_frame.cpp` and `client_core_lifecycle.cpp` use it. The
  headless clock never stops, so no capture's reads change. Unit test
  `SessionClientCoreSectorRewind.AFrozenRewindOutsideASnapshotAsksForAResync`
  (a forced rewind, frozen: one request; running: none); the B1 test of a
  refused sector rewind now sees the frozen request first and its own
  request finding it in flight. (2) The wire check's coverage: under the RC3
  default the strict replay of a snapshots-off multi-client capture of
  `core_ref_s7` stops at the old build's first body-set rewind (l8_cross
  c0 / c2 / c3 at 703 of about 2340 steps), and two builds that stop at the
  same record compare equal while most of the session went unchecked.
  `wire_check.sh` now fails a replay that does not reach its capture's end
  marker with rc 0 ("COVERAGE-SHORT", counted as DIFFERENCES), and takes
  `OLD_EXTRA` / `NEW_EXTRA` / `EXTRA` client arguments (`e0_replay.py
  --extra`). Until a reference set is re-captured under the RC3 default
  (queued: `core_ref_s8`, the same arms, at the next step with live runs
  to spare), the set's determinism gate is the control arm:
  `EXTRA=--body-set-corrections wire_check.sh OLD NEW TAG` (with a pre-RC3
  OLD: `OLD_EXTRA= NEW_EXTRA=--body-set-corrections`), which replays every
  capture to its end. Also: a free-clock replay no longer warns "arguments
  differ" (`--replay-free-cost-clock` is left out of the comparison), and
  `body_set_corrections` is read on the rollback path only - with rollback
  off (headless `--no-rollback`, retail `session rollback off`) the blend
  ladder still sets the balls from every snapshot, as before (said in its
  comment).

**Item R12D** (the late-player item of the batch; late-player design §2.5,
"As built (step B5RX)"): R12B is not built, and the end-of-replay
savestate check's text diff - the headless client's diagnostic - runs after
the rewind's last cost-clock read, out of the cap's meter; B5RC's
`--replay-free-cost-clock` landed with it as harness tooling (the replays of
item RC3A use it).

Verification (quick tier, 00:36:37-00:43:45, about 7 minutes): x64
BUILD-OK, x86 CLIENT-BUILD-OK (the Arch server not rebuilt: no server file
changed), GCC syntax 29/29 ok, `BallanceMMOMessageTests` 554/554; the wire
check and the replays above (all offline, each under 40 s). No live run:
R12D's cap was shown by the free-clock replay, RC3A's retail path is one
configuration line equal to the core's default (the retail smoke run is
deferred to the final verification). Gate round (00:53-01:00, about 7
minutes, after the review's fix-ups): the build trees record no header
dependencies (the localized MSVC `/showIncludes` prefix does not match
`msvc_deps_prefix`: `ninja -t deps` lists 0 for every object), so every
source that includes `client_core.hpp`, `rollback.hpp` or
`inbound_capture.hpp` was touched and rebuilt (x64 29 objects, x86 14);
x64 BUILD-OK, x86 CLIENT-BUILD-OK, GCC syntax 29/29 ok,
`BallanceMMOMessageTests` 555/555;
`OLD_EXTRA= NEW_EXTRA=--body-set-corrections wire_check.sh rB2SX rB5RXg
rB5RXgc`: ALL SAME, every replay to its end (12 sends SAME, 12 journals
IDENTICAL); the RC3 default replays (`rB5RXgd`) equal the implement round's
(`rB5RXw_new`) in all 12 sends and journals, the 8 known short ones
flagged COVERAGE-SHORT; solo1 / solo2 strict against the live journals:
SAME / IDENTICAL (`rB5RXg_solo`). No CK2, IVP or physics_RT change and no
engine change in this step; no protocol or journal change (13 / v7).

*Retail (run at step RTX, 2026-10-28; rollback-engine-gaps design, "As
built (step RTX)").* The retail smoke run of the RC3 default and of the
frozen-clock re-anchor: three two-copy L11 sessions, core on, snapshots
off (`scripts/retail/rtx_1..3`), client 1 forced 13:24 in each. Every core
rewind went through `rewind_world` and none asked for a resync: `rb` 114 /
10, 94 / 14, 103 / 17 (client 1 / client 2), `frozen=0` and `resyncs=0/0`
on all six, no "resync requested" line, `far=0`, `starved=0`. The other
half - a frozen clock asks once - is not reachable on retail: the world
never pauses in a session and no verb stops the clock; the unit test
`SessionClientCoreSectorRewind.AFrozenRewindOutsideASnapshotAsksForAResync`
covers it.

## As built (step SFD)

Step SFD root-causes the three drifts the body-set snapshots used to hide
(item RC3A above; the user, 2026-10-28: "不对，RC3要保留快照？现在应该尽可能假设快照
不存在来做"), each to the tick and the state where the client's world first
leaves the server's. Base 92836bd (protocol 13, journal v7; no protocol,
journal, CK2, physics_RT or IVP change; no engine change). Captures:
`core_ref_s7`'s snapshots-off arms, replayed with the RC3 default and
`--replay-free-cost-clock`; the server's side is its own journal (FULL rows
every 2 ticks) and SimTool replays of it.

**Method (the tools of the step).** A client's LOCAL checkpoint is its live,
predicted world, so it cannot say where a world first left the server's at
150 ms. The final state of a tick (after the last rewind that replays it)
is read from the headless client's `--trace-body NAME --trace-ticks A B`
lines, which now end in the body's row in hex (`| row pos=... lin=...
sim=`, `session_client.cpp` `describe_traced_body`), and compared bit for
bit with the server's FULL row (`.local-tools/work/SFD/final_bits.py`;
`hash_runs.py`, `first_body_div.py` for the LOCAL rows). The hidden
collision state is compared through savestate blobs:
`BMMO_DUMP_FINAL_LAG=N` with `--dump-savestate A B DIR` also writes
`DIR/final/<t>.sav`, the record of tick t taken N live ticks later (past
every rewind's reach, so the world's final state of t: an ordinary rewind
does not rewrite the dumped files), and the SimTool's `--replay-session`
with `BMMO_REPLAY_SAVESTATE_DUMP=FROM,UNTIL,DIR` writes the server world's
blob of each tick (`sim_tool.cpp`); `--savestate-describe` and
`.local-tools/work/SFD/sav_body_cmp.py` compare one body's sections across
two worlds (names mapped).

**(1) resync_snapoff c1, the Ball_Stone mirror of 4278410334 (0.109 m at
1768, 21.3 m by 2460, an extra resync).** Two causes, in this order.
- *First divergence: tick 296, a new cause, fixed (headless only).* The
  mirror is bit-equal to the server's row through 294 and 1.4e-7 m off at
  296, the tick it touches c1's own ball. The own ball has been 1.01e-6 m
  off since its birth at 231, with every velocity bit equal: the headless
  client's spawn offset (`capture_own_physicalize`) moved the body to the
  DOUBLE sum of its position and the float offset (`x = -20 + -4.2f =
  -24.1999998`), while the Physicalize event carries the FLOAT pose
  (`-24.2f = -24.2000008`) that the server's clone and every other
  client's mirror are built from - a float half-ulp at 24 m. The retail
  host (`core_host.cpp`) adds the offset to the entity's float matrix
  before the body is made, so it never had this. Fix
  (`session_client.cpp`, the spawn-offset block): the target is the retail
  sum `own_capture_.pose[3][k] + offset[k]` in float. After it the own
  ball is bit-equal to the server through 1236 and the mirror through 1658.
  Drift on the same captures (RC3 default, `rB5RX_rc3f` -> `rSFD_rc3`):
  resync c1 remote balls mean 0.949 -> 0.402 m, p99 14.76 -> 4.85 m, max
  21.26 -> 5.30 m; resync c3 mean 0.176 -> 0.137 m, p99 2.05 -> 1.54 m;
  l8_cross c1 / c3 ball max 0.9 / 0.7 mm -> 0.3 / 0.1 mm; every other cell
  unchanged.
- *Then: the own ball's collision bookkeeping differs from every clone of
  it from its birth (new; the fix is a choice, see the stop below).* With
  the fix the own ball's row (position, rotation, both velocities) is
  bit-equal to the server's at 1236 and differs at 1238: it crosses the
  seam of `A_Floor_017` / `A_Floor_020` and its lateral speed drops
  (-0.173 -> -0.040 m/s) one tick earlier than on the server (1238 against
  1239-1240), with the same inputs. c1's ball as a MIRROR (a clone) in
  c0's, c2's and c3's worlds stays bit-equal to the server through 1300.
  The blobs show why: at 1236 the server's clone and c3's mirror of c1's
  ball have the same hull entries (each mindist's recheck value) and the
  same overlap element (centre, radius), modulo numbering; c1's own ball
  has other hull entry values and another overlap centre (0x1.a21ae8p+8
  against 0x1.a2d2ccp+8, 0.7 m apart) and radius. The difference is there
  at the birth tick and on every own ball, the spawn offset or not: c0's
  own ball (join order 0) at 231 has hull value 14.74 and
  `time_of_next_reset` 16 where the server's clone has 0.045 and 0 (c1
  13.07, c3 14.36), and its overlap centre is not the spawn point; IVP's
  hull and overlap schedules then keep that phase for the body's life
  (`IVP_Hull_Manager::reset_times` shifts every entry by the same amount).
  The retail scripts create the own ball's body and move or recheck it
  inside its creation frame (what exactly adds the 13-15 to the hull is
  not pinned down: the bookkeeping, not the rows, shows it); a clone is
  made once at the event's pose. Rows equal, so nothing shows it until a
  contact that begins inside a PSI is found at another PSI - c1's seam at
  1238 (own ball 3.7 mm off), whose ball then hits the mirror at 1660
  (three-ball contact with c0's mirror; the mirror bit-equal through 1658,
  2.4e-5 m and 0.0085 m/s off at 1660), which grows by contact: 0.030 m at
  1768 (0.109 m before the fix), 1.69 m at 2200, 5.28 m at 2458 (21.3 m
  before the fix). Measured in the headless client; the retail mod creates its own
  ball through the same scripts and is expected to carry the same state
  (not measured: no retail run).
- *Also in c1's world (owned by queued items):* G-c one-client wakes
  (P_Modul_01_Pusher 612, P_Modul_30_Wippe 1060 - 0.34 m at 1100,
  P_Modul_34_Schiebestein 1276, P_Modul_37_Bridge 1358, P_Modul_03 1964:
  asleep in c1's world, woken on the server; world agreement, Q9) and the
  balloon from 2124 (exception (c)). None of them touched the own ball or
  the mirror before 1660.
- *The extra resync is a replay artefact.* The replayed own ball dies at
  2464 and is reborn at 2662, the live one at 2470 / 2668 (the live run was
  corrected by its snapshots); the capture holds the server's echo of the
  live birth only, so the replay reports "own birth moved 2662 -> never"
  and asks for a resync at 2876 (the replay stops there, 2974 of 3635
  steps, as before the fix). A live session's server echoes the birth the
  client really sent.

**(2) swing_snapoff c3, remote-ball p99 5.0 -> 17.2 m (the control's 3.29 m
correction of c0's ball at 3163).** Owned by exception (c), the balloon. The
mirror of c0 (Ball_Stone_BMMO_3493379180) is bit-equal to the server
through 3108 (4e-9 m at 3110). At 3111 the server's c0 ball hits
`PE_Balloon_Platform` (velocity 17.62 -> 14.81 m/s, vy +2.12); in c3's
world the platform is 0.88-0.91 m elsewhere (the balloon has differed since
2862: platform 0.013 m, balloons 0.02 m, flags c0 s1) and the ball passes
under it (contact found only at 3113). Same event, same tick on c1 (the
B5RX "c1 p99 3.4 -> 4.5 m"; the mirror bit-equal through 3108, 0.085 m at
3112). The balloon is never compared (exception (c)), but a ball that
touches the airship carries the balloon's divergence into the balls: this
is a consequence of that exception, for the user (the stop below).

**(3) l8_cross c0 / c2 / c3, the P_Modul_29 plates (0.62 m at 561, mean
1.4-1.8 m, worst 3.6 m).** Owned by world agreement's G-c (the client
proximity sensors; P_Modul_29's 80 m probe #47020 wakes the plates). Every
P_Modul_29 plate and P_Modul_01_Pusher is bit-equal to the server through
552 on every client; at 554 the server's plates are simulated (flags 3)
and on c0 / c2 / c3 asleep (flags 2) at their rest pose. The server's
journal holds the cause: c1 (2010866393, the ball within 80 m) reports
`BodyRevived P_Modul_29_Platte04` and `P_Modul_01_Pusher` at 554; the
server applies it, the other members ignore it (G-c / G-c3, "a wake that
only the reporting client's world made reaches only the server"); c2's own
probe wakes them at 614, too late. c1's world is the one that agrees
(plates within 0.02 m). The same signature, smaller, for
`P_Modul_25_Bridge001` (c0's report at 407, 0.011 m on c1-c3) and
`P_Modul_37_Bridge` (c3's at 501, 0.01 m on c0-c2). Not built here (the
world-agreement track; its Q9 waits for the user).

**The wire check after the fix** (control arm, the B5RX gate:
`EXTRA=--body-set-corrections wire_check.sh rB5RX rSFDh rSFDwc2`):
DIFFERENCES, every one attributed to the fix - the clients of join order 0
(resync c0, swing c0, l8 c0) are SAME / IDENTICAL; every other client's
journal first differs at its own ball's birth tick (l8 212, arena 231:
the 1 um), sends SAME on 10 of 12; resync c3's sends differ and resync c1
stops at 1886 steps (COVERAGE-SHORT: the control's snapshot rewind meets
a world that no longer needs it). `core_ref_s7`'s live journals are
therefore no longer the replays' strict reference for the clients of join
order 1-3; the queued re-capture (`core_ref_s8`) takes the fixed build.

**Stop (the user's choices).** (A) The own ball's collision bookkeeping
(above): options - (a) build every world's copy of a player's ball the
way its owner's scripts did (the clone put through the same creation-frame
move / recheck; needs the retail path pinned down first and, if the
pre-move pose must travel, a protocol field; changes the server and every
mirror); (b) make the owner's ball match the clone at birth: re-create or
reset its IVP collision state at the pose in the creation tick's
PreSimulate (where the hosts already capture it) - a host capability the
core drives, with an access-only physics_RT / IVP-private-access engine
change, and the own ball's navigation controllers re-attached (a
re-created body); (c) leave it (rare: it needs a contact that starts
inside a PSI next to another ball), with no snapshot to correct it, only a
resync. (B) The balloon leak: (i) accept it (only a ball that touches the
airship), (ii) bring the balloon back into the agreement work (changes the
user's decision "除了气球应该别的都是需要修的"), (iii) keep the airship out
of the balls' collisions in every world (changes the level's behaviour).

## As built (step OWNB)

Step OWNB answers SFD's stop (A) with the user's answer 2 of 2026-10-28,
verbatim: "应该尽可能遵循原版脚本" (follow the original scripts as far as
possible: every world's copy comes about the way the original scripts make the
owner's ball). Base be38d4d (protocol 13, journal v7); no protocol, journal,
CK2, physics_RT or IVP change, no engine change.

**The sequence, pinned down first (headless, a probe).** `BMMO_HULL_PROBE=<name
prefix>` with `BMMO_HULL_PROBE_FILE=<path>` (`physics_state.cpp`, the text from
`describe_hull_bookkeeping` in `physics_savestate_capture.cpp`; diagnostics
only, inert without the variables) appends a body's IVP collision bookkeeping -
the hull manager's clock and values, every hull entry (kind, value), the
overlap element (centre, radius), the core's position, velocity, queued
velocity change and mass - at its creation, its script wake, the start of its
first 8 PSIs, every `write_body_state`, and the hosts' points (`own_capture`,
`own_impulse`, `own_remade` in the headless client; `clone_made`,
`clone_impulse` in `physics_world::apply_event`). What the original scripts do
in the birth frame: `BallManager/New Ball` puts the entity on CurrentLevel[0,3]
three seconds earlier; in the birth frame the Physicalize block calls
`CreatePhysicsObjectOnParameters` (the body made at the entity's matrix,
`ensure_in_simulation`, `enable_collision_detection`) and `NotifyScriptWakeup`;
the PreSimulate pass runs the Physics WakeUp callback (`ensure_in_simulation`,
a no-op on an awake body), then the capture and the spawn kick
(`async_push_core`); then the PSIs. There is no move and no recheck of the body
inside its creation frame. On `core_ref_s8/solo1` (join order 0, no rewind)
the own ball and the server's clone are bit-equal in every probed field at
creation and through the first 8 PSIs: the server's clone and every mirror
(`physicalize` at the event pose, the same call with the same arguments, then
the same kick) already come about the way the scripts make the owner's ball.
SFD's attribution to "the level scripts moving or rechecking the body inside its
creation frame" is wrong; the 13-15 in the hull and the reset 16 come from two
other places:

- *The headless birth cell (join order 1-3, headless only).*
  `capture_own_physicalize` moved the script-made body to its birth cell with
  `set_body_state` (wake keep, recheck): a beam of a fresh body. IVP's
  `set_transformation` added the 4.2 m to the hull manager (`jump_add_hull`:
  last = next = 4.2) and rechecked the overlap element (its entry 2.0 -> 6.2,
  `tlast` set), where the clone made at the offset pose has 0. The retail host
  never had it: `core_host_on_physicalize` moves the entity before the block
  makes the body (BMLPlus broadcasts OnPhysicalize first).
- *The rewind's parking path (both hosts).* An own birth inside a rewind's
  window kept the parking path by design (B8EG item EGB, `keeps_parked`): the
  anchor's blob does not know the body (restore "unknown=1"), so it keeps the
  LIVE body's hull manager - from ticks after its birth: reset 16 =
  int(6.17 + 10), set by a live check_for_reset at the birth time 6.17 s - and
  is beamed to its birth pose (`write_body_state`, another `jump_add_hull` of
  the distance rolled since) and re-posed after its birth tick. resync_snapoff
  c0 crosses its own birth 231 three times (birth rewinds for the three late
  mirrors), which is SFD's 14.74 / 16 on an own ball of join order 0.

**What is built.**
- *Headless: the birth cell before the block (`session_client.cpp`).*
  `own_physicalize_hook` is installed on every Physicalize block of the level
  (`install_physicalize_hook`, by prototype GUID 0x7522370e,0x37ec15ec, once the
  navigation is known: 64 blocks on the arena) and calls `before_physicalize`
  before the prototype's function - the headless form of BMLPlus's
  OnPhysicalize. In a live tick, for the current ball without a body, it flags
  the spawn (the entity on CurrentLevel[0,3]) and moves the entity to its birth
  cell in float, the retail sum; `capture_own_physicalize` takes the flag from
  it (the pose is the entity's, where the block made the body) and no longer
  moves the body. Without the hook (none installed) the old spawn test runs
  and the cell is skipped with a log line.
- *Core: an own birth the scripts made is remade in the replay
  (`client_core_lifecycle.cpp` `scripted_own_life`, `destroyable_own_birth`,
  `destroy_moved_own_birth`).* With `own_body_control` and without the new
  `config.own_births_parked` (headless `--own-births-parked`, the control arm),
  an own birth made by the scripts at its stamp, not moved, of the body's
  current lifetime, is no longer kept parked: `keeps_parked` says no, the
  restore's `destroy_body` takes it out (`unmake_own_body`) and queues it in
  `own_due_` (`scripted`), and the replay's drain makes it again at its birth
  tick after that tick's remote events - where the live frame's scripts made it
  - through `remake_own_body` (the pose, recipe, group and spawn kick its
  Physicalize carried: the server's clone's way, measured equal to the
  scripts'). What no replay reaches is made live (`settle_own_due`).
- *Its step is its state (`client_platform.hpp` `own_birth_remade`,
  `session_host_world.hpp`).* The first build compared the remade body with its
  birth record after the step and wrote the record over it on a mismatch, as a
  parked body: on resync c0 the respawn at 2650 met a mirror the live frame did
  not have yet (velocity 2.80 -> 0.18 m/s in the first PSI, an impact) and the
  record's stale state was written back, which made the pinned resync c0 remote
  balls worse (p99 1.40 -> 21.1 m). The record was stepped with the remotes
  known then, the replay with those known now: like a mirror remade from the
  inputs (step LB), the core's `own_birth_remade` hook joins
  `rollback_world::birth_from_inputs`, and the step stands.
- **Adaptation (retail).** Retail declares no `own_body_control` (core design
  §2.3: the game's Physicalize block owns the own ball; R6 is the step that
  gives it), so on retail an own birth inside a window keeps the parking path
  and its live hull bookkeeping; the step text's "make the server's clone and
  every mirror come about by the same sequence" needs nothing there (they
  already do), and the retail live birth is the clone's (the cell before the
  block; measured, see *Retail* below). The retail remake is queued in the
  core design's R6 row (the R6 step removes the parking of an own birth
  inside a window when it declares own body control), not built here; no
  retail code changed.
- **Review fix-ups.** A binding test,
  `RollbackBinding.SessionWorldKeepsARemadeOwnBirthAsStepped`, holds
  `bind_session_world`'s composition of `own_birth_remade` into
  `birth_from_inputs` (the lambda whose omission gave the first build's 21.1 m);
  `ScriptedOwnBirthInsideTheWindowIsRemade` also drains the queued birth
  (`drain_pending_lives` 121: nothing; 122: one `make_own` at 122). A
  `scripted` due no replay reached is settled live like EL8's window lives,
  its kick indexed by the current tick (comment at `settle_own_due`). The
  Physicalize hook is installed with the navigation keys
  (`discover_navigation`) only: the own capture
  (`arm_own_physicalize_capture`) has the same precondition, so no capture
  ever runs without the hook.

**Evidence (core_ref_s8 and s7 captures, headless, rOWNB = this tree).**
- *Birth.* The probe, the remade own ball against the server's clone (SimTool
  replay of the arm's server journal): resync c1 (join order 1) after the third
  birth rewind - SAME in every field at creation and through 8 PSIs (before:
  hull last 4.2 at creation from the cell, then the parked body's reset 16);
  resync c0 - SAME through 7 PSIs, then one overlap-connector entry value
  differs (another ball's); l8_cross c1 (Ball_Wood, 212) - SAME through the 6
  PSIs both sides probed. At the live birth (no rewind yet) the own ball is
  equal at creation and the first PSI and parts at the second, when the
  server's clone already has the late mirrors the client does not.
- *Unit test.* `SessionClientCoreOwnLives.ScriptedOwnBirthInsideTheWindowIsRemade`
  (control x own_births_parked; take-out, the queued remake at the birth tick,
  `own_birth_remade`, a retired or later-generation life refused, the drain
  at the birth tick) and the binding test above. 582/582.
- *Retail (one two-copy L11 session, Arch server with placement on,
  snapshots off, core on, `BMMO_HULL_PROBE=Ball` per game copy; the server's
  clones probed by a SimTool replay of the server journal,
  `.local-tools/work/OWNB/retail`, `ownb_retail.py`).* Both own balls
  (Ball_Wood, join orders 0 and 1) against their server clones: SAME in every
  field at creation and at the first PSI; at the second PSI the clone has one
  more hull entry (type 0, 0.095) - the server's clones of both players are
  born at the same tick, while each client's mirror of the other arrives late
  and is made in a rewind's replay (the headless live birth parts at the same
  PSI the same way). That rewind (rb 3, parked 1) is cause 2 on retail: the
  restore's `write_body_state` puts the parked own ball back at its birth pose
  with its later hull state (vpsi 6.21, `time_of_next_reset` 16) where the
  clone has 0 - what R6 removes.
- *Pinned paired replays (own keys pinned to the live run's,
  `RC1d/gaps/pinned_replay.py`), resync_snapoff, remote balls against the
  server's rows, rEL9 -> rOWNB:* c0 mean 0.166 -> 0.059 m, p99 1.40 -> 1.18 m,
  max 2.35 -> 2.35 m (mechanisms mean 0.0170 -> 0.0153); c1 mean 0.469 -> 0.110
  m, p99 7.35 -> 2.94 m, max 9.19 -> 5.17 m (mechanisms mean 0.0397 -> 0.0269,
  p99 0.891 -> 0.833).
- *SFD's 1238 seam crossing (core_ref_s7 resync c1) is NOT this.* With rOWNB
  the own ball's final row is bit-equal to the server's through 1236 and
  differs at 1238 by exactly what rEL9 gives (d 0.00298 m, dv 0.131 m/s): the
  birth bookkeeping never caused it. Final-state blobs (`BMMO_DUMP_FINAL_LAG`,
  `BMMO_REPLAY_SAVESTATE_DUMP`, `.local-tools/work/OWNB/sav_obj_cmp.py`) at
  1236: the own ball's core, overlap element, exact mindists, contacts and hull
  values equal the clone's (before: another overlap centre and other hull
  values); what is left are three list orders - two tied hull entries (the
  P_Modul_30_Wippe entry before A_Floor_013/016 in c1, after on the server) and
  the A_Floor_017/020 overlap collision lists (P_Modul_25_Bridge's after the
  balls in c1, before on the server) - and both bodies that set them differ in
  c1's world (the Wippe's last PSI time, the bridge's rotation and spin): the
  G-c one-client wakes SFD listed (Wippe 1060; Bridge001 reported by c0 at 407).
  Pre-existing (rEL9 identical), owned by world agreement (G0-G5). Which of the
  two orders moves the crossing is not isolated. On core_ref_s8 c1's own ball
  is bit-equal to the server through 1698 with both binaries.
- *Wire check (rEL9 -> rOWNB, core_ref_s8): DIFFERENCES, every one attributed.*
  Every replay reached its end marker; sends SAME on 10/12 (resync c0 from
  2661, c1 from 2675: the keep-lane bot steering on the own ball after its
  respawn remake at 2650 / 2668); journals: swing c0 and l8 c0 IDENTICAL; every
  client of join order 1-3 first differs at its own birth (231 arena, 212 L8)
  in the TICK hash only (24-37 ticks, 0 checkpoints differ: the cell's hidden
  state), except resync c0 / c1, whose rows first differ at 2658 / 2674 (the
  respawn remakes). Solo1 / solo2 replays: IDENTICAL to their live journals.
  core_ref_s8's join-order-1-3 journals and resync c0 / c1 are therefore no
  longer the strict reference of the replays (no re-capture made: no protocol
  change; the next set re-captured takes this build).
- *Live smoke (one 4-client 60 s arena session, 150 ms, snapshots off, Arch
  server rebuilt from this tree):* VERDICT PASS, starved 0, resyncs 0, far 0,
  mism 0; 5-8 own-birth remakes per client, births recreated 59 / exact 59,
  parked 0.

## As built (step B6RC)

Step B6RC is the batch of RC4 and RC5. Base d97920e (protocol 13, journal
v7; no protocol or journal change). No CK2, IVP or physics_RT change and no
engine change.

**Item RC4 - the dormant-script encoding (built).** Trafo design 9.4: "record
v3 encodes a script that is inactive and unchanged since its set was built
as one byte, and the restore writes its built state." What the design left
open is how a capture knows "unchanged" without the walk it is meant to
save; as built:
- *The byte.* Record v3's per-trafo-script `u8 present` takes a third value,
  `kTrafoDormant` = 2 (`game/script_replay.hpp`): the script holds its set's
  built state (`trafo_part::built`) and no block follows. A capture writes
  it **exactly when the script's bytes equal the built bytes**, so the
  encoding is canonical: two records of equal states are equal byte for
  byte, which the rollback's live-against-replayed script compare
  (`rollback.hpp`, `state.scripts != live_scripts`) relies on. The record is
  the process's own (the wire and the journals carry the portable record,
  which is unchanged), so the value needs no record version and no protocol.
- *Knowing it without the walk: the ledger.* `trafo_part::dormant`, one
  entry per script of the trafo list, mutable (a cache over the world, never
  state; `rebuild_part` starts it empty). It is SET only by a capture that
  walked a script and found its bytes equal to the built ones (the ledger
  heals), and by a restore that wrote the built bytes (a dormant entry, a
  script the record does not list, or a recorded block equal to the built
  one); it is CLEARED by everything that can change a script:
  `execute_trafo` (the trafo frame, live and in the replay: a script whose
  root has `CKBEHAVIOR_ACTIVE` or a next-frame flag when the frame reaches
  it, one it executes - also one another script of the list activates
  inside the frame - and one that ends the frame active or pending),
  `run_trafo` (the input's next-frame flags), `remake_type_set` (the
  explosion executed once), a restore that writes other bytes, a missing
  script. An entry nobody set is unknown: the script is walked, as before.
- *The guard.* A capture that finds a ledger entry set still compares the
  root's runtime flags and active inputs / outputs with the built block's
  first words (`root_activation_is`) and walks the script when they differ
  (`dormancy().guard_trips`: a writer outside the hooked paths). The sets'
  built state is never active or pending (`build_trafo_set`'s I7), so a
  running script never passes it.
- *Localize* (the resync's portable record made local) writes a localized
  block equal to the built one as the byte too, so a localized record of a
  state is the record a capture of it gives (T2's check (5), "v2 localized
  == v3", holds).
- *Diff* names a dormant-against-block difference ("trafo <script>: built
  vs running") and walks on.
- `execute_frame`'s trafo argument is now the part (`const trafo_part*`,
  `rollback_binding_scripts.hpp`); `trafo_frame` calls `execute_trafo`.
- The restore still writes every listed script (a dormant one from its
  built bytes): the saving is the capture's walk, where design 9.4 and
  CPUPROF put the cost (107 us per live and 80 us per replayed capture for
  the 32 scripts).

Checks (quick tier, 02:11-02:17, about 6 minutes of wall clock): x64
BUILD-OK, x86 CLIENT-BUILD-OK (header deps recorded in both trees), GCC
syntax 29/29 ok, `BallanceMMOMessageTests` 555/555. SimTool
`--trafo-record-check` (level 2, two sets; `.local-tools/work/B6RC/rc/`):
T1 / T2's checks (1)-(5b) PASS with the byte in the records, and the new
check (6) PASS - an empty ledger's capture walks the 16 scripts and writes
16 bytes (healed 16), the next capture from the ledger is the same record
byte for byte (44052 bytes = v2 + 4 + 5 x 16), a root flag set outside
every hooked path is caught by the guard (one trip, that script walked),
and the compact record restored puts it back built. Its cost line
(Ballanced CK2, 200 captures each): a capture 130.1 us walked (62096 bytes
before RC4), 95.6 us from the ledger (44052 bytes), 93.1 us with no trafo
part - the two sets' part 37 us -> 2.5 us. The wire check `wire_check.sh
rSFD rB6RC rB6RCw` over `core_ref_s8` (trafo inputs off there: v2 records
only, RC4 not reached): ALL SAME, 12/12 sends SAME, 12/12 journals
IDENTICAL. The one live run, trafo inputs ON
(`.local-tools/work/B6RC/b6_run.sh`: H1 at 150 ms, four clients, 40 s,
snapshots off, placement off, `maps/arena-test/b6rc/h1_150`): server
journal 2782/2782 matched, starved 0, far 0, trafo_missed 0 on all; the
harness's only FAIL is RC3's "no ball-to-ball interaction observed" (the
harness artifact of `core_ref_s8`'s README). Its captures replayed by the
pre-RC4 build and this one (`REF=b6rc wire_check.sh rSFD rB6RC rB6RCi`):
ALL SAME, 4/4 sends SAME, 4/4 journals IDENTICAL, every replay to its end;
against the live traces (`- rB6RC rB6RCj`): ALL SAME; the script counters
`scr=` (restores / failures / mismatches / float / skips) are equal old and
new on every client (c0 52/0/12/13/3, c1 58/0/11/16/2, c2 60/0/11/10/0,
c3 48/0/19/5/0).

*Review fix-up.* (1) A rewind across a set's retirement failed the
restore: `rebuild_part` copied the built states of the selected sets only,
so once `retire_trafo_sets` deselected a set (it stays `history` ticks
because rewinds can still reach records that list it), a record taken
before that held its dormant scripts as the byte and the restore found no
built bytes for them ("a dormant trafo script without a built state";
before RC4 the block was in the record). `rebuild_part` now keeps every
held set's built states, selected or not; `scripts` (what the frame runs
and the capture writes) stays selected-only, and `built` is only looked up
by id. SimTool check (7) restores the all-dormant record with the first
set deselected and then with every set deselected: both restore, 16 + 16
dormant scripts written. (2) The evidence that the path runs in a session:
the headless client prints `trafo_dormancy: from_ledger= healed= walked=
guard_trips= restored_built=` after its status line in a session with the
trafo inputs. The h1_150 captures replayed by this build (`REF=b6rc
wire_check.sh rSFD rB6RC rB6RCi2`, ALL SAME, 4/4 journals IDENTICAL):
from_ledger 82293 / 85396 / 86738 / 81171, healed 32 each, walked 22731 /
25036 / 25774 / 23821, guard_trips 0, restored_built 1366 / 1483 / 1537 /
1272 on c0-c3 - the ledger serves about 78 % of the trafo scripts'
captures, the guard never refused one, and the identical journals and
`scr=` counters show the byte changes no result. (3) The counters are
plain process-wide statics, diagnostic only (game thread). (4) The build
note above was wrong for the x86 and x64 trees: when the two builds run
in parallel from one Bash command, neither ninja recognises cl's
`/showIncludes` lines (the logs carry 2969 and 12367 unconsumed
"注意: 包含文件:" lines; the same builds run one at a time carry 0), so the
objects they compile get `#deps 0` - the B5RX trap by another road, the
mechanism (a console both cmd.exe share) not isolated. Those objects (x86: `core_host`, `physics_session_client`,
`BallanceMMOClient`, `room_client`, `client_automation`; x64:
`session_client`, `sim_tool`, `physics_world`, `session_runner`,
`server`) were compiled from the current headers, then touched and rebuilt
one build at a time, and every C++ object of both trees now has its deps.
Reruns: x64 BUILD-OK, x86 CLIENT-BUILD-OK, GCC 29/29, tests 555/555,
`--trafo-record-check` (1)-(7) PASS, `wire_check.sh rSFD rB6RC rB6RCw2`
over `core_ref_s8` ALL SAME. The retail host does not print the counters
(no retail file changed). [Step RTX: the retail core host's status line now carries
them, `dorm=from_ledger/healed/walked/guard_trips/restored_built`
(`core_host.cpp`); its three L11 sessions with the trafo inputs on
(`scripts/retail/rtx_1..3`, four trafo explosions and two cancelled-activation
resets, each applied at its stamp in both worlds and on the server,
`trafo_missed=0 run_failures=0` on all six): `guard_trips` 0 everywhere;
rtx_1 client 1 / client 2 `dorm=53150/16/8866/0/1568` and
`19114/16/3158/0/152` - the ledger served 86 % of the trafo scripts'
captures (the headless h1_150 78 %); rtx_2 `44095/19/6830/0/1320`,
`16195/17/2540/0/191`; rtx_3 `43474/19/11771/0/1305`, `15915/17/4244/0/233`.]

**Item RC5 - the incremental restore (not built: stopped for a choice;
written into this note at step B9DX from B6RC's patch
`.local-tools/work/B6RC/rc5_stop_docs.patch`).** The premise check asked
for after B5RC's RC2 stop:
- *The restore cannot skip one body.* `physics_savestate_restore.cpp`'s
  step 2 (`restorer::teardown`, :720-) deletes every friction system and
  contact, every collision root of every live object, removes every
  overlap element from the tree ("the tree ends empty") and clears the
  revive list; steps 4-10 rebuild the units, the checkers (step 5), the
  overlap tree from the recorded nodes (step 6, `restorer::ov_tree`,
  :1171-: one tree every body shares), the mindist lists, the hull
  min-lists in reverse recorded order (step 8, :1342-: ties go newest
  first, so every entry's place is the record's) and the event queue. A
  sleeping body's collision state lives in those shared structures (its
  checkers pair it with awake bodies, its hull list holds other bodies'
  synapses, its overlap node is a node of the one tree), so "skip writing a
  sleeping body" means keeping its part alive through the teardown and
  rebuilding the rest around it in the recorded order: a selective
  teardown.
- *Knowing that a body's entry equals the world's* needs what RC2 lacked:
  IVP keeps no wake or write counter (adding one is an IVP change for a
  cost cut), and the blob's references are world-wide numbers assigned at
  capture (checkers, contacts, units, overlap nodes), so a comparison is a
  capture of the live world in the same numbering - the cost being saved.
- What is NOT collision state, and could be skipped without any of that:
  the core and object field writes (steps 3 / 11) and the entity's
  `SetWorldMatrix` (step 14, :1779-1790) of a body whose recorded fields
  equal the live ones. Not measured; cheap writes next to the 13-19 us a
  sleeping body costs a restore (CPUPROF), and skipping `SetWorldMatrix`
  can change the entity's local matrix by an ulp (CK recomputes it from the
  parent), so it would need its own exactness check.

Options: (a) a selective teardown keyed by an exact per-object equality -
needs RC2's option (a), the per-object blob with name-keyed references (a
protocol bump), and a rebuild of the shared lists (hull min-lists, overlap
tree, event queue) around the kept entries in the recorded order; the
largest exactness risk of the track; (b) only the non-collision writes of
an unchanged sleeping body (steps 3 / 11 / 14): no format, protocol or
engine change, a small part of the 2.8-4.2 % ceiling, to be profiled first
and checked for the `SetWorldMatrix` ulp; (c) leave RC5 unbuilt, as RC2
(option (c) of B5RC's stop). Recommended at B6RC: (c), or (b) only after a
profile of the restore's steps shows steps 3 / 11 / 14 are a real share.
**Decided 2026-10-28:** the user left the choice to the orchestrator
("4. 你决定", §11), who chose (c). Nothing of RC5 is in the tree.

## As built (step RCX)

Step RCX is the rewind-cost close-out, run **with snapshots off** by the
user's decision of 2026-10-28 ("改成关快照", §11; it replaces decision (b) of
2026-10-25). Base 120ee8d (protocol 13, journal v7). Nothing in the code
changed: `physics.trafo_inputs` (`BallanceMMOServer/config_manager.hpp:89`)
stays `false`, because H6 did not pass and the step stopped for the user.
No CK2, IVP or physics_RT change and no engine change.

**The two arms.** The runs are `.local-tools/work/RCX/g.sh`: TR3b's `g.sh`
and `live.sh` merged, with no `rm` and with new labels. Both arms ran on the
same binaries: x64 `build-agent-rB6RC` (byte-equal to HEAD's build), and an
Arch server rebuilt from the clean HEAD tree, id
`ballanced-9c52cd160b73+bmmo-120ee8dd4ace`. Both used 4 clients, 40 s,
150 ms, snapshots off (100000/100000), checkpoints every 2 ticks, the
server's `trafo_inputs` on, RC3's default (no `--body-set-corrections`).
Placement was on in both arms: it has been the server's default since SP10
(*adaptation*: TR3b's baseline predates the placement). The host was idle in
both runs (cpu_sampler max total 10.8 % and 11.6 %).
- `rcx_H1_150` is H4 with snapshots off, which is H1 at 150 ms. It is also
  H6's trafo arm.
- `rcx_H6_ctl0` is H6's control: the same batch onto bay 3, with no trafo.

| per client 0 / 1 / 2 / 3 | `rcx_H1_150` (trafo) | `rcx_H6_ctl0` (control) |
|---|---|---|
| starved | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| resyncs, rebases, far, unmatched, mism | 0 | 0 |
| rewinds `rb` (= input `inrew` + lifecycle `life`) | 47 / 59 / 59 / 56 (26+21, 36+23, 36+23, 33+23) | 14 / 27 / 29 / 27 (5+9, 18+9, 20+9, 18+9) |
| replayed ticks `resim` | 1032 / 1305 / 1290 / 1247 | 296 / 578 / 641 / 598 |
| t_rep (s) | 0.683 / 0.929 / 0.915 / 0.891 | 0.125 / 0.197 / 0.216 / 0.206 |
| t_cap (s) | 1.575 / 1.559 / 1.577 / 1.605 | 0.762 / 0.758 / 0.765 / 0.797 |
| t_rest (s) | 0.095 / 0.133 / 0.123 / 0.115 | 0.008 / 0.016 / 0.017 / 0.016 |
| t_max (ms) | 43 / 43 / 43 / 45 | 13 / 13 / 14 / 14 |
| t_rep per replayed tick (ms) | 0.66 / 0.71 / 0.71 / 0.71 | 0.42 / 0.34 / 0.34 / 0.34 |
| t_cap per live frame (ms) | 0.73 / 0.72 / 0.72 / 0.72 | 0.35 / 0.35 / 0.35 / 0.36 |
| t_rest per rewind (ms) | 2.0 / 2.3 / 2.1 / 2.1 | 0.57 / 0.59 / 0.59 / 0.59 |
| server step p99 per 660 ticks (ms) | 0.71, **3.11**, 1.05, 1.33 (max 4.6) | 0.70, 0.69, 0.65, 0.96 (max 5.0) |
| server journal replay | 2793/2793 matched, 0 checkpoint mismatches | 2810/2810, 0 |

Both harness verdicts read "FAIL - no ball-to-ball interaction observed".
That is the queued harness artifact (REF8: `scripts/arena_map_test.py`
:1941-1942 can only see that interaction through body-set corrections). It is
the only failing line in either run.

**H4 (snapshots off) = H1 at 150 ms: PASS.**
- (a) `tr3fix/pieces.py`: pieces 150/150 bit-equal on all four clients.
- (b) 4 sets on every client.
- (c) Resyncs, rebases, far and unmatched are 0. Starved is 0, against the
  control's 0.
- (e) `trafo_missed` 0, `trafo_original_leaks` 0, run_failures 0.
- With snapshots off, H4's own clause ("0 snapshot corrections naming a
  piece; mism at or below P's band") holds trivially: corrections 0, mism 0.
- (d) needs H1 at 0 ms, which was not run (deferred).

Against TR3b's trafo arm (pre-RC3 / RC4, no placement), the same recipe cost
less: t_rep 0.99-1.15 -> 0.68-0.93 s, t_cap 1.69-1.77 -> 1.56-1.61 s, t_rest
0.13-0.15 -> 0.10-0.13 s. t_max did not move (43-46 -> 43-45 ms).

**H6 (snapshots off): FAIL.**
- What passes:
  - Starved is within the band (0 against 0).
  - The server's step p99 stays far inside the 15.15 ms budget.
- What fails:
  - Every client figure is outside "the control's band + 10 %": t_rep is
    3.2-5.5x the control's, t_cap 2.0-2.1x, t_rest 7-12x, t_max 3.1-3.5x.
  - So is the server's step p99 in ticks 687-1346, the explosions' window:
    3.11 ms against the control's 0.69 ms.

**Cause.** The extra cost has two factors, and they multiply. This is T3's
known cost (trafo design §8, "As built (step TR3, fix-up round)", the root
cause of H4 / H6; CPUPROF's profile), now measured on the current build:
1. *Twice the replayed ticks* (1032-1305 against 296-641).
   - Every remote trafo is a dephysicalize and physicalize of a ball, and it
     arrives a trip late. Lifecycle rewinds are 21-23 per client against 9.
   - Input rewinds are 26-36 against 5-20. These come from the drive after
     the ball changes; they are not attributed further.
2. *Twice the cost of each replayed tick and each capture* (0.66-0.71
   against 0.34-0.42 ms; 0.72 against 0.35 ms).
   - Every world records, restores and replays every member's pieces:
     109-207 births tracked against 8-12.
   - The restore per rewind is 3.5x the control's (2.0-2.3 against 0.6 ms).

Factor (2) is what RC2 / RC5 target. Factor (1) is the cost of trafos
existing at all, not of `physics.trafo_inputs`: TR3b's option-off arm (the
same trafos, pieces owner-only; trafo design "As built (step TR3b)", the H1
table) replayed 984-1176 ticks at t_rep 0.34-0.41 s and t_cap 0.88-0.92 s,
already 1.6-3x this step's no-trafo control, and TR3's H6 found the same
pattern (trafo design "As built (step TR3, fix-up round)", clients at 2-4x
the bay-3 control). So H6 as written (against a batch without trafos) is
failed by the option-off default (d) too, and no implementation of the
option can pass it; only the option-off control of (b) isolates what the
option adds. No per-body optimisation changes factor (1). At the control's cost per tick, the
trafo arm's replayed ticks alone would cost 0.35-0.45 s of t_rep against the
control's 0.13-0.22 s, which is still 2-3x and outside the band. So "within
the control's band + 10 %" against a batch without trafos cannot be met by
RC2-RC5, even at their ceilings. The server's step p99 in the explosions'
window is the server's own simulation of the four sets (T2's cost), which
the clients' optimisations do not touch.

**Stop (the user's choice).** The options and their trade-offs:
- (a) **Judge H6 by its absolute limits and turn the option on.** The limits
  are starved within the band and the server's p99 within the budget, both
  met here.
  - Trade-off: a session with trafo inputs costs every client about twice
    the rewind CPU.
  - Its worst frame is 43-45 ms (three ticks) on the x64 headless client
    when explosions arrive. The x86 retail client has not been measured.
  - A retail client with `session core off` would be refused (it does not
    declare `CAP_TRAFO_INPUTS`), the way SP10's placement default refuses
    it.
- (b) **Re-state H6's control as the same recipe with the option off**
  (TR3b's control: the same trafos, pieces owner-only). This isolates the
  pieces' cost, factor (2).
  - TR3b's option-off control replayed 984-1176 ticks at about 0.35 ms each,
    so this arm would still be about 2x per tick. It fails the + 10 % band
    unless (c) is built too.
  - It needs a control run (the step's two live runs are spent).
- (c) **Build the remaining per-body cuts first** (RC2 (b): the per-body
  read cache, at most 1.3-1.4 us per sleeping body; RC5 (b): skip an
  unchanged sleeping body's non-collision writes, behind a restore profile).
  - Both were left unbuilt by the user's choice or recommendation (B5RX,
    B6RC).
  - They cut only part of factor (2) and none of factor (1).
- (d) **Leave `physics.trafo_inputs` off** (today's default). Pieces stay
  owner-only and the other worlds do not hold them, which is the pre-T3
  behaviour.
  - Trade-off: it does not satisfy H6's literal criterion either (factor
    (1) above: TR3b's option-off arm is 1.6-3x the no-trafo control), so
    choosing (d) is not "keeping H6 met"; (a) and (d) both depart from the
    literal row, and only (b) measures what the option itself costs.

Recommendation: (a), with the x86 frame time of a retail trafo run checked
at the stage close-out. Not built; the default is unchanged.

Checks (quick tier, 02:35-02:41, about 6 minutes):
- The Arch server rebuild (1 min).
- The two live runs (about 1 min each).
- `pieces.py` (under 1 min).
- No build, test, GCC or wire check: no code changed, and docs only.

Deferred to the stage close-out:
- H1 at 0 ms, for H1 (d).
- A second pair, for the run-to-run band.
- The option-off same-recipe control, if (b) is chosen (it was not: moot).
- A retail run with trafo inputs on (x86 frame time).
- CPU parity.

**Resolved (step B9DX, item TIO).** The user answered the stop on
2026-10-28, verbatim: "1. 打开" (turn it on) - option (a), H6 judged by its
absolute limits (starved 0 against the control's 0, the server's step p99
3.11 ms inside the 15.15 ms budget; the numbers above stay the record). The
close-out is passed by that ruling. `physics.trafo_inputs` is on by default
(`BallanceMMOServer/config_manager.hpp:91`, RCX's patch
`.local-tools/work/RCX/rcx_trafo_inputs_default.patch` applied unchanged);
adaptation M2 of the trafo design is closed. A member that cannot run the
trafo inputs is refused by such a session (the trafo design's
`CAP_TRAFO_INPUTS` rule). Checks (B9DX's batch gate): 561 tests, the Arch
server build (ARCH-BUILD-OK), the wire check `rB7EG -> rB9DX` over
`core_ref_s8` ALL SAME (12 send traces SAME, 12 journals IDENTICAL: the
headless captures carry the server's messages, so the server's default
reaches no replayed byte).
**A default reaches only a config that lacks the key.** The server's
loader writes every missing key's default into `config.yml` and saves the
file at every load (`config_manager.cpp:15-19` `yaml_load_value`, :84,
:144 `save(false)`), so every server that has run once since T2 has
`physics: trafo_inputs: false` stored and keeps it after a rebuild from
this commit; only a fresh config gets the new default (SP10's
`placement_clones` behaves the same). The deployed server (okbc.st,
`~/bmmo/config.yml`) needs `trafo_inputs: true` (and
`placement_clones: true`) set by hand at its next deployment. A one-time
config migration would be a design choice and is not made here.

## As built (step BALA)

The airship rule: SFD's stop (B) answered by the user on 2026-10-28 (answer 3,
verbatim: "每个世界应该都有自己独立的飞船副本，且不纳入同步，每个客户端看到的飞船不同。"),
refined the same day to option (A) "Own ball only" and to the ghost: "用转发位置的吧
... 然后进入飞船区域的时候，反物理化，显示半透明球，并同步位置等。" (the own ball as
usual, the others see it semi-transparent at its relayed pose). Base f145a9a,
protocol 13 -> **14**, journal v7 unchanged (the zone ticks are NOTE records),
bridge API 23 -> **24**. No IVP or physics_RT source changed; no engine change.

**The zone and the pair rule are the physics layer's** (`physics_state.cpp`
"bridge API v24: the airship zone", one implementation for the server, the
headless host and the retail plugin): `airship_zone_set(physics, enable,
own_group)` finds the walkable parts (`PE_Balloon_Platform*`,
`PE_Balloon_Platte*`) and installs a collision filter and an
`IVP_Listener_PSI` in the environment. The box is each part's mesh box
(`GetBoundingBox(TRUE)`) through its world matrix, taken at the first PSI
after the platform has its body - the level-end script places the airship
after the level loads (adaptation 1) - rounded outward to the 1/8 m grid,
plus 3.0 m sideways, 4.5 m below (the planks' 3.2 m sag) and 5.0 m above.
Arena: (905.250 -6.375 3.250) .. (941.625 7.250 26.750) in every world, the
server included (taken by tick 67 in swing, 1718/1771 in the drive arms: the
sector union physicalizes it). The rule, for a player ball (group `P#...`):
another player's ball never collides with an airship body (`keep_local_body`);
such a ball whose core is in the box collides with nothing, and at the start
of every PSI (after IVP's revives, before the controllers) it is rechecked
(every pair dropped) and frozen - a ghost, sorted by group so two in one PSI
freeze in one order; the own ball, from its first PSI in the box until its
body is gone, collides only with the static level and its own airship (the
review round's latch, below). **The ghost is positional, not stateful** (adaptation 2: the
design said "unphysicalized"; the ghost keeps its body, frozen and
collision-free): a rollback's restore and replay re-derive it with no state of
their own, a resync's blob carries it, and a navigation wake (the Physics
WakeUp of a key edge) is frozen again before it moves. The server's airship
collides with no ball (no own group) and stays simulated.

**The core** (`client_core_frame.cpp` `airship_frame`, after each live frame):
installs the zone at the anchor with `own_group_`, notes the box once taken,
the own ball's first latched tick (`own_airship_tick_`, cleared by a death
or a rewind that took the latch back) and each mirror's
(`remote_ball::airship_tick`) in the log and the journal; from the own zone
tick no held notice, forced frame or resync amendment touches the own record
or journal (`settle_held_tick`, `take_held_ahead`, `correct_own_tick`,
`take_forced_own`, `amend_for_resync`), no snapshot row is applied to a ball
out of the sync (`apply_snapshot`'s `entity_of`), and a resync keeps it
(below); while latched the own pose goes
out every 2 ticks as `SessionGhostPose` (44 bytes, unreliable;
`message/session_ghost_pose_msg.hpp`), which the server stamps with the
sender and relays to the other members only (`server.cpp`
`handle_session_ghost_pose`) and never applies; a receiver keeps the last 16
poses and shows a ghost (`session_platform::show_ghost`, retail only:
`game_objects::set_ghost` translucent, the CK matrix set) at the pose 4 owner
ticks behind the newest, linear position and normalized-lerp rotation. The
server journals `airship: pN in the airship zone at tick T` per clone
(`session_runner.cpp`). Test knob: headless `--accept-protocol N` replays a
capture of protocol N (the wire check of this step; adaptation 3: the five
protocol-13 arms of `core_ref_s8` were not re-captured).

**Checks (quick tier).** 585 tests pass (new `GhostPoseWire`); GCC 29/29 ok;
x64 BUILD-OK, x86 CLIENT-BUILD-OK, ARCH-BUILD-OK. Wire check `rN02R -> rBALA`
(`NEW_EXTRA=--accept-protocol 13`) over `core_ref_s8`: l8_cross 4/4 sends
SAME but SessionReady's protocol, journals IDENTICAL (its airship never gets a
body in 40 s); swing c0 likewise, c1/c2/c3 SAME but SessionReady and 34/35/34
ghost poses from the own zone tick; resync c2 SAME but SessionReady and 72
poses, c0/c1/c3 differ from tick 2385/2299/2285 (after their zone ticks
2217/2234/2225; the own ball out of the sync dies elsewhere, its respawn gets
no echo from the captured server, and the replay stops COVERAGE-SHORT at
"expected a clock read at resync_limiter"). Every journal's TICK hashes agree
up to the first zone freeze of its world (swing 2930/2923, resync 2200/2217).
Swing against the server (SimTool replays of `server.bmjr`, old and new): the
old server's copy of c2's ball leaves c1's at 3104 (0.03 -> 0.62 m by 3116,
the airship leak) and c1's ball is 5.3-6.3 m off in c3's world; with BALA
every ball but each client's own out-of-sync one is bit-equal to the server
at 3100-3116 on c1 and c3 (3/3); the mechanisms are G-c as before
(P_Modul_34 1.7 mm on c1, P_Modul_03 1.21 m on c3). One live run (4 headless
clients, arena, 60 s, 150 ms, drive + keep-lane, snapshots off, the Arch
server rebuilt; `maps/arena-test/BALA_live/airship`): PASS, server replay
4111/4111, starved/resyncs/far 0; zone entries server/owner 2067/2067,
2157/2157, 2173/2173 with owner and server bit-equal up to the entry, and
2165/2164 for c3; 222-235 relayed poses received by each client from the
three others, none of its own.

**Measured: the zone decided differently between worlds** - c3's own ball in
the live run entered at 2164, the server's copy at 2165; in the swing replays
c2's world ghosted c1's ball at 2922 (the others 2939) and the others c2's at
2930 (c2's own 2938). In each case the copies already differed by 0.07-9 m
before the zone (c3 from tick 234, c2's world from the start: the coincident
spawn pile, step SPGH's), not at the boundary; no case where bit-equal copies
decided differently was found. An owner-stamped event would make the tick one
in every world whatever the copies (the user's choice, not built).

**Review round (the gate's fix-up).** (1) Every own-correction path is gated
at the own zone tick, not only `amend_for_resync`: `settle_held_tick` (no
store, no SUBSTITUTED journal record, no amend, no past-the-reach resync
request), `take_held_ahead`, `correct_own_tick` and `take_forced_own` (a
SessionLead's forced frame is the server's input for a frozen ghost; the own
ball plays this client's keys), counted in `own_corrections_out_of_sync_`.
(2) A resync whose S is at or after the own zone tick keeps the own ball:
no row, no navigation, and the travelling blob excludes it by its exact name
(`savestate_travel::excluded_prefixes` takes an entry `=<name>` as one exact
name, `physics_savestate_restore.cpp map_objects`), so the restore takes it
from the anchor's own record like a keep-local body (rechecked into the
tree) and the re-step drives it from its records (`resyncs_own_kept_`). An S
before the zone tick still takes the server's row (the ball was in the sync
there). (3) The own side is one way for the life: the physics layer keeps a
latch per own life as IVP clock intervals `[entered, gone)` - opened at the
first PSI the own core is in the box (later lives a rollback took back are
forgotten first), closed at the first PSI with no own body - and the pair
rule restricts the own ball while latched, inside the box or not; at the PSI
the latch opened the own ball's pairs are rechecked (`recheck_collision_filter`),
so a contact it had with a mirror or a mechanism is dropped at the PSI the
other worlds freeze its copy, and a replay that reaches that PSI (same clock:
the savestate restores it) rechecks there again; a restore before it reads
the latch closed. The pose goes out while latched (the whole life, so the
others' ghost never stops at a last pose when the owner backs out of the 3 m
margin); `airship_zone_contains` answers 1 for a latched own ball, and the
core clears `own_airship_tick_` when the latch is gone (death, or a rewind
to before the entry the replay did not repeat). Not covered: a resync's
travelling blob sets the server's clock, so a latch whose interval the new
clock does not cover is re-taken at the next PSI in the box (a ball that
backed out is then unrestricted until it re-enters). (4) Documented, not
changed (review finding 5): retail `show_ghost` sets the CK world matrix of a
frozen, physicalized mirror to the relayed pose, so on retail a reader of the
CK matrix (TT proximity scripts, the per-player mechanism units of world
agreement Q7(b)) sees the owner's display pose there while IVP, the headless
client and the server keep the frozen entry pose; per-player units must
ignore ghosts (or read the IVP core) when they are built. (5) Nits: the
server's box line is logged again for a new level; the stale protocol
comment went.

**Gate round checks** (about 9 minutes): x64 BUILD-OK (header deps valid),
x86 CLIENT-BUILD-OK (all 10 includers of `client_core.hpp` rebuilt), GCC
29/29 ok, 585 tests pass, the Arch server rebuilt (ARCH-BUILD-OK, with the
slot-13 hack for the retail run). Wire check `rBALA -> rBALA2`
(`EXTRA=--accept-protocol 13`, both binaries): every journal IDENTICAL (12/12),
l8_cross and swing c0 sends SAME; in swing c1-c3 and resync c0-c3 every send
but the 44-byte ghost poses SAME, and the poses start at the same tick and
continue past the old binary's last one (swing c1 34 -> 114 poses, 2939..3165;
resync c2 72 -> 233): the latch keeps the pose going after the ball leaves the
box. The captures reach no resync with the own ball in the zone (the resync
arms stop COVERAGE-SHORT before it), so (2) is untested by replay. One
two-copy retail run (`scripts/retail/BALA_retail2`, arena in slot 13,
snapshots off, trafo_inputs and placement_clones on; a first launch was
refused at room start because the Arch server had been built without the
slot-13 hack, no session ran): client 1 activated sector 4, beamed onto
`PE_Balloon_Platform` (930, 0, 15) at tick 435, its own ball latched at 436
and rests on its own airship platform at (929.30, 2.02, 14.98) - it
collides with it; the server ghosted its clone at 436; client 2 ghosted the
mirror (logged at 439, its first live frame after the beam's lifecycle
rewind) and received 501 relayed poses, shown on 999 frames; 502 sent by
client 1, none received by it. **The level end did not run** (no "Sending
level finish message"): the ball rests 2.02 m above the platform pivot and
the platform does not sink; the player's own sector stayed 1 (the 'sector 4'
verb activates the sector through the union, not the player's checkpoint
chain), and whether PE_Levelende's proximity is armed only by the last
sector's checkpoint was not established in the budget. The ghost's
translucency was not seen (client 2's camera follows its own ball, 950 m
away).

**Not done / deferred.** The finisher's level end on retail (a run that
reaches the last sector by the checkpoint chain); the translucent ghost on
screen; remote input corrections of a ghost still rewind (harmless: the
replay re-freezes it); a protocol-14 reference set (`core_ref_s9`, five live
arms).

## As built (step SPGH)

(Landed with step SPGH2 below, which replaced the clock-interval record with
per-group flags in the IVP savestate and made a ghost life begin only at a
spawn birth; the rest of this note stands.)

The spawn ghost: the user's rule of 2026-10-28 (verbatim:
"开始的时候，在出生点区域是透明球（和未启用物理碰撞一样），然后远离出生点才物理化"),
settled when asked as "only ball-to-ball collision off": inside its spawn area
a ball rolls, falls and steers as usual, passes through every other player's
ball and is drawn translucent; once it has left the area it collides with
balls again and is drawn solid. Base cf7bcae, protocol 14 and journal v7
unchanged, bridge API 24 -> **25**. No IVP or physics_RT source changed; no
engine change; nothing on the wire.

**The rule is the physics layer's** (`physics_state.cpp` "bridge API v25: the
spawn ghost", one implementation for the server, the headless host and the
retail plugin; `spawn_ghost_set`, `spawn_ghost_state`). Spawn areas: a sphere
of `kSpawnGhostRadius` = 12 m (`session/spawn_impulse.hpp`; it holds all 16
birth cells of the default spacing, the farthest sqrt(5) x 4.2 = 9.39 m out,
with the ball inside) around `CurrentLevel[0][3]` and every row of the
level's `ResetPoints` table (arena: 4 areas, level 8: 5), read when the rule
is switched on. A collision filter refuses a pair of player balls (`P#...`)
when either is a ghost; every other pair is unchanged. A PSI listener, at the
start of every PSI, brings each group's lives up to date and turns a ghost
solid for the rest of its life at the first PSI its core is outside every
area while it overlaps no other player ball (centre distance at least the sum
of the cores' `upper_limit_radius`); a ball that leaves while overlapping
waits (`overlap_waits`) and turns solid at the first PSI it overlaps none, so
no world starts a contact from an overlap. All decisions of a PSI are taken
on the positions as they stand, then the new solid balls whose overlap-tree
sphere meets another player ball's have their pairs rechecked
(`recheck_collision_filter`, in group order) - the pairs a ghost was refused
are made at that PSI in every world; a ball with no other ball near is not
rechecked, so a solo world is unchanged to the bit (no recheck touches its
overlap tree). `kSpawnGhostRespawns` (true): a respawn at a resetpoint is
born a ghost like the first life; false builds the other reading (only a
group's first life a ghost) - the one switch, a constant (every world runs
one build), for the user.

**Record and rollback** (adaptation 1: the step asked for the rule "in the
script / savestate record"; it is kept, like BALA's latch, as IVP clock
intervals, which the savestate already carries): per group a list of lives
`[born, gone)` and the clock each turned solid. A rollback's restore puts the
clock back, so the filter reads the state as it stood at the anchor (a life
covering the clock, solid only if its solid clock is not after it) - a body
revived inside a window (gap L) gets its life's state - and every PSI first
forgets what lies after its clock (a life born later, a later solid clock,
an end the replay outlives), so the replay decides again at the same PSI.
Not covered: a restore that refuses its savestate and keeps the live clock
(`restore_clock` off: the clock is not rewound, the latch reads the live
clock), and a travelling blob of a world whose clock base differs (a late
joiner's resync) - there a ball outside every area and clear of the others
is solid again at the first PSI, a ball inside an area may differ until it
leaves. A world that enables the rule with balls already alive (a late
joiner) starts them all as ghosts.

**Bodies asleep count** (found by the retail run, fixed in the step):
`CKIpionManager::m_MovableObjects` drops a frozen core
(`PhysicsObjectListener::event_object_frozen`, physics_RT
`CKIpionManager.cpp:113-121`), so the first build, which listed the balls from
it, closed a resting ball's life and showed it a ghost again 22 ticks after it
came to rest (retail: the beamed balls, solid at 429 / 616 / 772, ghosts again
at 451 / 638 / 794 in all three worlds) - and a sleeping ball was invisible
to the overlap test. The listener now walks the manager's whole object table
(`m_PhysicsObjects`, sorted by group then name). The same list feeds BALA's
airship listener (own-ball latch closed at "no body", ghosts found): a latched
own ball that falls asleep on its airship closes its latch there - not
reproduced, queued for the airship's owner (BALA), not changed here.

**The core** (`client_core_frame.cpp` `spawn_ghost_frame`, after
`airship_frame`): installs the rule at the anchor after the airship zone (the
server after its airship zone too: one PSI-listener order), logs each ball's
ghost and solid ticks as this world's frames see them (log only, no journal
note: a solo journal stays byte-identical), and asks the host to draw a ghost
translucent on every frame it is one (retail: `show_spawn_ghost` - the own
ball's current mesh materials at alpha 0.5 with alpha blending, their own
state kept in file-scope statics of `core_host.cpp` and restored with alpha 1;
a mirror through `game_objects::set_ghost`; the airship's ghost display wins
on a mirror it shows). Every frame, because the host re-colours a ball under
the core (the level start's fade-in of the own ball; a mirror's unmake /
remake shows it solid): the run's first build drew the change once and both
balls were back to opaque by tick ~407 (screenshots). The headless host
draws nothing. The server logs each clone's transitions.

**The start pile, measured.** Root cause of the residual: the default
spacing does not keep the birth cells a diameter apart - the resetpoint's
axes are not unit length, so the 4.2 cells land 3.958 m apart (resync arm,
c0 and c2's cells; a stone ball is 4 across): two balls are born in contact,
and the contact resolves differently per world. In the old build exactly
that pair's own balls leave the server's rows first, at 232 (0.031 m), the
others at 234 and 252. Pinned replays (own keys from the live journals,
`work/SPGH/pinned_replay13.py`) against SimTool replays of the server
journals with the same binary, own ball vs the server's row, ticks 230-1100:

| arm | rBALA2 (before) | SPGH |
|---|---|---|
| resync_snapoff | own balls off from 232/252/232/234, 1.15 / 0.19 / 1.08 / 0.57 m in 230-330 | c0, c2, c3 bit-equal for the whole window; c1 bit-equal to 674 |
| swing_snapoff | off from 232/702/232/234, 1.13 / - / 1.13 / 0.62 m in 230-330 | all four bit-equal to 676-702 |

The later differences (674-702) are the replay's, not the rule's: the pinned
keys were steered in the old world, so in the new one c1's ball falls (death
script at 674, unphysicalized at 740; the live session's c1 died at 2470)
while the SimTool server replays the journal's old events. All worlds decide
the transitions at the same PSI: resync 329 / 333 / 341 / 341 and swing 328
/ 331 / 380 / 380 in every client world (a rewind took 330 back to 341 in the
three where it was decided first). Live (one run, 4 headless clients,
arena, 60 s, 150 ms, drive + keep-lane, snapshots off, the Arch server
rebuilt; `maps/arena-test/SPGH_live/lane`, `SPGH_live_lane`): PASS, server
replay 4119/4119, starved / resyncs / unmatched 0. Own ball vs the server's
rows against BALA's run of the same arm (`BALA_live/airship`): the first
difference moved from 232 / 252 / 232 / 234 (0.20-1.15 m in the start
window) to 1168 / 2088 / 676 / 712 (none in the start window); the
respawn window 2600-3000 (respawns at 2522-2608) is bit-equal on all four
in this one smoke run (not evidence by itself: no pinned replay covers the
respawns yet)
(before: 0.10 / 0.99 / 4.76 / 0.25 m). The 2000-2400 and 3000+ windows are
the airship (the own ball out of the sync, BALA), the 600-1600 ones
(0.7-0.95 m on c2/c3, present before) are not at a spawn. The live journals
replay IDENTICAL with the asleep fix (pinned, all four), and the live server
journal 4119/4119.

**Spawn spacing and the spawn impulse** (removing either is the user's
choice, not built): with the rule, balls inside a spawn area never touch, so
neither is needed for determinism any more - coincident balls stay ghosts
until they have separated and left the area. The spacing still decides
where the balls first meet once solid; the impulse still pushes them apart
so they leave the overlap sooner (without it two balls born on one point
holding the same keys stay overlapping ghosts). Not measured with either
removed (it needs a live run with another server config; deferred).

**Checks (quick tier).** 585 tests pass; GCC 29/29 ok (three times); x64
BUILD-OK, x86 CLIENT-BUILD-OK (header deps valid on both trees), ARCH-BUILD-OK
(clean, then with the slot-13 hack for the retail run, then clean with the
fix). Solo arms of `core_ref_s8` (`--accept-protocol 13`): sends SAME and
journals IDENTICAL against rBALA2 (solo1 2700, solo2 2427 sends), with the
first and the fixed build. Wire check `- rSPGH2 rSPGH2w rBALA2w_new`
(`EXTRA=--accept-protocol 13`): l8_cross c1-c3 sends SAME, c0's first
difference at 407 (an event's order); every journal's first TICK difference is
the first tick all balls exist (l8 213, swing / resync 252: the old build's
born-in-contact pair); the resync / swing sends differ from 253-799 (the
replay's steer reacts to the new trajectories) and the swing replays stop
COVERAGE-SHORT at 1160-1415, resync c0/c1/c3 near 2950 (a clock read the old
capture never made); these unpinned replays of a pre-rule capture cannot
stay equal, so the pinned replays above are the step's evidence. Retail, two
copies (`scripts/retail/SPGH_retail`, arena in slot 13, snapshots off,
trafo inputs and placement on; the first build): both balls born ghosts in
both worlds (own 212, mirror 214 / 215); client 1 beamed onto client 2's
ball at 238 stayed where it was put (no push); beamed 25 m down the lane it
turned solid at 429 in all three worlds (client 2's mirror at its birth
record's rewind); client 2 beamed 1 m from it and 6 m past it. The SimTool
replay of that session's server journal with the fixed build: client 1's
clone solid at 429 for good, client 2's stays a ghost while it overlaps
client 1's sleeping ball (616) and turns solid at 772 when beamed clear -
the overlap wait the first build missed (982/982 ticks matched, both builds).

**Not done / deferred.** A retail run with the fixed build (the step's two
live runs were spent: the translucent display re-asserted every frame and
the asleep fix are verified by build and by the server replay only, not on
screen); a run with spacing 0 or no impulse; a protocol-14 reference set;
a pinned replay over the respawn window; CPU cost of the listener (it walks
and sorts the whole object table at every PSI, replays and the server
included; CPU parity is the close-out's).

**Review findings (open, the user's decision first).** (1) The ghost state
does not travel with the one state transfer: `spawn_ghost_set(true)` at a
late joiner's anchor (`client_core.cpp` anchor) clears every group's lives,
so its first PSI opens a ghost life for every ball alive in the server's
world - a ball solid on the server but inside a resetpoint's 12 m sphere
stays a ghost in the joiner's world until it leaves it, and two solid
balls touching outside every area wait as overlapping ghosts there while
they keep colliding on the server; a running client's resync keeps the
lives its own world decided (the blob moves the bodies, not the lives).
Exact needs the lives in the transfer (the portable script record or the
savestate blob: a record/format change on the resync wire, so the next
protocol and journal numbers). (2) A trafo opens a new life: the group has
no `P#` body while the pieces fly, so the new ball is born a ghost and,
within 12 m of a resetpoint, passes through other balls until it leaves
the sphere (outside every area and clear of the others it is solid at its
first PSI, as before). A trafo is not a birth; telling it from a respawn
needs the lifecycle (an input) in the physics layer, or the lives in the
record of (1). (3) Retail `show_own_spawn_ghost` sets alpha on the own
ball mesh's materials and restores alpha 1: a material the level shares
with other objects would draw translucent with it (not checked).

## As built (step SPGH2)

Step SPGH's spawn ghost, landed with its two review findings fixed: the
ghost state travels with the world (the orchestrator's option (A'), under the
user's "run on, log problems and continue" of 2026-10-28) and a trafo is not
a birth. Base c9ca444 with SPGH's saved work applied (stash@{0}; only
`docs/roadmap.md` needed a hand merge). Protocol 14 -> **15**, savestate
format 2 -> **3**, bridge API 25 (SPGH's, one function added), journal v7
unchanged. No IVP or physics_RT source changed; no engine change.

**The state is a few flags per group, in the IVP savestate.** SPGH kept the
lives as IVP clock intervals beside the world; a rollback read them through
the rewound clock, but the one state transfer carried none of them (a
joiner's `spawn_ghost_set` cleared them and re-opened a ghost life for every
live ball; a resync kept the client's own decisions). Now
(`physics_state.cpp` `spawn_ghost_state_t::group_state`) each `P#` group
holds `seen` (its first life began), `present` (a body in collision
detection at the last PSI), `solid` and `marked` (a spawn birth since the
last PSI), read and written as `spawn_ghost_record`s
(`spawn_ghost_record_get` / `_set`). The savestate carries them after its
tables (`physics_savestate_internal.hpp` `world_record::spawn_ghost`,
format 3; `physics_savestate_capture.cpp` next to the random state;
`physics_savestate_restore.cpp` restores them in the clock step, before
anything rechecks a body; `describe_savestate` / `diff_savestate` show them
as `spawn_ghost` / `ghost <group>` records). So every rollback restore and
the travelling blob of a late join or an A3 resync put back the state the
world it belongs to had: the server's groups are every world's names
(`P#<join order>`). A blob without the section (the rule off where it was
captured, or format 2) leaves a rule-on world with no group seen; format 2
is still read (an old journal's RESYNC blob). The clock bookkeeping is gone
(no "forget the future" pass). Not covered, as for every other piece of
state: a rollback whose savestate restore was refused keeps the live flags.

**A trafo is not a birth** (adaptation: the step asked for "a life ends only
at a death / leave, from the lifecycle the physics layer is given"; built as
the equivalent "a ghost life begins only at a spawn birth"). A group's first
body begins its first life as a ghost; after that only
`spawn_ghost_birth(group)` (bridge `spawn_ghost_birth`) makes it a ghost
again: a Physicalize the lifecycle flags `PHYSICALIZE_FLAG_SPAWN` (the
retail script physicalized the ball at the level's resetpoint), in every
world at its applied tick, before that tick's step - the server's
`apply_event` (`physics_world.cpp`), a mirror's birth
(`client_core_lifecycle.cpp` `physicalize_remote`), an own remake
(`remake_own_body`), the live own birth (`session_client_core::own_birth_seen`
with the pose the host saw before the block made the body: retail through
`own_physicalized`, headless from its Physicalize hook `before_physicalize`,
or `capture_own_physicalize` where no block was hooked; the core decides with
`own_birth_is_spawn`, the predicate the event's SPAWN flag is set by) and, in a replay, the own
spawn birth of the tick (`drain_pending_lives`: the live capture does not
run in a replay, and retail parks its own window birth instead of remaking
it). A death keeps the group's state; while a group has no body at a PSI it
reads as a ghost, so the body that comes next is inserted with no ball
pairs, and at the next PSI a solid group's new body (a trafo's new ball) is
solid and has its pairs rechecked (`resumed`), a spawn's stays a ghost.
With `respawns` false a mark after the first life is ignored. A death and a
respawn between two PSIs (the group never bodiless) recheck the new body
there, which drops the pairs it was given while it read solid.

**A leave ends a life** (review finding 3). The server's `remove_player`
drops the leaver's group (`spawn_ghost_forget`, after its ball is gone), so
the next member that takes the join order (`assign_join_order` reuses the
lowest free one) begins its first life as a ghost however its first body is
made - also a first Physicalize that is not SPAWN-flagged, which before
took the `resumed` path and was solid. A late joiner and a resynced member
take this through the server's blob; the members are not told of a leave
(or of the joiner's join order, below) and follow with M0/M1.

**Parked bodies are left out.** A rollback restore keeps a body born after
its anchor parked outside collision detection until its birth tick (no
overlap-tree node); SPGH's listener counted it from the restore on (a life
opened early, an overlap partner that is not in the world yet). The listener
now skips a ball whose overlap element has no tree node.

**Protocol 15** (`entity/session.hpp`): no message layout changed; every
world runs the rule, a SPAWN Physicalize begins a ghost life everywhere, and
a SessionResyncState's blob is format 3, so a protocol-14 peer is refused at
the handshake rather than at its first resync. The journal format is
unchanged (the RESYNC record's blob is opaque; the reader takes 2 and 3).

**Checks (quick tier; about 8 minutes of check wall time, two live runs).** x64 BUILD-OK,
x86 CLIENT-BUILD-OK (with the slot-13 hack, reverted), Arch ARCH-BUILD-OK
twice (hacked, then clean for the live run); header deps valid on both
Windows trees; 586 tests pass; GCC syntax check every line ok.
- Wire check `rSPGH rSPGH2 rSPGH2w` on `core_ref_s8` (`EXTRA=--accept-protocol
  13`; `build-agent-rSPGH` is HEAD c9ca444's x64, the rule off): every
  client's send #5 differs (SessionReady's protocol 15); l8_cross c1-c3
  otherwise SAME, c0 from 407 (an event's order, as SPGH's); resync / swing
  from 253 / 627-647 (c1 1643) and every journal from 252 (the first tick
  all balls exist: the rule) - SPGH's signature against HEAD; the resync
  c0/c1/c3 replays stop COVERAGE-SHORT near 2950 with BOTH binaries (a clock
  read the capture never made: pre-existing).
- Solo (`core_ref_s8` solo1/solo2, HEAD vs SPGH2): journals IDENTICAL, sends
  SAME but #5.
- SPGH's live rule-on session (`work/SPGH/liverun`, recorded by SPGH's build,
  protocol 14) replayed with the own keys pinned by this build: all four
  journals IDENTICAL to the live ones (19468-19639 records; the start, the
  respawns at 2522-2608 and the window 2600-3000 included), and the SimTool
  replay of its server journal 4119/4119 ticks matched. The new state store
  and the trafo rule decide every transition of that session as SPGH did.
- The start pile (pinned replays of `core_ref_s8` resync / swing against
  SimTool replays of their server journals, this build, 230-1100): resync
  c0, c2, c3 own balls bit-equal throughout, c1 to 674; swing all four to
  676-702 (the replay's keys, as SPGH); before (rBALA2) 232 / 252 / 232 / 234,
  0.19-1.15 m in 230-330.
- The blob round trip with the groups in it: SimTool `--savestate-check
  --spawn-test 4` on the arena, 30-40 rounds each at impulse 3, 10 and 20 m/s:
  PASS (every restored-and-replayed tick's blob, ghost section included,
  byte-equal to the straight pass; at 20 m/s the four ghosts turn solid at
  tick 55 inside the window). Level 1's scene is vacuous (no contact).
- One live run (`maps/arena-test/SPGH2_live/latejoin`, 4 headless clients,
  arena, 60 s, 150 ms, snapshots off, trafo inputs and placement on, client 3
  joining at the host's tick 1200): server replay 4038/4038; starved 0. The
  joiner's RESYNC blob (S 1355) holds `P#0..P#2 seen=1 present=1 solid=1`;
  the joiner showed the three mirrors as ghosts before the apply (its anchor
  world) and solid after it, as the server had them since 334 / 382; its own
  ball ghost 1560, solid 1699 = the server's clone. (A first attempt never
  started: the server could not bind its port.) VERDICT FAIL on the
  harness's late-join criterion (client 3 relayed 102 frames past the
  rollback reach, lag max 47) - not a spawn ghost matter.

**The gate round** (the review's findings 1-3 fixed, 4 deferred, the nits
answered). (1) The `resumed` and `births` counters are exported
(`spawn_ghost_set`'s `info_out[7]`, bridge v25 as SPGH2 found it, both
hosts), printed in the client's session-end line, and the server logs them
whenever they move (`world: spawn ghost at tick T: ...`). (2) One predicate,
one point in time: the hosts no longer decide an own spawn birth
(`own_spawn_birth` is gone); the core's `own_birth_seen` applies
`own_birth_is_spawn`, and both hosts call it before the block makes the body
(retail in `own_physicalized`, headless in its Physicalize hook - before, it
marked after the block). The headless SPAWN flag is still decided at the
report (`captured` needs the capture of the same tick and entity), exactly
as the spawn kick has been since OWNB, so a birth the report does not
capture differs in the flag, the kick and the mark alike (pre-existing).
(3) A leave ends a life (above). (5) The name order in the listener never
picks between two bodies of a group (one ball per group in collision
detection; parked bodies left out), said in the code. (6) The shared ball
materials: for the deferred retail run.
Gate checks (about 4 minutes): x64 BUILD-OK, x86 CLIENT-BUILD-OK (no hack),
deps VALID on both trees, 586 tests pass, GCC 29/29 ok. Every journal of
the implement round is reproduced byte for byte by the gate build: the 12
multi-client `core_ref_s8` replays (sends SAME, journals IDENTICAL to
`rSPGH2w_new`; resync c0/c1/c3 COVERAGE-SHORT at 2954-2960 as before), solo1
/ solo2 (IDENTICAL to HEAD's and the implement round's), SPGH's live session
with the own keys pinned (all four IDENTICAL to the live journals), its server
journal 4119/4119. The trafo evidence: `maps/arena-test/b6rc/h1_150`
(protocol 13, four balls trafo Stone -> Wood at 818-862 and -> Paper at
1176-1224, one more at 1871), replayed with the rule on: the server
(SimTool) has every new ball resumed solid at its physicalize tick (`9
resumed solid`, no ghost line after 231), every client's mirror transitions
equal the server's (solid 328 / 331 / 380 / 380), and no trafo opens a
ghost life.

**Residual found at the gate (root-caused, queued: item OWG).** In every
client the own ball reads as a ghost for one frame at each of its trafos
(c0 818 -> solid 819, c1 824 -> 825, c2 841 -> 842, c3 866 -> 867; the
server's clone is solid at 818 / 824 / 841 / 862). Cause: both hosts give
the own ball its `P#` group in post_step step 17 (`own_body_upkeep`:
headless `session_client.cpp`, retail `core_host.cpp`; unchanged since M3),
after its birth tick's PSIs, while the server's clone has it from its
Physicalize. During its birth tick the live own body is no player ball
(`airship_is_player` reads the group): its pairs with player balls are
made at insertion (the server's clone: none as a ghost, or made by the
PSI's recheck when it resumes), and the group change does not re-ask the
filter. At a trafo both end solid with the same pairs (cosmetic: one
translucent frame on retail). At a spawn birth next to another player's
ball the live own ball would keep that pair through its ghost life; a
rollback over the birth tick remakes (headless) or re-inserts (retail
unpark) the body with its group and repairs it, so it is left only where
no rollback reaches back over the birth. Fix (item OWG): give the own body
its group before its birth tick's PSIs on both hosts (headless in
`capture_own_physicalize`, retail in the PreSimulate pass the spawn kick
uses) and re-ask the filter for it; checks: a pinned replay and the
retail run. It changes every live own birth, so it needs the live and
retail runs this gate had no budget left for.

**Residual found (pre-existing, queued: world agreement §1.2, the
member-added message, roadmap item 5 M0/M1).** The existing members do not
know the joiner's join order: they kick its mirror with index 8 where the
server and the joiner use 12 (63 vs 3 in the index), and its mirror's group
is `P#63`. The mirror drifts from the server's clone (17.8 m in clients 0-2)
and turned solid there at 1659 against the server's 1699 (it left the area
on another path). For the spawn ghost the group name matters too: a resync
of such a member brings the server's `P#3` state while its mirror is `P#63`
(read as never seen: a ghost until it leaves the area clear of the others).
Fixed by M0/M1's member-added message; nothing here. M0/M1 must also tell
the members of a leave, so that they drop the leaver's group as the
server does (`spawn_ghost_forget`) before a joiner takes its join order.

**Not done / deferred** (each beyond the step's 15-minute gate and two live
runs, for FULL): the retail two-copy run (ghosts pass through each other,
translucency every frame, a resting ball solid, on screen; the retail own
spawn re-marked inside a rollback window); a trafo near a resetpoint (no
check reaches it: no arm trafos inside a spawn area; the code path is the
`resumed` branch); an A3 resync of a running member across a ghost -> solid
transition and a touching solid pair near a resetpoint at a transfer; the
runs with spacing 0 and without the spawn impulse (measure only; removing
either is the user's choice); a protocol-15 reference set (the multi-client
`core_ref_s8` arms no longer replay comparably: the rule changes their
world from tick 252).

## As built (step OWG)

Queue item OWG (found at SPGH2's gate, "Residual found at the gate" above):
the live own ball is given its `P#` group before its birth tick's PSIs on
both hosts, its pairs re-asked of the filter; bridge API v26, no protocol or
journal change, no engine change (IVP untouched).

**What the reproduction found.** The retail script makes the own ball in
the collision group `Ball` (logged at every own birth of every check below:
`'Ball' -> player group`). The player filter refuses every pair between a
player ball and a `Ball` body (`player_collision_filter`), so during its
birth tick the live own body had *no* pair with any player ball - not the
pair SPGH2 supposed. Measured: the body's player pairs at the adoption, i.e.
what the old code carried through the birth tick, were 0 at all 28 own
births the checks reach (SPGH's live session pinned: 4 starts and 7
respawns; solo1/solo2: 2 starts; `b6rc/h1_150`: 4 starts and 7 trafos;
retail: 2 starts, 1 respawn, 1 trafo). What the late group did change: the
group's state in the spawn ghost rule. At the birth tick's PSI the own
group had no body (`present` false, `marked` cleared there, where the
server's clone counts), so the own ball read as a ghost for that frame
after a trafo and was `resumed` (its pairs rechecked when another ball is
close by) one PSI later than the server's clone; and a death and respawn
between two PSIs (`present && marked`, the recheck of the new body's pairs)
was decided differently. The spawn worry of SPGH2 (a pair kept through the
ghost life) does not arise; the trafo frame is the residual, and a solid
ball next to the trafo would have gone one PSI without its pairs.

**What was built.** One implementation in the physics layer
(`physics_state.cpp` `adopt_own_group` / `queue_own_group`): set the group,
and when it changed and the body has an overlap-tree element, call
`recheck_collision_filter` (IVP re-asks the filter only when the overlap
tree re-inserts a body); the result (old group, player pairs before / after,
the spawn ghost state after) is logged. The headless host calls
`adopt_own_group` in `capture_own_physicalize` (the PreSimulate pass of the
live birth tick, the block has made the body, before the PSIs and before
the spawn impulse). The retail host calls `queue_own_group` from its
OnPhysicalize (bridge API v26 `own_group_queue`): the body does not exist
yet, so an `own_group_callback` is queued into the same frame's PreSimulate
pass, ahead of the spawn impulse's `impulse_callback`, anchored to the Ball
Navigation behavior as that one is; it is one of BallanceMMO's own
callbacks (`is_bmmo_callback`), so the savestate and the gap Q queue check
leave it out as they leave the impulse out. The post_step step 17 upkeep
stays as the fallback (the same group: no recheck). Remade own bodies were
already made in the group (`make_own`, `spec.group`), so a replay is
unchanged. **Adaptation:** the design named the retail point "the spawn
kick's PreSimulate pass"; the kick is queued only at a spawn, so the group
has its own callback, queued at every own birth (a trafo included) where
the kick is queued.

**Checks (quick tier; about 7 minutes of check wall time, one live run).**
x64 BUILD-OK, x86 CLIENT-BUILD-OK (with the slot-13 hack, reverted), Arch
ARCH-BUILD-OK (hacked for the retail run, then clean); header deps VALID on
both Windows trees; 586 tests pass; GCC syntax check every line ok.
- Wire check `rSPGH2 rOWG rOWGw` on `core_ref_s8` (`EXTRA=--accept-protocol
  13`): every send SAME and every journal IDENTICAL on all 12 multi-client
  replays; the verdict line says DIFFERENCES only for the pre-existing
  COVERAGE-SHORT of resync_snapoff c0/c1/c3 at 2954-2960 (both binaries, as
  SPGH2 recorded). Solo1 / solo2: sends SAME, journals IDENTICAL to rSPGH2's.
- SPGH's live rule-on session, own keys pinned, replayed by this build: all
  four journals IDENTICAL to the live ones and to rSPGH2's pinned replays
  (the starts at 231, the respawns at 2522-2608 and 3479-3535 included).
- The trafo evidence `b6rc/h1_150`, replayed by this build: the own ball's
  one-frame ghost at each trafo is gone in all four clients (before, rSPGH2:
  c0 818/1176, c1 824/1190, c2 841/1203, c3 866; now no ghost line after
  the start); the four journals IDENTICAL to rSPGH2's (c3's replay stops at
  pass 7951 with both binaries: a clock read the capture never made,
  pre-existing).
- One retail run (`scripts/retail/OWG_retail`, two copies, the arena,
  snapshots off, trafo inputs and placement on): client 1's trafo at 1134
  shows no ghost frame (`resumed` at 1134, as the server's clone: "spawn
  ghost at tick 1134: ... 1 resumed solid"); the own transitions equal the
  server's (solid 436 / 778, client 2's respawn a ghost at 1239 on both);
  the physics stdout logs the four retail adoptions (`'Ball' -> player
  group`, player pairs 0 -> 0); the own spawn is re-marked in a rollback
  window ("P#0 marked a spawn birth at tick 212 (replayed own birth)").
  SPGH2's deferred retail checks, by screenshot: the balls overlap at the
  spawn and pass through (client 1 beamed onto client 2: neither pushed);
  client 2 beamed 1 m onto client 1's resting solid ball stays a ghost
  (client 1's ball not moved, 2.562 m before and after) and turns solid
  once 7 m past it (778); client 1's own ball shows opaque while the ghost
  mirror overlaps it (the shared-material nit not seen). The translucency
  is judged by eye on the screenshots, not measured frame by frame.

**Unverified: the branch where the recheck adds pairs.** Every adoption
the checks reached logged `player pairs 0 -> 0` and every journal equals
rSPGH2's, so the only branch through which OWG changes a simulation result
- `recheck_collision_filter` creating a pair with a solid player ball in
contact range of the own body at its birth tick (`pairs_after > 0`) - has
not run on either host. There the client's pair is made by IVP's recheck in
the PreSimulate pass while the server's clone got its pair at insertion; if
the two land in a different `collision_fvector` / mindist order, that is
known cause (A) (the own ball's collision bookkeeping) starting at the birth
tick. Nothing measured says it is wrong; it is simply not measured.

**Gate fix-ups (review nits).** `queue_own_group`'s immediate path (the
body already exists at OnPhysicalize) now logs its adoption to stdout as the
queued callback does (`[bmmo] own group (set at once): ...`), so every
retail adoption leaves a trace; a missing anchor behavior is reported as
such ("no live behavior to anchor the PreSimulate pass ...") and a missing
PreSimulate queue separately, instead of "'X' is not physicalized". Both are
log text only; neither is reachable from the headless client (it calls
`adopt_own_group`).

**Not done / deferred** (beyond the step's gate, for FULL): a trafo beside
another solid ball - the FULL cell: a deterministic headless arm with a
remote ball resting within about 1 m of a trafo point, the own ball compared
with the server's clone for 60 ticks after the trafo, and a logged
`player pairs 0 -> N` with N > 0 required (no arm or run has one yet; it is
the one case where the earlier recheck changes physics); a death and a
respawn between two PSIs; and a frame-by-frame translucency record on
retail.

## As built (step ASL)

Queue item ASL (found by reading the code at step SPGH; roadmap §4 item 3):
BALA's `airship_zone_listener` found the own ball in
`CKIpionManager::m_MovableObjects`, which physics_RT empties of a frozen
core (`PhysicsObjectListener::event_object_frozen`, `CKIpionManager.cpp:113-121`),
so a latched own ball that fell asleep read as bodiless and its latch closed
as at a death. No protocol, journal, savestate or bridge API change; no
engine change (IVP and physics_RT untouched).

**Reproduced** (the step's offline arm, `.local-tools/work/ASL/asl_rest.py`:
`BALA_live/airship` client 0's capture replayed with its own keys pinned to
the live journal until tick 2290 / 2305, then DOWN for 80 ticks and no key,
`--accept-protocol 14`). The own ball is latched at tick 2238 (clock
67.000002), brakes, rolls on past the box's +x face (941.625) and comes to
rest at x = 948.01 / 956.13, outside the box but inside its latch. With the
old binary (rOWG) the core logs at tick 2871 / 2922 "the own ball Ball_Stone
is not latched in the airship zone ... any more (a rewind to before its
entry at 2238): back in the sync" - no rewind happened, the ball fell asleep
- and from there the 44-byte `SessionGhostPose` sends stop (317 / 342 poses,
the last at 2870 / 2920), the own-correction gates of BALA's review round
open again and the pair rule no longer restricts the ball (outside the box,
unlatched). Inside the box the same close happens but the pair rule still
restricts by position; the ball is re-latched (and rechecked) at its wake.

**What was built** (`physics_state.cpp`): one helper for both PSI listeners,
`player_balls_in_detection(physics)` - every player ball (group `P#...`,
not `physical_unmoveable`) in the manager's whole object table
`m_PhysicsObjects` whose overlap-tree element has a node (a body a
rollback's restore parks until its birth tick is left out), sorted by group,
then name - and `player_ball_group` (SPGH2's `spawn_ghost_group`, renamed).
The spawn ghost listener's own scan (SPGH2) is now that helper, unchanged
in behaviour. The airship listener takes the own ball and the ghost
candidates from it; the ghost candidates still need a simulated core, and
come in (group, name) order, so the old sort by group is gone. The first
PSI of a latched life at which the own core is asleep is logged to stdout
once (`[bmmo] airship: the own ball ... is asleep in its latch at clock T
(latched at T0; not in m_MovableObjects): the latch holds`); no state or
decision reads it. **Adaptation:** the old list also offered a simulated
player ball with no overlap-tree node as a ghost candidate; the helper
leaves such a body out (it collides with nothing, and the rule that parks
it is the restore's), as the spawn ghost listener already did.

**Checks (quick tier; about 10 minutes of check wall time, one live
run).** x64 BUILD-OK, x86 CLIENT-BUILD-OK (no slot-13 hack: no retail run),
ARCH-BUILD-OK `ballanced-36c04e3d24c9+bmmo-cd66ed66b702-dirty`; 586 tests
pass; GCC syntax check 29/29 ok (it does not compile `physics_state.cpp`).
- The reproduction above with the new binary (rASL): no "back in the sync",
  the latch holds, poses continue to the capture's end (624, the last at
  3484), the asleep line at clock 86.166669 / 87.712124 ("not in
  m_MovableObjects"). Old vs new: journals IDENTICAL, every send but the
  44-byte poses and the tick-0 build-id hello the same.
- Wire check `rOWG rASL rASLw` over `core_ref_s8` (`EXTRA=--accept-protocol
  13`): every send SAME and every journal IDENTICAL on all 12 replays; the
  verdict line says DIFFERENCES only for the pre-existing COVERAGE-SHORT of
  resync_snapoff c0/c1/c3 at 2954-2960 on both binaries (as SPGH2 and OWG
  recorded). No asleep line in any of them. solo1 / solo2: sends SAME,
  journals IDENTICAL to rOWG's.
- The airship arm `BALA_live/airship` pinned (`SPGH2/pinned_replay_acc.py`,
  ACCEPT=14), rOWG vs rASL: all four journals IDENTICAL and sends SAME; both
  binaries stop DIVERGED at the same records (c0 2989, c1 1251, c2 2825, c3
  2835 steps): the capture's server is pre-SPGH2 (protocol 14) and SPGH2's
  spawn ghost changes the world from the first spawn (pre-existing).
- Server worlds, SimTool `--replay-session`, rOWG vs rASL: SPGH_live/lane
  4119/4119 on both; BALA_live/airship, swing_snapoff, resync_snapoff
  diverge at 232 on both (pre-SPGH2 journals, as above); all four written
  journals IDENTICAL old vs new.
- One live run (4 headless clients, arena, 60 s, 150 ms, drive + keep-lane,
  snapshots off, the Arch server rebuilt; `maps/arena-test/ASL_live/airship`):
  PASS, server replay 4049/4049, starved/resyncs/far 0, two own zone entries
  per client (2146/3156, 2086/3052, 2154/3109, 2162/3104), 8 server zone
  lines; no ball came to rest in its latch (a smoke test only).

**Not done / deferred** (for FULL): a retail run of a latched own ball at
rest (the shared code is the retail plugin's too; the x86 build compiles it,
no retail session ran), and an own ball asleep inside the box on the
airship's movable planks (the reproduction's ball rests on the static floor
past the box; the planks' sway may keep it awake), and CPU parity old vs new
on L8/L11 (cpu_parity.sh rOWG rASL): the airship listener now walks the
manager's whole object table (static bodies too) instead of the movable list,
so with the spawn ghost listener the table is walked twice per PSI, replayed
PSIs included; small, but this host runs near the CPU edge (7d11651). If it
shows, build the ball list once per PSI and share it between the listeners.
