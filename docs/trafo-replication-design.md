# Trafo replication: the explosion, the paper wind and the reset as lockstep inputs

Status: design (track 2 of the post-audit designs), revision 2 after two
reviews (§10); step T1 built (host support, bridge v21, SimTool; no wire:
§8, "As built (step T1)"); step T2 built (wire and server, protocol 11 /
journal v5, the world option off by default: §8, "As built (step TR2 =
T2)"); step T3 built and committed (core and headless host: §8, "As
built (step TR3 = T3)", its fix-up round, its merge round and "As built
(step TR3b)"; judged with snapshots off by the user's decision of
2026-10-25, §11: H4 and H6, the CPU gate with snapshots at interval 2,
moved to the rewind-cost optimization, `docs/roadmap.md` §4 item 10; the
server's `physics.trafo_inputs` stays off, past T4 until that
optimization's close-out). T3 waited for the rewind-cost step RC1
(`rewind-cost-design.md`, the hold-last re-prediction) by the user's
decision of 2026-10-25 and was re-gated at TR3b on top of RC1 and RL1
under its original H1 (c): one H1 pair at 150 ms, snapshots off (the
user's one-pair rule of 2026-10-27, §11), starved 0 / 0 / 0 / 0 against
the same-batch control's 0 / 0 / 0 / 0. Step T4 built (the retail host,
quick tier, not committed at the time of writing: §8, "As built (step
TR4 = T4)"; `physics.trafo_inputs` still off by default, adaptation M2).
T5 not. It implements the user's decision of 2026-10-23
on audit item #2 (and #11): option (b), the trafo becomes a replicated input
and its explosion, wind and reset run in every world for every player's trafo,
identically.

Base: HEAD 4882ec2 (R2) plus the uncommitted step P (the working tree, "WT").
Line numbers are the working tree's as of 2026-10-23. Files step P does not
touch have HEAD's numbers. (WT) marks code that exists only in the working
tree. Facts marked *measured* were taken for this document with
`build-agent/BallanceMMOServer/BallanceMMOSimTool.exe --root
C:/Users/geekerwan/Desktop/Ballance-Agent` (free runs, `--dump-script`,
`--explode`, `--beam`, `--track`, `--dump-array PH`) or read from existing
run logs and script dumps. The raw outputs are in the session scratch
directory `design/track2/`: script dumps `dump_*.txt`, awake-core series
`explode2_*.txt`, the PH proximity scripts `trafo_near.py` /
`balloon_near.py`, and the piece-reach measurement `reach/reach.py` with its
results `reach/all.tsv`. Gameplay_Ingame and Event_handler dumps are the
audit's (`audit/d_Gameplay_Ingame.txt`, `audit/d2_Event_handler.txt`).

Path key:

| key | file |
|---|---|
| PW | `BallanceMMOServer/sim/physics_world.cpp` |
| SR | `BallanceMMOServer/sim/session_runner.cpp` |
| SC | `BallanceMMOServer/sim/session_client.cpp` (headless host) |
| CH | `BallanceMMOClient/session/core_host.cpp` (retail host) |
| PSC | `BallanceMMOClient/session/physics_session_client.cpp` (legacy retail path) |
| CC / CF / CL | `BallanceMMOCommon/src/session/client_core{,_frame,_lifecycle}.cpp` |
| PS | `BallanceMMOCommon/src/physics/physics_state.cpp` (bridge) |
| SRP | `BallanceMMOCommon/include/game/script_replay.hpp` |
| SHS | `BallanceMMOCommon/include/game/session_host_support.hpp` |
| RB | `BallanceMMOCommon/include/session/rollback.hpp` |
| LC | `BallanceMMOCommon/include/session/lifecycle.hpp` |
| RSD | `docs/resync-and-sector-timing-design.md` (F3/F4, not built) |
| CK2 | `submodule/Ballanced/Source/CK2/src/` |
| PRT | `submodule/Ballanced/Source/BuildingBlocks/physics_RT/` |

Terms. A **set** is one member's copy of all three piece types and their
scripts (§2.4). A **type set** (p, T) is the part of p's set for piece type
T (`T` = 0 wood, 1 stone, 2 paper, as `ball_type`). The **ledger** is the
per-(p, T) record of accepted Trafo inputs (§2.2).

## 1. The problem and the rule

### 1.1 What runs where today

| | the trafoing player's own world | every other client | the server |
|---|---|---|---|
| explosion | Gameplay_Ingame's Trafo Manager activates `Ball_Explosion_<old type>` on the level's one piece set of that type | nothing: the mirror is re-made with the new type (CL:456-459), retail swaps spirit balls (`game_objects.h:96 on_trafo`) | nothing: the retail ball is parked (PW:342-346) and its Trafo Manager never fires; the clone gets the Unphysicalize and the Physicalize with the new type (PW:802-865) |
| paper wind | `Ball_PaperWind`: 18 SetPhysicsForce controllers (0.03 along (-1,1,0)) | nothing | nothing |
| reset | `Ball_ResetPieces_<type>` from the Fadeout Manager (20 s) or from the death chain | nothing | nothing |

### 1.2 Symptom

- The pieces are rigid bodies (17 stone, 16 wood, 18 paper) in the nocoll group
  `Ball`. IVP's group filter makes same-group objects pass through each other
  (PRT `ivp/ivp_physics/ivp_collision_filter.cxx:16-30`), and the BMMO player
  filter passes the player balls through `Ball` (PS:970-981). So a piece
  touches the level geometry and every movable mechanism or prop, nothing else.
- They live from the explosion to the reset: about 17.6 s after the explosion
  (the Fadeout timer starts at trafo detection, 156 ticks before the
  explosion, §1.5), or until the owner's next death. While they live, whatever
  they push is pushed in the owner's world only (audit #2). With snapshots off
  nothing repairs it. With snapshots on, the server's piece-free rows correct
  the owner again for as long as the contact lasts.
- The owner's world hash differs from the server's for the pieces' whole life
  (808-1041 in the R2 runs), so the hash is useless as a cross-world check
  around a trafo.
- **Where pieces go**, *measured* (`reach/reach.py`, `reach/all.tsv`): for each
  of the 191 distinct `P_Trafo_*` positions of L2-L13, a free run beams the
  level ball 1.3 m above the trafo at tick 600, explodes the original set of
  each type at 603 and tracks every piece for 1400 ticks, wind included. That
  is 573 runs. L1's 4 trafos are not measured: its ball is not physicalized
  by tick 600 or 1500 in a free run, so the beam fails.
  - Pieces that stay on the level reach at most 24.07 m from the explosion
    point (stone; wood 15.60 m, paper 16.78 m).
  - In 564 of the 573 runs at least one piece falls more than 20 m below the
    trafo, off an edge. A falling piece keeps its body until the reset and
    travels up to 2672 m in the 1400 ticks, past whatever level geometry,
    mechanism or other sector lies below.
  - The closest any piece came to `PE_Levelende` is 40.0 m (L2 s5
    `P_Trafo_Wood`, stone, a falling piece).
  - The free run has no sector's movables active and no player input, so these
    numbers describe the pieces' own reach, not a bound.
- Movables near a trafo in the same sector (*measured*, `trafo_near.py` over
  the PH dumps, distance < 20 m): L2 s3 Paper→Modul_25 19.5; L3 s1
  Wood→Modul_30 14.9; L4 s4 Paper→Modul_25 17.6; L6 s4 Wood→P_Ball_Stone 19.3,
  s5 Stone→Modul_29 11.0/11.6; L7 s1 Stone/Paper→Modul_29 16.0, s3
  Stone→Modul_30 14.9; L8 s4 Wood→Modul_30 14.9, s5 Stone→Modul_08 14.2,
  Paper→Modul_08 18.1; L9 s5 Paper→Modul_41 10.5/13.4; L10 six pairs 14.9-19.1;
  L11 s3 seven pairs 11.5-18.5; L12 s3 Wood→P_Ball_Stone 11.8, s7 Paper→P_Box
  9.7/12.4/16.5. With the falls above, this list is where pieces certainly
  reach a mechanism, not the only places they can.
- Audit #11 (the wind's controllers missing in a rollback) is the same scripts'
  problem one level down. Step P (WT) puts the wind in the owner's script
  replay, but only the owner's world has the wind at all.

### 1.3 Cause

The trafo is own-ball flow. The Trafo Manager, the Fadeout Manager and the death
chain all live in Gameplay_Ingame and act on `CurrentLevel[0][1]`, the one ball
of this world. The level has one piece set per type. The server's world has no
own ball (its retail ball is parked), and the clients' mirrors are bodies
without scripts. Nothing carries "player p exploded type T at tick t at
position x" or "player p's T pieces are reset at tick t" out of the owner's
world, so no other world can run it.

### 1.4 The rule and the invariant

The rule is the user's principle (a): every script that affects physics runs in
every world, driven by lockstep inputs. Applied here:

- **The trafo's explosion and its reset are inputs.** The owner stamps them like
  a sector entry (`hold_sector`, CF:142-153). The server accepts them through a
  per-(p, T) ledger that fixes their applied ticks (§2.2), and every world
  applies them at the applied tick.
- **The server closes every life.** No piece set stays live for want of a reset.
  The server stamps the resets no owner can send: a leaver's, and a deadline
  reset 20 s after any explosion its owner did not reset (§2.2).
- **Every player has its own piece sets.** Every world holds one set per member
  and runs that member's explosion, wind, collision-sound and reset scripts on
  it.
- **Pieces never touch local bodies** (exception (c)): one pair rule in the
  bridge's filter, the same in every world (§2.10).

**Invariant T.** For every member p, piece type T and tick t, two things hold:
p's T pieces have bodies in a world exactly when they have them in the
server's world, and their state after the step of t is bit-identical to the
server's. It is a function of the session's lockstep inputs alone, so it holds
in the owner's world, in every other client's world and in the server's world.
In particular:

- the owner's world hash equals the server's through a trafo when no ball is
  predicted;
- a mechanism a piece pushes is pushed in every world;
- pieces of different players never interact with each other: they share the
  nocoll group `Ball` (§1.2);
- no piece ever has a contact with a `PE_Balloon*` body (§2.10).

Principle (b) is not touched: an explosion changes no sector's state except
through contact, which is the same in every world. Exception (c) is kept by the
pair rule, not by distance. §1.2 shows that distance bounds nothing once a piece
falls.

### 1.5 The facts the design rests on (measured unless cited)

| fact | value / where |
|---|---|
| scripts (owner `All_Balls`, Balls.nmo) | `Ball_Explosion_{Wood,Stone,Paper}`, `Ball_ResetPieces_{Wood,Stone,Paper}`, `Ball_PaperWind`, `Ball_CollSound_Pieces`, `Balls_Init` (`dump_*.txt`) |
| explosion graph | `Set Position(0,0,0, Referential=Ball_Pos_Frame, Hierarchy)` on `Ball_<T>Pieces_Frame` → `Show` → group loop over `Ball_<T>_Pieces`: `Physicalize(Collision Surface=Get Name(Get Current Mesh), group "Ball")` → `Random` → `Physics Impulse(Position, Referential=element, Direction (0,0,1), Direction Ref=element)` → `Activate Script(Ball_CollSound_Pieces)`; paper: `Identity(TRUE)` into `W?ndOn?`, `Activate Script(Ball_PaperWind)`. Every link of the loop has delay 0: the whole explosion is one behaviour frame |
| draws | wood: impulse 1.5-3 (mass 0.2, friction 2); stone: impulse 4-9 (mass 0.8); paper: friction 1-5, mass 0.02-0.09, impulse 0.5-1.3. 5 hooked Random blocks (PS:2212-2235). Mass and friction are body properties a snapshot row does not carry |
| reset graph | `Fade <T> Pieces` (Bezier Progression, time based, 2000 ms, on the material `Ball_<T>_Pieces`) → `De Physicalize` (group loop, Unphysicalize, delay 0) → delay 1 → `Restore IC` (material) → `TT Restore IC` (frame, hierarchy); paper first `Identity(FALSE)` into `W?ndOn?` and `Activate Script(Ball_PaperWind)` (`dump_Ball_ResetPieces_Wood.txt:5-24`) |
| fade length | Bezier Progression adds the frame delta to its elapsed time on every execution, the first included, and fires Out when `elapsedTime > time` (`Logics/Behaviors/BezierProgression.cpp:149-168`). With Δ = 1000/66 = 15.151515f, the float sum is 1999.9976 after 132 executions and 2015.149 after 133 (computed). A reset first executed in frame R therefore runs De Physicalize in frame R+132, and Restore IC in R+133 |
| wind | `Ball_PaperWind`: `Binary Switch(Condition = W?ndOn?@Ball_Explosion_Paper)` → 18 chained `SetPhysicsForce`, Create (True) or Shutdown (False), force 0.03, direction (-1,1,0). The condition is a local parameter of **another** script |
| force block state | local 0 of a SetPhysicsForce block is the raw `PhysicsControllerForce*`. Its reset/delete/detach callback deletes the controller it points to (PRT `Behaviors/PhysicsForce.cpp:297-318`). The force hook's restore creates or deletes it (`:270-295`) and needs the target's body |
| Trafo Manager timing | detection (`Get Nearest In Group(Trafos)` < 4.3) → messages, delay 2 → `dephysic Ball` (Unphysicalize, `TT Set Dynamic Position` towards the trafo for 1350 ms) → `Delayer` 1000 ms → `start Explosion` (`Activate Script(BallParticle_Frame script)`, `Activate Script(Ball_Explosion_<old type>, Reset)`) → `Delayer` 150 ms → `set new Ball` (Set Cell `CurrentLevel[0][1]`, re-parents `Ball_Pos_Frame`) → `physicalize new Ball`. Observed: unphysicalize 652, pieces 808 (R2 `p_a_ctl`): 156 ticks |
| Fadeout Manager | one 20 000 ms Timer per type inside Gameplay_Ingame (`audit/d_Gameplay_Ingame.txt:519-621`), armed when `set Piecesflag` sets `<T>Pieses used?` (at detection, for the OLD type). At expiry: `Activate Script(Ball_ResetPieces_<T>, Reset)`. A second trafo of the same type while the timer runs fires the reset at once and re-arms |
| death chain | Gameplay_Ingame `Deactivate Ball` / `reset Ballpieces`. With lives left (`Test.out0(True)`), a Sequencer activates all three `Ball_ResetPieces_*`, then the 2 s screen fade, `Delayer` 1000 ms, and the Unphysicalize of the ball. Without lives, `Test.out1(False)` goes to `Game Over` and bypasses `reset Ballpieces` (`audit/d_Gameplay_Ingame.txt:1600-1760`, `:1633`) |
| level exit | Event_handler's `Exit Level` path sends a message and then, delay 2, runs `reset Ballpieces` (`audit/d2_Event_handler.txt:418-438`): a level restart or exit activates the original resets |
| awake piece cores (L2, free run, explosion at 300, baseline 4 cores) | wood: 16 → 3 still awake at 1700 (no reset); paper: 18 → 8 awake for good (the wind keeps them moving); stone: 17 → 0 after ~300 ticks (`explode2_*.txt`) |
| collision surface | the Collision Surface pin is the mesh's name (`Ball_Wood_piece16_Mesh`, `dump_Balls_Init.txt`). The hull is cached under it (PRT `CKIpionManager.cpp:510-525`) |
| hull drop | `RemoveCollisionSurface` deletes the surface manager and the compact surface (PRT `CKIpionManager.cpp:926-948`, `61-70`). With a shared mesh, dropping it while another body uses it is a use-after-free |
| activation | `Activate Script` skips a null script and still fires Out (`Narratives/Behaviors/ActivateScript.cpp:124`). With `Awake Object` it activates the script's owner in the scene (`:132`). `CKScene::ActivateScript` sets the script's next-frame flags, but hands the owner to the behaviour manager only when the owner is active in the scene (CK2 `CKScene.cpp:277-284`). It does nothing for a script that is not in the scene (`:281`) |
| execution order | the behaviour manager sorts active objects by priority only (CK2 `CKBehaviorManager.cpp:131-133`, `CKBeObject.cpp:964-970`). Ties follow insertion order through engine change #2's qsort |
| replayed scripts | `script_replay::execute_frame` applies the next-frame flags, then executes the selection in its order (SRP (WT):482-511). Restore demands that the record's script count equal the selection's (SRP (WT):450) |
| CK copy | in BUILD mode, copying a behaviour prepares its owner (CK2 `CKBehavior.cpp:2029-2034`), and the owner's class dependencies add every unlocked script of the owner (`CKBeObject.cpp:700-715`). A script whose owner is in the dependency set but not remapped loses its owner (`CKBehavior.cpp:2071-2079`) |
| IVP object name | the template name is the name the Physicalize call passes (PRT `CKIpionManager.cpp:1105`) |

## 2. The design

### 2.1 Overview

```
owner's world                                    every world (server, every client, the owner too)
 Trafo Manager / Fadeout / death chain / exit
   activates Ball_Explosion_<T> (original)  ─┐
   activates Ball_ResetPieces_<T> (original) ─┤ hold (after the live frame, from the anchor on):
                                              │ cancel, capture, stamp now+1+lead
                                              └──► Trafo event {kind, T, position}
                                                         │
                         server: trafo ledger per (p,T) ─┴─► applied tick A (+ server-stamped resets:
                                                              before a re-explosion, at the deadline,
                                                              for a leaver)
                                                         │
                         at A, before the step:  run_trafo(p, kind, T, position, seed(p, A, T))
                           - BMMO_TrafoRef_<T>_<p> := position
                           - flags on Ball_Explosion_<T>_Trafo<p> / Ball_ResetPieces_<T>_Trafo<p>
                         the trafo frame: every set's 8 scripts executed by the host, sets by player id
```

The level's own sets are never physicalized in a session. Each member has a
copy of all three types. The vanilla activations of the originals in the owner's
world become inputs, and the inputs drive the copies everywhere.

### 2.2 The input and the server's ledger

`event_type::Trafo = 5` (entity/session.hpp:74-80), a lifecycle event like the
others:

| field | Trafo use |
|---|---|
| `player` | the owner (0 from a client; the server fills it) |
| `tick` / `origin_tick` | from a client: the stamp, the owner's `tick + 1 + sector_lead`. The server fixes `applied` through the ledger below. A server-stamped event has `origin = applied` |
| `ball_type` | the piece type T (the type the ball WAS, for an explosion) |
| `flags` | bit 0 kind: `TRAFO_EXPLODE = 0`, `TRAFO_RESET = 1`; bit 1 `TRAFO_SERVER` (stamped by the server); bit 2 `TRAFO_FINAL` (a leaver's last input for this set, §2.4 "When sets go") |
| `position[3]` | explode: the world translation that the explosion's `Set Position` would have used (2.3). reset: zeros |

**The lead.** It is `SessionStart::sector_lead`, i.e.
`input_delay + 1 + input_delay_for_ping(worst_rtt)` (`session/timeline.hpp:79-81`).
The session already uses this one value for "an input stamped ahead so every
world gets it in time". The lp1_join run had input delay 17 and lead 35.

**Validation at acceptance.** It uses static level data only, so it does not
depend on which other events have arrived (review 1 S5). REJECTED goes to the
owner only (`EVENT_STATE_REJECTED`). The server checks:

- kind ∈ {0, 1}, T ∈ {0, 1, 2}, and the `TRAFO_SERVER` and `TRAFO_FINAL` bits
  clear;
- for an explosion: the position is finite and within `kTrafoReach` of the
  nearest `P_Trafo_*` entity of the level. The server reads the positions
  from its own world at the anchor.

The pull draws the old ball into the trafo: the detection radius is 4.3 m, and
`TT Set Dynamic Position` pulls for 1350 ms. `kTrafoReach` is provisionally
5 m. Step T2 sets it to the measured maximum × 1.5 from the existing logs
(9.3). The type and timing checks of revision 1 ("T is the ball's type before
the Unphysicalize, within 600 ticks of it") are dropped. They needed the
player's Unphysicalize and Physicalize to arrive first, which an outage does
not guarantee. A wrong T only explodes the owner's own set, the same in every
world.

**The ledger** (`trafo_acceptance` in `session/trafo_input.hpp`, CK-free, the
twin of `lifecycle_acceptance`, LC:384-500). The server keeps one entry per
(p, T):

- `live`: the newest accepted explosion {origin, applied} that no accepted
  reset follows, or none;
- `last_reset_applied`;
- `newest_explode_origin`.

Each Trafo event is taken on the simulation thread with the session's next
tick N, and three constants apply:

- `kTrafoResetSpan = 140`: a reset applied at R removes its bodies in R+132
  and restores the IC in R+133 (§1.5). 140 leaves a margin of 7 ticks.
- `kTrafoLife = 1320`: 20 s at 66 ticks/s.
- `L = sector_lead`.

| event | rule |
|---|---|
| explode, origin o | (1) If `live` exists (a second explosion of a type set that was never reset, e.g. after a lost or superseded reset), the server first queues its own reset of that type set: `TRAFO_RESET|TRAFO_SERVER` at `r = max(N, live.applied + 1)`, and sets `last_reset_applied = r`. (2) `applied = max(o, N, last_reset_applied + kTrafoResetSpan)`. (3) `live = {o, applied}`, `newest_explode_origin = o` |
| reset, origin o | If `o < newest_explode_origin`, it is **superseded**: never queued or relayed, and the owner is told SUPERSEDED. A reset made before an explosion that arrives after it has nothing left to reset: rule (1) already reset the earlier life. Otherwise `applied = max(o, N, live ? live.applied + 1 : 0)`, `live = none`, `last_reset_applied = applied` |
| deadline | after each server tick: for every `live` with `live.applied + kTrafoLife ≤ N + L`, queue a `TRAFO_RESET|TRAFO_SERVER` event at `max(N, live.applied + kTrafoLife)`, then `live = none` |
| leaver | §2.2 "Leavers" below |

What the rules guarantee, in every world, because every world applies the
server's applied ticks:

- An explosion and a reset of one type set are never in one tick, and a reset
  never comes before the explosion it resets.
- An explosion of a type set never lands while that type set's previous
  bodies exist: it lands at least 140 ticks after the reset, and after its
  bodies went in reset + 132.
- No type set has bodies for longer than `kTrafoLife` + 133 ticks after its
  explosion.

In normal play the rules change nothing:

- The owner's Fadeout reset has its origin about 1164 ticks after the
  explosion's (20 s from detection, detection 156 ticks before the
  explosion), which is ahead of the deadline.
- A same-type re-explosion comes 156 ticks after its reset (§2.9), and 156 ≥
  140.

The deadline fires when the owner's world does not reset in time:

- Game Over: the death chain bypasses `reset Ballpieces` (§1.5).
- The level-end Finish, or a pause that holds Gameplay_Ingame, if the
  Fadeout timer stops counting. Whether it does is not needed by this design,
  and S12 records it.
- An uplink outage longer than the 156-tick margin.

A late owner reset after the deadline is either a reset of nothing (accepted,
harmless) or older than a newer explosion (superseded). Pieces never live
longer in a session than vanilla's 20 s Fadeout. That is §6's one deliberate
difference from vanilla for the owner: pieces vanish 20 s after the explosion
on a Game Over.

**Leavers.** The leaver path (SR:340-385) queues events only under
`if (has_ball && queued)` (SR:349-358). A player that leaves without a ball
(between its dephysic and its new ball's Physicalize) would otherwise get
nothing. So for every leaver, whether or not it has a ball, the runner:

1. queues the leaver's Unphysicalize as today when it has a ball;
2. computes `F = lifecycle_leaver_removal_tick(s.events, player, next)`
   (LC:196-201) **after** that. F counts every queued event of the player,
   including an explosion accepted at E > next (`gone.applied` does not see it:
   `lifecycle_acceptance::accept` updates `newest_applied` only for Physicalize
   and Unphysicalize, LC:411-431);
3. puts every live type set through the ledger's reset rule at N = F. That is
   `applied = max(F, live.applied + 1)`, with origin = applied and the flags
   `TRAFO_RESET|TRAFO_SERVER|TRAFO_FINAL`. It also queues a `TRAFO_FINAL`
   reset at F for each type set with no `live`. The final reset tells every
   world that the set will get no more inputs, and a reset of a quiet type
   set is harmless (§2.7);
4. sets `leaving.remove_at` = 1 + the last of these, through the same
   function.

**Relation to the ball's life.** The ball's Unphysicalize (at the dephysic)
and its Physicalize with the new type (after "set new Ball") stay exactly as
they are, with the same stamps, own-life records and echoes. The Trafo event
is a separate input. It is not an own-ball life and is not in `own_lives_`.
The new ball now appears up to `lead - 10` ticks **before** the explosion.
Pieces never touch balls, so nothing depends on that order.

**Order inside a tick.** A Trafo event has rank 2 (LC:322-326, after
Physicalize). The ledger never puts two events of one type set into one tick,
so ordering events within a tick only orders flag writes on different type
sets. Each type set has its own referential (§2.4), so that order does not
change what any script reads. What runs the scripts is ordered by player id
(2.5).

### 2.3 The hold: the owner's own trafo scripts become inputs

New host-support function `bmmo::game::hold_trafo_activations(context)`, in
`game/trafo_sets.hpp` (2.11). It is called by the platform operation
`poll_trafo(tick)` in `post_step`, next to `poll_own_sector` (CF:591).

**When the hold is armed.** The hold runs from the anchor on, never before.
At the anchor, before the sets are built (§2.4), the host **quiesces the
originals**. This is the same function in every world:

- unphysicalize every original piece body through the bridge;
- delete the original wind's controllers through the force hook (`exists =
  false`; this NULLs local 0);
- clear the originals' next-frame flags;
- deactivate the 8 original scripts;
- run `restore_explosion_pieces`.

The retail session start restarts the level (`restarting` phase,
CH:718-722). That restart's `Exit Level` path activates the original resets
(§1.5), and an explosion made in single-player before the session may still
have bodies and a wind at the anchor. The quiesce takes all of that to the
state the server's and the headless worlds already have, so the owner's
world starts the session equal to the others. It is logged with what it
removed (`trafo quiesce: <n> bodies, <k> controllers, <s> scripts`). In the
server and headless worlds it removes nothing; a non-zero count there is a
bug.

**Each live frame from the anchor on:**

1. For each **original** `Ball_Explosion_<T>` and `Ball_ResetPieces_<T>` whose
   flags carry `CKBEHAVIOR_ACTIVATENEXTFRAME` after the live frame (set by any
   activator: the Trafo Manager, the Fadeout Manager, the death chain, the
   Event_handler's `reset Ballpieces`, the automation verbs `explode` /
   `activate`):
   - cancel the activation with `script->ModifyFlags(0,
     CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME)`. That is the
     public `CKBehavior` API, with no dependence on how a CK2 implementation's
     `CKScene::DeActivate` treats a script that is not running (review 1 N2).
     The behaviour manager then never starts it, and the owner `All_Balls`,
     which the activation may have queued (`CKScene.cpp:282-284`), executes
     nothing;
   - report `{kind, T}`.
2. For an explosion, also capture the position:
   - `Ball_Pos_Frame`'s parent's world translation if it has a parent, its own
     otherwise.
   - This is what the wrapped `Set Position` would have produced in the next
     frame: `restore_explosion_pieces` makes `Ball_Pos_Frame`'s local matrix the
     identity (PS:2341), and a zero position in a referential is that
     referential's translation, exactly.
   - Step P measured the alternative: with `Ball_Pos_Frame` on the new ball,
     the pieces start 1.16 m off (SHS (WT) comment at `manager_activations`).
   - Between the end of frame E0-1 (the hold) and the explosion's execution in
     frame E0, nothing moves the old ball entity: `TT Set Dynamic Position` was
     switched off 66 frames earlier, and "set new Ball" comes 150 ms later.
3. The core turns each report into an event through `hold_trafo`, the twin of
   `hold_sector`:
   - before the assignment, the report waits in order, and is stamped on the
     first assigned frame at that tick + lead, exactly as `request_sector`
     and `hold_pending_sector` do (CF:155-180). An anchor-relative stamp is
     never sent;
   - otherwise stamp `tick + 1 + sector_lead`;
   - `send_event`;
   - append it to the trafo input log as an own input (2.5);
   - log it.

**Effect checks** (every world, every tick, cheap): no body named
`Ball_<T>_piece<nn>` without the `_Trafo` suffix exists, and no original script
is active. A failure is logged with the activator's script, if known, and
counted (`trafo_original_leaks`). The gates require 0 (§7).

The server runs the same hold after every tick. It reports and cancels, sends
nothing (it has no own ball), and should never fire. A run that sees it fire has
found an own Trafo Manager acting on the parked ball, which is a bug.

Outside a session nothing is held. Single-player play, the menus and SimTool's
free-run `--explode` (design 9.10 traces) are unchanged.

**Cost of the hold**, deliberate and visible:

- The owner's explosion moves `sector_lead` ticks later than vanilla: 35 ticks
  (0.53 s) at input delay 17, after the new ball has appeared.
- The resets move by the same amount.
- Open question 9.5 is the way to remove the explosion's delay.

### 2.4 Per-player piece sets

**Contents of one member's set (id p).** Copies of:

- the three `Ball_<T>Pieces_Frame` hierarchies (3 frames, 51 pieces) with their
  meshes;
- the three groups and the three materials `Ball_<T>_Pieces`;
- `Ball_Explosion_<T>` ×3, `Ball_ResetPieces_<T>` ×3, `Ball_PaperWind`,
  `Ball_CollSound_Pieces`: 8 scripts.

Four objects are new rather than copied:

- `BMMO_TrafoOwner_<p>`, a 3D frame that owns the copied scripts;
- `BMMO_TrafoRef_<T>_<p>` for T ∈ {Wood, Stone, Paper}: three 3D frames, the
  referentials of the three explosions. With one referential per type set, two
  explosions of one player in one tick each read their own position. They do
  in two cases: a wood→stone chain re-stamped into one tick by an outage, and
  the reconcile of §4.7 (reviews 1 S1 and 2 #2).

**Naming.**

- Every copy is renamed explicitly to `<original name>_Trafo<p>`, e.g.
  `Ball_Wood_piece05_Trafo3920120238`, `Ball_Wood_piece05_Mesh_Trafo3920120238`,
  `Ball_PaperWind_Trafo3920120238`.
- The longest is `Ball_PaperPieces_Frame_Trafo4294967295`, 38 characters,
  inside `MAX_NAME`.
- Explicit renaming, rather than CK's append, keeps the names independent of
  CK2 implementation details.
- The `_Trafo` tag is chosen against the existing filters:
  - It must not contain `_BMMO_`. The server drops every body with that tag
    from snapshots and journals (PW:1120), and the headless wake filter treats
    it as a mirror (SC (WT):1057). `BMMO_Trafo...` objects have no leading
    underscore and no body.
  - It must not contain `_Peer_` (retail mirrors).
  - It keeps the `Ball_` prefix of the originals. Since step P the core tells
    balls by identity (`is_ball`), not by prefix.
- p is the server's player id. Every world knows it (relay, events,
  SessionStart). The join order is not used: the cores of existing members do
  not know a late joiner's join order (9.7).

**How the copy is made.** It is one `CKContext::CopyObjects` call per member,
over an explicit list: the scripts, frames, pieces, groups, materials and
meshes above. The copy:

- remaps every reference inside the list. Each set's references land on its own
  objects:
  - target frames, groups, materials and pieces;
  - the wind's 18 force targets;
  - the cross-script `W?ndOn?` link between the paper explosion, reset and wind;
  - CollSound's targets.
- shares every reference outside it: `Ball_Pos_Frame`, the sounds, the
  textures.

Then, in order:

0. **Extras out.** In BUILD mode the copy prepares a copied script's owner and,
   through the owner's class dependencies, every unlocked script of
   `All_Balls`, including `Balls_Init` (CK2 `CKBehavior.cpp:2029-2034`,
   `CKBeObject.cpp:700-715`; review 2 #7). The builder therefore compares the
   copy's result with the list, by count and class.
   - First, every listed script is detached from whatever owner the copy gave
     it.
   - Then every object that is not in the list (an `All_Balls` copy, its
     other scripts) is destroyed, without dependencies,
     before anything goes into the scene.
   - Invariant I0 records the result.
1. **The scripts' owner.** Each listed script is attached to
   `BMMO_TrafoOwner_<p>` (`AddScript`).
2. **The scene.** The owner and every copy are added to the level and the
   current scene. `CKScene::ActivateScript` ignores a script that is not in the
   scene, and the IC restore needs scene membership (PS:2241-2247). The owner
   is **not** activated in the scene, so the behaviour manager never runs its
   scripts on its own (2.5).
3. **No owner wake-ups.** The `Awake Object` setting of every copied `Activate
   Script` is set to FALSE. An explosion activating its wind or CollSound must
   not activate the owner (ActivateScript.cpp:132). With the owner inactive,
   such an activation only sets the target script's next-frame flags
   (`CKScene.cpp:277-290`), and the trafo frame executes it (2.5).
4. **The referentials.** Each copied explosion's `Referential` local, the
   source of `Set Position`'s referential pin
   (`dump_Ball_Explosion_Wood.txt`), is set to its own type's
   `BMMO_TrafoRef_<T>_<p>`.
5. **No borrowed controllers.** Every copied SetPhysicsForce block's local 0
   is set to NULL. The copy duplicates the raw controller pointer (§1.5). The
   quiesce makes the originals' pointers NULL anyway, but the builder does not
   rely on it (I7).
6. **Initial conditions.**
   - The copy is made from the originals right after the quiesce and
     `restore_explosion_pieces` (PS:2289-2343). So the frames are detached and
     squared, and the pieces sit at their exact IC.
   - The IC of every copied frame, piece and material is then saved in the
     scene, so the set's `TT Restore IC` and `Restore IC` restore it flat and
     opaque.
   - The builder also keeps the set's **built script state**: a v2 capture of
     its 8 scripts, taken right after the build. §4.5 and §4.7 restore it.
7. **The bridge.** The host's hooks (2.11) register the set:
   - patch its 5 Random blocks and 3 `Set Position` blocks (PS:2220-2287, now
     also matched with the `_Trafo` suffix);
   - add its 54 entities (3 frames, 51 pieces) to the body guard's exemption
     list (engine change #13, `m_KeepLevelBodiesFree`);
   - add its 51 piece names to the filter's piece registry (2.10).
   `restore_explosion_pieces` no longer clears the exemption list per
   explosion: it rebuilds it from the originals + every registered set.
8. **The hull cache.** Each set has its own meshes, hence its own collision
   surface names. The per-set restore drops only its own set's hulls, and only
   while that set has no body. No other body ever uses them, so the
   use-after-free of §1.5 cannot occur. Each set compiles its hulls at its first
   explosion from the exact flat hierarchy, so the stale game-start hulls of
   `Balls_Init` (engine change #9) never reach a copy.

**Builder invariants**, checked after every build. A failure is fatal for the
session (fail closed, logged with the offending object):

| | invariant |
|---|---|
| I0 | the objects the copy left after step 0 are exactly the list, by count and class per kind |
| I1 | every copied object and the owner are in the current scene; the owner is not active in it; every copied script's owner is `BMMO_TrafoOwner_<p>` |
| I2 | every object reference of every copied script, block and parameter (inputs' real sources, locals, outputs' destinations) resolves inside the set or to the allow-list {`BMMO_TrafoRef_<T>_<p>`, sounds, textures} |
| I3 | no copied `Activate Script` has `Awake Object` TRUE |
| I4 | names are exactly `<original>_Trafo<p>`; `GetObjectByNameAndParentClass` finds the copy |
| I5 | the bridge reports 5 Random and 3 Set Position blocks patched, 54 exempt entities and 51 registered pieces for the set |
| I6 | each copied piece's local matrix equals the original's bit for bit (same hull geometry in every world) |
| I7 | at build time, no original piece has a body, no original script is active or has next-frame flags, and every original and copied SetPhysicsForce local 0 is NULL |

The builder also logs a digest of the reference map (names, not ids), so that
the retail and headless logs can be compared (§7.4).

**When sets are made.**

- Server: at `add_player` (PW:683), and for a player that a join or resync
  ledger (4.7) names without a set. The server never needs the latter case.
- Every client:
  - at the anchor, after the quiesce, for every member in SessionStart and
    for the own player;
  - for a player first seen in a message (`on_event` / the relay) outside any
    replay, when the core finds no set for it. This covers late joiners, whom
    the existing members' cores learn of only through their events (9.7);
  - for every player a received ledger names (4.7). A leaver whose pieces are
    still fading at a join is in no SessionStart.

Building is CK only and touches no IVP state, so its timing changes no physics.
Two rules apply:

- a set always exists before its member's first Trafo input is applied (the
  first sight is the input's own arrival, so the set is built before the
  input is recorded);
- a set is never made inside a replay.

**When sets go** (review 2 #8). A set is retired when:

- its type sets have all received their `TRAFO_FINAL` reset;
- the set is inert: no body, no active script, no next-frame flags, no
  controller;
- the newest final reset is older than `history_ticks` (96, RB:353).

Retirement then has two stages:

1. **Deselect.** The set's scripts leave the trafo list, its pieces leave the
   filter registry, and its entities leave the exemption list.
2. **Destroy.** The set's objects are destroyed `history_ticks` ticks after
   the deselect, outside a replay.

Neither stage touches IVP or any selected script, so neither needs one tick
in every world. A record made before the deselect still lists the set's
script ids. Destroying only after those records have left the history keeps
every restorable record's ids alive (4.5). The server, which has no
rollback, destroys at the deselect. A player id seen again after its set was
retired gets a fresh set at first sight. Before the destroy, the retired set
is reselected instead.

At session end the host:

- unphysicalizes any live piece through the bridge;
- removes the sets from the guard list and the filter registry;
- destroys the objects.

A retail second session without a reload builds fresh sets.

### 2.5 Running an input: `run_trafo` and the trafo frame

`run_trafo(p, kind, T, position, seed)` is a platform operation backed by host
support:

- **explode:**
  1. `BMMO_TrafoRef_<T>_<p>` gets world matrix = identity rotation, translation =
     `position`. These are exact floats, the same in every world.
  2. The bridge's seed registry records `seed` for the frame
     `Ball_<T>Pieces_Frame_Trafo<p>` (2.6).
  3. `Ball_Explosion_<T>_Trafo<p>` gets `ACTIVATENEXTFRAME | RESETNEXTFRAME`
     (vanilla's `Reset ?` is TRUE).
- **reset:** `Ball_ResetPieces_<T>_Trafo<p>` gets the same two flags.

Nothing runs at that point. **The trafo frame** runs the sets' scripts:

- `script_replay::execute_frame` over the trafo list (the 8 scripts of every
  selected set, in ascending player id, the set's copied order inside);
- once per tick, after the tick's inputs and the sector union's pass, and
  before the CK frame's behaviour pass and step.

The 8 scripts include `Ball_CollSound_Pieces_Trafo<p>`. With its owner
inactive it has no other executor (review 2 #1). It plays sounds and creates
collision listeners, so it runs in every world: every player hears every
explosion. Its PreSimulate entries are §4.4's concern.

- **The host runs them because the behaviour manager would order them
  wrongly.** It orders active objects by priority, and ties by insertion order,
  which differs between worlds (late joiners, retail-only objects). Executing
  the sets outside the manager gives every world one order: player id, then
  the script order inside the set. Live frames and replays get the same order:
  a replayed tick runs the same function on the same list first (§4).
- The time-based blocks (the 2 s fade, CollSound's 3 s delayer) get the fixed
  `1000/66` ms delta (15.151515f), as the behaviour manager gives them under
  the session's fixed tick (collision-overhaul-design 3.1).

Where it runs in each world:

| world | inputs applied | trafo frame |
|---|---|---|
| server | SR's due-event loop → PW `apply_event` case Trafo → `run_trafo` (before `tick`, SR:902) | PW `tick`, after `update_sectors()` (PW:931), before `engine_->tick` |
| headless client | core `pre_step(T)`: `apply_trafo_inputs(T)` after `apply_held_lifecycle` (CF:325), before `sector_union_pass` (CF:330) | core `pre_step(T)` → platform `trafo_frame(T)` after the union pass; SC's step follows |
| retail client | the same core `pre_step(T + 1)`, which CH calls at the end of OnProcess(T), after `post_step(T)` (CH:947-952) | the same core call, inside that `pre_step(T + 1)`. The next `CKContext::Process` runs T + 1's behaviour pass and step after it |
| a replayed tick (both clients) | the core's `before_replayed_tick` re-applies the tick's inputs from the trafo input log | `execute_frame`: the trafo list first, then the selection (§4) |

The server applies inputs before `update_sectors`, and the clients apply them
before their union pass. Both run the trafo frame after the pass's flag-setting
and before the behaviour pass. So every world has the same order inside a
frame: inputs, sector pass, trafo scripts, level scripts (the sector manager
included), step.

**The trafo input log** (`trafo_input_log` in `session/trafo_input.hpp`,
CK-free) holds every applied or pending Trafo input by applied tick: own, remote
and server-stamped. It is used in three ways:

- the core applies the live tick's entries through `run_trafo`;
- `before_replayed_tick` re-applies a replayed tick's entries;
- it answers "the newest explosion of (p, T) at or before tick t" for §4.7.

SimTool's `--savestate-check` uses it without the core (§7.2 S6). Entries
older than the history are dropped, except each type set's newest explosion
and reset.

### 2.6 Random seeding per event

- Seed: `trafo_seed(session seed, p, applied tick, T)`, an integer-only mix
  (FNV-1a over the four values, folded to a nonzero int32) in
  `session/trafo_input.hpp`, CK-free like `spawn_direction_index`.
- The wrapped `Set Position` (`explosion_set_position`, PS:2260-2263) runs
  first in every explosion. When the registry has a seed for its target frame,
  it calls `random_reset(seed)` after the per-set restore. The originals have
  no entry and keep today's cursor (9.10 traces, legacy path).
- An explosion draws all its values inside its own execution in one frame
  (§1.5). Each draw sequence is therefore a pure function of (seed, set). It is
  independent of other explosions in the same tick, of the cursor's history,
  of whether the blob restores or keeps the cursor, and of the world.
- The applied tick, not the origin, goes into the seed, as with the spawn
  impulse (SR:894-899). The server and every receiver use it. An owner whose
  own event was MOVED re-runs the explosion at the applied tick anyway (§4.6).

### 2.7 The reset per player

- **The 20 s timer** runs only in the owner's world (Fadeout Manager, live
  Gameplay_Ingame). Its expiry reaches every world as a reset input (2.3). Every
  world resets at the applied tick the server fixed, so they agree whatever
  the owner's timer did.
- **"At the next death"**: the owner's death chain activates all three
  originals, which gives three held reset inputs (one per T). A reset on a
  type set with no bodies is harmless and the same everywhere: fade, De
  Physicalize of nothing, Restore IC, and for paper the wind Shutdown of NULL
  controllers.
- **Game Over, Finish, pause, outage**: the deadline reset (2.2).
- **A leaver**: server-stamped final resets (2.2).
- **The fade and the De Physicalize** run in each world's copy of the reset
  script, driven by the trafo frame. In a reset applied at R, the bodies go in
  frame R+132's trafo frame (their last step is R+131), and Restore IC runs at
  R+133 (§1.5) everywhere.

### 2.8 The paper wind per player

- Each set's `Ball_PaperWind_Trafo<p>` is activated by its own paper explosion
  (Create) and its own paper reset (Shutdown). The `W?ndOn?` local belongs to
  the set's copied explosion, so the link stays inside the set (I2).
- Its 18 controllers are named `rt:force/<block id>` (engine change #18). The
  ids differ per set, so no two sets' controllers share a name.

### 2.9 Several trafos in one lifetime

- **Two players within 20 s:** separate sets, separate scripts, separate seeds.
  The pieces pass through each other (one nocoll group). The only coupling is a
  mechanism both sets touch. That is ordinary shared physics, and the trafo
  frame's id order fixes their creation order.
- **The same player, same type, twice within 20 s:** vanilla's Fadeout fires
  the reset at the second detection D2.
  - The reset is stamped D2 + 1 + lead and De Physicalizes at D2 + 133 + lead.
  - The second explosion is stamped about D2 + 157 + lead.
  - The ledger requires 140 ticks between them and finds 156, so nothing
    moves. The old pieces are gone 23 ticks before the new ones appear, in
    every world.
  - Under an outage that delivers both together, the ledger moves the
    explosion to reset + 140, and the owner re-runs it there on the MOVED echo
    (§4.6).
- **A chain of types** (wood → stone → paper within 20 s): three type sets of
  one player alive at once (51 bodies), independent, each with its own
  referential.

### 2.10 The collision filter

The bridge's filter (PS:950-981, design 8.2) keeps its rules, so pieces:

- never collide with player balls in any world (own ball, mirrors, server
  clones, all `P#<n>`);
- never collide with each other (group `Ball`);
- collide with the level and with the mechanisms, which have no group.

One rule is added, only while `WORLD_TRAFO_INPUTS` is on (2.14): **a registered
piece never collides with a local body**.

- A piece is an IVP object whose name (the entity name the Physicalize passes,
  PRT `CKIpionManager.cpp:1105`) is in the filter's piece registry (2.4
  step 7).
- A local body is one whose name matches the resync design's `keep_local`
  predicate (`PE_Balloon` prefix, RSD §2.6 step 4, I3 there). It is one core
  rule for exception (c), shared by both designs.

The filter is asked when a pair first becomes a collision candidate, so the
rule is static. It acts only on piece × balloon pairs. That pair class is
exactly where the worlds would diverge, since the balloon's state is local to
each player. No other pair's result changes. It is a bridge change (API v20),
not an engine change. It replaces revision 1's distance warning (review 2 #9).
§1.2 shows a falling piece can travel kilometres, so no distance argument
holds.

Audit F2 (a), filtering pieces against movables, is **not** applied: the user
chose faithful pieces.

### 2.11 Where the code lives (one implementation)

| piece | place | used by |
|---|---|---|
| event payload, flags, key, rank, `trafo_seed`, `trafo_acceptance` (ledger, deadline, leaver resets), `trafo_input_log`, validation rule | `session/trafo_input.hpp` (new, CK-free), `lifecycle.hpp`, `entity/session.hpp`, `message/session_event_msg.hpp` | server, both cores, SimTool, tests |
| stamping, the own inputs, late/moved/rejected/superseded handling, the late-input rewind, reconcile at resync and join, journal records | the core: `hold_trafo` (CF), `apply_trafo_inputs` / echo branch / `trafo_rewind` (CL), `client_core.hpp` state | both hosts |
| quiesce, set builder, invariants I0-I7, hold of the originals, effect checks, `run_trafo`, the trafo list, set unmake/remake for reconcile, retirement | `game/trafo_sets.hpp` (new; public CK2 API only, like `sector_activation.hpp`); its registry is a value type (`trafo_registry`) | PW, SC, CH, SimTool |
| per-set restore, seed registry, set registration (patches, guard list, filter registry), the piece × local-body filter rule | bridge (PS, `physics_rt_api.h`, API v20). Reached through a hooks struct (`trafo_set_hooks`, like `script_replay_hooks`), because the retail mod does not link `src/physics` | the three hosts supply the hooks: PW/SC with `bmmo::physics::*`, CH with `physics_view` |
| platform operations `poll_trafo`, `make_trafo_set`, `run_trafo`, `trafo_frame`, `trafo_reconcile`, `retire_trafo_sets`; capability `caps.trafo_inputs` | `client_platform.hpp` | SC, CH (PSC lacks it, 2.14) |
| retail state | one file-scope static `trafo_host` in CH holding the `trafo_registry`, the cached ids and the trafo list. `BallanceMMOClient` gets no data member (CH:33-39) | CH |
| server side | PW (`add_player`, `apply_event`, `tick`, anchor quiesce, ledger state for joins/resyncs), SR (acceptance through the ledger, validation, deadline, leaver resets) | — |

No engine change. IVP is not touched. The bridge additions change no existing
simulation result: originals, free runs and sessions without the world option
take today's path.

### 2.12 World hash, snapshots, journals

- **World hash.** The hash covers every movable core
  (PS `capture_world_hash`). Every world now has the same pieces, so the
  808-1041 mismatch class disappears. The hash's "probe" still picks the first
  `Ball_` name (PS:226-228). It is diagnostic, and changing it would change
  every TICK record, so it is left as step P left it.
- **Snapshots.** Pieces are mechanism rows by name. The server numbers them in
  `body_index_` (PW:1080-1140). An explosion or reset changes the body set, so
  a full snapshot follows (`body_set_changed_`). Delta snapshots carry the
  awake ones (§1.5: up to 8 per paper explosion for the whole life).
- **Journals.** Server checkpoints carry every set's pieces. Client LOCAL
  checkpoints carry them as mechanisms (step P's `is_ball` rule).
  `worst_bodies.py`, `first_divergence.py` and `savestate_check.py` compare by
  name, and the names are equal in every world.

### 2.13 CPU and memory

- **Bodies.** Per explosion 16-18 bodies, all awake for about 300 ticks. After
  that (*measured*, §1.5): stone 0, wood 3, paper 8 awake until the reset, at
  most `kTrafoLife` + 133 ticks.
- **Where the cost lands.** Every world now pays for every player's trafo.
  Four players' paper trafos means 32 bodies awake for up to 20 s, in the
  server's world and in each client's, and inside every rollback window that
  covers them.
- **The script records.** The per-tick record grows by 8 scripts per selected
  set; the paper wind's 18 force blocks go through `capture_force`.
- **Memory.** About 125 CK objects per set, including 51 small meshes. Sets of
  leavers are retired (2.4), so the count is bounded by the members present
  plus those retired within the last ~2 × 96 ticks.
- **Gate.** The deployed snapshot interval 2 is at the CPU edge, so CPU is a gate
  (§7.3 H6), not a remark.  Since the user's decision of 2026-10-25 (§11)
  that gate (H4, H6) belongs to its own step, the rewind-cost optimization
  (`docs/roadmap.md` §4 item 10), with T3's snapshots-off CPU numbers as
  its baseline; T3 is judged with snapshots off.
- **If capture dominates.** Open question 9.4 has a compact encoding for
  dormant scripts.

### 2.14 Sessions without the capability (the legacy retail path)

PSC (session core off) cannot build sets or apply Trafo events, and the user's
decision keeps it untracked until R7 retires it. The sessions keep it working,
and keep the core-off BAR comparisons possible:

- **The capability.** `session_ready_msg::caps` gets
  `CAP_TRAFO_INPUTS = 1u << 1`. The core sets it; PSC does not.
- **The world option.** At session start the server sets
  `SessionStart::world_options` bit `WORLD_TRAFO_INPUTS` only when every member
  has the capability.
  - With the bit off, every world behaves as at HEAD + P: no sets, no hold, no
    quiesce, no filter rule, owner-only pieces. The server rejects Trafo
    events.
  - With it on, a late joiner without the capability is refused with a reason.

The off mode keeps owner-only physics in core-off sessions. That is the
recorded exception of the user's PSC decision, gone with R7.

## 3. Determinism argument

1. **Same sets.** Every world builds p's set from the same level file:
   - The objects are copied right after the same quiesce and exact restore, so
     piece matrices are bit-equal (I6, I7).
   - The meshes are copied, so the hull inputs are equal.
   - Names are explicit (I4). The copy holds exactly the list (I0).
   - CK ids differ between worlds and are used nowhere in physics. They only
     reorder `m_PhysicsObjects`, which no hash or simulation path depends on
     (audit2 §4 B).
2. **Same inputs.**
   - The server's ledger fixes a Trafo event's applied tick once, at
     acceptance, and relays it with the payload. The server-stamped resets are
     ordinary relayed events with `origin = applied`. Every world applies each
     at its applied tick before the step.
   - The owner applies its own event at the origin. It moves it on a MOVED
     echo and drops it on REJECTED or SUPERSEDED (§4.6).
   - The ledger decides in arrival order at the server only. Receivers never
     re-decide: they apply what was relayed.
3. **Same execution.**
   - Inputs only set flags and one referential each.
   - The trafo frame executes the sets in player-id order, at one fixed point
     of the tick in every world (2.5), with the fixed delta.
   - Inside a set, the scripts keep their copied order.
   - The draws depend on (seed, set) only (2.6).
   - Each type set's referential is set from its event's exact floats.
   - The ledger keeps an explosion and a reset of one type set out of one tick
     and out of each other's span (2.2).
4. **Same physics.**
   - The bridge, IVP and physics_RT are already bit-exact across the server
     builds and the retail client (collision-overhaul-design 6, 9.10-9.14).
   - The explosion's three exactness fixes (piece frames squared, engine changes
     #8 and #9) carry over, because the copy starts from the restored
     hierarchy.
   - The new hazard, a hull shared between sets, is removed by per-set meshes
     (2.4 step 8).
   - The filter rule is a static pair rule, the same in every world (2.10).
5. **Late joiners.**
   - A joiner builds sets at its anchor for every member in SessionStart, and
     for every player its join ledger names.
   - Members it learns of later get sets at their first message, before any of
     their inputs can apply.
   - Live pieces at the join come from the reconcile (§4.7): the explosion is
     re-run with the event's seed, so mass and friction match, and the rows
     and the server's script state are put over it.
6. **Resyncs.** A resync is a re-step from the server's tick S (RSD §2.6). At S
   the reconcile makes the body set of every type set equal to the server's
   ledger. That set is the one piece of state the rows and the portable script
   record cannot create (§4.7).
7. **Out-of-order sector passes** are irrelevant. A set is not tied to a sector.
   Its position comes from the event, its existence from the inputs.
8. **Order of a tick's work** is the same in every world: inputs → union pass →
   trafo frame → behaviours → step (2.5). The server's event loop order (join
   rank, origin, rank) and the clients' drain order only order flag writes on
   distinct type sets. So a client that ranks a late joiner at 256 (9.7) still
   executes the same frame.

## 4. Rollback interplay

The base is step P (WT), option (i). In the WT:

- the trafo scripts are in the script replay's selection (`is_trafo_script`,
  SRP (WT):214-218);
