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

**Revision 3 (step WAD, 2026-10-28)** applies the user's answers of
2026-10-28 (§11.1): Q7 = (b), so one mechanism responds to every player
through per-player copies of the sub-graphs that act on one ball (new §2.10,
rule P5, step Q7 in §8); Q8 is superseded, because the server refuses every
client without the session core (B9DX), so every "RL ends locally" part is
dropped; Q9 puts G-c (G0-G6) before the member lives (M0, M1), and step Q7
comes after M1 (§8 Order). §10.1 lists what changed. Revision 3 cites the
code at HEAD 473f6e8 where it adds a reference; the older references keep
their 2026-10-23 line numbers.

Status: design; step G0 built (WG0, 2026-10-28: SimTool `--probe-census`,
§12 "As built (step WG0)"), step G1 built (WG1, 2026-10-28: parts A and B,
the server on them with the bit clear, §12 "As built (step WG1)"), step G2
built (WG2, 2026-10-28: the server's world under `WORLD_PROXIMITY_UNION`,
§12 "As built (step WG2)"), steps G3 and G5 built (WG3, 2026-10-28: the
clients under the bit, BodyRevived retired, protocol 16, §12 "As built (step
WG3)"), step G4 built (WG4, 2026-10-28: the retail core host, §12 "As built (step
WG4)"), step G6 done (WG6, 2026-10-28: the documentation pass, §12 "As built
(step WG6)"), step M0 built (WM0, 2026-10-28: the tools, §12 "As built
(step WM0)"), step M1 built (WM1, 2026-10-28: member lives and J2, protocol 18,
§12 "As built (step WM1)"), step Q7 built (WGQ7, 2026-10-29: per-player
mechanism units, protocol 19, journal v8, §12 "As built (step WGQ7)"). Base: HEAD 4882ec2 (R2) with step P in the working
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
path, **SV** the server world. Since step B9DX the server refuses a physics
session to RL (answer 7, §11.1). Nothing in this design reaches RL any more;
it is named only where revision 2 did.

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
      #47687). Enter → `Get Cell` #46995 → `Test "Ball_Stone"` → breaks the
      bridge. *(Revision 3, read at WAD: the Test's True output goes to
      `10 Hinges` input 2, which shuts down one hinge, #47108, and to the
      probe's own Off and the break sound. Input 1 of `10 Hinges`, the reset,
      shuts down all ten.)*
- In both scripts the identity read is fed **directly** by one probe's output
  (#46002 → #45965, #47265 → #46995). That probe is the read's *gating probe*
  (§2.2).
- `SetPhysicsForce` creates a controller only when the block holds none (PF:200,
  `if (!controller)`), on its target as the deferred callback finds it
  (PF:139, :155-156).
- The `Gameplay_Tutorial` blocks (12, L1 only active) watch the ball entity
  itself (ObjectA = id 42240, the `Ball_Wood` that `PE_Balloon_Init` local 12
  names). They are not rewired anywhere (§9 Q5).
- *Revision 3, the consumer census (step WAD).* `consumers.py` follows every
  output of every mechanism-class proximity block through the behaviour links
  of L1-L13 (SimTool `--describe-script` of each distinct mechanism script,
  placement off; output in `.local-tools/work/WAD/consumers.txt`). Each block
  drives one of four things:
  - a wake: `Physics WakeUp`, and on P_Modul03 also `Add To Group` /
    `Group Iterator`. These are P_Modul_01, 03, 19, 25, 30, 34 and 37, and
    the 80 m probe of P_Modul_29;
  - a script activation: P_Modul_18's 80 m probe drives `Activate Script` /
    `Deactivate Script`;
  - sound: P_Modul_18's 25 m probe drives `Wave Player` and
    `TT ProximityVolumeControl`;
  - an identity read: P_Modul_18's 7 m probe and P_Modul_29's 4 m probe,
    and no others.

  The two identity-read probes match K0's array-read census exactly
  (`.local-tools/work/k0/census/l*_client.txt`: the only readers of
  `CurrentLevel[0][1]` in the client scope). There are 71 reads on L1-L13:
  58 fans and 13 bridges, as the last column of the table above. They are
  the only mechanism sub-graphs that act on *one* ball (§2.10).

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
- **P5 (one mechanism responds to every player; revision 3, the user's
  answer 6).** In every world, a mechanism sub-graph that acts on the ball
  whose identity it reads exists once per player slot. Slot k serves the
  ball of the life that holds join order k (§2.10): the fan's force and the
  bridge's stone test. Every other mechanism block stays single and measures
  P1's frame. P1's frame fires for every ball, because the nearest ball
  under the block's own metric is in range whenever any ball is.

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
| I | per-player mechanism units (revision 3): the CK-free unit rule; the copy step; slot binding; the session's slot count K and the join-order cap | new `session/mechanism_units.hpp`, `game/proximity_union.hpp`, PW, SRN, SV, CF, CC, ES, `session_start_msg.hpp`, journal, ST (§2.10) |

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

*Revision 3.* The user chose per-player copies (§9 Q7 (b), §11.1). Under
`WORLD_PLAYER_MECHANISMS` (§2.10, step Q7) the fan pushes every ball in its
box, each with its own slot's force. Any stone ball whose own sensor enters
breaks the bridge. The single-ball rules above are what a world does under
G3's bit alone, in the builds between steps G3 and Q7, and in their
journals. Under the Q7 bit, every identity read belongs to a per-player unit
and takes its slot's ball (§2.10). The gating probe's choice, the union
cells and the union latch then have no read left to serve.

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
   entity)`), or the hold, or `BMMO_NoBall` (a slot copy of §2.10: never
   the hold).
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
| RL | none. Revision 2 ended the legacy session locally on the bit. Since B9DX the server refuses a physics session to a client without the session core (answer 7), so RL never sees the bit and MC is not changed | | |
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

**Legacy path (RL).** Nothing (revision 3, answer 7). Since B9DX the server
refuses a physics session to a client without the session core, so no
legacy client ever receives a `SessionMember`. Revision 2 had the legacy
path end its session locally on a late member, and §9 Q8 asked the user
about it. Both are dropped, and MC is not changed.

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
| SPD, revision 3 | The copy step (§2.10) runs after `apply`, so a placed fan (`_R<row>`) gets its own slot copies. Its owner name keeps them unique. Under the Q7 bit the placed fan's force blocks (slot 0 included) are aliased like every unit's (§4.3). **Open, queued (roadmap §4 item 5, "WAD-A"):** without the Q7 bit, a placed copy's force controller is `rt:force/<copy's block id>` (sector-placement-design.md §4, "Savestate"), and so is an original's; whether those ids agree between the server and a resyncing or late-joining client (headless, and retail CK2 against Ballanced) has not been measured. The placement copies are made at each world's anchor before anything else of the level scene (`sector_placement.hpp:563`), which makes agreement between two Ballanced worlds likely but not shown, and the retail mod's own objects make it doubtful there. No run of the step logs shows a fan force held at an import. WAD-A measures it (a resync and a late join with a fan force held, placement on; headless and retail) and, if unmatched, brings the alias of §4.3 forward to every mechanism force block. |
| gap Q (`rollback-engine-gaps-design.md`, engine change #20, record v4/v5) | A slot copy's `SetPhysicsForce` is a block of a recorded script. Its queued Create is in the queue section by behaviour id, like the original's. Its controller is named `rt:force/<its own block id>` (engine change #18), so two slots' forces on one ball, or one slot's forces from two fans, stay apart in a blob of this process. A slot copy's block id is NOT the same in every world (each world copies at its own anchor), so a blob that crosses worlds (a resync's or a late join's, the server's) would name controllers no live one matches - trafo T3's failure (engine-changes.md, "The savestate's controller alias"). Every unit force block is therefore aliased `unit:<owner>/<n>/S<k>` through the one `set_controller_alias` of the physics manager (§4.3). |
| M1, revision 3 | Slot binding reads the life that holds a join order (§2.10). Every world knows that only under J1, so step Q7 comes after M1 (§8 Order). |

### 2.10 Per-player mechanism units (part I, revision 3, the user's answer 6)

The rule is P5 (§1.1.5). The user's words: "6. b，应该都复制一份逻辑，同一个
机关应该可以响应所有玩家" (b: copy the logic for every player; one mechanism
must respond to every player).

**What is per player (the census, §1.1.2).**
- Only the sub-graphs behind the 71 identity reads on L1-L13 act on one
  ball: 58 fans (P_Modul_18) and 13 bridges (P_Modul_29).
- Every other mechanism proximity block drives a wake, a script activation
  or a sound. Under P1 these already respond to every player, so they are
  not copied:
  - the fan's 80 m probe activates the force sub-graph while any ball is
    within 80 m;
  - the bridge's 80 m probe wakes the plates for the first ball.
- With the placement on, a placed copy of a fan or bridge (`_R<row>`) is
  one more unit. WG0's census (`--probe-census`) counts both.

**The unit (part I, CK-free, `session/mechanism_units.hpp`).** The rule runs
on an abstract graph: blocks with their prototype GUID, behaviour links with
their delays, parameter sources (pOut → pIn, through graph locals), and each
block's parent graph. For each identity read R and its gating probe P (§2.3):

- **D**, the identity consumers: the blocks with an input parameter whose
  source chain starts at R's output.
  - Fan: `Box Box Intersection` Entity 0, and `SetPhysicsForce`'s target.
  - Bridge: `Test`.
- **C**, the control path: the blocks on a behaviour-link path from P to a
  block of D.
  - Fan: the two `Binary Switch` and the two `Identity` blocks.
  - Bridge: none.
- **The unit**: U = {P, R} ∪ D ∪ C.
- **The state**: S, the graph locals that a block of U writes (a pOut
  destination) and a block of U reads.
  - Fan: locals 20-21 of `Physics Force`, `TurnON_Permission` and
    `TurnOFF_Permission`. The `Identity` blocks write them, and the switches
    read them as their conditions.
  - Bridge: none.
- **Graph or block set.**
  - If U and S are every block and every written local of U's parent graph,
    and that graph is a sub-behaviour graph, the unit is that whole graph.
    Fan: `Physics Force`, #46064 on L11, eight blocks and 22 locals.
  - Otherwise the unit is U's blocks. Bridge: #47265, #46995 and #47620 in
    the root graph.
- **Shapes.** WAD found exactly these two shapes. The rule reports a unit's
  shape, and the rewire refuses any other with the script and the blocks
  named. A third shape on a custom map is then a design question, never a
  guess. WG0's census asserts the two shapes and the parameter chains above
  on every unit, because WAD read them from behaviour links and L11's
  values.

**Links and sources of slot k's copy** (k ≥ 1; slot 0 is the original):

- **A behaviour link inside U** becomes the same link between slot k's
  copies.
- **A link from outside into U** is added from the same source to slot k's
  copy (fan-out).
  - Fan: `Activate Script` → `Physics Force.in0`, and the two
    `Deactivate Script` blocks → `in1` (probe Off) and `in2` (Shutdown).
  - Bridge: `Physics WakeUp.out` → probe In, and `Binary Switch.False` →
    probe Off (the reset).
- **A link from U to outside** is added from slot k's copy to the same
  target (fan-in).
  - Bridge: Test True → `Wave Player`, and → `10 Hinges.in2`.
  - CK keeps one activation flag per input. Two copies that activate one
    input in the same frame activate it once.
- **Delays.** Every added link takes the original's initial delay.
- **Parameter sources.**
  - A source in U or S becomes slot k's counterpart.
  - Any other source stays the original's and is shared: constants, the
    ObjectBs, the `Kollisionsquader`, the force vectors.
  - A graph unit copies its locals with it, so a shared constant local is
    duplicated by value and never written.
- **Probe Off on the bridge.** Test True → probe Off is inside U. A break
  therefore switches off the sensor of the slot that broke the bridge, as
  retail does for its one ball. The other slots' sensors stay on. A later
  stone ball fires `10 Hinges.in2` again. That is a Shutdown of a hinge with
  no constraint, which `PhysicsHinge` passes through without deleting
  anything (PhysicsHinge.cpp:183-194 at HEAD; #47108's Out2 is not linked),
  and a second play of the break sound, which is presentation only.

**The slot count K and the join-order cap.**

- K = `mechanism_slots`, fixed when the session starts and sent in
  SessionStart. Its value is the server's `maximum_members` (the room
  capacity; `config_manager.hpp:48`, default 8) clamped to
  [1, `MAX_PLAYERS_PER_SESSION`] (64).
- Slot 0 is the original blocks; slots 1..K-1 are copies.
- Why the room capacity: every player a room can hold gets a slot (answer
  6). 64 would cost about eight times the record of the default (below). A
  constant in the code would leave players of a larger room unserved.
- **The cap.** The server assigns join orders below K only
  (`assign_join_order`, SV:829 at HEAD).
  - A joiner that finds every order below K held or retiring (J2) waits in
    `waiting_orders`. `on_player_removed` releases an order, assigns it to
    the first waiting joiner and posts that joiner's add. That joiner's base
    is clamped to `leaving_horizon` anyway (J4).
  - A joiner that finds every order below K held by a live life is refused
    the physics session with "physics session full (K mechanism slots)" and
    stays in the room. This happens whenever the room capacity is above K:
    a `maximum_members` above 64 from the start (the room cap is clamped to
    `MAX_MEMBERS_PER_MESSAGE` = 256, SV:287 and SV:2326-2328, `room.hpp:19`,
    while K is clamped to 64, so with 100 the 65th physics joiner is
    refused without any reload), or a capacity raised by a configuration
    reload after the session started (K stays fixed for the session).

**Binding (in `place`, every world, every live and every replayed tick).**
Slot k's candidate at tick t is the ball of the life in this world's lives
(`players_`, §2.8) whose join order is k and whose [J, R) holds t. It must
have a body at the place point and not be held out by a replay (§2.2's
candidate rule). Otherwise slot k has no candidate.

- **Frame.** Slot k's copy of a unit probe measures its own frame. The frame
  goes to the candidate's position, or holds when there is none. Its initial
  position is `ResetPoints` row 0, as in §2.2.
- **Identity cell.** Each read has one private one-row copy per slot (§2.3).
  - By-name kind: the retail entity of the candidate's ball type, or
    `BMMO_NoBall` when slot k has no candidate (no hold, unlike §2.2's
    union read).
  - The initial by-name value is `BMMO_NoBall`, so `Test "Ball_Stone"` of a
    slot that never had a ball is false. The union read of §2.2 starts on the
    level's start ball instead.
  - Why no hold: a held `Ball_Stone` would outlive its ball. Stone ball k
    breaks the bridge, so slot k's Test True turns slot k's probe Off; its
    client leaves (or the ball dies) before the ball has left the 4 m range,
    and the held frame stays inside it. A later bridge reset (`Binary
    Switch.False` -> probe Off, then `WakeUp.out` -> probe In, both fanned
    out to every slot) re-arms slot k's probe, the probe fires Enter on the
    held frame, and a held `Ball_Stone` would make Test true and break the
    bridge with no stone ball present. With `BMMO_NoBall` that Test is
    false. The single-ball hold of §2.2 has no such case: there the cell
    always follows some live ball.
  - Body kind: the candidate's ball entity, or `BMMO_NoBall`.
- **Latch, body kind.** If slot k's Get Cell output does not name slot k's
  candidate, it is set to the cell. This is §2.2's rule with "a candidate of
  t" narrowed to slot k's candidate.
- **No gating-probe choice.** A slot's probe and cell follow the same ball
  by construction.
- **Unbound slots never act.** Both identity kinds read `BMMO_NoBall` in
  an unbound slot (from the tick it unbinds: death, leave, or before its
  first body), which is what makes this hold.
  - An unbound fan copy's box test reads `BMMO_NoBall` and is false, so it
    only reaches the Shutdown of a force it does not hold.
  - An unbound bridge copy's Test reads `BMMO_NoBall` and is false, whatever
    its probe does: an Enter of the held frame after a reset re-arm (above)
    breaks nothing. Only a bound stone ball's own Enter makes it true.
  - This holds even where a level's reset point or a held frame lies inside
    a unit probe's range. The held frame is not moved to a park point on
    unbind: that move would make the copy's probe fire an Exit the original
    script never sees, and the `BMMO_NoBall` cell already makes every
    consequent Test and box test false.
- **The core** passes each candidate with its life's join order
  (`probe_candidate` gains `slot`, read from `players_`). The server reads
  the clone's slot (PW:769-779 at HEAD, the join order).
- **Each candidate's place.** A ball can be the candidate of its slot and,
  for the probes that are not copied, the nearest ball of P1 at the same
  time. Both are written in one `place` call.

**Lives, trafos and deaths.**
- A life is bound at every tick it has a body, from its first Physicalize
  on.
- A death leaves the slot without a candidate: the frame holds, the
  by-name cell reads `BMMO_NoBall` from the next tick, and the latch moves
  to `BMMO_NoBall` at the next tick.
- A trafo keeps the slot. Body kind: the latch moves to the new ball entity
  at the next tick. By-name kind: the cell names the new type from the next
  tick, and a new Enter reads it.
- A leave closes the life at R.
- A rejoin is a new life (J-f). It takes the slot of its new order, which
  may differ from the old one.
- The state of a slot's copies carries over from one life of an order to
  the next: the held frame and the Proximity state. Every world has the
  same history of that slot, so it carries over the same way everywhere. A
  late joiner imports it with the portable record (RSD A5).

**The copy step (CK host support, `game/proximity_union.hpp`, at the
anchor).** It runs after SPD's `apply` and the script selection, and before
`rewire`. `rewire` then gives every slot its own frame and identity copy.

1. **Find the units.** Extract the abstract graph of every selected
   mechanism root script that has an identity read. Run the unit rule, and
   record every block's pre-copy walk position (the names below use it).
2. **Refuse a hot source**, as SPD step 3 does: a force controller, a joint
   handle or a queued PreSimulate entry on a unit block. At an anchor
   nothing is physicalized.
3. **Copy.** For k = 1..K-1, unit by unit in walk order:
   - copy the unit's behaviours (a graph unit: the graph; a block unit: its
     blocks) with `CKContext::CopyObjects` and a dependencies context that
     copies behaviours with their parameters, IO and sub-behaviours, and no
     other object;
   - add each copy to U's parent graph after its existing sub-behaviours
     (`AddSubBehavior`), in slot order, then unit order;
   - set the links and sources by the rules above.
   - Whether retail CK2 copies behaviours the way Ballanced does is §9 Q10.
     If it does not, the step falls back to a copy built by hand.
4. **Activation.** A copy takes the original's activation and runtime
   flags. At the anchor every unit is inactive until its script's own
   inputs reach it.
5. **All or nothing.** On a refusal, the copies and the links added so far
   are destroyed, and the world is as before the call. The anchor then
   fails, as a signature mismatch does.
6. **Release** at session end and world teardown, in this order:
   `proximity_union::release` (sources back, frames and identity copies
   destroyed), then the slot copies with their links, then SPD's release.

**Names.**
- Frames are `BMMO_Prox_<owner>_<n>_S<k>`, and identity copies
  `CurrentLevel#BMMOId_<owner>_<n>_S<k>`, for k = 0..K-1. Here n is the
  original block's *pre-copy* walk position.
- Probes that are not in a unit keep §2.3's `BMMO_Prox_<owner>_<n>`.
- Appending copies after a graph's sub-behaviours can shift the walk
  positions of later blocks. That is why n is taken before the copy.
- The portable record keys blocks by the post-copy walk. That walk is the
  same in every world: the same K and the same copies appended in the same
  order.

**The record and the replay.**
- The copies are sub-behaviours of selected root scripts. The v2 walk
  (`capture_behavior`) therefore records them like any block: activation,
  locals, outputs, the force and joint state, and delayed links.
- Gap Q's queue section records their queued Creates.
- The portable record (resync, late join) carries them by walk position.
- A replay restores them and re-runs them with their scripts. `place`
  re-derives every slot's frame, cell and latch at every replayed tick from
  that tick's bodies and the lives (§2.5).
- The hold section (§4.2) gets K frames per unit probe. A slot's by-name
  cell has no hold (it is `BMMO_NoBall` or the candidate's type, a function
  of the tick), so it needs no entry; the union read's by-name hold keeps
  its one entry.
- **Size, estimated** from the v2 layout (about 26 bytes per behaviour plus
  5 bytes + the data per local and output):
  - a fan copy is about 0.65 KB, a bridge copy about 0.17 KB;
  - at K = 8, placement off: L11 (5 fans, 1 bridge) gains about 24 KB on
    its about 40 KB record, and L12 (11 fans) about 50 KB;
  - the capture time grows with it;
  - check C1 measures both (§7.6, §9 Q11).

**The world option.** `WORLD_PLAYER_MECHANISMS` turns all of this on (§5).
With the bit clear, a world rewires as G3 does, with union reads, the
single-ball rules of §2.2 and no copies. That keeps the journals of builds
between G3 and Q7 replayable.

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
10. **Per-player units (revision 3).**
    - **Same K.** It comes from SessionStart, which is the same for every
      member of a session, late joiners included. The server journal's
      header carries it (§5).
    - **Same units.** The CK-free rule runs on the same scripts after the
      same placement. The signature covers the unit shapes, K and every
      copy's structure (§5).
    - **Same copies.** The copy order is fixed: units in walk order, slots
      in order, each copy appended after its graph's sub-behaviours. CK ids
      differ between worlds (each world copies at its own anchor, after a
      different history of created objects; a late joiner anchors long
      after the server), so nothing that crosses worlds may carry one:
      frames, cells and copies are found by name or by walk position. A
      force controller's own name `rt:force/<block id>` (engine change #18)
      does carry one, and a savestate blob crosses worlds at every resync
      and late join (the server's blob). Every unit force block's controller
      is therefore captured and matched under the world-independent alias
      `unit:<owner>/<n>/S<k>` (§4.3), as trafo T3 had to do for the trafo
      sets.
    - **Same binding.** Slot k is the life with join order k. J1 (M1)
      guarantees that every world knows every life's join order before any
      of that life's events is applied. That is why Q7 comes after M1.
    - **Same candidate.** It is P1's body rule, per slot.
    - **Same execution order.** Copies in one graph run in CK's graph order,
      the order the original scripts already rely on in every world. The
      queue section's Create order is compared across worlds (V7, R2).

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

*Revision 3.* The record versions 2-5 are taken (TRD's v3, gap Q's v4 and
v5, `game/script_replay.hpp:103-131` at HEAD), split by two sections that
combine (`has_trafo`, `has_queue`). A third section would double the version
list again, so G3 takes **one** new version, the next free one (6 at HEAD),
whose layout replaces the version-encoded combination with a flags word:

```
u32 version = 6
u32 sections;   bit 0 trafo list, bit 1 queue section, bit 2 hold section
<the v2 body>   byte for byte as in v2-v5
[trafo list]    if bit 0, as v3
[queue section] if bit 1, as v4
[hold section]  if bit 2, as above
```

- A writer emits v6 only when it writes the hold section, so every record
  without one stays v2-v5 byte for byte (the wire check's captures and the
  legacy readers are unchanged).
- `known_version` accepts 6; `has_trafo` / `has_queue` / `has_hold` read the
  word for v6 and the version for v2-v5. A v6 word with an unknown bit is
  refused.
- Q7's K frames per unit probe go into the same hold section with no new
  version: under the Q7 bit the section simply lists more frames, and
  `describe` names their slots.

### 4.3 Savestate

Frames, identity copies and `BMMO_NoBall` have no body. The blob neither contains
them nor refuses on them. The PreSimulate queue entries that the fan creates
against mirrors are F5's business (§2.9). No engine change, no IVP change.

*Revision 3.* A slot copy's force controller lives on its candidate's ball
core like the original's. It is named `rt:force/<block id>` (engine change
#18), so a blob of one process keeps two forces on one core apart. The block
id is not the same in every world (§3 item 10), so a blob that crosses
worlds needs an alias, exactly as the trafo sets did at T3 (engine-changes.md,
"The savestate's controller alias"; T3 measured a joiner's pieces 6.6e-7 m
off and "no live controller 'rt:force/54491'" without it):

- **The alias.** `mechanism_units` keeps, from the copy step on, a map from
  each unit force block's id in this world (slot 0's original and every
  copy) to `unit:<owner>/<n>/S<k>`: the owner script's name, the original
  block's pre-copy walk position n and the slot (the names of §2.10, the
  same in every world). Slot 0 is aliased too, so the rule does not rely on
  the originals' ids agreeing across worlds.
