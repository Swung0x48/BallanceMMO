# Record-format gaps: the ActiveBall cell (F13), the physics globals per tick (F14), the tutorial's clock (G-T)

Status: design, revision 2 (after two reviews; §10 lists every finding and
what became of it). Steps K0 (49e2cc8), K1 (C3, step RK1) and K3 (C4 as
option (c), step RK3, with the frame-9 hold of step RK3H) and K2 (C2 with
§11 Q4's written cells, step RK2; its `PH[*][4]` read waits for the user)
are built ("As built" notes at the end). Track C of the post-audit designs. It
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
engine change, no IVP change. **As landed elsewhere (2026-10-27):** bridge
v20 went to resync A3, v21 to trafo T1, v22 to trafo T4, and protocol 10 /
11 to resync A3 / trafo T2; K1 takes the next free bridge version and C4 the
next free protocol number at their commits (this text's v22 and 12 are
nominal).

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
| A20 | **the reference captures do not cross A18** (new) | in all 16 `core_ref_e0` client logs the first rewind is anchored at tick 220 (a6m, tick base 17) or 230 (the others, tick base 27): ≥ 193 ticks after the first record (the E0 set, now `core_ref_e0_pre_q`; the 2026-10-24 re-record in `core_ref_e0` has the same anchors) |
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
| **K3 = C4** (G-T, right after K1 per §11 Q2) | **option (c) as decided (§11 Q1)**, not §2.4's hold: on Level 1 every world (server, headless, retail core host) activates the tutorial's exit IO (`Kapitel Aktion` / `Tut continue/exit`, the main branch's `edit_Gameplay_Tutorial`) 10 frames after the level start, counted in frames, from shared host support; PW, SC, CH; the bit, config, protocol; RL ends locally; the 9.x text corrected (`docs/collision-overhaul-design.md:20, :333`) | U7; S3, S4; H2, H3, H5; R1 (written for the hold; K3 re-reads each for (c)) |

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

- **Q1.** Option (c), superseding an earlier (a): on Level 1 a session
  switches the tutorial off, the way the main branch's multiplayer restart
  does (BallanceMMOClient.cpp at 22530b5, lines 489-492: `edit_Gameplay_Tutorial`
  finds, inside `Kapitel Aktion` / `Tut continue/exit`, the block before its
  `Set Physics Globals` and keeps its output 0 as `tutorial_exit_event_`;
  at level start it activates that IO, which runs the tutorial's exit path,
  including the 2.0 unfreeze). In a physics session every world does the
  same at its anchor, from shared host support (the structural find is CK
  code; the server, the headless client and the retail host call it before
  the anchor reads the factor), so the tutorial never freezes any world and
  G-T's hold of the tutorial's writes is not needed. Gameplay_Refresh's
  `Tutorial freeze/unfreeze` then sees `freeze?` false and writes 2.0 (A14),
  which is the in-play value. The texts and arrows of Level 1 are not shown
  in sessions. The main branch activates the exit from a 10-frame timer
  after the level start (`m_bml->AddTimer(CKDWORD(10), ...)`, line 484
  there); the user says that delay is necessary. In a session the exit is
  therefore activated 10 frames after the level start in every world,
  counted in frames (not wall time), the same frame everywhere.
- **Q2.** G-T lands right after C3, in the F14 slot.
- **Q3.** The `world_options` field is the sector-placement design's.
- **Q4.** The fan's sound-slot array is not physics; the written cells are
  recorded with the other cells (no filtering), so a replay plays the same
  sounds; no check compares it.

## As built (step K0, 2026-10-25)

Read-only SimTool tools; no simulation result, record, wire or shared file
changes. Files: `BallanceMMOServer/sim/array_census.hpp` (new, SimTool-local)
and `BallanceMMOServer/sim/sim_tool.cpp` (the parked trafo T3, 02bbcf6, does
not change it; the additions are still kept to new functions and a new option
block - `parse_record_format_arg`, the `--globals-trace` functions and
`globals_trace_guard` before `scene_tick`, `resolve_record_format_switches`,
`run_array_reads` - plus one-line hooks in `parse()`, `parse_placement_arg`,
`create_session_world`, `scene_tick`, `run_drop`, `run_savestate_check` and
`main`).

**`--array-reads [--client-scope] [--array-reads-all]`** (§2.2, S2). The census
of `--level`'s session world at its anchor, nothing ticked; parameters are
read with `update = FALSE`, so no operation runs. Adaptations:

- The census is in a SimTool header, not yet `script_replay::array_reads`: K0
  touches no shared file. K2 moves it into SRP for the hosts' anchor log.
- **Scope.** The session world is the server's, whose ball identity reads are
  already private copies (`physics_world::rewire_ball_identity_reads`).
  `--client-scope` is S2's control: a read of a `*_BMMO_Ball_N` copy is
  classified as a read of the array it copies, which is what a client world
  (WAG's bit clear) reads; the line names the copy.
- **Level tables** (not in §2.2): a read of an array whose every writer block
  in the whole composition is in a load-time script (`Levelinit_*`,
  `*_Init`) is `load-time` ("table"). Writers are counted per constant
  column, and only the column-confined writers (`Set Cell`, `Column
  Transform`, `Column Operation`) keep their column: a row- or column-moving
  writer (`Sort Rows` on its sorting column, Add / Remove / Move / Swap Rows,
  Shuffle, Clear, ...) counts for every column. A constant row or column is
  an `int` local (by parameter type) that its script never writes. Without
  this class, the sector manager's `Get Key Row` on `PH_Groups` and
  P_Modul_18's sound `Get Cell` column 0 (only `P_Modul_18_Init` writes it)
  would be unrecorded.
- **Unwritten** (not in §2.2): a read of an array, or of a constant column of
  it, that no block of the composition writes is its own class, `unwritten`,
  counted in the closing line (`unwritten=N`): the level file's data, unless a
  native writer (a CK manager, host C++) the census cannot see writes it. K2
  confirms the class (no native writer) before it counts as load-time. Today
  it is the sector manager's `Get Key Row` on `Physicalize_Convex` and
  `Physicalize_Balls`, 2 per level.
- **Whole-row readers** (`Iterator`, `Iterator If`, `Get Key Row`, `Get Row`,
  `Get Nearest`, `Get Highest`, `Get Lowest`) output every column of the row
  (`WriteOutputParameters`), so their column is `*` (the key column is noted).
- **Sound.** A read is sound when its links and parameter flow reach a sound
  block and only sound blocks, blocks on the same array and flow blocks
  (Identity, Binary Switch, Op, Nop, Delayer, Test, Sequencer, Parameter
  Selector, Switch On Parameter), without leaving the start block's graph; a
  write is sound when every census read of its array is sound and its own
  flow meets nothing else (the end-of-sound `Set Cell` reaches no block).
  Leaving is conservative: a link to a graph's own output (the start's graph
  or a nested one: not followed further), a value written into the graph's
  exported output or into a parameter owned outside the graph, and a reader
  or operation anywhere else in the root script (shortcuts) all make the read
  not sound. A consumer nested in a subgraph has its links followed in its own
  parent graph.
- The `*_Init` scripts are not in the selection (their owners are not `P_*`),
  so A2's column-2 `Ball_Pos_Frame` reads do not appear.

Results (L1-L13, `census_all.sh`; selected + keep-local = RSD's 12, 31, 22,
17, 21, 28, 35, 31, 43, 45, 45, 45, 23; the review fix-up's stricter sound
walk, writer columns and `unwritten` class changed no unrecorded or sound
count, only moved the two `Physicalize_*` reads from load-time to unwritten):

| level | client scope: unrecorded (P_Modul_18 / P_Modul_29 `Get Cell` `CurrentLevel[0][1]`, + manager PH) | server scope: private / unrecorded | sound |
|---|---|---|---|
| 1 | 0 / 0 + 2 | 0 / 2 | 0 |
| 2 | 9 / 1 + 2 | 10 / 2 | 27 |
| 3 | 3 / 1 + 2 | 4 / 2 | 9 |
| 4 | 3 / 1 + 2 | 4 / 2 | 9 |
| 5 | 2 / 0 + 2 | 2 / 2 | 6 |
| 6 | 8 / 2 + 2 | 10 / 2 | 24 |
| 7 | 4 / 2 + 2 | 6 / 2 | 12 |
| 8 | 2 / 1 + 2 | 3 / 2 | 6 |
| 9 | 5 / 1 + 2 | 6 / 2 | 15 |
| 10 | 5 / 2 + 2 | 7 / 2 | 15 |
| 11 | 5 / 1 + 2 | 6 / 2 | 15 |
| 12 | 11 / 0 + 2 | 11 / 2 | 33 |
| 13 | 1 / 1 + 2 | 2 / 2 | 3 |

The control's `CurrentLevel[0][1]` reads are exactly A1's counts on every
level. `sound` is three blocks per fan (`Row Search` column 1 and both
`Set Cell` column 1 of `P_Modul18_ActiveSounds`), on every level with a fan.

**Finding: the sector manager's PH read (not in A1/A2's classes).** Both
`Iterator If` blocks of Gameplay_SectorManager (Deactivate Sector, Activate
Sector) output whole `PH` rows; every `activate Type 1/2/3` starts with
`Test(Set? == TRUE)` on column 4. Column 4 has two in-play writers:
`P_Extra_Point_MF Script`'s `Update Array` (an `Iterator If` on column 3 = its
own object, then `Set Cell` column 4 = FALSE, at the extra's pickup) and
Event_handler's `reset Level / Reset PH-Array` (every row TRUE). So only an
extra point's row changes in play, and only its non-physics `activate Type 1`
(Set World Matrix, Show, Activate Script of the extra) depends on it: the read
is unrecorded but not a physics read. It stays `unrecorded` in the census (2
per level, both scopes), so §2.2's rule (unrecorded = 0 with WAG's bit) cannot
hold until K2 decides it: record `PH[*][4]` with the cells, or a class for
"only non-physics rows are written in play" with the writers proven
structurally. Queued for K2.

**`--globals-trace`** (S5). In the drop scene (`--drop`) and the sector scene
(`--savestate-check` without `--drop` / `--spawn-test`): a line at the
anchor and after every scene tick with the factor, the physics delta and
the gravity (`CKIpionManager::GetGravity`), each change marked and summed up.
The world's tick hashes are identical with and without it (L2, 150 ticks) and
equal to rEG0's SimTool. Results, L1-L13 (L1-4, 9, 12, 13 drop scene; L5-8,
10, 11 sector scene; `globals_all.sh`, `globals_sector.sh`): one change after
the anchor on L2-L13 and the same change on L1, at world tick 1 (frame 2):
factor 0.001 -> 0.002, gravity (0, -9.81, 0) -> **(0, -20, 0)**, A18
reproduced. L1 has a second change, the tutorial's freeze: Gameplay_Ingame
writes 2.0 at tick 1, Gameplay_Refresh 2.0 at tick 2 (no change),
Gameplay_Tutorial **0 at tick 8** (frame 9; S5's pass on L1 needs C4, K3).
The trace's summary is printed on every way out of the scene, a failed setup
included.
Corrections to the text above: the SPG gravity is `0,-20,0`, not `0,0,-20`
(§1.4, A10); the tutorial's 0 is not in the first two frames (A12's
inference), which K3's 10-frame exit timer (§11 Q1) must land after.

