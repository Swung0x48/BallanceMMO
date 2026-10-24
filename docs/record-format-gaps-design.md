# Record-format gaps: the ActiveBall cell (F13), the physics globals per tick (F14), the tutorial's clock (G-T)

Status: design, revision 2 (after two reviews; §10 lists every finding and
what became of it). Nothing is built. Track C of the post-audit designs. It
covers audit items #13 and #14 of the rollback-coverage audit
(`scratchpad/audit/rollback-coverage-audit.md` §1, §2 F9/F10; the user's order
in §5 calls them F13/F14), and the gaps found while checking them, reported per
principle (d).

**What changed in revision 2.**

- **C1 (ball sensing) is withdrawn.** Track B's
  `docs/world-agreement-gaps-design.md` (WAG) designs the same thing: client
  proximity sensing (G-c) and the identity reads of `CurrentLevel[0][1]`
  (WAG G-c2, §1.1.4). WAG already uses the rule both reviews asked of C1: the
  candidate set of tick t is the ball part of the rollback record of t-1
  (WAG §2.2), with the replay hook before each replayed tick (WAG U3). One
  design per decision (`docs/session-client-core-design.md` §0), so C1 goes and
  this design builds on WAG. What C1 found that WAG does not have is handed to
  it in §4.5.
- **C2 no longer changes the record.** With WAG, no selected physics script
  reads `CurrentLevel[0][1]` in a client world. C2 becomes a structural census
  that proves it at every anchor (§2.2). No script record version is taken.
- **C3 covers the gravity too** and has no fallback (§2.3). The bridge version
  check makes a fallback unreachable (A21).
- **The pass criteria** exempt the session-start write, now measured (A18, A20).

**Numbers this design claims.** Bridge API **v22** (`physics_rt_api.h:23` is
v19; TRD claims v20, `docs/rollback-engine-gaps-design.md` (REG) v21). World
option **bit 3** `WORLD_SESSION_CLOCK` (SPD bit 0, TRD `WORLD_TRAFO_INPUTS`,
WAG bit 2). Protocol: the next free number at C4's commit (12 if WAG takes 11;
RSD Q5's rule). No script record version, no journal version of its own, no
engine change, no IVP change.

Base: HEAD 4882ec2 (R2) plus the uncommitted step P (the working tree). Line
numbers are the working tree's as of 2026-10-23; step P is still moving
`rollback.hpp`, `script_replay.hpp`, `session_host_support.hpp`,
`client_core*.cpp`, `session_client.cpp` and `core_host.cpp`. *Measured* facts
were taken with `build-agent/BallanceMMOServer/BallanceMMOSimTool.exe --root
C:/Users/geekerwan/Desktop/Ballance-Agent`; the raw outputs are in
`scratchpad/design2/trackC/` under the file names given per fact.

Path key:

| key | file |
|---|---|
| SRP | `BallanceMMOCommon/include/game/script_replay.hpp` |
| SHS | `BallanceMMOCommon/include/game/session_host_support.hpp` |
| RB | `BallanceMMOCommon/include/session/rollback.hpp` |
| RBB | `BallanceMMOCommon/include/session/rollback_binding.hpp` |
| CF | `BallanceMMOCommon/src/session/client_core_frame.cpp` |
| PS | `BallanceMMOCommon/src/physics/physics_state.cpp` |
| PSC_ | `BallanceMMOCommon/src/physics/physics_savestate_capture.cpp` |
| PSR | `BallanceMMOCommon/src/physics/physics_savestate_restore.cpp` |
| API | `BallanceMMOCommon/include/physics/physics_rt_api.h` |
| PV | `BallanceMMOClient/physics/physics_view.cpp` |
| PW | `BallanceMMOServer/sim/physics_world.cpp` (`.hpp`: PWH) |
| ST | `BallanceMMOServer/sim/sim_tool.cpp` |
| SC | `BallanceMMOServer/sim/session_client.cpp` (headless host, H) |
| CH | `BallanceMMOClient/session/core_host.cpp` (retail core host, RC) |
| PSC | `BallanceMMOClient/session/physics_session_client.cpp` (retail legacy, RL) |
| PRT | `submodule/Ballanced/Source/BuildingBlocks/physics_RT/` |
| TT | `submodule/Ballanced/Source/BuildingBlocks/TT_Toolbox_RT/Behaviors/` |
| RSD / TRD / SPD / WAG / REG | the resync, trafo-replication, sector-placement, world-agreement and rollback-engine-gaps designs in `docs/` |

## 1. The problems and the rules

### 1.1 The facts (measured unless cited)