- the pieces are mechanisms (`is_ball`, CC (WT));
- window-born pieces are destroyed at the restore and re-made by the replayed
  explosion (the sector births' path);
- pieces destroyed inside the window are revived at the restore (`revive_body`,
  RB (WT):242, 850-875; `session_host_world.hpp` (WT):348-357);
- the Random cursor goes back with the blob.

This design keeps all of that and changes what triggers the scripts and which
scripts are selected.

### 4.1 The body plan

- Pieces are mechanisms of their names. The record's `tracked.mechanisms` holds
  every set's live pieces (CF (WT):560).
- A piece is a ball for no one, so `destroy_body` / `insert_birth` /
  `revive_body` treat it as a mechanism (`hooks.ball`, `session_host_world.hpp`
  (WT)).

### 4.2 Births inside a window

- An explosion at A inside [anchor, now]:
  - the restore destroys the type set's window-born pieces;
  - the replayed tick A re-applies the input from the trafo input log (2.5),
    which sets the flags, the referential and the seed;
  - `execute_frame` runs the copied explosion, which physicalizes them again;
  - the birth check compares them with their recipe records.

  This is the path measured exact for sector births (53/53, arena).
- A reset's De Physicalize inside the window is revived at the restore and
  removed again by the replayed reset at the same tick.

### 4.3 What step P's mechanism is replaced by

| step P (WT) | here |
|---|---|
| `selection::activations` + `record_pending` record the trafo scripts' pending activations after each live frame, and `replay` re-issues them (SHS (WT):226-310) | removed for trafo scripts. The activation is an input in the trafo input log, re-applied in replays by the core. The list stays for the sector manager |
| `referential_by_tick_` and `end_override` re-parent `Ball_Pos_Frame` during a replayed explosion | removed. The per-type-set referential is set by `run_trafo` from the event, live and replayed alike |
| `explosion_pieces()` names the three original hierarchies for `reportable_wakeup` | names every set's pieces (and the originals). Still no BodyRevived for pieces, now because every world runs their scripts |
| `is_trafo_script` selects the originals by name prefix, and `Ball_PaperWind` by exact name | the originals are not selected (they never run). The copies are selected only through the registry's `trafo` list (4.5), not by name: `is_trafo_script`'s prefixes would also match the copies' explosions and resets, while its `strcmp` would miss `Ball_PaperWind_Trafo<p>` (review 1 N3, review 2 #12). `select()` loses the `trafo` argument in the core's selection. The legacy retail selection is unchanged |
| Random cursor restored by the blob | kept; irrelevant to the result (2.6) |

### 4.4 Savestate naming and the PreSimulate queue

- The blob keys bodies by name within one process. Names are unique per set.
- Controllers are matched by (priority, `rt:force/<block id>`). Each set's wind
  blocks have their own ids.
- **The PreSimulate queue.** Each set's CollSound runs in the trafo frame
  (2.5). Its `PhysicsCollDetection` Create on a piece without a body queues a
  callback that is retried every frame (PRT
  `Behaviors/PhysicsCollisionDetection.cpp:262-276`). CollSound's 3 s delayer
  loop can issue that Create after the reset's De Physicalize.
  `check_callbacks` compares every non-BMMO queue entry with the blob's and
  refuses the blob on any difference
  (`physics_savestate_restore.cpp:1760-1818`). An entry created or removed
  inside a window would therefore refuse rollbacks. S8 measures whether it
  happens. The decision tree is 9.2.

### 4.5 The script replay selection and the records

- **The selection.** It becomes `session_script_selection` = the mechanism
  scripts + the sector manager + the registry's `trafo` list: every selected
  set's 8 scripts (explosions, resets, wind, CollSound), in player-id order and
  copied order.
- **The record.** It grows when a member's set is made, which today's restore
  refuses (`count != list->size()`, SRP:450). Record v3 records the `trafo`
  list keyed by script id:
  - A selected script that is absent from a record (its set was made after
    that record) is restored to its set's **built script state** (2.4 step
    6). Leaving it as it is would keep whatever it has done since. Its pieces
    are not in that anchor's mechanisms, so the body plan destroys any it has
    (window-born births).
  - A recorded id that no longer exists fails the restore, as today.
    Retirement destroys a set only after every record listing it has left the
    history (2.4), so a correct session never hits this.
  - A recorded set that has been deselected but not yet destroyed is restored
    (it is inert by the retirement rule, so the restore is a no-op).
- **Replay execution.** `execute_frame` runs the `trafo` list first, then the
  satellites, then the scripts, matching the live frame (2.5: the trafo frame
  runs before the engine's whole behaviour pass, Gameplay_Ingame's satellites
  included). (Corrected at T1's review: this line said "satellites, then
  trafo", which contradicted 2.5.)
- **Stale exemptions.** `kept_live` / `reset_skips` do not apply to the sets:
  nothing outside the replay activates them.

### 4.6 The own input in rollbacks and echoes

- The own Trafo input is in the trafo input log from the moment it is stamped.
  Replays re-apply it at its tick. Gameplay_Ingame is not replayed, so the
  originals are never re-activated in a replay, and the hold never runs inside
  one (it is a `post_step` of a live frame).
- **Echo, applied == origin:** acknowledged.
- **Echo MOVED** (an outage, or the ledger's span rule):
  - re-key the input to the applied tick;
  - if the origin is not later than the current tick, mark a trafo rewind to
    `min(origin, applied)` (4.7). The explosion made at the origin is undone,
    and made again at the applied tick with the applied tick's seed.
- **Echo REJECTED or SUPERSEDED:**
  - drop the input;
  - mark a trafo rewind to `origin` if it was applied.
- **Server-stamped resets of the own player** (`TRAFO_SERVER`) arrive as
  relayed events and are remote inputs to the owner, like any other player's.
- **Which switches gate this.** Trafo echoes and late Trafo inputs go through
  the input-rewind path (4.7), gated by `config.rollback && config.input_rewind`.
  They do not go through the `own_echoes` / `lifecycle_rewind` paths (review
  1 B1):
  - The retail host has `lifecycle_rewind` and `own_echoes` off until R3
    (CH:597-598 in the WT; session-client-core-design R3, not built). The user's order
    puts this track before R3 (audit §5).
  - Those switches exist because retail has no own-body control, and a mirror
    birth from a row stands in for the lifecycle rewind. A Trafo input needs
    neither. It only sets flags in a replay, as a relayed input frame does,
    and `input_rewind` is on in both clients since R2 (CH:596).
  - A Physicalize echo's own-body handling stays where it is (CL:1039ff).
  - Own Sector echoes keep "kept where we stamped it" (CL:1111); that is F4's.

### 4.7 Late inputs, missed inputs, joins and resyncs

**A late Trafo input** has an applied tick ≤ the current tick. It may be
remote, server-stamped, or an own input moved earlier. Its handling:

- It goes into the trafo input log, and `trafo_rewind(applied)` joins the
  frame's pending input rewind. That is `s.input_rewind_tick_` = the earliest
  tick, as a relayed input correction does, and `replay_corrected_inputs`
  (CF:32-110) runs one replay for all of the frame's corrections.
- It is never applied at the live tick, so it never repeats audit #5.

**A missed input** has an applied tick outside the history (`knows_tick`
false), or a replay deeper than `max_resim_ticks` (48, RB:354) that
`rewind_to` refuses. The core calls `request_resync("a trafo input could not
be replayed")`, as CF:104 does for an own frame (review 1 B2). A snapshot
cannot repair it: rows cannot create pieces, start a fade or switch a wind,
and a reset missed this way would leave the pieces and the wind in that world
for good. The resync below does repair it. The miss is counted
(`trafo_missed`) and logged with the event.

**The ledger in the join and resync state.** F3's resync state (RSD §2.3)
and the joiner's first state (RSD A5) gain `trafo_ledger`, taken by the server
at S. It has one entry per type set that has bodies at S, or a live
explosion, or a reset not yet past its Restore IC:

```
{ player, T, explode_applied, position[3], has_bodies, reset_applied (or none) }
```

The seed follows from (session seed, player, explode_applied, T).

**The reconcile** (`trafo_reconcile`, a platform operation over
`game/trafo_sets.hpp`, called by the core) is a resync's step 6 addition. It
runs inside `s.rollback_.resync(...)` (RSD §2.6 step 6, §4.1), after the
synthetic anchor's world is restored and before the rows and the portable
scripts are written. Before the resync call, outside the replay, the core
builds a set for every player the ledger names that has none. For each type
set (p, T) in either the client's world or the ledger:

1. **Compare generations.** The client's generation at S is (the newest
   explosion of (p, T) applied ≤ S, from the trafo input log; and whether the
   restored world has any of its pieces). It is compared with the ledger's
   (`explode_applied`, `has_bodies`). If they are equal, nothing is done here,
   and the rows and the server's script state correct pose, velocity and
   script phase.
2. **Unmake.** If they differ, the type set goes back to its built state:
   - its pieces are unphysicalized through the bridge's per-set restore;
   - for paper, its wind controllers are deleted through the force hook;
   - its reset and explosion scripts (and the wind and CollSound for paper)
     are put to their built script state (2.4 step 6).
   This is the order review 1 B3 asks for: a re-run never physicalizes over
   existing bodies or creates a second wind.
3. **Remake.** If the ledger has bodies:
   - `run_trafo(explode)` with the ledger's position and seed;
   - that type set's explosion script alone is executed once by
     `execute_frame`.
   The pieces are created with the event's draws (mass and friction, which no
   row carries), and the impulse it queues is overwritten by the row (rows
   write zero queued pushes).
4. Then, as RSD specifies, the rows are written over every body and the
   portable script record over every selected script. The record is name-keyed
   across worlds (RSD §2.4 A1), and the set copies' names are equal in every
   world. This sets the explosion's and reset's progress, `W?ndOn?`, the
   CollSound state and the wind's controllers (the force hook creates them,
   now that the bodies exist).

The reconcile also takes care of:

- a set live locally but absent from the ledger (a reset this client missed):
  unmade;
- a leaver still fading at a join: its set is built from the ledger and
  remade;
- a ledger entry with bodies whose explosion this client never received:
  remade.

The replay from S then re-applies the inputs logged for (S, B]. That includes
any late one that arrived with or before the state, since the server relays
every accepted event reliably.

**Joins** take the same path (RSD A5). The joiner's synthetic anchor is its
fresh world after the quiesce and the set builds, so step 2 finds nothing to
unmake.

**Before F3** none of this exists. That is why F3 is a prerequisite (§8 T0).

**The residual class.** Remade pieces are fresh IVP objects. A remade piece
resting on contacts the rows cannot carry is in the general "rows-only"
residual of every body a resync or join creates (late-player design;
RSD §4.1). The pieces add bodies to it, not a new kind. H3b and H7 measure it,
and a non-zero residual on pieces is reported against that class with its
numbers.

### 4.8 Windows that span a set's creation or retirement

- Sets are made outside replays.
- A restore to an anchor before a set existed restores the set's scripts to
  their built state (4.5). Its pieces have no record there and are not in the
  anchor's mechanisms, so the plan destroys any as window-born.
- A set's first input can be inside a window that reaches back before the set
  existed (a late joiner's first trafo, relayed late). The set is built at
  the input's arrival, outside the replay, and the replay then applies the
  input at its tick on the set's built state. That is the state it had at
  every tick before its build.
- A retired set is destroyed only after no record lists it (2.4).

## 5. Wire, protocol and journal

| item | change |
|---|---|
| `PHYSICS_PROTOCOL_VERSION` | the next free number after F3 (RSD §5 takes 10) and pooled-module option B (audit2 §4 B); 9 at HEAD (entity/session.hpp:51) |
| `event_type` | `Trafo = 5`; `event_type_name` "trafo" (LC:41) |
| `session_event_msg` | Trafo serializes `ball_type`, `flags`, `position[3]` (the existing fields; the per-type switch gains a branch) |
| `lifecycle_event_key` | gains `flags` (Trafo only; 0 otherwise). An explode and a reset of one type at one origin are two events, and a server-stamped event (`origin = applied`) never collides with an owner's (LC:264-292) |
| `lifecycle_event_rank` | Trafo → 2 (with Sector, Finish, BodyRevived) |
| event state | SUPERSEDED (existing bit) for a reset older than a newer explosion; REJECTED for validation |
| `session_ready_msg::caps` | `CAP_TRAFO_INPUTS = 1u << 1` |
| `session_start_msg` | `world_options` byte after `spawn_spacing`; bit `WORLD_TRAFO_INPUTS` (shared with option B's world-option bit) |
| F3 resync state, joiner's first state | `trafo_ledger` list (4.7) beside F3's union state and portable scripts |
| server acceptance | `trafo_acceptance` (2.2): span, superseded resets, inserted resets, deadline, leaver final resets; validation → REJECTED |
| server journal | version 3 → 4 (`journal.hpp`): the header carries `world_options`; EVENT records of type 5 use the existing `ball_type`/`flags`/`position` fields (journal.hpp:578-601), with no layout change. Server-stamped resets are EVENT records like the leaver's Unphysicalize. A v3 reader refuses a v4 file (journal.hpp:784-787) |
| client journal | own sent, received and server-stamped Trafo events recorded like other events; `trafo_missed`, `trafo_original_leaks`, the reconcile's actions in the RESYNC record; LOCAL checkpoints carry every set's pieces as mechanisms |
| bridge | API v19 → v20 (`physics_rt_api.h:23`): `trafo_set_register`, `trafo_set_seed`, `trafo_set_unregister`, `trafo_set_unmake`; the filter's piece registry and the local-body rule; `restore_explosion_pieces` rebuilds the exemption list from the registry |
| SimTool | `--replay-session` honours `world_options`, applies Trafo events through `physics_world::apply_event` and runs the same `trafo_acceptance` decisions from the journal's records (no new code path) |

## 6. What must not change

- **Sessions without trafos** keep every body, hash and record of HEAD + P. The
  only differences are the version, caps and world-option bytes, and the
  quiesce log line (with zero counts). Sets exist but have no bodies, and the
  trafo frame executes nothing active.
- **The ball's own trafo life:**
  - Unphysicalize at the dephysic and Physicalize with the new type, with
    today's stamps, echoes, own-life records, spawn rules and validation;
  - the new ball's timing;
  - the Trafo Manager's other effects (navigation messages, `CurrentLevel[0][1]`,
    `Ball_Pos_Frame` re-parent, the trafo animation, particles).
- **The collision filter's existing rules** (PS:950-981) and the nocoll groups.
  The added rule acts only on piece × `PE_Balloon*` pairs, only with the world
  option on.
- **The originals:**
  - never run in a session, from the anchor on;
  - outside a session, the vanilla scripts, the wrapper and the Random cursor
    behave as today;
  - `rec_m3b.bmrc` 4169/4169; the 9.10 `--explode` traces on x64/x86/Linux; the
    9.14 guarded/unguarded two-explosion identity.
- **Sector machinery:** N1, N2, N3 and N4 holds, the sector union, the pacer, the
  9.28 pins, and the manager's activation records.
- **Engine and IVP:** no engine change, no IVP change.
- **Core-off sessions** (world option off): HEAD + P behaviour, the BAR.
- **Exception (c):** the PE_Balloon structure stays local. No piece can touch it
  in any world (2.10). The level-end balloon and airship's own behaviour is
  untouched.
- **The deliberate differences from vanilla**, all visible in the owner's own
  world:
  - the explosion and resets move by `sector_lead` (2.3);
  - pieces never outlive the explosion by more than `kTrafoLife` + 133 ticks,
    including after a Game Over (2.2);
  - a re-explosion squeezed by an outage moves to its reset + 140 (2.2);
  - pre-session pieces are removed at the anchor (2.3);
  - no piece-balloon contact (2.10).

## 7. Verification

The pass criteria are exact unless stated. "Bit-equal" means position, rotation,
linear and angular velocity compared as stored doubles (`p3_first_nonzero.py`
style).

### 7.1 Unit tests (`BallanceMMOMessageTests`)

- **Messages:** `session_event_msg` Trafo round trip. The key separates explode
  from reset and server-stamped from owner events. The rank.
- **`trafo_acceptance`:**
  - normal explode/reset: no move;
  - a reset and the next same-type explosion delivered in one tick: the
    explosion is moved to reset + 140;
  - an explosion of a live type set: a server reset is inserted at
    `max(N, live.applied + 1)` before it;
  - a reset older than the newest explosion by origin: superseded;
  - the deadline fires at `live.applied + 1320`, queued when `N + L` reaches
    it, and not when a reset came first;
  - a late owner reset after the deadline: accepted as a no-op;
  - leaver with a ball, leaver without a ball, leaver with an explosion
    accepted at E > next (review 1 S4): every live type set reset at
    ≥ `live.applied + 1`, `TRAFO_FINAL` on all three, `remove_at` after the
    last.
- **Validation:** an accept/reject table (inside and outside `kTrafoReach` of a
  trafo, NaN, bad kind, bad T, client-set server/final bits).
- **`trafo_seed`:** fixed vectors (x64, x86, GCC identical).
- **`trafo_input_log`:** newest explosion at or before t; pruning keeps each
  type set's newest explosion and reset.
- **Core with the fake platform:**
  - `hold_trafo` stamps `tick+1+lead`, and before the assignment defers to the
    first assigned frame;
  - inputs are applied at their tick, live and in a replay;
  - a late remote input joins the pending input rewind with
    `lifecycle_rewind = false` and `own_echoes = false` (the retail config);
  - an input outside the history requests a resync;
  - MOVED, REJECTED and SUPERSEDED own echoes;
  - a set is requested on first sight of a player, never inside a replay;
  - the reconcile's decision table (equal, missing locally, extra locally,
    different explosion), with every platform call checked.
- **Record v3:**
  - restore with a script added after the capture puts it to its built state;
  - restore with a vanished id fails;
  - the v2 mechanism lists are unchanged byte for byte.

### 7.2 SimTool (offline, new modes `--trafo-sets N`, `--explode-player ID TYPE TICK X Y Z`, `--reset-player ID TYPE TICK`, `--describe-trafo`)