- **One alias function per physics manager.** `set_controller_alias` takes
  one function (physics_savestate.hpp; the retail host through plugin API
  v22, `api_set_controller_alias`, which exists since T4). The host installs
  a chain: `trafo_controller_alias`, then the unit alias, then the own name.
  The two never claim the same name: a trafo set's blocks are not in a
  selected mechanism script. The server's world, the headless client and the
  core host (`trafo_controller_alias_cb` in `core_host.cpp` gains the second
  lookup) install it at the anchor under the Q7 bit and remove it at
  teardown.
- **No blob format change.** The alias is the name the capture records and
  the restore matches by; the blob already carries controller names as
  strings.
- **Checked by** J6 and H3c (§7.3): "no live controller" must not be
  logged at any import, and the joiner's slot force state is bit-equal to the
  server's.

Nothing else changes.

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

### 4.5 Per-player units (revision 3)

- The slot copies are blocks of recorded scripts, so their state rolls back
  with the record (§2.10). No new section.
- The binding is re-derived at every replayed tick from that tick's bodies
  and the lives. Lives change only outside replays (§4.4), so a replay
  across a life's J or R binds the slot as the live ticks did.
- A slot's first Physicalize inside a window is a held birth. The slot is
  unbound until the birth's re-pose, which is the live rule (§4.4).

## 5. Wire, protocol and journal

Protocol numbers follow RSD §11 Q5's rule (the next free number at landing).
The three earlier designs claim 10. This design takes two:

- **P_M** for M1 (member lives; 11 if the three land as one 10);
- **P_G** for G3 (the proximity union and `anchor_probes`; P_M + 1, or the next
  free number then). G5 lands in the same release as G3 and shares P_G.

So no build ever reports a number whose fields it does not send.

*Revision 3.*
- Protocol 13 is the current number (`entity/session.hpp:82` at HEAD), and
  `world_options` uses bits 0 and 1. Q9's order is G3 first, so **P_G is the
  next free number when G3 lands**: 14 at the earliest, unless gap Q's Q5
  takes 14 first. **P_M** is the next free number after that.
- **P_Q** is the next free number when step Q7 lands (§2.10). Q7 adds
  `mechanism_slots` to SessionStart, a world option bit and K in the server
  journal's header.
- Under the Q7 bit, the signature `anchor_probes` also covers the units, K
  and every copy's structure. Q7 changes what the signature covers, not the
  message.

| item | change |
|---|---|
| `PHYSICS_PROTOCOL_VERSION` (ES:51) | P_M: "SessionMember (member lives: added/removed with player_entry and life); player_entry.life; session_event_msg.life; ball leaves always queued". P_G: "WORLD_PROXIMITY_UNION: clients rewire mechanism proximity blocks and identity reads, every world places them before the leavers and the events by the block's metric with a hold; BodyRevived retired; SessionReady carries the probe signature" |
| `entity/session.hpp` | `player_entry.life` (P_M); `WORLD_PROXIMITY_UNION`, the next free `world_options` bit after SPD's `WORLD_PLACEMENT_CLONES` (bit 0) and TRD's `WORLD_TRAFO_INPUTS`: bit 2 (P_G); `WORLD_PLAYER_MECHANISMS`, the next free bit after it (P_Q, §2.10) |
| `session_start_msg` (P_Q) | `uint8_t mechanism_slots` (K, §2.10) after `world_options`; the reader refuses 0 and anything above `MAX_PLAYERS_PER_SESSION` when the bit is set |
| server join orders (P_Q) | below K only; `waiting_orders`; "physics session full (K mechanism slots)" (§2.10) |
| opcode | `SessionMember`, appended (message.hpp:85-100 order rule) |
| `session_member_msg` | §2.7; ReliableNoNagle plus `event_repeats`; about 47 bytes |
| `session_event_msg` | `uint16_t life` after `state` (P_M); 0 from clients |
| `session_ready_msg` | `uint64_t anchor_probes` appended after SPD's `anchor_placement`, read when protocol ≥ P_G, always filled (P_G) |
| server `handle_session_ready` | compares `anchor_probes` with the server's mechanism-class signature after the placement compare (SPD §5), late joiners included: "probe mismatch for X (client %016llx, server %016llx)"; late base clamp (§2.7) |
| server config | `physics_proximity_union`, default on from step G3; the negative control and old-journal replays use off. `physics_player_mechanisms`, default on from step Q7, with the same use of off; K comes from `maximum_members` (§2.10) |
| BodyRevived | not sent, and ignored with a NOTE, under the bit (§2.6) |
| client journal | no layout change: PLAYER(+) at J and PLAYER(-) at R from member messages (§2.8), NOTEs. `announce_player` no longer writes join=255 |
| server journal | no layout change: a ball leave is now EVENT Unphysicalize at `next` and PLAYER(-) at `next + 1`, the queued shape SimTool already reads; a rejoin is PLAYER(-) then PLAYER(+) at R. The header's `world_options` carries the bit; *revision 3:* the field is in the header since journal v5 (T2), so G2 needs no bump. **Q7 adds K to the header and takes the next free journal version** (v7 is the current one, `journal.hpp:106` at HEAD) |
| script record | the next SR `kVersion` after TRD's (§4.2); RSD's portable version the next after RSD's and TRD's, with the section and kind 4 |
| SimTool `--replay-session` | honours the bit (placement point, metric, hold, latch, BodyRevived not applied); PLAYER before inputs and events within a tick (§2.8); under the Q7 bit, builds K slot copies from the header's K and binds them per tick |
| legacy path (MC dispatch) | *revision 3:* nothing. The server refuses a client without the session core (B9DX, answer 7), so neither P_M nor P_G nor P_Q reaches it (§2.4, §2.8) |
| `docs/rooms-and-sessions-protocol.md` | SessionMember, lives, the bits, `anchor_probes`, `mechanism_slots` and the join-order cap |

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
- **Revision 3, per-player units.**
  - The wake, activation and sound blocks are not copied: they keep P1's
    frame.
  - The original blocks of a unit are slot 0. They keep their sources and
    their links, plus the links the copy step adds, and they lose nothing.
  - With one player, every copy is unbound and never acts (§2.10). The
    mechanism then behaves as under G3 alone, so a solo session still
    agrees with the server.
  - Spawn kicks, groups and ranks keep the join order. The cap below K
    changes which order a joiner gets only when every order below K is
    held or retiring, and then the joiner waits instead of taking an order
    of K or more.
  - No IVP change and no engine change: the copies are CK behaviours made
    through the public SDK, and their force controllers are the existing
    block's.

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
| U8 | revision 3, the unit rule (`mechanism_units.hpp`) on abstract-graph fixtures transcribed from L11's descriptions (`.local-tools/work/WAD/cons/l11_P_Modul_18_MF_Script.txt`, `..._29_...`): the fan gives the graph unit `Physics Force` with S = locals 20-21; the bridge gives the block unit {probe, Get Cell, Test} with S empty; the links come out as inside / fan-out / fan-in exactly as §2.10 lists them, with their delays; the parameter sources come out as slot-local or shared; a third shape (a consumer outside the parent graph, or a written local outside U) is refused with the blocks named | exact |
| U9 | revision 3, slot binding over a scripted life sequence (fake platform): lives of orders 0..K-1 with births, deaths, a trafo, a leave and a rejoin with another order; held births in a replay; an unbind (death, leave) while the slot's frame is inside a unit probe's range, then that probe re-armed (Off, In). Server: `assign_join_order` below K; a joiner waits while every order below K is held or retiring, and gets the first released one; with K live lives it is refused with the reason; `maximum_members` = 100 from the start gives K = 64 and refuses the 65th physics joiner with no reload | each slot's candidate per tick equals the life implied by the lives list and the bodies; the latch moves at the stated ticks; an unbound slot's by-name cell and latch read `BMMO_NoBall` from the tick after the unbind, and the re-armed probe's Enter reaches no Test true; the waits and the refusal as stated |

### 7.2 SimTool (offline, `--root C:/Users/geekerwan/Desktop/Ballance-Agent`)

| id | mode | pass |
|---|---|---|
| V1 | `--probe-census` (new; read-only): rewire per class on L1-L12 and the arena; prints per block the frame name, ObjectB, Distance, axes, `Use Outputs`, Barycenter, and per identity read its kind and gating probe, and the signature | mechanism counts equal §1.1.2's table (3, 32, 15, 12, 14, 29, 25, 14, 27, 32, 32, 43, 12); `all` counts equal the server's today (18 ... 24); identity counts equal; **on all 290 blocks**: Barycenter 0, axes 5 or 7, squared 1; every identity read has exactly one gating probe; names unique; with SPD `--placement on`, counts grow by exactly the copied rows' blocks |
| V2 | `--drop` two-player variant (`--drop-player-at` for a second clone): one ball at XZ 40 m / 60 m below a 50 m XZ probe, the other at XZ 55 m level; and P_Modul_29 with a stone ball 3 m from #47687 and a wood ball nearer in 3D to #47688 | the block fires under the bit, not with it off; the bridge breaks under the bit, not with it off (G-c5) |
| V3 | `--replay-session` (bit clear) of the current reference set's server journals (`maps/arena-test/core_ref_s9/*/server.bmjr` as of WG1), with the step's SimTool and with the previous step's. *Revised at WG1:* the row named `lp1_join/server.bmjr` with `matched=3528`; that journal replays 205 of 4135 ticks on every current build (the world changed under it after it was recorded) | every journal `matched` = its ticks, `checkpoint_mismatches=0`; the two SimTools' outputs identical |
| V4 | `--savestate-check` with two players and a probe in range, chained rollbacks across a no-candidate interval (both dead) | blobs and script records byte-identical to the straight run; `skipped` 0 |
| V5 | release: rewire, release, rewire on the same level | the second census and signature equal the first; no `BMMO_Prox_*` left after release |
| V6 | revision 3: `--probe-census --player-mechanisms K` (K = 2 and 8) on L1-L13 and the arena, placement off and on | units per level equal §1.1.2's identity column (58 fans, 13 bridges on L1-L13; placed units added with the placement on), every unit of one of the two shapes; each copy's structure equals its original's (prototype, IO counts, links and delays by the rule, local values, activation); copy walk order and names unique; release (after V5's) leaves the pre-copy graph (block count, links, sources) and the pre-copy signature; the same listing from the x64, x86 (`build/sim-x86-a3`) and Linux SimTool apart from block ids |
| V7 | revision 3: two-clone drops under the Q7 bit: (a) two balls in one fan's box (arena and L11); (b) a wood ball resting inside P_Modul_29's 4 m range, then a stone ball entering | (a) both balls pushed at every tick both are in the box: two controllers `rt:force/<id>` of two slots' blocks, one per ball; the queue section's Create order printed; (b) the bridge breaks at the stone ball's Enter under the Q7 bit, and not under G3's bit alone (the single-ball rule, §2.2) |
| V4b | revision 3: V4 with two balls in one fan box and slot forces on both, chained rollbacks across one ball's death | blobs and script records byte-identical to the straight run |

### 7.3 Headless harness (arena, 4 clients, Arch server, `scripts/arena_map_test.py`)

| id | arm | pass | negative control (HEAD) |
|---|---|---|---|
| H1 | Pusher arrival: client 1 drives to P_Modul_01 first, the others stay behind (beam/hold keys); `--snapshot-interval` normal and snapshots off; 0 ms and `--latency-ms 150` | the Pusher's row equals the server's at every checkpoint in [wake-10, wake+200] in every client journal (`body_series.py`); one wake tick in all five worlds | today's lp1_join numbers: 0.0897 m for 10-16 ticks |
| H2 | fan: a remote ball through the arena's P_Modul_18 box | the remote's mirror and the fan's force state bit-equal to the server's clone through the passage, all clients | mirror off on the non-owners |
| H2b | fan, two balls: A enters and stays, B enters while A is in range, then A leaves the box | in every world the same Get Cell output (by player) and the same force state at every tick; the log names which ball was pushed when (documents §2.2's semantics). *Revision 3, under the Q7 bit:* A and B are both pushed while both are in the box, each by its own slot, and every slot's Get Cell output and force state are equal in every world at every tick | |
| H2c | fan, leave while latched: A's client leaves while A is latched and B is in range; then the same with A dying in the death zone (§9 Q6) | latch moves at the same tick in every world (`relatched` equal); force state equal; the server never reads a destroyed clone | |
| H3 | rope bridge: a remote stone ball onto P_Modul_29 | the plates' rows bit-equal, one break tick everywhere | no break on the non-owners |
| H3b | revision 3: a remote wood ball parked inside the bridge's 4 m range, then another client's stone ball enters; then the stone ball's client leaves while its ball is still inside the 4 m range, and the bridge is reset with a wood ball present | one break tick in all five worlds, at the stone ball's own Enter; the plates' rows bit-equal; no second break after the reset (the left slot's cell reads `BMMO_NoBall`) | G3's bit alone: no break |
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
| J6 | revision 3: J1's late joiner, and J5's rejoin, through the arena fan under the Q7 bit, with another slot's fan force held on a ball at the join | the joiner's slot is its join order in all five worlds (anchor log); no "no live controller" is logged at the joiner's import of the server's blob, and the held force's controller is matched by its alias; its slot's force state is equal everywhere while it is in the box; the rejoin's old slot unbinds at R and its new one binds from its first body | |
| H3c | revision 3: two balls in the arena fan's box with both slot forces held, then one client's resync (`--resync-at TICK` on that client, step A3) imports the server's blob and portable record | no "no live controller" logged at the import; the client's two slot force controllers (`unit:<owner>/<n>/S<k>`) matched by alias; both balls bit-equal to the server's clones from the import on | without the alias: "no live controller 'rt:force/<id>'" at the import (T3's signature) |

### 7.4 Retail runs (core host, against the step's server)

| id | run | pass |
|---|---|---|
| R1 | arena: two retail clients + two headless; a headless ball wakes P_Modul_01 first | the retail journals' Pusher rows equal the server's (as H1); `anchor_probes` equal in SessionReady |
| R2 | L11, two retail clients: one crosses the P_Modul_01 / P_Modul_37 sensors alone; then the P_Modul_18 fan with two balls | as R1 on L11's rows; the fan as H2b (*revision 3:* under the Q7 bit both balls are pushed in both retail worlds and on the server; the retail anchor log's copies and `anchor_probes` equal the server's; the queue section's Create order equal to the server's) |
| R3 | a retail late joiner and a retail existing member (lp1_join recipe with retail client 0) | J1's criteria on the retail logs |
| R4 | session end, vanilla restart of the same level without reload | V5 on retail: the Pusher wakes for the own ball in vanilla play (release restored the sources) |

### 7.5 What each check proves

- U1-U3, V1, V2, V4: the rule itself, its determinism and its rollback state.
- H1-H4, R1-R2: P1/P2 across worlds; H2b/H2c the latch.
- H6: P4.
- U4-U7, J1-J5, R3: J1-J4.
- V3, H8: nothing old moved.
- U8, U9, V6, V7, V4b, H2b, H3b, H3c, J6, R2 (revision 3): P5 and its
  determinism and rollback state; H3c and J6 the controller alias across
  worlds.
- C1: P5's cost.

### 7.6 Cost (revision 3)

| id | check | pass |
|---|---|---|
| C1 | the script record's bytes and the capture time (`t_cap`) at K = 8 against the bit-clear control, with the same keys: the arena 4-client run and one L11 run. The anchor logs `player mechanisms: K slots, U units, C copies, +B record bytes` | recorded against §2.10's estimate, and judged by the stage close-out's CPU checks. A breach is reported with its numbers and goes to §9 Q11; it is never waived |

## 8. Steps

Each step is implement → review (code-review, high) → gate on its checks.

| # | step | files | closes on |
|---|---|---|---|
| M0 | tools: harness `--leave N:T` and `--rejoin N:T`, server `--hold-member-ms` (test builds), `body_series.py` into `scripts/` | scripts, SV | the selftest; the tools reproduce today's lp1_join numbers |
| M1 | member lives and J2 (protocol P_M) | `session_member_msg.hpp`, message.hpp, ES (protocol, `player_entry.life`), EM (`life`), LC, SRN/SRH (lives, always-queued leave, deferred rejoin, callbacks, `leaving_horizon`), SV (`retiring_orders`, `started`, send points, late SessionStart, late base, repeats), CL/CM/CC (`member_life`, `on_session_member`, parking, lookups by life), CJ, ST, SC/CH (decode, mailbox) (*revision 3:* MC's legacy end dropped, answer 7) | U4-U7; J1-J5; R3. Comes after G-c (Q9, §9) |
| G0 | `--probe-census` | ST | V1 (bit off) |
| G1 | parts A+B; the server on `proximity_union` with the bit clear (today's path, moved) | `proximity_rules.hpp`, `proximity_union.hpp`, PW | U1; V1; V3; the arena regression identical to the pre-step build |
| G2 | the server under the bit: `place_probes()` before the leavers and the events, the metric, gating choice, latch, hold, signature | PW, SRN, ST (`--replay-session` honours the bit), ES (bit), server config | V2, V4 (server-world parts); V3 still matched with the bit clear |
| G3 | clients: core step 7b, RB hook, `core_world_hooks.place_probes` bound in HW, platform op, record version, H host anchor/release; SessionReady `anchor_probes`; protocol P_G | CF, CC, RB, HW, CP, RBS, SR, SC, SV (compare) | U2, U3; V4; H1-H5, H2b, H2c, H7, H8 |
| G4 | RC host (*revision 3:* RL's local end dropped, answer 7) | CH | R1, R2, R4 |
| G5 | BodyRevived retired (same release as G3, P_G) | CF, SV | H6 |
| G6 | docs: this file's as-built notes; the SR:29-37 comment; RSD §2.4 amendment; protocol doc; `collision-overhaul-design.md` 9.x entry | docs, SR | review |
| Q7 | revision 3: per-player mechanism units (§2.10, P5; protocol P_Q): the unit rule, the copy step and release, slot binding and names, K in SessionStart and in the server journal's header, the join-order cap and `waiting_orders`, the bit and the config, `--player-mechanisms` in SimTool, the RC and H anchor calls | new `session/mechanism_units.hpp`; `game/proximity_union.hpp`; PW, SRN/SRH, SV, ES, `session_start_msg.hpp`, `journal.hpp`, CF/CC (`probe_candidate.slot` from the lives), SR/RBS (the hold section's K frames), SC, CH, ST, server config; docs (this file's as-built note, the protocol doc) | U8, U9; V6, V7, V4b; H2b, H2c, H3b, H3c, J6; R2; C1 |

**Order.** G1-G5 come after F5, F6 and F7 and after R3-R5
(`lifecycle_rewind` on RC), as the user's order already has it (audit §5:
"... F5, F6, F7, G-c, join-order message ..."). G3 needs RSD's
`session/resync_rules.hpp` (the `keep_local` predicate) and RSD A1 for the
portable section, or it adds the section to A1's format when A1 lands. G3's
record version is taken after TRD's T1 (§4.2). M1 needs nothing but its
protocol number; if it lands before G3, G3 builds on its lives.

*Revision 3, the order (Q9 answered, §11.1).* The order is G0, G1, G2,
G3 with G5, G4, G6, then M0 and M1, then **Q7**.
- Q7 binds slot k to the life that holds join order k. Before M1, an
  existing member does not know a late joiner's join order (the placeholder
  63 of §1.2.1, `join=255` in its journal), and nothing tells it about a
  leave (J-c). Such a member would bind the joiner to no slot, or to the
  wrong one.
- Q7 also needs G4's RC host for its retail anchor call, and M1's
  `assign_join_order` and `waiting_orders` path for the cap.
- So Q7 cannot follow G3 directly, as the chain's plan has it (step WGQ7
  "right after WG3"). It must follow WM1. Nothing in Q7 has to come
  earlier.
- Between G3 and Q7 every world runs the single-ball rules of §2.2. They are
  deterministic: the fan pushes the latched ball, and the bridge tests the
  ball whose Enter fired.

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
  Q7 of 2026-10-24). *Built into the design at revision 3 (step WAD):*
  §2.10 (the units, the copy step, slot binding, K, the record), P5, §3
  item 10, §4.5, §5's P_Q rows, §7's U8, U9, V6, V7, V4b, H2b, H3b, J6,
  R2 and C1, and §8's step Q7.

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
  moot; M1 and G4 add nothing to the legacy path. *Done at revision 3:*
  MC is out of M1's and G4's rows, and §2.4's RL row, §2.8's legacy
  paragraph and §5's legacy row say "nothing".

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
- *Revision 3:* step Q7 (Q7 (b)) comes after M1 and takes the next free
  number after P_M (P_Q). It needs J1 (§8 Order).

**Q10. Does a behaviour copy come out the same in retail CK2 and in
Ballanced?** (measurement)

- §2.10's copy step copies behaviours with `CKContext::CopyObjects`. Copying
  a behaviour copies its IO, its parameters and its sub-behaviours. Two
  details are not established for either implementation:
  - what happens to a copied input parameter whose source lies outside the
    copied set (kept or cleared), which the step overwrites anyway;
  - whether the Proximity block's settings-built outputs (`Use Outputs`)
    come out equal.
- Settle: V6 lists every copy's structure on the x64, x86 and Linux
  SimTool. R2's retail anchor log prints the same listing, and the
  signature (§5) compares it with the server's.
- If retail's copies differ, build each copy by hand from its prototype:
  `CreateObject(CKCID_BEHAVIOR)`, `InitFromGuid`, the settings with the
  block's settings callback, then the values, links and sources of §2.10.
  Compare again.

**Q11. Is the record and capture cost of K slots within budget?**
(measurement)

- §2.10 estimates +24 KB on L11 and +50 KB on L12 at K = 8, against
  records of about 40 KB. C1 measures the bytes and `t_cap`.
