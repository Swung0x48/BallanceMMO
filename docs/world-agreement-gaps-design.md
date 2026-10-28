# World agreement: client proximity sensing (G-c) and member lives (the join order)

Track B of the post-audit work. It closes two principle-(a) gaps that the
earlier designs found and left outside their tracks:

- **G-c**, client proximity sensors watch the own ball only
  (`docs/resync-and-sector-timing-design.md` §1.3, §9 Q6, §11);
- **the join order**, existing members never learn a late joiner's join order
  (`docs/trafo-replication-design.md` 9.7, §11: option (a), a member-added
  message).

It also fixes what designing and reviewing them turned up (§1.1.4, §1.2.3),
per principle (d). Revision 2 answers two reviews; §10 lists every finding and
what became of it.

Status: design, not built. Base: HEAD 4882ec2 (R2) with step P in the working
tree. **Every line number is the working tree's of 2026-10-23.** Step P is still
moving `client_core*.cpp`, `rollback.hpp`, `script_replay.hpp`,
`session_host_support.hpp`, `session_host_world.hpp`, `session_client.cpp` and
`core_host.cpp`, so re-check them at implementation. Scratch evidence is in
`C:/Users/GEEKER~1/AppData/Local/Temp/claude/D--repos-BallanceMMO/769d21ff-c9e2-42a0-a69a-d95ce0b06545/scratchpad/design2/trackB/`
(`census/`, `census.sh`, `census_table.py`, `body_series.py`, `lp1_revived.txt`).

Path key:

| key | file |
|---|---|
| PW | `BallanceMMOServer/sim/physics_world.cpp` (PWH: `.hpp`) |
| SRN | `BallanceMMOServer/sim/session_runner.cpp` (SRH: `.hpp`) |
| SV | `BallanceMMOServer/server.cpp` |
| SC | `BallanceMMOServer/sim/session_client.cpp` |
| ST | `BallanceMMOServer/sim/sim_tool.cpp` |
| CH | `BallanceMMOClient/session/core_host.cpp` |
| MC | `BallanceMMOClient/BallanceMMOClient.cpp` (legacy dispatch) |
| CC / CL / CF / CM | `BallanceMMOCommon/src/session/client_core.cpp` / `_lifecycle.cpp` / `_frame.cpp` / `_messages.cpp` |
| CJ | `BallanceMMOCommon/src/session/client_journal.cpp` |
| CP | `BallanceMMOCommon/include/session/client_platform.hpp` |
| HW | `BallanceMMOCommon/include/session/session_host_world.hpp` |
| LC | `BallanceMMOCommon/include/session/lifecycle.hpp` |
| RB | `BallanceMMOCommon/include/session/rollback.hpp` |
| RBS | `BallanceMMOCommon/include/session/rollback_binding_scripts.hpp` |
| SR | `BallanceMMOCommon/include/game/script_replay.hpp` |
| HS | `BallanceMMOCommon/include/game/session_host_support.hpp` |
| ES | `BallanceMMOCommon/include/entity/session.hpp` |
| EM | `BallanceMMOCommon/include/message/session_event_msg.hpp` |
| SPX | `submodule/Ballanced/Source/BuildingBlocks/TT_Toolbox_RT/Behaviors/ScaleableProximity.cpp` |
| PF | `submodule/Ballanced/Source/BuildingBlocks/physics_RT/Behaviors/PhysicsForce.cpp` |
| RSD / TRD / SPD | the resync, trafo-replication and sector-placement designs in `docs/` |

Hosts: **H** headless (SC), **RC** retail core host (CH), **RL** retail legacy
path, **SV** the server world.

## 1. Problem and rule

### 1.1 G-c: a remote ball wakes a mechanism in the server's world only

#### 1.1.1 What the code does

**Server.** At the anchor, `rewire_proximity_probes` (PW:351-381) walks every
behaviour. For each `TT Scaleable Proximity` whose ObjectA (input 1) resolves to
`Ball_Pos_Frame`, it creates a 3D frame `BMMO_Prox_<k>` and a parameter local, and
points the input at them with `SetDirectSource`. k counts the blocks in behaviour
list order. Every tick, `update_proximity_probes` (PW:386-415) runs from
`physics_world::tick` (PW:929). That is after the runner applied the tick's
events (SRN:820-910). It puts each frame on the physicalized player clone nearest
to the block's ObjectB, by full 3D squared distance. Ties go to the lowest player
id (`std::map players_`, PWH:313, strict `<`). With no physicalized clone, the
frame goes to the server's own `Ball_Pos_Frame`, the child of the parked retail
ball.

`rewire_ball_identity_reads` (PW:428-520) gives every `Get Cell(CurrentLevel,
ActiveBall)` inside a script with rewired probes a private one-row copy of
`CurrentLevel` (`CopyObject` with the suffix `"_" + name`, so the copy is
`CurrentLevel_BMMO_Ball_<k>`, PW:493-499). `update_ball_identity_reads`
(PW:529-560) fills the copy every tick with the nearest player, measured by 3D
distance to any of that script's ObjectBs:

- where the block's consumer tests the name (P_Modul_29's
  `Test == "Ball_Stone"`), the retail entity of that player's ball type;
- otherwise the player's clone (P_Modul_18's force target and box test).

With no physicalized player, the copy names the parked retail ball.

**Clients.** No client rewires anything. `TT Scaleable Proximity` appears only
in PW and in a comment of SR (SR:29-37). That comment still says that every
mechanism sensor measures the own ball's child frame. A client's mechanism
blocks therefore measure its own ball, and its fans and the rope bridge read the
own ball's identity.

**BodyRevived.** A client reports every script wake of a non-ball, non-piece body
in its live frames (CF:471-478, `reportable_wakeup`). The server applies the
report with `ensure_in_simulation()` at the applied tick (PW:887-901). The server
relays it to the other members (SV:1930-1934), and they ignore it (CL has no case
for type 4). So a wake that only the reporting client's world made reaches only
the server.

#### 1.1.2 Census (SimTool, every level)

`census.sh` lists every root script with SimTool `--list-scripts "*"` after the
level loads, and describes each one that contains a proximity block with
`--describe-script`. `census_table.py` then classifies the blocks by owner. The
mechanism class (owner `P_`/`PE_` minus `P_Extra`, minus the I3 balloon,
SR:188-195) holds the blocks this design rewires on clients. For every level, the
four classes sum to the server's own count (`census/server_rewire_counts.txt`).

| level | server rewires | mechanism | flames (PC_/PS_) | extras (P_Extra_*) | balloon | identity reads (P_Modul_18 body / P_Modul_29 name) |
|---|---|---|---|---|---|---|
| 1 | 18 | 3 | 7 | 6 | 2 | 0 / 0 |
| 2 | 47 | 32 | 7 | 6 | 2 | 9 / 1 |
| 3 | 32 | 15 | 7 | 8 | 2 | 3 / 1 |
| 4 | 30 | 12 | 7 | 9 | 2 | 3 / 1 |
| 5 | 28 | 14 | 7 | 5 | 2 | 2 / 0 |
| 6 | 43 | 29 | 7 | 5 | 2 | 8 / 2 |
| 7 | 39 | 25 | 7 | 5 | 2 | 4 / 2 |
| 8 | 29 | 14 | 7 | 6 | 2 | 2 / 1 |
| 9 | 43 | 27 | 7 | 7 | 2 | 5 / 1 |
| 10 | 47 | 32 | 7 | 6 | 2 | 5 / 2 |
| 11 | 47 | 32 | 7 | 6 | 2 | 5 / 1 |
| 12 | 60 | 43 | 7 | 8 | 2 | 11 / 0 |
| 13 (arena, md5 c58a76b9...) | 24 | 12 | 7 | 3 | 2 | 1 / 1 |

These counts are without placement copies. SPD option B multiplies the blocks
of each copied row. The mechanism column sums to 290 blocks.

Facts about the mechanism blocks. `census_table.py` multiplies one described
instance per (level, script name) by the instance count, so the facts below
were read on the **136 described blocks**, not on all 290. §7.2 V1 asserts them
on every block.

- All 136 have Barycenter = 0.
- Their `Check Axis` local is 5 (X and Z, 101 blocks) or 7 (X, Y and Z, 35
  blocks), and `Squared Distance` is 1.
- `Use Outputs` (local 2) selects which of In, Out, Enter, Exit Range the block
  has, numbered densely in that order (SPX:82-85, :263-271). So "output 0" is
  Enter Range on a block whose setting is 4, and output 1 is Enter Range on one
  whose setting is 5.
- What they drive (L11 descriptions, `census/l11.*.desc.txt`):
  - **One-shot wakes.** `In Range → Physics WakeUp` and `Off`: P_Modul_01, 03,
    19, 25, 29 (80 m), 30, 34 and 37.
  - **The fan, P_Modul_18** (`l11.P_Modul_18_MF_Script.desc.txt`).
    - The outer 80 m probe #45955 (setting 12: output 0 = Enter, 1 = Exit; axes
      5; ObjectB #45910) activates and deactivates the force sub-script.
    - The inner 7 m probe #46002 (setting 5: output 0 = In Range, 1 = Enter;
      axes 5; ObjectB #45910). Enter → `Get Cell` #45965 (ActiveBall): the
      identity is **latched at entry** in the Get Cell's output. In Range →
      `Box Box Intersection` #46013 (Entity 0 from that output, Entity 1 the
      `Kollisionsquader` #45915) → `SetPhysicsForce` #46048 Create or Shutdown
      on the same output.
    - A 25 m sound probe #46113 (axes 7, ObjectB #46274) drives only sound.
  - **The rope bridge, P_Modul_29** (`l11.P_Modul_29_MF_Script.desc.txt`).
    - The 80 m probe #47020 (setting 4, axes 5, ObjectB #47688) wakes the
      plates.
    - The 4 m probe #47265 (setting 4: output 0 = Enter; axes 7; ObjectB
      #47687). Enter → `Get Cell` #46995 → `Test "Ball_Stone"` → breaks the ten
      hinges.