**`--world-options BITS`** (a `physics_world` mode). A u32 number or names
joined by `,`/`|`: `PLACEMENT_CLONES` (bit 0, the sector placement's; it sets
`--placement on`, no constant exists before S5; an explicit `--placement`
that disagrees is refused) and `TRAFO_INPUTS` (bit 1). Only the world honours
bit 1, so it is accepted only with `--array-reads` and with `--drop` without
the `--trafo-*` sets: the savestate, portable-scripts, resync, late-run and
late-sector checks make their own script selection without it, and the drop
scene's trafo sets would build a second registry for the same players.
Bits 2 (WAG) and 3 (this design's `SESSION_CLOCK`, K3) are refused as not
built; so are `--world-options` on a journal replay (the header carries
them), in the modes without a session world, and a number above 32 bits.

## As built (step B2SW, 2026-10-27): Q5 / A23 decided - a harness gap

- **Session or harness: the harness.** The settle of §9 Q5, on the headless
  client: a solo session (`maps/arena-test/b2sw_q5h`, arena, snapshots off)
  with `--force-rollback 1:4` - a depth-4 rewind at every tick from the
  first frame (tick base 8; the first rewind "to tick 8", re-simulating 4
  ticks), which covers Init Ingame's sector-1 activation (the module scripts
  told to reset at 9, 10 and 14) and its births - against `core_ref_s7/solo1`
  (`e0_compare --live`): every LOCAL checkpoint's fields equal at every tick
  (`.local-tools/work/B2SW/ckdiff.py`: 1182/1182, only the body ORDER of 276
  from 1820 on, the bodies a replayed sector pass makes again), births
  recreated 12/12 exact by tick 63. The clients' recipe - the activations
  recorded every frame and re-issued, the window's births destroyed at a
  restore and made again, every selected script written back - handles the
  start window. SimTool's `--savestate-check` without a `--second-move`
  binds none of it (a plain savestate binding, `replay_activation` off), so
  anchors 4-6 met the sector-1 bodies born after them ("objects 43 48" /
  "53 58") and the depth-0 rounds 14-32 a manager restored from a record
  the replay did not activate again.
- **The harness fixed, as far as its own recipe goes.** `--session-recipe`
  (sim_tool.cpp) binds the S7 path's session recipe without a
  `--second-move` (the window that starts at the session start). With it,
  `--drop-settle 2` / `3`, 60 rounds, depth <= 1, seed 1 (build rB2SWb):
  L2 4 of 58 and 3 of 60 rounds failed (without it 48 and 47 of 60); L13 3
  of 32 and 2 of 58 (without it 2 of 6 and 7 of 15 before a salvaged world
  stopped the run); the two A23 kinds are gone (no "objects" mismatch, no
  manager active flag).
  What remains, each with its owner:
  - `--drop-settle 0` (t0 = tick 1): the live-world re-make restores t0,
    whose blob is refused - "the environment's gravity differs" - A18, the
    session-start write of gravity and time factor (C3 of this design,
    queued), so the round's premise fails at 6 and 14;
  - from tick 31-34: "the PreSimulate queue differs" - rollback-engine gap
    Q (roadmap §4 item 4, step EGQ);
  - **not root-caused here (root-caused and closed at step B2SX, below):**
    depth-0 rounds at anchors 4-5, right after the
    harness re-made the round's live world from t0 = 3: "Gameplay_
    SectorManager / Deactivate Sector: outputs 0 vs 1". The re-made world
    (t0's record restored, then the straight pass's live frames re-run to e)
    passes its blob premise but its manager's Deactivate Sector output
    differs from the straight pass's record; whether the re-make or the
    restore leaves it is not established. H (above) shows no such effect in
    a client (positions; the client journal carries no script state).
  Q5 is therefore decided (a harness gap, with the client exact) and its
  harness fix built, with one residual open (roadmap §4 item 2).

## As built (step B2SX, 2026-10-27): Q5's residual - the restore's re-armed delayed link, ignored at depth 0

- **The cause** (the orchestrator's investigation, `.local-tools/work/Q5R/`).
  Gameplay_SectorManager has one delayed link, `Deactivate Sector.out0 ->
  Set Cell.in0`, delay 4, in flight at ticks 3-5 of the straight pass
  (countdown 3/4, 2/4, 1/4; it fires at 6), its source output recorded off
  at every tick (the capture takes the output as it stands,
  `script_replay.hpp` `put_activation`; the graph switched it off when it
  queued the link). When the record says a link is in flight, the restore
  re-arms it: the countdown set to the record's and the source output
  turned back on, so the next frame queues the link again
  (`restore_behavior`, counted in `last_restore().links_rearmed`). A depth-0
  round of `--savestate-check` runs no frame between the rewind and its
  script compare, so it sees that output on against the record's off:
  "Deactivate Sector: outputs 0 vs 1". 14 of 14 depth-0 rounds at an
  in-flight anchor failed (12 seeds, L2 and L13); every depth >= 1 round
  and every depth-0 round at 6 or later passed; the live-world re-make was
  ruled out. Not a session gap: a client never compares its scripts before
  the next frame.
- **The fix, the user's option (a)** (2026-10-27): the harness's depth-0
  script compare leaves out exactly the output bits the restore turned
  back on and compares everything else; the restore and the record format
  are unchanged. `script_replay.hpp`: `last_rearmed_outputs()` (the
  (block, output) pairs `links_rearmed` counts, cleared at every restore),
  `rearmed_output_masks()`, and an optional `masks` argument of `diff`
  (`any` is then whether anything else differs). `sim_tool.cpp`: the
  depth-0 compare passes the masks; the summary counts the rounds that were
  equal but for those bits.
- **The case the investigation left untested**: a restore that re-arms
  while the live world also has the link in flight. SimTool
  `--savestate-rounds T:D[:F],...` runs named rounds (anchor tick, depth,
  then F live frames compared with the straight pass's blobs and scripts:
  the follow); a named round's live world is always made again at its own
  e (the scripts the binding does not rewind - the Gameplay_* chains - then
  stand at e like the selection, as in a client, whose rewind always ends at
  its live tick). Every round printed the live link in flight before its
  rewind (countdown 1/4 at e = 5, 2/4 at 4, 3/4 at 3), so every re-arm met
  a link already in the graph's list (Ballanced CK2 keeps a link in that
  list once, `CKBL_OLD_IN_DELAYED_LIST`, and leaves a running countdown
  alone). L2 and L13 `--drop-settle 2` (`3:2:3,4:1:0,5:0:3,4:1:0,3:0:5,
  5:0:3,3:1:4,4:1:0,4:0:4`) and L2 `--drop-settle 3`: 9, 9 and 6 rounds,
  0 failed; every follow tick's blob and scripts equal the straight pass's;
  the link's destination input on at 6 only, in every follow that crossed
  6 (6 of 22 follow ticks, 6 of 22, 4 of 12) - it fires once, at 6. That
  holds for Ballanced CK2 (the headless worlds, SimTool, the server); the
  retail Virtools `CK2.dll`'s handling of a re-armed link already in its
  list was not checked (the re-arm itself predates this step): a retail
  follow check is on the final verification's deferred list. A first
  version chained a named round's live world from the previous round's
  end: L13's round 8 (anchor 4, depth 0, the live world at 5 from round 7's
  replay) then failed at 5 on `P_Modul_18_MF Script`'s
  DEACTIVATENEXTFRAME - the Gameplay_* scripts, never rewound, were a tick
  ahead of the selection (a world no client has); with the world made at e
  the same round passes.
- **The gate** (`rB2SX`, `--session-recipe`, `--ticks 40 --rounds 60
  --max-depth 1 --seed 1`): L2 `--drop-settle 2` 1 of 58 rounds failed, `3`
  1 of 60; L13 `2` 1 of 32, `3` 1 of 58 - each the named owner gap Q ("the
  PreSimulate queue differs", ticks 31-34; the L13 `2` run stops there, a
  salvaged world, as before); the depth-0 compares equal but for the
  re-armed bit: 3, 2, 2, 1 rounds - exactly B2SW's residual rounds.
  `--drop-settle 0` keeps A18 (C3).

## As built (step RK1, 2026-10-29): K1 = C3, the physics globals per tick

C3 as §2.3 and the K1 row of §8 say, with the adaptations below. No
protocol, journal or script-record change; no engine change, no IVP change.

- **Bridge v28, not v22** (the header's §0 note: v20-v27 went to A3, T1,
  T4, B7EG, BALA, SPGH, OWG, RLC). `bmmo_physics_globals` and the two
  entries are appended at the end of `bmmo_physics_api_v2`
  (`physics_rt_api.h`); `physics_state.cpp` `get_physics_globals` /
  `set_physics_globals` (the factor written directly, `m_PhysicsDeltaTime =
  m_DeltaTime * m_PhysicsTimeFactor` without `SetDeltaTime`'s averaging,
  `IVP_Environment::set_gravity` only when the vector differs);
  `physics_rt_bridge.cpp`, `physics_direct.hpp`, `physics_view.hpp/.cpp`.
  Bridge note in `docs/engine-changes.md` (no number).
- **The gravity is IVP's double vector, not `float gravity[3]`.** The start
  gravity (0, -9.81, 0) is set from doubles (PRT `CKIpionManager.cpp:698-699`)
  and is not a float; a float round trip would put another vector back at an
  anchor before the session-start write. Doubles are exact for both the start
  value and every SPG value (floats). The blob still compares it as float
  (PSR:954-959), which the double write satisfies.
- **The binding's switch.** `rollback_binding_options::physics_globals`,
  default off; `bind_session_world` (`session_host_world.hpp`) turns it on
  for every session-core world (H, RC) and SimTool's `--savestate-check` and
  resync-check bindings turn it on. Off, the binding is what it was: the
  legacy retail binding (PSC) gets nothing, which is how §4.4's "RL gets C3"
  is dropped (the legacy path is refused since B9DX and gets nothing new).
- **Engine** (`rollback.hpp`): `rollback_world::get_globals / set_globals`;
  `tick_state::globals / have_globals`, filled by `capture()` next to the
  clock; `recorded_globals(tick)` and `first_globals_change()` (the first live
  record whose globals differ from the one before it; logged once per
  history, "globals: first change at tick T ..."; cleared by
  `invalidate_history`). In `restore_and_replay`: the live globals read next
  to the frozen decision (unchanged); the anchor's written right after
  `replay_follows` is known - before the outside-callback hold, the clock
  rewind, the scripts and the blob; each replayed tick's after
  `replay_remotes` and before `step()` (a tick without globals counts
  `globals_unknown`); the live ones back after the loop, a failed step's
  break included. Step 4 is literal: the values the world had before the
  rewind, not the record of the live tick's - the same in a session, and the
  only safe choice where a frame may rewind after its scripts wrote the
  globals of its coming step. Writes compare by bit pattern and only differing
  values are written. Stats: `globals_set`, `globals_replays`,
  `globals_replays_start`, `globals_unknown`, `globals_failures`.
- **A refused write** (step 6): the live values put back as far as the world
  takes them, the history dropped, `return true` with no report code (the
  journal's CORRECTION numbering has none for it; the frozen code would
  misname it). The core watches `globals_failures` where it watches
  `frozen`: `rewind_world` (client_core.cpp) and `on_snapshot`
  (client_core_messages.cpp) ask for a resync ("physics globals refused"),
  and the resync re-step (client_core_resync.cpp) counts as not applied (it
  asks again). Status line: `globals=set/replays/start/unknown/failures`
  after `frozen=`.
- **Measured, H4's question:** in all 14 replays of `core_ref_s11` the
  headless client logs "globals: first change at tick base+1" (tick 28 at
  base 27, tick 9 at base 8): the frame-2 write lands in the record after
  the first one, so a rewind anchored at the first record crosses it. No
  capture rewinds there: `globals=0/0/0/0/0` in every replay (§6, A20).

