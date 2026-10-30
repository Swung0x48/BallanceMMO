# Host platform audit (FULL step AUD, 2026-10-30)

The user's decision of 2026-10-30, "Shared helper + audit" (the answer to
FULL INV's finding 2, the one-tick-late beam into a probe): BMX made the
mirror clone one shared helper (`game/mirror_clones.hpp`), and this audit
lists every other platform operation and host-side rule the three hosts
still implement each for itself, says whether their semantics are the same
(read from the code at HEAD 4f55d447, quoted by file:line), and ranks every
difference by what it can do to a world. Offline, by reading; nothing was
built or run.

**Hosts.** `PW` = the server's world (`BallanceMMOServer/sim/physics_world.cpp`,
driven by `session_runner.cpp`, `SR`); `SC` = the headless client
(`BallanceMMOServer/sim/session_client.cpp`; its platform is `make_platform`,
SC:1650-2110, `add_trafo_operations` SC:1508-1639, the capabilities SC:2117-2136);
`CH` = the retail core host (`BallanceMMOClient/session/core_host.cpp`; its
platform is `core_host_platform`, CH:2292-3321, the capabilities CH:1320-1342;
the N1-N3 guards it shares with the legacy path are in
`BallanceMMOClient/session/physics_session_client.cpp`, `PSC`). The platform
interface is `BallanceMMOCommon/include/session/client_platform.hpp` (`CP`).
The server is no core host: its column names the code that does the same
job in the authoritative world, or "n/a" with why.

**Verdicts.** `same` = identical semantics by reading (the same shared helper,
or two copies that do the same thing); `same*` = identical in every session
the defaults run, different only under a test switch, a diagnostic or a
display path; `DIFF` = a difference, numbered `D<n>` and ranked in §9. "None,
by reading" = no effect on any world was found by reading the code; nothing
was measured here.

## 1. Transport, time and the journal