- In both scripts the identity read is fed **directly** by one probe's output
  (#46002 → #45965, #47265 → #46995). That probe is the read's *gating probe*
  (§2.2).
- `SetPhysicsForce` creates a controller only when the block holds none (PF:200,
  `if (!controller)`), on its target as the deferred callback finds it
  (PF:139, :155-156).
- The `Gameplay_Tutorial` blocks (12, L1 only active) watch the ball entity
  itself (ObjectA = id 42240, the `Ball_Wood` that `PE_Balloon_Init` local 12
  names). They are not rewired anywhere (§9 Q5).

#### 1.1.3 Symptom (measured)

`maps/arena-test/lp1_join` is the arena with 4 players and snapshots every 2
ticks. The P_Modul_01 Pusher is first woken by client 2's ball. The server's
journal holds the three BodyRevived reports at 621 (p3323962937), 631
(p3896792023) and 637 (p3920120238). The Pusher's position, from `body_series.py`
over the journals:

| tick | server | client0 | client1 | client2 |
|---|---|---|---|---|
| 620 | (162.508,-1.485,43.650) | 0 | 0 | 0 |
| 624 | (162.505,-1.575,43.646) | 0.0897 m | 0.0897 m | 0 |
| 632 | | 0.0435 m | 0.0892 m | 0 |
| 636 | | 0.0019 m | 0.0888 m | 0 |
| 660 | | 0.0007 m | 0.0012 m | 0 |
| 688 | | 0 | 0.0001 m | 0 |

The server's world (its union probe) and client 2's world (its own ball) wake the
Pusher together. Clients 0 and 1 wake it only when their own balls arrive, 10 and
16 ticks later. Snapshots and those late own wakes pull the Pusher back, and a
millimetre residue lasts about 60 ticks. With snapshots off, nothing corrects it
until that client's ball arrives, and never if its ball never comes.

The same holds for every row of the census table. A remote stone ball on
P_Modul_29 breaks the bridge in the server's world and in the owner's world only.
A remote ball in a fan is pushed by the server and by its owner, but not by the
other clients' copies of that fan, so their mirror of it diverges.

*As measured at step SFD (snapshots off, nothing corrects it any more;
`rewind-cost-design.md` "As built (step SFD)", case 3).* `core_ref_s7`'s
`l8_cross` (level 8, 4 players, 0 ms): every P_Modul_29 plate and
P_Modul_01_Pusher is bit-equal to the server's row through 552 on every
client. At 554 c1 (2010866393, the ball within the 80 m probe #47020)
reports `BodyRevived P_Modul_29_Platte04` and `P_Modul_01_Pusher`; the
server applies it, c0 / c2 / c3 keep the plates asleep (row flags 2
against the server's 3) at their rest pose until c2's own probe fires at
614. The plates reach 1.4-1.8 m mean, worst 3.6 m (P_Modul_29_Platte05 at
574), on c0 / c2 / c3; c1's world agrees (within 0.02 m). The same
signature, smaller: `P_Modul_25_Bridge001` (c0's report at 407, 0.011 m on
c1-c3) and `P_Modul_37_Bridge` (c3's at 501, 0.01 m on c0-c2). On the
arena (`resync_snapoff` c1): P_Modul_01_Pusher 612, P_Modul_30_Wippe 1060
(0.34 m at 1100), P_Modul_34_Schiebestein 1276, P_Modul_37_Bridge 1358 and
P_Modul_03 1964 are woken on the server and asleep in c1's world.

#### 1.1.4 Found while designing (each fixed here)

- **G-c2, identity reads.** The `CurrentLevel[0][1]` reads of P_Modul_29 and
  P_Modul_18 are the second half of the same gap. They are audit #13's two
  consumers. Once clients rewire them, those reads no longer touch
  `CurrentLevel[0][1]`, and #13 closes for physics (§2.9).
- **G-c3, BodyRevived re-wakes the server's bodies at ticks no client does.**
  - lp1_join's server journal has 162 BodyRevived records (`lp1_revived.txt`):
    45 for P_Modul_08_Schaukel and 42 for P_Modul_26_Sack. Their Delayer and
    Sequencer chains wake them periodically in every world, and each wake is
    reported by every client.
  - 19 of the 162 were applied 2-19 ticks after their stamp, for example
    Schaukel `stamped 2001 applied 2017`.
  - `ensure_in_simulation()` resets the resting detection even on an awake
    body (engine change #14 text). So each late report changes the server's
    world at a tick that no client world matches.
  - A wake that only one client's world made is no longer a correct input
    either: every mechanism script already runs in every world (principle (a)).
  - This is late-player design §7 Q4 ("can it be retired?").
- **G-c4, the server's placement point cannot be matched.**
  - The server places its frames after applying the tick's events (PW:929 runs
    inside `world->tick`, SRN:911, after SRN:902). So a ball born at t counts at
    t, and a ball dying at t does not.
  - The owner's own birth and death happen inside its behaviour pass of t (core
    design §1.3), after any point where a client could place frames.
  - No client can therefore reproduce the server's rule for its own ball.
- **G-c5, the server ranks by a metric the blocks do not use.**
  - Frames: the server ranks by 3D distance for blocks that measure X and Z
    only. Take a ball A at XZ 40 m and 60 m below a 50 m XZ block (3D 72 m),
    and a ball B at XZ 55 m and the same height (3D 55 m). The server picks B,
    so the block reads "out of range" although A is in range under the block's
    own metric. Ranking by the block's own metric picks a ball that is in range
    whenever any ball is.
  - Identity: the server picks by 3D distance to *any* ObjectB of the script,
    not by the metric and the ObjectB of the probe that triggers the read
    (PW:529-560). On P_Modul_29 the 4 m probe uses #47687 with axes 7 while
    the script also holds #47688; on P_Modul_18 the 7 m probe uses #45910 with
    axes 5 while the script also holds the sound probe's #46274 (axes 7). A
    stone ball 3 m from #47687 can make the bridge's Enter fire while a wood
    ball nearer in 3D to #47688 is named: the Test fails and the bridge does
    not break.
- **G-c6, the server's no-ball fallback is a server-only object.** With no
  physicalized clone, the frames go to the server's `Ball_Pos_Frame`, and the
  identity copies name the parked retail ball (PW:392-393, :534). Neither has a
  counterpart in a client world, and on a client the retail ball is the own ball,
  which has a body.
- **G-c7, a latched identity can name a ball without a body, and the worlds keep
  different things under that name.**
  - The fan's Get Cell latches at Enter (§1.1.2). While any ball keeps the 7 m
    probe in range, no new Enter fires, so the latch can outlive the latched
    ball's body.
  - After a death, the server keeps the clone entity (PW:857-864 only
    unphysicalizes it), a client keeps the mirror entity, and the owner's
    retail death scripts may move its own ball entity. After a leave, the server
    destroys the clone (PW:727-733) and clients keep the mirror.
  - `Box Box Intersection` reads that entity's box every In Range check, so the
    worlds' force decisions can differ while the latch names a body-less ball.

#### 1.1.5 The rule

- **P1 (one sensor rule).** In every world (server, H, RC), every mechanism-class
  proximity block whose ObjectA is `Ball_Pos_Frame` measures a per-block frame.
  Before the behaviour pass of tick t, that frame is put by one shared function
  of:
  - the **candidate set** of t: the balls that have a body in that world at the
    end of t-1, with their poses there (§2.2);
  - the block's ObjectB position and its `Check Axis`;
  - the frame's previous position (the hold, §2.2).

  The rule is the same in every world. Identity reads take their gating
  probe's choice, and a latched identity never names a ball without a body
  after the tick its body went (§2.2).
- **P2 (prediction).** A client evaluates P1 on its own world's state at that
  point:
  - the own ball as simulated;
  - the mirrors as predicted or as replayed.

  When a relayed input, a lifecycle event or a snapshot corrects that state, the
  rollback replays the affected ticks, re-evaluating P1 at every replayed tick
  from the replayed state. After the replay, the sensors' state equals what the
  true inputs give, which is the server's.
- **P3 (exceptions).** Flames, extras, the I3 balloon and the tutorial keep their
  own-ball sensors on clients: they are per-player gameplay, or local by decision
  (c).
- **P4 (no side channel).** No world changes a body because another world's
  script woke it. BodyRevived is retired (G-c3).

### 1.2 Member lives: the join order of a late joiner, leaves and rejoins

#### 1.2.1 Symptom (measured, `maps/arena-test/lp1_join/sector1`)

- The late joiner p142763540 has join order 3 (client3.log: `join order 3`).
- The server (server.log:66) and the joiner (client3.log:327) kick its spawn at
  tick 1844 with index 60. Clients 0, 1 and 2 kick its mirror with index 56
  (client0.log:859, client1.log:1004, client2.log:780). The index is
  `(join_order + (h & 63)) & 63` (`spawn_impulse.hpp:123-131`, 64
  directions) with h & 63 = 57 for this seed and tick: 3 gives 60, the
  placeholder 63 gives 56. So the mirror is pushed in another direction in
  three worlds out of five.
- Client 0's journal records the joiner at tick 1600, from its relay frames, as
  `player +p142763540 join=255` with the note `late join: ... join order unknown`.
  The joiner's own journal has `join=3`.

#### 1.2.2 Cause

- `players_` is set only from the client's own SessionStart (CC:145). Existing
  members are never told about a newcomer: `physics_session_member_joined`
  (SV:1192-1230) sends SessionStart to the joiner alone.
- So, for the joiner, in every existing member's world:
  - its mirror's group is `P#63` (CL:468-470). The group ident is engine change
    #7's ball-pair order key (`ivp_mindist.cxx:278-286`), compared by `strcmp`.
  - its spawn index uses 63 (CL:475).
  - its events rank 256 in the drain order (CL:159-162, used at CL:111 and
    :390), where the server ranks by its real join order (SRN:840-841).

#### 1.2.3 Found while designing and reviewing (each fixed here)

- **J-b, the join-order reuse race.**
  - `physics_session_member_left` erases the leaver's join order at once
    (SV:1254).
  - The runner may keep the leaver in its world until its queued events are
    applied (SRN:375-386, `leaving`, removed at `remove_at` by `finish_leave`,
    SRN:779-784).
  - A joiner in that interval gets the freed order (`assign_join_order`,
    SV:802-809). The world then finds the slot taken and falls back to another
    one (PW:687-699, "falling back to slot"). The server's clone then has
    `P#<fallback>` and kicks with the fallback slot (PW:842-843), while the
    joiner and its SessionStart use the announced order.
  - The runner's drain rank also ties the two players on one order (SRN:840).
- **J-c, no member-removed path.**
  - A leaver's entry stays in every member's `players_` for good.
  - `client_journal::leave_player` (CJ:104-107) has no caller.
  - Offline, SimTool `--replay-session` adds unannounced players with a guessed
    order (ST:2861-2881), which decides the kick direction.
- **J-d, pre-start joiners.**
  - A member that joins between `world_ready` and the start barrier gets
    SessionStart (SV:1216-1226).
  - The members that already have theirs are not told about it either. This is
    the same gap without "late".
- **J-e, the server removes a leaver's ball at another point than every client
  (review).**
  - With a ball and nothing queued (the usual case), `remove_player` relays an
    Unphysicalize with applied = `next` (LC:461-470, SRN:348, relayed as
    `msg.tick = a.applied`, SV:1888) and then calls `finish_leave` at once
    (SRN:389). `world->remove_player` destroys the clone (PW:719-736) in the
    posted command, between steps, before step `next` and before every event
    of `next`.
  - Every client applies the relayed Unphysicalize at `next` inside its drain,
    in the join-order rank of the leaver (CL:105-111). When another player has
    a body change at `next` with a lower rank, the server deletes the leaver's
    body before making that one and every client after it. IVP creation and
    deletion order is world state (the 9.x insertion-order work), so that is a
    disagreement.
  - It also breaks P1: the server's candidate set at `next` lacks the leaver,
    while every client's world still has its body at the end of `next-1`.
- **J-f, a rejoin cuts the old life's relayed events (review).**
  - A member that leaves the room and rejoins on the same connection keeps its
    id (SV:526 and :2025: room leave and join call the member hooks; SV:1521-1525
    says a dropped member "can rejoin the room to late-join").
  - If the runner still holds the old life in `leaving`, `add_player` calls
    `finish_leave` at once (SRN:292-293), which erases that life's queued events
    (SRN:316-317). They were already relayed through `on_event_accepted`, so
    every client applies them and the server does not.
- **J-g, one id, two lives.** After J-f's case, an id stands for two players
  in a row with different join orders. Any client rule that keys a player by id
  alone (the group, the kick index, the rank, a removal tick) mixes them up.
- **J-h, the late joiner's base and a leaver it was never told about.** A joiner
  is not told about a member that left before it joined (it is gone from
  `s.members`, SV:1240), but the server's world may hold that player until R.
  The joiner's base (`current + join_lead`, SV:1590-1591) is not bounded below
  by R.

#### 1.2.4 The rule

A **life** of a player is the server's interval of that player in its world:
from the add tick J to the removal tick R. A player id can have several lives in
one session (J-f); they never overlap.

- **J1.** Before any event of a life is applied in any world, every world holds
  that life's `player_entry`: join order and spawn assignment. A life's group,
  kick index and drain rank are therefore the server's in every world.
- **J2.** A join order is never held by two lives in the server's world at once.
  An order is free again only after the runner has removed its previous holder.
- **J3.** Every event of a life has an applied tick in [J, R). The server
  guarantees it (§2.7); a client asserts it and never drops an event because of
  R.
- **J4.** A world that has never been told about a life never needs it: a late
  joiner's base is at or after the R of every life the server still held when
  the joiner was added.

## 2. Design

### 2.1 Parts

| part | what | where |
|---|---|---|
| A | the sensor rules, CK-free: metric, nearest, tie, hold, gating choice | new `BallanceMMOCommon/include/session/proximity_rules.hpp` |
| B | the CK side: rewire, gating probes, place, relatch, state section, signature, release | new `BallanceMMOCommon/include/game/proximity_union.hpp` (host support, public CK2 API only) |
| C | the server on parts A+B; placement before the leavers and the events; world option | PW, SRN, ST |
| D | the core: candidates per tick, live (pre_step step 7b) and in a replay (new `rollback_world::place_probes`, bound in HW) | CF, RB, CP, HW |
| E | hosts: anchor call, platform op, session end release | SC, CH |
| F | BodyRevived retired under the world option | CF, SV |
| G | member lives on the server: always-queued ball leaves, deferred rejoin, `SessionMember`, life numbers on events, join-order reservation, late-join send point and base | new `message/session_member_msg.hpp`, EM, ES, LC, SRN, SRH, SV |
| H | the core's member lives: `players_` as lives, parking, journal | CL, CM, CC, CJ, ST |

### 2.2 The sensor rules (part A, CK-free)

```cpp
namespace bmmo::session {
    struct probe_ball { uint32_t player; float position[3]; };
    // The block's own metric (SPX:166-186): z, then y, then x, each term
    // d*d in float, added in that order, only the axes in `axes` (bits
    // 1 = X, 2 = Y, 4 = Z).  Squared: the block compares squares.
    float probe_metric(const float a[3], const float b[3], uint8_t axes);
    // Index into `balls` (sorted by player id) of the smallest metric to
    // `reference`; the first (lowest id) on a tie; -1 when empty.
    int probe_nearest(const std::vector<probe_ball>& balls, const float reference[3], uint8_t axes);
}
```

**The candidate set at tick t.** These are the balls that have a body in this
world at the end of t-1, one per player at most, ordered by player id:

- On the server: every player clone with `physicalized && ball` at the top of
  step t, before the leavers loop and the events (§2.4). With J-e fixed (§2.7),
  nothing between two steps changes that set; a guard counts any difference to
  the set captured at the end of step t-1 (`probe_set_drift`, required 0).
- On a client: every ball entity of this world that has a physics body at the
  place point (CF step 7b live, the top of the replayed tick in a replay) and
  that the replay does not hold out of the world (§2.5). The ball entities are
  the own ball's (`own_ball_entities_`, CF:414) for the own player and the
  mirror entities (`mirror_entity(player, type)` for each type the player had)
  for the others. A mirror counts whether or not it has navigation.

At the place point no lifecycle change of t has been applied in any world:

- A ball born at t is excluded in every world. The owner makes it inside its
  behaviour pass of t, and the server applies its event after placing.
- A ball dying at t is included in every world, at its pose at the end of t-1.

**Frames (the probe rule).** If the set is non-empty, the frame goes to
`probe_nearest(set, ObjectB position, the block's Check Axis)`. Otherwise it
**keeps its position (the hold)**.

- Its initial position, at the rewire, is the level's first reset point,
  `ResetPoints` row 0. That is the same array in every world, and the server
  already reads it (PWH:182).
- Retail's frame sits there too until the first spawn.
- A hold is retail-like: vanilla's `Ball_Pos_Frame` stays with the dead ball.
  A fixed far point was rejected. A sensor near a spawn would then measure
  infinity at every respawn, reschedule to its maximum frame delay (60) and wake
  up to a second late.

**Identity (the cell of a private copy).** Each rewired identity read has one
**gating probe**: the rewired proximity block one of whose outputs is linked
directly to the Get Cell's input (§1.1.2). The cell takes **that probe's choice**
at t: the candidate `probe_nearest` picked for the gating probe's frame, with
the same metric, the same ObjectB and the same tie. The Get Cell runs in the same
behaviour pass, right after the probe's output, so the read names exactly the
ball that made the probe fire.

- **By-name kind** (P_Modul_29): if the set is non-empty, the retail entity of
  the chosen ball's type. Otherwise the cell keeps the entity it names (hold).
  Retail ball entities exist in every world and are never destroyed. The initial
  value is the entity `CurrentLevel[0][1]` names at the anchor, the level's
  start ball, the same in every world and covered by the signature (§2.3).
- **Body kind** (P_Modul_18): if the set is non-empty, the chosen player's ball
  entity in this world: the clone on the server, the own retail ball for the own
  player, the mirror entity for a remote. If the set is empty, `BMMO_NoBall`, a
  body-less 3D entity the rewire creates in every world, far below every level
  (0, -100000, 0), so a box test against it is false.
- The ball type of a candidate comes from the entity name, by prefix against
  the `Physicalize_GameBall` names. Server clones are `<retail name>_BMMO_<id>`
  (harness: `Ball_Stone_BMMO_142763541`), and mirrors are named by their type as
  well. This keeps the rule a function of names, which are the same in every
  world.

**Latch (body kind only, G-c7).** Before the pass of t, after the cells:

- if the Get Cell's output names an entity that is **not** the ball entity of a
  candidate of t, the output is set to the cell's value (the gating probe's
  choice, or `BMMO_NoBall`);
- otherwise it is left alone.

So the fan keeps retail's latch while the latched ball has a body, and from the
tick after its body went (death, trafo, leave) its consumers read a ball that
has one, or nothing. Single-ball play changes only where retail itself read a
dead ball's entity. The write stands for the Get Cell's own execution, so it
uses the block's own path (`SetOutputParameterObject`, which propagates to the
output's destinations as the block does); this is unlike the record restore,
which must not propagate (SR). The output is script state the record already
carries (audit §3), so replays restore it.

**What the rule does with several balls.** One frame, one latch and one force per
block serve one ball at a time: the fan pushes the latched ball while it is in
the box, and a second ball in range is not pushed until the latch moves; the
bridge tests the ball whose Enter fired, and a stone ball arriving while another
ball keeps the 4 m probe in range does not break it. These are the same in every
world, which is what P1 asks. Whether multi-ball play should get per-player
copies of these sub-graphs is a gameplay question for the user (§9 Q7); it is
not a world-agreement gap.

### 2.3 Host support (part B, `game/proximity_union.hpp`)

```cpp
namespace bmmo::game {
    enum class probe_class : uint8_t { mechanism, all };
    struct probe_candidate { uint32_t player; std::string entity; };
    class proximity_union {
    public:
        // At the anchor, after the sector placement's apply and the script
        // selection.  All or nothing: on failure nothing is rewired.
        std::string rewire(CKContext* ctx, probe_class which, const script_replay::selection& sel);
        // Before the behaviour pass of a tick: frames, identity cells and
        // latches from the candidates, sorted by player id.
        void place(CKContext* ctx, const std::vector<probe_candidate>& candidates);
        // The hold state for the rollback records (§4.2).
        void capture(std::vector<uint8_t>& out) const;
        bool restore(CKContext* ctx, const uint8_t* data, size_t size, std::string& error);
        uint64_t signature() const;      // §2.3 "Signature"
        int release(CKContext* ctx);     // sources back, frames, copies and BMMO_NoBall destroyed
        std::string describe() const;    // the anchor log lines
        uint64_t skipped() const;        // candidates without an entity or a body
        uint64_t relatched() const;
    };
}
```

**Which blocks.**

- The structural test is the server's: the prototype is
  `TT Scaleable Proximity`, and `GetInputParameter(1)->GetRealSource()` holds
  `Ball_Pos_Frame` (PW:362-367).
- `probe_class::mechanism` (clients) adds: the block's root script is in the
  session selection (`session_script_selection`, HS:324) and is not
  `keep_local`. That is RSD's I3 predicate in `session/resync_rules.hpp`, owner
  `PE_Balloon*`; that header is RSD's to build, and G3 depends on it (§8). This
  leaves out the trafo scripts, which have no proximity blocks, and
  Gameplay_SectorManager, which has none either.
- `probe_class::all` (server): every block the structural test finds, as today.
  Flames, extras and the balloon keep running on the server as they do now, and
  under the same placement rules. §6 says what that must not change.
- **Identity reads.** These are the `Get Cell` blocks whose target is
  `CurrentLevel` and whose column is `ActiveBall`, inside a script that has at
  least one rewired probe. The by-name or body classification is the server's
  (`operation_on`, PW:55, :483). The function moves into the header unchanged.
- **Gating probe.** The rewired probe whose output has a link to the Get Cell's
  input (`CKBehaviorIO` links of the owning graph). Exactly one is required. A
  read with none or with two makes `rewire` fail with the script and the block
  named; the anchor then fails as a signature mismatch would. V1 asserts exactly
  one on L1-L12 and the arena.

**Names (the same in every world).**

- A frame is `BMMO_Prox_<owner name>_<n>`, where n is the block's position in
  a pre-order walk of the owner's root script in sub-behaviour index order. That
  is the walk RSD's portable record already keys blocks by (RSD §2.4).
- An identity copy is made by `CopyObject(CurrentLevel)` with the suffix
  `"#BMMOId_<owner name>_<n>"` (the Get Cell's walk position), so its name is
  `CurrentLevel#BMMOId_<owner>_<n>`.
- `BMMO_NoBall` is a single entity.
- Owner names are unique:
  - the builder's copies are `*_MF001`...;
  - SPD's copies are `_R<row>` (SPD §2.3 step 5);
  - RSD §2.4 checked the (owner, script) keys on L1-L12 and the arena.
- No name contains `_BMMO_` or `_Peer_`, so the server's snapshot filter and
  the mirror filter never see them (TRD §2.4 explains the same choice). Today's
  server copies (`CurrentLevel_BMMO_Ball_<k>`) do contain `_BMMO_`; they are
  data arrays, which neither filter reads, and the new names drop it anyway.
- None has a body.
- The server's names change from `BMMO_Prox_<k>`. Nothing reads them but logs.

**Rewire, per block.**

1. Remember the input's direct source and shared source
   (`GetDirectSource()`, `GetSharedSource()`). `release` puts back whichever was
   set.
2. Create the frame with `CreateObject(CKCID_3DENTITY, name)` at the first reset
   point, with no parent and no mesh.
3. Create a `CKParameterLocal` of `CKPGUID_3DENTITY` holding its id, and call
   `SetDirectSource(local)` (PW:369-377).
4. Read `Check Axis` (local 3) and ObjectB (input 2), and refuse a block with
   Barycenter = 1. None exists among the described blocks (§1.1.2, V1 checks
   all); refusing keeps the metric honest if a custom map adds one.

**Identity copies.** As PW:486-511: `CopyObject(CurrentLevel)` with custom
dependencies (data, names), one row ensured, a parameter local, and
`SetDirectSource` on the Get Cell's target. Only the ActiveBall cell of row 0
is ever read through a copy, because the rewire only takes Get Cells of that
column. The stale other columns are therefore irrelevant.

**Place.**

1. Resolve each candidate's entity (`GetObjectByNameAndParentClass`) and its
   `GetPosition`.
2. Skip a candidate whose entity is missing or has no physics object, and count
   it (`skipped`). The core only passes entities it found with a body, so this
   is a consistency check; V4 and H4 require 0.
3. Per frame: §2.2's probe rule, `SetPosition` with the ball's position (an
   exact copy).
4. Per identity copy: the gating probe's choice (`SetElementObject(0, column,
   entity)`), or the hold, or `BMMO_NoBall`.
5. Per body-kind Get Cell: the latch rule (§2.2), counted in `relatched`.

**Signature.** It is a 64-bit FNV over, in walk order per owner:

- the frame name;
- the ObjectB name;
- Distance, axes, `Use Outputs` and the min/max exactness inputs;
- the identity copies' names, kinds and their gating probes' frame names;
- the initial by-name entity name;
- the reset point's bytes.

Computed for the mechanism class in every world (the server too), it goes into
SessionReady (§5). A world that rewired a different block set cannot join.

**Release.** It runs at session end in every client host and at world teardown on
the server. On retail this matters:

- the level outlives the session (SPD §11 Q3: "leave the level");
- vanilla play afterwards would otherwise run its mechanisms on frames nobody
  moves;
- the next anchor's structural test would find no `Ball_Pos_Frame` sources.

**Retail CK2.** Every call above is public SDK API that the server already uses
against Ballanced (PW:351-381, :428-520) and SPD uses against retail CK2. The
retail mod keeps the `proximity_union` object as a file-scope static in CH, as it
does for the placement registry (the BML constraint, core design §3.2).

### 2.4 Where it runs

| world | anchor call | per-tick call | release |
|---|---|---|---|
| SV | `physics_world::anchor` replaces PW:239-240 (`rewire_proximity_probes`, `rewire_ball_identity_reads`) with `rewire(all)`; after SPD's placement (SPD §2.4) | **moved** out of `physics_world::tick` (PW:929-930) into a new `physics_world::place_probes()` that `session_runner::step` calls first, before the leavers loop (SRN:779-784) and the events (SRN:820-910). Candidates: `players_` with `physicalized && ball`, id order | world teardown |
| H | SC `anchor()`, right after the selection (SC:1475-1479); after SPD's apply | core pre_step step 7b and `rollback_world::place_probes` → platform `place_probes` | session end |
| RC | `core_host_anchor`, right after the selection (CH:883-885); after SPD's apply | as H | `core_host_end_local` (CH:1051) and level exit |
| RL | none: a SessionStart with `WORLD_PROXIMITY_UNION` ends the legacy session locally with "the proximity union needs the session core (session core on)", as SPD does for its option | | |
| SimTool | every `physics_world` mode through SV's path; `--replay-session` calls `place_probes()` at the top of each tick, before that tick's PLAYER and EVENT records, when the header's world option has the bit, and the old per-tick update otherwise | | |

The server and SimTool place at the same point: the live server has no removal
of a ball holder between steps any more (§2.7), and a rejoin's removal and add
run in the leavers loop, after `place_probes` (§2.7). With the world option clear,
the server keeps today's code path bit for bit, so old journals replay
unchanged.

### 2.5 The core (part D)

**Candidates.** `probe_candidates(held_out)` returns (player id, entity) sorted
by id:

- the own player, with the entity of `own_ball_entities_` (CF:414) that has a
  body (`platform_.body_state`, CP:324);
- each player of `remotes_` with the entity of its mirror entities (one per
  type it had, `mirror_entity`, kept in `remote_ball::entities`) that has a body
  and is not `held_out`.

More than one body for a player at the place point is an invariant violation:
counted (`probe_two_bodies`) and logged once; the lower-type entity is taken.
Nothing here reads `navigation`, the rollback record or `remote.physicalized`,
which in a replay describe the live state, not the replayed tick's (review).

**Live.** A new pre_step step **7b** runs between `hooks.after_rewinds` (CF:320)
and `apply_held_lifecycle` (CF:325):

```
platform_.place_probes(probe_candidates(nothing held out))
```

At that point every rewind of the frame has run, so the world is the end of T-1
as replayed. No lifecycle change of T has been applied yet: held births are made
at step 8, and own births and deaths come later still, in the behaviour pass.
This is the server's "before the leavers and the events" point.

**Replay.** RB gains a hook, part of `core_world_hooks` (CP:133) and bound by
`bind_session_world` (HW:264ff) like `replay_remotes` (HW:279):

```cpp
// Before anything of replayed tick t: the window-born re-creations, pre_step
// and the step.  `held_out(entity)`: the replay still holds that body out of
// the world (its birth record's tick >= t).
std::function<void(uint32_t tick, const std::function<bool(const std::string&)>& held_out)> place_probes;
```

It is called at the top of each replayed tick in RB's loop, before the
`born_now` insertions and re-creations (RB:1190-1231) and `world.pre_step(t)`
(RB:1233). The core's implementation calls `platform_.place_probes(
probe_candidates(held_out))`. Every replay path calls it:

- the input rewind;
- the lifecycle rewind;
- the snapshot rollback;
- RSD's resync re-step and late-Sector rewind, which run through `rewind_to` and
  `resync`.

**Platform.** CP gains a required op:
`std::function<void(const std::vector<bmmo::game::probe_candidate>&)> place_probes`
(the type moves to a CK-free header so CP stays CK-free). It also goes into the
"missing op" check (CP:531 style). H and RC implement it as
`proximity_union::place`. No capability: a host that cannot place cannot keep
P1, so the session must not start (§5 signature).

### 2.6 BodyRevived retired (part F)

With `WORLD_PROXIMITY_UNION` set:

- The core does not send BodyRevived (CF:471-478 behind
  `world_options() & WORLD_PROXIMITY_UNION`). `reportable_wakeup` stays for
  diagnostics: the count is kept in the status line (`revived_local=`).
- The server drops any BodyRevived it still receives in `handle_session_event`
  (SV:1802). It submits and relays nothing, counts the drop (`revived_ignored`)
  and journals one NOTE per player for the first one.
- Event type 4 keeps its wire value. Old journals apply it as before when their
  header lacks the bit.

The required evidence (§7.3 H6): with the bit, the mechanisms' rows, sleep flag
included, agree between the server and every client. In a same-batch control
where the server still applies the reports, any disagreement it shows is the
measured cost of G-c3.

### 2.7 Member lives on the server (part G)

**Life numbers.** The runner counts adds per player per session
(`session_state::lives`, SRN); the n-th `world->add_player` of an id is life n,
from 1. `accepted_event` (SRH:81) gains `life`, filled at acceptance (and for the
leaver's Unphysicalize, the leaving life's).

**Every leave with a ball goes through the queue (J-e).** In `remove_player`
(SRN:340-390), `has_ball` alone now inserts the Unphysicalize into `s.events`
(today `has_ball && queued`, SRN:351), and every player with a ball or with
queued events goes to `leaving`, with `remove_at =
lifecycle_leaver_removal_tick(...)` (LC:196-201), which is then at least
`next + 1`. Consequences:

- The server applies the leaver's death at `next` inside the events loop, in
  the leaver's rank, as every client does. Its journal gets the EVENT record at
  `next` and the PLAYER(-) at `next + 1`; SimTool already replays that shape
  (the queued case of today).
- `place_probes()` at `next` sees the clone with its body; at `next + 1` the
  clone has none. That is the client's candidate set at both ticks.
- The leaver's input at `next` comes from `leaving.frame` (SRN:812-814), as in
  the queued case today; its ball has no body by the step.
- A player without a ball and without queued events is removed at once, as
  today: it has nothing any world must agree on.

**A rejoin waits for its old life (J-f).** `add_player` (SRN:286-304) for an id
in `leaving` no longer calls `finish_leave`. It stores the add in the leaving
entry (`rejoin = {join_order, name}`); the leavers loop at `remove_at` runs
`finish_leave` and then the add, so the new life's J equals the old life's R and
no relayed event is erased. Events of the id that reach `submit_event` before
that add are refused today (SRN:621, "removed before its event got here"); with
the send point below, the rejoiner cannot produce any (it has no SessionStart
yet).

**Send points.** Runner callbacks on the simulation thread, under
`state_mutex_` as `on_event_accepted` (SV:1861):

- `on_player_added(session, player, life, join_order, J)` at the end of every
  successful add (the posted add, or the deferred one in the leavers loop);
  `J = world->tick_index()`, the tick of its PLAYER record (SRN:303).
- `on_player_add_failed(session, player, error)` when `world->add_player`
  fails (SRN:295-298).
- `on_player_removed(session, player, life, join_order, R)` at the end of
  `finish_leave` (SRN:306-319), with the tick of its PLAYER record and the
  removed life's join order, read before `s.join_orders.erase`.

What the server does there:

1. `on_player_added`, for a late join (`s.late`): **send the joiner its
   SessionStart here**, not in `physics_session_member_joined` (SV:1214-1216).
   The joiner then hears of the session only after the runner holds it, so
   nothing of the joiner can precede its add (and J-f's waiting rejoin produces
   nothing early). A pre-start joiner keeps today's send in
   `physics_session_member_joined`.
2. `on_player_added`, always: send `SessionMember(added)` to every member in
   `started` (a new set, filled by `send_session_start`) except the player.
3. `on_player_add_failed`: SessionEnd to that member, with the error, and
   `physics_session_member_left` for it. Today such a member got a SessionStart
   for a world that has no slot for it.
4. `on_player_removed`: send `SessionMember(removed)` to every member in
   `started`, and release the life's join order (below).

Every `SessionMember` is also pushed through `s.event_repeats` (SV:1943), like a
lifecycle event: a lost reliable copy is about 400 ms late by retransmit
(SV:1936-1940), past the rollback's reach. The client keys member messages, so
copies are idempotent (§2.8).

**Wire.** A new opcode, `SessionMember`, is appended to `opcode` after the last
session opcode in place at implementation (today `SessionLeadAck`, message.hpp:99).

```cpp
struct session_member_msg : public serializable_message {   // server -> client, ReliableNoNagle + repeats
    uint32_t session = 0;
    uint8_t kind = 0;                      // 1 added, 2 removed
    uint32_t tick = 0;                     // added: J; removed: R
    session::player_entry entry{};         // ES:133-139 plus `life`; removed: id, life and join_order
};
```

`player_entry` (ES:133-139) gains `uint16_t life`, so SessionStart lists each
member's current life too. `session_event_msg` (EM:19-35) gains `uint16_t life`
after `state` in its trailer: the server fills it on relay, a client sends 0.

The reader refuses:

- `kind` outside {1, 2};
- `join_order >= MAX_PLAYERS_PER_SESSION`;
- an entry id of 0 or a life of 0.

**Join-order reservation (J2).**

- `physics_session_state` gains `std::set<uint8_t> retiring_orders`.
- `physics_session_member_left` moves the leaver's order from `join_orders` into
  `retiring_orders` instead of erasing it (SV:1254).
- `assign_join_order` (SV:802-809) treats retiring orders as used.
- `on_player_removed` releases the removed life's order.
- PW's "falling back to slot" (PW:690-697) then becomes an invariant violation:
  logged as `ERROR join order N taken in the world` and counted. §7.3 J4
  requires zero.

**The late joiner's base (J4, J-h).** The runner exposes
`leaving_horizon(session)`: the largest `remove_at` in `leaving`, 0 when none (a
view value, like `current_tick`). A late joiner's first assignment becomes
`max(current_tick + join_lead, leaving_horizon)` (SV:1590-1591). Every life the
joiner was not told about ends at or before its base, and its events all apply
before their R (J3). So the joiner's world from its base never needs them.

**Why J3 holds.** Events of a life are accepted only while the runner holds that
life (`s.players`, SRN:621) and at `applied = max(stamp, next)` (SRN:628-640),
so at or after J. Events accepted for a leaver are queued, and `remove_at` is
one past the last of them (LC:196-201), so before R. The deferred rejoin keeps
the old life's events (J-f).

**Why `SessionMember(added)` comes before the joiner's events.**

1. The add runs on the simulation thread; `on_player_added` sends the message
   before the callback returns.
2. Every event of the new life is accepted later on that thread
   (`submit_event`), and relayed from `on_event_accepted`.
3. Both go to each member reliably on lane 0 of its GNS connection, which keeps
   send order (SessionStart uses `Reliable`, SV:962; the others
   `ReliableNoNagle`).

What can still arrive first: the unreliable repeats of the joiner's events and
its frames in `SessionRemoteInput`, when the reliable `SessionMember` and its
repeats are all lost or late. §2.8 parks those.

**Pre-start joiners (J-d)** go through the same send point. A member that got
its SessionStart before the join is in `started`. A member whose SessionStart is
sent after the join gets an entry list that already holds the joiner, plus
possibly a `SessionMember` copy, which is idempotent.

### 2.8 The core's member lives (part H)

**State.** `players_` (CC:145) becomes a list of lives:

```cpp
struct member_life {
    bmmo::session::player_entry entry;   // id, join order, spawn, life
    uint32_t from = 0;                   // J; 0 for a life listed in SessionStart
    uint32_t until = 0;                  // R; 0 while open
};
```

Nothing is erased during a session: a closed life is still read by replays that
cross its R (`drain_pending_births`, CL:381-405; `physicalize_remote`'s group and
kick index, CL:468-475), and relay frames and `remotes_` are kept as today, so a
rollback anchored before R drives the leaver's mirror from its ring (review).
The list is bounded by the session's joins.

Every lookup that used the id alone takes the event's life: `join_rank(player,
life)` (CL:159-162), the group and kick index (CL:468-475, via `pending_birth`
gaining `life`), `announce_player`. The own player's lookups use its own life.

**`on_session_member(msg)`** (decoded by the hosts, like the other messages; core
design E3; SC:815-830, CH:1090-1110, CH:491-515 mailbox). It is accepted in every
phase after `begin()` of the same session (`msg.session == s.session_`). Unlike
`on_session_event` (CL:51), it is **not** dropped while loading: a lost member
entry would be lost for good, and nothing re-sends it after SessionReady.

- **added (p, life n), unknown:** add the life with `from = J`.
  - Journal: `PLAYER +p join=k` **at J** (clamped to the journal's first tick),
    and `NOTE member +p life n join k (server tick J)`.
  - Move p's parked events of life n (below) into `pending_events_`. Each one is
    drained at its tick. One whose tick has passed takes the existing late path
    on the next drain: CL:96-138 → `apply_event` → `rewind_for_lifecycle` to
    stamp-1, or `request_resync` when the history does not reach it. Its mirror
    is then made at its stamp with the real group, kick index and rank.
  - The trafo design's set for p is built here, per life (TRD §2.4 "for a player
    first seen in a message"), outside any replay.
- **added (p, n), known with the same entry:** a copy, ignored.
- **added (p, n), known with a different join order:** a protocol error. A
  mirror group can no longer be trusted, and no resync carries `players_`. The
  core ends the session locally ("member p life n re-announced with join order
  X, had Y"), journaled.
- **removed (p, n, R):** set `until = R` on that life (a copy finds it set and
  is ignored). If the life is not known yet (its `added` copies all late), keep
  `(p, n, R)` and apply it when the life arrives. Journal `PLAYER -p` at R
  (`client_journal::leave_player`, CJ:104-107, gets its caller and a tick
  argument). Nothing else happens: the life's Unphysicalize, at R-1 or earlier,
  is an ordinary event that every world applies (§2.7).

Out-of-order copies are harmless: a life is keyed by (player, life), never by
arrival order or by tick.

**Parking (the refusal rule).**

- In `on_session_event` (CL:49-77), after the duplicate check and before
  `pending_events_.push_back`: if (`msg.player`, `msg.life`) is not a known life,
  the event goes to `s.parked_events_[(player, life)]` and is counted
  (`events_parked`).
- So no event of an unknown life is ever applied, and **no mirror is ever made
  with the placeholder**. A re-make is never needed.
- An event of a known life whose tick is outside [J, R) is applied as stamped
  (the server applied it), counted (`member_life_outside`) and logged. J3 makes
  it impossible; §7.3 requires 0.
- `join_rank`'s 256 (CL:161) stays as the defensive value, and so does
  `P#63` / 63 at CL:468-475. Neither is reachable after this change. §7.1 U5
  asserts that the "mirror of a life outside `players_`" counter stays 0.
- Relay frames of an unknown player are stored as today (CM:338): inputs do not
  depend on the join order, and two lives of one id never share a tick. But
  `announce_player` (CJ:93-102) writes no `PLAYER ... join=255` any more. The
  PLAYER record is the member message's.
- A parked event whose member message never comes is dropped at session end,
  with its count in the end note. The server cannot produce this case (§2.7).

**Own entry.** A `SessionMember` naming the own id is ignored and logged.

**Legacy path (RL).** It cannot keep J1 (it has no core). At protocol ≥ P_M it
ends the session locally on the first `SessionMember(added)` that names another
player: "a member joined late; join orders need the session core (session core
on)". Without late joiners nothing changes. §9 Q8 asks the user whether that is
the wanted trade until R7 deletes the path.

**SimTool.** `--replay-session` of a client journal now finds PLAYER(+) at J, at
or before every record of that life, so `ensure_player` (ST:2861-2881) is not
reached for it. Within a tick, PLAYER records are applied in file order before
the tick's inputs and events (a rejoin writes PLAYER(-) then PLAYER(+) at one
tick). `ensure_player` stays for old journals.

