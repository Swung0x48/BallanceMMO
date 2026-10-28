# Roadmap: one engine, one world from inputs, run-ahead with rollback

Status as of 2026-10-27 (HEAD fa8218d = sector placement S10, plus step
SP11's records: the sector placement is built and in §3).
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
- **The retail session core is on by default** (the user, 2026-10-27, told
  that S10 turned the server's placement on by default and that a member
  without the core is then refused from the session: "最终应该是要开启的？
  那就全部开启", then "core在正式客户端开启吧"). `session core off` still
  selects the legacy path for the next session until R7 deletes it.
- **Host logic is written once** (the user, 2026-10-27, asked why Stage C -
  one core - does not come first, told that the core is already shared and
  that what is still written two or three times is the per-host glue, e.g.
  the placement's host logic in the server's `physics_world` and the
  headless host with a third copy planned at SP10, and asked whether to
  insert a step before SP10: "对，插这一步，统一这些逻辑" - yes, insert this
  step, unify this logic). The rule for this and every later step: host-side
  logic is written ONCE, in a shared helper (`BallanceMMOCommon/include/game/`
  or `session/`: `game/host_backend.hpp`, `game/session_host_support.hpp` or
  the track's own header) that the server's world, the headless host and the
  retail core host all call; a host keeps only its platform operations (what
  only that platform can do: its physics backend, its log, its CK object
  lookups). No new per-host copy. Built at step HU1 (§4 item 1, "Host
  unification (step HU1)", with the inventory of what was unified and what
  is queued); `docs/session-client-core-design.md` §2.4 records it.

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
(T2, 074f6d5). Protocol 12 / journal v6: the sector placement's wire (S5:
`WORLD_PLACEMENT_CLONES`, `CAP_PLACEMENT`, SessionReady's
`anchor_placement`, the header's); the protocol-11 reference captures
(`core_ref_rc1`, `core_ref_t3`, `core_ref_t3off`) refuse it, so S6
re-captures the set. Bridge API: v20 A3, v21 trafo T1,
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
| Sector placement S5: protocol 12 / journal v6 - the world option bit, `CAP_PLACEMENT`, the signature compare (late joiners too), config default off (M3), SimTool replay honouring the header, the plan bound 63 | bc88b8e | 530 tests; a v5 journal replays 4112/4112; "mismatch" live |
| Sector placement S6: the headless host (anchor call, signature, release, option-off policy), the scene refusal, `--sector-at`, repeatable `--beam`; `core_ref_s6` re-captured | 0d1178b | L8 cross at 0 ms: five sides 39 copies, signature `91a138e4fd6e7254`, one walk and sector ticks, 45/45 |
| Replay order RO1 (from SP7): the script replay runs in the live frame's order, window-born fixed bodies made again by the replay, size-changed strings restored | 97bddbb | L8 tick-266 split closed; V9 50/50 rounds fail only on gap F |
| Sector placement S7: V7/V8/V9 in SimTool (id layout, surfaces, a pass inside a savestate window) | 3242755 | V7, V8 PASS; V9 PASS except gap F (after RO1) |
| Replay order RO2: the user's measurement of RO1's uncovered cases | 3ba04cb | no value change over 300 settling ticks; one relative order on every host; found item 4's P |
| Resync lag RL1b: the re-anchor rule before the schedule start, the 3-tick legacy floor, one start helper | 2ff5c6b | 538 tests; one live resync_snapoff, journal 3508/3508 |
| Trafo T4 residuals (step TR4c): the retail checkpoint read at 53-bit x87; the reference map's operation outputs by place | 6e14a9a, f379504 | pieces 32/32, 5056/5056 rows bit-equal to the server incl. rotation; L11 digest `017f037d7cfbc089/493` in retail and headless |
| Sector placement S8: the networked invariant in `--replay-session`; measured | d0c7b28 | invariant PASS on L8 (35916 lines) and L11 (46314), the option-off control fails; L11 cross at 150 ms, five sides 53 copies `5d22d3b5a61c38cb`, 54/54 |
| Host unification HU1: the placement's host logic and three other host copies as shared helpers (the rule "host logic is written once", §2) | 48d1c32 | byte-identical: wire check ALL SAME, server journals 2993/2993 and 4153/4153 |
| Sector placement S10: the retail core host, `CKContext::CopyObjects`, the legacy path's local end, `physics.placement_clones` on by default | fa8218d | two-copy L11 run, two sessions with the reload: the server's hashes and signature, 53 copies, every placement body exact by name |
| Core default CD1 (the user's decision, §2): the retail session core on by default | the `retail:` commit after 832f1a9 | two-copy L11 run, neither client sending `session core`: both core=on, both assigned in 10.6 s, 53 copies and signature `5d22d3b5a61c38cb` on both and the server, sector +2 at 103, 23/23 mechanisms, no rebase or resync |
| Late-player step 12 (REACH): the per-client reach from the relay lag, history 128 above 48; open question 1 measured | c1563b6 | x64: a 48-deep rewind frame p99 21.4 ms, 120 deep 50.9 ms; found the margin did not cover the server's wait |
| Late-player step R12 (the user's two decisions): the server's wait in the relay header (protocol 13 / journal v7) and the cap from the client's own rewind cost; `core_ref_s7` re-captured | the `session:` commit after c1563b6 | reach = clamp(p99 + 8 + w, 48, cap): w 22-23 on the wire, reach 52-53 (73 after a stall's burst) at 150 ms; cap 85-94 on retail x86 (0.64 ms restore, 0.48 ms a tick), 105-120 on x64; the cap's samples are captured clock reads (a capture replay reproduces it: 12/12 replay caps equal the live ones) |
| **Sector placement S1-S11, the track** (`sector-placement-design.md` §12 is the consolidated record): one object per PH row at the anchor in every world, the level-walk gate, the retail level reload; S11 the records | cd501af, 5b536b2, 81c6803, 72108fd, 5cd1c11, bc88b8e, 0d1178b, 3242755, d0c7b28, 48d1c32, fa8218d; S11 the `docs:` commit after fa8218d | built; the user's rule "entering the next sector changes nothing in the previous one" holds on L8 and L11 by the invariant check, offline and networked |
| Rollback engine gaps G0 (EG0): diagnostics - the counted birth failures, the 1378 difference, the outside producers | 0b73af2 | built |
| Record-format gaps K0: SimTool `--array-reads`, `--globals-trace`, `--world-options` | 49e2cc8 | built |
| Rewind cost RC1: the replay re-predicts a remote's tail past its newest relayed frame with that frame (hold-last) | 5f967b4 | input rewinds -83..-87 % (swing, H1 at 150 ms), -67..-77 % (cliff), -44 % at 50 ms; no rebase in any arm; CPU parity -36 % |
| Resync lag RL1: a re-anchor's base on the fixed deadline schedule (server only) | 3daa19a | the other clients' relay lag after a resync 36-37 -> 20-21 ticks (resync_snapoff); resync_at150 c0 26.77 -> 22.3 |
| Trafo T3 (step TR3b): the core and the headless host, merged onto RC1 and RL1 | 3394ffd | H1 at 150 ms, snapshots off: pieces 150/150 bit-equal on four clients, starved 0/0/0/0 against the control's 0/0/0/0 |
| Trafo T4 (step TR4): the retail host (core on), bridge v22 | 3940bf3 | one two-copy L11 session: pieces 32/32 bit-equal across the clients (5056 rows), drawn on both |
| Trafo T5 (step TR5): the records | 168e53e | `engine-changes.md` bridge note v21/v22, core design "As built (step T)", 9.29 |

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
x64 teardown) are item 2. From SP11 on, the sector placement is in §3 and
item 1 keeps only what is left of it (host unification's queue); the
numbers 1-10 are unchanged.

**Stage A - everything that affects physics is an input (goal 2).**

1. **Sector placement: built (S1-S11, moved to §3 at SP11); what is left
   of it here is host unification's queue.** The track's record is
   `sector-placement-design.md` §12 (commits, the code as built, the
   signatures, §9's answers, the open points). Its open points are queued
   elsewhere: the x64 teardown over-read (item 2), the residuals its
   crosses showed (G-c, item 5; gap L, item 4; gap F, item 2), Level 1
   (K3, item 6), and the checks the quick tier deferred, among them SP8 (g)
   c2's ball death at 736 (item 7's list, SP5-SP10 and HU1). Closed on the
   way: **adaptation M3** (the config default off from S5 to S10, on
   since); S2's open points (the plan bound 63 at S5, the scene refused in
   `apply` at S6); **the SP6 bridge trigger** (the original s2
   `P_Modul_29` woken in the leaders' worlds only: G-c - the module's own
   80 m proximity probe, `.local-tools/work/SP6/bridge/`, recorded at RO1;
   the placement-on worlds are the retail-faithful ones); **one placement
   host helper** (done at HU1, the release helper at SP10).
   - **Host unification (step HU1, 2026-10-27; the rule in §2).** The
     inventory of host-side logic written out in two or three of
     `physics_world.cpp` (PW), `session_client.cpp` (SC), `core_host.cpp`
     (CH) and SimTool, at d0c7b28:
     - *Unified at HU1* (behaviour-neutral: the wire check `rSP8 -> rHU1`
       on `core_ref_s6` ALL SAME, sends and journals; the server journals
       of `l8_cross` and `resync_snapoff` replayed matched with stdout
       byte-identical to rSP8's):
       (1) the placement's host logic (above);
       (2) the script replay's force hooks, `game/host_backend.hpp`
       `force_hooks_for(backend)`: PW's anchor (capture only before; the
       restore half is now set and never called: the server only captures),
       SC's anchor, `sim/trafo_hooks.hpp` (the headless trafo sets'
       `forces`) and five SimTool modes;
       (3) the level file's counters, `game/script_state.hpp`
       `capture_level_file_counters` / `restore_level_file_counters`: PW's
       load loop and anchor, SC's `load_step` and anchor (SC's proximity
       capture now runs before `capture_trafo_reference` instead of after
       it; both only read);
       (4) the sector union pass's writes, `game/sector_activation.hpp`
       `run_sector_pass`: PW `update_sectors`, SC `start_sector`.
     - *Already one copy* (nothing to unify): RO1's live script order
       (`session/script_order.hpp` + `script_replay::live_frame_order`; each
       host's call is one line beside its own trace), the script selection,
       activations and explosion pieces (`game/session_host_support.hpp`),
       the checkpoint hold (`hold_checkpoint_activation`), the headless
       hosts' trafo hooks (`sim/trafo_hooks.hpp`).
     - *Queued, retail* - **done at SP10**: CH's two force-hook copies
       (`make_retail_trafo_hooks`'s `forces`, the anchor's
       `host.script_hooks`) are `force_hooks_for(&physics_view_)`; CH's
       `run_sector` calls `run_sector_pass` (it keeps its own
       `sector_cell_is_own_` bookkeeping, set before the call).
     - *Queued, not behaviour-neutral or not one logic yet*: (a) the trafo
       set hooks, `sim/trafo_hooks.hpp` against CH's
       `make_retail_trafo_hooks`: `has_body` differs (the engine's
       `GetPhysicsObject` lookup against the bridge's `has_body`) and CH
       leaves `restore_originals` empty by design - one
       `trafo_hooks_for(backend)` needs `physics_direct` and
       `physics_view` to agree on those methods and a retail trafo run
       (with S10 or R6); (b) the retail level-file counters glue
       (`BallanceMMOClient::capture_level_sequencers` /
       `proximity_countdowns_restore`): shared with the legacy path, which
       gets nothing new, and it logs a census the headless hosts do not -
       onto the helper at R7 when the legacy path goes; (c) the placement's
       release - **done at SP10**: `game/sector_placement.hpp`
       `release_placement(placement, context, why, log)`, called by PW's
       destructor (no log), SC's `release_placement` and CH's
       `core_host_release_placement` (level exit, map load); (d) the anchor sequence itself
       (clock reset, counters, placement, hash, collision filter, rewiring
       or N1-N4, selection) is written per host in a different order per
       host - one recipe would change what a world does: Stage C (R6 / R7);
       (e) N2 on every world and (f) the core's 30-unmatched-snapshots rule
       (item 2): each changes what a world does and needs its own check.
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
     the server at 20485. **Built**: REACH (c1563b6, the per-client reach)
     and R12 (the user's decisions of 2026-10-27: the margin is 8 + the
     server's wait, which every relay header carries since protocol 13,
     and the cap is estimated from the client's own rewind cost with a
     3-tick budget; late-player design §2.5 "As built (step R12)", §7
     question 1 resolved). Still open: why the trial's server stalled
     (646 / 595 ms between steps), which no reach covers past 120.
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
     live tick + 1 (T4 fixed it with the inputs on only).  **(a) and (b)
     done at TR4c** (trafo design, As built (step TR4c)): the journal
     checkpoint's `list_bodies` read at 53-bit x87 in the retail core host
     (pieces 32/32, 5056/5056 rows bit-equal to the server including
     rotation on both clients), the map's operation outputs by place
     (Level 11 digest `017f037d7cfbc089/493` in retail and headless; every
     earlier digest changed).  (c) stays open.
   - **RL1b** (`.local-tools/work/CHAIN3/queue.md`, the redone review of
     RL1, no blocker): the As-built note's re-step depths (12 -> 28 on
     resync_snapoff, 34 -> 29 at 150 ms, and why), the held window, the late
     joiner off the per-player path, the base-missed fallback limited to
     `CAP_RESYNC_STATE` members; `tick_at` before `start_`, the floor for
     members without `CAP_RESYNC_STATE`, stale comments (late_tick.hpp:3-4,
     server.cpp:1289, :1734, :1791-1792); a runner-level test that both
     start paths store the schedule. One small step once nothing else is
     working in `server.cpp`. Built at step RL1b (late-player design §2.8,
     "As built (step RL1b)"): the docs, the signed schedule tick, the
     3-tick legacy floor, the stale comments, one start helper; no
     world-less runner fixture exists, so the runner-level test is a test
     of the pure rule the runner calls (`ReanchorOnSchedule`).
   - **Swept at B2SW (2026-10-27; the user: "感觉这些零散发现全都可以一起一次扫掉")**,
     one batch, each item's record in its design's "As built (step B2SW)":
     - *Resync T2*: closed without code - a forced resync of the
       trafoing client inside its pieces' life and TR3b's H3b both leave
       the pieces bit-equal to the server's after the apply (50/50, 50/50,
       33/33); with the option on the originals are never selected, with it
       off the server has no trafo (resync design).
     - *G1*: fixed - the script restore re-registers what it writes back
       active (`script_replay` `live_membership`, the core hosts' bindings
       and SimTool); the harness's own re-registration is now a count.
     - *The 30-unmatched rule*: in the core for both hosts, counted per
       snapshot (adapted: the host's copy reset every frame and could not
       fire), the session's (zeroed at `begin`); never fired live, the
       snapshots-on arm included (`b2sw_u30on`, 16 client runs) (core
       design).
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
   ball has no body (design §9.7, decided at G8). P the parented entity's
   local matrix (found at RO2, lockstep-without-snapshots.md "As built
   (step RO2)"): `restorer::physics_rt` (`physics_savestate_restore.cpp`)
   writes each body's recorded world matrix with `SetWorldMatrix`, and for
   a parented body CK re-derives the local matrix with rounding even when
   the world is bit-identical (`P_Modul_25_Bridge001` local y
   `-0x1.f55394p-3 -> -0x1.f554p-3`); a replayed pass that then moves the
   parent leaves the sleeping child's CK matrix ulps from a world that did
   not roll back (7.2e-07 m L8, 6.1e-05 m L11; the IVP core equal; a wake
   overwrites it). Options: skip the write when the live world is already
   bit-identical (measured: the three bridges then bit-equal; a restore
   path change, so the exact checks rerun), or record the local matrix of
   a parented body and restore it with `SetLocalMatrix` (a blob format
   change).
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
     server-only change, the x86 build is unchanged). Added at RL1b: (a)
     also reads the joiner's relay lag off the per-player path (up to about
     4 above the start members', `late_tick_lead_ticks` is not clamped);
     (b) also measures the resynced member's own correction in a non-stall
     resync while the server waits or lags (the held window lead + D - e);
     (c) also a legacy (core off) member's floor + 3; (d) logs the re-step
     depth and its apply time; and (e) a late joiner in the first
     start_base periods after the all-ready moment (the signed schedule
     tick).
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
   - SP5 (sector placement S5, the wire, quick tier, 2026-10-27; ran: x64,
     x86 and Arch builds, 530 tests, GCC 29 ok, a v5 arena server journal
     replayed matched 4112/4112 with stdout identical to rTR5's, ONE live
     two-client run of §7.4 "mismatch" (`sp5_mismatch`), its v6 journal
     replayed matched and refused when its signature is altered, a
     protocol-11 capture refused): (a) the wire check - impossible until
     S6 re-captures the reference set at protocol 12; (b) a retail two-copy
     run of the SP5 x86 build, core off and core on (no retail source
     changed, but SessionReady's bytes and the journal header changed for
     both modes; the core-off BAR rests on it); (c) a session with
     `physics.placement_clones` on: a member without `CAP_PLACEMENT` (the
     legacy path, the retail core host) refused out of the session, the
     rest running; (d) SimTool `--replay` (record mode) with its new
     default on, on a retail tick record of a pooled level made with and
     without the copies; (e) a `--replay-session` of a CLIENT journal of a
     trafo-on session (SP5 writes `world_options` into the client header, so
     it replays with the trafo inputs: does the client journal hold every
     Trafo event, the server's `TRAFO_SERVER` resets included?); (f) a
     placement mismatch of a member at session START (only the late joiner
     ran; same server branch).
   - SP6 (sector placement S6, the headless host, quick tier, 2026-10-27;
     ran: x64, x86 and Arch builds, 530 tests, GCC 29 ok, ONE live L8 cross
     at 0 ms with the server's placement on (signatures, level walk and
     sector ticks equal on all five sides, copies journalled, server
     journal matched; non-leader divergence on the original s2 bridge and
     Pusher = the queued G-c wake, the Pusher half reproduced bit for bit
     with the placement off, the bridge's trigger queued above as "SP6
     bridge trigger"; at the review round `l8_cross` re-captured with client
     2's second `--beam` made live, plus four option-off control runs in
     `maps/arena-test/sp6_ctl`), ONE arena regression (`resync_snapoff`, 150
     ms, snapshots off; vs rSP5 only SessionReady and the journal header
     differ), the minimal re-capture `core_ref_s6` (three arms + solo pair,
     wire check ALL SAME, solo IDENTICAL to `core_ref_rc1`'s): (a) §7.4
     "L8 late" (a late joiner after c1/c2 are in s2/s3: walk before its
     owed passes, signature equal, `mechanisms=K/K`); (b) the re-capture at
     protocol 12 of the rest of `core_ref_rc1`'s arms (`swing_snapon`,
     `a6m_snapoff`, `a5u_lead`, `resync_at150`, `resync_at150_down`,
     `h1_150`, `cliff`) and of `core_ref_t3`'s option-on arms; SP5's (a)
     wire check is now possible and was run on the new set; (c) the
     headless host's option-off release branch (a level still holding
     copies with the option off) and the scene refusal (an anchor outside
     the level scene) never ran live: no run reaches them (S10 territory).
   - SP7 (sector placement S7, SimTool only, quick tier, 2026-10-27; ran:
     x64 build, 530 tests, GCC 29 ok, V7 L8/L11 four arms, V8, V9 three
     arms plus two option-off/no-pass controls, the forced-rollback replays
     of `core_ref_s6/l8_cross` and one control of `sp6_ctl/l8_cross_off`;
     V9 passed except gap F once step RO1 fixed the replay order, the fixed
     births and the strings; the forced-rollback arm fails on gap L, see the
     design's As built (step SP7)): (a) V11 for V7's hashes (x86 `build/sim-x86-a3` and the
     Arch SimTool: the step's code was not built there); (b) V9 on L11 and a
     window without the retail-ball park once Q is built; (c) the forced
     rollbacks at 150 ms (§7.4 recipes are 0 ms here); (d) the Arch build
     of `physics_world.cpp` (compiled into the Linux server through
     `BallanceMMOSim`; `linux_test_syntax.sh` does not compile it) - ran
     since (noted at SP11): the Arch server builds of RL1b, TR4c and SP10
     (each ARCH-BUILD-OK, all after 3242755) compile it with S7's hooks.
   - RO1 (the replay order, the window-born fixed bodies, the size-changed
     strings; shared rollback code, quick tier, 2026-10-27; ran: x64 and x86
     builds, 534 tests, GCC 29 ok, the wire check rSP6 -> rRO1 on
     `core_ref_s6` (sends ALL SAME; journals identical except
     `resync_snapoff`'s four, order-only from tick 1778), solo1/solo2
     IDENTICAL to their live journals, V9 move (50/50 gap F only) and
     no-move (PASS 50/50)): (a) a retail two-copy session crossing a sector
     pass with a rollback over it, core on (the retail host records its
     live order through the SDK's public `m_BeObjects`; built,
     CLIENT-BUILD-OK, not run); (b) V9 on L11; (c) the V9
     `--free-ids-before-load 20000` arm (crashes at teardown: item 6's
     mechanism, root-caused at SP7's fix-up); (d) RO2, the user's
     measurement step: (A) an activation whose first execution is inside a
     replay and (B) the server and a retail client running one frame live
     in different orders - whether either changes numbers (switch to one
     canonical order on every host only if one does); (e) a core-off retail
     run (the legacy path gets none of RO1: its binding passes no live
     order and leaves `resize_strings` off, but the shared headers it
     compiles changed); (f) a GCC/Linux compile of `script_replay.hpp`'s
     CK-side code (`linux_test_syntax.sh` does not cover it); (g) a resync
     capture with a fixed body born inside its window (the review fix-up
     keeps the deferral under a travelling blob and after a failed script
     restore; gate round: 534 tests, both builds, the wire check and V9
     unchanged).
   - RO2 (the user's measurement of RO1's two uncovered cases, quick tier,
     2026-10-27; lockstep-without-snapshots.md "As built (step RO2)"):
     (A) a late activation (the pass first executed inside a replay, the
     fallback order): no body value change over 300 settling ticks on L8
     K=2/3/4, L11 K=2/3 and the arena K=4; the structure differs for good
     (the overlap tree, unit numbering, movable / hull / collision order;
     gone with the control's live order injected); contact through the
     late pieces untested (deferred (b)); (B) the server and the four
     headless clients of `core_ref_s6/l8_cross` run every common script in
     one relative order (0 of 17 change ticks differ). RO1's host-local
     order stays. Found (pre-existing, rRO1 alike, not the order,
     root-caused at the review fix-up): a sleeping parented body's CK
     matrix after a replay that moves its parent, the IVP core identical -
     `P_Modul_25_Bridge001` 7.2e-07 m (L8 K=3), `P_Modul_25_Bridge` /
     `P_Modul_37_Bridge` 6.1e-05 m (L11 K=3); the restore's
     `SetWorldMatrix` re-derives the local matrix with rounding (item 4's
     P). Ran: x64 build, 534 tests, GCC 29 ok,
     the wire check `rRO1 -> rRO2` (ALL SAME, 12 journals IDENTICAL), the
     trace-on `l8_cross` replays IDENTICAL to the live journals, the Arch
     server build with `physics_world.cpp`'s trace (review fix-up,
     ARCH-BUILD-OK). Deferred: (a) the retail half of (B) (the real
     CK2.dll's live order against the server's in a two-copy session
     crossing a pass); (b) (A) with a ball moving through the late pass's
     pieces after the replay (the structure that differs decides the
     collision order then).
   - TR4c (trafo T4's residuals (a) and (b), quick tier, 2026-10-27; ran:
     x64, x86 and Arch builds, 538 tests, GCC 29 ok, the wire check rRL1b
     -> rTR4c on `core_ref_s6` ALL SAME, SimTool L11 `--describe-trafo` on
     both builds (only the 6 lines per set expected), ONE two-copy retail L11
     session, core on, `trafo_inputs` on, snapshots off: pieces 32/32 and
     5056/5056 rows bit-equal to the server incl. rotation; trafo design As
     built (step TR4c)): nothing deferred of its own (recorded at SP11; the
     legacy path is untouched, so no core-off run is owed beyond TR4's (d));
     T4's (c) stays item 2.
   - SP8 (sector placement S8, measurement, quick tier, 2026-10-27; a
     SimTool-only tool change: `--replay-session` with `--placement-trace`,
     `--placement-trace-player` and `--drop-sectors-above`; ran: x64 build,
     538 tests, the networked invariant on `core_ref_s6/l8_cross` and on
     the new `sp8/l11_cross` (both PASS, traces byte-identical; the
     placement-off `sp6_ctl/l8_cross_off` fails from 313 as it must), ONE
     live L11 cross at 150 ms snapshots off (signatures, walk and sector
     ticks equal on all five sides, 54/54, journal matched; residuals = G-c
     and remote-ball contact on copies and originals alike), §7.6 SimTool
     x64 on/off on L8-L12; design As built (step SP8)): (a) §7.4 L8 cross
     at 150 ms and its networked invariant; (b) the L11 cross's
     placement-off control at 150 ms, for the per-body "no worse than the
     control" judgment; (c) the L11 cross at 0 ms with a ball on the s3
     sack copy `P_Modul_26_*_R35` (c2's ball died before its s3 beam); (d)
     §7.6 headless on vs off (L8 and an L9/L10/L12 cross, `t_rest` /
     `t_rep` / `t_cap`, rebases, resyncs, the p99 frame); (e) §7.6 SimTool
     x86 (`build/sim-x86-a3` predates S3's switches: reconfigure) and
     retail x86 (S10, with the anchor cost: +1.0-2.2 s per anchor on x64
     SimTool); (f) the GCC check of `sim_tool.cpp` (not in
     `linux_test_syntax.sh`'s list); (g) c2's ball death at 736 in
     `sp8/l11_cross`, the tick after its own `sector +3` at 735, not
     root-caused: which entity deleted the ball (a `DepthTest` volume
     of s3 or of a copy = the next sector changing the previous one, or
     the fall through the copy `P_Modul_37_Bridge_R30` from 581), by
     the server journal replayed with and without the s3 Sector event
     (design As built (step SP8)); with (c).
   - HU1 (host unification, a refactor, quick tier, 2026-10-27; ran: x64,
     x86 (`core_host.cpp` with the explicit instantiations) and Arch server
     builds, 538 tests, GCC 29 ok, the wire check `rSP8 -> rHU1` on
     `core_ref_s6` ALL SAME (12 send traces SAME, 12 journals IDENTICAL),
     the server journals of `l8_cross` (2993/2993) and `resync_snapoff`
     (4153/4153) replayed matched with stdout byte-identical to rSP8's, the
     server world's new option-off check on L1-L13 (`--list-placement`,
     every anchor reached, no level holds copies); no live run - a
     byte-identical refactor): (a) a retail two-copy run of the HU1 x86
     build, core off and on (no retail behaviour changed: the new
     `core_host.cpp` lines are explicit instantiations nothing calls, but
     the shared headers it compiles changed); (b) the solo snapshots-off
     exactness replay (no rollback or session code changed); (c) a live
     server session whose world holds copies with the option off (no run
     reaches `prepare_unplaced`'s release branch on the server: a fresh
     world never holds copies).
   - SP10 (sector placement S10, the retail core host, quick tier,
     2026-10-27; ran: x64, x86 and Arch builds, 538 tests, GCC 29 ok, the
     wire check `rHU1 -> rSP10` on `core_ref_s6` ALL SAME (12 send traces
     SAME, 12 journals IDENTICAL, `l8_cross` with the placement on),
     SimTool `--list-placement --placement on` on L8 and L11 identical to
     rHU1's but for the dropped "(remapped N)", ONE two-copy retail run on
     L11 with core on and the placement on, snapshots off, two sessions
     with the reload between them (`scripts/retail/sp10_run2`), design As
     built (step SP10)): (a) §7.5 runs 1 (on L8, with `level walk seen` and
     the `scripts` check of `P_Modul_30_MF Script@P_Modul_30_MF_R15`), 3
     (exit to the menu and a menu load, the release at the map load, and
     the builder copies' names after a leak check), 4 (a trafo copy:
     L8 row 19's paper trafo, with `trafo_inputs` on), 5 (a late retail
     joiner, `sector manager` trace) and 6 (the legacy path, `session core
     off`, with the placement on: the local end and the server's
     immediate "no CAP_PLACEMENT" refusal of that member, the others
     starting without the 8 s barrier; and off: run 1 as today), and the
     retail option-off release path (`prepare_unplaced` releasing copies a
     level still holds: a session with the option off on a level that ran
     one with it on, `session reload off`), never exercised on retail;
     (b) §7.6 retail x86 (run 1 on L9 or L12 at `net lag 150`, the
     cost lines; the anchor's apply took 1.43-1.45 s on retail x86 for
     L11's 53 copies); (c) the headless A/B arms with the default now on;
     (d) CPU parity; (e) three launches without a hang; (f) a retail
     session core off with the placement off (the legacy BAR: its only
     change is the local end on the option, which does not fire); (g) the
     review's nit on step 8: an object-valued attribute of a hierarchy
     member naming an object outside the hierarchy (expected none; the
     remap covers H only since SP10), counted in the listing.
   - SP11 (sector placement S11, the records, docs only, 2026-10-27; its
     row's gate is a review; `sector-placement-design.md` §12, the
     lockstep doc's "What the union broke on the original levels, and the
     placement", the core design's host support table and "As built (step
     S)", the protocol doc, `collision-overhaul-design.md` 9.30, this
     roadmap): nothing deferred.
   - REACH + R12 (late-player step 12 and the user's two decisions,
     2026-10-27, quick tier; late-player design §2.5 "As built (step
     R12)"): (a) a live notice in the (48, reach] band - no harness knob
     stalls the server or the downlink while the client steps on (the
     unit tests cover it); (b) the trafo-inputs-on cost and cap (a
     `trafo_inputs` session's estimator line); (c) a live run with
     patience on (the wait's patience branch is unit-tested only); (d)
     core_ref_s7's other arms (`core_ref_s6` README's "left for the final
     verification" list, re-captured at protocol 13); (e) a retail run
     with `session core off` (the legacy path parses the new header byte,
     uses nothing of it). Found at R12, queued here (item 2): the cap's
     estimator over-states the per-tick cost of shallow rewinds that make
     bodies (`core_ref_s7/l8_cross`: 2.6-3.3 ms "a tick", cap 48); whether
     the births' share leaves the replay stage is a design choice. Also
     for the user (R12 review): the cap before a client's first rewind is
     48, so a first stall deeper than 48 before any rewind still resyncs;
     a different starting value is the user's to choose. The cap's samples
     are captured clock reads since the review round, so a capture replay
     reproduces the cap exactly; (f) a retail run with the review round's
     x86 build (its retail behaviour is unchanged: retail's `now` is its
     clock).

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