- If the stage close-out's CPU checks fail on it, the numbers are reported
  with one option that is designed but not built:
  - **dormant slots.** When a slot unbinds, its copies are reset to their
    anchor state in every world. While it is unbound, the fan-out links to
    it are gated by its binding, so its copies never run and stay in that
    state. The record writes them as one byte, the way the trafo record
    writes `kTrafoDormant`. This changes §2.10's carry-over between two
    lives of one order into a reset.
  - Its trade-offs (a binding gate in the graph and a reset on binding)
    are then the user's to weigh against the cost.

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

### 10.1 Revision 3 (step WAD, 2026-10-28): what changed, and the adaptations

| answer | change |
|---|---|
| 6, Q7 = (b) | **P5** (§1.1.5) and **§2.10**: per-player units of the sub-graphs behind the 71 identity reads (58 fans, 13 bridges on L1-L13; WAD's consumer census, §1.1.2). The CK-free unit rule has two admitted shapes; the copy step at the anchor; K = the room capacity, sent in SessionStart; slot k bound to the life with join order k; the frames, cells and latches per slot; the join-order cap. §2.1 part I; §2.2's several-balls paragraph; §2.9 (SPD, gap Q, M1); §3 item 10; §4.2, §4.3, §4.5; §5's P_Q rows; §6; §7's U8, U9, V6, V7, V4b, H2b, H3b, H3c, J6, R2, C1; §8 step Q7; §9 Q10, Q11 |
| 7, Q8 superseded | §2.4's RL row, §2.8's legacy paragraph, §5's legacy row and M1's and G4's MC parts now say "nothing": the server refuses every client without the session core (B9DX) |
| 5, Q9 | §8 Order: G0-G6, then M0 and M1, then Q7. P_G is the next free protocol number at G3, P_M the one after, then P_Q (§5) |

Adaptations and findings recorded at this revision:
- **Step Q7 must follow M1, not G3.** The chain planned WGQ7 right after
  WG3. Slot binding needs every world to know every life's join order (J1).
  Before M1 a late joiner's order is unknown to the existing members, so Q7
  goes after WM1 (§8 Order). This is a dependency, not a trade-off. The
  orchestrator re-orders the chain.
- **The bridge break is one hinge.** The Test's True output shuts down one
  hinge, #47108 on L11, through `10 Hinges` input 2. Revision 2 said "the
  ten hinges", which is the reset's input 1 (§1.1.2). A second Shutdown on
  that hinge does nothing, and the copies rely on that (§2.10).
- **The journal header has `world_options` since v5**, so G2 needs no
  journal bump. Q7 takes one for K (§5).
- **The record versions 2-5 are taken** (TRD v3; gap Q v4 and v5), so G3's
  hold section takes the next free version (§4.2). Review finding 4: that
  is one version (6 at HEAD) with a section flags word, not one version per
  combination; records without the hold section stay v2-v5.
- **Review of the revision (gate round, 2026-10-28).** Four findings, all
  confirmed against the code and fixed in the text:
  1. *A slot copy's force controller name crosses worlds.* §3 item 10 and
     §4.3 said `rt:force/<block id>` is compared inside its own process
     only; a resync's or late join's blob is the server's, and each world
     copies at its own anchor, so the ids differ (trafo T3's failure,
     engine-changes.md). Fixed: every unit force block, slot 0 included, is
     captured and matched under `unit:<owner>/<n>/S<k>` through the one
     `set_controller_alias` chain (after `trafo_controller_alias`; the retail
     host through plugin API v22, which exists since T4). Checked by J6's
     new criterion and the new harness row H3c. The same exposure for the
     originals and SPD's placed copies without the Q7 bit is not measured:
     queued as WAD-A (§2.9's SPD row, roadmap §4 item 5).
  2. *Unbound slots could act.* The by-name cell of an unbound slot held
     its last entity, so a held `Ball_Stone` plus a bridge reset re-arming
     the slot's probe with the held frame inside 4 m broke the bridge with
     no stone ball present (the same in every world, so no divergence, but
     against "unbound slots never act"). Fixed: an unbound slot's by-name
     cell reads `BMMO_NoBall`, like the body kind; it then needs no hold
     entry. The frame is not parked on unbind (that would fire an Exit the
     original never sees). U9 and H3b gain the re-arm case.
  3. *The refusal is reachable without a reload.* The room cap is clamped to
     256 (SV:287, SV:2326-2328), K to 64. §2.10 and U9 now say so.
  4. *The record version combination was open.* §4.2 names it (above).
- **Census evidence:** `.local-tools/work/WAD/consumers.py` and
  `consumers.txt` (SimTool `--describe-script` of every distinct mechanism
  script on L1-L13, placement off), with the descriptions under `cons/`.
  Revision 2's scratch census (the scratchpad path at the top) is no
  longer on disk. K0's `.local-tools/work/k0/census` holds the
  identity-read lists.

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
  every player). It confirms §11's Q7 above. *Done at step WAD (revision
  3):* §2.10, P5 and step Q7 (§10.1).
- **Q8 is superseded** by answer 7, verbatim: "7. 直接拒绝，不要继续兼容非session
  core的客户端" (refuse it outright, no more compatibility with clients
  without the session core): built at step B9DX, the server refuses every
  such client, so the legacy path's local ends of this design (§2.4 RL,
  §2.8, §5's legacy row, M1's and G4's MC) are moot.
- **Q9**, answer 5, verbatim: "5. 你决定" (you decide) - the orchestrator
  chose G-c (G0-G5) before the member lives (M0, M1); the reason is in §9
  Q9.

## 12. As built

### As built (step WG0, 2026-10-28): G0, the census

**What exists.** SimTool `--probe-census` (`--level N [--placement on]
[--world-options B]`, a mode of its own): `BallanceMMOServer/sim/probe_census.hpp`
(new, SimTool only) and its dispatch in ST (`run_probe_census`). It builds
`--level`'s session world (`create_session_world`, the server's
`physics_world`), takes the selection every world makes
(`session_script_selection` with the world's trafo bit) and prints, per
block of the structural set, its class, the §2.3 frame name
`BMMO_Prox_<owner>_<n>`, ObjectB, Distance, `Check Axis`, `Use Outputs`,
Barycenter, `Squared Distance` and the exactness inputs; per identity read
its §2.3 copy name `CurrentLevel#BMMOId_<owner>_<n>`, its kind, its gating
probe and (revision 3) its unit; the §2.3 signature of the mechanism class
and of `all`; and V1's checks as `check ... PASS|FAIL` lines with a last line
`PROBE-CENSUS: PASS|FAIL` (exit 0 / 3, 1 when the world fails).
`.local-tools/work/WG0/census_all.py SIMTOOL OUTDIR` runs L1-L13, placement
off and on, in parallel and compares the two runs of each level.

**Adaptations (the code is what it is).**
- *Read-only, so on the server's rewired world.* G0 comes before G1, so
  there is no `proximity_union::rewire` to call, and the only session world
  SimTool has (`physics_world`) has already rewired its own blocks at its
  anchor (PW `rewire_proximity_probes`, `rewire_ball_identity_reads`). The
  census therefore rewires nothing: it computes what §2.3's rewire takes
  per class and the names it gives. Its structural test accepts an ObjectA
  of `Ball_Pos_Frame` or of a server frame `BMMO_Prox_*`, and an identity
  read's target of `CurrentLevel` or a server copy `CurrentLevel_BMMO_Ball_*`
  (same columns). The check `server rewire` asserts that the server took
  exactly the structural set: no block left on `Ball_Pos_Frame`, the
  census's count equal to the world's (`proximity_probe_count`), no read of
  a rewired script left on `CurrentLevel`. G1 replaces the reading of the
  server's names when the server moves onto `proximity_union`.
- *Classes.* `mechanism` is §2.3's predicate (root script in the selection,
  not `keep_local_script`); the others are named for the table: `balloon`
  (keep-local), `flame` (owner `PC_*`/`PS_*`), `extra` (`P_Extra*`),
  `other` (none on any level). An identity read belongs to a class when its
  root script has at least one probe of that class; its gating probes are
  taken among that class's probes, from the links of the Get Cell's own
  graph (an output IO of the probe to an input IO of the Get Cell).
- *The signature's bytes* (§2.3 lists the fields, not the layout): FNV-1a
  64 over the class's blocks in (owner, script, walk) order, each `str
  frame, str ObjectB, f32 Distance, i32 axes, i32 Use Outputs, f32
  exactness min, f32 exactness max`; then the identity reads in the same
  order, each `str copy, u8 kind (1 by-name, 0 body), str gating frame`
  (empty unless exactly one); then `str` the entity
  `CurrentLevel[0][ActiveBall]` names (`Ball_Wood` on every level); then the
  `ResetPoints` row 0 position as 3 x f32. A `str` is a u32 length and the
  bytes; numbers are little-endian. G1's `proximity_union::signature()` is
  to give the same values (the table below is its reference).
- *The unit (§2.10) is measured, not refused.* The unit rule and its
  refusal belong to Q7 (`session/mechanism_units.hpp`); the census applies
  §2.10's definitions read-only and asserts WAD's two shapes. D: every block
  with an input or a target whose real source is the Get Cell's output, a
  destination of it, or the output of a parameter operation reading one of
  those. C: the blocks of the Get Cell's graph on a behaviour-link path
  (links of that graph between its children) from the gating probe to a
  block of D. S: that graph's locals that a block of U writes (an output's
  destination) and a block of U reads. Shape `graph` when U is every child
  of a sub-behaviour graph and S is every local a child of it writes,
  `blocks` otherwise. Refused (check `unit shapes` FAIL): a block of D or
  the gating probe outside the Get Cell's graph, a `blocks` unit with
  state, or anything but the fan (`graph`, D = {Box Box Intersection,
  SetPhysicsForce}, C = 2 Binary Switch + 2 Identity, |S| = 2) or the
  bridge (`blocks`, D = {Test} through `Get Name`, C and S empty). Found:
  the fan's SetPhysicsForce takes the ball through its input 1 `Pos
  Referential` **and** its target (§2.10 names only the target); Box Box
  Intersection through input 0 `Entity 0`; the bridge's Test through input
  1, from the `Get Name` operation.
- *Placement.* Each copy the world's placement made (`placement().copies()`)
  is matched with its row's original: the copy's structural blocks (found
  by the copy's own object list) must equal the original's (its scripts
  walked), per class (check `placement copies`); the §1.1.2 counts, which
  the tool carries for L1-L13, are compared with the census minus the
  copies' blocks.

**Results** (x64 SimTool, `--root C:/Users/geekerwan/Desktop/Ballance-Agent`,
`.local-tools/work/WG0/v1c`; every run `PROBE-CENSUS: PASS`, `CENSUS-ALL:
PASS`; the signatures are the same in three runs):

| level | all / mechanism, placement off | identity body / by-name | signature (mechanism, off) | placement on: copies, all / mechanism, units (placed) | signature (mechanism, on) |
|---|---|---|---|---|---|
| 1 | 18 / 3 | 0 / 0 | `114852193bd4e4c6` | 23, 31 / 3, 0 (0) | `114852193bd4e4c6` |
| 2 | 47 / 32 | 9 / 1 | `97f181c37c994ea5` | 26, 74 / 41, 13 (3) | `5ee9383bab2d1ea9` |
| 3 | 32 / 15 | 3 / 1 | `7766ad6162079841` | 17, 43 / 16, 4 (0) | `e31825e59490084a` |
| 4 | 30 / 12 | 3 / 1 | `d4d42a426535af8a` | 29, 51 / 15, 5 (1) | `7a4bd76aef13b4d6` |
| 5 | 28 / 14 | 2 / 0 | `6914d9f9a53c4ea1` | 27, 50 / 23, 3 (1) | `f93386ef03be698d` |
| 6 | 43 / 29 | 8 / 2 | `ee920746cec7334a` | 25, 70 / 44, 15 (5) | `e329d4795b288466` |
| 7 | 39 / 25 | 4 / 2 | `72b3926dc08e669a` | 28, 65 / 39, 10 (4) | `d47b1bf38b0310b2` |
| 8 | 29 / 14 | 2 / 1 | `1a9a91787766b44d` | 39, 63 / 35, 9 (6) | `0813ea4dd1855f26` |
| 9 | 43 / 27 | 5 / 1 | `ebfef3e92d176b4e` | 46, 90 / 61, 16 (10) | `03e0c6df3cc40c64` |
| 10 | 47 / 32 | 5 / 2 | `f597b4fd51b3e3cf` | 47, 97 / 71, 18 (11) | `d363e670b07d2aad` |
| 11 | 47 / 32 | 5 / 1 | `40d4a5cd58c5e49c` | 53, 91 / 63, 11 (5) | `6fb28962d52e7d48` |
| 12 | 60 / 43 | 11 / 0 | `54dfd2fdb859a84a` | 62, 136 / 89, 26 (15) | `6048b208ea5a437b` |
| 13 (arena) | 24 / 12 | 1 / 1 | `3f04691f6dae5292` | 0, 24 / 12, 2 (0) | `3f04691f6dae5292` |

- Every count equals §1.1.2's table: the mechanism column sums to 290, the
  identity reads to 58 body (fans) and 13 by-name (bridges); `all` equals
  the server's own count on every level, placement off and on (the server
  rewires the copies too); every level's `all` is mechanism + 7 flames +
  extras + 2 balloon blocks, `other` 0. The `all` identity reads equal the
  mechanism ones on every level: no flame, extra or balloon script reads
  `ActiveBall` (check `all identities` asserts it).
- The placement-on signature is the one the server runs since SP10
  (`physics.placement_clones` on by default); it differs from the
  placement-off one wherever the copies hold mechanism blocks or identity
  reads, and equals it on L1 and L13 (no copied mechanism block).
- On all 290 mechanism blocks: Barycenter 0, `Squared Distance` 1, axes 5
  (219 blocks) or 7 (71). Every identity read has exactly one gating probe.
  Every frame and copy name is unique and none holds `_BMMO_` or `_Peer_`,
  placement on included.
- Units: 71 with the placement off (58 `graph` fans, S = locals 20
  `TurnON_Permission` and 21 `TurnOFF_Permission`; 13 `blocks` bridges),
  132 with it on (114 fans, 18 bridges): the placed copies of fans and
  bridges are units of the same two shapes.
- With the placement on, every copy holds exactly its original's blocks,
  per class, and the counts grow by exactly the copied rows' blocks (the
  census minus the copies equals the placement-off run on every level);
  L11's 53 copies are SP8's 53. Most copied blocks are extras (`P_Extra`
  rows: L12 +30).

**Not in G0.** V1's listing from the x86 and Linux SimTools is V6's (step
Q7); the release round trip is V5 (G1). `probe_census.hpp` has been compiled
by MSVC x64 only: the GCC syntax check (`linux_test_syntax.sh`) covers the
test target and the session core, not SimTool, so the first GCC compile of
the census is the Linux SimTool build of V6.

### As built (step WG1, 2026-10-28): G1, parts A and B, the server moved onto them

**What exists.**
- *Part A*, `BallanceMMOCommon/include/session/proximity_rules.hpp` (new,
  CK-free): `probe_ball`, `probe_candidate` (the CK-free type §2.5 moves out
  of CP's way; `bmmo::game::probe_candidate` is an alias of it),
  `probe_metric(a, b, axes)` (SPX's order: z, y, x, each `d*d` added onto 0,
  `d = b - a`) and `probe_nearest(balls, reference, axes)` (strict `<`, so
  the first, i.e. the lowest player id, on a tie; -1 when empty; the ball is
  ObjectA's posA, the reference ObjectB's posB, as the block has them).
- *Part B*, `BallanceMMOCommon/include/game/proximity_union.hpp` (new,
  public CK2 API only): the class `proximity_union` with §2.3's `rewire`,
  `place`, `capture` / `restore` (§4.2's hold section), `signature`,
  `release`, `describe`, `skipped`, `relatched`, and `has_body` (the host's
  body test for place's consistency check); `proximity_operation_on` is
  PW's `operation_on`, moved unchanged.
- *The server* (PW): the anchor calls `proximity_.rewire(all, {},
  options)` where `rewire_proximity_probes` / `rewire_ball_identity_reads`
  ran, and `classify(script_selection_)` once the selection exists; the
  tick calls `place_proximity_legacy()` where the two updates ran; the world's
  destructor releases the union before the placement's release. The anchor
  logs one more line, `world: proximity union: N blocks (mechanism M), K
  identity reads (mechanism J), signature mechanism S all A, bit clear`.
- *SimTool*: `--probe-census` now also checks `server names` (the server's
  frames and copies carry the census's §2.3 names) and `union signature` (the
  world's `proximity_union::signature()` equals the census's, mechanism and
  all); `--probe-census --release-check` is V5 (below). The S7 savestate
  scene's per-tick hook calls `place_proximity_legacy()`. `array_census.hpp`
  reads the new copy names as server copies.
- *U1*: `BallanceMMOServer/tests/proximity_rules_test.cpp` (3 tests, in
  `BallanceMMOMessageTests`).

**Adaptations (the code is what it is).**
- *Two paths in one class.* "The server on `proximity_union` with the bit
  clear (today's path, moved)": `place_legacy` is the server's code moved
  unchanged (3D `SquareMagnitude`, the nearest clone, the `Ball_Pos_Frame`
  fallback, the identity by the nearest player to ANY ObjectB of the script,
  the parked retail ball as its fallback), and `rewire_options::union_rules`
  false keeps the server's rewire: frames start at `Ball_Pos_Frame`'s world
  matrix, no `BMMO_NoBall`, a read is rewired whatever its gating probes, a
  failed creation skips that block. `place` and `union_rules` true are §2.2 /
  §2.3 under the bit, for G2 (the server) and G3 (the clients); in G1 only
  V5 exercises the union-rules rewire, and `place`, `capture` and `restore`
  are compiled but run by no check yet (G2's V2 and V4 are their checks).
- *The creation order is kept, the names change.* The rewire walks the
  behaviour list as the server did, frames (entity, then its parameter local)
  and then the copies (CopyObject, AddRow, parameter local), with the same
  CK calls in the same order (`GetValueObject` and `GetValue` with their
  default update where the server used them; every new read is
  `update = FALSE`), so every later CK object gets the id it had before G1.
  The objects take §2.3's names (`BMMO_Prox_<owner>_<n>`,
  `CurrentLevel#BMMOId_<owner>_<n>`; the parameter locals carry the same
  names); nothing reads them but the logs and SimTool's censuses.
- *The server rewires before its selection exists.* §2.3's `rewire(ctx,
  which, sel)` takes the selection; the server's anchor makes the selection
  after the rewire (and after `anchor_trafo`), so it passes an empty one
  for class `all` and calls the new `classify(sel)` afterwards. Moving the
  rewire after the selection would move the CK objects it creates.
- *Release at world teardown.* §2.4's SV release runs in `~physics_world`,
  before `release_placement` (whose copies' blocks the union may have
  rewired; `release` looks every block up by id and skips one that is gone).
- *The union rules refuse, the bit-clear path does not.* With `union_rules`
  a missing `Ball_Pos_Frame` / `CurrentLevel` / ActiveBall column, a block
  with Barycenter 1, a read with no or two gating probes, or a failed
  creation releases everything done and returns the reason (all or nothing).
- *V3's journal no longer replays on the pre-step build.*
  `maps/arena-test/lp1_join/server.bmjr` (an old protocol's journal)
  replays `matched=205` of 4135, `first_divergence=232`,
  `checkpoint_mismatches=1965` with the pre-step SimTool (build-agent-rWG0)
  already: the world has changed under it since it was recorded (V3's old
  `matched=3528` is not reachable by any current build; §7.2's V3 row is
  rewritten to the recipe below). V3's intent ("with
  the bit clear PW keeps today's path, old journals replay unchanged") is
  judged as the pre-step and the step's SimTool giving the same replay
  (identical summaries and stdout, the anchor's new log line and wall-clock
  fields aside), and by the current reference set's server journals, which
  both replay in full (below).

**Results** (x64 MSVC; `.local-tools/work/WG1`).
- U1: `BallanceMMOMessageTests` 590/590 (3 new). The pinned constants are
  the same from `u1_gen.cpp` built by GCC 16.2.1 (WSL Arch, -O2) and by MSVC
  x86 (PE machine 0x14c, /O2 /fp:precise): `u1_gcc.txt` = `u1_x86.txt`.
- V1 + V5 (`v1v5_all.py`, L1-L13, placement off and on, 26 runs): every run
  `PROBE-CENSUS: PASS` (`server names` and `union signature` included) and
  `RELEASE-CHECK: PASS`; every signature equals WG0's table (mechanism and
  all); every census line equals WG0's run (`.local-tools/work/WG0/v1c`) but
  the server copies' names. V5: the release puts back every input (arena:
  26 of 26 = 24 blocks + 2 reads), leaves no `BMMO_Prox_*`,
  `CurrentLevel#BMMOId_*` or `BMMO_NoBall` object, and the census then finds
  every block on `Ball_Pos_Frame` and every read on `CurrentLevel` with the
  same signatures; the bit-clear rewire again gives the first census's
  signatures; the union-rules rewire of the mechanism class (the clients'
  rewire) takes exactly the census's mechanism blocks and reads (arena 12 and
  2), makes `BMMO_NoBall` and gives the mechanism signature; its release
  leaves nothing (arena 14 of 14).
- The arena regression: the server journals of `core_ref_s9`
  (resync_snapoff 4122, swing_snapoff 3888, l8_cross 3166 with the
  placement, solo1 2911, solo2 2895 ticks) replay `matched` = ticks,
  `checkpoint_mismatches=0`, with the step's SimTool and with the pre-step
  one, the two outputs identical (`server_replays.sh`); the wire check
  rWG0 vs rWG1 over `core_ref_s9`: 12/12 sends SAME, 12/12 journals
  IDENTICAL, `WIRE-CHECK: ALL SAME`.
