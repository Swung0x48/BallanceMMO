# Rollbacks a player cannot feel, on an average player's machine (research, FULL step SRT)

Status: **research only, nothing built** (2026-10-31, base HEAD 4d8d60d4, protocol 24). The diagnostic builds of
this step are kept as a patch (`.local-tools/work/SRT/srt_diag.patch`, with `srt_diag.hpp`, `srt_retail.hpp`,
`srt_counters.hpp` and the `patch_*.py` that applied them); the tree was reverted and rebuilt clean. **Blocked on
the user**: the target's exact terms (section 6, T1-T4) and the approach (section 5).

The request (2026-10-30), verbatim: "在全链（所有队列里的任务）最后加一个优化任务，要让这套引擎的运行严格实时", and
its purpose: "其实目的是让玩家能感知不到回滚导致的卡顿发生。另外，因为这台开发机性能远高于平均水平的玩家，所以即使达成了
在这台机器上的硬实时可能还是不够的". The criterion (2026-10-30): "闪一下那种是可以接受的。只是不要让玩家觉得响应慢
（unresponsive）" - a remote ball snapping after a correction is acceptable; the game must never feel unresponsive.
The reference machine: this machine's E-cores ("你可以直接用小核，可以大致反映绝大多数玩家的CPU性能").

A game tick is 1/66 s = 15.15 ms (`timeline.hpp` kTickSeconds); the level time factor 0.002 makes one tick two IVP
PSIs. The deadline is per tick: everything a tick does - its live step (two PSIs, scripts), the session's work in
that frame (capture, journal, any rewind) and, on retail, the render - inside 15.15 ms, every tick.

## 1. What a player feels

### 1.1 How the retail game runs a late frame (code, then measured)

In a session the mod drives fixed ticks (`BallanceMMOClient/session/fixed_tick.cpp`): the CKTimeManager's minimum
and maximum delta are both pinned to 1/66 s, so one `CKContext::Process` is always exactly one tick (two PSIs) -
**a long frame never becomes a time jump inside one Process**. The pacing (`on_process`, at the top of OnProcess,
after Process(T) has stepped the world):

- ahead of the schedule: sleep until the tick's wall-clock slot (the frame's idle time);
- behind by more than one tick: `SkipRenderForNextTick` - the next Process follows at once, **with no render**, until
  the game is back within one tick. On screen: the last image is held for the whole overrun, then the next image
  shows the world several ticks further on (a freeze, then a jump);
