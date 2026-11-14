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
  resync's apply, and an input correction older than the lifecycle anchor (since step MRG taken along at the
  frame's final flush: 'Built (a lifecycle rewind takes a deeper input correction along, step MRG, 2026-11-12)').
  In the measured sets no frame held two.
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

## Measured (T3C: a replayed tick's parts, 2026-11-10)

With the relay hints a rewind is ~6 ticks deep for the member they help, so a rewind is still two thirds replay
(restore ~2.2-2.5 ms, a replayed tick ~0.74-0.81 ms on these recipes). The census (S0's recipe on 381dbd4d) with
28 more buckets (`work/S0/apply_cap.py` after apply_diag.py and apply_other.py; report `work/S0/rc_split.py`): the
capture's parts by context (replay / live) - the lookup scope's build, get_body, extras + contacts, nav + clock +
globals, observe_scripts, the scripts, the blob - each replayed record whole or light, and the replay loop's other
parts as laps. E-core retail, K = 8, one-way lag 150, 3 runs x 2 clients, means over the replayed ticks (they add
up to the replay meter within 4-5 us), us:

| a replayed tick | L12_h | ctl_h | L12 (12 rewinds) |
|---|---|---|---|
| all | 779 | 809 | 743 |
| capture: scripts (of it the slot images) | 237 (69) | 161 (35) | 274 (76) |
| capture: get_body | 184 | 199 | 103 |
| capture: blob, extras + contacts, rest | 48, 12, 13 | 67, 15, 24 | 56, 13, 11 |
| pre_step, step | 79, 44 | 84, 68 | 96, 39 |
| place_probes (the replay's union) | 78 | 68 | 72 |
| the record's build and store (the replaced one freed) | 42 | 52 | 34 |
| replay_mechanisms | 32 | 46 | 31 |
| the rest (feed, births, remotes, globals, after_step) | 12 | 25 | 14 |
| light records, us each / whole, us each | 84 %, 194 / 2071 | 84 %, 227 / 1691 | 82 %, 92 / 2080 |

- The scripts are the whole records' (one in six replayed ticks; a light record observes only, ~2 us).
- get_body is a light record's whole cost: 14 lookups a tick on L12 (37 on L11) at 13.5 us (5.4) each - every
  tracked name scanned over the 3D entities, because the capture's lookup scope builds its table only after
  kScanLookups = 12 scans (physics_state.cpp find_entity), so a capture of 13-14 names pays 12 scans and the
  table. The live capture the same: 214 us of L12's 1.95 ms (13 lookups at 16 us).
- The physics itself (pre_step's scripts and the step) is 16-19 % of a replayed tick.

The per-entity probes add ~40 us to a replayed tick against o3's 736 (L12_h). Next by size: a tracked body read
without the name scan (as v36 did for the trafo check: 184 -> ~10-20 us a replayed tick, ~200 us a live frame,
estimated), the whole records' scripts (e1, or fewer whole records), the replay's place_probes.

## Measured (T3C: the entity name cache, 2026-11-10)

find_entity's name -> CK id cache (47308459, bridge v37; docs/engine-changes.md "the entity name cache"). The
same census build on 47308459, `BMMO_LOOKUP_CACHE` off / on interleaved, 2 runs x 2 clients each, then one
verify run (`work/S0/run_lc.sh`), E-core retail, K = 8, one-way lag 150, us:

| | L12_h off -> on | ctl_h off -> on | L12 off -> on (12 rewinds) |
|---|---|---|---|
| a replayed tick | 751 -> 541 (-28 %) | 837 -> 569 (-32 %) | 678 -> 543 |
| of it get_body | 183 -> 11 | 206 -> 29 | 97 -> 7 |
| of it the replay's place_probes | 76 -> 54 | 71 -> 40 | 63 -> 45 |
| the restore | 2234 -> 2319 | 2556 -> 2543 | 2472 -> 2485 |
| a live frame's core (no rewind) | 2837 -> 2476 | 2857 -> 2336 | 2804 -> 2624 |
| of it the capture's get_body | 214 -> 12 | 198 -> 30 | 210 -> 13 |

So a rewind of the ~6 ticks a hinted member replays is ~1.3 ms shorter (L12_h: 2.3 + 6.2 x 0.75 -> 0.54), one of
24 ticks ~5 ms. Verify: 1.26 M hits over the three verify sessions, 0 mismatches, no shared name. The journals'
FINAL rows of off, on and verify, client by client: the balls and the 14 / 23 bodies bit-equal; ctl_h's 32 trafo
pieces differ between any two sessions, two off runs and two runs of 381dbd4d alike, and they must: an explosion's
Random seed is trafo_seed(session seed, player id, applied tick, type), and a player id is the server's
GameNetworkingSockets connection handle, a random number of each connection (c2off_ctl_h runs 1 and 2: the same
explosions at ticks 458 / 498 / 908, ids 484592829 / 508092805 vs 2462173594 / 109078206, seeds 1547941638 /
75651449 / 1829446291 vs 2051120431 / 347687379 / 1593814566). Within a session every client and the server agree;
across sessions only the balls and bodies are comparable. (Superseded by protocol 39: under WORLD_TRAFO_SEED_JOIN_ORDER
the seed mixes the life's join order, the same in every session of a recipe - trafo-replication design 2.6.) The restore is unchanged: it is the next fixed part (restore_scripts 1.4-1.5 ms
on L12, set_savestate 1.2 ms on L11), with the whole records' scripts (~0.2 ms a replayed tick).

## Measured (step A2: a remote correction cadence, 2026-11-10)

The architecture review of 2026-11-09 (after public test 1) proposed a cadence for the remote corrections: in a
13-player room every remote's key change is a rewind, ~23 a second, and kRemoteReplayCoalesceFrames (own_correction.hpp)
coalesced them only while the pacer was 2 ticks behind. As built: `core_config::remote_cadence_frames` (headless
`--remote-cadence N`, retail `session cadence N`, kept in the capture's header; default 0 = the rule as it was). N > 0
holds a remote's correction on schedule too, up to N frames after the last input replay, under the reach deadline
(own_replay_pacer::due with the cadence's frame count); the replay that runs carries every correction of the frames
it waited, from the earliest. Not held: a pending own correction (the own pacer's rule), and - on schedule - a
correction of a ball within kRemoteCadenceNearMeters (8 m, SRTJ f2's 'near') of the own ball in the newest record. A
lifecycle rewind that carries the pending replay counts as the replay's frame. Status: ` coalesced: ... deferred=N`
as before, plus ` deadline=D near=M` once either is non-zero. The world is the same either way: only the frame a
mirror's correction is shown in moves.

Measured on the public test's Level 6 recording (13 players, K = 20, each client at its public-test ping, 0-114 ms
round trip, virtual clock, hints on (A1), 6270 ticks, headless x64 on the dev machine, medians over the 13 clients;
two series run alone one after the other, `a2t_*` without the near rule and `a2u_*` with it):

| | cadence 0 | cadence 3 | cadence 4 |
|---|---|---|---|
| rewinds a second | 22.6 | 16.9 | 14.4 / 14.6 |
| depth mean, p90, max | 5.6, 11, 16 | 6.8, 12, 18 | 7.5, 13, 19 |
| replayed ticks | 12069 | 10851 | 10406 / 10386 |
| a rewind (restore + replay) ms | 7.1 / 6.7 | 8.2 | 8.6 / 7.7 |
| rewind work ms a second | 160 / 151 | 139 | 123 / 112 |
| frame p99 ms | 15.9 / 14.0 | 17.8 | 17.6 / 14.8 |
| frame p999 ms | 21.4 / 18.4 | 24.3 | 24.0 / 19.6 |
| frame max ms | 27.3 / 28.3 | 31.0 | 29.1 / 27.3 |

(a / b: the two series; the two cadence-0 arms run the same rule and differ by 1.9 ms at p99: the machine.) Every
arm 13/13 FINAL rows exact, the server journals byte-identical across the arms; no wait reached the reach's deadline;
on the near rule 18-123 corrections per client ran at once (1-6 % of the deferred). The physics-free model
(`.local-tools/work/ARCH/batch_model.py` on a1t_on's logs) had predicted 23.9 -> 13.6 rewinds a second and 160 -> 109
ms a second at cadence 4.

So the cadence does what SRTJ found for f2 (above, "What f2 would still buy"): it cuts the rewind work a quarter
(-23 to -26 %) and the rewind frames a third, and makes each remaining one deeper - the frame tail does not improve
(p99 +0.8 / +1.7 ms, p999 +1.2 / +2.6 ms, the max unchanged). It is a CPU lever (a client that cannot keep up), not
a stutter lever on a machine that can; the 2-ticks-behind rule already applies it where the CPU is short. Default
left 0. Gate: 840/840; fast LBM with the default 38/38 twins FINAL exact, every criteria row equal to A1's merged
tree (a1g); with cadence 4 38/38 FINAL exact, the only rows that moved already failing (A6M's late ball: input
rollbacks in D 3 -> 2, its mirror 0.85 -> 1.11 m off from T_off+60); x64 and x86 builds. Not run: retail with the
cadence on (own_replica_name matching the recorded own-ball name is verified headless only).

## Measured (step A5: how long a relayed frame waits on the client, 2026-11-10)

The review guessed the network polls cost 1-2 ticks of depth (both sides polling every 15.15 ms). The server's half
was already gone: since K4b the runner asks the loop for a receive pass at each step instant (server.cpp
wait_until / request_poll), so a frame that arrives before the step is in it. The client's half was measured with
SessionTelemetry's new inbound fields (the wait from the transport's receive stamp to the core's handover of each
SessionRemoteInput: retail, the network thread's poll plus the wait for the frame's mailbox drain; headless, its
poll). Retail, two x86 copies on the Arch server (protocol 38, hints on), RT1's L11 recipe at human key rates, copy
1 at 150 ms and copy 2 at 20 ms one way, E-cores: inbound p50 15.6 / 16.1 ms and p99 29.6 / 30.5 ms - a whole tick
at the median, two at p99: the network thread's idle pass slept to the next 1/66 s (client.h run(), uniform 0-15 ms)
and the drain waits for the next frame (another 0-15 ms). The idle pass now sleeps 1 ms (client.h kIdlePoll; a
pass that finds nothing costs microseconds, the Player.exe timer resolution allows it): inbound p50 8.3 / 8.3 ms,
p99 16.9 / 16.9 ms - the frame's own wait is what is left (a frame-paced game takes its messages once a frame). Depth
a rewind 6.1 -> 5.8 (copy 1) and 25.2 -> 23.9 (copy 2), one run each; FINAL rows exact in both runs, no resync; the
games' CPU in the session unchanged within the sampler's noise (20-40 % of a core each). The headless client already
polls every <= 2 ms while it paces.

## Built and measured (T3C: the restore skip, 2026-11-11)

The performance report of 2026-11-10 (the public servers' K = 20 slots, retail census on d6627c28) put the restore's
script write at 2.77 ms of a 3.65 ms restore on L12 and named it the largest fixed cost. Split by the census with eight
buckets inside `script_replay::restore` (`work/S0/apply_rs.py`, `rs_split.py`; K = 20, one-way 150, one run each; a probe
walked every unbound slot copy before it was written, its time excluded):

| per restore, us | L12_h | L12 | ctl_h |
|---|---|---|---|
| the selected scripts' walk (bound copies, satellites) | 895 | 847 | 537 |
| the unbound slot copies written | 765 (468 copies) | 789 (468) | 313 (234) |
| of them already holding the image written | 100 % | 100 % | 99.7 % |
| the trafo section | 134 | 151 | 112 |
| cells, queue, hold, joint order | 78 | 68 | 52 |

A plain walk of a copy (6.9 us) costs more than its write (1.6 us), so the skip cannot prove a copy unchanged by walking
it. It uses step e3's premise instead (`game/script_slots.hpp`, `BMMO_RESTORE_SKIP` = off | on | verify, on when unset):
an omitted copy is left as it is when its root still passes the reuse rule against its last ledger observation at the
restore's start (`quiet_now`, read before the restore writes any root's flags), its slot memo is current by the
capture's rule (`memo_refused` with the host-write generation at the restore's start, every force tail re-read), the memo
is the image the record names, and the copy has no delayed link (a write re-arms it). Copies with a joint block are never
memoized, so they are always written. Verify walks each copy it would skip, compares it with the image and writes it
anyway, so a verify run is the off run. The restore's binding bumped the host-write generation before restoring, which
refused every memo (the review's finding: the first build never skipped); the bump now comes after the skip has read
the generation (since the kept source, below: inside `script_replay::restore`, before its first write), and no capture
or observation runs in between. Counters on the `slot_memo` line (` restore_skip: mode=... skipped written
verified mismatches`).

Gate: 844/844 (a ledger test); fast LBM with verify 38/38 twins FINAL exact, every criteria row equal to the protocol 39
run, 5388 copies verified, 0 mismatches over 116 client sessions; with the skip on 38/38, the same rows, the same 5388
skipped; x64 and x86 builds. Retail, the census at K = 20, off / on interleaved, 2 runs x 2 clients:

| | L12_h off -> on | ctl_h off -> on |
|---|---|---|
| a restore | 3687 -> 2426 us (-34 %) | 2994 -> 2516 us (-16 %) |
| of it the script write | 2799 -> 1532 us (-45 %) | 1374 -> 915 us (-33 %) |
| copies skipped | 93-97 % | 93-97 % |

Verify in retail: 69 k copies walked, 0 mismatches. FINAL rows of off, on and verify, client by client: balls, bodies
and (ctl_h) the 32 trafo pieces bit-equal - the first cross-session check that includes the pieces (protocol 39). The
restore's other parts are next: the selected scripts' walk (0.5-0.9 ms) and set_savestate (1.1 ms on L11).

## Measured (the cost of a join, 2026-11-11)

The public test's server journals, replayed by SimTool with `--write-journal` (a replay journal's TICK records stamp
the replay's own wall clock as each tick ends; SimTool's stdout through a pipe is block-buffered, so timestamps of its
lines are wrong), found every tick over 15 ms on a P-core of the i7-14700KF to be a member's join: 50-66 ms there,
110-150 ms on an E-core, 24-31 ms on an Apple M6. The server's runner steps every room's world on one thread, so a join
stalls every room on it; a client builds the same set for each joining member. Timed by parts (L7 session 2, 46 joins,
P-core): `build_trafo_set` 46 ms, of it CopyObjects 3.7 ms and 39 ms in three snapshots of every CK object id (a level
context holds ~101 k objects; CK's list costs 0.19 ms, the `std::set<CK_ID>` built from it ~8 ms, and step 0's two
scans look every id up in it). Step 0 found no extra object in any of the 46 joins.

Built: `game/ck_object_ids.hpp`, a bitmap indexed by id with the ids in ascending order (the set's order, so every
loop and log reads as before), used by `build_trafo_set` and the placement copies (`sector_placement.hpp`). Same
checks, same order. (Since 'the server's replays' below: the ids come from CK's per-class lists and the later
passes ask for the ids added since.) P-core, the two heaviest sessions:

| | before | after |
|---|---|---|
| L7 s2 (21 players): slowest tick, ticks over 15 ms, worst 100-tick window | 53 ms, 35, 1.77 ms | 13 ms, 0, 0.94 ms |
| L8 s1 (20 players): slowest tick, ticks over 15 ms | 49 ms, 26 | 21 ms (no event in it), 1 |
| a join | ~46 ms | 11-13 ms |

The replays' world and pose hashes are identical tick by tick (44114 / 103762 ticks). Gate: 844/844; fast LBM 38/38
twins FINAL exact, every criteria row equal to the restore skip's run; x64 and x86 builds, the Arch server; a retail
L11 session at human key rates (K = 20, one-way 150): both clients' FINAL rows bit-equal to the restore skip's run,
balls, bodies and the 32 trafo pieces. What a join still costs: CopyObjects 3.7 ms, the 124 copies' name checks 0.9 ms,
the digests 0.7 ms.

## Measured (the cost of a resync state, 2026-11-11)

A resync state (and a late joiner's state, the same capture) is built on the runner's thread, which steps every
room: the public test's server built 2211 of them, p50 30.6 ms, max 49 ms. On the public L6 re-drive (13 players,
K = 20, hints on, six clients forced into a resync with `--resync-at` at 1045 / 1215 / 2030 / 2490 / 4000 / 5500,
the first four just after trafo explosions), timed by parts on the i7-14700KF: `physics_world::capture_resync_state`
22.8-46.4 ms, of it the portable script record 22-37 ms and the trafo ledger 0.4-9 ms (5-9 ms unless the name
cache was warm); the zstd coding 0.8-2.2 ms apart; rows, navigations, union and the IVP savestate together under
0.6 ms.

- The script record's ~1800 object and script references were each looked up again by name (`find_by_name` /
  `find_script`, a scan of every object of the level, ~12 us each): 19-32 ms. The look-ups only count `ambiguous`
  for the SimTool check's report; the record's bytes never depended on them. `capture_portable` without a report
  (the server's states, the trafo tool) now skips them (`portable_report::lookups`); a caller with a report
  (`--portable-scripts-check`) still gets every one.
- The ledger asked, piece by piece, whether a body exists by the piece's name (a scan of every 3D entity unless
  the name cache holds it). It now asks by the piece's id where the backend can (`has_body_id`, as the sets'
  effect check does); the same answer, since every copy's name is its own (`<original>_Trafo<player/life>`, I4).

Both checked byte for byte: an FNV-1a hash of each of the six states' raw bodies is the same before and after, and
both ledger answers were taken for every piece (663 pieces x 6 states): 32 pieces with a body, 0 that differ. After,
same recipe and ticks: capture 3.9-7.4 ms, coding 0.8-2.2 ms. Gate: 844/844; fast LBM 38/38 twins FINAL exact,
every criteria row equal to the join fix's run; x64 and x86 builds, the Arch server; L6's
`--portable-scripts-check` (a report: every look-up) prints what HEAD prints; two serial reviews, no defects. What
a state still costs: the record's walk ~3.5-7 ms (its ~375 KB of parameter values), the coding ~1 ms.

## Built (the kept source: script reuse across a rewind, 2026-11-11)

**Where a whole record's time goes in a full room.** The public L6 re-drive (13 players, K = 20, the players' own
pings, `work/PERF/run_pt.sh`) with the census buckets printed by the headless client (`work/PERF/cap_split.py`,
`cap_scripts.py`; taken while another job held 25 of the machine's 28 threads, so the times are ~1.3 times a quiet
machine's and only the shares count): a live whole record 2.37 ms, of it the scripts 2.06 ms (87 %; the omitted
copies' images 0.36 ms of that), the IVP blob 0.27 ms, bodies, extras and navigation 0.03 ms. A replayed whole
record 3.03 ms (scripts 2.72). By script, of the 1.68 ms a record spends walking the selected scripts: L6's fans
(`P_Modul_18_MF Script`, 8 of the level and 5 placement copies, 36 behaviours and 194 local parameters a copy)
82 %, the fans' particle scripts 6 %, `P_Modul_29` 6 %, the balloon 3.7 %, the sector manager 2.1 %. A restore's
script write is 2.5 ms of ~3.1; a replayed light tick is ~0.5 ms, 0.27 of it the probes' placement.

**Why the walks.** 12 of the 13 fan scripts are quiet all session and are copied whenever a capture has a reuse
source (step OPC), but a rewind takes the source away: the replay's whole records come after a light record of
t - 1 (SRT4), and the live record of the same frame is behind the engine's barrier at the rewind's end. Counted
on one client (8232 frames, 2835 rewinds): 2825 of the live captures without a source, each for the engine's write
generation; 4205 of the 4707 replayed whole captures, each for a light t - 1; host writes 16 in all. So 55 % of the
captures walked every script, and the restore's bump of the host-write generation refused every slot memo once per
rewind (385 k of the 455 k omitted copies that client walked).

**What was built** (`game/script_slots.hpp`, `game/script_replay.hpp`, `session/rollback_binding_scripts.hpp`;
`BMMO_REUSE_KEPT` = off | on, on when unset since its second gate, below). The slots keep the script list of the last record these scripts are
known to hold - the one a capture left, or the one a restore wrote - with the observation it was kept at and the
host-write generation then. The quiet-run ledger (step e3) observes every selected root, light records included.
A capture the engine offers no record of t - 1 copies a root's block from the kept source (step OPC2's copy: force
and joint tails and the slot image sections read fresh) when the generation is the source's and the root has been
quiet at every observation since; anything else walks, as before. A restore is one observation: a root it wrote
byte for byte, whose words read as the record's after it, has a run from the record's tick, and no other root has
one; a copy the restore skip left as it was keeps its memo under such a root. "Byte for byte" is counted where the
restore leaves a live byte in place (`restore_counters::inexact`): a parameter it cannot write that does not hold
the record's bytes, a delayed link re-armed (its source's output is on, the record's is not) or left at another
countdown. A restore that fails after its first write, or is not told its record's tick (the tools), forgets the
source and every run. `script_replay::restore` now bumps the host-write generation itself, after the skip has read
it and before its first write (the binding did, after the restore).

**What it gives up.** The replay's last record was a full walk, so the engine's comparison of the replayed world
with the live record of that tick set two independent walks side by side. With the kept source a quiet root's block
in both is a copy from one source, and that comparison no longer covers it; `BMMO_SCRIPT_REUSE=verify` and
`BMMO_SLOT_MEMO=verify` walk every copy and every memo hit and compare, which is the check to run for it.

**Counted** (the same recipe, 8000 ticks, the 13 clients summed; 5.18 M script records, 22.2 M omitted copies):

| | off | on |
|---|---|---|
| scripts walked | 67.8 % | 29.5 % |
| of them for want of a source | 2.82 M | 0.03 M |
| scripts copied | 1.67 M | 3.65 M (1.98 M from the kept source: 54 k captures from a captured one, 36 k from a restored one) |
| omitted copies walked | 26.1 % | 5.9 % |

**Timed, under load** (`work/PERF/run_pt.sh` arms kpoff / kpon interleaved twice, 15000 ticks, medians over the 13
clients, `kp_compare.py`; another job held 25 of the 28 threads, so only a pair run under the same load compares:
the replayed tick's time outside its captures, which the switch does not touch, is 0.50 / 0.52 ms in the first
pair and 0.42 / 0.36 in the second, so the first pair is the comparison and the second flatters the switch):

| first pair | off | on |
|---|---|---|
| a replayed whole record | 3.29 ms | 2.19 ms (-33 %) |
| the live capture, a frame | 2.46 ms | 2.23 ms (-9 %; the frames without a rewind copied already) |
| the replay of a rewind | 10.0 ms | 8.5 ms (-15 %) |
| a restore | 3.57 ms | 3.67 ms |
| rewind work + capture, a second | 496 ms | 435 ms (-12 %) |
| frame p99 / p999 / max | 27.6 / 38.2 / 56.8 ms | 24.8 / 35.0 / 54.8 ms |

(The second pair: a replayed whole record 2.64 -> 1.46 ms, rewind work + capture 423 -> 328 ms a second, frame p99
24.2 -> 16.9 ms.) So a record made of copies still costs about two thirds of one walked - from the live capture's
two figures a walked live record is ~2.85 ms and a copied one 2.23 - and that is what is left: a copy walks the
graph block by block for its structure and reads every force and joint tail from physics_RT again (205 tails a
capture on this recipe, and every memo hit reads its own), besides the blob (0.27 ms) and the roots that do run.
A frame with a rewind is ~2 ms shorter under this load, where the estimate from the walk's shares was 2.5 ms on a
quiet machine. Retail's rooms of two, where 6-15 % of the copies came from the kept source, are not timed.

**Gate.** Unit tests 845/845 (one new: the ledger's rules for the kept source and a restore). Verify
(`BMMO_REUSE_KEPT=on BMMO_SCRIPT_REUSE=verify BMMO_SLOT_MEMO=verify`), the L6 re-drive: 3.65 M copies and 20.9 M memo
hits walked again, 0 mismatches. Fast LBM with the same three: 38/38 twins FINAL exact, every criteria row equal to
the resync cut's run; over 140 client sessions 6.37 M copies (96 k from the kept source) and 1.66 M memo hits walked
again, 0 mismatches. Fast LBM with `BMMO_REUSE_KEPT=on`: 38/38, the same rows, the same counts. Fast LBM with the
switch unset: 37 of 38 with their rows equal; `a11u_on` left no summary (its server did not exit within the
harness's 60 s after the session ended, seen once before under load) and was exact with an equal row when run
again with its control. Retail at K = 20, one-way 150 (`work/PERF/run_rkept.sh`), L12 and L11 at human key rates,
two clients each: verify 618 k copies (53 k from the kept source) and 3.55 M memo hits walked again, 0 mismatches;
FINAL rows of the on and the verify sessions bit-equal, client by client, to the restore skip's run - balls, bodies
and L11's 32 trafo pieces. x64 and x86 builds, the Arch server; SimTool's `--portable-scripts-check` on L6 and L11
(its restores now bump the generation). Review: two serial reviews found the restore not byte-faithful below a root
in three branches (a re-armed link, a countdown left live, a parameter left alone), which the exactness count
closes; a third confirmed that and asked for the comparison of an unwritten parameter's bytes. No verify run met
one of those branches, before the count or after (the kept source served the same 95 997 copies over the fast LBM
both times): the count has been read, not exercised.

**On by default** (a1b35f80's successor). On the public server's machine (8845HS, idle, 13 clients on its 16
threads, three interleaved pairs that agree within 2 %): a replayed whole record 2.68 -> 1.84 ms, the live capture
2.40 -> 2.20 ms a frame, the replay of a rewind 9.56 -> 8.26 ms, rewind work + capture 501 -> 457 ms a second,
frame p99 26.8 -> 24.4, p999 32.3 -> 29.4, max 48.6 -> 45.4 ms; a frame without a rewind 5.0 -> 5.1 ms (the source
kept and indexed, ~0.08 ms a record by the split). The roots a restore keeps and the ones it wrote but does not
keep are on the `script_reuse` line (`kept: ... roots= refused=`): the exactness count decides in the headless
runs - 12 refused of 1.12 M on the L6 re-drive, 9 of 27 k over the fast LBM - and never in retail's two recipes
(0 of 15 k). Gate with the switch unset: 845/845; the L6 re-drive in verify, 3.65 M copies and 20.9 M memo hits,
0 mismatches; fast LBM verify 38/38 twins FINAL exact, every criteria row equal to the resync cut's run, 6.37 M
copies and 1.66 M memo hits, 0 mismatches; fast LBM 38/38, the same rows; retail verify at K = 20 (L12, L11), 619 k
copies and 3.55 M memo hits, 0 mismatches, FINAL rows bit-equal to the first gate's sessions with L11's 32 pieces;
x64 and x86 builds, the Arch server.

## Measured (hard real time on the public server's machine: the server's replays, 2026-11-11)

**The target** (the user's, 2026-11-11): the public test's records replayed on the machine its server ran on (a
Ryzen 7 8845HS, WSL2 Arch) with no tick or frame over 15.15 ms. Two halves, reported apart: the server - the four
heaviest server journals replayed by SimTool on one core (`work/PERF/bench_ids_8845.sh`, the replay's journal
stamps each tick's end to the millisecond) - and a client - the public L6 re-drive (13 players, K = 20, the players'
pings) with the measured client alone on a physical core (`run_8845_pin.sh`). The Linux binaries are the local Arch
tree's, stripped and copied over (`pkg_8845.sh`: same compiler and glibc on both).

**The server's slow ticks were joins, and a join was three passes over every object.** Every tick over 10 ms in
the four replays (80 of them) but five carries a member's join. A join there (818ed600's build, timed by parts,
`apply_join_laps3.py`): 12.3 ms (L7, 46 joins) to 15.3 ms (L13), the slowest 21.5, of it 7.8-10.4 ms the three
snapshots of every object id - `ck_object_ids` took `GetObjectListByType(CKCID_OBJECT, derived)`, an array of
every object's pointer, and read each of the ~100k objects for its id; the join fix had only replaced what the ids
were put into. Now the ids are read from CK's per-class id lists (`GetObjectsListByClassID`, every class under
CKCID_OBJECT; what that call is built from, and what the product reads elsewhere on both engines), the snapshot
is a bitmap of them, and the two later passes of a copy ask for the ids added since (`added`: one pass against the
bitmap, sorted, each once) instead of taking a snapshot each and filtering it. The loops in `trafo_sets.hpp` and
`sector_placement.hpp` get the same ids in the same order.

| 8845HS, one core | before (818ed600) | now |
|---|---|---|
| a join, L7 s2 / L13 s2 (mean; slowest) | 12.3 / 15.3 ms; 21.5 | 5.2 / 6.4 ms; 14.3 |
| of it the three passes over the ids | 7.8 / 10.4 ms | 1.0 / 1.5 ms |
| L7 s2 (21 players): ticks over 15.15 ms, slowest tick (two runs) | 3, 22 ms | 0, 9-11 ms |
| L8 s1 (20 players) | 2, 19 ms | 0, 10-12 ms |
| L13 s2 (12 players) | 4, 17 ms | 0, 12-13 ms |
| L6 s1 (14 players) | 2, 18 ms | 0, 10-14 ms |

What a join still costs: CopyObjects 1.7-1.9 ms, the copies found again by name (I4) 1.0-1.2, the digest 0.55, the
bridge's registration 0.3-0.5 (once 5.8). Every tick of the four replays has the previous build's world and pose
hash (314 496 ticks, `rw_same.py`). Not in a replay: the resync states the public server built (2211; 3.9-7.4 ms +
coding on a P-core since b14dc6b0, not timed here) and the other rooms the runner's one thread steps - a state
built in a join's tick is not covered by these figures.

**The client's baseline** (the same build, the kept source on, client 0 alone on a core, the whole recipe, 33 232
frames; `apply_slow_diag.py`, `slow_frames.py`): frame p50 5.0, p99 23.1, p999 27.6, max 41.9 ms; 4872 frames
(14.7 %) over 15.15 ms, on average 18.5 ms = restore 4.7 + replay 10.4 (10.5 ticks) + the live capture 1.8 +
the rest 1.7. The slowest are the start (ticks 232-235, 30-42 ms: 7-10 replayed ticks at 3-4 ms each while the 13
balls are born), frames near a trafo explosion (31-39 ms) and the deepest steady rewinds (17-18 ticks, 28-30 ms).
By parts on the same machine (13 clients sharing it, `copy_split.py`): a restore 4.7 ms, 3.8 of it the script
write; a replayed tick 0.99 ms = its captures 0.42 + the probes' placement 0.35 + step and pre-step 0.13 + the
rest; a whole record 2.1-2.5 ms. At 18 ticks the sum fits 15.15 ms only with the probes' placement down ~85 %, the
whole record halved again and the restore's script write down ~80 % (about 13.8 ms then), the start and the
explosions apart.

Gate (the id lists): 845/845; fast LBM 38/38 twins FINAL exact, every criteria row equal to the resync cut's run
(before the review's changes and after); x64 and x86 builds, the Arch server; retail at K = 20, L11 and L12 at human key rates: both clients' FINAL
rows bit-equal to the kept source's sessions with L11's 32 trafo pieces, the same `I0: copied 120 objects` lines
and the same 53 placement copies object for object; two serial reviews, no defects (the ids once each and the
interface's leftovers are theirs).

## Built (the restore skip for roots and dormant trafo scripts, and the dormant ledger's fault, 2026-11-11)

**A script restore by parts** on the public server's machine (the public L6 re-drive, 13 players, K = 20;
`work/PERF/apply_rs_diag.py`, the client's `rs_split:` line; ~4.2 ms a restore with the timers in): the selected
roots 2.46 ms - their parameters 1.65 (15.8 k parameters, 168 KB a restore), their activation flags and IO 0.67,
the force tails 0.06, the omitted copies 0.18, the links 0.10 - the trafo section 1.61 ms (13 members x 8 scripts,
each rewritten with its built bytes at every rewind though almost always dormant), the hold, cells and queue 0.17.

**Built** (`game/script_replay.hpp` restore, `game/script_slots.hpp`; under `BMMO_RESTORE_SKIP`, with its verify).
Two writes of bytes a script holds already are spared. A selected root that has been quiet since the kept source
was taken, and is at the restore's start, whose block in the kept source is the record's byte for byte
(`root_unchanged`): its parameters and activation words are not written (`restore_behavior(..., same)`). A trafo
script the record holds as built that the dormant ledger knows to hold its built state, with the capture's own
guard on the root's words (`trafo_built_already`): likewise. Everything else a restore does for such a script it
still does - the force and joint tails, the omitted copies, the delayed links (a pending one re-armed, and counted
inexact, as ever). Counters on the `slot_memo` line (`roots=skipped/written trafo=skipped`).

**The ledger's fault, found by verify.** Over the fast LBM in verify, 16 trafo candidates (the arms with a ball
transformation and a death) differed from their built bytes though the ledger said built and the root's words were
the built ones: `Ball_Explosion_Wood`'s local "actual Group" held a group's id. A script of a set that runs
writes a sibling through a parameter the two share, and the ledger left only the script that ran. The capture
trusted the same ledger, so the records had claimed "built" for such a script, and every rewind wrote the built
bytes over it - a fault that predates the skip and that the unskipped restore hid. Now a script that runs takes its
whole set (the scripts of its owner) out of the ledger (`trafo_part::touch_set`: the trafo frame and a remake), an
entry is cleared before its script is written and set only when the restore wrote it exactly
(`restore_counters::inexact`). The same arms then verify at 0.

**Measured** on the 8845HS (idle; HEAD against this, both built from one tree by one toolchain, two interleaved
pairs that agree; 13 clients on its 16 threads): a restore 4.68 -> 3.41 ms; but the replay outside its captures
0.54 -> 0.61 ms a tick (a rewind's replay 8.0 -> 8.7 ms), so rewind work 308 -> 292 ms a second, frame p99
24.0 -> 23.3, p999 28.8 -> 28.1 ms, the max unchanged (44-46). Per restore 71 % of the roots and 59 % of the trafo
scripts are left. Why the replay costs more after a restore that wrote less is not known yet (the first reading,
the earlier build against an older binary, showed the same); it is the next thing to split.

**Owed, from the review.** Every reuse here - step OPC, the kept source, the slot memo, this skip - rests on "a
root that is not executed keeps its bytes"; the ledger's fault is that premise failing inside a trafo set. Nothing
checks that no other root's output or operation feeds a parameter of a selected root (the trafo sets have I2 for
their own links); the verify runs have met none in the game's levels. A scan at selection that takes such a root
out of every reuse is the next step.

Gate: 845/845; the L6 re-drive in verify (the three switches), 7.51 M skip candidates, 3.65 M copies and 16.4 M
memo hits walked again, 0 mismatches; fast LBM verify 38/38 twins FINAL exact, every criteria row equal to the
resync cut's run, 80 k skip candidates and 6.37 M copies, 0 mismatches; fast LBM 38/38, the same rows; retail at
K = 20 (L12, L11 at human key rates): verify 84 k skip candidates, 0 mismatches, FINAL rows of the shipped and the
verify sessions bit-equal to the kept source's sessions with L11's 32 trafo pieces; x64 and x86 builds, the Arch
server; two serial reviews and a re-review (the link re-arm under the skip, the trafo tails, the entry cleared
before a write, the remake and the exactness of an entry are theirs).

## Built (the union's candidates by id, 2026-11-11)

**A place by parts** on the public server's machine (the public L6 re-drive, 13 players, K = 20;
`work/PERF/apply_pp_diag.py`): 0.375 ms a place - the 13 candidates found by name 0.22 (each a scan of every 3D
entity, `GetObjectByNameAndParentClass`), the identity cells 0.115 (300 cells), the frames 0.04 (329 probes). A
replayed tick places once, so of a replayed tick's 0.99 ms the place was 0.35.

**Built** (`game/proximity_union.hpp` `candidate_entity`; under `BMMO_LOOKUP_CACHE`, with its verify). The union
keeps name -> CK id for its candidates, by `physics_state.cpp find_entity`'s rule: a name is entered when one 3D
entity alone has it at the fill (the scan resumed after the first finds no other), a hit is taken while the id
still names a 3D entity of that name, anything else scans and fills again. A name shared at the fill is scanned
every time until the candidates change. The premise is `find_entity`'s too: no second entity of a candidate's name
appears before the first in the scan's order - the balls' and the clones' names are their own. Verify scans every
hit as well, returns the scan's answer and counts a difference. The line `proximity union lookups: hits= scans=
verified= mismatches=` is in the headless client's and the server's logs, the retail log and `session probes`.

**Measured** on the 8845HS (idle; 02ea2898 against this, both built from one tree by one toolchain, two
interleaved pairs that agree; 13 clients on its 16 threads): the replay outside its captures 0.61 -> 0.35 ms a
tick, a rewind's replay 8.7 -> 6.6 ms, rewind work 291 -> 241 ms a second, frame p50 5.1 -> 4.8, p99 23.2 -> 19.1,
p999 28.3 -> 25.8, max 43.6 -> 41.0 ms; 99.8 % of the lookups are hits (868 k of 869 k on client 0). With client 0
alone on a core (the baseline's recipe, 33 232 frames): p50 5.0 -> 4.6, p99 23.1 -> 18.0, p999 27.6 -> 21.8, max
41.9 -> 37.7 ms against the baseline of a1b35f80 (the restore skip is in that difference too).

Gate: 845/845; the L6 re-drive in verify, 4.96 M hits each scanned again, 0 mismatches, every session's `mism` 0;
fast LBM in verify 38/38 twins FINAL exact, every criteria row equal to the resync cut's run, 2.59 M hits scanned
again, 0 mismatches; fast LBM 38/38, the same rows (comments only changed in `proximity_union.hpp` after these
runs; the retail log line was added after them); retail at K = 20 (L12, L11 at human key rates) in verify: 28.7 k hits of the two games each scanned again against 90 scans, 0 mismatches, and FINAL rows of the shipped and the verify sessions bit-equal to the restore skip's sessions with L11's 32 trafo pieces; x64 and x86 builds, the Arch server; two serial reviews,
no defects (the comments' wording and the retail run are theirs).

## Built (the fed roots: the reuse premise checked, 2026-11-11)

**The premise.** Step OPC, the kept source, the slot memo and the restore skip all take a record's bytes for a
root script that did not run: "a root that is not executed keeps its bytes". The dormant ledger's fault (above) was
that premise failing inside a trafo set. The engine has one way around it that a graph shows: a `CKParameterOut`
copies its value into its destinations whenever it is written. A block's output is written when its script runs,
so a destination in a block of another root script changes that script without its running; a parameter
operation's output is written whenever anything reads it, so its destination changes its script without its
running even when that script owns the operation (the review's case).

**Built** (`game/script_feeds.hpp`; `script_replay::selection::fed`). `script_feeds` lists every such link;
`select` keeps the selected roots that are a target. A fed root is walked by every capture (`fed=` on the
`script_reuse` line counts the walks), is observed by the quiet-run ledger as absent - so it has no run, no memo
under it serves and the restore skip never spares it - and a restore never counts it as held exactly. The trafo
part's scripts are not in it: their links stay inside a set (I2) and a set leaves its ledger together
(`touch_set`). Not seen by a graph, and so not covered: a building block that writes through an input's source at
run time. SimTool `--script-feeds` prints the links of a level; the harness has `--trafo-inputs off` (the sessions
before the trafo sets, where the original trafo scripts are selected).

**Found.** In each of the 13 levels the same 20 links: 17 among the gameplay scripts (none selected) and three
among the original trafo scripts - `Ball_Explosion_Paper` and `Ball_Explosion_Stone` write `Ball_Explosion_Wood`'s
"actual Group" (the write the dormant ledger's fault came from, in the sets' copies), `Ball_ResetPieces_Paper`
writes `Ball_Explosion_Paper`'s wind switch. No operation's output has a destination. No mechanism script and no
sector manager is fed, so in a session with trafo inputs (the default) the set is empty and nothing changes; with
them off two selected roots are fed and are walked.

**The restore skip's replay cost, closed.** The restore skip had left "the replay outside its captures 0.54 ->
0.61 ms a tick" unexplained. Numbering the trafo sets once per part instead of asking every script's owner per
executed script (`touch_set`) changed nothing measurable (0.335 against 0.333 ms a tick, two pairs) and was
dropped. One binary with `BMMO_RESTORE_SKIP` off and on (two interleaved pairs on the 8845HS) settles it: with the
skip a restore is 5.09 -> 3.40 ms, a replayed whole record 2.14 -> 1.94 ms, the replay outside its captures
0.305 -> 0.340 ms a tick, a rewind's replay unchanged (6.48 ms); rewind work 277 -> 238 ms a second, frame p99
21.1 -> 18.9, p999 27.6 -> 25.0 ms. The extra 0.035 ms a tick is the skip's own - memory the restore no longer
touches is first touched by the replay - and is paid back by the cheaper records.

**Where the frames over 15.15 ms are now** (the 8845HS, client 0 alone on a core, 33 232 frames, this build): 1606
frames (4.8 %; 14.7 % at the baseline), p50 4.6 / p99 17.9 / p999 22.0 / max 37.7 ms. 1596 of them hold one
rewind: work 17.0 ms = restore 3.4 + replay 10.2 (11.5 ticks) + capture 1.8 + rest 1.5; by depth 4-7: 110, 8-11:
783, 12-15: 508, 16-19: 193. Ten hold two rewinds (a birth's and an input's), 24.8 ms on average and four of the
five slowest after the start. The slowest three are the start's (ticks 232-235, 33-38 ms, 3 ms a replayed tick:
the births); then the frames after a birth or a trafo explosion (27-31 ms, 1.2-2 ms a replayed tick while the
window holds the birth).

Gate: 845/845; with trafo inputs off (two LBM arms with a ball transformation, in verify) ~7960 fed walks a client,
0 mismatches, 4/4 twins FINAL exact; the L6 re-drive in verify (the four switches), 7.02 M copies, 31.1 M memo
hits, 15.1 M skip candidates and 10.6 M lookups checked again, 0 mismatches, every session's `mism` 0; fast LBM in
verify 38/38 twins FINAL exact, every criteria row equal to the resync cut's run, 6.37 M copies, 0 mismatches; fast
LBM 37 of 38 with equal rows - `a6m_superseded_on` left no summary (its server did not exit within the harness's 60
s, its four clients at `mism` 0; the second such exit today) and was exact with an equal row when run again; retail
at K = 20 (L12, L11 at human key rates) in verify, 619 k copies and 3.48 M memo hits checked again, 0 mismatches,
`fed=0`, FINAL rows of the shipped and the verify sessions bit-equal to the restore skip's sessions with L11's 32
trafo pieces; x64 and x86 builds, the Arch server; two serial reviews (the operation's case, the comments, the tool
printing the selection's own set are theirs).

## Built (the copy plan: a copy without the graph, 2026-11-11)

**What a copy still cost.** A root the capture copies (step OPC, the kept source) was still walked in step with its
source's bytes, block by block, to find the few bytes it reads fresh - a force or joint block's tail, the slot image
section of a block with an omitted copy - and to check the structure: ~0.6 ms of a 1.9 ms whole record on the
public server's machine (the public L6 re-drive, 13 players, K = 20).

**Built** (`script_slots::plan`, `script_replay` `plan_recorder` / `emit_plan`; `BMMO_COPY_PLAN` off | on, on when
unset, with the kept source). The first copy of a root by an observing capture leaves a plan: the bytes it copied,
end to end, and after how many of them which block's fresh part was emitted. The next copies of that root append
those bytes and make the same `put_force_and_joint` / `put_omitted_images` calls in the same order - no source
bytes, no graph. A plan serves under the memo's rule (the host-write generation it was stored under, the root quiet
since) and the same slot mask; a copy block added drops every plan; a restore that left the root as it was, and
found it exact, keeps its plan (`keep_plan`, `restored`). A plan checks no structure: it rests on every change of a
selected root's graph bumping the host-write generation or going through `add()` (the proximity union's rewiring
and the placement's copies do); `emit_plan` only checks that a part's block is there and still of its kind, and
walks otherwise. `BMMO_SCRIPT_REUSE`'s verify walks every root a plan emitted and compares. Counters on the
`script_reuse` line: `plans: emitted= kept= none= gen= mask= run=`.

**Measured** on the 8845HS (one binary, `BMMO_COPY_PLAN` off / on, two interleaved pairs that agree; 13 clients on
its 16 threads): a replayed whole record 1.92 -> 1.37 ms, the live capture 2.22 -> 1.61 ms a frame, a rewind's
replay 6.41 -> 5.31 ms, rewind work + capture 383 -> 316 ms a second, frame p50 4.75 -> 4.0, p99 18.7 -> 16.7,
p999 25.0 -> 23.5 ms; 99.7 % of the copies come from a plan. With client 0 alone on a core (33 232 frames): p50
4.6 -> 4.0, p99 17.9 -> 16.9, p999 22.0 -> 20.2 ms, frames over 15.15 ms 1606 -> 1173 (3.5 %).

**The slowest frames, read again** (the same run with every rollback hook timed, `work/PERF/apply_hooks_diag.py`).
The eight slowest frames of the start (ticks 228-235, 25-37 ms) are not the product's: the headless client's
savestate text diff (`world.diff_savestate`, its first eight differing replays; the mod has none) runs in them -
the ninth such frame, twelve ticks deep, is 12.8 ms. After the start the slowest (23-29 ms) hold five to seven
whole records (5-10 ms), two of them two rewinds. An ordinary frame over the budget (690 of them, 16.6 ms,
12.3 ticks): script restore 2.5 ms, place_probes 2.3, pre_step 1.5, the savestate's capture 1.0 and restore 0.7,
step 0.9, the rest captures and the live frame. A script restore by parts (`work/PERF/apply_rs_diag3.py`): 2.94 ms
= the 22 roots the skip leaves, still walked for their tails, 1.16 + the trafo section 1.17 + the 9 roots written
0.27 + hold, cells, queue and anchoring 0.2.

Gate: 845/845; the L6 re-drive with BMMO_SCRIPT_REUSE and BMMO_SLOT_MEMO in verify and the restore skip on, 7.02 M
copies walked again of which 6.99 M came from a plan and 1.60 M plans kept across restores, 0 mismatches, every
session's `mism` 0; fast LBM the same way 38/38 twins FINAL exact, every criteria row equal to the resync cut's
run, 6.37 M copies walked again (6.36 M from a plan, 20 k plans kept), 0 mismatches; fast LBM with the four
switches in verify 38/38, the same rows, 0 mismatches (there no plan outlives a restore: the restore skip's verify
leaves no root as it was); fast LBM 38/38, the same rows; retail at K = 20 (L12, L11 at human key rates), verify
with the restore skip on: 618 k copies walked again (617 k from a plan, 11.5 k plans kept) and 3.54 M memo hits, 0
mismatches, FINAL rows of the shipped and the verify sessions bit-equal to the restore skip's sessions with L11's
32 trafo pieces; x64 and x86 builds, the Arch server; two serial reviews, no defects (the premise written at the
plan, the check of a tail part's block, one rule for a block with an omitted copy, a plan kept only for a root
still exact are theirs).

## Built (the restore plan: a restore without the graph, 2026-11-11)

**What a restore still cost.** The restore skip leaves a quiet root's parameters and a dormant trafo script's as
they are, but each such script was still walked block by block: a force or joint block's tail, a block's omitted
copies and a delayed link are put back in any write. Of a 2.94 ms script restore on the public server's machine (the
public L6 re-drive, 13 players, K = 20) that walk was 1.16 ms for the 22 roots the skip leaves and most of the trafo
section's 1.17 ms.

**Built** (`script_replay` `restore_block`, `locate_parts`, `restore_parts`, `trafo_plan`;
`script_slots::plan_at_restore`; `BMMO_RESTORE_PLAN` off | on | verify, on when unset, with the restore skip). A
script the skip leaves goes by a plan when it has one that held at the restore's start: a root's copy plan (stored
under the restore's host-write generation and the record's slot mask, the root quiet since), a trafo script's plan
over its built bytes (made once, at the first restore that leaves the script as it is, by the copy's own walk with
its output dropped and without the capture hooks; kept with the part - the built bytes never change). The plan's
copied bytes are compared with the record's block on the way (`locate_parts`), each fresh part is found where the
plan says, and the parts are put back by the walk's own two functions (`restore_tail`, `restore_omitted`, taken out
of `restore_behavior`). The graph is not walked. Anything else walks as before: bytes that are not the plan's, a
part whose block is gone or no longer of its kind, a plan with a delayed link in flight in its bytes
(`plan.pending`: the walk re-arms such a link, a plan handles none). A restore that left a root as it was keeps its
copy plan only when that plan held at the restore's start (`keep_plan`). With the switch off no plan is looked up
or made. Verify walks as ever, compares where the walk and the plan found the fresh parts, and reports a delayed
link the walk re-armed or found changed. Counters on the script_reuse line: `restore_plan: mode= roots= trafo=
checked=`.

**The start's diagnostic frames** (`BMMO_REPLAY_DIFF=off`, the headless client). The eight slowest frames of a run
(ticks 228-235, 25-37 ms) were the headless client's savestate text diff before a restore, which the mod does not
have. The switch drops it for timing runs; sparse capture's verify compares the blobs byte for byte anyway. The
numbers below are with it off, so the max is the first one that is the product's.

**Measured** on the 8845HS, with the build before the review's changes (after it a trafo plan is made without the
capture hooks and no plan is looked up with the switch off; the restore's path with the switch on is the same). One
binary with the switch off / on, two interleaved pairs that agree, 13 clients on its 16 threads, the diagnostic off:

| | off | on |
|---|---|---|
| a restore | 3.37 ms | 2.49 ms |
| rewind work a second | 211 ms | 198 ms |
| frame p99 | 16.85 ms | 16.25 ms |

Client 0 alone on a core (33 232 frames), the copy plan's build against this:

| | the copy plan | the restore plan, diagnostic off |
|---|---|---|
| frames over 15.15 ms | 1173 (3.5 %) | 618 (1.9 %) |
| p50 / p99 / p999 | 4.0 / 16.9 / 20.2 ms | 4.0 / 16.0 / 19.3 ms |
| max | 36.9 ms (a diagnostic frame) | 31.7 ms |

**Half of what the walk cost.** The trafo section did not fall as the roots did: of the 104 trafo scripts ~44 are
not left by the skip at a restore, and each of those is written whole and walked whole by every whole capture.
Whether they hold other than their built bytes for good once a ball has changed type, or their set was touched
inside the window, is not established yet. Bringing them under a rule like the roots' ("not run since the record,
so the record's bytes") is the next step on both sides.

**Where the frames over the budget are now.** A frame over the budget is 16.5 ms: restore 2.65 + replay 10.9 over
12.8 ticks + capture 1.37 + rest 1.6. Of the five slowest frames four hold two rollbacks (25-32 ms, 16-20 ticks
replayed in all); the others are frames after a ball's explosion with its pieces alive (1.3-1.8 ms a replayed
tick).

Gate: 845/845; the L6 re-drive with `BMMO_RESTORE_PLAN`, `BMMO_SCRIPT_REUSE` and `BMMO_SLOT_MEMO` in verify and the
restore skip on, 5.93 M scripts walked again beside their plan and 7.02 M copies, 0 mismatches, every session's
`mism` 0; the same re-drive as shipped, 1.60 M roots and 4.33 M trafo scripts put back by a plan, `mism` 0; fast LBM
the first way 38/38 twins FINAL exact, every criteria row equal to the resync cut's run, 68.7 k scripts walked again
beside their plan and 6.37 M copies, 0 mismatches; fast LBM with the five switches in verify 38/38, the same rows,
80 k skip candidates and 6.37 M copies, 0 mismatches (there no script goes by a plan: the restore skip's verify
leaves none as it was); fast LBM 38/38, the same rows, 20.3 k roots and 48.3 k trafo scripts by a plan; retail at K
= 20, L12 and L11 at human key rates, verify with the restore skip on, 13.8 k scripts walked again beside their
plan, 617 k copies and 3.54 M memo hits, 0 mismatches; as shipped 11.4 k roots and 2.5 k trafo scripts put back by a
plan; FINAL rows of the shipped and the verify sessions bit-equal to the restore skip's sessions with L11's 32 trafo
pieces; x64 and x86 builds, the Arch server; two serial reviews, no defects (verify's note of a delayed link the
walk handled, a trafo plan made without the capture hooks, no plan looked up or made with the switch off are
theirs).

## Built (the held trafo scripts: a trafo script that did not run keeps its bytes, 2026-11-12)

**What the trafo section still cost.** The restore plan left it at half: of the 104 trafo scripts of the public L6
re-drive (13 players, K = 20) the skip left 59 at a restore and 45 were written whole, and every whole capture
walked ~44. A census (counters only, on the public server's machine and in retail) says what the 45 are: 98.3 % of
them already hold the record's bytes, tails apart, and nothing ran them or a script of their set since the last
restore. They are the scripts whose state is not the built one for good once a ball has changed type -
Ball_PaperWind and Ball_ResetPieces_Paper / _Stone / _Wood, one of each a player - so the dormant ledger, which
knows only "built", never has them. Their walk was 0.544 ms of a 1.37 ms whole record and most of what the restore
plan left of the trafo section.

**Built** (`trafo_part::held`, `script_replay` `hold_trafo`, `trafo_held_already`, `listening`;
`BMMO_TRAFO_HELD` off | on | verify, on when unset). Beside each dormant entry the ledger keeps a plan over the
script's last known bytes when they are not the built ones. Every `set_dormant` drops it, so it stands under the
dormant entry's own rule: the script and every script of its set have not run, and nothing wrote it, since. It is
set in two places only, after a capture walked the script and after a restore wrote it exactly, and only at the
second such finding in a row (a script that runs every tick never pays for a plan). A capture then emits the
script's block from the plan (`emit_plan`, the copy plan's function: the copied bytes, the fresh parts read), under
the capture's guard on the root's activation words; a block that reads as the built state is recorded as built,
the walk's rule. A restore leaves such a script as it is when the record's block has the plan's copied bytes
(`locate_parts`, which now also says where the block ends) and puts its fresh parts back by the restore plan's
path. Anything else walks as before: the record's block has other bytes, a delayed link in flight in the plan, the
guard. With the switch off no plan is made or looked up. Verify walks as ever and compares - a capture's block with
the plan's, a restore's script with the record before it is written - and counts a difference with the memo's or
the restore skip's mismatches. Counters on the slot_memo line: `trafo_held: mode= set= captured= restored= other=
pending= guard= gone= checked=` (`restore_plan`'s `trafo=` counts the held scripts a plan put back too).

**What the rule rests on, and its two exceptions.** "Not run, so the same bytes" holds for a trafo script as for a
root, with its set in place of the script: a script of a set that runs writes its siblings through parameters they
share (the dormant ledger's fault), so a run takes the whole set out of the ledger (`touch_set`), and nothing
outside a set feeds its scripts (the fed roots' census). What is left is a writer that changes a block without any
script running. Two such writers were found and closed. (1) `unmake_type_set` puts a set's
scripts back to their built bytes outside the frame: it now takes the script out of the ledger first
(`trafo_part::touch`). This is not behind the switch, so a session with the switch off is not byte for byte the
restore plan's after an unmake; the records agree (the gate's rows). (2) A Physics Collision Detection block with a
live listener has its outputs written, and its output activated, by the physics step: a script holding one gets no
plan (`listening`; a listener is made only by the block's execution, which clears the entry, so a script found
without one has none while its entry stands). The frame's end clears a held entry like a dormant one when the
script is left with a frame flag. A guard that trips clears the entry and is counted apart (`guard=`) from a part
whose block is gone (`gone=`); both are 0 in every run below.

**Measured** on the 8845HS (the public L6 re-drive, the text-diff diagnostic off). One binary with the switch off /
on, two interleaved pairs that agree, 13 clients on its 16 threads:

| | off | on |
|---|---|---|
| frame p50 | 4.0 ms | 3.5 ms |
| frame p99 | 16.1-16.3 ms | 14.4-14.7 ms |
| the live capture a frame | 1.63 ms | 1.18 ms |
| a replayed whole record | 1.39 ms | 0.97 ms |
| a restore | 2.49 ms | 1.98 ms |
| a replayed tick | 0.654 ms | 0.58 ms |
| rewind work + capture a second | 306 ms | 249 ms |

Client 0 alone on a core (33 232 frames), the restore plan's build against this:

| | the restore plan | the held trafo scripts |
|---|---|---|
| frames over 15.15 ms | 618 (1.9 %) | 115 (0.35 %) |
| p50 / p99 / p999 | 4.0 / 16.0 / 19.3 ms | 3.4 / 13.7 / 17.3 ms |
| max | 31.7 ms | 30.3 ms |

(The build before the reviews' changes measured 124 frames, 3.4 / 13.8 / 17.6 / 31.9 ms.)

Per restore 59 trafo scripts are left as built, 43.5 by a held plan and ~1 is written; a capture walks ~1 trafo
script where it walked ~44.

**Where the frames over the budget are now.** Of the 115 frames, 105 hold one rollback: 16.8 ms = restore 2.5 + replay 11.6
over 12.7 ticks + capture 1.1 + rest 1.6, the slowest 23.6 ms. Ten hold two rollbacks: 22.2 ms on average, 14.5
ticks replayed in all, the slowest 30.3 ms - the run's max. 76 of the 115 are in the first 50 s, where the players
are born and change type: a replayed tick of an over-budget frame is 0.92 ms against 0.58 on average. So what is
left is depth (a rollback of 12-13 ticks is over the budget by itself at 0.9 ms a tick), two rollbacks in one frame,
and the ticks with pieces alive - not the scripts' record any more.

Gate: 845/845; the L6 re-drive with `BMMO_TRAFO_HELD`, `BMMO_RESTORE_PLAN`, `BMMO_SCRIPT_REUSE` and
`BMMO_SLOT_MEMO` in verify and the restore skip on, 16.8 M held scripts walked again (13.6 M blocks a capture
emitted from a plan, 3.17 M restore candidates), 7.02 M copies and 40.2 M memo hits, 0 mismatches, every session's
`mism` 0; the same with `BMMO_TRAFO_HELD` on (verify of the held switch makes the restore write, so this is the run
that checks a held script put back by its plan), 9.10 M scripts walked again beside their plan of which 3.17 M
held, 0 mismatches, `mism` 0; the same re-drive as shipped, 3.17 M held scripts put back by a plan, 2568 refused
for other bytes, none for the guard, a gone part or a pending link, `mism` 0; fast LBM the first way 38/38 twins
FINAL exact, every criteria row equal to the resync cut's run, 1.29 M held scripts walked again, 0 mismatches; fast
LBM the second way 38/38, the same rows, 72.6 k scripts walked again beside their plan, 0 mismatches; fast LBM with
the six switches in verify 38/38, the same rows, 80.0 k skip candidates and 1.29 M held scripts, 0 mismatches; fast
LBM 38/38, the same rows, 1.28 M blocks emitted from a held plan; retail at K = 20, L12 and L11 at human key rates,
verify with the restore skip on 66.8 k held scripts walked again, with the held switch on 14.7 k scripts walked
again beside their plan (790 held), 0 mismatches, as shipped 66.2 k blocks from a held plan and 792 held scripts
put back, FINAL rows of the three ways bit-equal to the restore skip's sessions with L11's 32 trafo pieces; x64 and
x86 builds, the Arch server; two serial reviews, no defects - their risks are the two exceptions above, the built
encoding, the counters and the gate's second way, and the gate ran on the tree after those changes.

## Built (the slot cells: a place writes a slot's identity only when its candidate changed, 2026-11-12)

**Where an over-budget frame's time was.** With every rollback hook timed on the public server's machine (HEAD
46bebee0, client 0 pinned), frames holding one rollback: a deep frame UNDER the budget (963 frames, 14.0 replayed
ticks) is 11.9 ms = restore 1.92 + replay 7.82 + capture 0.79 + rest, the replay being whole records 2.15 (2.2
records of 0.98 ms) + place_probes 2.02 (0.145 ms a tick) + pre_step 1.35 (0.096) + step 0.79 (0.056) + the rest; a
frame OVER it (65 frames, 12.7 ticks) is 16.95 ms: whole records 4.06 (2.7 records of 1.5 ms: the script capture
0.98 ms a call against 0.63), place_probes 2.57, pre_step 1.80, step 1.34, restore 2.45. So even a frame that fits
spent 3.4 ms in the two per-tick hooks.

**What they did** (profiled on the L6 re-drive, the dev machine, per replayed tick). place_probes 114 us: the
candidates 12, their lookup 5, the probe frames 17 (they follow the balls: 11.08 M writes of a new value against
700 of the same), and the identity cells 78 - of which 67 are the 300 slot reads of a room of 13 at K = 20, each
writing its slot's cell and checking its latch, and 15.12 M of 15.13 M cell writes stored the value the cell
already held. pre_step 58 us: the replayed script frame 34 (the selected scripts run: real work), the own entities'
matrices 18 (the headless host found each entity by a scan of every 3D entity), the rest 6.

**Built.** `BMMO_SLOT_CELLS` (off | on | verify, on when unset; `proximity_union::place`): a slot read's cell and
latch are a function of its slot's candidate alone, so the slot pass is skipped while every slot has the candidate
of the last full pass (by CK id and name: a by-name cell takes the candidate's type from its name) and the
script-write generation is the one that pass ended with. The cells are the union's own (only place, the rewire and
the release write them; a rewire or release clears the state); a latch is a Get Cell's output, written by its own
block - which reads the cell, so writes the value it holds - and by a script restore or a host write into the
scripts, which bump the generation (a restore bumps it before it writes, so the first replayed tick after a restore
runs the full pass). A by-name cell's value is the retail object named after the candidate's type, taken to be the
same object while the rewire stands. Verify runs every pass and counts a pass the guard would have skipped that
changed a cell or moved a latch. `BMMO_MATRIX_LOOKUP` (off | on | verify, on when unset; the headless client's
`matrix_entity`): name -> CK id for entity_matrix / set_entity_matrix by candidate_entity's rule, cleared at the
rewire and the release; the retail host's two calls still scan. Counters: `; slot cells: skipped= full= verified=
mismatches=` on the union's lookups line, `own matrix lookups: hits= scans= verified= mismatches=` at the headless
session's end.

**Measured** on the 8845HS (the public L6 re-drive, the text-diff diagnostic off). One binary with the two switches
off / on, two interleaved pairs that agree, 13 clients on its 16 threads:

| | off | on |
|---|---|---|
| a replayed tick, outside its captures | 0.359-0.378 ms | 0.260-0.268 ms |
| a rewind's replay | 4.98-5.19 ms | 4.14-4.22 ms |
| rewind work a second | 170-173 ms | 147 ms |
| frame p50 | 3.5 ms | 3.3 ms |
| frame p99 | 14.2-14.6 ms | 12.3-12.6 ms |

Client 0 alone on a core (33 232 frames), the held trafo scripts' build against this:

| | the held trafo scripts | the slot cells |
|---|---|---|
| frames over 15.15 ms | 115 (0.35 %) | 37 (0.11 %) |
| p50 / p99 / p999 | 3.4 / 13.7 / 17.3 ms | 3.2 / 11.5 / 15.6 ms |
| max | 30.3 ms | 25.9 ms |

**Where the frames over the budget are now.** All 37 are in the first 50 s (before tick 3300), where the players
are born, die and change type: after that the session has no frame over a tick. 28 hold one rollback (17.1 ms =
restore 2.9 + replay 11.7 over 11.3 ticks + capture 1.2 + rest 1.3: 1.03 ms a replayed tick), nine hold two (21.2
ms, the slowest 25.9 - the run's max).

**Studied the same day, not built.** (1) Depth through the relay: the server takes a receive pass every 15.15 ms
and builds hints only in its step, which waits for the slowest member's frame; a hint waits ~0.9 tick there. A
1 ms idle receive pass (server only) is worth ~0.5 tick on every relayed frame, a hint-only send at arrival ~0.45
more. (2) The two rollbacks of one frame are always a late lifecycle batch (3-6 ticks) and then a remote input
(deeper), in the start pile; merged at the final flush under a narrow predicate they would save one restore and
4.3 ticks. (3) A running trafo script takes its whole set of eight out of the ledger every tick (touch_set), so
seven siblings are walked by every capture to be found unchanged (about half of the trafo walks); narrowing the
touch to the script and its feed targets needs the feeds enumerated with parameter operations and out-to-out
chains, and a gate arm that fires each feed. (4) Whole records every 4 ticks instead of 8: no better at today's
costs (the sparse model).

Gate: 845/845; the L6 re-drive with the two switches and BMMO_LOOKUP_CACHE in verify, 768 k skipped passes and
102 k matrix hits checked again, 0 mismatches, every session's `mism` 0; the same as shipped, `mism` 0 (the agent's
runs: every client's and the server's journal byte-equal with the switches off and on); fast LBM in verify 38/38
twins FINAL exact, every criteria row equal to the resync cut's run, 731 k skipped passes checked again, 0
mismatches; fast LBM 38/38, the same rows; retail at K = 20, L12 and L11 at human key rates, in verify 19 k and
48 k skipped passes checked again, 0 mismatches, as shipped the same; FINAL rows of the four sessions bit-equal to
the restore skip's sessions with L11's 32 trafo pieces; x64 and x86 builds, the Arch server; one review, no defects
(its points: the by-name premise stated, the switch read once, the matrix cache cleared per level).

## Built (the replay's last tick recorded light, behind a switch, 2026-11-12)

**What it was.** A replay recorded its last tick whole (SRT4's rule: `t == current_tick`), and the frame's live tick
is recorded whole right after, so a rollback frame paid two whole records back to back. On the public L6 re-drive
(13 players, K = 20) the replays made 1.67 whole records a rewind, one of them the last tick's.

**What reads that record** (rollback.hpp unless named). (1) The live capture that follows: never as "t - 1" - the
restore's barrier bumps `engine_writes_` on the way out (`restore_and_replay`, `write_barrier_on_exit`), so
`capture`'s cache and OPC's source test (`it->write_generation == engine_writes_`) refuse it. Its script reuse
copies from the kept source (`script_replay` capture: `kept_source(writes)`), which a whole capture sets
(`keep_source` at the capture's end) and a restore sets (`script_slots::restored`); with the last tick light the
source is the last whole replayed record or the restore's, and the ledger, which light records observe
(`observe_scripts`, `observe_roots`), says whether a root stayed quiet since. A root that ran in between is walked
by the live capture instead of by the last tick's - one walk either way. A host write in between (a relatch, a
sector pass) leaves the older source on another generation and the live capture walks: the captures with no source
go 1061 -> 1086 over the run's 13 clients (a net figure over every capture), and the slowest live capture of a
client (`t_cap`'s max) is 2-16 ms in both arms, no pattern between them. (2) The replay's own check at its last tick: the re-executed scripts against
the live record's (`script_mismatches` / `script_float_mismatches`, the status line's `scr=`) and the blob's
capped diff (`savestate_replay_mismatches`) need a whole capture; they are diagnostics no gate reads (with
snapshots off, `metrics.mism` is `stats_.mismatched`, counted only by `on_snapshot`). On the re-drive they counted
422 logical and 5682 float differences, by design (a replay that corrects an input differs from the live record);
light, 24 and 787 (the last ticks another rule keeps whole). (3) A later rewind anchored there: `restore_base`
takes the whole record before it and replays the difference - SRT4's path, which sparse verify checks. (4) The
resync's anchor (always the first record of a new history), a snapshot (sparse capture runs without them),
`amend_record` (the live tick only), the journal's rows and `retire_observer` (bodies, kept by a light record), the
union's records (by tick, the base's), `--dump-savestate` / `--compare-scripts` (sparse capture off with them).

**Built** (`rollback_engine::set_replay_last_light`, `capture_replayed`; `BMMO_REPLAY_LAST` = whole | light |
verify, read once in `client_core.cpp`, whole when unset; the engine's default is whole, so the tools and the unit
tests' engines are unchanged). Light: the last tick takes the rule of the other replayed ticks (whole only when a
multiple of k, a birth record, rows, no record before it). Verify: light, with sparse capture's verify turned on
with it - every light record captured whole all the same, so the last tick's own check still runs and every
rebased rewind's anchor is compared. Counter: ` replay_last=whole|light/N` at the end of the status line's sparse
section (the replays whose last tick ended light).

**Measured** on the dev machine (the L6 re-drive, one binary, `BMMO_REPLAY_LAST` whole / light interleaved twice,
medians over the 13 clients, `work/PERF/kp_compare.py`):

| | whole | light |
|---|---|---|
| whole records a rewind | 1.67 | 0.80 |
| the replay of a rewind | 3.16 / 3.14 ms | 2.34 / 2.31 ms |
| a replayed tick | 0.354 ms | 0.258 / 0.254 ms |
| the live capture a frame | 0.979 / 0.981 ms | 0.974 / 0.969 ms |
| rewind work + capture a second | 180 ms | 160 / 159 ms |
| depth mean | 8.85 ticks | 9.09 ticks |
| frame p50 / p99 | 2.6 / 10.7, 9.9 ms | 2.6, 2.5 / 8.5, 8.4 ms |
| frame p999 / max | 17.9, 15.2 / 32.8, 25.1 ms | 13.2, 12.6 / 20.4, 19.8 ms |

On the 8845HS (one binary, `BMMO_REPLAY_LAST` unset / light, two interleaved pairs that agree, 13 clients on its 16
threads): the replay of a rewind 4.10-4.15 -> 3.19-3.21 ms, a replayed tick 0.46 -> 0.35 ms, rewind work 145-147 ->
124-125 ms a second, frame p99 12.2-12.4 -> 10.9 ms, p999 18.5 -> 16.5 ms, depth mean 8.85 -> 9.09 ticks, the live
capture unchanged (1.18-1.19 ms a frame). Client 0 alone on a core with light (33 232 frames), against the slot
cells' build: frames over 15.15 ms 37 -> 22 (0.066 %), p50 3.2, p99 11.5 -> 10.2, p999 15.6 -> 14.4, max 25.9 ->
22.5 ms; the 22 are between ticks 983 and 1937 (16 with one rollback, 16.5 ms on average; six with two, the
slowest 22.5). The calibrated reach cap read the same in both runs (the headless hosts' cost clock); on retail the
meter now times a cheaper replay, so the cap may rise - to be logged before the default changes.

**Why the default stays whole: what sparse verify's differing anchors are.** Verify's 35 (HEAD's rule) / 36 (light)
differing anchors of the L6 re-drive are two older faults of the replay, neither of them sparse capture's; verify
shows them because it compares two replays of one tick made in different live frames. (A) `execute_scripts` admits
a script's owner into a replayed frame through `owner_pending_add`, which reads the behaviour manager's pending
table as it stands in the live frame: the live tick's own Activate Script makes a replay run in that frame resolve
a pending DEACTIVATENEXTFRAME that replays of other frames leave (P_Modul_18_Particle Script, ticks 235-237). The
word differs only in that bit on a script that is inactive either way: no reader of the bit computes anything else
from it - representation only. (B) A delayed link in flight in the live world but not at the restored record
cannot be cancelled by the restore (`restore_behavior` counts it, `links_live_pending`) and fires inside the
replay: seen at tick 1297, a fan's 'Wave Player End Playing -> Set Cell' (delay 2) freeing its sound slot four to
five ticks early. On that graph the cell is read by the fans' sound slots only (audio), which is why the journals
agree; but the mechanism is open to any selected script with a link of more than one frame's delay
(Gameplay_SectorManager has one of four), and a rebased rewind turns a link the restore would have re-armed into
one that fires astray - light makes rebased rewinds 6 % more frequent. So: first a census of such links on the 13
levels, the restore taught to take a live-only link out of flight (or to undo its fire), the pending adds of the
live tick taken back at a restore, and sparse verify reading 0 with the written cells compared too; then the
default goes to light.

**Gate.** 846/846 (one new: `SparseCaptureRecordsTheReplaysLastTickLightOnRequest`); the L6 re-drive light with
`BMMO_SCRIPT_REUSE`, `BMMO_SLOT_MEMO`, `BMMO_RESTORE_PLAN` and `BMMO_TRAFO_HELD` in verify (the run where the live
capture copies from an older source - `BMMO_REPLAY_LAST=verify` captures whole and so does not take that path),
0 mismatches, every session's `mism` 0; the same in `BMMO_REPLAY_LAST=verify`, 0 reuse mismatches, 56 631 anchors
compared, the 36 above; every client's and the server's journal byte-equal across the whole and light runs; fast
LBM light with the four reuse switches in verify 38/38 twins FINAL exact, every criteria row equal to the resync
cut's run, 0 mismatches; fast LBM light 38/38, the same rows; fast LBM as shipped (whole) 38/38, the same rows;
retail at K = 20, L12 and L11 at human key rates, light with the reuse verifies and light alone, 0 mismatches,
FINAL rows bit-equal to the restore skip's sessions with L11's 32 trafo pieces; x64 and x86 builds, the Arch
server; one review, no defects (its points: what verify does not exercise, the cost meter, the open class).

## Built (a delayed link in flight live taken out at the restore; the live tick's pending adds taken back, 2026-11-12)

**The census** (SimTool `--delayed-links`: every link of the selection with an initial delay above one frame and
the blocks its fire can reach, levels 1-13). Two links, nothing else (no trafo script has one): (1)
Gameplay_SectorManager `Deactivate Sector.Out 0 -> Set Cell.In`, delay 4, every level - its Set Cell
(CurrentLevel[0][4]) leads into Activate Sector: Set World Matrix, Show, Physicalize, Activate Script with reset of
the sector's modules, so a stray fire is a physics and script-activation divergence; (2) every fan's
`PlaySound / Wave Player.End Playing -> Set Cell.In`, delay 2 (P_Modul18_ActiveSounds[row][1] = 0, read by the
fans' sound slots only): audio. The per-slot unit copies and the trafo copies carry their originals' links (an inference: the census
walks a session-less load, where the per-slot copies do not exist; the restore walks a copy's links like its
original's). Custom maps were not censused.

**The sector manager's link in practice.** A stray fire of (1) would be a physics divergence, but a pass begins
with the manager activated WITH reset (the union's pass always, `sector_activation.hpp`; a pass a game script asked
for is re-issued with the reset flag it was recorded with), a reset empties the graph's delayed list, and the frame
that resets is the frame that queues the link - so no record has the link idle after the reset and before the
queue, and a link left in flight by the live world is cleared by the replayed pass's reset before it can fire.
SimTool rounds that rewind to the idle record while the live world has the link in flight (L2 and L13,
`--savestate-check --frames --drop P_Ball_Wood_MF 0 --drop-settle 2 --ticks 40 --savestate-lead 0
--savestate-rounds 2:1:6,2:2:5,2:3:4,3:2:3,2:3:4,3:1:4 --session-recipe`: live countdown 3/4, 2/4 and 1/4, anchor
2, then the follow across the fire at 6) are exact with the switches off (`live_pending=4`) and on (`withdrawn=4`,
`Gameplay_SectorManager ... 1 link(s) from link 4`: a stray in the middle of the manager's link array, the suffix
moved), 6 of 6 rounds and 26 of 26 follow ticks as the straight pass on both levels, the link firing once, at 6.
So (1) was harmless before this step whenever the pass resets (every pass seen), and the withdrawal is what covers
a pass re-issued without reset; the case that tells the two restores apart is the fan's link.

**Fault B fixed** (`restore_behavior`, `withdraw_links`; `BMMO_LIVE_LINKS` off | on, on when unset). A link in
flight in the live world (0 < countdown < initial) and idle at the record stayed in its graph's delayed list and
fired inside the replay. The SDK has no call for the list, but `RemoveSubBehaviorLink` takes a link out of it
with the graph's link array, and `AddSubBehaviorLink` appends one with its in-list flag cleared; retail CK2.dll
does exactly what the reimplementation does (disassembled: the graph data at +0x5c, the array at +0x18 and the list
at +0x24 erased with an order-keeping memmove, the flag bit 0 of the link's +0x18 cleared, then AddIfNotHere).
The restore removes every link from the first stray one on and appends them again in order, so the array (the
records' and CheckIOsActivation's order) is unchanged; then each stray link takes the record's countdown. It is
then as at the record - idle, out of the list, the record's countdown - and fires only when the replay queues it,
at the delay a live queue gives it, whatever the window, a second rewind, a rebased one or a reset inside it. A selected script the record does not
have (`unrecorded`: born inside the window) is not written and keeps its live links, uncounted, as before.
The other links of the moved suffix leave the list too: no change for a re-armed one (its source output is on;
the graph's next CheckIOsActivation queues it again keeping its countdown, as the list kept it) or an idle one.
Refused, and counted as before (`links_live_pending`, inexact), when a link after the first stray one is in
flight in both worlds with its re-arm refused (it must stay in the list); a moved array that does not read back
in order counts `links_unrestorable`. Paths that do not walk links (the restore plan, the held and dormant trafo
scripts) are scripts quiet since a record equal to this one, so their countdowns are the record's; a script the
restore does not write (the v14 rule) keeps its live links by design. Counter: `delayed links: rearmed=
unrestorable= withdrawn= live_pending=` at the headless session's end, with the first cases named.

**Fault A fixed** (`take_back_live_adds`; `BMMO_LIVE_ADDS` off | on, on when unset). At a live frame's first
restore the selection's owners' positive pending counts - the live tick's own adds, since its
ManageObjectsActivity consumed the table - are set to 0: the replay runs that tick again and makes again what the
corrected tick makes (the rewind's end re-issues the outside activations through CKScene::Activate, which adds
again), so no replay reads the live tick's adds and a stale one does not reach the next live frame. Later
restores of the frame take back the earlier replays' nets (FX1) and not the live adds again (the ledger's
`live_taken`). An owner with a script the restore does not write active or to be activated keeps its count (its
maker may be outside the replay). Counters on the membership line: `live_adds_taken=`, `live_adds_kept=`.

**Verify** now compares the written-cell sections too (`diff`'s `cells`, asked through `diff_scripts`'
`logical` in-value by sparse verify only; the replay's own check against the live record still leaves them out,
decision Q4). L6 re-drive in `BMMO_REPLAY_LAST=verify`: with both switches off (HEAD's restore, the cells
compared) 56 631 anchors, 36 differing (24 of A, 12 of B: the one stray fan link of 12 clients, `live_pending=1`
each), 0 float, 109 stale, no third class; with both on, 56 631, **0** differing, 0, 109 (`withdrawn=1` and
`live_adds_taken=1` on 12 clients each: client 8 withdrew none, client 4 took back none,
as their rewinds fell). Light with the four reuse switches in verify: 0 mismatches. Every session's
`mism` 0 in all four runs, and every client's and the server's journal equal past the build id to HEAD's runs of
the same recipes (the fan's cell and the Particle flag are journaled nowhere). The cell compare's own check: its
counters (`cells: ... compared= differed=`) read 470-503 sections compared a client in verify; a scratch build that
flipped one cell of the second record found every one (`written cells: array #45635 [3][1] 0 vs 16777216`), and a
scratch build that asked for the cells in the replay's own check too (live against replayed) found none differ.
A record with a written-cell section against one without counts as a difference (the review's point).
Sparse verify only asks for the walk when the two records' bytes differ, so with both fixes on the L6 re-drive's
`compared=` reads 0: all 56 631 anchors are byte-equal, the cells with them.

Not exercised by any run: a re-armed link in a moved suffix, the refusal, two strays in one graph, a second
restore in one live frame (FX1's `replayed_taken_back` 0; the replay's re-issue of outside activations,
`manager_activations::replay`, adds outside FX1's bracket, so a second rewind of the frame still reads the first
replay's re-issue adds), a manager pass re-issued without reset.

**Retail.** The retail host prints the counters at a session's end (`Physics session: delayed links: ...;
live_adds_taken= live_adds_kept=`). The withdrawal ran on the retail CK2.dll: L12 at K = 20 with
`BMMO_REPLAY_LAST=light`, one client `withdrawn=1` (`All_Gameplay / Gameplay_SectorManager: 1 link(s) from link
4`, the link array read back in order), FINAL rows bit-equal to the restore skip's session; L11 as shipped, one
client `live_adds_taken=1`, FINAL rows bit-equal with the 32 trafo pieces.

**Gate (the delayed links).** 847/847 (one new test); the L6 re-drive in `BMMO_REPLAY_LAST=verify` on the final
tree, 56 631 anchors, 0 differing, 0 float, 109 stale, `withdrawn=12`, `live_adds_taken=12`, every session's `mism`
0; the same light with `BMMO_SCRIPT_REUSE`, `BMMO_SLOT_MEMO`, `BMMO_RESTORE_PLAN` and `BMMO_TRAFO_HELD` in verify, 0
mismatches; fast LBM in `BMMO_REPLAY_LAST=verify` 38/38 twins FINAL exact, every criteria row equal to the resync
cut's run, 419 anchors compared over the arms, 0 differing in every arm (`rearmed=9`, no stray met,
`live_adds_kept=4` in the death arms); fast LBM as shipped (whole) 38/38, the same rows; fast LBM light with the
four reuse switches in verify 38/38, the same rows, 0 mismatches; retail at K = 20, L12 and L11 at human key
rates, as shipped and light, 0 mismatches, FINAL rows bit-equal to the restore skip's sessions with L11's 32 trafo
pieces; SimTool's manager rounds above; x64 and x86 builds, the Arch server (SimTool's counter line was added after
the gate's runs: x64 built and the tests run again); one review, no defects - its points (the retail calls read
from the DLL again by the orchestrator: both overloads of RemoveSubBehaviorLink erase from the two arrays with a
memmove and nothing else, AddSubBehaviorLink clears bit 0 and appends; the manager's link; the retail counters;
the one-sided cell section; the census's limits) are in.

## Built (the replay's last tick is light by default, 2026-11-12)

`BMMO_REPLAY_LAST` unset now means light (`client_core.cpp` `replay_last_from_env`; `whole` keeps SRT4's rule,
`verify` is light with sparse capture's verify). What held the default at whole - sparse verify's differing
anchors, the two faults of the replay behind them - is closed by the section above: 0 differing anchors on the L6
re-drive (56 631 compared) and on every fast LBM arm (419 compared), the withdrawal run on the retail CK2.dll.

**The reach cap on retail** (the review's point of the light rule: the cost meter now times a cheaper replay). At
K = 20, L12 and L11 at human key rates, whole against light: the replay's metered cost 0.39-0.41 -> 0.34-0.36 ms a
tick, the running cap 104-112 -> 118-120 ticks; the calibration at tick 44 reads the same (cap 74-91 whole, 76-98
light: its replay is the calibration rewind's, a whole record at its end either way). A deeper reach is what a
cheaper replay buys; the FINAL rows are bit-equal in both.

**Gate.** 847/847; as shipped (the variable unset, every session reporting `replay_last=light`): the L6 re-drive,
every session's `mism` 0, the 13 clients' journals equal past the build id to the light run of the commit before;
fast LBM 38/38 twins FINAL exact, every criteria row equal to the resync cut's run; retail at K = 20, L12 and L11
at human key rates, 0 mismatches, FINAL rows bit-equal to the restore skip's sessions with L11's 32 trafo pieces
(L12: `withdrawn=1` on Gameplay_SectorManager again); x64 and x86 builds, the Arch server. The light rule, its
verify and the explicit whole were gated on the commit before with the same code but for the default.

## Built (a slot relatch marks the roots it wrote, 2026-11-12)

**What it costs today.** Every host write into the selected scripts outside the engine's restore bumps one
process-wide generation (`script_record_reuse.hpp` `note_script_write`), and a record of t - 1, a kept source, a
copy plan or a slot memo made under another generation is never used. On the public L6 record (8845HS, client 0
pinned) 81 of the session's 82 captures without a source come from one site, the slot relatch in
`proximity_union::place_slot_identity`: it runs when a ball is born, dies or changes type, writes one probe
block's output, and makes all 31 roots lose their source. In the frames over the 15.15 ms budget, 40-47 % of the
grid whole records walk all 31 roots, about 2.8 ms each against 1.0-1.3 ms with a source.

**The rule.** A relatch that can name what it wrote no longer bumps the generation. It marks the root scripts that
own the written block's output and that output's destinations, transitively (`script_slots::mark`). A marked root
has no quiet run: `quiet_since` reads "never" and `quiet_now` false. So no kept source, copy plan or slot memo
serves it or the copies under it, and a restore writes it. A reusing capture walks it whatever its source, record
t - 1 included. Every other root keeps its source, plans and memos. An observing capture's walk of the root
(`walked`, after the walk, so the memos under it are refused during it) starts the root's run at that observation,
so a plan or kept source from before the write cannot serve it. The mark stays for the rest of that tick: a second
capture of the same tick is offered record t - 1 again (nothing bumped the generation), and without the mark it
would copy the root's block from before the relatch. That second capture is the amend after a record's own capture
(`client_core_frame.cpp` `amend_record` after `record()` with `after_record` set: the headless client's
`--dump-savestate` / `--compare-scripts`; the live path defers the record and captures once). The mark comes off in
four places:
- the first observation of another tick (`begin_observation(tick)` drops the marks walked at the tick before; a
  relatch after the walk marks the root again as not walked, so that observation keeps it);
- a restore that wrote the root exactly (`restored`, the `holding` roots);
- `forget` (a restore that failed after its first write, which bumped the generation before writing);
- `clear` (after the rewire's or the release's bump).

A capture that does not observe (one with no reuse request: the hold digest's, a tool's) walks a marked root as it
walks every root, and leaves the mark on. A restore in the frame after the walk, before the next tick's
observation, still finds the root marked and writes it.

**The readers, and their answers.**
1. *Record t - 1* (`rollback.hpp` capture: `scripts_previous` kept while the generation is unchanged; the copy rule in
   `script_replay::capture` is only `reusable_root` for a t - 1 source). A relatch leaves the words as they were,
   so without the mark record t - 1's stale block would be copied. Answer: one branch at the top of the root loop
   in `capture` walks a marked root before any source is looked at (the dynamic form of `sel.is_fed`).
2. *The kept source and the copy plans* (`quiet_since_kept`, `plan_for`, `plan_at_restore`, `keep_plan`). All of
   them read `quiet_since`, which reads "never" for a marked root. After the walk, the run starts at the walk's
   observation, so once the mark is off a plan or kept source from before is refused by its own rule.
3. *The slot memos* (`memo_refused`, a copy's root by `quiet_since`): refused "run" while the root is marked. The
   walk walks the copies again and leaves fresh memos.
4. *The restore skip*. Every root and omitted copy a restore leaves goes through the armed quiet set
   (`arm_restore` from `quiet_now`; `root_unchanged` and `copy_unchanged` read `restore_quiet`, and `root_unchanged`
   also reads `quiet_since_kept`). A marked root is never in that set, so it and its copies are written. Which roots
   a restore writes at all (`write`: active at the record or replay_activation) is unchanged.
5. *The kept source after a restore* (`script_replay.hpp` restore, the generation compared with the restore's own)
   needs nothing: no place runs inside a restore. *The slot cells' guard* (`proximity_union::place`) needs nothing
   either: a relatch happens only in a full slot pass, the pass re-stamps the generation it ends with, and the latch
   it moved holds the cell's value as the pass leaves it. Only the comment there changed.

So every copy decision for a root goes through `capture`'s root loop and `script_slots::quiet_since`, and every
skip decision through `quiet_now`. One per-root mark tested at those two points answers all five readers.

**The reach and the fallback.** `classify(sel)` (called by the rewire, and by the server once its selection exists)
resolves the reach of each body-kind slot read. `SetOutputParameterObject` stores the value in the read's output and
copies it into the output's destinations, and into theirs when they are outputs too. Each written parameter's owner
(a behaviour, or the behaviour a parameter operation belongs to) is under a root. If every such root is in
`sel.scripts`, the read is targeted and remembers its root list. Otherwise it keeps the global bump: a destination
owned by no behaviour, or one under a root outside the selection (a trafo script, a satellite: the trafo ledgers
never see a mark). The marks go into the union's own `recorded_slots_`, and a relatch marks only when a host took
that map for the captures of its record reuse through `proximity_union::hooked_slots()`: the retail host
(`core_host.cpp`) and the headless client (`session_client.cpp`), each right after its rewire (the rewire's `clear`
resets the flag). Every other binder gets the bump. The server's world (`physics_world.cpp`) takes
`recorded_slots()` plainly: its captures are the resync state's portable records, with no reuse. So do SimTool's
worlds, which are server worlds whose own bindings (`--savestate-check`, the late sector run and the others) capture
with reuse and hooks of their own, with no slot map: a mark there would be read by no capture, and record t - 1
would be copied stale. The non-slot identity path in `place` keeps the global bump. On L6, all 260 slot reads of each
client are targeted (3 380 over the 13), and none falls back.

**The switch and the counters.** `BMMO_RELATCH_MARK` = off | on (on when unset; off is the global bump everywhere),
read once (`script_slots.hpp` `relatch_mark_from_env`).
- The union's lookups line (every host prints it at a session's end) gains `; relatch: reads targeted= global=
  relatches targeted= global= (the first fallback's reason)`.
- The reuse line gains `marked: walked= unsourced= previous=d/c kept=d/c raw=p/k restored=`, and `scripts=` now
  counts the marked walks:
  - the marked roots a reusing capture walked;
  - of those, the ones with no block in the source;
  - the ones the rule would have copied from record t - 1 or from the kept source (c), and of those the walks whose
    bytes differ from that copy (d, the copy made with `copy_with_fresh_tails` into scratch, so physics tails do
    not count);
  - the walks whose bytes differ from the source's block as it stands, physics tails included (`raw`, from record
    t - 1 / the kept source);
  - the marked roots a restore wrote.
- One engine-free test (`ScriptSlots.MarkedRootHasNoSourceUntilWalkedOrRestored`) covers the rule. A marked root has
  no kept source, plan, memo or skip, while another root keeps all four. After a light observation it still has
  none. After the walk it still has none, and none after a second observation of the walk's tick. The first
  observation of the next tick takes the mark off; then nothing from before the walk serves it, and what the walk
  leaves does. A relatch after the walk keeps the mark across that observation. A restore that wrote the root
  exactly takes its mark off, one that did not leaves it, and `forget` drops every mark.

**What the L6 re-drive showed** (every reuse verify on: `BMMO_SCRIPT_REUSE`, `BMMO_SLOT_MEMO`, `BMMO_RESTORE_PLAN`,
`BMMO_TRAFO_HELD`, `BMMO_SLOT_CELLS` and `BMMO_REPLAY_LAST` = verify).
- With the mark on, the 1 120 relatches of the 13 clients all marked (86 on client 0), and none bumped the
  generation.
- Captures with no source fell from 1 133 to 13 over the 13 clients, and roots walked without a block from 35 123 to
  403. Plans refused for the generation went from 1 892 to 0 on client 0.
- Client 0's reuse line, mark off against on: `with_previous` 13 288 -> 13 342, `reused` 1 470 564 -> 1 472 456,
  `unrecorded` 2 697 -> 31, `kept: restored` 5 679 -> 5 709, `plans: emitted` 1 468 648 -> 1 472 432, `gen` 1 892 ->
  0.
- The differences between mark off and mark on, summed over the 13 clients, are whole multiples of the 1 120
  relatches. Roots walked without a block fall by 34 720 (31 x 1 120). Reused roots rise by 24 640 (22 x 1 120), and
  verify walked every one of them again with 0 mismatches. Walked roots rise by 8 960 (8 x 1 120), and each of those
  captures also walks its 1 marked root. So at each relatch capture, 22 roots keep a source and match a fresh walk.
  The 8 + 1 match the diagnosis's 9 roots that walk anyway.
- The capture walked every one of the 1 120 marked roots. Each had a block in its source, and every walk's bytes
  differed from that block (`raw=683/437`: 683 from record t - 1, 437 from the kept source). Not one of them passed
  the reuse rule (`previous=0/0 kept=0/0`): the root was not reusable at that capture, or it was fed. That is
  consistent with a relatch landing while the probe's script runs, and with the 8 + 1 above, but the counter does
  not say which. So on L6 the source a mark denies is one the rule would have refused anyway. The decisive branch
  of reader 1 never stood between a stale block and a copy, and the scratch-copy compare never ran.
- With the mark taken off at the walk (the first build, `rtv1`), no restore met a marked root (`restored=0`). With
  the mark kept for the walk's tick (`rtv2`), 12 restores over the 13 clients wrote a marked root: a rewind in the
  frame after the walk, before the next tick's observation, finds it marked and writes it. Every other counter of
  `rtv2` is equal to `rtv1c`'s (marked walks 1 120, as many as the relatches: no capture of a walk's tick again
  met a mark, and no mark lingered), and every journal is byte-equal.

**What the counters can and cannot say** (the review's points).
- `raw=` proves nothing about the relatch: a walk's bytes differ from the source's block whenever the marked root
  ran since, and all 1 120 fit "the root was active".
- What L6 does show: the reach was complete there (about 22 other roots per relatch capture were copied and verify
  walked equal), and every relatch marked a root that a capture then walked.
- What L6 does not show: with `previous=` and `kept=` at 0/0, neither the denial of a quiet root's copy nor the
  scratch compare ran. The gate needs an arm where those counts are not 0 (for example fast LBM in verify, a
  leaver's clone relatching an inactive unit).
- A lingering mark would show as marked walks well above the relatches targeted, beyond what the same-tick rule
  adds (a capture of the walk's tick again, from a rewind of depth 1 or an amend). A mark that no reusing capture
  meets is invisible (`marked: walked=` counts reusing captures only); it costs one walk a capture at most.
- Line endings: the index stores LF (`core.autocrlf=true`); in the working tree `script_slots.hpp`,
  `script_replay.hpp` and this file are LF and the other changed files CRLF. The commit normalises them; the diff
  holds content only.

**Not exercised.** A marked root that was quiet (the t - 1 and kept copies the mark denies, the scratch-copy
compare, and its `previous=` / `kept=` counters: 0/0 on L6 as shipped and in verify; the fast LBM and the retail
sessions have no relatch at all); a fallback (no read on L6, the LBM arms or retail reaches outside the
selection); `forget` with a mark on. A restore that writes a marked root ran on L6 (`restored=12` in verify, 122
as shipped). The server's reach no longer matters: the server's world never takes the map through
`hooked_slots()`, so every relatch there bumps. The amend's second capture of a tick (the headless client's
`--dump-savestate` / `--compare-scripts`) and a SimTool world with slots (`--savestate-check` with `--drop` and
`--mechanism-slots`), the two cases the review found, were not run; their rules are read from the code above.

**Gate.** Only what this step ran:
- builds: x64 and the tests, 848/848 (one new test); the x86 client build (no C1128);
- the L6 re-drive with the six verifies above, mark on (`pf_rtv1_1`; again as `pf_rtv1b_1` with `unsourced=`
  added, and as `pf_rtv1c_1` on the final tree with `raw=`, the `scripts=` sum and the named empty reach) and off
  (`pf_rtv0_1`). In all four, every verify's mismatches were 0 on all 13
  clients, sparse verify had 0 differing and 0 float anchors (3 719-4 722 verified per client, 6-10 stale), and
  every session's `mism` was 0;
- journals: the 13 clients' journals were equal past the build id between `rtv1` and each of `rtv0`, `rtv1b` and
  `rtv1c`, and the server's journal was byte-equal between `rtv1` and `rtv0`;
- the x86 client build again on the final tree (no C1128);
- after the review (the mark kept for the walk's tick, `hooked_slots()`): x64 and the tests, 848/848; the x86
  client build with `core_host.cpp` compiled again (no C1128); the L6 re-drive with the same six verifies, mark on
  (`pf_rtv2_1`): every verify's mismatches 0 on all 13 clients, sparse verify 0 differing and 0 float, every
  session's `mism` 0, relatches targeted 1 120 and global 0 (so both hosts' rewires took the map through
  `hooked_slots()`), `marked: restored=12`, every other counter equal to `rtv1c`'s, and the 13 client journals and
  the server's journal byte-equal to `rtv1c`'s.

**The full gate** (the final tree): x64 and the tests, 848/848; the x86 client; the Arch server. The L6 re-drive
with the six verifies (`pf_rtq_1`): every verify's mismatches 0, sparse verify 56 631 anchors, 0 differing, 0
float, 109 stale, every session's `mism` 0, 3 380 reads and 1 120 relatches targeted, none global; as shipped
(`pf_rts_1`): `mism` 0, the same 1 120 relatches targeted, the 13 clients' and the server's journals equal past
the build id to HEAD's shipped run (`pf_dfl_1`) and to `pf_rtq_1`. Fast LBM with the verifies and as shipped:
38/38 twins FINAL exact each, every criteria row equal to the resync cut's run, 0 mismatches, 419 sparse anchors
0 differing; 1 120 reads targeted, no relatch. Retail at K = 20, L12 and L11 at human key rates, as shipped: 0
mismatches, FINAL rows bit-equal to the restore skip's sessions with L11's 32 trafo pieces; 520 and 200 reads
targeted, no relatch (L12: `withdrawn=1` on Gameplay_SectorManager as before). So the step's effect on retail is
untried: its sessions here never relatch.

## Built (a lifecycle rewind takes a deeper input correction along, step MRG, 2026-11-12)

**What it costs today.** On the public L6 record (8845HS, client 0 pinned) 11-15 frames a run are still over the
15.15 ms budget, and 5 of them hold two rollbacks, always of one shape: the frame's late lifecycle batch (SRT3's
`flush_life_rewinds`) rewinds 4-7 ticks, then `replay_corrected_inputs` rewinds again from a remote input
correction's anchor 10-14 ticks back. The second replay steps again every tick the first did, from a deeper record,
so one restore and one replay from the deeper anchor cost what the second rollback costs alone: 5.8-7.7 ms less in
each such frame, which brings every one of them under the budget. SRT3 kept the case in its "not merged" list.

**The rule.** At the frame's final flush - `process_own_echoes`' (`flush_life_rewinds(true)`), after the own echoes
(step ODM) - a pending input correction that the batch's replay would not carry gives the batch's rewind its anchor
(the corrected tick T - 1, as `replay_corrected_inputs` takes it) and its reach (`life_rewind_reach(T)`, the value
`replay_corrected_inputs` passes); `drop_carried_input_replay` then drops it as carried, as it drops a subsumed one.
"Would not carry" is `drop_carried_input_replay`'s own test (`replay_subsumes`) against the record the batch
restores (`restore_base` of its anchor: a light anchor restores the whole record before it and carries what lies
after that). The early flushes keep their anchor: `settle_life_batch_before`'s, the GHF and A3 flushes before
`rewind_for_spawn_forgets` / `drain_pending_deaths`, and an own echo's flush before `move_own_birth` and the other
echo paths. Each runs because what follows it reads what the batch's replay leaves, and the input replay still
follows it in the frame. The code is `client_core_lifecycle.cpp` `merges_input_replay`, its conditions numbered in
its comment:
1. *Recorded and inside the reach*: `knows_tick(T - 1)` and `now - (T - 1) <= window_rewind_reach(T)`, the test
   `joins_life_batch` puts on an event's anchor. It is also `rewind_to`'s limit for the reach passed:
   `life_rewind_reach(T)` is 0 or the window's reach, and `min(max(reach, max_resim_ticks), resync_reach())` is then
   `window_rewind_reach(T)` either way. The running reach cap cannot move in between: `max_resim_ticks` changes only
   in `apply_reach`, from `sample_reach` at a datagram's arrival, never inside `pre_step`.
2. *Not before a pending exact resync's base*: an event does not join the batch there either (the server's state of
   base - 1 covers it).
3. *A whole record at or before T - 1* (`rewindable`): `rewind_to`'s third refusal, sparse capture's. The step
   excludes instead of falling back: a refused `rewind_to` writes no world, but `rewind_world` runs
   `settle_unreplayed_lives` and `settle_own_due` after it, which make the batch's queued births and deaths live,
   so a fallback rewind after a refusal would not be the batch's own. With 1-3 the merged `rewind_to` passes all
   three of its refusals (`find`, the limit, `restore_base`), and `restore_and_replay` returns true once its base is
   found. The miss path stays under it: the correction is then left pending and `replay_corrected_inputs` runs it.
4. *No step after the flush with work to do* that reads the world the replay leaves, or rewinds itself. Today they
   run between the two replays, merged after the one, which is the carried case's order:
   - `expire_own_echoes`: `re_report_current` takes the own ball's current pose into an event that is sent and
     journalled, `resync_own_life` may take a body out, `expire_rereports` likewise. Excluded when it would act
     (`own_echoes_expiring`: its gates, each overdue test on a copy of the deadline, since
     `own_echo_deadline::missing` notes the uplink when it looks).
   - `apply_forced_own_moves` (a test override; `move_own_birth` reads the body and rewinds): excluded when a forced
     move is due.
   - `try_apply_resync`: an exact resync's apply restores its base and re-steps over records a merged replay would
     have rewritten, and drops the input replay under `resync_rewinds_subsumed_`. Excluded when its gate passes
     (`have_resync_state_`, `resync_pending_`, `resync_exact_`).
   - `rewind_for_sectors`: a late Sector set rewinds from its own anchor and re-derives the union. Excluded while
     `sector_late_` holds one (nothing after the flush adds to it: `move_own_sector` runs in the echoes before it).
   - Not excluded: `settle_replaced_rereports` reads no world (the re-reports' ticks against the tick base, a resync
     request); `platform_.script_guards` re-pins the sector reset links and cells a restore or the scripts handed
     back - idempotent, no body read - so the merged replay runs under the guards the batch's replay runs under
     today. Their log lines: 26 in each run below.

**What the dropped rewind did outside the history.**
- *The batch's checks* (`make_remote_birth` / `remove_remote`'s "the replay did not reach it") run after the one
  rewind as before; the deeper replay reaches every stamp the batch's did.
- *Counters*: `life=` counts the merged rewind (one, as before), `life_rewind_depth_` its own depth (deeper);
  `inrew=` loses the input replay and ` coalesced: input=` gains it (carried; an own-only correction goes to subst's
  subsumed through `drop_carried_own_replay`, which the flush calls first); `resync_window_rewinds_` does not count
  it (as for a carried one); `window_inrew_src_` names its source as `drop_carried_input_replay` does.
- *The pacers* (condition 5, the review's point): a correction the input replay would not run in this frame is
  not merged - an own-only one (it waits on the own pacer), one under the remote cadence (step A2), one while the
  client is behind its schedule (`kRemoteCoalesceBehind`). Merged, a held correction would make this frame the
  deeper one, against the budget, and run a frame earlier than merge off would. Such a frame counts as excluded
  (the fifth reason's counter, with 4's). What remains is a remote correction the input replay runs whatever the
  pacers say; the merged frame marks the remote pacer `ran()` as that replay would (the carried path marks it only
  on the cadence) and the own pacer through `drop_carried_input_replay` when an own correction rode along.
- *The cost meter* takes one sample (the merged rewind's) where it took two, so a live meter's cap moves with
  it (a cheaper frame, a shallower average). The re-drive cannot show it: the canned test fixes the cost model
  (`rewind_cost_meter::fix`, `cap=92/3.00/0.460/1`, one sample) and the reach stays 48 in every run below (no
  `reach:` line). `calibrate_rewind_cost` runs only while no rewind has been sampled, which any rewind of the frame
  ends.
- *The frame's tail*: `make_unmade_remakes`, the `after_rewinds` hook, the "a rewind ran" test of the drive
  (`rewinds_before`), and core_host's end-of-frame hooks on a changed restore count (XFG's `extra_glow_frame`, RSC's
  sound frame) see a restore in the frame as before, after its last replay. `script_guards` runs after the frame's
  last rewind in a merged frame and before the input replay in a two-rollback one, the carried case's order: the
  two agree while no replay hands a guarded link back (its 26 log lines are the same in both runs).

**Not merged**, counted on the status line: a correction the batch's replay carries already (as before), its anchor
not recorded or outside the reach, before a pending exact resync's base, with no whole record to restore from, and a
frame with work for one of 4's steps or a correction the pacers hold (5). Not at all: a correction that arrives after the final flush (none can inside
`pre_step`), and every early flush.

**The switch and the counters.** `BMMO_MERGE_INPUT` = `off` | `on`, read once per process (`client_core.cpp`
`merge_input_from_env`, beside `replay_last_from_env`), on when unset, latched into `s.merge_input_` at the session
start; off is the behaviour before the step and its status line the text it was. The status line's coalesced section
(also opened by them, so retail reads them too) adds ` merged=M unmerged=a/b/c/d/e`: the frames merged, then the
candidates not merged - carried as before / not recorded or outside the reach / resync base / no whole record /
excluded by 4. The first 8 merges are logged: `lifecycle: the rewind to tick A takes the input correction at tick T
along (from tick T - 1, local tick N)`, the rollback's reason gains `; input corrected at tick T`, and the
batch's "late events taken by one rewind" and refusal lines name the anchor that ran.

**The runs** (the L6 re-drive, dev machine, 13 clients, one binary; `pf_mgoff_1` with `BMMO_MERGE_INPUT=off`,
`pf_mgon_1` with it unset):

| | client 0 | the 13 clients |
|---|---|---|
| merge off | `life=66/0 inrew=5643/0 rb=5709 resim=51699`, `coalesced: life=6 input=30` | 72 875 restores, 661 620 replayed ticks |
| merge on | `life=66/0 inrew=5633/0 rb=5699 resim=51649`, `coalesced: life=6 input=40 merged=10 unmerged=30/0/0/0/0` | 72 761 restores, 661 065 replayed ticks |

Every client merged (7-11 frames each, 114 in all), and on every client the restores it lost, the input replays
it lost and the carried ones it gained are each its `merged=`. Every candidate not merged was one the batch's replay
carried already (`unmerged=N/0/0/0/0`). The 8 merges client 0 logs start 1-7 ticks before the batch's anchor and
5-10 ticks before the live tick (the dev machine's relay). One frame, merge off: `rollback to tick 832 (ball of
65538 born at tick 837)` re-simulated 7 ticks (the anchor 836 rebased), then `rollback to tick 825 (... input
corrected at tick 830)` 14 ticks; merge on: one `rollback to tick 825 (ball of 65538 born at tick 837; input
corrected at tick 830)`, 14 ticks. 555 replayed ticks fewer in all.
Every session's `mism` 0; the 13 clients' and the server's journals equal past the build id between the two runs.
With the reuse verifies and sparse verify (`pf_mgv_1`: `BMMO_SCRIPT_REUSE`, `SLOT_MEMO`, `RESTORE_PLAN`,
`TRAFO_HELD`, `SLOT_CELLS` = verify, `BMMO_REPLAY_LAST=verify`, merge on): every `mismatches=` 0 (117 counters),
sparse verify 56 631 anchors compared, 0 differing, 54 stale (109 in HEAD's verify runs of this re-drive, which make
the 114 rewinds more), `mism` 0, its journals equal to the merge-off run's.
`pf_mgon_1` and `pf_mgv_1` ran the binary before two edits that change no decision (the log lines above name the
anchor that ran; a function moved in its file); the final binary's `pf_mgon2_1` repeats `pf_mgon_1`'s counters on
every client and its 14 journals equal the merge-off run's. Merge off equals the commit before's shipped run
(`pf_rts_1`: client 0's counters the same, the journals differing only inside the build id). Frame times on the
8845HS were not measured.

**The stored captures.** A capture records each rewind's three cost clock reads, so a strict replay of a capture
taken by a binary before this step diverges at its first merged frame, as at SRT3: such a replay runs with
`--replay-free-cost-clock` (SRT3's adaptation (2): `EXTRA=` when OLD and NEW both have the step, `NEW_EXTRA=` when
OLD predates it), or strictly with `BMMO_MERGE_INPUT=off` on the new side. A capture that holds no merged frame
replays strictly as before. Not run here.

**Not exercised**: every exclusion (4's counter is 0 everywhere), the refusal and resync-base cases, a correction
outside the reach, the pacer exclusion (5: no cadence, nobody behind, no own-only correction on L6), a merge inside
an exact resync's re-step window, a live cost meter, the wire check; the fast LBM and the retail sessions hold no
merged frame (every candidate there is carried as before).

**Gate.** 849/849 (one new test: `SessionClientCoreOwnDeaths.FinalFlushTakesAnOlderInputCorrectionAlong`, both
switch states); x64 BUILD-OK, x86 CLIENT-BUILD-OK; the L6 re-drive merge off, merge on, and merge on with the reuse
and sparse verifies as above, and merge on again with the final binary.

**The full gate** (the final tree, condition 5 in): x64 and the tests, 849/849; the x86 client; the Arch server.
The L6 re-drive with the six verifies (`pf_mgq_1`): every verify's mismatches 0, sparse verify 56 631 anchors, 0
differing, 0 float, 54 stale, every session's `mism` 0, 114 frames merged, 426 carried as before, no other reason;
as shipped (`pf_mgs_1`): `mism` 0, 114 merged, the 13 clients' journals equal to the merge-off run's (`pf_mgoff_1`)
and to `pf_mgq_1` past the build id, and to the slot relatch's shipped run (`pf_rts_1`) but for the 12 bytes of
the build id's commit. Fast LBM with the verifies and as shipped: 38/38 twins FINAL exact each, every criteria row
equal to the resync cut's run, 0 mismatches, 419 sparse anchors 0 differing; 0 merged, 41 carried. Retail at K =
20, L12 and L11 at human key rates, as shipped: 0 mismatches, FINAL rows bit-equal to the restore skip's sessions
with L11's 32 trafo pieces; 0 merged (1-3 carried per client). So on retail the step is untried.

## Built (a lifetime tick of a replay is recorded light, step LFL, 2026-11-12)

**What it costs today.** On the public L6 record on the 8845HS the frames over the 15.15 ms budget pay for the
replay's whole records, and every whole replayed record in them is a grid tick (every 8th, SRT4's `sparse=8`) or a
lifetime tick: a tick whose tracked set, generations or body set differ from the record of t - 1 (a remote born,
ended or reordered, the own ball's type change or death, a mechanism added), which `capture_replayed` recorded whole
whatever its place in the grid - 1.2-2.0 such records in an over-budget frame at 1.5-1.9 ms each (the census in
`.local-tools/work/WHOLE/wr3_agent2.txt`, projected 6-9 of 14-15 over-budget frames under). A restore reads such a
record, when it is not its base, only for the bodies born there: their pose and their extras (the parked body is
re-posed with them).

**The rule.** A replayed tick that only the lifetime rule would record whole - not a structural whole tick (k <= 1,
a grid tick, the last tick when it is not light, no record before, rows, no record of t - 1) - is recorded light
(`full = false`) and also keeps the extras of every name born at it: a body at t that the record of t - 1 has not,
or has under another generation (`birth_record`'s own test), listed in `tick_state::births`, its extras read with
one `get_body_extra` each (no contacts: the contact pass reads only the base and skips birth items; no capture
cache). The record carries the change's classes in `tick_state::lifetime`, a bit mask: born and ended (a remote new
at t, or gone, or of another lifetime), order (the same remotes in another order), own (the own ball renamed, made
or ended), mech (a mechanism added, removed or of another lifetime) and set (the body set differs from t - 1's;
only together with a lifetime change: a body set that changes alone keeps SRT4's whole re-capture); a generation change of a name in none of the lists counts as an end.
Two such ticks stay whole (review of step LFL): a body-set change with no lifetime change (SRT4's rule), and a
lifetime tick at which a name born by `keep_births`' test (`born_at`) reappears in a lifetime an earlier record
already carries - its body went and came back under the same generation, as a beam does (`reappears`, counted as
`lifetime_reappeared` and in `whole=`). Its oldest record is not t, so a rewind rebased across t would find no birth
record of that lifetime after its base (`birth_record`), nor a moved one, and would leave the body at its live pose
through a replay that starts where the body was absent; the unkept counter cannot see that case (no birth is read).
`capture()` clears both, so a whole capture of the tick (an `amend_record`, a later replay's whole record) carries
none. Code: `rollback.hpp` `capture_replayed`, `lifetime_classes`, `keep_births`, `born_at`, `reappears`,
`note_birth_read`. `keep_births` reads its births' extras inside the capture's lookup scope (`lookup_scope`, the
retail binding's one pass over the world for by-name reads), opened only when there is a birth to read.

**Each reader, and its answer.**
- *A birth read* (the birth item from `birth_record` or `moved_birth_record`, the remade life's lookup
  `find_const(remade_at)` in `revive_ended_lives`): reads the record's `bodies[name]` and `extras[name]`. The
  invariant: a read from a record that is not whole finds its name in `births`, so its extras are there. Every such
  read is counted (`lifetime_birth_reads`); one that finds the name not kept counts `lifetime_birth_unkept` (a fault:
  the body would be re-posed without its extras) and logs, capped at 8. A whole record holds every extra. The
  revived half of `revive_ended_lives` reads the base, which is always whole.
- *The restore base* (`restore_base`, `rewindable`, `restorable`): skips a record that is not whole, as for every
  light record; a rewind anchored at or after a lifetime tick rebases to the whole record before it and replays
  across the tick (the births and lives machinery then runs inside the replay: more remakes and revived lives, every
  one exact below). The grid ticks stay whole, so a rebase is still at most `sparse=8` - 1 ticks deeper and
  `kSparseCaptureMax` is unchanged; an anchor deeper than the standard reach can lose its only whole base and is
  refused as `sparse_unanchored` (0 in every run below).
- *The capture cache and the script reuse source* (`capture`'s record of t - 1): need a whole t - 1, as before.
  `keep_source` is called only from a whole script capture. A light lifetime tick calls `observe_scripts`, as any
  light record does, so a relatch's mark (step RELATCH) waits through it until the next whole capture walks the root.
- *Snapshots and resync*: `on_snapshot` refuses a light record (no session with snapshots runs sparse capture);
  `resync` anchors at S, the first record of a new history, which no replay re-records; the resync's localize reads
  `recorded_scripts(S)` of that same record.
- *`restore_candidates`, `generation_now`, the core's lifecycle reconcile, `recorded_body`, `recorded_input`, the
  retire observer*: read bodies, tracked sets and inputs, which a light record keeps.
- *`move_birth`*: erases the body, its extras and now its `births` entry in place; the replay's re-capture at the
  applied tick sees the name absent at applied - 1, so the record keeps it as a birth.
- *The core's uses of the base* (`merges_input_replay`, the sector re-derive in `rewind_for_sectors`,
  `drop_carried_input_replay` after a lifecycle rewind with `restored`): read the deeper base as for any light anchor.
  On the re-drive this drops one input replay on 12 of the 13 clients, the same one on each (the rollback lines
  aligned client by client: `Ball_Paper_BMMO_65546 input corrected at tick 1052`, client 0's own ball; nothing else
  differs). On client 1: the remote born at 1052 (its birth tick now light) is crossed by the two rebased rewinds of
  1059 and 1060 from 1050, which re-record 1052; the correction of 1052 at 1061, which the off run replayed 11 ticks
  for, is then not news. That the re-record took the relayed frame of 1052 (`client_core_messages.cpp` "the
  prediction was right") is inferred, by elimination: the relay, bodiless, hint and coalesced counters are unchanged.
  The journals are equal (below): the dropped replay changed no simulated byte.
- *The replay's own check against the live record of a tick, and the dump and compare tools*: need the record's
  scripts and blob. The check is a diagnostic no gate reads (as at RLL; sparse verify keeps it). The tools force
  `sparse_capture = 1` when they run (`session_client.cpp`), so they are unaffected; with an explicit
  `--sparse-capture` they skip a light record as before, and now say so at the session's end: `--dump-savestate /
  --compare-scripts: ticks skipped (record with none: light, or none taken): final= blobs= scripts= compared=`
  (`blobs=` also counts a live tick whose record took no blob, which is not a light record).

**The switch and the counters.** `BMMO_LIFETIME_LIGHT` = `off` | `on` | `verify`, read once per process
(`client_core.cpp` `lifetime_light_from_env`, beside `replay_last_from_env`), on when unset; off is SRT4's rule
exactly (the body-set re-capture included, now counted in `whole=` with its time, which SRT4 left uncounted). `verify` records the tick
light and turns sparse capture's verify on: the tick is captured whole all the same and still marked light with its
`births` filled, so every decision is the light one, and `verify_sparse_anchor` compares every rebased anchor. The
engine's `set_lifetime_light` is off by default (the tools and the unit tests' engines). The status line's sparse
section (retail reads the same line) adds, after `replay_last=`, ` lifetime=on|off/records/ms/births kept/birth
reads/unkept/reappeared cross=born:r/n/c/s,ended:...,order:...,own:...,mech:...,set:...`: the light records of that class, the
rebased rewinds whose replay crossed one (the classes of the records in (base, anchor], read in `rewind_to` before
the replay re-records them), and of their anchors those sparse verify compared and those it left stale. The
lifetime records count in `light=` too.

**The runs** (the L6 re-drive, dev machine, 13 clients, one binary; `pf_lfx1_1` off, `pf_lfs1_1` unset):

| | whole replayed | light (of them lifetime) | rewinds | rebased + extra ticks | replayed ticks |
|---|---|---|---|---|---|
| off (`pf_lfx1_1`) | 58 134 | 602 931 (0) | 72 761 | 56 685 + 210 007 | 661 065 |
| on (`pf_lfs1_1`) | 56 023 | 607 854 (3 228, 324 ms) | 72 749 | 56 858 + 212 939 | 663 877 |

Of the 3 228 lifetime records, 2 111 are ticks the off run recorded whole (whole 58 134 -> 56 023); the other 1 117
are among the extra ticks the deeper rebases replay (whole + light = replayed ticks in both runs: 661 065, 663 877),
ticks that were a rewind's whole base before. By class born 985, ended 772, order 1 289, own 262, mech 48, set 1 939 (a record can carry several). They kept 2 457
births' extras; 1 749 birth reads came from a light record, 0 unkept. Crossing a lifetime tick costs 173 more rebased
rewinds and 2 932 more rebased ticks (2 812 more replayed ticks net of the dropped input replay, one a client). `births=` 1 197 -> 2 007
recreated, every one exact; `lives=` ended 750 -> 1 094, revived own 52 -> 83, mirror 122 -> 435, mech 576 -> 576,
unrevivable 0. The off run's counters and the 13 clients' and the server's journals equal HEAD's shipped run
(`pf_mgs_1`) past the build id; the on run's journals equal the off run's and `pf_mgs_1`'s. `unanch=` 0 in both.
In verify with the reuse verifies (`pf_lfv1_1`: `BMMO_LIFETIME_LIGHT=verify`, `BMMO_SCRIPT_REUSE`, `SLOT_MEMO`,
`RESTORE_PLAN`, `TRAFO_HELD`, `SLOT_CELLS`, `REPLAY_LAST` = verify): every `mismatches=` 0, sparse verify 56 804
anchors compared, 0 differing, 0 float, 54 stale (HEAD's verify run: 56 631 / 54), unkept 0; by class compared /
stale: born 366 / 12, ended 313 / 0, order 418 / 0, own 82 / 0, mech 0 / 0 (no rebase crossed one), set 722 / 12;
its decisions are the on run's (every counter above equal) and its journals the off run's. On with the five reuse
verifies (`pf_lfo1_1`, `REPLAY_LAST` unset, `sparse=8` without `v`): every `mismatches=` 0, unkept 0, counters and
journals the on run's. The relatch marks (`marked:` over the 13 clients): walked 1 171 (1 051 off), unsourced 0,
previous 0/0, kept 0/0, raw 25/1 146 (105/946 off), restored 97 (75 off). In the marked branch (`script_replay.hpp`)
`raw=` counts marked walks whose source block (record t - 1 / the kept source) differs from the walked bytes, and
`previous=` / `kept=` the walks the reuse rule would have copied (a quiet, reusable root). The marks did wait
through light lifetime ticks: 120 more marked walks, and the kept source served 200 more of them where t - 1 (now
light) served 80 fewer. But the marked root was never quiet and reusable when walked, so the denial branch
(`previous=` / `kept=`) did not run. Every session's `mism` 0 in all four. Frame times on the 8845HS
were not measured.

**After the review** (the body-set-only tick and the reappearing name kept whole, above), the four runs again on
the fixed tree: off `pf_lfx2_1`, verify with the six verifies `pf_lfv2_1`, on with the five reuse verifies
`pf_lfo2_1`, as shipped `pf_lfs2_1`. `lifetime_reappeared` is 0 in every session, and every counter above equals
its first run's (lfx2 = lfx1 = `pf_mgs_1`'s counters, lfv2 = lfv1, lfo2 = lfo1, lfs2 = lfs1; the times aside):
sparse verify 56 804 / 0 / 0 / 54, the per-class compared / stale as above, unkept 0, the `marked:` totals, births
and lives. Every `mismatches=` 0 (468 lines), every session's `mism` 0; the 13 clients' and the server's journals
of lfx2, lfv2, lfo2 and lfs2 equal past the build id to each other's, to `pf_mgs_1`'s and to lfs1's.

**The review's other points.**
- *A body that goes on a lifetime tick* under its generation stays light: a later rewind anchored in [t, next grid)
  rebases to the grid tick before t, where the body is recorded, and restores it from there - what a rewind
  anchored in [grid, t) already did under SRT4's rule; SRT4's whole record of t would have had no opinion about it
  (absent at the base, its lifetime's oldest record before it). That case is kept as built.
- *The dropped input replays* (12, one a client, above): a rebased rewind re-recorded the light birth tick 1052
  with the relayed frame before the correction of 1052 arrived; the final state is the same (journals equal) and
  the frames between are corrected sooner.
- *The frame cost* is unmeasured: 2 111 fewer whole records against 2 812 more replayed ticks, 810 more births and
  344 more lives recreated. The 8845HS timing decides.
- `note_birth_read` logs a read that is not kept whatever the switch; with it off no birth was read from a light
  record (0 reads in every off run).
- `lifetime_compared` counts an anchor sparse verify compared, also one that differed (0 differed here; the
  differing ones are in `verify=`'s second field).
- The fast LBM with the switch as shipped (`lfr`) and in `verify` (`lfw`), run by the review on the tree before the
  fix: 38 of 38 twins FINAL exact each, the FINAL criteria rows equal to the resync cut's run; the mech class was
  crossed three times there (a8_r1, a8_r2, a8_r3, client 3), all compared exact. Not re-run after the fix, which can
  only keep more ticks whole (a sector body that keeps its generation).

**Not exercised**: a body-set-only tick (the off runs' whole + light equal their replayed ticks, so it never ran;
`set` rides along with another class; the unkept counter cannot see it) and a reappearing name
(`lifetime_reappeared` 0 on L6; the unit test checks the whole / light decision only, its fake world remakes the
body either way), the mech class across a rebase on L6 (48 records, none crossed; three in the fast LBM before the
fix and in its gate run `lgv`; three on retail L11, not compared there), the relatch's denial branch, an unkept read outside the unit test, an anchor refused at the reach edge,
`move_birth` on a light record, the dump and compare tools with an explicit `--sparse-capture`, the 8845HS
timing, the wire check.

**Gate.** 852/852 (three new tests: `RollbackEngine.ALifetimeTickOfAReplayIsRecordedLightAndKeepsTheExtrasOfItsBirths`,
a birth read from a lifetime-light tick and a rebase across it equal to SRT4's world, off / on / verify;
`RollbackEngine.ABirthReadFromALightRecordThatDidNotKeepItIsCounted`, the unkept counter;
`RollbackEngine.ABodyThatComesBackInItsLifetimeKeepsItsTickWhole`, the body-set-only tick and the reappearing name
kept whole); x64 BUILD-OK, x86 CLIENT-BUILD-OK; the L6 re-drive off (`pf_lfx2_1`), as shipped (`pf_lfs2_1`), on
with the five reuse verifies (`pf_lfo2_1`) and in verify with the six verifies (`pf_lfv2_1`), as above (and the
same four on the tree before the review's fix, `pf_lf?1_1`).

**The full gate** (the final tree, the review's fix in): x64 and the tests, 852/852; the x86 client; the Arch server.
The four L6 runs above, the journals checked again by the gate: the 13 clients' and the server's equal past the
build id, `pf_lfx2_1` to `pf_mgs_1`, and `pf_lfs2_1`, `pf_lfv2_1`, `pf_lfo2_1` to `pf_lfx2_1`. Fast LBM with the six
verifies and `BMMO_LIFETIME_LIGHT=verify` (`lgv`) and as shipped (`lgn`): 38/38 twins FINAL exact each, every
criteria row equal to the resync cut's run, 0 mismatches; 621 lifetime records, 837 births kept, 285 birth reads, 0
unkept; 440 rebased anchors compared in `lgv`, 0 differing, 0 stale (born 96, ended 18, own 27 among them). Retail at
K = 20, L12 and L11 at human key rates, as shipped (`lrs`): 0 mismatches, FINAL rows bit-equal to the restore skip's
sessions with L11's 32 trafo pieces; 7-37 lifetime records a client, 0 unkept, 0 reappeared, the mech class crossed
by 3 rebased rewinds on L11 (the retail sessions take no verify).

## Built (a heavy frame records its live tick light, step LVR, 2026-11-13)

**What it costs today.** Every frame recorded its live tick whole: the deferred live record, captured in
`rollback_engine::amend_record` after the frame's corrections. In the opening of the public L6 record the frames
over the 15.15 ms budget on the 8845HS are frames that replayed, and in them the live capture is 0.9-1.9 ms of
the frame. The offline model (`.local-tools/work/LIFE/anchor_model.py`, over the session log's rollback lines)
found that such a heavy frame's live record is re-recorded by a later replay before any rollback bases there: a
whole live record of a heavy frame pays for a base nobody restores. The spike's A/B on the 8845HS (the slow and
hooks diagnostics build, client 0 pinned, two interleaved pairs, `BMMO_LIVE_RECORD` whole / light at 10.5 ms;
`.local-tools/work/LIVE/lv_compare.txt`): frames over 15.15 ms 4 -> 1 and 4 -> 1 of 33 232, max 17.0 -> 16.3 and
17.5 -> 16.6 ms, p999 12.4 -> 12.0 and 12.4 -> 12.1 ms, p99 unchanged (10.2-10.3); 41 and 46 light live records on
client 0; the light frame's live capture 1.07 and 1.18 ms cheaper (mean; 0.66-1.89); the frame after it +0.16
and -0.30 ms on average, its tail up to +2.2 ms in frames far under the budget (8.00 -> 10.19, 2.86 -> 4.58 ms), none
of them over it. The one frame over the budget in the second light run (16.55 ms) is a light frame itself, its
replay's whole records 1.1-1.6 ms dearer than in the other runs.

**The rule.** On a host whose live step runs between `pre_step` and `post_step` in one pass
(`session_capabilities::live_step_in_frame`, declared by the headless client only; retail is whole, below), a heavy
frame - it replayed (the engine's `resim_ticks` moved since `pre_step`'s entry), its work
since `pre_step`'s entry is at least `BMMO_LIVE_RECORD_MS` (default 10.5) when `post_step` reaches the live
capture, and no resync is pending, in flight or holding its state - passes `heavy` to `amend_record`. The engine
records the tick light (`full = false`, `tick_state::live_light`) where the replayed ticks' rule would: sparse
capture on, not a grid tick, no rows, a record of t - 1 with the same tracked set and generations
(`live_light_before`; a lifetime change keeps the live tick whole: it keeps no births) and the same body names
after the capture (`same_body_names`, now also SRT4's body-set test in `capture_replayed`; a light capture that
finds another body set is captured whole again). Only the record's one capture (the awaiting branch): a second
amend of the tick, a record captured at `record()` (the headless dump and compare hooks) and a resync anchor stay
whole. `capture()` clears `live_light`, so a later replay's re-record decides the tick anew by its own rule. Code:
`client_core_frame.cpp` `post_step` (the gate) and `pre_step` (`frame_.started`, `frame_.resim_before`);
`rollback.hpp` `amend_record`, `set_live_record_light`, `live_light_before`, `same_body_names`.

**Each reader, and its answer.**
- *The restore base* (`restore_base`, `rewindable`, `restorable`): skips a light live record as any light record;
  a rewind anchored at or after it rebases to the whole record before. The grid ticks stay whole, so a rebase is
  still at most `sparse=8` - 1 ticks deeper and `kSparseCaptureMax` and the reach edge are unchanged; an anchor
  deeper than the standard reach that loses its only whole base is refused as `sparse_unanchored` (0 in every run
  below; the deepest added rebase 7 ticks).
- *The capture cache and the script reuse source of the next live capture* (`capture`'s record of t - 1): need a
  whole t - 1. After a light live tick the next live capture has neither: it reads every extra and contact and
  its script capture copies from the kept source (`keep_source` runs only in a whole script capture, so the kept
  source is the last whole one) where the ledger, which a light capture observes (`observe_scripts`), says a root
  stayed quiet. A relatch's mark waits through the light tick as through any light record. The next replay's
  records start at a whole base, as before. This is the path only a light run exercises (verify captures the
  tick whole and keeps the source): `pf_vro1_1` below, every reused copy verified.
- *Birth reads* (`birth_record`, `moved_birth_record`, the remade life's `find_const(remade_at)`): a live tick is
  light only with the tracked set, generations and body names of t - 1, so no lifetime begins there. A record
  becomes a birth record later only if `move_birth` erases the name from the records before it (an applied tick
  on a light live record), as for any light record of SRT4's; such a read is counted by `note_birth_read`
  (`lifetime_birth_reads`, a fault in `lifetime_birth_unkept`, logged): 0 unkept in every run below.
- *Resync*: the anchor S is the first record of a new history (`anchor_resync`'s `record()`, captured at once, no
  record of t - 1), never light; the gate excludes a frame with a resync pending, in flight or holding its state.
  An exact resync's base and its `recorded_scripts(S)` are that record.
- *Snapshots*: `on_snapshot` refuses a light record; a session with snapshots runs `sparse=1`, where
  `live_light_before` refuses every tick.
- *The core's uses of the base* (`merges_input_replay`, the sector re-derive in `rewind_for_sectors`, the held
  and trafo reach checks, the calibration rewind): read the deeper base as for any light anchor. On the L6
  re-drive the rewinds are 72 749 in every run below, light or whole.
- *The replay's own check against the live record* (the scripts and the blob of `current_tick`, kept aside before
  the replay): needs a whole live record, so after a light live tick it does not run (as after RLL's light last
  tick); a diagnostic no gate reads. Verify captures the record whole and keeps it.
- *`restore_candidates`, `generation_now`, `recorded_body`, `recorded_input`, `recorded_globals` (RK3's log line
  right after the amend), the retire observer and the journal*: read bodies, navigation, the clock, the globals,
  tracked sets and inputs, which a light record keeps.
- *The dump and compare tools*: the headless client forces `sparse_capture = 1` with `--dump-savestate` or
  `--compare-scripts` (`session_client.cpp`) and captures the live record at `record()` there, so neither sees a
  light live record; SimTool's engines run without sparse capture and never pass `heavy`.
- *The retail host* (`core_host.cpp`): records every live tick whole. One OnProcess runs `post_step(T)`, the
  mailbox and `pre_step(T + 1)`, and the engine steps T + 1 after it, with the render and the frame's pacing wait;
  the amend of T + 1 is in the next OnProcess. The span from `pre_step(T + 1)`'s entry to that amend is about a
  frame period, so at any threshold under one frame every retail frame that replayed would be heavy (the
  review's D1), and the light record would save time in the OnProcess after the replay, not in the one that
  replayed. The span also read `steady_clock`, outside the K7 capture's clock sites, while the decision moves the
  replayed ticks the cost meter divides the captured `rewind_*` reads by, and the cap from it the reach: a strict
  K7 replay run at another frame rate could choose otherwise and diverge (D2). Retail does not declare
  `live_step_in_frame`, so the core reads `BMMO_LIVE_RECORD` as `whole` there, the gate reads no clock and counts
  nothing, and the status line says `live=whole/.../0/0/...`. Nothing in `core_host.cpp` changed, no clock site
  was added and the K7 captures replay as before. The follow-up, once retail is measured: a host measure of the
  core's own work for the tick (`pre_step(T + 1)`'s duration plus `post_step(T + 1)`'s up to the amend), read at
  a new recorded clock site through `platform_.now` (appended before `count_`, not free in `retail_capture.hpp`,
  no read with `whole`, so older captures replay with it), with a retail threshold of its own.

**The clock.** The gate reads `std::chrono::steady_clock` (the C++ standard library, as the engine's capture
timers do), not `platform_.now`: in the coordinated headless runs - the L6 re-drive, the fast LBM, the 8845HS A/B -
`platform_.now` is the coordinator's virtual clock, which does not advance while a frame works, so no frame would
be heavy there. The decision is therefore not deterministic and not in any capture. It decides no record that a
rewind restores, but it decides what stands at a light live tick that a later rebased replay crosses: the live
state where the live record would have been restored, or the replay's. The two are equal in every byte but where
the live tick and its replay differ in float only - the fast LBM's `a4_swing` tick 1671 below, three proximity
hold frames that a replay run after a module frame moved places otherwise (traced below; at HEAD too, gone at the
next tick, never read). So the wall clock chooses between the live and the replayed value of those frames; the
journals below are equal with it light and whole. A strict replay can see the decision through what the deeper rebases
change: the replayed ticks the rewind cost meter divides the captured `rewind_*` clock reads by, and a rewind a
deeper replay makes unnecessary (one LFL saw); a replay whose cost cap does not bind and whose rewinds are the
capture's keeps its journal. The headless `--replay-inbound` has `--replay-free-cost-clock` for that. With
`BMMO_LIVE_RECORD_MS=0` the decision is a function of the session alone (a frame that replayed, no resync under
way), the same in a capture and its replay: a headless capture meant for a strict replay can be taken with it, or
with `BMMO_LIVE_RECORD=whole`. One message carries a count the decision moves: the A6 telemetry's `replayed` (the
headless client with `--telemetry`), beside its frame times, which are wall-clock reads already; the reach
(`max_resim_ticks`) follows the cost meter.

**The switch and the counters.** `BMMO_LIVE_RECORD` = `whole` | `light` | `verify`, read once per process
(`client_core.cpp` `live_record_from_env`, beside `lifetime_light_from_env`), light when unset;
`BMMO_LIVE_RECORD_MS` the threshold, latched at the session start (`strtod`; text that is not a number keeps 10.5,
where `atof` read 0 and made every frame that replayed heavy). `whole` keeps every live record whole and
still counts the heavy frames the rule allows. `verify` records the tick light and turns sparse capture's verify
on: the record is captured whole all the same and still marked light, so every decision is the light one, and
`verify_sparse_anchor` compares every rebased anchor. The engine's `set_live_record_light` is off by default (the
tools and the unit tests' engines). The status line's sparse section (retail reads the same line, with `live=whole`) ends with
` live=whole|light/ms/eligible/light/rebased+extra/max/deep/verified/differing`: the heavy frames' live records the
rule allowed light, those made light, the rebased rewinds whose (base, anchor] held a light live record, the ticks
they added (the base a whole live record would have given less the base taken), the most, those that added 4 or
more, and sparse verify's compares of an anchor that was a light live record with their differences. The
crossings and their ticks are a lower bound of the cost: a replay's re-record of the tick clears its mark, and
records it light by the replayed ticks' rule, so a later rewind anchored at or after it is rebased deeper again
uncounted, where a whole live record would still have been its base; the runs' deltas are the cost. The light
count counts a record made light even when a second amend of the tick later captures it whole. Sparse verify's
float-only differences of a light live anchor are not in `live=`'s last number (it counts only real
differences): they are in `verify=`'s third field, which a gate reads with it. Verify shows as `live=light` with
`sparse=8v`.

**The runs** (the L6 re-drive, dev machine, 13 clients, one binary; totals over the 13 clients):

| | live eligible / light | rebased + extra ticks | replayed ticks | whole / light replayed | rewinds |
|---|---|---|---|---|---|
| whole (`pf_vrx1_1`) | 360 / 0 | 56 858 + 212 939 | 663 877 | 56 023 / 607 854 | 72 749 |
| as shipped (`pf_vrs1_1`) | 218 / 218 | 56 859 + 212 945 | 663 883 | 56 023 / 607 860 | 72 749 |
| whole, MS=0 (`pf_vrw1_1`) | 63 055 / 0 | 56 858 + 212 939 | 663 877 | 56 023 / 607 854 | 72 749 |
| light, MS=0 (`pf_vro1_1`) | 63 055 / 63 055 | 59 238 + 226 100 | 677 038 | 56 023 / 621 015 | 72 749 |

As shipped on the dev machine 218 frames (17 a client) were heavy at 10.5 ms; one rebased rewind crossed a light
live record, 6 ticks deeper. With the threshold at 0 every frame that replayed records light where the rule
allows (63 055 of them, 4 850 a client): against the whole run at MS=0, 2 380 more rebased rewinds and 13 161
more rebased ticks, replayed ticks +13 161 (+2.0 %), 63 055 fewer whole live captures; the rewinds are the same
72 749. The `live=` field counts the first crossings of 2 064 of them, 7 892 ticks, at most 7, 1 071 of 4 or more. In verify with MS=0 and the six verifies
(`pf_vrv1_1`: `BMMO_LIVE_RECORD=verify`, `BMMO_SCRIPT_REUSE`, `SLOT_MEMO`, `RESTORE_PLAN`, `TRAFO_HELD`,
`SLOT_CELLS`, `REPLAY_LAST`, `LIFETIME_LIGHT` = verify): every `mismatches=` 0, sparse verify 59 184 anchors
compared, 0 differing, 0 float, 54 stale (LFL's verify run: 56 804 / 54); 2 064 light live anchors compared, 0
differing; its decisions are `pf_vro1_1`'s (every counter in the table equal). The script reuse with the five
reuse verifies, light (`pf_vro1_1`) against whole (`pf_vrw1_1`), both at MS=0: captures 190 985 / 254 040 (63 055
fewer whole live captures), with record t - 1 87 619 / 125 255, roots reused 4 201 332 / 5 588 542, walked 1 717 643
/ 2 285 124 (22.0 reused and 9.0 walked a capture in both), every reused root verified equal to a walk (0
mismatches in both). The captures with no record of t - 1 whose source was the kept one (`kept:`, by who kept it:
a capture 35 908 / 56 023, a restore 67 445 / 72 749) and those with record t - 1 account for the 63 055 captures
not made: 37 636 + 20 115 + 5 304 (the counts are of captures, so a capture not made takes its count with it;
as shipped, `pf_vrs1_1`, the restore-kept one is 72 747 against 72 749, two of its 218). In the whole runs the
restore-kept count equals the rewinds, 72 749. Roots copied from the kept source 2 273 766 /
2 832 984, refused 0 / 0; the relatch marks walked 1 157 / 1 171, unsourced 0, previous and kept 0/0, raw 25/1 132
/ 25/1 146, restored 127 / 97. Lifetime records 3 299 / 3 228, 1 749 birth reads, 0 unkept, 0 reappeared;
`unanch=` 0 everywhere. Every session's `mism` 0 in all five runs. Journals: the 13 clients' and the server's of
`pf_vrx1_1` equal HEAD's shipped run (`pf_lfs2_1`) past the build id; those of `pf_vrs1_1`, `pf_vrv1_1`,
`pf_vro1_1` and `pf_vrw1_1` equal `pf_vrx1_1`'s. `pf_vrx1_1`'s counters are `pf_lfs2_1`'s.

**The fast LBM** (38 twins, the coordinator's virtual time, so the threshold reads the dev machine's wall clock).
As shipped (`vrn`): 38 of 38 twins FINAL exact, every FINAL criteria row equal to the resync cut's run (`rz1`);
108 light live records over the 140 sessions, crossed by no rebased rewind, so this run proves no rebase across
one; every other replay counter equal to LFL's as-shipped run `lgn` (rewinds 1 711, replayed ticks 37 377,
rebased 440 + 1 405), its script captures 108 fewer. In verify at MS=0 (`vrq`, `BMMO_LIVE_RECORD=verify
BMMO_LIVE_RECORD_MS=0`, `sparse=8v` in every session): 38 of 38 FINAL exact, the rows equal to `rz1`'s, 0
mismatches; 1 425 light live records, 12 rebased rewinds crossed one (70 ticks, at most 7), rebased 452 + 1 475,
replayed ticks 37 447; sparse verify 452 anchors compared, 0 differing, 0 stale, and **6 float**: the 12 light live
anchors compared include 6 that differ from their record in a float script parameter only, which sparse verify
counts apart and not as a difference (as the replay's own check does, `script_float_mismatches`). LFL's verify run
`lgv` had 0 float, and so have the L6 runs here (2 064 light live anchors). All six are one tick: the live record of
1671 in `a4_swing` seed 0 and seed 2, the off twins, clients 1-3 each, rebased from 1664 by the rewind of the input
correction at 1672; the difference is `proximity hold: frame 9 differs [float] (0 logical, 1 float)` - one probe
frame's position in the proximity union's hold section; bodies, the blob and every logical script byte equal. In
that live frame a remote's spawn ghost turned solid (`spawn_ghost_frame` logs it; it reads the state and writes
the display only). The replay's own check of a live record against the re-executed scripts reports
float-only differences of the same kind in these arms without this step (`script replay: the scripts re-executed
to tick N differ in float parameters only: proximity hold: frame K differs [float]`: LFL's `lgn`, 25 such lines,
and `vrn` alike), so the live record's hold frames are sometimes not what a replay of the tick leaves, in float
only. Those lines in `lgn`'s two arms (read in the review round) are at the ticks a replay ended on and was
checked: seed 0 off 272, 928, 1088, 1168; seed 2 off 928, 1088, 1128 - none at 1671 and none at a tick where a
ghost turned solid (seed 0: 353, 680, 682, 701, 770, 1174, 1191, 1194, 1201, 1671; seed 2: 353, 680, 682, 701,
770, 781, 1174, 1190, 1194, 1198, 1671). No baseline replay was checked at a ghost-to-solid tick, so this says
nothing either way about 1671: inconclusive. Earlier verify runs of the arm (`kvg`, `lfw`, `lgv`, `mlv`, `rlv`)
read 0 in `verify=`'s third field only because none compared a light anchor at 1671.

*Traced* (one diagnosis, logging-only builds; runs `vfa` whole, `vfb` verify, `vfc` verify with the probes'
references logged, all at MS=0). A probe with no slot is placed on the ball nearest to its block's ObjectB, read
with `object_b->GetPosition()` (`proximity_union.hpp` `place()`). For three probes that reference is a module frame
of sector 2 (`P_Modul_25_MF`, `P_Modul_29_MF`, `P_Modul_30_MF`), a level entity with no body, whose position no
record carries and no restore writes. In the live run those frames sit at the origin up to `place(1676)` and at
their level positions from `place(1677)` on (moved in live tick 1676, four ticks after sector 2's activation stamped
for 1672; the block that moves them was not traced). A rewind run after 1676 and anchored in [1664, 1675] replays
ticks before 1677 against the moved frames, and `place()` picks the own ball where the live run picked the mirror:
frames 9, 10 and 15 of the hold section (the verify line names the first), the per-slot copies of each ball equal,
so the balls' positions are equal and only the choice differs. The ghost turning solid at 1671 is a coincidence,
and neither the capture's phase (the capture equals `place()`'s output at every tick, live and replayed), a stale
ghost pose (the four candidate bodies bit-equal) nor a relatch writes them. It is at HEAD without this step: in
whole mode (`vfa`) the rewind at 1713 restores 1671, its `place` line for 1672 is byte-equal to the live one, and
its record of 1672 differs from the live record of 1672 in the same hold bytes; HEAD never compares those ticks.
It is gone at the next tick and never read here: `place()` rewrites a no-slot frame on every tick with a
candidate, the three probes' block and root script were inactive at every place from 1640 to 1677, live and
replayed, and run first at 1678 with the same choice in both; from 1672 on `vfa` and `vfb` are equal for clients
1-3 (the hold captures of the 1713 replay at 1672, 1680, 1688, 1696 and all 58 replayed place lines with the
bodies' bits, 1672-1700); the journals of `vrn2` and `vzl` (light at MS=0), `vfa` and `vfb` are equal, every twin
FINAL exact. So a light live record only lets a rebased replay cross 1671 where HEAD restored it; the default
(light, headless) stands. The gap itself - a body-less reference that a replay reads at its moved position - is
its own correctness item: a reference moved back while its probe is active (a sector deactivation, a level reset)
could make a replay choose otherwise where the block reads it. Its fix is client-local: the record carries, and the
restore puts back, the positions of the body-less ObjectB references the union reads. The `live=`
field's last number counts only differences; the float-only ones are in `verify=`'s third field.

**The review round** (one review: no exactness defect; two gate defects, both retail, fixed by the capability
above; the float anchors traced after it, above). D1 (on retail the threshold did nothing) and D2 (the gate's clock
reads outside the K7 clock sites) are closed by `session_capabilities::live_step_in_frame`: the headless client
declares it (`session_client.cpp` `make_capabilities`); without it the core reads `BMMO_LIVE_RECORD` as `whole`
and the gate reads no clock. The headless rule is unchanged: the five L6 runs again on the fixed tree, with new
labels, give every counter of the first round's at MS=0 - `pf_vrv2_1` (verify, the six verifies), `pf_vro2_1`
(light, the five reuse verifies) and `pf_vrw2_1` (whole, the same) equal `pf_vrv1_1`, `pf_vro1_1` and
`pf_vrw1_1` in every count of the status and reuse lines (sparse verify 59 184 / 0 / 0 / 54, 63 055 light, 2 064
crossings + 7 892 ticks, replayed ticks 677 038 / 663 877, rewinds 72 749, reuse 0 mismatches) - and the
wall-clock runs differ only where the wall clock reads: as shipped (`pf_vrs2_1`) 324 eligible / 324 light (218 in
`pf_vrs1_1`), 7 rebased rewinds crossed one (29 ticks, at most 7, 5 of 4 or more), rebased 56 865 + 212 982,
replayed ticks 663 920, rewinds 72 749; whole (`pf_vrx2_1`) 263 eligible / 0 light, every other counter
`pf_vrx1_1`'s. Every session's `mism` 0 and every `mismatches=` 0 in all five; the 13 clients' and the server's
journals of `pf_vrx2_1` equal past the build id to `pf_vrx1_1`'s, `pf_lfs2_1`'s and those of `pf_vrs2_1`,
`pf_vrv2_1`, `pf_vro2_1` and `pf_vrw2_1`, 14 of 14 each. The fast LBM as shipped again (`vrn2`): 38 of 38 twins
FINAL exact, every FINAL criteria row equal to `rz1`'s, 0 mismatches; 178 light live records (108 in `vrn`), and
this time one rebased rewind crossed one (`a10x` off, client 0, 2 ticks); rebased 441 + 1 407, replayed ticks
37 379, rewinds 1 711, 0 unkept; the clients' journals of every arm equal `vrn`'s past the build id (located per
file: the 12 hex digits after `+bmmo-`, byte 91 in these arms' compressed headers and 92 on L6), 166 of 178, the
other 12 the Level 13 server journals, which differ between any two runs of the fast LBM (`lgn` against `lgv`
too). The rest of the review is in the paragraphs above: the clock comment reworded (the wall clock chooses
between the live and the replayed value of a held float), `BMMO_LIVE_RECORD_MS` parsed with `strtod`, and the
counters' notes (lower bounds, a light count kept through a later whole amend, `verify=`'s third field).

**Not exercised**: a live tick refused for another body set alone (not counted on L6; the unit test only), a birth
read from a light live record (move_birth's applied tick on one), an anchor refused at the reach edge, a strict
replay of a headless capture made with the step on, the block that moves the module frames, the retail
host after the capability (built, not run: whole by construction), the 8845HS timing of the product build (the A/B
above is the spike's, with the same rule), the wire check.

**Gate.** 854/854 (two new tests: `RollbackEngine.AHeavyFramesLiveRecordIsLightAndNeverABase`, a rewind anchored
at and after a light live record rebased to the whole record before and arriving at the all-whole engine's world,
whole / light / verify, verify keeping the bytes and comparing the anchor;
`RollbackEngine.AHeavyLiveTickWithALifetimeChangeOrAnotherBodySetStaysWhole`, a mechanism added, a grid tick and
another body set kept whole, every tick whole without sparse capture); x64 BUILD-OK, x86 CLIENT-BUILD-OK; the L6
re-drive whole (`pf_vrx2_1`), as shipped (`pf_vrs2_1`), whole and light at MS=0 with the five reuse verifies
(`pf_vrw2_1`, `pf_vro2_1`) and in verify at MS=0 with the six verifies (`pf_vrv2_1`), as above; the fast LBM as
shipped (`vrn2`), 38/38 twins FINAL exact, every criteria row equal to the resync cut's run; the first round's runs
(`*1`, `vrn`, `vrq`) were on the tree before the capability, with the same headless code. On the final tree (the
docs aside): x64 BUILD-OK and 854/854 again, x86 CLIENT-BUILD-OK (no C1128), the Arch server ARCH-BUILD-OK; retail at
K = 20, L12 and L11 at human key rates (`lvg`): 0 mismatches, every `mismatches=` 0, FINAL rows bit-equal to the restore
skip's sessions (`r6on`) with L11's 32 trafo pieces, every client `live=whole/10.5/0/0/0+0/0/0/0/0` (no frame eligible:
the capability keeps retail whole).

## Built (a replay restores the body-less references the proximity union reads, step OBJB, 2026-11-13)

**The gap** (the LVR section's *Traced* paragraph; the census in `.local-tools/work/OBJB/`). A probe with no slot
is placed on the ball nearest to its block's ObjectB, read with `object_b->GetPosition()` (`proximity_union.hpp`
`place()`). Where that ObjectB is a module frame or a body-less child of one, it is a level entity whose pose no
record carries and no restore writes. The sector manager moves it once: `Gameplay_SectorManager / Activate Sector
/ activate Type 1 / Set World Matrix (Hierarchy=TRUE)` writes each PH row's matrix to the row's module frame, four
ticks into the sector's activation (the delayed link `Deactivate Sector.Out 0 -> Set Cell.In`, delay 4), then
shows the module and activates its script. So a rewind run after the move and anchored before it replays the
ticks up to the move against the moved frame, and `place()` can choose another ball than the live run chose. In
the fast LBM's a4_swing arms that is `P_Modul_25_MF`, `P_Modul_29_MF` and `P_Modul_30_MF` (moved in live tick
1676, sector 2 stamped for 1672), the six float-only anchors of sparse verify at tick 1671 (`vrq`: hold frames
9, 10 and 15). Harmless there - the frames are placed again every tick and the blocks run first at 1678 - but a
reference moved while its probe is active, or moved back, would make a replay choose otherwise where the block
reads the frame. The census (mechanism-class probes, placement clones on): 9 roots in the arena, 3-42 per stock
level (16 on L6); with clones on each root moves exactly once (its sector's first activation + 4 ticks), always
before the module's `Activate Script` - "once" rests on the union never being torn down inside a level, so a level
restart is not covered (memory "level-reset joint rebuild"). No mechanism script has another Set World Matrix /
Set Position on a frame; whether the module scripts' `TT Restore IC (Hierarchy?=TRUE)` on their False branch (and
`Event_handler`'s three `restore Objects`) touch the frame is not known - the dumps do not print a block's target
(the review's point). Modules 08, 17, 26 and 41 have body-less referential frames read when their joints are
created, under roots no probe names and so not tracked: a forward move is safe, a move back is as at HEAD.

**The rule.** The union's hold section (local, record v6 bit 2) carries, after the by-name copies, one generation
per reference root - the topmost ancestor of a rewired probe's ObjectB - and a restore puts back the pose the
record names. Layout (`session/proximity_hold.hpp`): `u16 roots, u8 flags, roots x u32 generation`, and with
`flags & 1` (verify) `u16 references, references x f32 x, y, z` (each probe's ObjectB position). Each root has a
history of the world matrices it was observed at (bit for bit, 16 floats) and the generation the world is at:
an observation of the current pose changes nothing; of a later pose of the history advances to it (`remade`: a
replay making the live move again); of another pose drops the poses after the current one and appends it
(`moved` live, `truncated` when there was a tail) under a generation never given before for that root. So a
generation names one pose for the life of the rewire: a record naming a pose a truncation dropped (one left past
an anchor whose replay stopped early, or SimTool's pristine-engine copies) is refused rather than read as another
pose, and a replay that moves a root elsewhere than the live run compares logical against the live record (the
first cut reused the dropped number, so both of those passed silently: the review's first defect). Every record
at or before a rewind's anchor names a pose the history still holds; the records after it are re-captured by the
replay. The history keeps at least 1024 poses a root: once it holds 2048 the oldest 1024 go at once (amortized,
where dropping one at a time would copy 64 KB an observation for a root that moves every tick).

- *Capture* (`proximity_union::capture`): observes every root, then writes the tail - after the tick's step,
  so a whole record holds the tick's end pose. Light records have no hold section; a rebase restores the whole
  record before them.
- *Restore* (`proximity_union::restore`): observes every root first - the replay since the last restore may
  have re-made a move at a light tick no capture observed, and a stale index would leave the gap open (verify
  mode, which captures every tick whole, would not show it) - then checks the section's shape (extent, root and
  reference counts; another shape fails the restore, as a frame or copy count did at HEAD) before it writes
  anything, and looks each root's generation up: an unknown one (dropped, or past the depth) is refused and that
  root keeps its live pose, as at HEAD, and so does a root whose entity is gone - counted `refused=`, the first
  named on the line, the rest of the section restored (the first cut failed the whole script restore there, after
  the scripts, cells, trafo states and queue were written and before the membership fix-up: the review's second
  defect). Then the frames and copies as before, then each root whose generation differs: `SetWorldMatrix(pose,
  KeepChildren FALSE)`, so a body-less child follows through its local matrix, as the level's own Set World Matrix
  moves it. A restore that finds a root behind its history (a move present that the world did not make again)
  counts it `unmade` and names the first on the line: a replay that stopped early (`rollback.hpp`'s `break` on a
  failed `world.step()` or `put_globals`, which leaves a written root at its old pose in the live run where HEAD
  left it moved; the review's third defect, a failure path) or a writer outside the replay. It shows at the next
  restore only, not at the stop itself; the gate reads it as a fault (below).
- *Order*: `script_replay::restore` now calls the hold restore after its last step that can refuse (the
  written cells, the trafo built states, the joint order, the PreSimulate queue), so a refused restore leaves no
  root moved back under live scripts; nothing between the old place and the new reads a frame, a copy or a root.
  So a restore refused by one of those steps no longer writes the frames and copies back either (it did before).
  It is still inside `restore_scripts`, so before the window births are destroyed and put back (a local matrix
  then lands relative to the restored frame), before the IVP savestate and before the replay's first `place`.
- *Bodied*: at the first `place()` after a restore, a written root with a body anywhere under it is counted
  (`bodied=`, the first name kept): an entity write of a body the anchor had would derive its local matrix from
  the written root. Checked there, not at the write, because the window's births still have their bodies inside
  `restore_scripts`. Not expected with clones on (a module's parts are physicalized after its frame moved).
- *Resync*: `localize_hold` writes every root as `kReferenceKeep` (0xFFFFFFFF): the portable record and the wire
  are unchanged, and an exact resync keeps the live poses for its re-step, as before. The keep-only tail does not
  stay in the history: the anchor record is captured again after the resync restore (`rollback.hpp` ~2246,
  `capture(world, *at)`), so the records from the resync tick S on carry the generations the world is at - the
  behaviour is HEAD's, not wider. Not done (no wire change needed): splice the root tail of the client's own record
  at S into `at->scripts = rs->scripts` (`rollback.hpp` ~1412), so a resync whose base lies before a move restores
  it too.
- *Compare*: in the record diff a root generation that differs is logical; the verify positions are not compared;
  a frame stays float (a frame copies the position of the candidate `place()` chose, and a correction of that
  ball moves it - the replay's own check compares against a live record made before the correction). The
  hold's layout and its compare are `parse_hold` / `compare_hold`, which `script_replay` calls.
- *Scope*: a union rewired in probe class `mechanism` (the retail and headless clients; a SimTool world rewired
  in that class too, whose step-state restore then writes roots). The server's world (class `all`) tracks no
  root: it rewinds no record. The roots of the unit (slot) probes are tracked too, and not only for the cost:
  `place()` never reads their ObjectB, but the proximity block itself reads it when it runs in a replayed tick.
- *Two reads of ObjectB*: the rewire takes `GetInputParameter(2)->GetRealSource()` without an update (as the
  structural test reads it), `place()` takes `GetInputParameterObject(2)`. Were they to differ (an ObjectB fed by
  a parameter operation), a root would be tracked that nothing reads and verify would still pass, since it
  compares the tracked object. So verify also compares, at every `place()` of a probe with no slot, the id read
  there with the tracked one (`misread=`, its own counter: `verified=` keeps counting positions only).
- *A move and a move back between two observations* (a light tick, no capture) leaves the root at the pose the
  history's current generation names, so nothing is recorded for it - right, since no whole record could name
  the pose in between. A move back observed is a new pose under a new generation, not the older one.
- *Children of a written root*: `SetWorldMatrix(root, FALSE)` re-derives each child's world matrix as root x
  local, which is the child's live pre-move world matrix only if the child's local matrix did not change across
  the move (the level's Set World Matrix with Hierarchy moves it so). The fast LBM's writes were all on
  `P_Modul_25_MF`, `P_Modul_29_MF` and `P_Modul_30_MF`, whose ObjectB is the root itself; a body-less child ObjectB
  (perhaps `P_Modul_29_Platte06`, its parenting not confirmed) is the only child verify may have compared. So
  verify's position compares check mostly the generation bookkeeping, and the L6 re-drive's compares cover no
  write at all (none there). Untried: a clone at its source's pose (not identity), a pooled frame with clones off,
  a move back.

**Switch.** `BMMO_REFERENCE_POSES` off | on | verify, on when unset. Off tracks no root (the tail says 0 roots)
and restores none: HEAD's behaviour with three more bytes a record. Verify also carries every probe's ObjectB
position and, after a restore's writes, compares the position of each body-less one with the record's
(`verified=` / `differing=`; one with a body is the savestate's, written after), and compares the ObjectB each
`place()` reads with the tracked one (`misread=`). Counters, on the union's lookups line: `refs=<mode>/<roots>
moved= written= remade= truncated= unmade= bodied= verified= differing= refused= misread=`, then the first of the
unmade / refused / misread faults in parentheses. `truncated`, `unmade`, `bodied`, `differing`, `refused` and
`misread` are faults: a gate run passes only with all six 0 (`truncated` is not wrong in itself, but no run of
the stock replay makes one, so one is worth a look).

**C1128.** `proximity_union.hpp` is header code compiled into `core_host.cpp`, which sits at MSVC's section
limit. A first cut with the history inline in the header took the x86 object to 0x10070 sections (limit 0xFEFF),
so everything that is not a CK call lives in `BallanceMMOCommon/src/session/proximity_hold.cpp` (the history, the
tail, the check, the counters and their line, the switch) and the union keeps the CK reads and writes; the file
is a source of the client, of `BallanceMMOSim` and of the tests. `core_host.cpp` compiled with `/bigobj` alone to
count: HEAD 0xFED4 sections, this step 0xFED5 (counted again after the review's fixes: 0xFED5), which leaves
42 sections under the limit.

**Cost.** Per whole capture of a client: one `GetWorldMatrix` and one 64-byte compare per root and 3 + 4 bytes a
root (39 bytes in the arena, 67 on L6; verify adds 2 + 12 a probe). The tracker's share measured alone (g++ -O2,
`.local-tools/work/OBJB/bench_observe.cpp`): 42-43 ns a record for 9 roots, 183-189 ns for 42. A restore writes
only the roots whose generation differs (on the L6 re-drive none: its one move per client is before any anchor).

**Tests** (`proximity_rules_test.cpp`, engine-free): a restore names the earlier pose (a record after the move
writes nothing, one before it the old pose, an unknown generation is refused - nothing pending, the section still
restores, `refused=1` - and a section of another extent fails; the history's trim keeps the newest poses and
drops generation 0); a remade move keeps its generation, another move truncates under a new generation (2, not
the dropped 1: the record naming the live move is then refused and compares logical against the replayed one),
and a move re-made at a tick nobody observed is found by the restore's observation, so a rewind before it still
writes; the hold's extent and compare (verify's positions not compared, a frame float, a generation or a copy
logical).

**Verified** (first round, labels `ob*`, on the first cut: generations reused after a truncation, a refused root
failing the restore, no `misread=`):

- builds: x64 BUILD-OK and the tests 857/857 (three new); the x86 client build CLIENT-BUILD-OK (no C1128).
- fast LBM in verify (`obv`: `BMMO_LIVE_RECORD=verify BMMO_LIVE_RECORD_MS=0`, the seven verifies and
  `BMMO_REFERENCE_POSES=verify`): 38 of 38 twins FINAL exact, every FINAL criteria row equal to `rz1`'s, every
  `mismatches=` 0 (1 260 occurrences in the 140 client logs, 1 488 in every `.log` / `.out` / `.txt` / `.json` of
  the prefix's arms), every `mism` 0; sparse verify 452 anchors compared, 0 differing, **0 float**
  (`vrq`: 452 / 0 / 6), 0 stale; rewinds 1 711, replayed ticks 37 447, rebased 452 + 1 475 and light live
  1 425 (12 verified, 0 differing) as in `vrq` (which ran without the five reuse verifies), reuse 6 730 792
  verified, 0 mismatches.
  `refs=verify/9` on all 140 client logs: moved 1 072, written 63, remade 63, truncated 0, unmade 0, bodied 0,
  refused 0, verified 35 400, differing 0. Every write is in the a4_swing arms (9 a twin, 18 in seed 1 off: three
  roots per rewind anchored before 1676 and run after it); every written move was re-made by the replay. The
  anchors that wrote: 1671 (rebased to the whole record before it), 1664 and 1646 (before the activation of
  1672, which the replay issued again); none inside the delayed link's four ticks (1672-1675).
- the switch off (`obo`: the a4_swing seed 0 and seed 2 arms with `a0_first`, the same verifies,
  `BMMO_REFERENCE_POSES=off`): 6 of 6 twins FINAL exact, 0 mismatches, `refs=off/0`, and the six float anchors
  back - tick 1671, clients 1-3 of both `_off` twins, "proximity hold: frame 9 differs [float]", as in `vrq`.
- fast LBM as shipped (`obn`, the variables unset): 38 of 38 twins FINAL exact, rows equal to `rz1`'s, every
  `mismatches=` and `mism` 0, `refs=on/9` with the same 63 writes and 63 re-made moves; against `vrn2` past the
  build id (`arm_journals.py`) every client journal of every arm equal, 166 of 178, the other 12 the Level 13
  server journals of the six arms that differ between any two runs (the same 12 as `vrn` against `vrn2`). So the
  replayed choice the fix changed reached no journal - it was read nowhere, as the trace found.
- the switch alone in verify at the shipped capture cadence (`obp`: `a0_first` and the three a4_swing arms,
  only `BMMO_REFERENCE_POSES=verify`, so light records and the restore's own observation): 8 of 8 twins FINAL
  exact, 0 mismatches, the same 63 writes and 63 re-made moves as `obn`, 14 514 positions compared in its 32 client logs (13 140 of them in the a4_swing arms) 0 differing,
  unmade 0; every journal equal to `obn`'s.
- the L6 re-drive (13 clients, `run_pt.sh`): as shipped (`pf_obs_1`) every `mism` 0, every `mismatches=` 0, the
  13 clients' and the server's journals equal `pf_vrs2_1`'s past the build id (14 of 14), rewinds 72 749,
  `refs=on/16` with one move a client and no write; in verify (`pf_obv_1`, the six verifies,
  `LIFETIME_LIGHT=verify`, `LIVE_RECORD=verify` at MS=0, `REFERENCE_POSES=verify`) every `mism` and `mismatches=`
  0, sparse verify 59 184 / 0 / 0 / 54 and rewinds 72 749 / replayed ticks 677 038 as `pf_vrv2_1`, journals 14 of
  14 equal to `pf_vrv2_1`'s, 23 934 421 reference positions compared 0 differing; with the switch off
  (`pf_obf_1`) journals 14 of 14 equal to `pf_vrs2_1`'s. Client 0's frame times (not pinned, this machine): on
  p99 8.8 / max 12.5 ms, off 9.3 / 17.4 - inside the run-to-run spread; the record cost is the bench's.

**Verified again after the review** (labels `ob2*`, the tree as it stands: generations never reused, a refused
root kept, `unmade` / `refused` / `misread` named; `.local-tools/work/OBJB/run_ob2.sh`, aggregated by `agg2.py`,
which prints a FAULT line for any of the six fault counters or any nonzero `mismatches=` / `mism`):

- builds: x64 BUILD-OK and the tests 857/857 (the three OBJB tests extended, none added); x86 CLIENT-BUILD-OK (no
  C1128); `core_host.cpp` with `/bigobj` alone 0xFED5 sections, as in the first round.
- fast LBM in verify (`ob2v`, the same variables as `obv`): 38 of 38 twins FINAL exact, the 38 criteria rows equal
  to `rz1`'s; `mismatches=` 1 260 in the client logs and 1 488 in every file, all 0; `mism` 0 in all 140 logs;
  sparse verify 452 / 0 / 0 float / 0 stale, rebased 452; `refs=verify/9` on all 140: moved 1 072, written 63
  (9 a twin of the a4_swing arms, 18 in seed 1 off), remade 63, truncated / unmade / bodied / refused / misread 0,
  verified 35 400, differing 0 - every count equal to `obv`'s; against `obv` every client journal of every arm
  equal (166 of 178), the other 12 the Level 13 server journals of a10x, a5d_resync, a6_drop and a8_r1-r3, the
  run-to-run set. A first move still takes generation 1, so by construction the exercised records are the first
  cut's (the records themselves were not compared; the journals and every counter are equal).
- fast LBM as shipped (`ob2n`): 38 of 38 FINAL exact, rows equal to `rz1`'s, every `mismatches=` and `mism` 0,
  `refs=on/9` with the same 1 072 / 63 / 63 and every fault 0; against `obn` and against `vrn2` 166 of 178 journals
  equal each, the other 12 the same Level 13 server journals.
- the L6 re-drive as shipped (`pf_ob2s_1`): every `mismatches=` (117 in the client logs) and `mism` 0, rewinds
  72 749, `refs=on/16`, one move a client, no write, every fault 0; the 14 journals equal to `pf_vrs2_1`'s and
  to `pf_obs_1`'s. In verify (`pf_ob2v_1`): every `mismatches=` and `mism` 0, sparse verify 59 184 / 0 / 0 / 54,
  rewinds 72 749, replayed ticks 677 038, 23 934 421 positions compared 0 differing, misread 0; the 14 journals
  equal to `pf_vrv2_1`'s and to `pf_obv_1`'s. Both runs end in the harness's standing `VERDICT: FAIL - ...
  resolved no mechanism bodies (16/0)`, as every run of this re-drive does (`pf_obs_1`, `pf_obv_1` and PERF's).
- not repeated on this tree: the switch-off runs (`obo`, `pf_obf_1`), `obp` and `obq`.
- the gate (`.local-tools/work/OBJB/gate/run_gate.sh`, the final tree but the docs): x64 BUILD-OK and 857/857,
  x86 CLIENT-BUILD-OK (no C1128), the Arch server ARCH-BUILD-OK; retail at K = 20, L12 and L11 at human key rates
  (`objg`): 0 mismatches, every `mismatches=` 0, FINAL rows bit-equal to the restore skip's sessions (`r6on`) with
  L11's 32 trafo pieces. Retail writes roots: L12 `refs=on/37` moved 6, written 0 and 8; L11 `refs=on/42` moved 13,
  written 13 and 5, remade equal to written, every fault 0 (no verify there).

**Not exercised.** The review's three defect paths (a truncation, a refused root, a replay stopped early) outside
the unit tests: every run's truncated, refused and unmade are 0. A root moved while its probe's block is active, or moved back (a sector deactivation, a level
reset: memory "level-reset joint rebuild"); `truncated`, `unmade`, `bodied` and `refused` above 0 outside the
unit tests; an anchor inside the delayed link's four ticks (no run had one; SimTool's `--late-sector-check`
and `--savestate-check` were not run); the bodied check against a body the anchor had; an exact resync whose base lies before a move this
client already ran (it keeps the live pose, as before: the same gap as at HEAD); clones-off servers (a pooled
frame moves at every later sector); retail in verify; the wire check; `--compare-scripts` dumps of older runs,
which do not parse the new hold. Left out of the design: the `hold_choice` counter of sparse verify (the float
count it would split is 0 now).

## Built (the end-of-replay script diff leaves the rewinding frame, step SDF, 2026-11-13)

**What it costs today.** On the 8845HS the frames of the public L6 re-drive still over 15.15 ms at HEAD are mostly
1849 and 2089 (client 0), each over by at most 1.1 ms; 0.78-0.94 ms of each is the end-of-replay script diff
(`.local-tools/work/RSH`, `DEPTH`). At the last replayed tick of a rewind, when the re-executed scripts differ in
bytes from the live record, `rollback_world::diff_scripts` walks both records and classifies the difference
(logical, float only, or none outside the written-cell section). By design a replay that corrected an input
differs from the live record, so it runs in the same 44 of the ~518 deep rewinds of every run. Its only readers
are `script_mismatches` / `script_float_mismatches` (the status line's `scr=`) and the capped `script replay: the
scripts re-executed to tick ...` lines. No simulation reads it.

**The rule.** `BMMO_SCRIPT_DIFF = inline | deferred` (`client_core.cpp` `script_diff_deferred_from_env`, deferred
when unset, latched at each session start: `rollback_engine::set_defer_script_diffs`; the engine's own default
and SimTool's engines stay inline). Deferred, the byte compare stays on the rewinding frame, and a difference is
queued (`rollback_script_diff.cpp` `queue_script_diff`): its tick, the live blob moved (the replayed state
overwrites that record right after) and one copy of the replayed blob. A later call classifies it with the same
code as inline (`classify_scripts`, `count_script_difference`), so the counts and the log lines are the same in the
same order. Only the log lines' place in the log moves. The queue holds four. A fifth classifies the oldest on the
rewinding frame (`forced`, logged once) instead of dropping it.

- *When* (`client_core_frame.cpp` `session_client_core::end_frame`, which each host calls at its frame's end:
  the headless client after `post_step`, inside its timed frame (`session_client.cpp` `pace_and_frame`); retail after
  `pre_step` (`core_host.cpp` `core_host_frame`, whose frame is `post_step`, the mailbox, then `pre_step`)): one
  item on a frame that replayed nothing - `replay_work()` (replayed ticks + rewinds + snapshot rollbacks + resyncs)
  unchanged since the previous `end_frame`, so the message path and both steps count whatever the host's order -
  a rule with no clock read. Or forced, on a frame that replayed, once the oldest is 64 ticks old (counted forced
  and rewound).
- *Settled* (`settle_script_diffs`: everything queued classified): before the headless 5 s report and the end
  report (`session_client.cpp` `report()`: every 5 s of wall clock, between two frames, so a settle there costs
  the gap after a frame that queued one), in `end()` before `hooks.ending` and before anything of
  the session is torn down, and at a resync's apply before its trafo reconcile (`client_core_resync.cpp`, while
  trafo sets run: below).
- *Unclassified* (`session_client_core::drop_script_diffs`, counted and logged): what a new session finds still
  queued (`begin`: its binding names the previous session's selection), and what the retail fallback end finds
  (`OnPostExitLevel`'s `core_host_end_local(..., level_gone)`: an exit that did not pass `OnPreExitLevel` ends the
  session after the Exit Level chain destroyed the level's behaviours). Also counted unclassified: a classifying
  binding with no `diff_scripts`, where `classify_scripts` would count the blob logical by its sizes.
- *Counters*: ` sdiff=queued/classified/forced/unclassified/max queue/rewound` at the end of the status line, only
  while deferred. `rewound`: classifications `end_frame` made on a frame that replayed - the age-forced ones, and
  0 otherwise by construction (the rule and the count read the same stamp at the frame's last core call).

**What the classification reads** (`game/script_replay.hpp` `diff` / `diff_walk` / `diff_behavior`, through the
hosts' one binding, `rollback_binding_scripts.hpp` `world.diff_scripts`, `diff(..., cells = false)`). It reads the
two blobs, the session's selection (`sel.scripts`, `sel.satellites`) and the live CK graph. Through the graph it
gets names (`CKContext::GetObject`, `GetName`) and each changed parameter's class (`GetLocalParameter` /
`GetOutputParameter`, then `GetGUID`: float or logical). The hold section is compared byte by byte
(`compare_hold`). With `cells = false` it changes no counter (`written_cells_stats` moves only in the sparse
verify's call, which stays inline). It writes nothing to the world, the CK context or any state a tick reads.
Deferral therefore changes nothing as long as the named objects are the same:

- the selection is the session's, made at its anchor (`core_host.cpp` `core_host_anchor_finish`,
  `session_client.cpp` `anchor`) and changed only by the next session's anchor, after `begin` has dropped the
  queue;
- the level's behaviours outlive the session, and `end()` settles the queue before `hooks.ended` tears the trafo
  sets down (retail `core_host_end_local`: `end` before `trafo_teardown`; headless `end_trafo_sets` from
  `hooks.ended`). The one end that comes after the level's teardown, retail's `OnPostExitLevel` fallback, drops the
  queue instead (above);
- the only behaviours a running session destroys are trafo sets' scripts. A set deselected live:
  `retire_trafo_sets` destroys its scripts `history_ticks` (96, or 128 above the reach floor) after a deselect
  that comes at or after the queued tick, and the 64-tick age bound classifies first (a static_assert ties it to
  the default history). A set a resync's reconcile unmakes or retires (`retire_trafo_set_now` deselects it at the
  resync's base, which can be before the queued tick, so `retire_trafo_sets` can destroy it within 64 ticks of
  the queued one): the apply settles the queue before the reconcile.

If a named behaviour were gone anyway, its parameter would read as logical where inline read float (a null
parameter is not a float one). The proof below therefore compares the lines one by one, not only the counts.

**The rewind cost meter.** The inline diff ran before `timer.cost_ends()` (step B5RX), inside the replay stage
of the meter's sample. Deferred, its time leaves the sample on the rewinds that make it: about 9 % of the deep ones, 61 of client 0's
rewinds on the L6 re-drive and 767 of the 72 749 over its 13 clients (about 1 %). The replay
ms per tick reads lower, so retail's running cap (`(B - restore_ms) / replay_ms_per_tick`) reads equal or higher,
and binds only above the 48-tick floor. An overflow-forced classification stays in the meter (it runs in
`queue_script_diff`, before `cost_ends`); an age-forced one runs in `end_frame` after the rewind's sample, out of it. The headless clients run
the cost on the captured clock as before, and the K7 capture's clock reads are unchanged: the classification reads
no clock site (nor does the `world()` binding it calls: `bind_session_world`, `bind_script_replay` and the hosts'
`bind_world` read none), and the byte compare and `cost_ends` keep their order. Also lower: the rewind's own `t_rep` /
`t_max` stats, and the work LVR's heavy gate sees on those frames (about 0.8 ms). So `live=` light counts may
differ from an inline run. They decide no record a rewind restores, and the journals must not differ.

**Results** (`.local-tools/work/SDF`: `run_sdf.sh`, `sdf_sum.py`, the `sum_*`, `rows_*` and `armj_*` files; HEAD
`4cf106ae` plus this step).
- unit tests: 858/858. `DeferredScriptDiffsCountAndLogAsInline`: six differing rewinds, deferred, give inline's
  counts and lines in the same order. Two of them are forced by the four-item bound. A hook-less classifier leaves
  the other four unclassified.
- builds: x64 BUILD-OK; x86 client CLIENT-BUILD-OK (core_host.cpp has `/bigobj` since `4cf106ae`; no C1128).
- fast LBM, as shipped (`sdfd`) and inline (`sdfi`): 38/38 twins FINAL exact each, every row equal to `ob2n`'s.
  Over the 140 client logs: `scr=` logical 0 and float 105 in `ob2n`, `sdfd` and `sdfi`, the 105 lines equal line
  for line, 0 differences. `sdfd` queued 105, classified 105, forced 0, unclassified 0, max queue 1. Every
  `mismatches=` and `mism` 0. Journals: every client journal equal past the build id to `ob2n`'s and between the
  two. The 12 Level 13 server journals differ, as they do between any two runs since LVR.
- L6 re-drive, as shipped (`pf_sdfd_1`) and inline (`pf_sdfi_1`): every `mismatches=` and `mism` 0, 72 749 rewinds
  each. The 14 journals equal to `pf_ob2s_1`'s. `scr=` logical 24 and float 743 over the 13 clients in all three
  runs, the 128 logged lines equal. `sdfd` queued 767, classified 767, forced 0, unclassified 0, max queue 1
  (client 0: 61). Replayed ticks 663 877 in both (`pf_ob2s_1` 664 032: LVR's wall-clock gate; client 0 `live=`
  light 1 deferred, 0 inline).

**One review, two defects fixed and a sentence corrected** (`.local-tools/work/SDF`).
- A set a resync's reconcile retires is deselected at the resync's base (`retire_trafo_set_now`), so its scripts
  could go before the 64-tick bound classified a queued diff naming them (a null behaviour: a float difference
  counted logical, or a reused id read). The apply now settles the queue before the reconcile
  (`client_core_resync.cpp`, inside the trafo branch).
- Retail's `OnPostExitLevel` fallback ended the session after the Exit Level chain destroyed the level's
  behaviours, and `end()` classified the queue there. It now passes `level_gone` (`BallanceMMOClient.cpp`,
  `core_host_end_local`), which drops the queue unclassified, logged, before the end (`drop_script_diffs`, shared
  with `begin`).
- The meter sentence: only the overflow-forced classification stays in the meter.
- Taken from its points: the queue has out-of-line move members too (an engine move no longer copies blobs). Left:
  the headless 5 s settle is on the wall clock; `script_diff_full_logged_` is not reset per session, like the
  engine's other log-once flags. Its point that a rewind later in the same frame could follow a quiet-frame
  classification is the frame-end rule below.

Rerun after the fixes, as shipped (the fixes leave the inline path as it was): 858/858 (the SDF test also drops
four, counted unclassified); x64 BUILD-OK; x86 client CLIENT-BUILD-OK; the Arch server ARCH-BUILD-OK, the two g++
syntax checks and the step's common sources (`rollback_script_diff.cpp`, `client_core*.cpp`) ok. Fast LBM `sdf2d`:
38/38 twins FINAL exact, the rows file equal to `sdfd`'s and `ob2n`'s, every client's `mism`, `rb`, `resim`, `scr=`
and script lines equal to `sdfd`'s (140 clients: rb 1 711, resim 37 377, `scr=` 0 / 105, `sdiff=` 105/105/0/0 max 1),
every client journal equal to `sdfd`'s (the 12 Level 13 server journals differ, as between any two runs). L6
re-drive `pf_sdf2d_1`: every `mismatches=` and `mism` 0, rb 72 749, resim 663 877, `scr=` 24 / 743, the 128 lines,
`sdiff=` 767/767/0/0 max 1, all equal to `pf_sdfd_1`; the 14 journals equal to `pf_sdfd_1`'s and `pf_ob2s_1`'s.

**The 8845HS A/B** (`.local-tools/work/SDF` `run_sdp_8845.sh`, `sdp_frames.py`: the slow-frame build of this step,
client 0 pinned, three interleaved pairs inline / deferred, 13 clients on the one box, BMMO_REPLAY_DIFF=off). Frame
1849's replay part fell 11.4 -> 9.1 ms on average, but frame 1850, which rewinds 17 ticks in every run, rose in all
three pairs (replay part 7.9 -> 9.7 ms), so 1849 + 1850 stayed about 28.5 ms in both arms; 1849 and 2089 each still
went over budget in one deferred run. Inconclusive: inline alone spans p99 10.6-11.3 ms and 541-1 152 frames over
10 ms between its runs, every deferred run falls inside that spread, the other 12 clients show the same p99 gap,
and inline always ran first in a pair (drift, not the lever). The logs carry no classification line or status
line between 1849 and 1850, so they could not say where 1849's diff was classified.

**The frame-end rule** (`.local-tools/work/SDQ`; its review point: the old rule, in `pre_step` after
`replay_corrected_inputs`, checked "no replayed tick since the previous frame's call", which by its form does not
exclude a rewind later in the same frame). Traced: after that call nothing in a frame reaches a rewind outside the
test tools. `post_step` calls none of `rewind_world`'s callers (the lifecycle, own-echo, trafo, sector, input and
resync paths all run in the message path or in `pre_step` before the call); `apply_held_lifecycle` applies events
stamped for the frame's own tick, which `rewind_for_lifecycle` does not replay (not late); the `after_rewinds`
hook rewinds only under the headless `--force-rollback` options; the message path runs before `pre_step` on retail
and between frames on the headless client, so before the next call. Measured first, with the old call site and a
frame-end check (`end_frame` counting a
classification whose frame then replayed, plus diagnostic lines, never committed) on the local L6 re-drive
(`pf_sdq0_1`): 767 classifications over the 13 clients, 0 on a frame that replayed after the call, 0 forced, and
every classifying frame replayed nothing (work_after_call 0 in all 767). Client 0's diff queued at 1849 (tick 1848)
was classified at 1851, the one queued at 2089 at 2093; frame 1850 replays 17 ticks locally too (in both arms) and
so classified nothing. The "landed on 1850" reading is refuted, on this machine by count and on any schedule by the
trace: the 8845's 1850 replays 17 ticks before the call and could not classify, so the frame-end rule does not move
it there, and its rise in the A/B stays unexplained (noise is the reading the spread allows). The rule is still
made exact by construction, a hardening: `end_frame` is the frame's last core call on each host and its stamp covers
the whole frame (above, *When*), and it also counts a restore that replays no tick. With it (`pf_sdqd_1`, the same diagnostics) every one of the 767
classifications lands on the same (client, tick) as under the old rule; they run 1 to 7 frames after the queuing
frame (537 two ticks after the queued tick, 163 three, 24 four, 22 five, 1 seven, 20 eight).

Timing on the dev machine (the diagnostic `sdq_frame:` lines: `std::chrono::steady_clock` around the headless
client's `frame()` + `end_frame`, the same span as `frame_time:`; 13 clients on one box, one run, so indicative
only): the 767 classifying frames median 2.63 ms, p99 3.73, max 4.36, none with replay work; the 767 queuing
frames median 5.74, p99 10.22, max 11.31. Client 0's whole run: p50 2.2, p99 7.1, max 10.7 ms. Client 0 around the
named frames, deferred / inline: 1849 8.12 / 9.53, 1850 8.08 / 7.43, 1851 (classifies) 2.84 / 2.73, 2089 8.74 /
8.96, 2093 (classifies) 2.06 / 2.25 ms.

Rerun as shipped (the frame-end rule, no diagnostics): 858/858; x64 BUILD-OK; x86 client CLIENT-BUILD-OK. Fast LBM
`sdq1`: 38/38 twins FINAL exact, the rows equal to `sdf2d`'s; 140 clients' `mism` 0, rb 1 711, resim 37 377, every
`mismatches=` 0, `scr=` 0 / 105 and the 105 lines equal to `sdf2d`'s, `sdiff=` 105/105/0/0 max 1 rewound 0; every
client journal equal to `sdf2d`'s (the 12 Level 13 server journals differ, as between any two runs). L6 re-drive
`pf_sdq1_1`: 13 clients' `mism` 0, rb 72 749, resim 663 877, every `mismatches=` 0, `scr=` 24 / 743 and the 128
lines equal to `pf_sdf2d_1`'s, `sdiff=` 767/767/0/0 max 1 rewound 0 (client 0: 61/61/0/0/1/0); the 14 journals
equal to `pf_sdf2d_1`'s and `pf_ob2s_1`'s. Inline once (`pf_sdqi_1`): the same counters and lines, the 14 journals
equal to `pf_ob2s_1`'s, `pf_sdfi_1`'s status counters. "Inline reproduces HEAD" holds for the journals and the
script lines: resim and `live=` differ from HEAD's `pf_ob2s_1` (663 877 vs 664 032 replayed ticks) through LVR's
wall-clock gate, `ob2s` being a slower run (client 0 p50 3.2 ms vs 2.2, `live=` eligible 36-95 per client vs
0-3); a counter-level comparison with HEAD takes BMMO_LIVE_RECORD=whole or BMMO_LIVE_RECORD_MS=0.

**Its review: no defects.** Taken from its points: the HEAD wording above; retail's `core_host_end_local` settles
the queue first (when the level is not gone, the core not idle), before it hands back the host's script edits, so
the classification reads the behaviours as the session left them by construction (the edits it hands back were
sources, constants and data arrays, never a selected block's locals or outputs, so the order was harmless before);
`replay_work()` (resim ticks, rewinds, rollbacks, resyncs) leaves out `frozen` and `savestates_restored`, which
retail's own restore sums count: a frozen restore returns before `++rollbacks`, so a frame that only restored a
frozen hold reads as quiet, and both are absent in snapshot-free sessions. Left: `rewound` stays (it equals the
age-forced subset of `forced` by construction, documented as such); no unit test drives `end_frame` (few tests).

The gate on the final tree (`.local-tools/work/SDQ/gate` `run_gate.sh`, `gate.log`; only `core_host.cpp` and this
document changed after `sdq1` / `pf_sdq1_1`, so the fast LBM and the L6 re-drive were not rerun): 858/858; x64
BUILD-OK; x86 client CLIENT-BUILD-OK; the Arch server ARCH-BUILD-OK. Retail at K = 20, L12 and L11 at human key
rates (`sdqg_l12_h_150_1`, `sdqg_ctl_h_150_1`), as shipped: 0 mismatches (every `mismatches=` 0, 22 and 18 lines),
FINAL rows bit-equal to the `r6on` sessions (L12 2 ticks, 2 balls, 14 bodies; L11 6 ticks, 2 balls, 23 bodies,
32 trafo pieces), `sdiff=` on the end's status line 3/3/0/0/1/0 and 5/5/0/0/1/0 on L12, 8/8/0/0/1/0 and
11/11/0/0/1/0 on L11 (queued = classified, forced, unclassified and rewound 0), `scr=` present, no drop logged.

**Not exercised.** The 8845HS timing of frames 1849 and 2089 under the frame-end rule (the lever's measure); the
dev machine's runs show where the classification lands, not the 8845's cost. On retail (run in the gate above,
`end_frame` and the end's settle): its `session` status, which the console window's thread can run, does not settle
the queue, so the line it prints may leave up to the queue's items unclassified - `sdiff=` shows them - and the
game thread classifies them a frame or two later (not tried); the post-exit drop. The age-forced and
the overflow-forced paths, `rewound` and `unclassified` above 0, outside the unit test: every run's forced,
rewound and unclassified are 0 and its queue never held more than one.
A resync's settle with a diff queued, and a trafo set retired while one names it. The wire check.

## Built (production diagnostics that nothing reads leave the frame, step DCL, 2026-11-13)

**What it costs today.** The audit of `4cf106ae` (workflow `wf_6063dcb7-3fd`, read only) listed work the shipped
builds do on every frame or every rewind whose result no simulation, decision or wire byte reads. The largest is the
legacy corrector's pose histories, measured at 90-114 us of every frame on retail E-cores (S0 census, row `corr`,
`.local-tools/work/S0/report_s1.txt`). The rest are strings built before a cap or a switch is checked, and copies
kept for a diagnostic that is off. This step cuts the ones that lose nothing. Every decision, the K7 recorder, the
TICK hash, the FINAL verdict and the status line's counters stay as they were (the exceptions are named below).

**The cuts.**
1. *The legacy ladder's pose histories* (`client_core_frame.cpp` `post_step`). The own ball's, each tracked
   mechanism's and each mirror's `body_corrector::record` (with a `body_state` read per mechanism and mirror) and
   the blend loop run only with `config.rollback` off. Their one reader is the compare in `on_snapshot`
   (`client_core_messages.cpp`), which runs only on the rollback-off ladder; `blending()` is set only by that
   compare, and `status_text` reads only the stats it writes (`remote_corr=`, `mech_max_err=`, `corrections:`,
   0 with rollback on either way). `config.rollback` is fixed for a session (headless `--no-rollback`; retail
   `session rollback on|off`, latched at the session's start).
2. *The restore's record check* (`physics_savestate_restore.cpp` `restorer::validate` and the teardown check):
   the `who` name of every object and friction system is a lambda called only by a failing check. Same text.
3. *The reuse path's counter save* (`script_replay.hpp`): a root's copy attempt kept the whole `slot_memo_stats`
   (about 270 bytes and its `notes` vector) aside to take its counts back on the fallback. It now keeps only the
   eight counters `restore_memo_counts` writes (`memo_counts`). The save has to come before the attempt, so it
   stays on every attempt; it is a 64-byte copy now. Counters and lines are the same.
4. *The live scripts of a script-replayed rewind* (`rollback.hpp` `restore_and_replay`). The engine copied the
   live record's script blob at the restore, for the end-of-replay compare. The compare now reads the record
   itself at the last replayed tick (`recorded`, which is the live record of `current_tick` until the replayed
   state replaces it a few lines below) and moves its blob into SDF's queue when they differ. Nothing writes a
   record's scripts between the restore and that point (the writers are `capture` into a new state and the
   anchor's resync blob). Predicting "recorded whole" at the restore was not used: `capture_replayed` decides it
   after the capture (a body-set change, a reappearing name, verify).
5. *`BMMO_REPLAY_DIFF`* (`session_client.cpp`, `rollback_world::diff_replays`): the headless end-of-replay
   savestate text diff runs only with `BMMO_REPLAY_DIFF=on` (it ran whenever the switch was not `off` before).
   Unset, the `diff_savestate` binding stays for sparse capture's verify (the semantic diff, as before) and
   `diff_replays` is false: a rewind neither diffs nor copies the live record's savestate (the copy was made on
   every rewind while fewer than eight were logged). `BMMO_REPLAY_DIFF=off` drops the binding as before (the
   verify then compares the blobs byte for byte, as every L6 run since PERF did). The end-of-replay diff's only
   outputs are the `savestate replay:` lines and the `outside=` section (`savestate_replay_outside_controllers`).
6. *Lines past a log cap.* `rollback_world::log_open` (optional; the binding's `log_open`): false once the host's
   `log` drops what it is given. The retail host's is `line_logs < 200 || BMMO_TRACE_PLAN`; the headless hosts set
   none (they log every line). The per-rewind `rollback to tick ... re-simulated` line is built only while it is
   open, and the input rewind's reason (`client_core_frame.cpp`, read only by the engine's log lines) is built
   only while `session_hooks::engine_log_open` says so (retail: the same predicate). Past the cap the count stays
   at 200 instead of growing; the write-failure lines' own cap (16, on the same count) is past either way. The
   retail mirror-birth log (`core_host.cpp` `log_mirror_births`) checks its per-birth cap before the two history
   lookups. Text identical while under the caps.
7. *The bridge event log's diagnostic entries* (`physics_state.cpp`, bridge v39 `event_log_diagnostics`,
   `physics_state.hpp` `set_event_log_diagnostics`): a body's birth state (`birth`, with its surface hash),
   `revived`, `frozen`, `force` and `constraint` are recorded only while on. On by default (SimTool and the
   client's automation print the whole log). The headless client sets it to `--trace` at boot; the retail host
   to the session's `config.trace` at the anchor and back on at its end and at the next session's start (an end
   that was skipped). The client hosts read none of these
   entries unless tracing (`drain_body_events` takes `recipe`, `created`, `deleted`, `script_wakeup`; retail logs
   `birth`, `force`, `constraint` under trace). The server's world drains the log only under `--trace`, so its
   listener fills to its 256 KB bound once and records nothing more: left as it is.
8. *Server strings* (`session_runner.cpp`): `observe_lateness`'s `[session N] S1` prefix (every tick) is built only
   for a line; `observe_quorum`'s arrival trace (every tick) only under `--arrival-trace`. `capture_portable`
   (`script_replay.hpp`, the server's resync and join states, called without a report) built every report note
   and every script's block path to drop them: `note` takes the text as a lambda, called only when the lists are
   kept, and the root's path is built only then.
9. *The retail Physicalize recipe* (`core_host.cpp` `core_host_on_physicalize`): built from the block's arguments
   for every Physicalize of the session and read only by the own ball's birth report; it is now built there,
   into the report.

**Counters that move.** None of the status line's counters changes for these cuts except through item 5. With the
diff off, the fast LBM's clients no longer spend 10-20 ms on each of the first eight savestate diffs, so LVR's
wall-clock gate sees fewer heavy frames: `live=` light 67 in `sdq1`, 7 as shipped (`dcl1`), 80 with
`BMMO_REPLAY_DIFF=on` (`dcl1on`; 71 in `sdf2d`, two runs of one tree differ that much). `ccache=` hits follow
`live=` (a light live record serves no cache). The `savestate replay:` lines (105 in the fast LBM) and the
`outside=` section (0 in every run here) are gone unless `BMMO_REPLAY_DIFF=on`; `savestate_replay_mismatches` and
`savestate_replay_outside_controllers` only ever counted the removed work. Retail's per-rewind lines past its cap
are no longer built, and its `line_logs` stops at 200. The headless client's end-of-session `lookup_cache:` line
counts the removed `body_state` reads no more (L6 client 0: hits 2 971 703 -> 2 796 500, fills equal). Scripts:
`.local-tools/work/S3/ls3_cmp.py` and `SPX/deferral/judge.sh` count `savestate replay:` lines and need
`BMMO_REPLAY_DIFF=on` to count what they did; no script in `scripts/` sets the switch or reads those lines.

**Results** (`.local-tools/work/DCL`: `run_base.sh`, `run_dcl1.sh`, `run_l6.sh`, `run_norb.sh`, `run_gate.sh`,
`cmp_lbm.sh`, `status_all.py`, `subfields.py`, `frames.py`; HEAD `29adc904` plus this step).
- unit tests: 859/859. New: `AClosedLogTakesNoRewindLine` (a closed `log_open` takes the per-rewind line, the
  replay is the same). `OneCycleMakesTheTablesCallsInOrder` no longer expects the mirror's `body_state` read.
- builds: x64 BUILD-OK; x86 client CLIENT-BUILD-OK; the Arch server ARCH-BUILD-OK.
- fast LBM as shipped (`dcl1`) and with `BMMO_REPLAY_DIFF=on` (`dcl1on`): 38/38 twins FINAL exact each, the rows
  equal to `sdq1`'s. Over the 140 clients: `mism` 0, rb 1 711, resim 37 377, `scr=` 0 / 105 with the 105 lines
  equal, `sdiff=` 105/105/0/0 max 1, every `mismatches=` 0, all equal to `sdq1`. The fields that differ are the
  wall-clock ones (`t_cap`, `t_rep`, `t_rest`, `t_max`, the ms of `whole`, `light` and `lifetime`) and `live=` /
  `ccache=` (above), as between `sdq1` and `sdf2d`. `dcl1on`'s 105 `savestate replay:` lines equal `sdq1`'s. Every
  client journal equal past the build id to `sdq1`'s; the 12 Level 13 server journals differ, as between any two
  runs since LVR.
- L6 re-drive as shipped (`pf_dcl1_1`) and inline (`pf_dcli_1`): every `mismatches=` and `mism` 0, rb 72 749,
  resim 663 877, `scr=` 24 / 743 and the 128 lines, `sdiff=` 767/767/0/0 max 1 (deferred), all equal to
  `pf_sdq1_1`; the 14 journals equal to `pf_sdq1_1`'s. With `BMMO_LIVE_RECORD_MS=0` (`pf_dclz_1` against HEAD's
  `pf_dclhz_1`, run today: no MS=0 run of HEAD as shipped existed): every counter equal (resim 677 038), only the
  wall-clock fields differ; journals equal to `pf_sdq1_1`'s.
- End-of-session report lines (`slot_memo:`, `script_reuse:`, `membership:`, links, the union's `lookups:` and
  `refs=`): with `BMMO_LIVE_RECORD_MS=0` every line equal client by client but `lookup_cache:` (hits only); as
  shipped `slot_memo:` / `script_reuse:` also differ where `live=` does, as between two runs of HEAD.
- Sparse verify (item 5's other reader): OBJB's verify recipe on a0_first, a4_swing_seed0 and a6m. The first cut
  dropped the binding when the switch was unset (`dclv`: the byte compare, today's `BMMO_REPLAY_DIFF=off` path);
  with `BMMO_REPLAY_DIFF=on` (`dclvon`) and on the final tree with the switch unset (`dclfv`: the semantic diff, no
  end-of-replay diff): 6/6 twins FINAL exact each, 95 anchors compared, 0 differing in all three, every
  `mismatches=` 0, `refs=` differing 0; `dclfv`'s `sv=` and `verify=` fields equal `dclv`'s and `dclvon`'s client
  by client, and it logs no `savestate replay:` line (`dclvon` 180).
- Server (item 8): the 2 557 `S1` lines of the 38 arms' server logs (lateness summaries, raise / lower, quorum,
  patience, reset, lead-ack) equal to `sdq1`'s in count and shape, the numbers aside (two runs of HEAD differ in
  the numbers too); no run had `--arrival-trace` on.
- g++ (WSL Arch): `linux_sim_syntax.sh` and `linux_test_syntax.sh` ok, `client_core_frame.cpp` and
  `client_core.cpp` ok with the headless client's flags (the physics sources cannot be checked with them: no
  `bmmo_build_id.h`, IVP's protected members).
- Rollback off (`--no-rollback`, A0, `dcl_norb` and the final tree's `dclf_norb` against HEAD's `dclh_norb`):
  every status field equal, the corrector's compares matched (`unmatched=0`; client 3 compared 3, hard 2,
  mechanisms blended 3, hard 15), the 33 correction and 162 resync lines equal client by client.
- Frame time on the dev machine, client 0 of the L6 re-drive (13 clients on one box, 0.1 ms buckets), p50 / p99 /
  max: `pf_sdq1_1` 2.3 / 7.5 / 11.2 ms, HEAD today `pf_dclh_1` 2.4 / 7.6 / 11.4, `pf_dcl1_1` 2.3 / 7.4 / 11.4.
  Within the run-to-run spread: these runs already had `BMMO_REPLAY_DIFF=off`, and the histories' cost on a
  P-core is below a bucket.
- The review (no defect) and its points taken: the verify keeps the semantic diff when the switch is unset
  (`diff_replays`, item 5); the retail host turns the event log's diagnostics back on at `core_host_start` too, for
  a previous session whose end was skipped (item 7); the three legacy record blocks sit in one `if
  (!config.rollback)` (item 1). Reruns on the final tree (`.local-tools/work/DCL/fix/run_fix.sh`, `ssr_lines.py`,
  `live_sum.py`): fast LBM as shipped (`dclf1`) 38/38 twins FINAL exact, rows equal to `sdq1`'s, over the 140
  clients `mism` 0, rb 1 711, resim 37 377, `scr=` 0 / 105 with the 105 lines equal, `sdiff=` 105/105/0/0 max 1,
  every `mismatches=` 0, 0 `savestate replay:` lines, the status fields that differ from `dcl1`'s only the
  wall-clock ones and `live=` / `ccache=` (`live=` light 6, `dcl1` 7), every client journal equal past the build
  id to `sdq1`'s (the 12 Level 13 server journals differ, as before); with `BMMO_REPLAY_DIFF=on` (`dclf1on`)
  38/38 FINAL exact, the same counters, its 105 `savestate replay:` lines equal to `sdq1`'s client log by client
  log (`live=` light 87); the L6 re-drive as shipped (`pf_dclf1_1`): every `mismatches=` and `mism` 0, rb 72 749,
  resim 663 877, `scr=` 24 / 743 and the 128 lines, `sdiff=` 767/767/0/0 max 1, all equal to `pf_sdq1_1`, the 14
  journals equal to `pf_sdq1_1`'s past the build id, the status fields that differ from `pf_dcl1_1`'s only the
  wall-clock ones and `live=`.
- The gate on the final tree (`.local-tools/work/DCL/gate/run_gate.sh`, `gate.log`; the product files' hashes
  equal before and after it and to the reruns'): x64 BUILD-OK, 859/859, x86 CLIENT-BUILD-OK, the Arch server
  ARCH-BUILD-OK at the x86 build's id (the server logs' id matches). Retail at K = 20, L12 and L11 at human key
  rates (`dclg_l12_h_150_1`, `dclg_ctl_h_150_1` against `r6on`): 0 mismatches (22 and 18 `mismatches=0` lines),
  FINAL rows bit-equal (L12 ball 2/2, body 14/14 on both clients; L11 ball 2/2, body 23/23, piece 32/32 on both),
  `refs=` every fault 0 (L12 37 roots, 8 writes; L11 42 roots, 13 + 5 writes), `sdiff=` queued equal to classified
  on every client's last status line (2/2, 4/4, 9/9, 10/10; forced, unclassified, rewound 0, max 1). The per-rewind
  lines stayed under the cap (22, 57, 62, 92 per client). The builder's first gate run is kept as
  `scripts/retail/dclb_*` and `DCL/gate_builder`.

Bridge v39: the x86 client and its physics_RT bridge ship together (the client asks for exactly v39).

**Not exercised:** the retail census row `corr` (not attempted; with rollback on the block no longer runs); retail
past its 200-line cap, so the core's empty-reason branch ran nowhere (the engine half: the unit test); the K7 clock
sites (unchanged by inspection: no clock read added, moved or removed; no capture replayed); the event log's diagnostics switched on retail under trace; a failing
record check's text (no test makes one); the 8845HS timing; the wire check. Added by the review's fixes: the
`core_host_start` reset after a session whose end was skipped (no run skips an end); a unit test of
`diff_replays` false with the binding present (the fast LBM `dclf1` runs that case: 0 lines, every counter equal).

## Built (the server log keeps what an operator reads, step SLQ, 2026-11-13)

**Why.** The user, 2026-11-13: the server prints far too much; it needs the normal server log (`origin/main`'s, the
shadow-ball server) plus the lines a player or a server operator understands - a room joined or left, a session
started or ended - and the physics and script systems' internal state only in a verbose mode. On the public test
the server logs held 85 668 lines (16.1 MB) over 458 wall-minutes, 187 a minute, in bursts of 80-520 a second at
each world boot. Design: `.local-tools/work/SDF/srvlog_design.txt` (read only, at `4cf106ae`), followed as written
except where named below.

**The level.** One threshold, process-wide, in `DebugOutput` (`misc.cpp`): `bmmo::set_log_threshold(log_type)`, an
atomic, `everything` by default, so the client, SimTool, the headless client and the tests write every line as
before; a `bug` line always passes. The server sets `msg`, or `verbose` when verbose is on (`warning` ranks below
`important`, so the threshold is never `important`). A new `Printf(log_type, fmt, args...)` skips the formatting of
a dropped line; being a function, its arguments are always evaluated - two server lines read a recorded clock site
in their arguments (`server_boot_log`, `server_event_relay_delay`), and no call site guards a `Printf` on the
level, so a capture replays with the same clock reads at either level. `logging_level` stays the network
library's own (at most `msg`, so its lines pass).

**What the default log keeps.** Every line `main` prints, word for word (checked mechanically:
`.local-tools/work/SLQ/main_lines.py`, the 85 format strings of `origin/main`'s `server.cpp`: 82 are in the tree,
none tagged; the 3 others are two lines commented out on `main` and the public notification, which this branch
already prints with a `[Muted] ` prefix). The start-up lines (`Physics sessions enabled (...)`, the custom maps, the
unavailable / bad-config lines), the coordinator and capture lines of the test builds. Room lines: `created room`
(read by `trial_start.py`), a map set or cleared, the restart vote, and new: `P (#C) joined room N.`,
`P (#C) left room N.`, `Room N: P is the host now.`, `Room N: A kicked B (#C).`, `Room N closed by P (#C).` (was
`closed by #C`). Session lines: `Room R started physics session N on level L with K players: A, B, C.` (was
`(world booting)`), new `Physics session N is running (level L loaded in 6.2 s, K players).` at the first snapshot,
`Physics session N: P joined.` / `P joined late.` (the life and join order are the verbose `SessionMember` line's;
`joined late` stays for `late_other_level.py`), new `Physics session N: P left (<why>; X resyncs).` (`left the
room`, `disconnected`, `kicked from the room`, `finished the level`, `did not report ready` (the barrier's own line
is a count only); none where the caller's own default line says who and why: the refusals, `sent no input`, a
member the world could not add), every refusal and wait, the build mismatch, the mod denial, a released join
order, the `sessions` command's both lines (the runner's `describe` prints through the console's own callback, not
the verbose `[Sim]` sink), and
`Physics session N (room R, level L) ended after 12m34s: <reason> (K players, X resyncs, Y events flagged, Z rejected).`
(was `(room R) ended: <reason>`). The length is the ticks the session ran (the first to the last tick the runner
relayed) over `TICK_RATE`, and the load time the world-ready instant the runner reports: no clock is read for
either. Resyncs are counted where a member asks (`handle_session_resync`) and where the start handshake resyncs
one; the base-missed re-answer is the same resync, not counted again. `K players` is the lives the session gave
(late joiners included). Internal failures print by default at `error`: the server's `ERROR join order`, a member
that could not be added to the world, a malformed Physicalize, a resync state cut on the wire (the client will
refuse it; the design had it verbose, the review did not); the runner's journal directory / open / writer
failures, world boot failed, `add_player failed` (both), an event that failed, a tick that failed, a trafo life
mismatch and the proximity union's candidate set changed between two steps (both guards, capped and noted as
before; not in the design's list); the world's checkpoint activation NOT held, the depth test NOT pinned, the
checkpoint chain's phase cell and sector cell NOT split, the Level 1 tutorial's exit NOT found and its frame-9
freeze NOT held (each says this world acts apart from the clients'), the airship zone and spawn ghost setup errors,
the auto clone failures, `ERROR join order`, a failed spawn impulse, the tutorial exit not activated, the proximity
hold not restored, the trafo quiesce line when it says NOT EXPECTED (split from the expected one, same text), a
trafo input with no set held, the trafo hold FIRED, the leaks. These were printed in production before the step
at the same rate; the ones with no cap (no set held, the leaks, the hold not restored, an event that failed) keep
none: a cap would also cut SimTool's stderr, which the world shares.

**What moves to verbose, word for word.** 44 of the server's own lines (`Printf(kVerbose, ...)`): the room's world
option lines, `SessionMember`, boot phases, `world ready`, `ticking (first snapshot`, S2 and its cap, resync state /
base re-assigned / `resynced at` / `state follows`, Trafo resent, the SessionLead lines, telemetry, the
start handshake (but its refusal), `base was due` / `moved`, `ready (assigned tick`, the camera base, BodyRevived,
the rate limit, suspicious events, REJECTED Trafo (as the design has it: its count is the end line's `rejected`),
S5. Every runner and world line not in the error list: the
server's `callbacks.log` prints `[Sim] %s` at `verbose`, the new `callbacks.warn` at `error`.
`session_callbacks::warn`, `physics_world::options::warn` and the members `session_runner::warn` /
`physics_world::warn` fall back to `log` when unset, so SimTool, `trafo_synth.hpp` and the tests (which set only
`log`) print what they did, byte for byte; the runner's world lambda passes `warn` with the same `[session N]`
prefix. No line is classified by its text. The game layer and the engine are unchanged (their failures come back
as error strings `physics_world` logs). Lines that bypass the logger stay (the engine's spawn-impulse stdout line,
`physics_RT stdout redirected`, `[crash]`).

**The switches.** `log_verbose: false` (top level, next to `log_level_restarts`; `yaml_load_value` writes it back,
so a `config.yml` gains the key on the first start), `--verbose` (kept across a `reload`), the console's `verbose
on` / `verbose off` (`verbose` alone says which; until the next `reload`, which applies the config again), and
`physics.debug_trace`, `physics.arrival_trace` and the runner's debug variables `BMMO_SESSION_STALL`,
`BMMO_RUNNER_JITTER`, `BMMO_TRACE_LIVE_ORDER` turn it on (their lines are verbose). The level is set in
`load_config`, before the runner is built. The log says `Verbose logging on.` when it turns on (at the start or
later) and `Verbose logging off.` when it turns off. The journals do not go through the log: every note is its own
`journal.note` call, so a line built once for both keeps its note at either level.

**Consumers.** `scripts/arena_map_test.py` writes `log_verbose: true` (this covers `lbm_fast.sh`, the L6
re-drives, `late_player_harness.py`'s server) and fails a case whose server log's last level marker is not
`Verbose logging on.` (an external server's sits before the case, a console `off` after it; no log at all is a
note; a log with no marker that holds the runner's `[Sim] [session N] ... booting level` line passes: a server
built before this step writes every line), in `--report-only` too; `--quiet-server-log` writes `false` and skips
the check. The problem is also the case summary's `server_log_level`, and `scripts/lbm_fast_judge.py` does not
count such a twin FINAL exact (it prints `FAIL (not counted exact)`): the judge reads no VERDICT, and a server log
that is not verbose leaves the server-read rows empty on both twins, equal, so silent. `scripts/probe.py`, `retail_two_client_test.py`
write the key; `capture_replay.py` sets it in the replay's copy of the config (a capture made before the key has
none). `.local-tools/arch_server_lp.sh` (and through it `arch_server_snap.sh`, `ab_template.sh`, the retail
recipes' `work/RT1/rt1_retail.py`, which `S0`/`SRT` reuse) sets or appends it in the `config.yml` it edits, as do the live starters `work/H4`, `K7`,
`LBM7`, `LBM8`, `OCK`, `RLD`, `a4`. `telemetry_report.py` and `ghost_state.py` say in their headers that they read
verbose lines, as do `work/PERF/pt_server_resync.py` and `make_report_data.py`; `work/SRV/srvload.py`'s and the
public-test analyzer `work/ARCH/pt/server.py`'s end-line patterns take the new wording (the analyzer reads many
verbose lines too: whether public tests run verbose is the user's open question); `work/NPLAYERS/four_rooms.sh` /
`two_rooms.sh` wait for `ticking (first snapshot` or `is running (level`. Outside the repo: the 8845
re-drive package (`~/bmmo-kept-pkg`) must be rebuilt from these scripts; the production `~/bmmo-server/config.yml`
gets `log_verbose: false` on its next start. CI checks only `--dry-run`'s exit.

**Results** (`.local-tools/work/SLQ`: `run_slq.sh`, `run_pt_quiet.sh`, `cmp_lbm.sh`, `log_kinds.py`,
`l6_journals.py`, `main_lines.py`, `console_toggle.py`, `run_capture.sh`; HEAD `16412e11` plus this step).
- Builds: x64 BUILD-OK, 859/859 tests; x86 client CLIENT-BUILD-OK; the Arch server ARCH-BUILD-OK at the x86
  build's id. `--dry-run` exits 0 and writes `log_verbose: false` into a `config.yml` without it; with
  `--verbose` it prints the marker.
- Fast LBM as shipped (`slq1`): 38/38 twins FINAL exact, the rows equal to `dclf1`'s; over the 140 clients `mism`
  0, rb 1 711, resim 37 377, `scr=` 0 / 105, `sdiff=` 105/105/0/0 max 1, every `mismatches=` 0, the status fields
  that differ only the wall-clock ones and LVR's (`t_cap`, `t_rep`, `t_max`, `t_rest`, `whole`, `light`,
  `lifetime`, `live`, `ccache`); every client journal equal past the build id to `dclf1`'s, the 12 Level 13
  server journals that differ between any two runs differ (the same 12 as `sdq1` / `dclf1`). All 38 server logs
  end verbose (the marker). Their lines by kind (stamps, numbers, build id, label masked) equal `dclf1`'s but for
  the reworded and new lines: 13 034 -> 13 586 lines.
- The L6 re-drive as shipped (`pf_slq1_1`): every `mismatches=` and `mism` 0, rb 72 749, resim 663 877, `sdiff=`
  as `pf_dclf1_1`, the 14 journals equal to `pf_dclf1_1`'s past the build id; its server log 2 225 -> 2 239 lines.
- The default level: the same re-drive with `--quiet-server-log` (`pf_slqq_1`): the same counters, the 14
  journals equal to `pf_dclf1_1`'s, the server log 51 lines (3.6 kB) against 2 225 (571 kB): the start-up, 13
  connection requests and logins, `Ticking started.`, the room created and 12 joins, the session start with its
  13 players and its running line (the recipe stops the run before anyone leaves). Two fast-LBM arms at the
  default level (`slqq`: `a5d_resync`, `a8_r1`): 4/4 FINAL exact, the client journals equal to `slq1`'s, 45 and 46
  lines against 347 and 342 (a late joiner, a resync counted in the leaver's line and the end line, every
  disconnect, the host changes, the end). One `a8_r1_on` criterion reads the server's verbose lines and is n/a
  there, as `--quiet-server-log` says.
- The console: a real-time A0 session against a server started at the default level (`console_toggle.py`,
  `slqc2`): `verbose` said off; `verbose on` printed the marker and the `[Sim]`, S5 and telemetry lines began (0
  `[Sim]` lines before, 61 while on, 0 after); `verbose` said on, a second `verbose on` said already on; `verbose
  off` printed `Verbose logging off.` and they stopped; `verbose maybe` printed the usage. The run's finish path
  printed `left (finished the level; 0 resyncs)` and `ended after 0m33s`; its load time read 0.3 s (a coordinated
  run's boot takes no virtual time: 0.0 s).
- A capture at both levels (`run_capture.sh`, `slqcap1`, a coordinated A0 arm with `--capture`, the server log
  verbose): the server replayed by `capture_replay.py` (verbose) and by hand with `log_verbose: false` gave the
  live journal back byte for byte both times (1 160 597 bytes). The verbose replay's log equals the live one but
  for the port, the replay's own lines, and the values a replay of a coordinated capture already printed apart
  (the S1 quorum's `seen-at`, the boot time: `world ready`, an unchanged line, reads 23 872 s there, so does the new load time); the quiet replay's
  lines are the live log's but for the same.
- The review's fixes (`.local-tools/work/SLQ/fix`: `run_slqf.sh`, `console_default.py`): one review, its defects
  fixed - the `sessions` command's second line had become verbose (now through `describe`'s own output
  callback); five world lines, the union guard and the cut resync state had become verbose-only (now `error`); the
  marker check did not reach the fast-LBM judge and failed every server built before the step (both fixed, above);
  the public-test analyzer missed the new end line; the members dropped for not reporting ready went unnamed; two
  `NPLAYERS` waits and two `PERF` headers. Its points: the refused add's third line dropped, `--report-only`
  checks the level too; the uncapped error lines and REJECTED Trafo left as they are (above). Fast LBM as shipped
  (`slqf1`): 38/38 twins FINAL exact, the rows equal to `dclf1`'s (and so `slq1`'s); 140 clients, `mism` 0, rb
  1 711, resim 37 377, every `mismatches=` 0, `scr=` 0 / 105, `sdiff=` 105/105/0/0 max 1, the differing status
  fields only the 9 wall-clock / live-record ones; every client journal equal to `slq1`'s past the build id (the
  same 12 Level 13 server journals differ); all 38 server logs' last marker `on`; the line kinds equal `slq1`'s
  (13 034 -> 13 586 against `dclf1`); every summary's `server_log_level` empty. Two arms at the default level
  (`slqfq`): 4/4 FINAL exact, client journals equal to `slqq`'s, logs of 45, 45, 46, 46 lines as `slqq`'s. A live
  refusal (`console_default.py`, `slqf2`): a real-time A0 session against a server at the default level, the
  harness without `--quiet-server-log`: its VERDICT `FAIL - the server log is not verbose ...`, the summary's
  `server_log_level` set, and `lbm_fast_judge.py slqf2` printed `FINAL EXACT` with `FAIL (not counted exact)`,
  `0 of 1 twins FINAL exact`, exit 1; the server log 59 lines, `sessions` typed twice printed both lines each time
  (the only 2 `[Sim]` lines), `verbose` said off, no marker. The saved logs: a pre-step server's (`dclf1_a0_first`,
  `pf_dclf1_1`) passes the check, `pf_slqq_1`, `slqq_a8_r1_on` and `slqc2` fail it, `slq1` / `pf_slq1_1` pass. The
  analyzer: the 8 public-test logs give the same 30 sessions and end reasons as before the fix (29 by their
  line), the 38 `slq1` logs 38 of 38 ended by the line (0 with the old pattern).
- Gate on the final tree (`.local-tools/work/SLQ/gate/run_gate.sh`): x64 BUILD-OK, 859/859 tests; x86 client
  CLIENT-BUILD-OK and the Arch server ARCH-BUILD-OK at `ballanced-690d1c23beac+bmmo-16412e11b2da-dirty` (the
  retail servers' id matches); retail at K = 20 at human key rates, `slqg_l12_h_150_1` (L12) and
  `slqg_ctl_h_150_1` (L11), against `r6on_*`: 0 mismatches (22 and 18 `mismatches=0` lines), FINAL rows
  bit-equal (L12 ball 2/2, body 14/14 at 2 common ticks; L11 ball 2/2, body 23/23, piece 32/32 at 6), `refs=`
  every fault 0 (L12 37 roots, 13 writes; L11 42 roots, 18 writes), `sdiff=` queued equal to classified on every
  client; both retail server logs' last marker `on` (765 and 542 lines, 700 and 441 of them `[Sim]`): the Arch
  starter's set-or-append ran for real, from no key in `maps/arena-test/wsl/config.yml` to one `log_verbose:
  true`. The tree did not change during the gate.

**Not exercised:** the 8845 package rebuild; a `reload` that flips the level; the `error` lines (no run hit an
internal failure: the five world lines, the union guard, the cut resync state, the refused add); a member dropped
for not reporting ready (its named line); `physics.debug_trace` / `arrival_trace` / the `BMMO_*` variables turning
verbose on; a room kick and a room close by a host; `sessions` on a session with no world (`no such session`); the
wire check (no wire change). A capture made by HEAD's binary replayed by this tree (`slqcap1` shows that the level
moves no clock read; that the step adds none is by inspection: no clock read added, moved or removed). SimTool's
stderr is the same by construction (its callbacks set only `log`; the new `warn` sites fall back to it), not
compared. No unit test was added (the test executable does not link `misc.cpp`). The NPLAYERS and PERF scripts were
edited, not run. The L6 re-drives (`pf_slq1_1`, `pf_slqq_1`), the capture replay (`slqcap1`) and the console
toggle (`slqc2`) ran on the tree before the review's fixes and were not run again: the fixes change only the
severity of failure-path lines, `describe`'s output and two `member_left` arguments. (`--report-only` on `slqf2_off`
gave the same FAIL.) Open for the user: whether public tests run verbose (the analyzer reads verbose lines).

## Built (the journal carries what the analyses read, step JNL, 2026-11-13)

**Why.** The user, 2026-11-13, after SLQ made the server log quiet and the public-test analyzer was found reading
verbose lines (input delay, member joins, resyncs, lead): those data belong in the journal, not in a log; a log is
for people, and the journal must reconstruct the whole scene, these data included. Census and design:
`.local-tools/work/JNL/` (the census's two facts: the 12 Level 13 server journals that differed between any two
runs differed only in the resync note's capture / coding times, raw clock reads; and with snapshots off a client
journal had no record of a rewind at all - `rb=`, `resim=` and the depths were log-only).

**The format.** Three tags inside version 10 (no bump; `docs/session-journal-plan.md` 'Typed records'):
`SCHEMA` (10), written with the HEADER in its chunk, names this writer's kinds, their class and fields; `STAT` (11)
a record of a logical kind (class 0: follows from the ticks, compared run to run); `CLOCK` (12) one of class 1 (a
value that follows a recorded clock site: exact under the virtual clock and a capture replay) or 2 (a raw clock
read, a link's status, or a record whose tick thread timing sets). One table, `session/journal_stats.hpp`
(`stat_kind`, `kStatKinds`); `journal_writer::stat(tick, kind, {values})` writes each value as its field's type
(zigzag LEB128, f64, a text of up to 65 535 bytes - `kJournalMaxText`, the notes' 4 096 stays; one of exactly the
limit was cut, which the readers flag) and picks the tag from the class; a debug build asserts the value count. A
file describes itself, so neither reader keeps the table (`read_schema_payload` / `read_stat_payload` / `stat_text`
in `journal.hpp`, `journal_tick::stats`; `journal_trace.parse_schema` / `parse_stat`; `SimTool --list` prints
`stat` / `clock` lines). Only the hosts' journals write them (`set_stats`, where the files are made compressed): the
tests' and SimTool's files keep their bytes. Nothing reads a clock for a record: each carries a value its site
already had; no K7 site moved. Every write is on the thread that writes the journal already (the runner, the client
core); what server.cpp records goes through `session_runner::journal_clock`, which writes at once on the runner's
thread when the session is there (`s5`, from `on_event_accepted`) and posts like `journal_note` otherwise (the
start's `link` records, whose session a queued command is still creating), and only for a CLOCK kind. The
compression stays on the I/O strand.

**The kinds.** Server: `server_config`, `ready`, `resync_state` (the note's numbers and the state's raw / framed
bytes, its reference, the request's last full snapshot and counted-through tick), `resync_cost` (the capture and
coding times that left the note: raw), `resync_wire`, `resync_request` (each request as the runner takes it, with
`why` - `resync`, `start` (the start handshake's), `again` (a base the runner stepped first, answered again), `join`
(a late joiner's state) - and `state`, 0 when the legacy full snapshot answers it, which server.cpp records itself),
`lead` (from the decision's plan and the sector bound the log line prints, computed once), `lateness`, `s1` (the
lateness meter's `[session N] S1 ...` lines - decisions, resets, lead acks, patience, clock skew - as their
key=value text: class 1, not 0 - a lead ack's and a reset's tick is a posted command's, and a raise's line carries the
link's `rtt=`, both of which a capture replay gives again and two runs need not), `s1_quorum` (its quorum summary and arrival lines, whose quantiles are raw clock reads),
`step`, `link`, `telemetry`, `s5` (class 1), `ghost`. Client: `rewind` (what each rewind the status line's `rb=`
counts was asked - `rewind_world` and the resync re-step: the anchor, the tick, a cause code, the corrected ball of
an input rewind; logical) followed by `rewind_base` (the base its restore was and the ticks it re-simulated: 440
of the fast LBM's 1 711 rewinds and 56 858 of one L6 re-drive's 72 749 restore at a record before the one asked;
raw, because some of those follow step LVR's wall-clock gate - `steady_clock`, no clock site - which records a heavy
frame's live tick light, so a later rewind restores the whole record before it: of six coordinated runs of the
re-drive one differed from the others in one client's two bases), both after the engine's cost meter closed; `window` (the window line's numbers and its tails; logical: its
depths count from the anchor asked); `status` (the status line's logical counters, `mism`, `hard`, `scr` ... all but
`resim`, at `end()` and the headless stop before the journal closes); `status_clock` (`resim`, the clock values and
the whole line, there and at each headless 5 s report, so a client killed later keeps its last line, as its log
does); `own_death`, `stall`. The resync note keeps its rows and bytes; its times moved to `resync_cost`.

**Compares.** `scripts/journal_compare.py A B --mode run|replay|legacy [--checkpoint-sets]` (files or directories;
no journal found is an error) walks the records decompressed (`journal_trace.records`, `canonical`, `compare`) and
names the first that differs: `run` skips CLOCK, masks the build id's commit and drops a pre-step resync note's
times - two runs on the virtual clock (a real-clock run's header `utc_ms` and TICK `ms` are the wall clock's, so
those differ in every mode); `replay` skips only class-2 CLOCK (`capture_replay.first_difference` uses it; a capture
replay's journal no longer equals the live one byte for byte); `legacy` skips the three tags too;
`--checkpoint-sets` compares a checkpoint's bodies as a set (the retail capture replay, `.local-tools/work/K7/replay.py`,
uses it with `replay`).

**The readers.** `scripts/session_facts.py` reads a journal once in file order (`Facts`) and renders its records
as the lines the log readers parse (`client_lines`, `server_lines`), so the criteria's parsers are unchanged and a
journal from before the step falls back to its log; `rewinds()` pairs each `rewind` with its `rewind_base`, `gate()`
gives a session's end counters. `scripts/arena_map_test.py` reads the criteria's facts from the journals
(`CRITERIA_SOURCE`, `--criteria-source journal|log`): `read_client_log`, the status metrics, `event_report`'s server
lines, `parse_s1`, `airship_spans`, `lead_mode`; with a verbose server log it builds the report from the logs too and
compares (`criteria_source_check`), and `scripts/lbm_fast_judge.py` fails a twin whose check is not `equal`
(no check: said, not failed). `scripts/pubtest_report.py` replaces the public-test server analyzer
(work/ARCH/pt/server.py, SRV/srvload.py, PERF/pt_server_resync.py stay as the history of pubtest1's numbers): journals
only, the typed records where there are, else derived from the records every journal has, else '-'; its end column
is the reason's category, so no member's name leaves the machine. `scripts/telemetry_report.py` reads the
`telemetry` records. `scripts/probe.py` takes the final status from the journal. `scripts/journal_judge.py`
`check_stats`: one `rewind` and one `rewind_base` per `rb=`, the depths summing to `resim=`, the stored line's
`mism=` / `rb=` / `resim=` the records', no text cut. `journal_trace.py --selftest` passes again (its PLAYER plan
lacked the `life` key since step TRL) and round-trips SCHEMA / STAT / CLOCK. Left on the log or stdout, by design:
the log level check, the live harness cues, tools' stdout (`diverged`, VERDICT), file copies, human greps, and the
perf one-offs in work/.

**Proof** (`.local-tools/work/JNL`, the fix pass in `JNL/fix`, the gate in `JNL/gate`). The review's defects and
their proof: the run compare hides no logical counter - the builder's diverging L6 pair (`pf_jnl1_1` /
`pf_jnlq_1`) projected onto the fields now logical: 13 of 13 clients equal (the difference was in `rewind_base`'s and
`status_clock`'s fields only), and a copy of a client journal with `mism` 0 written as 3 fails `--mode run` at its
`status` record (`fix/compare_proof.py`). Fast LBM on the final tree, `jnlf6`: 38/38 twins FINAL exact, the 38 rows
and every fail line equal to SLQ's `slqf1`, 38 of 38 'criteria from the journals vs the logs: equal', the 178
journals equal to `slqf1`'s under `legacy`, 650 of 650 line kinds rendered from the journals equal to the logs'
(`lines_proof.py`), 140 of 140 clients' 1 711 rewinds one for one with their log lines (the base, the depth, the
ball) and with `rb=` / `resim=` (37 377 ticks) (`rewind_proof.py`); a second run of five arms, `jnlf7` (A0, A6B, A6M
superseded, A10X, A5D): 10/10 FINAL exact, rows equal, its 50 journals equal to `jnlf6`'s under `run` and `replay`.
The L6 re-drive verbose (`pf_jnl3_1`) and at the default level (`pf_jnlq3_1`): the 14 journals equal to each
other's under `run` and `replay` and to SLQ's `pf_slq1_1` under `legacy`; from the records alone mism 0, `rb` 72 749
= the rewind records, `resim` 663 877 = their depths, `sdiff` 767 queued = 767 classified, in both runs, and each
rewind one for one with its log line; this pair did not diverge (`pf_jnl3_1`'s 72 749 bases and depths equal to the
builder's `pf_jnl1_1`, `pf_jnl2_1` and `pf_jnlq2_1`, two differing from `pf_jnlq_1`'s), so defect 1's proof is the
projection and the planted `mism` above; `telemetry_report.py` from the journal equal to the old reader on the
verbose log. The periodic status line: a coordinated run's headless client reports once (its 5 s report clock is the
wall clock, and a coordinated run is short in it), so a real-time A0 run (`jnlrt1`, 25 s): each client's journal
holds 6-7 `status_clock` records, their texts the log's status lines one for one, and one `status` record; a
real-clock run's TICK `ms` differ (the retail L12 runs of SLQ's and this gate: 1 564 TICK records differ), which is
why `run` needs the virtual clock. A server capture replay (`jnlcap2`, `run_capture.sh`) at both levels: `capture_replay.py` SAME, the replayed
journals equal to the live one under `replay` - its 109 class-1 records (81 `s1`, 28 `s5`) compared and equal, the
typed records differing only in the 6 `s1_quorum` and 6 `step` ones - and under `run`. The public test
(`D:/repos/bmmo-pubtest1`, read with `python -I`, aggregates only; the 30 server journals predate the step):
`pubtest_report.py`'s columns as before the fix, against the frozen log analyzer's 18 public sessions: minutes, the
input delay and the raises (116) equal in 18, the end in 17 (a crash: the log ends, the journal has no end note),
the resyncs in 15 (the log 2 151, the journals 2 148: requests whose state was never sent), the tight ones in 16
(1 638 / 1 636), N0 in 17, the peak in 16, player-hours within 0.03 in 12; substituted ticks an upper bound before
the step (the journals 265 662, the log 190 332): a pre-step journal cannot tell the ticks a resync leaves unnumbered,
which `resync_request` records now; the round trip, the resync MB and the step time were never in a journal ('-').
Gate (`gate/run_gate.sh`): 860/860 and `journal_trace.py --selftest`; x64, x86 and the Arch server at one build id; retail at K = 20, L12 and L11 at human key rates (`jnlg_l12_h_150_1`, `jnlg_ctl_h_150_1`): 0 mismatches, FINAL rows bit-equal to `r6on`'s with L11's 32 trafo pieces, the server log verbose, `refs=` faults 0 (from the log); every retail client journal carries the typed records (L12 23 / 58 rewinds with their bases and 26 windows, L11 61 / 91 and 72), `check_stats` PASS on all four, from the records mism 0, `rb` = the rewind records, `resim` = their depths (546, 1 406, 1 450, 2 212), `sdiff` queued = classified (2, 4, 11, 13), the journal's status metrics equal to the log's last status line (its full text differs in `remotes=`, 1 in the journal's, written before the end's teardown, 0 in the log's, printed after it); both retail server journals carry their kinds (`link`, `ready`, `s1`, `s1_quorum`, `s5`, `step`, `telemetry`, `ghost`) and `pubtest_report.py` reads its row from each alone (the L12 one by hand after the gate, from the same file: the gate's newest-file pick raced the file's mtime; fixed to a marker).

**Cost.** `stat()` builds the payload and appends it to the segment: 0.18-0.20 us a rewind record, 0.45-0.50 us a
window record (`stat_bench.cpp`, against 1.6-2.4 us a note); a rewind now writes two. The most typed records
measured in one tick: a client's 3 (2.5 kB: the end's status pair and one more), a server's 22 (an S1 summary tick; 13 members on L6: 29).
Size against SLQ's runs: the fast LBM's client journals +0.09 % raw / +0.42 % on disk, the server's +0.22 % / +0.42 %;
the L6 re-drive's clients +8.6 % raw / +9.5 % on disk (72 749 rewinds, two records each), its server +7.1 % / +1.6 %.

**Not done** (left out rather than recorded unproven): the lobby journal (J7, the user's call); `rewind_cost`,
`frame`, `report` (the process counters), the client's `config` and `telemetry`, `boot`, `base_due`, the end's flagged
/ rejected counts and the leave reason; `refs=` (the reference poses' counters) is in no record - the gate reads it
from the log; the criteria still parse the status line's text (`status_clock.text`) rather than the `status`
record's fields, and `session_facts` renders a spawn ghost's ball as `?`; the harness's default stays verbose and
its join cue the verbose line; `srtj_judge.py`, the per-step `run_gate.sh`, `ghost_state.py`,
`classify_rollbacks.py`, `pre_rollback_error.py`, `moved_own_sector.py`, `server_client_drift.py` and
`recipe_run.anchored` stay on the logs (their records exist now: rewind, ghost); the harness builds each report
twice while the journal-vs-log proof lasts (an arm 45 s -> 61 s).

**Not exercised:** a retail K7 capture replay with the new records (`K7/replay.py`'s record compare ran only on an
older pair, `k7f_l11_raised` client 1: equal with the bodies as sets, 1 281 of 10 336 records differ by position
without); a retail session that crashes (retail prints and journals its status line at the end only); a headless
client killed between reports; a text cut at 65 535 bytes outside the selftest; the `resync_request` kinds `start`,
`again` and the legacy full snapshot's (the runs made 32 `resync` and 6 `join`); `s1`'s raise / lead lines (they
carry `rtt=`) and lead acks under a capture replay (`jnlcap2` is an A0 arm with no lateness or lead
record), and the arrival trace landing in `s1_quorum`; `lbm_fast_judge` failing a twin on a check that is not
`equal` (every check was `equal`); the debug build's value-count assert (every build was Release; a parser found
every call site's count equal to its kind's); the gate script's marker-based server-journal pick (changed after the
gate ran); a run with snapshots on; the fast LBM's arms at the default log level; the wire check (no wire change);
the 8845HS cost.