| check | run | pass |
|---|---|---|
| S0 builder | build sets for 4 players on L1-L13 and the arena, once after a pre-built quiesce of exploded originals (paper exploded at 300, anchor at 400) | I0-I7 OK everywhere; the quiesce log names the 18 bodies and 18 controllers it removed; reference-map digest identical across levels for the script part |
| S1 copy ≡ original | fresh L2 world, explode the original wood set at P with seed s (registry entry for the original frame, test only) vs player 7's wood set at P with seed s; repeated with player 7's CollSound blanked | every piece of the set bit-equal (name minus `_Trafo7`) over 600 ticks, in both runs: CollSound is physics-inert |
| S2 independence | arena (as level 13), 4 sets explode at one tick ≥ 30 m apart on the flat road vs each alone | each set bit-equal to its solo run, 600 ticks |
| S3 order | S2 with the hosts' player list reversed | bit-identical to S2 |
| S4 platforms | S2 on the x64, x86 and Linux headless builds | per-tick pose hashes identical |
| S5 lifecycle and hulls | L2: player 1 wood explode 300; player 2 wood explode 320 at a different place; player 1 reset 900; player 1 wood explode 1040 (the minimum span); death-style resets of all types for player 2 at 1200; player 3 wood and stone explode at one tick 1300 at two places. Linux ASan build | no ASan report; each reset's bodies gone in its frame R+132 (the last step with bodies is R+131) and Restore IC at R+133; cores back to the baseline afterwards; each set's collision surfaces present while it has bodies, dropped only for itself; player 3's two type sets start at their own positions |
| S6 savestate | `--savestate-check --frames` with explodes and resets of 2 players inside the window range, 50 rounds, inputs re-applied from `trafo_input_log` | `PASS failed=0` |
| S7 effect checks | free run with the hold armed and `--activate Ball_Explosion_Paper 500` | `trafo_original_leaks` 0; an `--activate` that slips past the hold (test switch) is detected the same tick |
| S8 PreSimulate queue | L2: explode paper (player 1), reset at +600, run 600 more ticks; queue dumped every tick (bridge counter) | decides 9.2: either no non-BMMO entry from any `_Trafo` block at any tick, or the named entries |
| S9 vanilla | `rec_m3b.bmrc`; `--explode wood 300` (originals); 9.14's two-explosion run guarded and unguarded | 4169/4169; traces identical to HEAD; 215 pose hashes identical |
| S10 server replay | `--replay-session` of every journal of §7.3/7.4 | matched N/N, 0 checkpoint mismatches, pieces included |
| S11 filter | a custom drop: player 1's paper set exploded 3 m above a `PE_Balloon` body (`--drop-at` over the L2 balloon), world option on vs off | on: no contact of a piece with any `PE_Balloon*` body (bridge contact dump), every other contact of the run identical to the off run until the first piece-balloon contact of the off run |
| S12 vanilla reset paths | `--describe-script Gameplay_Ingame`, `Event_handler` and a free run with Game Over, Finish (`--drop` into `PE_Levelende`) and the pause verb at 300 ticks after a paper explosion | records whether the Fadeout timer still fires in each state. It is information for the deadline's log line, not a gate: the deadline covers every outcome |
| S13 reach | `reach.py` rerun on the copies (player 7's set) | the reach table equals `reach/all.tsv` for the 573 measured runs |

### 7.3 Headless harness (arena, `scripts/arena_map_test.py`, Arch server)

| arm | recipe | pass |
|---|---|---|
| H1 four trafos | 4 clients, `--drive --beam-bays 0,0,0,0 --beam-dx 0 --beam-spread 2.5 --beam-tick 650` (all onto P_Trafo_Wood: four stone explosions within ~1 s, then wood and paper ones while driving; `p3_h4.sh` recipe), snapshots off, checkpoints every 2 ticks, at 0 and 150 ms latency | (a) for every piece of every set and every checkpoint at or after the arrival of every Trafo event concerning it (the client journal's RECEIVED record), every client's LOCAL row is bit-equal to the server's (`worst_bodies.py` 0.0000 on all pieces; `first_divergence.py` none); (b) every client holds all 4 players' sets' bodies; (c) unmatched, far, resyncs, rebases 0; starved within the control band; (d) late Trafo events 0 at 0 ms; (e) `trafo_original_leaks` 0, `trafo_missed` 0 |
| H2 staggered | 4 clients onto P_Trafo_Wood / Paper / Stone at ticks 650/900/1150/1400 (a per-client `--beam-ticks` option), so explosions overlap resets and one same-type repeat happens within 20 s | as H1 |
| H3 loss | H1 at `--latency-ms 150` and 10 % loss | (a) from the first checkpoint after each arrival; every late Trafo event made inside a rewind (log `inrew` naming trafo), 0 applied at the live tick |
| H3b blackout past the history | H2 with client 2 `BMMO_FAKE_LAG_MS` downlink blackout of 300 ticks starting 20 ticks before another player's paper explosion, and one before a reset | client 2 logs `trafo_missed` > 0 followed by a resync; after the resync's re-step every piece row of client 2 is bit-equal to the server's at every checkpoint, or, where not, the residual is reported per body against the rows-only class (4.7) with its numbers; the missed reset's pieces and wind are gone at the server's tick |
| H4 snapshots on | H1 with snapshots 2/66 | 0 snapshot corrections naming a piece; mism counts at or below P's band (`p3_h4_new*`). **Moved (user, 2026-10-25, §11): the gate of the rewind-cost optimization (`docs/roadmap.md` §4 item 10), not of T3** |
| H5 forced rollbacks | `savestate_check.py --journals` pairs 13:24, 50:12, 97:24, 131:47 over a trafo run (snapshots off) | every body exact, all players' pieces included (step P's open (2) extended to every set); no "PreSimulate queue differs" |
| H6 CPU | H1 at 150 ms, interval 2, against the same batch without trafos (`--beam-bays` to non-trafo bays) | server tick p99 within the budget (15.15 ms) and within the control's run-to-run band + 10 %; client `t_rest/t_rep/t_cap` within the band + 10 %; starved within the band. **Moved (user, 2026-10-25, §11): the gate of the rewind-cost optimization (`docs/roadmap.md` §4 item 10), not of T3; T3 records the same figures with snapshots off as that step's baseline** |
| H7 late join | the `lp1_join` recipe with client 3 joining 5 s after two members' paper trafos | the join's reconcile remakes both paper type sets; after the join's re-step client 3's pieces equal the server's rows at every checkpoint (or the rows-only residual, reported as in H3b); wind controllers present (savestate describe) |
| H8 leaver | (a) a client with live paper pieces leaves; (b) a client leaves between its dephysic and its new ball's Physicalize (no ball; `--leave-at` at dephysic + 50); (c) a client leaves right after an explosion stamped ahead | every remaining world and the server remove that set's bodies at the final reset's R+132; no piece row of that player afterwards; the set is retired in every world (deselect logged, destroy logged `history_ticks` later); H8 (c): the explosion runs everywhere before its final reset |
| H9 hash | 1 client, trafo, snapshots off, no rollbacks (`--no-rollback` control) | client TICK hash equal to the server's on every tick 600-1500 (today 808-1041 differ) |
| H10 world option off | H1 with a client that does not advertise `CAP_TRAFO_INPUTS` (test switch) | the session runs with the option off; behaviour equal to HEAD + P's H1 (pieces owner-only, no Trafo events) |
| H11 deadline | one client explodes paper, then a test switch drops its Fadeout reset at the sender | the server's deadline reset at A + 1320 is relayed; every world (owner included) De Physicalizes at its R+132; the server log names the deadline |

### 7.4 Retail

| run | recipe | pass |
|---|---|---|
| R1 solo arms | `r2_run.py` R2 scenario (`p_a_ctl`, forced arms 50:12, 97:24, 131:47, 13:24) and the two-trafo `r2t_run.py` | every body exact by name in every arm (21 / 37 bodies, pieces now `_Trafo<id>`); the client's pieces bit-equal to the server journal's at every checkpoint from the explosion's applied tick on (about 808 + lead; new: the server has them); `trafo_original_leaks` 0 |
| R2 two clients | `p3_two.py` (level 11, s1 P_Trafo_Stone, beams at 300/340, snapshots 2/66), with the R2 config (lifecycle_rewind and own_echoes off) | each client has both players' wood sets; every piece row bit-equal to the server's at every common checkpoint after arrival; late Trafo events made in input rewinds; reference-map digest of the retail logs equals the headless digest (I2 across CK2 implementations); far 0, resyncs 0, mism within `p3_b_P` band |
| R2b lag | R2 with `net lag 150` on client 2 | as R2; client 2's late Trafo events all inside rewinds |
| R3 second session | two sessions in one game process without a reload, with a single-player paper explosion 5 s before the second session's start | sets rebuilt; no `_Trafo` object of the first session left (object count); the quiesce removes the single-player pieces and wind (log counts 18/18); R2's criteria in the second session |
| R4 core off | `r2off_run.py` | world option off; bit-equal to `p3_off_P` |
| R5 wire | `wire_check.sh` old vs new on trafo-free captures | journals identical but header version/world options; sends identical but version, caps and world-option bytes |

### 7.5 What each check proves

| check | invariant T part it carries |
|---|---|
| S1-S5, H1-H3, R1, R2 | same bodies, same state, every world |
| S6, H5, R1 arms | the rollback keeps it |
| H3b, H7, H8, H11 | missed inputs, joins, leaves and lives the owner does not end |
| S8, H5 | the PreSimulate queue does not break rollbacks |
| S11 | exception (c) |
| S0, S7 | the builder and the hold |
| S9, H10, R4, R5 | must-not-change |

## 8. Steps (each: implement → review (two reviews) → gate on its checks)

| step | content | gate |
|---|---|---|
| **T0** prerequisites | step P closed (its open headless trafo arm exact); F3 landed (resync and join re-step with the portable script record); F4 recommended (late Sector rewind); the protocol number agreed with F3 and option B. R3 is **not** a prerequisite: the trafo inputs ride the input-rewind path (4.6) | step P's own gate; F3's gate |
| **T1** host support + bridge, no wire | `game/trafo_sets.hpp` (quiesce, builder, invariants I0-I7, hold and effect checks, `run_trafo`, trafo list, unmake/remake, retirement); `session/trafo_input.hpp` (seed, ledger, input log); bridge v20 (registry, per-set restore, seed, exemption rebuild, filter rule); SimTool modes; record v3 in SRP | 7.1 (ledger, seed, log, record v3), S0-S9, S11-S13; S8's outcome decides 9.2 before T3 starts |
| **T2** wire and server | event type, payload, flags, key, rank, caps, world option, acceptance through the ledger, validation, deadline, leaver resets, journal v4; PW sets at `add_player`, anchor quiesce, `apply_event` Trafo, trafo frame in `tick`, server hold, the ledger in the resync and join state; the sets' scripts in the portable script record (its version bump with this protocol: T1 kept it at 1) | 7.1 (messages, acceptance, validation), S10 on synthetic journals, `kTrafoReach` set from the trafo positions of the existing R2 and `p3_h4` logs (9.3) |
| **T3** core + headless host | `hold_trafo` and its pre-assignment deferral, the input log, live and replayed application, the trafo rewind and the missed-input resync, MOVED/REJECTED/SUPERSEDED echoes, `make_trafo_set` on first sight, reconcile, retirement, journal records; SC platform ops; remove step P's trafo `activations` and referential override; `explosion_pieces` over every set | 7.1 (core), H1-H3, H3b, H5, H7-H11, judged with snapshots off (user, 2026-10-25, §11); H4 and H6 moved to the rewind-cost optimization (`docs/roadmap.md` §4 item 10), T3 records its snapshots-off CPU numbers as that step's baseline; re-gated at TR3b after the rewind-cost step RC1 with one H1 pair at 150 ms (the one-pair rule, §11), the rest of the row deferred to the final verification |
| **T4** retail host | CH platform ops through `game/trafo_sets.hpp` and `physics_view` hooks, the `trafo_host` file-scope static, the quiesce at `core_host_anchor`, the hold in `post_step`, session-end teardown | R1-R5 |
| **T5** records | `docs/engine-changes.md` bridge note (v20, no engine change); session-client-core design "As built (step T)"; collision-overhaul-design 9.x entry; this document's status | review only |

Every gate reports symptom, cause and options for any check that fails. None is
waived.

**As built (step T1, 2026-10-24; HEAD b64fe49 + the working tree).** Host
support, the bridge and SimTool; no wire, no host uses any of it yet (T2-T4).

- **Where.** `session/trafo_input.hpp` (new, CK-free): the flags
  (`TRAFO_RESET` bit 0, `TRAFO_SERVER`, `TRAFO_FINAL`), `kTrafoResetSpan` 140,
  `kTrafoLife` 1320, `trafo_seed` (FNV-1a over the four values' bytes, folded
  to a nonzero int32), `trafo_acceptance` (explode: rule 1's server reset at
  `max(N, live.applied + 1)`, then `applied = max(o, N, last_reset + 140)`;
  reset: superseded when older than the newest explosion's origin, else
  `max(o, N, live.applied + 1)`; `deadline(N, L)`; `leaver(p, F)`: three
  `RESET|SERVER|FINAL` decisions, the live one at `max(F, live.applied + 1)`),
  `trafo_input_log` (by applied tick, (player, type, kind, origin) order inside
  a tick; `newest(p, T, reset, t)`, `move`, `drop`, `prune` keeping each type
  set's newest explosion and reset) and `plan_trafo_record_restore`.
  `game/trafo_sets.hpp` (new; only CK API the retail SDK headers declare,
  checked by name - the mod first compiles it at T4): `quiesce_originals`, `build_trafo_set` (I0-I7, the
  reference-map digest), `trafo_registry` (value type, `part()` = the trafo
  list with the built states), `run_trafo`, `trafo_frame`, `trafo_hold`
  (option E and the activation hold), `trafo_effect_check`,
  `unmake_type_set` / `remake_type_set`, `trafo_set_final`,
  `retire_trafo_sets`, `reselect_trafo_set`, `teardown_trafo_sets`; the
  bridge is reached through `trafo_set_hooks`.  `game/script_replay.hpp`:
  record v3 (`trafo_part`), `execute_scripts`, `capture_script_block` /
  `restore_script_block`; `execute_frame` takes the trafo list (run before the
  selection).  `session/rollback_binding_scripts.hpp`: `script_binding_options::
  trafo`.  Bridge: `physics_state.cpp` / `.hpp`, `physics_rt_api.h`,
  `physics_rt_bridge.cpp` (`docs/engine-changes.md`, "Bridge notes: the trafo
  sets").  `session/resync_rules.hpp`: `is_trafo_script_name` leaves the
  copies (`_Trafo` in the name) out, so no name rule ever selects a copy
  (4.3).  `physics_world::set_frame_hook` (called after `update_sectors()`,
  before the engine's frame; unset in the server and the headless client).
  `rollback_engine::forget_after` (test support; no host calls it).  SimTool:
  `sim/trafo_tool.hpp` and the switches in `sim_tool.cpp` (below).
- **Adaptations** (no principle touched):
  - Bridge API **v21**, not v20: A3's travelling savestate took v20.
  - **Record v3 is the local record** (design 4.5, 7.1): the version word 3,
    then the v2 record byte for byte, then the trafo scripts keyed by id;
    written only when a trafo part is passed, so every record without sets
    stays v2.  The **portable record keeps version 1**: its bytes are A3's
    resync state on the wire (protocol 10), and T1 is "no wire"; the sets'
    scripts enter the portable record with T2/T3.  `selection` is unchanged
    (held by `physics_session_state`, a `BallanceMMOClient` member): the trafo
    list travels beside it.
  - **Materials from the pieces' meshes**, not by name: stone has two
    (`Ball_Stone_Pieces_Inside`, `_Outside`, the reset's two Restore IC / fade
    targets).  The list is 120 objects per set on every level (54 entities,
    51 meshes, 3 groups, 4 materials, 8 scripts).
  - **The copy's dependencies are explicit**: the default copy options with
    the BeObject's scripts and groups off (no `All_Balls` scripts pulled, no
    copy put into the originals' groups), the mesh's materials and the 3D
    entity's meshes and children on, textures off.  The reimplementation's
    `CKBehavior::PrepareDependencies` pulls a script's owner only in BUILD
    mode, so I0 found 0 extras on every level; the retail CK2.dll is T4's
    (9.1).  The extras check is by new root objects (root scripts and
    BeObjects), the copies' kinds against the list's.
  - **Step 5 runs first**: every copied block's pointer-typed local is
    NULLed right after the copy, before any owner change (`AddScript`'s
    detach/attach callbacks would act on a copied controller or listener
    pointer).
  - **The quiesce also** resets the original collision sound's
    `PhysicsCollDetection` blocks (their listeners) and restores the
    pieces' materials' initial conditions (a fade the anchor interrupted).
  - **The digest is structural**: input sources and output destinations by
    place (script / block walk index / name), the objects constant locals
    hold - not runtime values.  493 lines.  The runtime values are made
    equal instead (the level reference, below) and have a digest of their
    own (`value_digest`, 597 lines).
  - **The level reference** (added at the review; 2.4 step 6's built
    state): `CopyObjects` copies the originals' current values, which are
    the last run's.  Measured on L2 (SimTool `--trafo-built-params`): an
    original paper explosion before the anchor left 11 values different in
    the copies (the Random outputs, Group Iterator's and Identity's outputs,
    `W?ndOn?`, the Paper explosion's In 0), and an original wood or stone
    explosion left `PhysicsCollDetection`'s Entity output naming
    `A02_Floor_02`, which failed I2 - the build refused.  So
    `trafo_reference` (`game/trafo_sets.hpp`): the 8 originals' non-opaque
    locals and outputs and their IO flags, captured once when the scripts
    first exist and before any original explosion
    (`capture_trafo_reference`; the scripts are `Balls.nmo`'s, loaded at
    game start in every world - SimTool captures at tick 16), kept in the
    registry; the builder refuses without it and writes it into every copy
    right after the copy (object references mapped as `CopyObjects` maps
    them; through the buffer, no propagation), before I2.  Result: the
    value digest `bf8575545ab7fbe0/597` for every set on L1-L13 and on L2
    after an original wood, stone or paper explosion before the anchor (0
    values rewritten in a fresh world, so every fresh-world check above is
    unchanged; 29 / 25 / 10 after wood / stone / paper); a wood set exploded
    at 1000 after each of the three histories gives 9600 piece lines
    bit-equal to the fresh world's.  Hosts (T2-T4): the server and the
    headless client capture at their world's load, the retail host when
    `Balls.nmo`'s scripts first exist (before the first level).
  - **Option E's detection is host support** (`trafo_hold::poll`): the
    Trafo Manager's Delayer whose Out reaches "start Explosion" is found
    structurally at `arm`; its start (its transition from inactive to
    active since the previous poll, or a restart: its elapsed time went
    back - not "elapsed exactly one delta", which a host whose frame delta
    is not bit-exactly 1000/66 would never match; changed at the review)
    predicts the explosion at that frame + `frames_until(1000 ms)` = 67
    (the float sum of 15.151515 ms); the later activation, cancelled, is
    confirmed and not reported again.  Stamping and the lead > 66 fallback
    are the core's (T3).  The hold's position is the referential's
    translation (Ball_Pos_Frame's parent, or the frame without one).
  - `run_trafo` sets the next-frame flags with `ModifyFlags` (2.5), not
    through `CKScene::Activate`.
- **S8 decides 9.2: no entries.** L2, player 1 paper exploded at 300, reset
  at 900 (+600), run to 1500, the PreSimulate queue dumped every tick: no
  non-BMMO entry at any tick (`presim max non-BMMO entries 0, of them from
  _Trafo blocks 0`).  The paper branch of `Ball_CollSound_Pieces` is a Wave
  Player only (no `PhysicsCollDetection`).  Also measured for wood and stone
  (the branches with listeners), resets at +2, +30, +60 and +600: 0 entries
  every time.  (The explanation first written here - the Delayer fires
  `Stop` and `Create` together - was wrong; corrected at the review from a
  per-tick dump of every copied `PhysicsCollDetection` block, SimTool
  `--trafo-colldump`.)  Why no entry: an entry is a `Create` that finds no
  listener in local 0 and no body (`PhysicsCallbackContainer::Process(pc)`
  executes the callback at once and keeps it queued only when
  `GetPhysicsObject` fails).  The dump (wood and stone, reset at +60 and
  +600): the collision sound's `Create`s come from its activation alone (In
  0), which the explosion issues in the frame after its Physicalize -
  explosion at 300, every block's listener set at 301 with its body there;
  the 3 s Delayer (E+198 = 498) issues `Stop` only (listener deleted, block
  inactive, in that frame: had it also sent `Create`, the block would have
  handled `Create` first and kept the listener one frame longer); the
  reset's De Physicalize (R+132: 492 for the reset at +60, 1032 for +600)
  leaves the listener in local 0 (`event_collision_object_deleted` clears
  only its object pointer) until that `Stop` deletes it.  No collision
  `Stop` was seen in these runs (the listener never dropped between 301 and
  498).  So no `Create` reaches a bodiless piece.  Positive control (the
  sampler sees such an entry): `--trafo-poke-create 1 290` gives one idle
  block of the copy its `Create` by hand on a bodiless piece - the sampler
  counts 1 non-BMMO entry from a `_Trafo` block (`Ball_CollSound_Pieces_
  Trafo1/PhysicsCollDetection on Ball_Wood_piece11_Trafo1`) at every tick
  from 290 to 399; the wood explosion at 400 physicalizes the piece and the
  queued callback then creates its listener (0 entries from 400).  A path
  not measured: a collision `Stop` followed by a `Create` from another
  source after the De Physicalize; the graph has no other `Create` source
  than the activation, and T3's H5 (the PreSimulate queue compared at every
  restore) is the backstop.  `flags.s8_entries = false`.
- **Checks** (x64 `build-agent-rTR1`, x86 SimTool `build/sim-x86-a3`, Linux
  `bmmo-build-arch`, Linux ASan `bmmo-build-asan-tr1`; every run its own
  process, outputs in `.local-tools/work/tr1/`):
  - 7.1: 485 tests (471 + 14: the ledger, the seed vectors, the log, the
    record plan, the copies' names).  Record v3's bytes (CK) by SimTool
    `--trafo-record-check`: (1) the v3 record's mechanism part equals the v2
    record (43,561 / 61,689 bytes); (2) a record of set 1 alone restored with
    both sets selected puts set 2's 8 perturbed scripts back at their built
    state; (3) a record naming a destroyed set is refused ("a recorded trafo
    script is gone"); (4) a v2 record with the trafo part puts all 16 to
    built.  All PASS.
  - S0: L1-L13 (13 = the arena installed in slot 13; the original Level 13
    is not on this machine's slot; L1's explosion at 2000, the tutorial holds
    physics before), paper exploded, anchor 100 ticks later, 4 sets each:
    quiesce `18 bodies, 18 controllers` on every level, I0-I7 hold for all 52
    builds, 0 extras, I5 `randoms=5 set_positions=3 exempt=54 pieces=51`,
    digest `90f856e0c8c20dd4/493` on every level and every set.
  - S1: L2, wood at (948.741, 2.066, -56.514), tick 600, seed 930656193: the
    original set (test seed entry, test referential) vs player 7's copy:
    9600 piece lines (16 x 600 ticks) bit-equal; with player 7's collision
    sound left out of the trafo list: bit-equal too (physics-inert).
  - S2/S3: arena, 4 sets (wood, stone, paper, wood) at x = 20/60/100/140 on
    the lane (z = 15), tick 600: each set bit-equal to its solo run over 600
    ticks (9600 / 10200 / 10800 / 9600 lines); built in reverse order:
    30600 lines bit-identical.
  - S4: S2 on x64, x86 and Linux: 701 per-tick pose hashes identical, 35751
    piece lines bit-equal.
  - S5: L2 (p1 wood 300, p2 wood 320, p1 reset 900, p1 wood 1040, p2 resets
    1200, p3 wood + stone 1300 at two places, resets 1400): every reset's
    bodies gone in R+132 (1032, 1332, 1532) and the pieces back at their
    initial conditions in R+133; p1's re-explosion recompiles p1's hulls
    (new signature) while p2's stay; p3's two type sets start at their own
    positions; cores and world pose at 1599 equal the control without inputs
    (`a74e0c13a0b4149b`, 1 core).  Linux ASan: 43560 piece lines bit-equal to
    Windows; the ASan reports are exactly the control's without trafo sets
    (31849 `alloc-dealloc-mismatch` in `CKFileObject::CleanData`, known in
    engine-changes.md; one `stack-buffer-overflow`, below): none from the sets.
  - S6: `--savestate-check --frames --level 2 --drop-settle 250 --trafo-sets
    2`, p1 wood explode +10, p2 paper explode +20, resets +60 / +100, window
    300, 50 rounds: `PASS rounds=50 failed=0`; 6 rounds crossed a piece birth
    (138 bodies destroyed at the restore and made again by the replayed
    explosion, 138 exactly at their birth records, 0 missing), 5 crossed a
    piece death (revived at the restore).  The harness binds the session
    world (`bind_session_world`: destroy, revive, the births from the event
    log's recipes), drops the engine's records after the round's live tick
    (`forget_after`), and re-makes each round's live world from the record
    of t0 (the rounds are no longer chained when the window changes the body
    set: a world chained from another tick has another set of pieces than
    the record it is rewound from).
  - S7: L2, hold armed at 280: `--activate Ball_Explosion_Paper 500` (after
    the frame) cancelled and reported, `trafo_original_leaks=0`; with
    `--trafo-hold-slip` the leak is detected in the frame it runs (501: 18
    bodies, then the script).  With a real trafo (ball beamed onto
    P_Trafo_Stone_MF at 600): the Delayer starts at 691, explosion predicted
    for 758, the activation cancelled after 757 and confirmed, its position
    bit-equal to the Delayer-time one; the Fadeout reset at 1919 reported;
    0 leaks.
  - S9: the free runs `--explode wood 300` and 9.14's two explosions
    (guarded and unguarded) give every per-tick world hash, every pose line
    and the bodies at 610 identical to HEAD's binary (`build-agent-rR5`);
    215 pose hashes guarded = unguarded.  `rec_m3b.bmrc` no longer exists
    on this machine; replaced by the key record `work/a4/r0_keys.bmrc`
    replayed by both binaries: identical output.
  - S11: L2, sector 5 world (`--drop` scene), player 1's paper set 3 m above
    `PE_Balloon_Platform`: filter on, 0 piece x keep-local contacts or
    mindists; off, 11 contacts and 13 mindists from the explosion's frame.
    Beside the ramp (1585, 27.7, 74): off has 8 piece x `PE_Balloon_Platte*`
    mindists (no contact), on none; the piece tracks are bit-identical over
    580 ticks and every other contact identical.
  - S12 (information): normal play: the Fadeout reset at detection + 1320
    (1919); a death with lives left (ball beamed into a DepthTestCube at
    1058): all three resets at 1061 and the Fadeout reset still at 1919.
    Game Over, Finish and pause were not reached in a free run (the lives
    live in the Energy array behind Gameplay_Energy, the last sector is
    inactive, no pause verb); the graph shows `Test.False -> Deactivate
    Ball.Game Over` bypassing `reset Ballpieces` (1.5).  The deadline covers
    every case.
  - S13: the reach measurement's reference (`design/track2/reach/`) is gone,
    so the check is re-run as a pair: for each of 77 distinct `P_Trafo_*_MF`
    positions of L2-L13 (as listed by `--list-trafos` at 600) and each type,
    the original set (seeded, placed 1.3 m above) against player 7's copy at
    the same tick and seed, 1400 ticks tracked: 231 of 231 runs bit-equal.
    Reach on the level (within 20 m below the trafo) 32.5 m wood, 43.0 m
    stone, 133.7 m paper (the wind); a fall > 20 m in 73 / 73 / 77 runs, up
    to 2737 m (`work/tr1/s13.tsv`).
  - The reconcile's halves, retirement and teardown (SimTool
    `--trafo-unmake/--trafo-remake/--trafo-final/--trafo-retire/
    --trafo-teardown`): p1 paper exploded at 300, unmade at 400 (18 bodies),
    remade at 401 with 401's seed: 10782 piece lines bit-equal to a direct
    explosion at 401; p2's final resets at 500: deselected at 633 (inert),
    destroyed at 729; the teardown at 1000 leaves 0 set objects and the
    exemption list at the originals' 54.
  - Builds and the BAR: x64 BUILD-OK, x86 CLIENT-BUILD-OK (client objects
    deleted first; no client file changed), `linux_test_syntax.sh` 27 lines
    ok; `wire_check.sh rR5 rTR1 rTR1w`: WIRE-CHECK: ALL SAME (24 journals
    IDENTICAL, 24 sends SAME); solo snapshots off (`e0_solo.sh rTR1b`, Arch
    server rebuilt with the tree): IDENTICAL to `rR4` (2227 TICK hash + pose
    common, 0 differ; LOCAL 1114/1114), server journal 2409/2409 matched (a
    first run, `rTR1`, had 26 starved input frames on the host: LOCAL
    1114/1114 equal, its input records' timing different).
- **The reviews' fixes** (two reviews; every should-fix confirmed in the
  code and fixed; reruns below):
  - `trafo_input_log::prune` took each key's newest over the whole log, so
    an input already logged for a later tick (a lead's pending explosion)
    let it drop the one still newest at or before every t up to it; the
    newest is now taken among the entries applied before the prune tick
    (test `PruningKeepsTheLiveExplosionWhenALaterOneIsPending`).
  - The builder's early I0 failures (after `CopyObjects`, before the
    renaming) returned without destroying the copies, which still carried
    the originals' exact names; every root object the copy made is now
    destroyed on those paths too (`undo_copy`).
  - `execute_frame` ran the trafo list after the satellites; the live frame
    runs the trafo frame before the engine's whole behaviour pass
    (Gameplay_Ingame's satellites included), so the replay runs it first
    now (4.5's line corrected; it contradicted 2.5).
  - The bridge registry could outlive its world (keyed by the manager,
    never cleared on a teardown that skipped `teardown_trafo_sets`): stale
    sets are now dropped before any decision reads it (frame id must still
    name the registered frame), and a second registration of a player is
    refused.  The piece-name map is looked up without building strings
    (`std::less<>`).
  - Option E's start detection (above).  The level reference (above).
  - S6 counts a round whose re-made live world is not the straight pass's
    (or whose t0 restore is refused) as failed (`premise:`), not only
    printed.
  - Reruns (x64, the fixed tree): 486 tests pass (485 + 1); S0 on L1-L13
    (4 sets, anchor 400) and on L2 after an original wood / stone / paper
    explosion: 0 failures, digest `90f856e0c8c20dd4/493` and value digest
    `bf8575545ab7fbe0/597` for every set; record check (1)-(4) PASS; S6
    `PASS rounds=50 failed=0` (138 / 138 births exact, 0 missing, the same
    as before the order change); S7 unchanged (the held activation
    cancelled, the slip leak at 501, the real trafo's Delayer at 691
    predicting 758 and confirmed, the Fadeout reset at 1919, 0 leaks); the
    reconcile halves / retirement / teardown exercise unchanged; S8 above.
    The fresh-world checks (S1-S5, S9, S11, S13) are not affected: the
    reference rewrites 0 values in a fresh world, the order change and the
    hold change are not on their paths, and the bridge's prune drops
    nothing while the sets live.
- **Found on the way** (not this step's, reported per principle (d)):
  - SimTool's `--savestate-check --frames` on L2's default sector scene (the
    window from tick 31) refuses its first round on HEAD's binary too: the
    retail ball is parked at 204, inside the window, and Gameplay_Ingame's
    `Physics WakeUp` and Sound_Manager's four `PhysicsCollDetection` then
    queue on the bodiless ball for good, so every anchor before 204 has
    another PreSimulate queue.  A harness scene property; S6 runs with
    `--drop-settle 250`.
  - ASan, headless x64/Linux only: `GeneralParticleSystemCallback`
    (TT_ParticleSystems_RT `GeneralParticleSystem.cpp:415`) clears its
    emitter pointer with `CKBehavior::SetLocalParameterObject(0, NULL)`,
    which hands a 4-byte `CK_ID` to `SetValue` of a pointer-sized local:
    4 stack bytes past `objID` are read (CKBehavior.cpp:1151-1154).  On the
    retail x86 build both are 4 bytes.  The local is the particle emitter's,
    deleted right before; no physics state.  Same in the control without
    sets.

**As built (step TR2 = T2, 2026-10-25; HEAD 2e6f026 + the working tree).**
Wire and server: the Trafo event, its acceptance, validation, deadline and
leaver resets, the world option, the sets in the server's world, the ledger
in the resync and join state, the portable record's trafo part.  No client
speaks it yet (T3, T4): with the option off - the default at T2 - every
session runs as before.

- **Numbers.** `PHYSICS_PROTOCOL_VERSION` 10 -> **11**, journal version 4 ->
  **5** (`entity/session.hpp`, `session/journal.hpp`; the next free ones, as
  expected).  `event_type::Trafo = 5`.  `CAP_TRAFO_INPUTS = 1u << 2` (the
  design's bit 1 is `CAP_RESYNC_STATE` since protocol 10).
  `WORLD_TRAFO_INPUTS = 1u << 1` in the sector placement design's field (a
  `u32 world_options` after `protocol` in SessionStart; the record-format
  design's Q3 settled the field; §5 here said a byte after `spawn_spacing`).
  Portable script record version **2** = version 1 plus the trafo scripts by
  name after the cells, written whenever a trafo part is passed - the option
  is on - an empty part (no set selected) included (every session without
  the option still sends version 1, byte for byte).
- **The world option (adapted).** 2.14 sets the bit "only when every member
  has the capability", but the capabilities arrive with SessionReady, after
  SessionStart (which carries the bit and before which the world is built).
  So the bit is the server's config `physics.trafo_inputs` (default **off**
  until both session-core hosts run the inputs: T3 / T4 turn it on), fixed at
  `start_physics_session` like the spawn impulse, and a member whose
  SessionReady lacks `CAP_TRAFO_INPUTS` in a session with the bit is refused
  with a reason (SessionEnd to it, out of the session, not of the room, as
  the start barrier drops a silent member) - start members as well as late
  joiners.
- **`kTrafoReach` = 4.5 m** (decision 9.3): the R2 / r2t / p3_two / p3_h4 logs
  (`maps/arena-test/r2_*`, `r2f_b_*`, `p3_t_ctl`, `p3_tR2_ctl`, `pr2_*`,
  `p3_b_*`, `p3_h4`) predate the hold and carry no captured position; the
  proxy is the new ball's Physicalize after the trafo ("set new Ball" puts it
  where the explosion read the old ball, 2.3).  624 trafo records, 4 distinct
  trafos (arena wood / paper / stone, L11 s1 stone): every one exactly
  **3.000 m** above its `P_Trafo_*_MF`; T1's S7 hold capture on L2 3.00002 m.
  Maximum 3.0 x 1.5 = 4.5 (`work/tr2/reach.py`, `reach.txt`).
- **Found and fixed: the trafo positions are the PH table's, not the
  anchor's entities.**  The original levels pool their trafos across sectors
  (sector placement design 1.2): L11's `P_Trafo_Stone_MF001` stands at its
  sector-6 row (1532.83, -101.89, -343.45) at the anchor and is moved to its
  sector-1 row (60.95, 5.10, -12.58) when sector 1 is placed.  Validated
  against the anchor's entities, every L11 sector-1 stone trafo (the p3_two
  and R2 two-client trafo, 3.0 m above that row) was 1.5 km from every trafo
  and would have been rejected.  The server's trafo points are every PH row
  of a `P_Trafo_*` type (static level data, read at the anchor) plus every
  `P_Trafo_*` entity where it stands (a level without a PH table): L11 16
  rows + 10 entities, the measured explosion 2.9999 m from
  `P_Trafo_Stone_MF001@PH12`.
- **Where.**  `session/trafo_input.hpp`: `kTrafoReach`, `trafo_point`,
  `trafo_validate` (kind, type, the client-set server/final bits, unknown
  bits, a finite position within the reach of the nearest point; a reset's
  position is read by no world and not checked).  `entity/session.hpp`:
  the constants and `trafo_ledger_entry` {player, type, has_explosion,
  explode_applied, position, has_bodies, has_reset, reset_applied, final,
  final_applied} (the last two from the review fix-up below).
  `message/session_event_msg.hpp` (Trafo: ball_type, flags, position),
  `session_start_msg.hpp` (world_options), `session_resync_state_msg.hpp`
  (`trafo_ledger` after the savestate, capped at 3 x 2 x 64 entries).
  `session/lifecycle.hpp`: the key gains `flags` (Trafo only), the name
  "trafo"; the rank of Trafo is 2 (unchanged code: every type but U/P).
  `session/journal.hpp` v5 (header `world_options`, trailing; a v4 file reads
  as 0).  `game/script_replay.hpp`: `capture_portable(..., trafo)`,
  `localize(..., trafo)` (a v2 record becomes record v3's trafo part, keyed
  and as strict as the selection; a v1 record with a trafo part passed gives
  a record without one; a v2 record without one fails).
  `game/session_host_support.hpp`: `session_script_selection(context,
  trafo_inputs)` leaves the originals out with the option (4.3).
  Server: `sim/physics_world.*` (`world_options::trafo_inputs`; the level
  reference captured on the boot's first tick the 8 scripts exist - engine
  tick 16; `anchor_trafo`: the trafo points, the quiesce - `0 bodies, 0
  controllers, 0 scripts` on L2 and L11, a non-zero count is logged as a bug
  -, the hold armed, the piece x local-body filter on; `make_trafo_set` at
  `add_player`, a kept set reselected, a failed build fails the session;
  `apply_trafo` before the player lookup, since a set outlives its member's
  slot; the trafo frame after `update_sectors()`; `after_trafo_tick`: the
  hold (reports logged as a bug), the effect checks, the body-set flag at a
  reset's R + 132, retirement destroying at the deselect; the ledger and the
  portable v2 record in `capture_resync_state`), `sim/trafo_hooks.hpp` (the
  hooks factory, moved out of `trafo_tool.hpp` for the world and T3),
  `sim/session_runner.*` (`create_session(..., sector_lead, world_options)`;
  `submit_event` takes a Trafo through `trafo_acceptance`; `queue_trafo`
  queues every decision and relays the server's own resets with token 0 and
  a journal note; the deadline after every step with N = the next tick and
  L = `sector_lead`; the leaver's final resets in `remove_player`; the
  journal header's world options), `server.cpp` (config, SessionStart,
  validation at intake with `reject_trafo` - REJECTED to the owner alone,
  reliable plus the protocol-6 repeats, and the cheating-or-corruption log
  line with player, stamp, server tick, position, nearest trafo and its
  distance, the piece type and the player's last ball type, session -, the
  refusal at SessionReady, `on_event_accepted` for the runner's Trafo
  events, the ledger into SessionResyncState), `config_manager.*`
  (`physics.trafo_inputs`).  SimTool: `--replay-session` builds the world
  with the header's options and applies Trafo EVENT records;
  `--synth-trafo-session DIR` (S10's journal producer, `sim/trafo_synth.hpp`);
  `--trafo-record-check` (5), the portable round trip.
  `scripts/journal_trace.py` reads v5.
- **Adaptations** beyond the numbers and the two above:
  - The leaver path (2.2 "Leavers") queues the leaver's Unphysicalize, when
    it has a ball, whenever the option is on - its final resets always
    follow - so the ball goes at the same tick everywhere; before, a leaver
    with nothing queued was dropped with its slot at once.
  - `finish_leave` keeps the player's Trafo events: a player that rejoins
    before its old self is removed would otherwise drop final resets every
    other world was relayed and applies.  Consequence (every world alike, by
    the retirement rule): those finals mark the kept set final, it is
    retired once inert, and the rejoined player's next explosion finds no
    selected set and gets one by `ensure_trafo_set` (review fix-up below).
  - A Trafo the runner refuses (option off, bad type - the network side
    refuses these first) is handed back `rejected` and echoed REJECTED to
    its owner alone, never queued.
  - The server's hold polls with option E off (it has no own ball).
- **Cost.** The hold and the effect checks after every tick: 101-118
  us/tick on L11 with three sets (the effect check's 51 by-name lookups),
  `trafo_after_us` in `describe`.
- **Not done here (T3/T4):** no client declares `CAP_TRAFO_INPUTS`, sends,
  applies or journals Trafo events, or reads the ledger; the client
  journal's RESYNC record carries no ledger yet (T3 adds it as a trailing
  field, which the format allows without a version); the config default
  flips when both hosts run the inputs.  (The implement round recorded here
  that a late joiner misses a Trafo event accepted before it joined and
  applied after its state's tick S, "unreachable in practice" with a 40-tick
  lead; the reviews showed the bound wrong - a re-explosion moved behind a
  reset lands up to the lead + `kTrafoResetSpan` = 140 ticks after its
  acceptance - and the fix-up below closes it.)
- **Checks** (x64 `build-agent-rTR2`, every run its own process, outputs in
  `.local-tools/work/tr2/`):
  - 7.1: **493 tests pass** (486 + 7: the protocol and bits, SessionStart's
    world options, the Trafo event's round trip and truncations, the key
    (explode / reset / server-stamped apart; Physicalize's SPAWN flag not in
    a key), the resync state's ledger (round trip, truncations, forged count,
    bad type), journal v5 (header, a v4 header reads 0, the Trafo EVENT), the
    validation table).  The acceptance rules were T1's tests; here they run
    through the real runner (below).
  - S10 on synthetic journals: `--synth-trafo-session` on L2 and L11 (3
    players, 1560 ticks, snapshots off, checkpoints every 20 ticks): 17/17
    echo checks PASS on both - on time (40, 60, 100, 620), a live type set
    exploded again (the server's reset at N, 180/181, the explosion at
    reset + 140), the owner's reset (300), the next explosion moved to
    300 + 140 = 440, a reset older than it superseded, a late reset moved to
    N (502), a bad type refused, the leaver's three finals at 621 (its wood
    explosion at 620 + 1), the deadline reset of a never-reset paper at
    60 + 1320 = 1380, queued at N = 1360 (= 1380 - L), none for a reset type
    set; every reset's pieces gone from the checkpoint after R + 132; the
    leaver's set deselected and destroyed at 818 (inert after its collision
    sound's 3 s); hold 0, `trafo_original_leaks` 0, every set's digests
    `90f856e0c8c20dd4/493` and `bf8575545ab7fbe0/597` (T1's).  Their server
    journals replayed: **L2 1575/1575 matched, 79 checkpoints, 0
    mismatches; L11 1575/1575, 79, 0** - every piece of every set in the
    checkpoints (up to 51 + 16 piece rows).
  - The off path: the six core_ref_r3 server journals (v4) replayed by the
    new SimTool: swing 4004/4004, a6m 4204/4204, resync 4178/4178, a5u
    4193/4193, resync_at150 4058/4058, resync_at150_down 4182/4182 matched,
    0 checkpoint mismatches each (A4's numbers).  Solo snapshots off
    (`e0_solo.sh rTR2`, the Arch server rebuilt with the tree): IDENTICAL to
    `rTR1b` (2330 TICK hash + pose common, 0 differ; LOCAL 1165/1165), its
    server journal 2329/2329 matched.
  - `--trafo-record-check` (L2, 2 sets): (1)-(4) PASS as at T1 (43968 /
    62096 bytes), (5) portable v1 49632 / v2 70392 bytes, v2 localized ==
    record v3, v1 with the trafo part == the v2 record, v2 refused without
    a trafo part.
  - `wire_check.sh rTR1 rTR2 rTR2w`: the 24 protocol-10 captures are refused
    at SessionStart by the protocol-11 build ("the server runs a different
    physics protocol than this client (11)"), as expected; not re-captured
    (T3 does, when the headless host speaks protocol 11).
  - Builds: x64 BUILD-OK, x86 CLIENT-BUILD-OK (the client's objects deleted
    first; map.hpp untouched), Arch ARCH-BUILD-OK; `linux_test_syntax.sh` 27
    ok.
  - Observed, every world alike: a reset applied while the same type
    set's previous reset is still fading restarts the fade (the reset
    script is activated with Reset), so the bodies go at the later reset's
    R + 132.  In the synthetic run p103's stone reset at 500 and its leaver
    final at 621: the stone pieces went at 753, not 632.  The ledger's
    guarantees hold (the span is counted from the last reset); a leaver's
    final reset of a fading type set extends that fade by up to 132 ticks.
- **Review fix-up (two reviews, 2026-10-25).**
  - **The ledger names every set the server holds** (review 1 #1).  4.7's
    filter (bodies, a live explosion, or a reset before its Restore IC) left
    out a member's set that never exploded and a leaver's set between its
    final resets and its retirement (about 230 ticks), while the portable
    record lists every selected set's scripts and `localize` is strict both
    ways: a joiner or resync in that span could not take the state.  It
    also never said a set was final, so a client that missed a
    `TRAFO_FINAL` reset would never retire the set the server retires.
    `capture_trafo_ledger` now writes the three type sets of every set the
    world holds (the server destroys at the deselect, so every held set is
    selected) and each entry carries `final` (bit 8) and `final_applied`
    (the set's newest final reset; u32 after `reset_applied`).  The cap stays
    3 x 2 x 64 entries (128 sets: the members and the leavers not yet
    retired); a ledger over it is logged with the other cut sections.
    Client side (T3): build a set for every player named, mark the final
    type sets with `trafo_set_final(..., final_applied)`.
  - **One rule for "a set for this player"** (review 1 #2).
    `game/trafo_sets.hpp` `ensure_trafo_set`, which the server's
    `make_trafo_set` (at `add_player`) and `apply_trafo` (before every
    input) call and T3's hosts must call: a selected set is kept; a retired
    set not yet destroyed - only a client holds one, history_ticks after its
    deselect - is selected again AND put back to what a fresh build is
    (every type set unmade: scripts to their built bytes, the set being
    inert nothing else goes; every copied object's initial condition, which
    the build saved, read back; the final marks and tick cleared); otherwise
    a fresh build.  Before, the server built fresh where a client would have
    reselected a set whose scripts kept their finished state.  Measured
    (`--trafo-ensure ID TICK`, new; `work/tr2/ensure_check.sh`, L2, one set,
    wood explosion at 300, finals at 400, deselected at 533, destroyed at
    629 with the client's history; the same explosion again at 760): arm A
    ensures at 580 (reselected, 61 initial conditions read back), arm B at
    720 (built afresh); the scripts' value digest equals the build's
    (`bf8575545ab7fbe0/597`) in both, and the 240 per-tick pose lines after
    760 (17 cores, 16 piece bodies) are **identical**.  The id stays alive
    for the records that still list it (4.5: a restore refuses a record
    naming a destroyed script), which is why the reselect is not a destroy
    and rebuild.
  - **Late joiners and resyncs get the Trafo events applied after S**
    (review 1 #3, review 2 #2).  The server keeps every relayed, applied
    Trafo event with its applied tick (`trafo_relayed`, pruned after 4 x
    1320 ticks, capped at 4096) and resends the ones applied at or after the
    base with the join's and the resync's catch-up (`resend_trafo_relayed`,
    next to `sector_history`); a member that has one drops the copy by its
    key.
  - **Smaller fixes.**  A reset's position is zeroed at intake, so the relay
    and every journal carry the zeros the server's own resets carry
    (review 1 #4).  A Trafo event over the rate limit is answered REJECTED
    to its owner (`reject_trafo`, reason "more events per second than the
    rate limit"), never dropped silently (review 1 #5, review 2 #3).  The
    portable record is version 2 whenever the option is on, an empty trafo
    part included; `localize` takes it with an empty local list and refuses
    it only when the world has no trafo list at all (review 1 #6).
    `journal_keep_last_events` keys a Trafo event with its flags, as
    `lifecycle_event_key` does: the server's reset at a re-explosion
    (origin = applied) and an owner's event of that type stamped for that
    tick were one key and the replay kept only the last (review 2 #1; unit
    test).  SessionStart's seed is the runner's `runner_config::seed`, the
    value every server-side seed derives from, not a second literal 1
    (review 2 #5).
  - **Not exercised by any check yet** (review 1 #7): the network side -
    the intake rejection and its log line, the REJECTED echo, the relay of
    the server's own resets, the refusal at SessionReady, the resend above.
    The synthetic sessions drive `session_runner` directly; the wire path
    runs first when T3's headless host sends Trafo events (its harness
    checks and H7).
  - **Gate round checks:** **494 tests pass** (493 + the journal key's;
    the ledger test now covers the final bit, the final tick and an unknown
    bit); `--synth-trafo-session` on L2 and L11 **22/22 PASS** each - the
    17 echo checks unchanged and 5 new ledger checks on a resync state of
    S = 659 (9 entries: p103's three final at 621 with its wood live and
    stone fading, p101's never-exploded stone and paper, p102's live paper)
    -, their server journals replayed **1575/1575 matched, 79 checkpoints,
    0 mismatches** each; `--trafo-record-check` (1)-(5) PASS as before plus
    (5b) (an empty part: portable v2 49636 bytes, localized with no set,
    refused by a world holding sets); the ensure check above; x64 BUILD-OK,
    x86 CLIENT-BUILD-OK (the client's objects deleted first), ARCH-BUILD-OK,
    `linux_test_syntax.sh` 27 ok.  Not rerun, since nothing the fix-up
    changed reaches them: the wire check (the protocol-10 captures are
    refused at SessionStart before any changed code), the core_ref_r3
    server-journal replays and the solo snapshots-off run (every change is
    behind the option or keyed to the Trafo type; the SessionStart seed is
    the same 1).

**As built (step TR3 = T3, 2026-10-25; HEAD 074f6d5 + the working tree, not
committed: the CPU gate below stops the step).**  The core and the headless
host run the trafo as an input; the retail host (T4) does not declare the
capability yet, so a retail core-on client is refused by a server with the
option and runs the step P path without it.

- **Where.**  `session/client_platform.hpp`: the capability
  `session_capabilities::trafo_inputs` and its operations `poll_trafo` (the
  hold's reports and the effect checks), `make_trafo_set` (`ensure_trafo_set`),
  `run_trafo`, `trafo_final`, `trafo_frame`, `trafo_type_set` and
  `trafo_reconcile` (the reconcile's view and halves), `retire_trafo_sets`
  (returns the deselected players); `missing_platform_operation` requires
  them with the capability.  `session/client_core.hpp` state (`trafo_log_`,
  `trafo_sets_`, `trafo_pending_`, `input_rewind_trafo_`, the counters) and
  `src/session/client_core_trafo.cpp` (new): `ensure_trafo_set` (first
  sight), `hold_trafo` / `hold_pending_trafo` / `poll_own_trafo` (stamp
  `tick + 1 + lead`, under option E the Delayer's prediction when it is not
  earlier than that, before the assignment on the first assigned frame at
  `tick + lead`), `on_trafo_event` (remote and server-stamped inputs logged
  at their applied tick, late ones rewound), `trafo_rewind` (the frame's
  input rewind; past the history or the reach `trafo_missed` and a resync;
  before a pending exact resync's anchor or the numbering's base: the
  state's), `apply_trafo_inputs` (live in `pre_step` after the held
  lifecycle and before the union pass, the trafo frame after the pass; in
  every replayed tick from `before_replayed_tick`; a `TRAFO_FINAL` input
  marks its type set final), `process_trafo_echo` (MOVED re-keyed with the
  applied tick's seed and rewound from the earlier tick; REJECTED /
  SUPERSEDED dropped and rewound from the origin; a server-stamped reset of
  the own set is an input, not an echo), `reconcile_trafo`, `retire_trafo`
  (retirement and the log's pruning after the live frame).  SessionReady
  declares `CAP_TRAFO_INPUTS` with the capability; `begin` turns the inputs on
  for `WORLD_TRAFO_INPUTS` and the capability; `anchor` builds every member's
  set and the own one.  The status line gains a `trafo:` section in such a
  session only (`trafo_missed=`, `trafo_original_leaks=`, ...).  The
  journal's RESYNC record gains a trailing trafo section (the ledger entry and
  the reconcile's action per type set, `JOURNAL_TRAFO_*`), written only with
  the inputs on; `journal.hpp` and `scripts/journal_trace.py` read it.
  `session/rollback.hpp`: `resync_spec::gone`.  `game/trafo_sets.hpp`:
  `add_trafo_set_pieces`; `trafo_set_inert` (below).
  `game/session_host_support.hpp`: `localize_session_scripts` passes the
  trafo part.  Headless host (`sim/session_client.cpp`): the registry, the
  hooks (`sim/trafo_hooks.hpp`), the hold and the operations; the level
  reference captured while the level loads; the anchor's quiesce, hold and
  piece filter; the selection without the originals and the binding's trafo
  part with the option; teardown at the session's end.
- **Adaptations** (no principle touched):
  - **The reconcile runs before the resync's restore, on the live world**
    (design 4.7 runs it inside the resync after the anchor's restore).  The
    row mapping needs every body the server's rows name, so the core builds
    the ledger's sets first, leaves the ledger's piece rows out of the wait,
    makes every type set what the ledger says (unmake when this world has
    bodies the server does not, or another generation; remake with the
    ledger's position and seed, muted, the births logged at S by the host),
    then maps the piece rows by name; `resync_spec::gone` carries every piece
    of a type set the server has no bodies for, so the restore plans nothing
    for it and does not revive it.  "This world's generation" is its newest
    explosion applied by now and whether every piece has its body.
  - **Step P's trafo activations and referential override stay for a session
    without the option** (the legacy retail path, a core host without the
    capability, the default config until T4): with the option the originals
    are not selected, so neither ever acts (comment in
    `session_host_support.hpp`).  `explosion_pieces` over every set is the
    host's `add_trafo_set_pieces` (for T4 too).
  - **`trafo_set_inert` does not count the collision sound**: after a stone
    and then a paper explosion of one set, `Ball_CollSound_Pieces`'s paper
    branch Wave Player stays active for good in a null-sound world (server,
    headless; SimTool L2 stone 300, paper 650, finals 1326: active at 1750),
    so such a leaver's set never retired (found by H8 (a)); a real sound
    manager (retail) ends the sound, so the decision also depended on the
    world's sound device.  The server was rebuilt with it.
  - `CAP_TRAFO_INPUTS` is declared whenever the host can, whatever the
    world option.  A failed set build is logged and counted (`made=x/y`), not
    fatal for the client.
  - H5 is judged per arm against that session's own server journal (each
    solo session's player has another id, so its seeds and its pieces differ
    from the control's); the non-piece bodies against the control as before.
  - Harness switches (headless): `--no-trafo-cap` (H10), `--trafo-no-resets`
    (H11), `--leave-at`, `--leave-after-unphys D`, `--leave-after-trafo D`
    (H8), `--beam2` (a paper ball onto a wood trafo), `--ignore-snapshots`
    (snapshots truly off, below), `--hold-event trafo:...`.  Tools:
    `worst_bodies.py --trafo-by-type`, `first_divergence.worlds(trafo_by_type)`,
    `savestate_check.py` keys pieces by type, `arch_server_lp.sh` eighth
    argument `trafo_inputs`.  The headless clients announce slot 13's
    original hash, so the Arch server for headless runs is built without the
    slot-13 hack (the hack is for retail runs).
- **Checks** (x64 `build-agent-rTR3`, Arch server rebuilt from the tree; runs
  `maps/arena-test/tr3_*`, helpers `.local-tools/work/tr3/`):
  - 7.1: **503 tests pass** (494 + 9: the capability and its operations, sets
    at the anchor and on first sight never in a replay, the hold's stamps and
    deferral with option E, live and replayed application of a late input
    with the retail switches, an input past the history asks for a resync,
    MOVED / REJECTED / SUPERSEDED / acked echoes and a server-stamped own
    reset, the reconcile's table (equal, extra, different, missing, a
    never-seen player, a final), retirement and the frame order).  GCC 28 ok.
    x64 BUILD-OK, x86 CLIENT-BUILD-OK (client objects deleted first),
    ARCH-BUILD-OK; map.hpp untouched.
  - The option off (the BAR): solo snapshots-off `e0_solo.sh rTR3` IDENTICAL
    to `rTR2` (2330 TICK hash + pose common, 0 differ; LOCAL 1165/1165),
    server 2413/2413.  `core_ref_r3` (protocol 10) is refused by both builds;
    `core_ref_t3` re-captured with the option on: 24/24 sends SAME, 24/24
    journals IDENTICAL, BYTE-IDENTICAL-EXCEPT-BUILD-ID 24 pairs (its README).
  - **H1, 0 ms** (`tr3_H1_0`): verdict PASS, resyncs and rebases 0, late 0,
    `trafo_missed` 0, leaks 0, every client holds all four sets' bodies (body
    sets equal at every checkpoint).  Pieces 72-111 of 150 bit-equal; the
    others drift after snapshot rollbacks (R2): c0 and c1 both roll back at
    1910/1911 (a mirror 1.95 m off) and Paper piece13 is off from 1926 on all
    four clients alike, up to 24 m under the wind.  **With
    `--ignore-snapshots`** (`tr3_h1_0i`): **150/150 pieces bit-equal on all
    four for the whole run**.
  - **H1, 150 ms** (`tr3_H1_150`): **FAIL (R1)**: every client rebases once
    (1223-1252) and resyncs; starved 356-451.  After each apply the pieces are
    bit-equal (`tr3_h1_150i`, snapshots ignored: 168/168 on all four from the
    apply on; the drift is inside the window before the apply).
  - H2 (`tr3_H2`, beams 650/900/1150/1400 onto wood / paper / stone / wood):
    PASS, 0 resyncs; pieces 78-81 of 83 bit-equal, Stone piece03 0.035 m from
    1270 on all four after the snapshot rollback at 1151 (R2).  No same-player
    same-type repeat within 20 s happened.
  - H3 (`tr3_H3`, 10 % loss): rebase and resync on all four (R1); late 0,
    missed 0.
  - H3b (`tr3_H3b`): client 2 `trafo_missed` 2 (the explosion at 1059 arrived
    at 1121, the reset at 2006 at 2281), two resyncs; the reconcile remade
    (S 1148, Stone of 3952298754) and unmade (S 2303, 17 bodies the server no
    longer had); after each re-step 50/50 and 33/33 pieces bit-equal; the
    missed reset's pieces stood in the window (2138, 2280] and were gone
    after the apply; the other clients 50/50 bit-equal throughout.
  - H4 (`tr3_H4`, snapshots 2/66, 150 ms): rebases 2-4, resyncs 1-4,
    unmatched 74-632 (R1); piece corrections 1, 1, 0, 0; mism 17-67.
  - H5 (`tr3_h5`): control plus 13:24 (103 forced), 50:12 (28), 97:24 (14),
    131:47 (10): **every piece 33/33 bit-equal to its session's server
    journal in all five, no "PreSimulate queue differs"**; the non-pieces
    equal the control but P_Modul_01_Pusher 0.0866 m from 656 in 131:47 (R4).
  - H6 (`tr3_H1_150` against `tr3_H6_ctl`, the same recipe and binary with
    the option off): t_cap 3.43-3.62 s vs 1.30-1.37 s, t_rep 5.5-7.6 s vs
    2.3-3.5 s, t_max 98-132 vs 43-64 ms, starved 356-451 vs 0-3 (R1).  The
    server's tick p99 was not measured (no per-tick timing line in this
    configuration).
  - H7 (`tr3_H7`, two paper trafos at 1159 / 1221, client 3 joins at 1550):
    the join's reconcile remade 6 type sets (both paper ones, wind included);
    after the join's re-step 102/102 pieces within **6.6e-7 m** (222 non-zero
    body-ticks; R3); the members' own resyncs (R1) left their pieces exactly
    equal.
  - H8 (`tr3_H8b`): (a) a leaver with live stone and paper sets (left 1300,
    finals 1834), (b) one without a ball (left 757, finals 1288), (c) one that
    left at 853 after stamping its explosion for 919 (finals 1400): every set
    deselected at the same tick on the server and the client (1421, 1533,
    1967), destroyed at once on the server and 96 ticks later on the client
    (1517, 1629, 2063); the last piece rows are R+131 (1532, 1966); (c)'s
    explosion ran everywhere at 919.  (Before the inertness change (a)'s set
    never retired.)
  - H9 (`tr3_H9`, solo, `--no-rollback`): TICK records 600-1500 equal on 899
    of 901 ticks; 651-652 differ by 1e-13 m on the own ball (the beam's
    in-place write, not the trafo); the trafo's window (808-1500) is equal.
  - H10 (`tr3_H10`): the member without `CAP_TRAFO_INPUTS` is refused ("no
    CAP_TRAFO_INPUTS (caps 00000003) in a session with trafo inputs"),
    SessionEnd with the reason; the option-off path is the solo check above.
  - H11 (`tr3_H11`, `--trafo-no-resets`): the server's deadline resets at
    2129, 2479, 2845 (explosion + 1320), queued at that tick - lead and
    applied in both worlds, the owner's included; pieces gone at R+132 (2261,
    2611) everywhere; 51/51 pieces bit-equal on both clients, the paper wind
    included.
- **Residuals** (each root-caused; R1 is this step's, so the step stops):
  - **R1, CPU.**  Every world now simulates and records every set's pieces.
    A record's cost grows with them: the IVP savestate blob (solo, 33 pieces:
    80 -> 125 KB, 0.25 -> 0.55 ms per capture), the per-body getters (three
    CK by-name lookups per body per capture: about 45 % of a capture at T3's
    body counts in the 4-client run), and the 32 trafo scripts in every
    record even when dormant (+10-15 % on the reference arms with no
    explosion).  Every live tick captures twice (record and amend), every
    replayed tick once.  After four stone trafos within ~1 s (68 awake
    pieces) the 150 ms arms rewind every frame for ~1 s (input corrections
    of the new balls, each ~17 ticks deep); at ~2.5 ms per replayed tick the
    frame loop falls 33 ticks behind and the schedule rebases (the CPU cliff
    of roadmap 4 item 8, which the option-off control does not reach).
    Options: (a) cut the capture cost - the dormant-script encoding of 9.4,
    one live capture per tick instead of record + amend, a name -> object
    cache for the getters; (b) an incremental savestate (sleeping bodies'
    state reused); (c) the CPU-cliff item first; (d) judge the 150 ms arms
    with fewer simultaneous trafos.
  - **R2, the snapshots-off configuration still snapshots.**
    `snapshot_cadence::decide` sends a full snapshot on every body-set change
    whatever the interval, and every explosion and De Physicalize is one in
    every world now.  A pre-existing ball mismatch (the fan blowing only the
    owner's paper ball, record-format design A4; mirrors at 150 ms) then
    triggers a snapshot rollback that writes every row, the pieces' included,
    losing what a row cannot carry; the pieces drift from there (the first
    divergence of each client follows its first such rollback: c1 at 832 ->
    836, c0 / c2 / c3 at 1203 -> 1208-1212 in a 0 ms run).  With the
    snapshots dropped every piece stays bit-equal (H1 above).  Options: no
    body-set full snapshot while snapshots are off; or a snapshot rollback
    that writes only the mismatched rows (snapshot-path work, which the
    roadmap defers to the stage close-out).
  - **R3, a joiner's remade paper pieces, 6.6e-7 m.**  The travelling
    savestate names force controllers `rt:force/<block id>` (engine change
    #18), and block ids differ per world: the joiner's restore reports "no
    live controller 'rt:force/54491'" and "live controller 'rt:force/52773'
    is not in the savestate" for every remade wind controller, so the server's
    controller state and order are not restored on the fresh cores.  Options:
    a world-independent controller key (the portable record's block identity)
    in the travelling restore.
  - **R4 (pre-existing).**  H5 131:47's P_Modul_01_Pusher 0.0866 m from 656,
    reproduced on rTR2 with the option off (`tr3_h5c`): the trafo's
    Unphysicalize at 653 starts a new own life two ticks after the beam at
    651, the beam's record is retired ("own beam stamped 651 retired ...
    generation 1 -> 2"), so a 47-deep rewind replays without the beam and
    the ball passes P_Modul_01's proximity.  Rollback-engine gaps (item 8).
  - **R5 (pre-existing).**  The balls diverge from the server in these
    recipes (the fan, mirrors at 150 ms), identically on rTR2.
- **Not done:** T4 (retail), T5; the config default stays off.

**As built (step TR3, fix-up round, 2026-10-25; still HEAD 074f6d5 + the
working tree, not committed).**  The reviews' findings, each confirmed in
the code; the implement round's verdicts above stand corrected here (H1 at
0 ms and H2 failed their criterion (a): pieces 72-111 of 150 and 78-81 of 83
bit-equal is not "every piece").

- **CPU (R1).**  Profiled with an in-process sampler (the host has no admin
  rights for ETW) and phase timers, H1 at 150 ms, client 0: a replayed tick
  cost ~2.5 ms, of it ~0.06 ms the physics step and ~0.02 ms the scripts;
  the rest was the record's capture (bodies 1.0 ms: three by-name scans of
  every CK 3D entity per tracked body; blob 1.5 ms: capture 0.9 + the byte
  writer 0.55; scripts 0.23) and the replay's blob diff (rendering two
  ~170 KB blobs as text on every replay that corrected an input: a twelfth
  of the frame time, for log lines past their cap).  Changes, none of which
  changes a byte of any record or blob:
  - `physics_state.hpp` `entity_lookup_scope_begin/_end` and
    `rollback_world::capture_scope` (bound by `rollback_binding.hpp` for a
    backend with `lookup_scope`, the headless `physics_direct`): inside a
    record's capture the by-name lookups after the first 12 are served from
    one pass over the 3D entities (open addressing over the entities' own
    names; the first entity of a name in `GetObjectByNameAndParentClass`'s
    scan order).  Checked live against the scan: 1.1-1.5 M lookups per
    client, 0 different.  Retail (physics_view) does not bind it.
  - The savestate capture (`physics_savestate_capture.cpp`,
    `_common.cpp`, `_internal.hpp`): pointer tables (`ptr_index`, open
    addressing) for the context's and the live index's `std::map`s, a
    friction-handle table for `describe_controller` (it scanned every
    system per controller), records moved instead of copied, error-context
    strings built only on a failure, the index without its name multimap
    for a capture, reserves sized by the thread's previous capture, the
    byte writer through a write position (no vector insert per field) and
    its `io` overloads declared inline (/Ob1).  Checked live by capturing
    every blob twice, with the new code and with the original kept
    alongside: 6000-8000 blobs per client on all four, 0 byte different.
  - `script_replay::capture` reserves its record (the previous one's size).
  - The replay's blob diff (`rollback_world::diff_savestate`, headless
    diagnostic) runs only while its 8 log lines last
    (`savestate_replay_mismatches` is read by nothing; now counted while
    logged).
  - One capture per live tick (`rollback_engine::record(..., capture_now)`):
    the core's post_step records without capturing and its amend, which
    re-captured the whole record anyway and before which nothing reads it,
    captures once.  A host that reads the record in between (`after_record`,
    the headless client's `--dump-savestate` / `--compare-scripts`, now
    installed only with those switches) keeps the capture at the record.
    `stats().captures_never_made` counts a deferred record no amend
    captured (0 in every run).
  - Per capture at the explosions' peak (~120 tracked bodies): 2.7 ms ->
    0.9 ms (bodies 0.2, scripts 0.21, blob 0.5).
- **Snapshots off still snapshotting (R2): fixed for the pieces.**
  `rollback_world::row_on_breach_only`: the core marks every set's pieces
  (`trafo_pieces_`, from `trafo_type_set` at the make, dropped at
  retirement); such a body's snapshot row is compared like any row but is
  a restore source only when it breaches (an awake row inside the
  tolerance used to be written, and a row - pose and velocities, none of
  the collision state - is what made exact pieces drift).  A breaching
  piece is still restored from its row.  Rollback test
  `AnInputDrivenBodyTakesItsAwakeRowOnlyOnABreach`.
- **Retirement inside the rollback window (review 1 #3).**
  `retire_trafo_sets` deselects a set only after it has been inert at every
  check for `history` ticks (`trafo_set::inert` / `inert_since`; reset when
  it is live again, and at a reselect), on the server and in every client
  alike: a rewind can no longer anchor where a deselected set was live.  H8's
  deselects move from R+133 to R+229 (1517, 1630, 2065).  This changes the
  server's retirement (and the collision-sound rule of the implement round
  already did), so server journals of T2 and of the implement round do not
  replay on this server.
- **The paper wind across worlds (R3).**  `physics_savestate.hpp`
  `set_controller_alias`: per physics manager, a name the capture records a
  controller under and the restore matches live controllers by.  The
  registry keeps every set's force blocks in script and walk order
  (`trafo_set::force_blocks`), and `trafo_controller_alias` maps
  `rt:force/<block id>` to `trafo:<player>/<k>`, which every world gives
  the same block.  The server's world (with the option) and the headless
  client (at its trafo anchor; removed at the teardown) install it; T4 must
  install it in the retail host.  Not an engine change (physics_RT still
  names the controller after the block).
- `caps.trafo_inputs` needs the script replay (`--no-script-replay` never
  bound the sets' scripts into the replay); the replayed tick applies the
  trafo inputs before the union's re-derivation, as the live tick does;
  a destroyed set's pieces leave the headless client's `explosion_pieces_`.
  The RESYNC record's trailing trafo section is read by "4 or more bytes
  left": it has to stay that record's last field (5).
- **Server step time.**  `session_runner` logs `step time over ticks a..b:
  p50 / p99 / max` every 660 ticks (H6's server criterion; the TICK records'
  wall-clock gaps measure the scheduler, not the step).
- **Checks** (x64 `build-agent-rTR3f`, Arch server rebuilt from the tree;
  runs `maps/arena-test/tr3g_*`, helpers `.local-tools/work/tr3fix/`;
  pieces judged bit for bit - position, rotation, both velocities - against
  the server journal's rows from the arrival of the event that made their
  state, `pieces.py`):
  - 7.1: 505 tests pass (503 + the two above).  GCC 28 ok.  x64 BUILD-OK,
    x86 CLIENT-BUILD-OK (client objects deleted first), ARCH-BUILD-OK.
  - The BAR, option off: the six arms captured with `build-agent-rTR2`
    and the option off (`maps/arena-test/core_ref_t3off`), replayed with
    rTR2 and with the fix build: 24/24 journals IDENTICAL, every send equal
    but the SessionReady (59 bytes: the build id and the new
    `CAP_TRAFO_INPUTS` bit, T3's declaration); solo snapshots-off: 2333
    TICK hash + pose common with rTR2's, 0 differ (one earlier solo run
    rebased at 2182 with no rewind in flight, a host stall, and is not
    counted).
  - H1, 0 ms: PASS, 0 resyncs / rebases, late 0, missed 0, leaks 0;
    **pieces 132/132 bit-equal on all four**.
  - H1, 150 ms: PASS, 0 resyncs / rebases, starved 0 / 16 / 15 / 0, t_max
    42-55 ms; **pieces 150/150 bit-equal on all four**.  (Merge round
    correction: (c) "starved within the control band" was not met - the
    control starves 0; see the merge round below.)
  - H2: PASS, pieces 83/83 on all four.  H3 (10 % loss): PASS, 0 resyncs /
    rebases, pieces 132/132; no late Trafo event.  H3b: client 2 missed the
    explosion at 1059 (arrived 1121) and the reset at 2006 (arrived 2281),
    two resyncs (remade, unmade); after each re-step (1186-2280,
    2342-end) its pieces 50/50 bit-equal; the others 50/50 throughout.
  - H5 (control + 13:24, 50:12, 97:24, 131:47): every piece 33/33
    bit-equal to its own session's server journal in all five, no
    "PreSimulate queue differs"; the non-pieces equal the control but
    P_Modul_01_Pusher 0.0866 m from 656 in 131:47 (R4, pre-existing).
  - H7: the joiner's reconcile remade 6 type sets; from the re-step on
    **102/102 pieces bit-equal on all four** (the 6.6e-7 m is gone), no
    controller left unmatched.
  - H8 (150 ms): the three leavers' sets deselected at the same tick on the
    server and the client (1517, 1630, 2065), destroyed 96 ticks later on
    the client; the last piece rows R+131 (1532, 1966).
  - H9: TICK 600-1500 equal but 651-652 (the own ball 9.65e-14 m off after
    the beam at 650); the same two ticks differ, with the same server
    hashes, on rTR2 with the option off - pre-existing, the beam's.
  - H11: deadlines at explosion + 1320 (2129, 2479, 2845), owner included;
    pieces 51/51.
  - **H4 (snapshots 2/66, 150 ms): FAIL.**  Two runs: resyncs on 3 and 4
    clients, starved 68-167 and 154-942, t_max 62-80 ms, t_rest 0.56-1.6 s,
    t_rep 3.3-5.7 s; pieces of the clients that did not resync bit-equal,
    the resynced ones' from their apply.  The same recipe with the option
    off, same binaries: PASS, starved 0, t_max 15-23 ms, t_rep 1.4-2.5 s.
  - **H6: FAIL.**  The row's control (the H1 batch onto bay 3, no trafo at
    all; snapshots at interval 2): t_cap 0.89-0.92 s, t_rep 0.5-1.4 s,
    t_max 13 ms, starved 0, server step p99 2.3-3.8 ms.  The trafo arm (H4):
    t_cap 1.46-1.56 s, t_rep 3.3-5.7 s, t_max 62-80 ms, server step p99
    2.8-9.1 ms.  The server stays inside its budget (p99 <= 9.1 ms of 15.15)
    but not within the control's band + 10 %, and no client figure is.
- **Root cause of H4 / H6.**  Every world records, restores and replays
  every member's pieces: a replayed tick costs 0.8-1.0 ms with four sets'
  pieces against 0.35 ms without (the blob 85 -> ~170-200 KB, ~120 more
  tracked bodies, their collisions in the restore), and with snapshots at
  interval 2 a mismatch of a predicted remote ball rolls the whole world back
  every few ticks; the peak rewind is 3-4 times the option-off one, and the
  frame loop falls behind the schedule during the four explosions.  The
  remaining capture (0.9 ms) and restore are the budget; the server's own
  cost is T2's.  Options: (a) a cheaper record for bodies that did not move
  (an incremental savestate: sleeping pieces' records reused) and the
  dormant-script encoding of 9.4; (b) judge H4 / H6 with snapshots off, the
  roadmap's primary configuration (H1 at 150 ms passes there - merge round
  correction: its (c) starvation does not), with the
  snapshot fallback's cost tracked by the snapshot-removal item - a change
  of the gate, the user's to make; (c) a snapshot rollback that does not
  re-step the trafo pieces when only a predicted ball breached (the pieces
  are exact by their inputs) - an engine rule change with its own checks.

**As built (step TR3, merge round, 2026-10-25; HEAD 49e2cc8 + the working
tree).**  The user chose option (b) above (§11.1): T3 is judged with
snapshots off, H4 / H6 move to the rewind-cost optimization
(`docs/roadmap.md` §4 item 10).  The parked work (02bbcf6, branch
`wip/trafo-t3`, the implement and fix-up rounds above on 074f6d5) was
brought onto HEAD 49e2cc8 (S1, S2, S3, S9, EG0, K0 had landed since).

- **The merge.**  `git cherry-pick -n 02bbcf6`: no conflict, the same 37
  files and +2915/-175 as the parked commit.  The one file both sides
  changed is `sim/physics_world.cpp`: S2's placement release and T3's
  controller alias in the destructor, T3's alias install after
  `load_composition` - separate hunks, both kept.  No code was changed
  after the merge.
- **A build-tree pitfall, not a code change.**  The x64 tree's
  `BallanceMMOSessionClient.dir/.../session/client_journal.cpp.obj` had an
  empty ninja dependency list (`ninja -t deps`: `#deps 0`, an object left by
  an interrupted build), so T3's change of `client_core_state`
  (`client_core.hpp`) did not rebuild it: the headless client wrote no
  journal (the stale object read the platform at the old offsets) and every
  capture replay stopped at the anchor ("expected a clock read at
  run_report, the capture has one at journal_utc").  The object was deleted
  and rebuilt; the x86 tree's `physics_rt_bridge.cpp.obj` and
  `ball_navigation.cpp.obj` (physics_RT) had empty lists too and were
  rebuilt.  `ninja -t deps | grep "#deps 0"` finds such objects.
- **`physics.trafo_inputs` stays off** (`config_manager.hpp:89`, the server's
  default).  2.14 and §5 refuse a member without `CAP_TRAFO_INPUTS` in a
  session with the option, and the retail core host declares it only from
  T4 (PSC never): with the option on now, every retail client would be
  refused.  T4 turns the default on (T2's note); its R4 (core off) keeps the
  option off as 7.4 says.
- **Checks** (x64 `build-agent-rTR3`, x86 rebuilt, the Arch server rebuilt
  from the merged tree, id `ballanced-3e4289e95f25+bmmo-49e2cc8e0253-dirty`;
  runs `maps/arena-test/tr3m_*`, helpers `.local-tools/work/tr3/`
  `live.sh`, `g.sh`, `h5.sh`, `ref_batch.sh`; pieces judged by
  `tr3fix/pieces.py` as in the fix-up round):
  - 7.1: **516 tests pass** (505 of the fix-up round + the 11 of S1-S3,
    EG0, K0).  GCC 29 ok.  x64 BUILD-OK, x86 CLIENT-BUILD-OK (client objects
    deleted first), ARCH-BUILD-OK; map.hpp untouched.
  - The BAR, option off: `REF=core_ref_t3off wire_check.sh rK0 rTR3`: 24/24
    journals IDENTICAL; every send equal but one per client, SessionReady
    (59 bytes: the build id and the `CAP_TRAFO_INPUTS` bit).  Solo
    snapshots-off against rTR2's `core_ref_t3off/solo/old1`: IDENTICAL (2394
    TICK hash + pose common, 0 differ; LOCAL 1197/1197).
  - **H1, 150 ms** (`tr3m_H1_150`, `tr3m_H1_150_b`): 0 resyncs, rebases,
    far, unmatched, late; `trafo_missed` 0, leaks 0, 4 sets everywhere;
    **pieces 150/150 and 168/168 bit-equal on all four**.  Starved 21 / 26 /
    18 / 0 and 0 / 1 / 10 / 0: the first run's harness verdict fails on
    client 1 (26 of 2211 ticks, its limit 1 %), the second passes.  Cause,
    the same in both and in the fix-up round's run (15 / 16 / 15 / 0, the
    same window): all four players' wood trafos fall within 1167-1216, each
    new paper ball's frames correct the others' prediction every frame for
    about a second (client 1: input rewinds of 14-30 ticks at 1202-1255) and
    the body-set full snapshots add 4 snapshot rollbacks (R2), so the frame
    loop falls behind and the server substitutes 18-26 own frames per client
    in 1260-1380.  That is R1's CPU cost with snapshots off.  **H1 (c),
    "starved within the control band", is therefore NOT met in any run**
    (the fix-up round's 15 / 16 / 15 / 0 was scored a pass, wrongly): the
    control starves 0 - the H6 control recipe (`tr3m_H6_ctl0`, below) and
    the same H1 recipe with the option OFF on the same binaries and server
    (`tr3m_H1_150_off`, gate round: verdict PASS, starved 0 / 0 / 0 / 0,
    resyncs, rebases, far, unmatched 0, t_rep 1.13-1.63 s, t_max 17-19 ms,
    t_cap 0.70-0.73 s, server step p99 2.2-3.6 ms).  The host was idle in
    all of them (cpu_sampler: max total 10-11 %).  So the starvation is
    T3's own (every world replaying every member's pieces), not
    pre-existing, and decision (b) - which moved H4 / H6, the snapshots-on
    arms, on the premise that every snapshots-off arm passed - does not
    cover it.  **Open, the user's ruling** (T3 is not committed until it is
    made): (i) extend (b) to H1 (c) at 150 ms, which then becomes a close
    criterion of the rewind-cost optimization (roadmap §4 item 10);
    (ii) keep it as T3's gate and do (part of) the rewind-cost work before
    T3 closes; (iii) another control for H1 - not available: the
    option-off arm of the same recipe starves 0 as well.
  - H3b (`tr3m_H3b`): client 2 `trafo_missed` 2 (the explosion at 1059
    arrived at 1122, the reset at 2006 at 2283), two resyncs: S 1149 -> 1187
    (remade Stone of 1389791795), S 2306 -> 2342 (unmade the 17 bodies the
    server no longer had); after each re-step (rows from 1188 and from 2343;
    the rows at 1187 and 2342 are the live frame's before the apply) its
    pieces 50/50 bit-equal; the others 50/50 throughout; the missed reset's
    pieces stood until the apply and were gone after it.
  - H5 (`tr3m_h5`, two invocations of `savestate_check.py --journals`, each
    with its control; server option on, snapshots off): control and 13:24
    (108 forced), 50:12 (26), 97:24 (14), 131:47 (10): **every piece 33/33
    bit-equal to its own session's server journal**, 0 "PreSimulate queue
    differs"; the non-pieces equal the control but P_Modul_01_Pusher 0.0866
    m from 656 in 131:47 (R4, pre-existing, as before).
  - H7 (`tr3m_H7`): the joiner's reconcile remade 6 type sets (S 1690 ->
    1727); **102/102 pieces bit-equal on all four** from the re-step on; no
    controller left unmatched (restore warnings 1, as in the fix-up round).
  - Not rerun (no merged file on their path: of HEAD's changes since
    074f6d5 only the server world's sector placement code reaches a live
    run - its signature at the anchor and its release, the placement itself
    off by default - and the headless client is T3's code alone):
    H1 at 0 ms, H2, H3, H8, H9, H10, H11; their fix-up round results stand.
    No retail run (quick tier): the shared savestate changes (the controller
    alias hook, `ptr_index`, `index_live_world(..., names)`) are compiled
    into the x86 mod, so the core-off BAR rests on the parked rounds'
    evidence; the alias is empty unless installed and only the headless
    world installs it.
  - **The CPU baseline for the rewind-cost optimization** (snapshots off,
    150 ms, 4 clients, totals per client over the 40 s run): H1 (two runs)
    t_cap 1.45-1.70 s, t_rep 2.7-3.9 s, t_rest 0.39-0.55 s, t_max 44-62
    ms, starved 0-26; the server's step p99 per 660 ticks 3.4-4.9 ms (max
    17.9 ms).  The H6 control recipe with snapshots off (`tr3m_H6_ctl0`,
    the same batch onto bay 3, no trafo, the option on): t_cap 0.95-0.99 s,
    t_rep 0.38-1.37 s, t_rest 0.02-0.10 s, t_max 13-16 ms, starved 0; server
    step p99 3.0-4.9 ms.  The server's step is within the control's band;
    the clients' rewind cost is 2-4 times it.
  - **`core_ref_t3` re-captured** with these binaries and the option on: the
    six arms of `core_ref_r3`, H1 at 150 ms as a seventh (`h1_150/`) and the
    solo pair; 28/28 sends SAME and journals IDENTICAL against the live runs,
    `BYTE-IDENTICAL-EXCEPT-BUILD-ID: 28 pairs`, every server journal replays
    matched with 0 checkpoint mismatches, solo old1 / old2 IDENTICAL (its
    README).  `wire_check.sh` replays the seven arms by default
    (`REF=core_ref_t3off` keeps six).
- **Not done:** T4 (retail; it also installs the controller alias and
  turns `physics.trafo_inputs` on), T5; the rewind-cost optimization.

**The rewind-cost profile (2026-10-25, step CPUPROF; HEAD 49e2cc8 + the TR3
working tree, nothing changed in it).**  Where a frame's time goes when a
client falls behind, measured, for the rewind-cost optimization (roadmap §4
item 10) and item 8's CPU cliff.  A measurement only: no behaviour, record,
wire or engine change.

- **Method.**  A temporary diagnostic build (`build-agent-rCPUPROFd`, kept
  as the evidence this block cites; the sources restored afterwards, `git diff --quiet` holds; the patch is
  `.local-tools/work/CPUPROF/patch_diag.py` + `edit3.py`, the timers
  `cpuprof_diag.hpp`), off unless `BMMO_CPUPROF` names a directory, then one
  line per frame, per rewind and per input correction.  Steady-clock timers
  around: `restore_and_replay` (the restore up to `replay_begins`, the
  replay after it: every input rewind, lifecycle rewind, snapshot rollback
  and resync goes through it; whether it ran inside `frame()` or in message
  handling), and around the headless world's hooks, outermost
  (`session_client.cpp`'s world builder): `step` (the IVP step of a replayed
  tick, `CKIpionManager::Simulate`), `pre_step` split into the core's
  `before_replayed_tick` (the birth remakes, the trafo inputs, the
  activation) and the scripts' frame (`execute_frame`), `recreate_body` /
  `destroy_body`, `set_savestate` / `set_travelling_savestate` (the restore's
  blob), `restore_scripts`, the record's capture (`capture_scope` brackets
  `rollback_engine::capture`) split into its blob (`get_savestate`), its
  script record (`capture_scripts`) and the per-body reads (the rest).  The
  live tick's IVP is the physics manager's `CKBaseManager::m_ProcessingTime`
  after the frame (CK2 `CKContext::ExecuteManagersPostProcess`,
  CKContext.cpp:1227-1236, times `CKIpionManager::PostProcess`, which is
  `Simulate`, CKIpionManager.cpp:306-308): no engine change.  Body counts
  (movable bodies, awake / asleep, and the trafo pieces among them by their
  `_Trafo` suffix) from `list_bodies` after each frame (10-13 us, reported
  and left out of the attribution).  `note_input_correction` logs what each
  disagreeing relayed frame disagreed on.  **Identity:** the diagnostic build
  with the profile ON replays the 28 captures of `core_ref_t3` with every send
  SAME and 28/28 journals IDENTICAL (`wire_check.sh - rCPUPROFd`: ALL SAME).
  Tools: `.local-tools/work/CPUPROF/` `live.sh` (one arm under the cpu
  sampler), `prof.py` (table / summary / rewinds / corr), `fit.py` (cost per
  awake and sleeping body), `ceil.py` (the per-rewind candidates'
  ceilings), `stale_tail.py` (the hold-last ceiling and the snapshot
  rollbacks' share), `reasons.py` (rewinds by cause); the tables and
  summaries in its `out/`.  **Bases:** every ceiling is given as a share of
  the window's frames with a rewind ("behind frames"), the share of the
  whole window's busy time in brackets.
- **Runs** (snapshots off, `arch_server_lp.sh ... 100000 100000 4.2 false
  false`, the merged tree's Arch server, one run per arm, host idle: cpu
  sampler max total 10.2 / 9.0 / 16.5 %):
  - `cpuprof_h1on`: H1 at 150 ms (§7.3; four wood trafos at 1167-1216), the
    option on: verdict PASS, 0 resyncs / rebases, **starved 17 / 8 / 0 / 0**
    (TR3's merge round: 21 / 26 / 18 / 0 and 0 / 1 / 10 / 0), t_max 42-56 ms.
  - `cpuprof_h1off`: the same recipe and binaries, the option off: PASS,
    starved 0 / 0 / 0 / 0, t_max 15-17 ms.
  - `cpuprof_cliff`: B2's cliff recipe (A5u at 150 ms, `--sectors 4
    --keep-lane --event 2:1500:step=300:dir=up`, flags off, the option off):
    the three respawns came at 2596 / 2606 / 2614 as in B2, but **the cliff
    did not reproduce as a rebase**: 0 rebases, 0 resyncs, starved 0 / 0 /
    733 / 0 (client 2's 733 is the recipe's own +300 ms uplink); the
    mechanism did, at the same ticks: every client's worst lag of the run
    follows from them (6.6-10.3 ticks behind, reached at 2684-2710, just
    past the profiled window 2580-2680; the rebase is at 33 behind).  So
    B2's run
    (`maps/arena-test/b2_a5u_ctl`) is used for the cliff itself; its
    captures are protocol 10 and a protocol-11 build refuses them, so it
    cannot be re-profiled, only read (its status lines and log).
- **The per-frame table** (full: `CPUPROF/out/table_h1on_c0.txt`,
  `table_h1on_c1.txt`, `table_h1off_c0.txt`, `table_cliff_c0.txt`; per
  client and window: `summary_*.txt`).  H1 on, client 0 (starved 17), ms;
  frame = the frame's busy time including rewinds run between frames;
  capture = bodies / blob / script record; restore = its blob / scripts;
  bodies = movable awake / asleep (pieces awake / asleep):

  | tick | behind | frame | rew | depth | ticks | IVP live / replay | capture | restore | scripts' frame | other | bodies | blob KB |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|
  | 1211 | 1.0 | 29.4 | 1 | 21 | 21 | 0.22 / 3.42 | 15.43 (3.40/8.50/3.52) | 3.53 (2.55/0.18) | 3.24 | 3.17 | 51/70 (49/67) | 255 |
  | 1220 | 1.7 | 70.4 | 2 | 25 | 50 | 0.25 / 10.20 | 36.82 (8.07/20.81/7.94) | 9.42 (6.99/0.24) | 6.88 | 6.29 | 67/70 (65/67) | 289 |
  | 1223 | 12.0 | 66.6 | 2 | 20 | 39 | 0.34 / 10.02 | 33.44 (7.44/18.93/7.07) | 9.89 (7.40/0.23) | 6.67 | 5.61 | 68/70 (65/67) | 294 |
  | 1227 | 15.6 | 2.1 | 0 | 0 | 0 | 0.28 / 0 | 0.85 (0.18/0.50/0.17) | 0 | 0 | 0.83 | 68/70 (65/67) | 296 |
  | 1245 | 2.9 | 85.5 | 2 | 28 | 56 | 0.34 / 15.75 | 48.17 (10.31/28.67/9.19) | 12.98 (10.30/0.25) | 0.89 | 7.26 | 64/74 (61/71) | 304 |
  | 1260 | 6.1 | 58.1 | 1 | 34 | 34 | 0.35 / 10.92 | 34.64 (7.55/20.38/6.72) | 6.23 (5.27/0.12) | 0.56 | 5.24 | 58/81 (54/78) | 308 |
  | 1265 | 16.3 | 38.0 | 1 | 24 | 24 | 0.24 / 6.58 | 21.52 (4.70/12.83/3.99) | 5.81 (5.07/0.11) | 0.33 | 3.39 | 56/83 (52/80) | 309 |
  | 1277 | 26.1 | 28.0 | 1 | 17 | 17 | 0.23 / 4.10 | 15.29 (3.50/9.10/2.69) | 5.35 (4.77/0.09) | 0.23 | 2.70 | 50/89 (46/86) | 303 |
  | 1278 | 26.9 | 2.1 | 0 | 0 | 0 | 0.22 / 0 | 0.93 (0.19/0.60/0.14) | 0 | 0 | 0.88 | 50/89 (46/86) | 302 |

  The episode: client 0's 32 frames with a rewind fall in 1211-1277, 41.1
  ms each on average (36.7-42.6 ms on the four clients; the worst 85.5 ms, two
  rewinds, one of them a snapshot rollback run between frames), 1.1-1.2
  rewinds and 23-28 replayed ticks per such frame; the loop ran those 67
  ticks at 20.6 ms per tick and ended 26.9 ticks behind its schedule; a frame
  without a rewind costs 2.0 ms and the loop wins back about 0.87 tick per
  such frame.  Starvation follows the lag: 26.9 / 23.7 / 18.6 / 14.5 ticks
  behind at most on clients 0 / 1 / 2 / 3 against starved 17 / 8 / 0 / 0.
  With the option off the same frames cost 11.6-13.5 ms (max 24-27 ms) and
  no client is more than 2.1 ticks behind.
- **Where the time goes** (window 1150-1400, the frames with a rewind, the
  four clients):

  | | H1, option on | H1, option off (Q4) | cliff (2580-2680) |
  |---|---|---|---|
  | mean frame with a rewind | 36.7-42.6 ms | 11.6-13.5 ms | 18.1-18.7 ms |
  | rewinds per such frame / replayed ticks | 1.09-1.22 / 23-28 | 1.05-1.11 / 23-25 | 1.00-1.06 / 37.5-40.2 |
  | share of all rewinds in it | 93.6-94.8 % | 93.1-93.6 % | 94.7 % (c0) |
  | per replayed tick | 1.23-1.27 ms | 0.41-0.47 ms | 0.41-0.43 ms |
  | - IVP step | 0.23-0.26 | 0.02-0.07 | 0.04 |
  | - capture: bodies / blob / script record | 0.17-0.18 / 0.44-0.49 / 0.16 | 0.05-0.06 / 0.15-0.19 / 0.08 | 0.08 / 0.17-0.18 / 0.08 |
  | - scripts' frame / remakes / bookkeeping | 0.07-0.13 / 0.002-0.004 / 0.11 | 0.01-0.06 / 0.001-0.003 / 0.04 | 0.003 / 0.001 / 0.05 |
  | per rewind: restore (its blob) | 4.7-5.4 ms (3.6-4.5) | 0.86-1.24 ms (0.50-0.96) | 1.06-1.16 ms (0.63-0.73) |
  | capture / IVP / restore / scripts' frame / other, % of the frames | 52-55 / 15-17 / 13-15 / 3-5 / 9 | 60-64 / 6-13 / 8-10 / 1-9 / 10 | 71 / 8 / 7 / 1 / 13 |
  | movable bodies awake / asleep | 1-69 / 70-133 (pieces up to 65 / 131) | 0-20 / 20-35 | 23-26 / 30 |
  | blob / script record | 157-311 KB / 62 KB | 97-143 KB / 34 KB | 124-134 KB / 33 KB |

  "Other" is the replay's bookkeeping (the inputs fed, the births' checks,
  the records: 0.11 ms per replayed tick with the pieces, 0.04 without), the
  restore's body plan and rechecks (0.77-0.87 ms per rewind with the
  pieces, 0.21-0.25 without), the live pre /
  post step outside them (0.93 ms per frame with the pieces, 0.32-0.36
  without).  The live tick itself is small: IVP 0.16 ms, scripts 0.13 ms, its
  capture 0.93-0.97 ms (0.43-0.46 ms without the pieces).  Per capture (fit
  over every frame without a rewind, R² 0.82-0.91): **400 us + 3.3-4.5 us
  per awake + 4.5-4.7 us per sleeping movable body** (blob 106 us + 3.5-4.1
  / 3.2; per-body reads 35 us + 1.0-1.2 / 1.3-1.4; the script record a
  constant 250 us, 150 us with the option off); the blob 75 KB + 2.1 KB per
  awake + 1.25 KB per sleeping body (R² 1.00); the IVP step 13 us + 4.1-4.6
  us per awake body and 0.1 us per sleeping one; the restore's blob 38-44
  us per awake and 13-19 us per sleeping body.  **A sleeping body costs the
  capture as much as an awake one and the step nothing.**
- **(1) The share of the piece bodies, and the ceilings** (ceil.py; option
  on, the frames with a rewind, the four clients; per-body costs from each
  run's own fits):
  - IVP step of the awake pieces: **15.6-17.8 %** of those frames (12.7-13.4
    % of the window; the replayed tick's step 0.23-0.26 ms against
    0.02-0.07 ms without the pieces).  It cannot be removed without changing
    what is simulated; nor can the replayed trafo scripts' frame (3-5 %).
  - The capture's part for sleeping bodies - the ceiling of **an incremental
    capture that reuses sleeping bodies' state: 22.2-24.3 %** (21.5-23.4 %
    of the window; of it the sleeping pieces 21.3-23.4 %; 19-22 % with the
    option off, 27 % in the cliff).  The awake bodies' part and the fixed
    part (blob 106 us, per-body 35 us, script record 250 us per capture)
    stay.
  - **The dormant-script encoding of 9.4: at most 5.2-5.3 %** (4.8 % of the
    window): the script record's on-minus-off difference, 107 us per
    live and 80 us per replayed capture for the 32 trafo scripts (62 KB
    against 34 KB), taken whole as if all of them were dormant.
  - The restore's blob for sleeping bodies: at most 2.8-4.2 % (1.8-3.1 %).
  - Together the incremental capture and 9.4 remove at most 27-30 % of a
    behind frame: 37-43 ms become 26-31 ms, still twice the 15.15 ms budget
    at this rewind rate.  These three cut the cost of each rewind; the
    hold-last re-prediction of (2) cuts the number of rewinds, so the two
    kinds compose multiplicatively on the rewinds' time, not additively.
  - The body-set full snapshot rollbacks (R2): 8.7-10.7 % (6.5-7.9 %), 3-4
    per client (`stale_tail.py`).
- **(2) Why the rewinds come every frame and are 14-30 deep.**  Every
  input rewind of the window (49-58 % of its busy time; the rest are 3
  lifecycle (birth) rewinds and 3-4 body-set full snapshot rollbacks per
  client, `reasons.py`) is caused by a relayed frame of a ball born in it;
  the depth is the **relay lag**, not a prediction horizon:
  - Of the 22-33 input rewinds per client (option on; 40-56 with the option
    off), the triggering corrections (308 on the four clients) are all of a
    newborn ball (0-39 ticks after its birth): 151 "flags only, 1 -> 5"
    (predicted PHYSICALIZED, relayed PHYSICALIZED | NAV_ACTIVE, the keys
    equal), 125 "no input recorded for the ball at that tick", 32 "flags 0
    -> 5"; **not one disagreed on the keys**.  Option off: 282 corrections,
    258 "flags 1 -> 5", 22 "no input recorded", 2 "flags 0 -> 5".
  - In 100 % of them the corrected tick is the newest relayed tick of that
    ball, and the rewind goes to it - 1: the depth is the age of the newest
    frame on arrival, the relay lag (mean 19.7-24.2 ticks on, 20.9-21.2 off:
    150 ms each way is 20 ticks).  The rewind depth (the lag + 1) stretches
    to 26-34 when the senders themselves are behind (client 0, 1244-1260:
    their frames leave late; up to 37-38 on clients 3 and 2) and shrinks to
    10-15 when the receiver is (client 0, 1223-1225, 7-14 ticks behind).
  - Why one per frame for about the relay lag in frames: a replay never
    re-predicts a remote's tail.  Past a remote's newest relayed frame
    `relay_frame_or_earlier` (session/input_relay.hpp:349) returns nothing
    and `recorded_input_at` (client_core_frame.cpp:495-502: "none for the
    tail the relay has not reached, where the recorded prediction stands")
    keeps the prediction recorded live, which was made before the change
    arrived.  This holds for **any** change of **any** remote's input, not
    only a birth: after a change at tick X every later tick up to the
    receiver's present still carries the old value, so each frame the
    relay brings in after X disagrees with that stale tail and asks for its
    own rewind, as deep as the relay lag, until the relay reaches ticks
    predicted live from the new value - about lag-in-frames rewinds per
    change, one per frame.  In H1 the changes are births: the ticks
    recorded between a ball's birth and its first relayed frame carry the
    birth's state (flags 1, navigation not active) or no input at all (a
    mirror rewound into existence, the "news" case of
    note_input_correction); about 20 rewinds per newborn ball, three
    newborn balls per client (the other players' paper balls) in 1190-1225
    make the ~1 s.  In the cliff (3) the same mechanism also runs on the
    late player's key changes.
  - **Ceiling of a hold-last re-prediction** (`stale_tail.py`: the replay
    re-predicts a remote's tail with its newest relayed frame; a frame's
    input rewinds are counted avoidable only when every correction noted in
    it repeats that entity's previous relayed value, the entity's first
    correction in the window counts as needed, the remaining rewinds keep
    their full depth): option on, 85-87 % of the input rewinds avoidable
    (only 4-5 corrections per client are new values against 62-79
    repeats), **63.9-66.3 % of the behind frames (43.0-49.3 % of the
    window)**; option off 92-95 % of the input rewinds, 76.4-78.9 %
    (51.8-61.4 %).  A prediction change - client-local, the server
    unaffected - with its own checks (the exactness set and a live drift
    check).
  - The rewind pattern is the same with the option off (H1 off: 1.05-1.11
    rewinds per such frame, 22 deep); what differs is the price of a rewind,
    31-37 ms against 10-12 ms (per replayed tick 2.7-3.0 times, the restore
    4.3-5.4 times).  With the option on the client coalesces more arrivals
    into one rewind while it is behind, so it runs fewer (22-33 against
    40-56).
  - The body-set full snapshots of a snapshots-off session (R2 above) add
    3-4 snapshot rollbacks per client (8.7-10.7 % of the behind frames,
    6.5-7.9 % of the window with the pieces; 4.5-5.6 % and 3.3-4.1 %
    without), 4 of client 0's 37 rewinds ran in message handling between
    frames.  They are today a correction a snapshots-off client receives,
    so dropping them is not a pure cost cut: it needs its own drift check.
- **(3) The cliff: depth times the per-tick cost, one rewind per frame.**
  At 2580-2680 the clients rewind in 42-70 of 101 frames, 1.00-1.06 rewinds
  per such frame, 37.5-40.2 ticks deep (correction lag 35-37: the late
  player's +300 ms uplink); 78-87 % of the time is input rewinds,
  lifecycle rewinds 1.6-3.8 %, snapshot rollbacks 2.8-4.4 %.  The input
  rewinds are one mechanism, the stale tail of (2): the late player's key
  changes (keys differ, 39-76 corrections; e.g. client 0, the late player's
  `Ball_Stone_BMMO_1651351904`: relayed keys 9 against predicted 1 or 5 on
  every tick 2545-2561, then relayed 1 against predicted 9 on every tick
  2587-2600, each corrected tick the newest relayed one) and the three respawned balls' births (no input
  recorded 44-68, flags 0 -> 5 10-17); each change costs one rewind per
  arriving frame for the relay lag.  That is why they come every frame:
  the hold-last ceiling (`stale_tail.py`, 2580-2680) is 68-75 % of the
  input rewinds avoidable (16-23 new values against 98-119 repeats per
  client), **57.1-66.2 % of the behind frames (52.1-64.1 % of the
  window)**; what stays is about one rewind per real key change or birth
  (12-20 per client in 101 frames).  A
  38-tick rewind costs 1.1 ms restore + 38 x 0.41 ms = 16.3-17.9 ms here, so
  one per frame is 1.2 times the budget: the clients drift 6-10 ticks behind
  and recover.  In B2's build a replayed tick cost **1.31-1.32 ms**
  (`b2_a5u_ctl`: t_rep 12.8-14.9 s over 9740-11268 replayed ticks; this run
  7.9-8.9 s over 19904-22387, 0.40 ms: TR3's capture work, fix-up round),
  so the same rewind cost about 50 ms, 3.3 frames' budget per frame, and the
  loop fell the 33 ticks to the rebase (2661-2665) in about 15 such frames.
  Correction of B2's wording ("38-39-tick lifecycle rewinds"): B2's own log
  (`b2_a5u_ctl` client 0, 2614-2660) shows the per-frame rewinds were input
  corrections 26-41 deep (the late player's ball and the respawned
  1029157707's), with two lifecycle rewinds and two snapshot rollbacks
  among them - the same picture as here.
- **(4) The baseline, the option off**: the second column of the table:
  per replayed tick 0.41-0.47 ms (IVP 0.02-0.07, capture 0.28-0.33, the
  scripts' frame 0.01-0.06), restore 0.86-1.24 ms per rewind, frames with a
  rewind 11.6-13.5 ms (capture 60-64 %, IVP 6-13 %, restore 8-10 %), at most
  2.1 ticks behind, starved 0.
- **Adaptations.**  Timers in a temporary build rather than off-by-default
  ones in the tree (the tree held TR3 uncommitted); a frame's time is its
  busy time, `frame()` plus the rewinds and captures run between frames in
  message handling (4 of client 0's 37 rewinds in H1 on); the live IVP is
  CK's own per-manager timer, the replayed one the step hook's; the cliff
  was read from B2's run, not re-profiled (no rebase in this run; B2's
  captures are protocol 10).  Review fix-up: the stale tail was first
  scoped to newborn balls and its ceiling stated on the window base
  without a script; it is a property of every remote's tail (above), its
  ceiling is computed by `stale_tail.py`, and every ceiling now carries
  both bases.
- **Ranked candidates with their measured ceilings**: roadmap §4 item 10
  (at the time; since RC1 they are `rewind-cost-design.md` §1, the
  roadmap item keeping RC1's record).

**As built (step TR3b, 2026-10-27; HEAD 3daa19a = RC1 5f967b4 + RL1 +
the working tree).**  T3 merged again, this time on top of the rewind-cost
step RC1 (the hold-last re-prediction) and the resync-lag step RL1, and
re-gated under its original H1 (c) (§11: the decision of 2026-10-25 and
the one-pair rule of 2026-10-27).

- **The merge.**  `git cherry-pick -n 64c5fa8` (the merge round's tree,
  branch `wip/trafo-t3-merged`; neither wip branch was checked out, reset
  or rebased).  Code: one conflict, the headless client's usage text
  (`session_client.cpp`: RC1c / RC1d's `--keep-lane-dwell`, `--brake-tap`,
  `--keep-lane-tap` against T3's `--beam2`; all four kept).  Auto-merged in
  separate hunks and read: `client_core_frame.cpp` (RC1's
  `recorded_input_at` -> `relay_frame_or_newest` beside T3's
  `hold_pending_trafo`, `apply_trafo_inputs`, the trafo frame, the record's
  deferred capture and the missed-trafo resync), the option parsing and
  frame loop of `session_client.cpp`, the unit tests
  (`session_client_core_test.cpp`, `rollback_engine_test.cpp`: RC1's
  hold-last tests and T3's trafo tests side by side), `session_runner`
  (RL1's re-anchor base vs T3's step timer).  Docs: this document's
  status and §11, roadmap §4 item 10 (RC1's committed text kept; the
  CPUPROF ranking it replaced is `rewind-cost-design.md` §1 and the
  profile block above).  38 files against the merge commit's 40: its
  one-line edits of `resync-and-sector-timing-design.md` and
  `rollback-engine-gaps-design.md` were RC1's identical ones, already in
  HEAD.  No code was changed after the merge.
- **Build tree.**  The x86 `BallanceMMOClient` objects were deleted before
  the build (T3 changes `client_core.hpp`'s layout); `ninja -t deps` shows
  no empty dependency list on a session object in either tree.
- **Checks** (quick tier, 15 minutes: about 10 of them used; x64
  `build-agent-rTR3b`, the Arch server rebuilt from this tree, id
  `ballanced-3e4289e95f25+bmmo-3daa19affd58-dirty`; helpers
  `.local-tools/work/TR3b/` `g.sh`, `live.sh` = `tr3/`'s with the labels
  `tr3b_*`):
  - 7.1: **522 tests pass** (rRL1's 511 + T3's 11).  GCC 29 ok.  x64
    BUILD-OK, x86 CLIENT-BUILD-OK, ARCH-BUILD-OK; `map.hpp` untouched.
  - The BAR, option off: `REF=core_ref_rc1 wire_check.sh rRL1 rTR3b
    rTR3bw`: **36/36 journals IDENTICAL** (nine arms incl. h1_150 and
    cliff); every send equal but one per client, SessionReady (#5 / #6,
    59 bytes: the build id and the `CAP_TRAFO_INPUTS` bit); the script's
    last line says DIFFERENCES for that send alone, as at the merge round.
    Solo snapshots-off (`tr3b_solo/solo/tr3b`) against `core_ref_rc1`'s
    solo1 and solo2: **IDENTICAL** both.
  - **H1 at 150 ms, snapshots off, one pair in one batch** (§11's
    one-pair rule; cpu_sampler max total 10.0 / 11.5 %):

    | per client 0 / 1 / 2 / 3 | `tr3b_H1_150` (option on) | `tr3b_H1_150_off` (control) | merge round, option on (`tr3m_H1_150`, `_b`) |
    |---|---|---|---|
    | verdict | PASS | PASS | FAIL, PASS |
    | starved | **0 / 0 / 0 / 0** | 0 / 0 / 0 / 0 | 21 / 26 / 18 / 0, 0 / 1 / 10 / 0 |
    | resyncs, rebases, far, unmatched | 0 | 0 | 0 |
    | input rewinds (`inrew`) | 25 / 29 / 35 / 30 | 21 / 28 / 30 / 29 | 128-181 |
    | replayed ticks | 1255 / 1279 / 1449 / 1321 | 984 / 1116 / 1176 / 1171 | 3385-4813 |
    | relay lag max / p99 (ticks) | 29/26, 29/26, 42/27, 42/42 | 24/21, 24/21, 42/22, 42/41 | 41-42 / 35-41 |
    | t_cap / t_rep / t_rest (s) | 1.69-1.77 / 0.99-1.15 / 0.13-0.15 | 0.88-0.92 / 0.34-0.41 / 0.03-0.04 | 1.45-1.70 / 2.7-3.9 / 0.39-0.55 |
    | t_max (ms) | 43-46 | 12-14 | 44-62 |
    | server step p99 per 660 ticks (ms) | 5.8, 4.5, 3.4, 5.2 (max 14.9) | 3.4, 3.6, 3.2, 7.5 (max 17.0) | 3.4-4.9 (max 17.9) |

    H1 (a): **pieces 150/150 bit-equal on all four** (`tr3fix/pieces.py`);
    (b) 4 sets on every client; **(c) met**: unmatched, far, resyncs,
    rebases 0, starved 0 against the control's 0; (e) `trafo_missed` 0,
    `trafo_original_leaks` 0, run_failures 0; the server journal replays
    matched (2234 ticks, 1117 checkpoints, 0 mismatches).  (d) is H1 at
    0 ms: not run (deferred).  The cause of the merge round's starvation
    was the stale tail RC1 removed: the input rewinds fell from 128-181 to
    25-35 per client, now the option-off control's count; what remains of
    T3's cost is the cost of each rewind (t_rep 2.6-2.9x the control,
    t_max about 3x), the rewind-cost optimization's work (roadmap §4 item
    10: RC2-RC5, closing on H4 / H6 with snapshots on).
  - **`core_ref_t3` is not replayable by this build** (checked on its
    h1_150 arm, `REF=core_ref_t3 LABELS=h1_150 wire_check.sh rTR3 rTR3b`):
    the journals differ from tick 253 on (the hold-last tail records the
    remote rows differently, as RC1's own note says of the pre-RC1 sets)
    and client 2's replay leaves the capture at pass 2954 (it asks for a
    resync the captured run never asked for: a different prediction rule
    replaying another rule's inbound stream).  The set stays usable for the
    pre-RC1 T3 build `rTR3` only; its re-capture with the final binaries is
    deferred (it does not fit the 15 minutes: seven live arms and the solo
    pair).  `wire_check.sh`'s default stays `core_ref_rc1`.
  - **`physics.trafo_inputs` stays off** (`config_manager.hpp:89`, the
    server's default), past T4 until the rewind-cost close-out passes
    (the orchestrator's adaptation M2): with it on, every retail client
    without `CAP_TRAFO_INPUTS` is refused (2.14).
  - Deferred to the final verification (roadmap §4 item 11): the second
    H1 pair, H1 at 0 ms for (d), H3b, H5, H7, the re-capture of
    `core_ref_t3`; the retail two-copy run of the merged x86 build, core
    off and on (none at TR3b: no BallanceMMOClient source changed, but the
    shared engine and savestate changes reach both retail modes - the
    savestate index rewrite, and `rollback.hpp`'s replay-mismatch counter
    now counted only while logged, the first 8; neither changes a
    simulation result; with core off the mod declares nothing new, and the
    core-off BAR rests on that deferred run).
- **Not done:** T4, T5; the rewind-cost optimization's RC2-RC5.

**As built (step TR4 = T4, 2026-10-27; HEAD 3394ffd + the working tree,
quick tier).**  The retail host runs the trafo as an input with
`session core on`; with `session core off` the legacy path is untouched
(PSC declares nothing new and gets no feature).

- **Where.**  `BallanceMMOClient/session/core_host.cpp`: one file-scope
  static `trafo_host` (registry, hooks, hold, `on`, the level reference
  captured at load, the birth-tick override; no `BallanceMMOClient`
  member); `make_retail_trafo_hooks` (the headless `make_trafo_hooks` over
  `physics_view`); `trafo_anchor` (hooks, controller alias, the level
  reference into the registry, `quiesce_originals`, the hold armed with
  1000/66 ms, the piece filter on) called in `core_host_anchor` right before
  the session clock reset; `trafo_teardown` (sets, alias, filter, hold) from
  `hooks.ended`, `core_host_end_local` and the next SessionStart; the
  platform operations `poll_trafo`, `make_trafo_set`, `run_trafo`,
  `trafo_final`, `trafo_frame`, `trafo_type_set`, `trafo_reconcile`,
  `retire_trafo_sets` (the headless host's, line for line);
  `caps.trafo_inputs = true`; the selection without the originals, the
  binding's and `localize_scripts`' trafo part, `local_only_body` off with
  the inputs, the sets' pieces in `explosion_pieces` (revivable, no
  BodyRevived).  The hold "in post_step" is the core's `poll_own_trafo`
  (CF), which calls `poll_trafo`.  `retail::trafo_scripts_loaded`
  (`core_host.hpp`) captures the level reference at `OnLoadObject` of
  `3D Entities\Balls.nmo` (`BallanceMMOClient.cpp`), read-only, whatever
  the switch.  `physics_view`: `has_body`, `trafo_set_register /
  unregister / seed / unmake / filter` (bridge v21) and
  `set_controller_alias` (v22).  Bridge **API v22**
  (`physics_rt_api.h`, `physics_rt_bridge.cpp`): `set_controller_alias`
  through a C callback (`alias(user, name, out, size)`), which installs
  `physics_savestate.hpp`'s `set_controller_alias` for the manager; the
  retail registry answers it with `trafo_controller_alias`.  No engine
  change, no protocol change.  Automation verb `drawn <substring>`
  (`client_automation.cpp`, read-only).
- **Adaptations** (no principle touched):
  - **M2 (the orchestrator's):** `physics.trafo_inputs` stays **off** by
    default after T4, until the rewind-cost close-out (H4 / H6 with
    snapshots on) passes: decision (b) of 2026-10-25 put that check there,
    and deployments run snapshots on.  T2's and T3's "T4 turns the default
    on" is adapted; the runs below set it per run (`arch_server_lp.sh`'s
    eighth argument).
  - **The originals' restore** (`trafo_set_hooks::restore_originals`) is
    empty in retail: the mod has no bridge call for
    `restore_explosion_pieces` alone, and the quiesce runs right before
    `reset_session_clock`, which ends with it - the same state as the
    headless order (quiesce with the restore after the clock reset; the
    restore is idempotent and nothing steps between).
  - **The level reference at the Balls.nmo load** (T1: "the retail host when
    Balls.nmo's scripts first exist"): `OnLoadObject` of that file, kept in
    `trafo_host` across sessions and handed to every anchor's registry;
    captured at the anchor only when the load was not seen (logged as a
    fallback: an original explosion before it may have changed values).
  - **Retail births inside a replayed tick** (found here; the headless host
    has the rule since S-steps): `OnPhysicalize` logged every birth at the
    live tick + 1, also when a replay across an explosion physicalized a set's
    pieces again, where a later rewind would take them for bodies born after
    its anchor.  With the trafo as an input, a replayed tick's births go
    under that tick when the log lacks them (`world.pre_step` wrapped), and a
    reconcile's remake under the resync's tick.  Without the inputs the
    R-steps' feed is unchanged (the same gap for mechanism births in a
    session without the option is queued, roadmap §4 item 5).
  - **Found and fixed: the builder crashed the retail mod at its return**
    (`trafo_sets.hpp` `copy_dependencies`).  `CKCopyDefaultClassDependencies`
    assigns the dependencies array inside CK2.dll, which in the game is
    another C runtime than the mod's; the mod's `delete[]` freed that
    buffer when the builder returned.  Symptom (first session `tr4_r1`): the
    first set registered, then `core.anchor` aborted silently after the
    journal opened (no "anchored (hash ...)" line, BML swallowed the fault),
    the other sets were made on first sight or never (client 2: "no trafo
    set for player <own>"); a diagnostic build placed it between
    `make_trafo_set`'s entry and `ensure_trafo_set`'s return, with the set in
    the registry.  Fix: the defaults read through
    `CKGetDefaultClassDependencies` and copied element by element into the
    mod's own buffer.  The same values: the headless build on L11 (SimTool
    `--trafo-sets 2 --describe-trafo`) gives the same digests
    (`90f856e0c8c20dd4/493`, `bf8575545ab7fbe0/597`) and 0 extras.
- **Checks** (quick tier; x64 `build-agent-rTR4`; the Arch server
  unchanged, TR3b's `bmmo-3daa19affd58-dirty` = HEAD's server code; runs
  under `scripts/retail/tr4_*`, helpers `.local-tools/work/TR4/`
  `tr4_retail.py`, `tr4_judge.py`, `field_diff.py`):
  - 522 tests pass; GCC 29 ok; x64 BUILD-OK, x86 CLIENT-BUILD-OK; map.hpp
    untouched.
  - The headless BAR: `wire_check.sh rTR3b rTR4 rTR4w` **ALL SAME** (36/36
    sends SAME, 36/36 journals IDENTICAL), once before and once after the
    `copy_dependencies` fix.
  - **One two-copy retail session** (`tr4_final`; Level 11, core on, the
    Arch server with `trafo_inputs` on, snapshots off; r3_retail.py's spawn
    recipe: both replay `r0_keys.bmrc`, client 1 beamed onto sector 1's
    `P_Trafo_Stone` at 300, client 2 at 340): both clients build both sets
    at the anchor (124 objects, 8 scripts each); each transforms once (wood
    -> stone), the explosion stamped at the Delayer's prediction (option E):
    458 and 498, applied there on both clients and on the server, the
    owner's at vanilla timing; resyncs 0, starved 0, rollback mism 0,
    `trafo_missed` 0, `trafo_original_leaks` 0, run_failures 0.
    - **Pieces across the two clients: 32/32 bit-equal** at every common
      checkpoint (5056 rows, ticks 458-812; position, rotation, both
      velocities).
    - **Drawn** (`drawn Ball_Wood_piece`, at about 520 and 760): at 520
      every piece of both sets is shown (`v`), its root in the render
      context, with its mesh and alpha 1, on both clients - the other
      player's copies included - and the originals hidden; at 760 the first
      set is hidden after its reset and the second fading (alpha 0.242 /
      0.205).  The screenshots (`shot_c2_520.png` of `tr4_d2`) show the
      wood pieces around the trafo.  The copies need nothing more than the
      scene: adding to the current scene puts the frame (a root) into the
      render context, which draws its children.
    - **Against the server's journal: positions and both velocities
      bit-equal on every row; 126 rows' rotation differs by <= 3.1e-8
      (12/32 pieces with no differing row), only at ticks 460-554.**  Root
      cause: the report, not the state.  The row's rotation is IVP's
      `calc_at_quaternion` (`physics_state.cpp` `fill_body_state`), whose
      `set_interpolate_smoothly` takes the slerp branch (acos, sin, sqrt)
      when the core turns more than 2.6 degrees per PSI - the pieces right
      after the explosion; the retail game thread runs the x87 at 24-bit
      precision (Direct3D; the anchor line's `fpu=000a001f`), so that slerp
      comes out about 1e-7 off unit length (0.99999992 against
      1.0000000000000062).  The simulation never reads it (positions and
      velocities stay bit-equal through 350 ticks of collisions).  Proof:
      the same session with the automation's `fpu53 on` on both clients
      (`tr4_d3`): **32/32 pieces bit-equal to the server on both clients,
      5056/5056 rows**.  Not T4's code (the report path and the FPU mode
      predate it); queued (roadmap §4 item 5).
    - **The reference-map digest differs from the headless one**:
      `d6e21ba72b993c66/493` on the retail CK2.dll, `90f856e0c8c20dd4/493`
      headless (the value digest `bf8575545ab7fbe0/597` is equal).  3 of 493
      lines: each explosion's `Physicalize:in11` reads the "Get Current"
      operation's output, named `Out` in retail and `Pout 0` headless; the
      same parameter by place.  Root cause: the reimplementation's
      `CKParameterOperation::Update` (Ballanced CK2
      `CKParameterOperation.cpp:105-107`) renames every operation output
      `Pout 0`, the game's CK2.dll keeps the file's name - the originals
      already differ (a diagnostic build's dump of the originals' map in
      `tr4_d5`).  Design 9.1 / R2's "reference-map digest of the retail logs
      equals the headless digest" therefore fails on a parameter name,
      pre-existing and outside T4's code; options: the map describes an
      operation output by its operation and index instead of its name
      (changes every recorded digest), or the reimplementation keeps the
      file's name (an engine-side change of the CK2 reimplementation).
      Severity: diagnostic only - the reference-map digest has no consumer
      beyond the build's log line (SimTool's trafo tool compares the value
      digest, which is equal; nothing on the wire), and the portable script
      record keys scripts by owner and name, so localization is unaffected.
      The cheap form of the first option: `where()` in
      `trafo_reference_map` describes an outside source parameter by its
      operation's place (root script + operation index + `:out`).
      Queued (roadmap §4 item 5); the user's call.
  - The verification ran over its 15 minutes (about 27 minutes of wall
    clock): about 15 of them went to finding the builder crash's root cause
    (four diagnostic sessions `tr4_d1`-`tr4_d5` and their builds), the rest
    to the gate itself.
- **Deferred to the final verification** (roadmap §4 item 11): R1-R5 beyond
  the one session (R1's solo forced arms, R2 with the R2 config and R2b at
  150 ms, R3's second session in one process, R4 core off, R5 the wire on
  trafo-free captures with the retail build), an out-of-reach trafo
  rejected and logged, the H arms' A/B, CPU parity, three launches without a
  hang.
- **Not done:** T5 (records: `docs/engine-changes.md`'s bridge note for v22
  is T5's, together with v21's); the default of `physics.trafo_inputs`
  (M2).

## 9. Open questions

**9.1 CK copy semantics on the retail CK2.dll.** The open points are:

- what `CopyObjects` adds through a copied script's owner (I0's extras);
- whether a block's instance function (the Random and Set Position patches) is
  copied;
- whether `AddScript` re-owns the copy cleanly;
- whether `SetObjectInitialValue` behaves the same.

The reimplementation is readable; the game's DLL is not.

*Settle:* T1 runs the builder in SimTool (I0-I7). T4 runs the same builder in
the mod at the anchor, logs I0-I7 and the reference-map digest, and compares
the digest with the headless one (R2). The builder removes extras, re-applies
the patches itself (idempotent) and renames explicitly, so none of these
depends on copy details. A failing invariant names the object.

**9.2 `Ball_CollSound_Pieces` and the PreSimulate queue.**

- *Facts.* CollSound runs in every world in the trafo frame (2.5). Its
  `PhysicsCollDetection` Create on a bodiless piece queues a retried callback
  (`PhysicsCollisionDetection.cpp:262-276`). `check_callbacks` refuses a blob
  whose queue differs (§4.4).
- *Settle:* S8 at T1, before T3 starts.
  - **No entry from a `_Trafo` block at any tick:** nothing to do.
  - **Entries:** first the cause. Which link issues the Create, and does the
    reset's De Physicalize precede it? Then two options, for the user to
    choose before T3:
    - (a) the trafo frame clears the set's CollSound flags and deactivates it
      in the frame of its reset's De Physicalize. This is the same in every
      world, and S8 is rerun. It holds only if the deactivation callback
      removes the queued entries; S8 shows whether it does;
    - (b) F5's selected-callback rule (audit F5) moves before T3. The user's
      order currently puts F5 after this track.
  - The sound is not the gate. The gate is a clean queue (H5: no "PreSimulate
    queue differs").
- *Also:* a replay re-executes CollSound's Play Sound blocks, as it does every
  selected mechanism script's. That is no new class; the server and headless
  hosts have null sound managers.

**9.3 The validation bound** `kTrafoReach` (provisional 5 m).

- *Settle* in T2 from the existing logs. For every trafo in the R2, `r2t`,
  `p3_two` and `p3_h4` runs, take the distance from the captured position to
  the nearest `P_Trafo_*`. Set the bound at the measured maximum × 1.5 and
  record it.
- If the user prefers not to reject (only log), the owner's undo path is never
  taken. That is a decision for the user.

**9.4 The per-tick script record cost** of 8 scripts per selected set.

- *Settle:* H6.
- If the capture exceeds the band, record v3 encodes a script that is inactive
  and unchanged since its set was built as one byte, and the restore writes its
  built state.

**9.5 The explosion's lead delay** (35 ticks = 0.53 s at input delay 17).

- *Option E:* stamp at the end of `dephysic Ball` instead of at the activation.
  - The hold finds the Trafo Manager's 1000 ms Delayer structurally (the
    Delayer whose Out reaches `start Explosion`).
  - When it starts, the explosion's frame is known 66 frames ahead (`Delayer`
    in fixed frames), and so is the position: nothing moves the old ball after
    `TT Set Dynamic Position` stops.
  - The event is stamped at that frame + 1 when the lead is ≤ 66.
  - The activation is still cancelled when it comes.
  - The owner's explosion is then at vanilla timing.
- *Settle:* measure the lead distribution in the harness and retail logs
  (`sector lead` in the session lines). If any session's lead exceeds 66, E
  falls back to the hold for that session. The user decides whether vanilla
  timing is worth the structural dependency on the Trafo Manager graph.

**9.6 The deadline and vanilla's Game Over.**

- *Fact:* a Game Over bypasses `reset Ballpieces` (§1.5). In vanilla, the
  pieces then stay until the level is restarted or exited, and Event_handler's
  `reset Ballpieces` resets them.
- *Design:* the deadline resets them 20 s after the explosion in every world
  (2.2, §6).
- *Settle:* the user confirms that a 20 s cap on piece life is acceptable in
  sessions. The alternative is a server-stamped reset only at session-level
  restart/exit, which needs the session's restart events. That would leave a
  Game Over player's pieces, and for paper the wind, live in every world
  until then. S12 records what vanilla does in each state.

**9.7 Found on the way (outside this track, reported per principle (d)): existing members do not know a late joiner's join order.**

- *Symptom:* measured in `maps/arena-test/lp1_join`. For late joiner 142763540
  at tick 1844:
  - the server and the joiner kicked its spawn with index 60;
  - clients 0, 1 and 2 kicked its mirror with index 56.

  The mirror is pushed in another direction than the server's ball in every
  existing member's world.
- *Cause:* a client's `players_` is set only from its own SessionStart
  (CC:145). Existing members are never told a newcomer's `join_order`. So, for
  the joiner:
  - its mirror's group is `P#63` (CL:468-470), and the group ident is engine
    change #7's ball-pair order key;
  - its spawn index uses 63 (CL:475);
  - its events rank 256 in the drain order (CL:160), where the server uses its
    real join order.
- *Options:*
  - (a) the server sends existing members a member-added message with the
    `player_entry` (id, join order) before the joiner's first event is relayed;
  - (b) the join order travels in every relayed event and in the relay header.

  (a) is one message per join and fixes all three uses.
- This design avoids the dependency: it orders sets by player id.

**9.8 Found on the way: the original piece sets have one referential and one
reset per type in vanilla too.** Vanilla has the same shared-`Ball_Pos_Frame`
property for a wood→stone chain within one frame. It cannot happen there,
because the Trafo Manager serializes trafos by 156 ticks. No action. It is
recorded so that nobody "fixes" the per-type referential back to one.

## 10. Review (revision 2)

Both reviews' blockers and should-fix findings were checked against the code
and the data. All were confirmed except where noted, and the document now
addresses each:

| finding | status | where |
|---|---|---|
| R1 B1 (retail rewind/echo switches off) | confirmed (CH:597-598 in the WT, CL:189). Fixed by the second option: Trafo inputs use the input-rewind path, gated by `rollback && input_rewind` only. R3 is not made a prerequisite, because the user's order puts this track before R3 | 4.6, 4.7, T0, 7.1, R2/R2b |
| R1 B2 (input later than the history) | confirmed (CL:205-206 only logs) | 4.7 missed input → resync; H3b |
| R1 B3 (a resync is not a join) | confirmed | 4.7 reconcile: unmake before remake, ledger names leavers |
| R1 S1 / R2 #2 (one referential per player) | confirmed | 2.4 one referential per type set; S5 player 3 |
| R1 S2 (copied controller pointer, pending original reset) | confirmed (`PhysicsForce.cpp:297-318`) | 2.3 quiesce at the anchor; 2.4 step 5; I7; R3 |
| R1 S3 (13.41 m is no bound) | confirmed and measured: falls travel up to 2672 m | 1.2; 2.10 filter rule; S11, S13 |
| R1 S4 (`gone.applied` misses an accepted explosion) | confirmed (LC:411-431) | 2.2 leavers use `lifecycle_leaver_removal_tick` after the Unphysicalize; 7.1 |
| R1 S5 (clone pose at acceptance) | confirmed | 2.2 validation uses static trafo positions only |
| R1 N1 (fade length) | confirmed that 132 is wrong. Partly corrected: the first execution is in the activation frame R, so Out fires on the 133rd execution in frame R+132, not R+133. De Physicalize is in R+132 and Restore IC in R+133 | 1.5, 2.7, S5 |
| R1 N2 (DeActivate semantics) | accepted | 2.3 `ModifyFlags`; effect checks; S7 |
| R1 N3 | accepted | 4.3, 4.5 (8 scripts), terms, 4.7 name-keyed |
| R2 #1 (CollSound never runs) | confirmed (`CKScene.cpp:282-284` with an inactive owner) | 2.5 CollSound in the trafo list; 4.4; 9.2 |
| R2 #3 (explosion and reset in one tick, or reversed) | confirmed (LC:322-326 ranks them equally) | 2.2 ledger: span, inserted reset, superseded reset; 7.1 |
| R2 #4 (owner never sends a reset) | confirmed for Game Over (`d_Gameplay_Ingame.txt:1633`); Finish and pause not established | 2.2 deadline; H11; S12; 9.6 |
| R2 #5 (leaver without a ball) | confirmed (SR:349-358) | 2.2 leavers; H8 (b) |
| R2 #6 (when the hold starts) | confirmed | 2.3 armed at the anchor, quiesce, pre-assignment deferral |
| R2 #7 (copy pulls `All_Balls`) | confirmed on the reimplementation (CK2 `CKBehavior.cpp:2029-2034`, `CKBeObject.cpp:700-715`); retail to be seen | 2.4 step 0, I0; 9.1 |
| R2 #8 (sets never destroyed) | confirmed | 2.4 retirement; 4.5; 4.8; H8 |
| R2 #9 (warning is a waiver) | confirmed | 2.10 filter rule; S11 |
| R2 #10-#14 | accepted | 2.11 retail static; 2.5 input log in `trafo_input.hpp` so S6 runs at T1; 4.3; R1 criterion; 2.5 retail row |

Nothing was refuted outright. One finding was corrected in detail (R1 N1, the
frame number), and one was resolved by the alternative its reviewer offered
(R1 B1).

## 11. Decisions (user, 2026-10-23)

- **9.6, piece life.** A 20 s cap is accepted: the server stamps a deadline
  reset at explosion + 1320 ticks in every world, Game Over included.
- **9.5, the explosion's timing.** Option E: stamp at the end of `dephysic
  Ball`, from the Trafo Manager's 1000 ms Delayer, so the owner's explosion
  keeps vanilla timing; a session whose lead exceeds 66 falls back to the
  hold for that session.
- **9.3, validation.** A trafo position farther than `kTrafoReach` from every
  `P_Trafo_*` is **rejected**: the server does not apply it, the owner gets a
  REJECTED echo and undoes its local effect, and the server logs the
  rejection as possible cheating or corruption with the details that matter
  (player, tick, the position, the nearest trafo and its distance, the
  ball types, the session).
- **9.7, join order.** Option (a): a member-added message with the
  `player_entry` before the joiner's first relayed event (scheduled with the
  other gaps, outside this track).
- **2026-10-25, the rewind cost before T3** (verbatim): "先把"重放沿用远端旧预测尾巴"这个缺陷修掉（hold-last
  重预测，客户端本地，带自己的精确性和漂移检查），然后 T3 按原门槛 H1 (c) 重新过，过了就提交，再继续
  T4、T5、S5-S11。" The replay's stale remote tail (CPUPROF's first
  candidate) is fixed first as step RC1 of `rewind-cost-design.md` (committed
  with hold-last by the user's ruling of 2026-10-27: "As built (step
  RC1)" there); T3 is then re-gated under its original H1 (c)
  - starved within the control band at 150 ms, snapshots off - at TR3b
  (done: §8, "As built (step TR3b)"; passed, committed). This settles
  the ruling §11.1 below left open: H1 (c) stays T3's gate and is judged
  after RC1.
- **2026-10-27, the one-pair rule** (verbatim, asked how a step whose row
  needs more live runs than the 15-minute verification holds is gated):
  "15 分钟内只做最关键的一对" - ONE H1 run at 150 ms with the option on and ONE
  same-batch control of the same recipe with the option off, snapshots
  off, judged by H1 (a)-(e) of §7.3 as written. The rest of T3's row (the
  second H1 pair, H1 at 0 ms for (d), H3b, H5, H7) and the re-capture of
  `core_ref_t3` go to the final verification (`docs/roadmap.md` §4 item
  11, "Deferred to the final verification").

## 11.1 Decisions (user, 2026-10-25)

- **T3's gate, option (b)** of the fix-up round's root cause of H4 / H6
  (§8, "As built (step TR3, fix-up round)"), in the user's words "好的，就b":
  T3 is judged with **snapshots off**, the roadmap's primary configuration.
  The proposal stated that every snapshots-off arm passed there; that was
  not so for H1 at 150 ms, whose (c) "starved within the control band"
  fails in every run (starved up to 26 own frames per client, control 0;
  §8, "As built (step TR3, merge round)").  Whether H1 (c) moves too is
  the user's open ruling; (b) as given covers H4 and H6 only.  The CPU with snapshots on - H4 and H6 as §7.3 states them - becomes its
  own step, **the rewind-cost optimization** (`docs/roadmap.md` §4 item 10,
  before the stage A close-out): an incremental savestate record that
  reuses sleeping bodies' state, the compact encoding of dormant scripts
  (9.4), and whatever else the profile shows.  It closes on H4 / H6 as
  §7.3 states them, against T3's snapshots-off CPU numbers (the merge
  round's H1 at 150 ms, §8) as its baseline.  §7.3's H4 / H6 rows and §8's
  T3 row say so.  The snapshot fallback itself stays (the user's decision
  of 2026-10-24) and nothing is added that makes a client depend on it.
- **H1 (c), the ruling left open above** (made 2026-10-25, §11: "然后 T3 按原门槛
  H1 (c) 重新过"): it stays T3's gate, judged once RC1 has fixed the stale
  tail; it passed at TR3b (§8, "As built (step TR3b)").
