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

## 3. Built

| area | state |
|---|---|
| Deterministic engine across Windows x86/x64, Linux, ARM | bit-exact |
| Mechanisms, sector activation as an input (the union), spawn impulse, deterministic explosions, IVP savestate rollbacks, script replay | built |
| Shared session-client core (E0-E11); the headless client is a host | built |
| Retail mod hosts the core: R0 plumbing, R1 frame and snapshot path, R2 relay rewind | built |
| Late-player parts one (own frames substituted -> owner rewinds) and three (events re-stamped at acceptance, owner birth move, no barrier) | built (headless); retail: R4, R6 |
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
4. Retail completes goal 3: R3 lifecycle rewind, R4 part one, R5 the slew
   (`session-client-core-design.md` §6.2).
5. The trafo as a replicated input (`trafo-replication-design.md`): T1-T5.
   Per-player piece sets; explosion, paper wind and reset in every world;
   a 20 s piece life; the explosion stamped at the dephysic (vanilla timing);
   out-of-reach trafos rejected and logged.
6. Sector placement (`sector-placement-design.md`): S1-S11. One object per
   placement at the anchor; the late joiner's lost sector-1 walk (S4, which
   may land early).
7. World agreement (`world-agreement-gaps-design.md`): client proximity
   sensors rewired to the nearest ball (own or mirror), the member-added
   message, leave and rejoin.
8. Rollback engine gaps (`rollback-engine-gaps-design.md`): the PreSimulate
   queue in the script record (engine access change #20), lives that end
   inside a window, the retail beam's generation.
9. Record-format gaps (`record-format-gaps-design.md`): time factor and
   gravity per tick, Level 1 switches the tutorial off (main-branch exit,
   10-frame delay, every world), the array-read census.
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

**Stage D - the original levels.**

13. Levels 8 and 11, sector 1: solo savestate check and four clients,
    snapshots off. The pooled-modules diagnosis's open points (retail CK2
    `Reset`, P_Modul_01's Rinne/Filler) are checked here.

Protocol numbers: the first of the resync track, the trafo replication and
the sector placement to land takes 10 (journal v4); the next takes the next.
