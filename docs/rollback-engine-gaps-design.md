# Rollback engine gaps: the PreSimulate queue, lives that end inside the window, the retail beam's generation

Status: design, revision 2, 2026-10-23. Revision 2 answers two reviews; §10
lists every point and what became of it. Step G0 (diagnostics, 2026-10-25)
is done: its measured facts are written into §1.2, §1.3, §2.2.4 and §2.3.4,
its new question is §9.7, and "As built (step EG0)" at the end records the
runs and tools. Nothing else is built.

Scope: three gaps of the rollback coverage audit
(`scratchpad/audit/rollback-coverage-audit.md`, the "audit"):

| audit gap | fix | short name here |
|---|---|---|
| #6 | F5 | **Q**: the PreSimulate callback queue is never rewound |
| #7 | F6 | **L**: a body whose life ends inside the window is missing from the replay |
| #8 | F7 | **B**: the retail own beam bumps the generation when it is requested |
| — (found at resync step B1, 2026-10-24) | — | **F**: the script replay consumes next-frame flags of scripts the live frame skips |

**F (queued, not designed yet).** Symptom: after any rewind across the
tick a sector's walk sets it, `P_Modul_18_Particle Script` (arena) has lost
its `CKBEHAVIOR_DEACTIVATENEXTFRAME` bit (0x400000), which the world that
never rewound keeps for the rest of the run; every `--late-sector-check`
case and its ordinary-rewind reference differ in that bit at all 601
boundaries (the check's verdict is PASS-EXCEPT-REFERENCE, exit 4, until
this is fixed). Cause: `script_replay::execute_frame`
(`BallanceMMOCommon/include/game/script_replay.hpp:503-511`) applies and
clears the next-frame flags of every selected script, while the live CK
frame does so only for scripts whose owner is in the behaviour manager's
object list - the inactive Particle script's owner has left it (A2's G1
mechanism). Pre-existing: the reference arm runs the code the build before
B1 runs (`resync-and-sector-timing-design.md`, As built (step B1),
"Residuals"). Fix: `execute_frame` consumes a script's next-frame flags
only where the live frame would (shared script replay, every host); check:
`--late-sector-check` exits 0 on the arena cases of that note.

This design fits the three designs already written: the sector placement
design (SPD, `docs/sector-placement-design.md`), the trafo replication design
(TRD, `docs/trafo-replication-design.md`) and the resync and sector timing
design (RSD, `docs/resync-and-sector-timing-design.md`).

**Numbers this design claims.** All are nominal and assigned at landing; the
sibling designs `record-format-gaps-design.md` and
`world-agreement-gaps-design.md` claim some of the same nominal numbers.

- Engine change **#20** (`docs/engine-changes.md`; #19 is the last; no other
  design claims #20). Access only, in the style of #17: free functions, no
  data member, no changed path.
- Bridge API **v21** (`physics_rt_api.h:23` is v19; TRD takes v20; the
  record-format design also names v21 nominally). **As landed (2026-10-27):**
  v20 is resync A3's, v21 trafo T1's, v22 trafo T4's, and protocol 11 /
  journal v5 trafo T2's; G2 takes the next free bridge version and G5 the
  next free protocol number at their commits (v21 and 11 here are nominal).
- Script replay record: one more `kVersion` (SRP:80 is v2; TRD, the
  record-format design and the world-agreement design each take v3
  nominally). Called **v-Q** below.
- RSD's portable record (`kPortableVersion = 1`, RSD 2.4): one more
  version for the queue section.
- Protocol **11** (nominal; RSD/SPD/TRD claim 10): the server's queue in the
  resync state (§2.2.6). Shared with whichever design takes 11 if both land in
  one commit. No journal version: the RESYNC record carries the portable
  bytes, which are versioned by themselves (§5).

**Abbreviations.** Line numbers are those of the working tree (WT), which
holds the uncommitted step P on top of HEAD 4882ec2.

| short | file |
|---|---|
| RB | `BallanceMMOCommon/include/session/rollback.hpp` |
| SRP | `BallanceMMOCommon/include/game/script_replay.hpp` |
| SHS | `BallanceMMOCommon/include/game/session_host_support.hpp` |
| SHW | `BallanceMMOCommon/include/session/session_host_world.hpp` |
| RBS | `BallanceMMOCommon/include/session/rollback_binding_scripts.hpp` |
| CC | `BallanceMMOCommon/include/session/client_core.hpp`, `src/session/client_core.cpp` |
| CL | `BallanceMMOCommon/src/session/client_core_lifecycle.cpp` |
| CF | `BallanceMMOCommon/src/session/client_core_frame.cpp` |
| CP | `BallanceMMOCommon/include/session/client_platform.hpp` |
| SSC / SSR | `BallanceMMOCommon/src/physics/physics_savestate_capture.cpp` / `_restore.cpp` |
| SSI | `BallanceMMOCommon/src/physics/physics_savestate_internal.hpp` |
| PS | `BallanceMMOCommon/src/physics/physics_state.cpp` (the bridge) |
| API | `BallanceMMOCommon/include/physics/physics_rt_api.h` |
| PRT | `submodule/Ballanced/Source/BuildingBlocks/physics_RT` |
| CH | `BallanceMMOClient/session/core_host.cpp` (retail core host, RC) |
| PSC | `BallanceMMOClient/session/physics_session_client.cpp` (retail legacy, RL) |
| SC | `BallanceMMOServer/sim/session_client.cpp` (headless client, H) |
| PW | `BallanceMMOServer/sim/physics_world.{hpp,cpp}` (server world) |

Hosts: **H** = headless client, **RC** = retail core host, **RL** = retail
legacy path, **S** = server. The server never rolls back. Of this design it
runs only the read of its own queue for the resync state (§2.2.6).

---

## 1. Problem and rule

### 1.1 Q: the PreSimulate queue (audit #6, F5)

**What the queue is (checked in PRT).**

- `CKIpionManager::m_PreSimulateCallbacks` is a `PhysicsCallbackContainer`:
  three `IVP_U_Vector<PhysicsCallback>` slots (PRT `PhysicsCallback.h:27-40`).
  The fields are public; the bridge already reads them (SSC:797-811,
  SSR:1776-1792).
- Ten block classes queue into it: Physics Force, Physics WakeUp, Physics
  Collision Detection, Set Physics Ball Joint, Hinge, Slider, Spring,
  Buoyancy, Continuous Contact, Delete Collision Surfaces (PRT
  `Behaviors/*.cpp`). Every callback class passes type 2, so every entry lives
  in slot 2.
- **A retail callback carries no state of its own.** `PhysicsCallback` holds
  the manager, the type and the block id (`PhysicsCallback.h:13-24`). None of
  the ten subclasses declares a data member. Every `Execute` re-reads its
  inputs from the block when it runs, e.g. `PhysicsForce.cpp:143-196`. So an
  entry is fully described by (block id, slot), and the block's prototype
  fixes its class.
- **The bridge's own entries** (`navigation_callback`, `impulse_callback`,
  `clock_guard_callback`; SSI:463-471) do carry state (`armed_`). They are
  excluded from every capture and compare today and stay excluded.
  `clock_guard_callback` is queued once per session and stays
  (`physics_state.cpp:2390-2455`), so it is the oldest entry. The navigation
  and impulse callbacks are armed when queued and consumed by the next step
  (`ball_navigation.cpp:291-330`, `physics_state.cpp:2015-2030`).
- **Life cycle of a retail entry.**
  - A block's function builds one and calls `Process(pc)`
    (`PhysicsCallback.cpp:108-140`). That executes it at once. If `Execute`
    returns 0 (a body is missing), the entry is appended to its slot.
  - `Process()` runs before every step (`CKIpionManager.cpp:793-794`). It
    walks each slot **from the newest entry to the oldest**
    (`PhysicsCallback.cpp:94-104`), and drops an entry whose `Execute`
    returned non-zero or whose block is gone.
  - `ClearBehavior` (`PhysicsCallback.cpp:56-80`, from
    `CKIpionManager.cpp:336`) drops the entries of a deleted block.
- **No dedup.** `PhysicsForce` queues a new callback on every Create while
  its block holds no controller (`PhysicsForce.cpp:210-221`). `WakeUp` and
  `Buoyancy` queue on every activation. A force callback that runs creates a
  controller and **overwrites** local 0 (`:190-192`), so a second copy leaks
  the first controller. Shutdown deletes only the one local 0 holds
  (`:226-234`).
- **A controller made live survives a blob restore.** `order_controllers`
  (SSR:1584-1636) keeps a live controller that the record lacks, at the end
  of the list, in lenient mode (SSR:1624-1632). A controller that a
  non-replayed block made inside the window is therefore still on its core
  during the replay.

**Symptom.** The audit counted 12 logs with refusals, up to 46 per client.
The refusal message names the entries (SSR:1813-1815):

| log | entries | pattern |
|---|---|---|
| `e8ab2_swing_n1/sector4/client3.log` (8 lines) | `Physics WakeUp in P_Modul_34_MF Script` (48462) | live 1 vs recorded none, then live k+1 vs recorded k |
| `core_ref_e0/cov_e9/tr/resync_snapoff/new/client0.log` (8) | `SetPhysicsForce in P_Modul_08_MF Script` (45709), WakeUp in P_Modul_19 (46818) | the same growth |
| `r4_lane_loss10/sector1/client0.log` (8) | WakeUp in P_Modul_25 (47026), WakeUp in P_Modul_29 (47697) | the same growth |
| `lp19_a6_drop/sector1/client2.log` (1) | `Physics WakeUp` (26418) and `SetPhysicsForce` (26432) **in Gameplay_Ingame** | the own ball has no body (own Physicalize dropped, A6 drop arm), keys held |

All selected-script cases predate N4 (21129b2): their target bodies were the
N4 bug's lost sector bodies. The audit counted 0 refusals in 245 post-N4
logs. The Gameplay_Ingame case does not depend on N4: it only needs the own
ball without a body while a key is held. After TRD, each set's
`Ball_CollSound_Pieces_Trafo<p>` can queue `PhysicsCollDetection` entries on
bodiless pieces (TRD 4.4, 9.2), in every world including the server.

**Cause.**

1. **The refusal.** `check_callbacks` (SSR:1772-1821) compares every
   non-BMMO entry with the blob's record and fails even in lenient mode. RB
   then counts the failure, destroys the bodies `revive_body` made (RB:974),
   drops that anchor's blob for good, and takes the per-body path. On that
   path the clock is not rewound (`restore_clock = false` on every host,
   audit §0), untracked bodies stay live, and IVP's collision state stays as
   the live world has it.
2. **The growth.** Nothing restores the queue. The script record (SRP:367-394)
   restores the block state (inputs, outputs, flags, force controller, joint
   presence), but not the block's queued entry. A replayed Create or WakeUp
   edge then queues a copy next to the live one: k → k+1 per rewind. The
   blobs captured inside that replay hold the grown queue, so every later
   rewind is refused again.
3. **The leak.** For SetPhysicsForce, the extra copies are physics. When the
   body appears, every copy runs `new PhysicsControllerForce` and overwrites
   local 0. Shutdown deletes one controller; the others keep pushing. Derived
   from the code, not observed.
4. **Outside entries in a replay.** An entry of a script the replay does not
   execute (Gameplay_Ingame) is live state: its block is not restored, and the
   controller it made live stays on its core (above). If such an entry is in
   the queue during a replay, the replay runs it whenever the replay world has
   its body. That would make a second controller next to the live one and
   overwrite the block's local 0. With L (§2.3), which revives the own ball
   for [A, D), an entry pending now for a dead ball would run at A+1 on the
   revived body. Its controller then dies with the revived core at D and
   leaves local 0 dangling.

**Invariant Q.** At every restore to anchor A, when a replay follows:

- the queue entries of the scripts the restore **writes** are exactly the
  anchor's, in the anchor's order. A script with no entry in the record has
  none, including a script absent from the record (a TRD set made after A,
  TRD 4.5);
- the entries of every script the replay does **not execute** (outside
  scripts, v14 kept-live scripts) take no part in the replay: none runs, none
  is compared. After the replay they are back in the queue at the places
  they had;
- no restore is refused for an outside entry;
- `check_callbacks` stays fail-closed for every entry it compares;
- a resync gives the selected scripts the server's entries at S.

### 1.2 L: a life that ends inside the window (audit #7, F6)

**Cause, per kind of body.**

- **Generations.** A record keeps each tracked name's lifetime generation
  (RB:281-294). `record_at` returns an anchor record only if its generation
  equals the generation in the newest record (`generation_now`, RB:694-706).
  `tracked.generations` is the whole `s.generations_` map (CF:561): a dead
  name keeps its bumped generation in every later record.
- **Every end of a life bumps the generation:**
  - mirror death: `unphysicalize_remote` (CL:218); a mirror trafo, which
    takes the old body out inside `physicalize_remote` (CL:456-460);
  - own ball and mechanisms: the drained `deleted` event (CF:455-462).
- A life that ends inside (A, now] therefore fails `record_at`. Without a
  later birth record the plan has no item (`continue; // no row, no record`,
  RB:795). Then:
  - **blob path:** the restorer skips the object, "in the savestate but not
    in the world" (SSR:512-516), and drops its contacts with it;
  - **per-body path:** `if (!world.get_body) continue` (RB:1021).
- Either way the body is absent from the whole replay, not only from D on.
- Its recorded inputs are still fed (`feed`, RB:1161), to a body that is not
  there. For H mirrors `recorded_input_at` cannot even map the dead life's
  name to a player (CF:261-283).
- **A name reborn inside the window** (a new life at E, A < D ≤ E ≤ now) has
  a birth item (RB:731-765). That item parks the new life from A (balls) or
  destroys it and lets the replay remake it (mechanisms). Either way the old
  life never comes back: step P's revive skips such a name and logs it as the
  known gap (RB:863-882).
- **Mechanisms that die inside the window without a rebirth** are revived by
  step P (RB:863-882, SHW:368-391), on the savestate path only and only for
  the trafo pieces (`revivable`, CH:1548, SC:2665).
- The death's own lifecycle rewind is anchored at D-1 (CL:186-207). The body
  is removed before step D, which the skipped object reproduces. Every other
  anchor before D-1 is not.

**Naming facts that decide the design.**

- **H mirrors have one name per life.** `remote_entity` looks up
  `<ball>_BMMO_<player>` and otherwise clones the ball with
  `CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME` (SC:2193-2210). The clone's name
  never matches the lookup, so every `mirror_entity` call makes a new clone.
  Player 538118464 in `ab2_s17_beam_1/sector4/client2.log`:
  `Ball_Stone_BMMO_538118465` at 231 (line 189), `_466` at 701 (444), `_467`
  at 1058 (606), `_468` at 3081 (1371).
- **H's `make_mirror` only accepts that fresh name.** It takes
  `mirror_made_entity_` or calls `remote_entity` again, and refuses any other
  `spec.entity` with "no 3D entity named" (SC:917-931). An old clone survives
  its death (`unmake_mirror` only unphysicalizes, SC:934-937), but nothing
  can find it by name today.
- **RC mirrors have one name per (player, ball type).** `get_ball_entity`
  returns the same spirit ball for every life of that type
  (`game_objects.h:301-306`, CH:1426-1453). A death and a same-type rebirth
  reuse the name. The remote beam pair (Unphysicalize + Physicalize at one
  stamp, e.g. ab2 c2 lines 432/444 at 701) does exactly that on RC.
- **The own ball keeps its name per type.** A death and a respawn reuse it.
  - Retail: 96 death→rebirth pairs in 72 logs, the shortest 165 ticks, all
    longer than the 96-tick history. On RC, an own death and an own rebirth
    of the same name never share a window.
  - Headless: pairs as short as 6 ticks exist
    (`core_ref_e0/cov_e9/tr/resync_snapoff/new/client0.log`: unphysicalized
    2425, physicalized 2431). On H both can share a window.
- **Life spans exceed the log.** RSD's `life_log_` is trimmed to
  `history_ticks + 128` = 224 ticks (RSD 2.6.1). The life `_466` above was
  born at 701 and died at 860; at a live tick of 956 (a 96-tick window over
  the death) its Physicalize is 255 ticks old.

**Symptom (measured).**

- `cross_death.py` (audit scratch) finds rollbacks with A < D-1 for a remote
  death in 5,541 of 1.10 M logged rollbacks.
- Example: `ab2_s17_beam_1` c2, anchor 693 with deaths at 701. The dead
  ball's corrected inputs are replayed onto no body.
