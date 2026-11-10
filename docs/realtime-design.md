# Rollbacks a player cannot feel, on an average player's machine (research, FULL step SRT)

Status: **research only, nothing built** (2026-10-31, base HEAD 4d8d60d4, protocol 24). The diagnostic builds of
this step are kept as a patch (`.local-tools/work/SRT/srt_diag.patch`, with `srt_diag.hpp`, `srt_retail.hpp`,
`srt_counters.hpp` and the `patch_*.py` that applied them); the tree was reverted and rebuilt clean. **Blocked on
the user**: the target's exact terms (section 6, T1-T4) and the approach (section 5).
**Answered (2026-10-31)** and in progress: Q1 "你决定" (the orchestrator set the target: hard T2' no two consecutive
renders skipped, T3, T4; T1 reported), Q2 "先a看看" (the cost cuts first, f2 not built: measure, then the user
decides). Built: SRT1 (f1, the catch-up renders; "As built (step SRT1)" below), SRT2 (a3's cause found: the retail
own ball's in-place beam), BEAMF (the beam as a fresh body, which removed SRT2's blob window), SRT3 (a1, one
lifecycle rewind per frame), SRT4 (a2, sparse capture of replayed ticks), SRT2B (a3's remainder, the capture's
entity lookup scope on retail); each has its "As built" note below. **Judged after the cost cuts: SRTJ** ("Judged
(step SRTJ)", the last section): T2' and T4 met on every run, T1 and T3 not; **blocked on the user** for f2 and for
whether the target is met.

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
  shows the world several ticks further on (a freeze, then a jump). Since SRT1 (f1) a core session never skips two
  renders in a row: the catch-up shows every other tick (the legacy path is unchanged);
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
| d | **Fewer / shallower rewinds**: prediction, the reach cap, input delay | depth d is the relay delay; a local input delay of D ticks (GGPO) shortens every rewind by D but **adds D ticks to the own ball's key-to-screen time - forbidden**; better prediction cuts the count (TAS / trial evidence: hold-last already beats R6 for humans); the reach cap (B = 45.45 ms) sits at its 48-tick floor on every E-core client (`cap=48/...` on every status line: the meter would allow only ~24 ticks; after SRT3 / SRT4 this holds only for a headless client on L12 with the trafo inputs - late-player-design §2.5 "As built (step LBR)": E-core caps of 54-120 headless and 73-90 retail) | - | - | - | - |
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

## As built (step SRT1, f1: the catch-up renders)

- **Code** (`BallanceMMOClient/session/fixed_tick.cpp` `on_process`, retail host only): in the "behind by more than
  one tick" branch a frame skips its render only if the previous counted frame did not; a frame after a skipped one
  renders without waiting (it does not sleep: it is still behind). The flag is a file-scope static
  (`previous_render_skipped`), not a member: `fixed_tick_` is a data member of `BallanceMMOClient`, whose layout BML
  fixes; there is one driver. It applies only with a `pacer` (`core_host_pacer()`, non-null while a core session
  runs): without one - the legacy path, and a core session before its running phase - every behind frame skips as
  before. The ticks, their order and the rebase rule are unchanged; the SessionLead slew (`lead_slew::advance`)
  gains a tick only while less than one frame is due, so a raise is a single skip and is not changed beyond a kept
  render's cost; a SAXH late start (`slew(N)`, the origin moved N ticks at once) does catch up with every other
  frame rendered. No shared, headless or server file changed (no wire, GCC or headless check is reached).
- **Measured** (the `srt_ctl` recipe = `work/SRT/srt_retail.py arm`, RT1's ctl, L11, 0 ms, two copies on E-cores
  16-21 / 22-27, snapshots off, the deployed defaults; `srt_diag.patch` re-applied for the run and reverted; run
  `scripts/retail/srt1_ctl`; `work/SRT1/skip_runs.py`, `work/SRT/rcensus.py`):

  | | longest run of skipped renders | longest render-to-render gap | key-to-present p99 / max | frames over 15.15 (tick+render) | renders kept after a skip |
  |---|---|---|---|---|---|
  | before `srt_ctl` c1 / c2 | 167 / 125 frames | 2459 / 2366 ms | 2166 / 2094, max 2556 / 2485 ms | 72 / 35 | - |
  | before `srt_ctl2` c1 / c2 | 837 / 145 frames | 12637 / 2113 ms | 12334 / 1806, max 12725 / 2197 ms | 59 / 88 | - |
  | after `srt1_ctl` c1 / c2 | **1 / 1** (173 / 268 skips, every one single) | 277 / 219 ms (one long rewind or resync frame, not skips) | **120 / 128, max 419 / 360 ms** | 163 / 52 | 173 / 268 |

  T2' (no two consecutive renders skipped) holds on both clients. The remaining long gaps are single frames (a
  rewind, client 1's resync re-step of 51 ticks), the work SRT2-SRT4 cut. The share of presses over two ticks is
  11.4-13.0 % (before: `srt_ctl` 11.1-11.7 %, `srt_ctl2` 14.6 / 35.0 %): f1 shows the catch-up but does not shorten
  it, so T3 is not met (SRTJ's judgement after the cost cuts). Frames over 15.15 ms (tick + render, T1, reported) rise where the game is behind
  (client 1: 163, 140 of them "core" frames of the blob window): a kept render adds its ~0.6 ms to a frame that was
  already at the budget - the cost the option table named.
- **FINAL rows against the server** (`work/RTR/judge_run.sh`, `rt1_judge.py`, `final_fields.py`): RTR's `rtr_ctl`
  signature exactly - mechanisms 23/23 bit-equal on both clients, mirrors worst position 0 m (rotation-only rows
  <= 2.98e-8), trafo pieces worst position 0 m (rotation-only rows <= 2.97e-8, RT1's x87 read), the own balls'
  one row at the trafo beams 300 / 340 (0.001946 m); the server journal replays 2568/2568, 0 checkpoint mismatches.
- **Resync (T4, not met, pre-existing).** Client 1 resynced at 752 ("own unphysicalize stamped tick 752 echoed
  moved by the server (applied 766)"): section 3.4's E-core slip (the savestate blob 133 085 bytes at the resync,
  the window of section 3.3). Client 1 was about 22-26 ticks behind its schedule at 700-751 (wall ticks less
  counted ticks, against its 200-500 baseline); f1's kept renders in 560-752 (96 renders, 81.7 ms = 5.4 ticks) are
  a fifth of that, and an own pair moved by even two ticks resyncs (`srt_ctl2`: 750 -> 752). The same signature
  and cause as the old policy's E-core runs (client 1 resynced in 3 of 4: RTR twice, `srt_ctl2`; not in
  `srt_ctl`, whose client 2 rebased), f1's share bounded at 5.4 of the 22-26 ticks. Its cause is SRT2's (a3).
- **The render span confirms the skips** (the census's r0 / r1): every frame the driver did not skip rendered and
  every skipped one did not, but the last row of each client (the session ended before its render), so
  `SkipRenderForNextTick` skips the render of the frame that calls it, as assumed.
- **Not covered by this run:** the orchestrator's target layout (150 ms, four members, L12 / the arena) - the
  pass rule names the `srt_ctl` recipe; SRTJ judges the set.
- **Assumed: a cheap, unsynchronised present.** The ~0.6 ms a kept render costs was measured on the test copies,
  which run `UnlockFrameRate 1` / `SetMaxFrameRate 1200` (BML.cfg). With a present that blocks on vblank (VSync
  forced by the driver) a kept render can cost up to 16.7 ms; a catch-up whose ticks cost more than ~8.3 ms each
  would then run two ticks per 33.3 ms, below the 66 Hz schedule, and end in a rebase where the old all-skip policy
  converged. Not measured here; SRTJ measures it on a present-blocking setup (a host-side refinement, still
  presentation only, would keep the render only when the previous render span was short).

## As built (step SRT2, a3: the IVP blob window - stopped for the user)

- **The trigger is the own ball's in-place beam, not the trafo pile.** Section 3.3's hypothesis (a pile's collision
  state grown per client) is wrong. Per capture (a section profile of `physics_savestate_capture.cpp` and
  `get_savestate`, one row per call, run `scripts/retail/srt2_diag`, the `srt_ctl` recipe on E-cores 16-21 / 22-27,
  snapshots off; `work/SRT2/srt2_diag_proto.patch`, `capstat.py`): the window opens at exactly the tick of the
  automation `beam` verb's own beam - client 1 at 520 and 1100, client 2 at 600 - and closes when the own ball is
  unphysicalized (client 1's resync at 752 and death at 1168, client 2's resync, tick base 1205 from old tick 1166). In it the collision
  checkers jump from 218-609 to a per-row maximum of 21 971 (client 2) and 24 458 (client 1) (all plain mindists
  under 121 OO watchers; `cap_*.csv` count 2), one object holds up to 21 604 / 23 837 hull entries (count 13), and
  the blob grows from 145-190 KB to 2.5-2.8 MB (not 137 KB: that was a resync blob);
  the capture is 9-12 ms: collisions 4.7-4.9 ms, hull lists 1.9-2.2, `write_world` 1.6-1.7, the rest under 0.7.
  The object is the own ball (`Ball_Stone`, its world's unsuffixed retail ball): its overlap element's radius is
  **1255.5 m** (every other ball 4-12 m), so its watchers hold a mindist to every ledge of every floor and rail of
  the level (A01-A05). The same ball's copies have 4.0-4.6 m on the server (`Ball_Stone_BMMO_3594538873`, a SimTool
  replay of the server journal with `BMMO_REPLAY_SAVESTATE_DUMP`) and 10.5-11.8 m as the mirror in the other client.
- **Cause.** Retail has no own body control, so the core beams the own ball in place (`write_beam`,
  `client_core_lifecycle.cpp:1967`, through `platform_.write_body` -> `physics_view::set_body_state` with the
  default `recheck = false`). That is IVP's optimized beam (`beam_object_to_new_position(..., IVP_TRUE)`,
  `physics_state.cpp:1773`), which sets `current_speed = moved_distance * i_delta_time` ("simulate speed",
  `ivp_calc_next_psi_solver.cxx:289`) and jumps the hull by the distance (`jump_add_hull(diff, moved_distance)`,
  `:300-307`); the hull jump fires the overlap synapse inside the same call (`check_hull_synapses`, `:323`), whose
  recheck sizes the element by `get_coll_range_in_world` = 0.1 s x speed + radius (`ivp_mindist.cxx:827`,
  `ivp_range_manager.cxx:120-123`) while the simulated speed is still set - IVP zeroes `current_speed` only at the
  end of the call (`:328`). Client 2's beam
  at 600 moved the ball 189.6 m in one PSI: 12 515 m/s, 1251.5 m + 4.0 m = the 1255.5 m read. The element's hull
  budget (1253 m of travel) is then not used up at 0.5 m/s, so it stays until the body goes. The server and the
  other worlds rebuild the ball (Unphysicalize + Physicalize, a fresh body) and never see it.
- **What it causes.** The 9-13 ms live capture of section 3.3, the replayed ticks' 2.8-6.2 ms (82-89 % capture),
  the 25-55 ms restores near L11's sector-2 pass (a 2.5 MB blob restored), and through them RTR's E-core slip and
  own-pair resync of section 3.4 (this run: both clients rebased and resynced once, 752 and 1166). Positions are not
  affected: the FINAL rows against the server keep RTR's signature (own ball worst 0.001946 m only at the trafo
  beams 300 / 340, pieces and mirrors 0 m, mechanisms 23/23). It is per-world IVP collision bookkeeping over a
  body whose positions agree, which the plan routes to the user. It is a retail-only mechanism of Stage C's R6 (the
  in-place beam): it is of the same kind as SFD's cause (A) but does NOT explain it - (A) is a headless own ball,
  different from its birth tick, and the headless client has own body control and never calls `write_beam`
  (`client_core_lifecycle.cpp:2226`, it remakes the body). (A) stays open.
- **The beam also makes IVP state depend on the pre-beam pose.** The simulated speed, the hull jump and so the
  overlap radius all scale with the moved distance, not with the beam's target. The replay re-applies the own-life
  beam stamp (`client_core_lifecycle.cpp:2228`, when `!caps_.own_body_control`); a replay that diverges before the
  stamp beams from a slightly different pose and builds a different element and hull budget than the live world did.