### 2.9 Interplay with the other designs and the audit fixes

| item | interplay |
|---|---|
| SPD (placement copies) | Rewire runs after `apply`, so every copy's probes and reads are rewired. The names use the copies' unique owner names. The signature covers them. |
| RSD A1 (portable record) | Frames and identity copies have the same name in every world, so the hold section (§4.2) is portable by name. RSD lists `BMMO_Prox_*` / `BMMO_Ball_*` as unportable (RSD §2.4); that sentence becomes "portable since this design". An object reference to a **player ball** (the latched Get Cell output naming a clone on the server) gains a portable kind 4, (player id, ball type), localized to that player's ball entity in this world; `BMMO_NoBall` is portable by name. Kinds 1-3 are unchanged. |
| RSD I1 / G-c note | RSD §1.3 says I1 holds for the imported Proximity state at S "and not after it". With this design it holds after S too. |
| RSD `resync_rules.hpp` | the `keep_local` predicate (§2.3); G3 needs it built. |
| TRD | Sets are built at the member message, per life (§2.8), instead of at the first event. There is no other contact: pieces are mechanisms, not candidates. |
| audit #13 / F9 | The two physics readers of `CurrentLevel[0][1]` are the rewired identity reads. On clients they now read private copies, which every replayed tick re-derives (body kind) or which the record restores (by-name hold). What remains on the original cell is `Ball_Shadow`, `Event_handler` and `Sound_Manager` (census "left on the retail array"), and they move no body. F9 reduces to its unit test, which now asserts that a cell change inside a window cannot change a replayed rewired read (§7.1 U3). |
| audit #6 / F5 | Under G-c, the fan's `SetPhysicsForce` Create runs in every world against the latched ball, mirrors included. Its PreSimulate callback waits for a body (PF:155-158 returns 0 without one), and a second Create while the first is still queued queues another (PF:210-221): retail behaviour, the same in every world once the queue is restored. F5 (the queue restored for selected scripts) is a prerequisite (§8). |
| audit #7 / F6, #8 / F7 | A replay that lacks a body its window had (#7), or an own record with a wrong generation (#8), gives a wrong candidate set at the replayed tick, because candidates are the replayed world's bodies. F6 and F7 land first (§8). |
| RC `lifecycle_rewind = false` (CH:597) | A late mirror birth or death is applied at the live tick, not at its stamp. RC's candidate set then lags the server's by that lateness. R3-R5 (the user's order: before G-c) remove it. |
| late-player plan step 22 (own MOVED death) | An own Unphysicalize the server applied later than stamped leaves the server's clone alive for those ticks (CL:1062-1069 keeps it where stamped). Candidate sets then differ because the bodies differ; P1 inherits that gap and adds none. It is step 22's to fix. |

## 3. Determinism

1. **Same candidate set.**
   - Client: the ball bodies of its world at the place point, minus the bodies a
     replay holds out (§2.5).
   - Server: physicalized clones at the top of the step (§2.4).

   These agree whenever the body sets agree, which the protocol-9 event order
   already guarantees (stamps, origins, join-order ranks); this design adds the
   joiner's rank (J1) and the leaver's death point (J-e). On RC this needs
   `lifecycle_rewind` on (§2.9).
2. **Same order.** The candidates are sorted by player id, a u32 every world
   knows. Ties go to the first. There is no container iteration order in the
   rule.
3. **Same arithmetic.**
   - `probe_metric` and `probe_nearest` are header code compiled into the
     three targets (x64 MSVC, GCC, x86 MSVC). They use plain float multiplies
     and adds in a fixed order and call no VxMath. The project already relies
     on bit-equal float arithmetic across these three targets for the physics
     (engine changes #1, #3, #11).
   - The comparison runs on floats that are themselves bit-equal: the ball
     entities' CK positions come from physics_RT's matrix write. The savestate
     restores them (audit §3), and the bit-exact retail/server runs compare
     them.
   - §7.1 U1 pins the results on fixed vectors in all three builds.
4. **Same block set and same initial state.** The signature (§2.3) in
   SessionReady refuses a world whose rewire differs. It covers the block list,
   the parameters, the gating probes, the reset point and the start-ball entity.
5. **Exact frame writes.** `SetPosition` copies the ball's position into an
   unparented frame, so world equals local and nothing is rounded.
6. **No CK-id dependence.** Names come from (owner, walk position). Creation
   order follows behaviour list order and is world-dependent, but nothing reads
   it: frames have no body and are not in the world hash. Latch comparisons are
   by entity, resolved per world from (player, type).
7. **The block's own arithmetic.** On retail, `TT Scaleable Proximity` is the
   original TT_Toolbox_RT DLL. On the server and H it is Ballanced's
   reimplementation (SPX). The two read bit-equal inputs, but their float
   evaluation (x87 or SSE) is not proven equal. This is not new: the own-ball
   sensors depended on it before. §9 Q1 settles it.
8. **The owner's in-pass lifecycle.** The owner applies its own death inside the
   behaviour pass of t. Every other world applies it before the pass. P1 is
   immune, because it samples at the end of t-1. The fan's force is the one
   consumer that acts on the body during t: its Create is deferred to the
   PreSimulate of step t (PF:210-221), which comes after the whole behaviour
   pass in every world, so in every world it finds the dying ball without a
   body and stays queued (PF:155-158). The box test at t reads the latched
   entity's box: the owner's own ball entity at t may already have been moved
   by its death script earlier in the same pass. §9 Q6 measures it; from t+1
   the latch rule removes the dead ball.
9. **Member lives.** Every lookup is by (player, life), and a life's entry is the
   server's. The relayed `life` and the member messages are the server's
   numbers, so the mapping cannot differ between worlds.

## 4. Rollback interplay

### 4.1 Per tick

The frames, the body-kind identity cells and the latches are recomputed before
every live and every replayed tick from that tick's candidate set (§2.5). They
need no record except the hold (§4.2).

The Proximity blocks' own state is already in the script record (audit §3:
"Delayer, Sequencer and Proximity locals and outputs"), so are the Get Cells'
outputs (the latch), and so are the WakeUps' effects (blob: sleep, calm refs,
unit countdowns). A wake that the live frame made from a mispredicted mirror is
therefore undone by the replay, and the corrected one is made there.