**Checks (quick tier).** `BallanceMMOMessageTests` 630/630 (U1-U5 as
`RollbackEngineGlobals.*`, U1's control with a no-op setter: the clock 8 kDt
ahead; U7, added at the review, `TheLiveGlobalsComeBackAfterTheReplay`: the
world's live factor (3) differs from the last record's (2), as after a retail
frame's scripts wrote the next tick's globals; the anchor write puts 2, the
replay runs at 2, and 3 comes back after the last step, also after a step
that fails mid-replay - the one check of 2.3 step 4, since in U1-U5 and S1
the live values equal the last record's (S1's harness writes e's record over
whatever the engine put back); U6 as `RollbackBinding.ThePhysicsGlobalsMapToTheBackend` and
`TheSessionCoresWorldHasThePhysicsGlobals`); GCC 30/30; x64 BUILD-OK, x86
CLIENT-BUILD-OK. Wire check rWGQ7 -> rRK1 on `core_ref_s11` (protocol-18
captures, `EXTRA="--accept-protocol 18"`): ALL SAME, 12/12 journals
IDENTICAL; solo1 / solo2 sends SAME and journals IDENTICAL.

- **S1** (`--level 1 ... --drop-settle 1600 --ticks 100 --rounds 30
  --max-depth 40 --seed 7`): rWGQ7 23/30 FAIL ("restore identity: clock -
  physics_delta_time"); rRK1 30/30 (C3 set 46, replays 23, all anchored
  before the first change at 1665 = U); seeds 1-5 with 300 rounds 0/1500
  failed; the 1700 control 30/30 with no write.
- **The harness's live world (SimTool, `run_savestate_check`).** Before the
  correction below, 19 of the 30 rounds failed at the end witness only,
  "world hash: physics_delta_time 0 vs 0x1.f07c2p-6": every round with its
  live tick e inside the freeze (e < 1665). Restore identity and every
  replayed tick's blob (which carries the factor, the physics delta and the
  gravity) were equal; the four rounds anchored in the freeze with e after
  it (10: 1661+34, 19: 1652+21, 27: 1637+39, 28: 1652+24), the A15 / H1 case,
  passed without it. Cause: the chained rounds' stand-in live world enters
  every round with the straight pass's end's globals (factor 2), not e's, and
  step 4 rightly puts those back. In a client the live world is at e. So the
  harness, as B2SX made a named round's live world stand at e: puts the
  straight end's globals on the stand-in before each round (what every round
  had before C3: a running clock) and e's record's right after the rewind,
  before the witnesses and the follow; a re-made live world gets t0's after
  its `rewind_to(t0, t0)` (not exercised by S1). The witness rule is
  unchanged; the per-tick replay identity already verifies the globals of
  every replayed tick, e's included. The summary prints
  `savestate-check: globals (C3) set= replays= replays_start= unknown=
  failures= first_change= harness_put_failures=`.
- **S1b** (`--drop-settle 0 --ticks 40 --rounds 60 --max-depth 1`, L2 and
  L13, seeds 1-3, globals and clock kinds only): rWGQ7 "the environment's
  gravity differs" in 4 of 6 runs (L2 s1-s3, L13 s2), each stopping the run
  on the damaged world (14, 3, 41 and 3 rounds); rRK1 none and no clock
  failure, those runs reaching 58, 60, 60 and 37 rounds. What fails is
  A23's family without `--session-recipe` (the world's object count, the
  sector manager's and module scripts' flags, a script reset in the window)
  and four restore errors of window-born bodies (a joint's 'sys:unknown'
  controller, a paper ball the world no longer has), the same kinds as in
  rWGQ7.

**Deferred to the final verification** (over the 15-minute budget, or a live
check where the deterministic ones above cover the change): H1 (L1, a
forced rewind inside the freeze, live); H4 (2 clients, a forced rewind to
the first record, live - the case exists, measured above); R2 (the retail
H4); the 16 captures of §7.5 are now `core_ref_s11` (done above).

## As built (step RK3, 2026-10-29): K3 = C4, the Level 1 tutorial switched off

