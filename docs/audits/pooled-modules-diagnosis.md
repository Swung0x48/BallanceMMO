# Pooled level objects under the sector union: diagnosis

Report file: `C:/Users/GEEKER~1/AppData/Local/Temp/claude/D--repos-BallanceMMO/769d21ff-c9e2-42a0-a69a-d95ce0b06545/scratchpad/audit2/pooled-modules.md`

All evidence comes from offline SimTool runs (`build-agent/BallanceMMOServer/BallanceMMOSimTool.exe --root C:/Users/geekerwan/Desktop/Ballance-Agent`), from code at HEAD 4882ec2, and from graph dumps. Outputs are in `C:/Users/GEEKER~1/AppData/Local/Temp/claude/D--repos-BallanceMMO/769d21ff-c9e2-42a0-a69a-d95ce0b06545/scratchpad/audit2/` (written `audit2/` below; `audit/` is the sibling scratch directory). Nothing in the repository was modified.

## 0. Corrections to the premise

- **The briefed command fails.** `--drop P_Modul_30 1 --drop-sector 1 --drop-move 2 120` exits with `no 3D entity named P_Modul_30 (pass --drop-at X Y Z)` (sim_tool.cpp:1146-1150). The numbers below were made with `--drop-at`, for example `--drop-at 76.5 15 0` or `75.202 8.5 0.031`.
- **1.158 m comes from `--drop-move 2 0`, not 120.** `--drop-move 2 0` activates sector 2 at world tick 30, while the plank is still awake. With `--drop-move 2 120` the activation lands at tick 150, after the plank has gone to sleep, and the result is 0.092 m, against 0.085 m with no move. The bug is still present in that run but stays latent until the plank is woken (§1).
- **L8 has one see-saw object.** It is a single P_Modul_30_MF (PH_Groups Amount 1) used in s1, s2 and s4, not two `P_Modul_30_Wippe`.

## 1. SYMPTOM

### What a player sees

Every original level, L1-L13, re-uses objects across sectors. The arena does not. Once a second sector joins the union, the shared objects fail in four ways, each the same on every side:

1. **Jointed mechanisms in the earlier sector lose their pivot.** The body stays where it is, but its hinge, ball joint or force is rebuilt in the later sector's frame, hundreds of metres away. Results range from a sag to the module flying out of the level.
2. **The later sector's copy is missing.** At its PH pose there is either nothing at all, or a mesh with no collision behind it.
3. **Fans, trafos and extras leave the earlier sector.** Players still in that sector lose them.
4. **Wake triggers move to the later sector.** An earlier module stops waking near its own players. A player at the later site wakes it from about 330-965 m away.

### SimTool numbers

All runs are 361-600 ticks. "Move S" means sector S is activated at world tick 30 unless a different tick is given.

| Level / object | Scenario | With the later sector | Without it |
|---|---|---|---|
| L8 P_Modul_30 see-saw (s1) | stone ball on the plank, move 2 (plank awake) | falls **1.158 m**; z -0.119 → -0.340 | 0.085 m |
| same | move 2 at ticks 50 / 70 | 1.503 / 1.377 m | 0.085 m |
| same | move 2 at tick 150 (plank asleep) | 0.092 m (latent) | 0.085 m |
| same | ball dropped at the **s2 site**, nothing near s1, move 2 at tick 0 / 300 | s1 plank wakes and drops **1.160 m** (at tick 330 in the 300 case) | 0.000 m |
| L8 P_Modul_29 rope bridge (s2) | move 4 | planks flung up to **2049.9 m** (Platte06 at 2347.95, -1614.89, 411.12) | sags 8.6 m |
| L8 P_Modul_29 (other setup, bridge_mv60) | sector 4 at tick 160 | Platte08 1716 m away | sags 2.697 m |
| L10 P_Modul_41 | move 4 | falls **464.68 m** | 0 m |
| L11 P_Modul_37 bridge | move 2 | 0.177 m | 0 m |
| L11 P_Modul_25 Bridge001 | ball at the s2 site, move 2 | pulled 1.7 m down and 4.85 m along z | stays put |
| L11 P_Modul_25 | ball on the s1 bridge after move 2 | bridge never wakes and acts as static geometry (ball rests at x 114) | bridge tilts, ball rolls to x 111.4 |
| L11 P_Modul_34 slider | drop-sector 2, move 3 | never moves | moves 0.657 m |
| L11 P_Modul_26 sack (ball joints + force) | move 3 at tick 130 | swings along x (83.9-98.0), up to 10.32 m from control | swings in z only, x = 90.73 |
| L11 P_Modul_01 Pusher001 | move 4 at tick 0/20 | never woken (y -31.895) | woken, y -31.981 |
| L11 P_Modul_01 | player at the s4 site, move 4 | both s1 Pushers wake, about 965 m from the player | nothing wakes |
| L11 s4 P_Modul_01 site | stone ball on it | falls 8.07 m and rolls away: no collision | rests on Pusher001 (`--drop-sector 4` alone) |
| L8 s2 see-saw / s4 see-saw / s4 bridge PH poses | sector-1-only world (what fix A would leave) | ball falls **138.2 m** (no floor) | 5.49 m with the s2 see-saw present |
| L8 P_Box_MF001-003 | sector 2, then move 5 | bodies stay at s2; s5 rows 73-75 have no boxes | — |
| L10 P_Dome_MF001 (fixed) | s1, then s5 | dome body stays at s1; ball at s5 falls to the floor at -16.996 | lands on the dome at -13.33 |
| L10 fan P_Modul_18_MF001 | paper ball over the s1 fan, move 3 or move 2 | falls to -13.08 | hovers at 4.4-5.3 |
| L11 / L1 props | P_Ball_Stone move 3; P_Box move 2 | bit-identical to control in s1; nothing appears in s3 / s2 | — |