### 4.2 The hold section in the script record

TRD takes the script record's `kVersion` 2 → 3 (TRD:940-946, :1191; step T1).
This design takes **the next version after the last one landed** when G3 lands:
4 if TRD's T1 is in, else 3, and TRD then takes the next. The section follows
the cells (and TRD's `trafo` list when present):

```
u16 frames;     per frame (walk order per owner, selection order): f32 x, y, z
u16 identities; per by-name copy: u32 object id of the held retail entity
```

- `capture` writes the section from `proximity_union::capture`.
- `restore` puts the frames and by-name cells back at the anchor. Without that,
  a no-candidate tick inside the window would read the live hold.
- `diff` reports a frame difference as logical. A frame that differs is a
  candidate-set difference, not float noise.
- RSD's portable record carries the section by name (§2.9), in its next version
  after RSD's own and TRD's.
- At L12's 43 frames and 0 by-name copies the section is 2 + 43×12 + 2 = 520
  bytes per record.

### 4.3 Savestate

Frames, identity copies and `BMMO_NoBall` have no body. The blob neither contains
them nor refuses on them. The PreSimulate queue entries that the fan creates
against mirrors are F5's business (§2.9). No engine change, no IVP change.

### 4.4 Window edges

- **Births inside the window.** A body the replay holds out is not a candidate
  until its re-pose, which is the live rule.
- **Deaths inside the window (#7).** The replay must have the body until the
  death's stamp. Until F6 it may lack it, and the frame goes to the next-nearest
  ball; that is a wrong replayed tick, the same one #7 already causes for the
  physics. F6 is ordered first (§8).
- **Resync and late join (RSD A3/A5).** The re-step from S calls the hook at
  S+1..B. The hold at S comes from the server's portable record.
- **Member lives inside a window.** `players_` changes outside replays, on
  message arrival, and only grows or closes lives. A closed life stays readable,
  so a replay across its R finds its entry, its relay frames and its mirror
  state (§2.8). Parking guarantees that no replayed event of a life predates
  that life's entry.

## 5. Wire, protocol and journal

Protocol numbers follow RSD §11 Q5's rule (the next free number at landing).
The three earlier designs claim 10. This design takes two:

- **P_M** for M1 (member lives; 11 if the three land as one 10);
- **P_G** for G3 (the proximity union and `anchor_probes`; P_M + 1, or the next
  free number then). G5 lands in the same release as G3 and shares P_G.

So no build ever reports a number whose fields it does not send.

| item | change |
|---|---|
| `PHYSICS_PROTOCOL_VERSION` (ES:51) | P_M: "SessionMember (member lives: added/removed with player_entry and life); player_entry.life; session_event_msg.life; ball leaves always queued". P_G: "WORLD_PROXIMITY_UNION: clients rewire mechanism proximity blocks and identity reads, every world places them before the leavers and the events by the block's metric with a hold; BodyRevived retired; SessionReady carries the probe signature" |
| `entity/session.hpp` | `player_entry.life` (P_M); `WORLD_PROXIMITY_UNION`, the next free `world_options` bit after SPD's `WORLD_PLACEMENT_CLONES` (bit 0) and TRD's `WORLD_TRAFO_INPUTS`: bit 2 (P_G) |
| opcode | `SessionMember`, appended (message.hpp:85-100 order rule) |
| `session_member_msg` | §2.7; ReliableNoNagle plus `event_repeats`; about 47 bytes |
| `session_event_msg` | `uint16_t life` after `state` (P_M); 0 from clients |
| `session_ready_msg` | `uint64_t anchor_probes` appended after SPD's `anchor_placement`, read when protocol ≥ P_G, always filled (P_G) |
| server `handle_session_ready` | compares `anchor_probes` with the server's mechanism-class signature after the placement compare (SPD §5), late joiners included: "probe mismatch for X (client %016llx, server %016llx)"; late base clamp (§2.7) |
| server config | `physics_proximity_union`, default on from step G3; the negative control and old-journal replays use off |
| BodyRevived | not sent, and ignored with a NOTE, under the bit (§2.6) |
| client journal | no layout change: PLAYER(+) at J and PLAYER(-) at R from member messages (§2.8), NOTEs. `announce_player` no longer writes join=255 |
| server journal | no layout change: a ball leave is now EVENT Unphysicalize at `next` and PLAYER(-) at `next + 1`, the queued shape SimTool already reads; a rejoin is PLAYER(-) then PLAYER(+) at R. The header's `world_options` (SPD's v4 field) carries the bit. **If G2 lands before `world_options` exists in the header, G2 adds it and takes the next journal version (v5)**; otherwise no bump |
| script record | the next SR `kVersion` after TRD's (§4.2); RSD's portable version the next after RSD's and TRD's, with the section and kind 4 |
| SimTool `--replay-session` | honours the bit (placement point, metric, hold, latch, BodyRevived not applied); PLAYER before inputs and events within a tick (§2.8) |
| legacy path (MC:2438 dispatch) | ends locally on `SessionMember(added)` of another player (P_M) and on the bit (P_G) (§2.4, §2.8) |
| `docs/rooms-and-sessions-protocol.md` | SessionMember, lives, the bit, `anchor_probes` |

## 6. What must not change

- **The server never depends on snapshots or on any client's world.** After
  part F, no client message changes a server body except the lockstep inputs and
  lifecycle events.
- **No IVP change, no engine change.** Everything is CK host support, core logic,
  server session logic and the bridge's existing calls.
- **Own-ball sensors on clients:**
  - checkpoints (PC_TwoFlames 6.5 m and 70 m, PS_FourFlames);
  - extras;
  - the I3 balloon;
  - the tutorial.

  A client's own Sector crossing still comes only from its own ball (N3: the
  stamped own activation).