- Live smoke (Arch server rebuilt from this tree, 4 headless clients, the
  harness's default four sector sessions of 60 s, snapshots off, placement
  on): the four server journals replay 4518/4590/4251/4612 matched, and the
  pre-step SimTool replays the same four journals in full too (the server's
  world under the step is the pre-step code path's for the same inputs).
  Sector 1 PASS; sectors 2-4 FAIL on resyncs and input starvation (sector 4:
  clients 0 and 1 rebased at 886 and ran ~22 ticks late for the rest of the
  session, starvation 2755/3581). UNATTRIBUTED: its signature is input
  relay (sector 2: 7-11 resyncs and rebases per client, relayed frames past
  the 48-tick rollback reach, relay lag max 60-61, starvation 431-606), and
  no pre-step control at the same settings was run (the 15-minute budget);
  the earlier four-sector smokes `el6_smoke` / `el6_smoke2` (0 resyncs, 0
  starvation) ran a 150 ms proxy and are no control either. What is
  established: the server's world under the step is the pre-step code's for
  the same inputs (the replays above), the headless clients are unchanged
  (the wire check), and no path of the diff reaches input timing (the
  per-tick cost fell: one `retail_ball_entity` lookup per player instead of
  per read and player; the release runs only at world teardown, between
  sessions). Queued for FULL: a pre-step four-sector control at the same
  settings (`build-agent-rWG0` clients, the pre-step Arch server).

**Left for later steps** (the review's notes, none reached by G1's bit-clear
path).
- *G3, re-anchor without release.* `rewire()` starts with `clear()`, which
  forgets the objects of a previous rewire without releasing them; a host
  that re-anchors on the same level without `release()` would find no
  `Ball_Pos_Frame` source, rewire 0 blocks and leak the old frames (visible
  only as a signature mismatch). G3 makes `rewire()` release a non-empty
  union first (`if (!probes_.empty() || !identities_.empty())
  release(context);`), with V5's rewire-release-rewire as its check.
- *G2, the latch and a null output.* Under the bit `place()` re-points a Get
  Cell output that is still null (a block that has never run) to
  `BMMO_NoBall` or the chosen ball, and `relatched` counts it; §2.2 speaks of
  an output that "names an entity that is not" a candidate's. G2 either
  skips a null output or records this reading in its as-built note.
- *G3, the hold section order.* The capture orders the hold section (owner,
  script, walk); §4.2 says walk order per owner, selection order. Equal in
  every world, but G3 records it as an adaptation when it puts the section
  in the savestate.
- *G3, the retail SDK.* `proximity_union.hpp` uses the public CK2 API only
  (§2.3) but has been compiled against the x64 server SDK and GCC only; the
  first x86 translation unit including it (G3's, or a compile-only TU in
  `build/sim-x86-a3`) is its check.
- `game/checkpoint_phase.hpp:35` still names
  `physics_world::rewire_ball_identity_reads` in a comment (a retail-compiled
  header; G6's documentation pass).

### As built (step SMK, 2026-10-28): the WG1 smoke regression attributed - a host storage stall, not code

WG1's live smoke (above, "UNATTRIBUTED") is attributed. No code changed.

- **What the metrics count.** A client's `frames` (status line) is the
  number of steps since its tick base, so it restarts at every rebase /
  resync: `frames` 311 of tick 3585 means "the last base was 311 ticks
  ago", not "311 steps were run". `starved` counts the server's input
  reports that found no fresh frame from the client; `lead` is how far the
  client's own tick runs ahead of the server's. The pacer
  (`session_client.cpp:2699-2713`) rebases a client whose schedule is more
  than `rebase_threshold()` frames behind and asks for a resync.
- **Where the time went (WG1's capture, sector 4).** The inbound captures
  record every wall-clock read (`.local-tools/work/SMK/gaps.py`,
  `gapabs.py`): every gap over 100 ms (11-13 per client) lies between
  `pace_expected` and the next pass's `run_report`, i.e. inside `frame()`,
  never in the pacer's sleep; on the shared steady clock the four clients'
  gaps END at the same instant (to 1 ms: e.g. 17.765-17.776 s ->
  18.935-18.936 s, the 1.16 s stall that rebased clients 0 and 1 at 886)
  while they start up to 30 ms apart. Four processes released together are
  blocked on one external resource, not running code. The server's journal
  (`align.py`: header `utc_ms` + TICK `ms`) has its step gaps in the same
  wall windows, although it runs in the WSL VM: the windows BEGIN together
  (server 9.745..9.852 / clients 9.724..9.843 s; 13.825..14.017 /
  13.815..14.033; 20.646..20.835 / 20.633..21.804 - the clients' third
  window ends ~1 s after the server's, it is the 1.16 s stall above), so
  the stall reaches Linux and Windows processes alike. The server's
  windows recur on a ~1.36 s cadence (9.745 s + 1, 3, 6, 7, 8, 9 x ~1.36 s:
  11.100, 13.825, 17.921, 19.261, 20.646, 22.034; 17.327 is off it), a
  periodic outside event rather than anything the session does per tick. Sector 2 c0: a 1887.9 ms frame = its 124-tick rebase at 690; the
  server held all four players' inputs 689..754 (`S2 held`), step max
  716 ms. The four clients of each sector share their maximum frame time
  (s2 1888-1911 ms, s3 2189.5-2189.6, s4 1160-1171).
- **The cause.** Every process involved writes to the one NVMe drive D:
  (Acer SSD N7000 2TB, disk 1): the clients' journals, logs and captures,
  the server's log through 9P, and the Arch VM's ext4 (its `ext4.vhdx` is
  `D:\Arch`, so the server's journal too). NTFS's hourly IO latency summary
  for D: (`Microsoft-Windows-Ntfs/Operational` event 170, interval
  14:54-15:54, which holds WG1's smoke 15:16-15:21) has `Write: Paging,
  NonCached, Sync` 85 writes at 0.5-1 s, 29 at 1-2 s, 4 at 2-10 s, `Write:
  NonPaging, Cached, Sync` (an ordinary buffered write) 5 at 0.5-1 s and 4
  at 1-2 s, `File flush` max 1 s; the hour before (13:54-14:54) has none
  over 512 ms. The drive stall is the trigger; what lets it reach the
  tick is the code's: every write of a session runs synchronously on the
  tick thread - the journal flushes the stream after every player, event,
  checkpoint, note, correction and resync record
  (`session/journal.hpp:817-843`, `record()`), the headless client's
  `logf` / `log_line` `fflush(stdout)` after every line
  (`sim/session_client.cpp:692-707`; stdout is `clientN.log` on D:), the
  inbound capture `fwrite`s through the CRT buffer
  (`sim/inbound_capture.hpp:270`), and the server's journal and log are
  written from its step loop the same way. The shared core does the same
  in the retail mod, on the game thread, where the same stall is a
  1-2 s game freeze. A write that waits 1-2 s inside `frame()` puts the
  client past the rebase threshold: rebase, resync, inputs late at the
  server (starvation, relayed frames past the 48-tick reach). The
  capture has no per-write timing, so which write each gap waited on is
  not shown gap by gap; the `Paging, NonCached` bucket can also stall a
  process on a page fault, which no writer thread removes. Queued as its
  own item (roadmap §4 item 2, "Session I/O on the tick thread"): the
  journal, log and capture writes move to a buffered writer thread (or
  at least the per-record flushes become asynchronous), so a storage
  stall delays the files, not the tick. Why the drive
  stalled (its own write cache / garbage collection after the step's
  writes, or a monitoring tool's polling) is outside the code and was not
  identified; REF9's swing arm saw the same signature once (one 455-470 ms
  frame in all four clients, 14:07-14:14, in the 256-512 ms bucket of that
  hour).
- **Not reproduced at HEAD 949f960, two runs** (WG1's exact arguments:
  `s9_run.sh <label> snapoff 4 60 13 true`, clients `build-agent-rWG1`,
  through `.local-tools/work/SMK/mon_run.sh` with typeperf / cpu_sampler /
  vmstat monitors; stores `maps/arena-test/smk/head1|head2`, per-sector
  `maps/arena-test/s9x_head1|head2`): head1 (15:38-15:44, the Arch binary
  WG1 ran) and head2 (15:54-15:59, started 3 s after relinking the Arch
  server from HEAD, `ballanced-36c04e3d24c9+bmmo-949f960cefe9`, WG1's
  precondition): 4/4 sectors PASS in both, every client starved 0,
  rebases 0, resyncs 0, relay_late 0, lag max 1-12, frame max 18-40 ms,
  server step max 14.8 / 15.1 ms, no capture gap over 50 ms; mechanisms
  3/17/9/32 per sector (WG1: 3/17/9/14), journals replay in full. The
  sector-4 14 is the stall's resyncs, not fewer modules reached: WG1's
  sector-4 clients also had 32/32 (c0 at tick 304, c1 331, c2 384, c3
  420), then 0/0 after the resync at 886 (c0 966, c1 974) and 14/14 to
  the end (c0 1261, c1 1300, c2 1065, c3 1080). `mechanisms=` is the
  client's comparison registry (`mechanism_tracking_`, known/resolved),
  which a resync or re-assign clears (`client_core.cpp:642`, `:703`) and
  which learns names only from the Mechanism rows of a snapshot
  (`client_core_messages.cpp:73-76`); with periodic snapshots off, after
  a late resync it holds only what that resync's snapshot named. The
  code is from 2026-10-22 (d64ff882, 90298726), before EL6. Sectors 2
  and 3 resynced too and kept 17 and 9. It is a snapshot-path
  diagnostic, not the world's content (the world's exactness is the
  server journal replay); not queued. The WG1
  numbers stay a one-off. The pre-step control (`build-agent-rWG0`) and the
  bisect were not run: nothing reproduces at HEAD.
- **el6_smoke2 is no control for it** (as WG1 said): `work/EL6/live.sh` ran
  `--drive --latency-ms 150`, no captures, a single server for the four
  sectors as here; it says nothing about a host stall either way.
- **For later live judges:** a starvation / rebase failure whose clients
  share their `frame_time` maximum, or whose capture gaps end at one
  instant (`gapabs.py`), is a host stall; check
  `Microsoft-Windows-Ntfs/Operational` event 170 for D: before attributing
  it to code.

### As built (step WG2, 2026-10-28): G2, the server's world under the bit

**What exists.**
- *The bit.* `session::WORLD_PROXIMITY_UNION = 1u << 2` (`entity/session.hpp`,
  the bit §5 reserved). No protocol change: SessionStart and the journal
  header (since v5) already carry `world_options`; P_G stays G3's.
- *The server config.* `physics.proximity_union` (`config_manager`, default
  **off**): sets the bit in every new session (`server.cpp`, beside the
  placement bit) and says so in the room log. No client runs the rule before
  G3, so a session with it on is the server's own test switch until then; G3
  turns the default on with `anchor_probes`.
- *The world* (PW). `world_options::proximity_union`. The anchor rewires class
  `all` with `union_rules` set (frames at the first reset point,
  `BMMO_NoBall`, exactly one gating probe per identity read, all or nothing);
  a refused rewire fails the anchor ("proximity union (world option)
  refused: ..."; only under the bit - with it clear the anchor ignores the
  rewire's result as step G1's did); the anchor's log line ends "bit set" or
  "bit clear". `probe_candidates()`:
  every player whose clone is physicalized, in player id order, with the
  clone's entity name. `place_probes()`: `proximity_union::place` from them,
  once per tick (`probes_placed_tick_`); `tick()` under the bit places only
  when nobody did for its tick (SimTool's scenes), and keeps
  `place_proximity_legacy()` with the bit clear. `has_body` is the IVP body
  test (`GetPhysicsObject` and `m_RealObject`). `step_state` carries the hold
  section (`proximity_union::capture`) under the bit and
  `restore_step_state` puts it back (§4.2's restore, on the server world's
  rewinds: SimTool's savestate rounds). A hold that does not restore is
  logged and counted (`proximity_hold_failures()`); a savestate round fails
  on a change of the count. (`--replay-session`'s RESYNC restore of a client
  journal captures and restores the replay world's own hold around its
  counters - the server's hold is not on the wire - and a client journal is
  replayed with the bit clear anyway, below.)
- *The runner* (SRN). `place_probes()` first in `step()`, before the leavers
  loop and the events (§2.4). The guard: the candidate set after each step is
  kept and compared at the top of the next one; a change counts in
  `probe_set_drift` (logged, the first 20, and a journal NOTE each). The
  session end logs and journals "proximity union: skipped S, relatched R,
  candidate set drift D".
- *SimTool.* `--replay-session` takes the bit from a **server** journal's
  header and places at the top of every stepped tick, before its PLAYER and
  EVENT records. A client journal carries the server's world options
  (`client_journal.cpp`), but no client runs the rule before G3, so its
  replay keeps the bit clear and prints "journal world option
  PROXIMITY_UNION ignored" (G3 lifts this with the clients' rule).
  `--world-options PROXIMITY_UNION`
  (bit 2) for the drop scene, alone or with `--savestate-check` (whose
  replayed ticks then call `place_probes()`; the hold is restored with the
  step state); the other modes refuse it. New drop-scene switches (their own
  parser, `parse_union_arg`: `parse()`'s else-if chain is at MSVC's nesting
  limit, C1061): `--drop-second-ball TYPE X Y Z` (player 2's ball, a second
  candidate), `--drop-kill TICK` (both balls unphysicalized; with
  `--savestate-check` before the window), `--probe-trace` (the frames and the
  identity copies whose names hold the `--drop` name, on every report line),
  `--probe-scramble before|after` (with the drop scene's savestate check
  under the bit: each round moves every frame 1000 m along x through the
  hold section just before the anchor's step state is restored - the restore
  must put the hold back - or just after it, a stand-in for a lost hold
  restore that the round must catch). A savestate round under the bit also
  compares the hold section at its end tick with the straight pass's
  ("proximity hold: ... differ"). `--savestate-check --verbose` now prints a
  script difference's detail.

**Adaptations (the code is what it is).**
- *The latch and a null output* (WG1's note): `place()` reads the body-kind
  Get Cell output's raw CK_ID. Id 0 (the block has not run) is left alone:
  §2.2's rule speaks of an output that names an entity, and retail's
  consumers read the output only after the Get Cell ran. A non-zero id whose
  object is gone (a leaver's destroyed clone, `remove_player`'s
  `DestroyObject`) names no candidate and is moved to the cell's value at
  that tick, like any other (H2c, G-c7); reading the object instead of the
  id would have taken a dead id for "never ran" and kept the fan on the
  leaver (the step's review). No SimTool scene has a leave, so this branch
  is exercised first by G3's H2c.
- *The guard's set* is the runner's (the design puts the guard on the server
  without naming the owner): physics_world gives the candidates, the runner
  compares, so SimTool's scenes (which apply events between ticks) do not
  count their own drops as drift.
- *J-e is M1's.* With the trafo inputs on (the default since T2) every
  ball-holding leaver already goes through `leaving` (its final resets are
  queued), so its clone leaves after `place_probes()`, as §2.7 wants. With
  the trafo inputs off, a ball holder with nothing queued is still removed
  between two steps (SRN `remove_player`): under the bit the guard counts it,
  and a replay of that server journal places before its PLAYER(-) record
  where the live server had already removed the clone. Not reachable with
  the default config; M1's always-queued leave (§2.7) closes it.
- *V2's bridge scene.* The bridge module's two ObjectBs are 3.4 m apart
  (`P_Modul_29_MF` and `Platte06` on the arena; the module is the same on
  every level) and the 4 m probe's output 0 is In Range, so the Get Cell and
  the Test run at every check while a ball is in range: a falling wood/paper
  ball soon stops being the nearest to `P_Modul_29_MF`, and the bit-clear
  identity then names the stone too (a first scene broke the bridge both
  ways). The scene holds the paper ball at `P_Modul_29_MF` (re-beamed before
  every tick, `--drop-again`) while the stone rests on the plates.
- *V4's no-candidate interval* is a window in which both balls are dead
  (killed in the lead), not a window across the deaths: `--savestate-check`
  sets every event up before its window and no replay re-applies one.

**Results** (x64 MSVC; `.local-tools/work/WG2`).
- Builds: x64 BUILD-OK, x86 CLIENT-BUILD-OK (session.hpp is shared; deps
  recorded in both trees). `BallanceMMOMessageTests` 590/590. GCC syntax
  check 30/30 ok.
- V2 (`v2.sh`, the arena): (a) P_Modul_01, ball A (stone) at XZ 40 m and
  60 m below the Pusher, ball B (paper) at XZ 55 m and 3.5 m above: bit
  clear, the frame follows B (3D nearest) and the Pusher never wakes (fell
  0.000 m); under the bit the frame follows A and the Pusher is awake from
  report tick 40 (fell 0.086 m). (b) P_Modul_29, the paper held at
  `P_Modul_29_MF`, the stone on the plates: bit clear, the identity copy
  names `Ball_Paper` throughout and the bridge holds (fell 2.507 m, the sag);
  under the bit it names `Ball_Stone` and the bridge breaks between report
  ticks 40 and 60 (fell 6.919 m). G-c5 both halves: PASS.
- V3 (`v3.sh`, bit clear): the five server journals of `core_ref_s9`
  (resync_snapoff 4122, swing_snapoff 3888, l8_cross 3166, solo1 2911, solo2
  2895 ticks) replay `matched` = ticks, `checkpoint_mismatches=0` with the
  step's SimTool and with the pre-step one (rSMK); the outputs equal but the
  anchor's wall-clock `cktime`. Wire check rSMK vs rWG2 over `core_ref_s9`:
  12/12 sends SAME, 12/12 journals IDENTICAL, `WIRE-CHECK: ALL SAME`.
- V4 (`v4.sh` and the runs beside it, `--savestate-check --frames`, the
  arena, two balls, under the bit): with both balls alive and the Pusher
  woken inside the window, 23 named rounds anchored after its probe's Off
  (69..145, depths 1-48) are byte-identical (blobs, world hashes, exact
  cores, script records), as with the bit clear; with both balls dead from
  tick 75 (windows 91-120, 91-150, 91-180), 50 rounds each, 0 failed.
  `skipped` 0 in the V2 runs under the bit (`--probe-trace`; the
  savestate rounds do not print it).
- **Review round** (the fix-ups: the latch's raw id, the hold exercised and
  compared, a failed hold restore failing the round, the bit-clear anchor
  not failing on the rewire, client journals replayed with the bit clear;
  `.local-tools/work/WG2/gate.sh`, `v3g.sh`, `g/`): x64 BUILD-OK (no x86
  rebuild: the fix-up touched no file the retail mod includes), tests
  590/590, GCC syntax check ok. V4's hold: the dead 90-tick window (90..179)
  passes 50/50; with `--probe-scramble before` 50/50 (the restore puts the
  frames back); with `--probe-scramble after` (the lost-restore stand-in)
  **50/50 fail** - "proximity hold: ... differ" or P_Modul_18's 7 m probe
  (`Last Check` 27 vs 32, ...) - so the rounds see the hold and the restore
  is what makes them pass. The 23 named alive rounds pass under the bit, with
  `--probe-scramble before` and with the bit clear; the 50-round alive
  schedule fails exactly residual 1's two rounds (144:4, 87:9). V3 again:
  the five `core_ref_s9` server journals matched = ticks,
  `checkpoint_mismatches=0`, stdout equal to rSMK's but `cktime`. Wire check
  rSMK vs rWG2: 12/12 sends SAME, 12/12 journals IDENTICAL, ALL SAME.
- **Three residuals, all pre-existing and identical with the bit clear:**
  - *The G1 membership leak* (queued, roadmap §4 item 4). The 50-round
    schedule of the alive scene fails 2 rounds (anchor 144 depth 4, anchor
    87 depth 9: P_Modul_01's root and its probe active in the replay, local 0
    1 vs 0, `Last Check` 10 vs 6 / 1). The same two rounds fail with the bit
    clear and one ball (the Pusher then wakes too) on this build and on
    rSMK; each round alone passes; a round anchored before the probe's Off
    (34:3, 47:20, 54:8) followed by one anchored after it fails, one
    anchored after the Off (47:45, 68:48, 75:12) does not. Cause: the
    earlier round's restore wrote P_Modul_01 back active and `reregister`
    (`game/script_replay.hpp`) set its scene flag, `CKScene::Activate`d it
    and so put its owner into `m_BeObjectNextFrame`; no live frame flushes
    that between rounds and no restore takes it back when a later record has
    the script finished, so `owner_pending_add` admits the owner and the
    replayed frame runs the finished script from its start.
    *(Corrected at step RLM, rollback-engine-gaps design "As built (step
    RLM)": the two rounds fail identically at depth 0, with no replayed
    frame, so the cause is not the pending list but SimTool's v14 recipe:
    a script inactive at the anchor keeps its live state, here the state
    the previous chained round left. The leak itself is real and fixed at
    RLM; this artefact is queued in roadmap §4 item 4.)*
  - *A 120-tick dead window's restore is refused* ("the PreSimulate queue
    differs from the savestate's": the live queue at the end of the straight
    pass holds `Physics WakeUp` of Gameplay_Ingame and four
    `PhysicsCollDetection` of Sound_Manager, the anchor's none). Traced at
    the review (`.local-tools/work/WG2/r2.sh`): it is neither the union's
    nor the deaths'. On the **pre-step** SimTool (rSMK), one ball, no second
    ball, no `--drop-kill`, no bit, the window 90..203 passes (50 rounds)
    and 90..209 fails the same way; the world logs "retail ball parked (body
    removed) at tick 204". The level start's Gameplay_Ingame physicalizes
    the retail ball at 204 (its `Physics WakeUp`, Sound_Manager's collision
    detection registrations) and the server world parks it at once
    (`physics_world` `park_retail_ball`: Unphysicalize), so the entries find
    no body and stay queued (`Process()` keeps an entry whose `Execute`
    returns 0, rollback-engine-gaps §1.1) - a window that crosses 204 holds
    entries its anchor's record lacks. (That the entries target the parked
    ball is read from the timing; the queue was not dumped per entry.) They
    are outside entries (no selected script's), which gap Q's hold sets
    aside (`hold_outside_callbacks`, rollback-engine-gaps "As built (step
    B7EG)"), but SimTool's `--savestate-check` binds no `callback_queue`, so
    its restore compares the whole queue (mode 0) and refuses. A harness gap
    of the server world's savestate check (the server never rewinds live;
    both clients bind the hold): queued in roadmap §4 item 4.
  - *The bridge scene's V4* (`v4.sh`'s `bridge`, 50 rounds) stops at its
    first round, anchor 47 depth 45: "savestate restore, step 12
    (controllers), world damaged: core of 'P_Modul_29_Platte06': no live
    controller 'sys:unknown' (priority 405)". Priority 405 is
    `IVP_CP_CONSTRAINTS`: the anchor's record holds a hinge of Platte06 that
    the bridge's break inside the window (`Test "Ball_Stone"` True ->
    `10 Hinges` input 2, Shutdown) destroyed, and the blob restore matches
    recorded controllers to live ones (engine changes #17/#18) - it never
    re-creates one. The same with the bit clear (`bridge_off`) and on the
    pre-step SimTool with the stone alone (`g/bridge_smk.txt`, no step
    switch): pre-existing, not the union's. Queued in roadmap §4 item 4 (a
    constraint shut down inside the window).

**Not run (deferred, with why).** A live session: with the bit clear (the
default) nothing of the live path changed (V3, the wire check), and under the
bit no client runs the rule before G3, so a live run would measure the clients'
own-ball sensors against the server's union. The Arch server is not rebuilt
(GCC compiles PW / SRN / server.cpp first there; the GCC syntax check covers
the test target and the core only) - the next step with a live run rebuilds
it. V4's alive-to-dead transition inside one window needs a lifecycle event
inside a savestate window (G3's U2/U3 and H4 cover the client side). The
latch's dead-id branch (a leaver's destroyed clone) has no SimTool scene (no
leave in the drop scene): G3's H2c.

### As built (step WG3, 2026-10-28): G3 and G5, the clients under the bit and BodyRevived retired (protocol 16)

**What exists.**
- *Protocol 16* (`entity/session.hpp`): P_G is the next free number at
  landing - 14 and 15 were taken by BALA and SPGH2 after revision 3 named
  "14 at the earliest". `session_ready_msg::anchor_probes` (u64 after
  `anchor_placement`, read from protocol 16 on, always written).
- *The core (part D).* `client_core_frame.cpp`: pre_step **step 7b**
  (`place_probes(frame_tick, {}, false)`) between `hooks.after_rewinds` and
  `apply_held_lifecycle`, only while `probes_active_` (the world option and a
  platform with `place_probes`, `proximity_union_active()`, fixed at
  `begin`). `probe_candidates(held_out)`: the own player's entities of
  `own_ball_entities_` with a body (`platform_.body_state`), every other
  player's mirror entities with a body from the new `mirror_owners_` (entity
  -> player, ball type; filled where `mirror_entities_` is, in
  `physicalize_remote` and the gap L revive), whatever their navigation;
  sorted by player id. `world_hooks_.place_probes` calls the same with the
  replay's predicate. SessionReady carries `report.probes` when the union is
  active, else 0; the anchor logs `anchor probe signature ...`.
- *The engine (RB, HW, CP).* `rollback_world::place_probes(tick, held_out)`,
  called first in every replayed tick of the one replay loop (input,
  lifecycle, snapshot and resync rewinds), before the `born_now` insertions,
  the re-creations and `pre_step`. `held_out(e)`: `e` has a plan item whose
  `repose_tick` is this tick or later, unless it is a revived life still
  alive at this tick (`revived_until` not passed: a ball dying at t counts at
  t). `bind_session_world` binds it from `core_world_hooks::place_probes`.
  `session_platform::place_probes` (CK-free: `proximity_rules.hpp`'s
  `probe_candidate`).
- *The record (SR, RBS).* Record **v6** exactly as §4.2's revision 3: `u32
  sections` (bit 0 trafo, bit 1 queue, bit 2 hold), the v2 body, then the
  sections; `read_sections` serves restore, diff and `index_record`;
  unknown bits refused. `hold_hooks` (`script_replay.hpp`) and
  `script_binding_options::hold`: a host binds them only under the bit, so
  every other record stays v2-v5 byte for byte. The restore puts the hold
  back after the trailing-bytes check; diff reports a hold difference as
  logical (`proximity hold: frame k differs`).
- *The headless host (SC).* `rewire_proximity()` right after the selection
  and `anchor_trafo()` (so after the placement's apply), class `mechanism`,
  `proximity_union::level_options(ctx)` (new: union rules, ResetPoints row
  0's translation else CurrentLevel[0][3]'s, the Physicalize_GameBall names
  - the server world's reads); a refused rewire ends the session like a
  refused placement. `platform.place_probes` = `proximity_union::place`;
  `has_body` the IVP body test; the hold hooks in the script binding;
  `release_proximity()` before every `release_placement()` (client exit,
  engine reboot, a new session). Host counters `probes_skipped` /
  `probes_relatched`; the status line gains `probes: active places=live/replayed
  two_bodies skipped relatched revived_local` only under the bit.
- *G5.* The core sends no BodyRevived under the bit (`revived_local_`
  counts the wakes); `server.cpp` drops one right after the duplicate
  check: neither submitted nor relayed, `revived_ignored` per member, the
  first noted in the log and as a journal NOTE (`session_runner::
  journal_note`, new); `physics_world::apply_event` does not apply one
  under the bit (`revived_ignored()`), which is SimTool's half.
- *The server.* `physics.proximity_union` defaults **on**; SessionReady's
  `anchor_probes` is compared with `physics_world::probe_signature()` (the
  world's mechanism-class signature, through `world_ready_info`) after the
  placement compare, late joiners included: a mismatch refuses that member
  ("refused: probe mismatch (client ..., server ...)"; out of the session,
  not out of the room, the rest go on - §2.3's "cannot join"; the gate
  round's change after the review, which first had `end_physics_session`
  as the placement mismatch does).
- *SimTool.* `--replay-session` takes the bit from a client journal too
  (the "ignored" line is gone).
- *`proximity_union::rewire`* releases a previous rewire first (WG1's
  "re-anchor without release").

**Adaptations (the code is what it is).**
- *No capability, and `place_probes` is not in the "missing op" check.* The
  check runs at construction, before any world option is known; a required
  op would stop the retail core host (no G4 yet) from running any session.
  A host without the op runs with the union inactive, logs that it cannot
  place, and sends 0, which the server refuses (§2.5's "the session must not
  start", through §5's signature).
- *The signature is compared under the bit only.* Clients rewire only under
  the bit; with it clear they keep today's own-ball sensors and send 0.
- *Interim state until G4: the retail core host is refused* by a server
  with the default on ("probe mismatch (client 0000000000000000, ...)", the
  SessionEnd reason "... this client's host cannot place its probes"); the
  headless members of a mixed room go on. A retail test before WG4 needs
  `physics.proximity_union: false` on the server.
- *Two bodies of one player* (an invariant violation): the first entity by
  name, not the lower ball type - a rule of names every world applies the
  same way, and the core has no ball type for its own entities (the type
  `mirror_owners_` stores is unused; the server never has two balls of one
  player, so the rule is never exercised).
- *A frame difference is float in `diff`* (gate round, the review's
  finding): §4.2 says logical, but a frame is a copy of its candidate's
  position, so every correction of that ball moved it and every correcting
  rollback counted a logical script mismatch (8 "proximity hold: frame 0
  differs" in the first resync_snapoff run). A count or a held by-name
  entity that differs stays logical ("proximity hold: by-name copy k
  differs"); the label no longer names a copy by a frame index.
- *The hold section's order* (WG1's note): the union's (owner, script,
  walk) order, the same in every world; a client's section lists the
  mechanism blocks only, the server's every block (the section never
  crosses worlds).
- *The portable record carries the hold (version 3, protocol 16, §4.4 / §5;
  the gate round, the review's finding 1).* `capture_portable` writes a
  sections word (bit 0 the trafo scripts, bit 1 the hold) and, after the
  trafo scripts, `u32 size` + the hold by name
  (`proximity_union::capture_portable`: per frame its name and position, per
  by-name copy its name and the held entity's name); only the server's
  world under the bit passes the capture (`physics_world`'s resync state),
  so every other record stays version 1 / 2 byte for byte. `localize` hands
  the section to the host's localizer (`proximity_union::localize_hold`:
  this union's frames and copies in its own order from the entries of their
  names, the held entity found as `place` finds it; a missing name refuses
  the whole record) and writes a local **v6** record with the hold section,
  which the restore puts back at S. The headless host passes the localizer
  while the union is active (`localize_session_scripts`' new `hold`
  argument); the retail core host passes none until G4, and then the
  section is left out (the anchor's own hold stays; logged).
- *The player-ball reference is a rename, not a kind 4/5.* The mirrors
  carry the server clones' names in every world; only the own ball's clone
  has another name here, and the resync state already carries the clone
  list the blob's renames are made from (`resync_renames`). The core hands
  those renames to the localize (`resync_script_report::renames`, an input,
  only while the union is active) and an object reference whose recorded
  name is a clone's resolves to this world's ball of that player
  (`objects_renamed`). So the portable format gains no parameter kind, and
  the bit-clear localize is unchanged.

**Results** (x64 MSVC, x86 MSVC, Arch GCC; `.local-tools/work/WG3`).
- Builds: x64 BUILD-OK (header deps recorded: `client_core_messages.cpp.obj`
  #deps 39), x86 CLIENT-BUILD-OK (the shared core; no retail file changed),
  Arch server ARCH-BUILD-OK `ballanced-36c04e3d24c9+bmmo-13f7d8664a68-dirty`.
  `BallanceMMOMessageTests` 595/595 (5 new: `ProbeWire`, three
  `SessionClientCoreProbes` - U2's candidate rule with a held-out newborn, an
  undriven mirror, a body-less mirror, two bodies, the own death; the
  SessionReady signature in three arms; step 7b's order and no BodyRevived -
  and `RollbackEngine.TheProbePlacePointComesFirst...` - U3's call order
  `place@4 held, recreate Prop@4, pre_step@4`, `place@5 free`).
  GCC syntax check 30/30 ok.
- Wire check rRLC vs rWG3 over `core_ref_s9` (`NEW_EXTRA="--accept-protocol
  15"`: the set is protocol 15, recorded with the bit clear): 12/12 journals
  IDENTICAL; sends DIFFER in exactly one line per client, send #5/#6, 67 ->
  75 bytes = SessionReady (protocol 16 and `anchor_probes` 0). With the bit
  clear the headless client is unchanged but for that message's layout. The
  set is not re-captured (queued).
- Live 1, `l8_cross` (level 8, 4 clients, 0 ms, placement on, trafo inputs
  off, snapshots off, the bit on by default; `maps/arena-test/wg3/l8_cross`):
  every client's signature `0813ea4dd1855f26` = the server's mechanism
  signature (35 blocks with the placement copies, 9 identity reads; the
  server's `all` 63); server journal 3115/3115 matched, 0 BodyRevived
  records; every one of the 33 mechanism bodies (the P_Modul_29 plates, the
  Pusher, P_Modul_25/37 bridges, ...) **bit-equal to the server's FULL rows
  at every common tick on all four clients** (`mech_rows.py`). On
  `core_ref_s9` (bit clear) the same tool gives SFD's numbers: the plates
  from 554 to 3.594 m on c0/c2/c3, Pusher 0.095 m, P_Modul_25 0.022 m,
  P_Modul_37 0.020 m. Status: `skipped=0 relatched=0 two_bodies=0`,
  `revived_local` 44-47 per client (the reports no longer sent).
- Live 2, `resync_snapoff` (arena, 150 ms, client 2 paused 1000 ms at 1500 ->
  one resync): verdict as `core_ref_s9`'s (FAIL on the pause's resync,
  starvation 44, 129 frames past the reach); server 4127/4127 matched, 0
  BodyRevived (169 on `core_ref_s9`); 28 mechanism bodies bit-equal
  throughout on c0, c1, c3 (on `core_ref_s9`: P_Modul_34 2.74 m, P_Modul_03
  1.37 m, P_Modul_30 0.34 m, Pusher 0.090 m, P_Modul_37 0.020 m - G-c's
  signature, gone). **T1 (a) is gone**: SimTool replays of the four client
  journals (`--write-journal`) against the server journal
  (`replay_tick_compare.py`): c1 3484/3484 and c3 3539/3539 ticks identical
  (hash and pose), c0 identical through 3478 and c2 through 3496 (a
  4- and 41-tick tail past the last relayed frames), FULL checkpoints 0
  differing on c1/c3; no BodyRevived exists to be applied at its stamp.
- Not a divergence (root-caused in the gate round): c2's (the paused
  client's) LOCAL rows of `P_Modul_26_Sack` / `_Rope` differ from the
  server's by up to 4.81 / 2.05 m at 1588-1614 and are bit-equal from 1616
  on (`core_ref_s9`'s c2: 4.65 / 1.98 m, the same shape). The exact resync
  rebases the tick schedule first ("resynced: tick base 1588 (old tick
  1521, delta -66) ... the server's state of it follows") and keeps
  stepping its own world until the state arrives (1615, "[arrival]", then
  the restore at 1587 and the 28-tick re-step). The rows of 1588-1614 are
  that old world under the new numbers: each equals the server's row of
  the tick 66 earlier bit for bit (`sack2.py`, every row of both bodies,
  worst 0.0). A comparison of a resynced client's rows has to skip the wait
  between "resynced" and "[arrival]"; nothing to fix.
- *WG2-era journals.* SimTool's `--replay-session` and
  `physics_world::apply_event` now apply G3/G5 to every journal whose
  header has the bit; a protocol-15 journal recorded with the bit set (only
  WG2's own test runs: the server's default was off until this step) had
  its BodyRevived records applied live and replays differently now. Journal
  v7 carries no protocol number to tell them apart; such a journal is
  replayed with a pre-WG3 SimTool (e.g. `build-agent-rRLC`).

- Gate round (after the review: the portable hold, the clone renames, the
  refusal, the float frame diff): x64 BUILD-OK, x86 CLIENT-BUILD-OK, Arch
  ARCH-BUILD-OK, `BallanceMMOMessageTests` 595/595, GCC 30/30; the wire
  check over `core_ref_s9` exactly as before (12/12 journals IDENTICAL, the
  one SessionReady line per client 67 -> 75 bytes): the bit-clear path
  does not reach the new code. `resync_snapoff` again
  (`maps/arena-test/wg3/resync_snapoff_g`, a third live run, over the
  step's two): server 3847/3847 matched, 0 BodyRevived; c2 logs "the
  server's proximity hold at 1588 localized into the record" and the
  re-step's 41159 script bytes localize with 0 unportable; every mechanism
  body bit-equal on c0, c1, c3 throughout and on c2 from the arrival (1616)
  on; c2's rows of the wait 1589-1615 equal the server's rows 66 ticks
  earlier bit for bit (`wait_rows.py`: 286 rows, 22 bodies, worst 0; the
  first run: 308 rows, worst 0). The diff now reports "proximity hold:
  frame k differs [float]" (0 logical).
**Deferred to FULL** (the 15-minute gate ran builds, tests, GCC, the wire
check and two live runs): U2's full scripted life sequence (only the
subsets above), U3's script-record round trip with a changed
`CurrentLevel[0][1]` (needs CK: a SimTool scene), V4 on the clients'
records, H1-H5, H2b, H2c, H4, H7 and H8 in full, H6 (3 minutes with the
same-batch control), the re-capture of the reference set at protocol 16,
CPU parity, and the retail runs (G4 first).

### As built (step WG4, 2026-10-28): G4, the retail core host (RL's part dropped, answer 7)

**What exists** (`BallanceMMOClient/session/core_host.cpp`, CH; no protocol,
record or journal change - G3 made all of them).
- *The host state.* A file-scope `proximity_host` (the BML constraint: no
  member) holds the `bmmo::game::proximity_union`, `active`, the last
  anchor's signature and counters (anchors, refused, releases, restored);
  `proximity_rewire` / `proximity_release` are the headless
  `rewire_proximity` / `release_proximity` with the bridge's body test
  (`physics_view::has_body` by the entity's name) and the logger.
- *The anchor* (§2.4's RC row): right after the selection, the trafo
  part and the force hooks - so after the placement's apply, as the
  headless anchor - the union is released (a leftover) and, when the core
  says `proximity_union_active()` (the world option and the platform op),
  rewired with class `mechanism` over the session's selection and
  `proximity_union::level_options`. A refused rewire ends the session
  there ("the proximity union (world option) refused: ..."), as a refused
  placement does. `anchor_report::probes` carries the signature (0 with
  the bit clear).
- *Per tick and in every replay* (§2.5): `session_platform::place_probes`
  = `proximity_union::place`; the core runs step 7b and
  `rollback_world::place_probes` as it does for the headless host.
- *The records* (§4.2, §4.4): the script binding's `hold` capture /
  restore while the union is active (record v6, as headless), and the exact
  resync's localize gets `proximity_union::localize_hold`, so the server's
  hold at S goes into the record instead of being left out (WG3's interim
  "the retail core host passes none until G4" is closed).
- *Counters.* `host_counters::probes_skipped / probes_relatched` in both
  places this host fills them (the core's status line and the `session`
  verb's), so the `probes:` field reads as on headless.
- *Release* (§2.3 "Release", §2.4): in `core_host_end_local` after the
  core's end ("proximity union released (the session ends): N inputs
  back"), and at the top of `core_host_release_placement` (the level's
  exit and the map load) before the placement's copies, as the headless
  order has it.
- *Tooling.* `session probes [status]` (automation verb; `ok probes
  active= blocks= identities= signature= anchors= refused= releases=
  restored= skipped= relatched=`), routed in `client_automation.cpp`.

**Adaptations.**
- *A shared header fix the retail build needed.*
  `proximity_union::level_options` read the `Physicalize_GameBall` names
  with the four-argument `GetElementStringValue`, which only Ballanced's
  CK2 has (retail's takes three). It now reads the STRING cell's pointer
  through `GetElement(row, 0)`, `sector_placement.hpp`'s read for both
  SDKs, only when column 0 is `CKARRAYTYPE_STRING` (any other type leaves
  the name empty, so the probe signature refuses - closed, where reading a
  value as a pointer would crash), cut at 127 characters like the server's
  128-byte read (`physics_world.cpp`). Headless reads the same names as
  before: Ballanced's `GetStringValue` returns the cell's pointer for a
  STRING column (`CKDataArray.cpp` 788-791) and `CopyStringToBuffer` cuts
  at size - 1 = 127; `GetElement` there is `CKUINTPTR*`, so nothing is
  truncated on x64. This is a code reading: the wire check below cannot
  show it, because the `core_ref_s9` captures predate the union (their
  journals have no "proximity union" line, `level_options` never runs in
  them); a headless session with the bit on is deferred to FULL.
- *The body test looks the entity up by name.* The retail
  `proximity_rewire`'s `has_body` asks the bridge
  `physics_view::has_body(entity->GetName())` (the first entity of that
  name, then its body state); headless tests the entity it is given
  (`GetPhysicsObject(entity)`). The two differ only when two 3D entities
  share a candidate's name and the first found has no body (not seen:
  `skipped=0` on both clients). A by-`CK_ID` body test in the bridge would
  close it; its per-candidate cost (a lookup and a body-state copy each
  tick and replayed tick) is part of FULL's CPU parity.
- *The map load drops, it does not release.* A union still recorded at a
  map load (the session's end and the level's exit both missed - not seen)
  names objects of the previous level by `CK_ID`, which the load may have
  reused; its record is dropped without touching the scene (logged), and
  the placement's release follows as before.
- *`active` is the host's* (the rewire made), not
  `core.proximity_union_active()` re-read: the two agree for a session
  that ran (a refused rewire ends it), and the host's survives the core's
  end for the release order.

**Results** (x86 MSVC, x64 MSVC, Arch GCC; `.local-tools/work/WG4`).
- Builds: x86 CLIENT-BUILD-OK, x64 BUILD-OK (header deps recorded:
  `session_client.cpp.obj` #deps 348). `BallanceMMOMessageTests` 595/595.
  GCC syntax check 30/30 ok.
- Wire check rWG3 vs rWG4 over `core_ref_s9` (`EXTRA="--accept-protocol
  15"`): WIRE-CHECK: ALL SAME (12/12 sends SAME, 12/12 journals
  IDENTICAL): the headless client is unchanged on captures without the
  union (the header's names: the code reading above). Rerun after the
  review's fix-up (column type check, 127-character cut): see the gate
  round below.
- Retail, L11, two copies, the Arch server of WG3 (union on, placement on,
  trafo inputs off, snapshots off), client 2 with forced rollbacks every 25
  ticks 10 deep, client 1 into sector 2, then the room closed
  (`.local-tools/work/WG4/wg4_gate.py`; `scripts/retail/wg4_gate1`,
  `wg4_gate2`):
  - both anchors: "63 mechanism blocks, 11 identity reads rewired,
    signature 6fb28962d52e7d48" = the server's mechanism signature (91
    blocks, mechanism 63; 11 identity reads); assigned, no "probe
    mismatch".
  - `probes: active=1 places=694/6` (c1) and `706/226` (c2: 25 forced
    rollbacks, 226 replayed ticks), `two_bodies=0 skipped=0 relatched=0`;
    the server "skipped 0, relatched 0, candidate set drift 0"; 0
    BodyRevived; no restore failure.
  - Every mechanism body of the server's FULL rows that `mech_rows.py`
    compares (17: the P_Modul_01 Pushers, P_Modul_19 Flaps, P_Modul_25
    and P_Modul_37 bridges with their placement copies `_R21` / `_R23` /
    `_R30`, the P_Modul_26 sacks and ropes, P_Modul_30, P_Modul_34)
    bit-equal on both clients at every common tick (run 2: 352 ticks).
    Run 1 showed `P_Modul_26_Sack`/`_Rope` 0.26 m on c1 at its last tick
    only: c1 holds two LOCAL checkpoints of 732, the regular one (bit-equal
    to the server's, and the tick hashes are equal) and the one `journal
    mark` writes at the command's time, after tick 733 stepped (0.262 m =
    one tick of the swinging sack); the tool took the last. A tooling
    artifact of the mark, not a divergence.
  - After the room's close: "proximity union released (the session ends):
    74 inputs back" on both (63 + 11), `session probes` `active=0
    blocks=0 releases=1 restored=74` (R4's half: the sources back).
  - c2 once logged "proximity hold: frame 0 differs [float]" in a replay -
    a frame is a copy of its ball's position, so a corrected remote moves
    it (WG3's float classification); 0 logical.

**Deferred to FULL** (the 15-minute gate ran the builds, tests, GCC, the
wire check and two retail smoke runs): R1 (the arena with two retail and
two headless clients, a headless ball waking P_Modul_01 first - needs the
slot-13 hack and a scripted drive), R2's sensor crossings (P_Modul_01 /
P_Modul_37 alone, the P_Modul_18 fan with two balls - needs driven balls on
L11), R4's vanilla half (the Pusher waking for the own ball after the
session without a reload), a retail exact resync under the bit (the
hold's localize on retail), CPU parity, and a headless session with the
bit on after the `level_options` change (the wire check's captures do not
reach it).

**Gate round** (the review's fix-up: the column type check and the
127-character cut in `level_options`, the note). x64 BUILD-OK (the
header's includers rebuilt, `session_client.cpp.obj` #deps 348),
`BallanceMMOMessageTests` 595/595, GCC 30/30 ok, the wire check rWG3 vs
the fixed rWG4 over `core_ref_s9` WIRE-CHECK: ALL SAME (24/24), x86
CLIENT-BUILD-OK (`core_host.cpp` rebuilt). No live rerun: the fix-up
changes the names only for a non-STRING column or a name over 127
characters, neither on L11 (the runs above: 11 identity reads, the
server's signature). The server does not call `level_options` (its own
reader, `physics_world.cpp`), so the Arch server is not rebuilt.

### As built (step WG6, 2026-10-28): G6, the documentation pass

**What changed** (comments and documents only; no code, protocol, record or
journal change).
- *SR:29-37* (`game/script_replay.hpp`, the header's "The sensors'
  reference" paragraph): it no longer says that every mechanism sensor
  measures the own ball's child frame. It says that the retail graph does,
  and so does a session with `WORLD_PROXIMITY_UNION` clear (G-c). Under the
  bit (protocol 16) every world rewires the mechanism blocks and reads to
  `BMMO_Prox_<owner>_<n>` / `CurrentLevel#BMMOId_<owner>_<n>`
  (`game/proximity_union.hpp`) and places them before the lifecycle
  changes of every live and replayed tick. What a tick without a candidate
  keeps travels in the record's hold section (v6 bit 2, `hold_hooks`).
- *`game/checkpoint_phase.hpp:35`* (WG1's "Left for later steps"): the
  precedent it cites is now named as the union's identity copies (once
  `physics_world::rewire_ball_identity_reads`).
- *RSD §2.4* (`docs/resync-and-sector-timing-design.md`): the `unportable`
  bullet gains the amendment of §2.9's "RSD A1" row. Under the bit the
  frames and the identity copies are made by every world with the same
  names, so they are portable since this design. The hold travels by name
  (portable version 3, sections bit 1), and a clone reference resolves
  through the resync's renames.
- *`docs/rooms-and-sessions-protocol.md`*: protocol 16 (the bit, the
  rewire and the place point, record v6 and portable version 3,
  `anchor_probes` and its compare and refusal, BodyRevived retired), the
  `world_options` row's bit 2, the BodyRevived row, the
  `physics.proximity_union` config line, and the current protocol number
  (13 -> 16).
- *`docs/collision-overhaul-design.md` 9.31* (Chinese, `glossary-zh.md`'s
  terms): G-c's problem, G0-G6, the numbers of WG0-WG4 and the residuals.

**Adaptations.**
- *`BMMO_NoBall` is named in the amendment as portable by name but
  carried by no section yet* (the review's nit on the "RSD A1" row). Under
  the bit `make_no_ball` creates it in every world, but only the body-kind
  cells name it; the hold section carries the by-name copies, which keep
  their entity. `localize_hold` looks a held name up as a `CKCID_3DOBJECT`
  and `BMMO_NoBall` is a plain 3D entity, so Q7, whose slot rules make a
  by-name cell read it (§2.10), must widen that lookup when it puts it in
  the hold.
- *§2.9's kind 4 is described as not built.* The row wanted RSD §2.4 to
  gain "a portable kind 4, (player id, ball type)". WG3 resolved a clone
  reference through the resync renames instead ("As built (step WG3)", "The
  player-ball reference is a rename"). The amendment says that and names
  the portable section that was built.
- *RSD §1.3 G-c and §9 Q6 get a closing note too.* §2.9's "RSD I1 / G-c
  note" row wants I1 to hold after S as well. The row names only RSD §2.4,
  but the gap paragraph and the open question would otherwise still read
  as open. Each gets one short note that points to this design and leaves
  the original text as it was.
- *The protocol doc covers P_G only.* SessionMember, lives, `life`, the
  bits of Q7, `mechanism_slots` and the join-order cap (§5's last row)
  belong to M1 and Q7, whose rows name the protocol doc, so they are not
  written here. The doc had not described protocols 14 (BALA) and 15
  (SPGH2) either. It gains one paragraph that points to
  `entity/session.hpp`'s comment for them, not their layouts. Its two
  opcode lists (the file header and §2) still end at `SessionLeadAck`,
  without `SessionResyncState` (A3) or `SessionGhostPose` (BALA). They
  are left as they are: they are not this design's, and the authoritative
  list is `message/message.hpp`.
- *§1.1's "Clients" and "BodyRevived" paragraphs stay as written.* They
  are the problem statement of 2026-10-23. The as-built notes are the
  record of what the code does now.
- *The roadmap* (§4 item 5) gains one line saying that G3-G6 are built,
  beside WG1/WG2's line, with a pointer to these notes for the checks
  deferred to FULL. It is not in the row.

**Checks** (`.local-tools/work/WG6`; the two headers changed in comments
only).
- x64 BUILD-OK (50 steps; `session_client.cpp.obj` #deps 348, both
  headers among them), x86 CLIENT-BUILD-OK (24 steps),
  `BallanceMMOMessageTests` 595/595, GCC syntax check 30/30 ok.
- Wire check rWG4 vs rWG6 over `core_ref_s9` (`EXTRA="--accept-protocol
  15"`): 12/12 sends SAME, 12/12 journals IDENTICAL, `WIRE-CHECK: ALL
  SAME`.
- No live run: nothing that runs changed.

### As built (step WM0, 2026-10-28): M0, the tools

**What exists.**
- *`scripts/body_series.py`* (new; the design's scratch script of
  2026-10-23 was gone, `.local-tools/work/a4/body_series.py` was its
  band-change variant): one body's error, the server journal's FULL
  checkpoints against each client journal's LOCAL ones
  (`first_divergence.worlds`, so the keys are the same), the Euclidean
  distance per tick. `--body NAME` (a name prefix; every match is its own
  series) or `ball:OWNER` (the own ball in the owner's journal, the mirror
  in the others: J1/J5's "keyed by owner"); `--ticks`, `--from/--to`,
  `--changes` (the rows where a band changes: exact, < 1 mm, < 5 cm,
  beyond), `--summary` (max and its tick, the first and last error tick,
  the error ticks counted, the first exact tick after). A cell reads `0`
  for bit-equal, `-` for no checkpoint, `absent` for a checkpoint without
  the body. `--selftest` (9 checks).
- *The harness* (`scripts/arena_map_test.py`): `--leave N:T` and
  `--rejoin N:T`, parsed by `parse_client_ticks` (client in range, tick >
  0, one spec per client and option; `--leave` and `--rejoin` of client
  0, the host, refused: its leave would hand the room over; a `--leave` at
  or before the same client's `--rejoin` refused)
  and given to client N by `member_args` as the client switches below.
  Two selftest checks.
- *The headless client* (SC), two switches. **Adapted:** the row's files
  are "scripts, SV", but a leave at a given tick and a rejoin on the same
  connection are things only the client can do, so the harness options
  drive these:
  - `--rejoin-at TICK`: after `post_step` of TICK the core's session is
    ended locally (`core_.end`), and room Leave then room Join are sent in
    the same tick on the one reliable lane; the server takes the leave
    (`room_remove_and_notify` -> `physics_session_member_left`) and
    late-joins the same id (`physics_session_member_joined`), and the
    late SessionStart begins the next life through the host-restart path
    of `begin_session` (the engine rebooted under the running level). The
    second life's journal is `<stem>.rejoin1<ext>` (the journal path op),
    beside the first one.
  - `--leave-room-at TICK`: room Leave at TICK, the session ended
    locally, and the run loop exits once the server's room state no
    longer has the client in a room (2 s at most). **Adapted:** the
    existing `--leave-at` (step T3) ends the session and exits without
    closing the connection, and the server removes the player only at
    the connection timeout: in the first smoke run client 1 stopped at
    its tick 1800 and the server held its input (S2) until it dropped
    the connection at 2322, 522 ticks later. J2/J4 need the leave at the
    asked tick, so `--leave` maps to the new switch (the second run: the
    server relayed the leaver's Unphysicalize "for tick 1800 (next tick
    1800)"). `--leave-at` is unchanged.
  - **Found and fixed (the smoke run).** `begin_session` kept the
    previous session's `own_replica_` (the own ball's navigation replica
    record). After the engine reboot the record still said "active on
    Ball_Stone", so `own_body_upkeep` took the new world's Ball_Stone for
    already driven and never attached a replica: the rejoiner's log
    printed `own navigation rows: no navigation for 'Ball_Stone'` every
    tick, where the first life printed `own ball Ball_Stone driven by the
    navigation replica`, and `body_series.py ... --body ball:2489030504`
    gave the rejoiner's own ball 0.0042 m at 1436 (as every mirror: the
    spawn) and then 0.029, 0.076 ... 80.6 m at 1700 off the server's
    clone while client 0's and client 3's mirrors of it were exact from
    1438 on. The record (and `revive_navigations_`) is now reset there
    with the rest of the per-session state. Pre-existing: any second
    session in one headless process (the host-restart path) had it; no
    reference capture has one, so the wire check cannot reach it.
- *The server* (SV): `--hold-member-ms MS`, a test flag compiled only
  with the new CMake option `BMMO_SERVER_TEST_FLAGS` (off by default, so
  a release build has no such flag; on in `build-agent`'s cache and in
  the Arch test tree, whose `.local-tools/arch_build_server_dirty.sh`
  now passes `-DBMMO_SERVER_TEST_FLAGS=ON`). `parse_args` reads it and
  `server::set_test_hold_member_ms` stores it (the start-up log says so
  when it is non-zero). **Adapted:** there is no SessionMember message
  before M1, so M0 carries the value only; M1's send points (the
  reliable copy and the `event_repeats` copies, §2.7) read
  `test_hold_member_ms()` and hold the message that long. The guards are
  `#if defined(BMMO_SERVER_TEST_FLAGS)` and the OFF build marks the unused
  parameter `(void)` (the review: `#if` on an undefined macro warns C4668
  under `/Wall`, the parameter `-Wunused-parameter` under `-Wextra`).

**The lp1_join numbers reproduced** (the row's "closes on"):
`body_series.py maps/arena-test/lp1_join/server.bmjr
maps/arena-test/lp1_join/sector1/client{0,1,2}.bmjr --body
P_Modul_01_Pusher --ticks 620,624,632,636,660,688` prints §1.1.3's table
digit for digit (620: all 0; 624: 0.0897 / 0.0897 / 0; 632: 0.0435 /
0.0892 / 0; 636: 0.0019 / 0.0888 / 0; 660: 0.0007 / 0.0012 / 0; 688: 0 /
0.0001 / 0). With `--from 600 --to 720 --summary`: client 0 in error
from 622 to 684 (max 0.0898 m at 626, the 0.04-0.09 m band 622-632),
client 1 from 622 to 690 (the band 622-638), client 2 exact at all 61
checkpoints: H1's negative control "0.0897 m for 10-16 ticks". J1's
other numbers are log lines the tool does not need: the joiner's kick
index 60 on the server and the joiner, 56 on clients 0-2
(client0.log:859).

**What today's code does at a rejoin** (the second smoke run, for M1):
the server relays the rejoiner's Unphysicalize for 1201, logs "left with
events queued: it stays in the world until tick 1203", then at once
"player ... removed" and "player ... added" (J-f: `add_player` runs
`finish_leave` immediately), and WG2's proximity union warns "the
candidate set changed between two steps (... at 1201)". The new life got
join order 2 again, base 1230, its spawn kick index 45 on the server and
on the rejoiner.

**Checks** (`.local-tools/work/WM0`; QUICK tier, about 11 minutes of
checks, two live runs).
- x64 BUILD-OK (59 steps after the CMake option's reconfigure, then 3 and
  3 for the two client-only fix-ups; `session_client.cpp.obj` #deps 348,
  VALID), ARCH-BUILD-OK (`ballanced-36c04e3d24c9+bmmo-9f9ec7eacd90-dirty`,
  test flags on), `BallanceMMOMessageTests` 595/595, GCC syntax check
  30/30 ok, `arena_map_test.py --selftest` PASS, `body_series.py
  --selftest` PASS. No x86 build: no shared or retail file changed.
- Wire check rWG6 vs each of the three headless builds (rWM0, rWM0b,
  rWM0c = the final one) over `core_ref_s9` (`EXTRA="--accept-protocol
  15"`): 12/12 sends SAME, 12/12 journals IDENTICAL, `WIRE-CHECK: ALL
  SAME` each time. (Without the `EXTRA` every replay is refused at
  SessionStart, protocol 15 against 16, on the old binary as on the new.)
- Live smoke 1 (`maps/arena-test/wm0_live`, rWM0, Arch server snapshots
  off, trafo inputs and placement on, 4 clients 45 s, `--rejoin 2:1200
  --leave 1:1800`): all clients rc 0, the server journal replays 3679/3679,
  `checkpoint_mismatches=0`, VERDICT PASS; it found the connection-timeout
  leave and the stale replica above.
- Live smoke 2 (`maps/arena-test/wm0_live2`, rWM0b, the same recipe):
  rc 0 everywhere, 3616/3616 matched, PASS; the prompt leave at 1800 and
  the rejoin at 1200/1201 as described.
- **Deferred:** a live run of the replica fix (rWM0c): it would be a
  third live run. The evidence for it is the log line above and the
  first life's attach; the wire check shows it changes nothing for one
  session. J5 (M1's row) is its first real use: J5 first checks the
  rejoiner's own ball against the server's clone (`body_series.py
  --body ball:OWNER` over `client{N}.rejoin1.bmjr`), before anything of
  M1's own is judged.
- **Left for M1** (the review's nits): the harness's verdict reads only
  `client{i}.bmjr`, so a rejoiner's second life is not judged and its
  second SessionAssign base would count among the resync bases; M1 reads
  `client{N}.rejoin*.bmjr` and labels the rejoin base. A refused rejoin
  Join (`RequestDenied`) lets the client's room handling join some other
  room and run on; M1 ends the session with a log line instead. A switch
  whose tick is already behind a late joiner's base fires at once without
  a warning.

### As built (step WM1, 2026-10-28): M1, member lives and J2 (protocol 18)

**Protocol.** P_M is **18** (`PHYSICS_PROTOCOL_VERSION`, 17 was step
BALL's; `PHYSICS_PROTOCOL_MEMBER_LIVES` names it). `SessionMember`
(`message/session_member_msg.hpp`) is appended after `SessionResyncState`:
it takes the value `SessionGhostPose` had (retired at 17), and no other
opcode moved. `player_entry.life` (u16) is written into SessionStart from
protocol 18 on (the reader reads it only from a sender that says 18, so an
older server's SessionStart is still refused by its number, not as a
truncated message). `session_event_msg.life` (u16) ends the trailer after
`state`, unconditionally. The reader refuses kind outside {1, 2}, join order
>= 64, id 0 and life 0 (U4). The journal layout is unchanged (v7).

**What exists.**
- *CK-free rules* (`session/member_lives.hpp`, new): `member_life`
  (entry, `from` = J, `until` = R, and the client journal's two flags),
  `find_member_life`, `member_life_covers`, `member_life_at` (the life a
  tick names), and the server's `lowest_free_join_order` (J2),
  `late_join_base` (J4) and `leaver_stays` (J-e).
- *The runner* (SRN/SRH). `add_player(..., life, name)`; `session_state::lives`
  (each id's current life); an add of an id still in `leaving` is stored
  in the leaving entry (`rejoin`) and run by `finish_leave` right after the
  old life's removal, at the same tick (J-f: no event erased, J = R).
  `add_now` fires `on_player_added` or `on_player_add_failed`;
  `finish_leave` fires `on_player_removed` (the removed life's order, read
  before the erase). Every leave with a ball queues its Unphysicalize and
  stays in the world to `next + 1` (J-e; `has_ball` alone, was `has_ball
  && queued`). `accepted_event.life` on every path (submitted events, the
  leaver's Unphysicalize, `queue_trafo`'s server resets, a dropped event).
  `leaving_horizon(session)`: the view value J4 reads, published by
  `publish_state` (also after a remove). The session end logs and journals
  `member lives: join orders taken in the world N`.
- *The world* (PW): the "falling back to slot" path is logged as `ERROR join
  order N taken in the world` and counted (`join_orders_taken()`); it keeps
  working (an old journal's replay can still meet it).
- *The server* (SV). `lives` (per id, never erased), `retiring_orders`,
  `started` (filled by `send_session_start`), the SessionStart entries'
  `life`; `begin_life` at the start and at every join; a late joiner's
  SessionStart goes from `on_player_added` (not from
  `physics_session_member_joined`); `send_session_member` to every started
  member except the player, reliable plus `event_repeats`, held by
  `--hold-member-ms` (test builds: the held copies leave from the main
  loop, `send_assign_repeats`, then repeat like any event);
  `on_player_add_failed` ends that member (SessionEnd with the error, then
  `physics_session_member_left`); the late base is clamped by
  `late_join_base(..., leaving_horizon)` and logged when it moves;
  `on_event_accepted` relays `msg.life = a.life` before the one serialize
  every copy (`last_physicalize`, `sector_history`, `trafo_relayed`, the
  repeats) is taken from.
- *The core* (CC/CL/CM). `players_` is now `lives_`: SessionStart's lives
  (from 0) and every `SessionMember(added)`; nothing erased during a
  session. `on_session_member` as §2.8 (copies ignored, a removal before its
  addition kept in `early_removals_`, own id ignored, a conflicting join
  order ends the session locally), accepted from `begin()` on (loading or
  running). Parking in `on_session_event` before the queue; the parked
  events of a life go to `pending_events_` when it arrives (the drain then
  takes the late path for a passed tick). `join_rank(player, tick)` and
  `mirror_join_order(player, tick)` (the group and kick of a mirror and of
  a remade one). The session end logs and journals `member lives: N lives,
  M member messages (C copies), events parked P (applied A, dropped D),
  outside their life O, mirrors outside the lives X, own echoes of another
  life E, catch-up stamps of untold lives K, PLAYER records not written W`.
- *The client journal* (CJ). PLAYER records come from the lives
  (`journal_member_lives`, `add_player(entry, J)`, `leave_player(player,
  R)`); `announce_player` writes a note once for an id no life names, never
  a PLAYER record (no `join=255`).
- *The hosts.* SC decodes `SessionMember` into `on_session_member`; CH
  names it, decodes it, latches it into the mailbox (the core path alone)
  and dispatches it, and ends the host's session locally when the core
  ended on a conflict. The room `PlayerLeft` no longer writes a PLAYER(-)
  on either host (the member message does, at R). MC is not changed.

**Adapted** (the code is what it is; each is within the principles).
1. *Life numbers are the server's, carried to the runner.* §2.7 has the
   runner count adds. A pre-start joiner's SessionStart is sent from
   `physics_session_member_joined`, before the posted add runs, so the
   server could not list the life the runner would count. The server
   numbers lives at its own add call (`begin_life`: the count of adds of
   the id in the session, from 1; the start members are 1, as
   `create_session` numbers them) and passes the number to `add_player`.
   A failed add skips its number; lives are keys, not a sequence.
2. *`retiring_orders` is a map keyed by (id, life)*, not a
   `std::set<uint8_t>`: a rejoin's new life may already hold another order
   while its old one retires. `on_player_removed` releases by the key; it
   also fires (with `in_world` false, no SessionMember) for a life that
   never reached the world: a deferred rejoin cancelled by a second leave
   (`remove_player` of an id the world does not hold), so no order sticks.
   A failed add releases its order in `on_player_add_failed`.
3. *Runner callbacks take one struct* (`member_change`: session, player,
   life, join order, tick, `in_world`) instead of the design's argument
   lists.
4. *Lookups by (player, applied tick), not a life field threaded through
   `pending_birth` / `due_life` / `pending_deaths`.* J3 puts every event
   of a life inside [J, R), and two lives of one id never overlap, so the
   applied tick names the life (`member_life_at`); where an old life's R is
   not heard yet it is still open, and the newer life (the later J) wins
   from its J on. The event's own life is used where the event is in hand
   (parking, the outside check). Both counters (`member_life_outside`,
   `mirrors_outside_lives`) stay at 0 when the rule holds.
5. *A late joiner's catch-up Sector stamps are not parked.* The server
   resends its whole `sector_history` to a joiner (and to a resynced
   member), stamps of lives that left before the joiner joined included;
   §2.8 would park them for ever and the joiner's union would never grow by
   their sectors. A Sector event of an unknown life stamped before this
   numbering's first tick (`tick_base_`, once assigned) takes the existing
   catch-up path ("starts live") and is counted (`catch-up stamps of untold
   lives`); every other event of an unknown life is parked.
6. *An own event of another life* (a repeat of the previous life's
   Unphysicalize reaching a rejoiner) is no echo of ours: dropped and
   counted.
7. *The journal's PLAYER records.* A life's PLAYER(+) waits until every
   older life of its id has its PLAYER(-), so a rejoin's PLAYER(-) and
   PLAYER(+) at one tick are in that order in the file whatever order the
   messages came in (§2.8 needs it for SimTool's file order). A life told
   of after SessionStart is journalled once the numbering is assigned, at
   `max(J, tick_base_)`: a record below a late join's or a resync's base
   would sit in the hole the renumbering leaves and make the replay see one
   more jump. The removal is clamped the same way.
8. *`/bigobj` for `BallanceMMOMessageTests`*: the core test unit passed
   MSVC's section limit with M1's tests (C1128), as SimTool's did at RO1.
9. *Test fixtures*: the core tests' other ids (5-10) get a life with join
   order 63 - the placeholder an unknown id had before M1, so the ranks,
   groups and kicks those tests were written against are unchanged; every
   test event carries life 1.

**Checks** (QUICK tier; about 11 minutes of checks, one live run).
- x64 BUILD-OK (`session_messages_test.cpp.obj` #deps 111, VALID); x86
  CLIENT-BUILD-OK (23 objects, `core_host.cpp`, `BallanceMMOClient.cpp`,
  `physics_session_client.cpp`, `room_client.cpp` among them: the header
  deps held, #deps 1426 / 1240 VALID before the build); ARCH-BUILD-OK
  (`ballanced-36c04e3d24c9+bmmo-a7127c43375c-dirty`); GCC syntax check 30/30
  ok; `BallanceMMOMessageTests` 603/603 (595 + U4 `SessionMemberMsg`, U5
  (a), (b) on time and past its tick (the late path: made at its stamp as
  `P#5` with the life's kick, its rewind asked for), (d), (e), (h),
  loading, the journal's file order `SessionClientCoreMemberLives`, U6's
  rules `MemberLives`).
- Live smoke (`maps/arena-test/wm1_live`, rWM1, Arch server snapshots off,
  trafo inputs and placement on, 4 clients 45 s, `--rejoin 2:1200
  --join-late 3:1500 --leave 1:1800`): every client rc 0, the server
  journal 3573/3573 matched, `checkpoint_mismatches=0`, VERDICT PASS,
  `join orders taken in the world 0`.
  - J5: the rejoiner's old life leaves at 1201 and stays to 1203; its new
    life (join order 3: its old order 2 was retiring) is added at 1203 =
    R; SessionMember removed then added at 1203 to clients 0 and 1; its
    spawn kick index 14 at 1442 on the server and in clients 0, 1, 3; its
    own ball in `client2.rejoin1.bmjr` exact at all 180 checkpoints of
    1442-1800 (`body_series.py --body ball:2727132572`); the mirrors on
    clients 0 and 1 off 0.0152 m for 3 checkpoints after its spawn
    (1444-1448), exact from 1450.
  - J1: the joiner added at 1515 with join order 2 (released at 1203);
    every client journal has `player +p1747523703 join=2` at 1515 and no
    `join=255`; spawn kick index 40 at 1734 on the server and in clients
    0, 1 and 2's second life (before M1: 60 against 56); its mirror off
    0.0109 m for 3 checkpoints after its spawn (1770-1774), exact from
    1776 in every client.
  - J2/J-e: client 1's ball leave at 1799, removed at 1801 (`next + 1`),
    SessionMember removed at 1801 to the three others.
  - `events_parked` 0 (the reliable member message came first: J3's
    held-message arm is deferred), copies 18 of 21 on client 1.
  - SimTool `--replay-session` of `client0.bmjr`: the rejoin's and the
    joiner's PLAYER records add them with their recorded orders (kick
    indices 14 and 40, as the server), no "no PLAYER record" note, 20
    checkpoints, 0 mismatches.
- Seen, not this step's: client 3's (the late joiner's) LOCAL checkpoints
  are off the server's between its base 1530 and the state's re-step to
  1551 (15.48 m at 1550; exact from 1552): A5's join window, where the
  world runs from the level start until the state of base - 1 is applied.
  Its journal's replay stops at a jump 212 -> 788: the catch-up events are
  journalled at their own (old) ticks, as before M1.

**A known bound.** `lowest_free_join_order` still hands out orders 64-255
(as `assign_join_order` did), while the SessionMember reader refuses a join
order of 64 or more: a 65th order held or retiring at once would give a
life no client can be told of (its events parked for good). Before M1 the
world's 64 slots and the fallback bounded it the same way; step Q7's cap
below K closes it.

**Deferred** (with why).
- *The wire check and the re-capture.* Protocol 18 makes every reference
  capture refuse (the events have no `life` trailer, so even
  `--accept-protocol` cannot replay them: SessionStart parses, the first
  SessionEvent does not). A new reference set means re-capturing the five
  arms (`core_ref_s9`'s recipe, about 7 minutes, five live sessions): past
  the step's two-live-run cap. Every later headless step's wire check is
  vacuous until a protocol-18 set exists, so the re-capture is best queued
  as its own small step right after M1 commits (`s9_all.sh` with a new
  store, the committed binaries), before Q7.
- *R3* (a retail late joiner and a retail member): a second live run of
  about 6 minutes past the 15-minute budget. The x86 build compiled the
  host's plumbing; R3 is its first live use.
- *J3* (`--hold-member-ms 300`): `.local-tools/arch_server_lp.sh` starts
  the server with `--port` alone, so the flag cannot be passed without
  changing the shared launcher; the parking itself is covered by U5 (b).
- *J1b (10 % loss, 5 seeds), J2 (order reuse with contact), J4 (a join inside a leaving interval), U5 (c),
  (f), (g), (i), U6's fake-runner test, U7's SimTool-level test*: more live
  arms and test scaffolding than the budget holds; the live run above
  covers J1, J5 and J-e once each.
- *WM0's harness nits*: the harness verdict reads `client{i}.bmjr` only
  (the rejoiner's second life is not judged; its second base would count
  among the resync bases); a refused rejoin Join; a switch behind a late
  joiner's base. Not changed here.

**The review round.** Three changes, no behaviour a live run reaches:
the late joiner's `assigned_lead` grows with the J4 clamp (`join_lead` plus
the delta the clamp moved the base by), so `lead_adjust` does not read a
clamped joiner as early by that delta (as before, `assigned_lead` is the
nominal lead: `reanchor_tick_on_schedule` already places a joiner off
`next + join_lead`); `on_player_add_failed` ends the member's session only
while the member is still on the failed life (one that left and came back
meanwhile is on a newer life the failure is not about); the runner's two
`if (has_ball)` blocks in `remove_player` are one. The second-leave cancel
and the failed-add release were verified by reading, not by a test: the
dropped change carries the waiting life (`rejoin_life`) and
`on_player_removed` erases `(id, life)` before it checks `in_world`, so
the new life's order is released and no SessionMember goes out; `add_now`
never ran, so the runner's `lives` still holds the old life, which
`finish_leave` reports at R with the leaving entry's `rejoin` cleared (no
add follows); `on_player_add_failed` inserts the order into
`retiring_orders` through `physics_session_member_left` and erases it right
after. A runner-level test needs a world-less `session_runner` fixture,
which the test target does not have (it does not link `BallanceMMOSim`;
lead_test.cpp says the same), and the live window between a leave and its
R is about 3 ticks, too narrow to hit from the harness: the fixture is
queued for the close-out. The protocol-18 reference set is queued as step
REF10 (core_ref_s10), right after this commit.

### As built (step REF10, 2026-10-28): core_ref_s10, the protocol-18 reference set

Protocol 18 made every `core_ref_s9` capture refuse, so WM1 queued the set's
re-capture. `maps/arena-test/core_ref_s10` holds `core_ref_s9`'s five arms
(`resync_snapoff`, `swing_snapoff`, `l8_cross`, `solo1`, `solo2`) with their
arguments unchanged, captured by WM1's committed x64 build (`build-agent-rWM1`)
against the Arch server built from e12e832 with the clean id
`ballanced-36c04e3d24c9+bmmo-e12e832ad8de`, snapshots off; its README.md has
the binaries, the numbers and the comparison with `core_ref_s9`, and
`.local-tools/wire_check.sh` now defaults to it (`core_ref_s9` stays for
protocol 15-17 binaries; a protocol-18 binary cannot replay it: refused at
SessionStart, and with `--accept-protocol 15` the replay of its solo1 diverges
from the capture at step 522, the events having no life trailer). No source
changed.

- **The bit.** The arms run with WORLD_PROXIMITY_UNION set: it is the server's
  default (`config_manager.hpp:109`) and the test config's, so the set is what
  a deployed server runs. The visible consequences in the sends: no
  BodyRevived (G5), so about 190-290 fewer sends a client and an `l8_cross`
  event count of 126 instead of about 1015; with BALL's retired poses and
  WM1's life trailer this accounts for every send-mix difference against
  `core_ref_s9`. `trafo_inputs` and `placement_clones` stay as `core_ref_s9`
  had them (the arguments are unchanged).
- **A void first pass.** The first capture of the two 60 s arms (without the
  host-load sampler, right after the Arch build) logged 509-1227 ms frames on
  all four clients at the same ticks (resyncs on every swing client); the
  same binaries under `cpu_sampler.ps1` minutes later logged at most 49 ms.
  This is attributed to a host stall, not measured: it has SMK's signature
  (all four processes stalled at the same ticks, gone on the rerun under
  `cpu_sampler.ps1`, where total CPU was 6.7-13.5 % and no outside process
  passed 1.6 %), but no sampler ran during the void pass. A process
  `krig-miner.exe` (started from the user's Downloads folder at 21:50) was
  running; it used under 0.2 % during the kept runs and is unmeasured for the
  void pass, so it is not excluded as a cause. The two arms were re-captured
  and the void logs kept in `maps/arena-test/ref10_void_a/`.
- **The x64 tree's lost header deps.** `build-agent` had `#deps 0` on 17
  objects (physics_state, physics_world, sim_tool, config_manager, the
  session client's client_journal and seven client_core_* among them), the
  B5RX/B6RC signature; `bmmo_build_id.h` changed and physics_state.cpp was
  not rebuilt. The sources were touched and rebuilt: every object's code and
  data sections are the same as before (only `.chks64` and the debug sections
  differ; physics_state's `.rdata` is the build-id string), so rWM1 is HEAD's
  code, which the wire check rWM1 vs `build-agent-rREF10` (HEAD, clean id)
  confirms. rWM1's journals carry WM1's pre-commit id `...+bmmo-a7127c43375c-dirty`.
- **Checks.** Each server journal replays matched (4136, 3947, 3171, 2930,
  2927 ticks). `wire_check.sh - rWM1` (replays vs the live traces) and
  `wire_check.sh rWM1 rREF10`: no COVERAGE-SHORT, 12/12 sends SAME, 12/12
  journals IDENTICAL, ALL SAME; the replays' final status fields and `reach:`
  lines equal the live ones. solo1 / solo2 replay to their end markers, sends
  SAME, journals byte-identical with rWM1. 603 tests pass, GCC check all ok.

### As built (step SIO, 2026-10-29): session I/O off the tick thread

The item SMK queued (roadmap section 4 item 2, "Session I/O on the tick thread"),
built as the user decided (2026-10-29: "开独立写盘线程，然后不要主动flush" - a
separate writer thread and no proactive flush; and, since asio is already
there, an asio strand of a pool instead of a dedicated thread).

- **One writer for every host** (`BallanceMMOCommon/include/session/io_queue.hpp`,
  asio-free, so the SimTool's sim-only builds still include the journal):
  `io_queue::append` moves finished bytes into an in-memory queue under a short
  lock (no syscall, no lock held across I/O) and, when no write task is
  pending, posts one through the host's `io_poster`; the task (`run_batch`)
  takes the whole queue and `fwrite`s it in order, **with no flush**. One
  strand per file keeps the order. `close()` is posted too (the strand writes
  what is queued, then `fclose`s) and returns at once; a destructor of a
  still-open queue closes synchronously (drain + close, waits at most 2 s for
  the file). The asio half is `session/io_pool.hpp`: `strand_poster(executor)`
  (`asio::make_strand` + `asio::post`) and `io_pool`, a one-thread
  `asio::thread_pool` whose destructor `join()`s (every queued write and close
  runs) - included only where asio is on the path.
- **The journal** (`journal.hpp` `journal_writer`): its `std::ofstream` is an
  `io_queue`; `open(path, header, max_bytes, error, poster = {}, max_queued =
  kJournalMaxQueued)`. The bytes are built and counted exactly as before (the
  size cap and its note included, `bytes_` counts what was queued), so the file
  is byte for byte the old writer's. With a poster nothing is flushed; without
  one (the SimTool, the tests, the retail legacy path's
  `session_journal_client`) it writes on the caller's thread with the same
  per-tag flushes as before (adaptation: the BAR keeps the legacy path as it
  was; only its buffer is a `FILE*` now, same bytes, same flush points).
- **Where the strand comes from.** The core asks its platform: a new
  `session_platform::io_strand` (`client_platform.hpp`, beside `journal_path`;
  empty = the tick thread writes, the fallback) that `client_journal::open`
  calls once per file. Headless (`sim/session_client.cpp`): one static
  `session_io` holding a one-thread `io_pool` and the log's queue, started at
  the top of `main`; the journal, the inbound capture and the log each get a
  strand of it. Retail (`session/core_host.cpp`): `strand_poster(thread_pool_.get_executor())`
  - the mod's existing `asio::thread_pool`, no new data member. Server:
  **adaptation** - the pool is the server's (`server::io_pool_`, declared
  before `runner_` so it outlives it; made once, in `init_physics_runner`,
  which only `setup()` calls) and reaches the runner as
  `runner_config::io_strand`, because `session_runner` lives in `BallanceMMOSim`,
  which is also built sim-only without the asio include path.
- **The log** (headless `logf` / `log_line`): formatted on the caller's thread,
  appended to the stdout queue under one mutex (the order is the call order)
  and written with **no flush** (the user's decision taken literally: stdout's
  stdio buffer reaches the file when it fills, at the close and in the exit /
  crash drains). **Pending the user's decision:** `arena_map_test.py`'s
  `LogWatch` follows a client's log live and arms the proxy link events and
  the late joins (`--join-late`) from the moment it reads `tick base N
  assigned`; without a flush that line is only visible once the 4 KiB buffer
  it sits in fills - measured on the `sio_stall` client logs, 0 s for two
  clients and about 2.6-3.1 s (ticks 200 / 231 against base 27) for the other
  two - so those events fire late by that much until the user decides
  between (a) an `fflush` per written batch on the writer thread (never on
  the tick thread; the implement round had it), (b) no flush (built), (c)
  a per-process opt-in the harness sets (the server's `auto_flush_log` is
  the precedent). **Answered:** the user chose (a) and asked for a flush
  option with all three modes, default per batch - built by step IOF (next
  note). Native code writing to stdout directly (an engine DLL's
  `printf`) can now land ahead of queued lines; cosmetic. Before
  `start_session_io()` and after static destruction a line is written and
  flushed directly, as before. **The capture** (`sim/inbound_capture.hpp`): the records of a main-loop
  pass are collected in memory and handed over once per pass (or at 256 KiB);
  its close stays synchronous at the run's end (bounded 60 s) so
  `write_failed()` is final when the client logs it.
- **What still runs on the tick thread:** opening a file (`fopen`, once per
  session; its failure is still reported there), and on retail
  `journal_path`'s `create_directories` / prune. Nothing per record. One
  exception to "opened there": a reopen of the path a queue's posted close
  still holds (the headless client's journal for a second SessionStart:
  `client_core.cpp` closes it and opens `args_.journal` again) is **deferred to
  the writer** - its first write task (or a drain) writes the old file's queue
  out, closes it, then truncates the path - so the tick thread neither
  truncates under the open handle (the review's finding: the old strand's tail
  would land at its old offset inside the new journal) nor waits for a stalled
  disk; such an open's failure shows as the queue's `failed()`. The queue
  mutex is timed: the crash drains only try it (bounded), so a fault inside
  `append`'s critical section cannot hang the handler.
- **Memory bound, never blocking:** a journal's queue holds at most
  `kJournalMaxQueued` = 32 MiB; past it the journal stops like the size cap
  with the note `cap: writer queue full (N bytes waiting for the disk),
  recording stopped` (the file stays a valid prefix). The log's queue: 16 MiB,
  then lines are dropped and counted, and the next line that fits is preceded
  by `[log] N bytes of log lines dropped: ...`. The capture's: 64 MiB, then the
  capture stops as a failed write does (no end marker, logged). The heaviest
  journal measured (checkpoint every 2 ticks, the smoke below) is about 135
  KB/s, so 32 MiB is about 4 minutes of a disk that takes nothing; at the
  default cadence much longer.
- **Crash-loss bound.** A hard kill (TerminateProcess, SIGKILL, the harness's
  10-minute kill) loses what is still in the queue plus the stdio buffer: in a
  healthy run the queue is taken as soon as it is posted (peaks 2.7-6.2 KB on
  the retail and server journals below), so about one stdio buffer (the CRT's
  4 KiB on MSVC, BUFSIZ-sized on glibc; the capture keeps its 1 MiB buffer);
  under a disk stall, up to the stall's records (at most the 32 MiB bound).
  Before SIO a kill lost only the TICK and INPUT records since the last
  flushed record (player, event, checkpoint, note, correction, resync). A
  crash the handlers see drains every queue first (best effort, 1 s per file):
  `bmmo::sim::set_crash_drain` (new, `sim/crash_report.cpp`: the unhandled
  exception filter, abort, the Linux fatal signals) for the headless client
  and the server, and on retail the `CrashCallback` of `dumpfile.cpp`'s
  `UnhandledExceptionFilterEx` (`on_fatal_error`) - the retail mod has no
  `BMMO_CRASH_DUMP` handler, that variable is the sim's. `OnExitGame` drains
  too, last (after `cleanup` ended a running session and posted its close;
  bounded 2 s), while `thread_pool_` still exists; a write task a stopped pool
  abandons unrun is caught by the queue state's destructor, which writes what
  is queued and closes the file; the inbound replay's divergence exit drains
  before `std::exit`. A process that never had a queue with a writer thread
  (the retail legacy path) never builds the drain's registry: its crash
  handler allocates nothing new. Power loss: the OS cache, as
  before (no fsync anywhere, before or after).
- **Test switch:** `BMMO_IO_TEST_DELAY_MS` (environment, read once): every
  write task sleeps that long holding its file before writing, as a stalled
  `WriteFile` would. Never applies to a synchronous queue.
- **Checks (QUICK).** `BallanceMMOMessageTests` 610/610 (six new
  `SessionIo.*`: the strand's bytes equal the synchronous writer's for the
  synthetic session, through a real pool and through a stalled poster released
  later, with nothing written before the release; a destroyed writer drains its
  queue; the size-cap note with a writer thread is byte for byte the
  synchronous file; a full queue stops the journal with its note and blocks
  nothing; `io::drain_all` writes a stalled queue; the stdout shape keeps 2000
  lines in order and leaves the stream open). GCC syntax check all ok; the
  Arch server built (`ballanced-36c04e3d24c9+bmmo-04d93c012c00-dirty`). Wire
  check `rSPX` vs `rSIO` over `core_ref_s10`: 12/12 sends SAME, 12/12 journals
  IDENTICAL, ALL SAME. Live smoke (`work/SIO/live.sh sio_stall 2000`: 4
  clients, 60 s, LANE arm, snapshots off, the clients with
  `BMMO_IO_TEST_DELAY_MS=2000`, i.e. each of the three files of a client got a
  write every ~6 s): starved 0, rebases 0, resyncs 0 on all four, frame t_max
  19-23 ms; the server journal replays matched 4601/4601 (checkpoint
  mismatches 0); each client's captured stream replayed with the same binary
  and no delay writes a journal byte-identical to the one the stalled writer
  wrote (4/4), and every capture reached its end marker; journal queue peaks
  1.24-1.26 MB, capture 168-177 KB, the server's 6.2 KB. Its window A's
  `own 147.7-153.9` is not this step's: wm1_live (WM1, before SIO) shows the
  same 149.67. Retail (`work/CD1/cd1_gate.py sio_retail`, L11, two copies,
  core on, snapshots off, placement on): both client journals written by the
  `thread_pool_` strand (queue peak 2690 bytes), read to their end with no
  trailing bytes and `end: room closed` last; the game exited cleanly.
  Gate round (the reopen deferral, the timed queue mutex, the log flush
  removed): one more `SessionIo` test (a reopen of the same path under two
  stalled strands, run in either order and through `drain_all`, ends as the
  second session's journal alone, byte for byte), the builds, the tests, the
  GCC check and the wire check rerun (numbers in the commit message); the
  live smoke and the retail run were not rerun (the live paths' bytes are
  unchanged and the step had used its two live runs).

### As built (step IOF, 2026-10-29): a flush option for session I/O

SIO's pending decision, answered by the user (2026-10-29): "(a) Flush per
batch", then "加个flush的选项，然后包含你说的那些模式。默认flush per batch" (add a
flush option with the modes discussed; the default is flush per batch).

- **One mode enum, in the shared writer** (`session/io_queue.hpp`):
  `io_flush { none, batch, record }`. `none` = no flush until the close (and the
  exit / crash drains): SIO's committed log behaviour. `batch` = one `fflush`
  after each batch the strand writes (the default everywhere; a batch that ends
  with the close is not flushed separately, its `fclose` does it). `record` =
  one `fflush` after each appended chunk - a journal record, a log line, a
  capture pass (the capture hands its records over once per main-loop pass or
  per 256 KiB, so its "record" is a pass) - the pre-SIO granularity without the
  tick thread. Every mode flushes **on the writer strand only** (`run_batch` /
  `write_chunks`), never on the tick thread, and no mode changes a byte of a
  file. The mode is per queue (`queue_state::flush_mode`, atomic), given to
  `io_queue::open` / `open_stream` (default `batch`), changeable with
  `set_flush` (the headless log is open before its command line is read);
  `io_queue::flushes()` counts the mode's flushes (not the close's or a
  drain's). `io_flush_config { log, journal, capture }` and
  `parse_io_flush_config` (`MODE` for every stream, or
  `log=MODE,journal=MODE,capture=MODE`, any subset) are the one spelling the
  hosts share. `queue_state::flush_batches` (SIO's always-false flag) is gone.
- **Synchronous writers keep their flush points** (no poster: the SimTool, the
  tests, the retail legacy path): the mode is not consulted there;
  `journal_writer` still flushes after player / event / checkpoint / note /
  correction / resync records. `journal_writer::open` and
  `inbound_capture::open_capture` take the mode as a defaulted last argument.
- **Per host.** Headless client: `--io-flush MODE | log=..,journal=..,capture=..`
  (usage line; each stream batch by default), the log's mode set right after
  the command line is parsed, the journal's through a new
  `session_platform::journal_flush` (`client_platform.hpp`; empty = batch;
  `client_journal::open` asks it once per journal), the capture's through
  `open_capture`. `--io-flush` is stripped like `--trace-sends` by
  `replay_arguments_differ`, so a capture replays under any mode (the reference
  captures were recorded without it). Server: `config.yml` gets a top-level
  `session_io:` map with `journal:` (none / batch / record, default batch;
  anything else is a warning and batch), carried as
  `runner_config::journal_flush` to `session_runner::open_journal`.
  **Adaptation:** the step names log / journal / capture keys for the server,
  but the server writes one session I/O stream, its journals: its log file is
  written by the logging thread itself (`misc.cpp` `DebugOutput`, no writer
  strand, so no batches) and it has no capture. So the server reads `journal`,
  and a `log` or `capture` key present in the file is answered with a note
  ("a headless client stream; the server has none (its log file follows
  auto_flush_log)") and has no effect; nothing is written for them by default.
  `auto_flush_log` keeps working unchanged and still decides the server's log
  file (true flushes after every line, like `record`; false leaves it to the
  buffer, like `none`); the config comment says so. Retail: a BML property
  `Gameplay/SessionJournalFlush` (string, default `batch`; anything else is
  logged and taken as batch), read with `SessionJournal` when a journal opens,
  through `config_manager`'s existing `props_` map (no `BallanceMMOClient` data
  member) and `p.journal_flush` in `core_host.cpp`; the mod has no session-I/O
  log or capture. SimTool: `--io-flush` is parsed (merged into the
  `--write-journal` branch: one more `else if` hit MSVC's C1061 nesting limit)
  and passed to its `--write-journal` writer, which has no poster, so - as the
  step's own rule "without a poster behaviour stays as SIO left it" requires -
  it changes nothing there (the usage line says so).
- **Defaults changed by this step** (the user's decision): every writer with a
  poster now flushes once per batch - the headless log (was none), the
  headless and retail journals, the server's journals and the headless capture
  (were none). The crash-loss bound of SIO's note shrinks accordingly: a hard
  kill now loses the queue (empty in a healthy run) plus at most the last
  batch's bytes not yet flushed, instead of up to one stdio buffer.
- **Checks (QUICK, about 13 minutes).** `BallanceMMOMessageTests` 614/614
  (three new `SessionIo.*`: the parser (every form, a stream not named kept,
  eight malformed specs refused with the config unchanged); a journal by a
  stalled strand released in two batches, then closed, in each mode - the
  final bytes equal the synchronous writer's in all three, the bytes on disk
  after the first batch are none / the whole batch / the whole batch, the
  mode's flushes 0 / 2 / 9 (header + 5 notes + 3 notes), and a synchronous
  writer given `none` still flushes at its NOTE; the stdout shape in each mode
  after a late `set_flush`: a batch of four lines visible after the batch
  except under none, flushes 0 / 1 / 4, every line there after the close).
  GCC syntax check 30/30 ok. Builds: x64 BUILD-OK, x86 CLIENT-BUILD-OK (the
  ten includers of `client_platform.hpp` rebuilt, deps valid), Arch server
  `ballanced-36c04e3d24c9+bmmo-b65f7eadc1ef-dirty`. Wire check `rSIO` vs
  `rIOF` over `core_ref_s11`: 12/12 sends SAME, 12/12 journals IDENTICAL, ALL
  SAME. Live smoke (`work/IOF/live.sh iof_live`: 4 clients, 60 s, LANE arm,
  snapshots off; clients 0-2 default, client 3 `--io-flush log=none` as the
  control; a 0 ms proxy spike on clients 1 and 3 at tick 600; the server's
  test config with `session_io.journal: record` and a `log:` key; a 5 ms log
  poller beside the harness): the server kept `journal: record`, printed the
  `log` note, and its journal replayed 4117/4117 matched (checkpoint
  mismatches 0); VERDICT PASS, starved 0, rebases 0, resyncs 0 on all four;
  both events armed from tick base 27. Visibility of the clients' tick-stamped
  log lines after the base line, against the tick clock: batch clients 0-2
  saw every line 0.001-0.088 s after it was due (polls of 10 ms plus a batch),
  and the three base lines within 5 ms of each other; the `none` control saw
  them up to 2.586 s late in buffer-sized steps (72 reads against 470-598),
  SIO's symptom. Not run: a retail session (retail code only reads the
  option and hands it to the shared writer, which the tests and the headless
  run exercise; the x86 build compiles it), the solo replay (no rollback or
  session logic touched; the wire check's journal identity covers the bytes).
  SIO's `.local-tools/work/SIO/sio_log_flush_per_batch.patch` is superseded
  (not applied).

### As built (step WGQ7, 2026-10-29): Q7, per-player mechanism units (protocol 19)

**Protocol and journal.** P_Q is **19** (`PHYSICS_PROTOCOL_VERSION`;
`PHYSICS_PROTOCOL_PLAYER_MECHANISMS` names it): `session_start_msg`
gains `mechanism_slots` (u8) right after `world_options`, written and read
from protocol 19 on; under the bit the reader refuses 0 and anything above
`MAX_PLAYERS_PER_SESSION`. The journal takes **v8**: the HEADER's trailing
`mechanism_slots` (an older file reads 0); the server's and the clients'
headers carry it, and `scripts/journal_trace.py` reads it.

**What exists.**
- *The unit rule* (`session/mechanism_units.hpp`, new, CK-free): the
  abstract graph (blocks with prototype, parent, children, parameters and
  pre-copy walk position; links with their initial delays; parameter
  operations), `find_unit` (D, C, U, S, the shape, the refusals), the link
  classes (inside, fan-out, fan-in), the shared sources, the operations a
  block unit owns, `describe`, `mechanism_slot_count` (K) and `bind_slots`.
- *The copy step and slot binding* (`game/proximity_union.hpp`):
  `rewire_options::slots` (K). With the union rules and K >= 1, `rewire`
  first runs `copy_units`: the units of every identity read of the class
  (each read's one gating probe), the abstract graph extracted from CK
  (`extract_model`), refused on a third shape, an overlap or a hot source
  (a non-zero Pointer parameter); slot 0 is the original; for k = 1..K-1,
  unit by unit in (owner, script, walk) order, `CopyObjects` with
  dependencies that copy names only (a behaviour copies its IO,
  parameters, sub-behaviours, links and operations with it), the copy added
  to the unit's graph, the copied inside links and operations added there,
  and the fan-out and fan-in links created with the original's initial
  delay. Each copy's structure (blocks, IO and parameter counts, links and
  delays by walk position, operations; no CK id) must equal its
  original's, and its locals their values; else the rewire refuses (all or
  nothing, Q10's check in every world). Then `rewire_probes` /
  `rewire_identities` name a unit's blocks `BMMO_Prox_<owner>_<n>_S<k>` /
  `CurrentLevel#BMMOId_<owner>_<n>_S<k>` (n the original's pre-copy
  walk position; every other probe keeps its G3 name, also by its pre-copy
  position), and a slot read's gating probe must be its slot's. `place`
  binds slot k to the candidate whose `slot` is k (`bind_slots`): the
  frame follows it or holds; the identity cell is its ball entity (by-name:
  the retail entity of its type) or `BMMO_NoBall`, never the hold (the
  cells start on `BMMO_NoBall`); the body-kind latch moves an output that
  does not name the slot's candidate. A candidate outside [0, K) binds
  nothing (`unslotted`). The hold sections (`capture`, `restore`,
  `capture_portable`, `localize_hold`) list every frame (K per unit probe)
  and no slot by-name cell (§2.10). `signature` appends, only with units,
  K and per unit its owner, script, read position, shape and every slot's
  structure, so the G3 signatures are unchanged without the bit.
  `release` destroys the copies with their links and operations after the
  union's own objects. `controller_alias` gives every unit force block,
  slot 0 included, `unit:<owner>/<n>/S<k>` (§4.3). `units_summary` is
  C1's line `player mechanisms: K slots, U units, C copies, +B record
  bytes` (B: each copy's blocks as the v2 record writes them).
- *The bit and the config.* `WORLD_PLAYER_MECHANISMS` (`entity/session.hpp`)
  and `physics.player_mechanisms` (default on, with the union only); K =
  `maximum_members` clamped to [1, 64].
- *The server.* SessionStart carries K; `assign_join_order` hands out
  orders below K (`capped_join_order`, `session/member_lives.hpp`); a
  joiner that finds every order below K held or retiring waits in
  `waiting_orders` (first come, first served: `on_player_removed` and
  `on_player_add_failed` call `admit_waiting`), and one that finds every
  order below K held by a live life gets SessionEnd "physics session full
  (K mechanism slots)" and stays in the room; a waiter that leaves is taken
  off the list, and a session's end tells the waiters as it tells the
  members. The runner takes K (`create_session`), the world copies
  the units at its anchor (class `all`) and a clone's slot is its join
  order (`probe_candidates`), the server journal's header records K.
- *The core* fills `probe_candidate::slot` from the life the placed tick
  names (`member_life_at`, the server's join order; live and in every
  replay), keeps SessionStart's K (`mechanism_slots()`: K under the bit
  while the union is active) and journals it.
- *The hosts.* SC and CH pass `core.mechanism_slots()` into the rewire and
  install one alias function per manager: the trafo sets' names first, then
  the units', then the own name (SC `install_controller_alias`, CH
  `controller_alias_cb` over both statics; the trafo teardown keeps the
  units' part); the server's world installs the same chain at its boot.
- *SimTool.* `--replay-session` takes K from a v8 header; V6's
  `--release-check` also counts the CK objects per class around each round
  and lists a copy's own objects a release left alive (none); the drop scene
  takes `--player-mechanisms K` with `--world-options PROXIMITY_UNION`
  (V7) and `--probe-trace` prints each unit force block's slot and whether
  it holds a force; `--probe-census --release-check --player-mechanisms K`
  is V6; `--world-options` knows `PLAYER_MECHANISMS` (bit 4).
- *Tests* (`BallanceMMOMessageTests` 621/621): U8 (`MechanismUnits`: the
  fan and the bridge fixtures transcribed from L11, the three refusals),
  U9's CK-free parts (slot binding, the join-order cap, K from 100 members
  = 64 and the 65th refused), `SessionStartMsg.MechanismSlots`,
  `SessionJournal.V8MechanismSlots`.

**Adapted (the code is what it is; each within the principles).**
1. *The bit is 4, not "the next free after bit 2".* Bit 3 is reserved by
   the record-format design's `WORLD_SESSION_CLOCK` (not built); taking it
   would collide when K1 lands.
2. *AddSubBehavior sorts the graph, so the children are put back.* CK's
   `AddSubBehavior` sorts a graph's children by priority with the CRT's
   (emulated) qsort, which is not stable: the fan's root graph came out
   permuted at the first copy, and a graph runs its active children in that
   order (`CKBehavior::CheckIOsActivation`); the release then left the
   permuted order (V6 found the second round's walk positions moved). §2.10
   says each copy goes AFTER the existing sub-behaviours, so the copy step
   restores the pre-add order with the copy last, through a pointer to the
   protected `m_GraphData` (both SDKs declare `BehaviorGraphData` the same
   way); release's `RemoveSubBehavior` then leaves the pre-copy order. The
   signature does not cover the children's order, which the portable
   record's post-copy walk keys blocks by; it is the same in every world by
   construction: the load order every world already shares, with the copies
   appended in slot and unit order, and no longer a function of the qsort
   emulation.
3. *A block unit owns its operations.* The bridge's Test reads the cell
   through a `Get Name` parameter operation of the root graph (WG0's note);
   §2.10 does not list operations. A copy without its own would read slot
   0's cell, so the rule lists the operations between two unit blocks and
   the copy step copies them with the blocks (the copied Test's source is
   remapped to the copied operation by CopyObjects).
4. *The server copies before its selection exists.* The server rewires
   class `all` before its selection (G1's CK creation order), so its copy
   step takes every identity read's unit (not keep-local) and `classify`
   marks the mechanism units afterwards; on every level every identity
   read is in a mechanism script (WG0), so the server's units are the
   clients' (the signatures agree, below).
5. *A copy's local values are checked in the world, not signed.* The
   structure goes into the signature; the values (and Pointer locals, whose
   size differs between the x86 and x64 builds) are compared with the
   original's at the copy step only.
6. *The census keeps its own CK reading of the unit.* SimTool's
   `probe_census.hpp` (G0) still measures D, C and S from CK for its listing
   and its two-shape check; the worlds use `mechanism_units.hpp`. V6
   compares the two through the unit counts and the census's `unit shapes`
   check (both PASS on every level).
7. *`/bigobj` for the headless client* (C1128, as SimTool at RO1 and the
   tests at M1).

**Checks** (QUICK tier; about 13 minutes of checks, two live runs).
- Builds: x64 BUILD-OK, x86 CLIENT-BUILD-OK (core_host.cpp and the shared
  core rebuilt: the copy step compiles against the retail SDK),
  ARCH-BUILD-OK `ballanced-36c04e3d24c9+bmmo-badfea92e8d2-dirty`;
  `BallanceMMOMessageTests` 621/621; GCC syntax check 30/30 ok;
  `journal_trace.py --selftest` OK.
- **V6** (`.local-tools/work/WGQ7/v6_all.py`; L1-L13, placement off and
  on, K = 2 and 8, 52 runs): every run `PROBE-CENSUS: PASS` and
  `RELEASE-CHECK: PASS`; units 71 with the placement off (L2-L12 10, 4, 4,
  2, 10, 6, 3, 6, 7, 6, 11; the arena 2; L1 0), 132 with it on; copies =
  units x (K - 1); each copy's structure equal to its original's; two
  rewire/release rounds give the same signature and leave the CK object
  counts of the pre-copy graph (behaviours, links, IOs, operations, pIns,
  locals) - but see the parameter outputs below. The x86 and Linux SimTool
  listings are deferred (below).
- **V7** (`.local-tools/work/WGQ7/v7.sh`, the arena): (a) two paper balls
  in the P_Modul_18 box under the Q7 bit: both slots' forces held
  (`unit:P_Modul_18_MF/11/S0` and `/S1`, two controllers) and both balls
  rise (y 8.887 and 8.132 at report 80; each slot's cell names its own
  clone); under G3's bit alone one frame and one cell serve the nearer
  ball. (b) a wood ball held on Platte06, a stone ball 2.8 m beside it:
  the bridge breaks under the Q7 bit (fell 7.093 m; slot 1's cell
  `Ball_Stone`, slot 0's `Ball_Wood`) and not under G3's bit alone (fell
  2.461 m, the sag; the one cell names the wood).
- **Wire check** rIOF vs rWGQ7 over `core_ref_s11` (`NEW_EXTRA="--accept-
  protocol 18"`: the set is protocol 18 and has no Q7 bit): every replay
  reaches its end marker; the sends differ in exactly one line per client,
  SessionReady #5/#6 (75 bytes both: the protocol field 18 -> 19); every
  journal differs in exactly one record, the HEADER (118 -> 119 bytes: v8's
  K), with every TICK hash and pose and every checkpoint equal (12/12, e.g.
  l8_cross c0 11938 records, 2163 ticks, 1095 checkpoints). The bit-clear
  path is unchanged.
- **Retail, L11, two copies** (`.local-tools/work/WGQ7/wgq7_gate.py`,
  `scripts/retail/wgq7_gate1`; the Arch server of this tree: union, placement
  and player mechanisms on, K = 8, trafo inputs off, snapshots off; client 2
  forced rollbacks every 25 ticks, 10 deep; client 1 into sector 2): both
  retail anchors `player mechanisms: 8 slots, 11 units, 77 copies, +39998
  record bytes` and signature `68db2214ab881bbd` = the server's mechanism
  signature (the server's line is the same, digit for digit): **Q10 is
  settled for L11 - retail CK2 copies the units as Ballanced does** (the
  per-copy structure is in the signature). Both assigned (no "probe
  mismatch"), `skipped=0 relatched=0 two_bodies=0`; client 2 228 replayed
  places, 0 logical script differences (one `frame 6 differs [float]`, WG3's
  classification); the server "candidate set drift 0"; after the room's
  close "228 inputs back, 77 unit copies destroyed" on both. The server
  journal replays 705/705 with K = 8 from its v8 header,
  `checkpoint_mismatches=0`.
- **Headless smoke** (`.local-tools/work/WGQ7/live.sh`, WM1's recipe:
  arena, 4 clients 45 s, `--rejoin 2:1200 --join-late 3:1500 --leave
  1:1800`, `maps/arena-test/wgq7_live`): every client rc 0, `mism`/`hard`/
  `mech_hard` 0, the server journal 3462/3462 matched,
  `checkpoint_mismatches=0`; every anchor (the rejoin's and the late join's
  included) `player mechanisms: 8 slots, 2 units, 14 copies, +4907 record
  bytes`, signature `c7e224f5cf8c631b` = the server's; the releases "14
  unit copies destroyed". VERDICT FAIL on "client2 relayed frames past the
  rollback reach: 2" - client 2 is the rejoiner, both frames in its first
  window after the rejoin's late-join restore (window tick 1259, `late=2`,
  lag max 13); WM1's run of the same recipe had 0. **Attributed in the
  gate round (offline, `arena_map_test.py --report-only` on `wgq7_live` and
  `wm1_live` with the v8 reader; outputs in `.local-tools/work/WGQ7/fix/`):
  a counting artefact at the rejoin boundary, in code the step does not
  change, with no simulation effect.** `note_input_correction`
  (`client_core_messages.cpp:464-467`) counts a frame late on two
  conditions only: the rewind depth past `window_rewind_reach`, or
  `!knows_tick(tick - 1)`. The window's lag is 1..10 per remote (lag max 13
  over the run) against a reach of 48 (`reach=48/48/96` on every status
  line), so the depth term cannot fire; both frames took the second branch.
  The rejoined life's first record is the resync anchor, base - 1 = 1232,
  written at the assignment (`anchor_resync`,
  `client_core_resync.cpp:64`), and every later tick is recorded as it is
  stepped, so the anchor of a late frame lies before 1232: both are first
  arrivals of relayed frames for ticks <= 1232 handled after the
  assignment - inputs the server's state of tick 1232 already contains (the
  re-step uses the frames in (1232, 1247]: "15 relayed frame(s) in (1232,
  1247], 1233..1247" for both mirrors, complete). One frame per remote (the
  two remotes' lag histograms are identical: one relay message), so 2.
  Whether that message is handled before the assignment (then the core's
  current tick is still 0 and the frame is "not simulated yet", counted
  nowhere: WM1, wm0_live, wm0_live2) or after it (here) is the order of the
  unreliable relay against the reliable SessionAssign. Client 2's outcome:
  own ball 0.0000 m in B/D/A, other balls 0.0139 m, mechanisms 0.0000,
  `mism` 0; the server journal 3462/3462. The step changes neither branch
  (`client_core_messages.cpp`, `client_core_resync.cpp` and the relay are
  not in its diff; the join path gave the rejoiner the same join order 3 at
  the same tick 1203 as in WM1). Pre-existing by the code; not reproduced on
  the previous build within the budget (three earlier rejoin runs, WM0's
  two and WM1's, had 0). Queued (roadmap §4 item 2): a frame whose tick is at or before the
  last resync's anchor is covered by the state, not past the reach - count
  it apart (core) or exempt it in the criterion (harness). Late joins of
  earlier builds count many more (`SPGH2_live_latejoin` 96 at lag 24-34,
  `a5_join_1` 24); they are not analysed here.

**C1 (§7.6) and Q11.** The copies' v2 record bytes per record at K = 8
(the anchor line): L11 +20503 (placement off; §2.10 estimated +24 KB) and
+39998 (on, the default since SP10), L12 +42889 (off; estimate +50 KB) and
+101374 (on), the arena +4907. Measured on a real record: the arena late
joiner's resync script record 67359 bytes (WM1, same recipe) -> 73048
(+5689, +8.4 %; the copies plus the K-fold hold frames). Capture time: the
headless clients' `t_cap` per frame 0.470 vs WM1's 0.464 ms (client 0;
1260 / 2680 against 1222 / 2636). The CPU judgement is the close-out's;
L12 with the placement on is the case Q11 names.

**Found, pre-existing, queued (roadmap §4 item 5).**
- *An identity copy leaves two parameter outputs at its release.* V6 counts
  CK objects per class around a rewire and release: every
  `CopyObject(CurrentLevel)` copy (step G1's code, union rules or not)
  leaves 2 `CKParameterOut` behind when it is destroyed, their owner pointer
  dangling (reading it crashed two diagnostic runs). V5 never counted
  parameter outputs. The units' copies leave nothing (V6's baseline round
  measures the per-copy leak, and every round's count is the baseline's
  times its identity reads). Harmless while nothing reads them; queued.
- *WAD-A is not decided by these runs.* No fan force was held at any import
  in the two live runs. The one force controller held at the late joiner's
  import (`rt:force/47161`, P_Modul_26_Sack, not a unit) found no live
  controller, but so did the same core's constraint controllers
  (`sys:unknown`), and the joiner's script state was refused (the
  rejoiner's trafo set): the module had no live controllers at the import,
  so the run cannot tell an id mismatch from an absence. WM1's run of the
  same recipe logged the same lines. Under the bit every unit force block
  is aliased, slot 0's included (`register_force` in `copy_units` and
  `copy_unit`, `proximity_union.hpp`), so WAD-A now concerns only the force
  blocks outside units and the bit-off control; it is deferred to FULL
  with its recipe (below).
- *The server's unit set equals the clients' by the census, not by
  construction, and nothing guards it.* The server copies every identity
  read's unit (class all, before its selection exists; adaptation 4), a
  client only its mechanism scripts'. Equal today (L11 11 = 11 units, the
  arena 2 = 2: the server's and the clients' anchor lines), because WG0's
  census found every identity read in a mechanism script. But the anchors
  compare `signature(mechanism)` (`physics_world.hpp:427`,
  `session_client.cpp`, `core_host.cpp`), whose unit tail skips a unit
  `classify()` leaves out of the class: a level that broke the census would
  give the server K copies a client lacks, with no refusal (the review's
  "the signature catches it" does not hold; the comment at `copy_units`
  says so). Queued: write the count of the server's non-mechanism units
  into the tail when it is not 0 (every current signature unchanged), or
  refuse such a level at the server's anchor.

**Deferred to FULL** (the 15-minute gate ran the builds, the tests, GCC,
V6, V7, the wire check, one retail and one headless run): V6's x86 and
Linux SimTool listings; V4b (chained savestate rounds with two slot forces
across a death); H2b, H2c, H3b, H3c, J6 (driven fan and bridge crossings,
the resync and late join with a slot force held - the alias's live check);
R2's driven two-ball fan on retail; U9's runner-level parts (the waiting
joiner and the refusal in a live session: a room of 9 with K = 8); the
same-batch control for the headless smoke's relay verdict; **WAD-A** (a
resync and a late join with a fan force held, headless and retail,
placement on, the bit on and a bit-off control; pass = no "no live
controller" at the import; under the bit only force blocks outside units
are at stake); **REF12**, the
protocol-19 reference set (every `core_ref_s11` capture is protocol 18:
`--accept-protocol 18` replays them, above).

### As built (step F0, 2026-10-29): WM0's harness nits, WGQ7's re-read, J3's launcher

The close-out's tooling pass (FULL plan rows WM0-n, WGQ7-hn, WM1-J3's
launcher half). Harness `scripts/arena_map_test.py`, the headless client,
`.local-tools/arch_server_lp.sh`; no protocol, no shared simulation code.

- **A member's lives** (WM0's nit 1). `read_client_log` splits a client log
  into lives at each `session N starting:` line (a `--rejoin-at` begins the
  next): each life's assigned base, its **join import** (the late joiner's or
  the rejoiner's first state, `resynced: tick base N (old tick 0` - not a
  resync of a running client), where that import's re-step ended, and the
  life's real resyncs. The report reads **every life's journal**
  (`client{i}.bmjr` up to the next life's base, `client{i}.rejoin{k}.bmjr`
  from its own base on), so a rejoiner's next life is judged; its base is
  printed as `rejoined at`, not among the resync bases, and the import's
  count comes off its window's `resyncs=` (only when the import's own state
  applied: a state that never applied, superseded by a later resync, leaves
  the real one counted - TRL run B's client 1).
- **Before the import** (WGQ7's re-read, "client3 `oth` 530-728 m"): a
  joiner's world is not the session's until its import re-stepped, so the
  checkpoints from the life's base to the re-step's end are not compared
  (wgq7_live client 3: 1530..1551, 11 checkpoints; its `oth` in D 649.55 m ->
  0.0060 m). The mechanism error the joiner showed after it (421 m / 4402 m,
  the pre-TRL trafo-set refusal) stays visible.
- **The airship span** (WGQ7's re-read, "client0 own 152 m in A"): a ball is
  out of the comparison from the server log's `is in the airship zone at
  tick T` to its next `is a spawn ghost` birth (BALA; the balloon is the
  design's exception) - for its own ball and for every mirror of it.
- **A8's joiner lead** (WGQ7's re-read): `D + ceil(rtt) + 2` is the joiner's
  lead with `physics.lead_adjust` ON; the rule is now taken from the server
  log's start line (`lead adjust on|off`): off (the default) -> the joiner's
  printed input delay must equal the founders' (the session's).
- Every exclusion is printed per client (`not compared (step F0): before the
  join import 1530..1551 (11 checkpoints); c0 ball in its airship span
  2233..2672 (121 checkpoints); join imports not counted as resyncs 1`).
  Selftest: five new checks (two lives with an import, an import that never
  applied, the life journals, the airship spans, the lead mode).
- **The headless client** (WM0's nits 2 and 3, `session_client.cpp`): a
  refused Join of a `--rejoin-at` ends the run with
  `--rejoin-at: the Join of room R was refused (...): no next life, the run
  ends` instead of letting the room handling join another room, and the
  client exits with code 4 (the review: an exit 0 would let a run whose
  rejoin never happened pass; the harness fails every non-zero client exit);
  a switch (`--leave-at`, `--rejoin-at`, `--leave-room-at`, `--resync-at`)
  whose tick is behind the life's first assigned tick (a late joiner's or a
  rejoiner's base; `begin_session` re-arms the check for every life) logs
  `warning: ... fires at once` once. Neither fires in a reference capture
  (the wire check is ALL SAME).
- **J3's launcher** (WM1 deferred: `arch_server_lp.sh` started the server
  with `--port` alone): every argument after the 9th now goes to the
  server's command line (`... true true --hold-member-ms 300`; the Arch tree
  is built with `BMMO_SERVER_TEST_FLAGS=ON`). Dry start: the server log's
  `Test flag: --hold-member-ms 300 (the SessionMember messages leave 300 ms
  late).` (`.local-tools/work/F0/dry_start_server.log`). The launcher lives
  outside git, like every `.local-tools` script (`.git/info/exclude`): the
  change is in the working copy of this machine only.

**Re-verdict** (`.local-tools/work/F0/reverdict.sh`, copies of the runs
under `.local-tools/work/F0/rev`, each run's own arguments): wgq7_live
FAIL 4 -> 1, wm1_live 3 -> 0, trl_live_B 5 -> 2. Gone: A8's lead (three
runs), client 0's own ball in A (152.09 / 149.67 / 152.09 m, the airship
span), the rejoiners' `resyncs 0` (the join import). Judged now: the
rejoin lives (wgq7_live client 2 from 1233, wm1_live client 2 from 1238,
trl_live_B clients 1 and 2 from 1297 / 1655: own ball 0.0000 m in every
phase). Left, each attributed and pre-existing: wgq7_live client 2
`relay_late 2` (the anchor-tick frames RLT reclassified as covered; this
run's binary predates RLT's field), trl_live_B client 1 `relay_late 13` and
`resyncs 1 (bases [1530])` (the delayed rejoiner's first state, the
close-out's DRJ, F2). The rejoin lives also show the balloon module
(`PE_Balloon_*`, `PE_Box_slide` 6.7 mm above the platform; exception c) in
the `mech` column, in every late-joined world only, and trl_live_B client
1's life shows P_Modul_26_Sack / Rope falling after its resync refused the
server's script state (the TRL bug, fixed at 7913d90b: gone in
trl_live_B2). **Negative controls**: a copy of wgq7_live with client 1's own
ball moved +1 cm over 300..600 FAILS `own ball 0.0000 in B: 0.0100 m`; the
same in client 2's rejoin journal over 2000..2200 FAILS `own ball 0.0000 in
A: 0.0100 m` (`.local-tools/work/F0/doctor.py`, `rev_neg/`).
