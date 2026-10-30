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
- **Collectibles are per-world like the airship** (the user, 2026-10-29,
  verbatim:
  "如果PH*4是管加分球的，那么它应该和飞船一样不纳入回滚/物理同步，每个玩家有自己独立的一份。（同理，生命球也是）" -
  extra points and extra lives stay out
  of the rollback and the physics sync, each player has their own copy):
  the `P_Extra_*` scripts are outside the replay selection, and their
  `PH[*][4]` column is neither recorded nor rolled back - a census class
  "per-world collectible state" with a load-time writer check (answer to
  RK2's options, verbatim "选：加检查" = option (b); built by FULL step F4).
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
  selects the legacy path for the next session until R7 deletes it, and
  every server refuses that session (next point).
- **Clients without the session core are refused** (the user, 2026-10-28,
  answer 7 of the seven quoted in §4 item 3, verbatim: "直接拒绝，不要继续兼容
  非session core的客户端" - refuse it outright, stop keeping clients without
  the session core compatible). The server refuses a physics session to a
  member whose SessionReady does not come from the session core, whatever
  the session's world options (step B9DX, item LGX; the core design's "As
  built (step B9DX)"); the legacy code still compiles until R7 deletes it,
  but no check, run or deferred item is spent on `session core off` any
  more.
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
| World agreement G0 (WG0): SimTool `--probe-census`, the read-only proximity census | the `sim:` commit after dba4d3d | V1 PASS on L1-L13, placement off and on: 290 mechanism blocks, 58 fans + 13 bridges (132 units with the placement), every count as design §1.1.2 |
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
   - **SimTool's re-made live world skips the Level 1 tutorial's freeze**
     (step RK3, a harness gap, not a client gap): `--savestate-check`'s
     re-made live world re-runs the straight pass's frames on a
     Gameplay_Tutorial it never rewinds (the exit already ran), so a round
     whose t0 is before the freeze (frames 9..15) and whose e is after it
     fails its premise (L1 `--drop-settle 3 --session-recipe`, seed 2:
     rounds e = 6 chained and e = 16, named round 9:7 too). Fix: the
     re-make puts each re-run frame's recorded globals (C3), or the harness
     rewinds Gameplay_Tutorial with the selection. **Moot since step RK3H**
     (the frame-9 freeze is held, no world freezes): the same command, seed
     2, passes 40/40 (seed 1 40/40); the harness gap stays for any other
     script-driven globals write a re-made world re-runs, none known.
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
     - *Step P's race*: re-measured snapshots off - both clients in state B,
       every body exact from 218; moot since R3; closed (core design).
     - *Q5 / A23*: a harness gap (the headless client exact through the
       start window with a depth-4 rewind every tick); SimTool's
       `--session-recipe` closes both A23 kinds; a depth-0
       "Deactivate Sector outputs" difference at anchors 4-5 was left open
       here (closed at B2SX, below); the rest of the start window is A18
       (C3, item 6) and gap Q (item 4).
     - *The x64 teardown over-read*: engine change #21 (CK2); V9's
       `--free-ids-before-load 20000` arm and L6-L9 of a 20000-id sweep no
       longer crash, tick hashes identical (sector-placement design 12.6).
     - *N2 on every world*: `game/sector_deactivate_pin.hpp`, the retail
       mod's copy a use of it; the A4 race reproduced offline and fixed.
       **Found, queued here**: the chain's [0][1] Set Cell in the frame of
       a pass of another sector activates the chain's sector instead (every
       world, the retail mod included; options: a private cell for the
       chain's [0][1] with the own poll reading it, as the phase cell's
       split, or another source for the own sector poll - the user's;
       built at B2SX, below).
     - *F*: fixed - the replay processes next-frame flags only where the
       live frame did (the recorded live order); V9 0 FAIL lines,
       `--late-sector-check` exit 0. *The CPU cliff*: closed - the recipe
       (with `--drive`) rebases nowhere, t_max 29-30 ms.
     - *Resync T1's findings*: (b) fixed in `run_replay_session` (A/B:
       1560/1560 ticks identical before S, 0 before); (c) established, by
       design (the resync's held window sends no notices); **(a) the
       user's choice** (an echo now, a protocol change G5 would retire, or
       wait for world agreement's Q9; the user chose the wait at B2SX,
       below).
     - *Trafo T4 (c)*: the retail birth log's replayed-tick rule in every
       session (trafo design).
     - *The trial's server stall*: established - the journal's flush to
       `/mnt/d` through WSL's 9P (writes up to 523 ms measured, ext4 0.7 ms)
       holds a step, and the catch-up loop then substitutes up to 10 queued
       frames per member; **the fix is the user's choice** (late-player
       design, "As built (step B2SW)"; fixed at B2SX, below). The harness's server stall
       (`BMMO_SESSION_STALL`) is built; a 56-deep notice was taken by a
       56-tick rewind live, no resync.
     - Deferred to the final verification: a same-recipe retail run of the
       previous build for T4 (c)'s contrast; P's instrumented pair (moot).
       [A retail run with the session core off was removed at B9DX: no
       core-off check is run any more, §2.]
   - **Swept at B2SX (2026-10-27; the user: "好的按你推荐的做，并且明确记录下来CK2有哪些改动（和之前一样）")**,
     B2SW's leftovers, one batch (the CK2 part - engine change #21 in the
     Swung0x48/CK2 fork, `docs/engine-changes.md` - was the orchestrator's,
     8a0b7cc):
     - *Q5 / A23*: **closed** - the depth-0 residual was the restore's
       re-armed delayed link (`Deactivate Sector.out0 -> Set Cell.in0`,
       in flight at 3-5: the restore turns its source output back on, a
       depth-0 compare sees it before any frame); the harness's depth-0
       compare ignores exactly those bits (option (a)); a restore that
       re-arms while the live world has the link in flight fires it once,
       at 6 (record-format design, "As built (step B2SX)"). What remains of
       the start window: A18 (C3, item 6), gap Q (item 4).
     - *The trial's server stall*: **fixed** - the local test server's
       journal on WSL's ext4, copied back as each session's journal closes
       (`.local-tools/arch_server_lp.sh`, `arch_journal_mirror.sh`,
       `arch_stop.sh`; option (a), config only), and the catch-up judges
       each frame by its arrival against its own tick's deadline (option
       (b); `input_buffer::take`'s cutoff, the queue drained before every
       step): no substitution after a 560 or 660 ms stall (B2SW: 8 per
       member) (late-player design, "As built (step B2SX)").
     - *The [0][1] cell*: **fixed** on every world - the checkpoint
       chain's [0][1] Set Cell writes a private copy, which the own sector
       poll reads (`game/sector_cell_split.hpp`, option (a)); the B2SW
       scene makes sector 3's bodies at T 79-81 (resync design, "As built
       (step B2SX)").
     - *T1 (a)*: **waits for world agreement's Q9** (the user's option
       (ii)); the cause, the server's relay skipping the owner for
       BodyRevived, is recorded in the resync design's B2SX note.
     - Deferred to the final verification: [a retail two-copy run with the
       private [0][1] cell (core on): run at step RTX, `scripts/retail/rtx_1`
       - client 1's own crossing stamped 828 (poll 819 + lead 9), the pass
       at 828 on both clients and the server, client 2's crossing into the
       started sector stamped 1028 (resync design, B2SX note)]; a retail
       follow check of Q5's re-armed link (the "fires once" result is
       Ballanced CK2's, the retail Virtools `CK2.dll` was not checked).
   - **Session I/O on the tick thread** (queued by step SMK, 2026-10-28):
     every write of a session is synchronous on the tick / game thread -
     the journal flushes after every player, event, checkpoint, note,
     correction and resync record (`session/journal.hpp:817-843`, server,
     headless client and retail mod alike), the headless client
     `fflush`es its log after every line (`sim/session_client.cpp:692-707`)
     and writes its capture there. WG1's smoke (949f960) met a 1-2 s
     write stall of the host's D: drive (NTFS event 170) and every client
     and the server stalled with it: rebases, resyncs, starvation past the
     48-tick reach; on retail the same stall is a game freeze. Fix: a
     buffered writer thread for the journal, the log and the capture (or
     at least asynchronous flushes), so a storage stall delays the files,
     not the tick; a crash then loses the writer's queue, which the
     journal's flush-per-event rule was written to bound (choose the
     bound when it is built). Evidence and numbers:
     `world-agreement-gaps-design.md`, "As built (step SMK)". **Built by
     step SIO (2026-10-29)**: one writer (`session/io_queue.hpp`) on an asio
     strand of the host's pool (retail `thread_pool_`, the server's and the
     headless client's own one-thread pool), no proactive flush, bounded
     queues, crash drains; the crash-loss bound and the checks in
     `world-agreement-gaps-design.md`, "As built (step SIO)". **Flush
     option built by step IOF (2026-10-29)**, the user's answer to SIO's
     pending question: a per-stream mode none / batch / record (log,
     journal, capture), default batch, always on the writer strand, never
     on the tick thread (headless `--io-flush`, server `config.yml`
     `session_io.journal`, retail `Gameplay/SessionJournalFlush`);
     `arena_map_test.py`'s `LogWatch` sees each line within one batch
     again; `world-agreement-gaps-design.md`, "As built (step IOF)".
   - **relay_late counts frames the resync state covers** (queued by step
     WGQ7, 2026-10-29): `note_input_correction`
     (`client_core_messages.cpp:464-467`) counts a relayed frame "past the
     rollback reach" when its anchor tick is not in the history; after a
     rejoin or late join the history starts at the resync anchor S (written
     at the assignment, `client_core_resync.cpp:64`), so a frame for a tick
     <= S first handled after the assignment is counted, though the
     server's state of S already contains it. WGQ7's smoke failed on it
     (client 2, the rejoiner, 2 frames = one relay message after
     SessionAssign; own ball 0.0000 m, server journal matched); whether it
     shows is the order of the unreliable relay against SessionAssign. Fix:
     count such frames apart in the core, or exempt them in
     `arena_map_test.py`'s criterion. Evidence:
     `world-agreement-gaps-design.md`, "As built (step WGQ7)". **Built by
     step RLT (2026-10-29)**, the user's choice (a): a frame for a tick at
     or before the anchor of the exact state the client stands on (pending
     or applied) is counted `relay_covered` (status `relay=.../covered`,
     window `covered=`), never `relay_late`; `session-client-core-design.md`,
     "As built (step RLT)".
   - **A delayed rejoiner's first state never finds its bodies** (queued
     by step TRL, 2026-10-29; cause NOT established). Run B
     (`maps/arena-test/trl_live_B`, client 1, `--rejoin-at 1200:1000`):
     the join state of S 1296 waited from tick 1306 for 20 bodies
     (`P_Ball_Paper_MF` first; only the first is logged) and never applied:
     the pacer brought sectors 2 and 3 in at 1306 / 1313 (no member had
     entered sector 4 before 1814), yet the wait ran until the resync hold
     timed out at 1391 and the client asked again (re-anchored at 1529,
     delta -137). The same wait resolved in 6 frames for run A's 0 ms
     rejoiner (14 bodies, same first name), 17 for run B's 5 s rejoiner
     and 21 for the late joiner, so it is particular to this rejoin; which
     of the 20 never came is not known. Consequences in that run: the
     second resync's numbering jump gave the server 157 substituted
     inputs of client 1 in B and the client 125 starved reports (0 at the
     status of 1346, 125 at 1680, worst 125: one episode), and 13 relayed
     frames counted past the reach (the relay_late item above). Next: rerun
     the case; step TRL's fix-up logs the whole waiting list once a wait
     reaches 60 frames (`client_core_resync.cpp`), which names the missing
     bodies. **Built by FULL step F2 (2026-10-29)**: the missing body was
     the ball of the 5 s rejoiner, which left at 1300 inside the wait; the
     dead-after-S path (resync design 2.6.3) required `birth_known`, which
     is false for every mirror a joiner makes live at base, so the mirror
     was neither present nor "dead after S". The path now takes any
     recorded birth; live f2_live_B / D (the death at S + 4 / S + 1): the
     mirror made from its row and its death applied again in the replay,
     every join state applied after 12-22 frames, holds 0, starved 0,
     relay_late 0; the pre-fix control f2_ctrl_B reproduced the stall
     (`resync-and-sector-timing-design.md`, "As built (step F2)").
