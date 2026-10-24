# Rollback engine gaps: the PreSimulate queue, lives that end inside the window, the retail beam's generation

Status: design, revision 2, 2026-10-23. Nothing here is built. Revision 2
answers two reviews; §10 lists every point and what became of it.

Scope: three gaps of the rollback coverage audit
(`scratchpad/audit/rollback-coverage-audit.md`, the "audit"):

| audit gap | fix | short name here |
|---|---|---|
| #6 | F5 | **Q**: the PreSimulate callback queue is never rewound |
| #7 | F6 | **L**: a body whose life ends inside the window is missing from the replay |
| #8 | F7 | **B**: the retail own beam bumps the generation when it is requested |

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
  record-format design also names v21 nominally).
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
  - This is consistent with the own ball's absence over 1355-1368.
  - The proximity measures 3D entity 52384 against 46336
    (`audit/d_P_Modul_18_MF_Script.txt`, block 46429), so the "unrecorded CK
    matrix" class of audit #8 is not excluded. Step G0 settles it.
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
  respawn 1567) that lies in a window. My count of windows over those births
  and the three beam stamps does not reproduce the totals exactly: one of
  f13's 11 is unexplained, and f131 is one short. Step G0 attributes them
  with the plan trace before G1 is judged.
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
  For the one known producer that effect is:
  - SetPhysicsForce 26432: a zero-force controller (retail navigation leaves
    are zeroed, audit §3). §7.2 L-S3 measures the body states;
  - WakeUp 26418: a pending WakeUp runs when its body appears, i.e. on a
    ball made in that frame. Whether that ball is already simulated, so that
    the wake changes nothing, is checked by G0 (the own ball's `simulated` at
    its birth record in lp19_a6_drop, with and without the entry), not
    assumed.

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
  did live. G0 confirms the target with `--dump-script`.

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
| Q-S2 outside entries | the same with Gameplay_Ingame's WakeUp 26418 and force 26432 queued against an unphysicalized own ball (`--drop-player-at` + a new `--ball-off <from>:<to>`), `--hold-outside` | no restore refused; no outside entry runs inside a replay; the queue after each round, order included, equals the straight run's; one controller per force block; the own ball's `simulated` at its re-birth equals the straight run's (the WakeUp fact of 2.2.4) |
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

### 7.4 Retail (Level 13 arena with `r2_keys.bmrc`, core on, Arch server of the step's commit)

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