| operation (CP line) | PW / SR | SC | CH | verdict |
|---|---|---|---|---|
| `send` (CP:461) | n/a (the server's own GNS send) | SC:1652 `send_bytes(bytes, flags)`; `--stall-after-ready` sleeps after SessionReady (SC:1661, test switch) | CH:2296 `send(copy.data(), ...)` (a copy: `client::send` takes a mutable buffer) | same* |
| `now` (CP:465) | SR's `clock::now()` | SC:1672 `wall_now(site)` (the inbound capture / replay of the clock) | CH:2303 `clock_type::now()`, the site ignored | same* (SC's capture is a replay harness, CP:463) |
| `log` (CP:468) | `log()` | SC:1673 `log_line` | CH:2304 `logger_->Warn/Info` | same* |
| `utc_ms` (CP:471) | journal header | SC:1676 `wall_utc_ms(site)` | CH:2308 `system_clock::now()` | same* |
| `journal_path` (CP:799) | SR:230 `journal_path(session)` | SC:1703 `--journal`, `.rejoin<N>` beside it | CH:3238 `ModLoader/BMMOJournals`, newest 10 kept, only with the SessionJournal property | same* (location only) |
| `io_strand` (CP:806) | SR's own | SC:1701 `session_io_strand()` | CH:3226 `strand_poster(thread_pool_)` | same* |
| `journal_flush` (CP:811) | - | SC:1702 `--io-flush` | CH:3227 the SessionJournalFlush property | same* |
| `counters` (CP:861) | n/a | SC:4612 | CH:3248 (`rebases` from `fixed_tick_`) | same* (report text) |
| `world_hash` (CP:488) | SR:1408 `capture_world_hash` | SC:2071 `capture_world_hash` | CH:2353 `physics_view_.capture` (the bridge's same function) | same |
| `list_bodies` (CP:477) | `snapshot_for_journal` (PW:1558) | SC:1681 `physics_direct.list_bodies()` | CH:2320 the same under `report_precision_53` (x87 53-bit reads, TR4c) | same* (journal rows only; the world is not touched) |

## 2. Reading the world

| operation | PW / SR | SC | CH | verdict |
|---|---|---|---|---|
| `body_state` (CP:475) | `get_body_state` | SC:1677 `get_body_state` | CH:2314 `physics_view_.get_body_state` | same |
| `sector_manager_active` (CP:482) | PW:890 `manager_script->IsActive()` | SC:1832 found once by `find_root_script`, cached id | CH:2324 `physics_session_sector_manager_active()` | same |
| `poll_sector` (CP:486) | n/a (Sector events are inputs, PW:979) | SC:2860: the resetpoint watched (`resetpoint_moved`, SC:2862), the private cell (B2SX) or IngameParameter[0][1] always read, `checkpoint_held = checkpoint_held_` (the anchor's hold, SC:2637) | CH:2329: no resetpoint watch; the cell read only while the hold stands (`sector_activate_.block && !sector_activate_failed_`), `checkpoint_held = cell_read`; crossings outside the hold come from the checkpoint hook (CH:3788) | **D7**, **D11** |
| `own_ball` (CP:491) | n/a | SC:2022 CurrentLevel[0][1], `ball_type_of` (SC:2983, its own copy of the ball table) | CH:2357 `get_current_ball()`, `ball_row_of` (CH:613, reads the table each call) | same |
| `own_replica_name` (CP:501) | n/a | SC:2033 | CH:2408 | same |
| `discover_navigation` (CP:505) | PW:1038 every leaf keyed (`complete`) | SC:2005 every leaf keyed; installs the OWNB Physicalize hook (SC:2019) | CH:2412 unbound leaves dropped and renumbered, >= 4 leaves | **D12** (none with the mod alone) |
| `drain_body_events` (CP:511) | n/a | SC:3675: `recipe` lines go to the birth log (`local_births_.add`), the rest to the core | CH:2435: no recipes (the retail birth log is fed by OnPhysicalize, CH:3625) | **D4** |
| `reportable_wakeup` (CP:514) | n/a (PW applies BodyRevived unless the union is on, PW:1008) | SC:2041 not `is_ball`, not own, not `is_mirror_name` ("_BMMO_"), not an explosion piece | CH:2457 the same with `"_Peer_"` | same (the name tests sit behind `is_ball`) |
| `observe_input` (CP:516) | n/a | SC:3706 keys from `frame_keyboard_`, `nav_active` = any graph leaf's Key Event active (SC:3216), never paused | CH:2471 keys only with the input hook and no mute; `nav_active` from the replica's key blocks, else the graph's; `paused` = the mute | same* (the leaves are the same blocks; PAUSED is retail-only by design, CP:267) |
| `own_camera_rows` (CP:520) | n/a | SC:2046 | CH:2507 | same |
| `outside_forces` (CP:529) | n/a | SC:2057 `read_forces<physics_direct>` | CH:2516 `read_forces<physics_view>` | same |
| `movable_bodies` (CP:536) | n/a | SC:4172 `movable_body_names` (physics_direct.hpp:231: `body.movable`, table order) | CH:2522 the same filter written inline over `list_bodies` | same (two copies of one filter) |
| `note_script_resets` (CP:539) | n/a | SC:2061 | CH:2530 | same |
| `script_guards` (CP:543) | once at the anchor, never kept (PW:329-397: N3 hold, N2 pin, depth test, N4, chain cell; no N1: the server's retail ball never dies) | SC:1930 N1 `reset_guard_.apply` (game/sector_reset_guard.hpp), N4 keep, N2 `repin_checkpoint_deactivation`, chain cell keep, tutorial hold keep, depth test keep - **no N3 keep** | CH:2536 N1 `death_reset_apply` (PSC:986, retail's own copy), N2 `sector_deactivate_apply` (PSC:1124), **N3 `sector_activate_apply`** (PSC:1266), N4, chain cell, tutorial hold, depth test | **D3** |

## 3. Writing the world: bodies, sectors, the resync

| operation | PW / SR | SC | CH | verdict |
|---|---|---|---|---|
| `write_body` (CP:552) | n/a (the server never beams) | SC:1685 `set_body_state` | CH:2555 `physics_view_.set_body_state` (clears `error` first, CP:549) | same |
| `run_sector` (CP:558) | PW:897 `run_sector_pass` | SC:2845 `run_sector_pass` | CH:2563 `run_sector_pass`, plus `sector_cell_is_own_` bookkeeping for the legacy sector display | same |
| `record_activation` (CP:562) | n/a (no rollback) | SC:4784 `manager_activations_.record` | CH:2578 `host.activations.record` | same |
| `record_pending_activation` (CP:565) | n/a | SC:1980 with the cached manager id | CH:2770 finds the manager each call | same |
| `record_script_order` (CP:573) | n/a (the trace PW:1100 only) | SC:1985 | CH:2775 | same |
| `set_union_pass` (CP:580) | n/a | SC:1845 | CH:2585 | same |
| `sector_cell_adopted` (CP:587) | n/a | SC:1849 the cell | CH:2589 the cell, plus `sector_cell_is_own_` | same |
| `localize_scripts` (CP:596) | the producer: `capture_portable` (PW:1588) | SC:1859 `localize_session_scripts` | CH:2601 the same helper | same |
| `forget_activations_from` (CP:602) | n/a | SC:1872 | CH:2613 | same |
| `forget_births_after` (CP:608) | n/a | SC:1880 | CH:2621 | same |
| `forget_pending_activations` (CP:615) | n/a | SC:1886 | CH:2626 | same |
| `birth_tick_of` (CP:620) | n/a | SC:1889 | CH:2627 | same |
| `unpack_bytes` (CP:624) | the packer (CKPackData) | SC:1890 `CKUnPackData` | CH:2628 `CKUnPackData` (retail CK2.dll) | same |
| `local_only_body` (CP:628) | n/a | SC:1901 `!trafo_on_ && explosion_pieces_` | CH:2639 the same | same |
| `place_probes` (CP:723) | PW:604 `proximity_.place(probe_candidates())`, called first in `SR::step` (SR:1192) | SC:2087 `proximity_.place` | CH:3262 `proximity_host.union_.place` | same (candidates: PW's `p.physicalized && p.ball`, PW:587; the core's "ball with a body") |
| `tutorial_exit` (CP:734) | PW:1077 `tutorial_exit_.activate`, frame counted from the world's first step | SC:2092 | CH:3267 | same (one helper, game/tutorial_exit.hpp) |
| `depth_test` (CP:847) | PW:1068 `depth_test_.step` after the trafo frame | SC:1566 | CH:2701 (over `physics_view*`) | same (one helper, game/depth_test.hpp) |

## 4. Mirrors, the own ball, driving

| operation | PW / SR | SC | CH | verdict |
|---|---|---|---|---|
| `mirror_entity` (CP:637) | PW:742 `clone_ball` -> `mirror_clones_.get` | SC:1715 -> `remote_entity` (SC:3868) -> `mirror_clones_.get` | CH:2781 `objects_.get_ball_entity` (game_objects' spirit balls, made per player at room join, named `<ball>_Peer_<n>`, game_objects.h:369) + `check_mirror_contract` (CH:779: counted and logged, not enforced) | **D10** |
| `make_mirror` (CP:644) | PW:921-967 `physicalize` in `p.group`, spawn ghost birth, kick | SC:1719 `make_mirror_body(direct, ...)` | CH:2791 `make_mirror_body(physics_view_, ...)` + the spirit ball's display state (`set_physicalized`, `on_trafo`) | same (one helper, session_host_support) |
| `unmake_mirror` (CP:648) | PW:970 `unphysicalize` | SC:1734 | CH:2811 + display | same |
| `detach_mirror_navigation` (CP:650) | PW:971 `navigation->shutdown_all()` | SC:1738 `navigation_destroy` | CH:2817 the same | same |
| `attach_mirror_navigation` (CP:654) | PW:790 `player_navigation` per player (ball_navigation.hpp, the same class the bridge registry runs) | SC:4285 force of `ball_rows_[type]` | CH:2821 `ball_force(type)` | same |
| `mute_event_log` (CP:658) | n/a | SC:1746 | CH:2825 | same |
| `own_driven_entity` (CP:662) | n/a | SC:1690 | CH:2831 | same |
| `drive` (CP:665) | PW:836 `set_input` -> `player_navigation::apply` in its PreSimulate callback | SC:1949 `navigation_input` | CH:2832 the same through the bridge | same |
| `force_own` / `unforce_own` (CP:671-672) | n/a | SC:1954 / SC:1964 polling back on | CH:2837 / CH:2847 polling back as the mute says | same* (SC never pauses) |
| `arm_own_capture` (CP:676) | n/a | SC:1968 (the PreSimulate capture, SC:3036) | empty: own births from OnPhysicalize (CH:2851) | design (own_lifecycle_source, CP:70) |
| `ball_recipe_of` (CP:683) | PW:696 `retail_recipe` | SC:1750 `fill_retail_recipe` (SC:2990) | CH:3017 `fill_ball_recipe` (CH:632) | same (three copies of one rule: friction/elasticity/mass/damps from Physicalize_GameBall, sphere r=2 or the paper convex) |
| `own_birth_capture` (CP:688) | n/a | SC:1755 -> SC:3795 | empty (host_hooks) | design |
| `make_own` (CP:695) | n/a | SC:1756 -> SC:2940 (`hull_probe` diagnostic) | CH:2860 (in a rewind only: `own_body_control` false) | **D1**; **D14** for an unknown ball row |
| `unmake_own` (CP:700) | n/a | SC:1757 -> SC:2919 | CH:2891 | same (both through `unmake_own_body`) |
| `entity_matrix` / `set_entity_matrix` (CP:709-710) | n/a | SC:1762 / SC:1819 | CH:2913 / CH:3005 | same |
| `own_body_upkeep` (CP:496) | n/a (a clone gets `p.group` at its Physicalize, PW:933) | SC:3649: body guard, group, replica attached only with `--rollback` (default on) and a body | CH:2369: body guard, group, replica always attached (polling off while muted) | same* |

## 5. The trafo, the airship, the spawn ghost

| operation | PW / SR | SC | CH | verdict |
|---|---|---|---|---|
| `poll_trafo` (CP:820) | n/a (Trafo events are inputs, SR:988) | SC:1509 (`--trafo-no-resets` test switch) | CH:2644 | same* |
| `make_trafo_set` (CP:827) | PW:803 at `add_player` (PW:1396) | SC:1532; the core calls it on a member message or first sight (client_core_trafo.cpp:79, client_core_lifecycle.cpp:284) | CH:2662 | same (`ensure_trafo_set`) |
| `run_trafo`, `trafo_final`, `trafo_frame` (CP:831-839) | PW:1447, PW:1063 | SC:1553-1562 | CH:2689-2698 | same |
| `trafo_type_set`, `trafo_reconcile`, `retire_trafo_sets` (CP:849-855) | PW:1480 `after_trafo_tick` (retirement) | SC:1571-1638; a remake's births from the muted recipes (SC:1603-1613) | CH:2706-2767; a remake's births through OnPhysicalize at `trafo_host.birth_tick` (CH:2739) | same, two routes into the birth log (**D4**) |
| the trafo set hooks (not a CP field) | `sim/trafo_hooks.hpp` (`has_body` by `GetPhysicsObject`, `restore_originals` set) | the same header | CH:1073 `make_retail_trafo_hooks` (`has_body` through the bridge, `restore_originals` empty; the anchor quiesces before `reset_session_clock`, CH:1801, whose restore does it) | same by two routes (HU1 queue (a)) |
| `airship_zone`, `airship_zone_contains`, `airship_zone_box` (CP:741-746) | PW:469 `airship_zone_set(true, "")` (no own group) | SC:1777 / 1786 / 1809 | CH:2924 / 2933 / 2934 | design (exception c: the balloon is per world) |
| `show_ghost` (CP:755) | n/a | empty (draws nothing) | CH:2959 | display only |
| `spawn_ghost`, `spawn_ghost_state`, `spawn_ghost_birth` (CP:764-773) | PW:487 `spawn_ghost_set(true, kSpawnGhostRespawns, kSpawnGhostRadius)`; PW:932 a SPAWN birth; **PW:829 `spawn_ghost_forget` at a leave** | SC:1791 / 1803 / 1806; no forget | CH:2983 / 2995 / 2996; no forget | **D2** |
| `show_spawn_ghost` (CP:777) | n/a | empty | CH:2997 | display only |

## 6. Rollback binding and hooks

| operation | SC | CH | verdict |
|---|---|---|---|
| `core_world_hooks` (CP:168-243) | the core's half of the binding, handed to the host's `bind_world` and composed by the shared `bind_session_world`; no host implements any of its 16 fields | the same | same (not a host operation) |
| `bind_world` (CP:785) | SC:4331: `restore_clock = --restore-clock` (default false), `savestate_strict` (default false), `park_births` / `insert_births` (default false), `whole_frame` only with `--replay-scripts`, `bind_scripts` when `--script-replay` (default on) and a selection; diagnostics (`diff_savestate`, step / restore traces); `pre_step` wrapper logs a replayed tick's births from the muted recipes, balls and known births skipped (SC:4567) | CH:3027: `restore_clock = false`, `savestate_flags = 0`, no park/insert, `bind_scripts` when a selection; `pre_step` wrapper sets `trafo_host.birth_tick` / `if_absent` so OnPhysicalize logs a replayed tick's births (CH:3156) | same* at the defaults; the birth log's route is **D4** |
| `keep_live_cells` (CP:791) | SC:1692 `live_cells_guard(..., !park_births, ...)` | CH:3183 the same with `true`, plus a log of own bodies made / taken out | same* |
| `hooks.numbering_restarted` (CP:85) | SC:1907 the pacer's origin (DX9: a resync keeps the phase) | empty (the fixed tick driver counts on its own) | design |
| `hooks.filter_event` (CP:88) | SC:1919 `--hold-event` | CH:3302 `session drop-birth` | test tools |
| `hooks.ending` (CP:91) | SC:1924 discards held events | empty | test tool |
| `hooks.after_rewinds` (CP:98) | SC:1948 `--force-rollback` | CH:3274 `session force-rollback`, one-shot rewinds, mirror-birth log | test tools |
| `hooks.after_record` / `after_amend` / `after_journal` (CP:99-101) | SC:2079 / 2080 / 2097 dumps and traces | empty | diagnostics |
| `hooks.window` / `hooks.status` (CP:108-109) | SC:2095-2096 | CH:3270-3271 | same (print only) |
| `hooks.ended` (CP:114) | SC:2104: body guard off, then `end_trafo_sets` | CH:3312: `trafo_teardown`, then body guard off | **D9** |
| `hooks.resync_hold_ended` (CP:119) | SC:1918 the origin moves by the hold | empty (the driver holds itself) | design |
| `hooks.describe_movables` (CP:125) | SC:2108 | empty | diagnostic |

## 7. Capabilities (CP:875-909)

| flag | SC (SC:2117) | CH (CH:1320) | verdict |
|---|---|---|---|
| `own_body_control` | true | **false** ("the game's Physicalize block owns the own ball", CH:1321) | **D1** |
| `own_body_in_replay` | true | true | same |
| `slew` | true | true (R5) | same |
| `nonblocking_hold` | true | false (the anchor hold blocks in OnProcess, CH:2009) | design (CP:872: one state machine) |
| `whole_frame` | `--replay-scripts` (default false) | false | same* |
| `own_lifecycle` | `event_log` | `host_hooks` | design (CP:67-70) |
| `trafo_inputs` | `!--no-trafo-cap && --script-replay` (default true) | true | same* |
| `placement` | true | true | same |

## 8. Host-side rules outside the platform struct

| rule | PW | SC | CH | verdict |
|---|---|---|---|---|
| The own birth's pose, spawn flag and birth cell | n/a (the event's pose, PW:927) | SC:3079 `before_physicalize` (the OWNB Physicalize hook, before the block makes the body) and SC:3137 the PreSimulate capture | CH:3625 OnPhysicalize (BML broadcasts before the block makes the body) | same (OWNB, SFD) |
| The own group before the birth tick's PSIs (OWG) | the clone is made in `p.group` (PW:933) | SC:3156 `adopt_own_group` in the PreSimulate capture | CH:3750 `own_group_queue` into this frame's PreSimulate pass | same |
| The own spawn kick | PW:953 index from `p.slot` | SC:3207 from `own_join_order()` | CH:3758 from `own_join_order()` | **D8** |
| The birth log | n/a | SC:3684 `add` (append) from the event log's `recipe` lines (engine change #19: every Physicalize physics_RT runs outside a mute) | CH:3675 `put` (replace the same name at the same tick) from OnPhysicalize (script Physicalize blocks only) | **D4** |
| The anchor sequence | PW:180-453: clock reset, counters, placement, hash, ball rows, filter, airship, spawn ghost, rewire (class `all`), trafo, selection, N3, N2, depth test, N4, chain cell, census, tutorial | SC:2553-2748: clock reset, counters, placement, hash, filter, ball rows, selection, trafo, rewire (`mechanism`), census, N1, N3, N2, depth test, N4, chain cell, tutorial | CH:1787-2033: trafo, clock reset, clock guard, pause edits, N1, N2, N3, N4, chain cell, tutorial, depth test, counters, placement, hash, filter, selection, rewire (`mechanism`), census | **D5** (HU1 queue (d)) |
| The proximity rewire's class | PW:286 `probe_class::all` (union rules under the bit) | SC:2767 `probe_class::mechanism` | CH:974 `probe_class::mechanism` | design (world-agreement-gaps §2.3: only the mechanism class is the union's) |
| The controller alias | PW:129-133 | SC:2795 lambda | CH:1058 `controller_alias_cb` | same (trafo sets, then the Q7 units) |
| A player leaves | PW:812 `remove_player`: navigation shut down, clones released and destroyed, CamRef destroyed, **`spawn_ghost_forget(group)`** | the core closes the life (client_core_lifecycle.cpp:302-319); the clone kept (SC:3864, the helper's rule) | the same; the spirit ball kept by game_objects | **D2**; the clone release is by design (mirror_clones.hpp `release_player`) |
| The frame loop and clock | SR `step` on the scheduler | SC:3224 `pace_and_frame`: `origin_`, slew, rebase on 33 frames behind | CH:2041 `core_host_frame`: fixed tick driver, clock guard re-attached per frame, pause clock and pause edits (retail has an ESC menu) | design (core design §3) |
| The session's end | the world destroyed with the session | SC: `hooks.ended`, then begin_session hands the edits back at the next session (SC:2452-2488) | CH:2136 `core_host_end_local` hands every edit back at once | same* (the order of hand-backs; the level outlives a retail session) |
| Lives and collectibles | PW:797 `adds_` / `lives_` (TRL); collectibles per world (record-format decision PH[*][4] (b)) | the core's member lives; the census `array_reads::anchor_log` (SC:2616) | the same (CH:1970) | same (no host operation) |
| Display and automation | - | - | spirit balls, ghosts' translucency (`show_ghost`, `show_spawn_ghost`), `beam`, `sector`, `journal` verbs | retail only, no world effect except the beam (**D1**) |
| Host -> core entry points (not CP fields) | n/a | `--beam` / `--beam2` -> `core_.request_beam` (SC:3522-3525); `--start-sector` / `--sector-at` -> `core_.request_sector` (SC:2723); own births / deaths through the event log (`report_physicalize`, client_core_frame.cpp:771-774); no Finish event (the headless runs never reach a level end); a leave is the process exit (`--leave-at`) or `--leave-room-at` | `core_host_on_physicalize` / `on_unphysicalize` (CH:3625 / 3773, queued, committed at post_step); `core_host_on_sector` (CH:3788, **D7**); `core_host_on_finish` (CH:3803: a Finish event stamped `current_tick()`, T - 1 inside frame T); `core_host_leave_player` (CH:3815: only answers whether a core session runs); `core_host_beam` / `due_beams` (CH:3822 / 3868 -> `request_beam`); `core_host_sector` (CH:3892 -> `request_sector`) | same* (Finish only marks `p.finished` in the server's world, PW:1001, which nothing in the world reads: no world effect; the beam is **D1**) |

## 9. The differences, ranked

Ranked by what they can do to a world: first the ones that make two worlds
differ from the same inputs (measured, then by reading), then the ones that
change only when an input is stamped or a host's own diagnostics, then
host nature. "Candidate" names where the fix belongs; none of them is a
FULL fix: none can move the headline (the headless own ball 0.000000 m,
FULL plan H1C2), because every one needs a retail host, a level reset, a
leave followed by an unflagged first birth or a failed anchor edit, and the
headline's runs have none of these.

**D1. Retail has no own body control - a beam is an in-place write, a moved
own birth costs a snapshot or a resync (measured).** CH:1321
`caps.own_body_control = false`; the core's fallbacks: the beam's stamp
writes the live body in place (client_core_lifecycle.cpp:2156 `write_beam`)
where the server makes a fresh clone (PW:921-933), and a moved own birth is
"left to the snapshots" or resynced (client_core_lifecycle.cpp:2705-2717
`resync_own_life(..., "no own body control, no snapshots")`); SC rebuilds
the body at its tick (`remake_own_body`, SC:2940). Effect, measured: the
in-place body keeps its contacts and lane state into the stamp - EG0's arena
death beam 8.6 mm / 0.879 m/s off the server at 1300; FANX's beam off the
paper trafo plate 1.7 cm off by 620 (roadmap Stage C item 9). With snapshots
off every moved own birth on retail is a resync. **Already queued: Stage C
item 9 (R6, retail own-body control).** Design 2.3 settles today's state;
nothing new here.

**D2. A leaver's spawn-ghost group: the server forgets it, the clients keep
it (by reading; never observed).** PW:829 `spawn_ghost_forget(manager,
p.group)` at `remove_player`; no client path calls it (no platform
operation exists for it: grep of `BallanceMMOCommon/src/session`, SC and CH
finds none), and the core's member removal only closes the life
(client_core_lifecycle.cpp:302-319). physics_state.hpp:459-465: after a
forget "the next member that takes the join order begins its first life as
a ghost however its first body is made". Effect: a member that takes a
freed join order and whose first Physicalize is not SPAWN-flagged is a ghost
in the server's world and solid (the `resumed` path) in every existing
member's world: different collision pairs from the same inputs until it
leaves the spawn area. A second, already observable effect: every client
keeps the leaver's group record in its IVP savestate
(physics_savestate_capture.cpp:819-836, `spawn_ghost_record_get`) while the
server's blob no longer has it, so any cross-host savestate comparison after
a leave (`--savestate-check` / `diff_savestate`, a late joiner's blob against
an existing member's state) reports a `spawn_ghost` group difference; until
AUD-1 lands that difference is expected, not a new divergence. A rejoiner's
first birth at the level start is
SPAWN-flagged, which makes it a ghost everywhere, so the common rejoin is
unaffected; a late joiner and a resynced member take the server's state
through the blob. The roadmap (§4 item "SPGH2", docs/roadmap.md:703-708)
says "M0/M1 has them drop it too"; WM0/WM1 did not add it. **Candidate:
Stage C item AUD-1** - a platform operation `spawn_ghost_forget(group)` both
clients bind (SC and CH already reach the bridge's `spawn_ghost_*`), called
by the core when a SessionMember `removed` closes a life, at the server's
tick of the removal; inside a rollback window the forget would have to be
replayed like a lifecycle event (the IVP savestate carries the group
records, physics_state.hpp:426-431), which is the design question to settle
with it.

**D3. The checkpoint hold (N3) is re-applied every frame only on retail,
and N1-N3 are retail's own copies (by reading; never observed).** CH:2539
`sector_activate_apply()` (PSC:1266) re-holds the Script input when "a level
reset handed the retail source back" (PSC:1264); SC holds once at the
anchor (SC:2637, the shared `hold_checkpoint_activation`,
game/sector_activation.hpp:93) and its `script_guards` (SC:1930) keeps N1,
N2, N4, the chain cell, the tutorial hold and the depth test but not N3;
`checkpoint_held_` is never checked again. PW holds once and its world never
resets a level. Effect: if a mid-session level reset does hand the input
back, a headless crossing runs Gameplay_SectorManager in the frame of the
crossing (the pre-N3 failure: the sector's bodies ticks early in this world)
while `poll_sector` still says held and stamps the same sector ahead.
Unobserved: no "held again" or "re-pinned" line in any log under
maps/arena-test, .local-tools/work or the agent game's ModLoader. N1 and N2
are kept on both clients by two copies of one edit (PSC:986 against
game/sector_reset_guard.hpp; PSC:1124 against
game/sector_deactivate_pin.hpp `repin_deactivate_cell`, which retail
already calls). **Candidate: Stage C item AUD-2** - one N3 helper with a
`keep()` in game/sector_activation.hpp (the headless guard keeps it; retail
moves `sector_activate_*` onto it), and N1 retail onto
`sector_reset_guard` at R7 (the legacy path shares PSC's copy until then,
the reason HU1 queued (b)). **FULL check, not a fix:** the batch that runs
`--reset-level-at` (H5b / RK3R-rec) could settle it, but no log line today
shows a manager run outside a union pass (`manager_activations::record_pending`,
game/session_host_support.hpp:342, records it silently): the check needs a
diagnostic line at that record (or a SimTool read of the Execute Script's
Script source after the reset) - none exists, so grepping today's logs
would be a false pass.

**D4. The birth log is fed two ways (by reading).** SC appends every
`recipe` line of the event log (`local_births_.add`, SC:3684; engine change
#19: every Physicalize physics_RT runs while the log is not muted) and, in a
replay, the muted recipes of the bodies the replayed tick made (SC:4567).
CH replaces by name and tick (`host.births.put`, CH:3675) from OnPhysicalize,
which fires for a script's Physicalize block only, and in a replay logs
through `trafo_host.birth_tick` / `if_absent` (CH:3156). `find(entity,
tick)` returns the first entry (session_host_world.hpp:245-251). Effect:
(a) an entity physicalized twice in one live frame - SC restores the first
birth's matrix and recipe at a later rewind, CH the last; (b) a body made
during a live frame through the bridge outside a mute (not a script block)
has a birth entry on SC only. Every body the core makes itself is a ball
(excluded, `is_ball`), so (b) needs a host-made level body, of which none
was found; (a) needs a level script that physicalizes one entity twice in a
frame, not looked for. **Candidate: Stage C item AUD-3** - one feed rule
(`put` or `add`, one source) in host support, decided with R6, which moves
the retail own births onto the bridge anyway.

**D5. The anchor sequence is written per host in a different order (by
reading).** §8's row: retail edits N1-N4, the tutorial and the depth test
before the counters' restore and the placement; SC and PW after; PW rewires
the probes before the selection (class `all`), the clients after it; retail
quiesces the trafo originals before the clock reset. The CK objects these
create (the hold's `BMMO_SectorHold`, the pins' constants, the copies, the
union's units) get IDs in a different order per host. No effect found: each
edit touches its own blocks, the copies keep their order relative to each
other, and the script order a replay runs is recorded live per host (RO1).
**Already queued: HU1 inventory (d)** (docs/roadmap.md §4 item 1), Stage C
(R6 / R7).

**D6. The trafo set hooks: two implementations (by reading equivalent).**
`has_body` through `GetPhysicsObject` (sim/trafo_hooks.hpp:27) against the
bridge's `has_body` (CH:1076); `restore_originals` set on PW/SC and run by
`quiesce_originals` (trafo_sets.hpp:508), empty on CH, whose
`reset_session_clock` right after its quiesce does the restore (CH:1801).
**Already queued: HU1 inventory (a).**

**D7. A crossing's stamp by the retail checkpoint hook is one tick earlier
than by the poll (only when an anchor edit failed).** CH:3791 calls
`request_sector` from the checkpoint hook inside frame T, which stamps
`current_tick() + 1 + lead` = T + lead (client_core_frame.cpp:188; inside
the frame `current_tick()` is T - 1, CH:3633); the poll after step T stamps
T + 1 + lead (client_core_frame.cpp:466). With B2SX's split applied the
chain writes the private cell, and `update_current_sector`
(BallanceMMOClient.h:990) reads [0][1], which the chain no longer writes,
so the crossed sector reaches the core through the poll - the same tick as
SC (whatever the hook passes on is a sector a union pass wrote, already in
the union). The hook path stamps first only when the split failed (logged ERROR at
the anchor, CH:1845); outside the hold the hook reports at T - 1 where SC
reports at T. Every world applies the stamp it is sent, so no two worlds
differ; the sector starts one tick earlier relative to the crossing.
**Candidate: none needed (cleanup at R7**, when the legacy hook path goes).

**D8. The server kicks a spawn by `p.slot`, the clients by join order.**
PW:955 `spawn_direction_index(seed, p.slot, tick)`; SC:3207 and CH:3758
`own_join_order()`; the mirror's kick index comes from the core's join
order. Equal unless `add_player` fell back to another slot (PW:763-777,
logged as ERROR: an M1 invariant violation, "a SimTool replay of an old
journal can still meet it"). **Candidate: none** (the invariant is
the fix; a PW one-liner to use the join order would make the fallback's
kick agree too).

**D9. `hooks.ended`: the trafo teardown before or after the body guard is
lifted.** SC:2105-2106 guard off, then `end_trafo_sets`; CH:3314-3316
`trafo_teardown`, then guard off. After the journal closed; no world of the
session sees it. If the guard (#6) refused a bridge unphysicalize, a retail
teardown would leave piece bodies in the level after the session (check: a
retail T4 log's "torn down (N piece bodies removed)" against the sets'
live pieces). **Candidate: none** (fold into AUD-3's host-support pass).

**D10. The mirror entity: shared clones on PW/SC, spirit balls on CH.**
PW and SC call `mirror_clones` (one CK copy per player and type, made at
the first life, `<ball>_BMMO_<player>`; PW destroys a leaver's, SC keeps
it); CH uses game_objects' spirit balls (`<ball>_Peer_<n>`, made per player
at room join) and only checks the contract (CH:779, counted, the session
goes on). No world effect found: the body is made anew at every life in
every world, collision groups are by join order, and the journals compare
mirrors by (kind, owner). Known since BMX (world-agreement-gaps-design.md
"As built (step BMX)").

**D11. Only SC watches the resetpoint in `poll_sector`.** SC:2862 sets
`resetpoint_moved`, which makes the core re-read the cell even when it did
not change (client_core_frame.cpp:461); the core then stamps only a sector
not requested before (`sectors_requested_`, client_core_frame.cpp:163).
None, by reading.

**D12. `discover_navigation`: retail drops unbound leaves.** CH:2416-2421
erases leaves with key 0 and renumbers (another mod's grafted leaves, the
"mod-injected unbound leaf" bug); SC:2009 and PW:1041 wait until every leaf
is keyed. With the mod alone the graphs are the same; with a grafted leaf
SC and PW never meet one (no mods load there). None, by reading.

**D13. Test switches and diagnostics that only SC has**: `--stall-after-ready`,
`--trafo-no-resets`, `--hold-event`, `--force-rollback`, `--park-births`,
`--insert-births`, `--restore-clock`, `--savestate-strict`, `--replay-scripts`,
`--no-script-replay`, `--no-trafo-cap`, `--no-rollback`, the dumps and
traces, `hull_probe`; CH's verbs `session drop-birth`, `session
force-rollback`, `beam`. Equal at their defaults (§6, §7). None.

**D14. `make_own` for an entity outside the ball table**: CH:2872 attaches
no revive navigation; SC:2949 attaches one with force 0. The own entity is
always a table ball. None, by reading.

## 10. What this audit did not do

- No check was run (a docs-only step: no build, no test, no replay; the
  FULL plan's AUD row is "offline, docs only").
- D2-D4 are by reading and were never observed; D3 needs a diagnostic line
  before a `--reset-level-at` run can test it (above); D2 and D4 need a run
  built for them.
- The IVP sources and physics_RT are out of scope: every host reaches the
  same bridge functions (`physics_direct` in process, `physics_view` through
  the plugin), so a difference below the bridge is not a host difference.
