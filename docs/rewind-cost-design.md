# Rewind cost: the hold-last re-prediction, and the cost of each remaining rewind

Status: design, 2026-10-25. Step RC1 (the hold-last re-prediction) built and
committed with hold-last by the user's ruling of 2026-10-27 (§11; "As built
(step RC1)" at the end: its drift check, a FAIL under the step's rule at the
build, was settled by the pinned method of Measurement RC1d and the user's own
play, Measurement RC1e). RC6, the tail prediction refinement (R6 / R7), is not
adopted (§8). RC2 is not built (the user's choice), RC3 is built (B5RX),
RC4 is built (B6RC); RC5 and the close-out
are not built. This document is roadmap §4 item 10 (the rewind-cost
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
| RC5 | incremental restore | the savestate checks, the wire check |
| RC6 | **not adopted at RC1** (the user's ruling of 2026-10-27, quoted in §11); R6 and R7 measured (RC1d, RC1e) and kept as a possible later refinement if human rapid-trigger play shows periods. (History: proposed by Measurement RC1d; ruled 2026-10-27 "R6 直接并入 RC1", then the R7 variant measured, Measurement RC1e: R7 not selected, the choice between R6, R7 and hold-last went back to the user, who chose hold-last.) What the refinement would be, as specified when it was proposed: the tail prediction refinement: hold-last replaced past a remote's newest relayed frame by **R6** (per-key pattern-repeat: each key bit's own period repeated, the other keys of the newest frame by R4's history-adaptive hold, k = 5; §9.2; R5, the whole-frame pattern-repeat, is the measured fallback choice), one function in IR next to `relay_frame_or_newest` (IR:353-367) called from both sites (`recorded_input_at`'s tail branch, CF:494; `live_drive`, CL:467-470); its history window fixed relative to the newest frame (the 128 ticks before it - not the ring's cap `kRelayRing`, IR:47, so a later change of the cap cannot change a prediction), no allocation on the path (the measured build's per-key `std::vector` goes), and the periods found once per arrival, not per tail tick | unit tests of the function (a 1:1 and a 1:3 lateral alternation, an UP/DOWN 1:1 and 2:1 alternation, a 1-tick DOWN every 10 ticks repeated, a direct LEFT -> RIGHT switch, a hold with and without complete holds, a 6-tick brake, flags as hold-last, holes, the start and a birth unchanged) and a session test pinning the two sites on the tail; the wire check against `core_ref_rc1` (every first difference attributed to a tail tick the rule fed differently); solo IDENTICAL; **the paired drift check with the own keys pinned** (`.local-tools/work/RC1d/gaps/pinned_replay.py`, every rule's replay of the same captures; R1 replays byte-identical on every cell) on the RC1d cells with these **required cells at both lags (resync_at150 and resync_at150_down)**: the native steer d0; the 1-tick lateral taps t1x1 (rapid-trigger 1:1 alternation) and t1x3; the brake cells d0_brake and t1x1_brake (a 6-tick DOWN in every 90 UP ticks) and d0_ud1x9 (a 1-tick DOWN in every 10: the 1-tick brake tap); the UP/DOWN fast switches d0_ud1x2 (UP UP DOWN) and d0_ud1x1 (UP/DOWN 1:1); the holds d5 / d10 / d15 and d10_brake (direct LEFT -> RIGHT switches: 405 in d10, Measurement RC1d). Criteria: (1) pooled mean no worse than hold-last's on every required cell and no client more than 5 % worse; (2) on the hold and brake cells (d0, d5, d10, d15, d0_brake, d10_brake) input rewinds per real change no more than hold-last's + 0.1, on the tap and UP/DOWN cells no more input rewinds than hold-last; (3) replayed ticks reported per required cell next to hold-last's. **As measured, R6 (and R5) fail (2) on resync_at150_down d0: 0.96 against 0.80 per change, replayed ticks +21 % (42893 -> 51716), all of it on the mirrors of the lowered player (Measurement RC1d (e)); every other required cell passes (largest excess +0.06).** RC6 is therefore not passable as measured unless the user accepts that cell or RC6 removes the excess. [Checked after the fix-up round, 2026-10-27, `gaps/record_model.py`: a physics-free model of the engine's records (a tick keeps the prediction made when it was last simulated, live or in the last rewind) reproduces the cell's rewinds on the lowered player's mirror (client 1: R1 232, R4 / R5 / R6 299; measured 231 / 296). The excess is all lateral: corrections where the rule had released a lateral key the player still held go 187 -> 266, the opposite kind 157 -> 136, UP / DOWN corrections 20 in every rule. So the excess is R4's fallback releasing a lateral key early at max(5, H - r) on the native steer's irregular long holds, and holding the forward / backward keys (named here before this check) would not remove it; a fix has to change the lateral release, not measured.] The live A/B (snapshots off) on the required cells, t1x1, d0_ud1x2 and d0_ud1x9 at least three sessions per rule at each lag (one session is not evidence of starvation either way); CPU parity, against the replayed ticks of (3) | quick tier |
| close-out | H4 and H6 of the trafo design §7.3, **with snapshots on** (decision (b) of 2026-10-25), against T3's snapshots-off CPU numbers as the baseline | full tier |

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
(no retail file changed).