- **Option (a), prototyped (not committed):** the in-place beam through IVP's unoptimized path (`recheck = true`,
  `current_speed = 0`, the overlap element rechecked at once). Run `scripts/retail/srt2_proto` (same recipe): no
  window (at most 722 checkers and 191 KB; the blob p50 0.52-0.53 ms, max 2.5-3.2 ms); resyncs 0 / 0, rebases 0 / 0, renders skipped
  2 / 0 (before 133 / 289); the own ball's overlap element then equals the server's fresh copy and the other
  client's mirror at 625 and 700 (radius 4.161 / 4.188 m, the same centre, 248 / 193 checkers in all three worlds);
  the FINAL rows keep the same signature (positions bit-equal but one own row per in-place beam: 0.00195 m at
  300 / 340 and, reached now that client 1 did not resync at 752, 0.00114 m at its beam 750 - R6's residual;
  every other differing row rotation-only, <= 3.85e-8); the server journal replays 2572/2572. The beam verb is
  the only caller of `write_beam` (`client_core_lifecycle.cpp:2019`, `:2228`): in human play the trafo and the
  death respawn rebuild the ball, so the window needs an in-place write of a far displacement - also any
  snapshot or blend correction through `write_body` (the same optimized path), not reached with snapshots off. Live capture through 450-1250:
  p50 2.12-2.19, p99 3.43-3.77 ms (before 2.59-11.10 / 12.9-14.3) - the rest is the pile's per-body reads
  (scripts 0.7 + blob 0.55 + per-body 0.8-1.4 ms: 50-100 moving trafo pieces, three bridge reads each, the RC2 (b)
  cache serving only sleeping bodies), so the pass rule's 2 ms needs a further per-body cut whichever option is
  chosen. Option (a) removes the speed's and the radius's dependence on the pre-beam pose; the hull jump
  (`jump_add_hull`, common to both IVP paths) still advances by the moved distance (not measured).
- **The options for the user** (none chosen): (a) the beam with `recheck = true`, scoped to `write_beam` (a flag on
  the platform hook or a hook of its own: `physics_state.hpp:224-233` reserves `recheck` for snap teleports; the
  prototype gated every retail `write_body` and isolated the beam only because snapshots were off) - window and
  resync gone, the own ball's collision state equal to the other worlds', positions unchanged in the one run; it
  changes what the beam does. (b) the same in the shared bridge for every optimized pose write in
  `write_body_state` (every host and the server; not measured) - zeroing `current_speed` after the write, as first
  proposed, would do nothing: IVP already zeroes it (`:328`) after the radius is fixed. (c) retail own body control,
  the ball rebuilt like headless (Stage C's R6, roadmap item 9; when depends on the stage order). (d) leave it (only
  the test `beam` verb reaches it in human play) and change the recipe; the pass rule on `srt_ctl` as written then
  cannot be met.
- **Left for a step of its own (SRT2b, queued in the realtime track):** the per-body read cut of the moving trafo
  pile (`rollback.hpp:3494-3566`, three bridge reads per moving piece; the RC2 (b) cache serves only sleeping
  bodies). The row's "<= 2 ms a tick through the windows" does not name its statistic; measured with (a): p50
  2.12-2.19, p99 3.43-3.77, max 2.5-3.2 ms for the blob alone. Which statistic is the target is set with the
  user's answer above.

## As built (step BEAMF: the own ball's beam as a fresh body in the shared core)

The user's answer to SRT2's options (decision 49 item 4, Q-R6-BEAM, verbatim "a，并且合到core内"): the beam's stamp is
a fresh body - unmake + make, the server's way - and it lives in the shared client core, used by both hosts. The
orchestrator pulled this part of Stage C's R6B forward (plan section BEAMF) so SRTJ measures without the window.