- `r2_a_f13` (retail, forced 13:24), own death at 1369 (client.log line
  347): one forced rewind crosses it with A < D-1 (anchor 1354, live 1378,
  line 352). It is the only replay after 1369 whose scripts end differently
  from the live record ("differ in float parameters only: P_Modul_18_MF
  Script / TT Scaleable Proximity: output 0 (Current Distance)", line 351).
  - **Cause (step G0, measured).** The block is P_Modul_18's outer
    proximity 46382 (80 units, a check every 10-60 frames; the inner 46429
    and the sound's 46540 only run in range, and the ball was ~230 units
    away). Its ObjectA is `Ball_Pos_Frame`, a child of the current ball
    entity, against `P_Modul_18_Particle` (46336). 52384 is not a client
    entity: it is the server world's per-player probe frame for 46429
    (`physics_world::rewire_proximity_probes`), which `--describe-script`
    shows because it loads that world; the plain level (`--dump-script`) and
    every client world have `Ball_Pos_Frame` in all three P_Modul_18
    proximities.
  - The rerun `eg0_f13` (same recipe and events, `BMMO_TRACE_ENTITY`) traces
    the same window 1354→1378: in every replayed tick 1355..1370
    `Ball_Pos_Frame` is at (-20.000, 3.650, 15.000)@Ball_Wood, the live run
    had it falling at (300.773, -57.537, 15) .. (300.851, -65.772, 15):
    up to 328.28 m (`work/EG0/entity_trace_cmp.py`).
  - Two halves make it. The own ball has no body over 1355..1368 (this gap,
    L), so nothing moves its entity; and the entity's CK matrix is no
    rollback state, so it stays where the live frames had put it by the
    rewind: the resetpoint, to which the death sequence (outside the replay)
    moved the dead ball at frame 1370 (live trace: 1369 (300.851, -65.772,
    15), 1370 (-20, 3.65, 15)). Not the "unrecorded CK matrix" class alone,
    and not the absence alone: L's revive fixes 1355..1368, but not the
    ticks after D (§2.3.4 "found at G0", §9.7).
  - Symptom vs cause: the frame differs in every window across the death
    (also 1367→1391); the script record only shows it when the proximity's
    own schedule puts a check inside 1355..1370. r2_a_f13 had one there
    (line 351); eg0_f13, with its diff logs uncapped, had none. Later r2
    arms (pr2_a_f13, pi_a_f13) also lacked the line because the core host
    logs only 8 script differences (`script_diff_logs`), already used up by
    650-819.
- Physical effect on another body: not yet measured (audit).

**Invariant L.** For every life of a tracked name and every replayed tick t
in (A, now]:

- the body exists in the replayed world if and only if the life, judged by
  its stamps, covers t;
- a life alive at A starts the replay from its own record of A;
- a life that ends at D is removed before the step of D by the operation the
  live frame used: the core's mirror removal, the own-body removal, or the
  replayed selected script for mechanisms;
- a life that begins at E inside the window is made before the step of E by
  the operation the live frame used. This includes a life that begins and
  ends inside the window;
- for remote lives the tick is the event's stamp (its applied tick), even
  when this world applied it later (RC with `lifecycle_rewind = false`,
  CH:597);
- the CK entity of a dead own ball ends the replay with the matrix it had
  before the rewind.

### 1.3 B: the retail own beam's generation (audit #8, F7)

**Cause.**

- `beam_own_ball(T-1)` runs from `post_step(T-1)` via `apply_requested_beam`
  (CF:612). That is after `record(T-1)` (CF:563). `amend_record` (CF:618)
  re-captures bodies only (RB:484-489).
- With navigation and without own body control, it bumps the generation at
  the request (CL:703-704). This copies the non-navigated branch (CL:700-702),
  where the move is an in-place write outside every replay.
- The beam's stamp T is applied in place by `write_beam`, live and in every
  replay that crosses it (CL:883-890). The life does not change.
- With the bump, an anchor A ≤ T-1 sees a different generation than the
  newest record, and `record_at` fails (RB:701-706). The ball counts as born
  at T: it is parked at its tick-T pose (RB:731-765) and fed pre-beam inputs.
  The replay then re-records A+1..T-1 with the parked pose.
- H keeps the generation on a beam (its remake is muted, CL:808-845).

**Symptom.**

- Ball_Pos_Frame is wrong in the replayed pre-beam ticks, so the TT Scaleable
  Proximity blocks schedule differently. The r2 forced arms show "differ from
  the live record" at 650, 900, 910 and 917: Last Check 40 vs 17 and 23 vs 37
  (P_Modul_01), P_Modul_18_Particle flags (r2_a_f13 client.log line 246).
- The parked ball's destroy is refused (SHW:335-336). `destroy_body` returns a
  bool, so RB counts the refusal as `birth_recreate_failures` (RB:931), the
  same counter as a real failure:
  - `r2_a_f13` `births=…+0/0/0/11`
  - `r2_a_f50` `…/3`
  - `r2_a_f131` `…/3`
  - `r2_a_f97` `…/0`
- These counters also count every parked own birth (spawn 212, trafo 817,
  respawn 1567) that lies in a window.
- **Attributed (step G0).** The rule (RB:885-920, `birth_record`
  RB:2252-2259): the own ball gets an item in the restore to A iff its
  current lifetime (the generation of the newest record) has a body now and
  its first record S lies in (A, now]. A beam's lifetime starts at record T
  (the bump is after record(T-1)); a lifetime that has ended by now gives no
  item. Each item is a refused destroy (a ball), counted once:
  - `r2_a_f13` (11): spawn 212 at anchors 197 and 210; beam 650 at 626
    only (639→663 finds the stone dead since 652: no item); trafo birth 817
    at 795 and 808; beam 900 at 886 and 899; beam 1300 at 1276 and 1289;
    respawn 1567 at 1549 and 1562.
  - `r2_a_f50` (3): beam 650 at 638, beam 900 at 888, beam 1300 at 1288.
  - `r2_a_f131` (3): beam 900 at 870, beam 1300 at 1263, respawn 1567 at
    1525 (the respawn was the "one short").
  - `r2_a_f97` (0): no window of 97:24 contains a birth or a beam stamp.
  - The model reproduces every arm's total from its log
    (`work/EG0/birth_items.py`); the rerun `eg0_f13` with the plan trace
    (`BMMO_TRACE_PLAN`) logs exactly these 11 items at these anchors, one
    refused-destroy line each ("plan trace: birth item <name>@<tick> not
    destroyed" in that run; the line now reads "refused destroy of
    <name>@<tick>", since the savestate-failure path's uncounted
    `destroy_body(entity, 0)` goes through the same wrapper; a refused
    `recreate_body`, also counted, is not logged by it and did not occur).
    f50's and f131's items (f131's "one short" included) are the model's,
    from their r2 logs, not a plan trace: the model is validated on the f13
    rerun only.
  - So after G1 (B1): f13 has 6 items (the births), f50 0, f131 1, and all
    of them count `births_parked_balls`; `birth_recreate_failures` is 0.
- No body has diverged yet. `write_beam` has 2 callers and is automation only.

**Own death and respawn inside a window (examined, both hosts).**

- **Death.** It is L's own-ball case: the ball is missing for A+1..D-1 on
  both hosts.
- **Beam inside the dead life.** r2_a_f13 beams Ball_Stone at 650 (line 245)
  and unphysicalizes it at 652 (line 249). In a replay, `apply_own_lives`
  asks `own_life_current` (CL:776-779), which compares with the live
  generation. A window over both ticks therefore retires the beam (CL:875-883)
  and never applies it, even once L revives the stone. §2.3.4 fixes the test.
- **Respawn.** The new life is parked from A to E and released at E. Balls
  are refused by `destroy_body` and `insert_birth` (SHW:293, 335-336). On
  the savestate path the parked ball stays out of collision detection until E
  (RB:1029-1037). The birth's sleep state and extras are written at the
  re-pose (RB:1327-1329). Its exactness class is "parked birth": the
  end-of-replay identity check is skipped for it (RB:1400-1405).
- On H a death and a same-name respawn can share a window (§1.2). Then L must
  remove the parked new life during [A, D), or the two lives would share one
  entity. §2.3.4 does this.

**Invariant B.** A beam is a move within one life. The own ball's generation
never changes at a beam, on either capability setting, and the records before
and after the stamp belong to one life.

---

## 2. Design

### 2.1 Overview

| part | what | where |
|---|---|---|
| Q1 | engine change #20: a factory for the callback of a block; detach, insert and free one entry | PRT `PhysicsCallback.{h,cpp}`, the ten `Behaviors/*.cpp` |
| Q2 | bridge API v21: read the queue; rebuild the entries of a set of scripts; hold and unhold the entries of all other scripts; the scope of `check_callbacks` | PS, SSR, API; physics_direct, physics_view |
| Q3 | script record v-Q: the selected scripts' entries, captured and restored with the scripts | SRP; hooks filled in RBS for every host |
| Q4 | outside entries held out of every replay and put back in place after it | RB (two hooks), RBS |
| Q5 | the server's selected entries in RSD's portable record, for a resync | SRP (portable), PW, protocol 11 |
| L1 | the engine finds the lives that end inside the window and asks the world to restore them before the plan; a plan item can carry a revived life and a later birth of one name | RB |
| L2 | mirrors: the core reconciles every remote life of the window from the life log and applies each event at its stamp | CC/CL/CF, with RSD's `life_log_` and `pending_deaths_`; SC exact-name lookup |
| L3 | own ball: revived at A, removed at D, same-name rebirths remade; replay-tick life test | CC/CL; CP `own_body_in_replay` |
| L4 | mechanisms: step P's revive generalized | SHW, RB |
| B1 | the beam keeps the generation; parked balls counted apart | CL, RB, SHW |

### 2.2 Q: the queue as script state

#### 2.2.1 Engine change #20 (PRT; access only)

Free functions only. `CKIpionManager.h` and every class layout stay as they
are, and no existing path changes.

1. **The factory.** Each of the ten behaviour files gets one function next to
   its callback class, which is file-local:

   ```cpp
   PhysicsCallback *BmmoMakeCallback_<Block>(CKIpionManager *man, CKBehavior *beh)
   { return new <ClassOfThatFile>(man, beh); }
   ```

   `PhysicsCallback.cpp` gets the dispatcher, keyed by prototype GUID. The ten
   GUIDs are the files' `SetGuid` lines, e.g. Force `0x56e20c57,0x0b926068`,
   WakeUp `0x38b851b5,0x72ca74ac`, CollDetection `0x7435488d,0x201d1188`. It
   returns NULL for any other prototype:

   ```cpp
   extern "C" void *BmmoPhysicsCallbackMake(CKIpionManager *man, CKBehavior *beh);
   ```

   The object is constructed, **not executed and not queued**. That is the
   recorded state of an entry: a queued entry is one whose last `Execute`
   returned 0.
2. **One entry out, one entry in, one entry freed.** Allocation and deletion
   stay inside physics_RT, because the retail bridge lives in another module:

   ```cpp
   extern "C" void *BmmoPhysicsCallbackDetach(CKIpionManager *man, int slot, int index);           // remove_at, no delete
   extern "C" int   BmmoPhysicsCallbackInsert(CKIpionManager *man, int slot, int index, void *cb); // at index, + m_HasCallbacks
   extern "C" void  BmmoPhysicsCallbackFree(void *cb);                                              // virtual delete
   ```

   `Insert` with index = len appends. Otherwise it appends and moves the entry
   down with `swap_elems` (`ivu_vector.hxx:123`), because `insert_after`
   cannot insert at 0 (`:133-146`).

Not an IVP change; `IVP_PORTABLE_BMMO` does not apply. The simulation is
unchanged: the exports are only called by the bridge, at a restore or after a
replay.

#### 2.2.2 Bridge API v21

Appended to `bmmo_physics_api_v2` (API:23 bumped to 21). Every entry follows
the existing error convention.

```c
typedef struct bmmo_physics_callback {
    uint32_t behavior;      /* the block that owns the entry */
    uint32_t owner_script;  /* CKBehavior::GetOwnerScript()->GetID(), 0 when none */
    uint8_t  slot;          /* 0..2 (always 2 today) */
    uint8_t  bmmo;          /* 1: one of the bridge's own (is_bmmo_callback, SSI:463) */
} bmmo_physics_callback;

/* Every queued entry, slot ascending, index ascending (queue order). */
int32_t (*callbacks_get)(void* m, bmmo_physics_callback* out, int32_t max, char* e, uint32_t es);
/* Every non-BMMO entry whose owner script is in `scope`: removed and freed.
 * Then `target` (each entry's owner must be in `scope`) appended in order,
 * each made by the factory.  Atomic: all targets are made first; a target
 * the factory refuses frees what was made and changes nothing (-1, the
 * block named). */
int32_t (*callbacks_rebuild)(void* m, const uint32_t* scope, int32_t n, const bmmo_physics_callback* target,
                             int32_t count, char* e, uint32_t es);
/* Every non-BMMO entry whose owner script is NOT in `executed` is detached
 * into the bridge's held list with its place: (slot, index in the queue as it
 * was, the block of the nearest older non-held entry).  Returns how many.
 * Fails when entries are already held. */
int32_t (*callbacks_hold)(void* m, const uint32_t* executed, int32_t n, char* e, uint32_t es);
/* The held entries back into the queue, oldest first, each after the entry
 * it followed when that block still has an entry in the slot (its first
 * such entry after the previous reinsertion), else at its old index clamped
 * to the slot's length.  Returns how many went back by the clamp. */
int32_t (*callbacks_unhold)(void* m, char* e, uint32_t es);
/* Which entries check_callbacks compares, live and recorded alike:
 * mode 0 = every non-BMMO entry (today's rule, SimTool's default);
 * mode 1 = the entries whose owner script is in `scope` (may be empty). */
int32_t (*callback_scope_set)(void* m, int32_t mode, const uint32_t* scope, int32_t n, char* e, uint32_t es);
```

- **Capture is unchanged.** SSC:797-811 keeps recording every non-BMMO entry
  in queue order; the blob layout stays.
- **`check_callbacks` filters both sides by the scope** (SSR:1772-1821). The
  owner of a recorded entry is looked up from its block id, as the error text
  already does (SSR:1801-1803). A recorded entry whose block no longer exists
  is compared (fail-closed). The BMMO exclusion is unchanged.
- **Where the code lives.** PS implements the API: the held list is on the
  same manager-keyed state as the event log. physics_direct (H, SimTool, S)
  and physics_view (RC, RL) expose it.

#### 2.2.3 Q3: the selected entries in the script record

**Record v-Q.** After the cells (SRP:386-391), one section:

```
u16 count; count x { u32 behavior; u8 slot }     // queue order: slot ascending, index ascending
```

- It lists the entries whose owner script is in `sel.scripts`. The capture
  hook returns the whole queue; `capture` filters it.
- A selected entry has no further state (§1.1).

**Hooks.** `script_replay_hooks` (SRP:69-72) gains:

```cpp
std::function<bool(std::vector<queued_callback>& out)> capture_queue;                       // callbacks_get
std::function<bool(const std::vector<CK_ID>& scope, const std::vector<queued_callback>& target,
                   std::string& error)> restore_queue;                                       // callbacks_rebuild
```

`bind_script_replay` (RBS:49) fills both from its `Backend` when the host
left them empty. Every host that binds the script replay (H, RC and RL,
PSC:3228) gets them with no host code. That is one implementation, unlike the
force hooks, which each host fills itself (CH:886-900, SC:1482-1496,
PSC:1526-1540).

**Restore.**

- `restore` (SRP:442-484) reads the section last and calls `restore_queue`:
  - **scope** = every script the restore writes. With `replay_activation`
    (both core hosts, CH:1571, SC:2690) that is every selected script,
    including a script absent from the record that TRD restores to its built
    state (TRD 4.5). Under the v14 rule (RL) it is the `replayed` list;
  - **target** = the recorded entries of the scope's scripts. A script absent
    from the record has none: its built state has no queued entry, and its
    live entries are removed.
- A rebuild failure fails the restore. The binding counts it
  (`script_restore_failures`, RB:902-903), and the blob's scoped
  `check_callbacks` then refuses as today. The world stays fail-closed.
- **Order with the force hooks.** The record restores the blocks first (the
  force controller via `restore_force`, SRP:879-889), then the queue.
  Example: a block whose anchor had no controller and one queued entry comes
  back with no controller and one entry. When its body appears in the replay,
  the entry runs and creates the controller the live frame created. The
  replayed Create meets `controller == NULL`, and it queues only if the live
  frame queued. No leak.
- **The scope of the check** is set by the binding at the same restore
  (`callback_scope_set(1, written scripts)`), so the blob compares exactly the
  entries Q3 wrote. Without a script record at A (`have_scripts` false), no
  script is written and the scope is empty: the selected scripts then run
  from their live state, as they do today in that case. This is counted
  (`cbq no_record`); the gate is 0.
- **Order in the restore (RB).** `restore_scripts` (RB:902) runs before
  `set_savestate` (RB:937). The queue is the anchor's before `check_callbacks`
  runs, so the scoped check is a post-condition: it passes when Q3 worked and
  refuses as today when it did not.

**Order against the other entries.** At the record of A, after A's step, the
only BMMO entry left is `clock_guard_callback`, the oldest (§1.1). Every
selected entry is newer than it. The rebuild appends after the BMMO entries,
and `set_nav` (RB:935) queues the navigation callback after the rebuild. That
is the live order at the start of A+1. The held outside entries are not in
the queue (Q4).

**The "idempotent replayed Create" of the audit is not built.** With the
restore in place, the replayed Create queues exactly what the live frame
queued. A replay-only dedup would make a replay differ from the live run
wherever retail itself queues twice: two Creates while the body is missing
leak a controller in the live game, and on the server too.

#### 2.2.4 Q4: outside entries are live state, held out of the replay

**Rule.** An entry whose owner script the replay does not execute is live
state, like the time factor (audit F5's rule). The replay must not run it, and
must not leave it where a replayed tick would run it. So every such entry is
out of the queue for the whole replay and goes back to its place after it.

- **Executed scripts** = `scripts_replayed` when the binding has it, else
  `sel.scripts` (RBS:100-106, `execute_frame` SRP:488-510).
- **Why not run them in the replay.** An outside entry that ran live inside
  the window made its controller then. That controller is still on its core
  during the replay (§1.1, SSR:1624-1632), and the block's local 0 holds it.
  Running the entry again would make a second controller and leak the first
  (cause 4). An entry pending now has never run live. Running it on a body
  the replay has and the live world lacks (L's revived own ball) would leave
  a controller the live run never made.
- **What the replay then lacks.** An outside entry that ran live at r in
  (A, now] does not run at r in the replay. Its effect is already present in
  the live world, and the replay starts with it (the controller from A on).
  For the producers that effect is:
  - SetPhysicsForce 26432 (and 26354, 26392, 26414, 26447): a zero-force
    controller (retail navigation leaves are zeroed, audit §3). §7.2 L-S3
    measures the body states;
  - WakeUp 26418 (and 25094, 27403): nothing, measured (below).

**The outside producers (Q-S3, step G0).** `SimTool --list-callback-blocks`
over L1-L12, the original L13 and the arena lists the same 28 blocks outside
the session selection in every level (same ids), all in four global scripts:

| script | blocks | target |
|---|---|---|
| Gameplay_Ingame | Physics WakeUp 25094, 26418, 27403; SetPhysicsForce 26354, 26392, 26414, 26432, 26447 | the current ball (a Gameplay_Ingame local) |
| Sound_Manager | Physics Collision Detection 6271, 6471, 6489, 6507; Physics Continuous Contact 6619, 6675 | the current ball (a Sound_Manager local) |
| Sound_Manager | Physics Collision Detection 7346 | each stopper (a Group Iterator) |
| Event_handler | Delete Collision Surfaces 4547 | none (the Level); its callback returns CKBR_OK, i.e. stays queued, while any physics object exists (`DeleteCollisionSurfaces.cpp`) |
| Ball_CollSound_Pieces | 12 Physics Collision Detection (41668-41797 stone, 41869-41998 wood) | the retail stone and wood pieces |

- **Which of them queue.** `--queue-trace` on the arena (`r0_keys.bmrc`, a
  key held) finds the queue empty in every frame through the spawn, a death
  and its respawn, and two trafos (stone to wood, wood to paper): when the
  game itself takes a ball's body away, its navigation is inactive and the
  sound blocks run after the new body exists. An entry appears only in a
  session's own state, the body taken out while Gameplay_Ingame still
  drives the ball (`--ball-off 600 700`, the A6 drop's state): two key
  edges queue 26418 four times, 26414 and 26432. Every refusal logged under
  `maps/arena-test` (all runs on disk) names, of these 28, only 26418 and
  26432.
- So the observed producers are Gameplay_Ingame's. Sound_Manager,
  Event_handler and Ball_CollSound_Pieces are producers by code only (a
  missing target when they run); none was seen queued. Q4 holds every
  outside entry whoever made it, so none needs a rule of its own; if one is
  ever seen, its effect is the argument above (collision listeners for the
  sounds, no force). Delete Collision Surfaces 4547 has no target and
  nothing to wake or push; once queued it stays queued (the table above), and
  Q4 holds it outside every replay with the rest, so no replay runs it.

**The WakeUp fact (step G0, measured).** A WakeUp queued against a bodiless
ball runs in the first `Process()` that finds the body: the step of the frame
that made it (the retail Physicalize and the bridge's `physicalize` make the
body before that frame's step). The ball is not simulated yet then: a
non-frozen Physicalize calls `ensure_in_simulation` (CKIpionManager.cpp:643-644,
669-670), which for an object in `IVP_MT_NOT_SIM` only puts its core in the
revive list (ivp_object.cxx:1324-1329), and `add_revive_core` returns at once
for a core already there (ivp_physic_private.cxx:872-877). The WakeUp's own
`ensure_in_simulation` therefore changes nothing; its `NotifyScriptWakeup`
only adds a `script_wakeup <own ball>` event-log entry, which the core drops
(`reportable_wakeup` excludes the own ball). Measured with `--ball-off 600 700`
on the arena (`r0_keys.bmrc`, SimTool `--replay`), two arms with the same
inputs: the four WakeUps pending at the re-make (the two forces dropped,
`--ball-off-wake-only`) against every entry dropped (`--ball-off-drop-all`,
queue empty): world hash, pose hash, IVP time and core count identical in
all 2400 frames (`bmrc_hash_cmp.py`), the ball `sim=1` at the re-make in
both (`--queue-trace`). The first measurement's control flattened the two
key edges out of the record instead (inputs different), with the same
result. In lp19_a6_drop every own birth record (231, 2522, 2642, 2772,
2780) has `st=1` too; that run is a session journal, which `--ball-off`
(a `--replay` switch) cannot run, so its with/without pair is the arena's
above, whose state is the A6 drop's (the own body out while
Gameplay_Ingame drives the ball and its blocks queue). Condition: a body made
after its frame's step would be found simulated, and the WakeUp would reset
its calm references (`reset_freeze_check_values`, ivp_object.cxx:1320-1323);
no session path makes the own body there.

**Hooks.** `rollback_world` gains two optional hooks, filled by
`bind_script_replay`:

```cpp
std::function<int(uint32_t anchor)> hold_outside_callbacks;   // callbacks_hold(executed scripts)
std::function<void()> unhold_outside_callbacks;               // callbacks_unhold
```

**Placement in `restore_and_replay`.**

- `hold_outside_callbacks` runs as soon as `replay_follows` is known (RB:827),
  before `restore_lives`, the scripts, the blob or any body. Nothing in the
  restore can then run an outside entry. It runs whether or not the anchor
  has a script record (review 1 #10).
- An RAII guard created there calls `unhold_outside_callbacks` on every exit
  of `restore_and_replay`: after the replay loop, after a `world.step()`
  failure (RB:1278), and on early returns.
- A frozen clock or a too-far window (RB:1100-1125) has `replay_follows`
  false: nothing is held.

**Order after the replay.** The held entries go back after the entry each
followed (§2.2.2). Entries of one block are interchangeable (§1.1), so "after
the first entry of block b" restores the live order whenever the replay ends
with the same selected entries as the live run. A clamp means it did not; the
count goes to `cbq unhold_clamped`, and the gate is 0 on every run without a
logical script difference.

**Several rewinds in one frame.** Each restore holds and its guard unholds,
so the next restore holds again from the live order.

#### 2.2.5 Q5: the selected entries in a resync

RSD's resync restores the scripts from the server's portable record
(RSD 2.4) and re-steps from S. The selected entries are script state, so the
server's must travel with it.

- **Portable section** (portable version +1), after the three cells:

  ```
  u16 count; count x { str owner_name, str script_name, u32 walk_position, u8 slot }   // queue order
  ```

  `walk_position` is the block's pre-order index in its script, the walk RSD
  already uses to identify a block (RSD 2.4 "Keys").
- **Server.** `capture_scripts_portable()` (PW, RSD §2.3) reads its queue
  with `callbacks_get` through physics_direct and writes the entries of its
  selected scripts. It only reads: the server's world, hash and journal do not
  change.
- **Client.** `localize` (RSD 2.4) maps each entry to the local block id by
  key and walk position, and writes the section of the local record. An
  entry of a script the local selection lacks, or a walk position the local
  script lacks, counts as `unportable` (RSD's counter; gate 0 on the arena).
- **Outside entries** are not in the portable record. The resync's restore
  holds them like any restore (Q4).
- An old portable record (version 1, from a journal) has no section. Its
  `localize` writes an empty one, which removes the selected entries; the
  SimTool journal reader accepts it for old journals only.

#### 2.2.6 Placement per world

| world | Q1 | Q2 | Q3 | Q4 | Q5 |
|---|---|---|---|---|---|
| S | built | `callbacks_get` only | — | — | `capture_scripts_portable` reads its queue |
| H | yes | physics_direct | hooks from RBS; scope per restore | hooks from RBS | `localize` |
| RC | yes | physics_view | the same | the same; the held list is the bridge's (no `BallanceMMOClient` member) | `localize` |
| RL | yes | physics_view | the same (PSC:3228 binds RBS) | the same | — (RL has no resync state) |
| SimTool | yes | physics_direct | `--savestate-check` gains the scope switch and queue checks (§7.2) | `--hold-outside` | `--portable-scripts-check` covers the section |

### 2.3 L: lives that end inside the window

#### 2.3.1 L1: the engine finds the ended lives and asks first

`restore_and_replay` (RB:670) gets a step after `replay_follows` and the hold,
before the plan (RB:720).

1. **Ended lives.** For every name with a body record at A (own entity,
   remote entities, mechanisms), its end is the first record t in (A, now]
   whose generation for that name differs from A's.
   - A record without a body but with A's generation is **not** an end. That
     is a moved birth's gap: `move_birth` (RB:1567-1600) erases the body from
     the records of one generation from `origin` on and keeps the generation
     (review 2 #4). Every real end bumps the generation (§1.2).
   - A life still alive now has no end.
   - The list is `ended = { name, generation_at_A, end_tick }`.
2. **`world.restore_lives(anchor, now, ended, out)`**, a new optional hook in
   `rollback_world`. `bind_session_world` (SHW:264) composes it:
   - the core's half first: balls (own and mirrors), L2/L3;
   - then the host's half: mechanisms, L4.
3. Each half returns per name, in `out`:
   - `revived {until = end}`: the A-life's body exists again; its state comes
     from the A record; it is removed at `end` by the replay;
   - `remade_at = E`: a later life of the name was taken out here and will be
     made again by the replay at E;
   - a name can carry both (RC mirror beam pair, H own same-name rebirth, L4
     reborn piece);
   - `unrevivable`: logged and counted; the gate is 0 outside the documented
     class of §2.3.3.
4. **The plan item** gets the two halves explicitly (review 1 #1):

   ```cpp
   struct restore {
       ...                              // as now: state, mode, extra = what the restore writes at A
       uint32_t revived_until = 0;      // != 0: a revived life, written from the A record, ends at this tick
       uint32_t repose_tick = 0;        // != 0: a birth at this tick (as now)
       struct birth_half { bmmo_physics_body_state state{}; wake_mode mode{}; bmmo_physics_core_extra extra{};
                           bool have_extra = false; } birth;   // the birth record's, for the re-pose / compare
       ...
   };
   ```

   - A `revived` name takes the recorded branch with `at->bodies[name]` and its
     extra, bypassing the generation test of `record_at` (RB:701-706).
   - A `remade_at` name gets `repose_tick = E`, `birth` from its birth record
     (RB:731-765), and `recreate = true` (it is already taken out).
   - A plain birth (neither) keeps today's item, with `state` and `birth` both
     the birth record.
5. **Every reader of the item, changed:**

   | reader | today | with L1 |
   |---|---|---|
   | blob `covered` (RB:946) | `!from_row && !repose_tick` | `!from_row && (!repose_tick \|\| revived_until)` |
   | destroy loop (RB:920-934) | every `repose_tick` item | skips items with `recreate` already set; balls by `keeps_parked` (§2.4) |
   | per-body write (RB:1012-1040) | `parked_inert = savestate_applied && repose_tick` | `&& !revived_until` |
   | extra at the write (RB:1046), unit pass (RB:1060) | `!repose_tick` | `!repose_tick \|\| revived_until` |
   | contacts (RB:1071) | skip `repose_tick` | skip `repose_tick && !revived_until` |
   | `born_now` (RB:1192) | `repose_tick == t` | unchanged |
   | birth compare / re-pose (RB:1287-1330) | `item.state` | `item.birth.state`, `.mode`, `.extra` |
   | `replay_remotes` parked test (RB:1251-1253) | `repose_tick > t` | `repose_tick > t && !(revived_until > t)`: the A-life is driven and recorded over [A, D) |
   | `replay_mechanisms` early-made test (RB:1362-1366) | `repose_tick > t` | only for `t ≥ revived_until` |
   | end-of-replay `births` test (RB:1404) | `repose_tick && !recreate && !inserted` | unchanged (a dual item has `recreate`) |
   | plan trace (RB:804) | births / rows / records | adds `revived` |

6. **Step P's revive call site (RB:863-882) moves into the mechanism half of
   `restore_lives` unchanged at G6** (its `revivable` set, its savestate-only
   condition). L4 (G9) then generalizes it (review 2 #8).
7. **Only when a replay follows**, on both the blob and the per-body path. A
   refused blob no longer destroys the revived bodies (RB:973-974). They stay,
   and the per-body path restores them from their A records.

The hook runs before the scripts' restore (RB:902): a force block restored
there needs its body (step P's reason, RB:856-860).

#### 2.3.2 What every half guarantees

- **Muted.** Every make and unmake runs with the bridge event log muted
  (`event_log_mute`, CL:369-406). Nothing is reported to the server, and the
  drained `created`/`deleted` entries never bump a generation for a restore's
  own work.
- **No kick at a revive.** The A record carries the state; a spawn kick
  belongs only to a birth.
- **Generations label the life a record's tick holds.**
  - Mirrors: the core bumps generations explicitly (CL:218, :458, :485) and
    re-runs those bumps in the replay. So L2 sets
    `s.generations_[entity] = at->tracked.generations[entity]` (0 if absent)
    for every mirror entity with a life event in (A, now]. The replayed events
    bump as the live ones did, and the final value equals the live one.
    `replay_remotes` (RB:1241-1275) reports the core's counter per replayed
    tick, so the re-captured records carry each tick's life.
  - Own ball and mechanisms: their bumps come from the drained event log in
    live frames only. The restore leaves `s.generations_` alone. The replay's
    records take each tick's generation from the live record of that tick
    (`step_tracked = recorded->tracked`, RB:1152).

#### 2.3.3 L2: mirrors (core)

**The life log.** RSD's `s.life_log_` (RSD 2.6.1) records every remote
lifecycle event applied. Each entry holds, explicitly (review 1 #15):

- `stamp`: the event's applied tick from the server (`event.tick`,
  `pending_birth::tick`, CC:213);
- `made_at`: the tick this world applied it (the stamp inside a replay or a
  lifecycle rewind; the live tick L on RC with `lifecycle_rewind = false`);
- player, type (Physicalize or Unphysicalize), and the event (pose, recipe,
  ball type, flags, origin);
- **`entity`**: the mirror entity the event made or removed;
- the generation it gave that entity.

This design adds two rules to RSD's:

- **The entry is made before the event's own rewind**, so
  `rewind_for_lifecycle` (CL:186) finds it.
- **The trim keeps each player's newest Physicalize older than the horizon**
  (`history_ticks + 128`) besides every newer entry (review 1 #2). The A-life
  of any anchor in the history is the player's newest Physicalize stamped at
  or before A. If that is older than the horizon, no Physicalize of the player
  lies between it and the horizon, so it is that kept entry.

If L lands before RSD's A3, L builds `life_log_` to this definition.

**The entity in a replayed birth (review 1 #3, review 2 #1).**

- `pending_birth` (CC:212-227) gains `std::string entity`. Filled from the
  log, it names the entity the live event made.
- `physicalize_remote` (CL:449-452) uses `event.entity` when set and skips
  `mirror_entity`. Inside a replay no mirror entity is ever minted again.
- H's `make_mirror` (SC:917-931) resolves `spec.entity` by exact name first
  (`GetObjectByNameAndClass(spec.entity, CKCID_3DOBJECT)`), then the two
  existing ways. The old clone survives `unmake_mirror` (SC:934-937), so the
  revive and the remake find it. No clone is made by a rewind, and none leaks.
- RC's `make_mirror` (CH:1435-1453) already resolves by (player, type), which
  gives the same name. Its presentation side effects (`mirror_type`,
  `set_physicalized`, `on_trafo`) re-run with the replayed events and end in
  the live state; G7 checks that.

**The reconcile** (`reconcile_remote_lives(A, now)`, the core's
`restore_lives` half). For each player with a log entry stamped in (A, now]:

- the **A-life** is the newest Physicalize stamped ≤ A with no Unphysicalize
  (or later Physicalize) stamped in (birth, A];
- the **window events** are every logged event stamped in (A, now], in stamp
  order.

Steps, in this order:

1. **Take out what the window made.** Every mirror body of that player that
   exists now and whose life began in (A, now] is unmade
   (`unphysicalize_mirror`, muted), with its navigation detached (CL:212-224).
2. **Revive the A-life**, if it has a body record at A and no body now:
   - `make_mirror` with its logged entity and the spec of its Physicalize
     (recipe, ball type, group `P#<join_order>`), at the A record's pose,
     `kick = false`;
   - `attach_remote_navigation`;
   - `s.remotes_[player]` set back to that life: entity, ball_type,
     `physicalized`, `navigation`, `born_tick`, `driven_from`, corrector
     cleared;
   - the generation set as in 2.3.2;
   - reported `revived {until = its end}`.
3. **Queue every window event at its stamp** (review 1 #4, review 2 #6): each
   Physicalize to `pending_births_` (with its entity), each Unphysicalize to
   `pending_deaths_` (RSD 2.6.3). A name made again at E is reported
   `remade_at = E`. This covers:
   - a late birth at its own rewind (A = E-1): no body yet, the entry is in
     the log, so it is queued and made at E;
   - a life that begins and ends inside the window: made at E, removed at D';
   - a trafo: the next Physicalize with another entity ends the A-life inside
     `physicalize_remote` (CL:456-460), because `remotes_[player]` is the
     A-life again.
4. A life alive at A that has a body now and no window event needs nothing.

**`make_remote_birth`** (CL:236-258) logs the event, then rewinds. It no
longer pushes `pending_births_` itself; the reconcile does. Its fallback stays
explicit (review 1 #6): after `rewind_for_lifecycle`, if the player's remote
is not physicalized with this event's entity, the birth is made live at
`live_first_tick`, and the log entry's `made_at` is set to that tick. That
covers a refused rewind (unknown anchor or too deep, RB:1626-1638), a frozen
restore (RB:1100-1111) and a replay cut short.

**In the replay.**

- `drain_pending_births` (CL:381-409) becomes `drain_pending_lives(t)`: due
  deaths and births of t, plus the own lives due (`apply_own_lives`), plus the
  own deaths and remakes of L3.
- Remote events are sorted as `drain_due_events` sorts live events: join
  rank, applied tick, origin, `lifecycle_apply_before` rank (CL:110-118). A
  death and a rebirth of one stamp (a beam pair) come out in the server's
  order.
- A death runs `unphysicalize_remote(player, t)`; a birth runs
  `physicalize_remote(birth, t)`. Both are muted, as now.

**The RC stamp rule.**

- Remote events with a stamp in (A, now] are applied at the stamp in every
  replay, whatever tick this world applied them at live.
- **Documented class.** A birth stamped at or before A but made live after A
  (RC, `lifecycle_rewind` off, made from the row at L) has no A record and
  usually no row. It cannot be revived at A. It keeps today's path (birth
  record at L, parked) and is counted `lives late_birth_kept`, not
  `unrevivable`. R3 (`lifecycle_rewind` on) removes the class. In the user's
  order R3 lands before this track (audit §5), so the class should be empty
  when L lands; the gate requires 0 then.

**Behaviour change, declared (review 1 #8).** Today a remote birth inside a
window is parked: `destroy_body` refuses mirrors (SHW:335-336). With step 1
and 3 every such birth is taken out at the restore and made again at its stamp
by `physicalize_remote`: the path a late birth's own rewind already takes,
measured 0.0000 m against the owner (session-client-core design R3 row). It
affects every rewind that crosses a remote birth, the spawn piles included.
The blob then meets the object set it recorded, instead of an unknown parked
body. §7.3 L-H6 measures it against the parking path in one batch;
`config.remote_births_parked` keeps the old path for that arm only.

**Inputs of an ended life.** `recorded_input_at` (CF:261-283) maps a name to
a remote by `remote.entity == entity`. For an H life whose name is not the
current one, it looks the name up in `life_log_` (entity → player) and reads
that player's ring. Frames of an earlier lifetime are kept in the ring
(CL:492-500).

**Rows.** A snapshot rollback's row for a revived mirror outranks its record
(RB:766-770), as for any body. The revive happens before the plan, so the
row's write finds the body.

**RSD's resync** (RSD 2.6.3, the mirror rows of its table) becomes a call of
this reconcile at S, with the server's row as the source for a mirror alive at
S. Its "mirror in the anchor, no row at S, Physicalize in (S, R]" is step 1
and 3. Its "mirror row at S, absent from the anchor, Unphysicalize in (S, R]"
is step 2 with the row, then step 3. One implementation.

#### 2.3.4 L3: the own ball (core; both hosts)

- **Own deaths in the log.** `report_unphysicalize` (CL:604-613), the single
  site both own-lifecycle sources reach (CF:469, CL:636), appends
  `own_death { tick, entity, generation, matrix }` to `s.own_life_`, as a third
  `own_life::kind` (CC:183).
  - `matrix` is the own entity's CK world matrix at that moment (frame D,
    after its scripts and step), read by a new platform op
    `entity_matrix(entity, float[16])` (CP; H and RC read
    `CK3dEntity::GetWorldMatrix`). Review 1 #7, review 2 #5.
  - It retires under the rule of the other entries (CL:958-969): once no
    rewind can be anchored before it.
- **The life test at a replayed tick** (review 1 #5). `own_life_current`
  (CL:776-779) compares with the live generation, so a replay retires every
  entry of a life that has died since (CL:875-883). New rule:
  - live frames (`live = true`): unchanged;
  - a replay at tick t: the life is current if its generation equals the
    generation of its entity in the live record of t-1 (the world the change
    at t acts on). RB gains the read `generation_at(entity, tick)`;
  - a replay never sets `retired`. Only `live_own_lives` and
    `retire_own_lives` retire (CL:927-969).
  - With this, a beam at 650 inside a revived life that dies at 652 is applied
    in the replay.
- **Revive** (the core's `restore_lives`, own part). The own entity has a
  body record at A and no body now, or the same-name step below took its
  later life out. The core then:
  - calls `make_own(spec)` muted, with the A record's pose (rotation from its
    quaternion), the recipe `ball_recipe_of(entity)`, the group `own_group_`,
    no kick, and `spec.with_replica = true` (the host re-attaches the
    navigation replica, so `set_nav` (RB:935) and the blob find its
    controllers);
  - saves the entity's current CK matrix (the pre-rewind one) for the guard;
  - queues the `own_death` at its tick D;
  - reports `revived {until = D}`.
- **In the replay at D:** `unmake_own(entity, matrix of own_death, D)` muted,
  the replica detached first (CL:784-805), at the end of
  `drain_pending_lives(D)`: after the remote events of D, before the scripts'
  frame. Live, the retail scripts removed the ball in frame D's behaviour pass
  after the drain, and Gameplay_Ingame runs before the modules (SRP:492-495).
- **At the replay's end** (the `keep_live_cells` guard): the own entity's CK
  matrix is set back to the saved pre-rewind matrix. The guard already ends
  the explosion override there. Between D and now the live scripts may have
  moved the dead entity; the replay does not run them, so the guard puts the
  live result back.
- **Own events inside the window** (review 1 #4, own half). For each own
  birth stamped in (A, now] (an `own_life` birth entry):
  - **parked**, as today, when its body exists now and no earlier life of its
    name is revived in this window (RC respawn 1567, spawn 212);
  - **remade at E** when an earlier life of the name is revived (H, §1.2) or
    when its life also ended inside the window (no body now). The live body,
    if any, is taken out at the restore (`unmake_own`, muted). At E,
    `remake_own_body(life, E)` (CL:808-845) runs at the end of the drain like
    the death above. The name is reported `remade_at = E`, and its own death
    in the window, if any, is queued like a revived life's.
- **RC capability.**
  - `own_body_control` stays false (CH:542): the live game's Physicalize block
    owns the own ball (design 2.3), and R6 is the job that changes that.
  - New capability `caps.own_body_in_replay`: the host fills
    `make_own`/`unmake_own` (CP:468-470) and the core calls them **only**
    from `restore_lives` and the replay's drain. Every body they make is
    removed again at a tick ≤ now, and the guard puts the matrix and the
    replica back, so the live world after the replay is the pre-rewind one.
  - RC implements them on physics_view (`physicalize`/`unphysicalize`,
    `navigation_destroy` and the replica's create from `attach_own_replica`,
    SHS:136-157). The body part moves to host support as
    `make_own_body`/`unmake_own_body` templated on the backend. H's
    `remake_own_body`/`unmake_own_body` (SC:1626-1670) become calls of it.
  - RC saves `host.own_replica` at the revive and puts it back in the guard,
    because live the replica follows the dead ball.
  - Without the capability, an own life ending inside a window is counted
    `unrevivable` and logged by name. The gate requires 0.
- **Selected entries on the own ball.** P_Modul_18's SetPhysicsForce 46475
  (the fan) is created on its Box-Box test of the ActiveBall cell (Get Cell
  46392, audit #13; `d_P_Modul_18_MF_Script.txt`), so its target is the own
  ball. Q3 restores its entry; inside [A, D) it acts on the revived body as it
  did live. **Confirmed (step G0):** `--list-callback-blocks` gives 46475's
  target parameter's source as Get Cell 46392's output (CurrentLevel[0][1],
  the ActiveBall cell; null until it runs), and `--dump-script` shows its Pos
  Referential and Box Box Intersection's Entity 0 from the same Get Cell, which
  runs on the inner proximity 46429's Out Range. The ids are the level
  file's, the same in every world (the retail and headless refusals name
  Gameplay_Ingame's 26418/26432 with the inventory's ids).
- **Found at G0: the dead own entity moves live after D.** Scripts no replay
  runs move a dead own ball's CK entity, and `Ball_Pos_Frame` with it (every
  proximity reads that frame):
  - the death sequence puts it on the resetpoint at D+1 (eg0_f13: 1369
    (300.851, -65.772, 15), 1370 (-20, 3.65, 15));
  - the Trafo Manager moves the dead stone through the trafo animation
    (0.83 m at 653, < 0.0001 m by 731).
  `own_death.matrix` (the matrix after frame D) is right for D only: a
  replayed tick after D+1 needs that tick's live matrix, or its proximities
  read another frame than the live ones did. In eg0_f13, 22 of 207 rewinds
  replay `Ball_Pos_Frame` differently from the live run: 11 over a birth or
  a beam stamp (the parked pose; B and parked births), 7 after the stone's
  death in the trafo (652→676 .. 730→754, ≤ 0.83 m), 3 across a death
  (639→663, 1354→1378, 1367→1391; up to 328 m) and the one-tick snapshot
  correction 1300→1301 (8.6 mm: the retail in-place beam's own residual,
  which the control without forced rollbacks has too, As built EG0). §9.7.

#### 2.3.5 L4: mechanisms (host support)

- **The rule.** A mechanism whose life ends inside the window is revived
  when the replay ends it again with a replayed selected script:
  - the trafo pieces (`Ball_ResetPieces_*`'s De Physicalize, TRD 4.2);
  - every TRD set's pieces;
  - any selected module script that unphysicalizes its own bodies.
- **Mechanism deaths made by outside scripts** must not happen in a session.
  The 9.28 pins keep the level reset from tearing a sector down; SPD keeps
  sectors from being deactivated; the PE_Balloon teardown is exception (c).
  If one does happen, L counts it as `unrevivable` with the name, and the
  gate requires 0. A non-zero count is a new gap, to be traced to its script.
- `revivable` (SHW:244-249) therefore becomes "a name the host's selection
  can kill": the registry of TRD's sets plus any selected module body, not
  `explosion_pieces` alone.
- **Same-name rebirth inside the window** (a second explosion of one type
  set, TRD 2.9, or a reset and an explosion inside one window):
  - the live body of the later life is destroyed first, through the wrapped
    `world.destroy_body` (RBS:84-99), which releases the joint handles held on
    it;
  - then the A-life is revived (`lifetime_at(entity, A)`, SHW:81-88), as
    step P does;
  - the name goes out as `revived {until = R}` and `remade_at = E`.

  The replayed reset removes the A-life at R; the replayed explosion makes the
  later life at E; the birth check compares it with `item.birth`. This is the
  case step P logs as its known gap (RB:869-880).
- **Per-body path.** The revive now also runs when no blob is applied, so the
  per-body path finds the body and writes it from its A record (L-S2).
- **TRD's retirement** stays compatible: a set is destroyed only after no
  record lists it (TRD 2.4). Its pieces' last death is then older than the
  history, so no window reaches it, and the registry can drop the set from
  `revivable` at the deselect.

### 2.4 B1: the beam keeps the generation; parked balls counted apart

- CL:703-704 goes. A navigated beam keeps the generation on both capability
  settings:
  - without own body control, its stamp is the in-place `write_beam`, live
    and in every replay (CL:883-890);
  - with it, the muted remake keeps the generation (CL:808-845).
- The non-navigated branch (CL:700-702) keeps its bump. That move is an
  in-place write outside every replay, made at the request, and there the bump
  is right.
- The beam's `own_life` takes the unchanged generation (CL:710-713), and the
  life test (2.3.4) holds as before.
- Nothing else reads the bump: echoes for the pair are acked or "kept where we
  stamped it" (CL:1101-1123), and `rebase_own_lives` keys on the origin tick
  (CL:977-989).

**Counter split (review 2 #13).** `destroy_body` returns a bool, so RB:931
cannot tell a ball it refuses by design from a failure. `rollback_world` gains
`std::function<bool(const std::string&)> keeps_parked`, which
`bind_session_world` fills from the `ball`, `own_body` and `mirror` hooks
(the tests SHW:335-336 already makes). The destroy loop asks it first: a ball
counts `births_parked_balls` and is not passed to `destroy_body`. With L2 a
mirror birth is taken out by the reconcile (`recreate` set before the loop),
so only parked own births reach this test. `birth_recreate_failures` then
means a real failure, and its gate is 0.

### 2.5 Where the code lives

| piece | place | used by |
|---|---|---|
| factory, detach/insert/free | PRT (#20) | the bridge |
| queue read/rebuild/hold/unhold/scope | PS + API v21; physics_direct, physics_view | S (read), H, RC, RL, SimTool |
| record queue section, `queued_callback`, hooks | SRP | all script replay users |
| hooks filled from the backend; hold/unhold hooks; scope per restore | RBS (`bind_script_replay`) | H, RC, RL |
| portable section, `localize` mapping | SRP (portable, RSD 2.4) | S (capture), H, RC |
| hold guard, ended-life detection, `restore_lives`, two-half plan item, `generation_at`, `keeps_parked`, counters | RB, SHW (`bind_session_world`) | H, RC |
| `life_log_` fields and trim, reconcile, `pending_deaths_`, `pending_birth::entity`, `drain_pending_lives`, `own_death`, replay-tick life test, generations, `recorded_input_at`, `make_remote_birth` fallback | CC, CL, CF | H, RC |
| `caps.own_body_in_replay`, `entity_matrix`, `make_own_body`/`unmake_own_body` (body part), matrix and replica restore in the guard | CP, SHS; CH ops; SC ops call the shared part | RC, H |
| exact-name mirror lookup | SC `make_mirror` | H |
| beam generation | CL | H, RC |

Every session decision is in the core or in shared host support. The hosts
add platform operations only (CH, SC). RC adds no `BallanceMMOClient` data
member: the held list is the bridge's, and the saved replica and matrix live
in CH's file-scope `host` struct.

---

## 3. Determinism

**Live frames do not change.**

- The server's world, hash and journal are untouched. Its only new work is a
  read of its queue when it builds a resync state (Q5).
- In live client frames B1 changes a bookkeeping label, `own_death` and the
  life log store data, and nothing else changes. Every other change acts at a
  restore or inside a replay. The live worlds of all players (the lockstep
  invariant) are what they were.

**Q.**

- **Selected entries.**
  - The block's local state (SRP) and its queued entries come back together.
  - An entry is stateless and was queued un-executed (§1.1), so a factory
    entry appended un-executed is the anchor's entry.
  - The order is the anchor's. It matters: `Process()` runs the newest first
    (`PhysicsCallback.cpp:94`), two entries that make controllers on one core
    make them in that order, and selected entries can target the own ball
    (the fan, 2.3.4).
- **Outside entries** never run inside a replay, so they cannot reorder
  anything the replay makes. Their live effects are present from A on, as
  they are today (2.2.4 "What the replay then lacks"). After the replay they
  are where they were.
- **BMMO entries** keep their places; the order argument is in 2.2.3.

**L.**

- The same functions with the same arguments, at the same ticks, as the live
  or server path:
  - `physicalize_remote`/`unphysicalize_remote` with the logged events and
    entities;
  - `remake_own_body` with the own birth record, `unmake_own` at the own
    death;
  - the replayed selected scripts for mechanisms;
  - for a revive, the A record (blob or per-body).
- What the replay re-creates is what a lifecycle rewind already re-creates
  bit-exact. The arena's mirror births at their stamp measured 0.0000 m
  against the owner (session-client-core design R3 row).
- **Changed relative to today, declared:**
  - **A remote birth inside a window** is remade at its stamp instead of
    parked (2.3.3). §7.3 L-H6.
  - **The anchor-D-1 rewind of a late death** no longer skips the dead body
    at the restore. It restores it from the blob and removes it before step D
    the way IVP removes any body. That is the server's path (a deletion
    before the step of D) instead of a blob entry skipped with its contacts
    dropped. The body set from D on is unchanged. §7.3 L-H2 judges it against
    the server.
  - **A revived own ball in [A, D)** lacks the outside controller its A-life
    had at A (`rt:force/26432`): that controller died with the core at D, and
    `order_controllers` skips "no live controller" (SSR:1584-1636). The
    opposite case, an outside controller made live after A that is present
    from A on, is today's behaviour (2.2.4). Both are zero-force controllers
    of Gameplay_Ingame; §7.2 L-S3 measures them, 9.3 decides.

**B1** changes no physics. The records before the stamp become usable, and
the replay then reproduces the live path that the in-place write already
reproduced.

---

## 4. Rollback interplay

| situation | behaviour |
|---|---|
| snapshot rollback (rows) | the hold and the revives come before the plan; a row outranks the record for a revived body (RB:766-770); a remade-at-E body's rows are outranked by its birth, as today (RB:731-736) |
| input rewind, forced rewind | as any restore |
| lifecycle rewind (A = D-1 or E-1) | the reconcile handles the late event's own life from the log; `make_remote_birth` keeps its live fallback (2.3.3) |
| resync (RSD) | one reconcile, row-sourced at S (2.3.3); `pending_deaths_` shared; the synthetic anchor's `tracked` (RSD 2.6.1 step 3) gives the generations; the selected entries come from the server's portable record (Q5); outside entries are held like any restore |
| frozen clock / too-far | `replay_follows` false: no hold, no `restore_lives`; `make_remote_birth` makes a late birth live |
| refused blob | revived bodies stay (2.3.1 step 7); the queue was already rebuilt by Q3, so a refusal now means a real difference, logged with its entries |
| several rewinds in one frame | each restore holds and its guard unholds |
| TRD sets | CollSound's entries are selected entries: restored by Q3, compared by the scoped check, carried by Q5. A set made after A is in the scope with an empty target (2.2.3). TRD 9.2 ends in (b) without a special rule. A set's retirement deselects it only when no entry of its blocks is queued, else waits a frame: a deselected set's entry would turn into an outside entry |
| TRD trafo frame | the copies run outside the behaviour manager (TRD 2.5) but are selected: their entries are Q3's |
| SPD clones | the cloned module scripts join the selection at the anchor; scope and executed list follow; no mechanism dies (SPD) |
| F4 (late Sector rewind) | unrelated; its rewinds use the same restore |
| `kept_live` (legacy v14 rule, RL) | a kept-live script is neither written nor executed: its entries are held like outside entries |

---

## 5. Wire, protocol and journal

| item | change |
|---|---|
| protocol | **11** (nominal): the portable record carries the queue section (Q5), in RSD's `full = 2` payload (RSD:1116), whose layout is unchanged. If G5 lands in the same commit as RSD's A2-A3, it shares protocol 10 and portable version 1 instead |
| portable record (RSD 2.4) | version +1: the queue section after the cells; `localize` reads version 1 (no section) for old journals only |
| server journal | none: the RESYNC record (RSD §5) stores the portable bytes, which carry their own version |
| client journal | no format change. Counters in the status and window lines (`client_core_report.cpp:173-268`): `cbq=rebuilt/removed/held/unhold_clamped/rebuild_fail/scope_refused/no_record`, `lives=revived_own/revived_mirror/revived_mech/remade/inside_window/unrevivable/late_birth_kept`, `births=… parked_balls` |
| script replay record | `kVersion` +1 (v-Q, after the v3 sections of TRD and the sibling designs): the queue section (2.2.3); an older record is refused (`snapshot version`, SRP:452), as at every bump |
| savestate blob | layout and capture unchanged; the compare is scoped |
| bridge | API v19 → v21 (TRD's v20 between): 2.2.2 |
| engine | change #20 |

---

## 6. What must not change

- **Live play** on every side. Live frames' bodies, scripts, hashes and
  journals are bit-identical to the previous step's (§7.3 Q-H0 and L-H0
  controls).
- The **server's** world and every server journal; the server never depends
  on snapshots.
- **BMMO's own callbacks** stay out of every capture, compare, rebuild and
  hold (`is_bmmo_callback`, SSI:463-471).
- **`check_callbacks` stays fail-closed** for every entry in scope. SimTool
  without a scope compares everything, as today.
- **Retail behaviour of the queue**: no dedup, no change in the blocks'
  functions, a double Create in a live frame still leaks as retail does, on
  every world alike.
- **The force hooks**, `restore_force` included, keep their contract (#18
  names, v14).
- The **death's own rewind** stays anchored at D-1 with the same body set from
  D on (§3 notes the changed IVP path).
- The **9.28 pins**: no level-reset teardown or rebuild; N1's death
  neutralization; nothing a reset destroys "for good" becomes revivable.
- **Own births without an earlier life in the window** whose body exists now
  stay parked (2.3.4).
- **RC**: `own_body_control` stays false; no `BallanceMMOClient` data member;
  the live own body is never made or taken out outside a replay.
- **H's own-body control paths** (moved births, beams) keep their behaviour.
- **The non-navigated beam** keeps its generation bump.
- **The time factor's keep-live rule** and step P's live Random cursor.
- **Exception (c)**: nothing here touches `PE_Balloon*`.

---

## 7. Verification

Every check gives its pass criterion. A failed check is reported as symptom,
cause and options, and is not waived.

### 7.1 Unit tests (`BallanceMMOMessageTests`)

| test | file | pass |
|---|---|---|
| Q-U1 record round trip | `rollback_binding_test.cpp` (fake queue backend) | capture with 3 selected + 2 outside + 1 BMMO entries writes the 3 in order; restore with a live queue of 5 selected (2 extra copies) rebuilds exactly 3 in recorded order; outside and BMMO untouched; a factory refusal fails the restore and leaves the queue unchanged; a selected script absent from the record loses its live entries (TRD 4.5 case) |
| Q-U2 kept-live scope | same | under the v14 rule a script outside `replayed` is not rebuilt and its entries are held |
| Q-U3 hold and unhold | same | hold takes exactly the non-executed non-BMMO entries; the replay's `Process` never sees them; unhold restores the pre-hold order when the selected entries end equal, including interleavings [s1, o1, s2, o2]; a missing predecessor is clamped and counted; the RAII guard unholds on a step failure and on an early return |
| Q-U4 scoped check | savestate unit (SimTool Q-S1 if SSR has no unit harness) | entries outside the scope never refuse, live or recorded; an in-scope difference still refuses with its entries named; mode 0 compares as today |
| Q-U5 portable section | `rollback_binding_test.cpp` | capture → portable → `localize` round trip maps (key, walk position) to the local ids; an unknown key counts `unportable`; a version-1 record yields an empty section |
| L-U1 ended-life detection | `rollback_engine_test.cpp` | end tick = first record with another generation; a record without the body but with A's generation (a `move_birth` gap) is no end; a life alive now has none; `restore_lives` is called after the hold and before the plan |
| L-U2 two-half plan item | same | a revived name is planned from `at->bodies` and covered on the blob path; a revived + `remade_at` name is covered at A, driven and recorded over [A, D), absent over [D, E), compared with its birth record at E (`births_recreated_exact`); per-body path writes the revived body from its record; every reader of 2.3.1 step 5 |
| L-U3 mirror reconcile, H naming | `session_client_core_test.cpp` (fake platform whose `mirror_entity` clones a new name on every call and whose `make_mirror` resolves by exact name) | death at D inside the window: the old name revived at A with no kick, unphysicalized at D; no `mirror_entity` call inside the replay; generations: A+1..D-1 records carry the old life's generation, the final counter equals the live one; `recorded_input_at` of the old name returns the relayed frame |
| L-U4 mirror reconcile, RC naming | same (one name per player and type) | beam pair at one stamp inside the window: old life revived, new life unmade at the restore and made at E after the death in the server's order; final generation equals the live one |
| L-U5 RC stamp rule | same (`lifecycle_rewind = false`) | a death applied live at L > D is replayed at D; a birth stamped ≤ A, made at L > A is `late_birth_kept` |
| L-U6 own death | same | `own_death` recorded with the matrix; revive at A through `make_own` with `with_replica`; unmade at D with that matrix; the guard puts the pre-rewind matrix back; without `own_body_in_replay` → `unrevivable` |
| L-U7 own same-name rebirth (H) | same | live body taken out at the restore, remade at E by `remake_own_body`; `births_recreated_exact` |
| L-U8 late birth and its fallback | same | a late birth at its own rewind is made at E from the log; with the rewind refused (unknown anchor, too deep) or frozen, the birth is made live once |
| L-U9 life inside the window | same | a mirror life born at E and dead at D' within (A, now], no body now: made at E, removed at D'; same for an own life on H |
| L-U10 log trim | same | a player whose A-life Physicalize is older than `history_ticks + 128` is still revived (its entry kept); other old entries trimmed |
| L-U11 beam inside a dying life | same | beam at T, death at T+2, window over both: the beam is applied at T in the replay; no entry retired by a replay |
| B-U1 beam without own body control | same | no generation change at the request; `record_at` of the own ball succeeds for every anchor ≤ T-1; no birth item for the own ball at T |
| B-U2 counter split | `rollback_engine_test.cpp` | a ball birth item counts `births_parked_balls` and never reaches `destroy_body`; a refused mechanism destroy counts `birth_recreate_failures` |

### 7.2 SimTool (offline, `--root C:/Users/geekerwan/Desktop/Ballance-Agent`)

| check | recipe | pass |
|---|---|---|
| Q-S1 queue restore matrix | `--savestate-check` on the arena sector-4 scene (`--drop-sector 4`) with a new switch `--queue-block <block id>:<from>:<to>` that unphysicalizes the block's target between the two ticks so the module's own edge queues the entry (P_Modul_34's WakeUp 48462 and P_Modul_08's force 45709; ids re-read with `--list-scripts` on the current build), scope = the session selection | chained seeded restores reproduce every blob byte for byte; the selected queue equals the record at every restore; after the target's body returns, exactly one controller exists per force block (`describe_force_blocks`) |
| Q-S2 outside entries | the same with Gameplay_Ingame's WakeUp 26418 and force 26432 queued against an unphysicalized own ball (`--drop-player-at` + `--ball-off FROM TO`; G0 built `--ball-off` for `--replay` runs only, with two arguments instead of `<from>:<to>`: G4 adds it to the scene it uses), `--hold-outside` | no restore refused; no outside entry runs inside a replay; the queue after each round, order included, equals the straight run's; one controller per force block; the own ball's `simulated` at its re-birth equals the straight run's (the WakeUp fact of 2.2.4) |
| Q-S3 G0 inventory | `--list-scripts` + `--dump-script` over L1-L13 and the arena: every block of the ten prototypes, its owner script, selected or not, and its target parameter | the outside producers are exactly Gameplay_Ingame's (else each new one is listed with its effect, and 2.2.4's argument is made for it); the fan 46475's target recorded |
| Q-S4 portable section | `--portable-scripts-check` (RSD A1) with a queued selected entry at the capture tick | bytes identical after capture → localize; `unportable` = 0 on the arena |
| L-S0 single explosion across a reset | `--explode wood 300 --activate Ball_ResetPieces_Wood 330` with `--savestate-check --max-depth 48` | every restore anchored in (300, 330) revives the pieces; the replayed reset removes them at 330; blobs byte for byte (step P's case, after its move into `restore_lives`) |
| L-S1 piece reborn in one window | `--explode wood 300 --activate Ball_ResetPieces_Wood 310 --explode wood 330` with `--savestate-check --max-depth 48` | every restore anchored in (300, 310) revives the first life; the replayed reset removes it at 310; the replayed explosion makes the second at 330 (`births_recreated_exact`); blobs byte for byte |
| L-S2 revive on the per-body path | L-S0 and L-S1 with `--legacy-restore` | the revived pieces come from their records; no "in the savestate but not in the world" for a piece |
| L-S3 zero-force controllers | two replays: an own death with Gameplay_Ingame's force controller present on the own ball at A; and a live ball whose outside force entry runs inside the window | every body state of the replayed records bit-identical to the straight run's; the only blob difference allowed is the outside controller's absence (first) or presence (second) on the own core (9.3 decides whether that difference is closed) |

### 7.3 Headless harness (arena, `scripts/arena_map_test.py`, 4 clients, Arch server of the step's commit)

| check | recipe | pass |
|---|---|---|
| Q-H0 control | the step's binary, `--latency-ms 150` | server journal and every client's LOCAL rows bit-identical to the previous step's same-batch control |
| Q-H1 outside entries | the lp19_a6_drop recipe (A6 drop arm: own Physicalize dropped, keys held) | `sv=N/0/0`; no "PreSimulate queue differs"; `cbq` held > 0, `rebuild_fail = scope_refused = unhold_clamped = no_record = 0`; the queue (order included) and the own ball's controller list after each rewind equal the pre-rewind ones |
| Q-H2 selected entries | e8ab2_swing_n1 sector 4 at 150 ms and 10 % loss, plus the test-only verb `drop-body <entity> <from>:<to>` on client 3 for `P_Modul_34_Kiste` and `P_Modul_08_Schaukel` (the pre-N4 cases) | `sv=N/0/0`; queue size bounded by the live run's (no growth across rewinds); one controller per force block after the body returns; the dropping client's mechanism rows equal its own control's after the body returns |
| Q-H3 resync with a pending selected entry | RSD's resync_snapoff recipe with `drop-body` on the server's world for a module body across S (server test verb) | the resynced client's selected queue after the apply equals the server's at S; `unportable = 0`; its rows of that module equal the server's after the body returns |
| L-H0 control | as Q-H0 for L's binary | as Q-H0 |
| L-H1 remote deaths in windows | the ab2_s17_beam recipe at `--latency-ms 150`, deaths in the arena death zone, relay corrections | `cross_death.py` counts > 0 rewinds with A < D-1; for each, the dead mirror is logged `revived` and its death replayed at D; the restorer's skip list names no ball (per-name skip logging added in G6); no new `_BMMO_` clone appears after a rewind (clone count = lives); `unrevivable = 0`; a ball that touched the dying one within [A, D) matches the server (`first_divergence.py` over the death windows) no worse than the same-batch control |
| L-H2 the death's own rewind | same run | for every A = D-1 rewind, the body set from D on equals today's; the survivors' rows against the server (`first_divergence.py`) are no worse than the same-batch control's, in count and magnitude |
| L-H3 own death in a window | 4 clients, `session force-rollback 13:24` on client 1 (headless verb), deaths in the death zone | every forced window across an own death logs the own ball `revived` and its removal at D; the own entity's matrix after the rewind equals the pre-rewind one; no "differ from the live record" at those ticks; savestate replay mismatches 0 |
| L-H4 own same-name rebirth | a recipe producing an own death and rebirth < 48 ticks apart (the cov_e9 `tr` pattern) with forced rewinds across both | `remade_at` logged; `births_recreated_exact` for the own ball; no parked own ball during [A, D) |
| L-H5 pieces across a reset (after TRD) | TRD H5's forced pairs over a run with an explosion, reset and second explosion of one set within a window | every piece exact against the control |
| L-H6 remote births remade vs parked | the start pile at `--latency-ms 150`, 4 clients, same batch: arm (L) vs `remote_births_parked` | `births_recreated_exact` for every remote birth in the arm; start-pile host error against the server (`worst_bodies.py`) no worse than the parked arm; `births_parked_balls` counts only own births |

### 7.4 Retail (Level 13 arena, core on, Arch server of the step's commit)

`r2_keys.bmrc` and `r2_run.py` no longer exist (As built EG0). The `r2_a`
arms below are rebuilt with `.local-tools/work/EG0/eg0_retail.py LABEL
EVERY:DEPTH` (`EVERY` 0 for the control): `work/a4/r0_keys.bmrc` and the three
r2 beams (650, 900, 1300), whose events land on r2's ticks (spawn 212, stone
dead 652, wood 817, death 1369, respawn 1567); the arm and its control are
run with the same binaries. With this record the server's full snapshot of
the beam stamp 1300 corrects the own ball by 8.6 mm in every run, the control
too (As built EG0): the own ball's LOCAL rows from 1300 to that correction
depend on the snapshot's arrival tick and are compared from the correction
on.

| check | recipe | pass |
|---|---|---|
| B-R1 the beam arms | `r2_a_f13`, `r2_a_f50`, `r2_a_f131` (beams at 650/900/1300, forced 13:24, 50:12, 131:47) and the control `r2_a_ctl1` | no "differ from the live record" at 650, 900, 910, 917 (or any beam window); the plan trace shows no own-ball birth item at a beam stamp; `birth_recreate_failures = 0`; `births_parked_balls` equals the own births (212, 817, 1567) inside forced windows as the plan trace lists them; every non-piece body bit-exact against the control |
| L-R1 own death on retail | `r2_a` with one-shot forced rewinds anchored 1360, 1364 and 1368 (`session force-rollback at <live>:<depth>`, a new one-shot form: 1377:17, 1380:16, 1384:16), plus one anchored 640 over the beam at 650 and the death at 652 | each logs the own ball `revived` (RC `own_body_in_replay`) and its removal at D; the 640 arm applies the beam at 650; no script difference at the rewind's end (the 1378 "Current Distance" difference of r2_a_f13 does not appear); savestate replay mismatches 0; after the rewinds the own entity's matrix and replica state equal the pre-rewind ones; the respawn at 1567 is unchanged against the control |
| Q-R1 outside entries on retail | the same level with the own Physicalize held by the retail `hold-event` verb (A6 analogue) and keys held during the hold, forced 13:24 | `sv=N/0/0`; no refusal; `cbq` held > 0; queue order after each rewind equals the pre-rewind one |
| Q-R2 legacy path | `r2_a` with core off, forced 13:24, keys held while dead | no "PreSimulate queue differs"; `cbq` rebuilt/held counters present in the RL status line |
| L-R2 two retail clients | Level 11, two clients, core on, a beam pair of client 2 (RC same-name mirror) inside a forced window of client 1 | client 1 logs `remade_at` and `revived` for client 2's mirror; client 1's rows of client 2's ball equal the server's at the stamp and after; the spirit-ball presentation state (`mirror_type`, physicalized flag) after the rewind equals the pre-rewind one |

### 7.5 What each check proves

| claim | checks |
|---|---|
| live play unchanged | Q-H0, L-H0, B-R1's control comparison |
| the selected queue is the anchor's and refuses nothing | Q-U1/2/4, Q-S1, Q-H2, Q-R2 |
| outside entries never run in a replay and keep their places | Q-U3, Q-S2, Q-H1, Q-R1 |
| a resync carries the server's selected entries | Q-U5, Q-S4, Q-H3 |
| ended lives are present until their end | L-U1..11, L-S0..2, L-H1/3/4, L-R1/2 |
| no mirror entity minted by a rewind | L-U3, L-H1 |
| the declared behaviour changes are no worse | L-H2, L-H6, L-S3 |
| the beam is one life | B-U1, B-R1, L-U11 |
| parked balls are not failures | B-U2, B-R1 |

---

## 8. Steps (each: implement → two reviews → gate on its checks)

The user's order places F5-F7 after R3-R5 (audit §5). Within this track, G1
(B) goes first: it is core-only, small and independent. If TRD's S8 picks
9.2 (b), G2-G4 move before TRD's T3, and the record numbering of §5 follows.

| step | content | closing check |
|---|---|---|
| **G0** diagnostics, no code change in the session | plan trace (`trace_plan_`) on the r2_a forced arms to attribute every `birth_recreate_failures` (f13's 11th, f131's count); the 1378 "Current Distance" difference traced (entity 52384's matrix in the live and replayed tick, `--describe-script`); Q-S3 inventory with targets (the fan 46475); the WakeUp fact of 2.2.4 (the own ball's `simulated` at its birth record, lp19_a6_drop) | every counted failure named; the 1378 cause stated as symptom + cause; the outside-producer list and the fan's target written into §2.2.4 / §2.3.4; the WakeUp fact measured |
| **G1** B1 | CL:703-704; `keeps_parked`, `births_parked_balls` (RB, SHW, report line) | B-U1, B-U2, B-R1 |
| **G2** #20 + API v21 | PRT factories, detach/insert/free; PS/API; physics_direct, physics_view; `docs/engine-changes.md` #20 and the v21 bridge note | builds of S, H, RC, SimTool; Q-H0 bit-identical; SimTool `--savestate-check` on the existing scenes unchanged |
| **G3** Q3 | record section, hooks in RBS, scope per restore, scoped check | Q-U1, Q-U2, Q-U4, Q-S1, Q-H2, Q-R2 |
| **G4** Q4 | hold/unhold hooks, the RB guard, `--hold-outside` | Q-U3, Q-S2, Q-H1, Q-R1 |
| **G5** Q5 (after RSD A1-A3) | portable section, PW capture, `localize`, protocol 11 | Q-U5, Q-S4, Q-H3; a v-old journal still replays |
| **G6** L1 | RB detection, `restore_lives`, two-half plan item and its readers, `generation_at`, refused-blob change, step P's revive moved unchanged into the host half, per-name skip logging | L-U1, L-U2, L-S0 (blob and `--legacy-restore`), L-H0 |
| **G7** L2 | `life_log_` fields and trim, reconcile, `pending_deaths_`, `pending_birth::entity`, `drain_pending_lives`, generations, `recorded_input_at`, `make_remote_birth` with its fallback; SC exact-name `make_mirror` | L-U3..5, L-U8..10, L-H1, L-H2, L-H6, L-R2 |
| **G8** L3 | `own_death` with the matrix, `entity_matrix`, replay-tick life test, own revive/remake, own events inside the window, `caps.own_body_in_replay`, shared `make_own_body`/`unmake_own_body`, RC ops, replica and matrix restore in the guard | L-U6, L-U7, L-U9 (own half), L-U11, L-S3, L-H3, L-H4, L-R1 |
| **G9** L4 | `revivable` generalized, same-name destroy-first, revive on the per-body path | L-S1, L-S2, L-H5 (after TRD T3) |
| **G10** records | session-client-core design "As built (G1-G9)"; collision-overhaul-design 9.x entry; audit #6/#7/#8 closed with their numbers | review only |

---

## 9. Open questions

**9.1 L on the retail legacy path (RL). Needs the user.**

- *Symptom.* RL runs the script replay and the blob (audit §0), so gap #7
  exists there: a mirror or own life ending inside a window is missing from
  the replay of [A, D).
- *Cause.* RL's lifecycle is PSC's own code, not the core's, so L2/L3 do not
  reach it. Q (G3, G4) does reach it through RBS.
- *Options.* (a) port the reconcile and the own death to PSC (a second
  implementation of a session decision, against §0 of the session-client-core
  design); (b) retire RL at R7, as planned, and record #7 as open on RL until
  then; (c) refuse, on RL, every rewind whose window crosses a recorded death
  (counted; snapshots repair it). The user's decision "legacy stays untracked"
  covers the pieces only (audit §5).
- *Settle:* the user chooses. (b) is my recommendation, because it keeps one
  implementation.
- **Superseded (2026-10-28, the user's answer 7, verbatim: "直接拒绝，不要继续兼容
  非session core的客户端"):** the server refuses every client without the
  session core (step B9DX, item LGX), so RL never runs a session and #7 on
  RL is moot until R7 deletes it; Q-R2 (void since B7EG) is not run either.

**9.2 `own_body_in_replay` on retail.**

- The revived own body exists only inside a replay and is removed again at
  D. Three things are unproven on the retail CK2:
  - whether a bridge-made own body, while it exists, disturbs anything the
    game's Physicalize block or Gameplay_Ingame keeps (the body guard of
    engine change #6 exempts the player's ball; the sound listeners belong to
    the old core, which is gone);
  - whether `navigation_create` for the replica inside a replay matches H's;
  - whether `host.own_replica` and the entity matrix restore cleanly.
- *Settle:* L-R1 plus a retail log line per revive (body made/unmade, the
  replica's create/destroy, the guard's view).
- The rejected alternative refuses windows that cross an own death on RC
  (snapshot or resync fallback). That only moves the gap.

**9.3 Outside controllers on the own ball in a replay. Needs the user if L-S3
shows only the controller-list difference.**

- *Symptom.* Two zero-force controller-list differences on the own ball in a
  replay (§3): a revived life lacks the outside controller it had at A; a
  live ball carries from A an outside controller made after A.
- *Cause.* Outside blocks are live state; their controllers are not in any
  script record, and the blob can only reorder live controllers.
- L-S3 bounds the effect to the controller list. The question is whether to
  close it.
- Option (a): the bridge tracks outside controllers. For the first case it
  makes a stand-in `rt:force/<id>` controller on the revived core from the
  blob's controller record, freed at D; that needs the blob to record the
  controller's point and force (a blob version bump). For the second it
  detaches each outside controller created after A for the replay and
  re-attaches it after; that needs the creation tick (a bridge controller
  log) and a detach/attach of a controller (an access-only addition to #20).
- Option (b): with L-S3's proof of bit-identical body states, the identity
  tools learn to name this one difference instead of failing on it.
- *Settle:* L-S3's result. If any body state differs, (a) is required
  without asking.

**9.4 RC births at their stamps before R3.**

- 2.3.3 applies remote events with a stamp inside the window at the stamp in
  every replay. With RC's `lifecycle_rewind = false` that is R3's half inside
  replays only.
- *Settle:* if R3 has landed when G7 starts (the user's order), nothing to
  decide. Otherwise the user chooses between this and gating 2.3.3 on
  `lifecycle_rewind`. Gating keeps the parking path for RC mirrors and the
  same-name conflict of §1.2 for RC beam pairs; L-U4/L-R2 then stay open
  until R3.

**9.5 TRD 9.2's ordering.**

- If TRD's S8 finds CollSound entries, G2-G4 are a prerequisite of TRD's T3.
- *Settle:* the outcome of S8. The record version is assigned at landing (§5).

**9.6 The protocol number of Q5.**

- G5 changes the content of RSD's resync payload. If RSD's A2-A3 have not
  shipped when G5 is ready, the section joins portable version 1 and protocol
  10; otherwise protocol 11 (or the next free number at landing).
- *Settle:* the landing order; no user decision.

**9.7 The own entity's CK matrix while it has no body (found at G0). Settle at G8.**

- *Symptom.* After an own death D (and through a trafo animation), a replay
  leaves the dead own entity, and `Ball_Pos_Frame` under it, where the live
  frames had put it by the rewind; live, scripts outside the replay moved it
  tick by tick (§2.3.4 "found at G0": the resetpoint at D+1, the trafo
  animation). Proximities read the frame: a check inside such a window reads
  another distance than the live one (§1.2, the 1378 line).
- *Cause.* The entity's CK matrix is no rollback state; with no body, no
  replayed step writes it. L3's `own_death.matrix` covers D, not D+1..now.
- *Options.* (a) Record the own entity's world matrix in every tick's record
  while the own ball has no body, and write it before each replayed tick's
  scripts (the live tick's value; the guard then puts the pre-rewind one
  back): one matrix per tick, no protocol. (b) (a) for `Ball_Pos_Frame`'s
  parent chain as well, if a level re-parents the frame while the ball is
  dead. (c) Leave it: the difference is CK-only and reaches physics only
  through a proximity that fires inside the window (the fan's force, a
  module's activation).
- *Settle:* G8, with L-R1 extended by `BMMO_TRACE_ENTITY` (no replayed
  `Ball_Pos_Frame` differing from the live run in a window across an own
  death). (a) is my recommendation; it also covers the trafo animation of a
  live run.
- **Settled (step EL8):** (a) built (As built (step EL8)), with the
  record of t itself written before replayed tick t; confirmed on retail
  by L-R1 with `BMMO_TRACE_ENTITY` (`el8_lr1g`).

---

## 10. Review (revision 2)

Every point of the two reviews was checked against the WT. Disposition:

| review | point | disposition |
|---|---|---|
| 1 #1 | one plan item per name | confirmed (RB:720). Two-half item with `revived_until` and `birth`, every reader listed (2.3.1 steps 4-5); L-U2 |
| 1 #2 | revive spec trimmed from `life_log_` | confirmed (`_466`: born 701, died 860). Trim keeps each player's newest old Physicalize (2.3.3); L-U10 |
| 1 #3, 2 #1 | H cannot reproduce mirror names | confirmed (SC:917-931, SC:2193-2210). `pending_birth::entity`, no `mirror_entity` in a replay, exact-name lookup on H (2.3.3); L-U3 models clone-on-lookup; L-H1 counts clones |
| 1 #4, 2 #6 | late birth and in-window lives never made | confirmed (CL:236-258). Reconcile step 3 queues every window event (2.3.3); L-U8, L-U9 |
| 1 #5 | `own_life_current` against the live generation | confirmed (CL:776-779, 875-883; r2_a_f13 lines 245/249). Replay-tick life test, replays never retire (2.3.4); L-U11, L-R1's 640 arm |
| 1 #6 | `make_remote_birth` loses its fallback | confirmed. Explicit live fallback (2.3.3); L-U8 |
| 1 #7, 2 #5 | `own_death` has no pose | confirmed (CP:469-470, SC:1630-1640). `own_death.matrix`, written at D; pre-rewind matrix put back by the guard (2.3.4); L-U6, L-H3, L-R1 |
| 1 #8 | undeclared behaviour change | confirmed (SHW:335-336). Declared (2.3.3, §3); L-H6 same-batch A/B |
| 1 #9, 2 #2 | wrong facts about RSD's portable record; the queue would go on the wire | confirmed (RSD 2.4 keys blocks by walk position; `full = 2` is wire). Q5: entries keyed (script key, walk position), the server reads its queue, protocol 11 (§2.2.5, §5) |
| 1 #10 | the hold inside `restore_scripts` | confirmed (RB:887-903). The hold is an engine hook gated on `replay_follows`, with an RAII unhold (2.2.4) |
| 1 #11, 2 #12 | `issue` op unused | confirmed. Removed; Q4 no longer issues anything |
| 1 #12, 2 #11 | observer placed before `m_ProcessingQueuedCallbacks` | moot: revision 2 has no observer and no data member (2.2.1) |
| 1 #13 | U_A wrong when a block is deleted | moot: revision 2 has no U_A arithmetic (outside entries are held, not reconstructed) |
| 1 #14, 2 #7 (first half) | "selected entries target mechanism bodies" unverified | confirmed wrong: the fan 46475 is created on the ActiveBall test (audit #13), so it targets the own ball. §3 rewritten; G0/Q-S3 records every target |
| 2 #7 (second half) | outside entries released appended, order lost | confirmed, and the draft was worse than stated: re-running an outside entry that ran live made a second controller next to the live one, which survives the blob (SSR:1624-1632). Revision 2 holds outside entries for the whole replay and puts them back after their predecessor (2.2.4); Q-U3, Q-H1 assert the order |
| 1 #15 | applied tick vs stamp | confirmed. The log stores `stamp` and `made_at` (2.3.3) |
| 2 #3 | entries of a set made inside the window never cleared | confirmed (TRD 4.5). Scope = every written script, empty target for absent scripts (2.2.3); Q-U1 |
| 2 #4 | moved birth taken for an ended life | confirmed (RB:1567-1600). An end is a change of generation only (2.3.1 step 1); L-U1 |
| 2 #8 | G5 cannot pass its own check and regresses step P | confirmed. Step P's revive moves unchanged at G6 with the single-explosion L-S0; L-S1/L-S2 at G9 (§8) |
| 2 #9 | L-H2 contradicts the change it checks | confirmed. L-H2 is judged against the server, no worse than the control |
| 2 #10 | RL left on today's refusals | confirmed for Q: RBS gives RL Q3 and Q4 with no host code (2.2.3, 2.2.4); Q-R2. L on RL is §9.1, for the user, with symptom, cause and options |
| 2 #13 | counter split cannot be made from a bool; B-R1 pass | confirmed (RB:931). `keeps_parked` predicate (2.4); B-R1 passes on `birth_recreate_failures = 0`; B-U2 |

No point was refuted. Line references the reviews confirmed are kept; the
ones found wrong while checking (RB:720 for the plan; the proximity block 46429 measures 52384 against 46336 in the audit dump; the r2_a_f13 log lines) are
fixed.

## 11. Decisions (user, 2026-10-24)

- **9.1.** Option (b): the retail legacy path is retired at R7 as planned;
  gap #7 stays open on it until then and is recorded.
- Step P is committed with its two pre-existing residuals recorded as their
  own items: the snapshot row not re-applied inside a later replay (a new
  gap, fixed as its own step), and the spawn frame-timing race (measured
  again after R3).

## As built (step EG0)

Step G0 of §8, 2026-10-25, on HEAD 5cd1c11. Diagnostics only: no session
code changed; every switch below is off by default and changes no simulation
result. Its facts are in §1.2 (the 1378 cause), §1.3 (the counted failures),
§2.2.4 (the outside producers, the WakeUp fact), §2.3.4 (the fan's target, the
dead own entity) and §9.7.

**Tools.**

- SimTool (`BallanceMMOServer/sim/sim_tool.cpp`, a file TRD's T3 also
  changes; the additions are their own region `namespace eg0` before
  `run_free`, four fields at the end of `arguments`, four parse lines before
  the `parse_trafo_arg` fallback, two calls in `run_free` beside
  `--list-scripts`, two lines in `run_replay` and the `PhysicsCallback.h`
  include):
  - `--list-callback-blocks` (a level run, at `--dump-at` or the end): every
    block of the ten queueing prototypes (§1.1) with its root script and
    owner, selected by `session_script_selection` or OUTSIDE, and its target
    parameter's source;
  - `--queue-trace` (a `--replay` run): a line whenever the PreSimulate queue
    or the current ball's body changes - every entry (`slot:block<prototype>
    @script`, or the C++ class of a bridge entry) and the ball's `sim` flag;
  - `--ball-off FROM TO` (a `--replay` run; §7.2 Q-S2's switch, two
    arguments instead of `<from>:<to>`): the current ball's body out before
    frame FROM, made again before frame TO with the retail recipe
    (`physics_world::retail_recipe`'s fields from the Physicalize_GameBall
    row) at its entity's matrix;
  - `--ball-off-wake-only`: the non-WakeUp entries of the ten prototypes
    dropped before the re-make; `--ball-off-drop-all` (added at the review):
    every entry dropped there, the WakeUp fact's control arm with the same
    inputs. Both drop through the container's own `ClearBehavior`, which
    keeps `m_HasCallbacks` right (measurement switches: they change that
    run). The `--ball-off` recipe is a copy of `physics_world::retail_recipe`
    (a `--replay` run has no `physics_world`); both say so in a comment.
  - The usage text lists the five switches.
- Retail core host (`BallanceMMOClient/session/core_host.cpp`), environment
  switches read at the session start (file-scope helpers, no
  `BallanceMMOClient` member): `BMMO_TRACE_PLAN=1` (the engine's plan trace
  of its first 16 restores, a "plan trace: refused destroy of <name>@<tick>"
  line for every refused destroy - the binding's `destroy_body` wrapped after
  `bind_session_world`; the runs below logged it as "birth item ... not
  destroyed" - and the rollback log uncapped),
  `BMMO_SCRIPT_DIFF_LOGS=N` (the core's `script_diff_logs`, default 8),
  `BMMO_TRACE_ENTITY=a,b,...` (each entity's world position and parent, hex
  floats, after every live `post_step` and before every replayed tick's
  scripts, the host's `before_replayed_tick`; solo runs only: the entity is
  the first object of that name, which with peers can be a clone's child).
- Scripts in `.local-tools/work/EG0`: `birth_items.py` (§1.3's model over a
  client.log), `entity_trace_cmp.py` (replayed vs live entity lines per
  rewind), `eg0_retail.py` (the solo retail arm below; `EG0_NO_DIAG=1` runs
  it with every switch off, `EVERY` 0 the control without forced
  rollbacks), `bmrc_flatten.py`, `bmrc_hash_cmp.py`, `ball_at_beam.py`
  (every ball's journal row in a server and a client journal over a tick
  range) and `bodies_near.py` (the bodies near a point at one tick).

**Runs.**

- Retail, solo, arena, core on, snapshots off, the Arch server rebuilt from
  HEAD with the slot-13 hack (`ballanced-3e4289e95f25+bmmo-5cd1c11b5243-dirty`):
  `maps/arena-test/eg0_f13` (forced 13:24, all switches on) and
  `eg0_f13_nodiag` (the same, all off). `r2_keys.bmrc` and `r2_run.py` no
  longer exist: the arm runs `work/a4/r0_keys.bmrc` with the r2 beams (650,
  900, 1300). The events land on r2's ticks (spawn 212, stone dead 652, wood
  817, death 1369, respawn 1567). Both runs: `rb=207 sv=207/0/0`,
  `birth_recreate_failures` 11. Their journals (`e0_compare.py --live`):
  TICK hash and pose equal at 2702 of 2703 common ticks. 1302 differs
  because the server's full snapshot of the beam stamp 1300, which corrects
  the wood ball by 8.6 mm in both runs (below), arrived at local tick 1301 in
  the traced run (corrected before the LOCAL checkpoint 1302) and at 1302 in
  the other (after it): network timing, not the switches. The births
  counters differ (51 vs 45 pieces re-created) because the traced run's last
  status line comes after sector 3's activation at 2712.
- The control `maps/arena-test/eg0_ctl` (the same arm without forced
  rollbacks, `eg0_retail.py eg0_ctl 0`, switches on; added at the review):
  `rb=1`, the beam stamp's correction alone, with the same numbers.
- SimTool, arena: `--list-callback-blocks` over L1-L12, the original L13
  (scratchpad `root_l13orig`) and the arena; `--queue-trace` with
  `--beam 60 -30 15 300` (death), `--beam 0 4 43 300` (trafos) and
  `--ball-off 600 700` (with `--ball-off-wake-only`, with
  `--ball-off-drop-all`, with every entry pending, and over the flattened
  record; all four identical in every frame).

**The beam stamp 1300 correction (a pre-existing residual, root-caused).**

- **Symptom.** In every run of the arm, the control without forced
  rollbacks included, the server's full snapshot of the beam stamp (sent on
  the body-set change with snapshots off, core design As built R3) trips one
  rollback: "mismatch at tick 1300: Ball_Wood dp=0.0086 dv=0.879",
  local (300.008, -40.022, 15) against the server's (300.010, -40.014, 15).
  `first_divergence.py` at 1 mm: the client's ball equals the server's at
  every checkpoint up to 1298 (position, rotation and both velocities as the
  journal prints them) and first differs at 1300, the stamp. At 1300 the
  server's ball has the identity rotation, angular velocity 0 and linear
  (0.4495, -0.6019, 0); the client's is turned, with angular (0, 0, -0.0807)
  and linear (0.3858, -1.4789, 0).
- **Cause.** A beam is two different operations (core design §2.3,
  "Beam"). The server, and every mirror, makes a fresh body at the beam pose
  (Unphysicalize + Physicalize, `client_core_lifecycle.cpp:845-858`). The
  retail owner, without `own_body_control`, writes its existing body in
  place: `write_body` → `physics_view::set_body_state` → the legacy
  `physics_state.cpp:1520` entry, `beam_object_to_new_position` on the
  optimized path (no recheck, the PSI anchor not restored). The written
  body keeps the IVP collision state it had rolling on the lane at x = 385.7
  (15.24 m/s, -7.62 rad/s) into its first tick 42 m below that lane; a fresh
  body has none. The pose and velocity written are not the difference: the
  server's fresh ball at the same pose and velocity gets no angular velocity
  in free fall, the client's gets -0.0807 rad/s; the lane beam 900 of the
  same run, a ball rolling at 12.83 m/s written the same way 2 m above the
  same lane, is exact; and once the correction has written the server's
  1300 row into the same body (the restore's write, three ticks into free
  fall), the client follows the server exactly (the replayed 1301-1303 and
  the live 1304 equal the server's). Which item of the carried state
  (contacts, distances, collision timers) makes the 0.879 m/s is not
  separated: the fix below removes all of it.
- **Pre-existing.** The same disagreement at the same stamp is in every r2
  arm on disk (`r2_a_f13`, `pr2_a_f13`, `r2f_a_f13`, `pi_a_ctl`; builds of
  2026-10-23, before this chain): the client (300.0000, -40.0040) with v_y
  -0.2833 against the server's (300.0000, -40.0137) with -0.6019, 9.7 mm and
  0.319 m/s; R3 measured the death beam's owner residual at 0.009914 m.
  That is under the ball's velocity tolerance of 0.5 m/s
  (`rollback.hpp:702-720`), so nothing was corrected or logged. With
  `r0_keys.bmrc` the ball reaches the stamp faster (15.24 m/s and
  -7.62 rad/s against r2's 6.86 and -3.43), with a key held (the server's
  fresh ball gains +0.4495 m/s in x in its first tick; r2's gains 0), and
  the carried state gives 0.879 m/s, over the tolerance. The beam path
  (`client_core_lifecycle.cpp`, `client_core_frame.cpp`, `physics_view.cpp`,
  the retail `write_body`, `physics_state.cpp`'s write) is unchanged since
  b64fe49, so neither S2, S9 nor TR2 is in it; the server's result at the
  stamp is the r2 arms' in y. Not run: the pre-chain mod with
  `r0_keys.bmrc` (that mod was not kept).
- **Where it is queued.** R6 part three (own body control: the beam made
  as `unmake_own` + `make_own`, a fresh body), roadmap §4 item 13, where
  this measurement is now recorded. A narrower option, untested: write the
  beam stamp with a recheck (`set_body_state_ex(..., recheck)`), so IVP
  re-evaluates the moved body's collision state. G1's B-R1 compares the own
  ball against the control from the correction on (§7.4).

**Adaptations.** 52384 (§1.2's draft) is the server world's probe frame, not
a client entity (§1.2). Q-S3 uses the plain level engine (`run_free`), not
the session world `--describe-script` loads, whose ball-identity reads are
rewired. The design's "outside producers are exactly Gameplay_Ingame's" holds
for what was observed queued, not for the static inventory (§2.2.4).

**Review fix-up.** The beam stamp 1300 correction root-caused above (the
first draft called it a traced-run-only network effect); the control
`eg0_ctl` run; the WakeUp fact's same-input control arm
(`--ball-off-drop-all`); §7.4 points at `eg0_retail.py`; §1.3 marks f50's
and f131's items as the model's; §2.2.4 adds 4547's effect; the queue drops
go through `ClearBehavior`; the SimTool usage text; the refused-destroy log
line reworded; the `--ball-off` recipe and `physics_world::retail_recipe`
cross-referenced (one comment above `retail_recipe` in
`BallanceMMOServer/sim/physics_world.cpp`, a file T3 also changes, away
from its hunks). Files of T3's set touched by this step: `sim_tool.cpp`
(its own region and call sites, as above) and that one comment.

## As built (step B7EG, 2026-10-28): gap Q (G2-G4); Q5 and gap L not built

- **G2, engine change #20 and bridge API v23** (`docs/engine-changes.md` #20
  and its bridge note). The API number is v23, not the nominal v21 (v20-v22
  are A3's and T1/T4's). `physics_savestate_common.cpp` holds the queue
  functions and the manager-keyed held list and scope;
  `physics_savestate_restore.cpp` `check_callbacks` filters both sides by the
  scope. `bmmo_physics_callback` is `physics_rt_api.h`'s.
- **G3, the record.** `script_replay` record **v4** (= v2 + the queue
  section) and **v5** (= v3 + it), not one "v-Q" bumped over v3: the section
  is written only when the binding passes the new `queue_hooks`
  (`script_binding_options::callback_queue`, set by both core hosts, and only
  with `replay_activation`), so the legacy retail path (no `callback_queue`)
  keeps its v2 records byte for byte and gets nothing new (the chain's BAR:
  the legacy path is not changed). **Adapted from §2.2.3/§2.2.6:** RL does
  not get Q3/Q4 (§2.2.6's "RL: the same (PSC binds RBS)" and Q-R2 are
  therefore void); the headless client with `--park-births` (the v14 rule,
  `replay_activation` false) does not either. The restore rebuilds the
  entries of every root script it wrote, of every selected script that
  exists now but has no state in the record (`present = 0`: no recorded
  entry, so its live entries are removed, as §2.2.3 says; fixed at the
  step's review) and of every trafo script (written
  or put to its built state); the capture keeps the entries whose block's
  owner script is selected or a trafo script. A recorded block that is gone
  fails the restore; so does a refused rebuild (`script_restore_failures`,
  as §2.2.3 says). `diff` compares the section (a logical difference,
  "PreSimulate queue a vs b"). `localize` still writes v2 / v3 (no section):
  see Q5 below.
- **G4, the hold.** `rollback_world::hold_outside_callbacks(anchor,
  have_record)` / `unhold_outside_callbacks()`, filled by
  `bind_script_replay`; `restore_and_replay` calls the hold right after
  `replay_follows` is known and an RAII guard unholds on every exit
  (`rollback.hpp`, `struct outside_callbacks`). **Adapted:** the hold also
  sets the check's scope to the executed scripts (every selected and every
  trafo script) and the unhold sets it back to mode 0, so the binding needs
  no separate scope call per restore; a restore that never rebuilds (no
  record at the anchor, a failed script restore) is then still compared on
  the executed scripts' entries - fail-closed, the refusal of today - instead
  of an empty scope (§2.2.3's `cbq no_record` is still counted).
  `no_record` counts an anchor without a script record (`have_scripts`); a
  script restore that fails is counted by `script_restore_failures` and
  `rebuild_fail`, not by it.
- **The put-back names the predecessor by block and occurrence** (adapted
  from §2.2.4's "after the first entry of block b"): a held entry goes back
  after the k-th entry of the block it followed, k counted over the entries
  left in the queue at the hold. "The first entry" put an entry that
  followed two entries of one block between them ([s1, s1, o1] came back as
  [s1, o1, s1]: Process() order changed although the selected entries ended
  equal); the unit test found it. The decisions are pure functions over
  block ids in `physics/callback_queue_plan.hpp` (`plan_hold`,
  `unhold_position`, `compared` - the scoped check's filter), called by
  `callbacks_hold` / `callbacks_unhold` and `check_callbacks`. A clamp (the
  predecessor missing, or found before an older held entry's place) keeps the
  held entries' order and is counted.
- **Diagnostic noise, not a refusal:** a blob captured inside a replay lacks
  the held entries, so the live-vs-replay savestate diff at `current_tick`
  (a log line, at most 8, `rollback.hpp` "savestate replay") reports a
  queue difference whenever outside entries are held. A restore to such a
  tick is not refused: its scoped check never compares an outside entry.
- **Counters** (`script_replay::queue_stats()`): the headless client's
  report prints `cbq: captured rebuilt removed held holds unhold_clamped
  rebuild_fail hold_fail no_record`; the retail core host's status line ends
  with `cbq=rebuilt/removed/held/unhold_clamped/rebuild_fail/hold_fail/no_record`.
  `scope_refused` is not separate (a refused scoped check is the blob's
  refusal, logged as before).
- **Checks run** (quick tier): x64 and x86 builds, the unit tests (556, all pass;
  the new `RollbackEngine.OutsideCallbacksAreHeldForTheWholeReplayAndPutBackOnEveryExit`
  covers Q-U3's guard half: the hold before the scripts' restore and the
  blob, the unhold after the last replayed step and after a failed step,
  none for a frozen clock or a too-far window), the GCC syntax check, the
  wire check rRCX -> rB7EG over core_ref_s8 (ALL SAME: 12/12 sends, 12/12
  journals IDENTICAL; the solo1 / solo2 replays SAME and IDENTICAL; every
  replay's `cbq:` line shows the hold ran on every rewind, e.g. holds=42-65
  per swing_snapoff client, with no entry captured or held - post-N4
  sessions queue none, design 1.1).
- **Review round checks.** Unit tests 560 (all pass): the
  `CallbackQueuePlan` suite covers Q-U3's placement half (the hold takes
  exactly the outside entries; the put-back restores the pre-hold order for
  [s1, o1, s2, o2], a held head, two held in a row, [s1, s1, o1],
  [s1, o1, s1, o2] and a longer mix; a missing or reordered predecessor is
  clamped and counted) and Q-U4's filter. **Real entries** (a first slice of
  Q-S2): SimTool `--replay work/a4/r0_keys.bmrc --ball-off 1600 1900` with
  the new `--queue-roundtrip` - after each of the 280 frames whose queue
  holds Gameplay_Ingame's entries (3, then 6: WakeUp 26418 four times,
  SetPhysicsForce 26414 and 26432) the bridge holds every entry (the queue
  then holds none; a second hold is refused), puts them back (0 clamped,
  the queue equal in order), rebuilds them from the queue as it was (6
  removed, 6 re-made by the #20 factories, the queue equal) and refuses an
  out-of-scope rebuild without a change: 0 failures, and the run's
  `--write-record` equals the same run's without the switch in all 2400
  frames (`EG0/bmrc_hash_cmp.py`: 0 differing; the same tool tells the
  `--ball-off 600 700` arm apart from frame 600 on). The re-made entries ran
  when the body came back (the queue empty after 1900). The window moved
  from G0's 600-700: the arena's ball is Ball_Wood by 600 now and queues
  nothing there. Wire check rRCX -> rB7EG over core_ref_s8 again ALL SAME,
  solo1 / solo2 c0 SAME and IDENTICAL; GCC 29/29 ok; x64 and x86 builds.
- **Retail: the crash is the machine's.** The control - HEAD 781f981's x86
  build, the same two copies, the same driver (`CD1/cd1_gate.py`) - crashed
  identically (`d3d8.dll+0x54acd`, a write to 0x11a80, both copies, during
  `TT Change ScreenMode`). [Corrected at step RTX: this note first blamed
  the monitor being off line (availability 8); the display was not
  involved. The cause was a bad state of the user's RTSS (RivaTuner
  Statistics Server) overlay, which hooks d3d8: every launch from 03:05 to
  03:28 on 2026-10-28 crashed there, the mod's builds and the game without
  the mod alike; the user restarted RTSS at 03:29 and the same build reached
  the start menu.] The step's retail session and Q-R1 were deferred; step
  RTX ran them (below).
- **Not run, deferred:** Q-U1/Q-U2 as unit tests (the record needs CK; no
  fake queue backend exists), Q-S1 and the rest of Q-S2 (SimTool's
  `--queue-block` and `--hold-outside` scenes with a savestate restore),
  Q-H1 (the A6 drop recipe), Q-H2 (the `drop-body` verb), Q-R1.
- **Q5 not built, so Q is partial in a resync.** Q5 changes the resync
  payload (the portable record's queue section) and needs a protocol number;
  this step names none (the chain's rule). The protocol is 13 now
  (`PHYSICS_PROTOCOL_VERSION`), so Q5 takes 14 at the earliest; it is queued
  in roadmap §4 item 4. Until then a resync goes through
  `restore_and_replay` like any restore: its outside entries are held for
  the re-step and put back (Q4 applies), while the selected scripts come
  from the localized record without a queue section, so their live entries
  stay and run in the re-step (the behaviour before this step), and the
  travelling blob does not compare the queue.
- **Gap L (G6-G9) not built** in this step; queued as the four steps §8
  names (G6, G7, G8, G9), each with its own checks.

## As built (step RTX, 2026-10-28): the deferred retail runs

The retail runs B7EG, B2SX, B5RX and B6RC deferred (B7EG's because of the
RTSS crash above), in three two-copy sessions of the x86 build of HEAD
3eab89f plus this step's status fields: level 11, session core on (the
default), the Arch server of B9DX's tree (`ballanced-36c04e3d24c9+bmmo-
415f00be562d-dirty`, no server file changed since) with snapshots off
(100000 / 100000), `trafo_inputs` and `placement_clones` on. Driver
`.local-tools/work/RTX/rtx_retail.py`; outputs `scripts/retail/rtx_1..3`.
Every game started (RTSS healthy).
- **The retail status line** (`core_host.cpp` `core_host_status_text`, the
  'session' verb) now ends with `cbqx=captured/holds` (the two queue
  counters the headless `cbq:` line has and `cbq=` left out) and
  `dorm=from_ledger/healed/walked/guard_trips/restored_built` (RC4's
  counters, the headless `trafo_dormancy:` line); appended, so every field
  before them keeps its place. Diagnostics only; no behaviour changed.
- **The sessions.** rtx_1: client 1 `session force-rollback 13:24`, beamed
  onto sector 1's `P_Trafo_Stone_MF001` at A = 285 (its Wood explosion
  stamped and applied at 443 in both worlds), the up key held 305-435;
  client 2 onto the same trafo at 585 (explosion 743, both worlds); client 1
  onto the sector-2 checkpoint (`PC_TwoFlames_MF`, +4 m) at 785, client 2
  at 985. rtx_2 / rtx_3: client 1 forced 13:24, the up key held from A + 30
  to A + 400 (317-687) through an own death - rtx_2 rolled off the level
  (the trafo reset of the cancelled activation at 721), rtx_3 beamed into
  `Quader01` (no L11 entity is named Depth*; the ball died there: the death's trafo reset stamped 301,
  unphysicalized 357, respawned 555) - and client 2's trafo at 387 (its
  explosion 545, both worlds).
- **Q-R1 / the #20 paths in Player.exe: partial** (client 1 / client 2):
  only the empty-queue hold ran. The hold ran on every rewind and never
  failed - `cbqx` holds 114 / 10, 94 / 14,
  103 / 17 against `rb` 114 / 10, 94 / 14, 103 / 17; `cbq=0/0/0/0/0/0/0`
  everywhere (nothing rebuilt, removed or held, no clamp, no rebuild or
  hold failure, no rewind without a record); `sv=N/0/0` on all six (no savestate refusal);
  `scripts_refused=0 blobs_refused=0`; no crash. **But `held` stayed 0**
  (and `captured` 0): no entry, outside or selected, was in the queue at any
  hold, even with keys held through two own deaths. The cause: the queue
  fills only when a steering block runs on a ball without a body
  (`Gameplay_Ingame`'s WakeUp / SetPhysicsForce, §1.1, the A6 drop arm:
  the own Physicalize dropped while the ball is alive). A retail death
  deactivates BallNav first and unphysicalizes a second later (rtx_3: the
  beam into the cube at 287, unphysicalized 357), so no steering block runs
  while the ball has no body. Q-R1's held > 0 and its queue-order compare
  need its own recipe, the own Physicalize held while the ball is alive -
  the retail `hold-event` verb, which does not exist (roadmap item 9, R6):
  they stay on the close-out's deferred list. So the part of #20 that moves
  entries - `callbacks_hold` returning > 0, then `callbacks_unhold` putting
  them back - never executed in Player.exe; Q-R1 is not done (and the
  `cbq=` field, which the row named, showed no hold: the holds show only in
  the new `cbqx`).
- **Found (item 4, queued): the sector-deactivation cell after a rewind
  over the own crossing.** Symptom: rtx_1 client 1, the forced rewind at
  819 (795 -> 819), whose last tick is the own crossing's poll tick (the
  Sector stamped 828 = 819 + lead 9): "the scripts re-executed to tick 819
  differ from the live record: cell IngameParameter[0][2] 0 vs 1" (live 0,
  replayed 1). Cause: `IngameParameter[0][2]` is one of the record's three
  cells (`script_replay.hpp` `cell_of`), written back at the anchor by a
  restore with `replay_activation`; its live writer on a crossing is
  Gameplay_Events / activate Sektor's pinned Set Cell (N2,
  `sector_deactivate_pin.hpp`: a constant 0), a chain no replay runs. The
  cell is 1 before the crossing and the pinned write turns it 0 at the
  crossing tick (SimTool `--level 13 --drop P_Ball_Wood_MF 0 --drop-at 220
  7.65 15 --drop-sector 1 --ticks 120 --cells-trace`, rB9DX: `tick=-1
  [0][2]=1`, `tick=80 [0][2]=0`), so a rewind whose window holds the
  crossing restores 1, and the world holds 1 until the next union pass:
  here from 819 to the pass of sector +2 at 828, which writes
  `[0][2] = 0` in every world (`sector_activation.hpp` `run_sector_pass`).
  The divergence is that short and not a three-world split: on a client
  the chain runs only for the own crossing (`sector_cell_split.hpp`; on
  the server also for a clone's), and client 2 had none before 985, while
  the anchor record at 795 held 1 (no pass had written 0 before), so
  client 2's world also held 1 over 819-828 (inferred from the same
  history; not logged) and only the server's clone crossing had written
  0. The rewind made client 1 agree with client 2; the cell was per-player
  state before this step either way. Effect today: none seen - the cell's
  only reader is the sector manager's Start, and every manager run in a
  session follows `run_sector_pass`'s `[0][2] = 0`. Pre-existing (the cell record is bridge v14's, the pin
  9.25 / B2SW's; RTX changed only status text). Options: (a) leave
  `[0][2]` out of what a restore writes (the passes set it themselves;
  the record keeps it for the compare); (b) record the crossing's write
  and replay it in the window; (c) leave it (no reader). **Answered
  (2026-10-28): the user's (b), verbatim "(b) Record and replay"; built at
  step N02R** (resync design, As built (step N02R): every live-frame write
  of [0][2] - the crossing's and the death chain's - recorded and written
  back after its replayed step; step N01R does the same for the death
  chain's [0][1], the user's (b) again).
- **Not run:** Q-R2 (void since B7EG); the B2SX record-format note's retail
  follow check of the re-armed delayed link (no recipe reaches it in a
  session); a frozen clock on retail (no knob stops it; the unit test
  covers the request).

## As built (step B2SW, 2026-10-27): gap F fixed; the CPU cliff's closing check

- **F.** `script_replay::execute_frame` / `execute_scripts`, under
  `live_membership()` (the switch the core hosts' bindings set, step B2SW's
  G1 shares it; the legacy retail path keeps the old rule), process a selected
  script's next-frame flags only where the live frame did: its owner among the
  scripts the host's recorded live order lists for the replayed tick
  (`live_script_order::at(tick)`, step RO1: the behaviour manager's
  `m_BeObjects` as that live frame walked it), or put into the list for the
  next frame by the replay itself (a replayed activation, a restore's
  re-registration: the pending table, walked); without a recorded order, the
  list as it is now. The trafo frame's list is not judged (the trafo frame
  runs it directly, live too). Adapted from the first build, which judged by
  the list as it is now: in the session-start window (record-format Q5's
  scene) that kept a DEACTIVATENEXTFRAME the live frame of tick 4 had
  consumed (`P_Modul_18_MF Script` flags 0x80000000 vs 0x80400000) - the
  owner had left the list since; the recorded order is that frame's.
  Checks: SP7's V9 (`.local-tools/work/SP7/v9r.sh`'s four arms, through
  `B2SW/v9.sh`): rR12 50 FAIL lines, every one P_Modul_18_Particle Script's
  flags 4194304 vs 0; rB2SWb 0 in every arm. `--late-sector-check` on the arena
  (K 2 / 3 / 4, A - T 3 / 9 / 40, `--late-reference`): PASS, exit 0 (B1's
  PASS-EXCEPT-REFERENCE, exit 4, before); script differences of the arm the
  reference does not have 0.
- **The CPU cliff's closing check.** B2's recipe (core_ref_rc1/cliff: 60 s,
  `--latency-ms 150 --arm A5u --sectors 4 --keep-lane --event
  2:1500:step=300:dir=up`, snapshots off, flags off) plus `--drive`, which
  the harness needs since the drive stopped being implied (without it no
  ball left sector 4 and no respawn happened: `b2sw_cliff`, `b2sw_cliff_off`):
  `maps/arena-test/b2sw_cliff_drive`, build rB2SWb, the placement off - the
  respawns at 2502, 2598, 2606, 2612 (three within 14 ticks) and 3436, 3531;
  rebases 0 and resyncs 0 on all four clients, t_max 29-30 ms (RC1's capture
  61-71 ms, B2's build rebased), the server journal 3542/3542, verdict PASS.
  Closed.

## As built (step B8EG, 2026-10-28): gap B (G1) and gap P

Two items of roadmap §4 item 4, built on HEAD 622d3f2; the quick-tier gate's
numbers below.

**Item EGB: gap B, step G1 (§2.4), as designed.**

- `beam_own_ball` (`client_core_lifecycle.cpp`): the navigated beam without
  own body control no longer bumps the generation at the request (the old
  CL:703-704 branch is gone); only the in-place move without the
  navigation replica, which no replay redoes, still bumps. Invariant B
  holds on both capability settings.
- Counter split (review 2 #13): `rollback_world::keeps_parked` and
  `rollback_stats::births_parked_balls` (`rollback.hpp`); the destroy loop
  asks it first, a ball's birth item is not passed to `destroy_body` and is
  counted apart, the rest (a refused mechanism destroy, a failed moved own
  birth or mirror remake) still counts `birth_recreate_failures`. The status
  line carries it as `parked=` after `births=`.
- **Adaptation:** the design's `keeps_parked` takes the entity only. It
  takes the birth tick too, because the core's moved own birth
  (`destroy_own_birth`, with own body control) is a ball that
  `destroy_body` does take out: asked first, a ball-only test would have
  kept it parked. `bind_session_world` builds it, as the design places it,
  right next to `destroy_body` and from the same hooks, in `destroy_body`'s
  order (moved own birth -> not parked; mirror -> not parked; any other
  ball or the own body -> parked), so the world is the same as before. The
  first test needs `destroy_own_birth`'s answer without its side effect
  (`unmake_own_body`), so the core gains one pure hook,
  `core_world_hooks::own_birth_destroyable`, and both it and
  `destroy_moved_own_birth` go through one function,
  `session_client_core::destroyable_own_birth` (own body control and
  `moved_own_life`): the two cannot drift apart. A host that gives
  `destroy_own_birth` without that query gets no `keeps_parked` (every
  refusal counts as a failure, as before); with `park_births` neither is
  installed. SimTool's binding (`rollback_binding_scripts.hpp`) has no
  balls and leaves it unset. (The review's should-fix: the first build had
  the core compose `keeps_parked` itself, a second copy of `destroy_body`'s
  order in another file.)
- Tests: B-U1 = `SessionClientCoreOwnLives.BeamWrittenInPlaceWithoutOwnBodyControl`
  (no generation change at the request, the beam's life keeps generation 0,
  the stamp written live and in a forced replay; the non-navigated move
  bumps); B-U2 = `RollbackEngine.AParkedBallBirthIsCountedApartFromARefusedDestroy`
  (the ball reaches `keeps_parked` and never `destroy_body`,
  `births_parked_balls` 1, the refused mechanism `birth_recreate_failures`
  1), the binding half `RollbackBinding.KeepsParkedIsDestroyBodysTestsWithoutItsSideEffects`
  (asking never calls `destroy_own_birth`; `destroy_body` refuses wherever
  it says parked) and the core half
  `SessionClientCoreOwnLives.OwnBirthDestroyableIsDestroyOwnBirthsTest`.
- **B-R1, one arm on retail** (`maps/arena-test/b8eg_f13`,
  `work/EG0/eg0_retail.py b8eg_f13 13:24`, arena, core on, snapshots off,
  Arch server of this tree with the slot-13 hack): `rb=205 sv=205/0/0`,
  `births=0/0/0+45/45/0/0` - `birth_recreate_failures` 0 (EG0's `eg0_f13`:
  11), no refused destroy in the plan trace (EG0: 11, five of them the beams
  650 / 900 x2 / 1300 x2); the script replay differs from the live record at
  one tick only, 2184 `IngameParameter[0][2]` 0 vs 1 (RTX's queued finding;
  EG0: 5, among them the beam windows 650 and 910); resyncs 0. The arm's
  binary predates the `parked=` field (added after the run), so the parked
  count was not read; §1.3's model (`work/EG0/birth_items.py` on this log)
  gives 11 items under the old rule, 5 of them beams, so 6 parked births
  (212, 817, 1567, two windows each) remain to be read off the status line.
- The control `maps/arena-test/b8eg_ctl` (`eg0_retail.py b8eg_ctl 0`, the
  final binary with `parked=`; `parked=0`, no forced rewinds) hit a 580 ms
  host stall at tick 456 (04:45:12.576 -> 13.158), its tick driver rebased
  and it resynced (`resyncs=1/1`), so `e0_compare.py --live` matches the arm
  only up to there: TICK hash and pose identical at every tick 8..495 (the
  forced windows over the spawn 212 included), different from 496 on. The
  beam windows' "every non-piece body bit-exact against the control" and
  the parked count are deferred to the stage close-out (two live runs per
  step).
- **TR3's R4 is not B's** (roadmap §4 item 4 named it under B): R4 is
  headless (H5 131:47), and the headless host has own body control
  (`session_client.cpp:1612`), which B1's removed branch never ran on. Its
  line "own beam stamped 651 retired ... generation 1 -> 2" is
  `apply_own_lives`' life test against the LIVE generation, bumped by the
  trafo's Unphysicalize at 653 inside the window - §2.3.4's replay-tick life
  test, step G8 (L3). The retail arm shows the same signature: "own beam
  stamped 650 retired at tick 650 (replay): Ball_Stone has had another life
  since (generation 1 -> 2)" (the stone dies at 652). R4 moves to G8.

**Item EGP: gap P, option (i) built** (roadmap §4 item 4; found at RO2,
`lockstep-without-snapshots.md` "As built (step RO2)").

- Both options measured on RO2's cases with a switchable measurement build
  (`work/B8EG/egp_measurement.patch`, not committed: `BMMO_EGP_RECORD_LOCAL=1`
  records a parented body's local matrix in the blob behind a flag bit,
  `BMMO_EGP_RESTORE=skip|local` picks the option; `work/B8EG/egp_runs.sh`:
  `--late-sector-check K 60 69 --late-keep --late-after 300`, every boundary
  described and compared with RO2's `bodycmp.py`, control vs late arm of the
  same cell, locals recorded in every cell):

  | cell | as built | (i) skip | (ii) SetLocalMatrix |
  |---|---|---|---|
  | L8 K=3 | 0/301 bit-identical: `P_Modul_25_Bridge001` 7.153e-07 m | 301/301 | 0/301: `P_Modul_30_Wippe` 7.451e-09 m |
  | L11 K=3 | 0/301: `P_Modul_25_Bridge`, `P_Modul_37_Bridge` 6.104e-05 m; the locals of `P_Modul_01_Filler/_Pusher/_Rinne` (+`001`) and `P_Modul_25_Bridge001` | 301/301 | 301/301 |

  (ii) puts every local back bit-exact but re-derives the world as parent
  world x local, which rounds differently from a world that was written
  directly: `P_Modul_30_Wippe`'s local equals the control's while its world
  differs in one element (`0x1.4f7a36p-23` vs `...38p-23`) and in z
  (`-0x1.e87c32p-4` vs `...30p-4`) at all 301 boundaries. Only (i) works, so
  (i) is built (the roadmap's rule: one working option is built, two go to
  the user). The structure differences (movable order, OV tree, units) are
  RO2's order effects, identical in every column.
- **Built:** `restorer::physics_rt` (`physics_savestate_restore.cpp`) skips
  `SetWorldMatrix` when the entity's live world matrix is bit-identical to
  the recorded one. No format, protocol or engine change. The final binary
  (rB8EGb, no switch) on the same two cells (`work/B8EG/egp_final.sh`): L8
  K=3 and L11 K=3 301/301 bit-identical.
- **Not covered by (i):** a parented body whose world differs from the
  record at the restore (it moved since the anchor) is still written, and
  its local re-derived against the parent's world as it is then; no
  measured case has one.
- Exact checks (the restore path changed): the wire check of core_ref_s8
  (rRTX's traces -> rB8EGb) 12/12 sends SAME, 12/12 journals IDENTICAL, ALL
  SAME; solo1 / solo2 against the live traces: sends SAME (2616, 2427),
  journals IDENTICAL (the tool's DIFFERENCES line is its missing
  client1-3 files of a one-client arm). The retail arm above ran it (205
  restores in Player.exe, `sv=205/0/0`).

## As built (step EL6, 2026-10-28): gap L, step G6 (L1)

Step G6 of §8 on HEAD 5eea6a5: the engine's half of L1 and the host's
mechanism half; the ball halves (L2, L3) are G7 / G8.

- **Detection** (`rollback_engine::revive_ended_lives`, `rollback.hpp`):
  only when the world has `restore_lives` and a replay follows (both
  paths). Candidates are the names of A's tracked set with a body record
  at A, in A's tracked order (own, remotes, mechanisms: the order step P
  made the mechanisms again in), minus a resync's `gone`. A name whose
  newest record in (A, now] has A's generation is alive (generations only
  grow), so the forward scan for the first record with another generation
  runs only for the ended ones; a moved birth's gap keeps the generation
  and is no end. `ended_life = {name, generation, end_tick, reborn_at,
  kind}`: **adapted**, `reborn_at` (the plan's birth item of a later life)
  and `kind` (own / mirror / mechanism by A's tracked set) were added, so
  that a half can keep step P's "not a name born again in the window" and
  leave the balls to the core.
- **`rollback_world::restore_lives(anchor, now, ended, savestate, out)`**:
  **adapted**, it takes `savestate` (whether the blob will be tried):
  step P's revive is savestate-only until G9, and the hook is called on
  both paths. `life_outcome = {revived_until, remade_at}`; a name with
  neither is unrevivable (no explicit flag).
- **Order, adapted:** the plan is computed first, as before (it is pure
  computation over the history); the hook then runs where step P's
  revive ran - after `replay_follows`, the hold and the clock, before the
  scripts' restore and the blob - and its answers amend the items: a
  revived name gets its A record (state, sleep as recorded, extra) unless
  a snapshot row planned it (§4: the row outranks), and `revived_until`;
  a remade name becomes a birth item at E from the record of E (or keeps
  its birth item) with `recreate` set (the destroy loop skips it,
  `from_inputs` from `birth_from_inputs`). A revive answered for a name
  whose later life is still in the world (a birth item without `recreate`)
  is logged and ignored. The plan trace moved after the hook and adds
  `revived`.
- **The two-half item** (`restore::revived_until`, `restore::birth`): every
  reader of 2.3.1 step 5 as the table says; plus two the table did not
  list: `reapply_rows`' parked test and the resync's mechanism list exempt
  a revived life before its end. A plain birth item's `birth` is its
  planned state / mode / extra (unchanged behaviour); the re-pose writes
  `birth` (pose and extra).
- **Refused blob:** the revived bodies stay (the old
  `destroy_body(entity, 0)` of the revive list is gone); their items are
  not covered, so the per-body path writes them from their A records.
- **Step P's revive in the host half** (`bind_session_world`,
  `session_host_world.hpp`): the core's half (`core_world_hooks::
  restore_lives`, unset until G7 / G8) first, then the mechanism half:
  savestate only, mechanisms only, a name no half answered, no later life
  in the window (the old "born again" log line, unchanged), no body now,
  `revive_body` (unchanged: the revivable set, the A-life's birth, muted);
  `revived_until` = the end. `rollback_world::revive_body` stays as the
  host's primitive; the engine never calls it. Two differences from the
  old engine loop, both outside every measured case: a name whose only
  plan item is a snapshot row is now revived (the row then written on it),
  and a mechanism with no body now but A's generation (a muted removal) is
  no ended life and is not revived. Two more, from the revived body now
  having a plan item (both what 2.3.1's reader table asks, neither in
  L-S0's byte-identical rounds nor in the captures, whose trafo inputs are
  off): when the blob drops its contacts the item is marked `recontact`
  and `set_contacts` writes A's contacts on it (under step P the blob
  alone did); and in the resync branch with a travelling blob, a revived
  piece the server's blob does not know gets `force_recheck` and a write
  from its A record, where step P left it at its `put_back` birth pose for
  the unknown-body loop to recheck.
- **Detection's reading of an unlisted name, documented:** detection reads
  `generation_in(newest, name)`, which is 0 for a name the newest record
  does not list, so such a name counts as an end, while `generation_now`
  reads it as keeping its last listed generation. No case today: the core
  copies its cumulative `generations_` into every record
  (`client_core_frame.cpp`) and clears them only at a session's start, so
  a tracked name is listed in every record of its session. A host or
  harness with sparse generation maps would see false ends; they can only
  add to `unrevivable` (the host half checks `get_body`), never revive a
  body wrongly.
- **Per-name skip logging, adapted:** the engine logs each unrevivable
  life by name (first 8: "lives: <name> (alive at tick A, generation g)
  ended at tick D inside the window and was revived by no half ... (the
  savestate skips it)"), not the restorer's own skip list: the bridge
  carries no names of skipped objects to the retail host (a bridge API
  change), and every ball is a tracked name, which is what L-H1 asks.
  The cap of 8 lines is per engine and shared with the inconsistent-answer
  lines, so G7's L-H1 ("names no ball") is judged on the `lives=`
  counter's `unrevivable`, not on the log.
- **`generation_at(entity, tick, out)`** (public): the generation in the
  record of `tick` (0 when unlisted; false without a record). For G8.
- **Counters:** `rollback_stats::lives_ended / lives_revived_own /
  lives_revived_mirror / lives_revived_mech / lives_remade /
  lives_unrevivable`; the status line (shared, so retail's too) carries
  `lives=ended/own/mirror/mech/remade/unrevivable` after `missing=`;
  SimTool's `--savestate-check` with trafo inputs prints a `lives (gap L,
  L1)` line whose `revived` sums all three kinds (own, mirror, mechanism),
  so it stays right once G7 / G8 revive balls. `late_birth_kept` waits for
  G7.
- **Layout:** `rollback_stats` grew by the six `uint64_t` counters and
  `rollback_engine` by `lives_logs_`; `physics_session_state` holds the
  engine by value and `BallanceMMOClient` holds that state, so the BML
  class grows, against core design §3.2's "no counter (layout)" for
  `rollback_engine` until R7. Precedent: B8EG's `births_parked_balls`,
  REACH and R12 grew it the same way and the RTX retail runs did not hang.
  The x86 build recompiled every includer of `BallanceMMOClient.h` (21
  objects, `BallanceMMOClient.cpp`, PSC, `core_host`, `client_automation`,
  `room_client` among them), so no stale layout ships; the FULL retail
  launch confirms there is no start-up hang.
- **Checks (quick tier):** x64 and x86 builds (deps valid), GCC 29/29 ok,
  unit tests 566 pass - L-U1 =
  `RollbackEngine.EndedLivesAreFoundByTheirGenerationAndHandedOverBeforeTheScriptsAndTheBlob`
  (end = first record with another generation, a moved birth's gap and a
  living name are no ends, hold < restore_lives < scripts < blob, none
  without a replay, `generation_at`), L-U2 =
  `RollbackEngine.ARevivedLifeWithALaterBirthIsPlannedInTwoHalves` (blob
  and per-body: covered at A / written from A, present before D, absent
  over [D, E), `births_recreated_exact` at E), step P's test rewritten
  (refused blob: the body stays and is written once), the binding's
  composition test (savestate-only, kinds, reborn, order, core first).
  **L-S0, adapted recipe** (`work/EL6/ls0.sh`: `--savestate-check` drives
  explosions only as trafo inputs - level 2, `--drop-settle 250 --ticks 250
  --trafo-sets 1 --explode-player 1 wood 10 ... --reset-player 1 wood 40
  --rounds 80 --max-depth 48 --seed 7`): rEL6 PASS 80/80, 9 rounds cross
  the pieces' death, 144 lives ended, 144 revived, 0 unrevivable, 48
  births re-made, all exact; the round lines (end hashes) equal rB8EG's
  byte for byte. `--legacy-restore` (the negative control, per-body):
  FAIL 80/80 in both builds with identical lines; 144 unrevivable (the
  per-body revive is G9's). Wire check rB8EG -> rEL6 over core_ref_s8:
  12/12 sends SAME, 12/12 journals IDENTICAL; solo1 / solo2 sends SAME,
  journals IDENTICAL. The replays' `lives=` show the open ball halves:
  swing_snapoff c0-c3 4 / 8 / 9 / 4 and resync_snapoff 3 / 2 / 2 / 2
  ended lives, all unrevivable (`Ball_Stone` and its mirrors, e.g.
  `Ball_Stone_BMMO_2755103046` alive at 700, ended at 721); l8_cross 0.
- **Live smoke (gate round):** the implement round's run (`el6_smoke`)
  never started a session: the Arch server was a stale build made with
  the slot-13 hack applied, while the headless client (built without it)
  announces `original_map_hashes[13]`, so the server's
  `is_original_level` refused it ("physics sessions need an original
  level", `server.cpp:2403-2405`). The gate round rebuilt the Arch server
  from this step's tree without the hack (`ballanced-36c04e3d24c9+bmmo-
  5eea6a5a1dd1-dirty`; a retail run needs it rebuilt with the hack) and
  ran `work/EL6/live.sh el6_smoke2`: 4 clients, 60 s, 150 ms, snapshots
  off, every client exit 0, mism 0, max_err 0, mechanisms 52/52, 24-52
  savestate rollbacks each, `lives=` 1-2 ended per client, all
  unrevivable - `Ball_Stone` (the own ball's death inside a window, G7's
  half). L-S0 rerun with the SimTool's summed `revived`: unchanged
  (144 / 144 / 0; the round lines of both paths equal rB8EG's).
- **Not run, deferred:** L-H0 as a same-batch live control (the wire
  check is its deterministic form); a retail session (no retail code
  changed, one comment).

## As built (step EL7, 2026-10-28): gap L, step G7 (L2)

Step G7 of §8 on HEAD 9ea33b6: the mirrors' half of L (2.3.3). The own
ball's half (L3) is G8; the mechanisms' generalization (L4) is G9.

- **The life log** (`client_core_state::life_entry`, `log_life` in
  `client_core_resync.cpp`): each entry keeps the stamp (`applied`), the
  origin, the player and type, `made_at`, the Physicalize as it came
  (`birth`), the `entity` and `generation` the event gave
  (`note_life_made`, from `physicalize_remote` and the live death) and,
  **adapted**, `skipped` (`note_life_skipped`: an overtaken birth dropped,
  a superseded death, a death of no mirror or of one with no body - events
  that changed no body are never queued again). Made in `apply_event`
  before the event's own rewind, as before. The trim keeps the last
  `history_ticks + 128` ticks and, of the older entries, each player's
  newest Physicalize and (review finding 2) each player's newest entry of
  any kind, so a death after that birth still makes the reconcile's
  consistency test refuse the life (an exact resync makes a mirror from
  its row with no log entry; a trimmed death would let the stale birth
  stand in for it) (L-U10).
- **The reconcile** (`session_client_core::reconcile_remote_lives`,
  `client_core_lifecycle.cpp`; `core_world_hooks::restore_lives` set in the
  core's constructor): per ended mirror life, the A-life is the newest
  logged Physicalize of that entity stamped <= A, checked against the log
  (no event of the player in (birth, A]); then steps 1-3 of 2.3.3 (the
  player's body now taken out muted, the A-life made by `make_mirror` with
  its logged entity, recipe and ball type at the A record's pose -
  `quaternion_rows` of its quaternion - no kick, navigated, the remote set
  back to it; every window event queued at its stamp, births with their
  logged entity) and the generations of 2.3.2 (A's value, erased when A had
  none). **Adapted:**
  - *Who is reconciled.* First the players whose mirror life alive at A
    ends in the window (the engine's ended lives). Then (review finding 1)
    every other player whose logged window events LB cannot replay: a life
    born and ended inside the window (no record at A, no body now - no
    restore item names it, so before the fix the replay ran without it and
    no counter showed it), two births in the window, or the body now made
    after a stamp inside the window (the RC stamp rule: replayed at the
    stamp). For those the reconcile takes the body now out, puts the
    generations of the window's entities back to A's and queues every
    window event (steps 1 and 3, no revive); counted `mlives=.../window`.
    The engine (`revive_ended_lives`) therefore asks the halves at every
    restore a replay follows, with no ended life too (EL6 asked only with
    one; the host's mechanism half does nothing then). A player whose only
    window event is the birth of its body now, made at its stamp, keeps
    LB's path (`destroy_body`, made again at its stamp: the declared
    behaviour change of 2.3.3 is LB's for it); one whose window begins with
    a death keeps nothing to replay - the engine did not hand that life over
    because no record of the window shows its end yet (a late death's own
    rewind, A = D - 1, and the late birth's of the same beam pair: the
    first version counted these as failures, 9/18/4 per client in the
    capture replays, before the gate round skipped them). So a birth made
    late keeps parking only when it is stamped at or before A (the
    documented class, `late_births_kept`, now counted once per (player,
    stamp) and only for stamps at or before the restore's anchor - review
    finding 4). `make_remote_birth` keeps queueing its own late birth (the
    reconcile de-duplicates by player and stamp) and keeps its live
    fallback for a refused or cut-short rewind.
  - *Remade at which birth.* A name is answered `remade_at` its LAST window
    birth (RC: one name per player, several lives in the window): the life
    its restore item - the first record at the name's current generation -
    is about. A body made live after its stamp has that item at the tick it
    was made (L > E): the engine moves such an item to the answered stamp
    (`move_to_stamp`, review finding 3) and recreates it; before, it found
    no item at E, logged "kept on the birth item", counted the life
    unrevivable and re-posed the body at L while the core made it at E.
  - *The end.* `revived_until` is the stamp of the player's first window
    event, not the engine's `end_tick`: records re-captured by a death's own
    rewind keep the old generation without a body, so the record-based end
    can be later (swing_snapoff: the beams at 701, the engine's end 721 in
    EL6's lines).
  - *Another name.* On H the later life is another clone, which was no life
    at A; the reconcile answers `remade_at` for it, and the engine
    (`revive_ended_lives`, `rollback.hpp`) now recreates the birth item of
    any answered name that ended no life here whose item is a birth at that
    tick or later (moved to it; the destroy loop leaves it alone;
    `birth_from_inputs`). The core's `mirror` hook knows
    every mirror entity this session made (`mirror_entities_`), so that
    test answers for the clone the remote no longer points at; an old
    clone has no body outside such a replay, so `keeps_parked`,
    `destroy_body` and `revive_body` answer for it as before.
  - *A failed revive* puts the A-life's generation one past A's (its end
    bumps nothing without a body) and still queues the window events.
  - *The parked arm* (`core_config::remote_births_parked`, headless
    `--remote-births-parked`, L-H6's control): LB's remake is off, and the
    reconcile revives no player that has a body now (counted failed).
  - *The resync.* While an exact resync's restore runs
    (`resync_restoring_`) the reconcile does nothing: the resync's own
    reconcile (resync design 2.6.3, its deaths after S) handles its
    mirrors. RSD 2.6.3's "one implementation" is **not built**; the
    resync's mirror made from its row still mints a new clone on H, as
    before.
- **`drain_pending_lives`** (was `drain_pending_births`): the births and
  deaths due at the tick in one list, sorted by `lifecycle_apply_before`
  (join, origin, rank: a beam pair's death first), our own lives at our
  rank. Before, a resync's deaths came after every birth of the tick. A
  reconciled birth whose make fails is queued again. `pending_death` gained
  `origin` (the resync fills it from the log) and `reconciled`.
- **`settle_unreplayed_lives`** (after `rewind_to` in `rewind_world`, and
  after `on_snapshot`): the reconciled events no replay applied are applied
  live in stamp order (L-U8's fallback).
- **`physicalize_remote`** makes `event.entity` when it is set (no
  `mirror_entity` inside a replay). **SC's exact-name `make_mirror`** was
  already there (step LB): the headless client changes only by the new
  switch.
- **`recorded_input_at`**: a name that is no remote's current entity is
  mapped to its player by the life log (`mirror_player_of`) and reads that
  player's ring.
- **Counters:** the status line's
  `mlives=revived/failed/settled/kept/window` after `remake=`.
- **Layout:** `client_core_state` grew (the log entry's fields, the
  counters, `mirror_entities_`, `resync_restoring_`); the x86 build
  recompiled all 21 includers (`BallanceMMOClient.cpp`, PSC, `core_host`,
  `client_automation`, `room_client` among them; deps valid), EL6's
  precedent.
- **Checks (quick tier, about 12 minutes):** x64 build (deps valid) and x86
  build (CLIENT-BUILD-OK); GCC 29/29 ok; unit tests 573 pass (566 + 7):
  L-U3 `SessionClientCoreLives.AMirrorDeadInTheWindowIsRevivedUnderItsOwnName`,
  L-U4 `...ARetailBeamPairInTheWindowIsRevivedThenRemade` (with the parked
  arm), L-U5 `...TheRetailStampRuleReplaysAtTheStamp`, L-U8
  `...WindowEventsNoReplayMadeAreMadeLiveOnce`, L-U9
  `...ALifeInsideTheWindowIsMadeAndRemoved`, L-U10
  `...TheTrimKeepsEachPlayersNewestOldBirth`, and the engine's
  `RollbackEngine.ALaterLifeUnderAnotherNameIsRemadeNotDestroyed`. Wire
  check rEL6 -> rEL7 over core_ref_s8 plus solo1 / solo2: 14/14 sends
  SAME, 14/14 journals IDENTICAL (the verdict line reads DIFFERENCES only
  because the tool looks for clients 1-3 of the one-client solo arms). The
  replays do revive mirrors (swing_snapoff c0-c3 `mlives` 4 / 6 / 7 / 4
  revived, 0 failed), all anchored at 700 with the beam pairs at 701: the
  revived body goes before the first replayed step, so the journals stay
  identical. What is left unrevivable is the own `Ball_Stone` (G8's), in
  swing_snapoff (0 / 2 / 2 / 0) and resync_snapoff (3 / 2 / 2 / 2).
- **Live (two runs; the Arch server is EL6's build - no server source, and
  no file any server object depends on, changed in this step):**
  `el7_smoke` (4 clients, 60 s, 150 ms, 10 % loss, sector 4 beams at 700,
  snapshots off): every client exit 0, `sv=N/0/0`, mism 0, `mlives` 4 / 6 /
  6 / 6 revived, 0 failed, 0 settled, all at A = 700. `el7_forced` (the same
  with `--force-rollback 13:40` on every client, for windows anchored
  before a remote death - L-H1's case): 283-301 rollbacks each,
  `sv=N/0/0`, mirrors revived 11 / 10 / 13 / 9, 0 failed, 0 settled, across
  the beams (A = 688, D = 701) and death-zone deaths (A = 831 ... 3223,
  D = 844 / 866 / 914 / 946 / 3157 / 3169 / 3227); unrevivable only the own
  `Ball_Stone`; on every client the clone names equal the lives (9 / 9 / 9 /
  10 distinct (player, stamp) births and as many clones: no clone minted by
  a rewind).
- **Not run, deferred (FULL):** L-H1's and L-H2's comparisons against the
  server with a same-batch control (`first_divergence.py` over the death
  windows), L-H6 (remade vs `--remote-births-parked`, same batch), L-R2
  (two retail clients on Level 11, a beam pair inside a forced window; no
  retail code changed, the reconcile reaches retail through the shared
  core). A capture replay cannot add forced rewinds (the capture's clock
  reads: "expected a clock read at rewind_start"), so the A < D-1 case has
  no deterministic offline check beyond the unit tests.
- **Gate round (the review's findings 1-4, about 6 minutes):** x64 and x86
  builds (the 14 x86 includers of the changed headers recompiled, deps
  valid), GCC 29/29 ok, unit tests 576 pass (573 + 3: L-U9 without an
  A-life `SessionClientCoreLives.ALifeInsideTheWindowWithNoLifeAtTheAnchorIsMadeAndRemoved`,
  `...WindowLivesWithABodyNowAreTakenOutAndReplayedAtTheirStamps` (RC
  naming: the last window birth answered, a body made after its stamp
  remade at it, LB's case left alone), `RollbackEngine.AWindowBirthMadeAfterItsStampIsRemadeAtTheStamp`;
  L-U5 counts a kept birth once, L-U10 keeps player 5's death, EL6's
  engine test now expects the halves asked with nothing ended). Wire
  check rEL6 -> rEL7h over core_ref_s8 plus solo1 / solo2: 14/14 sends
  SAME, 14/14 journals IDENTICAL; `mlives` as in the implement round
  (swing_snapoff 4 / 6 / 7 / 4 revived, 0 failed, 0 window: no capture
  holds a life born and ended inside one window). No live run (the step's
  two were spent in the implement round): the window path has the unit
  tests as its only check until FULL.

## As built (step EL8, 2026-10-28): gap L, step G8 (L3)

Step G8 of §8 on HEAD 6931c90: the own ball's half of L (2.3.4) and §9.7.
The mechanisms' generalization (L4) is G9.

- **Own deaths in the log** (`client_core_state::own_death`,
  `note_own_death`): **adapted** in two ways.
  - A list of its own (`own_deaths_`), not a third `own_life::kind`: the
    echo matching (`process_own_echo`, `client_core_lifecycle.cpp`) takes
    the first `own_life` with the echo's origin, ball type and verdict
    `sent`, so a death entry would be taken for a birth echoed at the same
    tick (a trafo's old ball dies in the frame its new one is born).
  - Recorded at the frame's drain (`client_core_frame.cpp`, the "deleted"
    entry of an entity that is the own ball or ever was:
    `own_ball_entities_`), before the generation bump, not in
    `report_unphysicalize`: the drain is the one site both own-lifecycle
    sources reach, and the only one that sees the trafo's old ball (the
    event-log path reports a death only for the current ball, and
    `report_unphysicalize` knows no entity). Nothing new is sent.
  - The matrix is `entity_matrix` after frame D; retired once no rewind
    can be anchored before it (`retire_own_deaths`, from
    `retire_own_lives`), cleared at every renumbering
    (`clear_own_window_state`).
- **Platform** (`client_platform.hpp`): `own_body_spec::with_replica`;
  `unmake_own` gained `bool death`; new `entity_matrix` /
  `set_entity_matrix` (CK world matrix, row-major); new capability
  `own_body_in_replay` (requires make_own / unmake_own / entity_matrix /
  set_entity_matrix). The headless client declares it beside
  `own_body_control`; the retail core host declares it with
  `own_body_control` still false.
- **The life test at a replayed tick** (`own_life_current(life, tick,
  live)`): as specified - a replay compares with `generation_at(entity,
  tick - 1)` (the replay re-captures its records with the live records'
  generations, so this is the live record's), a live frame with the live
  generation; a replay never retires (it logs "not applied ... in another
  life there" instead). The echo paths and `moved_own_life` keep the live
  test.
- **The own reconcile** (`session_client_core::reconcile_own_lives`,
  asked by the core's `restore_lives` hook before the mirrors'):
  per ended own life, its logged death (same entity and generation, the
  first in (A, now]); a body of that name now must be a later life born
  after the death (the newest own birth record, made at its stamp), which
  is taken out at the restore (`unmake_own`, not a death) and queued for
  `remake_own_body` at E (answered `remade_at = E`); the A-life is made by
  `make_own` muted (A record's pose, `ball_recipe_of`, `own_group_`, no
  kick, `with_replica`), its death queued at D (answered `revived_until =
  D`). Then every own birth record stamped in (A, now] whose life died in
  the window and that has no body now (L-U9) is remade at its stamp and
  removed at its death. Own births whose life is alive now and whose name
  was not revived stay parked, as before. **Adapted:** generations are not
  touched (the muted changes bump nothing; the re-captured records carry
  the live generations); the pre-rewind matrix is saved by the core's
  rewind scope (below), not by the revive.
- **In the replay** (`apply_own_due`, at the end of
  `drain_pending_lives`, after the remote events and `apply_own_lives`):
  a death is `unmake_own(entity, pose of the death's matrix, D, death =
  true)` muted; a remake is `remake_own_body`. What no replay applied is
  applied live after the rewind (`settle_own_due`, beside
  `settle_unreplayed_lives`; counted).
- **The replica, adapted** (2.3.4 says "the replica detached first" at D
  and "RC saves `host.own_replica` ... and puts it back in the guard"): a
  replayed death leaves the replica as the live death left it - its leaves'
  controllers lose their core with the body
  (`core_is_going_to_be_deleted_event`, `ball_navigation.cpp`), exactly
  as live - so neither host's replica record or navigation state is
  changed by a revive and nothing needs putting back. When the replica is
  not on the revived entity (a trafo's old ball: live, the replica moved
  on to the new ball), host support's `attach_revive_navigation` makes a
  navigation of its own for it (polling off; the restore's `set_nav` then
  gives it A's state), and the host destroys it with the body at the
  replayed death (`revive_navigations_`, headless member; retail
  file-scope `host.revive_navigations`). The body part is host support's
  `make_own_body` / `unmake_own_body` (moved from the headless client,
  unchanged); both hosts call them.
- **The matrix (§9.7, settled: option (a) built)**: `record_own_matrices`
  stores, after each live record, the CK matrix of every own entity
  without a body; `write_own_matrices(t)` (first in
  `before_replayed_tick`) writes the record of t itself (the matrix the
  live frame of t ended with) to each of those that has no body in the
  replay either, and nothing where the entity already has that value bit
  for bit (gap P's rule: `SetWorldMatrix` re-derives the local matrix,
  and the children's, with rounding); and the core's rewind scope
  (`session_client_core::keep_live_cells`, which now wraps the host's
  guard at all four rewind sites) saves the bodiless own entities'
  matrices on entry and writes them back on exit when they still have no
  body and differ. **Gate round (review finding 1):** the implement round
  wrote the record of t - 1, and the capture replays showed new float
  script differences at the end of windows across a revived own death
  (resync_snapoff c0 2459 and c2 2444 P_Modul_18 / P_Modul_29, c1 2472
  PE_Balloon "Current Distance"). Cause, measured with a per-tick
  diagnostic of the own entities' matrices (live before the step, live
  record, replay before the scripts): c0's ball dies at 2452, and the
  live frame 2453 moves the bodiless `Ball_Stone` from its death pose
  (1017.56, -97.37, 15.02) to the resetpoint (700, 3.326, 15); that mover
  (the death sequence) is not replayed and, live, runs before the modules
  whose `TT Scaleable Proximity` reads the ball, so the modules of 2453
  read the resetpoint. Writing the record of 2452 left the replayed 2453's
  ball at the death pose; P_Modul_18's scaleable proximity sampled it
  there and held that distance until the window's end (its check
  interval grows with the distance). With the record of t every one of
  those differences is gone (and the swing_snapoff c1 867 / c2 922
  P_Modul03 float differences that rEL7 had stay gone); against rEL7 the
  only changes in the replays' script-difference lines are those three
  float differences removed (resync c2 2444, swing c1 867, c2 922), none
  added. The candidates "the revived ball is not driven as live" and
  "the death pose is written too early" are refuted by the same
  diagnostic: in all 17 revive windows of resync_snapoff / swing_snapoff
  the own ball's full body state before each replayed tick (position,
  rotation, linear and angular velocity, simulated, movement state) is
  bit-identical to the live state before that tick, and over 4,285
  bodiless own-entity ticks the live state after every rewind equals the
  one before it. Matrix writes dropped from 76-364 per client to 0-4
  (swing_snapoff c0 196 -> 0: the inactive ball types after a trafo are
  never written). (b) (the frame's parent chain) is not built: no measured case
  re-parents `Ball_Pos_Frame`. The retail confirmation (L-R1 with
  `BMMO_TRACE_ENTITY`) is open (below).
- **Retail verb:** `session force-rollback at L:D[,L:D...]` (one-shot
  rewinds after the frame of live tick L; L-R1's form).
- **Counters:** the status line's `olives=revived/failed/remade/settled/
  matrices` after `mlives=`.
- **Checks (quick tier):** x64 build, x86 build (21 objects recompiled,
  `BallanceMMOClient.cpp`, PSC, `core_host`, `client_automation`,
  `room_client` among them; deps valid), GCC 29/29 ok, unit tests 580 pass
  (576 + 4: L-U6 `SessionClientCoreLives.AnOwnDeathInTheWindowIsRevivedAndRemovedAtItsTick`
  (with the capability-off half), L-U7
  `...AnOwnSameNameRebirthIsTakenOutAndRemadeAtItsStamp`, L-U9 own half
  `...AnOwnLifeInsideTheWindowIsMadeAndRemoved`, L-U11
  `...ABeamInsideALifeThatDiesLaterIsAppliedInTheReplay`). Wire check rEL7
  -> rEL8 over core_ref_s8: 12/12 sends SAME, 12/12 journals IDENTICAL;
  solo1 / solo2 sends SAME (2616, 2427), journals IDENTICAL. The replays
  now revive the own `Ball_Stone` that EL6 / EL7 left unrevivable:
  `lives=` unrevivable 0 in every replay (swing_snapoff c1 / c2 3 / 4 own
  revived, resync_snapoff 3 / 3 / 2 / 2; l8_cross and the solo arms have
  none), `olives=` failed 0, matrices written 76-364 per client.
- **Gate round.** Also: a moved own birth is not a later life
  (`birth_of`): `rollback_world` calls `restore_lives` before
  `destroy_body`, so a moved own birth (headless, `own_body_control`)
  still has its body at the reconcile and the moved path owns it; such a
  life fails ("its body now was not born after its death", olives'
  failed) and keeps today's behaviour, L-H4 covers it (review finding 4,
  stated in the code). Retail: a revived ball with no ball table row gets
  no navigation (logged) instead of row 0's force (finding 5).
- **Checks (gate round):** x64 BUILD-OK, tests 580 pass, GCC 29/29 ok
  (unchanged), x86 CLIENT-BUILD-OK (20 objects, the slot-13 hack applied
  and reverted), wire check rEL7 -> rEL8g over core_ref_s8: ALL SAME
  (12/12 sends SAME, 12/12 journals IDENTICAL), solo1 / solo2 sends SAME,
  journals IDENTICAL; `lives=` unrevivable 0, `olives=` failed 0
  everywhere. **L-R1** (`maps/arena-test/el8_lr1g`, after
  `r3_retail.py arena-install`; eg0_retail.py, core on, snapshots off,
  `at 664:24,1377:17,1380:16,1384:16`): the four forced rewinds each log
  the own ball revived (`Ball_Stone` <652 over the beam at 650, which the
  replay applies: the replayed 650 has the ball at (0, 4, 43); `Ball_Wood`
  <1369 three times) and its removal at D ("taken out ... replayed
  death"); `olives=4/0/0/0/15`, `lives=4/4/0/0/0/0`; no script
  difference and no savestate-replay difference at any rewind's end (the
  1378 "Current Distance" difference of r2_a_f13 does not appear); no
  warning; the guard's matrix after each rewind equals the live trace's
  before it (664: (-0x1.3204d8p-9, 0x1.abca96p+1, 0x1.58p+5)), replica
  inactive as live; `BMMO_TRACE_ENTITY` (§9.7's settle): before every
  replayed tick the traced `Ball_Pos_Frame` / `Ball_Stone` / `Ball_Wood`
  equal the live run's - after tick t - 1 while the ball has a body,
  after tick t (the written record) while it has none - except at the
  ticks a body appears or goes (650: the beam; 1370: the resetpoint
  written for D + 1); the own lifecycle (deaths 652 / 1369, births 817,
  respawn 1567 at the same bits) equals `eg0_ctl`'s (the same recipe on
  EG0's binary; not a same-batch control). **L-S3 not run:** SimTool has
  no own-ball revive (the own half of `restore_lives` is the core's,
  which SimTool does not run), so the check needs a new SimTool scene;
  and the capture replays cannot stand in: Gameplay_Ingame's force
  controller (`rt:force/26432`) is on the own core only while a key is
  held, and at every anchor of the 15 distinct revive windows of
  resync_snapoff / swing_snapoff the live core was `n=1 sys:unknown/1000`,
  exactly the revived core's. So §9.3 has no L-S3 result: it stays open
  and does not go to the user yet.
- **Not run, deferred:** L-S3 (a SimTool own-revive scene: new tooling),
  L-H3, L-H4 (headless live runs: the step's two live runs went to L-R1,
  the first `el8_lr1` with no session because both game copies had the
  original `Level_13.NMO` back), L-H6 (FULL). Seen in the offline
  replays and pre-existing (rEL7 has it at the same ticks): the logical
  `IngameParameter` cell differences at the end of windows across an own
  death (`[0][2] 4 vs 0`, `[0][1] 1 vs 4`), L-H3's to judge.

## As built (step EL9, 2026-10-28): gap L, step G9 (L4)

Step G9 of §8 on HEAD ec456ba: the mechanisms' half of L (2.3.5), step
P's revive generalized. With it the four steps of gap L (G6-G9) are built;
G10 (records) is review only.

- **The revivable set (2.3.5 "a name the host's selection can kill"),
  adapted:** a new optional `rollback_world::revivable` (`rollback.hpp`;
  the engine never calls it), composed like `remade_by_replay`:
  `bind_session_world` sets the host's part from
  `session_world_options::revivable` - the hosts' `explosion_pieces`,
  which already hold every trafo set's pieces since T3
  (`add_trafo_set_pieces`) - and `bind_script_replay`
  (`rollback_binding_scripts.hpp`) widens it with every body of a module
  a selected script owns (`script_replay::owned_by_selection`, RO1's
  test). Only for a host that names a revivable set at all (the SimTool's
  pass and late-sector bindings name none and are unchanged). The hosts'
  options are unchanged (comments only, `session_client.cpp`,
  `core_host.cpp`). TRD's retirement dropping a set at its deselect is
  not built: a retired set's pieces have no record left (TRD 2.4), so no
  window reaches them and their name in the set is inert.
- **Late binding:** `revive_body` and the mechanism half read the
  finished world's `revivable` and `destroy_body` (the script binding's
  wrapper, which releases the joint handles held on the body) through a
  shared `late_world_hooks` filled after `bind_scripts`
  (`session_host_world.hpp`), since both are wrapped after the half is
  composed. `revive_body` is still installed only when the host names a
  revivable set; its other tests (no ball, no own body, no mirror, the
  A-life's birth, muted) are unchanged.
- **The mechanism half** (`bind_session_world`'s `restore_lives`): on the
  savestate and the per-body path alike (the `!savestate` return is gone:
  L-S2). A mechanism with no body now and no later life is revived as
  before. A name born again inside the window (`reborn_at`): only when it
  is revivable and its A-life's birth is known (so no later life is taken
  out for nothing), its later life - when it has a body now - is
  destroyed through the wrapped `destroy_body` at its birth tick, and the
  name is answered `remade_at = reborn_at`; **adapted (order):** every
  such destroy runs before any revive, so the revives meet the world the
  single-life case meets (no later life), in A's tracked order. Then
  `revive_body`; a revived name gets `revived_until = end`. A destroy
  that fails leaves the name unanswered (unrevivable, the later life
  stays; logged); a revive that fails after the destroy keeps
  `remade_at` (the engine plans the remake, counts the A-life
  unrevivable; logged). The engine side was G6's (a name answered both
  becomes one item: `recreate`, `birth` from E's record, `state` from A,
  `revived_until`). Step P's "born again ... the replay runs without
  them" line is gone.
- **Layout:** `rollback_world` is built per engine call (no BML member);
  `rollback_stats` and `rollback_engine` unchanged.
- **Checks (quick tier, ~10 min):** x64 BUILD-OK (deps valid), x86
  CLIENT-BUILD-OK, GCC 29/29 ok, unit tests 580 pass
  (`RollbackBinding.SessionWorldComposesTheCoresHooksInTheDesignsOrder`
  extended: both paths, the later life destroyed first through the
  wrapped `destroy_body`, revived and remade, a non-revivable or unborn
  name's later life left alone, a refused destroy left unanswered, the
  selection's widening read late). Wire check rEL8 -> rEL9 over
  core_ref_s8: ALL SAME (12/12 sends SAME, 12/12 journals IDENTICAL).
  **L-S1, adapted recipe** (`work/EL9/ls1.sh`, like EL6's L-S0: trafo
  inputs, level 2, `--explode-player 1 wood 10 ... --reset-player 1 wood
  20 --explode-player 1 wood 165 ... --rounds 160 --max-depth 48 --seed
  7`; the reset De Physicalizes about 133 ticks after it and the ledger
  wants 140 between a reset and the next explosion, TRD 2.9, so the
  pieces die at 404 and are born again at 417): rEL8 FAIL at round 24
  (anchor 401: "16 mechanism(s) ... born again inside the window",
  "object 'Ball_Wood_piece01_Trafo1' is in the savestate but not in the
  world"); rEL9 PASS 160/160, 25 rounds cross the death, 400 lives ended,
  400 revived, 0 unrevivable; 12 of them also cross the rebirth (192
  remade, all exact: 352 births exact, missing 0, no birth difference
  logged). **L-S2** (`--legacy-restore`, the per-body negative control,
  never byte-exact): rEL8 0 / 400 revived, rEL9 400 / 400; no "in the
  savestate but not in the world"; the per-round object-count mismatches
  of the death-crossing rounds (rEL8: 63 vs 47, the pieces missing) are
  gone (rEL9 fails those rounds on collision counts only, as the
  per-body path does everywhere). L-S0 (`work/EL9/ls0.sh`): rEL8 and
  rEL9 PASS 80/80, 144 / 144 revived; legacy 0 -> 144 revived.
- **L-H5** (`work/EL9/lh5.sh`, after TR3's h5.sh: one client, trafo
  inputs on, placement on, snapshots off, beams onto P_Trafo_Wood /
  P_Trafo_Paper at 650 / 850 / 1050 / 1250, so the wood set explodes at
  1009 and again at 1409 after the reset of 1261; control plus
  `--force-rollback 13:47`, 109 forced): every piece bit-equal to its own
  session's server journal in both arms (`pieces.py`: 51/51 and 51/51),
  no "PreSimulate queue differs"; the arm's restores of 1370 and 1383
  cross the pieces' death and their rebirth at 1409 and revive them
  (the same-name case live), and no replay difference is logged in
  those windows. The arm's other replay differences (Ball_Stone
  controllers at 637, P_Modul_18 Row Search and the pieces'
  collision-sound script at 819-1053) are all in windows across an own
  death, G8's area (L-S3 / L-H3 / L-H4).
- **The own ball in L-H5, measured not this step's (review finding 1,
  gate round; `work/EL9/gate/`):** every non-ball body is bit-equal to
  its server at every tick 1400-1470 in both arms, and both arms' server
  balls are identical (same inputs). The live own ball leaves its server
  20 ticks after its birth at 1018 in every arm, the rewind-free control
  included, with one signature (1038: 0.00643409, 1040: 0.047821, 1042:
  0.214452 m); after the birth at 1418 the control leaves at 1444
  (0.0225551 m) and the 13:47 arm shows the 1018 signature again from
  1438. A rewind-free SimTool replay of each arm's client journal
  (`--replay-session --write-journal`, `ballrep.py`) gives an own ball
  bit-equal to the server's at every tick 1418-1470 in both arms, so the
  inputs and the events reproduce the server and only the live client's
  own ball differs: SFD's cause (A) (the own ball's IVP collision
  bookkeeping from its birth, waiting for the user). The 13:47 arm's
  first divergent record (1438) follows the restore anchored at 1383,
  which revives, so a second live arm settled it: `lh5.sh b 13:20 rEL9`
  (`maps/arena-test/el9_lh5_b`), whose only revive restore is anchored at
  1384 and ends at 1404 and whose only window over the 1418 birth is
  anchored at 1410 (no revive, no later life destroyed), shows the
  identical signature from 1438 (0.00643409 / 0.047821 / 0.214452 /
  0.347734 m), pieces 51/51 and 51/51, "PreSimulate queue differs" 0.
  What exposes the signature at 1418 is a window over the own ball's
  birth (the core's own-birth path), not the mechanism half. (The
  savestate_check verdict compares the two sessions' clients with each
  other and is MISMATCH in both runs, 52 of 55 bodies, worst a paper
  piece from tick 1210, the same in the implement round; it is not the
  L-H5 judgement, which is each arm against its own server.)
- **Review fix-ups (gate round):** the same-name branch now also refuses
  a ball, an own body or a mirror (`revive_body`'s refusals, in its
  order) before the destroy, so no later life is taken out that
  `revive_body` would not make again (latent: no such name reaches the
  half today); unit test extended (a ball and a mirror the revivable set
  names, with an A-life and a later life: no destroy, wrapped or not, no
  answer). Accepted, not changed: the half reads `destroy_body` as
  `bind_session_world` leaves it, before `core_host.cpp` wraps it with
  the EG0 `diag_trace_plan` logger, so a destroy the half makes is not
  in that (off-by-default) trace; the half logs a refusal itself.
  Rerun: x64 BUILD-OK (deps valid), x86 CLIENT-BUILD-OK (core_host.cpp
  recompiled), GCC 29/29 ok, unit tests 580 pass, wire check rEL8 ->
  rEL9 ALL SAME (12/12 sends SAME, 12/12 journals IDENTICAL), L-S1 rEL9
  PASS 160/160 (400 revived, 352 births exact, missing 0).
- **Not run, deferred:** L-H6 (FULL), a retail session (the change
  reaches retail through the shared binding; FULL).

## As built (step RLQ, 2026-10-28): the savestate check binds gap Q's hold

Found at WG2 (roadmap §4 item 4, "harness, gap Q's hold"): SimTool's
`--savestate-check` bound no `callback_queue`, so its blob restores compared
the whole PreSimulate queue (mode 0) where both clients set the outside
entries aside (Q4, B7EG's hold).

- **The entries, dumped per entry** (new: `--queue-trace` works in
  `--savestate-check`, each entry with its target and whether that has a
  body; `.local-tools/work/RLQ/v1/dump.txt`). WG2's scene (arena, P_Modul_01
  drop, one ball, lead 60, window 90..209): the world logs "retail ball
  parked (body removed) at tick 204"; the queue is empty to record 205, holds
  `27403 <Physics WakeUp>@Gameplay_Ingame` at record 206 and that plus
  `6471, 6489, 6507, 6271 <Physics Collision Detection>@Sound_Manager` from
  record 207 on - all five targeting `Ball_Stone#42243` (a Gameplay_Ingame /
  Sound_Manager local), which has **no body**. WG2's inference (read from
  the timing only) is confirmed: they target the parked retail ball.
- **The binding (shared, one implementation).** `script_binding_options`
  gains `hold_outside` (`session/rollback_binding_scripts.hpp`): Q4's hold
  bound without `replay_activation` too. **Adapted:** B7EG bound the hold
  only with `callback_queue && replay_activation` (the clients' recipe);
  the savestate check's default recipe is the v14 one (a script inactive at
  the anchor keeps its live state), which no client runs, so the hold alone
  is bound there by the new switch. The hold is the clients' lambda,
  unchanged; its executed set (every selected and trafo script) moved into
  `replay_executed_scripts`, which the hold and the check both call. On the
  v14 recipe an entry of a selected script the restore does not write stays
  compared (fail-closed, the refusal of before); no queue section is
  recorded there. Both clients are unchanged (they never set
  `hold_outside`; the wire check below). The savestate check sets
  `callback_queue` and `hold_outside` (`--savestate-no-hold` neither: the
  binding before RLQ, kept as the negative control); with the session
  recipe (`--session-recipe` / a `--second-move` in the window) that is the
  clients' whole Q binding, record v4 / v5 included.
- **The compared blobs (adapted, the harness's half of Q4).** Binding the
  hold alone fixes the restore but not the compare: the replay never runs
  an outside script, and a blob captured inside a replay lacks the held
  entries (the B7EG note's "diagnostic noise"), so a round crossing record
  206 failed `world - callbacks 1 0` (2 of 50 rounds, measured). The
  check therefore captures every blob it compares against or with - the
  straight pass's expected B[t], the depth-0 restore probe, the re-made
  live world's premise and the follow - with the same outside entries set
  aside (`callbacks_hold(executed)`, the capture, `callbacks_unhold`; a
  clamped put-back fails the capture). The engine's own record of a tick
  keeps the whole queue (the restore's scoped check skips the outside
  entries there, as in the clients), and the executed scripts' entries are
  still compared byte for byte. The hold and put-back around a straight
  capture leave the straight pass bit for bit as it was: its 120
  `straight` trace lines (world hash, pose hash, cores, blob size, contacts)
  are identical with and without the hold. **What this gives up, and how it
  is judged instead (the review's finding):** the compared blobs never hold
  the outside entries, so a blob compare cannot see one put back in the
  wrong place. The order is judged separately: a round fails when the
  engine's put-back at the end of the replay clamped an entry or its hold
  failed (the cbq `unhold_clamped` / `hold_fail` counters grew during the
  round). The NUMBER of outside entries is deliberately not compared with
  the straight pass's: an outside script runs in the harness's live time,
  which the rounds' rewinds never turn back, so it drifts ahead of the
  straight pass's tick. Measured: comparing the count (the depth-0 probe and
  the follow) failed B2SX's L13 cell 13/60 and L2 1/60 with "5 outside
  entries vs 0" - Gameplay_Ingame's level start physicalizes the retail
  ball during round 31 of L13 (the world logs "retail ball parked (body
  removed) at tick 11", its rewound tick) and round 58 of L2, after enough
  cumulative live steps, while the straight pass (ticks 3..43) never
  reaches it; every depth-0 round after it carries the five WG2 entries.
  That is Q4's premise (outside entries are live state, a rewind leaves
  them), so the count stays out and only the put-back's order is judged
  (B7EG's clamp). Since RLQ the straight pass's `--savestate-trace` `blob_hash` /
  `blob_size` describe the compared blob (the outside entries left out)
  on the ticks that have some (206 on in WG2's scene), so such a trace
  differs from a pre-RLQ one there without a real difference. The summary prints
  `cbq (step RLQ, hold only|+queue section|off): straight ticks with
  outside entries=N (at most M)` and the clients' cbq counters.
- **Checks** (quick tier, `.local-tools/work/RLQ/v1.sh`, `v2.sh`, `v5.sh`;
  rRLQ = this tree, rWG2 = the pre-step build): x64 BUILD-OK (deps valid),
  x86 CLIENT-BUILD-OK (`core_host.cpp` recompiled, deps valid), unit tests
  590/590, GCC syntax check 30/30 ok, wire check rWG2 -> rRLQ over
  `core_ref_s9`: 12/12 sends SAME, 12/12 journals IDENTICAL, ALL SAME.
  - WG2's scene, 90..209, 50 rounds, depth <= 48: **PASS 50/50** (rWG2:
    refused at round 0, "the PreSimulate queue differs", the five entries
    above); 90..203 PASS 50/50 (unchanged); `--savestate-no-hold` on rRLQ
    refuses at round 0 exactly as rWG2 (negative control); `--legacy-restore`
    still FAILS 50/50 (`restore identity: world - collisions 17 5`).
  - WG2 V4's dead scene under `PROXIMITY_UNION` (two balls killed at 75),
    90..209: **PASS 50/50** (WG2: refused); with `--probe-scramble after`
    still **FAILS 50/50** (P_Modul_18's `Last Check` 27 vs 32): the
    proximity hold's scramble is not hidden by the queue hold.
  - B2SX's session-recipe gate cells (`--drop P_Ball_Wood_MF 0
    --drop-settle 2 --ticks 40 --rounds 60 --max-depth 1 --seed 1
    --session-recipe`; the clients' whole Q binding here): L2 **60/60 PASS**
    (rWG2: round 57 refused on gap Q, 58 run), L13 **60/60 PASS** (rWG2:
    round 31 refused, 32 run) - the residual B2SX's gate named as gap Q's
    is gone; held 25 / 185 outside entries over 74 / 83 holds, 0 clamped,
    0 rebuild or hold failures.
  - Unchanged (the two other WG2 residuals, queued): the alive scene's
    50-round schedule fails exactly 144:4 and 87:9 on rRLQ and rWG2 (the G1
    membership leak); the bridge scene stops at its first round 47:45 on
    both ("no live controller 'sys:unknown' (priority 405)").
  - Gate round (the put-back verdict added, SimTool only): x64 BUILD-OK,
    GCC 30/30 ok; WG2's scene 90..209 and 90..203 PASS 50/50, the dead
    scene PASS 50/50, L2 / L13 PASS 60/60 (0 clamped, 0 hold failures),
    and the controls fail as before (`--savestate-no-hold` refused at round
    0, `--legacy-restore` 50/50, `--probe-scramble after` 50/50). The new
    verdict never fired here (nothing clamped); no run forces a clamp.
- **Not run, deferred:** a live session and the solo snapshots-off replay
  (no client path changed: the clients never set `hold_outside`, and the
  executed set is the same expression moved into a function; the wire
  check's 12 replays are byte-identical); a retail run (the retail mod
  only recompiled the refactored header).

## As built (step RLM, 2026-10-28): the G1 membership leak

Roadmap §4 item 4 (found at WG2). What was built, what the measurement
showed about the item's premise, and what stays queued.

- **The leak, as it really is.** `script_replay::reregister` (B2SW's G1)
  puts the owner of a script the restore wrote back active into the
  behaviour manager's pending table (`CKScene::Activate`: +1 in
  `m_BeObjectNextFrame`) and may set its scene flag. Only a live frame's
  `ManageObjectsActivity` consumes a pending add, and the replay's
  `execute_frame` calls `Execute` directly (no `ExecuteBehaviors`, so no
  `RemoveObjectNextFrame` when the script finishes). So the registration
  outlived what it was for: (1) a replay that ran the script to its end
  left the owner pending for the live frame after it, which in the straight
  run had dropped it a frame after the script ended; (2) a later restore
  of the same live frame (SimTool's chained rounds; a host frame with two
  rewinds) that wrote the script back finished still found the owner
  pending, so `owner_pending_add` admitted it in the replayed frames.
- **The fix, in the shared restore (`game/script_replay.hpp`, one copy for
  every world):** a registration ledger. `reregister` enters what it did
  (the script, its owner, whether the pending add and the scene flag are
  its own - the pending count is read before and after the `Activate`).
  `restore` (under `live_membership`) first takes back the registrations
  of the scripts it writes (`take_back_registrations`: the pending add
  decremented in the public table, never below what was there; the scene
  flag cleared only if the ledger set it), then registers again what it
  writes back active. `execute_frame` notes which registered scripts ran
  (`note_ran`) and at its end settles the ledger
  (`settle_registrations`): a registered script that did not run in this
  frame and is neither active nor to be activated gives its owner's
  pending add back - the live frame finds nothing to execute for the owner
  in the frame after the script's end and drops it, so after that frame
  the straight run's `owner_in_frame` is false too. The scene flag is left
  there: the script's own end (`CheckBehaviorActivity`) clears it as live.
  A script the restore does not write (the v14 rule's kept live state)
  keeps its registration. The ledger belongs to one live frame (the time
  manager's main tick count): a live frame in between consumed the pending
  adds and the scene flags are live state again, so the entries are
  dropped and nothing is taken back; a take-back only decrements a
  positive count, so a table a live frame cleared within the same tick
  count loses nothing that is not ours. Counters `membership().unregistered`
  and `settled`. No host change: the core hosts' binding already sets
  `live_membership` around the restore and the replayed frame.
- **The witness (SimTool `--savestate-check --frames`):** the straight pass
  records at each kept tick, per selected script, whether the next live
  frame would run its owner (`owner_in_frame`); each round compares its end
  with it and the summary prints `owner membership at round ends (step
  RLM): N of M round(s) differ`, with the ledger's counters. Reported, not
  judged (the replay does not run the live frame's list, gap F; a new
  verdict would change every earlier gate's meaning). Negative control
  `--savestate-no-ledger`: the ledger emptied before every rewind and
  every replayed frame, i.e. the restore before RLM.
- **Measured** (WG2 V4's alive scene: level 13, P_Modul_01 with its
  Pusher's probe, two balls, `PROXIMITY_UNION`, `--frames --ticks 120
  --max-depth 48`; `.local-tools/work/RLM/v1.sh`, `v2.sh`), the clients'
  recipe (`--session-recipe`), 50-round schedules: seeds 1/2/3 PASS 50/50
  on rRLQ and on rRLM (scripts, blobs, hashes). The witness: with the
  ledger **0/50, 0/50, 0/50** rounds differ (registrations 25/20/17, taken
  back by a restore 8/6/6, settled 17/14/11); with `--savestate-no-ledger`
  **42/50, 44/50, 41/50** differ, every one `P_Modul_01_MF Script 0 vs 1`
  (the straight pass has dropped the owner, the replay left it pending).
  The named pair 54:8,144:4 PASS 2/2 with and without the ledger (the
  explicit rounds with the session recipe re-make the live world at e with
  whole frames, which flush the table).
- **The item's premise was wrong for its measured case** (the WG2 note's
  cause): WG2's failing rounds (anchor 144 depth 4, anchor 87 depth 9:
  `flags 2147483648 vs 2147483649`, the probe's locals) run SimTool's
  default **v14 recipe** (no `--session-recipe`: `replay_activation`
  off), and they fail the same with depth 0 - `--savestate-rounds
  54:8,144:0` (and 70:0, 100:0, 120:0) fail identically with no replayed
  frame at all (`.local-tools/work/RLM/i2.sh`), so nothing ran the
  finished script. Cause: under the v14 rule a script that is inactive at
  the anchor is not written; it keeps its live state. SimTool chains its
  rounds without a live frame or a re-made live world (when no pass is in
  the window), so after a round ending at e' = 62 with the probe active,
  a round anchored at 144 > e' keeps the probe's state of 62 - a live
  state no host holds (a host's anchor is never after its live tick, and
  every core host binds the clients' recipe, which writes every selected
  script). Identical on rRLQ and rRLM with and without the ledger (50
  rounds: 2 failed, the same two); the witness shows those two rounds as
  the kept registration, correctly (the kept live script is active).
  Pre-existing, root-caused, a harness artefact of a recipe only the legacy
  retail path uses: queued in roadmap §4 item 4 with its options (not
  chosen here).
- **Checks (QUICK):** x64 BUILD-OK (twice: the shared change, then the
  SimTool control switch; `ninja -t deps` of session_client.cpp.obj
  VALID, 346 deps), x86 CLIENT-BUILD-OK (core_host, physics_session_client
  and the core sources rebuilt), unit tests 590/590, GCC syntax check 30/30
  ok. Wire check rRLQ -> rRLM over `core_ref_s9`: 12/12 sends SAME, 12/12
  journals IDENTICAL, `WIRE-CHECK: ALL SAME`. `core_ref_s9`'s solo1 /
  solo2 replayed from their captures with rRLMb
  (`.local-tools/work/RLM/solo_replay.sh`, REF9's recipe): both reach the
  end marker, sends SAME (2718, 2694), journals IDENTICAL under
  `e0_compare.py` (byte compare differs at the header's build id only).
  The script-difference lines at window ends: unchanged (the 14 replays'
  journals are identical).
- **Review fix-up: the pending add is the owner's.** `reregister` enters
  the pending add against the first script of an owner it re-registers; a
  second script of the same owner finds the owner already pending and
  enters none. The live frame keeps an owner while any of its scripts
  executes (`CKBeObject::ExecuteBehaviors` drops it only when none did),
  so `settle_registrations` holds an entry that carries the pending add
  while any script of its owner ran in the frame (`note_ran` marks the
  owner's entries too) or is active / to be activated (`owner_keeps`,
  over `GetScriptCount`/`GetScript`, scripts outside the selection
  included), and `take_back_registrations` keeps the pending add on the
  entry (taking back only the scene flag) when a script of the owner that
  this restore does not write is still active. Without it, an owner with
  two scripts written back active lost its membership when the first one
  ended in the replay, and the second never ran live again. The arena's
  module owners have one selected script each, so the measured numbers
  are unchanged; the gate round re-ran them on the fix-up build rRLMc
  (x64 BUILD-OK, x86 CLIENT-BUILD-OK, tests 590/590, GCC 30/30 ok, wire
  check rRLQ -> rRLMc `ALL SAME`, solo1/solo2 sends SAME and journals
  IDENTICAL; witness 0/50, 0/50, 0/50 with the same counters, control
  42/50, the pair 2/2, v14 seed 1 the same two rounds 144:4 and 87:9).
  The multi-script owner itself has no scene in the arena: it is fixed by
  the live rule, not measured.
- **Not run, deferred:** the live 4-client headless smoke session and a
  two-copy retail run (the Arch server would have to be rebuilt from this
  tree first, which with the session takes the gate past 15 minutes; the
  change is behaviour-identical on all 14 replayed captures, so a single
  live session adds no evidence the replays lack); a headless session with
  two rewinds in one host frame on a real capture (no capture holds one;
  SimTool's chained rounds are the clients' binding - `rollback_world`,
  `bind_scripts` with `live_membership` - without a live frame between
  them, which is that case).

## As built (step RLC, 2026-10-28): a constraint shut down inside the window

Roadmap §4 item 4 (found at WG2). The arena's P_Modul_29 bridge breaking
inside a savestate window (`Test "Ball_Stone"` -> `10 Hinges` input 2,
Shutdown of one `Set Physics Hinge`) made every restore to an anchor before
the break refuse: "savestate restore, step 12 (controllers), world damaged:
core of 'P_Modul_29_Platte06': no live controller 'sys:unknown' (priority
405)" - the blob names the hinge (`IVP_CP_CONSTRAINTS`) by (priority, name)
and the restore only matches recorded controllers to live ones.

- **Cause, in full.** A joint block's (Hinge, Ball Joint, Slider)
  `IVP_Constraint_Local` is made by its deferred callback, and `init()`
  turns the world-space anchor and axes into each core's space with the two
  bodies' poses of that moment (`m_Rfs_f_Rcs`, `m_Afs_f_Acs`, the mappings,
  the borders); nothing the blocks call changes those fields afterwards.
  The script record kept only whether the block held a handle (v2); a
  restore whose record had one and whose live block had none counted
  `joints_missing` ("nothing can make those") and left it, so the blob
  refused. No pose at the anchor computes the constraint again (the anchor
  is after its Create; the plates have swung about the hinge since), so
  "re-running the creation" cannot be exact; the constraint's own fields
  can.
- **The fix (no IVP change, no engine change, no protocol change; bridge
  API v27).** physics_RT's side (`physics_state.cpp`, `joint_block_get` /
  `joint_block_set`, reached through `ivp_private_access.hpp`'s
  explicit-instantiation `thief`): the image of a block's constraint - the
  two bodies' roles (target / Object2), the axis types, borders, factors,
  both anchors with their rotation matrices, the mappings and dims - and a
  constraint made on the block's two bodies through the block's own path
  (`CKIpionManager::CreateConstraint`, a default template naming the two
  objects, which decides only the cores it links) and given every field of
  an image. Transient fields stay out of the image: the FIXED bit
  `do_simulation_controller` sets and clears on a LIMITED axis every step
  and `matrix_size`, both recomputed before any use in the step; the
  restore writes them as `init()` leaves them. A constraint with a maximum
  impulse (none of the blocks makes one) is not imaged (fail-closed).
  The script replay's side (`game/script_replay.hpp`, one copy for every
  host): `script_replay_hooks` gains `capture_joint` / `restore_joint`
  (bound by `host_backend.hpp force_hooks_for` for the headless client, the
  SimTool, the server world and the retail core host); a LOCAL record's
  handle byte becomes 2 (`kJointImaged`) followed by the u32 FNV-1a hash of
  the image whenever the hook reads one, and the image goes into a
  per-process ledger (per block, the 8 most recently seen images). The
  portable record (the wire, the resync) keeps the 0 / 1 byte, so a
  localized record names no image and its restore takes the block's latest;
  the legacy path binds no joint hook, so its records stay byte for byte as
  before. The restore (`restore_joint`, before the blob, as a force block's
  controller): record had none, live has one -> the block's reset, as
  before; record had one, live has none -> made again from the recorded
  image (`joints_recreated`; no image -> `joints_missing`, the refusal of
  before); both have one with different images (a Shutdown and a Create
  inside the window made another) -> the block's reset, then made again
  from the recorded image (`joints_replaced`); `diff` reports two images as
  a logical difference ("joint image"). The blob then finds the constraint
  among the core's controllers and puts the units and the movement state
  back as recorded - but NOT, on its own, the order of the constraints
  among themselves (below).
- **The constraints' order (the review's finding, fixed in the step).** The
  blob names a controller by priority and name, and every constraint is
  ('sys:unknown', 405), so `order_controllers` matches the recorded ones to
  the live ones in live order: a constraint made again is appended to its
  cores' `controllers_of_core`, and when it was not the last of its key there
  the restored lists (and the simulation units the blob builds from them,
  `sim_unit_calc_redundants`) hold the constraints in another order than
  recorded - no refusal, another solve order. So the local record's
  `kJointImaged` entry also carries the constraint's two places (in the core
  lists of its anchor R's and anchor A's bodies, counted among the
  controllers of its key; `bmmo_physics_joint_image::order`, outside the
  image bytes and the hash), `diff` reports a changed place ("joint order"),
  and the restore, once every block is written and before the blob, reads
  the places of every imaged block's constraint and, when one differs from
  the record, puts them back in one call (`script_replay_hooks::order_joints`
  -> bridge `joint_blocks_order`: swaps among the places the key's
  controllers take in each list; a list holding such a controller no given
  block holds, or places that are not 0..n-1, refuse the restore,
  `order_failed`, nothing changed). The arena's bridge alone does not
  exercise it: its `10 Hinges` chain is made in one order and the stone's
  Shutdown reaches the LAST hinge made (block 47535), so the remade one is
  last in its lists anyway. `--joint-shuffle` (SimTool) exercises it: before
  every rewind every constraint a joint block holds is taken out by the
  block's reset and made again from its own image, highest block id first.
  Also from the review: the capture finds the block's constraint among its
  bodies' cores' controllers instead of dereferencing local 0 (a body a
  script Dephysicalizes deletes its constraint without the block knowing:
  no image then, the presence byte alone); the image carries the CK ids of
  the block's two bodies and `joint_block_set` refuses an image of others
  (the ledger is the process's: a block id a later level reuses never gets
  a stale image); and the image is read whole before anything is made.
- **Adapted:** the design named two roads (the script replay re-running the
  creation; physics_RT re-making the joint from recorded parameters). The
  first is not exact (the poses of the Create are gone); the parameters are
  recorded as the constraint's own fields in a process-local ledger rather
  than in the record, so neither the portable record nor a journal changes
  (no protocol number taken). Record versions are unchanged (the local
  record is this process's own).
- **Checks (QUICK, `.local-tools/work/RLC`; rRLC / rRLCb = this tree, rRLM =
  HEAD's build).** x64 BUILD-OK (`ninja -t deps` of session_client.cpp.obj
  VALID, 346), x86 CLIENT-BUILD-OK (core_host / physics_view / the bridge
  rebuilt; deps VALID, 1260), unit tests 590/590, GCC syntax check 30/30
  ok, plus `physics_state.cpp` under GCC 16 with the Arch tree's own flags
  (`gcc_state.sh`) ok.
  - The bridge window (WG2's scene: level 13, P_Modul_29, the stone on the
    plates, the paper held at the MF, `PROXIMITY_UNION`; `r0.sh`, `r2.sh`):
    anchor 47 depth 45 **PASS** (rRLM: refused, "no live controller
    'sys:unknown' (priority 405)"), the same with the bit clear PASS;
    `joints: recreated=1`. The 50-round schedule on the clients' recipe
    (`--session-recipe`): **PASS 50/50**, recreated=20, replaced=0,
    missing=0 (rRLM: refused at round 0). On SimTool's default v14 recipe
    42 of 50 pass; the 8 that fail (132:8, 115:15, 100:40, 144:4, 90:20,
    87:9, 91:44, 119:16) all fail `P_Modul_25 script: flags 2147483648 vs
    2147483649` and 132:8 and 100:40 pass alone - the queued v14 chain
    artefact of step RLM (a script inactive at the anchor keeps a later
    round's state), not a joint.
  - A constraint created inside the window: the arena with sector 2's
    activation inside the window (`--drop-sector 1 --second-move 2 260`,
    window 210..409, 50 rounds; the bridge's plates and hinges are born at
    the activation): **PASS 50/50** (7 rounds cross it, 84 bodies destroyed
    at a restore and made again by the replay, every one exact; rRLM PASS
    50/50 too) - the hinges made in the window go with their bodies at the
    restore and the replay makes them again with the same image (the
    records, hashes included, compare byte for byte).
  - Wire check rRLM -> rRLC over `core_ref_s9`: 12/12 sends SAME, 12/12
    journals IDENTICAL, `WIRE-CHECK: ALL SAME`. Solo snapshots-off replays
    (solo1 / solo2, rRLC): end marker reached, sends SAME (2718, 2694),
    journals IDENTICAL under `e0_compare.py` (bytes differ at the header's
    build id only).
  - Live, headless (one run, `live.sh`, label `rlc_live`): four rRLCb
    clients beamed onto P_Modul_29 in slot 13 (every ball Ball_Stone),
    `--latency-ms 150`, Arch server snapshots off (trafo inputs and
    placement on; the server binary not rebuilt - this step changes no
    server behaviour): the harness VERDICT PASS (server journal 3860/3860
    matched, 0 checkpoint mismatches). The bridge broke in every world;
    clients 2 and 3 rewound across their break (`joints: recreated=1` each)
    and every blob restore of every client was taken (`sv=32/0/0, 51/0/0,
    53/0/0, 51/0/0`: restored / refused / capture failures). The worlds
    disagree on WHEN the bridge breaks (the server and client 0 at ~741,
    client 1 ~775, clients 2 and 3 ~910; the four-ball pile on the plates
    from tick 742 on): the bridge's 4 m probe and its identity read run on
    the client's own ball, the G-c residual (world agreement, Q7 (b)), not
    this step.
  - Live, retail (one run, CD1's two-copy L11 gate, core on by default,
    snapshots off): both assigned, core=1, rollbacks 3 / 4 with every blob
    taken (`sv=3/0/0`, `sv=4/0/0`), no crash. L11 has no P_Modul_29, so the
    run shows the v27 bridge and the hooks load and capture on the x86
    plugin, not a recreated joint.
  - The gate round, after the order fix-up (`r3.sh`, one fix-up build
    copied to rRLC): x64 BUILD-OK (physics_world.cpp.obj deps VALID, 269),
    x86 CLIENT-BUILD-OK (core_host deps VALID, 1260), tests 590/590, GCC
    30/30 ok and `physics_state.cpp` under the Arch flags ok. The bridge
    window 47:45 PASS (recreated=1, reordered=0), the 50-round session
    recipe PASS 50/50 (recreated=20, reordered=0), the window across
    sector 2's activation PASS 50/50. `--joint-shuffle` (12 constraints
    made again before each of 20 rewinds, session recipe): **PASS 20/20**,
    22 core lists put back (recreated=8, order_failed=0); its control with
    `--joint-order-off`: **FAIL 20/20**, every round "P_Modul_29_MF Script /
    Set Physics Hinge: joint order 0/-1 vs 1/-1 (2 logical, 0 float)" at the
    window's end - the record now sees the order the blob cannot; on these
    windows no blob differed before it (0 float: the bridge's solve order
    left no bit changed there, which the order fix does not rely on). Wire
    check rRLM -> rRLC over `core_ref_s9`: `WIRE-CHECK: ALL SAME` (24 of 24
    SAME / IDENTICAL); solo1 / solo2 replays: sends SAME (2718, 2694),
    journals IDENTICAL (bytes differ at the build id only). No live run in
    the gate round (the step's two were spent in the implement round).
- **Not run, deferred:** a retail run on the arena's bridge (slot 13) that
  rewinds across a break (the retail core logs no joint counters; one
  retail run fitted the budget and L11's gate was the short one); the Arch
  server rebuild (no server behaviour changed; `physics_state.cpp` checked
  under GCC instead); a live run of the fix-up (the order pass runs in a
  live rewind only when a place differs; the step's two live runs were
  spent).

## As built (step F0, 2026-10-29): SimTool's chained rounds default to the clients' recipe (RLM-v14)

RLM queued it (harness only, the orchestrator's call: option (a)): WG2's
measured failures 144:4 and 87:9 were SimTool's default v14 recipe chaining
its rounds without a live frame, a state no core host holds. Now
`--savestate-check` binds the clients' recipe (`--session-recipe`) **by
default wherever the scene can bind it** - `--frames`, no
`--legacy-restore`, no trafo inputs (the recipe's own preconditions, which
already returned 2 when `--session-recipe` was asked without them) - and a
level without `Gameplay_SectorManager` falls back to v14 with a line;
`--v14` asks for the old recipe (parsed in `parse_union_arg`: `parse()`'s
else-if chain is at MSVC's nesting limit, C1061, as WG0/WG1 found). The
first output line says which recipe ran and why
(`savestate-check: recipe session (the clients') (default)`). The
physics-only matrix (`scripts/savestate_check.py`, no `--frames`) keeps v14
by that rule.

Measured on rF0's SimTool (`.local-tools/work/F0/rlm.sh`, WG2 V4's alive
scene of RLM's `v1.sh`: level 13, P_Modul_01 with its Pusher's probe, two
balls, `PROXIMITY_UNION`, `--frames --ticks 120 --max-depth 48`, 50 rounds,
seed 1): default **PASS 50/50**; `--session-recipe` PASS 50/50; `--v14`
**FAIL 2/50 - round 28 (anchor 144, depth 4) and round 45 (anchor 87,
depth 9)**, `P_Modul_01_MF Script flags 2147483648 vs 2147483649`, exactly
RLM's; a scene without `--frames` (`--spawn-test 4`) stays v14 and PASS
10/10.
The savestate matrix (`scripts/savestate_check.py`) changes with it: its
`frames` cells now run the clients' recipe (their physics cells stay v14);
`--quick` on rF0: 5/5 as expected, `arena-sector2.frames` PASS 12/12 on the
session recipe, the physics cells PASS 12/12 on v14, the `--legacy-restore`
negative control FAILS at round 0 as before.
