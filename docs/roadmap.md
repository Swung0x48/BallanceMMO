# Roadmap: one engine, one world from inputs, run-ahead with rollback

Status as of 2026-10-24 (HEAD a52ce48). The user's three goals, the
principles that follow from them, what is built, and the order of what is
left. Each design named here has its own document with its step table.

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
- Full verification only when a stage closes; ordinary steps get a quick
  verification; checks that cannot influence each other run in parallel.
- The retail legacy session path gets nothing new and is deleted at R7.

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
- Defaults chosen for likely forks: if the offline resync check (A2) shows
  rows alone miss 1 mm, the server's IVP savestate travels with the resync
  state (measured first); if the trafo's collision-sound script leaves
  PreSimulate entries that refuse the savestate, the rollback engine's
  idempotent replayed Create comes first rather than a change to the sound
  script; no further snapshot-path work before the stage A close-out.

## 3. Built

| area | state |
|---|---|
| Deterministic engine across Windows x86/x64, Linux, ARM | bit-exact |
| Mechanisms, sector activation as an input (the union), spawn impulse, deterministic explosions, IVP savestate rollbacks, script replay | built |
| Shared session-client core (E0-E11); the headless client is a host | built |
| Retail mod hosts the core: R0 plumbing, R1 frame and snapshot path, R2 relay rewind | built |
| Late-player parts one (own frames substituted -> owner rewinds) and three (events re-stamped at acceptance, owner birth move, no barrier) | built (headless); retail: part one at R4 (built), part three at R6 |
| Late-player part two revision 2 (meter, patience, SessionLead) | built behind flags, off; revision 3 replaces its policy |
| Trafo pieces tracked as mechanisms, explosion scripts replayed (P) | built |
| Proximity countdown restored at the anchor (Q) | built |
| Applied snapshot rows re-applied in later replays (RR; the last snapshot-path item) | built |

## 4. What is left, in order

**Stage A - everything that affects physics is an input (goal 2).**

1. LB: mirrors born inside a replay are one tick of navigation force short
   (found by RR). Root cause, then fix.
2. REF: re-record the headless reference captures with the current binaries.
3. The resync and sector-timing track (`resync-and-sector-timing-design.md`):
   T0, B1 late or moved Sector stamps rewind, A1 portable script record, A2
   offline resync fidelity, A3 the exact resync on the wire, **A4 full-tier
   close-out**, A5 late joiner through the same path, B2, T1.
   **RL1 done** (2026-10-27, found by the user's trial; late-player design
   §2.8, "As built (step RL1)"): every re-anchor's base (resync, resync at
   `L_eff`, late join) is taken from the session's fixed deadline schedule,
   `max(tick_at(now, lead), current_tick + 2)`, instead of the current tick,
   so a resync made while the server waits on that member no longer leaves
   every other client about 15 ticks behind (resync_snapoff: 36-37 -> 20-21;
   resync_at150: c0 26.77 -> 22.3). Server only, no protocol change.
4. Retail completes goal 3: R3 lifecycle rewind, R4 part one (built), R5 the slew
   (built; its A5u analogue ends in one resync of the raised client, the CPU
   cliff of item 8 meeting the fixed reach of plan step 12, reproduced
   without a raise: core design "As built (step R5)")
   (`session-client-core-design.md` §6.2).