| # | fact | value / where |
|---|---|---|
| A1 | readers of `CurrentLevel[0][1]` ("ActiveBall") through Get Cell | the server enumerates every Get Cell on CurrentLevel's ActiveBall column at its anchor (PW:428-519). Per level, P_Modul_18 / P_Modul_29: L1 0/0; L2 9/1; L3 3/1; L4 3/1; L5 2/0; L6 8/2; L7 4/2; L8 2/1; L9 5/1; L10 5/2; L11 5/1; L12 11/0; L13 (arena) 1/1 (`desc_tut_l1..13.txt`; WAG §1.1.2 has the same counts). Left on the array on every level: Ball_Shadow ×1, Event_handler ×1, Sound_Manager ×2, none selected |
| A2 | array blocks in the selected L13 scripts (`dumps13/`, the 61 dumps whose root is P_\*, PE_\* or Gameplay_SectorManager, minus P_Extra/PC_/PS_) | Get Cell column 4 (the phase) in every module's MF script; Get Cell column 2 (`Ball_Pos_Frame`) in every `*_Init` (load time); Get Cell column 1 in P_Modul_18_MF and P_Modul_29_MF (A1); Gameplay_SectorManager: Get Cell columns 1 and 2 (IngameParameter), Set Cell column 4, Iterator If; PE_Balloon (keep-local): Iterator, Get Cell columns 0 and 4; **P_Modul_18_MF's sound sub-graph: Row Search column 1, Get Cell column 0, Set Cell column 1 = 1 / 0 on a sound-slot array** (`desc_p18_l13.txt:200-262`, blocks #46555, #46565, #46576, #46602) |
| A3 | writers of `[0][1]` | Gameplay_Ingame: `Init Ingame/Init CurrentLevel` Set Cell #26062 (level start) and the Trafo Manager's `set new Ball` Set Cell #24850; Levelinit_build #36583 (load). No BMMO code writes it |
| A4 | P_Modul_18 (fan) inner sub-graph | 7 m probe on `P_Modul_18_Particle`, Use Outputs 5: out0 = In Range, out1 = Enter Range (TT ScaleableProximity.cpp:236-273). out1 → Get Cell #46392 (ActiveBall); out0 → Box Box Intersection (Entity 0 = `@Get Cell`, Entity 1 = `P_Modul_18_Kollisionsquader`) → Binary Switch → SetPhysicsForce Create / Shutdown (`dumps13/95.txt:13-33`). There is no Exit output: Create and Shutdown fire only on a check that is In Range |
| A5 | the probe's schedule | a block evaluates only every `Last Check` frames, 1..`Maximum Framedelay` by distance (TT ScaleableProximity.cpp:190-226); the counter is script state (local 1), recorded |
| A8 | trafo timing | Unphysicalize, a 1350 ms drag (89 ticks), a 1000 ms Delayer (66), the explosion, a 150 ms Delayer (10), `set new Ball` (TRD §1.5, :160) |
| A10 | Set Physics Globals (SPG) blocks | PRT `Behaviors/SetPhysicsGlobals.cpp:69-88` is the only caller of `CKIpionManager::SetTimeFactor` (scales by 0.001, PRT `CKIpionManager.cpp:861-866`) and calls `SetGravity` with it (PRT `CKIpionManager.cpp:874-878`). In the L13 scripts: Gameplay_Ingame ×1, Gameplay_Refresh ×1 (`dumps13/26.txt:15`), Gameplay_Tutorial ×7 (`dumps13/29.txt:36, 481, 635, 785, 788, 858, 862`), Event_handler ×3. Every block writes gravity `0,0,-20`. The tutorial's blocks exist on every level; the script is only activated on Level 1 |
| A11 | writes per level, free run (`BMMO_TRACE_TIMEFACTOR=1`) | L2, L5, L13: Gameplay_Ingame 2.0 only. L1: Ingame 2.0, Refresh 2.0, Tutorial 0, then Tutorial 2.0 at engine tick 2006 (`l1_free1.txt`, `tf_l*.txt`) |
| A12 | L1 session world (server) | anchor at engine tick 541; the dropped ball is frozen until drop tick 1633 and moves from 1634; no further write in 8000 ticks (`l1_drop.txt`, `l1_drop8k.txt`). Every savestate round whose current tick is ≥ 2 takes the frozen path (`ssc_early_l1_s1..3.txt`, 180/180), so Ingame, Refresh and the tutorial's 0 all write in the first two frames |
| A13 | the tutorial's unfreezes | `Tut continue/exit` SPG (2.0) fires on either of two Key blocks or on `wait for continue`, a 25 000 ms Timer. `Physic on/off` ×2: Wave Player Start → SPG 0 → Key 28 (Enter) → SPG 2.0; no timer. Chapters start on own-ball probes `Wait for ExtraLife/Rampe/SteinTrafo/Extrapoint/Checkpoint` (`audit/ds_Gameplay_Tutorial.txt:331-610, 2039-2737`) whose ObjectA is the ball entity, not `Ball_Pos_Frame` (`dump_tut_l1.txt:254-296`) |
| A14 | Gameplay_Refresh `Tutorial freeze/unfreeze` | on every activation of Gameplay_Refresh (level start, the pause menu's resume chain): if `CurrentLevel[0][0] == 1` it writes 0 when Gameplay_Tutorial's `freeze?` is TRUE, else 2.0 (`dump_refresh_l1.txt`) |
| A15 | the F14 failure, offline | `--savestate-check --frames --level 1 --drop P_Ball_Wood_MF 0 --drop-settle 1600 --ticks 100 --rounds 30 --max-depth 40 --seed 7`: **23/30 FAIL**, each anchored at or before 1661 and failing at "restore identity: clock - physics_delta_time 0 vs 0x1.f07c2p-6" (`ssc_l1_unfreeze.txt`). `--drop-settle 1700`: 30/30 PASS (`ssc_l1_after.txt`) |
| A16 | the replay step | `step_physics` calls `CKIpionManager::Simulate` (PS:835-843). Simulate runs in PostProcess, after the scripts (PRT `CKIpionManager.cpp:306-308`), and recomputes `m_PhysicsDeltaTime = m_DeltaTime * m_PhysicsTimeFactor` first (`SetDeltaTime`, PRT `CKIpionManager.cpp:855-859`) |
| A17 | what a record holds of the globals | the blob holds `gravity`, `time_factor` and `physics_delta_time` (PSC_:786-789). The restore writes neither factor nor gravity: the factor stays live and the delta is recomputed from it (PSR:641-660); the gravity is compared, and a strict restore fails on it while a lenient one notes it (PSR:891-896). The per-body path's `tick_state` has no globals (RB:1651-1678) |
| A18 | **the session-start write** (new) | `--savestate-check --frames --drop P_Ball_Wood_MF 0 --drop-settle 0 --ticks 40 --rounds 60 --max-depth 1`, seeds 1-3: on L2 and L13 the round anchored at 1 with depth 1 fails "the environment's gravity differs from the savestate's" (`ssc_early_l2_s1..3.txt`, `ssc_early_l13_s2.txt`). The world starts with gravity (0, -9.81, 0) (PRT `CKIpionManager.cpp:698-699`) and factor 0.001 (PS:862); Init Ingame's SPG writes both **in frame 2** of the server world |
| A19 | sessions restore leniently | the rollback binding is lenient unless asked (RBB:178); SimTool's check is strict (ST:1842). A live rewind across A18's write therefore replays the anchor's ticks under the live gravity and factor |
| A20 | **the reference captures do not cross A18** (new) | in all 16 `core_ref_e0` client logs the first rewind is anchored at tick 220 (a6m, tick base 17) or 230 (the others, tick base 27): ≥ 193 ticks after the first record |
| A21 | **no backend lacks a bridge entry** (new) | PV:106-111 refuses a physics_RT whose `api_version` is not exactly `BMMO_PHYSICS_API_VERSION`; H, the server and SimTool link `physics_direct` |
| A22 | **SimTool's server-world replay does not re-place the proximity frames** (new) | `--savestate-check --frames --level 13 --drop P_Ball_Wood_MF 0 --ticks 40 --rounds 60 --max-depth 20 --seed 1`: `--drop-settle 30` 14/60 FAIL (P_Modul_01_MF's proximity `Current Distance`, its script's active flag); `--drop-settle 10` 53/60 FAIL (P_Modul_18_MF `Current Distance`, P_Modul_18_Particle Script flags) (`ssc_early_l13_settle30.txt`, `ssc_early_l13_settle10.txt`). The straight pass moves the frames in `world->tick` (PW:929-930); a replayed tick runs `execute_frame` only (ST:1807-1830), so the probes measure the frame where the straight pass left it |
| A23 | **the start window has other failures** (new, §9 Q5) | with `--drop-settle 0` on L2 and L13, besides A18: "restore identity: world - objects 43 48" (L2) / "53 58" (L13) for anchors 4-6, and "scripts: Gameplay_SectorManager: flags 2147483648 vs 2147483649" at depth 0 for anchors 14-32 (`ssc_early_l13_s1.txt`, `ssc_l2_early.txt`) |

### 1.2 F13: the ActiveBall cell is not recorded

**Symptom.** None observed. P_Modul_18 and P_Modul_29 read the cell in replayed
frames (A1, A4). The record carries `CurrentLevel[0][4]` and
`IngameParameter[0][1..2]` only (SRP:382-402), so a replayed frame reads the
cell's live value. A replay anchored before a trafo's `set new Ball` (A3, A8)
reads the new ball in frames where the live run read the old one.

**Cause.** The cell's only in-session writer, the Trafo Manager, is in
Gameplay_Ingame, which is not replayed, and the record has no copy of it.

**What closes it.** WAG rewires both readers in every world to private copies
that every live and replayed tick re-derives from the candidate set of the
record of t-1 (body kind) or restores from the record's hold section (by-name
kind) (WAG §2.2, §4.1-4.2, §2.9 "audit #13 / F9"). After WAG G3/G4 no selected
physics script reads `CurrentLevel[0][1]` on any client, and the gap is closed
for physics by construction. What remains is to **prove** it on every level and
to keep it proved as levels and selections change (§2.2), and one non-physics
residue the census found (A2: the fan's sound-slot array, §9 Q4).

With WAG's bit clear (the negative control and old journals), the two reads
stay on the live cell. That path is the old behaviour by definition and is not
a session configuration (`physics_proximity_union` defaults on from WAG G3).

**Invariant I-C.** In every client world with WAG's bit set, every array read of
a selected, non-keep-local script in a replayed frame t reads the value the live
frame t read: either a recorded cell (restored at the anchor and written by
replayed scripts only), a private copy WAG re-derives or restores, or a
load-time constant.

### 1.3 F14: the physics globals are kept live and the replay is blind to them

**Symptom (A15, A18).** A rewind whose window contains a change of the time
factor or the gravity restores a world whose physics delta (and gravity) is the
live one, not the anchor's, and steps every tick before the change with the
live values:

- after the L1 tutorial's unfreeze U, a rewind anchored inside the freeze steps
  each frozen tick at two PSIs; the IVP clock and every awake body end
  2·(U-1-A) PSIs ahead of the straight run (23/30 rounds fail, A15);
- on every level, a rewind anchored at tick 1 steps tick 2 under the in-play
  gravity and factor where the live step 2 used (0, -9.81, 0) and 1.0, and a
  strict restore refuses the blob (A18).

**Cause.** The blob carries the factor, the gravity and the physics delta
(A17), but the restore deliberately writes neither the factor nor the gravity:
the 9.x fix for a blob that put 1.0 back for good
(`docs/ivp-savestate-design.md` "time factor ... left LIVE"; PSR:644-656). The
replay then steps every tick with whatever is live (A16), and no replayed
script writes them (A10: the writers are Gameplay_*, Event_handler). The
per-body path has no copy at all (A17). The frozen check reads only the live
factor (RB:693, :1100-1110).

**Invariant I-F.** Every replayed tick t is stepped at the factor and gravity
the live step t used; the restored world at the anchor has the anchor's factor,
physics delta and gravity; after the replay the world has the live values. The
blob still never writes them.

### 1.4 G-T: the tutorial freezes one world at a time

**Symptom (code, A12-A14).** In a Level 1 session:

- **The server's world** freezes in the first two frames and stays frozen until
  the 25 s timer, 1656 ticks during which a dropped ball does not move (A12).
  It never freezes again: its tutorial chapters watch the parked retail ball,
  which does not move (A13; `mirror_clone_to_retail` is false in the session
  world, PWH:82).
- **A client's world** freezes at the same tick. It unfreezes when its player
  presses Enter, or at the timer (A13). Then every chapter whose spot the own
  ball reaches (ExtraLife, Rampe, SteinTrafo, Extrapoint, Checkpoint) freezes
  it again until Enter, with no timer.
- **A client frozen while the server runs** takes the frozen path at every
  rewind (RB:1100-1110): the history is dropped and only a resync re-anchors it
  (`docs/session-client-core-design.md:765`). Its mirrors and mechanisms stand
  still while the server's move. **The reverse:** a client that presses Enter
  before the timer runs while the server is frozen.
- **The pause menu's resume chain** re-activates Gameplay_Refresh, which writes
  0 again while the tutorial's `freeze?` is set (A14), in that client only.

Not observed in a run: no Level 1 session exists in `maps/arena-test` (grep of
every log). The 9.x text "both ends freeze together"
(`docs/collision-overhaul-design.md:20, :333`) holds only until the first key
press. WAG §9 Q5 found the same and handed it to F14's owner.

**Cause.** Gameplay_Tutorial is own-player flow (own-ball probes, own
keyboard) that writes world state every player shares. Principle (a) forbids
it. IVP has one clock per world, so no per-player version exists.

**Invariant I-T.** In a session, the time factor and gravity of every world are
a function of the session's lockstep inputs alone. On every level they are:
the anchor's 1.0 and (0, -9.81, 0) (PS:862, A18), then Init Ingame's 2.0 and
`0,0,-20` from frame 2 on. The pause chain is already neutralized on retail
(RC: CH:843-848, :964 re-applied each frame; RL: PSC:507-560), and `reset
Level` writes the same values.

The writers, complete (A10-A14):

| script / block | writes | when | session rule |
|---|---|---|---|
| Gameplay_Ingame / Init Ingame | 2.0 | level start, restart | unchanged: the in-play value |
| Gameplay_Refresh / Tutorial freeze/unfreeze | 0 or 2.0 (L1 only) | every Refresh activation | held (§2.4) |
| Gameplay_Tutorial / Init | 0 | L1 tutorial start | held |
| Gameplay_Tutorial / MoveKeys | 2.0 | MoveKeys chapter | held |
| Gameplay_Tutorial / Tut continue/exit | 2.0 | Enter, exit key or 25 s timer | held |
| Gameplay_Tutorial / Physic on/off (two graphs) | 0, then 2.0 | chapter start, Enter | held |
| Event_handler / Pause Level, Unpause Level | 0, 2.0 | ESC, resume (retail only) | written to the factor in use (`pause_clock_apply`), unchanged |
| Event_handler / reset Level | 2.0 | restart (9.28) | unchanged |

The hold finds the blocks by prototype and root script, never by id.

## 2. Design

### 2.1 Parts

| part | closes | where | worlds |
|---|---|---|---|
| ~~C1 ball sensing~~ | G-c, G-c2 | withdrawn: WAG parts A-E | |
| **C2 array-read census** | proves I-C (F13) | SRP (`array_reads`); SV/H/RC anchor log and counter; SimTool `--array-reads` | server, H, RC |
| **C3 globals per tick** | F14 | RB (records, restore, replay, stats); RBB hooks; bridge v22 `get/set_physics_globals`; `physics_direct`, `physics_view` | H, RC, RL, SimTool |
| **C4 session clock hold** | G-T | `game/session_clock.hpp` (new, shared host support); PW, SC, CH call it | server, H, RC |

C3 changes only how a world replays its own history, so it is always on. C4
changes what a world does live, so it is a world option (§5). C2 changes
nothing and logs.

### 2.2 C2: the array-read census (F13)

**`script_replay::array_reads(CKContext*, const selection&)`** (SRP, static,
public CK API only). For every block of every selected script that is not
`keep_local` (RSD §2.4), recursively, it lists each input parameter of type
`CKPGUID_DATAARRAY` whose real source resolves to a `CKDataArray`, with:

- the block's prototype name and walk position (the RSD §2.4 key);
- the array's name, and the column when the block's column input is a constant
  (Get Cell, Set Cell, Row Search, Iterator If); otherwise "any";
- the access (read, write, both: Set Cell, Add Row, Remove Row are writes).

Each entry is then classified:

| class | rule |
|---|---|
| recorded | (array, column) is in the record's cell table (SRP:395-402) |
| private | the array is one of WAG's `BMMO_Ball_*` copies or `checkpoint_phase`'s copy |
| load-time | the block's root is a `*_Init` script (A2: column 2, `Ball_Pos_Frame`) |
| sound | the block's outputs reach only sound blocks (Wave Player, TT ProximityVolumeControl, PlaySound) within its graph: the walk follows output links to the graph's end |
| **unrecorded** | anything else |

**Placement.** At the anchor in every world, after WAG's rewire and SPD's
placement: PW next to the checkpoint split (PW:262), SC after the selection
(SC:1529), CH after `checkpoint_phase_resolve` (CH:863). One log line with the
counts per class and every unrecorded entry by name; a counter
`unrecorded_array_reads` in the core's status. The server runs it on the
mechanism class (WAG `probe_class::mechanism`), which is what the clients
replay.

**Rule.** `unrecorded` must be 0 with WAG's bit set. An unrecorded entry is a
gap to fix per (d), by recording the cell or rewiring the read, not a log line
to live with. `sound` entries are reported separately (§9 Q4).

### 2.3 C3: the physics globals per tick (F14)

**Bridge v22** (API). One struct and two entries:

```
typedef struct bmmo_physics_globals {
    float time_factor;     /* the engine value: m_PhysicsTimeFactor (0.002 in play) */
    float gravity[3];      /* IVP_Environment::get_gravity(), as float */
} bmmo_physics_globals;
int32_t (*get_physics_globals)(void* ipion_manager, bmmo_physics_globals* out, char* error, uint32_t error_size);
int32_t (*set_physics_globals)(void* ipion_manager, const bmmo_physics_globals* in, char* error, uint32_t error_size);
```

- `get` reads `m_PhysicsTimeFactor` (public, PRT `CKIpionManager.h:291`) and
  `GetGravity` (PRT `CKIpionManager.cpp:868-872`).
- `set` writes `m_PhysicsTimeFactor = in->time_factor` directly (not through
  `SetTimeFactor`, whose ×0.001 would round), recomputes `m_PhysicsDeltaTime =
  m_DeltaTime * m_PhysicsTimeFactor` as `SetDeltaTime` does, and calls
  `SetGravity` only when the gravity differs. `SetGravity` is the live SPG's
  own call; `IVP_Environment::set_gravity` writes the vector, its length and the
  standard gravity controller's vector and nothing else
  (`ivp_environment.cxx:409-414`). The gravity floats come from floats on the
  live path (SPG's VxVector), so the round trip is exact.
- It fails without a manager or environment.

This is BMMO bridge code over public fields and members: no engine change, no
IVP change. `physics_direct` (server, H, SimTool) and `physics_view` (RC, RL)
gain the two methods. Every backend has them (A21), so there is no degraded
mode.

**Hooks and record** (RB, RBB):

```
// rollback_world (RB:74-)
std::function<bool(bmmo_physics_globals& out)> get_globals;
std::function<bool(const bmmo_physics_globals& in)> set_globals;
// tick_state (RB:1651-1678)
bmmo_physics_globals globals{};
bool have_globals = false;
// RBB, beside `simulating`: both bound to the backend's new methods
```

`record()` fills `globals` where it reads the clock (RB:1819). That is after
the frame's scripts and step, so it holds the values step t used: the writers
are scripts (A10), the retail clock guard pins in PreSimulate (API:407-423),
and nothing writes them after Simulate in a frame (A16). The blob holds the
same values (A17), but only on the savestate path and only after a parse; 16
bytes in `tick_state` serve both paths and every replayed tick.

**Restore and replay** (`restore_and_replay`, RB:670-):

1. At entry, next to the frozen decision (RB:693): `live = get_globals()`. The
   frozen decision is unchanged.
2. **Before the restore** (before `set_savestate`, RB:937, and before the
   per-body path's `rewind_clock`, RB:848-851), when a replay follows: if the
   anchor's record has globals that differ from the current ones,
   `set_globals(anchor)`. The blob's gravity compare (PSR:891-896) then
   passes, and PSR:660 recomputes the delta from the anchor's factor, so the
   restored world equals the record (A15's and A18's failure points).
3. For each replayed t, after `pre_step(t)` and `replay_remotes` and before
   `step()` (RB:1233-1278): if the record of t read at the top of the iteration
   (RB:1148) has globals that differ from the current ones, set them. A tick
   with no record or no globals keeps the current ones (`globals_unknown`).
4. After the loop, including the `break` on a failed step: if the current
   globals differ from `live`, `set_globals(live)`.
5. The re-captured record of t reads what step 3 set, so a later rewind sees
   the same history.
6. A failed `set_globals` is handled like the frozen path: `globals_failures`,
   `invalidate_history()`, return; the caller re-anchors through a resync, the
   existing recovery (RB:1100-1110). This is the only degradation, and it needs
   a broken environment.

The replay's `simulating` check (RBB:227-231) and the clock guard's sampling
branch (PS:2416-2419) see the set factor and write nothing. The guard's pause
sensor never fires in a session (`docs/collision-overhaul-design.md:260`).

**Stats** (`rollback_stats`, RB:357-): `globals_set` (writes),
`globals_replays` (replays that wrote at least once), `globals_replays_start`
(those anchored before the history's first globals change),
`globals_unknown`, `globals_failures`. The engine keeps `first_change_tick_`,
the first record whose globals differ from the record before it since the
history began; `invalidate_history` clears it. The core's status line prints
the five next to `frozen`. No journal record is added (§5).

### 2.4 C4: the session clock hold (G-T)

**Option (a), recommended: hold the tutorial's writes at the in-play value.**
`bmmo::game::session_clock_hold` (new, `game/session_clock.hpp`, the pattern of
`checkpoint_phase.hpp`):

- **`apply(context)`, at the anchor**, in every world with
  `WORLD_SESSION_CLOCK`, after the checkpoint phase split (PW:262, SC:1529,
  CH:863):
  - It reads the in-play value from Gameplay_Ingame's Init Ingame SPG input 1
    (2.0 on every level, A10). Without it the hold refuses and logs.
  - It creates one private `CKParameterLocal` `BMMO_SessionTimeFactor` with
    that value.
  - It redirects input 1 ("Physic Time Factor") of every SPG block (prototype
    GUID `(0x72af347c, 0x3da71e1)`, PRT `SetPhysicsGlobals.cpp`) inside the
    root scripts Gameplay_Tutorial and Gameplay_Refresh to that parameter with
    `SetDirectSource`, remembering the direct and shared sources (WAG §2.3
    rewire step 1).
  - Expected: 7 + 1 blocks wherever Gameplay_Tutorial is loaded, which is
    every level (A10). Another count is logged with the blocks found, which are
    still held. Gravity (input 0) is untouched: `0,0,-20` in every block, the
    in-play gravity.
- **`keep(context)`** after a level reset (9.28): the targets are re-checked
  and re-held, as `checkpoint_phase_.keep` does (SC:989, CH:1385).
- **`restore(context)`** at session end and level exit (SC:1398, CH:1059): the
  sources are put back.

Every tutorial step still runs: texts, arrows, keys, `freeze?`,
`IngameParameter[0][3]`. None of it touches a selected script: the only
selected reader of IngameParameter is Gameplay_SectorManager, columns 1 and 2
(A2). Only the factor writes become writes of the in-play value, as the pause
chain's became in 9.18. No SetValue is used: the redirect never writes a
parameter another block reads (the propagation trap of
`script-restore-parameter-propagation`).

The late joiner needs nothing extra if its anchor precedes the tutorial's first
write, as A12 shows for the server world. §7.3 H3 checks it for H and RC.

**The core's part.** The core keeps `world_options()` (SPD §2.4); the hosts call
`apply` at their anchor when the bit is set and `restore` at the end, as for
SPD's placement and WAG's rewire.

**Option (b): lockstep the freeze.** Any player's chapter or key would freeze
every world, and the server would need each player's tutorial state.
Rejected.

**Option (c): deactivate Gameplay_Tutorial in sessions.** Deterministic and
simpler, but the tutorial's texts and arrows disappear for Level 1 players
(§9 Q1).

**Without C4.** C3 still makes every replay exact across a freeze; what remains
is G-T itself (worlds frozen at different ticks, the frozen path and its
resyncs on L1).

## 3. Determinism

- **C2** reads the graph and logs; it writes nothing.
- **C3.**
  - A replayed step at the recorded factor runs `Simulate` with the live step's
    `m_PhysicsDeltaTime`: `m_DeltaTime` comes back with the blob (PSR:653) and
    is averaged by `SetDeltaTime` as live (A16), and the factor is the
    recorded one. The PSI count per step is the live one, so the IVP clock and
    every core advance identically. The gravity reaches the standard gravity
    controller exactly as the live SPG's call did.
  - The values are the world's own: they never cross worlds. Two worlds whose
    globals agree live (C4) replay them identically.
  - A15 and A18 are the checks.
- **C4.** The held value is the Init Ingame input, read by structure, the same
  in every world. After the hold, the globals of every world are the anchor's
  values, then Init Ingame's write in frame 2: both at lockstep ticks.

## 4. Rollback interplay

### 4.1 Order inside a replayed tick

`find(t)` (RB:1148), the births and rechecks (RB:1182-1222), WAG's
`place_probes(t)`, `pre_step(t)` (deferred births, re-issued activations,
`execute_frame`), `replay_remotes`, **`set_globals(t)` (C3)**, `step()`, the
birth re-pose, the mechanism additions, the re-record.

At the restore: **`set_globals(anchor)` (C3)**, then the blob or the per-body
restore, the scripts' restore.

### 4.2 Records, renumbering, resync

- C3 lives in `tick_state`; a re-assign invalidates it with the history
  (RSD §2.6.1), and `first_change_tick_` with it.
- A resync's re-step (RSD A3) is a `restore_and_replay` from a synthetic
  anchor whose ticks have no local records: they keep the current globals and
  count `globals_unknown`. Under C4 the only change after the anchor is A18's
  write in frame 2, so the re-step is exact whenever S > 2. If RSD's import
  carries the server's blob at S, the synthetic anchor takes its globals from
  it (A17); otherwise a resync with S ≤ 2 is the one unknown case, checked in
  §7.3 H4.

### 4.3 The savestate

- The blob's rule is unchanged: factor and gravity are compared, not written
  (PSR:641-660, :891-896). C3 writes them from the engine, before the blob.
- `--savestate-check`'s restore identity (A15) and its strict gravity compare
  (A18) become the regressions for C3.

### 4.4 The legacy retail path (RL)

C3: RL shares RB and RBB, so it gets C3 when its binding passes the hooks
(PSC:3074, :3130). C4: a SessionStart with `WORLD_SESSION_CLOCK` ends the legacy
session locally with "the session clock needs the session core", as SPD does
for its option (SPD:444). R7 deletes the path.

### 4.5 Handed to WAG (from withdrawn C1)

- **The fan's force without an Exit output** (A4). With Use Outputs 5, a probe
  whose frame is moved out of range stops reporting In Range, so neither Create
  nor Shutdown fires: a force created on ball A **stays on A** after A leaves
  the box if, meanwhile, the frame went to a ball out of range. On entry, a
  ball whose 3-D-nearer rival is out of X,Z range never gets Enter and is never
  pushed. WAG's metric (G-c5) fixes both; WAG H2 should include them:
  - (i) entry: A high in the fan column (inside 7 m X,Z, inside
    `P_Modul_18_Kollisionsquader`'s Y extent), B beside the fan at the
    particle's height, X,Z 7.2-7.5 m, so that B is nearer in 3-D. Pass: A is
    pushed; old rule: it is not.
  - (ii) exit: A pushed; B arrives as in (i); A leaves the box. Pass: the force
    shuts down; old rule: it stays on A.
  - If the box's Y extent above the particle is under B's X,Z distance, the
    case cannot occur on the fan; the arm then runs on a 50 m wake probe
    (WAG V2). The box extent is read at setup (`--dump-entity`).
- **SimTool's replay coverage** (A22). WAG V4's binding of `place_probes` into
  SimTool's rollback binding is what makes A22's arena commands pass; they are
  ready-made checks: pass = 0 failures of the "Current Distance" and module
  flag kinds at settle 10 and 30.

## 5. Wire, protocol and journal

| item | change |
|---|---|
| `PHYSICS_PROTOCOL_VERSION` (entity/session.hpp:51) | the next free number at C4's commit (12 if WAG takes 11). Comment: "N: world option SESSION_CLOCK (the tutorial's time-factor writes held at the in-play value)". C2 and C3 need no bump |
| world options | `WORLD_SESSION_CLOCK = 1u << 3`; server config `physics_session_clock`, default true from C4's step. Carried by `session_start_msg::world_options`; SPD defines it as `uint32_t` after `protocol` (SPD:774), TRD as a byte after `spawn_spacing` (TRD:1106) — the two must be reconciled by whichever lands first (§9 Q3); this design needs only bit 3 of it |
| journal | no record and no version of its own. The header's `world_options` (SPD's journal v4) carries the bit; if C4 lands before any `world_options` exists, it adds the field and takes the next journal version (as WAG §5 states for itself). SimTool `--replay-session` runs the hold when the header has the bit. **No NOTE per live globals change**; C3's counters go to the status line only |
| script record | none |
| bridge | API v22: `bmmo_physics_globals`, `get_physics_globals`, `set_physics_globals`; `docs/engine-changes.md` bridge note (no engine change number) |

## 6. What must not change

- **The blob never writes the factor or the gravity** (PSR:641-660,
  :891-896). The arena run that went to half rate
  (`docs/ivp-savestate-design.md:384-386`) stays fixed.
- **The frozen path** (live factor 0, RB:693, :1100-1110). With C4 on it never
  fires in a session (§7.3 H2); it stays the rule for a stopped live clock.
- **The record's cells**, their restore, the live-cells guard and RSD §4.6's
  adopt rules: C2 changes none of them.
- **The pause chain neutralization** (CH:843-848, :964; PSC:507-560) and
  `reset Level`.
- **Live play everywhere**: C3 acts only inside `restore_and_replay`. Every
  live step, record and journal byte is unchanged by C3.
- **Levels and sessions without a window across a globals change**: no
  `set_globals` call, `globals_replays = 0`.
- **The 16 reference captures** (`core_ref_e0`) under C2 and C3: none crosses
  A18 (A20), so journals byte-identical except header and build id. C4's step
  changes the protocol, so its check is the arena regression (§7.3 H5), not the
  captures.
- **Gravity values**, **IVP** and **engine**: no change.
- **The server never depends on snapshots**: nothing here reads one.

## 7. Verification

### 7.1 Unit tests (`BallanceMMOMessageTests`)

`rollback_engine_test`: the fake world's `step` advances `clock += kDt * factor`
and moves bodies by `factor` and `gravity` (RB test :236-258); `set_globals`
records each call.

- **U1, a frozen window then a rewind.** Factor 0 for ticks 10..19, 2 from 20.
  At 25, rewind to 15.
  - Pass: the factor in effect at the restore is 0 and at each step 16..19 is
    0, at 20..25 is 2; after the replay it is 2. `set_globals` is called twice
    (0 before the restore, 2 before step 20). The clock and every body equal
    the straight run bit for bit. `globals_replays = 1`.
  - Control: with `set_globals` bound to a no-op, the clock ends 8·kDt ahead
    (2·(U-1-A), U = 20, A = 15), pinning the old failure.
- **U2, gravity.** Gravity g0 at tick 1, g1 from 2; rewind at 10 to 1. Pass:
  g0 before the restore and at no replayed step, g1 before step 2; bodies equal;
  `globals_replays_start = 1`.
- **U3, live factor 0.** The frozen path runs, no `set_globals` call, history
  dropped (as today).
- **U4, a failing setter.** `set_globals` returns false at the restore: no
  replay, `globals_failures = 1`, history dropped.
- **U5, re-record.** After U1, a second rewind at 25 to 17. Pass: the
  re-captured records of 16..25 carry the same globals as U1's live ones;
  `set_globals` is called with 0 before the restore and with 2 before step 20,
  and nowhere else; clock and bodies equal the straight run.

`rollback_binding_test`:

- **U6.** `get_globals` / `set_globals` map to the backend's new methods.
  `SimulatingFollowsTheLocalClock` (:398-409) is unchanged.

C2 needs a CK context, which the unit build does not have; its checks are
SimTool S2.

`session_messages_test`, `session_journal_test`:

- **U7.** `WORLD_SESSION_CLOCK` round-trips; a v3 journal reads as options 0.

### 7.2 SimTool (offline)

- **S1, F14 across the tutorial's unfreeze (A15).**
  `--level 1 --savestate-check --frames --drop P_Ball_Wood_MF 0 --drop-settle
  1600 --ticks 100 --rounds 30 --max-depth 40 --seed 7`, without C4 (SimTool's
  world options default off).
  - Today 23/30 FAIL. Pass: 30/30; the same with `--rounds 300` and seeds 1-5;
    the control at `--drop-settle 1700` stays 30/30.
- **S1b, F14 at the session start (A18).** `--level 2` and `--level 13`,
  `--drop-settle 0 --ticks 40 --rounds 60 --max-depth 1`, seeds 1-3.
  - Pass: no "gravity differs", no clock identity failure at any anchor.
  - The other failures of this window (A23) are not C3's; they are reported in
    §9 Q5 and must be closed by their owner. The check therefore counts only
    the globals and clock kinds.
- **S2, C2 census.** New read-only mode `--array-reads [--client-scope]` on
  L1-L13 with WAG's bit (after WAG G3).
  - Pass: `unrecorded = 0` on every level; `sound` lists P_Modul_18's slot
    blocks (A2) on every level with a fan; with WAG's bit clear the unrecorded
    list is exactly the A1 counts of Get Cell column 1 (the control).
- **S3, C4.** `--level 1 --drop P_Ball_Wood_MF 0` with `--world-options
  SESSION_CLOCK` (new flag) and `BMMO_TRACE_TIMEFACTOR=1`.
  - Pass: every tutorial and Refresh write traces as 2; the dropped ball moves
    from its drop on. Off: A12 reproduced (frozen until 1633).
- **S4, C3 + C4 on L1.** S1's command with `--world-options SESSION_CLOCK`
  across the ticks the tutorial would have frozen: 30/30, `globals_replays = 0`
  for every round anchored after frame 2.
- **S5, the tool for A18's tick.** New `--globals-trace` in the drop and
  sector scenes: one line per world tick with factor and gravity. Pass: on
  L1-L13 the only change after the anchor is in frame 2 with C4 on.

### 7.3 Headless harness (same-batch old/new, `--latency-ms 150`)

- **H1, F14 in a live session (L1, C4 off, no keys).** Both worlds unfreeze at
  the timer U. `--force-rollback` at U+10 with depth 20 (anchor inside the
  freeze).
  - Pass: the client's IVP clock (`ivp_time` in its status) at U+100 equals the
    server's world clock at U+100; the journals' TICK records of the own ball
    equal the unforced same-batch arm. Old: ahead by 2·(U-1-A) PSIs.
- **H2, G-T (L1, 2 clients, C4 on).** Client 1 is beamed to the ExtraLife
  chapter spot at tick 2000 and held there 300 ticks.
  - Old (C4 off): client 1 freezes; `frozen` grows and a resync follows, while
    the server and client 0 run.
  - Pass: `frozen = 0` everywhere, no resync, `globals_replays −
    globals_replays_start = 0` on every client; client 1's ball and the
    mechanisms equal the server's within prediction (`first_divergence.py`
    clean on mechanisms).
- **H3, late joiner on L1 (C4 on).** The lp1_join recipe on level 1 with
  `BMMO_TRACE_TIMEFACTOR=1` on the joiner. Pass: the joiner never traces a 0
  write; its `--globals-trace`-equivalent status shows factor 0.002 from its
  frame 2 on.
- **H4, the start window live.** L2 and the arena, 2 clients, `--force-rollback`
  at base+20 with depth 19 (anchor at the first record).
  - Log (not journal) line per client: "globals: first change at tick T"; this
    measures where H's frame-2 write sits relative to its records.
  - Pass: `globals_replays_start = 1`, no "differ from the live record" at the
    replay's end, journals equal to the unforced same-batch arm. Old: the
    end-of-replay savestate difference appears when T > base.
- **H5, regression.** The 10 recent arena recipes with C4's bit set: journals
  equal to the pre-change build except the header; `globals_replays = 0` on
  every client. The 16 captures (§7.5) for C2 and C3.

### 7.4 Retail runs (core on, against the server of the step's commit)

- **R1 (L1, two clients).** The players press Enter at different times; one
  opens ESC during a chapter. Pass: no freeze in any world, `frozen = 0`, no
  resync with reason frozen; the tutorial texts still advance on Enter.
- **R2 (any level, one retail).** A forced rewind anchored at the first record
  (H4's recipe on RC). Pass as H4.

### 7.5 The 16 captures (C2 and C3 steps)

`wire_check.sh OLD NEW TAG` on `core_ref_e0`. Pass: WIRE-CHECK ALL SAME;
every replay reaches its end marker; journals byte-identical except header
version and build id (`e1_bytecheck.py`); `globals_replays = 0` in every
replay's status (A20).

## 8. Steps

The user's order puts F13/F14 after "F5, F6, F7, G-c, join-order message"
(audit §5). C2 depends on WAG G3/G4; C3 depends on nothing; C4 is G-T, which
the order does not place yet (§9 Q2: proposed with F14).

| step | content | closing check |
|---|---|---|
| **K0** (read-only tools, any time) | SimTool `--array-reads`, `--globals-trace`, `--world-options` | S2 control (bit clear) lists A1's reads; S5 reproduces A18 |
| **K1 = C3** (F14 slot) | bridge v22 (`physics_direct`, `physics_view`); RBB hooks; RB record/restore/replay/stats; core status line; `docs/engine-changes.md` bridge note | U1-U6; S1 30/30 (and seeds 1-5, 300 rounds); S1b; H1; H4; R2; §7.5 |
| **K2 = C2** (F13 slot, after WAG G3/G4) | `script_replay::array_reads`; anchor log + counter in PW, SC, CH | S2 pass on L1-L13; §7.5 |
| **K3 = C4** (G-T, slot per §9 Q2) | `game/session_clock.hpp`; PW, SC, CH; the bit, config, protocol; RL ends locally; the 9.x text corrected (`docs/collision-overhaul-design.md:20, :333`) | U7; S3, S4; H2, H3, H5; R1 |

Each step: same-batch old/new A/B per `docs/session-client-core-design.md`
§5.3, build from Bash (memory: build-tree pitfalls), implement → review → gate.

## 9. Open questions

**Q1 (user). Hold the tutorial's factor (option a) or switch the tutorial off
(c)?** Both satisfy I-T. (a) keeps the texts, arrows and the chapters' key
waits with the world running; (c) removes the tutorial in sessions.
- Settle: R1 with both, judged by the user.

**Q2 (user). Where does G-T go in the order?** It is new and not in audit §5.
- Proposal: K3 right after K1, in the F14 slot (it is F14's principal finding
  and K1's H1 needs C4 switchable).

**Q3 (user / SPD, TRD owners). Which `world_options` field?** SPD: `uint32_t`
after `protocol` (SPD:774); TRD: a byte after `spawn_spacing` (TRD:1106).
WAG takes bit 2, this design bit 3. A byte holds 8 bits; the u32 more.
- Settle: whichever of S5/T-step lands first defines it; the other design's
  step reads that field. Recommendation: SPD's `uint32_t` (room for the four
  bits already claimed and more).

**Q4 (user). The fan's sound-slot array written in replays (A2).**
- Symptom (code, not observed): P_Modul_18_MF's sound sub-graph claims a slot
  (Row Search column 1 = 0, Set Cell column 1 = 1) and frees it at the sound's
  end (Set Cell 0, link delay 2). The array is neither recorded nor private,
  and the graph is replayed: a replay crossing a slot claim re-runs Row Search
  on a slot table the live frame already changed and claims another slot. No
  body is involved (the graph ends in Wave Player and TT ProximityVolumeControl).
- Cause: the record carries only the three physics cells (SRP:395-402).
- Options: (a) record every cell a selected script writes (a record section;
  takes the next free script record version after TRD's v3 and REG's v4);
  (b) mute array writes during `execute_frame` for arrays outside the record
  (a write filter, sound-only blocks); (c) accept as non-physics.
- Settle: a forced-rewind arm at a fan on L2 counting free slots in the array
  before and after (SimTool `--dump-array` at the window's end vs the straight
  run). If they differ, (a) or (b) per the user; (d) says fix, so (c) needs the
  user's explicit call.

**Q5 (RSD owner, user informed). The session-start window's other failures
(A23).**
- Symptom (measured): with the window starting at tick 1, rounds anchored at
  4-6 fail with 5 fewer objects in the restored world than in the record, and
  depth-0 rounds at 14-32 fail on Gameplay_SectorManager's active flag.
- Likely cause (not proven): Init Ingame's sector-1 activation from frame 2
  (CF:389-395, `record_pending_activation`) creates sector 1's bodies around
  tick 5, and SimTool's binding does not record the pending activation the way
  the clients' platform does; the depth-0 case suggests a record captured
  before the flag is set. Existing scenes use `--drop-settle 30` and never see
  this window.
- Settle: `--drop-settle 0` rounds with `--verbose` and `--describe-script
  Gameplay_SectorManager` at ticks 13-15; then the same window on H with a
  forced rewind at base+5 depth 4. If H shows it, it is a session gap in the
  start window for RSD's sector timing track; if only SimTool does, it is a
  harness gap (ST:1843-1869 lacks the activation hook).

**Q6 (user, from review 2). WAG narrows the user's G-c decision.** RSD §11
records "each client rewires its TT Scaleable Proximity blocks like the
server". WAG P3 keeps flames, extras, the balloon and the tutorial on the own
ball in client worlds, for gameplay reasons this design agreed with in
revision 1. The narrowing should be confirmed by the user; it is WAG's
decision to carry.

## 10. Review

Two reviews of revision 1. Each finding, checked against the working tree:

| finding | verdict | what changed |
|---|---|---|
| R1-1 / R2-2: the replay senses today's roster, not R(t) | confirmed (RB:1148-1153: the record of t names the set step t was fed with; `remotes_` holds only current state) | C1 withdrawn; WAG's candidate set is the record of t-1 (WAG §2.2, U2-U3), which is the requested fix |
| R1-2 / R2-1: `after_step` before the birth re-pose | confirmed (RB:1278-1326 re-poses after the step) | moot with C1; WAG reads positions before iteration t, after t-1's re-pose |
| R1-3 / R2-5: script record v3 already claimed | confirmed (TRD:942, :1191, :1260; REG claims v4) | C2 no longer touches the record |
| R1-4 / R2-11: NOTE records and `factor_set = 0` contradict the start write | confirmed; measured: the write is in frame 2 (A18) and no capture crosses it (A20) | no NOTE per live change; `globals_replays_start` separates the start window; criteria rewritten (§6, §7) |
| R1-5: S4(i) cannot pass (Box Box) | confirmed (A4) | moot for this design; the corrected entry/exit arms handed to WAG H2 (§4.5) |
| R1-6: SimTool does not re-run sensing in a replay | confirmed and measured (A22) | handed to WAG V4 with ready-made commands (§4.5) |
| R1-7: "no record carries the factor" is wrong | confirmed (PSC_:786-789) | cause restated (§1.3); `tick_state` kept for the per-body path and per-tick access, with the reason |
| R1-8 / R2-9: block count | confirmed (`dumps13/29.txt`, `26.txt`) | 7 + 1 everywhere (§2.4) |
| R1-9 / R2-10: U1 asserted calls the design does not make | confirmed | tests assert the values in effect (§7.1) |
| R1-10: world options field conflict | confirmed (SPD:774 vs TRD:1106) | §5 and Q3 |
| R2-3: own entity read before the drain | confirmed in principle | moot with C1 (WAG uses the record, built after the drain, CF:525-536) |
| R2-4: the `record(t-1)` injection drifts | confirmed | moot: no injection |
| R2-6: the fallback is unreachable | confirmed (PV:106-111) | fallback removed; the setter's failure invalidates the history (§2.3 step 6) |
| R2-7: steps not in the user's order | confirmed | §8 slots; G-T asked (Q2) |
| R2-8: departure from the recorded G-c decision | confirmed | C1 withdrawn; the same narrowing in WAG is put to the user (Q6) |
| R2-12: "made stone" had no mechanism | confirmed | H1 of revision 1 withdrawn with C1 |

Nothing in either review was refuted. Revision 1's other claims that the
reviews checked and found correct (A4's output mapping, PW:386-413 and
:529-558, the cell section at the record's tail, the clock guard's sampling,
A15) are kept as they were.

## 11. Decisions (user, 2026-10-24)

- **Q1.** Option (a): the tutorial's time-factor writes are held at Init
  Ingame's value; the tutorial is shown as usual.
- **Q2.** G-T lands right after C3, in the F14 slot.
- **Q3.** The `world_options` field is the sector-placement design's.
- **Q4.** The fan's sound-slot array is not physics; the written cells are
  recorded with the other cells (no filtering), so a replay plays the same
  sounds; no check compares it.