- **The server's own checkpoint chain stays inert** under the new placement
  point. N3's hold and N4's phase cell hold it as before. Check: the arena
  regression's `sector +N` ticks are unchanged on the server and every client
  (§7.3 H8).
- **SPD's invariant.** Rewiring touches inputs of blocks, never PH rows or
  module frames. A later sector's pass changes nothing of an earlier sector's
  sensors.
- **Old journals.** With the bit clear, PW keeps today's path, and lp1_join's
  `server.bmjr` replays matched 3528/3528 as recorded (harness.out). The
  always-queued leave changes new journals only.
- **Spawn kick, group and rank of members known at SessionStart.** They are
  unchanged: the same join orders, the same code.
- **The resync, trafo and placement designs' records and steps,** except the
  stated amendments (§2.9) and the version ordering (§4.2, §5).
- **Solo sessions agree with the server.** Solo sessions are not promised to
  match pre-change journals bit for bit: the birth tick of a ball now holds the
  frames where the server used to see the newborn, on the server too. They are
  promised to agree with the server under the bit.

## 7. Verification

Every check runs against the step's own build, server included. Every networked
arm has a same-batch control (core design §5.3). A failing check is investigated
to its root cause and reported as symptom, cause and options (principle d).

### 7.1 Unit tests (`BallanceMMOMessageTests`)