3. **The rewind-cost optimization** (`rewind-cost-design.md`; RC1 built):
   RC2 an incremental capture for sleeping bodies (ceiling 22-24 %), RC3 no
   body-set full snapshot while snapshots are off (9-11 %, its own drift
   check), RC4 the dormant-script encoding (5 %; trafo design 9.4), RC5 an
   incremental restore (3-4 %). **RC2 is not built (the user's choice,
   2026-10-28, option (c) of step B5RC's stop: "按推荐继续")**: the blob
   has no per-body bytes (`capture_world` numbers checkers, contacts,
   units and overlap nodes world-wide at first encounter, so a sleeping
   body's record changes while it sleeps), IVP keeps no wake counter (an
   IVP change, not allowed for a cost cut) and the blob is on the wire; the
   options kept for later are (a) a per-object blob with name-keyed
   references (at most 3.2 of 4.6 us per sleeping body, a protocol bump)
   and (b) a cache of the rollback engine's per-body reads (at most
   1.3-1.4 us, no format change) - `rewind-cost-design.md` "As built (step
   B5RX)". **RC3 is built and on by default (step B5RX)**: a snapshots-off
   session takes a body-set full snapshot for its names and rows only (no
   compare, no rewind; headless `--body-set-corrections` is the control
   arm). The user, 2026-10-28, verbatim: "不对，RC3要保留快照？现在应该尽可能假设
   快照不存在来做" - this supersedes, for this work, the 2026-10-24 rule that
   the snapshot fallback stays until the known drift sources are fixed: the
   drifts those snapshots hid (B5RC: a Ball_Stone mirror 0.109 -> 21.3 m and
   an extra resync on resync_snapoff c1, remote-ball p99 5.0 -> 17.2 m on
   swing_snapoff c3, the P_Modul_29 plates 0.03 -> 1.4-1.8 m mean, worst 3.6
   m, on l8_cross) are gaps for step SFD to root-cause and fix. **Step SFD
   (`rewind-cost-design.md` "As built (step SFD)")**: (1) resync c1 first
   left the server at tick 296 through the headless own ball's spawn offset
   (a double sum against the event's float pose, 1 um; fixed in
   `session_client.cpp`: remote balls p99 14.8 -> 4.8 m, max 21.3 -> 5.3 m),
   then at 1238 through the own ball's IVP collision bookkeeping, which
   differs from every clone of it from birth on every own ball (hull value
   13-15 against 0.045, another overlap centre) while the rows are equal -
   a new cause whose fix is the user's choice (SFD's stop: build clones the
   owner's way, reset the owner's ball to a clone's at birth, or leave it);
   (2) swing c3 is the balloon (exception (c)): at 3111 the server's ball
   hits `PE_Balloon_Platform`, 0.9 m elsewhere in the client's world - the
   exception leaks into a ball that touches the airship (for the user);
   (3) l8_cross is G-c: c1's BodyRevived of P_Modul_29 at 554, applied by
   the server only (item 5). Two
   consequences (B5RX's review): the frozen-clock re-anchor now follows
   every core rewind (`rewind_world`), not only a snapshot's; and the
   snapshots-off multi-client arms of `core_ref_s7` replay to their end
   only in the control arm (`EXTRA=--body-set-corrections wire_check.sh`,
   which now fails a short replay) until a set is re-captured under the
   RC3 default (`core_ref_s8`, queued for the next step with live runs to
   spare). **RC4 is built (step B6RC)**: a trafo script at its set's built
   state is one byte of record v3, known without the walk from a ledger
   the trafo frame, the trafo inputs and the restores keep (a capture
   writes the byte exactly when the bytes equal the built ones; the two
   sets' part of a capture 37 -> 2.5 us in SimTool; no protocol change).
   See `rewind-cost-design.md` "As built (step B6RC)". **RC5 is not built**:
   stopped at B6RC for a choice - the restore tears down and rebuilds the
   whole world's collision state (the shared overlap tree, the hull
   min-lists in recorded order, world-wide numbers), so skipping a sleeping
   body is a selective teardown and knowing it unchanged needs an IVP wake
   counter or RC2's per-object blob (options (a) a selective teardown + the
   per-object blob, (b) only the non-collision writes of restore steps 3 /
   11 / 14, (c) leave it); the user left it to the orchestrator on
   2026-10-28 ("4. 你决定", quoted below), who chose (c), like RC2
   (`rewind-cost-design.md` "As built (step B6RC)", item RC5, and §11).
   The close-out is the trafo design's H4 / H6
   **with snapshots off** - the user, 2026-10-28, asked whether the
   close-out should follow RC3's snapshot-free default, verbatim: "改成关快照"
   (switch it to snapshots off); this replaces decision (b) of 2026-10-25
   (H4 / H6 with snapshots on) - against T3's snapshots-off numbers (TR3b's
   H1 at 150 ms: t_rep 0.99-1.15 s, t_max 43-46 ms, starved 0, against the
   option-off control's 0.34-0.41 s, 12-14 ms, 0). **On its pass
   `physics.trafo_inputs` turns on by default** (adaptation M2: it stayed off
   until then). **Step RCX ran it and stopped for the user**
   (`rewind-cost-design.md` "As built (step RCX)"): H4 with snapshots off
   (= H1 at 150 ms) passes - pieces 150/150 bit-equal on all four, 0
   resyncs / rebases / far / unmatched, starved 0; H6 fails its "control's
   band + 10 %" on every client figure (t_rep 0.68-0.93 s against 0.13-0.22
   s, t_cap 1.56-1.61 against 0.76-0.80 s, t_max 43-45 against 13-14 ms) and
   on the server's step p99 (3.1 ms against 0.70 ms in the explosions'
   window, within the 15.15 ms budget); most of the excess is trafos
   existing at all, not the option (TR3b's option-off arm, same trafos,
   is already 1.6-3x the no-trafo control), so the option-off default
   fails the literal H6 too; `physics.trafo_inputs` stays off until the
   user rules. **The user ruled on 2026-10-28** - the answers to the
   orchestrator's seven open decisions after RCX, verbatim: '1. 打开 2. 应该尽可能遵循原版脚本 3. 每个世界应该都有自己独立的飞船副本，且不纳入同步，每个客户端看到的飞船不同。4. 你决定 5. 你决定 6. b，应该都复制一份逻辑，同一个机关应该可以响应所有玩家 7. 直接拒绝，不要继续兼容非session core的客户端。 另外改动你可以提交，以后也可以自己提交'
   (1: RCX; 2: SFD (A), the own ball's IVP bookkeeping; 3: SFD (B), the
   airship; 4: RC5; 5: world agreement's Q9; 6: its Q7; 7: its Q8 and the
   legacy path; the last sentence lets the orchestrator commit and push).
   Answer 2 is built at step OWNB (`rewind-cost-design.md` "As built (step
   OWNB)"): the scripts make the own ball exactly as the server makes its
   clone (no move inside the creation frame); SFD's differing bookkeeping was
   the headless birth cell moved after the body was made (now before the
   block, as retail) and the parked own birth of a rewind window (now taken
   out and remade at its birth tick, its step kept; own body control only -
   retail keeps parking until R6, measured there, queued in the core design's
   R6 row); s7's 1238 seam crossing is unchanged to the
   bit and traced to G-c (item 5).
   Answer 3 (the airship, refined to "own ball only" with a ghost at the
   relayed pose) is built at step BALA (`rewind-cost-design.md` "As built
   (step BALA)"; protocol 14, bridge API 24): a positional airship zone in
   every world, another player's ball frozen there as a ghost and never
   touching any airship, the owner's pose relayed for display.
   **Queued, item ASL** (found by reading the code at step SPGH, not
   reproduced): BALA's `airship_zone_listener` (`physics_state.cpp`
   `event_PSI`) finds the own ball in `CKIpionManager::m_MovableObjects`,
   which drops a frozen core (`PhysicsObjectListener::event_object_frozen`,
   physics_RT `CKIpionManager.cpp:113-121`; SPGH's retail run showed a
   resting ball leaving that list 22 ticks after it came to rest), so a
   latched own ball that falls asleep closes its latch as if it had died
   (no body) and is re-latched only when it is in the box again. Fix: list the
   balls from the manager's whole object table (`m_PhysicsObjects`, sorted),
   as the SPGH patch's listener does; check with a pinned replay of an
   airship arm. **Done at step ASL** (`rewind-cost-design.md` "As built
   (step ASL)"; no wire or API change): reproduced offline - a latched own
   ball braked to rest just past the box's face fell asleep and the old
   binary closed its latch ("back in the sync", the relayed poses stopped);
   both listeners now take their balls from one helper over
   `m_PhysicsObjects`, and the latch holds.
   The spawn ghost the user added to it (2026-10-28: a ball is a
   translucent ghost with ball-to-ball collision off in its spawn area
   until it has left it) is built at step SPGH (`rewind-cost-design.md`
   "As built (step SPGH)"; bridge API 25, no wire change): every world
   decides each ball's solid PSI on its own copy; the start pile
   (born-in-contact cells: the 4.2 spacing lands 3.958 m apart) is gone
   in pinned replays (own balls bit-equal with the server's rows through
   the start window); one live smoke run also showed the respawn window
   bit-equal (not evidence by itself; a pinned replay over the respawns is
   owed); spacing and impulse are no longer needed for determinism -
   removing them is the user's choice - **the user removed both
   (2026-10-29), built at step SPX** (`rewind-cost-design.md` "As built
   (step SPX)"; no wire change: SessionStart carries 0 for both while the
   spawn ghost is on, every ball is born a ghost at the resetpoint itself,
   unkicked). The review's two findings are
   fixed at step SPGH2 (`rewind-cost-design.md` "As built (step SPGH2)";
   protocol 15, savestate format 3): the per-group ghost state is in the
   IVP savestate, so a rollback, a late join and a resync carry it, and a
   ghost life begins only at a spawn birth (a trafo keeps the state).
   Left with world agreement M0/M1 (item 5): existing members name a late
   joiner's mirror `P#63` and kick it with another index (§1.2), so its
   ghost state is their own until the member-added message lands; they are
   not told of a leave either, so they keep the leaver's group state, which
   the server drops at the leave (`spawn_ghost_forget`) - M0/M1 has them drop
   it too. **Item OWG, done at step OWG** (found at SPGH2's gate;
   `rewind-cost-design.md` "As built (step OWG)"; bridge API v26, no wire
   change): both hosts give the live own body its `P#` group in the
   PreSimulate pass of its birth tick (headless `capture_own_physicalize`,
   retail a callback queued at OnPhysicalize ahead of the spawn kick), its
   pairs re-asked, as the server's clone has it from its Physicalize. The
   one-frame ghost at a trafo is gone (h1_150 and retail); the spawn pair
   SPGH2 supposed never existed - the script's group is `Ball`, which the
   player filter keeps apart from every player ball (0 player pairs at all
   28 own births checked). Unverified: the branch where the recheck adds a
   pair (a trafo or respawn in contact range of a solid player ball, logged
   `player pairs 0 -> N`, N > 0) never ran; FULL owes one deterministic
   headless cell for it (a remote ball resting within ~1 m of a trafo point,
   own ball vs the server's clone for 60 ticks). A protocol-15 reference set
   (REF9) is owed before the next wire check: `core_ref_s8`'s multi-client
   arms differ from tick 252 under the rule.
   Answer 1 is option (a): H6 is judged by its absolute limits (starved
   within the band, the server's p99 within the budget, both met), so **the
   close-out is passed by the user's ruling** (H6's numbers stay recorded in
   RCX's note) and **`physics.trafo_inputs` is on by default since step
   B9DX** (item TIO; adaptation M2 closed). The x86 retail frame time of a
   trafo run stays deferred to the stage close-out (item 7). The default
   reaches only a `config.yml` without the key: the server writes every
   missing default into its config at load, so an existing config (the
   deployed okbc.st server's included) keeps `trafo_inputs: false` and
   needs it set to `true` by hand at the next deployment (RCX's note).
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
   **Step B8EG built B (G1) and P** (design "As built (step B8EG)"): B - the
   navigated beam keeps the generation, ball birth items are counted
   `births_parked_balls` (`parked=`) apart from real failures; one retail
   arm (13:24): `birth_recreate_failures` 11 -> 0, no script difference in a
   beam window. TR3's R4 is not B's: it is headless (own body control) and
   is the replay-tick life test of G8 (L3), where it now waits. P - both
   options measured on RO2's L8 K=3 / L11 K=3: (i) 301/301 bit-identical on
   both, (ii) leaves `P_Modul_30_Wippe`'s world 7.45e-09 m off on L8 (the
   world re-derived from the restored local rounds), so (i) is built (the
   restore skips a bit-identical world). Deferred to item 7: B-R1's control
   comparison of the beam windows and its parked count (the control run
   stalled 580 ms and resynced at 456).
   Step B7EG built Q's G2-G4 (engine change #20, bridge v23, record v4/v5,
   the hold; design "As built (step B7EG)"). Its retail session crashed at
   start-up in `d3d8.dll` - the user's RTSS overlay in a bad state (every
   launch 03:05-03:28 on 2026-10-28, even without the mod; fixed by
   restarting RTSS), not the display as B7EG first recorded. **Step RTX ran
   the deferred retail runs** (design "As built (step RTX)"; three two-copy
   L11 sessions, core on, snapshots off, trafo inputs and placement on,
   `scripts/retail/rtx_1..3`): the #20 hold ran on every rewind in Player.exe,
   with an empty queue only (holds = rewinds: 114 / 10, 94 / 14, 103 / 17; no hold, rebuild or unhold
   failure; `sv=N/0/0`), RC3's re-anchor asked for no resync (`frozen=0`,
   `resyncs=0/0`), RC4's guard never tripped with the trafo inputs on, and
   B2SX's own crossing was stamped at its poll tick + lead. Q-R1's held > 0
   was not reached: a retail death deactivates BallNav before it
   unphysicalizes, so keys held while dead queue nothing; it needs the own
   Physicalize held while the ball is alive (the retail `hold-event` verb,
   item 9), so the part of #20 that moves entries never ran on retail and
   Q-R1 is partial, not done. **Found at RTX, queued here:** a rewind whose window holds an
   own checkpoint crossing leaves the record's `IngameParameter[0][2]` at
   its pre-crossing 1 (the restore writes the cell; its live writer, the
   chain's pinned Set Cell, is not replayed), so that client's world holds
   1 until the next union pass writes 0 everywhere (rtx_1: 819-828); the
   other client, with no own crossing, held 1 there too and only the
   server's clone crossing had written 0, so it is per-player state for a
   few ticks, not a lasting split; no reader today (every manager run
   follows `run_sector_pass`'s 0). Options were (a) leave `[0][2]`
   out of what a restore writes, (b) record and replay the crossing's
   write, (c) leave it. **The user chose (b) (2026-10-28, verbatim "(b)
   Record and replay"); built at step N02R** (resync design, As built
   (step N02R)): every live-frame write of `[0][2]` (the crossing's and the
   death chain's) is recorded and written back after its replayed step;
   the `[0][2]` script-check lines of core_ref_s8 and rtx_1 are gone.
   **Found at N02R:** the death
   chain's `IngameParameter[0][1]` write (Gameplay_Ingame / Deactivate
   Ball: the dying ball's sector) is not replayed either - swing_snapoff's
   "[0][1] 1 vs 4" / "4 vs 3" at the ends of windows across an own death;
   no reader with the core on; options were (a) leave it, (b) the same record
   and replay for `[0][1]`, (c) leave `[0][1]` out of the compare. **The
   user chose (b) (2026-10-28, verbatim "(b) Record and replay"); built at
   step N01R** (resync design, As built (step N01R)): N02R's mechanism
   keeps one more cell, the union pass's own writes stay unrecorded, the
   private copy and the own poll are unchanged; swing_snapoff's eleven
   `[0][1]` lines are gone and retail n01r_1 writes the death's 1 after
   its replayed tick. Still
   queued here: **Q5** (the
   server's selected entries in a resync's portable record; a protocol
   change, 14 at the earliest - until then a resync holds the outside
   entries but keeps the live selected ones), and **gap L as the design's
   four steps G6, G7, G8, G9** (§8; G6 = L1 built at step EL6), each with its own checks (L-H1..L-H6
   and L-R1/L-R2 are live runs beyond one batch's gate; G8 may stop on §9.3).
   **Found at WG2, queued here (G1 membership leak):** `script_replay`'s
   restore re-registers the owner of a script it writes back active
   (`reregister`: the scene flag, `CKScene::Activate`, the owner put into
   `m_BeObjectNextFrame`), and nothing undoes it when a later restore writes
   that script back finished: a replay that ran no live frame in between
   (SimTool's chained rounds; a host frame with two rewinds) then finds the
   owner pending in the list and runs the finished script from its start.
   Measured (world-agreement design, As built (step WG2)): the arena's
   P_Modul_01 after its probe turned itself Off, a round anchored before
   the Off followed by one anchored after it fails (`flags ...48 vs ...49`,
   the probe re-run), each round alone passes, the same on the pre-step
   build.
   **Built at step RLM** (rollback-engine-gaps design, As built (step
   RLM)): `script_replay`'s registration ledger - a restore takes back an
   earlier restore's registrations of the scripts it writes, a replayed
   frame gives a finished script's pending add back; SimTool's witness
   (`owner membership at round ends`) 41-44 of 50 rounds -> 0 of 50 on the
   clients' recipe. **The measured WG2 failure was not this leak:** it
   fails at depth 0 too. **Queued here (the v14 chain artefact):**
   SimTool's default `--savestate-check` recipe is v14's (no
   `--session-recipe`): a script inactive at the anchor is not written and
   keeps its live state, and the rounds are chained without a live frame,
   so a round anchored after the previous round's end keeps a script state
   from before its anchor (P_Modul_01's probe, active at 62, kept at 144) -
   a state no host reaches. Options (not chosen): (a) judge such scenes
   with `--session-recipe` (every core host's recipe; v14 is the legacy
   path's, frozen until R7); (b) SimTool re-makes the live world at e for
   every v14 round (as the pass rounds do: whole frames from t0, ~e frames
   per round); (c) leave it as a known artefact of the v14 recipe.
   **Found at WG2, queued here (harness, gap Q's hold):** SimTool's
   `--savestate-check` binds no `callback_queue`, so its restore compares
   the whole PreSimulate queue (mode 0) instead of setting the outside
   entries aside as both clients do (B7EG's hold). On the arena a window
   that crosses tick 204 - the level start's retail ball, physicalized by
   Gameplay_Ingame (`Physics WakeUp`, Sound_Manager's
   `PhysicsCollDetection` x4) and parked by the server world at once, so the
   entries find no body and stay queued - is refused ("PreSimulate queue
   differs"); 90..203 passes, 90..209 fails, on the pre-step build with one
   ball and no step switch. Fix: bind the hold in the savestate check (the
   server world never rewinds live). **Built at step RLQ**
   (rollback-engine-gaps design, As built (step RLQ)): the savestate check
   binds the clients' hold (`hold_outside`, and with the session recipe the
   queue section too) and compares blobs with the outside entries set
   aside, as a replay captures them; 90..209 passes 50/50,
   `--savestate-no-hold` still refuses.
   **Found at WG2, queued here (a constraint shut down inside the
   window):** the arena's P_Modul_29 bridge breaking inside a savestate
   window (`Test "Ball_Stone"` -> `10 Hinges` Shutdown) makes the restore
   to an anchor before the break refuse: "core of 'P_Modul_29_Platte06': no
   live controller 'sys:unknown' (priority 405)" - the record names a hinge
   (`IVP_CP_CONSTRAINTS`) the live world destroyed, and the blob restore
   only matches recorded controllers to live ones, it never re-creates one.
   Pre-existing (the pre-step SimTool, the stone alone, anchor 47 depth 45).
   A client's rewind across a break goes through the same restore (not
   measured live).
   **Built at step RLC** (rollback-engine-gaps design, As built (step
   RLC)): a joint block's constraint is named in the local record by the
   hash of its image (its own fields, read through physics_RT, bridge API
   v27, no engine change) and kept in a per-process ledger; a restore whose
   record had a constraint the live block lost (or another one) makes it
   again from that image before the blob, and puts the constraints back
   in their recorded places among their cores' controllers (the blob
   cannot tell constraints apart; the place is in the local record too).
   The bridge window 47:45 and the 50-round schedule on the clients'
   recipe PASS (20 constraints made again); every constraint made again
   in another order before each rewind (`--joint-shuffle`) PASS 20/20
   with 22 lists put back, FAIL 20/20 ("joint order") with the order
   pass off; live, four stone balls on the bridge at 150 ms, clients 2 and 3
   rewound across their break with every blob taken. Residuals met, not
   this item's: SimTool's v14 chain artefact (above, P_Modul_25's flags in
   8 of 50 v14 rounds) and G-c (the worlds break the bridge at different
   ticks: the probe and the identity read run on each client's own ball).
   **Queued at FULL step S2 (2026-10-29, found by the forced-rollback
   capture replays of `core_ref_s12`, pre-existing):** gap L's mirror half
   misses one case - a mirror whose death arrives late (its own rewind at
   D-1) and a later rewind of the same frame anchored at A < D-1 (a forced
   one, or an ordinary input correction): the records (D-1, now] the
   death's own rewind re-captured keep the dead mirror's old generation
   (`rollback.hpp` copies the record's tracked set and refreshes only the
   remotes alive in the replay), so `revive_ended_lives` (a life ends only
   at a generation change) hands no ended life over, no half revives it,
   the restore skips its blob entry and the replay runs without the body
   over [A+1, D); the re-captured records lack it (`swing_snapoff` c0
   900-904, `h1_150` c0 1056-1058). Fix: the death's own re-capture (or
   the end test) must see the end - e.g. a re-captured record of a remote
   with no body takes the generation the live death gave it. Check: the
   S2 forced replays (`.local-tools/work/S2/forced.sh`, FINAL rows
   `--final-checkpoints`) with no presence difference.
   **Queued at FULL step S2's gate round (2026-10-29, pre-existing,
   RLM's family):** a pending add a replayed frame makes
   (`CKScene::Activate` from a replayed script's activation: +1 in
   `m_BeObjectNextFrame`) outlives the replay; the RLM ledger takes back
   only `reregister`'s. A second restore before the next live frame
   (a host frame with two rewinds, the second anchored before the
   activation tick; SimTool's chained rounds) admits the owner through
   `owner_pending_add` in frames where the live frame did not run it and
   consumes a next-frame flag the live world kept (WGQ7-V4b's alive scene:
   `P_Modul_18_Particle Script` DEACTIVATENEXTFRAME, 8/50 rounds). Fix:
   the ledger also records each pending add a replayed frame makes (the
   owner's count before and after `execute_frame`) and the next restore
   of the same live frame takes those back with its own. Check: V4b's
   alive scene (`.local-tools/work/S2/sim_jobs.sh` v4b, seed 1) 50/50 and
   its `--verbose` "owners held live" line at pending 0. **Fixed at FULL
   step FX1** (rollback-engine-gaps-design.md "As built (step FX1)"): the
   ledger's `replayed` list, taken back by the next restore of the same
   live frame; V4b alive 50/50, the after-restore reading at pending 0 in
   50/50 rounds (the before-the-rewind line shows the last replay's adds,
   left for the next live frame by RLM's rule).
   **Queued at FULL step H1 (2026-10-29, found by SP7-c's forced replays of
   the live L8 cross at 150 ms, pre-existing, not decided here):** the
   deferral of an untracked window-born fixed body (RO1: kept inert from
   the restore and rechecked into the tree at its birth tick) leaks into
   the records the replay captures before that tick - they hold the inert
   body - so a SECOND rewind anchored on one of them (before the birth
   tick) restores the body as a known one, runs no recheck, and its
   overlap-tree membership stays wrong for good. Measured
   (`maps/arena-test/h1_full/l8_cross_150`, client 2's capture, rH1; the
   pair arm and its control replayed again with rS3, HEAD 193c9a5b's build
   without H1's switches: both journals IDENTICAL to rH1's, the same
   "hull P_Dome_MF" line - pre-existing on HEAD):
   `--force-rollback-at 350:24` (anchor 326, 1 body unknown to the blob)
   or `360:24` (anchor 336) alone are exact; the pair 350:24,360:24 (the
   s2 pass at 335 and s4 at 342 inside both windows) logs at the second
   rewind "the world re-simulated to tick 360 differs from the live
   record: hull P_Dome_MF entries [ov@...] [] | ov P_Dome_MF in_hull 1 0",
   and from then on the world has 3 overlap-tree nodes fewer than the
   straight run (P_Dome_MF, a fixed body of the s4 pass, is born at 346 in
   the straight run - absent from its blobs at 345, present at 346 - and is
   `in_hull=1` at 350; the first replay's records hold it, inert, from
   330 on; after the pair it is `in_hull=0 node=-1` at 350, 360 and 700) (`--savestate-diff` of the dumps: ov_nodes 69/66 at
   350..362, 75/72 at 700), every body value still bit-equal (45/45
   bodies at every checkpoint) until client 2's own ball, beamed onto s3
   at 741, lands: collisions 176/175 at 742, the ball 0.41 m off the
   server at 758, 8.1 m at 942 (`first_divergence.py`). A host frame
   with two natural rewinds 10 ticks apart over a sector pass is ordinary
   at 150 ms, so a live client can part from the server this way. Options
   (each a rollback-engine change, none decided): (a) mark a deferred
   body in the records captured before its birth tick (a later restore
   from such a record treats it as unknown again and keeps the deferral);
   (b) extend RO1's destroy-and-remake to these bodies (the replayed
   activation makes them; needs the guard RO1 lacked for them); (c) at
   every restore, treat a body whose birth-log tick is after the anchor
   as window-born even when the record knows it. Check: the pair arm
   above and `--force-rollback 10:24` on all four l8_cross_150 captures:
   FINAL rows equal to the free-clock control (now: bodies 0, one ball
   off from 758). Tools: `.local-tools/work/H1/h1_forced.sh`.
   **Measured at FULL step SPX (2026-10-29, offline; the choice waits for
   the user):** each option prototyped as an uncommitted patch on HEAD
   fccf4f7d (`.local-tools/work/SPX/deferral/option_{a,b,c}.diff`, x64
   copies `build-agent-rSPXa/b/c`, judges `judge.sh` / `extras.sh` there).
   Why RO1 kept P_Dome_MF on the deferral: its guard's script half -
   `owned_by_selection` is false (P_Dome_MF is a level object of the s4
   pass, no selected script is owned by it or a parent); the CK-free half
   (fixed recipe, no lifetime at the anchor) passes. Every option PASSES
   the entry's check: pair 350:24,360:24 and `--force-rollback 10:24` on
   all four captures, LOCAL and FINAL rows equal to the option's own
   free-clock control at every checkpoint (balls 0, bodies 0 off; the
   4-8 FINAL presence gaps of the forced arms at 498/500 are the same
   without any option), no "hull P_Dome_MF" line; each option's control
   IDENTICAL to H1's; wire check `rH7X -> rSPX{a,b,c}` over
   `core_ref_s12` ALL SAME, 36/36 journals IDENTICAL (none of its
   captures reaches the changed paths: no SPX line in any of the 36
   replays of any option);
   `ro1g_l8` (resync, "0+3 deferred") and `rlm2rw` replays IDENTICAL to
   the live journals, `h1_150_on` c0 844:29,862:30 equal to its control;
   662 tests pass on each. `rlm2rw` (L8, live `--force-rollback 5:12`)
   shows the same leak live at 75 (P_Dome_MF, born 66): every option
   removes it, journal unchanged (no ball touched the dome); no natural
   (unforced) rewind in `h1_full` or `core_ref_s12` shows it. Rewind cost:
   one pair sample per option, no HEAD baseline and no noise measured
   (t_rest 14-15 ms, t_rep 239-262 ms): not distinguishable from one run
   each; (b) adds one destroy + Physicalize per crossing restore. (a) marks, in each record a replay captures before a
   deferred body's birth tick, that body (`tick_state::unborn`, in memory:
   no record, blob, journal or protocol change; rollback.hpp only, ~30
   lines); a non-travelling restore from such a record defers it again.
   Explicit, no new inference (the birth tick is the one RO1 already used
   at the first restore); by construction it also marks a resync's
   re-step records (same capture loop; not measured). (b)
   destroys such a body at every crossing restore like RO1's module parts
   (rollback_binding_scripts.hpp guard, ~10 lines, + a diagnostic log in
   rollback.hpp that is not part of the option): it changes the FIRST
   restore too, the records never hold an inert body, and the body is the
   replay's own; P_Dome_MF came back every time - but the prototype's
   guard only trusts the replayed activation: a product guard needs the
   body's maker (the script running at its Physicalize: a birth-log field
   fed by both hosts) or a destroyed body whose maker is not replayed is
   lost for good. (c) infers it at the restore: an untracked body the blob
   knows whose birth-log tick is in the window and that has no lifetime
   at the anchor (a new CK-free hook `unborn_at` in session_host_world.hpp
   plus ~30 lines in rollback.hpp) is deferred again; no state, but it
   relies on the birth log and would recheck a known in-tree body if the
   log were ever wrong. All three skip a travelling blob. (a) and (c)
   leave RO1's deferral as it is (the inert body is the anchor's live
   object, kept out of the tree while the replayed activation runs its
   Physicalize over it: exact in every arm here, as the single rewind
   already was); only the second restore changes. Landing: (a) changes
   `tick_state` (an element of `history_`, a deque: `rollback_engine`'s
   size and so `BallanceMMOClient`'s do not move) and (c) adds a hook to
   `rollback_world` (rebuilt per call) - both header layout changes, so
   the x86 objects are deleted before that build; (b) changes no struct;
   any log cap stays a function-local static (no engine member until R7).
   Recommended:
   (a) (smallest, explicit, no new trust in the birth log or in a maker).
   **BUILT at FULL step DX1 (2026-10-29, the user's choice "SP7-c: a"):**
   option (a) re-based on f9775b82 unchanged in logic (rollback.hpp only;
   log `deferral: <name> is unborn in the record of <anchor> ...`); the pair
   and `--force-rollback 10:24` arms balls 0 / bodies 0 off against the
   control, the H1/RO1 replays unchanged, rlm2rw c1's tick-75 leak gone,
   663 tests (one new), wire check rH2s -> rDX1 over `core_ref_s12` ALL
   SAME: rollback-engine-gaps-design.md "As built (step DX1)".
   **Found at FULL step H1 (2026-10-29, H7 at 150 ms); FIXED at FULL
   step H7X (2026-10-29) - the cause and the fix after the original text,
   which named a candidate that was wrong:**
   a late joiner's trafo pieces remade by the join's reconcile run a
   fraction of a tick behind the server's. `h1_full/h7_join` (client 3
   joins at 1550, S 1705, re-stepped to 1761; the reconcile remade 6 type
   sets, both paper sets among them, 102 pieces kept out of the restore):
   pieces 73/102 bit-equal; `Ball_Paper_piece06_Trafo3569020499`, falling
   off the level at a constant (-6.464, -3.333, 6.464) m/s on both sides
   (the wind's horizontal part included, bit-equal velocities), is
   0.098 m behind client 0's and the server's at its first LOCAL
   checkpoint after the re-step (1762) and stays on the same line - a
   quarter of a 2-tick step, i.e. a time offset of the remade pieces'
   cores, not a state difference (the 128 rows at S were equal);
   `pieces.py` first counts it at 2006 (its player's next Trafo event),
   worst 4.42 m. Candidate cause, not established: a leapfrog-phase
   difference between the cores the reconcile made fresh and the
   founders' blob-restored ones (the IVP restore's leapfrog delta family);
   from 1724 to 1762 the joiner's piece covers 37.5 ticks of displacement
   where client 0's covers 38.0. Not confirmed because a late joiner's
   capture does not replay (S2), so no savestate dump of the joiner's
   world exists, and the piece names (`_Trafo<id>`) are known only after a
   run starts; the check: one live H7 with `--trace-body <piece>
   --trace-ticks S S+5` on the joiner and a founder (or `--dump-savestate`
   at S+1 on both). TR3's H7 (102/102) had no moving piece at the join,
   so no earlier build ran this case: whether it predates H1's HEAD is not
   known (H1 changed no shared or headless code path - wire check 36/36
   IDENTICAL - so it is not H1's). Deferred, not waived: the trace run is a
   13th live run, over H1's planned 11-12 (12 used); the reconcile code
   (`client_core_trafo.cpp` `reconcile_trafo`, the remake branch) makes the
   pieces fresh with `birth_tick = anchor` and leaves their state to the
   rows and the blob of S, which is where the trace must look first.
   **H7X: the cause** (read from H1's own `h7_join` logs and journals, no
   new trace needed): not the remade cores. The joiner's re-step stepped
   its first replayed tick (S+1 = 1706) at the physics globals of its own
   level start - factor 0.001, gravity -9.81, the values every world has
   until Init Ingame's frame-2 write, which the joiner made at 1707 - while
   the server stepped 1706 at 0.002 / -20. Since RK1 (982370d8, C3) a
   rewind writes the anchor record's globals before the restore and each
   replayed tick's record before its step; a resync's anchor is the
   synthetic record at base-1 and its window records are the joiner's live
   ones, i.e. the joiner's level start, not the server's world (design
   §4.2 assumed a re-step's ticks had no local records). Evidence:
   `client3.log` "globals: time factor 0.001 -> 0.002 at tick 1707 (frame
   2 after the anchor)", the restore's "the physics time factor is 0.001
   live and 0.002 in the savestate (kept live)", the first replayed step's
   "factor=0.0010 phys_dt=0.015152" and the second's 0.0020 / 0.030303;
   every moving body of the joiner's world is then behind by exactly one
   half step: offset / velocity = -0.01515 s in x and z for every falling
   paper piece (-0.02287 s in y, the gravity of that tick), the Stone
   mirror cruising at 17.858 m/s 0.270 m behind, P_Modul_26 -0.011 s -
   at 1762 121 of 154 bodies equal. A regression of RK1, which predates
   H1's HEAD (before RK1 a replay wrote no globals and ran at the live
   0.002); TR3/TR3m's H7 had no moving body at the join. **The fix** (one,
   as design §4.2 / A17 states it: "the synthetic anchor takes its globals
   from [the server's blob]"): bridge v29 `savestate_globals` reads the
   blob's factor and gravity; `rollback_engine::restore_and_replay` writes
   them at a resync's anchor and keeps them for every replayed tick whose
   record still equals the anchor record's (from the first record that
   differs, the records, as C3). No protocol, journal, record or engine
   change. **Measured** (`h7x_full/h7_join`, the same recipe at 150 ms,
   snapshots off): the joiner logs "the re-step runs at the server's
   physics globals of S (factor 0.002, gravity y -20), not the anchor
   record's (factor 0.001, gravity y -9.81)", `globals=0/0/0/0/0` (nothing
   written: the live world already had the server's); pieces 102/102
   bit-equal on all four clients (H1: 73/102 on the joiner); at the first
   checkpoint after the re-step 153 of 154 bodies equal on the joiner, the
   one left `P_Ball_Wood_MF` (0.021 m), a ball-pushed body that is off on
   the founders too (0.008 m on c1/c2 at the same tick: pushed by the
   predicted paper mirrors). Record-format design "As built (step H7X)".
   **Queued (gap L, found by step PRX's review, pre-existing in the revive
   path): a life whose birth this world's birth log does not hold cannot be
   revived.** `revive_body` (`session_host_world.hpp`, step P / EL9) makes a
   life again only from `births->lifetime_at`; the log starts empty at the
   anchor (both hosts drain and discard the event log's history there,
   `session_client.cpp` / `core_host.cpp` "install the listener, discard
   history"; retail's OnPhysicalize records only while a core session runs)
   and keeps the last 4096 ticks (`local_birth_log::add`). So a mechanism or
   prop alive at the anchor (sector 1's), or born more than 4096 ticks before
   the rewind, that the window destroys is "unrevivable": the replay runs
   without it from S until the tick that destroys it again, and those ticks'
   re-captured records lack it. Measured for the DepthTest props
   (`h123_150f` server journal): `P_Ball_Wood_MF` has a row from the first
   checkpoint (34, alive at the anchor), the other three from 630 (sector 2).
   A fallen prop's window lies 200 m below the level's lowest DepthTest cube,
   so only that prop's own rows differ, and only inside the window; before
   PRX the clients never removed it at all. The fix (an anchor birth for
   every body alive at the anchor, from a recipe the host can read without
   the event log, and a log that keeps each name's latest birth past 4096
   ticks) is its own step; world-agreement design "As built (step PRX)".
5. **World agreement** (`world-agreement-gaps-design.md`), after F5-F7
   (WAG:1204, order constraint M6): client proximity sensors rewired to
   the nearest ball (own or mirror; the G-c leader-only wakes P_Modul_03
   1.36 m, P_Modul_19_Flaps 0.8-1.8 mm, P_Modul_34 2.7 m; step SFD: the
   l8_cross P_Modul_29 plates, 3.6 m worst, from c1's BodyRevived at 554
   that only the server applies), the
   member-added message (trafo design 9.7: existing members do not know a
   late joiner's join order), leave and rejoin, BodyRevived retired (G5).
   **Its questions are answered (2026-10-28, the user's message quoted in
   item 3; design §9 and §11.1):** Q7 = (b), verbatim "6. b，应该都复制一份
   逻辑，同一个机关应该可以响应所有玩家" - per-player copies of the mechanism
   logic, one mechanism responds to every player (the design revision it
   needs is step WAD's); Q8 is superseded by answer 7 (no client without
   the session core is served, step B9DX, so "RL ends locally" is moot);
   Q9 was left to the orchestrator ("5. 你决定"), who chose **G-c first
   (G0-G5), then the member lives (M0, M1)** - the user's own audit order
   ("... F5, F6, F7, G-c, join-order message ..."), G-c being the residual
   every snapshots-off check meets while M1 matters only with late joiners,
   leaves and rejoins; P_G takes the next free protocol number at G3, P_M
   the one after.
   **Step WAD revised the design for these answers (revision 3, design
   §10.1).** Q7 (b) is §2.10: per-player copies of the fan's force graph
   and the bridge's stone test, the only mechanism sub-graphs that act on one
   ball (71 identity reads on L1-L13). Slot k serves the life with join
   order k, and K is the room capacity, sent in SessionStart (protocol P_Q).
   The legacy parts are dropped (answer 7). The design's step Q7 needs
   every world to know every life's join order (J1), so it comes after M1,
   not right after G3: the order is G0-G6, M0, M1, Q7.
   **G0 is built (step WG0):** SimTool `--probe-census`, V1 PASS on
   L1-L13 with the placement off and on (design §12 "As built (step
   WG0)": the counts, the signatures G1 must reproduce, the two unit
   shapes on every fan and bridge, placed copies included).
   **WAD-A (queued by WAD's review, a measurement):** a force controller is
   named `rt:force/<block id>` (engine change #18), and a block id agrees
   across worlds only if both worlds created the same objects in the same
   order; T3 found the trafo sets' ids differ and aliased them. Q7 aliases
   every unit force block (design §4.3), but before Q7 the original fans and
   the placed fan copies (placement on since SP10) cross worlds unaliased at
   every resync and late join. Measure it (a resync and a late join with a
   fan force held: headless and retail, placement on; pass = no "no live
   controller" at the import); if unmatched, the §4.3 alias moves forward to
   every mechanism force block.
   **G1 and G2 are built (steps WG1, WG2):** the server's world places by
   the union's rule under `WORLD_PROXIMITY_UNION` (bit 2, config
   `physics.proximity_union`, default off until G3; V2 and V4 pass, the
   bit-clear path unchanged: V3 and the wire check). Its J-e interplay
   (a ball holder removed between two steps, trafo inputs off only) is
   counted by the guard and waits for M1 (design, As built (step WG2)).
   **G3-G6 are built (steps WG3, WG4, WG6):** the clients run the rule
   under the bit (protocol 16, `anchor_probes` compared, BodyRevived
   retired), and the server's default is on. The retail core host runs it
   too (WG4), and the documents follow it (WG6: RSD §2.4, the protocol
   doc, `collision-overhaul-design.md` 9.31). The checks deferred to FULL
   are listed in the design's "As built (step WG3)" and "(step WG4)".
   Next: M0, M1, then Q7.
   **Q7 is built (step WGQ7, protocol 19, journal v8):** the per-player
   units, K = the room capacity, the join-order cap; V6 passes on L1-L13
   (placement off and on, K 2 and 8), V7 on the arena, and the retail CK2
   copies give the server's signature (Q10). Queued from it (design, As
   built (step WGQ7)): **REF12**, the protocol-19 reference set (every
   `core_ref_s11` capture is protocol 18; *done at FULL's REF12,
   2026-10-29: `core_ref_s12`, protocol 22 at HEAD 03e9f4d2, eleven arms
   with the deployed defaults trafo_inputs and placement on, swing_snapon
   dropped under the snapshots-off rule; server journals N/N, the wire
   check ALL SAME 36/36, now `wire_check.sh`'s default; its README is
   `maps/arena-test/core_ref_s12/README.md`*); the identity copies' leftover
   parameter outputs at a release (pre-existing since G1, 2 per copy, their
   owner pointer dangling); WAD-A's verdict, which the step's runs could not
   give (no fan force held at an import; the one force controller held at
   the late join's import, P_Modul_26_Sack's, belonged to a core whose
   constraint controllers were missing too; under the bit every unit force
   block is aliased, so only force blocks outside units and the bit-off
   control remain: FULL); a guard for the server's unit set (equal to the
   clients' by WG0's census, not by construction: the compared mechanism
   signature skips a non-mechanism unit, so a level breaking the census
   would diverge without a refusal; **built, step F1**: the signature's
   tail counts the server's non-mechanism units, a member is refused; and
   the identity copies' outputs, **released at F1**: 0 left); the checks
   deferred to FULL listed there.
6. **Record-format gaps** (`record-format-gaps-design.md`; K0 built): K1
   the time factor and gravity per tick (**built, step RK1**: bridge v28,
   the engine puts each tick's factor and gravity back in a rewind, no wire
   change; S1 failed rounds 23/30 -> 0/30 (seed 7), 0/1500 (seeds 1-5 x
   300); the design's "As built
   (step RK1)"); **K3 = the Level 1 tutorial
   switched off** (**built, step RK3**, protocol 20, world option
   `WORLD_SESSION_CLOCK`: every world activates the tutorial's exit before
   frame 10 after its anchor; measured: the exit lands in frame 16, so every
   world was frozen at frames 9..15 - founders and the server at the same
   ticks, a late joiner alone (its import re-steps its own frozen ticks);
   the design's "As built (step RK3)"), **closed by step RK3H** (the user,
   2026-10-29: 'Hold frame-9 write'; protocol 22): every world holds the
   tutorial's frame-9 factor-0 write (Gameplay_Tutorial / Init's Set
   Physics Globals) at the in-play 2.0 from its anchor on, so no world
   freezes; live L1 with a late joiner at 150 ms: no factor-0 anywhere, the
   joiner's own ball and every mechanism exact against the server from its
   import on (the design's "As built (step RK3H)"). **Open, queued by
   RK3H** (two writers the narrow hold leaves, both read from the graph
   dumps, neither run): **RK3R** (**decided 2026-10-29: not handled**, a
   known limit - the user, verbatim:
   "RK3R那个，看看原来的影子球模式，会处理玩家手动重开那种导致的新手教学触发吗？印象中好像不处理（只会处理ready-go时候的跳过教程），如果是这样的话，这个遗留可以直接不处理"; the main branch skips the tutorial only at the countdown
   restart, `BallanceMMOClient.cpp:633`, and replays it after a manual
   restart; FULL records what a reset does, row RK3R-rec; the design's
   "Decided (2026-10-29): RK3R"), a Level 1 level reset after the exit
   (9.28's mid-session reset): `Init Ingame` sets `IngameParameter[0][3]`
   again, the next new ball's `activate Tutorial?` (#27646) re-activates
   Gameplay_Tutorial, RK3's exit fires once per anchor and does not fire
   again, so the tutorial's chapter `Physic on/off` writes run and freeze
   each world when its own ball reaches a chapter point - the worlds
   diverge (RK3's gap; options: re-fire the exit on every new ball under
   the bit, or hold the chapter writes - a choice for the user if the
   design does not settle it); **RK3F**, a Gameplay_Refresh activation
   (a respawn or a pause resume; a retail ESC is host-local) inside frames
   9..15 after the anchor writes 0 through `Tutorial freeze/unfreeze`
   (#29832) while the tutorial's freeze flag is set, freezing that world
   alone - a known limit of the user's 'Hold frame-9 write' (it predates
   RK3H: before it every world was at 0 in those frames), to be measured
   with R1's ESC and put to the user if it shows (shown at FULL H1; the
   user, 2026-10-29: "D5: a"; **built at FULL step DX2**: #29832's Set
   Physics Globals #29784 held onto RK3H's constant in every world,
   `record-format-gaps-design.md` "As built (step DX2)"); **K2, the ActiveBall
   census, is built (step RK2)**: every world (server, headless, retail core
   host) runs the array-read census at its anchor (`game/array_reads.hpp`,
   one log line per unrecorded read, the status line's `array_reads:`
   section), and §11 Q4's decision is built with it - the cells a selected
   script writes (the fans' sound-slot column) travel in a local record
   section and come back at the anchor, never compared; no wire or journal
   change. S2: `unrecorded = 2` on L1-L13, both the sector manager's
   `PH[*][4]` read (written in play only by the extra points' own rows,
   outside the replay); **decided 2026-10-29: option (b)** (the user,
   verbatim: "选：加检查"; the collectible rule in §2): a census class
   "per-world collectible state" with a load-time check of column 4's
   writers, not recorded, not rolled back - built by FULL step F4 (the
   design's "Decided (2026-10-29): `PH[*][4]` = option (b)"); built
   2026-10-29, the design's "As built (step F4)": S2 `unrecorded = 0`,
   `collectible = 2` on L1-L13 and the arena).
7. **Stage close-out (the one FULL verification):** four clients, 150 ms,
   10 % loss, snapshots off: the spawn pile and every mechanism
   bit-identical to the server at every checkpoint; and every check below.
   On pass: snapshots off in the deployed configuration and in the retail
   default; only the one state transfer remains.

   **Removed at step B9DX** (the user, 2026-10-28, answer 7: the server
   refuses every client without the session core, so no check exists for
   `session core off` any more, §2): RL1 (c)'s legacy member's floor + 3;
   TR3b (g)'s and SP5 (b)'s and HU1 (a)'s core-off halves (their core-on
   runs stay); TR4 (d) (R4, core off against `p3_off_P`); SP5 (c) (a
   legacy member refused by a placement session - the refusal is now
   B9DX's, run there, `scripts/retail/b9dx_lgx`); RO1 (e) (a core-off
   retail run); SP10 (a)'s run 6 (the legacy path with the placement on
   and off) and SP10 (f) (core off with the placement off); REACH + R12
   (e) (a retail run with `session core off`); item 2's B2SW entry's
   retail run with the session core off. The items below keep their
   letters; each removed one says so.

   **Deferred to the final verification** (checks a quick-tier step did
   not run; each step appends its own, the close-out runs them all):
   - RTX (the retail runs B7EG, B2SX, B5RX and B6RC deferred, quick tier,
     2026-10-28; three two-copy sessions, `scripts/retail/rtx_1..3`; done
     and off this list: B7EG's empty-queue hold in Player.exe, B2SX's
     [0][1] cell, RC3's no-false-resync half, RC4 with the trafo inputs on;
     Q-R1 is partial and stays): (a) Q-R1's held > 0
     and its queue-order compare (need the retail `hold-event` verb, item
     9; no session reached a queued entry); (b) B2SX's retail follow check
     of the re-armed delayed link (unchanged, item 2); (c) a frozen clock
     on retail asking for one resync (no verb stops the clock; unit-tested).
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
     (c) [a legacy (core off) member's floor + 3: removed at B9DX]; (d) logs the re-step
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
     two-copy run of the merged x86 build, core on (built,
     CLIENT-BUILD-OK; no BallanceMMOClient source changed, but the
     shared engine and savestate changes reach the core - the savestate
     index rewrite, and the replay-mismatch counter of `rollback.hpp` now
     counted only while logged, the first 8; neither changes a simulation
     result; its core-off half was removed at B9DX).
   - TR4 (trafo T4, the retail host, quick tier, 2026-10-27; ran: x64 and
     x86 builds, 522 tests, GCC 29 ok, the wire check rTR3b -> rTR4 ALL
     SAME twice, ONE two-copy retail session on L11 with core on and
     `trafo_inputs` on, snapshots off): (a) R1, the solo forced arms
     (`r2_run.py` R2 scenario, 50:12, 97:24, 131:47, 13:24) and the
     two-trafo `r2t_run.py`; (b) R2 with the R2 config (lifecycle rewind and
     own echoes off) and R2b with `net lag 150` (late Trafo events inside
     input rewinds); (c) R3, a second session in one game process after a
     single-player paper trafo (sets rebuilt, no `_Trafo` object left,
     the quiesce's 18/18); (d) [R4, core off against `p3_off_P`: removed
     at B9DX]; (e) R5, the
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
     run of the SP5 x86 build, core on (no retail source changed, but
     SessionReady's bytes and the journal header changed; its core-off
     half was removed at B9DX); (c) [a member without `CAP_PLACEMENT`
     refused out of a placement session, the rest running: removed at
     B9DX, whose run `b9dx_lgx` shows the refusal of every such member,
     the rest running]; (d) SimTool `--replay` (record mode) with its new
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
     canonical order on every host only if one does); (e) [a core-off
     retail run: removed at B9DX]; (f) a GCC/Linux compile of `script_replay.hpp`'s
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
     build, core on (its core-off half removed at B9DX; no retail
     behaviour changed: the new
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
     joiner, `sector manager` trace) [run 6, the legacy path with the
     placement on and off: removed at B9DX], and the
     retail option-off release path (`prepare_unplaced` releasing copies a
     level still holds: a session with the option off on a level that ran
     one with it on, `session reload off`), never exercised on retail;
     (b) §7.6 retail x86 (run 1 on L9 or L12 at `net lag 150`, the
     cost lines; the anchor's apply took 1.43-1.45 s on retail x86 for
     L11's 53 copies); (c) the headless A/B arms with the default now on;
     (d) CPU parity; (e) three launches without a hang; (f) [a retail
     session core off with the placement off: removed at B9DX]; (g) the
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
     verification" list, re-captured at protocol 13); (e) [a retail run
     with `session core off`: removed at B9DX]. Found at R12, queued here (item 2): the cap's
     estimator over-states the per-tick cost of shallow rewinds that make
     bodies (`core_ref_s7/l8_cross`: 2.6-3.3 ms "a tick", cap 48); whether
     the births' share leaves the replay stage is a design choice
     [the user, 2026-10-27: "拆成固定开销 (Recommended)" - built at step
     B5RC as R12B and NOT built (the user, 2026-10-28, "按推荐继续"): the
     over-statement was not the births (a birth tick ~0.1 ms over an
     ordinary one) but the headless-only end-of-replay savestate text
     diff, 7-10 ms in each of a session's first 8 such rewinds; step B5RX
     (R12D) leaves that diff's time out of the meter (option (b)), the
     l8_cross cap 48 / 82 / 48 / 48 -> 73 / 92 / 71 / 70; late-player
     design §2.5 "As built (step B5RX)"]. Also
     for the user (R12 review): the cap before a client's first rewind is
     48, so a first stall deeper than 48 before any rewind still resyncs;
     a different starting value is the user's to choose [the user,
     2026-10-27: "保持 48 (Recommended)" - it stays 48]. The cap's samples
     are captured clock reads since the review round, so a capture replay
     reproduces the cap exactly; (f) a retail run with the review round's
     x86 build (its retail behaviour is unchanged: retail's `now` is its
     clock).
   - RK1 (record-format K1 = C3, the physics globals per tick, quick tier,
     2026-10-29; ran: x64, x86, tests 629/629, GCC 30/30, the wire check
     rWGQ7 -> rRK1 on `core_ref_s11` with `--accept-protocol 18` ALL SAME,
     solo1 / solo2 IDENTICAL, SimTool S1 (seed 7 and seeds 1-5 x 300) and
     S1b (L2, L13, seeds 1-3)): (a) H1, a live L1 forced rewind anchored
     inside the tutorial's freeze (headless; until K3 switches the tutorial
     off); (b) H4, a live forced rewind anchored at the first record (the
     frame-2 write lands at base+1 in every headless client, measured in
     the 14 replays); (c) R2, H4 on a retail core host (the x86 build is
     CLIENT-BUILD-OK, not run); (d) S1b with `--session-recipe` (the
     re-made live world gets t0's globals since RK1, not measured).
   - RK3 (record-format K3 = C4, the Level 1 tutorial switched off, quick
     tier, 2026-10-29; ran: x64, x86, Arch, tests 633/633, GCC 30/30, the
     wire check rRK1 -> rRK3 on `core_ref_s11` (SessionReady's protocol
     field only, journals IDENTICAL), SimTool S3 / S4, one retail L1 run and
     one headless L1 run with a late joiner, both on the first build's
     server (its frame-numbering bug, fixed after them and checked by
     replay)): (a) a live run of the fixed server, L1, founders and a late
     joiner (H3: the joiner's 7-tick lag measured); (b) H2, a client at a
     chapter spot; (c) H5, the arena recipes with the bit; (d) R1's ESC;
     (e) a test of `physics_world`'s frame count with a non-zero
     `set_tick_index` (the message-test target does not link
     `BallanceMMOSim`; the shared rule `tutorial_exit_due` is unit-tested,
     the server's count is guarded by the replay log line "exit activated
     before frame 10 (tick base+9)" of a Level 1 journal with base != 0).
     RK3H (below) ran (a).
   - RK3H (the Level 1 tutorial's frame-9 freeze held, the user's 'Hold
     frame-9 write', quick tier, 2026-10-29; ran: x64, x86, Arch, tests
     644/644, GCC 30/30, the wire check rTRL -> rRK3H on `core_ref_s11`
     (SessionReady's protocol field only, journals IDENTICAL), SimTool S3
     (no factor-0 write in 1800 ticks, exit at frame 16 / 20, the tutorial
     inactive after it) and S4 (seeds 1 and 2, 40/40 each), one headless L1
     run (4 clients, a joiner, 150 ms, snapshots off) and one retail L1 run
     (two copies, core on, snapshots off)): (a) H2, H5, R1's ESC, RK3's (e)
     as RK3 left them; (b) a Gameplay_Refresh activation inside frames
     9..15 (a respawn or a pause resume that early) still writes 0 while
     the tutorial's freeze flag is set - the narrow hold does not cover it,
     not exercised (no trigger that early in any run; queued as RK3F in
     item 6; shown at FULL H1, built at FULL DX2); (c) the old
     (protocol 20/21) Level 1 journals replay with the hold and diverge at
     their frame 9 by construction; (d) a Level 1 level reset after the
     exit: `Init Ingame` sets `IngameParameter[0][3]` again and the next new
     ball's `activate Tutorial?` re-activates Gameplay_Tutorial, while RK3's
     exit fires once per anchor - its Init write stays held, its chapters
     could freeze that world (RK3's, read from the dumps, not run; queued
     as RK3R in item 6).

   **The close-out's verdicts on this list** (FULL plan, 2026-10-29,
   `.local-tools/work/FULL/plan.md` §1 and §4, written down at its step F0;
   the plan's row ids in brackets). Every item not named here stays OPEN
   and is run by the step the plan names.
   - DROP (already satisfied): RC1 (d) [RC1-d], TR3b (g), SP5 (b), HU1 (a)
     and REACH+R12 (f) (every later step ran retail on a newer build; the
     close-out's RET-HEAD session closes them); TR3b (c) (B2SW ran H3b);
     SP5 (a) (SP6 ran it); SP7 (d) (the Arch build of `physics_world.cpp`
     ran since); RO1 (c) (fixed by engine change #21 at B2SW); RO1 (d)
     (done at RO2); HU1 (b) (solo1 / solo2 IDENTICAL in every wire check
     since); REACH+R12 (a) (B2SW: a 56-deep notice taken by a rewind
     live); RK3 (a) (RK3H ran it); RK3H (c) (by construction, not a check);
     item 2's B2SW step P pair (moot), T1 (a) (BodyRevived retired at
     WG3), the RK3 harness gap TUTH (moot since RK3H), relay_late's
     covered frames (built at RLT), the session I/O writer (built at
     SIO / IOF), SFD (2) (answered and built at BALA).
   - MERGE (run inside another row): TR3b (f), SP6 (b), SP10 (c), REACH+R12
     (d), RK3 (c) and the BALA / BALL / WG3 re-captures -> REF12 (the
     protocol-22 reference set at the deployed defaults); RC1 (b), TR4 (h),
     SP10 (d) -> CPU-1 (HEAD-only CPU parity); REACH+R12 (c) -> RL1 (b).
   - STALE (replaced): RTX (c) (no verb stops the retail clock) -> RK3R-rec,
     a Level 1 reset's tutorial freeze taking the frozen-clock path; RC1
     (a) and B2SW's T4 (c) contrast (a pre-protocol-22 binary cannot join
     or replay: old-vs-new A/Bs become HEAD-only axes or absolute figures,
     plan §4 C2); RK1 (a) (no world freezes since RK3H) -> a forced rewind
     anchored at frame <= 9 across the exit at frame 16.
   - BLOCKED: RTX (a) (the retail `hold-event` verb, Stage C item 9; its
     headless half runs as B7EG's Q-H1 / Q-H2).
   - PARTIAL: TR3b (e) (TRL run B covered the trafo-on late joiner; H7's
     own criteria read from TRL's journals). TR4 (e): the tool is unclear
     (the retail mod cannot replay headless captures; plan §4 C11).
   - DECIDED at F0: RK3 (e) - the replay log line stays the guard
     (`record-format-gaps-design.md` "As built (step F0)").
   - **Not on this list before F0** (plan §4 C3: the deferrals of B5RX..RK2
     live in the design As-built notes and queue.md; the close-out runs
     them as these rows): EL6-EL9 (ELH: L-H1..L-H6 with same-batch
     controls; ELR: L-R2 retail; ELS3: a SimTool own-revive scene); B7EG
     (B7EG-QS: Q-U1/Q-U2 tests, Q-S1/Q-S2 scenes; B7EG-QH: Q-H1/Q-H2); B8EG
     (B8EG-R1: B-R1's control comparison, retail); RLM (RLM-2rw: two
     rewinds in one host frame; RLM-own: a multi-script owner scene;
     RLM-v14 built at F0); RLC (RLC-r: a retail rewind across a bridge
     break; RLC-o: the order pass live); WG3 (WG3-U, WG3-V4, WG3-H,
     WG3-dr, WG3-cr); WG4 (WG4-R1 arena retail + headless, WG4-R2, WG4-R4,
     WG4-rs); WG6 (the protocol doc, built at F0); WM0 / WM1 (the harness
     nits, built at F0; WM1-R3 retail late joiner; WM1-J3 with the
     launcher's server flags built at F0; WM1-J: J1b, J2, J4; WM1-U: U5, U6,
     U7 and the world-less `session_runner` fixture; WG2-Je); WGQ7 (V6
     x86 / Linux listings, V4b, H2b / H2c / H3b / H3c / J6, U9 live, the
     smoke's same-batch control, WAD-A; UGRD the server's unit-set guard
     and IDCP the identity copies' leftover outputs, built at F1; its
     harness criteria, built at F0); BALA / BALL / SPGH / SPGH2 / OWG /
     ASL / SPX (BALA-end, BALA-tr, BALA-F2, SPGH-tr, SPGH-a3, SPGH-pr,
     OWG-cell, OWG-dr, ASL-r, ASL-pl, ASL-cpu, BALL-pm, BALL-gp, BALL-deps,
     SPX-ct: its tools built at F0, the per-world read at H6; the SPX
     nits, fixed at F0); SIO / IOF (SIO-st, SIO-lx, IOF-c); TRL
     (`trafo_life_mismatches` 0 in every trafo-on run); RLT (RLT-c: a
     live `covered` > 0 and its retail field); RK2 (RK2-fan: a rewind
     across a fan slot claim; RK2-PH / RK2-S2: the decided option (b),
     built at F4); N01R (c) (the retail control of c1's tick-728
     difference); the delayed rejoiner's first state (DRJ, F2); the
     lifecycle-event arrival margin (LCM, F3); the trafo-on retail frame
     time (RCX-x86) and the deployed `config.yml` keys (RCX-cfg, DEP).
   - **Run at S1 (2026-10-29, HEAD fc1be258; SimTool / compile batch A,
     no source change):** SP7 (a) = V11 for V7 - the 24 runs (L8 / L11,
     arms `--perturb-ids 0 / 37 / 1000`, `--free-ids-before-load 20000`)
     on the x64 (`build-agent-rS1`), x86 (`build/sim-x86-a3`, rebuilt) and
     Linux (Arch) SimTool give one tick-hash stream per level, 501 lines
     each, byte-identical across arms and platforms (signatures
     `91a138e4fd6e7254` / `5d22d3b5a61c38cb`; the L8 20000 arm now exits
     0 - an observation, SP7's teardown crash there is item 6's and was
     not root-caused here). WGQ7 V6 - L1-L13 (13 = the arena in the
     agent's copy), placement off / on, K 2 / 8: V6-ALL PASS on all
     three; plus the ORIGINAL Level 13 (a scratch root with
     `Level_13.NMO.orig`, md5 = `original_map_hashes[13]`): every check
     passes but `table counts` / `table identities`, whose level-13 row is
     the arena's by construction (`probe_census.hpp` `expected_for`,
     :357-377), on all three.
     The 112 listings (24,798 lines, 2,140 signatures) are byte-equal to
     x64's with block ids NOT masked; only the wall-clock `cktime=`, the
     anchor line's FPU control word (`fpu=`, x64 0008001f / x86 0009001f /
     Linux 0) and Linux's VxMath start-up banner differ. SP8 (e)'s SimTool
     x86 half (§7.6, two repetitions, host load <= 10 %): placement on
     costs at most 0.19 ms per tick on x86 with every sector active (x64
     at most 0.14 in the same batch), on-off within the scatter the 0-tick
     runs' outliers make (about 0.3 ms); the anchor +1.0 to 2.3 s one-off
     as on x64. SP8 (f), RO1 (f), SIO-lx: the Arch GCC build
     of SimTool, the headless client and the unit tests (656/656 on
     Linux and Windows), and the new `.local-tools/linux_sim_syntax.sh`
     (sim_tool.cpp, session_client.cpp, probe_census.hpp,
     script_replay.hpp and io_queue.hpp alone: all ok; a deliberate error
     fails it). BALL-deps - A CAUSE (reproduced, not the only one): a
     `wsl.exe` running in the same console as a Windows ninja build
     switches the console to code page 65001; every cl started meanwhile
     prints its `/showIncludes` prefix in UTF-8, which ninja's GBK
     `msvc_deps_prefix` does not match, so the object is recorded
     `#deps 0` (scratch tree: 30-34 of 40 objects in 3 of 3 attempts, 0
     in 3 controls; a wsl in another tool call does not share the
     console; a tree configured under 65001 stores a UTF-8 prefix and
     loses every dep in later GBK builds). It does NOT account for the
     B6RC loss (x64 and x86 built in parallel, no wsl recorded) nor for
     the `deps_heal.log` mass heals of 2026-10-29 03:10 / 03:18 (27
     objects lost again inside the heal's own rebuild): those stay
     unattributed. So both rules hold - one tree at a time AND no wsl in
     the same command or script as a Windows build - and `deps_heal.py`
     (now also in `build_sim_x86.bat`) remains the guard. `ninja -t
     deps`: no zero-deps object whose source has a quoted include on the
     x64 and both x86 trees (excluded: the allowlisted
     `replxx/wcwidth.cpp`, and the 57 .c objects whose headers come
     through the precompiled header and legitimately record none), after
     seven SDL3 C objects of the x64 tree that really had lost their
     headers (built about 2026-10-19, cause not attributable now,
     skipped by `deps_heal.py`) were rebuilt.
   - **Run at S2 (2026-10-29, HEAD 3af8aaaf + SimTool-only tooling;
     SimTool batch B, replays on existing data; `.local-tools/work/S2/`,
     binary `build-agent-rS2`):** SimTool gained `--drop-kill-player`
     (V4b), `--late-ball TYPE X Y Z TICK` (RO2 (b)), the SP10-g count in
     `--list-placement --placement on` (with a whole-level control line),
     `--list-group NAME`, a `--savestate-trace` dump of a re-made live
     world that fails its premise (its own flag) and, with `--verbose
     --frames`, the selected scripts' owners held live before each rewind
     and the straight pass's membership per kept tick; nothing outside
     `BallanceMMOServer/sim` (656 tests, Linux SimTool syntax ok; the gate
     round's build `build-agent-rS2g`). **TR3b-d (H5)**, adapted offline: the four
     `core_ref_s12/h1_150` captures replayed with `--force-rollback`
     13:24 / 50:12 / 97:24 / 131:47 (16 replays, 16-169 forced rewinds
     each; a forced rewind needs `--replay-free-cost-clock`, whose control
     replays IDENTICAL): the 153 non-ball bodies (every piece of all four
     sets) 0 off at every LOCAL checkpoint; every body bit-equal at every
     confirmed FINAL row (D0); 0 "PreSimulate queue differs"; the
     replay-mismatch lines equal the control's. A mirror's LOCAL row
     differs over a few ticks after a forced rewind (a hold-last
     prediction re-made with a newer frame; the FINAL rows are equal).
     **Found (pre-existing, gap L, item 4):** a mirror whose death arrives
     late (its own rewind at D-1) is missing from the records of
     (A, D) re-captured by a later rewind of the same frame anchored at
     A < D-1 - forced (`swing_snapoff` c0 900-904, `h1_150` c0 1056) and
     an ordinary input-correction rewind alike (h1_150 1058 in the
     control too): the records the death's own rewind re-captured keep
     the old generation (`rollback.hpp` step_tracked copied from the
     record, only alive remotes refreshed), so `revive_ended_lives`
     (end = a generation change only) sees the life alive, no half
     revives it and the restore skips its blob entry: the replay runs
     without the body in [A+1, D) (no position effect in these runs: the
     mirrors were falling). **SP5-e:** the `h1_150` client journals hold
     13/13 of the server's Trafo events in their spans and replay with
     the trafo inputs, `checkpoint_mismatches=0`; `TRAFO_SERVER` resets
     of the FINAL kind are in client journals (`f1_live` c0 6, c1 3,
     replayed alike); a deadline `TRAFO_SERVER` reset falls after every
     client's last tick in every protocol-22 run (h1_150: 2491-2535 vs
     2168-2249): H1 owes an arm with one. A late joiner's client journal
     does not replay (`f1_live` c3 stops at its tick jump). **SP7-b V9:**
     L11 lead 180 move / no move, default lead (the retail-ball park at
     204 inside the window) move at 80 / no move, L8 the same: 50/50
     each, 0 gap F lines. **RO2-b:** L8 K=2 (T 60, A 69) with a wood ball
     dropped off-centre and a stone ball onto the late `P_Modul_41` row
     after A (the blocks tilt, the balls roll off), and L11 K=3 with a
     stone riding the late swinging sack `P_Modul_26_Sack`: every body
     bit-identical to the on-time control at 301/301 boundaries; the
     structure differs as RO2 measured (L8: movable 18, hull/collision
     83-245, OV 301 boundaries; L11: world, units, OV 301). **SP8-c (the 736 death):** the killer is the
     level depth cube `Quader01` (`DepthTestCubes`, world box y -208.3 ..
     -87.5) through `Gameplay_Ingame/BallManager`'s Box Box Intersection
     on `Ball_Wood` (`Deactivate Ball`): a static level object, no PH
     row, no copy, the same in every world; the copies' `DepthTest`
     group is the props' reset group, not the ball's death test, so
     hypothesis (i) (an s3 or copy volume) is refuted and the ball died
     of its fall through `P_Modul_37_Bridge_R30` (ii). Not explained: the
     ball was inside Quader01 from about 663 and died only at 736, the
     tick after `sector +3`; needs a trace of the BallManager iterator in
     c2's world (a capture replay with the SP8 binary; the server world
     does not run that test for a clone). **SP10-g:** 0 object-valued
     attributes naming outside the hierarchy on L1-L13 (23-62 copies a
     level), and 0 by absence: the filter's control over every
     behavioural object of each level (1073-1809) finds the six
     object-valued attribute types registered (Occluder, Pickable,
     PickableScreen, PhysicsScript, ObjectToReset, RepresentFrame) with 0
     holders in any level, so step 8's remap has nothing to remap in the
     retail levels; the value read itself (`GetReadDataPtr(FALSE)`) has
     no holder to exercise (gate round, `S2/gate/lp_L*.txt`). These lines
     are printed by the x64 SimTool from S2 on only; S1's platform
     comparisons read the `tickhash` lines, not whole listings. **RK1-d:**
     `--resync-check` A2 cases (l11_ER, l11_A5, arena_A4, arena_ER, every
     arm PASS, RESULT lines identical to B2SW's). **S1b with
     `--session-recipe`: NOT RUN TO A VERDICT, open.** At the session start
     (`--drop-settle 0`) every run (L2, L13, seeds 1-5) stops at round
     3-4 on the harness's premise ("the re-made live world at e is not
     the straight pass's", then a restore error on a window-born
     `P_Ball_*_MF`), with C3 replays 0-1, so A18's frame-2 globals write
     is not tested. Cause (the dumped worlds): the re-made live world
     (`sim_tool.cpp` 4196-4246; it puts back t0's globals only, 4229, as
     the trafo re-make at 4156 does) lacks the level start's frame-2 globals
     write (straight: gravity -20, factor 2; re-made: -9.81, 1) and the
     outside-the-selection Init Ingame activation of sector 1 (births at
     7) - a SimTool harness gap, not a client one (a client replay puts
     back each tick's recorded globals, C3; H1's H4 measures the client
     case live); the v14 recipe fails there too (A23). The settle-12
     runs (60/60 on L2 and L13, seeds 1-5) print `globals (C3) set=0`:
     their windows hold no globals change and do not cover A18. Fix
     (SimTool): re-make the live world with the straight pass's globals
     put back per tick and the outside-selection activations re-issued;
     then rerun S1b. **WG3-V4:** WG2's four scenes on the clients' recipe
     50/50 each (the 120-tick dead window across the park at 204 and the
     bridge break both pass now: RLQ, RLC); the `swing_snapoff` 13:24
     forced replays (the clients' records with the hold) as TR3b-d.
     **WGQ7-V4b:** two paper balls in the P_Modul_18 box, K = 2, one ball
     killed in the lead: 50/50 (player 2 killed), 50/50 (player 1). **The
     alive scene FAILS 8/50** (seed 1, chained rounds; pre-existing: the
     same 8 rounds with one ball, the bit off, K = 8 and on rRK2 / rRK1 /
     rRLQ; the schedule as named rounds 50/50): `P_Modul_18_Particle
     Script` flags 0x400000 (DEACTIVATENEXTFRAME, kept by the straight
     pass) vs 0, and the RLM witness 0 vs 1, in exactly the rounds whose
     window lies inside [34, 67]. Cause, root-caused at the gate round
     (`--verbose` now prints each selected script's owner held live
     before a rewind: `S2/gate/v4b_alive_diag.txt`): the replayed frame of
     tick 68 runs a replayed script's activation of the particle
     script (`CKScene::Activate`: +1 in the behaviour manager's
     `m_BeObjectNextFrame`); only a live frame's `ManageObjectsActivity`
     consumes it, and the RLM ledger takes back only the restore's own
     `reregister` adds (registrations 0 in this run). With no live frame
     between SimTool's chained rounds the owner's pending count grows by
     one per round replaying tick 68 (0 at the straight end, 1 after
     round 0, 3 at round 8, 10 at round 31); a later round anchored
     before 68 then finds the owner pending, `execute_scripts` admits it
     (`owner_pending_add`, `script_replay.hpp` 1592-1596) and consumes the
     DEACTIVATENEXTFRAME the live frame kept until 68. Named rounds re-make
     the live world with whole frames, which empty the table. A host
     reaches it with two rewinds in one host frame, the first replaying
     an activation tick and the second anchored before it (RLM-2rw's
     case; one rewind per frame is consumed by the next live frame). The
     records differ in that bit only (blobs and hashes equal); the fix is
     shared (`script_replay.hpp`): queued in item 4. "Across the death"
     inside one window needs a
     lifecycle event replayed in a savestate window (SimTool sets events
     up before the window): S3. **SPGH-pr:** the REF12 live journals are
     the pinned replay (the wire check replays them IDENTICAL with the
     live input records); own ball vs the server's rows over every
     respawn window: resync_snapoff c0-c2 bit-equal from the respawn to
     the airship, c3 bit-equal 2692-3163 then a LOCAL-row contact
     prediction 3164-3206 (FINAL rows bit-equal, D0); swing_snapoff all
     four bit-equal over both respawn windows.
   - **Run at FX2 (2026-10-29, HEAD cd77a19b + SimTool only; S1b to a
     verdict, record-format-gaps-design.md "As built (step FX2)";
     `.local-tools/work/FX2/`, binary `build-agent-rFX2`):** SimTool's
     re-made live world now gets, before each re-stepped frame, the
     globals the straight step ran with (the record of t+1's) and the
     activations the straight pass's frame consumed that its own world
     lacks (Init Ingame's Execute Script of Gameplay_SectorManager at the
     session start); nothing outside `BallanceMMOServer/sim`. **S1b
     (`--session-recipe --drop-settle 0`, L2 and L13, seeds 1-5): 10/10
     runs to a verdict** (60 rounds each, window 1..40), C3 replays 1-2
     per run (12 in all, every one anchored before the first change at 2),
     0 "gravity differs", 0 clock failures, 0 premise failures (S2: every
     run stopped at round 3-4 on the premise); 165 re-made worlds, 161
     per-tick globals writes, 145 outside activations re-issued, 0 the
     other way. **Not 600/600:** 20
     of 600 rounds fail, all of one harness residual (below). Regression
     arms PASS with 0 re-issues and 0 globals writes: S1b at
     `--drop-settle 12` 10 x 60/60, V4b alive 50/50, V9 L11 move (default
     lead) and L8 move (lead 180) 50/50 each (a union pass inside the
     re-make, never re-issued), EL9's L-S1 trafo scene 160/160 (21249
     re-made steps; the trafo branch's per-tick globals put is built but
     its window has no globals change, so no write is exercised). Tests
     656/656, GCC 31/31 and `linux_sim_syntax.sh` 5/5 ok.
     **Residual (harness only, queued here):** every failing round
     compares at a tick e <= 2 (t0 = 1), on Gameplay_SectorManager only:
     (i) depth 0 at e 1 or 2 - its delayed link `Deactivate Sector.out0 ->
     Set Cell.in0` (delay 4) "countdown 4 vs 0": the straight pass's
     record holds the initial 4 (never queued yet), the stand-in live
     world 0 (fired in its own later history), and the restore writes a
     countdown only when the record has the link in flight
     (`script_replay.hpp` `restore_behavior`); (ii) anchor 1 depth 1
     ending at 2 - flags (the pending activation from Init Ingame's frame
     1), the same countdown and `CurrentLevel[0][4]` 1 vs 0 (Init Ingame's
     write): the round compares at e before what a client does at a
     rewind's end (`live_cells_guard`: the live cells written back, the
     pending activation re-issued), B2SX's family. Not a client gap: a
     client whose live tick is <= 2 has never fired the link, and named
     rounds 1:2:2, 1:3:2, 1:4:2, 2:1:2, 2:2:2, 2:3:3, 2:4:2, 3:1:2,
     1:12:2, 2:12:2 (L2 and L13) - replays crossing frames 1-2, one with
     the link in flight live - pass every replayed tick, the end and the
     follow. The two forms split (FX2's review): **(i) is a harness
     fidelity gap, queued here as its own harness item** (the
     orchestrator's to schedule, no user decision): the round's live
     world takes the straight pass's not-in-flight delayed-link
     countdowns of e before the round, re-made or chained (12 rounds);
     **(ii) is the user's rule question** (8 rounds, not decided here):
     (a) leave it, S1b counted on its own kinds (globals and clock,
     §7.2); (b) the round's compare at e runs after the client's
     end-of-rewind steps (a change of the rule the round is judged by,
     as B2SX's option (a) was). **Both BUILT at FULL step DX4
     (2026-10-29; the user "你决定" -> (b)):** SimTool's round compares at
     e after the client's end of the rewind (live_cells_guard's steps in
     its order: the cells written back, the live frame's activation
     re-issued through manager_activations::replay), its live world takes
     e's not-in-flight countdowns and e's three guard cells (the cells: an
     adaptation the run found); S1b 600/600, FX2's regressions unchanged;
     a follow across frame 1 (named 1:0:2) is a harness limit, not a
     client gap (record-format-gaps-design.md "As built (step DX4)").
     **Queued (DX4's review, one implementation):** SimTool's
     end_of_rewind is a copy of live_cells_guard's destructor
     (game/session_host_support.hpp; SimTool has no
     session_client_core). The next step that touches that header (or
     R7) moves the destructor's body into a free function taking the
     context, the activations, the kept cells, `rederived`, `rolled` and
     a re-issue callback, which the guard and SimTool both call; until
     then a comment at the guard names the copy.
   - **Run at H1 (2026-10-29, HEAD 193c9a5b + two test switches of the
     headless client; headless batch 1; `.local-tools/work/H1/`, x64
     `build-agent-rH1`, the Arch server `ballanced-36c04e3d24c9+bmmo-193c9a5b3878`
     (no maphack), every run snapshots off, trafo inputs and placement
     on unless named, captures under `maps/arena-test/h1_full/`):** new
     `--reset-level-at T|b+N` ("Reset Level" to Level and All_Balls, as
     the retail `restart` verb's message) and `--activate-script NAME:T|NAME:b+N`
     (session_client.cpp; off by default; wire check rS3 -> rH1 over
     core_ref_s12: 36/36 journals IDENTICAL, ALL SAME; 661 tests; GCC sim
     syntax ok). 12 live runs. **TR3b-a** H1 at 150 ms: option on
     pieces 150/150 on all four, starved/rebases/far/resyncs 0 against the
     off control's 0, late 0, missed 0, leaks 0, server journal matched;
     the on arm's VERDICT FAIL is client 3's 18 relayed frames past the
     reach in its last 11 ticks: when client 0's run ends (2121) it stops
     sending without leaving, the server waits ~340 ms for its frame
     before holding the last one (TICK gap 342 ms at 2122; the control
     shows 351 ms at 2111), so every frame relayed meanwhile is late by
     that much and the last client's lag (21-28 before) climbs to 50
     (reach 48) - an end-of-run harness artefact, in every 150 ms H arm
     (REF12's h1_150 c3 45/42, TR3b's 42/42), not a world difference.
     The pair's band (per client, on / off): t_rep 741-1031 / 370-527 ms,
     t_max 48-50 / 14-15 ms, t_cap 1576-1639 / 889-922 ms, t_rest 94-130 /
     30-43 ms, inrew 23-34 / 19-30, server step p99 0.8-3.2 / 0.6-0.9 ms;
     TR3b's first pair: t_rep 990-1150 / 340-410, t_max 43-46 / 12-14,
     t_cap 1690-1770 / 880-920, inrew 25-35 / 21-30 - the same band.
     **TR3b-b** H1 at 0 ms: PASS, pieces 132/132 on all four, late Trafo
     events 0. **TRL-lm** `trafo_life_mismatches` 0 in every trafo-on
     server log. **TR3b-e**: `trl_live_B2`'s joiner kept all 12 sets (no
     remake to read), so H7 was run (`h7_join`): the reconcile remade 6
     sets incl. both paper sets; wind present (identical wind velocities);
     **pieces 73/102 - queued in item 4 above**; its VERDICT names 12
     late frames of the joiner at the join (lag 55/48: RL1-a's joiner
     lag, H3's). **SP7-c** L8 cross at 150 ms (`l8_cross_150`, PASS live,
     signatures and sector ticks equal): plain replays IDENTICAL;
     `--force-rollback 10:24`: 45/45 bodies equal to the control at every
     LOCAL and FINAL checkpoint, `birth_recreate_failures` 0, but client
     2's own ball (and every mirror of it) 0.41 m off from 758 - the
     chained-rewind deferral leak queued in item 4 above. **RO1-g**
     (`ro1g_l8`): client 0's resync S 322 -> 348 holds the s2 / s4 passes,
     "rechecked 0+3 deferred" under the travelling blob, 0 refused; its
     bodies after the resync differ from a non-resynced client exactly as
     in the run without a resync (the see-saw's mirror effect, 0.027 m);
     the capture replays IDENTICAL (the arena's `ro1g` holds no fixed
     birth: 0+0). **RLM-2rw** (`rlm2rw`, client 1 `--force-rollback 5:12`
     live): 2 host frames with a natural and a forced rewind (720, 860),
     the capture replayed with the switch IDENTICAL, sends SAME;
     taken back 0 (neither second restore anchored before a replayed
     activation). FX1's ledger exercised offline instead: `h1_150_on`
     client 0 with `--force-rollback-at 844:29,862:30` (each in the frame
     of a natural rewind whose window holds a trafo explosion, anchored
     before it): `replayed_adds=2 replayed_taken_back=1`, LOCAL and FINAL
     rows equal to the unforced replay at every checkpoint (153 bodies,
     4 balls) - the ledger's first exercised take-back. Pieces were
     compared only in the arms with explosions (h1_150_on, h1_0_on,
     h7_join); the Level 1, `rlm2rw` and `qh1_a6drop` runs had trafo
     resets only (no piece moved). **RK1-a/b/d** (`l1_main`, base 27, offline from the
     captures, clients 0 and 1, `BMMO_TRACE_TIMEFACTOR=1`): 47:19 (anchor
     28, H4's first record), 46:19 (anchor 27, across the frame-2 globals
     write: `globals=2/1/1/0/0`, RK1-d's rewind across a change) and 50:16
     (anchor frame 8, across frames 9..16): LOCAL and FINAL rows equal to
     the control at every checkpoint, no factor-0 write, no added "differs
     from the live record" (8 = the control's). **RK3-b** (H2, client 1
     beamed onto the ExtraLife spot (-170.978, 21.5, -137.735) at 2001 -
     beamed once, not held): frozen 0, resyncs 0, `globals=0/0/0/0/0` on
     all four; client 1 equal to the server from 1990, mirrors from 2040.
     **RK3F: shows** (`l1_reset` client 2, `--activate-script
     Gameplay_Refresh:b+10`, i.e. frame 12): Gameplay_Refresh writes
     factor 0 at tick 38 (frame 12) in that world alone, until the exit's
     2.0 at frame 16; frozen 0, no resync; the level props differ from
     tick 38 on for good (P_Ball_Paper_MF* 0.56 m, P_Ball_Wood_MF 0.37 m,
     boxes 4-18 mm) - the plan's §3 D5, the user's (widen the hold or
     accept); the trigger is a direct activation, standing in for a
     respawn / pause resume in frames 9..15. **RK3R-rec** (`l1_reset`,
     `l1_reset2`, client 3 `--reset-level-at 1500`): own ball
     unphysicalized at 1536, own trafo resets stamped and applied by the
     server at 1538, the sector manager told to reset, Event_handler /
     Ingame / Refresh / Tutorial (its held Init) write 2.0; no new own
     ball in 940 ticks (own_phys 0 to the end; a beam at 1700 found no
     body), so no chapter, no freeze, no resync - the headless switch
     sends the message only, not the menu's restart output; RT2's retail
     `restart` is where the full path is recorded. **B7EG-QH Q-H1**
     (`qh1_a6drop`, client 2 `--hold-event physicalize:2:drop`): cbq
     held 61 over 68 holds, `unhold_clamped = rebuild_fail = hold_fail =
     no_record = 0`, `sv=N/0/0`, 0 "PreSimulate queue differs"; client 2
     resyncs once (the drop arm's own-birth resync, expected); forced
     rewinds at 2400/2450/2600/2680 (`--trace-restore`): rows equal to
     the control at every LOCAL and FINAL checkpoint; the queue equal
     after two, and after the two in the bodiless span short of the
     bridge's own `Ball Navigation` callback (bridge entries are never
     held; replayed frames do not run Gameplay_Ingame, which queues it;
     `rearm_own_navigation` returns early without an own body; the next
     live frame queues it again - no body effect). **Q-H2 not run -
     deferred**: the design settles its recipe and pass
     (rollback-engine-gaps-design.md §7.3 Q-H2: `drop-body <entity>
     <from>:<to>` on client 3 for P_Modul_34_Kiste and P_Modul_08_Schaukel,
     e8ab2_swing_n1 sector 4, 150 ms, 10 % loss; `sv=N/0/0`, no queue
     growth, one controller per force block, the dropping client's
     mechanism rows equal to its control's), but the verb does not exist
     (the gate-round review's finding; the run's first report wrongly
     called the case unsettled). Building it needs it tick-keyed and
     re-applied in replayed frames (a live-only drop is undone by any
     restore anchored before FROM, which the pass criteria rule out), plus
     one live run with its control - over H1's planned 11-12 live runs
     (12 used). Post-N4 no session queues a selected entry naturally
     (B7EG), so the #20 hold of selected entries is exercised only by the
     SimTool scenes (Q-S1) until then.
   - **Run at H2 (2026-10-29, HEAD 12ccae76 + a SimTool-only switch;
     headless batch 2, sectors and placement; `.local-tools/work/H2/`,
     x64 `build-agent-rH2s` (the headless client is HEAD's, `rH2` without
     the switch), the Arch server rebuilt from the tree
     `ballanced-36c04e3d24c9+bmmo-12ccae76aabc-dirty` (no maphack; the
     dirt is SimTool's, which the server does not compile), every run
     snapshots off, trafo inputs on, placement on unless named, captures
     under `maps/arena-test/h2_full/`; 10 live runs, each judged before the
     next, host load <= 19.8 %):** new SimTool `--placement-policy-check`
     (with `--list-placement --placement on`; `placement_tool.hpp`
     `list_placement_policy_check`, refused elsewhere; the default listing
     byte-identical to rH2's; it calls `prepare_unplaced` directly, not the
     host-facing `place_at_anchor(active = false)`, whose option-off branch
     is a one-line call of it; gate round: the test scene is launched and
     left with `CK_SCENEOBJECTACTIVITY_DONOTHING`, then removed from the
     level and destroyed, and the listing after it is labelled as the
     unplaced world - PASS again on L8 / L11 x64 and L8 Linux, the default
     listing still byte-identical; a wire check `rH7X` -> `rH2` over
     core_ref_s12 certifies the x64 headless binary rebuilt after an
     LNK1163 corrupt object: WIRE-CHECK: ALL SAME, 36/36 journals
     IDENTICAL). **SP6-c (headless + server): PASS offline;
     no live path reaches it** - the headless client releases its copies
     at `begin_session`, before an engine reboot and in its destructor, and
     the server's world is made fresh per session and releases in its
     destructor, so `prepare_unplaced` always meets a level without copies
     there; the shared helper was exercised in SimTool on L8 (39 copies)
     and L11 (53), x64 and the Linux SimTool (GCC, Arch tree): the registry
     that made the copies releases them (inventory equal to the unplaced
     world, signature `629f6128b4fc8389` / `4f444149dc156465`, holds_copies
     no, 0 objects of a copy's name left), a registry that made none is
     refused with "the level still holds placement copies (row 15 names
     P_Modul_30_MF_R15)" and leaves the placed world as it was, and an
     apply with a non-level scene current is refused ("the current scene
     is not the level scene ...") with the world unchanged. The retail half
     stays RT2's. **SP5-f** (`sp5f_mismatch`, arena, client 3
     `--placement-signature 0000000000000001` at the start): the server
     logs "placement mismatch for Arena3 (client 0000000000000001, server
     ecfbfd9b63a330b5)" right after the world is ready and **ends the whole
     physics session** (`server.cpp:2065-2069`, `end_physics_session`): no
     member reaches tick 1, room 1 is kept (reset) and the server runs on.
     That is design 5 / 7.4's "ends the session ... the same as a late
     joiner": **PASS as designed** (the design settles it). The plan's
     wording "refuses that member, the room runs" contradicts the design
     (plan §4 C15); the server refuses a member (out of the session, the
     rest go on) only for the session core and the probe signature (WG3's
     review), not for the placement. Not changed. A proposal for the user,
     blocking nothing: refuse the member like the WG3 probe check
     (`send_session_end` to it + `physics_session_member_left`, a few
     server lines, no protocol change) - today one misbuilt client ends
     everyone's session, at the start or as a late joiner. **Built at step
     DX7** (the user's answer 8 of 2026-10-29: the member alone is refused,
     with `placement_mismatch_reason` echoed to it; a start member and a late
     joiner refused in each of four arena sessions while the two others
     played to the end, server journals matched; sector-placement-design.md
     §12.9). **SP6-a** L8 late (`sp6a_l8_late`,
     0 ms, client 3 `--join-late 3:900`, then its own `+4`): c3 joins with
     the state of 1004, `level walk seen at tick 1008` before its owed
     `+2` (1014) and `+3` (1021), its own `+4` at 1109 on all five sides,
     signature `91a138e4fd6e7254` (39 copies) everywhere, `mechanisms=45/45`
     on all four; after its apply (1026) the joiner is exact on all 45
     bodies; server journal 3154/3154. **SP8-a** L8 cross at 150 ms
     (`sp8a_l8_cross150`): five sides 39 copies / `91a138e4fd6e7254`, walk
     30, `+2` 335 / `+4` 342 / `+3` 735, 45/45, 0 rebases / resyncs / far /
     starved, journal 2585/2585; every residual over 1 mm is a single
     checkpoint at a remote beam (R48 0.126 m, R15 0.033 m, the s2 bridge
     0.014 m at 420/520/580), exact at the end; the networked invariant
     (`SP8/inv.sh`, rH2s) **TRACE IDENTICAL** (31020 lines, 4 Sector events
     dropped). **SP8-b** L11 cross at 150 ms, on / off control
     (`sp8b_l11_150_on` / `_off`, the same recipe): on - five sides 53
     copies / `5d22d3b5a61c38cb`, walk 30, `+2` 335 / `+3` 735, 54/54;
     off - `4f444149dc156465`, 0 copies, the same ticks, 40/40; both
     journals matched; per body present in both (40): **on worse than off
     for none** (on: worst `P_Modul_34_Schiebestein001` 0.0138 m, one
     checkpoint; off: `P_Modul_25_Bridge` 0.303 m, `P_Modul_37_Bridge`
     0.228 m, `P_Modul_25_Bridge001` 0.074 m, and `Schiebestein001` /
     `Kiste001` 0.3 mm at the end); the networked invariant on the on run
     TRACE IDENTICAL (45900 lines, 5 dropped). **SP8-c** L11 at 0 ms
     (`sp8c_l11_0_sack`): client 2 beamed onto the s3 sack copy at 1101,
     its ball in friction contact with `P_Modul_26_Sack001_R35` at 1122,
     1125 and 1134 (client2.log's ball contacts; the copy swings from 1092),
     all 54 bodies 0.0000 m on all four clients at every LOCAL checkpoint,
     the sack copy and its rope included; **c2's death at 736 (SP8 (g),
     S2's open latency) explained**: client 1's ball, beamed into a free
     fall in s2 at 601 with no sector event near (`--sector-at 3:1000`,
     entered at 1009), enters `Quader01`'s box at about 660 and dies at
     729 - the same ~70 ticks as sp8's c2 (box ~666, death 736) at another
     depth and speed. The delaying block (gate round, SimTool
     `--describe-script Gameplay_Ingame` on L11): the BallManager's
     `Box Box Intersection` (id 26972) True -> `Deactivate Ball` (27282) ->
     a Send Message chain (one link `delay 2`) -> `Get Cell` -> `Test` ->
     `reset Ballpieces` -> `Überblendung` (its out through a Nop at once)
     -> **`Delayer` 27109, Time to Wait 1000 ms = 66 ticks** ->
     `Physicalize` (27103) Unphysicalize, the "own ball unphysicalized"
     line. So an unphysicalize at 736 needs the depth test to have fired
     at about 736 - 66 - the link delays = ~667, some 68 ticks before the
     `+3` at 735; the sector pass cannot have started it. Not the sector
     pass; SP8 (g) closed. (The plan row's method, the server journal
     replayed with and without the s3 Sector event, cannot show it: the
     server world does not run the depth test for a clone, §12.7.)
     **RC1-c** (4 drift sessions, arena, 150 ms, dwell 30, the deployed
     server defaults - RC1c ran trafo and placement off; old-vs-new is
     impossible, plan C2): `cliff` x2 and `resync_at150` x2: 0 rebases, 0
     resyncs but `resync_at150`'s client 0 (`--resync-at`, its arm), far 0,
     journals matched. Drift (`H2/summarize_az.py` = RC1's summarize.py
     with the airship exception: ball rows with x >= 905.25 and the
     `PE_Box_slide` are not compared, as BALN decided; the filter is
     one-sided - a row is dropped when EITHER side is at x >= 905.25, so a
     ball that reached the zone in one world only is hidden from that tick
     on, not from when the airship takes it): own ball 0.0000 m
     on every client but cliff c3 3.52 m (98 checkpoints) and resync c3
     1.06-1.15 m / c0 0.52 m (the forced resync's 892..920 window), each a
     span that heals to 0 (live-written rows before the confirming rewind,
     plan C1); mechanisms 0.0000 m outside c0's resync window; remote
     prediction mean 0.014-0.113 m, p99 0.13-2.92 m. Unfiltered, every
     own ball shows 110-115 m: the drive ends at the far end, where the
     owner's world (the "out of the sync from here" log line) lets its own
     ball fall past the airship platform while the server and every mirror
     hold it at x 905.29-905.36 - the decided airship exception, not a
     drift. On clients 0 and 3 (the ones compared) both cliff sessions
     reproduce REF12's `cliff` own-ball error spans tick for tick (first
     and last tick of every span; the maxima of the live-written rows
     within a few cm). Rewinds 7-15x fewer than RC1c's table (recorded in
     rewind-cost-design.md's RC1c entry, not explained). Tables:
     `H2/rc1c/*.txt`.
   - **Run at H3 (2026-10-29, the headless client of the tree committed as
     00082de8, `build-agent-rH2s`, and H2's Arch server built from
     12ccae76 - the same client and server code; headless
     batch 3, resync lag; `.local-tools/work/H3/`, `h3_run.sh`; every run
     snapshots off, trafo inputs and placement on, captures under
     `maps/arena-test/h3_full/`; 7 live runs (the seventh,
     `rl1b_waitother`, in the gate round), each judged before the next,
     host load <= 16.4 %, every server journal N/N: 4036, 4574, 4078, 4083,
     3673, 3672, 4062; no code change):** RL1 (a), (b), (c), (e) in
     late-player design 2.8, "As built (step H3)"; F2-a / F2-b in
     `resync-and-sector-timing-design.md`, "As built (step H3)". RL1-a:
     the joiner's base on the schedule (deadline 47.3 periods after the
     step it was computed at = its lead 31 + D 17); "signed schedule tick
     logged" only partially met - the server logs no such line, the tick
     is derived from the server journal; its lateness +4.1 and relay lag
     24 against the start members' 20 - at the "+ 4" bound on the steady
     windows, + 5 in the first (join-transient) window. RL1-b: the
     stall-triggered resync with lead_adjust and patience on - re-step 28
     ticks, apply frame 28 ms, lateness -0.3 of the others', no second
     resync; both of RL1b's non-stall variants: while the server lagged 26
     ticks (backlog) own correction 0.532 m, while it waited on a different
     member at its deadlines (patience and lead_adjust off) own correction
     0.0000 m, held window 43 = lead + D, re-step 48 ticks; each followed by
     a pacer slip of the headless host (1.5 / 0.85 tick, every other
     member's relay lag + 1-2; cause inferred from wall clocks: the
     headless pacer's origin restarts at the end of the overrunning frame
     that handled the assignment; retail sets no such hook). RL1-c: D 6,
     no shortfall, but the resynced LAN member stays 6 ticks ahead of the
     start members for good (its relay lag 6 against 0): the LAN start
     members lose their start base (inferred from wall clocks) because the
     core sends SessionReady
     before its anchor work (journal, airship zone, spawn ghost, a trafo
     piece set per member: ~130 ms), which on a LAN outlasts the
     assignment's round trip. RL1-e: not run, unreachable with these
     binaries (window 121-409 ms, a joiner's admit -> first frame 216-523
     ms); the rule is unit-tested, the runner's start path is not. F2-b:
     PASS (the made-from-row joiner's FINAL rows equal the server's from
     its apply to its end). F2-a: root-caused on the control (a ~124 ms
     stall of the joiner process right after its SessionReady ate the 212
     ms LAN budget; with lead_adjust off nothing re-paces a member whose
     frames are late), not reproduced on HEAD (also not with a 300 ms
     runner stall at the join).
     Queued from H3: **(item 2)** the F2-a re-pacing gap - the user's
     choice of (a) lead_adjust on by default, (b) a resync after N starved
     reports, (c) a larger LAN joiner floor, (d) accept; **built at step
     DX6** (option (b), the orchestrator's choice: 330 starved reports in a
     row ask for a resync; a LAN joiner stalled 300 ms after SessionReady
     resynced once and was starved 0 after; resync-and-sector-timing-design.md
     "As built (step DX6)"); **RL1 LAN excess**
     (the cause above; options: send SessionReady after the anchor's work -
     a shared-core change, which also takes that work out of a LAN
     joiner's budget, F2-a's "normal ~100 ms" - or count the work into the
     LAN start base; **fixed at step DX5**, the user's option 1: LAN start
     lateness 6.0 -> 12.5, the resynced member's relay lag excess 6 -> 0,
     late-player-design.md 2.8 "As built (step DX5)"); **harness: the resync pacer slip** (the headless
     host's `numbering_restarted` hook restarts the pacer's origin on an
     exact resync; fix: keep the old phase as retail does, or take the
     origin at the assignment's arrival; not a user decision, it does not
     reach retail; until fixed, headless relay-lag numbers after a resync
     carry + 1-2; since DX5 on a LAN the resynced member is the latest one,
     so the slip costs every member + 1 relay lag after any resync; **built
     at step DX9**: the phase kept, the others' relay lag 20 -> 20 after a
     resync, H3's 20 -> 21 gone); **harness: a pre-booted late joiner** (world anchored
     and held before its Join, the Join sent at the all-ready moment), then
     RL1-e's live check (**built and run at step DX9**: `--join-cue` /
     `--join-late N:cue`; RL1-e PASS - the joiner re-anchored 228 ms before
     the schedule's start at base 30, the signed rule, against 46 under
     RL1's clamp; FINAL rows exact); **server: one log line in the re-anchor path**
     (`session_runner::reanchor_tick`: the signed schedule tick, the floor
     and the base; **built at step DX9**; all three: late-player-design.md
     2.8 "As built (step DX9)"); live-only
     server steps of 0.28-1.07 s inside steps that log (`rl1c_lan_resync`
     2059, the airship line; `f2_ctrl_B` 1638), not established, the
     likeliest cause B2SW's 9P class (`server.log` on `/mnt/d`).
     **(item 5)** P_Modul_37_Bridge on the session's first member, 20 mm at
     1482 (`f2_rejoin_B` client 0, deterministic in its capture's replay
     with every build back to rD0), cause not established.
   - **Run at H4 (2026-10-29, the same client and server code as H3:
     `build-agent-rH2s`, the Arch server built from 12ccae76 with the test
     flags; headless batch 4, members, rejoins, I/O; `.local-tools/work/H4/`,
     `h4_run.sh`; every run snapshots off, trafo inputs and placement on,
     captures under `maps/arena-test/h4_full/`; 17 live runs, each judged
     before the next, host load <= 24.8 %, every server journal N/N; the
     clients judged on their FINAL rows from each capture's replay, the
     airship spans and the balloon out; no code change):**
     `world-agreement-gaps-design.md`, "As built (step H4)". PASS: IOF-c (a
     fresh / stripped config gets `session_io` written back; a 300 ms spike
     armed from the base line, its lag visible), RLT-c (covered 24-96 with
     late 0 in every join run), WGQ7-ctl (bit on / off: late 0, covered 0,
     FINAL rows exact in both), J1, J1b seeds 1 / 4 / 5, J2 (order 1
     reused, the dropped ball bit-equal on the resting one in all five
     worlds), J3 (adapted hold 4150 ms: parked 1 on each member, released
     inside the history, FINAL rows exact; 300 ms parks nothing - the
     joiner's first event comes ~258 ticks after its member message), J4's
     join half, U9 (the rejoin waits for a released order, the 9th member
     is refused "physics session full (8 mechanism slots)" and stays in the
     room - reached by a reload between the room's creation and the start:
     a reload after the start cannot raise a room's capacity, design §2.10
     corrected in the note), SIO-st (server writer delay 2 s: queue peak
     408 KB, starved 0, resyncs 0, 4/4 capture replays byte-identical),
     WG2-Je (candidate set drift 0 in every leave arm). Known residual seen
     live for the first time: S3's U5 (c), tied to D4 (plan step DX8: the
     user's "你决定", the orchestrator's (b') with U5 (c)) - J3 at a 4500 ms
     hold releases the parked birth outside the history, applied live, no
     resync, the joiner's mirror 12.7 m off for good.
     Queued from H4 (each pre-existing - no code changed - root-caused at
     the gate round, and none a user decision): **(item 4) the import
     re-step leaves a mirror undriven before its `driven_from`** - J1b
     seeds 2 and 3, the late joiner's mirror of the braking leader
     0.05-0.87 m off from the import and up to 13.8 m later (clients 0-2
     exact, inputs identical): the re-step from S drives a remote only from
     its `driven_from` (`client_core.cpp` 68-74 `replay_remotes`) or where a
     live record names it (physicalized and navigated only), and the
     joiner's live world set `driven_from` after S by either writer - seed
     3: the mirror's catch-up birth, repeated under the loss, made live at
     1669 (S 1655, `client_core_lifecycle.cpp` ~805), undriven 1656-1668;
     seed 2 and j1 alike: mirrors made at S+1, keys learned at S+3,
     `discover_navigation` (`client_core_frame.cpp` 692-697) moved
     `driven_from` to S+3 before the import. An undriven mirror keeps the
     live prediction's keys (hold-last of the newest relayed frame at the
     import): j1's was a brake (1663, the true keys of S+1..S+2), so j1 hit
     the same gap with the right stale keys; seeds 2 / 3 held forward
     (1709 / 1685). The fix shape: at an import (and every exact resync),
     `driven_from` = min(`driven_from`, S + 1) for every mirror with a row
     at S (`client_core_resync.cpp` ~497 `rows`) - not keyed on the local
     `born_tick`, which is 1669 > S for seed 3's mirror - and the discovery
     rule must not move it past S + 1 for a discovery after the import;
     the fix step also handles a mirror not navigated at the import
     (`replay_remotes` skips it) and a replayed tick with no relayed frame
     in (S, t] (seed 2's ring starts at S + 2); then j1b seeds 2 / 3 on
     their captures (**built at step DX10**: the rule, `born_tick` lowered
     to S with it, a mirror not navigated yet holds the state like a
     missing body, the no-frame case named; H4's captures of j1b seeds 1-5
     and j1 replay with the joiner's mirrors 0.0000 m - seed 3 13.84 -> 0,
     seed 2 2.14 -> 0; world-agreement-gaps-design.md "As built (step
     DX10)"). **(item 4) a removal in a rewind's window drops the
     friction system the removed body shared** - J4's probe half: client 1
     leaves in contact with the moving P_Modul_01 Pusher; the Pusher
     bit-equal through 1080, then 0.0085 m at 1082 and 0.025 m for good on
     every non-leaver (identically). The first rewind (anchor 1080) lets it
     in - the second's replay is bit-identical to the first's: the leave is
     applied live (the mirror destroyed) before `rewind_for_lifecycle`
     restores 1080, the record's friction system of the ball and the
     Pusher's two Rinne contacts has a missing core and is dropped whole
     (`physics_savestate_restore.cpp` 626-660), and the Pusher's contacts
     are re-created fresh (`physics_state.cpp` ~1440-1500, "2 re-created of
     3 recorded"); the server's `remove_player` at 1081 lets IVP delete only
     the ball's points (`ivp_friction.cxx` 1001-1025), the Rinne points keep
     their state. Fix shapes (a) salvage the system minus the missing core
     in the restore, (b) keep the body to the rewind and remove it in the
     replay's pre-step at its stamp, as IVP does on the server - for the fix
     step (design note "As built (step H4)"). **Built at step DX11** (shape (b):
     a late death inside the history is queued, the anchor restored with
     the body present and the replay removes it at its stamp; the H4 J4
     captures replay with the Pusher 0.0000 m on clients 0, 2, 3 - 0.0085 /
     0.0267 m on rDX10 -, J1 / J2 / J3c and the forced control exact;
     world-agreement-gaps-design.md "As built (step DX11)"). **Queued from
     DX11 (item 4): a later rewind of the same frame anchored before a
     removal the previous replay made** - each late body change takes its
     own rewind, so a beam pair's birth rewind, a second same-stamp death's
     and a dead ball's input rewind restore an anchor into a world the
     earlier replay already took the body out of (its friction systems
     dropped whole, J4's mechanism; pre-existing, met in J1 / J2 / J4 with
     no other body touched, 0.0000 m); fix shapes (i) one rewind per frame
     for its late lifecycle events, (ii) revive the removed body before the
     restore - which depends on an open question: by the code EL7's
     `revive_ended_lives` should already hand those lives over as ended,
     and it does not (lead: the `at.tracked.remote_entities` / navigation
     gate, a beam pair's new name) - plus the overtaken-birth branch's
     death left `skipped` in the life log (no revive for a later input
     rewind between that birth and death; `note_life_made` after
     `remove_remote`) (design note "As built (step DX11)"). **The first
     built at step GL1** with gap L's late death (S2): the open question's
     answer is that a replay re-captured the removed mirror under its old
     generation (only the remotes `replay_remotes` lists get the current
     one), so no record showed the end; the replay now records a remote
     with no body whose generation the core moved on as ended
     (`rollback_world::remote_generation`), and the later rewind revives
     it before the restore - `h123_150` 0 differing keys on all four
     clients (5 on c0 / c3 before), J1 / J2 / J4 savestate misses 0
     (rollback-engine-gaps-design.md "As built (step GL1)"). **Still
     queued:** the overtaken-birth branch's `skipped` death entry.
     **Found at GL1, queued, cause open:** `core_ref_s12/h1_150` client 2's
     FINAL rows differ from the server's identically on rDX11 and rGL1
     (pre-existing; clients 0, 1, 3 exact): mirror 3035006883 rotation /
     angular from 1186, mirror 1821203589 10.8 m from 1190, the own ball
     10.9 m from its own paper trafo birth at 1202 (the replay logs
     "Ball_Paper made again at tick 1202 ... kept against its birth
     record: dp=(0.0338,-0.0107,0.00205)"), mirror 814755977 55.5 m from
     1224, P_Ball_Wood_MF from 1292, the Pusher 29.7 mm from 1436; REF12's
     live LOCAL rows show the own ball off from 1202 too. Not FANP or
     INV's finding 2 on its face; possibly the own-ball birth residual (A)
     of SFD, not shown. Next: first divergence of client 2 against the
     server before 1186 (rollback-engine-gaps-design.md "As built (step
     GL1)"). **Cause established at FULL step H1C2 (2026-10-30), fix
     waiting on the user:** the named-hull first-build scale of
     `Ball_Paper_Mesh` - physics_RT caches a named hull per world, built
     with the scale of the first body that uses the name
     (`PhysicsScaleFromMatrix` of its local matrix, CKIpionManager.cpp
     ~471-472 / ~510-546, engine change #10), and a rotated float matrix's
     row lengths round by rotation: c2 built it from its own paper birth
     (y scale 0x1.000012), c0/c1/c3 and the server from mirror
     3035006883's pose (0x1.00000e) - every world's first paper ball in
     tick order is player 3035006883's at 1180, but c2, running ahead,
     simulated its own 1202 trafo live before that birth reached it
     (`h1c2/t5/client2.log` 1319 vs 1531: the mirror clone is made after),
     and the rollback to 1180 does not undo the hull cache (not in the
     savestate); each new paper ball inherits the rotation of the ball it
     replaces, so c2's differs; first differing state 1180 (the
     newborn mirror's radius 1 ulp larger). Forcing c2's build to the
     shared scale: 157 keys, 0 differing on all four (`maps/arena-test/
     h1c2/t6`). Not SFD (A) (stone and wood are plain spheres), not
     rollback-specific; the cache is cleared by every reset
     (ClearCollisionSurfaces). Options (A1) a hull pre-build entry point
     (engine change #22) / (A2) a template body at load and after each
     reset / (B) per-player mirror hull names / (C) a rotation-free cached
     scale (engine) / (D) accept; HL's h1_150 c2 own-ball criterion waits
     on it. **Found at FULL step H6, queued (item 4, pre-existing,
     cause established): an import's anchor record does not track the
     mirrors the server's state brought** - `h6_full/a3_trans`'s late
     joiner (S 501; its mirrors born live at 503): a rewind anchored at S
     (an input of 502 relayed after the import) parks the three mirrors as
     born at 502 with the re-step's record of 502 and never simulates 502
     for them, so client 0's mirror keeps the hold-last brake of 502
     (0.26 m/s, 1.08 m; the Pusher it reaches 3149 m); identical on rDX11.
     `rollback.hpp` ~1983-2006 re-captures the anchor with the old tracked
     set (`capture` ~3310 grabs by it), ~1398 finds no `recorded` body and
     `birth_record` (~3270) returns 502. Fix shapes: (i) add DX10's moved
     mirrors to the anchor's `tracked.remote_entities` before the
     re-capture, (ii) a body the anchor's blob holds is present at the
     anchor (world-agreement-gaps-design.md "As built (step H6)").
     **Fixed at FULL step LJ1** (shape (i)): the core names the navigated
     mirrors with a row at S (`resync_spec::mirrors`) and the import's
     re-capture tracks them under their current generation; the capture
     replayed: own ball and mirrors 0.0000 m on the joiner, a3_pair2 c2's
     missing anchor row (H6's side note) covered, DX10's seeds exact
     (world-agreement-gaps-design.md "As built (step LJ1)"). **Found at FULL
     step LJ1, queued (item 4, pre-existing, cause established): an import
     keeps the live births of untracked fixed bodies the server's state has
     at S** - the same joiner's live frames made `P_Modul_01_Rinne` /
     `_Filler` at 509; the server's blob knows them, but
     `forget_births_after` (`client_core_resync.cpp` ~707-711) forgets only
     the rowed bodies' births, so the rewind anchored at S destroys them
     (RO1, `rollback.hpp` ~1708) for replayed scripts that never make them:
     the Pusher falls through from 634 (3149 m by 1452), every other body
     exact; identical on rDX9 / rDX11 / rGL1. The fix is settled by the
     design, not a user choice: `forget_births_after`'s platform contract
     (`client_platform.hpp` ~603-607) already names "the bodies the
     server's state has at S"; the core passes only the rowed ones. So the
     queued step is a bug fix bringing the core to the contract: the import
     forgets the births after S of every body the server's blob knew,
     between the restore and the replay (an engine hook or a
     `resync_outcome` list). Not taken: RO1's guard sparing a body the
     anchor's blob holds (it changes a rule every rewind reads and collides
     with DX1's `unborn` marks). Evidence for LJ1's own fix is the capture
     replays and the unit test: neither live run had a rewind land on S
     after the import. **Fixed at FULL step LJ2**: between the server's blob
     and the replay the engine hands the core's import hook
     (`rollback_world::forget_births_after`) every window birth of a body
     the server's blob knew and no plan item covers; the core forwards it
     to the platform's `forget_births_after` (the rowed bodies keep the
     core's after-the-re-step call). The capture replayed: the joiner's
     Pusher exact, all four clients 0.000 m at every FINAL row; a3_pair2,
     J1b seeds 1-5 and j1 still exact (world-agreement-gaps-design.md "As
     built (step LJ2)").
     **Found at FULL step H5b, queued (item 4, pre-existing on rGL1, cause
     established): an own life that ends inside a resync's re-step window
     is not made again** - `h5b_full/fan_dr` client 0 (WG3-dr: beamed into
     the death zone at 1400, resync requested at 1460, S 1461, applied at
     local 1472 after its death at 1470): the own ball's rows of 1462-1468
     are missing on the client (35 m under the death plane: no other body
     differs). The engine finds the life ended in the window and "revived
     by no half": `reconcile_own_lives` returns while a resync's restore
     runs (`client_core_lifecycle.cpp` ~1558, `s.resync_restoring_`), the
     resync's reconcile handles mirrors only, and the restore skips the
     server blob's own-ball entry. Settled by the design (resync design
     §2.6.3: "A body exists at S if and only if the server has a row for
     it at S" - the server has the own ball's row at 1461 - and its table
     restores the own ball from that row; its death applies at its stamp
     as every own event inside a window), so a bug fix, not a user choice: make the own A-life
     again before the resync's restore and queue its death at its stamp
     (headless; retail has no own body in the replay, R6)
     (world-agreement-gaps-design.md "As built (step H5b)"). This is the
     resync variant of gap L, not GL1's: GL1 fixed a life ending inside a
     REWIND's window (the dead mirror kept in the records a later rewind
     re-captures); here EL7's `resync_restoring_` guard (set on purpose:
     "the mirrors are this apply's own reconcile") leaves the OWN life of
     a RESYNC's window to no half - a separate fix on the resync path, not
     to be merged into gap L's.
     **(item 2) 8 members at 0 ms
     start late, measured**: U9 and its control without the FIFO and the
     reload (`u9ctl`) starve - every client steps tick T 2.6-4.5 ticks after
     the server from its first tick to the end (4 members, h1_0_on: 0.3-1.2
     ticks before it), and its journal header -> first tick takes 280-300
     ms against 124-128 ms with 4 members (header written right after the
     SessionReady send, before the anchor work: one trafo piece set per
     open life, 8 here). Joins H3's RL1 LAN excess above (plan step DX5, the
     user's "选1"); DX5 measures the piece sets' share and takes u9ctl as a
     check recipe (starved 0 expected). **Fixed at step DX5**: u9ctl
     starved 0 on all eight clients in all four sectors, S1 lateness
     12.5 for every member (the 4-member level), ~35 ms a piece set
     (derived; late-player-design.md 2.8 "As built (step DX5)").

**Stage B - the run-ahead policy (goal 3, the persistent late case).**

8. Late-player design revision 3, R0-R5 (arrival ring, baseline, quorum
   patience, the lead decision), plus plan steps 12 (per-client reach; see
   item 2's first entry), 21 (events on the input datagram), 22 (own death
   move). With step 22 (found at F3, late-player-design.md 3.1 "As built
   (step F3)"): a lone own Unphysicalize the server re-stamps late is only
   logged "kept where we stamped it" (`client_core_lifecycle.cpp`, the
   `own ... echoed` branch of the lifecycle echo; only a moved pair
   resyncs, through `own_moved_unphysicalize_`), so the own world removes
   its ball up to 2 ticks before the server and every mirror, and contacts
   in those ticks differ with no correction (snapshots off). About 1 % of
   lone events at 10 % loss; how much of it remains depends on the D4
   answer (the stamp rule / repeat schedule). D4 answered and built at
   step DX8 ((b') hybrid repeats, late-player-design.md 3.1 "As built (step
   DX8)"): modelled late lone events at 10 % loss 9.1 -> 0.1 per 1000
   (burst 1), about 50 -> 2.7 (burst 2), with the copies coalesced into
   the input datagrams as the proxy logs show for 83 of 86 events (an
   upper bound on the gain); step 22 stays queued for the rest.

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
   Measured again at FULL step FANX (world-agreement-gaps-design.md "As
   built (step FANX)"): a ball beamed off a rolling contact on the paper
   trafo plate leaves the server's at the stamp (dv 2e-3, dw 3.4e-3) and
   gains an extra upward 0.03 m/s in the six ticks after (the contact the
   in-place body keeps), 1.7 cm off by 620 in the fan column; the stamp
   impulse is in the pre-FANX `rtx_3` too.
   - **The host platform audit (FULL step AUD, 2026-10-30, the user's
     "Shared helper + audit")**: `docs/host-platform-audit.md` lists every
     platform operation and host-side rule of the server's world, the
     headless host and the retail core host, and ranks the differences
     (§9). Its candidates for this stage: **AUD-1** a leaver's spawn-ghost
     group dropped in every world (the server's `spawn_ghost_forget`; no
     client does it - D2); **AUD-2** one checkpoint-hold (N3) helper with
     a per-frame keep for both clients, retail's N1 onto
     `sector_reset_guard` at R7 (D3); **AUD-3** one birth-log feed rule
     (`add` vs `put`, event log vs OnPhysicalize), decided with R6 (D4).
     D1 is R6 above; D5 / D6 are HU1's queued (d) / (a). None breaks the
     FULL headline; D3 needs a diagnostic line (none today) before the
     FULL batch that runs `--reset-level-at` can check it.

**Stage D - the original levels.**

10. Levels 8 and 11, sector 1: solo savestate check and four clients,
    snapshots off. The pooled-modules diagnosis's open points (retail CK2
    `Reset`, P_Modul_01's Rinne/Filler) are checked here.
