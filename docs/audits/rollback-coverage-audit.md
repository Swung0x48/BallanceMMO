# Rollback/replay coverage audit: what the live simulation carries and a rewind does not restore

Report file: `C:/Users/GEEKER~1/AppData/Local/Temp/claude/D--repos-BallanceMMO/769d21ff-c9e2-42a0-a69a-d95ce0b06545/scratchpad/audit/rollback-coverage-audit.md`

Tree audited: `collision-overhaul`. HEAD is **4882ec2** (R2 is already committed, not fe1cdde as the brief says). The working tree (WT) holds the uncommitted **step P**: the pieces are tracked, parked across their birth, and the Random cursor is kept live. Line numbers are WT unless marked HEAD.

## 0. Premise corrections (they change which host each gap hits)

- **Retail applies the IVP savestate blob. It is not per-body only.**
  - Core path: `core_host.cpp:587` sets `config.savestate = true`, and `:1502` sets `savestate_flags = 0u`. Flags 0 means lenient, not off.
  - Legacy path: `physics_session_client.cpp:3074` sets flags 0. It never calls `set_exact_restore`, so `rollback.hpp:1831` `exact_restore_ = true` and `rollback_binding.hpp:39` `savestate = true` both apply.
  - Logs confirm it: r2_a_f13 `sv=170/0/0`, r2_x_t1_f13 `sv=169/0/0`.
  - **So the blob path and its gaps apply to all three hosts.** The per-body path is only the fallback after a refusal, plus headless `--no-savestate`.
- `restore_clock = false` on all hosts (`core_host.cpp:580`, `physics_session_client.cpp:3072`, `session_client.cpp:209`). This only matters on the per-body fallback, because the blob rewinds the clock itself.
- **Step P covers the core host and the headless host only.** The legacy retail path still drops the pieces:
  - `physics_session_client.cpp:47` skips names with `strncmp(body.name,"Ball_",5)`.
  - `:3150` makes `destroy_body` refuse `Ball_*`.
  - It has no `parked` hook.

## 1. The gaps, ordered by impact

"Host" uses these abbreviations: **H** = headless `session_client`, **RC** = retail core host, **RL** = retail legacy `physics_session_client`.

