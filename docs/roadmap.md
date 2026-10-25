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
4. Retail completes goal 3: R3 lifecycle rewind, R4 part one (built), R5 the slew
   (built; its A5u analogue ends in one resync of the raised client, the CPU
   cliff of item 8 meeting the fixed reach of plan step 12, reproduced
   without a raise: core design "As built (step R5)")
   (`session-client-core-design.md` §6.2).
5. The trafo as a replicated input (`trafo-replication-design.md`): T1-T5
   (T1 built: host support, bridge v21, SimTool S0-S13; S8 found no
   PreSimulate entry, so 9.2 needs nothing: "As built (step T1)").
   Per-player piece sets; explosion, paper wind and reset in every world;
   a 20 s piece life; the explosion stamped at the dephysic (vanilla timing);
   out-of-reach trafos rejected and logged.
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
   frame's lifecycle set, or the rewinds' own cost).
9. Record-format gaps (`record-format-gaps-design.md`): time factor and
   gravity per tick, Level 1 switches the tutorial off (main-branch exit,
   10-frame delay, every world), the array-read census. Also three found
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
10. **Stage close-out:** four clients, 150 ms, 10 % loss, snapshots off: the
    spawn pile and every mechanism bit-identical to the server at every
    checkpoint. On pass: snapshots off in the deployed configuration and in
    the retail default; only the one state transfer remains.

**Stage B - the run-ahead policy (goal 3, the persistent late case).**

11. Late-player design revision 3, R0-R5 (arrival ring, baseline, quorum
    patience, the lead decision), plus plan steps 12 (per-client reach),
    21 (events on the input datagram), 22 (own death move).

**Stage C - close the core (goal 1).**

12. R6 retail own-body control (bridge and script work; after plan step 21's
    measurement), R7 delete the legacy path; three launches without a hang.
    R6 also removes the retail beam's own residual (measured at EG0,
    rollback-engine-gaps-design As built EG0): the owner writes its body in
    place where the server makes a fresh one, and the body carries its lane
    state into the stamp; the arena's death beam at 1300 with
    `r0_keys.bmrc` is 8.6 mm / 0.879 m/s off the server, over the ball's
    0.5 m/s tolerance, so with snapshots off the beam stamp's full snapshot
    corrects it (9.7 mm / 0.319 m/s with r2's keys, under it, since R3).

**Stage D - the original levels.**

13. Levels 8 and 11, sector 1: solo savestate check and four clients,
    snapshots off. The pooled-modules diagnosis's open points (retail CK2
    `Reset`, P_Modul_01's Rinne/Filler) are checked here.

Protocol numbers: the first of the resync track, the trafo replication and
the sector placement to land takes 10 (journal v4); the next takes the next.