| id | test | pass |
|---|---|---|
| U1 | `probe_metric` / `probe_nearest` on fixed vectors: axes 5 vs 7 pick different balls (the G-c5 example); a tie picks the lower id; empty returns -1; the float results as hex constants | identical constants on x64, x86 and GCC builds |
| U2 | core `probe_candidates` against a fake platform over a scripted life sequence: own birth at t excluded at t, included at t+1; own death at t included at t; held mirror birth at t excluded; late birth (rewind) included from stamp+1; a mirror **without navigation** included; a body held out by the replay excluded; a trafo gives one entity per player; the result equals the set implied by the applied lifecycle events at every tick | all equal; `probe_two_bodies` 0 |
| U3 | RB: `place_probes(t, held_out)` called once per replayed tick, before the `born_now` work and `pre_step`, through `bind_session_world`; a held-out body reported as such; a script-record round trip (this design's version) with a changed `CurrentLevel[0][1]` inside the window leaves the replayed identity cells and latches equal to the straight run's (F9's test) | exact |
| U4 | `session_member_msg` serialize/deserialize; refuses kind 0/3, join order 64, id 0, life 0, truncated input; `player_entry.life` and `session_event_msg.life` round trips | exact |
| U5 | core lives: (a) member then events: group `P#k`, kick index from k, rank k; (b) an event's unreliable repeat before the member message: parked, then applied at its stamp via the late path, the same world as (a); (c) as (b) with the stamp outside the history: `request_resync`, no mirror made; (d) copies of added and removed: no change; (e) conflicting join order for one life: session ends locally with the reason; (f) **leave with a ball, nothing queued**: Unphysicalize at u applied, removed at R = u+1, no event dropped, PLAYER(-) at R; (g) order reuse: life of k removed, another id added with k: ranks, group and kick from k; (h) **rejoin on the same id**, k' = k and k' ≠ k, removed(p,1,R) and added(p,2,R) in one mailbox drain and in reverse arrival order: old-life events with their old order, new-life events with the new one, no session end; (i) a replay across R uses the closed life's entry and relay frames; (j) the "mirror of a life outside `players_`" and `member_life_outside` counters | (a)-(i) as stated; (j) 0 in every case |
| U6 | server (fake runner and world): `assign_join_order` skips retiring orders; a ball leave with nothing queued yields EVENT at `next`, `leaving` with `remove_at = next + 1`, `place_probes` at `next` includes the clone and at `next + 1` does not; a rejoin while leaving is added at R after `finish_leave`, with no event erased; `on_player_added` fires before the joiner's first `on_event_accepted` and the late joiner's SessionStart is sent from it; `on_player_removed` carries `finish_leave`'s tick and the removed life's order and releases it; the late base ≥ `leaving_horizon`; `SessionMember` goes into `event_repeats`; `probe_set_drift` 0 | exact |
| U7 | journal: a client journal with PLAYER(+) at J earlier than the write position and frame records after it: SimTool's reader adds with the recorded order and never calls `ensure_player`; PLAYER(-), PLAYER(+) at one tick in file order | the replayed world's slot = k |

### 7.2 SimTool (offline, `--root C:/Users/geekerwan/Desktop/Ballance-Agent`)

| id | mode | pass |
|---|---|---|
| V1 | `--probe-census` (new; read-only): rewire per class on L1-L12 and the arena; prints per block the frame name, ObjectB, Distance, axes, `Use Outputs`, Barycenter, and per identity read its kind and gating probe, and the signature | mechanism counts equal §1.1.2's table (3, 32, 15, 12, 14, 29, 25, 14, 27, 32, 32, 43, 12); `all` counts equal the server's today (18 ... 24); identity counts equal; **on all 290 blocks**: Barycenter 0, axes 5 or 7, squared 1; every identity read has exactly one gating probe; names unique; with SPD `--placement on`, counts grow by exactly the copied rows' blocks |
| V2 | `--drop` two-player variant (`--drop-player-at` for a second clone): one ball at XZ 40 m / 60 m below a 50 m XZ probe, the other at XZ 55 m level; and P_Modul_29 with a stone ball 3 m from #47687 and a wood ball nearer in 3D to #47688 | the block fires under the bit, not with it off; the bridge breaks under the bit, not with it off (G-c5) |
| V3 | `--replay-session maps/arena-test/lp1_join/server.bmjr` (bit clear) | `matched=3528`, `checkpoint_mismatches=0` |
| V4 | `--savestate-check` with two players and a probe in range, chained rollbacks across a no-candidate interval (both dead) | blobs and script records byte-identical to the straight run; `skipped` 0 |
| V5 | release: rewire, release, rewire on the same level | the second census and signature equal the first; no `BMMO_Prox_*` left after release |

### 7.3 Headless harness (arena, 4 clients, Arch server, `scripts/arena_map_test.py`)

| id | arm | pass | negative control (HEAD) |
|---|---|---|---|
| H1 | Pusher arrival: client 1 drives to P_Modul_01 first, the others stay behind (beam/hold keys); `--snapshot-interval` normal and snapshots off; 0 ms and `--latency-ms 150` | the Pusher's row equals the server's at every checkpoint in [wake-10, wake+200] in every client journal (`body_series.py`); one wake tick in all five worlds | today's lp1_join numbers: 0.0897 m for 10-16 ticks |
| H2 | fan: a remote ball through the arena's P_Modul_18 box | the remote's mirror and the fan's force state bit-equal to the server's clone through the passage, all clients | mirror off on the non-owners |
| H2b | fan, two balls: A enters and stays, B enters while A is in range, then A leaves the box | in every world the same Get Cell output (by player) and the same force state at every tick; the log names which ball was pushed when (documents §2.2's semantics) | |
| H2c | fan, leave while latched: A's client leaves while A is latched and B is in range; then the same with A dying in the death zone (§9 Q6) | latch moves at the same tick in every world (`relatched` equal); force state equal; the server never reads a destroyed clone | |
| H3 | rope bridge: a remote stone ball onto P_Modul_29 | the plates' rows bit-equal, one break tick everywhere | no break on the non-owners |
| H4 | forced rollbacks (`--force-rollback`) with anchors before, at and after a mirror-triggered wake | journals equal to the unforced same-batch arm; `skipped` 0 after F6 | |
| H5 | the joiner (lp1_join recipe) with G-c on top of RSD A5 | the joiner's imported frames and Proximity state make its P_Modul_01/03/29 rows equal to the server's after its base | |
| H6 | BodyRevived retired: 4 clients, 3 minutes, the arena's Schaukel and Sack sectors; same batch with server-side application on | the server journal has no BodyRevived EVENT; the mechanism rows including the sleep flag agree server vs every client; the control's disagreement is recorded as G-c3's measured cost | |
| H7 | H1-H3 at 10% loss | as H1-H3 | |
| H8 | the 10 recent arena regression recipes | `sector +N` ticks unchanged on every side; mechanism errors ≤ control | |
| J1 | lp1_join repeated (`--players 4 --join-late 3:1500`, link 150 ms); the joiner holds no keys for 200 ticks after its spawn | all five worlds log the same spawn impulse index for the joiner (today 60/60 vs 56); its mirror in clients 0-2 equals the server's clone at every checkpoint of [1844, 2044] (keyed by owner, `first_divergence.py`); every client journal has `PLAYER +p142763540 join=3` at the server's J and no `join=255` | 56 on clients 0-2 |
| J1b | J1 at 10% loss, 5 seeds | as J1; `events_parked` may be > 0; no resync caused by a member message (`request_resync` reasons logged) | |
| J2 | order reuse with contact: client 1 leaves at host tick 900 (new harness `--leave N:T`), a fifth client joins at 1500 and gets order 1, is beamed onto client 2's resting ball | server log: joiner order 1, no "falling back to slot"; the contact pair bit-equal in all worlds for 300 ticks | with `P#63` vs `P#2` the strcmp order flips (`P#1` < `P#2` < `P#63`): expected divergence; if the control shows none, the contact is insensitive and another contact is chosen |
| J3 | the member message delayed: server test flag `--hold-member-ms 300` (reliable and repeats) | `events_parked` > 0 on every existing member; the joiner's mirror as in J1; journals exact against J1's arm | |
| J4 | a join inside a leaver's `leaving` interval (`--leave` with a queued event, join 1 tick later); a leave with a ball and nothing queued while the leaver is nearest to a P_Modul_01 probe that checks at `next` | no "falling back to slot"; the joiner's order differs from the leaver's until R; joiner base ≥ R; the probe's wake tick equal in all worlds | |
| J5 | rejoin on the same connection: client 2 leaves the room with a ball and rejoins within 2 ticks (`--rejoin N:T`), then plays 300 ticks | server: rejoin added at R, no event erased; every client: two lives of the id, old events with the old order, new with the new; the rejoiner's mirror equals the server's clone at every checkpoint | |

### 7.4 Retail runs (core host, against the step's server)

| id | run | pass |
|---|---|---|
| R1 | arena: two retail clients + two headless; a headless ball wakes P_Modul_01 first | the retail journals' Pusher rows equal the server's (as H1); `anchor_probes` equal in SessionReady |
| R2 | L11, two retail clients: one crosses the P_Modul_01 / P_Modul_37 sensors alone; then the P_Modul_18 fan with two balls | as R1 on L11's rows; the fan as H2b |
| R3 | a retail late joiner and a retail existing member (lp1_join recipe with retail client 0) | J1's criteria on the retail logs |
| R4 | session end, vanilla restart of the same level without reload | V5 on retail: the Pusher wakes for the own ball in vanilla play (release restored the sources) |

### 7.5 What each check proves

- U1-U3, V1, V2, V4: the rule itself, its determinism and its rollback state.
- H1-H4, R1-R2: P1/P2 across worlds; H2b/H2c the latch.
- H6: P4.
- U4-U7, J1-J5, R3: J1-J4.
- V3, H8: nothing old moved.

## 8. Steps

Each step is implement → review (code-review, high) → gate on its checks.

| # | step | files | closes on |
|---|---|---|---|
| M0 | tools: harness `--leave N:T` and `--rejoin N:T`, server `--hold-member-ms` (test builds), `body_series.py` into `scripts/` | scripts, SV | the selftest; the tools reproduce today's lp1_join numbers |
| M1 | member lives and J2 (protocol P_M) | `session_member_msg.hpp`, message.hpp, ES (protocol, `player_entry.life`), EM (`life`), LC, SRN/SRH (lives, always-queued leave, deferred rejoin, callbacks, `leaving_horizon`), SV (`retiring_orders`, `started`, send points, late SessionStart, late base, repeats), CL/CM/CC (`member_life`, `on_session_member`, parking, lookups by life), CJ, ST, SC/CH (decode, mailbox), MC (legacy end) | U4-U7; J1-J5; R3. Independent of G-c: may land as soon as the user orders it |
| G0 | `--probe-census` | ST | V1 (bit off) |
| G1 | parts A+B; the server on `proximity_union` with the bit clear (today's path, moved) | `proximity_rules.hpp`, `proximity_union.hpp`, PW | U1; V1; V3; the arena regression identical to the pre-step build |
| G2 | the server under the bit: `place_probes()` before the leavers and the events, the metric, gating choice, latch, hold, signature | PW, SRN, ST (`--replay-session` honours the bit), ES (bit), server config | V2, V4 (server-world parts); V3 still matched with the bit clear |
| G3 | clients: core step 7b, RB hook, `core_world_hooks.place_probes` bound in HW, platform op, record version, H host anchor/release; SessionReady `anchor_probes`; protocol P_G | CF, CC, RB, HW, CP, RBS, SR, SC, SV (compare) | U2, U3; V4; H1-H5, H2b, H2c, H7, H8 |
| G4 | RC host; RL ends locally | CH, MC | R1, R2, R4 |
| G5 | BodyRevived retired (same release as G3, P_G) | CF, SV | H6 |
| G6 | docs: this file's as-built notes; the SR:29-37 comment; RSD §2.4 amendment; protocol doc; `collision-overhaul-design.md` 9.x entry | docs, SR | review |

**Order.** G1-G5 come after F5, F6 and F7 and after R3-R5
(`lifecycle_rewind` on RC), as the user's order already has it (audit §5:
"... F5, F6, F7, G-c, join-order message ..."). G3 needs RSD's
`session/resync_rules.hpp` (the `keep_local` predicate) and RSD A1 for the
portable section, or it adds the section to A1's format when A1 lands. G3's
record version is taken after TRD's T1 (§4.2). M1 needs nothing but its
protocol number; if it lands before G3, G3 builds on its lives.

## 9. Open questions

**Q1. Does retail's original TT_Toolbox_RT compute the same distances as the
reimplementation?** (measurement)

- Settle: in R1 and R2, compare the Proximity blocks' output 0
  (`Current Distance`, float) and local 1 (`Last Check`) between the retail
  client's script records and the server's at equal ticks. The portable records
  carry both by name.
- If they are bit-equal: nothing to do.
- If they differ:
  - (a) evaluate the threshold and the scheduling in host support for rewired
    blocks only, by rewriting the block's distance input so the comparison
    cannot flip. The input is a public pIn.
  - (b) report the residual against the own-ball baseline, which has had the
    same dependence since M3.

**Q2. Does any BodyRevived still carry a wake the server's world lacks?**
(measurement)

- Settle: in H6's control, list every report whose body the server's own world
  had not woken by script within ±1 tick. Use SimTool `--replay-session` of the
  server journal with a new `--trace-wakeups`, which prints the bridge's
  `script_wakeup` entries.
- Expected: none.
- Any hit is a script that runs in one world only. It is reported against
  principle (a) with its script and block, not papered over by keeping
  BodyRevived.

**Q3. (closed in revision 2.)** A joiner inside a leaver's `leaving` interval is
covered by J4: its base is clamped to `leaving_horizon` (§2.7), checked by J4's
"joiner base ≥ R".

**Q4. Does the metric change move the server's sessions?** (measurement)

- Settle: on the recorded 4-player arena, L8 and L11 journals, count the ticks
  where the 3D-nearest ball differs from the metric-nearest ball for a block
  that is checking that tick, and where today's identity pick differs from the
  gating probe's. Use a Python pass over FULL checkpoints, with V1's ObjectB
  positions, axes and gating probes.
- Expected: rare.
- The design keeps the metric either way, because it is the retail semantics
  (G-c5). The count is for the record.

**Q5. The tutorial's 12 proximity blocks watch the ball entity (`Ball_Wood`)
directly and drive `Set Physics Globals` (audit #14).** (measurement)

- They are untouched here: not `Ball_Pos_Frame`, not a mechanism.
- On L1 in a session, each world's tutorial would key its time-factor writes on
  its own ball: the owner's own ball on a client, the parked retail ball on the
  server. That is an owner-only physics script if it is active in sessions.
- Settle: a two-client L1 headless run that logs `time_factor` on each side
  (the retail status line already prints it, `physics_session_client.cpp:1341`).
  - If the factors differ, it is reported to F10's owner as a principle-(a)
    gap.
  - If the session never activates the tutorial, it is recorded as covered.

**Q6. The fan's box test at the death tick of the latched ball.** (measurement)

- At t, the owner's death script may move its own ball entity before the fan's
  `Box Box Intersection` reads it in the same pass; the others read the mirror
  or the clone at the end of t-1 (§3 item 8). From t+1 the latch rule applies.
- Settle: H2c's death arm, with the box test's result logged per world at t.
- If it differs: the body-kind cell and latch take `BMMO_NoBall` for a candidate
  whose death is stamped at t. Remote deaths at t are known before the pass;
  the own death is not (it is committed at post_step(t)), so the owner would
  need a replay of t. Reported with numbers before choosing.

**Q7. Multi-ball semantics of the fan and the bridge (needs the user).**

- Under this design one ball per block is served: the fan pushes the latched
  ball, the bridge tests the ball whose Enter fired (§2.2). Every world agrees.
- Option (a), as designed: retail's single-ball graphs, deterministic.
- Option (b): per-player copies of the force and break sub-graphs (one latch,
  one force per player), so every ball in a fan box is pushed and any stone
  ball breaks the bridge. It needs a graph-copy step like SPD's and more script
  state per record.
- Settle: the user picks; H2b's log shows what (a) does in practice.
- **Answered (2026-10-28, the user, verbatim): "6. b，应该都复制一份逻辑，同一个
  机关应该可以响应所有玩家"** - option (b): every player gets a copy of the
  mechanism logic, one mechanism responds to every player (it confirms §11's
  Q7 of 2026-10-24). The design revision (b) needs - the per-player copies
  of the force and break sub-graphs, the graph-copy step, the record's
  extra script state, the H2b / V2 criteria - is step WAD's, not written
  here.

**Q8. The legacy path and late joiners (needs the user).**

- At P_M the legacy path ends its session locally when another member joins
  late (§2.8), because it cannot keep J1. Until R7 deletes the path, a retail
  player without the session core then cannot stay in a session that gets a
  late joiner.
- Alternative: the legacy path learns `SessionMember` (its own `players_`
  list), which adds a feature to a path R7 deletes.
- Settle: the user picks.
- **Superseded (2026-10-28, the user's answer 7, verbatim: "直接拒绝，不要继续
  兼容非session core的客户端"):** the server refuses every client without
  the session core (step B9DX), so no legacy client is ever in a session
  and "RL ends locally" (on a late member at P_M, on the bit at P_G) is
  moot; M1 and G4 add nothing to the legacy path (MC's "legacy end" in
  M1's and G4's rows, and RL's row of §2.4, are dropped at WAD's revision).

**Q9. Landing order of the protocol numbers (needs the user).** P_M and P_G
follow RSD §11 Q5's rule; if the user wants M1 before the three designs that
claim 10, M1 takes 10 and those take the following numbers.
- **Answered (2026-10-28):** the user left it to the orchestrator ("5.
  你决定"), who chose **G-c first (G0-G5, P_G), then the member lives (M0,
  M1, P_M)**. The reason: it is the user's own audit order ("... F5, F6, F7,
  G-c, join-order message ...", §8 Order), G-c's leader-only wakes are the
  queued residual every snapshots-off check meets (P_Modul_03, _19_Flaps,
  _34, the l8_cross P_Modul_29 plates), while M1 only matters with a late
  joiner, a leave or a rejoin; and M1 builds on G3 cleanly (§8 Order: "if
  it lands before G3, G3 builds on its lives" - the other way round is the
  simpler one). P_G takes the next free protocol number when G3 lands (14
  at the earliest; Q5 of the rollback gaps also claims 14 - whichever lands
  first takes it) and P_M the next after it.

## 10. Review (revision 2)

Both reviews were checked against the working tree. Every blocker and should-fix
was confirmed; none is refuted. Where the fix differs from the one proposed, the
reason is given.

| finding | verdict | what changed |
|---|---|---|
| R1.1 / R2.2 the leaver's Unphysicalize at R and the server's candidate set at R | confirmed (LC:461-470, SRN:348/389, SV:1888, PW:719-736) | J-e: every ball leave goes through the queue, so the death is an event at `next` on every side and R = `next + 1` (§2.7). Chosen over "apply events ≤ R": it also fixes the deletion-order disagreement and needs no client rule tied to R. J3 now says no event is dropped because of R. U5(f), U6, J4 |
| R2.1 rejoin on the same id | confirmed (SV:526, :2025, SRN:292-293, :316-317) | J-f, J-g: deferred rejoin at R, life numbers on member messages and on relayed events, `players_` as lives keyed by (player, life) (§2.7, §2.8). Mapping by an explicit life, not by tick or epoch inference. U5(h), J5 |
| R1.3 removal erases state replays need | confirmed (CL:381-405, :468-475) | nothing is erased during a session; relay state kept (§2.8, §4.4). U5(i) |
| R1.4 / R2.7 record path vs bookkeeping path for mirrors without navigation | confirmed (CF:533-536, SC:2525, CH:1465, HS:117) | candidates are the world's ball bodies at the place point, independent of navigation and of records; a replay passes what it holds out (§2.2, §2.5). Chosen over a list in `rollback_tracked`: the re-captured records in a replay would need the same upkeep, and `remote.physicalized` is live state inside a replay. U2 |
| R1.5 / R2.3 identity metric, reference and latch | confirmed (census L11 P_Modul_18/29; SPX:82-85, :263-271) | gating probe per read, its choice as the cell; latch rule for body kind (G-c5 second half, G-c7, §2.2); multi-ball semantics stated and put to the user (Q7). V2, H2b, H2c |
| R1.6 SessionMember reliable only | confirmed (SV:1936-1940) | pushed through `event_repeats`; copies idempotent (§2.7). J1b |
| R1.7 PLAYER(+) at the current tick | confirmed (CJ:111-119, ST:2861-2882) | PLAYER(+) at J, PLAYER records first within a tick (§2.8). U7 |
| R1.8 / R2.4 script record version collides with TRD | confirmed (TRD:940-946, :1191) | the next version after TRD's; portable version after RSD's and TRD's (§4.2, §5) |
| R2.5 M1 and G3 both "protocol N" | confirmed | P_M and P_G, G5 shares P_G (§5) |
| R2.8 file lists | confirmed (HW:264ff, RSD `resync_rules.hpp`) | HW and CP in D, G3; dependency in §8 Order |
| R2.9 legacy path and SessionMember | confirmed | legacy ends locally on a late member (§2.8); Q8 to the user |
| R1.9 names with `_BMMO_` | confirmed (PW:493-499) | copy suffix `#BMMOId_...` (§2.3) |
| R1.10 / R2.6 size | confirmed | 520 bytes (§4.2) |
| R1.11 136 vs 290 | confirmed | §1.1.2 says what was read; V1 asserts on all 290 |
| (own work) J-h, the late base | found while revising | clamp to `leaving_horizon`; Q3 closed |
| (own work) add failure | found while revising | `on_player_add_failed` → SessionEnd (§2.7) |

## 11. Decisions (user, 2026-10-24)

- **Q7.** Several balls on one fan or cork bridge: every player gets their
  own force and their own test. The fan pushes every paper ball on it, and
  the bridge tests every stone ball that steps on it, the same in every
  world.
- **Q8.** The legacy retail path gets nothing new; a late join ends its
  session locally. It is deleted at R7.
- **Q6.** The narrowing of G-c stands: checkpoint flames, extra balls, the
  balloon and the tutorial stay on the own ball in a client world.

### 11.1 Decisions (user, 2026-10-28)

The user's answers to the orchestrator's seven open decisions (the whole
message is quoted in `docs/roadmap.md` §4 item 3); three are this design's:

- **Q7 = (b)**, verbatim: "6. b，应该都复制一份逻辑，同一个机关应该可以响应所有玩家"
  (b: every world copies the logic per player; one mechanism must respond to
  every player). It confirms §11's Q7 above; the design text (§2.2, §8) is
  revised to it at step WAD.
- **Q8 is superseded** by answer 7, verbatim: "7. 直接拒绝，不要继续兼容非session
  core的客户端" (refuse it outright, no more compatibility with clients
  without the session core): built at step B9DX, the server refuses every
  such client, so the legacy path's local ends of this design (§2.4 RL,
  §2.8, §5's legacy row, M1's and G4's MC) are moot.
- **Q9**, answer 5, verbatim: "5. 你决定" (you decide) - the orchestrator
  chose G-c (G0-G5) before the member lives (M0, M1); the reason is in §9
  Q9.