| # | What is not restored, or comes from the wrong history | Created by | Host/path that misses it | How it reaches balls or mechanisms | Confidence |
|---|---|---|---|---|---|
| 1 | **Trafo pieces born inside the window.** `Ball_{Stone,Wood,Paper}_piece##` (17/16/18). The `Ball_` filter gives them no plan item. The blob does not know them, so they go to `unknown_bodies`. Their recheck is deferred to the birth tick (`rollback.hpp:912-921`, `:1159-1170`), but their core stays in the revive list (`physics_savestate_restore.cpp:1686`). So they fly through [A, birth) with their live, future pose and velocity and no collisions. | `Ball_Explosion_<type>` from the Trafo Manager (Gameplay_Ingame, not replayed) | H, RC, RL at HEAD. **WT step P fixes H and RC (unverified). RL stays open.** Also hit when a blob is refused (see #6): pieces keep their live state through the whole rewind. | They hit the floor and every movable that is not a player ball (`physics_state.cpp:970-981`). r2_a_f13, r2_x_t2_f13 and r2f_a_f13: 17/22 bodies off from tick 820, piece05 off 7.32 m on one axis (14.58 m full state). Exactly one rewind, anchor 795 at live tick 819, logs `deferred=17`. The ball and the mechanisms stayed exact. | confirmed |
| 2 | **Pieces exist in one world only.** The server never trafos (its retail ball is parked, `physics_world.cpp:419-423`). No mirror explodes. The only other caller is SimTool `--explode`. Pieces live from the trafo until `Ball_ResetPieces`, which runs about 20 s later (Fadeout Manager Timers of 20000 ms) or at the next death. That was ~600 ticks in r2_a_ctl1 (807 to 1434), not 2 s. | The trafo, live, before any rollback | All hosts. The worlds disagree in the live run. Tracking the pieces (#1) only makes each client consistent with itself. | Pieces that land on or roll into a mechanism or prop push it in the owner's world only. With snapshots off nothing corrects it. With snapshots on, the server's piece-free rows contradict the client, and it rolls back again for as long as the contact lasts. Retail geometry from the PH tables: 20 trafos have a movable of their sector within 20 m (L6 s5 Trafo_Stone to P_Modul_29 11.0 m; L12 s7 Trafo_Paper to P_Box_MF001-003 ~9.3 m; L8 s5 to P_Modul_08 14.2 m). Measured maximum piece travel is 13.41 m. The arena has 0 such cases. | likely (geometry and filter; no run with contact) |
| 3 | **Resync: the scripts' phase.** `on_session_assign` (re-assign branch, `client_core.cpp:369-397`) and `client_core_messages.cpp:69-136` write the full snapshot of tick S at the client's tick B (B-S is 18-28) through `write_body`. They do not step it forward and do not touch scripts. The scripts keep the client's own pre-resync phase (e.g. "tick jumped from 1475 to 1527, 51 frames skipped"). Later snapshot rollbacks restore bodies from server rows but scripts from the client's own records, which is the wrong history. | Every resync ("tick driver rebased", "substituted own frame past the reach"); about 1 client in 2 in the r1_c* / r2_x_t1_b runs | RC and RL observed. H runs the same code; untested on Level 11. | Delayer/Sequencer force toggles stay out of phase for the rest of the session. P_Modul_26_Sack is 4.48 m off from 1528 to 2610 (r1_c_on1 c2), 8.27 m (r1_c_off1 c2, legacy), 5.70 m (r2_x_t1_b, both clients), with 41-72 sack corrections after each resync. Clients without a resync stay exact. P_Modul_08 and the balloon use the same Delayer/Sequencer pattern but are not tested. | confirmed |
| 4 | **Resync: one-shot script effects between S and B are undone for good.** The body goes back to its tick-S pose and sleep flag (wake = SIMULATED). The script that fired between S and B never fires again. | Same write as #3 | RC observed; H has the same code | r2_x_t1_b c1: Pusher001 was dropped at 508, then the resync to 524 from S=496 put it back to the pre-drop pose, asleep. It stayed 0.086 m off the server from 524 to 2654. That is below the widened tolerance (0.05 x min(lag/7,4)), and a sleeping row inside tolerance is not authoritative (`rollback.hpp:584-592`), so nothing repairs it. c2, whose resync came before its own trigger, is fine. | confirmed |
| 5 | **A late remote Sector event starts at the live tick, not at its stamp.** `client_core_lifecycle.cpp:330-336` only stamps it and starts no rewind. `sector_union.hpp:58-65` and `:82-104` start the sector at the next tick. `client_core_frame.cpp:195` records the activation at that live tick, and every replay re-issues it there (`session_client.cpp` replay_manager_activation; `core_host.cpp:1123`). | Relay later than the sector lead (uplink step, lead slew, loss) on a sector's first crossing | All hosts (RL: `physics_session_client.cpp:2094`) | The whole sector's mechanisms and scripts are phase-shifted until the session ends. core_ref_e0_pre_q/a5u_lead (the E0 set): the server and c2 started sector 4 at 1714; c0, c1 and c3 at 1723 (the 2026-10-24 re-record in core_ref_e0/: 1713 and 1722). Schaukel off 2.873 m, PE_Balloon up to 6.8 m, P_Modul_03 1.36 m. Known open item: late-player-design steps 13/14 are not built. | confirmed (one run) |
| 6 | **The PreSimulate callback queue (`CKIpionManager::m_PreSimulateCallbacks`) is never rewound.** (a) `check_callbacks` (`physics_savestate_restore.cpp:1772-1821`) refuses the blob on any difference, even in lenient mode. `rollback.hpp:922-930` then falls back to per-body restore and drops that anchor's blob for every later rewind. The fallback has no clock rewind, restores no untracked body, and leaves IVP collision state live. (b) A replayed selected `SetPhysicsForce` Create against a missing body queues one more copy each rewind, because `PhysicsForce.cpp:208-222` has no dedup. When the body appears, every copy runs `new` on a controller and overwrites local 0 (`:190-192`). Shutdown deletes one copy; the others keep pushing. | Retail Physics WakeUp or SetPhysicsForce whose target has no body: the ball held key while dead or mid-trafo (Gameplay_Ingame 26418/26432), or a module with a missing body | All hosts (RL included) | (a) Rewinds drop to pre-savestate fidelity: piles and contacts, and pieces left live, which is #1. Observed 12 logs, up to 46 refusals per client: e8ab2_swing_n1 c3 (WakeUp 48462 x1 to x6, 22 FAILED), core_ref_e0_pre_q resync_snapoff (the E0 set; 2:45709 P_Modul_08 force x1 to x5), r4_lane_loss10, lp19/lp20. (b) A permanent extra force on a swing, sandbag or fan. Derived from the code; not observed. **Every observed case predates N4 (21129b2). 245 post-N4 logs show 0 refusals.** | confirmed (code), observed pre-N4 |
| 7 | **A body whose life ends inside the window is missing from the replay of [A, D).** Per-body path: `rollback.hpp:966-970` (`if (!world.get_body) continue`). A remade name keeps only the new life (`:681-686`, `:718-745`). Blob path: lenient skip, "in the savestate but not in the world" (`physics_savestate_restore.cpp:512-516`). The lifecycle rewind is correct only for its own anchor D-1 (`client_core_lifecycle.cpp:174-179`). | A remote ball's death or trafo, followed by a later rewind anchored before it (e.g. a relay correction). RC with `lifecycle_rewind=false` (`core_host.cpp:593`): every mirror death at the live tick. `Ball_ResetPieces` De Physicalize (bites once #1 tracks pieces). | All hosts | Contacts with the vanished body are missing for A+1..D-1, and the dead ball's corrected inputs are replayed onto no body (ab2_s17_beam_1 c2.log:449, anchor 693, death 701, skipped=49). Found in 5,541 of 1.10 M logged rollbacks. Physical effect measured: none yet. | confirmed (path), impact unmeasured |
| 8 | **The retail own beam bumps the generation when it is requested.** That happens in post_step(T-1), after record(T-1) (`client_core_lifecycle.cpp` ~703). An anchor A <= T-2 then fails `record_at` (`rollback.hpp:681-685`). The ball counts as born at T, is parked awake at its tick-T pose, and is fed pre-beam inputs for A+1..T-1 (`:709-741`, `:985-990`). The replay re-records A+1..T-1 with the parked pose. | Own beam with `own_body_control=false` (RC only, `core_host.cpp:538`) | RC only. H keeps the generation on a beam (`rollback.hpp:714`). | Ball_Pos_Frame sits in the wrong place, so every TT Scaleable Proximity schedules differently: r2 Last Check 40 vs 17, P_Modul_18_Particle ACTIVE. This appears only for anchors before the stamp (886/893/876/626 differ; anchor 899 on the same stamp does not). No body diverged yet; the beam is automation-only (`write_beam` has 2 callers). **Probably also own death/respawn inside a window, on both hosts. Not verified.** | confirmed (state), body effect unmeasured |
| 9 | **Pooled module entities.** One PH Object serves several sectors (L8 P_Modul_30_MF in s1/s2/s4; P_Box_MF001-005 in s2/4 and s5; multi-sector movables: L1 11, L5 4, L7 7, L8 10, L9 15, L10 13, L11 17; arena 0). Under the union, a later sector's Type 1 activation moves the module frame (Set World Matrix, Hierarchy) and re-creates its hinge from the moved referential (`PhysicsHinge.cpp:152-158`). Physicalize short-circuits (`Physicalize.cpp:104-116`), so the body stays where it is. Rollback part: `script_replay.hpp:853-863` compares only whether a joint handle is present, so the old constraint is gone for good. Unphysicalized frame matrices are in no record. | Sector activation of a later sector while an earlier one is in the union | All hosts, identically (gameplay); rollback part on all hosts | **Gameplay bug on every side:** the L8 sector-1 see-saw sags 1.158 m instead of tilting 0.085 m once sector 2 activates (SimTool drop). The later sector's instance never exists. Rollback: a window across the activation replays [A, act) on the post-activation joint and frame. | confirmed (gameplay, SimTool); rollback part derived |
| 10 | **The deterministic Random cursor is rewound by the blob, but no replayed script draws from it.** `physics_savestate_restore.cpp:662` (HEAD). The only consumers are the explosion scripts (`physics_state.cpp:2218`, `:2345-2358`): stone/wood 1 impulse draw per piece; paper draws mass, friction and impulse. | A blob rewind with A < explosion E <= now | All hosts at HEAD. **WT step P keeps it live** (restore.cpp:662ff, note-only). | The next explosion repeats E's draws. Measured by step P: the second trafo's wood pieces 0.27 m off the control. Owner's world only. | confirmed; fixed in WT (unverified) |
| 11 | **Ball_PaperWind's 18 SetPhysicsForce controllers (0.03).** Created by Ball_Explosion_Paper and shut down at the start of Ball_ResetPieces_Paper, ~2 s before De Physicalize (Fade 2000 ms). The owner is All_Balls, so the script is unselected and has no force capture. `order_controllers` (`physics_savestate_restore.cpp:1584-1636`) only reorders and skips "no live controller". | A window with A in (E,S) and now in (S,D] | All hosts (blob); per-body never touches controllers | The paper pieces replay A..S without wind, which then reaches whatever they touch (#2). Paper trafos only; not observed (r2 is stone only). | likely |
| 12 | **The level-finish PE_Balloon reset is lost.** Gameplay_Events (unreplayed) runs ~50 s after End Level: Set Cell [0][4]=0, Activate Script(PE_Balloon Script, reset) id 30317, Unphysicalize, Restore IC. `replay_activation=true` (`core_host.cpp:1557`, `session_client.cpp:2687`) makes `script_replay.hpp:429` ignore `kept_live`, and only SectorManager activations are re-issued (`session_host_support.hpp:181-190`, `:224`). | The finisher's own world, ≥50 s after finishing | All client hosts (the server does not roll back) | Rollback after T_r+1: PE_Balloon Script stays active on torn-down bodies (script state only). Rollback at now == T_r: the pending reset flags are overwritten from the anchor (`kRuntimeFlags`, `script_replay.hpp:83`/`826`), the teardown branch never runs, and the balloon structure stays physical with lift, springs and hinges for good. The server tore it down. | possible (code + graph) |
| 13 | **The ActiveBall cell `CurrentLevel[0][1]` is not recorded.** The record carries only [0][4] and IngameParameter[0][1..2] (`script_replay.hpp:339-358`). It is written by "set new Ball" (Set Cell 24850, unreplayed) and read in replayed frames by P_Modul_29 (Get Cell 47422, then Test == "Ball_Stone", then the hinges break) and P_Modul_18 (Get Cell 46392 on Out Range, then Box-Box and the SetPhysicsForce target). The server rewires these reads (`physics_world.cpp:428-520`); clients do not. | Trafo inside the window | All client hosts | The bridge breaks early, or the fan tests or pushes the wrong body, in the replayed pre-trafo ticks. **This needs a fan or rope-bridge trigger within proximity range (7 m / 4 m) of a trafo; none exists in a level or the arena.** The Ball_Pos_Frame re-parent has no effect: the new ball copies the old ball's matrix and is parked there. | possible, narrow |
| 14 | **The time factor is kept live** (`physics_savestate_restore.cpp:641-660`). The clock record has no factor (`physics_rt_api.h:199-208`). The frozen check uses only the live factor (`rollback.hpp:634`, `:673`). | Level 1 tutorial unfreeze (Gameplay_Tutorial's 7 Set Physics Globals, Gameplay_Refresh "Tutorial freeze/unfreeze" #29784) | All hosts | A rewind after unfreeze tick U, anchored inside the freeze, replays the frozen ticks at 2 PSI/tick. Every awake body and the IVP clock end up 2*(U-1-A) PSIs ahead of the server. **The session-start case is refuted:** the tick-base tick fires 0 PSI and no anchor precedes the 2.0 write. | possible (code only) |
| 15 | **`--replay-scripts` (whole_frame) is not a valid control.** Unselected scripts re-run from live state on every replayed tick, and the selected ones run twice (`rollback_binding_scripts.hpp:100-106` plus `engine_->tick()`). | Experiment flag only | H with `--replay-scripts`; RC hard-codes `whole_frame = false` (`core_host.cpp:541`) | None on the default recipe. Any A/B run with it is invalid for every scripted body. | confirmed, documented |

## 2. Fixes

### F1. Trafo pieces in the rollback (closes #1, the piece half of #6, and #10)

**Status.** Step P in the WT does all of this for H and RC; it is unverified.
- Core: `movable_bodies` uses `is_ball`, not the `Ball_` prefix (`client_platform.hpp`, `client_core.hpp:927`).
- Host support: `explosion_pieces()` in `session_host_support.hpp:279`.
- Both hosts:
  - `w.parked`: `core_host.cpp:1536`, `session_client.cpp:2664`.
  - `reportable_wakeup`: `core_host.cpp:1297`, `session_client.cpp:1057`.
  - `explosion_pieces` set at the anchor: `core_host.cpp:879`, `session_client.cpp:1476`.
- `session_host_world.hpp` `destroy_body` / `insert_birth`: the `ball` hook instead of the prefix, and `parked` leaves the pieces on the parking path.
- The Random cursor is kept live: `physics_savestate_restore.cpp:662ff`.
- Tools: `first_divergence.py` `ownerless_by_name`, `worst_bodies.py`, `savestate_check.py`.

**Rule.** The `Ball_` prefix must exclude the balls only, and "ball" means the core's `is_ball` (own and mirrors). Pieces are mechanisms that no replayed script re-creates, so they are parked at their birth record and re-posed after their birth tick.

**Must not change.**
- The own ball and the mirrors stay off the mechanism path. `destroy_body` and `insert_birth` must still refuse them.
- A piece's Physicalize wake-up is never a `BodyRevived`. No other world has the piece, so the server would be told about a body it lacks.
- The time-factor keep-live rule stays as it is.

**Still open after step P.**
- **RL:** `physics_session_client.cpp:47` (filter) and `:3150` (destroy). Either port the same three hooks or declare legacy retired for physics sessions.
- **Restores that cross `Ball_ResetPieces`:** the pieces go missing from [A,R), which is #7. See F6.
- **Paper wind:** #11. See F8.

**Verify.**
1. Retail forced-rollback 13:24 arm against r2_a_ctl1. The anchor-795 window must show `deferred=0` and `parked` > 0, and `worst_bodies.py` (WT default) must report 0/22 bodies diverging. The 50:12, 97:24 and 131:47 arms must stay exact.
2. Add an arm that crosses the death and reset window. In r2_a, death is at 1369 and pieces are removed at 1434, so the arm needs anchors in 1370-1434.
3. Add an arm with two trafos to cover the cursor; step P's headless 13:24 run is the reference (0.27 m today).
4. Add a paper trafo arm.
5. Headless: the same arms with savestate on and with `--no-savestate`, plus `savestate_check.py`.
6. Unit tests: `rollback_binding_test` and `session_client_core_test`, both touched in the WT.

### F2. Pieces must not make the worlds disagree (#2)

This is not a rollback fix. Choose one of two options.

- **(a) Cheap and deterministic: filter the pieces against movables.** In `player_collision_filter` (`physics_state.cpp:970-981`), a "Ball"-group body that is not a player ball should collide only with fixed bodies. It should not collide with movable bodies other than pieces.
  - It must not change piece-vs-floor, piece-vs-piece (currently excluded by the group anyway), or any player-ball pair.
  - This is a gameplay deviation from retail: pieces no longer nudge props.
  - It is a filter change, so pairs already in contact need a recheck. Install it at the session anchor.
- **(b) Faithful: make the trafo a replicated input.** The explosion would run in every world, with the cursor seeded per event (player, tick), on the server's clone and on every mirror.
  - This is much larger: the server's parked ball never trafos, the explosion is Gameplay_Ingame logic, and #1, #10 and #11 would then apply to every world.

**Recommendation:** (a).

**Verify.**
- SimTool on L6 (s5 Trafo_Stone, P_Modul_29 11 m away) or L12 (s7 Trafo_Paper, P_Box stack): use `--explode` next to the mechanism, with and without the filter.
- Arena: add a trafo within about 10 m of a prop in the Blender testbed, then run the 4-client harness. Server and client journals must agree on every mechanism through a trafo. Today the world hash mismatches from 808 to 1041 even with no rollbacks, because of the piece rows; that hash would need to exclude piece cores.

### F3. Resync puts body and script history together (#3, #4)

**Where:** the core: the re-assign branch at `client_core.cpp:369-397` and `client_core_messages.cpp:69-136`. RL has its own copy.

**Rule.** A resync must leave the world at tick B consistent with the server for both bodies and selected scripts. Writing tick-S bodies at tick B is wrong whichever way the scripts are handled. Two ways:

- **(a) Re-step after the snapshot.** Apply the snapshot as the state of tick S, then re-step S+1..B with the relayed inputs, as a rollback anchored at S.
  - This requires the scripts' state at S. The server runs the same selected mechanism scripts (ball identity reads rewired), so it can serialize `script_replay::capture` (record v2) into the resync full snapshot.
  - The client restores them with `script_replay::restore` before re-stepping.
- **(b) Ship only script state with the snapshot.** Still apply at S and re-step, without the server's script state. The scripts would then still be the client's own, so this fixes #4 only when the client's records at S exist. They do not: `invalidate_history` runs first. So (a) is needed.

**Must not change.**
- The renumbering and pacer origin (`numbering_restarted`).
- The live-cells guard semantics.
- The non-resync snapshot path.

**Verify.**
- Level 11, two clients, retail core and legacy. Use the `r1_c_on1` / `r2_x_t1_b` recipes, which hit "tick driver rebased".
- P_Modul_26_Sack/Rope must match the server after B, with 0 sack corrections.
- Pusher001 must match the server within 1 mm in r2_x_t1_b c1.
- Headless: force a resync with `--event ... step` on the arena, and compare P_Modul_08, PE_Balloon and P_Modul_01 against the server.

### F4. A late Sector event rewinds (#5)

**Where:** the core: `client_core_lifecycle.cpp:330-336` and `sector_union.hpp`.

**Rule.** A remote Sector stamp for a tick at or before the current tick does what a late Physicalize/Unphysicalize already does: `rewind_for_lifecycle` to stamp-1. The union is re-stamped at the stamp tick, and the activation is recorded at the stamp, not at the live tick. This is plan steps 13/14 in `docs/late-player-design.md:1090-1093`.

**Must not change.**
- On-time stamps.
- The N3 own activation, which is held and stamped.
- The pacer's hold of the server's own checkpoint chain.

**Verify:**
- Repeat the a5u_lead arm (`--event 2:1500:step=300:dir=up`). Every client must log `sector +4` at the server's tick (1714 in the E0 set, `core_ref_e0_pre_q/`; 1713 in the 2026-10-24 re-record, `core_ref_e0/`).
- Sector-4 bodies must match the server.
- `first_divergence.py` must be clean after 1714.

### F5. PreSimulate queue (#6)

**Where:** the bridge, plus an engine access change in the style of #17 for `m_PreSimulateCallbacks` and `PhysicsCallbackContainer`, plus `script_replay`.

**Rule.**
- Callbacks queued by blocks of **selected** scripts are part of the script record: captured with the block and restored to the anchor's set, removing live copies the anchor did not have.
- Callbacks from **unselected** scripts are live behaviour and stay live, like the time factor.
- `check_callbacks` compares only the selected-script entries. The Gameplay_Ingame navigation callbacks must no longer refuse a blob.

**Minimum fix if the restore is deferred.** Make a replayed `SetPhysicsForce` Create idempotent: do not queue a callback if one for the same block is already queued. This stops the leaked controllers in (b).

**Must not change.**
- BMMO's own callbacks, which are already excluded.
- The fail-closed check for any entry it still compares.

**Verify.**
- Hold a key while the ball is dead or mid-trafo (lp19_a6_drop recipe), and run the swing with a missing body (e8ab2_swing_n1 sector4). Both need `sv=N/0/0`, no "PreSimulate queue differs", and a queue that does not grow across rewinds.
- `SimTool --savestate-check` needs a server, so run it separately.

### F6. Lives that end inside the window (#7, plus the piece reset after F1)

**Where:** the core for mirrors, host support for pieces.

**Rule.** A body that existed at A is present in the replay until its recorded end tick D, and is removed at D. This is the mirror image of how births are parked.
- **Mirrors:** the core already has the remote lives. In the replay, re-make the mirror at A from its record (`make_mirror_body`, which already exists for births) and apply its unphysicalize at D.
- **Pieces:** at A, re-physicalize them from their record (the host's `recreate_body` / `insert_birth` machinery), and De Physicalize them at R.
- **RC `lifecycle_rewind=false`:** the mirror's death tick must be its stamp, not the live tick. Otherwise this reduces to #5-style lateness.

**Must not change.**
- The death's own rewind anchored at D-1, which is correct today.
- Bodies destroyed for good by a level reset. 9.28 pins those.

**Verify:**
- 4-client harness at `--latency-ms 150` with deaths in the death zone and relay corrections (the ab2_s17_beam recipe).
- Count rollbacks with A < D-1, which `cross_death.py` can do. "Skipped" must be 0 for the dead ball, and a ball contacting the dying one must match the server.

### F7. Retail beam generation (#8)

**Where:** the core: `beam_own_ball` around `client_core_lifecycle.cpp:703`.

**Rule.** A beam is a move, not a new life. Without own body control it must keep the generation, as it already does with own body control (`rollback.hpp:714`).

**Must not change.**
- Death and trafo generation bumps.
- The headless path.

**Verify.** r2_a_f13 / f50 / f131: there must be no "differ from the live record" at 650/900/910/917, and `birth_recreate_failures` must drop by the beam count. Then check own death and respawn inside a window on both hosts (#8's unverified tail): a forced arm anchored at 1360-1368 in r2_a.

### F8. Activations of selected scripts issued from outside the replay (#11, #12)

**Where:** host support (`record_pending_activation` and `manager_activations` in `session_host_support.hpp:150-245`) and both hosts' bindings.

**Rule.** Extend "SectorManager activations requested by unselected scripts are recorded and re-issued" to every Activate Script whose target is a selected script, including its reset flag.
- This covers Gameplay_Events 30317 to PE_Balloon Script.
- Add `Ball_PaperWind` to the selection (its force blocks then get capture_force/restore_force), and record its two activations: Explosion 40660 pIn=1, ResetPieces 41153 pIn=0.
- With that rule in place, `kept_live` and `reset_skips` can be deleted. They are inert under `replay_activation=true` (`script_replay.hpp:429`).

**Must not change.**
- The N1 death neutralization and the 9.28 pins.
- Ball_Explosion itself stays unselected. F1 parks the pieces, and re-running the explosion would draw the cursor again.

**Verify:**
- Arena: a finisher with others still playing for more than 50 s, at `--latency-ms 150`. The balloon structure must be torn down in the finisher's world, matching the server.
- Paper trafo forced arm with anchors between explosion and ResetPieces (needs F1). Paper pieces must match the control.

### F9. Record the ActiveBall cell (#13)

**Where:** `script_replay.hpp:339-358` (`kCellCount` 3 to 4, record version bump) and `live_cells_guard` (`session_host_support.hpp:204-241`), so it stays live after the replay.

**Must not change.** The server's rewired private copies.

**Verify.** A unit test in `rollback_binding_test` with the cell changed inside a window. There is no level geometry to test it on.

### F10. Time factor per tick (#14)

**Where:** the rollback records and the binding.

**Rule.** Record `m_PhysicsTimeFactor` in each tick's clock state. Each replayed tick is stepped at its recorded factor, and the live factor is put back after the replay. As a cheaper fallback, a window whose recorded factors differ from the live one is treated like the frozen path: drop the history and resync.

**Must not change.** The blob still must not reinstall the factor. The original bug was a blob put back at 1.0 for good.

**Verify.** Level 1 multiplayer through the tutorial end, with a relay correction anchored inside the freeze. The PSI count must match the server.

### F11. Pooled modules (#9)

**Where:** gameplay first, in the sector union (`client_core_frame.cpp:188-198` `run_sector` and the server's union).

**Rule.** A sector activation must not re-run Type 1 (Set World Matrix + script reset) on a PH Object that an earlier union sector still holds. Either skip those rows, or (heavier) give each sector its own copy at load. Once the module is not moved while in play, its rollback half (joint presence in `script_replay.hpp:853-863`, unrecorded frame matrices) disappears.

**Must not change.** Single-sector levels, the arena, and N2's no-deactivate rule.

**Verify.** SimTool L8: `--drop P_Modul_30 1 --drop-sector 1 --drop-move 2 120`. The Wippe must rest at a 0.085 m tilt, not sag 1.158 m. Then a two-client L8 run across s1 and s2.

### F12. `--replay-scripts`

No fix. Keep it out of every A/B.

## 3. Covered: do not re-audit

**Body inventory** (LOCAL checkpoints of 500 journals, a grep over 4532 .bmjr, SimTool PH and list_bodies for L1-13):
- Own ball: tracked by entity. Mirrors (`_Peer_` / `_BMMO_`): `remote_entities`. Server clones never roll back.
- Every non-ball movable is tracked: P_Modul_01/03/08/19/25/26/29/30/34/37/41 parts, Modul17_Dreharme, PE_Balloon_*, PE_Box_slide, P_Ball_*_MF*, P_Box_MF*.
- No two bodies share a name.
- Fixed bodies (A_*, FixCube, Rinne/Filler, P_Dome) do not move. Born in the window, they are rechecked in at their birth tick, which is correct.
- These have no physics body: trafos, fans (force on the ball), P_Extra_*, PC_/PS_ flames, the death zone.

**When the blob is applied** (all hosts):
- The IVP clock, PSI times, next_movement_check and env state; m_DeltaTime; ivp_srand.
- Core integrator state, queued speed_change, both quaternions.
- Calm refs, unit countdowns and sleep, m_world_f_core_last_psi.
- Mindists, friction systems and contact points, sim units, controller order.
- The OV tree, the cache ring, m_MovableObjects, and the CK matrices of physicalized entities.
- Gravity is asserted only. Nothing in a session changes it.

**Script restore** (P_/PE_ minus P_Extra, plus Gameplay_SectorManager; `replay_activation` on both hosts):
- SetPhysicsForce exists/pos/force.
- Joint handle presence. IVP constraints carry no state of their own.
- Delayer, Sequencer and Proximity locals and outputs; delayed links; the three cells, kept live afterwards.
- Sector activations re-issued, including those requested by unselected scripts.
- The N4 phase cell.
- The 9.27 Sequencer counters.
- No selected script has a Random block or waits for a message.
- The Level_Finish re-send from a replayed PE_Balloon is harmless: Wait Message 30077 is one-shot.

**Navigation:**
- `bmmo_physics_nav_state` is set before the blob, and controllers are named `bmmo:force/<leaf>`.
- Mirrors are re-driven with `remote.applied` (`client_core_frame.cpp:331-345`), and the own ball's polling is re-armed.
- Retail navigation leaves are zeroed, so they push 0. The `rt:force` order notes on Ball_Stone (4331 lines) are cosmetic.
- `wake_pending_` is negligible.

**Camera rows:** record(T+1).cam is the camera step T+1 used.

**Spawn impulse:**
- Consumed in the same frame.
- Birth records are taken after the impulse.
- Re-made bodies are kicked again.

**Other:**
- Body guard keep-free list (#13): refilled at the anchor and at every explosion.
- Collision filter: stateless.
- Hull cache (#9): process-wide and correct.
- Other Random blocks (Blitz, sound) feed no physics.

**Non-mechanism scripts that touch bodies:**
- Gameplay_Ingame: ball lifecycle and navigation. Covered by own_life records and replicas.
- Event_handler: pause neutralized; level reset is 9.28.
- Balls_Init and Levelinit_build: load time.
- Ball_CollSound_Pieces: sound only.
- AnimTrafo: no physics.

**Checked and refuted:**
- **Collision group ident not in the blob:** it is fixed per life. The own ball's group set after its birth step is a live prediction difference, not a rollback gap.
- **Force controller orphaned by `destroy_body`:** unreachable after N4 and cf08f11.
- **The own beam's 1-step server/client difference at 1300:** a live model difference (write in place vs fresh body), automation only.
- **Solo forced-rollback script diffs:** all explained by #8.
- **Time factor at session start:** the tick base fires 0 PSI.
- **Measurement blind spots:** fixed in the WT tools. `client_divergence.py` is still blind: no `ownerless_by_name`, and it skips bodies that exist only in the arm.

**Clean in data:**
- Retail solo forced arms: every non-piece body is bit-exact.
- r1 two-client runs without a resync: exact apart from healing prediction lag (Pusher001 508-542).
- Sector ticks are aligned in all 10 recent headless runs.
- The P_Modul_03, balloon and Schaukel residue in cpswrv*/e8ab2 is identical on the non-owning clients, which points to prediction, not restore.
- e8ab2 c3's 6 lost sector-3 bodies are the N4 bug, now fixed.

## 4. Recommended order

1. **F1 (pieces, step P):** gate it on RC and H with the arms listed in F1. Decide RL: port the hooks or retire the path. Step P already changes the core (`is_ball`) and both hosts' bindings, so **H and RC must land together**: the core's `movable_bodies` contract changed, and a host still filtering `Ball_` would compile and silently keep #1. The Random cursor half (#10) is inside step P; keep it.
2. **F2 (pieces vs movables filter):** directly after F1. It is the only thing that makes pieces safe across worlds, and it lives in `physics_state.cpp`, so it applies to H, RC, RL and the server at once. Pick (a) unless faithful piece physics is wanted.
3. **F3 (resync carries script state, re-steps S to B):** the largest measured error after the pieces (metres, permanent, about 1 in 2 resyncing clients). The core and the server's snapshot sender change together. RL needs its own port or retirement.
4. **F4 (late Sector rewinds):** core only, so both core hosts get it at once.
5. **F5 (PreSimulate queue):** the engine/bridge access change goes first. Ship the dedup of `SetPhysicsForce` Create first if the full restore is slow to land. It affects every host that runs `script_replay`.
6. **F6 (lives ending in the window):** mirrors in the core, pieces in host support. It must follow F1, because tracked pieces otherwise inherit the gap at `Ball_ResetPieces`.
7. **F7 (retail beam generation):** core. Also check own death and respawn on both hosts.
8. **F8 (outside activations of selected scripts + PaperWind selected):** host support, used by both hosts' bindings, so land H and RC together. It depends on F1.
9. **F11 (pooled modules):** gameplay. Server and clients must change together, because the union's activation set is a lockstep input.
10. **F9, F10:** narrow (no geometry for F9; Level 1 tutorial only for F10). Both are record-format changes, so bump the journal and record versions on both hosts together.

**Must land in both core hosts at once:** F1, F3, F5, F6 (pieces half), F8, F9, F10. F2 and F11 also need the server.

**Core-only, so both get it automatically:** F4, F6 (mirror half), F7.