**Timing.** The damage depends on the body's state when the later sector's pass lands:

- **Awake:** the sag or flight starts at once.
- **Asleep:** the hinge is already wrong but nothing moves, because the rebuilt constraint is created satisfied. It fires the next time the body wakes, for example from a ball near the moved proximity centre (the s2wake runs). A ball touching the frozen body does not wake it (the L11 P_Modul_25 case).
- **Level and unloaded at the pass:** about 1.16 m, once woken.
- **Tilted and loaded at the pass:** 0.007 m or less within the run.

Solo play hits the bug as well: under the union nothing is ever deactivated.

## 2. CAUSE

### 2.1 The levels pool objects by design

- **Count PH sets the pool size.** In Levelinit_build / Process Placeholder, Count PH (id 38691; `audit/d_Levelinit_build.txt:1767-1880`) sets `PH_Groups.Amount` to the largest number of rows of a type in any **one** sector:
  - Change Value If (#38652) zeroes Amount.
  - A Counter walks the sectors.
  - Iterator If plus Value Count count each type in a sector.
  - Test Cell (#38618, less-than) and Set Cell keep the maximum.
- **Copy Objects makes the copies.** It (#37262, `:788-900`) makes Amount-1 copies of `<type>_MF` with Object Copy (Dynamic=1), then TT Save IC with hierarchy, then Add To Group.
- **"set MF-Objects in Array" deals them out.** It (#37637, `:1097-1150`) hands the group's members to the PH rows (column 3, Object). Its Group Iterator loops back to its own In, so later sectors' rows re-use earlier sectors' objects.
- **Example, L8:** P_Modul_30 has Amount 1, and the PH rows that name P_Modul_30_MF are:
  - row 0, s1, (75.20, 4.50, 0.03)
  - row 15, s2, (402.57, 17.82, -58.07), rotated 90°
  - row 48, s4, (790.47, 8.56, -465.24)

The pooling is only correct when one sector is live at a time.

### 2.2 What vanilla does at a crossing

1. **Start the crossing.** Gameplay_Events / activate Sektor (id 30071; `audit/d_Gameplay_Events.txt:235-264`) sets `[0][2]` = current and `[0][1]` = current+1, then Execute Script(Gameplay_SectorManager, Reset=1).
2. **Deactivate the old sector.** The manager sets `CurrentLevel[0][4]=0` (#31270), then runs Deactivate Sector. For Reset-1 rows it runs Activate Script(reset). The module script reads `[0][4]=0` and takes the False branch: Hinge Shutdown, then Unphysicalize, then TT Restore IC (hierarchy). For Reset-2 rows it runs Unphysicalize, then Hide.
3. **Activate the new sector.** After a delay-4 link the manager sets `[0][4]=1` (#30926) and runs Activate Sector. Each object is then placed at the new row and physicalized fresh there.

The pooled object is torn down before it is moved.

### 2.3 What the union does, step by step

Example: P_Modul_30_MF, with sector 1 live and sector 2 activating.

1. **Every side starts the same pass at the same lockstep tick.** Each writes `IngameParameter[0][2]=0` (deactivate nothing) and `[0][1]=S`, then calls `scene->Activate(Gameplay_SectorManager, TRUE)`:
   - server: `physics_world.cpp:789-793`
   - headless client: `session_client.cpp:1570-1573`
   - retail client: `core_host.cpp:1397-1399`

   The N2 hold (`sector_activation.hpp:83-96`) gives the level's own Execute Script a null script. `sector_union.hpp:17-25`: the union only grows.
2. **The teardown is skipped.** The manager sets `[0][4]=0`. Deactivate Sector's Test #30888 (deactivate != 0) is false, so nothing is torn down. After the delay-4 link, `[0][4]=1`.
3. **The live object is moved.** Activate Sector's Iterator If reaches row 15. PH_Groups gives P_Modul_30 Activation 1, so "activate Type 1" (#31134) runs:
   - Test #31108 (Set? == TRUE)
   - Set World Matrix (row matrix, Hierarchy=1) on P_Modul_30_MF
   - Show
   - Activate Script(MF Script, Reset=1)

   Set World Matrix moves MF and every child that has no body: P_Modul_30_HingeFrame, the hinge referential, which is a sibling of the physicalized P_Modul_30_Wippe under MF. It also moves the proximity ObjectB.
4. **The script reset leaves the joint handle in place.** On the next frame `CKBehavior::Activate(TRUE,TRUE)` calls `Reset()` (`CK2/src/CKBehavior.cpp:1176-1182, 2194-2250`). Reset only restores link delays and clears IOs. It sends no CKM_BEHAVIORRESET; only the context reset at `CKContext.cpp:274` does. So the joint blocks' handle locals survive.
5. **The script rebuilds the hinge.** Start → Get Cell `[0][4]` (now 1) → Binary Switch True → Physicalize, then:
   - **The body is kept.** It already exists, so Physicalize fires Out1 and returns at once (`Physicalize.cpp:104-116`). It stays at s1.
   - **The old hinge is deleted.** Set Physics Hinge Create deletes the old constraint (`PhysicsHinge.cpp:152-158`) and queues a PreSimulate callback.
   - **The new hinge is built at s2.** The callback runs `set_hinge_ws` from HingeFrame's **current** world position and its row-2 axis (`PhysicsHinge.cpp:118-131`), with Object2 = FixCube 40077, a shared static body. The s1 plank is now hinged on an x-axis about 59 m away in the y-z plane, r = (-11.32, 57.95). The swing is measured to match a rigid rotation about that axis to about 1 mm (dz/dy ≈ 0.19-0.2). Only x-motion remains, which shows the old z-axis hinge is gone.
   - **The wake trigger moves.** Out1 re-arms the one-shot TT Scaleable Proximity (ObjectB on MF, 50 m on X/Z), which now centres on s2. Building the constraint does not wake the body.
6. **The result** (replay mode, server-like; `audit2/mech_rep_19.txt`, `mech_rep_40.txt`, `mech_rep_bodies.txt`):

   | Part | Frame 19 | Frame 40 |
   |---|---|---|
   | MF | (75.202, 4.500, 0.031) | (402.572, 17.815, -58.067) |
   | HingeFrame | (75.201, 7.062, 0.031), axis (0,0,-1) | (402.572, 20.377, -58.067), axis (1,0,0) |
   | Wippe entity | — | (402.722, 22.371, -57.716) |
   | Wippe body | (76.2473, 12.9950, -0.1193) | unchanged (every frame from 18 to 40) |

   No body ever exists at s2.

### 2.4 Effect by block type

| Block | On a reset Create | Effect on a pooled module |
|---|---|---|
| Set Physics Hinge | always deletes and rebuilds (`PhysicsHinge.cpp:152-158`) | pivot moves to the later sector |
| Slider (`SetPhysicsSlider.cpp:152`), Ball Joint (`PhysicsBallJoint.cpp:170`), Spring (`SetPhysicsSpring.cpp:179`) | builds only if the handle is null | old constraint kept; any later Shutdown/Create toggle rebuilds it from the moved frames |
| SetPhysicsForce (`PhysicsForce.cpp:151-164, 210`) | turns Direction through Direction Ref when it is created | re-aimed at the moved frame (L11 sack pushes along x) |
| Delayer | In re-entered from Start | restarts |
| Sequencer | 'Current' is a parameter | keeps its value |
| TT Scaleable Proximity | re-armed | centre at the later site (lost wake / remote wake) |

The Type 2/3 chain (`audit2/l8_sectormanager.txt:210-240`; P_Box, P_Dome, P_Ball_Paper are Type 2, P_Ball_Stone and P_Ball_Wood are Type 3) is Test(Set?) → Set World Matrix → Show → Get Key Row → Physicalize. Physicalize short-circuits, so the body stays in the earlier sector and the later sector never gets it. Trafos have no script, so they only get Set World Matrix and Show, and they move.

### 2.5 Server and clients

**Physics is identical on every side, so this is a gameplay bug, not a desync.** The same writes run at the same tick under the shared union and pacer. The hinge is built from HingeFrame, which is not physicalized, so every side gets the same constraint.

**One difference is the entity matrix, and nothing observes it.**
- The clients arm the body guard: `session_client.cpp:1980-1983`, `core_host.cpp:1214-1218`, `physics_session_client.cpp:1789-1792`. The server's physics_world never does; the only writer of `m_KeepLevelBodies` is `physics_state.cpp:1688`.
- With the guard on, the Physicalize short-circuit calls `UpdateObjectWorldMatrix` (`Physicalize.cpp:113-114`). That snaps the entity of a pooled **physicalized** part (Wippe, P_Box, dome) back to its body.
- On the server, a sleeping body's entity stays at the later sector's pose. Only awake bodies are in `m_MovableObjects` and synced back (`CKIpionManager.cpp:103-121, 807-812`).
- The state hash, the pose hash and the journals cover only IVP cores (`physics_state.cpp:215-261`), so nothing compares the entity. Once the body wakes, every side agrees again.

**What players see.** On clients, the s1 see-saw and s1 boxes are drawn at s1, and nothing is drawn at s2/s5 for physicalized parts. For P_Modul_01 at the later site, the meshes of Rinne and Filler (fixed, never synced) and of a sleeping Pusher are drawn there with no collision behind them.

**Consequence for testing.** No rollback or correction fires on it in the steady state. A test that counts mismatches passes on a broken level.

## 3. SCOPE

### 3.1 Pooled objects per level

The pooled count is the number of objects named in rows of more than one sector, out of all objects in the level's PH table. Source: `--dump-array PH`; `audit2/ph{1..12}.txt`, `ph13orig.txt` (the original L13, from a scratch copy with `Level_13.NMO.orig` restored), `pharena.txt`. All 1054 rows have Set? = TRUE.

| Level | PH rows | Pooled / objects | Later-sector rows re-using an object (T1/T2/T3) | Jointed | P_Modul_01 | Props (balls/boxes) | Fixed | Fans |
|---|---|---|---|---|---|---|---|---|
| L1 | 51 | 18/27 | 24 (13/7/4) | 0 | 0 | 11 | 0 | 0 |
| L2 | 62 | 15/36 | 26 (26/0/0) | 0 | 0 | 0 | 0 | 3 |
| L3 | 53 | 14/35 | 18 (17/1/0) | 1 | 0 | 0 | 1 (d=0) | 0 |
| L4 | 75 | 19/46 | 29 (28/0/1) | 0 | 0 | 1 | 0 | 1 |
| L5 | 55 | 15/28 | 27 | 2 | 2 | 0 | 0 | 1 |
| L6 | 54 | 16/29 | 25 | 0 | 0 | 0 | 0 | 5 |
| L7 | 64 | 18/36 | 28 | 6 | 1 | 0 | 0 | 3 |
| **L8** | 86 | 23/46 | 40 (34/6/0) | **5** | 0 | 5 boxes | 1 (d=0) | 2 |
| L9 | 96 | 32/50 | 46 (45/0/1) | **12** | 2 | 1 | 0 | 5 |
| L10 | 109 | 32/62 | 47 (43/3/1) | **8** | 2 | 3 | 1 (682 m) | 5 |
| **L11** | 105 | 32/52 | 53 (45/0/8) | **9** | 2 | 6 | 0 | 5 |
| L12 | 115 | 31/53 | 62 (61/0/1) | 1 | 0 | 1 | 0 | 11 |
| L13 (orig) | 104 | 30/54 | 50 (41/0/9) | 1 | 0 | 9 | 0 | 9 |
| arena | 25 | 0/25 | 0 | 0 | 0 | 0 | 0 | 0 |

**Notes on the table**

- **Movable totals.** Jointed + P_Modul_01 + props reproduces the audit's counts exactly: L1 11, L5 4, L7 7, L8 10, L9 15, L10 13, L11 17. The audit left out L3 (1 jointed), L4 (1 prop), L12 (1 jointed + 1 prop) and L13 (1 jointed + 9 props).
- **Trafos and extras** are pooled in every original level: 2-11 trafos and 4-8 extras per level. Pooled rows are 72.9 m to 2515.7 m apart.
- **Same-pose pairs.** Only three pooled pairs sit at the same pose (d=0): P_Trafo_Wood_MF in L1 s3/s4, P_Dome_MF in L3 s1/s2, and P_Dome_MF in L8 s3/s4. These are harmless, and any cloning fix must leave them pooled.
- **P_Modul_03** is never pooled in any level.

**Jointed class (the blockers)**

| Level | Pooled objects (sectors) |
|---|---|
| L3 | 25_MF001 (s4/s5) |
| L5 | 34_MF001 (s3/s4); 37_MF (s1/s2/s5) |
| L7 | 25_MF (s4/s5); 29_MF001 (s1/s3); 34_MF002 (s3/s5); 41_MF001-003 (s1/s2) |
| L8 | 25_MF001 (s1/s3); 25_MF (s3/s5); 29_MF (s2/s4/s5); 30_MF (s1/s2/s4); 37_MF001 (s4/s5) |
| L9 | 17_MF001 (s3/s4); 25_MF (s4/s5); 26_MF/MF001/MF002 (s2/s5); 29_MF (s3/s4); 30_MF (s3/s4); 37_MF (s4/s5); 41_MF001-004 (s2/s5) |
| L10 | 08_MF (s2/s3); 25_MF001 (s4/s5); 29_MF (s3/s4); 30_MF (s2/s3/s4); 37_MF001 (s1/s4); 37_MF (s2/s4); 41_MF001/002 (s1/s4) |
| L11 | 19_MF (s2/s3); 19_MF001 (s2/s6); 25_MF (s1/s2/s3/s4); 25_MF001 (s1/s2/s4); 26_MF001 (s1/s3); 30_MF (s3/s6); 30_MF001 (s2/s6); 34_MF001 (s2/s3); 37_MF (s1/s2/s3/s4) |
| L12 | 37_MF (s3/s6) |
| L13 | 34_MF (s2/s3) |

**Module physics** (from `--describe-script`, `audit2/scripts/*.txt`):

| Module | Bodies | Constraints / controllers |
|---|---|---|
| 01 | Rinne and Filler fixed; Pusher movable, starts frozen | none |
| 03 | 9 movable | slider + spring |
| 08 | 1 movable | hinge + 2 forces |
| 17 | 1 movable, starts unfrozen | hinge + spring |
| 18 | no body (fan) | force only |
| 19, 25, 30, 37 | 1 movable, starts frozen | 1 hinge |
| 41 | 1 movable, starts unfrozen | 1 hinge |
| 26 | 2 movable | 2 ball joints + 2 forces |
| 29 | 9 planks | 10 hinges |
| 34 | 2 movable | slider |

Extras and trafos have no body.

### 3.2 L8 sector 1

Sector 1 has 11 PH rows: rows 0-9 plus row 84, PS_FourFlames_MF, which is the only sector-1-only row. All ten P_ objects are pooled. The only bodies are P_Modul_30_Wippe and P_Modul_25_Bridge001 (`audit2/fb8.txt`).

| Row | Object (type) | s1 position | Later rows send it to |
|---|---|---|---|
| 0 | P_Modul_30_MF (hinge) | (75.20, 4.50, 0.03) | s2 332.8 m, s4 853.3 m |
| 8 | P_Modul_25_MF001 (hinge) | (96.00, 19.65, 4.70) | s3 581.2 m |
| 5 | P_Modul_18_MF001 (fan) | (87.59, 4.66, 81.33) | s3 612.7 m, s4 907.3 m |
| 1 | P_Trafo_Wood_MF001 | (169.90, 5.09, 9.99) | s2 124.9 m, s3 545.6 m, s5 949.4 m |
| 4 | P_Trafo_Wood_MF | (79.77, 19.75, -36.82) | s3 473.0 m, s4 837.8 m, s5 931.0 m |
| 6 | P_Trafo_Paper_MF | (169.90, 5.09, -10.01) | s2 217.5 m, s4 657.9 m |
| 7 | P_Trafo_Paper_MF001 | (121.73, 0.84, -23.82) | s3 535.2 m, s5 922.8 m |
| 2, 3, 9 | P_Extra_Point_MF001/002/003 | — | s2 to s5, 258-1041 m |

**What each later sector does to s1:**
- **Sector 2** breaks the see-saw (1.158 m measured) and takes one wood trafo, one paper trafo and one extra.
- **Sector 3** re-hinges the bridge 581 m away (code path; not measured under load), moves the fan, the second wood trafo and two extras. After that, s1 has no wood trafo.
- **Later sectors lose content too:** s2 and s4 have no working see-saw (and nothing to land on), s4 has no rope bridge, and s5 has no P_Box_MF001-005.

### 3.3 L11 sector 1

Sector 1 has 17 PH rows: rows 0-15 and row 103. 15 of the 16 module rows are pooled. Only P_Modul_26_MF (row 10) and PS_FourFlames_MF are sector-1-only.

| Row | Object | Later rows |
|---|---|---|
| 14 | P_Modul_25_MF (hinge) | s2 305.1 m, s3 464.2 m, s4 882.5 m |
| 15 | P_Modul_25_MF001 (hinge) | s2 314.6 m, s4 929.1 m |
| 6 | P_Modul_37_MF (hinge) | s2 322.8 m, s3 761.3 m, s4 1002.1 m |
| 9 | P_Modul_26_MF001 (2 ball joints + force) | s3 573.5 m, yaw 0 → 90 |
| 11 / 0 | P_Modul_01_MF / MF001 | s4 963.9 m / s4 975.9 m, s6 1470.6 m |
| 1, 4, 5, 7, 8, 13 | P_Ball_Stone_MF..MF005 | s3 581-699 m (4 of them), s6 1493-1531 m (4 of them) |
| 12 | P_Trafo_Stone_MF001 | s3 725.4 m, s6 1512.4 m |
| 2, 3 | P_Extra_Point_MF001/002 | s2 to s6 |

**What each later sector does:**
- **Sector 2** alone re-hinges three s1 bridges 305-323 m away: Modul_37 sags 0.177 m, and the Modul_25 bridges stop waking near s1 and are woken from s2.
- **Sector 3** adds the sack swing (Sack001 5.4-10.3 m off), removes s3's four stone balls (they stay in s1), and moves the trafo.

L11 cannot be tested in a union session as built.

## 4. OPTIONS

Common to A and B: the world each builds differs from today's, so `PHYSICS_PROTOCOL_VERSION` goes from 9 to 10 (`entity/session.hpp:51`). `journal_header` (`journal.hpp:102-117`) carries only `build_id`, so it needs a world-option field, or an old journal will replay silently differently.

### A. Skip the later rows of an object that is already placed

**What it does.** Set PH column 4 (Set?) to FALSE on the later rows of an object an earlier row already placed. The three activate Types gate on Test 31108 / 30987 / 31195 (Set? == TRUE), and the False output goes straight to Out, so no move, no reset and no Physicalize run.

**Determinism.** Use a **static** rule: the earliest-numbered sector's row keeps the object, computed at the anchor. PH row order and object assignment are the same on every platform (engine change #2, deterministic qsort). A **dynamic** rule ("held by a live sector") would break for late joiners, who replay the owed sectors lowest-first (`sector_union.hpp` tick()), while members can run them out of order (+4 @352 then +2 @827, lockstep-without-snapshots.md:1148).

**Trap found in verification.** Event_handler / reset Level ends in "Reset PH-Array" (Set Cell column 4 = TRUE on every row; `audit2/v_fixA/ds_Event_handler.txt:267-268`). A client-side restart or the loss of the last life (design 9.28, the automation `restart` verb) therefore re-enables every row on that client only, and the server never resets. So A must re-assert Set? = FALSE immediately before **every** manager pass on every side, or after the reset chain, the way N1 re-applies sector_reset_guard. The death reset is the second SectorManager call site (`sector_reset_guard.hpp:8-13`) and must see the same rows skipped. Retail must write TRUE back at session end, as N3 does.

**Rollback.** Nothing is left. A held module is never re-placed or re-hinged, so the joint-presence gap and the unrecorded frames never come into play. The Set? cells are not rewound, but if they are asserted before every pass, a replay reads the same values.

**What it breaks.** The later sectors' instances are missing **on purpose**:
- L8 s2 and s4 have holes where the see-saw belongs, and s4 where the rope bridge belongs (ball falls 138.2 m).
- s2 loses its paper trafo, 25 m from the s2 fan, and a wood trafo.
- s5 loses 5 boxes and P_Modul_25/29/37.
- L11 s3 loses its stone balls and five modules.
- Trafos and extras switch sides: today the leading sector gets them, under A the earliest sector keeps them. A variant that skips only movable/jointed rows keeps trafos moving forward but leaves the same holes.

L8 and L11 stay **unplayable** past sector 1. A is a guard against explosions, not a fix.

**Work.** About one day: one game/ header (~40 lines plus the per-pass hook), the server/headless/retail anchor and pass sites (`physics_world.cpp` ~236 / `update_sectors`, `session_client.cpp` ~1473 / 1570, `core_host.cpp` ~860 / 1397), the retail restore, the protocol constant and the journal field.

### B. One object per placement, cloned at the anchor (recommended by the audit)

**What it does.** At each world's anchor, walk PH in row order. For every row whose Object was already used by an earlier row at a **different** matrix (the three d=0 pairs are skipped):

1. Copy it with `CKContext::CopyObjects`, using the builder's own CKDependencies* (Copy Objects local 1, read the same way `ObjectCopier.cpp:130` reads it).
2. Name the copy deterministically and keep the `P_` prefix (for example `_BMMO_S<sector>`, under the 64-byte MAX_NAME), so `is_mechanism_script` (`script_replay.hpp:195-202`) selects its script.
3. Create it dynamic, in the level and scene, with its scripts **inactive and hidden**. The builder copies with CK_OBJECTCREATION_ACTIVATE and relies on a later sector reset that does not run at the anchor.
4. TT Save IC with hierarchy.
5. Add it to the PH_Groups type group, as the builder does (the Trafo Manager finds trafos with Get Nearest In Group).
6. `SetElementObject(row, 3, copy)`.

CopyObjects remaps references inside the copy (referential 47608, target 47615, ObjectB 47616, meshes) and keeps outside ones shared (FixCube 40077, Ball_Pos_Frame, CurrentLevel, IngameParameter). The builder's own copies already work, for example L8 P_Modul_41 to P_Modul_41006 in s2.

**Ordering constraints**

- **Before any hinge exists.** The copied Set Physics Hinge local 0 is the IVP_Handle, and both Create and the reset callback delete it (`PhysicsHinge.cpp:152-158, 194-207`). The server anchor has no bodies yet.
- **Server:** before `rewire_proximity_probes` / `rewire_ball_identity_reads` (`physics_world.cpp:239-240`). These find blocks by structure, so the copies get their own probes.
- **Headless:** before select (`session_client.cpp:1473`).
- **Retail:** after `level_sequencers_.restore` (`core_host.cpp:860`) and before select (`:878`).
- **Sequencer counters on retail.** Retail records them only right after a map load (`BallanceMMOClient.cpp:346-357`), and the PH stays rewritten across sessions. So every anchor must restore each copy's Sequencer counter from its original's recorded value, or the P_Modul_26 copies (L9 3, L11 1) differ between worlds in a second session without a reload.
- **The PH_Groups.Amount variant does not fit retail.** Raising Amount before Copy Objects only takes effect at level load, and the retail session anchors on a restart, not a reload.

**Determinism.** The clones depend only on the PH table, never on pass order. Late joiners, resyncs and out-of-order passes all build the same world. CK_ID differences only reorder `m_PhysicsObjects`, and the hashes do not depend on that order.

**Rollback.** Each copy's bodies are born in its own sector's pass. That is the existing in-window birth path (destroyed at the restore, put back by local matrix, re-made by the replayed script), measured exact on the arena (53/53 bodies). What remains is **naming**: the savestate keys objects by name, and `local_birth_log`, `destroy_body`, `put_back_birth` (`GetObjectByNameAndParentClass`, first match; `session_host_support.hpp:317`) and `joints_on` (`script_replay.hpp:158-175`) all look objects up by name. Clone names must be unique and identical on all three hosts. The world and surfaces anchor hashes change.

**Costs**

- **L8 adds** 22 non-extra Type 1 copies (P_Modul_30 ×2, 18 ×4, 25 ×2, 29 ×2, 37 ×1, trafos ×11), 5 P_Box and 12 extras.
- **L11 adds** 8 stone balls plus its modules.
- **More selected replay scripts run for the whole session.** CPU needs measuring, because the deployed interval 2 is already at the CPU edge.
- **More props** mean more of the known pile residual.

**What it breaks.** Nothing in gameplay: it is the only option that keeps every sector as designed.

**Work.** About 3-5 days:
- a game/ header (planner ~250 lines plus a CK-free unit test of the row plan)
- 3 call sites, the protocol constant and the journal field
- SimTool honouring the flag

**Validation**

- **SimTool, L8:** the s1 see-saw tilts about 0.085 m with s2 up; a ball lands on the s2 see-saw; the s2 bridge sags about 2.7 m with s4 up; the s4 bridge exists.
- **Savestate check:** `--savestate-check --frames` across a later pass.
- **Four-client harness:** L8 and L11.
- **Retail:** a run that includes a second anchor without a reload.

### C. Hand-off or deactivating sectors

- **C1, hand the module forward.** When a later pass would re-place a live pooled object, first run its False branch (Hinge Shutdown → Unphysicalize → TT Restore IC), then place it. The pass tick is lockstep, so this is deterministic. But it:
  - takes the module away from stragglers in the earlier sector, the mirror image of A;
  - needs a guard exemption: the body guard swallows Unphysicalize on clients only (`Physicalize.cpp:230-238`; KeepsBodyOf `CKIpionManager.h:336-339`), so without an exemption like #13 the server tears down and the clients do not, and the worlds diverge;
  - brings back body death and re-birth inside rollback windows, the class N2 removed (`collision-overhaul-design.md:172`; `lockstep-without-snapshots.md:1112-1124`).

  The rollback half would still need the frame matrices recorded.
- **C2, deactivate sectors no member occupies.** Occupancy has to become a lockstep input (stamped sectors, leaves, late joins). It still fails in exactly the case that triggers the bug (leader in s2, straggler in s1), and it reverses N2's measured reason.
- **C3, the status quo.** Test only the arena. Every original level, L13 included, pools at least trafos, extras or fans.

C is dominated by B for gameplay and by A for cost.

### Which option the rollback half needs

The rollback findings:

- **R1. A window that crosses T_act keeps the post-activation hinge.**
  - `script_replay.hpp:853-863` at HEAD (`887-899` in the working tree) acts only when handle **presence** differs. Here both the anchor and the live world hold a handle, so nothing is reset and no counter fires.
  - The IVP savestate records constraints only as (priority, name) descriptors (`physics_savestate_capture.cpp:228-247, 424-427`).
  - So the replayed ticks a..T_act-1 run on the far hinge, while the live run and the server had the near one.
- **R2. The replayed activation rebuilds the hinge again.** `manager_activations::replay` (`session_host_support.hpp:220-238`, enabled via replay_activation at `core_host.cpp:1555` and `session_client.cpp:2684`) rebuilds the hinge from the replayed pose P'(T_act). The rest pose depends on that pose (1.159 / 1.503 / 1.377 m for activations at ticks 30 / 50 / 70), so an awake plank can leave the client with a different constraint from the server. No snapshot carries constraints, so corrections fight the client's own constraint.

**With A or B, neither R1 nor R2 can occur**, because a held module is never re-placed or re-hinged. Only if neither ships does rollback need:
1. joint identity (creation tick or generation) in the record, with a handle newer than the anchor reset;
2. the matrices of the unphysicalized frames (MF, HingeFrame) in the script record, restored before the savestate;
3. object-space constraint state in the savestate (an engine change) to recreate the anchor's hinge exactly.

The gameplay bug would remain even then.

## 5. Related findings

- **A late joiner loses the level's own sector-1 walk.** This is a real desync, independent of pooling, and reproduced in a networked run.
  - **Cause:** the union pacer starts idle (`sector_pacer.hpp:32-39`, kIdleTicks=3) and reads `Gameplay_SectorManager.IsActive()` before the step (`physics_world.cpp:788`, `client_core_frame.cpp:189`). The level's own sector-1 activation is only a pending next-frame flag at that point, so a sector owed at the anchor gets its pass at relative tick 2 and overwrites the walk. Sector 1's modules are never started, and the union, which holds 1 as active, never re-issues them.
  - **SimTool, L8 `--drop-second-sector 2`:** only sector-2 bodies exist. The Wippe is born at s2 (402.722, 22.371, -57.716), and Bridge001 and the s1 Wippe are missing (`audit2/v_base.txt`, `audit2/v_p2s2.txt`).
  - **Networked:** `maps/arena-test/lp1_join` (arena), with client3 joining late:

    > sector +2 at tick 1642 (stamped 858, held by the pacer)

    Client3 never resets P_Modul_01/18/41. The harness reports mechanisms 52/45 on client3 against 52/52 on the others; lp1_check shows the same.
  - **Who is hit:** every late joiner whose sector_history holds a sector above 1 (`server.cpp:1612`). Resynced members are not affected.
  - **Not repairable by snapshot:** a snapshot cannot create the missing bodies.
  - **Fix:** in the shared pacer/union, count the pending sector-1 activation as busy.
- **Pass-order dependence.** Today a pooled frame and hinge follow the **last** pass, and a pooled body stays where it was **first** physicalized. A joiner replays passes in ascending order. If members ran them out of order (only via the automation `sector N` verb or a write to the `[0][1]` cell, such as a sector cheat), the joiner's world differs from theirs. Snapshots fix body poses only. B and static A remove this; dynamic A and C keep it.
- **The existing checks cannot see this class.**
  - `--savestate-check` refuses `--drop-move` (`sim_tool.cpp:1712-1716`) and sets every activation up before its window.
  - `savestate_check.py`'s retail scenes are single-sector (`:68-83`), and the journals arm runs the arena (`:297, 491`).
  - `--level 8 --savestate-check --frames ... --drop-second-sector 2` gives `PASS rounds=24 failed=0` on a world with no s1 see-saw (`audit2/ssc_l8.txt`).
  - No journal from the union era crosses a sector on L8 or L11: there are no L8 v2/v3 journals, and all 384 v3 L11 journals have 0 Sector events. Server journal replay checks determinism, so a shared bug replays bit-exact (`p3_b_P_1`: matched 2587/2587).
  - Two checks are needed: a gameplay assertion (the pooled body matches its frame/joint after a later sector, i.e. the `--drop-move` sag against the 0.086 m single-sector tilt), and a rollback check whose window contains a Sector activation on L8 s1+s2 with the plank awake.

## 6. Uncertain or unverified

- **Retail CK2.dll.** "Reset does not send CKM_BEHAVIORRESET" is taken from Ballanced's reimplementation. A retail run across an L8 s1 → s2 crossing would settle it.
- **No guarded-client run across a crossing.** Neither the client-side entity snap-back nor what retail draws for sleeping pooled bodies was run: SimTool's `--sector` exists only in replay mode and `--body-guard` only in free-run. The client side rests on the code (`Physicalize.cpp:113-114`).
- **P_Modul_01 Rinne/Filler on the server.** The module's proximity-driven Physicalize/Unphysicalize cycle may re-create them at the new frame on the server, which has no guard, but not on clients. That would make P_Modul_01 a real server/client divergence. Not run.
- **Lost wake, two results.** The P_Modul_01 lost wake reproduced with the move at tick 0/20 (Pusher001 never woken). A separate run with sector 4 at tick 60 and a ball on Pusher001 matched the baseline. Both fit "only while the body is still asleep at the pass", but the exact window was not mapped.
- **Untested sag cases.** The L8 s1 bridge re-hinged 581 m away (sector 3) follows from the code path and was not measured under load. The L11 P_Modul_25 control moves only about 4 mm, so that case is weak.
- **Trafo and extra pickups.** Pickup after a move was not run. The Trafo Manager uses Get Nearest In Group, which is position-based. That losing the s1 wood trafos blocks L8 progress is plausible but not shown.
- **Slider, spring and force modules.** Behaviour of P_Modul_34 sliders, P_Modul_17 springs and P_Modul_08 forces under a later Shutdown/Create toggle was not checked.
- **Option B open points:**
  - which CK dependency flags the builder's CKDependencies holds (read it, do not hard-code it);
  - whether dynamic copies are destroyed at level unload;
  - whether anything reads the sector attribute (54) or group membership of P_ objects;
  - the CPU cost of the extra scripts.
- **Mismatched L11 P_Modul_26 coordinates.** The finding's exact coordinates depend on an unrecorded drop setup. The rerun (drop at 0 500 0) shows the same effect with different numbers.