Option (c) of §11 Q1, as the K3 row of §8 says: not §2.4's hold. Protocol
**20** (the next free number; the header's "12" is nominal), world option
bit 3 `WORLD_SESSION_CLOCK` (the name §5 reserved; it now means "the
tutorial switched off"), the server's config `physics.session_clock`,
default on. No journal version (the header's `world_options` carries the
bit, §5), no bridge or engine change, no IVP change, no script-record
change.

- **The shared support** is `game/tutorial_exit.hpp` (new; CK API only):
  `tutorial_exit::resolve` is the main branch's `edit_Gameplay_Tutorial`
  find (Gameplay_Tutorial / `Kapitel Aktion` / `Tut continue/exit`, its
  `Set Physics Globals`, the block linked into that block's input 0 - the
  exit-key `Key` block, #34198 headless, #34506 retail - and its output 0);
  `activate` sets that IO active. The level and the frame are CK-free
  constants in `entity/session.hpp` (`SESSION_CLOCK_TUTORIAL_LEVEL = 1`,
  `SESSION_CLOCK_TUTORIAL_EXIT_FRAME = 10`).
- **Who decides.** In a client world the core: `tutorial_off()` (the bit
  and level 1), `world_frames_` (world steps since the anchor; unlike
  `frames_since_anchor_` a re-assign does not reset it, a new anchor does),
  and in `pre_step`, last, once per anchor and never in a replay, the
  platform's new optional `tutorial_exit(tick, error)` right before frame 10
  (the tick is base + 9). Both hosts fill it (headless `session_client.cpp`,
  retail `core_host.cpp`, its exit IO a file-scope static) and resolve at
  their anchor when `tutorial_off()`. The server's world decides it in
  `physics_world::tick` from `world_options::session_clock` (session runner
  from the bit; SimTool from `--world-options SESSION_CLOCK` and from a
  replayed header). The retail main branch's own exit on a countdown restart
  (`OnStartLevel`'s 10-frame timer, `BallanceMMOClient.cpp`) now stays out
  of a core session that switches the tutorial off
  (`core_host.hpp` `core_tutorial_owned()`); with `session core off` it is
  as before.
- **Frame numbering, found by the live run.** The first build counted the
  server world's frames from `tick_`, which the session runner numbers from
  the start base (`set_tick_index`): with base 8 the server activated the
  exit before frame 2, where it has no effect (below), and its world stayed
  frozen until the 25 s timer while every client unfroze - founders'
  mechanisms 0.05 m apart at the unfreeze tick 42 (base 27) and 1 m by 52 in
  the headless run. `physics_world` now counts from its first step
  (`first_step_tick_`), and the timing rule is stated once,
  `entity/session.hpp` `tutorial_exit_due(frames_stepped, exit_frame)`,
  which the core (`world_frames_`) and `physics_world` (`tick_ -
  first_step_tick_`) both call (the review's should-fix; a unit test of the
  rule, `SessionClientCoreTutorial.ExitDueRule`; a unit test of
  `physics_world`'s count with a non-zero `set_tick_index` cannot be built
  by the test target, which does not link `BallanceMMOSim` - its guard is
  the replay log line below). Replaying that run's server journal with the fixed
  binary (`--write-journal`) and comparing the three founders' live
  journals with it: every mechanism exact through the freeze and the
  unfreeze; the first divergence is a mirror ball at 268 (0.060 m), the
  queued snapshots-off residual.
- **The trace** (new, a log line only): the core logs the live time
  factor's changes in the first 64 frames after the anchor from each
  tick's record (C3), "globals: time factor a -> b at tick T (frame F after
  the anchor)"; no platform call, nothing sent or journalled.

**Measured: the exit does not make the tutorial freeze-free.** §11's "so
the tutorial never freezes any world" does not hold. SimTool S3 (`--level 1
--drop P_Ball_Wood_MF 0 --globals-trace`, `BMMO_TRACE_TIMEFACTOR=1`, with
`--tutorial-exit-frame N`, a measurement switch): the tutorial writes 0 in
frame 9 (tick 8) in every case; the exit activated before frame 5 or 9 has
no effect (frozen until the 25 s timer, tick 1664, as with the bit clear);
activated before frame 10, 11, 13, 15 or 16 the exit path writes 2.0 in
frame 16 (tick 15) every time, activated before frame 20 in frame 20 itself
(tick 19); Gameplay_Tutorial is no longer an active root afterwards (no
chapter freeze can follow; 1800 ticks, no further write). So an activation
from frame 10 on is kept until frame 16, the first frame whose behaviour
pass takes it, and the 2.0 cannot come earlier through this IO: **every world is frozen for 7 ticks, frames
9..15 after its anchor**. The same in the retail world (two copies, L1,
base 8: 0 at tick 16, the exit before tick 17, 2.0 at tick 23 on both) and
the headless client (base 27: 0 at 35, 2.0 at 42). What follows:

- **Founding members and the server:** the same session ticks everywhere
  (all count from the same base), so no divergence (the replay above).
  C3 replays a window across the freeze exactly (S4 below).
- **A rewind while the live clock is frozen** (the live tick in
  base+8..base+14) takes the frozen path (§6: unchanged): history dropped,
  a resync asked for. None happened in either run (no rewind in that
  window), but a player holding a key through the "Go!" can cause one.
- **A late joiner** freezes its own world at its frames 9..15, while the
  server's runs: in the headless run client 3 (base 893) logs 0 at 901 and
  2.0 at 908, and its import's re-step (892 -> 921) replays its own records
  of 901..907 at factor 0, so its world stays 7 ticks of physics behind the
  server's from then on. The size of that divergence was not measured: that
  run's server had the frame-numbering bug above, so the joiner imported a
  frozen world. **This was the decision handed to the user** (the step's
  report; **closed 2026-10-29: the user chose 'Hold frame-9 write', built
  as step RK3H below**): accept it; or add to (c) a hold of the tutorial's factor writes
  (§2.4's (a), or only the frame-9 0 write), so no world ever freezes; both
  depart from nothing built here. The review named one more, not
  investigated: stop Gameplay_Tutorial before its Init writes the frame-9
  0, instead of the exit IO (Init Ingame already writes 2.0 in frame 2); it
  departs from the main branch's way and from the user's "10 frames are
  necessary", and nothing is measured for it.

**Checks (quick tier).** `BallanceMMOMessageTests` 633/633, 634/634 after
the review's fix-up (U7 as
`SessionStartMsg.SessionClockBitRoundTrips` and
`SessionJournal.SessionClockInTheHeader`; the core's decision as
`SessionClientCoreTutorial.TheExitBeforeFrameTenOnLevelOneUnderTheBit`:
once at base + 9 on L1 under the bit, not without it, not on L13, not
without the op, once in the world's 10th frame with a re-assign at frame
5, again after a new anchor; the rule as `ExitDueRule`); GCC
30/30; x64 BUILD-OK, x86 CLIENT-BUILD-OK, Arch ARCH-BUILD-OK. Wire check
rRK1 -> rRK3 on `core_ref_s11` (`--accept-protocol 18`): every send equal
but SessionReady's protocol field (19 -> 20) in all 12 clients, 12/12
journals IDENTICAL, every replay to its end. S3 as above. S4 (S1's command
with the bit, seed 7): 30/30, no globals write (every round after the
freeze). Across the freeze (`--drop-settle 3 --ticks 30 --rounds 40
--max-depth 12 --session-recipe`): seed 1 40/40 (C3 replays 18, 8 of them
from before the frame-2 write), seed 2 38/40; the two failures are the
harness's re-made live world (round premise at 6 and 16: SimTool re-runs the
straight pass's frames on a Gameplay_Tutorial it does not rewind, so the
re-made world skips the freeze; as named rounds the e = 6 one passes, the
e = 16 one fails the same way; no client re-makes a live world); the
bit-clear control fails 40/40 on the frozen path. Retail, two copies, L1,
snapshots off (`.local-tools/work/RK3/rk3_retail.py`): both anchors found
the exit, both activated it before frame 10, `frozen=0`, `resyncs=0`,
`globals=0/0/0/0/0`; the server journal replayed 1763/1763 (with the bug
above, the old binary). Headless, L1, 4 clients, client 3 joining at 700,
150 ms, snapshots off (`maps/arena-test/rk3_h3`): every replay rc 0, as
analysed above.

**What the live runs did and did not exercise.** Both ran on the first
build's server (the exit before frame 2): the retail run shows the retail
host path and the retail timing (0 in frame 9, 2.0 in frame 16, as the
headless client and the fixed server world), not retail-vs-server
agreement; the fixed server is checked only by the replay above, and the
Arch binary is rebuilt with it, so the next live L1 run is its first live
exercise. The regression guard for the frame numbering: `--replay-session`
of any L1 journal whose start base is not 0 must log "exit activated before
frame 10 (tick base+9)" (SimTool's own scenes number from 0 and cannot show
it). The two runs' server journals replay 1763/1763 and 3804/3804 only with
the pre-fix binary; with the fix they diverge at the unfreeze tick by
construction. The S4 premise failures are a SimTool harness gap (queued in
roadmap §4 item 2 with B2SW/B2SX's harness residuals), not a client gap.

**Dropped:** §4.4's legacy retail half (the legacy path is refused since
B9DX). **Deferred to the final verification:** H2 (a client beamed to a
chapter spot: covered by the measurement that the exit deactivates
Gameplay_Tutorial, not run live), H3 with the fixed server (the late
joiner's divergence, measured), H5 (the arena recipes with the bit: the
wire check covers the bit-clear path; the bit is a no-op off L1, not run),
R1's ESC during a chapter (no chapter runs), and a live run of the fixed
server.

## As built (step RK3H, 2026-10-29): the tutorial's frame-9 write held

The user's answer to RK3's gate (2026-10-29, verbatim: 'Hold frame-9
write'): keep RK3's exit at frame 10 as built and hold ONLY the tutorial's
frame-9 time-factor 0 write, so no world ever freezes - not §2.4's hold of
every tutorial write, not stopping Gameplay_Tutorial. Protocol **22** (the
next free number after TRL's 21; a world without the hold would freeze),
no journal version (the header's `world_options` carries the bit), no
engine change (a CK parameter redirect: IVP and physics_RT untouched, no
number taken; #22 stays free), no wire layout change.

- **The block held.** `Gameplay_Tutorial / Init / Set Physics Globals`,
  found the way RK3 finds the exit: the root script by name, its direct
  child `Init` (the tutorial's start graph, `Gameplay_Tutorial.in0 ->
  Init.in0`), and the one direct child of that graph whose prototype is
  `Set Physics Globals` (refused when there are none or several) - #31475
  in the headless level, #31783 retail. Its input 1 ("Physic Time Factor")
  reads Init's local "Physic Time Factor" (0); the hold refuses unless that
  value is 0 (so it names the freeze write and nothing else). That block is
  the frame-9 writer: with only it held, the physics_RT writer trace
  (`BMMO_TRACE_TIMEFACTOR`) shows Gameplay_Tutorial writing 2 in tick 8
  where it wrote 0, and nothing else changes (S3 below).
- **How.** `game/tutorial_exit.hpp` `tutorial_factor_hold` (CK API only,
  next to RK3's `tutorial_exit`): input 1 is fed a fresh private
  `CKParameterLocal` `BMMO_TutorialFactorHold` with `SetDirectSource`,
  holding the in-play factor read (`GetValue(..., FALSE)`, no operation
  run) from `Gameplay_Ingame / Init Ingame`'s one Set Physics Globals
  (#25679 headless, #25987 retail; 2.0, the frame-2 write; refused when
  missing or not positive). The original local is never written (the
  propagation trap). Gravity (input 0) is untouched (`0,-20,0` there as in
  play). The block still runs in frame 9 and its output still fires, so the
  tutorial's flow up to the exit is unchanged: it writes 2.0 over 2.0
  (`CKIpionManager::SetTimeFactor` / `SetGravity` only store the values).
  `keep` re-holds an input handed back to its old source; `restore` hands it
  back (`ShareSourceWith` or `SetDirectSource` of the remembered source) and
  destroys the constant, only when the object under that id still carries
  the constant's name (the pattern of `sector_deactivate_pin.hpp`).
- **When, and who decides.** The decision is stated once,
  `entity/session.hpp` `session_clock_tutorial_off(bit, level)` (the core's
  `tutorial_off()` and `physics_world` both call it; unit-tested). The hold
  is made at the world's anchor, i.e. before its frame 1 counted as RK3
  counts frames, in every world: the server's (`physics_world` beside the
  exit's resolve, once - the world dies with its session), the headless
  client's and the retail core host's (`session_client.cpp`, and
  `core_host.cpp`'s file-scope `core_tutorial_hold`, beside the exit's
  resolve, under `core.tutorial_off()`), a late joiner's at its own anchor.
  Adapted from "the same frame everywhere": a redirect is state, not an
  event, so it is made at the anchor rather than right before frame 9; it
  is in force in every frame the write could run in, in every world.
  `SESSION_CLOCK_TUTORIAL_FREEZE_FRAME = 9` names the frame of the write (a
  static_assert keeps it before the exit frame). The client hosts keep it in
  their frame guard (`script_guards`) and hand it back before the next
  anchor's hold and at the session's end (retail `core_host_end_local`;
  headless at the next session's start), since base.cmo's Gameplay_Tutorial
  outlives a session. The legacy path gets nothing (refused since B9DX); no
  BallanceMMOClient data member.
- **Nothing else writes the factor later on Level 1 under the bit.** S3
  (1800 ticks): the writers are Gameplay_Ingame (2, tick 1), Gameplay_
  Refresh (2, tick 2), Gameplay_Tutorial / Init (2, held, tick 8) and the
  exit's `Tut continue/exit` (2, tick 15) - one change after the anchor in
  all (0.001 -> 0.002 at tick 1). Gameplay_Tutorial is no longer an active
  root after the exit, so no chapter's `Physic on/off` can run.
  Gameplay_Refresh's `Tutorial freeze/unfreeze` (#29832) writes only on
  Level 1 (its `Test` on cell [0][0] == 1) when its column-12 cell is set,
  0 or 2.0 by the Boolean its `Binary Switch` (#29809) reads (the
  tutorial's freeze flag, which the tutorial's Init sets and the exit path
  clears before its Set Cell `IngameParameter[0][3] = FALSE` and its
  Deactivate Script - read from the graph dumps, not traced), so a Refresh
  after frame 16 writes 2.0 or nothing. **Residual, a known limit of the
  user's narrow choice ('Hold frame-9 write'):** a Gameplay_Refresh
  activation inside frames 9..15 (a respawn or a pause resume within 7
  frames of the level start; a retail ESC is host-local) would still write
  0 in that world alone while the flag is set; no run triggers one (none
  that early), not exercised. It predates RK3H (before it every world was
  at 0 in those frames anyway). Queued as **RK3F** in `docs/roadmap.md` §4
  item 6, to be measured with R1's ESC and put to the user if it shows -
  not a re-decision here.
- **A level reset (read from the graph dumps, not run; RK3's, not new).**
  Gameplay_Tutorial is (re)activated by `Gameplay_Ingame / BallManager /
  New Ball / activate Tutorial?` (#27646) on every new ball when
  `CurrentLevel[0][0] == 1`, `CurrentLevel[0][12]` and
  `IngameParameter[0][3]` are set. The exit path clears `[0][3]`, so a
  respawn after the exit does not bring the tutorial back; `Init Ingame`
  sets `[0][3]` again, so after a mid-session level reset (9.28) the next
  new ball re-activates it. RK3's exit is once per anchor and would not
  fire again (unless that reset re-anchors the core, not checked); the
  hold persists (a redirect, kept by the frame guard), so that run's Init
  writes 2.0, but its chapters' `Physic on/off` writes are not held (the
  user's narrow choice) and could freeze that world - each world when its
  own ball reaches a chapter point, so the worlds diverge. This contradicts
  "nothing else writes the factor later on Level 1 under the bit" for a
  session with a level reset; queued as its own step **RK3R** in
  `docs/roadmap.md` §4 item 6 (re-fire the exit on every new ball under the
  bit, or hold the chapter writes).
- **Old journals.** A protocol-20/21 Level 1 journal under the bit, replayed
  with this binary, gets the hold and diverges at its frame 9 by
  construction (the RK3 runs' server journals; they are no longer a
  pre-RK3H control). Left so on purpose: forward compatibility is
  deliberately broken and server and clients are rebuilt together, so the
  hold is not gated on the journal's protocol.

**Checks (quick tier).** `BallanceMMOMessageTests` 644/644
(`SessionClientCoreTutorial.TheFrameNineHoldRule`: the rule on L1 / L2-L13
with and without the bit, the core's `tutorial_off()` equal to it on L1 /
L2 / L13, the freeze frame before the exit frame and no exit due before
it, protocol >= 22; `SessionStartMsg.SessionClockBitRoundTrips` at 22;
`tutorial_factor_hold`'s `apply` / `keep` / `restore` have no unit test of
their own - they need a CK context, which the message-test target does not
have - and are covered by S3, S4 and the two live runs below);
GCC 30/30; x64 BUILD-OK, x86 CLIENT-BUILD-OK, Arch ARCH-BUILD-OK. Wire
check rTRL -> rRK3H on `core_ref_s11` (`--accept-protocol 18`): every send
equal but SessionReady (tick 0, 75 bytes; its protocol field 21 -> 22, the
send RK3 attributed the same way) in all 12 clients, 12/12 journals
IDENTICAL (the arena and L8 recipes have no tutorial). SimTool S3
(`--level 1 --drop P_Ball_Wood_MF 0 --globals-trace`,
`BMMO_TRACE_TIMEFACTOR=1`, `.local-tools/work/RK3H/s3s4.sh`): the bit with
the hold, exit at frame 10 (default), 16 and 20: one change after the
anchor (tick 1), no factor 0 in 1800 ticks, the exit's 2.0 still in frame
16 (tick 15; tick 19 for 20), Gameplay_Tutorial not an active root after
it; controls: rTRL's binary with the bit 0 at tick 8 -> 2.0 at 15, the bit
clear 0 at 8 -> 2.0 at 1664. S4 across the former freeze (`--savestate-check
--frames --drop-settle 3 --ticks 30 --rounds 40 --max-depth 12
--session-recipe`, window ticks 4..33, frames 8..17 inside it): seed 1
40/40, seed 2 40/40 (RK3: 38/40, the harness's re-made world skipping the
freeze - moot now). Live headless L1 (`maps/arena-test/rk3h_h3`, the Arch
server of this tree, snapshots off, 4 clients, client 3 joining with base
898, 150 ms each way, `.local-tools/work/RK3H/live_h3.sh`): every client and
the server log the hold and the exit at base + 9; the only factor change in
the first 64 frames of every client is 0.001 -> 0.002 at frame 2 (the
joiner included, at 899); the server's replay 3396/3396; VERDICT PASS. The
joiner is not behind: its import re-steps 897 -> 927 from the server's
state, and from tick 928 on every mechanism and prop body equals the
server's at all 720 compared checkpoints (0.0000 m), and so does its own
ball (331 checkpoints, 0.0000 m) - as on the founders (their mechanisms
0.0000 m too); what differs are mirror balls from 1028 / 1032 on (2-8 m),
the queued snapshots-off mirror residual, the same on every founder
(`.local-tools/work/RK3H/mech_max.py`, `ball_max.py`). Before the import
(904..926) the joiner's own level props settle locally (P_Ball_Paper_MF*
1.2 m), which the import replaces. Retail L1 (`scripts/retail/rk3h_retail`,
two copies, core on, snapshots off, `.local-tools/work/RK3/rk3_retail.py`):
both hosts held #31783 and activated the exit at tick 17 (base 8 + 9);
their only core factor change 0.001 -> 0.002 at frame 2; the writer trace
of the session shows Gameplay_Tutorial writing 2 twice (frame 9 held, frame
16 the exit) where RK3's run wrote 0 then 2; `frozen=0`, `resyncs=0`, 1764
frames played; the server journal replays 1764/1764 with this SimTool.

**Deferred** (the final verification): H2, H5, R1's ESC and RK3's (e) as RK3
left them; a Gameplay_Refresh activation inside frames 9..15 (above;
queued as RK3F); a Level 1 level reset after the exit (above: the tutorial
re-activated, the exit not re-fired; queued as RK3R).

### Decided (2026-10-29): RK3R is not handled - a known limit

The user, asked whether a Level 1 level reset inside a session (the tutorial
re-activated by the next new ball, its chapter `Physic on/off` writes
freezing each world at its own moment) should be fixed by re-firing the
exit on every new ball or by holding the chapter writes, answered
(verbatim, the RK3R part of the message; its first part asked what
`PH[*][4]` holds): "RK3R那个，看看原来的影子球模式，会处理玩家手动重开那种导致的新手教学触发吗？印象中好像不处理（只会处理ready-go时候的跳过教程），如果是这样的话，这个遗留可以直接不处理" -
does the old shadow-ball mode handle the tutorial a player's manual
restart triggers? as I remember it does not (it only skips the tutorial at
ready-go); if so, leave this one unhandled. Verified: the main branch skips the tutorial only at the
server's countdown restart (`countdown_restart_`, `BallanceMMOClient.cpp:633`)
and never after a manual restart, where the tutorial plays again. So RK3R
is **not handled**: a documented known limit, not a gap to fix. What a
Level 1 reset does in a session (expected: the tutorial plays in that
world, its freezes take the frozen-clock path and ask for a resync) is
recorded once by the close-out (FULL rows RK3R-rec, in H1 headless and RT2
retail), not judged beyond "as expected". Do not re-ask it.

## As built (step RK2, 2026-10-29): K2 = C2, the array-read census, and Q4's written cells

C2 as §2.2 and the K2 row of §8 say, with §11 Q4's decision built in it and
the adaptations below. No protocol, journal or portable-record change; no
engine change, no IVP change. One item is open for the user: the sector
manager's `PH[*][4]` read (below).

- **Where the census lives.** `BallanceMMOCommon/include/game/array_reads.hpp`
  (new), class `bmmo::game::array_reads`: K0's census moved out of SimTool
  unchanged in its rules, its report as lines (`lines`) instead of
  `printf`, and `anchor_log` - the one function every world calls at its
  anchor. Adaptation: not `script_replay::array_reads` in SRP (the census
  needs `script_replay::cell_of`, so a member there would make the two
  headers include each other); `sim/array_census.hpp` is now a thin wrapper
  that prints `lines` for `--array-reads`.
- **Placement (§2.2).** After the selection, the world-agreement rewire and
  the placement: the server next to the checkpoint splits
  (`physics_world.cpp`, after the sector-cell split, a log only: that world
  keeps no records), the headless client right after `rewire_proximity`
  (`session_client.cpp`), the retail core host right after
  `proximity_rewire` (`core_host.cpp`; the census uses the public CK API
  only and ran under the retail Virtools `CK2.dll`). One log line per
  unrecorded (and sound) entry, the written-cell columns, the grouped
  unrecorded entries, and the closing `ARRAY-READS:` line.
- **The counter (§2.2).** The core makes no CK call, so the census's counts
  reach the status line as host counters (`client_platform.hpp`
  `host_counters::unrecorded_array_reads`, `written_cell_columns`; -1 = no
  census ran), printed as the status line's last section
  `array_reads: unrecorded=N written_columns=M` only from a host that ran one
  (every other status line is the text it was).
- **§11 Q4 built: the written cells recorded with the other cells.** The
  body's "C2 no longer changes the record" (§2.1, §3, §5, §6) predates the
  decision of 2026-10-24; Q4 (a) is built here, in the local record only:
  - `script_replay.hpp`: record v6's sections word gets **bit 3,
    `kSectionCells`**, after the hold section - per column u32 array id,
    u32 column, u32 rows, then every row's 4 bytes. Q4's "the next free
    script record version" is adapted to a section bit (v6's word is how
    content is added since G3). A record with the section is v6 even
    without the hold section; a record without it is the record it was.
  - **Which cells**: `array_reads::written_cells` - every (array, constant
    column) a selected, not keep-local, script writes with a
    column-confined writer (Set Cell, Column Transform, Column Operation),
    of an int or float column, that is neither a private copy nor one of the
    three recorded cells; all rows (the fan's slot row is found by Row
    Search, not a constant). On L1-L13 that is exactly
    `P_Modul18_ActiveSounds[*][1]` (4 rows) on every level with a fan, none
    on L1. Derived by the same walk the census classifies with, once at
    every binding (`rollback_binding_scripts.hpp`
    `script_binding_options::written_cells`), so the census and the record
    cannot disagree.
  - **Bound by the core hosts only** (the headless client, the retail core
    host, SimTool's session recipe and late-sector check); the legacy
    retail path binds nothing new.
  - **Restore**: the three cells' rule (`replay_activation`); a column
    whose array is gone or whose row count or type changed is left alone
    and counted (`last_restore().written_cells_skipped`;
    `written_cells_rows` counts the rows written).
  - **Never compared** (Q4: "no check compares it"): `diff` skips the
    section, its header compare ignores bit 3 (and the version word it
    changes), and two records equal but for the section are equal (`any`
    false); the rollback binding answers such a pair with an empty string,
    which `rollback.hpp` (the end-of-replay script check) no longer counts
    as a mismatch (`--compare-scripts` likewise). A divergence inside the
    slot column is therefore masked, by the decision.
  - **Counters**: `script_replay::written_cells_stats()` (records captured
    with the section, their columns and rows; restores, rows written,
    columns skipped), printed in the headless client's end-of-session
    `cells:` line and in SimTool's savestate-check summary.
  - **Local only.** The portable record (the resync state on the wire, the
    journals) carries no section, so a localized record has none and a
    resync import keeps its own slot column (non-physics). Forward
    compatibility is unchanged: no protocol or journal number.
  - Census class: the slot blocks (`Row Search` column 1, both `Set Cell`
    column 1) are now `recorded`, note "(written-cell section)"; `sound` is
    0 on every level (the class and its walk stay as the census's report of
    such a read). S2's "sound lists P_Modul_18's slot blocks" therefore
    reads "sound = 0, recorded includes them" since this step.
- **`unwritten` confirmed (K0's open point).** No source in the repository
  (BallanceMMO; the Ballanced submodule's CK2, physics_RT and TT) names
  `Physicalize_Convex` or `Physicalize_Balls`, the two arrays the class
  holds today, so no native code writes them: the class stays separate
  from load-time as K0 built it, counted, not a gap.

**S2 (`.local-tools/work/RK2/s2/`, SimTool rRK2, L1-L13).** Server scope
(the clients' scope since G3 / WG4: every world rewires under the bit):
`unrecorded = 2` on every level - the sector manager's two `Iterator If` on
`PH` (below) - `sound = 0`, `written_columns = 1` on L2-L13 and 0 on L1,
`unwritten = 2`. Control (`--client-scope`, the bit clear): unrecorded = 2 +
exactly A1's `CurrentLevel[0][1]` counts per level (L2 10, L3 4, L4 4, L5 2,
L6 10, L7 6, L8 3, L9 6, L10 7, L11 6, L12 11, L13 2, L1 0). S2's pass
(`unrecorded = 0`) therefore waits for the `PH[*][4]` decision alone.

**Open for the user: the sector manager's `PH[*][4]` read** (K0's finding,
"queued for K2"; not built either way).
- Symptom: the census's 2 unrecorded entries on every level, in every
  world (server, headless, retail: the same `unrecorded=2`).
  Gameplay_SectorManager's `Deactivate Sector` and `Activate Sector`
  `Iterator If` output whole `PH` rows; every `activate Type 1/2/3` starts
  with `Test(Set? == TRUE)` on column 4. A replayed sector pass reads the
  live column, not the column the live frame read.
- Cause (measured on L1-L13 with the census's writer list): column 4's only
  in-play writers are `P_Extra_Point_MF Script`'s `Update Array` (its own
  row, `Iterator If` on column 3 = its object, `Set Cell` FALSE at the
  pickup; 1-7 per level) and Event_handler's `Reset PH-Array` (every row
  TRUE, the level reset); neither is in the selection on any level, so no
  replayed frame writes the column, and the record does not carry it. Every
  other writer is `Levelinit_build` (load). `PH_Groups` (L2) gives
  `P_Extra_Point` activation 1 (Set World Matrix, Show, Activate Script of
  the extra), so only extra-point rows change in play; the physics rows
  (types 2 and 3, and the type-1 modules) read a column no in-play writer
  touches.
- When it matters: a rewind window that holds both a pass of sector k (the
  read) and a pickup of one of k's extra points after it (the write) - a
  boundary case under the sector union (the window is input_delay + a
  trip, a pickup needs the sector already shown); no run here met one.
- Options (their consequences are read from the graph dumps and the
  census, not run):
  - (a) Record the column (the written-cell section, or the three cells'
    table). Its writer is outside the replay, so restoring the anchor's
    value alone leaves a picked extra's row TRUE after a rewind across the
    pickup (the next pass of that sector would show and re-activate a picked
    extra); a faithful version needs N02R's bookkeeping too (the live
    writes recorded per tick, re-written in the replay, the live value kept
    across the rewind) - and then a replayed pass before the pickup
    re-runs Show / Activate Script on an extra the live world has already
    picked, an object outside the replay, so the extra comes back after the
    replay. Cost: 4 bytes per PH row per record (62 rows on L2), the
    bookkeeping.
  - (b) A census class for it, proven structurally at the anchor: every
    in-play writer of the read column is outside the selection and writes
    only its own object's row, whose group activation is type 1 and whose
    object is not physicalized. The replay keeps reading the live value,
    which is the right one for objects outside the replay (today's
    behaviour); `unrecorded` goes to 0 by classification. It softens §2.2's
    rule ("record the cell or rewire the read"). Cost: the writer analysis
    in `array_reads.hpp`.
  - (c) Accept it as logged (unrecorded = 2, S2 stays failing on it).
  - §11 Q4 does not settle it: Q4 is about cells a selected script writes;
    this column is written by scripts outside the selection.
- Recommendation: (b).

**Checks (QUICK tier: implement round 07:04-07:11, about 7 minutes; gate
round after the counters 07:15-07:18, about 3 minutes; one live run).** x64
BUILD-OK and x86 CLIENT-BUILD-OK, DEPS-OK on both; ARCH-BUILD-OK
(`ballanced-36c04e3d24c9+bmmo-a1ee0218161a-dirty`; GCC compiles
`array_reads.hpp` through `physics_world.cpp`); BallanceMMOMessageTests
645/645; GCC syntax 30/30. Wire check rRLT -> rRK2 over core_ref_s11 with
`EXTRA="--accept-protocol 18"` (a protocol-22 binary refuses the protocol-18
set without it, as RK3H and TRL ran it): 12/12 send traces SAME, 12/12
journals IDENTICAL; every replay's `scr=` counters equal old vs new; solo1 /
solo2 (`LABELS="solo1 solo2"`): c0 SAME and IDENTICAL (the script's
c1-c3 lines are its four-client assumption). SimTool L2
`--savestate-check --frames --session-recipe --drop P_Ball_Wood_MF 0
--drop-settle 3 --ticks 40 --rounds 60 --max-depth 1 --seed 1`: 60/60 PASS on
rRLT and rRK2, the outputs equal but for the census lines; with the counters
(rRK2b): `cells: captured=69 columns=69 rows=276 restores=72
restored_rows=288 skipped=0` - the records carry the section and every
rewind wrote its 4 rows back. Wire check rRLT -> rRK2b on l8_cross and
resync_snapoff (`--accept-protocol 18`): 8/8 sends SAME, 8/8 journals
IDENTICAL; every client's `cells:` line shows the section in every record
(l8_cross 2172-2251 records, resync_snapoff 4425-4899) and restored at
every rewind (restores = the `scr=` rollbacks: 9/7/9/7 and 44/57/56/62,
4 rows each), `skipped=0`. The journals are identical with the section
because a journal's checkpoint stores nothing of the local record; the wire
check cannot see the section, the counters do. One live run, retail two copies on
L11 (`scripts/retail/rk2_retail`, `.local-tools/work/CD1/cd1_gate.py`, the
Arch server of this tree, snapshots off, placement on): both assigned, the
census in both hosts and the server identical (`selected=64 ... recorded=87
private=88 ... unrecorded=2`, `written_columns=1`), the status lines
`array_reads: unrecorded=2 written_columns=1`, three rollbacks each, no
restore failed.

**Review fix-ups (gate round).** (1) The written-cell restore had no check
that saw a value come back: savestate-check (`--session-recipe`) now records
the binding's columns with every straight tick, sets every row of each
column to a per-row sentinel before each rewind, and fails a round whose
columns at e are not the straight pass's (`savestate-check: cells round
check`, one world restoring itself, not a compare between worlds, so Q4's
"no check compares it" holds). L2's recipe (`--ticks 40 --rounds 60
--max-depth 1`): 60 rounds checked, 0 differ, 240 sentinel rows overwritten
by the restore. (2) The headless `--compare-scripts` ran `diff` twice per
differing tick; once now. (3) The status line's `written_columns` came from
the anchor's census, found apart from the binding's columns; both hosts now
pass `written_columns` to the binding and report its count (the columns the
records really carry; the census's own count stays in its log line).
Gate round (07:24-07:29, about 4.5 minutes): x64 BUILD-OK, x86
CLIENT-BUILD-OK, DEPS-OK on both; BallanceMMOMessageTests 645/645; GCC
syntax 30/30; L2's recipe at `--max-depth 1` (above) and `--rounds 40
--max-depth 12` (40 rounds checked, 0 differ, 160 sentinel rows), both
PASS; wire check rRLT -> rRK2c on l8_cross and resync_snapoff
(`--accept-protocol 18`): 8/8 sends SAME, 8/8 journals IDENTICAL; the
step's second live run, retail two copies on L11 with this build
(`scripts/retail/rk2_retail2`): both assigned, the census identical in
both hosts and the server (`unrecorded=2 ... written_columns=1`), the
status lines `array_reads: unrecorded=2 written_columns=1` now from the
binding, four rollbacks each, no restore failed.

**Deferred** (the final verification): a rewind across a fan's slot claim
showing a changed column restored (the sentinel check shows every row
written back from the record, but no run here claimed a slot inside a
window, so a recorded value that differs from the live one was never seen);
Q4's
settle arm (waived by the decision's "no check compares it"); S2's pass,
which waits for the `PH[*][4]` decision.

### Decided (2026-10-29): `PH[*][4]` = option (b), per-world collectible state

The user's rule for collectibles (verbatim):
"如果PH*4是管加分球的，那么它应该和飞船一样不纳入回滚/物理同步，每个玩家有自己独立的一份。（同理，生命球也是）" -
if PH column 4
belongs to the extra points, it should, like the airship, stay out of the
rollback and the physics sync, each player having their own copy (likewise
the extra lives). The answer to RK2's three options (verbatim): "选：加检查"
= option (b), add the check. So `PH[*][4]` is **neither recorded nor rolled
back**: a census class "per-world collectible state" in `game/array_reads.hpp`,
granted by a load-time structural check that column 4's in-play writers are
only `P_Extra_*` scripts writing their own row (plus `Event_handler`'s level
reset); a map that breaks the rule is reported and its read stays unrecorded
and logged. No protocol, record or wire change. This is the design already
(world agreement Q6 / P3: the `P_Extra_*` scripts are out of the replay
selection, the own-ball sensors run in each client's world; extra points and
extra lives alike); the roadmap's §2 states the rule beside the airship
exception. Built by the close-out's step F4 (with S2's pass: `unrecorded=0`
on L1-L13 with the new class). Do not re-ask it.

## As built (step F0, 2026-10-29): RK3 (e) - the replay log line stays the guard

RK3 (e) asked for a unit test of `physics_world`'s frame count with a
non-zero `set_tick_index`. **Decision (the FULL plan left it to this step:
"a test target linking `BallanceMMOSim`, or a recorded decision to keep the
replay log guard"): keep the replay log line as the guard; no new test
target.** A `physics_world` exists only over a loaded CK context (the game
root, a level): a test target linking `BallanceMMOSim` would need the game
data at test time, which the message-test target (run on every build, and
GCC-checked for the CI's Linux job) deliberately does not. What stays
guarded and by what: the timing rule itself is shared and unit-tested
(`entity/session.hpp` `tutorial_exit_due`, `SessionClientCoreTutorial.ExitDueRule`);
the server world's count is `tick_ - first_step_tick_` with
`first_step_tick_` taken at its first step (`physics_world.cpp`, step RK3's
fix), and every Level 1 server journal with a base != 0 shows it in the
replay's log line "the Level 1 tutorial's exit activated before frame 10
(tick base+9)" - which the close-out's H1 Level 1 runs read (RK3R-rec, RK3F)
with the session runner's start base 8.

## As built (step F4, 2026-10-29): `PH[*][4]` as per-world collectible state

The user's option (b) (above, "Decided (2026-10-29)"), built as the FULL
plan's F4 row says. No protocol, journal, record or wire change; no engine
or IVP change; no world changes (the census only reads the graph).

- **The rule** (`BallanceMMOCommon/include/session/collectible_rules.hpp`,
  CK-free so the unit tests can feed it maps that break it): a read is
  `collectible` when every in-play writer (every writer outside
  `Levelinit_*` / `*_Init`) of what it reads is a `Set Cell` of one
  constant column C, the same C for all, in a root script outside the
  replay selection, and is either a `P_Extra_*` script writing its own
  row or `Event_handler`'s level reset; and at least one is a `P_Extra_*`
  script. "Its own row": the Set Cell's Row Index is output 0 (Index) of
  an `Iterator If` of the same root script on the same array whose Column
  is a constant key column other than C, whose Operator is the constant
  `CKEQUAL` and whose Reference Value is a constant local holding the
  root script's owner (compared by value, `GetValue`, not by the
  myself-parameter flag). "The level reset": the Row Index is output 0 of
  an `Iterator` of the same root over the same array with Starting Row 0
  and Ending Row -1 (every row). The reset writes every row TRUE, the
  physics rows' too: accepted as the plan's text states it (it is a level
  reset; on the shipped levels and the arena the value it writes, TRUE, is
  the level file's). The rule does NOT check that value: it is an
  assumption of the rule, true of every level it was run on. A custom map
  whose Event_handler reset writes anything else to a row the sector
  manager tests would still be granted, and a reset in one world would then
  change that row there alone; checking the Set Cell's value against the
  column's load-time value is the fix if such a map appears.
  A collectible read is also no sound read: the "sound, reads first" pass
  counts it as an unsound read of its array (as it was while unrecorded),
  so the class cannot turn another column's unrecorded write into `sound`.
- **The class** (`game/array_reads.hpp`): `kind::collectible`, decided only
  in `run()`'s level-table pass on reads still unrecorded, after
  `table_of`: the written-cell section (and so the local record) is the
  first pass's and cannot change by the class. `array_writers` now keeps
  each writer's root id and, for a Set Cell, where its row index comes
  from (`row_source_of`; `constant_word` reads a 4-byte local - an enum or
  an object id - by `constant_int`'s rule without the int type). The
  report prints `collectible=N` in the `array-reads level=` line and at the
  end of the `ARRAY-READS:` line, and every collectible entry by name with
  its note ("per-world collectible state: column 4 written in play only by
  P_Extra_* scripts on their own rows and Event_handler's level reset, all
  outside the selection"); a read with in-play writers that fails the rule
  stays `unrecorded`, its note "(not per-world collectible state: <why>)".
  The status line's section is unchanged (`array_reads: unrecorded=N
  written_columns=M`; `host_counters` and `anchor_summary` keep their
  layout).
- **The negative fixture.** No level breaks the rule, so it has two
  forms: the unit tests (`BallanceMMOServer/tests/collectible_rules_test.cpp`,
  8 tests: L2's writer set granted, an extra life's writer granted, and
  a non-`P_Extra` writer of column 4, a `P_Extra` writer of a constant /
  unproven / all-rows row or keyed on the column it writes, a second
  column, a row-moving in-play writer, a selected writer, a reset that is
  not every row, no `P_Extra` writer, no in-play writer - each refused
  with its reason), and SimTool `--collectible-fixture other-writer|other-row`
  (with `--array-reads`): before the census, the first `P_Extra_*` Set Cell
  (by id) whose row is an `Iterator If` output gets its root script
  renamed (`F4_Fixture_Writer Script`, a non-`P_Extra` writer) or its Row
  Index fed from a new constant local (a `P_Extra` writer of another row),
  in the loaded level only.
- **S2** (`.local-tools/work/F4/s2/`, SimTool rF4, server scope). The
  game copy `Ballance-Agent` (and both retail copies) hold the arena in
  the Level 13 slot (md5 c58a76b9...); RK2's "L13" row was the arena. The
  original Level 13 (`Level_13.NMO.orig`, 9b5be1ca...) ran with the
  file swapped in for that run only and the arena put back (md5
  checked). L1-L12, L13 (original) and the arena: `unrecorded=0`,
  `collectible=2` on every one (the sector manager's two `Iterator If` on
  `PH`), every other class's count equal to RK2's; the writer lists
  `P_Extra_Point_MF Script [Set Cell] col 4` x1..x7 plus `Event_handler`
  x1 (no `P_Extra_Life` script writes `PH`). Client scope (the bit
  clear): `collectible=2` and `unrecorded` = exactly A1's
  `CurrentLevel[0][1]` counts (L2 10, L3 4, L4 4, L5 2, L6 10, L7 6, L8 3,
  L9 6, L10 7, L11 6, L12 11, arena 2, L1 0). Fixtures on L2, the arena
  and the original L13: `collectible=0 unrecorded=2`, both notes naming the
  broken writer ("F4_Fixture_Writer Script [Set Cell] col 4 is neither a
  P_Extra_* script's own row nor Event_handler's level reset";
  "P_Extra_Point_MF Script [Set Cell] col 4 does not write its own row
  (row index: a constant)").
- **Checks** (implement round 09:29-09:37, about 8 minutes, one live
  run): x64 BUILD-OK, x86 CLIENT-BUILD-OK (core_host.cpp recompiled),
  DEPS-OK on both, `ninja -t deps` of physics_world.cpp.obj VALID with the
  header; ARCH-BUILD-OK (`ballanced-36c04e3d24c9+bmmo-21125462ff68-dirty`);
  BallanceMMOMessageTests 654/654; GCC syntax 31/31. Wire check rF2 ->
  rF4 over core_ref_s11 (`EXTRA="--accept-protocol 18"`): 12/12 send
  traces SAME, 12/12 journals IDENTICAL; solo1 / solo2 c0 SAME and
  IDENTICAL (c1-c3 "missing" is the script's four-client assumption, as
  at RK2); the replays' logs differ only in the census lines and the
  status lines' `array_reads: unrecorded=2` -> `unrecorded=0`. The live
  smoke (`.local-tools/work/F4/live.sh`, four headless clients on the
  arena, 60 s, the Arch server of this tree, snapshots off, trafo inputs
  and placement on): VERDICT PASS, the server journal 4618/4618 ticks
  matched, 2309 checkpoints, 0 mismatches; every world's census
  `unrecorded=0 ... collectible=2` (the server and the four clients) and
  every client status line `array_reads: unrecorded=0 written_columns=1`.
- **Not run here:** a retail run (the retail core host runs the same
  `anchor_log`; its status line is read by the close-out's retail batches,
  RT1-RT3).

## As built (step FX2, 2026-10-29): S1b with `--session-recipe` to a verdict - the re-made live world's per-tick globals and outside activations

FULL step FX2 (the stage A close-out, `.local-tools/work/FULL/plan.md` FX2;
RK1-d, "S1b with `--session-recipe`", which S2 could not run to a verdict).
SimTool only (`BallanceMMOServer/sim/sim_tool.cpp`); no shared, client,
server, protocol, journal, bridge or IVP change.

- **The cause (S2).** A round whose live world must stand at e on the other
  side of a window birth re-makes it: t0's record restored, then the straight
  pass's whole frames re-stepped to e. The scripts outside the selection are
  not rewound (they stand where the straight pass and the earlier rounds left
  them), so their one-shot work of the window's frames never recurred there:
  at `--drop-settle 0` (t0 = 1) Init Ingame's Set Physics Globals of frame 1
  (A18: gravity -20, factor 2 from record 2 on; the re-made world kept t0's
  -9.81, 1) and its Execute Script of Gameplay_SectorManager (pending before
  frame 2; sector 1's births at 7). The premise "the re-made live world at e
  is the straight pass's" failed at round 3-4 of every run.
- **The fix.** (1) Before each re-stepped frame at t the globals the straight
  step at t ran with - the record of t+1's, the engine's own rule for a
  replayed tick (C3, §2.3 step 3, `rollback.hpp`) - written when they differ
  (`put_step_globals`), in the session-recipe re-make and in the trafo
  re-make alike. (2) The straight pass notes, before each step, the
  activations its frame consumes - Gameplay_SectorManager's (with
  IngameParameter[0][1] / [0][2]) and those of the selected scripts
  `session_script_activations` names (`straight_tick::pending`,
  `capture_pending`); the re-make re-issues, before the same step, only the
  ones its own world lacks (`reissue_pending`: the cells, then
  `CKScene::Activate(script, reset)`), so an activation the selection makes
  again itself is never issued twice (an Activate counts in
  `m_BeObjectNextFrame`, step FX1), and a union pass is made by the
  re-stepped frame itself, never re-issued. An activation the re-made world
  has that the straight pass's frame had not is counted, not undone. The
  summary prints `re-made live worlds (step FX2): steps, outside activations
  re-issued, pending the straight pass's frame had not, per-tick globals
  written`.
- **S1b** (`--savestate-check --frames --session-recipe --drop
  P_Ball_Wood_MF 0 --drop-settle 0 --ticks 40 --rounds 60 --max-depth 1`, L2
  and L13, seeds 1-5; `.local-tools/work/FX2/s1b.sh`, `run1/`): **10/10 runs
  to a verdict**, 60 rounds each; C3 replays 1-2 per run, 12 in all, every one
  `replays_start` (anchored before the first change at 2: A18's frame-1 write
  exercised); 0 "gravity differs", 0 clock failures, 0 premise failures; 165
  re-made worlds, 161 per-tick globals writes, 145 outside activations
  re-issued (Gameplay_SectorManager, reset, before the step at 2), 0 the other
  way. By §7.2's count (globals and clock kinds only) S1b passes; 20 of the
  600 rounds fail, all of the residual below.
- **The residual (harness, root-caused, not fixed here).** Every failing round
  compares at a tick e <= 2, on Gameplay_SectorManager only, in two forms:
  - depth 0 at e = 1 or 2 (12 rounds): "delayed link 0 countdown 4 vs 0" -
    the link `Deactivate Sector.out0 -> Set Cell.in0` (delay 4, B2SX) holds the
    initial 4 in the straight pass's records 1-2 (not yet queued: the manager
    first runs in frame 2), 0 in the stand-in live world (fired in its own
    later history), and `restore_behavior` (`script_replay.hpp`) writes a
    countdown only when the record has the link in flight (0 < c < initial),
    so a depth-0 compare reads the live world's own value;
  - anchor 1, depth 1, ending at 2 (8 rounds): the manager's flags (the
    pending activation Init Ingame's frame 1 left), the same countdown and
    `CurrentLevel[0][4]` 1 vs 0 (Init Ingame's write in frame 1, which the
    manager overwrites with 0 in frame 2 before any reader): the replay never
    runs Init Ingame, and the round compares at e before what a client does
    at a rewind's end (`live_cells_guard`: the live cells written back, the
    activation pending for the live frame re-issued) - B2SX's family ("a
    client never compares its scripts before the next frame").
  Not a client gap: a client whose live tick is <= 2 has never fired the link
  (its live value is the record's), and every round that compares after frame
  2 passes - named rounds 1:2:2, 1:3:2, 1:4:2, 2:1:2 (the link in flight
  live, at 3), 2:2:2, 2:3:3, 2:4:2, 3:1:2, 1:12:2 and 2:12:2 on L2 and L13
  (`FX2/named/`, `try1/`): every replayed tick's blob, the end's scripts and
  the follow equal; the replays from 1 re-make sector 1's 5 births exactly.
  The two forms are different kinds of thing (FX2's review):
  - form (i) is a fidelity gap of the harness's stand-in live world, not a
    rule: no client whose live tick is <= 2 holds the fired countdown 0. Its
    fix is harness only and inside FX2's own premise ("the live world at e is
    the straight pass's"): the straight pass notes each kept tick's
    not-in-flight countdowns of the selected scripts' delayed links, and the
    round's live world takes those of e before the round - in a re-made
    world and also in one that is not re-made (a depth-0 round at e <= 2 may
    run on a world chained from the previous round, whose bodies already
    match, so a write inside `reissue_pending` alone would not reach it).
    It is the orchestrator's to queue as a harness item; not built here,
    because it changes every round's live world and so needs the S1b and
    regression arms again, beyond FX2's gate round;
  - form (ii) is a rule question and the user's: the round compares at e
    before a client's end-of-rewind steps; options (a) leave it - S1b judged
    by its own kinds (globals and clock, §7.2), the 8 rounds a known harness
    artifact; (b) the round's compare at e runs after the client's
    end-of-rewind steps (`live_cells_guard`'s write-back and re-issue) - a
    change of the rule the round is judged by, as B2SX's option (a) was.
- **Known limits of the re-issue** (FX2's review, not changed): a watched
  script already pending in the re-made world with another reset bit than
  the straight pass's, or a manager already pending whose IngameParameter
  [0][1] / [0][2] differ, is skipped without a count (the cells are written
  only with a re-issue) - neither is in the blob the premise compares; the
  re-issues of one frame go in `pending_watch` order (the manager, then
  `session_script_activations`), not the straight pass's order of addition
  to `m_BeObjectNextFrame` - harmless while only the manager is ever
  re-issued (145/145), a frame-order question (replay-script-order family)
  if two ever are; with both trafo and passes the two summary lines print
  the same `remake_steps` / `remake_globals_written` totals, not split by
  branch.
- **Regressions** (`FX2/regress.sh`, `settle12/`): S1b at `--drop-settle 12`
  10 x 60/60 with 0 re-issues and 0 globals writes (S2's settle-12 result);
  V4b alive 50/50; V9 L11 move (default lead) and L8 move (lead 180) 50/50
  each - a union pass inside the re-make, 0 re-issued, 0 the other way; EL9's
  L-S1 trafo scene 160/160 (21249 re-made steps; its window has no globals
  change, so the trafo branch's write is built but not exercised). Tests
  656/656; GCC `linux_test_syntax.sh` 31/31, `linux_sim_syntax.sh` 5/5 (with
  `sim_tool.cpp`); `ninja -t deps` of sim_tool.cpp.obj 289 VALID. No wire
  check: nothing the headless client or the shared code compiles changed.

## As built (step H1, FULL close-out, 2026-10-29): H1 (replaced), H2, H4, RK3F, RK3R-rec

Rows RK1-a/b/d, RK3-b, RK3F and RK3R-rec of the close-out plan, run on the
Arch server of HEAD 193c9a5b and the headless client rH1 (two test switches
added, off by default: `--reset-level-at` and `--activate-script`,
session_client.cpp); the full numbers are in `docs/roadmap.md` §4 item 7,
"Run at H1". Level 1 founders' tick base 27 (frame f = tick 26 + f).

- **H1 (replaced) and H4, offline from the live captures** (`h1_full/l1_main`,
  clients 0 and 1, `BMMO_TRACE_TIMEFACTOR=1`): `--force-rollback-at` 47:19
  (anchor 28, the first record: H4), 46:19 (anchor 27, across frame 2's
  globals write: `globals=2/1/1/0/0`, one replay across the change -
  RK1 (d)) and 50:16 (anchor frame 8, across the tutorial's frames 9..16):
  rows equal to the unforced replay at every LOCAL and FINAL checkpoint,
  no factor-0 write, no "differs from the live record" beyond the control's.
  Adapted: H4 on Level 1 instead of L2 / the arena (the arena's H4 rewind
  is the same code path; the globals change exists only at frame 2).
- **H2** (client 1 beamed onto the ExtraLife chapter spot at 2001; beamed
  once, not held - no hold verb): frozen 0, resyncs 0, `globals=0/0/0/0/0`
  on all four; no chapter runs under the bit.
- **RK3F shows** (§3 D5 of the plan, the user's): Gameplay_Refresh
  activated before frame 12 in one world (`--activate-script
  Gameplay_Refresh:b+10`, standing in for a respawn or a pause resume)
  writes factor 0 at frame 12 until the exit's 2.0 at frame 16; that
  world's level props part from the others for good (0.56 m, 0.37 m),
  frozen 0 (four ticks are below the frozen-clock detection), no resync.
  Cause: RK3H's hold covers Init's frame-9 block only; Gameplay_Refresh's
  `Tutorial freeze/unfreeze` (#29832) still writes 0 while the tutorial's
  freeze flag is set, i.e. until the exit path clears it (its 2.0 lands in
  frame 16). Options for the user (not decided): (a) hold #29832's factor
  input too, with RK3H's redirect (its other writes are 2.0 already);
  (b) accept it as a known limit (exposure: a respawn or a pause resume
  within frames 9..15 of the level start, about 106 ms; it is not
  detected). Decided (the user, 2026-10-29: "D5: a"): built as step DX2
  below.
- **RK3R-rec**: "Reset Level" sent at 1500: the own ball unphysicalized,
  the trafo resets and the sector manager's reset as in a death, the
  tutorial's Init re-runs (its held write 2.0); no new own ball within 940
  ticks, so no chapter freeze was reached headless - the retail run (RT2)
  records the full path through the menu's restart.
- **The two switches are live-only** (gate-round review): neither action
  is journaled or re-applied in replayed frames, so a run measures it only
  while no restore in the acting world is anchored at or before the action
  tick (a restore of Gameplay_Refresh's or the reset handling's script state
  would undo it and no replay redoes it). Read from the `l1_reset` logs
  ("savestate restore at tick"): client 2 (activation after tick 37)
  restores first at 259; client 3 (reset after tick 1500) restored at 1489
  before the reset and at 1527 next - no anchor at or before the action
  after it, so both runs hold. A
  replay of those two captures needs the same switch and still diverges
  wherever a rewind crosses the action, by construction; RK3F and RK3R-rec
  were read from the live journals and logs, not from replays.

## As built (step H7X, FULL close-out, 2026-10-29): a resync re-step at the server's globals of S (§4.2, A17)

**The symptom** (FULL H1, `h1_full/h7_join`, roadmap §4 item 4): a late
joiner's remade trafo pieces matched 73/102, every moving one a constant
fraction of a tick behind with bit-equal velocities.

**The cause.** §4.2 says a resync's re-step replays "from a synthetic anchor
whose ticks have no local records: they keep the current globals", and that
the anchor takes its globals from the server's blob of S (A17). As built at
RK1 neither holds: the synthetic anchor (`base - 1`) and the live records of
`base..arrival` are local records of this world, and the engine wrote the
anchor record's globals before the restore and each replayed tick's before
its step. For a running member those equal the server's (C4), so nothing
showed; for a late joiner they are its own level start - factor 0.001,
gravity -9.81 until its scripts' frame-2 write (tick base+1) - while the
server stepped S+1 at 0.002 / -20. The joiner's re-step ran its first tick
at half the server's step (and the start gravity): every moving body of its
world ended exactly one half step behind (offset / velocity = -1/66 s in x
and z for every falling paper piece, the Stone mirror 0.270 m behind at
17.858 m/s; 121 of 154 bodies equal at the first checkpoint after the
re-step). A regression of RK1 (982370d8): before it a replay wrote no
globals and ran at the live ones, which by the arrival were 0.002.

**The fix** (no protocol, journal, record or engine change):

- `physics_savestate.hpp` / `physics_savestate_common.cpp`
  `savestate_globals(data, size, factor, gravity, error)`: the blob's
  factor and gravity, parsed with the savestate's reader, no world touched.
  Bridge v29 (`physics_rt_api.h` appends `savestate_globals`;
  `physics_rt_bridge.cpp`, `physics_direct.hpp`, retail `physics_view`);
  bridge note in `docs/engine-changes.md` (no engine number).
- `rollback.hpp`: `rollback_world::blob_globals` (optional; installed by
  `bind_rollback_world` for a backend with the entry, so every session-core
  world and SimTool's bindings, never a test backend without it).
  `restore_and_replay`, when called by `resync()` with a non-empty server
  blob whose globals read: the anchor's write is the server's globals
  instead of the anchor record's (the factor from the blob; the gravity
  from the blob unless the anchor record's double is the same float, in
  which case that double is kept - the blob stores floats and the start
  gravity is not one); each replayed tick whose record still equals the
  anchor record's keeps the server's; from the first record that differs
  the records are written, as C3 (the joiner's own frame-2 write at
  base+1 then writes the server's values again: no change). A re-step whose
  server globals differ from the anchor record's counts
  `rollback_stats::globals_server` and logs "rollback to tick S (...): the
  re-step runs at the server's physics globals of S (...), not the anchor
  record's (...)". The live globals still come back after the loop (2.3
  step 4). The re-recorded ticks and the re-captured anchor then hold the
  server's values, so a later rewind into the window replays them.
- **Adaptation of §4.2.** The design's two premises are replaced by the
  rule above: the window's records are applied as changes made after the
  anchor, not ignored, so a re-step anchored before a session-start write
  (§4.2's "S <= 2" case, where the server's S still has the start values)
  still crosses that write at its recorded tick. What this rule does not
  cover: a write in the joiner's own level start after its frame 2 that the
  server's world does not have (the Level 1 tutorial is off in every world
  since RK3; D5 / RK3F's one-world freeze is the user's open decision)
  (decided "D5: a" and built at step DX2 below).

**Checks.** Unit tests 662/662 with the new
`RollbackEngineGlobals.AResyncReStepRunsAtTheServerBlobsGlobals` (U8: a
joiner's records at 1 / -9.81 until tick 3, the server's blob at 2 / -20:
no write at all, every re-record at 2, the clock nine server steps on; its
control without the hook writes 1 at the anchor and 2 before tick 3 and
ends one half step short). GCC syntax 31/31. x64 BUILD-OK, x86
CLIENT-BUILD-OK (plugin and mod rebuilt with v29). Wire check rH1 -> rH7X
over `core_ref_s12`: ALL SAME, 36/36 journals IDENTICAL (the running
members' resyncs in `resync_snapoff`, `resync_at150*` unchanged: their
blob's globals equal their records'). Live H7 (`h7x_full/h7_join`, Arch
server rebuilt from this tree, 150 ms, snapshots off, trafo on, placement
on): VERDICT PASS, server journal replay 3329/3329; the joiner's re-step S
1707 -> 1764 logs the line above with `globals=0/0/0/0/0`; pieces 102/102
bit-equal on all four (`tr3fix/pieces.py`); at 1766 the joiner has 153 of
154 bodies equal to the server, the Stone mirror 0.0000 m (H1: 0.270), the
one left `P_Ball_Wood_MF` 0.021 m, off on c1/c2 too (0.008 m), a body the
predicted paper mirrors push. No retail run (the retail host gets the hook
through the same binding; a retail late join is RT2's item).

## As built (step DX2, FULL close-out, 2026-10-29): D5 / RK3F - Gameplay_Refresh's tutorial freeze write held with RK3H's

The user's decision (2026-10-29, verbatim: "D5: a") on H1's RK3F finding:
hold `Gameplay_Refresh / Tutorial freeze/unfreeze`'s factor write with
RK3H's redirect. No protocol number, no journal version, no engine change,
no wire layout change.

- **The block held.** `Tutorial freeze/unfreeze` (#29832 headless; the
  level names it with a trailing space, so it is found as the one direct
  child of `Gameplay_Refresh` whose name is that once trailing spaces are
  dropped) runs `Test` (`CurrentLevel[0][0] == 1`) -> `Get Cell` column 12
  -> `Binary Switch` #29777 -> `Binary Switch` #29809 (the tutorial's
  freeze flag) -> `Parameter Selector` #29771 (`pIn 0` = 0, `pIn 1` = 2)
  -> `Set Physics Globals` #29784. That block's Physic Time Factor input
  (1), which reads the selector's output, is fed the same private constant
  `BMMO_TutorialFactorHold` (the in-play factor read from Init Ingame,
  2.0) with `SetDirectSource`: both branches now write 2.0 (the False
  branch wrote it already). Gravity (input 0) untouched; the selector and
  the switches still run; nothing is written into the graph's locals.
- **Adapted from RK3H's guard.** RK3H refuses unless the input reads 0;
  here the input reads the selector's output, which keeps whatever the
  last run selected (0 on a fresh load, 2.0 after a Refresh that took the
  False branch; base.cmo outlives a session), so the guard is structural:
  the input's real source must be the output of a `Parameter Selector`
  whose every input is a float that is 0 or the in-play factor, one of
  them 0 (otherwise the Refresh write is not held and the reason is
  logged; RK3H's Init hold stays, see below).
- **How (one place, every world).** `game/tutorial_exit.hpp`
  `tutorial_factor_hold` now carries two redirects (`init`, RK3H's;
  `refresh`, DX2's) onto its one constant. `apply` makes RK3H's Init hold
  exactly as before (nothing changed when it cannot), then adds the
  Refresh hold on top: a Refresh block that is missing, wired otherwise
  (the structural guard) or refuses the constant leaves `refresh` empty and
  the Init hold in place, `apply` still succeeds and the logged `where`
  says "Gameplay_Refresh's tutorial write is NOT held (reason)" - a world
  is never worse off than at RK3H (the review's should-fix: the first
  version was all-or-nothing, so a retail Refresh graph edited by another
  mod would also have dropped RK3H's hold; in the success case the two
  versions make the same redirects in the same order). `keep` re-holds
  either input handed back to
  its old source, `restore` hands both back and destroys the constant only
  when neither still reads it. The hosts call the same three functions as
  at RK3H, so the server's world (`physics_world`), the headless client
  (`session_client.cpp`) and the retail core host (`core_host.cpp`'s
  file-scope `core_tutorial_hold`) all hold it at the anchor, keep it in
  their frame guard and hand it back where RK3H's is handed back - no call
  site changed, no `BallanceMMOClient` data member. Their log line names
  both blocks: "... Gameplay_Tutorial / Init / Set Physics Globals (#31475)
  writes 2 instead of 0, Gameplay_Refresh / Tutorial freeze/unfreeze / Set
  Physics Globals (#29784, by Parameter Selector #29771) writes 2 instead
  of 0 or 2 ...". Adapted: the server's world is included although the
  step names the headless client and retail - the hold lives in the one
  shared struct every world calls, and every world must agree (a Refresh
  in the server's world inside frames 9..15 would otherwise freeze it
  alone).
- **No protocol number.** The step names none and server and clients are
  rebuilt together (forward compatibility is deliberately broken).
  Difference from RK3H, recorded for the orchestrator: RK3H took 22 because
  a world without its hold freezes in every Level 1 session; a pre-DX2
  world differs from a DX2 world only when Gameplay_Refresh runs inside
  frames 9..15 of a Level 1 session, and the reference journals replay
  IDENTICAL.
- **A control that stops being one.** `h1_full/l1_reset` client 2's
  world wrote 0 live at tick 38 (frame 12). A DX2 binary that re-makes
  that live world (SimTool's `--session-recipe`) or replays it from an
  anchor before frame 12 now holds the write and diverges there by
  construction - not a regression. This step used that capture only as
  a journal-against-journal control (`mech_max.py` reads the recorded
  journals, no replay), which stays valid.
- **Not covered (unchanged, by the user's decisions):** RK3R (a Level 1
  reset re-activating the tutorial; its chapter `Physic on/off` writes are
  not held - decided not handled).

**Checks (quick tier, 2026-10-29, about 12 min of checks).**
`BallanceMMOMessageTests` 663/663 (`tutorial_factor_hold` has no unit test
of its own: it needs a CK context, as at RK3H); GCC syntax 31/31 ok; x64
BUILD-OK / DEPS-OK, x86 CLIENT-BUILD-OK / DEPS-OK (`core_host.cpp`
recompiled), Arch ARCH-BUILD-OK `ballanced-36c04e3d24c9+bmmo-2f2c737a0b47-dirty`.
Wire check rDX1 -> rDX2 over `core_ref_s12`: WIRE-CHECK: ALL SAME, 36/36
journals IDENTICAL (no Level 1 arm in the set: it shows nothing else moved,
not that the hold works). Live (the one run, `maps/arena-test/dx2/rk3f`
and `maps/arena-test/dx2x_rk3f/sector1..4`, `.local-tools/work/DX2/dx2_run.sh
rk3f true 4 45 1 -- --client-arg-for "2:--activate-script
Gameplay_Refresh:b+10"`, `BMMO_TRACE_TIMEFACTOR=1`, the Arch server of
this tree, snapshots off, trafo inputs and placement on; the harness ran
Level 1's four sectors as four sessions): in all four sessions client 2's
Gameplay_Refresh, activated after tick 18 (base 8, frame 12), writes **2**
(H1: 0 at frame 12 until frame 16); no factor 0 in any client log; every
client's only globals change is 0.001 -> 0.002 at frame 2 (tick 9); the
tutorial's Init still writes 2 (RK3H's hold) and the exit is activated
before frame 10 (tick 17), as at RK3H; frozen 0, resyncs 0, starved /
rebases / far / unmatched 0, the server journals replay 3540/3540 and
3594/3594 matched, VERDICT PASS; the server logs the hold in every
session. Level props against the server (`.local-tools/work/RK3H/
mech_max.py` from tick 0, every non-ball body): 0.0000 m on all four
clients in all four sessions (no tick over 1 mm); the same tool on H1's
`l1_reset` (the control) shows client 2's P_Ball_Paper_MF001 0.5616 m
from tick 38. Client 2's first restore is at tick 221/222, after the
activation (the switch is live-only, H1's rule), so the run measures it.
Gate round after the review fix-up (the independent Refresh hold, about
6.5 min): x64 BUILD-OK / DEPS-OK, x86 CLIENT-BUILD-OK, Arch ARCH-BUILD-OK,
663/663, GCC 31/31, wire check ALL SAME 36/36; no second live run (only
the failure paths changed, which the headless level does not reach).
**Deferred** (FULL's retail batch RT2): a retail Level 1 run with a
respawn or a pause resume inside frames 9..15 (the retail host gets the
hold through the same struct; only its compile was checked here).