- **Built (core).** `session_client_core::beam_remakes_body()` (`client_core.hpp`, beside `write_beam`): true when
  the host fills `make_own` / `unmake_own`, i.e. declares `own_body_control` (headless) or `own_body_in_replay`
  (retail, since EL8). `apply_own_lives`' beam-stamp branch (`client_core_lifecycle.cpp`, "if (beam_stamp)") now
  tests it instead of `own_body_control`: on both hosts the stamp takes the own body out and makes it fresh at the
  beam pose (`remake_own_body`: `make_own` inside the event log's mute, no kick, no replica), live at T+1 and in every
  replay that crosses T+1 (the same branch with `live = false`); never an in-place write. `beam_own_ball`'s log line
  takes the same test ("rebuilt here at that tick" on retail too). `write_beam` stays for a host with neither
  capability (none of ours) and for a ball without the navigation replica (`own_driven_entity` empty: headless
  `--no-rollback`, or retail before the navigation graph is known - the game's force controllers drive it then).
  Retail gets no new platform operation: its `make_own` / `unmake_own` (core_host.cpp, host support's
  `make_own_body` / `unmake_own_body`, the headless client's code on the bridge) were EL8's; only their comments say
  "also live at a beam's stamp". No `BallanceMMOClient` data member, no protocol or journal change (the server
  already applies the beam's Unphysicalize + Physicalize pair as a fresh body), no IVP or physics_RT change.
- **Not changed (R6's).** `own_body_control` keeps meaning the own BIRTHS: moved / rejected / missing / superseded
  echoes (`move_own_birth`, `resync_own_life`), `destroyable_own_birth` (OWNB) and the resync's remake. On retail a
  MOVED beam echo therefore still resyncs (`move_own_birth`'s no-control branch), as before this step; a moved or
  never-made beam is not reached by the automation recipes (no such echo in either run below). Making it follow the
  headless move path would, for a never-made beam, take the retail player's body out with no remake - Q-R6-NEVER's
  open question - so it is left to R6B / R6P.
- **The premise (R6D's question, for the beam only), checked in the code and the runs.** The bridge's `physicalize`
  (`physics_state.cpp:1864`) goes through `CKIpionManager::CreatePhysicsObjectOnParameters`, so the fresh body is in
  the manager's entity table the Physicalize / Unphysicalize blocks use; `unphysicalize` (`:1977`) is the block's own
  `delete_silently` (`Physicalize.cpp:243`). So the next trafo and the next death of a bridge-made own ball go through
  the game's blocks as for a script-made one: measured in both runs - client 1's beam into the trafo at 300, then
  "own ball unphysicalized at tick 302" (the trafo), its beam at 1100, then the death at 1157, each reported by
  `OnUnphysicalize`; client 2's at 340 / 342. The body guard (#6) is keyed by the entity and the own group is set by
  `make_own_body`. **Cosmetic, found:** the own ball's collision-sound listeners (`PhysicsCollDetectionListener`,
  `PhysicsCollisionDetection.cpp:80-200`, and `PhysicsContinuousContact.cpp:70`) are bound to the IVP object;
  deleting the body nulls their object (`event_collision_object_deleted`, `:193-196`) and nothing re-arms them until
  the game's scripts physicalize the next ball (trafo / death / respawn). So after an automation beam the own ball's
  hit and roll sounds are silent until then (from the code; not listened to). No simulation effect (a listener only
  reads the contact and fires a behaviour output), the `beam` verb is automation-only (no human play path beams the
  own ball), and it is the same kind of "script bound to the ball's physics object" R6D lists for the live own
  births: noted there for R6D / R6M, not handled here.
- **Known residual BEAMF-S: the own ball's sounds after a beam (review round, the census).** What is bound to the
  ball's physics object rather than its entity, listed and not guessed: physics_RT has exactly two behaviours that
  install a listener on an IVP object (`PhysicsCollisionDetection.cpp`, `PhysicsContinuousContact.cpp`; nothing
  else calls `add_listener_collision`), and EG0's census (`SimTool --root <game> --level 11 --list-callback-blocks
  --dump-at 600`, `work/BEAMF/cb/l11_600.txt`: 144 blocks of the ten queueing prototypes in game, 28 outside the
  selection) names 14 that target the current ball (`Ball_Wood#42240`): Gameplay_Ingame's SetPhysicsForce 26354,
  26392, 26414, 26432, 26447 and Physics WakeUp 25094, 26418, 27403 (zero-force controllers on retail, whose
  navigation leaves are zeroed - rollback-engine-gaps 2.2's outside producers, DX3's recorded difference; a WakeUp
  only wakes, and the remake leaves no bodiless frame), and Sound_Manager's Physics Collision Detection 6271, 6471,
  6489, 6507 and Physics Continuous Contact 6619, 6675 - the only listeners on the ball, all in the sound script,
  and their callbacks only set the block's outputs (`PhysicsCollisionDetection.cpp:124-189`,
  `PhysicsContinuousContact.cpp:96-200`: no write into IVP). Why they go silent: deleting the old body calls
  `event_collision_object_deleted`, which unhooks the listener and nulls its object (`:190-197`) but leaves it in
  the block's local 0; the block's On input makes a new listener only while local 0 is null (`:267-287`), so it
  stays silent until Sound_Manager switches the block Off (`:289-299`, which deletes it) and On again for the next
  game-made ball (trafo, death, respawn). No crash path: a nulled listener gets no more callbacks and its
  destructor checks the object (`:99-101`, ContinuousContact `:83-84`). So the simulation is unaffected (the FINAL
  rows above), only the automation `beam` verb reaches it, and nothing of R6D's open design was needed for the
  fresh body itself (guard, group, OnUnphysicalize measured above). The options are the user's (put through the
  orchestrator's decision list, not chosen here): (a) leave it, the test verb only; (b) re-arm Sound_Manager's six
  blocks on a bridge-made own ball (an Off / On of the blocks, or a listener re-bind in the bridge), built with R6D
  item 1's survey and checked by R6B's "sound after a remake" live check. Queued as R6D item 1 / R6B (roadmap §4
  item 9).
- **Checks** (x64 BUILD-OK DEPS-OK, `build-agent-rBEAMF`; x86 CLIENT-BUILD-OK DEPS-OK, `#deps 46 (VALID)` on an
  unchanged includer of `client_core.hpp`; GCC `linux_test_syntax.sh` 33/33 ok; `BallanceMMOMessageTests` 727/727,
  the new `SessionClientCoreOwnLives.BeamRemadeAtItsStampWithTheRetailCapabilities`: retail's capabilities, the stamp
  is `make_own` inside the mute live at T+1 and again in a forced replay crossing T+1, no `write_body`):
  - wire check on core_ref_s13 (`EXTRA=--replay-free-cost-clock wire_check.sh - rBEAMF rBEAMFw rSRT4dw_new`):
    45/45 sends SAME, 45/45 journals IDENTICAL (`fan_dr` and `h123_150` beam the headless own ball: the headless beam
    path is unchanged); HL run 16's three arms (`hl_full/r16_l11_cross`, `r16_swing_s0`, `r16_swing_s1`) replayed by
    rBEAMF (free clock): 12/12 journals IDENTICAL, 12/12 end markers.
  - retail L11 `srt_ctl` (SRT2's recipe: `work/BEAMF/beamf_retail.py`, RT1's arm '0:0 0:0', client 1 beams at
    300 / 520 / 750 / 1100, client 2 at 340 / 600; each game on E-cores of its own, 16-21 / 22-27; the Arch server of
    this tree, snapshots off), two sessions: `beamf_r1` (with OWNB's `BMMO_HULL_PROBE=Ball`, one probe file per copy)
    and `beamf_r2` (no probe). Both: resyncs 0 / 0, rebases 0 / 0, starved 0, the server journal's replay 2576/2576
    and 2583/2583; every beam's "own body ... made for tick N (made)" at its stamp.

    | run | rb / resim (c1, c2) | t_rest | t_rep | t_max | t_cap total / max (ms), c1 and c2 |
    |---|---|---|---|---|---|
    | srt1_ctl (SRT1, in-place beam; c1 resynced once) | - | 223 / 406 | 1011 / 656 | 261 / 122 | 6922 / 28, 8878 / 23 |
    | srt2_proto (SRT2's option (a) prototype) | 33/79, 57/144 | 67 / 112 | 174 / 293 | 18 / 12 | 4268 / 12, 4123 / 4 |
    | beamf_r1 (probe on) | 31/67, 48/132 | 62 / 95 | 129 / 245 | 13 / 23 | 4043 / 7, 3995 / 3 |
    | beamf_r2 | 30/71, 50/128 | 60 / 100 | 115 / 202 | 10 / 11 | 4099 / 4, 4032 / 3 |

    No blob window: the largest live capture is 3-7 ms (srt1_ctl: 23-28, the window), the largest rewind 10-23 ms
    (122-261). The checker counts and blob sizes per capture were not re-measured (SRT2's diag patch no longer applies
    after SRT4's capture change and was not ported); the window's cause is measured directly instead, below.
  - The own ball's collision bookkeeping against the other worlds (`work/BEAMF/probe_ov.py`, `probe_cmp.py`; the
    server's from a SimTool `--replay-session` of `beamf_r1`'s server journal with the same probe): at every beam the
    owner's fresh body, the server's clone and the other client's mirror have the same overlap element - centre the
    beam pose, radius **4.0000 m** in all three (SRT2: 1255.5 m in the owner's world only) - and the owner's probe
    rows (hull gradient and values, hull entries, overlap element, core speed and position) are byte-identical to the
    server clone's at every probed PSI after each of the six beams (8 PSIs at 520 / 600 / 1100, 4 at the trafo beams
    300 / 340 / 750 until the trafo takes the ball). The other client's mirror equals both except its live rows at
    the trafo tick (the mirror predicted on before the owner's trafo Unphysicalize arrived; its FINAL rows are exact).
  - FINAL rows against the server (`work/RT1/rt1_judge.py`, `work/BEAMF/rowdiff.py`), both runs: mechanisms 23/23
    bit-equal; the own ball's beam rows **exact** - SRT2's / RTR's 0.001946 m (lin 0.064, ang 0.081) at the trafo
    beams 300 / 340 and srt2_proto's 0.00114 m at 750 are 0 m with equal velocities; every differing own, mirror and
    piece row is rotation-only, at most 2.98e-8 (own / mirror) and 4.2e-8 (pieces), at the same ticks as srt2_proto's
    mirror rows (298, 906-962, 1436-1524, 1974-2062, 2332-2390: RTR's known signature, not a beam's).
- **Deferred (the plan section's two retail sessions are spent):** EG0's arena beam (`work/EG0/eg0_retail.py`, 8.6 mm
  / 0.879 m/s at 1300; needs the slot-13 maphack builds), FANX's paper-plate beam (1.7 cm) and RTR's stall recipe
  (`rtr_retail.py stall`, 0.981 m at 598) were not re-run; each is an in-place-beam residual of the mechanism this
  step removes and is expected at 0, to be confirmed in a later retail batch (R6B's list carries all three). A retail
  replay that crosses an own beam's stamp was not reached live (both runs' rewinds are 2-3 ticks deep and none
  anchors before an own stamp): the replayed stamp is covered by the unit test and by the headless arms, whose core
  and restore path are the same; a forced-rollback arm (`rt1_retail.py arm LABEL E:D 0:0`) would reach it on retail.

## As built (step SRT3, a1: one rewind per frame)

- **Code** (shared core only; no protocol, journal, engine or server change): `client_core_frame.cpp` `pre_step` drains
  with `s.life_batch_.collecting` set and calls `flush_life_rewinds()` right after the drain, where the last of the
  sequential rewinds used to run (before the own echoes, an exact resync's apply, the Sector rewind and the input
  replay). `client_core_lifecycle.cpp`: `rewind_for_lifecycle` returns whether the event joined the batch
  (`joins_life_batch`: collecting, late, past a pending exact resync's anchor, its anchor recorded and inside the
  standard reach - exactly the two refusals `rollback_engine::rewind_to` makes with no reach, so a joined event can
  never be refused by the batch's rewind, and a deep one is refused alone as before); the batch keeps the lowest
  anchor, the reasons, the players, and the "the replay did not reach it" fallbacks of `make_remote_birth` /
  `remove_remote` (`checks`), which `flush_life_rewinds` runs after its one rewind (a frozen world only). Counters:
  `life=` counts real rewinds; the new status section ` coalesced: life=N input=M` (only when non-zero) counts the
  events that rode along and the input replays carried.
- **Where the batch rewinds early** (`settle_life_batch_before`): before an Unphysicalize of a player that has an
  event in the batch (it reads the mirror's body and entity, which that event's replay changes - a P then U of one
  player in one drain), and before any lifecycle event that will not join (applied live or refused between rewinds
  that ran before and after it). A Physicalize that joins reads nothing a replay changes (queued for the replay), so
  a beam pair's U and P and every other player's events stay in one rewind - the measured L12 tick-521 frame (three
  beam pairs, six rewinds) is one.
- **The input replay carried** (`client_core_held.cpp` `drop_carried_input_replay`, beside DX's
  `drop_carried_own_replay`): an input correction pending at the flush whose earliest tick the lifecycle anchor
  subsumes (`replay_subsumes`) was replayed by that rewind already - the relayed frame is in its ring from arrival,
  an own substituted frame amended in its record - so it is dropped, counted as carried (`window_inrew_src_` still
  names its source). Not merged (each keeps its rewind, as before): a late Sector set (its re-derivation starts at
  its own anchor; after the lifecycle replay, as before), an own echo's moved birth (`move_own_birth`), an exact
  resync's apply, and an input correction older than the lifecycle anchor. In the measured sets no frame held two.
- **Adaptations.** (1) The life log's remote Unphysicalize entry gets its generation at the apply, not after its own
  replay (the field is read for own lives only). (2) **The pass rule's replays need `--replay-free-cost-clock`**
  (B5RC's flag): a capture records each rewind's three cost clock reads (`rewind_start`, `rewind_replay`,
  `rewind_end`) in order, so a strict replay by a coalescing build diverges at the first coalesced frame by
  construction ("expected a clock read at journal_ms, the capture has one at rewind_start"; every core_ref_s13 arm
  at the start pile, srt_E_l12x_on2 at 230). Those reads decide only the reach's cap; the journals below are
  IDENTICAL with them free, so no cap decision moved. (3) `.local-tools/wire_check.sh`'s coverage test accepts a
  free-clock replay's last line when the client log shows the capture's end marker. **From SRT3 on, core_ref_s13
  (and every older set, and any capture of a pre-SRT3 binary) is replayed with `EXTRA=--replay-free-cost-clock` on
  both sides** when OLD and NEW are both SRT3 or later (`NEW_EXTRA=` alone when OLD predates SRT3): a strict replay
  is COVERAGE-SHORT on both sides by construction (`wire_check.sh`'s header says so). Until a later REF step
  re-captures the set with an SRT3-or-later binary (queued: core_ref_s14), these replays do not cross-check the
  cost-based reach cap.
- **Review fix (gate round).** The batch's early flush (`settle_life_batch_before`) runs at the top of `apply_event`,
  before the event is journalled and logged (`log_life`), not inside the Unphysicalize branch after it: flushed after
  `log_life`, the rewind's reconcile (`reconcile_remote_lives`) would find the not-yet-applied death (no entity yet)
  in its player's window and queue it as a reconciled death - a window [P0, U1, P1, U2] with U2 flushing - so the
  replay would remove the mirror, `note_life_skipped` would then mark U2 skipped and a second rewind follow, leaving
  a life log the sequential path never wrote. The sequential path ran the earlier events' rewinds before the event
  was logged; the flush now does too. The calls in `remove_remote` / `make_remote_birth` stay (no-ops then; the
  overtaken birth's `remove_remote` still flushes the birth that just joined, as its own rewind ran then). Not
  exercised by the measured sets (their journals were IDENTICAL before the fix). A carried input correction does
  not count in `resync_window_rewinds_`: that counter counts input rewinds that ran past the standard reach, and a
  carried one runs none.
- **Checks** (x64 BUILD-OK, `build-agent-rSRT3`; x86 CLIENT-BUILD-OK, `#deps` VALID; GCC `linux_test_syntax.sh` 33/33
  ok; `BallanceMMOMessageTests` 724/724):
  - `cpu_full/srt_E_l12x_on2` replayed by rSRT3 (free clock): 4/4 journals **IDENTICAL** to the live ones (rSRT2,
    same flag: 4/4 IDENTICAL). Client 0: rollbacks 25 -> 15, one "6 late events taken by one rewind to tick 500"
    (`rollback to tick 500 (...6 reasons...), re-simulated 21 ticks`), `coalesced: life=8 input=2`.
  - HL run 16's three arms (`hl_full/r16_l11_cross`, `r16_swing_s0`, `r16_swing_s1`, 10 % loss): 12/12 journals
    IDENTICAL to the live ones; rollbacks per client 13-19 -> 8-13 (l11), 38-64 -> 29-55 (swing); 5-7 events
    coalesced and 2-7 input replays carried per swing client.
  - The wire check on core_ref_s13 (`NEW_EXTRA=--replay-free-cost-clock wire_check.sh - rSRT3 rSRT3wf rSRT3w_old`):
    44/44 sends SAME, 45/45 journals IDENTICAL (fan_dr's rejoin included), 44/44 replays to the end marker. A strict
    run first: every arm COVERAGE-SHORT at its first coalesced frame (above). solo1 / solo2 strict: IDENTICAL.
  - **The frame** (the census tools: `srt_diag.patch` applied to this tree for `build-agent-rSRT3d`, reverted and
    rebuilt; `work/SRT3/e_pair.sh`: the four `srt_E_l12x_on2` captures at once, each pinned to E-cores of its own,
    free clock; `SRT/census.py`), same host, one binary at a time, SRT's diag build `rSRTd` as before:

    | | rewind frames (with > 1 rewind) | rewinds | replayed ticks | max rewind frame per client (ms) | max one-rewind frame (ms) | restore per rewind |
    |---|---|---|---|---|---|---|
    | before (rSRTd) | 54 (21) | 88 | 1958 | 301.5 / 197.3 / 204.4 / 195.7 (tick 521, 4-6 rewinds, d 84-126) | 66.9 / 70.5 / 76.6 / 66.6 | 3.44 ms |
    | after (rSRT3d) | 54 (**0**) | 54 | 1237 | **75.9 / 77.7 / 79.8 / 74.9** (one rewind, d 21-22) | the same | 2.55 ms |

    Every rewind frame holds one rewind, so the max frame of a rewind is one rewind's time (the pass clause). Per
    replayed tick 1.93 -> 2.08 ms (capture 1.37 -> 1.42; SRT4's cut). The largest frames of the replay are not
    rewinds: tick 230 (82-136 ms wall with 9-10 ms of thread CPU in the first pair: the thread waiting, "host" in
    the census; the old build 55-58 ms wall with 9-12 ms CPU). A second pair (`old2` / `new2`) has 9-10 ms wall
    there on every client of both builds, with the max rewind frame 73.0-78.2 ms (new) and 195.8-299.5 ms (old)
    again: noise of four processes starting at once, not session work.
- **Not run (deferred to SRTJ, which measures live E-core sessions and retail anyway):** a live E-core headless
  session (the Arch server would first need rebuilding from HEAD; the replays above run the same client code over
  the captured inbound stream, deterministically); the retail two-copy session the plan section's header names (its
  pass list asks an x86 compile, done).

## As built (step SRT4, a2: sparse capture of replayed ticks)

- **Code** (shared engine and core only; no protocol, journal format, engine-change or server change; the legacy
  retail path untouched). `rollback.hpp`: `rollback_engine::set_sparse_capture(k, verify)` (engine default off: the
  SimTool and the unit tests' engines record as before); a replay records tick `t` **whole** when `k <= 1`, `t` is a
  multiple of `k`, `t` is the replay's last tick, the record of `t` had snapshot rows, there was no record of `t` or
  `t - 1`, or `t`'s tracked set, lifetimes or body set differ from the record of `t - 1` (a birth record: its extras
  are what a later restore re-poses the body with); otherwise **light** (`capture_replayed`, `capture(.., whole)`,
  `tick_state::full`). A light record keeps the bodies (one `get_body` per tracked name), navigation, clock, globals,
  inputs, tracked set, rows and unborn marks, and drops the extras, contacts, scripts and blob. Live records are
  always whole (`record` / `amend_record` unchanged; the deferred-capture path `awaiting_capture_` needed nothing).
  `rewind_to` restores the newest whole record at or before a light anchor (`restore_base`) and replays the
  difference - every hook of a replayed tick reads the core's logs by tick (lifecycle queues, trafo inputs, relayed
  rings, recorded sector passes), so the ticks before the anchor replay as recorded; the reach and history checks
  stay the requested anchor's, and the replay may run up to `k - 1` ticks deeper. RC2 (b)'s cache and OPC's script
  reuse take only a whole record of `t - 1`. `on_snapshot` and `resync` refuse a light record (never reached: below).
  Core: `core_config::sparse_capture` (8) / `sparse_capture_verify`, set at SessionStart only when `snapshots_run()`
  is false (a snapshot's rows land on the record of their tick); `rewind_for_sectors` starts the union's
  re-derivation at the restored base + 1 (`union_records_` of the base); status section ` sparse=k[v]
  light=records/ms whole=records/ms rebased=rewinds+extra ticks unanch=N verify=compared/differing/float/stale`
  (only while on; `whole` = the replays' whole records, added by the review's fix-up).
  Headless `--sparse-capture N`, `--sparse-capture-verify`.
- **Adaptations.** (1) **FINAL rows stay on every tick**: a light record keeps its bodies (FINAL rows, `recorded_body*`,
  the window's body sets and the birth test need them), so no journal-format note is needed - the design expected
  FINAL rows only where captured. (2) **k = 8 is also the ceiling** (`kSparseCaptureMax = kReachHistoryTicks -
  kReachCapTicks`): the deepest standard anchor (120) then still has a whole record in the deepest history (128). A
  rewind whose base has left the history (only a correction inside an exact resync's re-step window, whose reach is
  the whole history - `window_rewind_reach`, step A3 - or a forced rewind deeper than the standard reach can ask for
  one) is refused by the engine as unmatched (`sparse_unanchored`, 0 in every run below). The core's reach checks
  therefore ask `rollback_engine::rewindable(anchor)` (the record and a whole base before it) instead of
  `knows_tick` - the held own notice (`client_core_held.cpp`), the late relayed frame (`client_core_messages.cpp`),
  the trafo input (`client_core_trafo.cpp`) and the sector rewind (`sector_rewind_reachable`) - so such a correction
  takes the fallback its check already has (a late own notice counted `subst_late`, a late relayed frame
  `relay_late`, a trafo input `trafo_missed` and a resync, a sector move past the reach a resync) instead of running
  into the refusal (review finding 2). `replay_corrected_inputs` has no pre-check: a refused remote-only correction
  there is counted `inrew_missed` and, with snapshots off, its error stays, as for any correction out of the history
  before this step; only an anchor 121-127 ticks deep inside a re-step window can meet it. (3) **No materialize for a resync**: S is always whole - `anchor_resync` records it after the re-assign
  drops the history, as the first record of the new history, and a replay re-records only the ticks after its anchor
  - so the engine only refuses a light S defensively. (4) **Verify mode** (capture anyway; each rebased rewind
  compares its re-simulated anchor with the record a k = 1 engine would have restored): differences in float script
  parameters only are counted apart (`float`, as the replay's own script check does; all of them `PE_Balloon Script /
  TT Set Dynamic Position: local 1 (Old Position)` - the balloon, exception c), and an anchor up to which a tick was
  replayed with other inputs or lifetimes than its record had is not compared (`stale`: a relayed frame differing
  from the prediction whose own, older input rewind is still to run - SRT3 keeps an input correction older than a
  lifecycle anchor apart; first seen as h123_150 tick 602 / h1_150 652, a newborn mirror's first relayed frame,
  dv 0.184 m/s). In those stale cases the rebased replay applies the pending older correction before its own rewind
  does, which the dense path does not: the intermediate state after the lifecycle rewind differs from the dense one,
  and the two paths converge only because the pending correction's own rewind (or, below, its drop) leaves the world
  where the dense path's later rewind does - the final records agree (journals IDENTICAL), the in-frame state between
  the two rewinds is not compared. (5) **A frame's lifecycle rewind drops the pending replays from its restored base**
  (`rewind_to`'s `restored`, `rewind_world`'s, `flush_life_rewinds`): a rebased replay read every correction after
  the base (the rings by tick, the amended own records), so a pending input or own replay from a tick in (base,
  anchor] is carried too instead of running a second time (review finding 4). (6) **Not in the control arms**: k = 1
  with `body_set_corrections` (its full snapshots still land on the records of their ticks, review finding 3) and in
  a headless run with `--dump-savestate` / `BMMO_DUMP_FINAL_LAG` / `--compare-scripts` unless `--sparse-capture` is
  given (they read every record's blob and scripts, review finding 5). (7) `capture_replayed` captures a tick twice
  when its body set changed without a lifetime change (light first, then whole): time only, rare, left (review
  finding 6).
- **k by measurement** (the stored `cpu_full/srt_E_l12x_on2` captures, the four clients pinned to E-cores of their
  own, `--replay-free-cost-clock`, one binary at a time, `work/SRT4/e_run.sh` + `cost.py`; journals IDENTICAL to the
  live ones in every arm): per replayed tick (stats, t_rep / resim, the headless-only blob diff included) and the
  reach meter's replay ms per tick (B5RX: what a retail client would measure):

    | k | rewinds | replayed ticks (rebased + extra) | per replayed tick | meter per tick | per rewind (replay + restore) | light capture |
    |---|---|---|---|---|---|---|
    | 1 (twice) | 54 | 1237 | 1.86 / 1.89 ms | 1.58-1.72 ms | 43 + 2.3 ms | - |
    | 2 | 54 | 1242 (5 + 5) | 1.70 ms | 1.41-1.51 ms | 39 + 2.4 ms | 0.19 ms |
    | 4 | 54 | 1246 (7 + 9) | 1.42 ms | 1.11-1.19 ms | 33 + 2.4 ms | 0.19 ms |
    | 8 | 54 | 1257 (12 + 20) | **1.19 ms** | **0.89-0.95 ms** | **28 + 2.4 ms** | 0.19 ms |

  Each step of k pays; the extra ticks of a rebased rewind (20 of 1257 at k = 8) cost less than the captures saved,
  so k = 8, the ceiling of (2), is the default. Retail L12 on E-cores (`scripts/retail/srt4_l12`, RT2's SP8-e recipe
  through `work/RTR/rtr_retail.py l12`, net lag 150 on both, games on 16-21 / 22-27, snapshots off, the Arch server
  rebuilt from this tree): **0.60 / 0.57 ms** a replayed tick (t_rep 42 / 40 ms over 70 ticks, meter 0.592 / 0.566;
  before, SRT's `srt_l12`: 1.39 / 1.39), 3 rewinds per client, 1 rebased (+1 tick), light 59 records at 0.09 ms,
  resyncs 0 / 0, rebases 0 / 0; `worst_bodies.py` server vs each client (balloon excluded) printed its header only,
  which means no tick with a comparable body row (it prints every compared body, an exact one as `never`), so the
  retail run shows no FINAL-row exactness either way - it is a smoke test; the reach
  cap rose from the floor 48 to 73 / 76 (the meter's cheaper replay; history 128, inside (2)).
- **The pass rule's per-tick figures are not met: 0.6 ms headless L12 (measured 1.19, meter 0.89-0.95) and 0.4 ms
  retail L12 (measured 0.57-0.60).** Cause, measured on this build (the review's fix-up added `replay_whole` /
  `replay_whole_us`, the status section's `whole=records/ms`; `srt_E_l12x_on2` replayed again at k = 8 on E-cores,
  `work/SRT4/e/srt_E_l12x_on2/k8g`, journals IDENTICAL): t_rep 1625 ms over 1257 replayed ticks = 1.29 ms a tick
  (this run; 1.19 the earlier one - the scatter of a timed replay), of which the light records 189 ms (1019 records,
  0.19 ms each; 0.15 ms a replayed tick), the whole records 634 ms (238 records - the last tick of every rewind, the
  multiples of k, the births - **2.66 ms each**; 0.50 ms a replayed tick) and the rest of the replayed tick, which is
  not capture (the step, the scripts, inputs, births, the tracked set, the hooks), 802 ms = **0.64 ms a replayed
  tick**. So (i) the headless 0.6 ms cannot be met by any capture cut: the non-capture remainder alone is above it
  (it is pre-existing: the same code at k = 1); (ii) a whole record costs 2.66 ms here against about 1.2 ms of
  capture in a k = 1 replayed tick (1.86-1.89 less the 0.64 remainder); the probable cause, not separated by a
  measurement, is that the record of t - 1 is light for every whole record but a birth's, so neither RC2 (b)'s
  capture cache nor OPC's script reuse has a source (`capture`: `it->full`) and the fifth of the ticks that stays
  whole pays the uncached price - this step's own residual; (iii) the
  retail split was not re-measured on this build (the one retail run preceded the counter); SRT's census put the
  retail non-capture remainder at about 0.28 ms, a light record there costs 0.09 ms. Options (not chosen here): (a) take the figures as measured and let
  SRTJ judge the hard gates (T2' / T3 / T4) on them; (b) a k above 8 by deepening the history (kReachHistoryTicks =
  120 + k; at k = 16 an estimated 0.48 ms retail, more memory and up to 15 extra ticks per rebased rewind); (c) a
  step on the replayed tick's non-capture bookkeeping (0.64 ms headless measured above; its own split needs SRT's
  diag patch re-applied); (d) SRT2B's per-body read cut lowers the whole captures that remain; (e) keep in a light
  record what the cache and the script reuse need of t - 1 (the extras of the sleeping bodies, the scripts), so the
  whole records after it are cached again - a costlier light record against cheaper whole ones, not measured.
- **Checks** (x64 BUILD-OK, `build-agent-rSRT4c`; x86 CLIENT-BUILD-OK, DEPS-OK; GCC `linux_test_syntax.sh` 33/33
  ok; `BallanceMMOMessageTests` 726/726, two new: `SparseCaptureRecordsEveryKthReplayedTickWholeAndRebasesALightAnchor`,
  `SparseCaptureRefusesALightAnchorWithNoWholeRecordBeforeIt`):
  - The wire check on core_ref_s13 (`EXTRA=--replay-free-cost-clock`, rSRT3 vs rSRT4, then rSRT4c in "-" mode over
    the same old traces): 45/45 sends SAME, 45/45 journals IDENTICAL; no arm's journal changes. Sparse capture ran in
    every arm (e.g. swing_snapoff c2: 1025 light records, 21 rebased rewinds + 76 ticks; resync_snapoff c1: 959 /
    16 + 63), 0 unanchored.
  - Verify mode: core_ref_s13 at k = 8 403 anchors compared, **0 mismatches** (179 float / balloon, 10 stale); at
    k = 2 224 compared, 0 mismatches (101 float, 2 stale); HL run 16's three arms (`hl_full/r16_*`) 154 compared, 0
    mismatches (22 float); `srt_E_l12x_on2` 12 compared, 0. Every verify replay's journals IDENTICAL.
  - HL run 16's three arms replayed by rSRT4c (k = 8, free clock): 12/12 journals IDENTICAL to the live ones.
  - SimTool `--savestate-check` (OPC's RK2 recipe, K = 2 seed 1 x 40): PASS 40/40 on rSRT3 and rSRT4c (the SimTool's
    engines keep k = 1, so it does not reach the sparse path).
  - The review's fix-up (findings 2-5, the whole-record counter; `build-agent-rSRT4d`): x64 BUILD-OK DEPS-OK, x86
    CLIENT-BUILD-OK DEPS-OK, GCC 33/33 ok, 726/726 tests (the refusal test also asks `rewindable` and `restored`), the
    wire check on core_ref_s13 against rSRT3's traces (`EXTRA=--replay-free-cost-clock`, "-" mode, `rSRT4w_old`):
    44/44 sends SAME, 45/45 journals IDENTICAL, swing_snapoff c2's sparse figures unchanged (1025 light, 21 rebased
    + 76).

## As built (step SRT2B, a3's remainder: the moving trafo pile's per-body reads)

- **Which cut, and why: the pile's reads batched (the capture's entity lookup scope on retail), not one combined
  read per piece.** Each of a moving piece's three bridge reads (`get_body_state`, `get_core_extra`, `contacts_get`;
  `rollback.hpp` `capture()`'s `grab`) resolves the piece's name with `find_entity` (`physics_state.cpp`), which
  without a scope is CK's `GetObjectByNameAndParentClass` - a scan of the context's objects; the fills themselves
  are a few field copies and a walk of the body's friction synapses. The headless client has served those lookups
  from one pass over the 3D entities since TR3 (`entity_lookup_scope_begin` / `_end`, installed as
  `rollback_world::capture_scope` by `rollback_binding.hpp` for a backend with `lookup_scope`), but the retail
  backend (`physics_view`, the plugin's C API) had no such entry, so a retail capture scanned the context about
  110 times (the verify run below: 226 416 served lookups in 2048 captures). A combined read would have cut those
  scans to one per body (a third); the scope cuts them to one table built per capture and serves every other lookup
  of the capture with it (the own ball's, the mirrors', the navigation replicas'), leaves `capture()` and the RC2 (b)
  cache exactly as they are, and is the code the headless client and every wire check already run. A combined read
  was not built: the measured remainder (below) does not call for it.
- **Code** (no protocol, journal, record, IVP or physics_RT change; bridge API v31). `physics_rt_api.h`:
  `lookup_scope(manager, begin, flags, totals)` appended to `bmmo_physics_api_v2`, `bmmo_physics_lookup_totals`,
  `BMMO_LOOKUP_SCOPE_VERIFY`; `physics_rt_bridge.cpp` `api_lookup_scope`. `physics_state.cpp`: the scope takes a
  `verify` flag at its outermost begin - every lookup the table serves also scans, the scan's answer is returned,
  a difference is counted - and keeps running totals per thread (`entity_lookup_totals()`: scopes, built, served,
  verified, mismatches); without verify `find_entity` is unchanged. `physics_view::lookup_scope(on, verify,
  totals)` (stateless: no member, the BML layout rule; a bridge without the entry leaves every lookup a scan) and
  `physics_direct::lookup_scope` with the same signature. `rollback_binding.hpp`: `rollback_binding_options::
  capture_lookup_scope` (default on) and `capture_lookup_verify`; in verify the hook logs each new mismatch (at
  most 16 lines) and a summary every 1024 scopes ("lookup scope (verify): ..."). The session core turns verify on
  with its capture cache's verify mode (`core_world_hooks::capture_verify`, set from `core_config::capture_cache`
  in `client_core.cpp`; `bind_session_world` passes it): retail 'session cache verify', headless `--capture-cache
  verify`. The legacy retail path (`physics_session_client.cpp`) sets `capture_lookup_scope = false` and so keeps
  scanning: nothing new for it. Unit test `RollbackBinding.CaptureLookupScopeInstalledOffAndVerified`.
- **Exact by construction, verified.** The table holds, for each name, the first 3D entity in the order
  `GetObjectListByType(CKCID_3DENTITY, TRUE)` lists them, which is the scan's answer on Ballanced's CK2 (TR3's own
  live check against the scan, `docs/engine-changes.md` T4's notes: 1.1-1.5 M lookups per client, 0 different). On
  the retail CK2.dll it was not yet shown: the verify run (`srt2b_verify`, both copies 'session cache verify')
  checked every served lookup of the whole session against the scan - **0 mismatches** on both clients (a new
  mismatch logs at once, at most 16 lines, and none was logged; the summary at 2048 scopes: 226 416 / 228 082
  lookups verified, 2042 of the 2048 scopes built a table), and RC2 (b)'s cache 0 mismatches (55 785 / 55 696 hits).
- **The statistic, before / after** (SRT2's recipe with the SRT census, `work/SRT2B/srt2b_retail.py LABEL [verify]`:
  RT1's L11 ctl arm '0:0 0:0', client 1 beams at 300 / 520 / 750 / 1100, client 2 at 340 / 600, each game on
  E-cores of its own, 16-21 / 22-27, the Arch server of HEAD 230042c6, snapshots off; the census build = the
  `BallanceMMOClient/*` hunks of `work/SRT/srt_diag.patch`, whose `d_cap_us` is the in-tree `capture_us` per frame;
  `work/SRT2/livecap.py DIR 450 1250`: live frames, no rewind in the frame):

  | run | client | capture p50 / p99 / max (ms), 450-1250 | frames over 2 ms | whole session p50 / p99 / max | t_cap total / max |
  |---|---|---|---|---|---|
  | srt2b_before (HEAD + census) | c1 | 2.15 / 2.96 / 4.15 | 517 / 789 | 1.54 / 2.75 / 4.15 | 4159 / 4 |
  | | c2 | 2.08 / 2.69 / 2.96 | 451 / 780 | 1.49 / 2.59 / 2.96 | 3990 / 3 |
  | srt2b_after (SRT2B + census) | c1 | 1.54 / 2.04 / 2.65 | 8 / 789 | 1.37 / 1.89 / 3.04 | 3505 / 3 |
  | | c2 | 1.53 / 1.89 / 2.32 | 3 / 782 | 1.35 / 1.79 / 2.32 | 3456 / 2 |

  A cut of 0.55-0.61 ms at p50 and 0.80-0.92 ms at p99 through the pile. SRT2's split (scripts about 0.7, blob
  about 0.55 ms) leaves about 0.3 ms for the body reads now (an estimate from the totals, not a section profile:
  SRT2's section diag no longer applies). Both runs: resyncs 0 / 0, rebases 0 / 0, starved 0; rb / resim 31/72,
  47/129 (before) and 30/80, 49/128 (after). As the plan section says, the 2 ms is not gated here; SRTJ judges the
  hard gates with these captures.
- **FINAL rows against the server** (`work/RT1/rt1_judge.py`, all three runs): worst position 0 m in every class,
  mechanisms 23/23 bit-equal, every differing own / mirror / piece row rotation-only at the ticks of RTR's known
  signature (298, the pieces from 460), the same before and after (pieces bit-equal 21/49 in both, 19/49 in the
  verify run).
- **Checks.** x64 BUILD-OK DEPS-OK (`build-agent-rSRT2B`), x86 CLIENT-BUILD-OK DEPS-OK; `BallanceMMOMessageTests`
  728/728; GCC `linux_test_syntax.sh` 33/33 ok; the wire check on core_ref_s13 (`EXTRA=--replay-free-cost-clock
  wire_check.sh - rSRT2B rSRT2Bw rBEAMFw_new`): 45/45 sends SAME, 45/45 journals IDENTICAL (the headless path had
  the scope already; verify off leaves `find_entity` as it was); SimTool `--savestate-check` (OPC's RK2 recipe,
  K = 2 seed 1 x 40, rSRT2B): PASS 40/40, every summary line equal to rSRT4c's but the timing line; HL run 16's three
  arms (`hl_full/r16_l11_cross`, `r16_swing_s0`, `r16_swing_s1`) replayed by rSRT2B (`work/SRT2B/replay_set.sh`,
  free clock): 12/12 journals IDENTICAL to the live ones.
- **Adaptation:** three retail sessions instead of the section's 1-2 (before, after, and a separate verify session,
  since verify scans every lookup again and would have spoiled the after figure). Disk: D: was full at the slim
  copy; nine build copies were removed with `clean.sh`. Six of them ARE cited by design notes as the binaries
  behind a measurement (the review's correction; the earlier text called them uncited, which was wrong):
  rB2SWb (rollback-engine-gaps 1945/1954, resync-and-sector-timing 3002/3081, record-format-gaps 902) and rB2SWc
  (session-client-core 6544) - dirty intermediates of step B2SW (commits 1c0e2d29..0d078b5d), not rebuildable
  exactly; rDX3b, rDX3c, rDX3d (rollback-engine-gaps 3613-3741) - dirty gate-round intermediates of step DX3,
  the nearest commit is da5ba844 (the final tree, not any of the three); rBALA2 (rewind-cost 2463-2950) - step
  BALA's gate-round build, rebuildable from cf7bcaed if that commit is the gate tree. Those figures stay as
  recorded, their binaries are gone. The other three (rD0g, rDEPg, rH1Xg; gate builds of 9286cad2, d5010dfc,
  eed4d158) are cited by no note.

## Judged (step SRTJ: the target after the cost cuts, for the user's f2 decision)

The user's Q2 answer (2026-10-31), verbatim: "先a看看" - measure what the cost cuts reach, then decide on f2. The
target (SRT1's section, set by the orchestrator under Q1 "你决定"): **hard** T2' no two consecutive renders skipped
because of session work, T3 the own ball's key-to-screen time never above two ticks (30.3 ms) because of session
work, T4 no rebase or resync caused by a slip; T1 (a tick's work + its render <= 15.15 ms, every frame) reported.
Judged on HEAD 606d37f6 (SRT1, SRT2 / BEAMF, SRT3, SRT4, SRT2B built; no f2), the E-core layout, snapshots off, the
deployed defaults (trafo inputs, placement, union, session clock), the Arch server rebuilt from HEAD.

- **Method.** The census build of section 2 again (`srt_diag.patch` applied to HEAD: its three `rollback.hpp`
  hunks that no longer apply after SRT4 ported by `work/SRTJ/port_diag.py`, the whole diff kept as
  `work/SRTJ/srtj_diag.patch`; x64 `build-agent-rSRTJd`, x86 installed for the runs; reverted, both trees rebuilt
  clean and HEAD's x86 build re-installed in both copies). Headless (`work/SRTJ/srtj_axes.sh`, SRT's recipes):
  four clients, 150 ms, 40 s, each pinned to 3 E-cores of its own (16-18 / 19-21 / 22-24 / 25-27): L12
  `l12x_on2`, L11 `l11x` (HL's `work/H2/l11_args.txt` without the loss), the arena `h1_on`, and each again with
  **human-rate keys** (`*_h`): client N plays `work/SRTJ/rec/humanN.bmrc`, four 63 s windows of the applied keys of
  the user's trial round 1 (`work/TRIAL1/rounds/round1-holdlast`, server journal; `work/SRTJ/human_records.py`:
  the two players from their first press and the same two 4000 ticks later; 1.59 / 0.94 / 0.92 / 3.25 key changes a
  second, mean 1.7 - the trial's 1.3-1.5). Retail (`work/SRTJ/srtj_retail.py`, SRT's `srt_retail.py` under it:
  copies on E-cores 16-21 / 22-27, census CSVs, checkpoint ticks 1): `ctl` = RT1's L11 ctl arm (beams 300 / 520 /
  750 / 1100 and 340 / 600, `r0_keys.bmrc`) **with `net lag 150` on both** (the target's latency; the arm itself is
  0 ms), `ctl_h` the same with copy 1 / 2 playing `human0` / `human1`, `l12` = RT2's SP8-e (net lag 150, sector
  commands, no key record), `l12_h` the same with `human0` / `human1` armed. Judge: `work/SRTJ/srtj_judge.py`
  (T1 counts, T2' longest run of skipped renders, T3 `rcensus.key_to_present` - presses spread evenly through the
  session on a 0.25 ms grid, to the end of the render that first shows the step that read them, flip and scan-out
  not included -, T4, and the f2 classification below); `judge_retail.txt`, `judge_headless.txt`,
  `judge_retail_before.txt` (the same judge over SRT's / SRT1's / SRT2B's stored census sessions).
- **What "because of session work" leaves at the bottom.** With no frame over budget the pacing alone (a key waits
  for the next poll, then that frame's sleep and render) gives key-to-present p50 22.8, p99 31.9-32.0, max 45.6-57
  ms and 3.3 % of presses over 30.3 ms, 0-0.06 % over 45.45 (SRT2B's `srt2b_after`, HEAD's code, the ctl arm at
  0 ms, where the rewinds are 2-3 ticks deep and the worst frame 18.5-31.6 ms). So T3 is read on p99 / max / the
  share over 45.45 ms against that floor, not on the raw share over 30.3 ms. The floor was measured at 0 ms on L11
  ctl; the retail target sessions run at 150 ms (and L12), so it is the nearest measured floor, not one of the same
  setting: a session's p99 near 31-32 ms is read as "at the floor", not as an equal measurement.

### Retail (two copies; E-cores; snapshots off)

| session | client | T1 frames > 15.15 / 30.3 / 45.45 (share) | max frame | T2' longest skipped run (renders skipped) | T3 key-to-present p50 / p99 / max (ms), > 30.3 / > 45.45 | T4 resyncs / rebases | rewinds; per replayed tick; restore |
|---|---|---|---|---|---|---|---|
| **before** `srt_ctl` (SRT, 0 ms) | c1 / c2 | 72/9/5 (2.8 %), 35/10/8 | 96.9, 254.5 | 167 (218), 125 (149) | 23.5 / 2166 / 2556, 23.4 / 2094 / 2485; 11.7 / 8.1 %, 11.1 / 7.3 % | 0/0, 1/1 | 30, 58; 3.76, 3.79 ms; 7.5, 5.2 ms |
| before `srt_ctl2` (SRT, 0 ms) | c1 / c2 | 59/20/19, 88/16/12 | 277, 209 | 837 (840), 145 (285) | p99 12334, 1806; max 12725, 2197; 35.0 / 32.7 %, 14.6 / 11.0 % | 0/0, 1/0 (by the logs: the copy with the 837-frame run is client 2, the resync was client 1's) | 2.80-6.25 ms; 7.3-11.2 ms |
| before `srt1_ctl` (SRT1, 0 ms) | c1 / c2 | 163/21/13, 52/12/12 | 270, 206 | 1 (173), 1 (268) | p99 120, 128; max 419, 360; 11.4 / 3.8 %, 13.0 / 2.9 % | 1/0, 0/0 | - |
| before `srt_l12` (SRT, 150 ms) | c1 / c2 | 9/3/1, 10/3/1 | 50.0, 50.8 | 5 (7), 6 (8) | 22.8 / 31.6 / 112.8, 22.8 / 31.8 / 130.8; 3.2 / 0.50 %, 3.1 / 0.57 % | 0/0, 0/0 | 3, 3; 1.39 ms; 1.9 ms |
| after `srt2b_after` (HEAD, ctl at 0 ms; SRT2B's run) | c1 / c2 | 14/1/0, 8/0/0 | 31.6, 18.5 | 1 (1), 0 (0) | 22.8 / 32.0 / 57.0, 22.8 / 31.9 / 45.6; 3.4 / 0.06 %, 3.3 / 0.00 % | 0/0, 0/0 | 30, 49 (2-3 ticks deep) |
| **after `srtj_ctl`** (150 ms, scripted) | c1 / c2 | 35/6/0 (1.4 %), 43/9/0 (1.7 %) | 40.7, 42.7 | **1** (10), **1** (15) | 22.9 / 43.2 / 102.4, 22.9 / 46.7 / 87.5; 4.9 / 0.81 %, 5.4 / 1.13 % | **0/0, 0/0** | 31, 42; 0.75, 0.77 ms; 2.3 ms |
| **after `srtj_ctl_h`** (150 ms, human keys) | c1 / c2 | 92/10/0 (2.1 %), 109/22/0 (2.5 %) | 40.2, 40.0 | **1** (19), **1** (32) | 22.9 / 44.9 / 103.7, 22.9 / 48.3 / 104.3; 5.6 / 0.93 %, 6.1 / 1.37 % | **0/0, 0/0** | 83, 106 in 64 s; 0.69, 0.70 ms; 2.1, 2.3 ms |
| **after `srtj_l12`** (150 ms, scripted) | c1 / c2 | 9/0/0, 9/1/0 (0.6 %) | 29.8, 31.9 | **1** (1), **1** (1) | 22.8 / 31.1 / 64.0, 22.8 / 31.1 / 65.8; 2.9 / 0.11 %, 2.8 / 0.12 % | **0/0, 0/0** | 3, 3; 0.62 ms; 1.8 ms |
| **after `srtj_l12_h`** (150 ms, human keys) | c1 / c2 | 38/4/0 (2.4 %), 86/3/0 (5.5 %) | 37.4, 38.3 | **1** (8), **1** (5) | 22.9 / 46.5 / 78.4, 22.9 / 48.0 / 76.3; 5.9 / 1.13 %, 9.8 / 1.70 % | **0/0, 0/0** | 28, 72 in 23 s; 0.80, 0.69 ms; 1.8, 1.9 ms |

- The over-budget frames now are one rewind each (SRT3: no frame held two) of 21-26 replayed ticks at 0.62-0.80 ms
  plus a 1.5-5 ms restore, 25-43 ms in all, and, on L12, the **sector activations** (ticks 34, 231, 388-391,
  686-688: 14.6-18.8 ms of core work with no rewind and a normal 1.5-1.8 ms capture; 6 per client scripted, 10 / 14
  with human keys - a5, not addressed by any step: SRT measured 15.0-16.0 ms). The live frame without a rewind is
  3.3-3.8 ms p50 of work, 4.4-5.7 ms p99. No frame over 45.45 ms in any after session (before: 1-19 per client).
- **Per replayed tick 0.62-0.80 ms** (before 1.39 on L12, 2.80-6.25 on L11 ctl): of it capture 0.33-0.49 (SRT4's
  light records and the whole every 8th), scripts 0.07-0.08, the step 0.04-0.07, the rest 0.16-0.20.
- **Present-blocking (SRT1's open assumption): not reached.** The copies run `UnlockFrameRate 0` /
  `SetMaxFrameRate 240` (BML.cfg; SRT1's note named 1 / 1200, which is `Ballance-Agent`'s). One more ctl session
  with `SetMaxFrameRate 0` ("turn on VSync", BML's comment) on both copies (`srtj_ctl_vsync`, restored to 240 after)
  rendered exactly as before: render p50 0.63-0.64, p99 1.09-1.46 ms, the same T1 / T2' / T3 / T4 figures as
  `srtj_ctl` (35/4/0 and 46/8/0, longest skipped run 1, key p99 42.7 / 46.4, 0 resyncs / rebases). The display is
  60 Hz and the game windowed (800x600): the render time is unchanged (p50 0.64 ms), so the present did not appear
  to block (no present-wait time was measured), and whether a blocking present (a driver-forced VSync, a full-screen game) breaks
  f1's catch-up stays unmeasured; producing one needs a driver setting this step may not change (no system
  setting is changed, section 2).

### Headless (four clients pooled, E-cores, 150 ms)

| arm | frames | T1 frames > 15.15 / 30.3 / 45.45 (share) | max frame | longest run behind > 1 tick (frames) | T4 resyncs / rebases | rewinds; per replayed tick; restore | before (SRT, section 3.1) |
|---|---|---|---|---|---|---|---|
| L12 `l12x_on2` | 6382 | 57/45/29 (0.9 %) | 60.6 | 7 | 0 / 0 | 57 (none two in a frame); 1.37 ms; 2.9 ms | 54/54/44, max 294, 21 frames with 2-6 rewinds, 1.87 ms a tick |
| L11 `l11x` | 6911 | 43/23/13 (0.6 %) | 61.9 | 3 | 0 / 0 | 43; 1.25 ms; 2.6 ms | - (no SRT L11 arm) |
| arena `h1_on` | 8462 | 125/49/16 (1.5 %) | 80.8 | 13 | 0 / 0 | 144; 0.82 ms; 4.2 ms | 117/93/68, max 110 |
| L12 human `l12x_h` | 6703 | 405/300/20 (6.0 %) | 73.4 | 9 | 0 / 0 | 404 in 26 s; 1.03 ms; 3.1 ms | - |
| L11 human `l11x_h` | 7266 | 432/55/0 (5.9 %) | 41.3 | 4 | 0 / 0 | 432; 0.73 ms; 2.8 ms | - |
| arena human `h1_h` | 8206 | 288/0/0 (3.5 %) | 26.8 | 1 | 0 / 0 | 494; 0.44 ms; 2.0 ms | - |

Every harness VERDICT PASS, the server journal replayed N/N with 0 checkpoint mismatches, starved / far 0. Every
headless frame over 15.15 ms is a rewind frame (but one `post` frame in `l12x_h`). The headless frame is dearer than
retail's for the same rewind (the scripted L12 rewind frame 54-61 ms: replay 36-45 + post_step 11-14 ms; the
replayed tick's "rest" 0.54 ms includes the headless-only blob diff of the first 8 rewinds, section 3.1), so the
retail rows are the player-facing ones. Human-rate keys multiply the rewinds by 3-8 (4-5 a second per client with
three human remotes, section 3.5's estimate), and so T1's count; they do not deepen a rewind (d 21-30).

### Per criterion and level (after; met = on every client of every session of that level)

| criterion | L11 retail (ctl, 150 ms) | L12 retail (150 ms) | headless L11 / L12 / arena | before |
|---|---|---|---|---|
| **T2'** no two consecutive renders skipped | **met** (longest run 1, scripted and human) | **met** (1) | n/a (no render; behind > 1 tick for at most 3-13 frames in a row) | not met: runs of 125-837 frames (SRT); met since SRT1 |
| **T3** key-to-screen never above two ticks because of session work | **not met**: p99 43-48 ms, max 87-104 ms, 0.8-1.4 % of presses over 45.45 (floor 0-0.06 %) | **not met**: scripted p99 31.1 (= the floor) but max 64-66 ms, 0.11-0.12 % over 45.45; human p99 46.5-48.0, max 76-78, 1.1-1.7 % | n/a (no screen); frames over 30.3 ms (a key polled in one waits its length): 23-300 per arm | L11 p99 1.8-12.3 s, max 2.2-12.7 s (SRT); 120-128 / 360-419 ms (SRT1); L12 p99 31.6-31.8, max 113-131 ms |
| **T4** no rebase / resync from a slip | **met** (0 / 0) | **met** | **met** (0 / 0 on all 24 clients) | not met: 1 rebase + 2 resyncs in SRT's two E-core ctl runs, 1 resync in SRT1's |
| T1 (reported) every frame <= 15.15 ms | not met: 1.4-2.5 % of frames, max 40-43 ms | not met: 0.6 % scripted (activations + 3 rewinds), 2.4-5.5 % human, max 30-38 ms | not met: 0.6-6.0 %, max 27-81 ms | L11 1.4-6.3 %, max 96-277 ms; L12 0.6 %, max 50-51 ms |

**Not met: T1 and T3**, on every level, scripted and human. What the cost cuts reached: the worst retail frame
from 96-277 ms (L11) / 50 ms (L12) to 30-43 ms; no frame over 45.45 ms; T2' and T4 met; per replayed tick 2.2-8x
cheaper. What is left is the single rewind at 150 ms: d = 21-26 ticks x 0.62-0.80 ms + restore 1.5-5 ms + the live
frame ~3.7 ms + render ~0.6 ms = 25-43 ms, i.e. about two ticks - so a key pressed during one waits up to three
frames (the T3 tail), and a frame inside 15.15 ms needs the rewind itself under ~11 ms (15.15 - 3.7 - 0.6): **at
d = 24 that is <= ~0.37 ms per replayed tick including the restore's share, about half of today's**, or a cheaper
restore and live frame. On L12, also the sector activations (15-19 ms with no rewind: a5).

### What f2 would still buy (bounded)

f2 (section 4's row): a correction that does not touch the own ball waits, and is applied later from the same
anchor - deeper. Without a second world it cannot be split across frames, so it can only **batch**: several
deferred corrections share one deeper rewind.

- **Which frames it could touch** (`srtj_judge.py`: each over-budget rewind frame's reasons from the client log's
  `rollback to tick` lines; "own" = a reason naming the own ball or life; "near" = the corrected remote ball came
  within 8 m of the own ball between the anchor and the frame, from the client journal's rows - f2's conditions (1)
  / (2) force those; "far" = everything else, an UPPER bound, since a remote touching a mechanism the own ball rides
  is not seen): retail L11 ctl 26 / 34 of the 35 / 43 over-budget frames far, human 78 / 94 of 92 / 109, L12
  scripted 1 of 9 (the rest are activations and near rewinds), human 26 / 68 of 38 / 86. No "own" frame in any
  after session. What f2 can never defer: the max over-budget frame that is not "far" is 29.8-40.7 ms on every
  retail client - **T3's tail does not move with f2**; T1's max falls only where the max frame was itself far
  (2 of 8 clients, below) and then only to that kept maximum.
- **What batching does to them** (`work/SRTJ/f2_model.py`, physics-free, on the retail census: the far corrections
  deferred; a batch opened by the first, joined by every far correction within D ticks, applied at the deadline as
  one rewind of depth d0 + D at the client's measured restore and per-tick cost, plus the live frame and render;
  `f2_model.txt`): D = 10 ticks: ctl 34 -> 25 and 26 -> 20 rewinds of 30.5-34.7 ms, human 78 -> 56 and 94 -> 56 of
  27.9-34.3 ms, L12 human 26 -> 18 and 68 -> 46 of 28.2-37.0 ms; frames over 15.15 down 0-35 % (17-35 % where far
  corrections exist; L12 scripted 9 -> 9), frames over 30.3
  ms UP (ctl 9 -> 27, 6 -> 22; human 10 -> 22, 22 -> 35; L12 human 4 -> 18, 3 -> 25); the max rises on no client
  and falls on two whose max frame was a far correction (ctl client 2 42.7 -> 35.1, ctl_h client 2 40.0 -> 34.3 ms,
  -5.7 to -7.6 ms; L12 human client 1 37.4 -> 37.0), unchanged on the rest. D = 25: over 15.15 down 0-54 % (34-54 %
  where far corrections exist), every batch 36.5-48.9 ms, the max up by 3.4-11.5 ms on every client (L12 human
  37.4 -> 48.9), so frames over 45.45 come back. The model leaves out the forced applies of condition (2) and the re-evaluation
  replays, so it is the most f2 could reach.
- So on the frames left over **f2 trades fewer over-budget frames for deeper ones**: it cannot bring a single frame
  inside 15.15 ms (a deferred correction is still one whole rewind in one frame), it does not touch the non-far
  29.8-40.7 ms frames that set T3's tail, and with a deferral long enough to batch it makes the remaining frames
  longer (over 30.3 and 45.45 ms) - the opposite of T3. Its gain is T1's count (0 to -54 %), and at a short
  deferral (D = 10) T1's max on the two clients whose max frame was far (-5.7 / -7.6 ms); never T3.

### A FINAL-row divergence the human-key L11 run exposed (not a target figure; for the orchestrator's queue)

`rt1_judge.py` / `BEAMF/rowdiff.py` on the after sessions (`work/SRTJ/final_rows.txt`): `srtj_ctl` and
`srtj_l12_h` keep the known signature (positions 0 m in every class, mechanisms 23/23 and 14/14 bit-equal,
rotation-only rows <= 2.1e-8, pieces from 460 as RTR), but **`srtj_ctl_h` client 2's own ball differs in position
from the server's from tick 507 to 598: 3.46 cm at 508, 2.0-2.8 cm through 598** (until its beam at 600 remakes it),
and its mirror of client 1's ball 1.1-1.6 cm at 508-518; client 1's world is bit-equal to the server in both balls.
`work/SRTJ/rowticks.py` and the journals' rows: client 2's own ball came out of its trafo (stone, physicalized at
507, the game's trafo script live, `make_own` in the replays that cross 507) about 1.5 m from client 1's stone
ball, which had come out of the same trafo at ~460 and still sat at its exit (both players were beamed into that
trafo by the recipe, 300 / 340, and the human keys did not drive them away) - the two balls overlap at the birth.
In the server's world the overlap is resolved in step 507 (the newborn moves (-0.030, +0.028, +0.021) m from its
event pose, client 1's ball is pushed (+0.010, -0.002, -0.017) m: the server's received row at 507); in client 2's
world neither moves at 507 (own ball (60.9513, 8.0814, -12.5775) = the event pose less one tick of fall; the
mirror continues its own motion), and the live row and the final row of 507 agree, so the live birth and the
replayed birth do the same thing and both differ from the server and from client 1 (where this ball is a mirror,
born by the remote path: bit-equal). In client 2's final rows the own ball first rises at 508 (8.0814 -> 8.0972):
the contact acts one tick late. **Symptom**: an own ball born overlapping another ball diverges from the server
from its birth tick, by centimetres, until the next remake. **Where, as far as measured**: a trafo's new ball is
inserted with its pairs to other player balls refused (the spawn ghost's pair filter, `physics_state.cpp`
`spawn_ghost_filter`), and the spawn-ghost layer's PSI listener makes them at the next PSI start - the body "came
to a solid group", is "resumed solid" and rechecked (`recheck_collision_filter`) when its overlap-tree sphere meets
another player ball's (`spawn_ghost_listener::event_PSI`, `physics_state.cpp` ~4242-4352: `++state->resumed` 4281, the recheck 4348; steps SPGH / SPGH2). The
server did that here: its log's cumulative counters went from "2 births, 1 resumed solid, 2 solidified, 0
rechecks" at 467 (client 1's trafo ball, nothing near) to "2 resumed solid, ... 1 rechecks" at 507 - this birth,
rechecked, so the pair existed in step 507. In client 2's world the own ball read solid after the frame (no "own
ball ... is a spawn ghost" transition logged at 507, so it was not left a first-life ghost and no overlap wait was
counted: 0 in the session) but its pair with the mirror acted only at 508. So either the resumed own newborn was
not rechecked at 507's first PSI or the recheck's `close_by` test (the two overlap-tree spheres) failed for it -
the own newborn's collision bookkeeping (its overlap element) differing from the server's clone, i.e. the class of
SFD's cause (A) (the own ball's collision state differing from every copy of it from its birth tick; a known
residual waiting for the user), here on retail and visible as a position because the newborn is in contact. The
spawn ghost was checked: no ghost applies at 507 (the trafo exit is outside the 6 spawn areas; the own ball was
solid from 340, the mirror from 322, in client 2's log). **Not established**: which of the two (no recheck, or a
recheck that found the spheres apart), what in the own-birth path makes the overlap element differ, and whether a
pre-cost-cut build does the same (no earlier run had two balls born overlapping; SRT3 / SRT4 / SRT2B change
neither the birth nor the step: the rows 508-518 come from the replay anchored at 504, a whole record, no rebase).
What would settle it: a probe of the overlap elements and of the listener's recheck decision at the birth PSI in
both worlds (OWNB's `BMMO_HULL_PROBE=Ball` rows on client 2 against a SimTool replay of the server journal with the
same probe), and a headless arm with two balls born overlapping out of one trafo (a deterministic reproduction).
Left for the orchestrator's queue (world agreement, beside (A)); this recipe (`srtj_retail.py ctl_h`) reproduces
the setting. **Open, not root-caused; queued in roadmap.md section 4 item 5 (FULL step SRTJ).** This step changed no
code, so it cannot be its cause; whether an earlier build does the same is not shown (no earlier run had two balls
born overlapping).

**As built (step OVX, 2026-11-01): root-caused, the fix blocked on the user.** Base a4c12289 (R6H). Probe added
(kept, diagnostics only, inert without the variable): `BMMO_SPAWN_GHOST_TRACE=<file>` (`physics_state.cpp`,
`spawn_ghost_trace*`) appends the spawn-ghost listener's per-PSI decisions (every player ball's group, movement
state, overlap element and pairs with other player balls; "rechecked" / "not close" with the pair count before and
after) and the own group's adoption (`adopt_group_now`, before / after), each line stamped with the IVP clock.
- *Reproductions.* Retail: `.local-tools/work/OVX/ovx_retail.py` = `srtj_retail.py ctl_h` with the trace and the
  hull probe, Arch server of a4c12289 + the trace. `scripts/retail/ovx_r1` reproduces SRTJ's numbers exactly (the
  recipe is deterministic up to the server's world: the server rows at 506-508 of ovx_r1 and ovx_r2 are equal):
  client 2's own ball 0.03459 m off the server at 508, its mirror 0.01605 m, 46 own rows differing (rt1_judge: own
  0/1, mirror 0/1). Headless (`maps/arena-test/cpu_full/ovx_repro1`: L11, 2 clients, `--beam 60.951 9.095 -12.578`
  at 300 / 340, latency 150, trafo inputs + placement on): the same two-balls-out-of-one-trafo overlap (the server
  pushes the newborn at 508: v 1.146 m/s up), and the owner's rows equal the server's from the birth - headless
  does NOT show it (`.local-tools/work/OVX/probe_arm.sh`: offline replays of the captures and of the server
  journal with both probes).
- *Neither of SRTJ's two candidates.* The live retail birth does exactly what the server's clone does: the trace of
  client 2 at the birth PSI (IVP clock 0x1.d00000e8p+3) has the group adopted in the PreSimulate pass (pairs 0 ->
  0, the body not yet simulated), then "resumed", `close_by` true, "rechecked Ball_Stone pairs 0 -> 1" - the
  server's replay of the same journal logs the same line at the same clock. **The cause is the replays.** All three
  rewinds that cross the birth (anchors 496, 498, 504) log the newborn's resume and recheck one tick late, at
  0x1.d0f83ef8p+3 (508's first PSI): on retail an own birth inside a rewind's window keeps the parking path
  (`session_host_world.hpp` `keeps_parked` ~601: `own_birth_destroyable` is false because
  `client_core_lifecycle.cpp` `destroyable_own_birth` ~2591 remakes a scripted own birth only with
  `own_body_control`, which retail does not declare - OWNB's "Adaptation (retail)", rewind-cost-design.md), so the
  body is held out of collision detection through its birth tick's step (no overlap-tree node: the listener does
  not see it at 507) and re-posed after the step at its live record of 507 (`rollback.hpp` ~2263-2285, ~2009).
  Its group is then "not present" at 507's PSIs, comes back at 508's first PSI, is resumed and rechecked there:
  the contact the server had in 507 acts at 508 in every replay, and the FINAL rows keep it. The live rows hide
  it because the live mirror is a prediction far from the contact.
- *The fix measured (option A, a prototype, `.local-tools/work/OVX/ovx_optionA.patch`, not in the tree).*
  `destroyable_own_birth` also takes the scripted path on a host with `own_body_in_replay` (retail since EL8; the
  BEAMF precedent, `beam_remakes_body`): the restore takes the scripted own birth out and the replay's drain makes
  it again at its birth tick (`remake_own_body`, the server's clone's way), as OWNB does headless. Retail
  `scripts/retail/ovx_r2` (same recipe): the replays' recheck at 0x1.d00000e8p+3 (507), and rt1_judge FINAL rows:
  client 2 own 1/1 bit-equal (1876/1876 rows), mirror 0 m (the four known rotation-only rows <= 2.1e-8 at
  1068-1074), mechanisms 23/23, pieces 0 m (the known 21/49 signature); client 1 the same. Unit tests 736/736
  with the patch's test case (`ScriptedOwnBirthInsideTheWindowIsRemade` for the retail capabilities); the wire
  check rR6H -> the patch's x64 build over core_ref_s14: ALL SAME (headless declares own body control: unchanged).
- *Why it is the user's choice (not applied).* The design deferred this remake to R6 ("the R6 step removes the
  parking of an own birth inside a window when it declares own body control", OWNB), EL8's capability itself
  leaves it out ("Own births in the window whose life is alive now and whose name was not revived stay on the
  parking path", `reconcile_own_lives`' comment, client_core_lifecycle.cpp ~1738), and option A has a
  player-visible trade-off: a bridge-made own ball loses Sound_Manager's six collision / contact listeners
  (BEAMF-S, the user's (b): re-arm them, designed in R6D item 1, not built). Today that silence follows only
  automation beams; with option A it follows every own birth a rewind crosses - in ovx_r2 5 of the 8 scripted own
  births (both level-start spawns, the trafo at 507, 1367, 2299) - and lasts until Sound_Manager switches its blocks
  Off and On for the next game-made ball, so with a remote player present the own ball would be silent for most of
  a session from its first spawn (the documented mechanism above; not measured here). The patch widens only the
  scripted path: a moved own birth keeps R6H's rule (forced only), so R6H-SUP's options stay open. Options: (A) the patch now, with BEAMF-S's
  silence widened to those births until R6D's re-arm lands; (A') the patch together with R6D item 1's re-arm
  (pulls that item forward into this step); (B) keep the parked body but put it into collision detection at its
  Physicalize pose before its birth tick's step (no silence, but its live hull bookkeeping stays - OWNB's cause 2
  - so not shown bit-exact); (C) leave it to R6B / R6P's own body control (the residual stays until then).

### For the user (blocked_on_user)

- **Is the target met?** T2' and T4: yes, everywhere (L11 / L12 retail at 150 ms, scripted and human-rate keys;
  every headless arm). T3: no - the key-to-screen p99 is 43-48 ms with human keys (31.1 ms, the pacing's own floor,
  only on the scripted L12 session) and the max 64-104 ms, because one rewind at 150 ms is 25-43 ms on the E-cores.
  T1 (reported): 0.6-5.5 % of retail frames over 15.15 ms, none over 45.45.
- **f2?** By the bound above it buys T1's count (0 to -54 %; at D = 10 also T1's max on 2 of 8 clients, -5.7 /
  -7.6 ms) and makes the remaining frames longer (at D = 25 the max +3.4 to +11.5 ms); it does not move T3. The options as measured: (i) take the target as met in its hard part T2' / T4 and relax T3 to the
  measured tail (e.g. "p99 <= 50 ms, never over ~105 ms at 150 ms"); (ii) more cost cuts aimed at T3 - the single
  rewind under ~11 ms needs about half today's replayed-tick cost (SRT4's options (b) deeper sparse capture, (c) the
  replayed tick's non-capture bookkeeping, (e) a cheaper whole record) plus the L12 activations (a5); this scales
  with the machine as section 4.1 says; (iii) f2 as designed (measured above: fewer over-budget frames, the
  remaining ones as long or longer, T3's tail unchanged); (iv) a different budget mechanism (none feasible without a second world, section 4
  b / c). The choice and the target's final terms are the user's.

### Checks run, artifacts, not done

- Builds: x64 BUILD-OK DEPS-OK and x86 CLIENT-BUILD-OK with the census patch (`build-agent-rSRTJd`); the patch
  reverted and both trees rebuilt clean (BUILD-OK, CLIENT-BUILD-OK, build id `...bmmo-606d37f62095`), HEAD's x86
  build re-installed in both copies; the Arch server built from HEAD (ARCH-BUILD-OK). No product file changed: no
  unit test, GCC check or wire check was due (the step's only tree change is this note and the status lines).
- Live (one at a time, sampler beside: host max 17.8-27.9 %, worst outside process 1.0-3.5 %): 6 headless arms
  (`maps/arena-test/cpu_full/srtj_E_*`, census CSVs `work/SRTJ/live/`), 5 retail sessions (`scripts/retail/srtj_ctl`,
  `srtj_ctl_h`, `srtj_l12`, `srtj_l12_h`, `srtj_ctl_vsync`). The plan section's run set (L11 / L12 / arena headless,
  retail L11 ctl and L12, scripted and human) set the count, not the quick tier's two-run cap; the vsync session is
  SRT1's deferred present-blocking check.
- Not covered: four members on retail (two copies; the headless arms carry the four-member count), a retail arena
  session (it needs the slot-13 builds, CMAP retires that), a present that blocks on vblank (above), a P-core
  retail session.

## Measured (T3C steps S0 and S1: the census re-baselined on HEAD, and the a5 spike removed)

The user's decision 56 chose (ii), more cost cuts; T3C is that work (plan report
`.local-tools/work/RCX/plan-report.md`). SRTJ's numbers were 176 commits old (606d37f6: before SLOT, R1's slot
images, R6B, the 53-bit reads, the remote coalescing, engine changes #24 / #25), so S0 measured again.

**Method (S0).** The SRTJ census patch ported to HEAD (`work/S0/apply_diag.py`, reverted after the runs) with eight
more buckets (`work/S0/s0_counters.hpp`: the world hash, rollback_world binds, restore_scripts, set_savestate,
put_omitted_images, list_bodies, the corrector reads, the SectorManager lookups). SRTJ's four retail recipes (L11
ctl / ctl_h, L12 / L12_h; `work/S0/s0_retail.py` over `work/SRTJ/srtj_retail.py`), the two copies on E-cores 16-21
and 22-27, each at one-way `net lag` 150 (SRTJ's setting, ~300 ms round trip, D 22-23) and 75 (~150 ms round trip,
D 12-14), three runs each, checkpoint cadence 660 (the deployed value; four more runs at SRTJ's cadence 1 differ
from them by noise: list_bodies +25 us, nothing else). Buckets `work/S0/s0_report.py`, the target
`work/SRTJ/srtj_judge.py` (`work/S0/judge_table.py`); medians over 3 runs x 2 clients.

**What a rollback frame is now (S0, L12 / L11, us):**

| | L12 | L11 |
|---|---|---|
| per replayed tick | 800-930 (capture 480-620, pre_step 80-100, step 30-45, rest 140-180) | 820-890 (capture 480-530, pre 85-90, step 65-77, rest 195-205) |
| restore per rewind | 2170-2420 (restore_scripts 1350-1530, set_savestate 440-620) | 2260-2590 (scripts 810-900, set_savestate 1060-1190) |
| put_omitted_images per rewind | 3340-4560 | 1390-2090 |
| live tick (core, no rewind) | 3750 (capture 2490-2520, of it omitted images 1040) | 2960-3040 (capture 1810-1870, omitted images 440-460) |
| the world hash, a normal frame | 21 | 17-28 |
| render | 480-560 | 630-670 |

So R1's slot images (`put_omitted_images`, the unbound copies' images of every whole record) are now the largest
single item after the hash spike: ~40 % of the live capture on L12 and ~0.15-0.2 ms of every replayed tick.

**The a5 spike (S0) is the world hash's surface signature.** On every frame with no rewind and core work over
10 ms the hash bucket took 11.8-14 ms on L12 (p50 12.4-12.7) and 8.9 ms on L11, against 17-28 us on the others: a
birth or a sector pass drops the surface memo whole and the per-tick TICK hash recomputed every surface - 5-10
such frames per 1000. The TICK record carries no surfaces.

**S1 (b76e503d, bridge v35):** the per-tick TICK hash without the surface signature (anchors, hold digests,
recordings and SimTool keep it). Same recipes on S1 (24 runs): those frames 5-10 / 1000 -> 0 everywhere, the
hash 9-19 us. The target, one-way 150 (75 in brackets), S0 -> S1:

| recipe | frames over 15.15 ms | T1 max | T3 p99 | T3 max |
|---|---|---|---|---|
| L12 scripted | 10.5 -> 3 (11 -> 3) | 33.8 -> 25.3 (31.6 -> 20.8) | 32.2 -> 31.0 (32.3 -> 31.1) | 73 -> 55 (64 -> 46) |
| L12 human keys | 56 -> 40 (52 -> 39) | 44.9 -> 35.5 (35.0 -> 29.0) | 52.6 -> 49.5 (42.2 -> 39.7) | 96 -> 75 (66 -> 55) |
| L11 ctl | 32 -> 24.5 (23 -> 20) | 43.3 -> 37.7 (30.6 -> 28.1) | 45.9 -> 40.2 (35.5 -> 33.2) | 103 -> 87 (61 -> 63) |
| L11 ctl_h | 88 -> 77 (78 -> 64) | 55.9 -> 42.2 (37.3 -> 32.6) | 51.1 -> 46.3 (39.3 -> 37.2) | 106 -> 90 (71 -> 65) |

L12 scripted is at the pacing's own floor (p99 31.0, the floor 31.9-32.0); the rest is the rewind frame itself.

**Found on the way:** three of 28 S0 starts ended "did not report ready" - the server's start barrier took its
deadline in on_world_ready, on the runner at the logical instant the boot began (the boot blocks the runner), so a
12-15 s boot left the members 5 s of the 20 (fixed: the deadline is taken at the main loop's first barrier check
after the world is ready).

**Next (by the S0 buckets):** the slot images (plan option e3: a copy with no force or joint block keeps t - 1's
image; or capture them only where a slot's block can have changed), then the restore's script write (1.3-1.5 ms
on L12) and set_savestate (~1.1-1.2 ms on L11), then the bookkeeping (c) and the (e1) chain.

## Built (T3C step e3: the slot memo)

`game/script_slots.hpp` (the memo and the quiet-run ledger), `game/script_replay.hpp` (put_omitted_images, the
observing capture, observe_slot_roots), `session/rollback.hpp` (`rollback_world::observe_scripts`: a light
record's observation). An omitted slot copy keeps its last walked image and section entry; a whole record appends
the entry instead of walking when the host-write generation is the memo's, the copy's unit root has been quiet
(OPC's `reusable_root`) at every observation since the memo was walked - every capture is one observation, light
ones included; a gap, the same tick again or a backward jump ends a run - and every force tail reads as it did.
Copies with a joint block are never memoized; only a capture that observed may use a memo (the hold digest's and
the tools' walk). `BMMO_SLOT_MEMO` = off | on | verify, on by default since the gate.

The design round (`.local-tools/work/E3/design.json`): the literal e3 (copy t - 1's entry for copies without a
force or joint block) covers 0 of L12's 156 omitted copies (every fan copy has its SetPhysicsForce); the memo
reaches replayed whole records without e1. Found on the way: the proximity union's relatch wrote a Get Cell output
without bumping the generation (31bd941b).

**Gate.** Unit tests (the ledger rules); fast LBM with `BMMO_SLOT_MEMO=verify` and `BMMO_SCRIPT_REUSE=verify`
(every hit walked again and compared; OPC's verify walk with the memo off): 38/38 FINAL exact, criteria equal arm
by arm, 9.8k hits per client, 0 mismatches; retail verify (L12, L12_h, L11 ctl_h at one-way 150): hit rate 91-95 %,
0 mismatches for both; fast LBM with the memo on by default: 38/38, criteria equal.

**Measured** (S0's recipe on one diag build: memo on 3 runs, off 1 run, medians over runs x 2 clients):

| | L12 off -> on | L11 off -> on |
|---|---|---|
| live core (no rewind) | 3.79-4.00 -> 3.09-3.13 ms | 2.91-3.22 -> 2.71-2.81 ms |
| live capture | 2.53-2.69 -> 1.82-1.85 ms | 1.79-2.00 -> 1.56-1.63 ms |
| of it the slot images | 1.05-1.13 -> 0.35 ms | 0.44-0.49 -> 0.19-0.20 ms |
| slot images per rewind | 3.3-5.4 -> 1.75-2.2 ms | 1.4-2.2 -> 0.8-1.1 ms |
| L12_h at 150: over-15.15 / over-30.3 frames, T3 p99, T3 max | 39.5 / 20 -> 21 / 7.5, 54.7 -> 47.6, 87.9 -> 72.5 ms | ctl_h: equal within noise |

The retail recipes run K = 8 slots; the public servers run 20 (29 of 30 rooms), about three times the omitted copies,
so the saving there is about three times as large (a projection, measured by T3C's public-test re-drive).

## Measured (T3C: the live core's "other" share)

The census (S0's recipe) with twelve more buckets: the inclusive post_step, inbound drain and pre_step, and self
time - an enclosed timed scope excluded - for the own trafo poll and retirement, drain_body_events,
record_script_order, the tracked set's build, rollback_.record, the input uplink, the airship and spawn ghost frames
and place_probes; plus a count of physics_view's by-name lookups. E-core retail, one-way lag 150, 3 runs, frames
without a rewind, means in us:

| | L12_h | ctl_h | L12 |
|---|---|---|---|
| live core | 3088 | 2751 | 3117 |
| of it uncovered before ("other") | 916 | 833 | 934 |
| trafo poll (the effect check) | 344 | 251 | 349 |
| place_probes (the union) | 146 | 106 | 155 |
| post_step not in a bucket | 91 | 79 | 94 |
| core_host_frame outside post / drain / pre | 80 | 78 | 80 |
| drain_body_events | 56 | 54 | 49 |
| uplink | 47 | 55 | 50 |
| ghosts | 34 | 32 | 41 |
| tracked + record | 60 | 116 | 58 |
| drain + order + pre_step rest | 58 | 62 | 58 |
| by-name lookups: count, time (across buckets) | 67, 528 | 77, 447 | 68, 539 |

The trafo effect check asked has_body of the 51 original pieces by name every frame; each lookup scans the 3D
entities. By CK id (bridge API v36, docs/engine-changes.md): trafo poll 8-11 us, by-name lookups 16-26 per frame,
live core 3088 -> 2724 (L12_h), 2751 -> 2521 (ctl_h), 3117 -> 2828 us (L12). An entity lookup scope over the 51
lookups saved only ~50 us (its table is built from every 3D entity once per scope). Next by size: the union's probe
placement (grows with K), the per-frame GetScriptByName("Gameplay_Ingame") in core_host_frame, the event drain's
256 KiB buffer.
