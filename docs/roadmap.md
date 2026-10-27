# Roadmap: one engine, one world from inputs, run-ahead with rollback

Status as of 2026-10-27 (HEAD 3940bf3 = trafo T4, plus step TR5's records).
The user's three goals, the principles that follow from them, what is
built (§3, with each step's commit), and the order of what is left (§4,
the orchestrator's queue of 2026-10-27). Each design named here has its own
document with its step table and its "As built" notes, which are the record
of what the code does.

## 1. The goals

1. **One core engine.** SimTool, the headless client and the retail mod share
   one implementation of rollback, the IVP savestate, the script replay, the
   physics bridge, the sector union and the session flow.
2. **One world from inputs, no snapshots.** Given the same lockstep inputs,
   every peer's physics world (the server's, every client's) is bit-identical.
   Snapshot corrections are not part of the design; they are a fallback kept
   only until the known divergence sources are fixed, and then switched off.
3. **Run-ahead with rollback.** A client runs a few frames ahead of the
   server. A divergence is handled by rewinding to the last frame that was
   not in doubt and replaying the inputs now known. The divergence source is
   an input the client did not have yet when it stepped the frame.

## 2. Principles (user decisions)

- Every script that affects physics runs in every world, driven by lockstep
  inputs. No owner-only physics script.
- A player entering the next sector changes nothing in the previous sector.
- Exception: the level-end balloon and its airship are local to each player.
- Every gap found is fixed, none is ignored. A failed check or a gap is
  investigated to its root cause first, then reported as symptom, cause and
  options; the user decides.
- The server never depends on snapshots. Clients judge every change with
  snapshots off first.
- **What counts as an input** (accepted 2026-10-24): the players' key frames;
  the lifecycle events with their stamps (birth, death, trafo, beam, sector
  entry); and the input the server applied for a player when that player's
  own frame was late (the server substitutes the previous frame and tells the
  owner, who amends its record and rewinds). The persistent case is reduced
  by the per-player lead (late-player design part two).
- **The one state transfer** (accepted 2026-10-24): a client whose history
  cannot reach the divergence (a stall past the reach) and a late joiner
  receive the server's state of one tick once (rows, the portable script
  record, the union), apply it as that tick and re-step to their own tick.
  This is not a per-tick correction and stays after snapshots are off.
- The retail legacy session path gets nothing new and is deleted at R7.

**Verification (the user's rules of 2026-10-27, verbatim).**

- "感觉我最多能接受15分钟的验证时间"
- "以后每轮步骤都要控制在15分钟内，不然推进太慢了。只有完结的时候再跑完整的验证"
- "每一步应该运行和本步最相关的验证"
- Asked how a step whose row needs more live runs than 15 minutes hold is
  gated: "15 分钟内只做最关键的一对" (one run with the change and one
  same-batch control, judged by the row's criteria as written).

So: every step's verification takes at most 15 minutes of wall clock (the
writing of the code not counted) and runs the checks most relevant to its
change - the ones that would catch its likely failures; a track close-out
(TR4, SP10) is such a step too. Checks that cannot influence each other run
in parallel; timing-sensitive ones run alone. Each step lists the row checks
it did not run and appends them to "Deferred to the final verification"
under §4's stage close-out; the complete verification runs once, at the
stage close-out (§4 item 7). A single live session is a smoke test; the
evidence for a behaviour claim comes from deterministic checks (the wire
check, pinned paired replays, the physics-free models).

**Operating rules while the user is away (accepted 2026-10-24).**

- A step whose own change is correct may be committed when a check exposes
  a pre-existing problem, provided the residual is reproduced on the build
  before the change, its root cause is found and written into the commit
  message, and its fix is queued as a step of its own. A residual whose
  cause is unknown, or which the step introduced, stops the step.
- A design detail that turns out wrong may be adapted without asking, as
  long as no principle above is touched; the adaptation goes into the
  design's as-built note and the commit message, for review on return.
  Anything that touches a principle waits.
- A step blocked on the user's decision does not stop the other tracks:
  work that does not depend on it goes on, and the blocked step is recorded.
- Disk: run outputs of committed steps under `maps/arena-test/` and stale
  build copies may be deleted; the reference capture sets, the directories a
  design note cites as a reproduction, and the current and previous step's
  build copies are kept.
- Defaults chosen for likely forks (both forks have been met): if the
  offline resync check (A2) shows rows alone miss 1 mm, the server's IVP
  savestate travels with the resync state (A2: rows alone missed, 14.4 mm
  and 8.4 mm; the blob travels since A3); if the trafo's collision-sound
  script leaves PreSimulate entries that refuse the savestate, the rollback
  engine's idempotent replayed Create comes first (T1's S8: no entries);
  no further snapshot-path work before the stage A close-out.

**Protocol numbers.** Protocol 10 / journal v4: the resync track's exact
resync (A3, 93c13f6). Protocol 11 / journal v5: the trafo replication's wire
(T2, 074f6d5). The next change of the wire is the sector placement's S5,
which takes the next free numbers (expected 12 / v6, and makes the
protocol-11 reference captures refuse). Bridge API: v20 A3, v21 trafo T1,
v22 trafo T4; the next is free. Forward compatibility is deliberately
broken: server and clients are rebuilt together.

## 3. Built

| area | commit | state |
|---|---|---|
| Deterministic engine across Windows x86/x64, Linux, ARM | - | bit-exact |
| Mechanisms, sector activation as an input (the union), spawn impulse, deterministic explosions, IVP savestate rollbacks, script replay | - | built |
| Shared session-client core (E0-E11); the headless client is a host | - | built |
| Retail mod hosts the core: R0 plumbing, R1 frame and snapshot path, R2 relay rewind | - | built (R2's checks: (a) since step P, (b) with R3's switch) |
| Late-player parts one (own frames substituted -> owner rewinds) and three (events re-stamped at acceptance, owner birth move, no barrier) | - | built (headless); retail: part one at R4, part three at R6 (not built) |
| Late-player part two revision 2 (meter, patience, SessionLead) | - | built behind flags, off; revision 3 replaces its policy |
| Trafo pieces tracked as mechanisms, explosion scripts replayed (P) | - | built; the path of every session without `physics.trafo_inputs` |
| Proximity countdown restored at the anchor (Q) | - | built |
| RR: applied snapshot rows re-applied in later replays (the last snapshot-path item; closed step P's check-2 residual) | a52ce48 | built |
| LB: a mirror born inside a replay is made again at its birth tick from the inputs | bf3fa76 | swing c0's four mirrors within 4.1e-6 m/s of the server at 702-712 (RR: 0.17 m/s) |
| Resync T0: `first_divergence` / `worst_bodies` `--from`, `--exclude` | 2f0c28a | built |
| REF: the headless reference set `core_ref_e0` re-recorded (765267c); the old set kept as `core_ref_e0_pre_q` | fcb4d37 | built |
| Resync A1: the portable script record, keep-local scripts | 8dbc0fe | built |
| Resync A2: `--resync-check` offline; Q1 decided - the server's blob travels (rows alone 14.4 mm / 8.4 mm) | 765267c | built |
| Resync B1: the sector union is rollback state; a late or moved Sector stamp rewinds and re-derives | 64be32e | built |
| Resync A3: the exact resync on the wire, protocol 10 / journal v4 | 93c13f6 | built |
| Resync A4: the full-tier gate; forced rewinds past 48 re-step to the reach; `core_ref_r3` captured | 114fd04, baf9d83 | 81 applies, 0 waits, -50 <= Δ <= +17, reach cap 95 kept (deepest D 59) |
| Resync A5 + sector placement S4: a late joiner takes the resync-state path behind the level-walk gate | 72108fd | built |
| Resync B2: a raise of a Sector-rewinding client is bounded by its rewind reach | 3fbc402 | built |
| Resync T1: `--replay-session` restores a client journal's RESYNC record | f1073ff | built (its findings (a)-(c): §4 item 2) |
| Retail R3: the lifecycle rewind and own echoes | 91df87d | mirror p99 4.99 m against the O runs' 6.10-18.04 m |
| Retail R4: the own corrections; the starvation detector deleted | abfee28 | built |
| Retail R5: the slew for a SessionLead, `CAP_LEAD` | b64fe49 | built (one resync at the run's end: the fixed 48-tick reach, §4 item 2) |
| Trafo T1: per-player piece sets, host support, bridge v21, SimTool S0-S13 | 2e6f026 | I0-I7 on 52 builds; S13 231/231 runs bit-equal; S8 0 PreSimulate entries |
| Trafo T2: the Trafo event, ledger, validation (4.5 m), deadline, leavers; protocol 11 / journal v5 | 074f6d5 | S10 22/22 on L2 and L11, server journals 1575/1575 matched |
| Sector placement S1: the row plan, `--list-placement` | cd501af | built |
| Sector placement S2: apply, signature, release; the world option (off) | 5b536b2 | built (its x64 teardown over-read: §4 item 2) |
| Sector placement S3: the invariant check I1 (`--second-move`, `--placement-trace`, `placement_check.py`) | 81c6803 | built |
| Sector placement S9: a level that ran a session is reloaded at the next countdown (retail) | 5cd1c11 | built |
| Rollback engine gaps G0 (EG0): diagnostics - the counted birth failures, the 1378 difference, the outside producers | 0b73af2 | built |
| Record-format gaps K0: SimTool `--array-reads`, `--globals-trace`, `--world-options` | 49e2cc8 | built |
| Rewind cost RC1: the replay re-predicts a remote's tail past its newest relayed frame with that frame (hold-last) | 5f967b4 | input rewinds -83..-87 % (swing, H1 at 150 ms), -67..-77 % (cliff), -44 % at 50 ms; no rebase in any arm; CPU parity -36 % |
| Resync lag RL1: a re-anchor's base on the fixed deadline schedule (server only) | 3daa19a | the other clients' relay lag after a resync 36-37 -> 20-21 ticks (resync_snapoff); resync_at150 c0 26.77 -> 22.3 |
| Trafo T3 (step TR3b): the core and the headless host, merged onto RC1 and RL1 | 3394ffd | H1 at 150 ms, snapshots off: pieces 150/150 bit-equal on four clients, starved 0/0/0/0 against the control's 0/0/0/0 |
| Trafo T4 (step TR4): the retail host (core on), bridge v22 | 3940bf3 | one two-copy L11 session: pieces 32/32 bit-equal across the clients (5056 rows), drawn on both |
| Trafo T5 (step TR5): the records | the `docs:` commit after 3940bf3 | `engine-changes.md` bridge note v21/v22, core design "As built (step T)", 9.29 |

## 4. What is left, in order

**Item numbers before this rewrite.** Notes written before 2026-10-27's
step TR5 cite this section by its older numbers. From a7afd09 to RC1
(5f967b4, exclusive): 1 LB, 2 REF, 3 the resync track, 4 retail R3-R5, 5 the
trafo, 6 sector placement, 7 world agreement, 8 rollback engine gaps, 9
record-format gaps, 10 the stage close-out, 11 late-player revision 3, 12
R6 / R7, 13 levels 8 and 11. From RC1 to TR4 (3940bf3): the same 1-9, 10 the
rewind-cost optimization, 11 the stage close-out, 12 late-player revision
3, 13 R6 / R7, 14 levels 8 and 11. Today: sector placement 1, rewind cost
3, rollback engine gaps 4, world agreement 5, record-format gaps 6, the
stage close-out 7, late-player revision 3 8, R6 / R7 9, levels 8 and 11 10;
the old items' open findings (N2, F, the CPU cliff, the T1 findings, the
x64 teardown) are item 2.

**Stage A - everything that affects physics is an input (goal 2).**

1. **Sector placement S5-S11** (`sector-placement-design.md` §8): one object
   per placement at the anchor; S5 the wire (protocol 12 / journal v6
   expected; its world-options field exists since T2), S6 the headless host,
   S7 the savestate check with a pass in the window, S8 the measurement, S10
   the retail host (the track's close-out, a 15-minute step), S11 the
   records. **Adaptation M3** (the orchestrator's): the placement's config
   default stays false until S10 (as T2 kept `trafo_inputs` off until its
   hosts ran it), otherwise every retail session on L1-L13 ends with
   "placement mismatch" until S10. S2's open points go with it: the plan's
   name bound of 63 (S5) and a scene that is only logged, not refused
   (S6 / S10).
2. **The items found unqueued** (the trial findings' critic,
   `.local-tools/work/TRIAL1/analysis/final/part4.md`, and the steps'
   findings), before the stage close-out, ordered by dependency:
   - **Stalls -> resyncs past the 48-tick reach at 150 ms each way.** With
     no raise, any server substitution of an own frame becomes a resync,
     because the substitution notice comes back past the fixed reach (the
     user's trial: three resyncs, "held notice for own tick 20585 arrived at
     tick 20648, past the reach of 48 ticks"; R5's run-end resync is the
     same). Late-player plan step 12 (the per-client reach) and open
     question 1. Not established: why the trial's client stopped reaching
     the server at 20485.
   - **Resync T2**: the trafo scripts out of `keep_local`
     (`resync-and-sector-timing-design.md` §8), unless TR3b's deferred H3b
     and a forced resync during a piece life show T3 covers it.
   - **Resync finding G1**: a script the restore makes active that the
     live run had finished never runs again in the hosts (it is not
     re-registered with the behaviour manager); only SimTool's
     `--resync-reregister` handles it. Fix: the restore re-registers what it
     activates (shared code, both hosts).
   - **The core's 30-unmatched-snapshots rule** (session-core design §7.1
     decision 1): still the retail host's transitional trigger
     (`core_host.cpp:1558`); into the core for both hosts, closed by an
     A/B in which it never fires.
   - **Step P's spawn frame-timing race** (core design "As built (step
     P)"): one run at R3 showed no mismatch at 214-218; re-measure, and
     identify what made P land in state B more often.
   - **Record-format Q5 / A23**: the session-start window's other failures
     (5 objects fewer at anchors 4-6, the sector manager's active flag at
     depth 0 for anchors 14-32); first decide session gap or harness gap.
   - **The x64 teardown over-read** (found at S2): Ballanced's
     `SetLocalParameterObject` (CK2 CKBehavior.cpp:1146-1155) makes
     `CKParameter::SetValue` copy a pointer parameter's 8 bytes from a
     4-byte `CK_ID`, so the particle block's BEHAVIORDETACH
     (GeneralParticleSystem.cpp:415) leaves 0x00007FF600000000 instead of
     NULL; a context reset that detaches a P_Extra copy's particle block
     before its Extra block makes `ShowParticles` read it: 0xC0000005 at
     world teardown. S2's `release` orders the placement's own copies; the
     builder's `*_MF001` copies are unordered, so an x64 server or headless
     world can crash at session end once an Extra block ran (retail x86
     cannot meet it). Fix: an engine change (write a null of the
     parameter's size, or have CK2 copy the full size), then an x64
     teardown sweep (sector-placement design, As built (step S2)).
   - **N2 on every world** (found at A4; resync design §7.4, As built (step
     A4)): the retail mod pins the checkpoint chain's Set Cell of
     IngameParameter[0][2] to 0; the headless client and the server's world
     do not, so an own crossing (or a clone's, on the server) in the frame
     of a sector pass deactivates the previous sector in that world alone
     (a4_resync_n2 c1: P_Modul_26_Sack 8498 m). N2's rewire moves into a
     shared `game/` header used by all three; [0][1] stays.
   - **F and the CPU cliff's closing check.** F (found at B1): the script
     replay consumes the next-frame flags of scripts the live frame skips
     (`P_Modul_18_Particle Script`; rollback-engine gaps). The CPU cliff
     (found at B2: three respawns within 15 ticks made every frame a
     38-39-tick rewind and the schedule rebased): profiled at CPUPROF as one
     input rewind per frame from the stale tail, removed by RC1 (B2's build
     1.31 ms per replayed tick, now 0.41 ms; the recipe no longer rebases);
     what is left is the closing check on the cliff recipe.
   - **Resync T1's findings** (resync design, As built (step T1)): (a) an
     own BodyRevived the server applies late is in the client journal only
     at its stamp - the server never echoes it to its owner
     (server.cpp:2046-2050); fix: echo a moved BodyRevived to its owner, the
     core journals it without acting on it (it overlaps world agreement's
     G5, which retires BodyRevived); (b) SimTool's client-journal replay
     runs one IVP tick ahead before its first restore; fix in
     `run_replay_session` with its own A/B; (c) a client journal lacks the
     SUBSTITUTED records for the owner frames substituted at 1098-1119 -
     first establish whether the server sent the notices.
   - **Trafo T4's findings** (trafo design, As built (step TR4 = T4)): (a)
     the retail rotation report under the game's 24-bit x87 (IVP's
     `calc_at_quaternion` slerp of a body turning > 2.6 degrees per tick is
     ~1e-7 off unit length; such rows' rotation differs from the server's
     by <= 3.1e-8, the state exact, `fpu53 on` makes all 5056 rows equal):
     a 53-bit control word around the bridge's body reads, or a row
     tolerance; (b) the reference-map digest across CK2 implementations
     (the reimplementation's `CKParameterOperation::Update` names every
     operation output `Pout 0`, the game keeps `Out`; diagnostic only:
     nothing on the wire, localization keys scripts by owner and name):
     describe an operation output by its operation's place, or keep the name
     in the reimplementation - the user's call; (c) the retail birth log in
     a session without trafo inputs logs a replayed tick's births at the
     live tick + 1 (T4 fixed it with the inputs on only).
   - **RL1b** (`.local-tools/work/CHAIN3/queue.md`, the redone review of
     RL1, no blocker): the As-built note's re-step depths (12 -> 28 on
     resync_snapoff, 34 -> 29 at 150 ms, and why), the held window, the late
     joiner off the per-player path, the base-missed fallback limited to
     `CAP_RESYNC_STATE` members; `tick_at` before `start_`, the floor for
     members without `CAP_RESYNC_STATE`, stale comments (late_tick.hpp:3-4,
     server.cpp:1289, :1734, :1791-1792); a runner-level test that both
     start paths store the schedule. One small step once nothing else is
     working in `server.cpp`.
3. **The rewind-cost optimization** (`rewind-cost-design.md`; RC1 built):
   RC2 an incremental capture for sleeping bodies (ceiling 22-24 %), RC3 no
   body-set full snapshot while snapshots are off (9-11 %, its own drift
   check), RC4 the dormant-script encoding (5 %; trafo design 9.4), RC5 an
   incremental restore (3-4 %). The close-out is the trafo design's H4 / H6
   **with snapshots on** (decision (b), 2026-10-25) against T3's
   snapshots-off numbers (TR3b's H1 at 150 ms: t_rep 0.99-1.15 s, t_max
   43-46 ms, starved 0, against the option-off control's 0.34-0.41 s, 12-14
   ms, 0). **On its pass `physics.trafo_inputs` turns on by default**
   (adaptation M2: it stays off until then; deployments run snapshots on).
   RC6 (the R6 / R7 tail refinement) is not adopted; it stays a possible
   later refinement if human rapid-trigger play shows periods (its open
   points: RC1c's dwell captures not re-measured, the R4 fallback's
   forward / backward keys, the resync_at150_down d0 cell).
4. **Rollback engine gaps F5-F7** (`rollback-engine-gaps-design.md`): Q the
   PreSimulate queue in the script record (engine access change #20,
   reserved), L lives that end inside a window (gap L), B the retail beam's
   generation (it also covers TR3's R4: H5 131:47's P_Modul_01_Pusher
   0.0866 m, the beam's record retired by the trafo's new life). G0 is
   built (EG0): L3 needs the dead own entity's CK matrix per tick while the
   ball has no body (design §9.7, decided at G8).
5. **World agreement** (`world-agreement-gaps-design.md`), after F5-F7
   (WAG:1204, order constraint M6): client proximity sensors rewired to
   the nearest ball (own or mirror; the G-c leader-only wakes P_Modul_03
   1.36 m, P_Modul_19_Flaps 0.8-1.8 mm, P_Modul_34 2.7 m), the
   member-added message (trafo design 9.7: existing members do not know a
   late joiner's join order), leave and rejoin, BodyRevived retired (G5).
   Its Q9 (the protocol landing order) needs the user.
6. **Record-format gaps** (`record-format-gaps-design.md`; K0 built): K1
   the time factor and gravity per tick; **K3 = the Level 1 tutorial
   switched off** right after K1 (decision (c): every world activates the
   tutorial's exit 10 frames after the level start, counted in frames, the
   delay the main branch uses and the user requires); K2, the ActiveBall
   census, after world agreement's G3 / G4 (it must also decide the sector
   manager's `PH[*][4]` read that K0 found, written in play only on
   extra-point rows).
7. **Stage close-out (the one FULL verification):** four clients, 150 ms,
   10 % loss, snapshots off: the spawn pile and every mechanism
   bit-identical to the server at every checkpoint; and every check below.
   On pass: snapshots off in the deployed configuration and in the retail
   default; only the one state transfer remains.

   **Deferred to the final verification** (checks a quick-tier step did
   not run; each step appends its own, the close-out runs them all):
   - RC1 (committed 2026-10-27, quick tier; the committed tree is the
     measured build: wire check ALL SAME, solo IDENTICAL): (a) the live
     four-client snapshots-off A/B of the committed build (§7 check 5 of
     `rewind-cost-design.md`; run at the build, not re-run at the commit:
     nothing changed); (b) its CPU parity (§7 check 7; -36 % at the build,
     not re-run: FULL only); (c) the repeated drift sessions RC1c named and
     did not run, O2 / N2 at dwell 30 on cliff and resync_at150; (d) a
     retail two-copy run of the committed x86 build with core on (built,
     CLIENT-BUILD-OK; the user's round 1 ran the same tree's hold-last
     build, `TRIAL1/holdlast`).
   - RL1 (quick tier, 2026-10-27; ran: the base-rule unit tests, x64
     build, tests, GCC, the wire check rRC1 -> rRL1 ALL SAME, one live
     resync_snapoff and one resync_at150 against the rebuilt Arch server,
     both server journals exact): (a) the late-join arm (the joiner's base
     moved onto the schedule too); (b) lead_adjust / patience on with a
     stall-triggered resync (the `L_eff` path); (c) the LAN case (D at its
     floor of 6, where the floor `current_tick + 2` may bind); (d) a
     retail two-copy run with a stall-triggered resync (the trial's case;
     server-only change, the x86 build is unchanged).
   - TR3b (trafo T3 merged on RC1 + RL1, quick tier, 2026-10-27; ran: x64,
     x86, Arch builds, 522 tests, GCC 29 ok, the wire check rRL1 -> rTR3b
     on `core_ref_rc1` (36/36 journals IDENTICAL, one SessionReady send per
     client), solo IDENTICAL, ONE H1 pair at 150 ms snapshots off (option on
     and its same-batch option-off control; the user's one-pair rule):
     (a) the second H1 pair at 150 ms; (b) H1 at 0 ms, for H1 (d) (late
     Trafo events 0); (c) H3b (missed trafo inputs and their resyncs);
     (d) H5 (`savestate_check.py` on the pieces); (e) H7 (the late
     joiner's reconcile); (f) the re-capture of `core_ref_t3` with the
     final binaries (the pre-RC1 set is not replayable by the merged
     build: its journals differ from tick 253, hold-last); (g) a retail
     two-copy run of the merged x86 build, core off and on (built,
     CLIENT-BUILD-OK; no BallanceMMOClient source changed, but the
     shared engine and savestate changes reach both retail modes - the
     savestate index rewrite, and the replay-mismatch counter of
     `rollback.hpp` now counted only while logged, the first 8; neither
     changes a simulation result, and the core-off BAR rests on this run).
   - TR4 (trafo T4, the retail host, quick tier, 2026-10-27; ran: x64 and
     x86 builds, 522 tests, GCC 29 ok, the wire check rTR3b -> rTR4 ALL
     SAME twice, ONE two-copy retail session on L11 with core on and
     `trafo_inputs` on, snapshots off): (a) R1, the solo forced arms
     (`r2_run.py` R2 scenario, 50:12, 97:24, 131:47, 13:24) and the
     two-trafo `r2t_run.py`; (b) R2 with the R2 config (lifecycle rewind and
     own echoes off) and R2b with `net lag 150` (late Trafo events inside
     input rewinds); (c) R3, a second session in one game process after a
     single-player paper trafo (sets rebuilt, no `_Trafo` object left,
     the quiesce's 18/18); (d) R4, core off against `p3_off_P`; (e) R5, the
     wire on trafo-free captures with the retail build; (f) an out-of-reach
     trafo rejected and logged; (g) the H arms' A/B; (h) CPU parity;
     (i) three launches without a hang.
   - TR5 (trafo T5, the records and this roadmap, docs only, 2026-10-27;
     its row's gate is a review): nothing deferred.

**Stage B - the run-ahead policy (goal 3, the persistent late case).**

8. Late-player design revision 3, R0-R5 (arrival ring, baseline, quorum
   patience, the lead decision), plus plan steps 12 (per-client reach; see
   item 2's first entry), 21 (events on the input datagram), 22 (own death
   move).

**Stage C - close the core (goal 1).**

9. R6 retail own-body control (bridge and script work; after plan step 21's
   measurement; with it late-player part three on retail, the
   `own_body_backstop` measurement and the retail hold-event verb), R7
   delete the legacy path; three launches without a hang. R6 also removes
   the retail beam's own residual (measured at EG0,
   rollback-engine-gaps-design As built EG0): the owner writes its body in
   place where the server makes a fresh one, and the body carries its lane
   state into the stamp; the arena's death beam at 1300 with
   `r0_keys.bmrc` is 8.6 mm / 0.879 m/s off the server, over the ball's
   0.5 m/s tolerance, so with snapshots off the beam stamp's full snapshot
   corrects it (9.7 mm / 0.319 m/s with r2's keys, under it, since R3).

**Stage D - the original levels.**

10. Levels 8 and 11, sector 1: solo savestate check and four clients,
    snapshots off. The pooled-modules diagnosis's open points (retail CK2
    `Reset`, P_Modul_01's Rinne/Filler) are checked here.