5. The trafo as a replicated input (`trafo-replication-design.md`): T1-T5
   (T1 built: host support, bridge v21, SimTool S0-S13; S8 found no
   PreSimulate entry, so 9.2 needs nothing: "As built (step T1)"; T2 built:
   the wire and the server, protocol 11 / journal v5; **T3 built**: the
   core and the headless host, judged with snapshots off, its snapshots-on
   CPU gate moved to item 10; committed at step TR3b, the commit after
   RL1 3daa19a that adds this note; the server's
   `physics.trafo_inputs` stays off, past T4 until item 10's close-out).
   Per-player piece sets; explosion, paper wind and reset in every world;
   a 20 s piece life; the explosion stamped at the dephysic (vanilla timing);
   out-of-reach trafos rejected and logged.
   T3 (its H1 (c) at 150 ms failing on starvation at its merge round)
   waited for item 10's step RC1 by the user's decision of 2026-10-25 and
   was re-gated on top of RC1 and RL1 under its original H1 (c) at TR3b
   with one H1 pair (the user's one-pair rule of 2026-10-27): starved
   0 / 0 / 0 / 0 against the same-batch control's 0 / 0 / 0 / 0, input
   rewinds 25-35 per client (128-181 before RC1; control 21-30), pieces
   150/150 bit-equal on all four (trafo design §8, "As built (step
   TR3b)"). T3's remaining CPU (t_rep 2.6-2.9x the control, t_max 43-46
   ms against 12-14) is item 10's.
   **T4 built** (step TR4, 2026-10-27, quick tier; trafo design §8, "As
   built (step TR4 = T4)"): the retail host (core on) builds every
   member's set, runs the inputs, the hold, the quiesce and the teardown,
   installs the controller alias (bridge API v22); one two-copy L11
   session: pieces 32/32 bit-equal across the two clients, the other
   player's copies drawn; `physics.trafo_inputs` stays off (adaptation M2,
   until item 10's close-out). Queued from it, each root-caused there:
   (a) **retail rotation reports under the game's 24-bit x87**: IVP's
   `calc_at_quaternion` slerp of a body turning > 2.6 degrees per tick is
   ~1e-7 off unit length on the game thread (Direct3D's precision), so such
   rows' rotation differs from the server's by <= 3e-8 (state exact;
   `fpu53 on` makes all 5056 piece rows bit-equal) - fix the report (a
   53-bit control word around the bridge's body reads) or accept it as a
   row tolerance; (b) **the reference-map digest across CK2
   implementations** (R2's criterion) differs on one name: the
   reimplementation's `CKParameterOperation::Update` renames every
   operation output `Pout 0`, the game keeps the file's `Out` - describe an
   operation output by place in the map, or keep the name in the
   reimplementation; the user's call.  Severity: diagnostic only - both
   digests are only logged (`trafo_sets.hpp` build step; SimTool's
   `trafo_tool` compares the value digest, which is equal), nothing on the
   wire carries them, and the portable script record keys scripts by owner
   and name, never by an operation output's name, so localization is
   unaffected.  The cheap option: `trafo_reference_map`'s `where()` names a
   source parameter outside the blocks by its operation's place (root
   script + operation index + `:out`) instead of its name - it changes only
   logged digests (every recorded one once), never a simulation result; (c) the retail birth log in a
   session without trafo inputs still logs a replayed tick's births at the
   live tick + 1 (T4 fixed it with the inputs on only).
6. Sector placement (`sector-placement-design.md`): S1-S11. One object per
   placement at the anchor; the late joiner's lost sector-1 walk (S4, which
   may land early). Also found at S2, **the x64 teardown over-read** (not
   placement-specific): Ballanced's `SetLocalParameterObject` (CK2
   CKBehavior.cpp:1146-1155) makes `CKParameter::SetValue` copy a pointer
   parameter's 8 bytes from a 4-byte `CK_ID`, so the particle block's
   BEHAVIORDETACH (GeneralParticleSystem.cpp:415) leaves 0x00007FF600000000
   instead of NULL; when the context reset detaches a P_Extra copy's particle
   block before its Extra block (CK's hash order inside one copy, F5),
   `DestroyExtra` deactivates the particle scripts and `ShowParticles` reads
   it: 0xC0000005 at world teardown. S2's `release` orders the placement's
   own copies; the builder's `*_MF001` copies are unordered, so an x64 server
   or headless world can crash at session end once an Extra block ran.
   Retail x86 cannot meet it. Fix: an engine change (write a null of the
   parameter's size, or have CK2 copy the full size), then an x64 teardown
   sweep (sector-placement-design.md, As built (step S2)).
7. World agreement (`world-agreement-gaps-design.md`): client proximity
   sensors rewired to the nearest ball (own or mirror), the member-added
   message, leave and rejoin. Also its own step, **N2 on every world**
   (found at A4; `resync-and-sector-timing-design.md` §7.4, As built (step
   A4)): the retail mod pins the checkpoint chain's Set Cell of
   IngameParameter[0][2] to 0 (N2); the headless client and the server's
   world do not, so an own crossing (or a clone's, on the server) in the
   frame of a sector pass deactivates the previous sector in that world
   alone (a4_resync_n2 c1: P_Modul_26_Sack 8498 m; r4_lane_loss10 c1,
   before B1). N2's rewire moves into a shared `game/` header used by all
   three; [0][1] stays (the own sector poll reads it).
8. Rollback engine gaps (`rollback-engine-gaps-design.md`): the PreSimulate
   queue in the script record (engine access change #20), lives that end
   inside a window, the retail beam's generation, and F (found at B1): the
   script replay consumes the next-frame flags of scripts the live frame
   skips (`P_Modul_18_Particle Script`). G0 (diagnostics) is done: the
   r2 arms' `birth_recreate_failures` are all own-ball birth items, the
   1378 difference is the dead own entity's CK matrix, which scripts
   outside the replay move after the death (the resetpoint at D+1, the trafo
   animation): L3 needs it per tick while the ball has no body (design
   §9.7, decided at G8). Also the CPU cliff (found at B2,
   no design yet): three respawns within 15 ticks (A5u at 150 ms, 2596-2612)
   make every frame a 38-39-tick lifecycle rewind, the frame loop falls
   about 33 ticks behind wall time, the schedule rebases and the client
   resyncs - on all four clients with the flags off, on the raised one with
   them on (`resync-and-sector-timing-design.md`, As built (step B2)).
   Root cause the per-frame cost first (one rewind per frame for the
   frame's lifecycle set, or the rewinds' own cost). Profiled at CPUPROF
   (item 10): the per-frame rewinds are input corrections, not lifecycle
   ones, one per frame, 37-40 deep, and one mechanism - the replay keeps a
   remote's stale tail past its newest relayed frame (item 10, candidate
   1, removed by RC1's hold-last since), so each key change of the late player and each respawned ball's
   birth costs one rewind per arriving frame for the relay lag; B2's build
   paid 1.31 ms per replayed tick, today 0.41 ms, and the recipe no longer
   rebases.
9. Record-format gaps (`record-format-gaps-design.md`): time factor and
   gravity per tick, Level 1 switches the tutorial off (main-branch exit,
   10-frame delay, every world), the array-read census (K0's tools built;
   K2 must also decide the sector manager's `PH[*][4]` read that K0's census
   found - written in play only on extra-point rows, As built (step K0)). Also three found
   at T1 (`resync-and-sector-timing-design.md`, As built (step T1)):
   (a) an own BodyRevived the server applies late is in the client journal
   only at its stamp - the server never echoes it to its owner
   (server.cpp:2046-2050), so a client journal's replay wakes the body
   early (`resync_at150_down` c0 not exact after S); fix: the server echoes
   a moved BodyRevived to its owner and the core journals it without acting
   on it; (b) SimTool's client-journal replay runs one IVP tick ahead before
   its first restore (group 0, the founding PLAYER records, steps an empty
   tick 27); fix in `run_replay_session` with its own A/B, since every
   client-journal replay's output changes; (c) the same journal has no
   SUBSTITUTED record for the owner frames the server substituted at
   1098-1119 (masked there by the second RESYNC, whose state covers them).
   Known: the client had fallen behind (the schedule rebased at 1067 and
   asked for that resync; its FRESH records stop at 1106, its old tick at
   the apply; SUBSTITUTED ones stop at 1097) and its status counts 0
   stale-epoch notices and 3 base ones, so the server's notices for
   1098-1106 never reached its old epoch. Not known: whether the server
   sent them; establish that, then decide whether a client journal should
   carry them (a replay is the server's world only from S either way).
10. The rewind-cost optimization (`rewind-cost-design.md`; user's decision,
    2026-10-25): **RC1 committed with hold-last** (the commit after 49e2cc8
    that adds this note, `rollback: ...`; the user's ruling of 2026-10-27,
    `rewind-cost-design.md` §11: "先接受那个吧", "Commit RC1 with hold-last,
    then fix RL1 right after, then continue the chain under the 15-minute
    rule.") - the replay re-predicts a remote's tail past
    its newest relayed frame with that frame (hold-last, the live
    predictor's rule), so a relayed frame repeating it is no correction; it
    comes before trafo T3 by the user's decision of 2026-10-25. Measured
    (snapshots off, same batch, "As built (step RC1)"): input rewinds -83..-87 %
    on swing and H1 at 150 ms (option off), -67..-77 % on the cliff, -44 % at
    50 ms; no schedule rebase or rebase resync in any arm (the old build
    rebased and resynced on every client of five arms); CPU parity -36 %. The
    drift check's FAIL at the build (unpinned) was settled by the pinned
    method (Measurement RC1d: 2.6x / 3.4x fewer input rewinds than the old
    stale tail at 20 / 42 ticks; the drift split by lag) and by the user's own
    play (Measurement RC1e: hold-last, R6 and R7 within about 2 % in drift,
    R6 +6-11 % input rewinds pinned, +24 % modelled). RC6 (the R6 / R7 tail
    refinement) is not adopted; it stays a possible later refinement if
    human rapid-trigger play shows periods. Remaining: RC2 an incremental capture
    for sleeping bodies (ceiling 22-24 %), RC3 no body-set full snapshot while
    snapshots are off (9-11 %, its own drift check), RC4 the dormant-script
    encoding (5 %), RC5 an incremental restore (3-4 %); the close-out is the
    trafo design's H4 / H6 with snapshots on (decision (b), 2026-10-25),
    against T3's snapshots-off CPU numbers (step TR3b, H1 at 150 ms with
    RC1 in: t_rep 0.99-1.15 s, t_max 43-46 ms, starved 0, against the
    option-off control's 0.34-0.41 s, 12-14 ms, 0; trafo design §8, "As
    built (step TR3b)"). The CPUPROF profile and its ranked candidates:
    `rewind-cost-design.md` §1 and trafo design §8, "The rewind-cost
    profile".
11. **Stage close-out:** four clients, 150 ms, 10 % loss, snapshots off: the
    spawn pile and every mechanism bit-identical to the server at every
    checkpoint. On pass: snapshots off in the deployed configuration and in
    the retail default; only the one state transfer remains.

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

**Stage B - the run-ahead policy (goal 3, the persistent late case).**

12. Late-player design revision 3, R0-R5 (arrival ring, baseline, quorum
    patience, the lead decision), plus plan steps 12 (per-client reach),
    21 (events on the input datagram), 22 (own death move).

**Stage C - close the core (goal 1).**

13. R6 retail own-body control (bridge and script work; after plan step 21's
    measurement), R7 delete the legacy path; three launches without a hang.
    R6 also removes the retail beam's own residual (measured at EG0,
    rollback-engine-gaps-design As built EG0): the owner writes its body in
    place where the server makes a fresh one, and the body carries its lane
    state into the stamp; the arena's death beam at 1300 with
    `r0_keys.bmrc` is 8.6 mm / 0.879 m/s off the server, over the ball's
    0.5 m/s tolerance, so with snapshots off the beam stamp's full snapshot
    corrects it (9.7 mm / 0.319 m/s with r2's keys, under it, since R3).

**Stage D - the original levels.**

14. Levels 8 and 11, sector 1: solo savestate check and four clients,
    snapshots off. The pooled-modules diagnosis's open points (retail CK2
    `Reset`, P_Modul_01's Rinne/Filler) are checked here.

Protocol numbers: the first of the resync track, the trafo replication and
the sector placement to land takes 10 (journal v4); the next takes the next.