- behind by more than the catch-up limit (33 ticks + a slew's remainder): the schedule is restarted (a rebase); the
  core session answers a rebase with a resync - a second hitch.

Without a session (vanilla) the time manager runs `CK_FRAMERATE_SYNC` with the delta clamped to 1-200 ms, so a late
frame is one long step (up to 200 ms of motion in one Process). The headless pacer (`session_client.cpp`
`pace_and_frame`) mirrors the session rules (catch up back to back, rebase beyond the threshold), without a render.

Measured on the E-cores (section 2), retail: a frame's own work outside the session is small - the gap between
frames (the buffer flip plus Process: input poll, behaviours, the two PSIs, managers) is **0.54-0.60 ms p50, 4.7 ms
max**, and the render (pre-render to post-sprite-render callbacks) **0.52-0.65 ms p50, 0.75-1.5 ms p99, 7.3 ms
max**. The one segment no column holds - OnProcess's return to the pre-render callback (the managers after BML's
and the Player's loop) - is 0.06-0.07 ms p50, 0.13-0.16 p99, 4.0 ms max, so "work" + render + sleep is the whole
frame. The pacing sleep is 10.6-10.8 ms p50: at the median a retail frame is 70 % idle. Everything that breaks the
deadline is the session core's work (section 3).

The freezes are long, not one frame: the catch-up only renders again once the game is within one tick of its
schedule, and while each frame's work stays near 15.15 ms the game cannot gain on the schedule. In L11
(`scripts/retail/srt_ctl`) client 1 rendered **nothing for 167 consecutive frames (2.5 s, ticks 600-766)** and
client 2 for 125 (1.9 s); in the second run (`srt_ctl2`) client 1 skipped 840 renders in all. Both runs ended in a
resync on one client (run 1: a tick-driver rebase at 758 after two 250 ms frames; run 2: "own beam moved 750 ->
752 ... an own pair, resync" - RTR's E-core slip, section 3.4).

### 1.2 The own ball's key-to-force and key-to-screen latency

- **Key to first force: 0 ticks, at 0 and at 150 ms, headless and retail.** The INPUT record of tick T is what the
  own replica polled for step T (core design §4, observe_input); the own ball's acceleration v(T) - v(T-1) jumps
  in step T itself (`work/SRT/keylat.py` on journals with one checkpoint per tick: headless `core_ref_s13/solo1`
  (0 ms, `--drive`) 7 of 8 key edges at +0 (one -1), `core_ref_s13/h1_150` (150 ms) 3 of 3 at +0; retail
  `srt_ctl` clients 1 / 2: 9 edges at +0, 2 at -1, 4 undecidable (ball in the air or dead)). The -1 edges are
  undecidable too, not an earlier response: `keylat.py` starts its search at T - 1 on purpose (to expose an
  off-by-one in the key-to-record attribution) and reports the FIRST jerk above threshold; a jerk at T - 1 is an
  acceleration change before the INPUT record carries the key, so it is another event (a contact or landing) that
  hides whether the key's own jump came at +0. An attribution off by one would move every edge, not 3 of 25. The
  0-tick result rests on the 19 edges at +0 and none later. The
  input delay is not on this path: it is the server's wait for a tick's inputs (`tick_scheduler::deadline`) and
  sets the mirrors' lag and the rewind depth, never the own ball's response.
- **Key to screen (retail).** A key is polled by the next Process, which starts right after the previous frame's
  render; the step's result is presented by that frame's render after the pacing sleep. Over presses spread evenly
  through the session (`rcensus.py --keys`, 0.25 ms grid, flip and display scan-out not included): L12
  (`srt_l12`, 150 ms) **p50 22.8, p90 29.0, p99 31.7, p999 89-107, max 113-131 ms**; 3.1-3.2 % of presses wait
  more than two ticks (30.3 ms). L11 (`srt_ctl`): p50 23.5, p90 31, **p99 2094-2166 ms, max 2.5 s** (the 2.5 s with
  no render), 11-12 % over two ticks. So without session overruns the key-to-screen time is 1-2 ticks (the pacing's
  own sampling and sleep); **every session overrun adds its whole length to every key pressed during it**, and a
  catch-up without renders adds the whole freeze. The responsiveness criterion and the frame criterion are
  therefore the same budget.

### 1.3 A threshold to judge by (proposal; the terms are the user's, section 6)

General HCI sources put the limit for a response to feel instantaneous at about 0.1 s (Miller, "Response time in
man-computer conversational transactions", AFIPS FJCC 1968; Card, Moran and Newell, *The Psychology of
Human-Computer Interaction*, 1983, the perceptual processor's ~100 ms cycle; Nielsen, *Usability Engineering*, 1993,
ch. 5). Continuous control of a rolling ball is stricter than a conversational response: a held image of more than
one tick is a visible stutter of the own ball and the camera. The proposal therefore takes the game's own tick:

- no displayed frame later than one tick because of session work, i.e. a tick's work plus its render inside
  15.15 ms on the reference machine, every tick (the max, not a percentile);
- key-to-screen never above what the pacing itself gives (two ticks, 30.3 ms) because of session work, and no
  option adds a tick to it or makes it vary (the user's rule).

## 2. Method, layout and the P/E ratio

- **Reference cores (decision 33).** i7-14700KF: logical processors 0-15 = 8 P-cores (2 threads each), 16-27 = 12
  E-cores. Headless: four clients, each pinned right after launch to 3 E-cores of its own (16-18, 19-21, 22-24,
  25-27; `work/SRT/pin_clients.py`); the P arm on 4 logical P-processors each (0-3, 4-7, 8-11, 12-15). Retail: two
  game copies on 6 E-cores each (16-21, 22-27; RTR's `opc.pin`). Server: the Arch WSL VM (HLX's build), unpinned.
  Host sampler beside every run: max total 15.6-25.1 %, worst outside process 0.9-2.2 %; the census's
  thread-cycle check (wall time minus the thread's own cycles) found **no preemption** in any over-budget frame
  (headless cpu = wall within 1-5 %; retail gap preemption <= 0.1 ms), so nothing below is host contention.
- **Diagnostic builds** (reverted): headless `session_client.cpp` + `rollback.hpp`: per frame pre_step / live tick
  / post_step, thread cycles, the deltas of the rollback stats (rewinds, replayed ticks, restore, replay, live
  capture) and the replay loop's split (script pre-step, physics step, capture); retail `BallanceMMOClient.cpp` +
  `core_host.cpp` + `rollback.hpp` + `script_replay.hpp`: OnProcess entry, the pacing sleep, `core_host_frame`,
  OnProcess exit, CK pre-render and post-sprite-render callbacks, the same deltas, and every 50 ticks the capture
  split (scripts / IVP blob / per-body reads) and each selected script's capture time. Rows kept in memory and
  written at the session's end (no I/O on a frame). Every headless replay with the diagnostic build was
  **IDENTICAL** to its live journal (`e0_compare.py`, 20 replays), so the timers change nothing.
- **Runs** (one at a time, snapshots off, the deployed defaults: trafo inputs, placement, union, session clock):
  headless live E `l12x_on2`, `h1_on`, `l8x_on`, P `l12x_on2` (RCB's axes recipes, 150 ms, 4 clients, 40 s; every
  VERDICT PASS, server journals N/N, 0 mismatches, 0 resyncs / rebases / starved / far); retail E: RTR's L11 ctl
  twice (`srt_ctl`, `srt_ctl2`, 0 ms) and RT2's SP8-e L12 (`srt_l12`, `net lag 150` on both); offline: the E
  captures of `l12x_on2` and `h1_on` replayed by one binary on E and on P (paired), and HL's `r16_l11_cross` on E.
- **P/E ratio** (same frames, same binary, E-replay over P-replay, sum of frame times per client): **1.60-1.86,
  median 1.7** (L12 1.60 / 1.75 / 1.86 / 1.67, h1 1.60 / 1.69 / 1.73 / 1.66); by part: live tick 1.31-1.52, post_step
  1.65-1.92, replay 1.52-1.73, restore 1.67-1.81, capture 1.60-1.98. Live, the same recipe: frame p50 4.36 (E) vs
  2.62 ms (P), replay 1.87 vs 1.17 ms per tick, restore p50 2.80 vs 1.59 ms. Earlier P-core / unpinned figures
  (RCB, OPTR, RTR's unpinned rows) convert by about x1.7.

## 3. The census (E-cores)

### 3.1 Headless (150 ms, 4 clients, 40 s; all four clients pooled; `work/SRT/live/*`, `census.py`)

| arm | frames | live frame p50 (pre / tick / post, capture) | live p99 / max | rewind frames (with >1 rewind) | rewinds per frame max | depth per rewind p50 / max | restore p50 / max | per replayed tick: scripts + step + capture + rest | frames > 15.15 ms (all rewinds) | max frame |
|---|---|---|---|---|---|---|---|---|---|---|
| L8 `l8x_on` | 7621 | 2.86 (0.16 / 0.32 / 2.36, 1.32) | 4.06 / 7.33 | 28 (14) | 4 | 20 / 27 | 2.41 / 3.48 | 1.42 = 0.03 + 0.06 + 0.72 + 0.61 | 28 (0.4 %) | 180 |
| L12 `l12x_on2` | 6536 | 4.36 (0.24 / 0.35 / 3.75, 2.40) | 5.97 / 12.3 | 54 (21) | 6 | 22 / 33 | 2.80 / 3.42 | 1.87 = 0.07 + 0.05 + 1.32 + 0.43 | 54 (0.8 %) | 294 |
| h1 `h1_on` | 8377 | 2.86 (0.13 / 0.29 / 2.22, 1.24) | 4.70 / 11.3 | 128 (33) | 6 | 22 / 34 | 3.24 / 12.6 | 1.19 = 0.08 + 0.16 + 0.71 + 0.24 | 117 (1.4 %) | 110 |
| L12 on P (conversion only) | 7218 | 2.62 (0.17 / 0.26 / 2.18, 1.28) | 3.87 / 6.99 | 54 (23) | 6 | 21 / 28 | 1.59 / 2.31 | 1.17 = 0.05 + 0.03 + 0.81 + 0.28 | 54 (0.7 %) | 183 |

- **Every frame over 15.15 ms is a rewind frame**; no live frame reaches the budget on the E-cores (live max 7.3-12.3
  ms). Frames over 15.15 / 30.3 / 45.45 ms (the engine's hitch budget B): L8 28 / 20 / 14, L12 54 / 54 / 44, h1
  117 / 93 / 68 (P-core L12: 54 / 37 / 23). Retail, the same three counts: L11 `srt_ctl` 72 / 9 / 5 and 35 / 10 / 8,
  `srt_ctl2` 59 / 20 / 19 and 88 / 16 / 12, L12 10 / 3 / 1 and 9 / 3 / 1. The live step itself (scripts + two PSIs) is 0.29-0.35 ms p50; the live frame is post_step (capture and
  journal), 2.2-3.8 ms.
- **The replayed simulation is cheap; its bookkeeping is not.** Per replayed tick the scripts and the physics step
  are 0.09-0.24 ms; the replayed tick's capture is 0.71-1.32 ms (50-71 %); "rest" (inputs, births, the amend, the
  headless-only blob diff of the first 8 rewinds) 0.24-0.61 ms.
- **Several rewinds in one frame.** 33-50 % of the rewind frames hold two to six rewinds. Each late lifecycle event
  rewinds on its own (`client_core_lifecycle.cpp:405-412`, `rewind_world` per event): at L12 tick 521 three players
  beamed at 500, and each client restored tick 500 and replayed 21 ticks **six times in one frame** (one per
  Unphysicalize and Physicalize, the "ball of N born at tick 501" rewinds of the client log): 126 replayed ticks,
  196-294 ms. That is **why the L12 maxima exceed t_max** (RCB's open question): t_max is one rewind's time, a frame
  can hold up to six. (Also: a rewind frame's post_step is 4.5 ms longer than a live one's, 8.2 vs 3.5 ms p50.)
- The four clients' over-budget frames coincide in wall time (50 of 54 on L12) because the same relayed event
  reaches all of them at once - the same work, not host contention (cpu = wall).

### 3.2 Retail x86 (E-cores; `scripts/retail/srt_*`, `rcensus.py`)

Per frame: work = the gap (flip + Process) + the mod's OnProcess without the sleep (core included); total = work +
render.

| session | work p50 / p99 / max | core p50 / p99 / max | render p50 / p99 | frames over 15.15 (work+render) | causes | per replayed tick (capture) | restore per rewind | renders skipped | resyncs |
|---|---|---|---|---|---|---|---|---|---|
| L11 ctl `srt_ctl` c1 / c2 | 3.6 / 15.9 / 96, 3.4 / 17.3 / 255 | 2.9 / 15.1 / 95, 2.8 / 16.3 / 254 | 0.64 / 1.35, 0.61 / 1.07 | 72, 35 | rewind 14 / 24, core (no rewind) 58 / 11 | 3.76 (3.09), 3.79 (3.26) | 7.5, 5.2 (max 55) | 218, 149 | 0, 1 (rebase at 758) |
| L11 ctl `srt_ctl2` c1 / c2 | 3.4 / 23.3 / 277, 3.5 / 19.3 / 208 | 2.7 / 22.5 / 277, 2.8 / 18.1 / 207 | 0.55 / 1.05, 0.63 / 1.39 | 59, 88 | rewind 23 / 25, core 36 / 63 | 6.25 (5.55), 2.80 (2.35) | 11.2, 7.3 | 840, 285 | 1 (own pair 750 -> 752), 0 |
| L12 `srt_l12` c1 / c2 (150 ms) | 3.7 / 4.4 / 51, 3.7 / 5.0 / 50 | 3.0 / 3.6 / 50, 3.0 / 4.1 / 49 | 0.52 / 0.75, 0.52 / 0.83 | 10, 9 | rewind 3 / 3, sector activation 7 / 6 | 1.40 (1.12), 1.39 (1.11) | 1.9 | 8, 7 | 0, 0 |

- **L12**: the rewinds are 22-27 ticks at 1.39 ms a tick plus 2 ms restore = 37-51 ms each (3 per client in 23 s
  of scripted play); the other over-budget frames are the **sector activations** (ticks 388-389 and 689-690 after
  the s3 / s4 crossings, 231 and 34): 15.0-16.0 ms of core work with no rewind and a normal capture.
- **L11**: the restores are 5-11 ms on average, **25-55 ms around ticks 599-653** (the sector-2 pass at 628), and the
  replayed tick is 2.8-6.2 ms, 82-89 % of it capture. And the live capture itself jumps.

### 3.3 The live capture after a trafo explosion (retail): the IVP blob

The 50-tick capture split of `srt_ctl2` (`scripts_top.py`):

| window | capture per tick | scripts | IVP blob | per-body reads and the rest |
|---|---|---|---|---|
| c1 300-450 | 0.99-1.31 ms | 0.60-0.69 | 0.24-0.38 | 0.16-0.23 |
| c1 600-1150 | **10.8-15.4 ms** | 0.69-1.02 | **9.4-13.3** | 0.74-1.30 |
| c2 550-750 | **12.3-13.5 ms** | 0.66-0.82 | **10.8-11.8** | 0.77-0.85 |
| c2 750-1100 | 1.9-2.7 ms | 0.47-1.17 | 0.46-0.83 | 0.62-1.40 |

**Measured (the symptom):** the selected scripts are unchanged (no script over 0.07 ms a tick: OPC's reuse
holds); **the IVP savestate blob's section of the capture grows forty-fold** (0.24-0.38 -> 9.4-13.3 ms a tick), for
2.3 s to 8+ s; the blob was 137 005 bytes at client 1's resync at 807. At 10-15 ms of capture a tick the live frame
alone is at the budget, so after any rewind the game cannot gain on its schedule: client 1's work in ticks 600-740
of `srt_ctl` is 14.2-15.7 ms per 10-tick bin with a 0.1 ms sleep, and the renders stop. **Not established (the
trigger):** both worlds hold the same two Wood piece sets from 458 and 498 (51 pieces each; FINAL rows bit-equal
to the server), yet the windows differ per client and per run (`srt_ctl`: c1 from ~520, c2 from ~600 - each
client's own checkpoint beam; `srt_ctl2`: c2 from ~515 to 750, c1 from ~600 to past 1150, with a Stone explosion
at 921 and c1's resync at 765 "70 pieces kept out of the restore"), so a shared pile alone does not explain it:
per-client state is involved. The working hypothesis - a pile's pairwise overlap / mindist / contact state, grown
by something per client (the own ball's beam into or beside it, the pieces a resync keeps out) - is to be settled
by a per-section profile of `physics_savestate_capture.cpp` with the window's trigger and end named (SRT2's first
task, section 5). The same observation leaves a **correctness question open**: two worlds with bit-equal bodies
whose savestate blobs differ in size and capture time over different windows hold different IVP collision
bookkeeping (pairwise overlap, mindist, contact state) - the class of SFD's cause (A) (the own ball's hull entries
and overlap centre differ from every copy of it), and a possible source of a later divergence, not only a cost.
SRT2 therefore also compares the blob's sections across the two clients (and the server's world where it can be
captured) at the same tick inside a window; if they differ, the finding joins cause (A) on the world-agreement
track. For scale: the headless h1 arm (trafo pieces, fewer per pile) has 1.24 ms live capture, HL's L11
cross (no trafo) 0.7-1.4 ms throughout.

### 3.4 RTR's E-core L11 own-pair resync: root cause

Symptom (RTR, reproduced twice here): on the E-cores, L11 ctl client 1 falls behind its schedule from about tick
600 and resyncs at 749-750 ("own beam moved 750 -> 75x ... an own pair, resync"); P-core runs slip 8-9 ticks and
do not. Cause: the IVP blob section of the capture is 9-13 ms a tick (3.3; after the Wood explosions, its trigger
per client not yet named) for seconds, so a tick's work is 14-16 ms with no rewind; the rewinds of the sector-2 pass (restores 25-55 ms at 599-653) put the
game 18+ ticks behind and it cannot catch up (each frame ~0.1 ms under the tick at best). The beam the core stamps
one tick ahead (`armed for tick 750 requested before post_step(749)`) then leaves the client after the server has
passed 750, is moved, and an own pair moved is a resync by design (plan step 22). In run 1 the slip was deeper
(two 250 ms frames at 756-757 on client 2: two rewinds of 15-16 ticks at 12 ms a replayed tick with the pile
alive) and the tick driver rebased (33 ticks) - a resync too. The resync itself is a capacity failure of the E-core
reference (the slip, then a moved own beam; option (a3) and f1 below). Whether the blob window behind it is only a
cost is NOT established: per-client blob windows over bit-equal bodies mean per-client IVP collision bookkeeping
(3.3), possibly SFD's cause (A) class. That stays open until SRT2 names the trigger and compares the blob sections
across clients.

### 3.5 Human play multiplies the rewinds

The scripted runs rewind 20-47 times in 40 s. In the user's own trial (`work/TRIAL1/rounds/round1-holdlast`, L11,
two humans, 538 s) each human changed keys **1.3-1.5 times a second** and client 1 made **1019 rewinds (1.9 a
second), 24 ticks deep on average**. With three remote humans that is about 4-6 rewinds a second. On the E-core
reference each is 35-50 ms on L12 (more on L11): several over-budget frames every second - a game that stutters
continuously, not occasionally.

## 4. The options, measured or bounded on the E-cores

"Scales" = whether the worst frame stays bounded as the machine gets slower (a budget) or grows with it (a cost
cut). d = the rewind depth, about the relay delay in ticks (input delay + one trip; 21-28 at 150 ms; it cannot be
cut without delaying the own ball, which the responsiveness rule forbids; resim-at-latency).

| | option | worst frame on the E-cores it reaches (measured / bounded) | exactness risk | protocol / engine / design rule | effort | scales |
|---|---|---|---|---|---|---|
| a1 | **Coalesce a frame's corrections into one rewind** (all late inputs and lifecycle events drained in pre_step applied, then one restore to the earliest anchor and one replay) | L12 tick 521: 294 -> ~50 ms (6 x (3.1 + 21 x 2.1) -> 1 x); removes the 2-6 x of 33-50 % of rewind frames; one rewind per frame at most | **medium**: the last sequential rewind replays with every event known, but each rewind also does per-rewind bookkeeping between them (the tick-521 log: "mirror ... revived until tick 501; 2 window event(s) queued", "generation 1 -> 2 ... recorded so from here on", remakes) - the final state is the same only if that bookkeeping is independent of how many rewinds ran and in which order (the class HLX just closed a hole in). Pass rule: the stored `cpu_full/srt_E_l12x_on2` captures (the six-rewind frame at 521) replayed by the coalescing build -> journals IDENTICAL, plus the wire check and the exactness replays | none | small-medium (shared core) | no (cost) |
| a2 | **Do not capture every replayed tick**: re-capture every k-th replayed tick (and the last), a later rewind into an uncaptured span restoring the nearest record before it and replaying the difference. (Lifecycle and sector corrections anchor anywhere inside the reach, kReachHistoryTicks, not only at an unconfirmed input's tick, so "capture only from the oldest unconfirmed input on" is not a valid bound.) The builder starts from RC2 (b)'s cache, which reads the record of t - 1, and the existing deferred-capture path (`awaiting_capture_`, `rollback.hpp` ~975-990) | per replayed tick: headless L12 1.87 -> ~0.55-0.88 ms, retail L12 1.39 -> ~0.28-0.56; a 25-tick L12 rewind on retail E 2 + 25 x 0.28-0.56 + 3.7 live = **13-20 ms** | medium: records missing in a span change which anchor a later rewind uses (up to k - 1 more ticks replayed, the same result if the replay is exact); RC2 (b)'s t - 1 source, amends and FINAL rows of uncaptured ticks need a rule | none (journal FINAL rows only where captured: a format note) | medium | no |
| a3 | **The IVP blob after the explosions** (3.3): name the window's trigger per client and profile `physics_savestate_capture.cpp` by section on x86, then cut (e.g. a sleeping pile's collision state reused like RC2 (b), or a cheaper encoding) | retail L11 live capture 12-15 -> ~1.5 ms through the windows (bounded by the pre-window figure); removes the 2.5 s render freezes and RTR's resync | depends on the cut (a blob change is checked by the savestate exactness set) | none expected; an access-only engine change only if IVP must be read differently | medium (profile first) | no |
| a4 | **Restore** (retail 5-11 ms average, 25-55 ms around L11's sector-2 pass; headless 2.4-3.2): profile the retail restore at the sector pass (steps 0 / 5, OPTR R.4) | bounded by the measured share: -2 to -40 ms on the worst L11 frames | low-medium | none | medium | no |
| a5 | **Sector activation** on retail (15-16 ms core in one frame, L12) - spread or prepare the activation's setup | -10 ms on those frames | medium (activation is replayed exactly; a split must keep its tick) | none | medium | no |
| b | **Spread a rewind over frames** (time-sliced replay) | bounded at any budget *if* a second world existed; without one, the corrected world shown behind real time - the own ball's response delayed by up to d ticks (= option d's cost) | a second world: high | **a second CK context and IVP environment do not exist in Player.exe** (one CKContext, physics_RT's one manager, the scripts bound to it); a savestate swap per slice costs two restores (retail 2 x 5-11 ms) - more than a slice | very large / infeasible on retail | yes, but only with a second world |
| c | **Work off the main thread** (capture serialization, replay on a worker world) | capture = reading IVP / CK state, which must happen on the game thread; only the byte encoding could move (a small share); a worker world is (b)'s second world | high (CK / IVP are not thread-safe) | - | large | partly |
| d | **Fewer / shallower rewinds**: prediction, the reach cap, input delay | depth d is the relay delay; a local input delay of D ticks (GGPO) shortens every rewind by D but **adds D ticks to the own ball's key-to-screen time - forbidden**; better prediction cuts the count (TAS / trial evidence: hold-last already beats R6 for humans); the reach cap (B = 45.45 ms) sits at its 48-tick floor on every E-core client (`cap=48/...` on every status line: the meter would allow only ~24 ticks) | - | - | - | - |
| e | **The live tick on heavy levels** | not the problem on the E-cores: the live step (scripts + two PSIs) is 0.29-0.35 ms headless, 0.54-0.60 ms gap retail; the live frame is the session's capture / journal (2.2-3.8 ms headless, 2.7-3.0 ms core retail). The no-rewind floor is under budget everywhere except retail's sector activation (15-16 ms) and the pile blob (12-15 ms) | - | - | - | - |
| f1 | **The catch-up renders**: never skip more than one render in a row while catching up (render every other frame) | the 2.5 s freezes become a 33 Hz display while behind; the slip itself is not removed (a render costs 0.5-0.65 ms, slowing the catch-up by that) | none (presentation only; the simulation's ticks are unchanged) | none (retail host only) | small | yes (bounds the freeze, not the work) |
| f2 | **A run-time per-frame budget** (the reach meter's idea at frame scope): measure the per-tick replay and restore cost on the player's machine; when a frame's corrections would exceed the budget, carry the rest to later frames | the only true budget without a second world is to *not* correct inside the frame - i.e. let the correction wait (the mirrors snap later: acceptable) **only for corrections that do not touch the own ball**; a correction that changes the own ball's past must be applied before its next present | medium, and exact only under three conditions: (1) **classification is conservative and cannot use the replay it postpones**: whether a correction touches the own ball is unknown until it is replayed, so "touches" must be decided before it from the live window (any contact, overlap or mindist entry between the own ball and the corrected body, or anything the corrected body touches, in the window = touches; then it is applied now); (2) **while it waits the live world runs on against the stale mirror** - if the own ball then meets that mirror (or what it touched), the deferred correction would change the own ball's past, an own-ball snap the responsiveness rule forbids: the classification must be re-evaluated every frame and the correction forced as soon as the condition turns true; (3) **a maximum deferral inside the reach**: the anchor ages one tick per frame and drops out of the reach (floor `kReachFloorTicks` 48) and the history (`kReachHistoryTicks` 128, rollback.hpp:689-692), after which the correction is a miss, i.e. a resync - so a deferral is forced at (reach - d - margin) ticks whatever the budget. Applied in time from the same anchor, the replay is the same replay, deeper | none; changes when (not whether) a correction is applied | medium-large | yes for remote-only corrections, up to the forced cases (2) and (3) |

### 4.1 What the numbers say about scaling

- The replayed simulation itself costs 0.09-0.24 ms a tick on the E-cores; a 25-tick rewind's *simulation* is 2-6
  ms. Everything else is bookkeeping: the replayed captures (50-89 %), the restore (2-55 ms), several rewinds per
  frame. Cost cuts (a1-a5) can bring L12 at 150 ms near the budget on the E-cores (a1 + a2: ~13-20 ms for one
  25-tick rewind on retail) but not with margin, and the worst frame grows with d (the latency) and with the
  machine's slowness: on a machine 1.3x slower than an E-core the same rewind is 17-26 ms.
- A budget that holds whatever the machine needs either a second world (b; infeasible in Player.exe) or permission
  to apply a correction later than the frame it arrives in (f2). Under the user's criterion the latter is allowed
  exactly where the correction does not change the own ball: a remote ball snapping a few frames later is the
  "闪一下" the user accepts. A correction that changes the own ball (a collision with a mirror, a mechanism the own
  ball rides) cannot wait, so its rewind's cost stays a cost-cut problem; how often that happens in human play is
  not measured here. Nor can a correction wait once the own ball meets what it would correct, or past its maximum
  deferral inside the reach (the three conditions in f2's row): f2 bounds the frame only for the corrections that
  stay remote-only for their whole deferral, and the forced ones fall back to the cost cuts.

## 5. Recommended combination and step plan (each step judged on the E-cores)

Order by measured payoff per effort; each step's pass rule against the confirmed target (section 6), measured with
this step's census tools (`srt_diag.patch` re-applied) on the E-core layout of section 2.

1. **SRT1 (f1) catch-up renders** (retail host only). Pass: `srt_ctl` recipe on E-cores: no run of more than one
   skipped render; the simulation's journals unchanged against a same-build run with the old policy (FINAL rows
   bit-equal to the server as RTR).
2. **SRT2 (a3) the blob after the explosions** - first name the window's trigger and end per client (3.3), with a
   per-section profile of the blob capture (x86 SimTool or retail), then cut. Pass: retail L11 ctl live capture
   <= 2 ms a tick through the windows; the savestate exactness set; no resync in `srt_ctl` on E-cores (RTR's
   control clause).
3. **SRT3 (a1) one rewind per frame**. Pass: the stored `cpu_full/srt_E_l12x_on2` captures (the six-rewind frame at
   521) replayed by the coalescing build with journals IDENTICAL to the live ones; the wire check (sends SAME); the
   solo / swing exactness replays; L12 E-core max frame <= one rewind's time.
4. **SRT4 (a2) sparse replay capture**, with a verify mode (capture anyway, compare the anchor a later rewind
   would have used). Pass: exactness set; per replayed tick on E-cores <= 0.6 ms headless L12, <= 0.4 ms retail L12.
5. **SRT5 (f2) deferred remote-only corrections**, the budget measured at run time - only if the user accepts its
   rule (section 6, Q2), with f2's three conditions built in (a conservative pre-replay "touches the own ball"
   test re-evaluated every frame, a forced apply when it turns true, a maximum deferral inside the reach). Pass:
   every E-core frame (retail L11 / L12, 150 ms, four members incl. human-like key rates from the trial's
   journals) inside the confirmed target; own ball key-to-screen unchanged (`rcensus.py --keys` p100 <= the
   no-session bound); the journals' FINAL rows bit-equal to the server; zero corrections that changed the own
   ball's state after being deferred (a verify mode replays each deferred correction at arrival too and compares
   the own ball); no miss or resync caused by a deferral; the forced-apply rate reported.
6. (a4 / a5 restore and activation profiles as needed after 1-4.)

## 6. For the user (blocked_on_user)

- **Q1, the target's terms.** Proposed: T1 every frame's session work plus its live step plus the render inside
  15.15 ms on the E-core reference (max over the gate sessions); T2 no render skipped because of session work
  (or: never two in a row); T3 the own ball's key-to-screen time never above the pacing's own two ticks because of
  session work; T4 no rebase or resync caused by a slip. Which of these, at what latency (150 ms?), with how many
  members, and on which levels.
- **Q2, the approach.** Cost cuts only (a1-a4 + f1: measured to reach L12 at 150 ms near, not inside, the budget on
  the E-cores, and growing with latency and machine slowness), or cost cuts plus f2 (a budget: corrections that do
  not touch the own ball applied over later frames, so the mirrors snap later; the own ball never waits). f2's
  conditions: "touches the own ball" is decided conservatively before the replay (it cannot be known without it),
  re-checked every frame, and the correction is forced when the own ball meets the stale body; a deferral has a
  maximum inside the reach (48-tick floor), after which it is forced too - so f2 bounds only the frames whose
  corrections stay remote-only, and the forced ones still cost what the cost cuts leave. (b) and (c) are not
  feasible in Player.exe; a local input delay is excluded by the responsiveness rule.

## 7. Checks run, artifacts, not done

- Builds: diagnostic x64 (three rounds) and x86 (three rounds), BUILD-OK / CLIENT-BUILD-OK, DEPS-OK; reverted and
  rebuilt clean (see the step result). No product file changed, so no unit test, GCC check or wire check was due.
- Live: 4 headless (E: l12x_on2, h1_on, l8x_on; P: l12x_on2; stores `maps/arena-test/cpu_full/srt_{E,P}_*`), each
  harness VERDICT PASS with its server journal N/N; 3 retail two-copy sessions (E: L11 ctl x2, L12;
  `scripts/retail/srt_ctl`, `srt_ctl2`, `srt_l12`) - a census, so RTR's FINAL-row judges were not re-run on them.
  The plan section's run list (profiles + live runs + retail sessions + E-core runs) set the count, one run at a
  time, each read before the next.
- Offline: 16 paired E / P replays (L12, h1) and 4 E replays (L11 cross), all IDENTICAL to their live journals;
  2 key-latency replays (`--checkpoint-ticks 1`, HEAD's build), both to the capture's end marker.
- Tools: `.local-tools/work/SRT/` - `srt_axes.sh`, `pin_clients.py`, `pe_pair.sh`, `srt_retail.py`, `census.py`,
  `rcensus.py` (`--keys`), `scripts_top.py`, `keylat.py`, `key_rate.py`, the diagnostic patch (base 4d8d60d4);
  `build-agent-rSRTd` is the diagnostic x64 build (not HEAD's). Note: `cpu_full/pins.json` predates ARX's
  Level_13 change; replays of new `cpu_full` captures take `--pins maps/arena-test/core_ref_s13/pins.json`.
- Not done: a P-core retail session (the P/E ratio is from the headless pairs); the blob's per-section profile and
  its trigger (SRT2's first task); human-rate key streams on the E-cores (SRT5's pass).
