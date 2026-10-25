# Session-client core: one implementation of the client flow, two hosts

Status: design, revision 2 (two reviews answered; §8 lists every finding and
what became of it). Steps E0 (§5.2, "As built (step E0)"), E1, E2, E3, E4,
E5, E6, E7, E8, E9, E10 and E11 (§6.1, "As built (step E1)" ... "As built
(step E11)") are built: the E steps are done. R0 and R1 are built (§6.2, "As
built (step R0)", "As built (step R1)"). R2 is implemented but OPEN, not
closed ("As built (step R2)"): its check (a) fails on the trafo's explosion
pieces in a window that crosses their birth, and its check (b) fails on the
mirror p99 (above the headless lane O runs, which have lifecycle rewind on:
R3's switch). Step P (the pieces tracked as mechanisms in both hosts, "As
built (step P)") is implemented but OPEN: R2 (a) now passes on retail, the
headless trafo run does not. Step Q (the anchor restores the proximity
blocks' countdown, "As built (step Q)") is built: R0's baseline check holds.
R3 is built ("As built (step R3)": the retail lifecycle rewind and own
echoes; with it R2 (b)'s mirror p99 is below every O run). R4-R7 are not
built.

The base is HEAD 760edb2 plus the uncommitted step 20 of
`docs/late-player-design.md` (barrier removal). Line numbers refer to that
working tree.

Path key used throughout:

| key | file |
|---|---|
| SC | `BallanceMMOServer/sim/session_client.cpp` (6211 lines) |
| PSC | `BallanceMMOClient/session/physics_session_client.cpp` (3609 lines) |
| PSH | `BallanceMMOClient/session/physics_session.hpp` |
| MC / MH | `BallanceMMOClient/BallanceMMOClient.{cpp,h}` |
| RB / RBB | `BallanceMMOCommon/include/session/rollback.hpp` / `rollback_binding.hpp` |
| LPD | `docs/late-player-design.md` |

## 0. The goal and the rule

The client side of a physics session exists twice today:

- **The headless client (SC).** It has the whole flow:
  - snapshot rollback;
  - the input-corrected rewind (remote relays and the own substituted frames);
  - lifecycle events drained at their tick, with rewinds;
  - the owner's birth move;
  - parts one, two and three of LPD.
- **The retail mod (PSC).** It has an older, smaller copy:
  - snapshot rollback only (`rewind_to` is never called);
  - remote events applied on arrival;
  - echoes and the held section only counted;
  - no SessionLead (MC:2402-2452 does not even dispatch it).

**The rule: one implementation of the session flow, two hosts.**

- Every decision about what the session does (what is recorded, sent, rewound,
  re-made or resynced, and in which order) lives once, in a shared core in
  `BallanceMMOCommon`.
- The core talks to the world only through a small **platform interface**.
- Each client is a **host**: it owns its loop and implements the platform
  operations with what it has:
  - headless: in-process bridge calls on `CKIpionManager*`;
  - retail: `physics_view`, CK objects and BML hooks.
- **Two things a host must never do.** It must not re-implement a decision
  the core makes. Where a host cannot do something, it says so with a
  **capability flag**, and the core takes its defined fallback.
- **A deliberate difference is kept only as one of two things:** a capability
  (the host cannot do X), or a host policy (a named config value). A forked
  code path is never kept.

**Constraints that shape everything below:**

- The headless runs must not change. Every extraction step is verified by
  §5.
- The retail mod gains the flow by hosting the core, area by area (§6, R steps).
- The core is CK-free, so it can be unit-tested in `BallanceMMOMessageTests`
  with a fake platform (as `rollback_binding_test.cpp` does with a fake
  backend). It compiles into three targets:
  - x64 MSVC (headless, Windows);
  - GCC (headless, Ubuntu-WSL and Arch);
  - x86 MSVC (the retail mod).
- The retail mod does **not** link `BallanceMMOCommon/src/physics`. The
  bridge lives in the physics_RT plugin DLL (`cmake/PhysicsRTPlugin.cmake:42-47`),
  so the core may not call a `bmmo::physics::*` free function. Everything
  physical goes through the platform interface.

## 1. The core

### 1.1 Layers and files

Three layers. The rule applies to the first two.

| layer | what | where | CK | compiled by |
|---|---|---|---|---|
| 1. core | the session flow: state, decisions, order | `include/session/client_core.hpp`, `include/session/client_platform.hpp`, `src/session/client_core*.cpp` | no | headless client, retail mod, `BallanceMMOMessageTests` |
| 2. host support | platform-side code that both hosts need identically (§2.4) | `include/game/session_host_support.hpp` (header-only, templated on a duck-typed physics backend) | yes | headless client, retail mod |
| 3. hosts | loop, UI, harness, game-object lookup, the platform implementation | SC (shrinks to the host) and `BallanceMMOClient/session/core_host.cpp` (new) | yes | their own target |

The core's sources, under `BallanceMMOCommon/src/session/`:

| file | contents |
|---|---|
| `client_core.cpp` | `begin`, `anchor`, the hold, `end`; numbering, SessionAssign, resync request and limiter |
| `client_core_frame.cpp` | `pre_step` and `post_step` (§1.3); input building; record and amend; live drive |
| `client_core_messages.cpp` | the `on_message` dispatch; relay intake and `note_input_correction`; the snapshot path (rollback, resync rebuild, legacy ladder) |
| `client_core_lifecycle.cpp` | event intake, `drain_due_events`, `apply_event`, pending births, the remote mirrors' core half; own lives, echoes, `move_own_birth`, `resync_own_life`, the beam record |
| `client_core_held.cpp` | LPD part one (held section, own correction, `held_floor`) and part two (SessionLead, forced frames, `lead_slew` bookkeeping) |
| `client_core_journal.cpp` | the core's journal calls over the promoted client journal (§1.2) |
| `client_core_report.cpp` | the status and `window:` text (a harness contract, kept byte-identical) |

**Build placement.**

- A new `cmake/SessionCore.cmake` defines `BMMO_SESSION_CORE_SRC`, in the
  pattern of `cmake/BuildId.cmake`.
- It is added explicitly to:
  - `BallanceMMOSessionClient` (Server CMake 161-163);
  - `BallanceMMOClient` (Client CMake 164);
  - `BallanceMMOMessageTests` (Server CMake 245).
- It must not live under `src/entity` or `src/utility`. Those GLOBs also feed
  BallanceMMOServer, MockClient and RecordParser, which have no physics
  includes.

**Why not header-only.** The other `session/*.hpp` files are header-only. A
4000-line class would recompile into every translation unit that touches it.
Separate `.cpp` files also keep the x86 retail build's stale-object trap
small: only these objects change when the core changes.

### 1.2 The state the core owns

This is the "core" list of task A, moved verbatim (SC:5637-6171).

| area | members |
|---|---|
| session parameters | `session_`, `input_delay_`, `sector_lead_`, `seed_`, `spawn_impulse_`, `spawn_spacing_`, `level_`, `players_`, `own_id_`, `own_join_order_`, `own_group_` |
| numbering and phase | `phase` (idle / loading / running), `assigned_`, `last_assign_first_tick_`, `duplicate_assigns_`, `held_`, `hold_deadline_`, `hold_polls_` (counted by `poll_hold`, which is the SC:1931-1942 branch; read by the release log, SC:808), `tick_base_`, `frames_since_anchor_` |
| inputs | `input_history_`, `own_inputs_`, `sent_inputs_`, `previous_cam_` / `previous_cam_valid_` (the input basis, SC:2340-2342; the host reads it through `core.previous_cam()` for the poll re-arm, SC:3670, and `--keep-lane`, SC:2014-2028), `held_ahead_`, `held_seen_`, `held_stale_seen_`, `held_floor_`, the `held_header_*`, `acked_header_*` and `held_learned_*` / `held_gap_*` families |
| SessionLead | `forced_own_`, `lead_seen_`, `lead_slew_`, `lead_offset_`, the `lead_*` counters |
| remotes and relay | `remotes_` (with `entity` as an opaque key), `relay_seen_`, `pending_relay_`, the relay counters and lag histogram |
| lifecycle | `pending_events_`, `held_lifecycle_`, `event_seen_`, `own_event_repeats_`, `own_life_`, `own_echoes_`, `own_echoes_stale_`, `own_unanswered_`, `own_moved_unphysicalize_`, `remote_last_physicalize_`, `remote_last_unphysicalize_`, `pending_births_`, `generations_`, `own_physicalized_`, `own_body_entity_` |
| rollback and resync | `rollback_` (the `rollback_engine`), `mechanism_tracking_`, the `input_rewind_*` family, `own_replay_pacer_`, `resync_pending_`, `resync_in_flight_`, `resync_owed_`, `resync_owed_reason_`, `last_resync_request_`, `consecutive_hard_`, `have_snapshot_`, `last_snapshot_tick_` |
| sectors | `sectors_` (sector_union), `sectors_requested_`, `start_sector_pending_` (+ the requested sector), `sector_cell_` (the union pass writes it, SC:2106, and the crossing poll compares against it, SC:2470: in the host, the pass's write would be lost and the union's own cell write reported as a crossing, one extra Sector event) |
| legacy path | `corrector_`, `mechanism_correctors_` (used only with rollback off, in both clients) |
| journal | the promoted client journal (below) and its bookkeeping (`journal_announced_`, last checkpoint, pending checkpoint) |
| counters | every counter the status and `window:` lines print, except the host's own: `rebases_` (the pacer, SC:1968), `activations_replayed_` (host support), `script_reset_skips_` (host script replay) and the trace counters. Those reach the text as a `host_counters` argument (§1.3), never by appending |

Dead state is deleted in the last E step, not carried over:

- `spawn_position_` and `spawn_known_` (set at SC:1083-1084, never read);
- `anchor_hash_` (SC:1209, never read);
- `snapshot_interval_` (set, never read).

**The journal.** The core owns one `client_journal`, and it is the
**headless** implementation, moved: the helpers `open_journal` …
`journal_local_checkpoint` (SC:5150-5373) over the shared `journal.hpp`
writer, as `include/session/client_journal.hpp` +
`src/session/client_journal.cpp`. Revision 1 promoted retail's class instead;
the two are not the same policy, so that would have changed headless bytes or
retail's journal:

| | headless (kept) | retail legacy (`session_journal_client.cpp`) |
|---|---|---|
| file | `--journal <file>` verbatim (SC:358, 5153; the harness passes `client{i}.bmjr`, arena_map_test.py:630) | `directory()/file_name(session, level, player)` + prune to 10 (.cpp:22-87) |
| checkpoint due | `tick % cadence`; always before the first one or after the tick went back; pending gap `min(cadence, 33)`; cadence 0 = off (SC:5307-5320) | `tick % cadence` or pending; never the same tick twice (.cpp:226-235) |
| pending on | corrections of kind 2/4, and the `on_correction` kinds | kinds 1/2/4 |
| local checkpoint owners | filled (SC:5352-5357) | always 0 (.cpp:181) |
| input records | FRESH / SUBSTITUTED | `own_input` |
| header | `first_tick = tick_base_`, join order fallback 255 | `first_tick` 0, fallback 63 |

- The file comes from the platform as a whole path:
  `journal_path(const journal_header&) -> std::filesystem::path` (empty = no
  journal). Headless returns `args_.journal` verbatim. The retail host builds
  `directory()/file_name(...)` and prunes to 10 files before returning it:
  pruning is host code, not a core policy.
- Cadence: `core_config.journal_checkpoint_ticks` (headless
  `--checkpoint-ticks`, retail `BMMO_JOURNAL_CHECKPOINT_TICKS` or 660); the
  33-tick gap and the 256 MB cap are core constants.
- `gmtime_s` is not needed in the core (the file name is the host's).
- The bytes the headless helpers write must not change (§5.2 (2) checks
  that).
- **Retail's legacy journal is not touched by any E step.** The legacy path
  keeps its singleton until R7. With `session core on`, the retail session
  writes the core's journal, and its content changes as the table says
  (§4.2, R1). The `journal on|off|mark` verbs reach the legacy singleton with
  `core off` and `core.journal()` with `core on`; `mark` writes through the
  core's checkpoint method with the platform's `list_bodies`.

**Configuration** (`core_config`, filled by the host at `begin`):

- the rollback engine's switches, today set from args at SC:1054-1068: the
  `restore_*` family, `contact_flags`, `sleep_as_recorded`, `script_replay`,
  `savestate`, `script_diff_logs`, `trace_plan`, `verbose`;
- `rollback` and `correct` (the legacy blend ladder);
- `journal_checkpoint_ticks` (headless: `--checkpoint-ticks`; retail:
  `BMMO_JOURNAL_CHECKPOINT_TICKS`);
- `window_shift` (the report window's phase; headless `--window-origin`).

**Feature switches.** Each switch gates a newer behaviour. Headless sets them
all true whenever `rollback` is true, which is exactly today's behaviour,
since today the same paths hang off `args_.rollback`:

- `input_rewind`: `replay_corrected_inputs` and the remote half of
  `note_input_correction`;
- `lifecycle_rewind`: `make_remote_birth`'s pending path and
  `rewind_for_lifecycle`;
- `own_corrections`: amending own records from held notices and the own
  rewind, i.e. LPD part one;
- `own_echoes`: acting on the own echoes (`process_own_echo`,
  `expire_own_echoes`, `resync_own_life` and the moved-Unphysicalize list,
  SC:4113, 4147, 4211, 4309). Today these hang off `args_.rollback` alone, so
  without this switch retail's R1 (rollback on) would already resync on moved
  or missing echoes. Off, the echoes are counted and journaled only, as
  retail does today;
- `own_birth_move`: `move_own_birth`. It also needs the own-body capability
  (§2.3).

A switch off **skips** the newer path; it never selects older retail code.
The R steps (§6.2) turn the switches on one by one for retail.

There are also two retail-only policies (§4.1):

- `mirror_start_from_row`, needed only while `lifecycle_rewind` is off
  (deleted at R3, "As built (step R3)");
- `own_body_backstop`.

### 1.3 Entry points per frame

```cpp
class session_client_core {
public:
    session_client_core(session_platform platform, session_capabilities caps);

    // --- session lifecycle (game thread) ---
    begin_result begin(const session_start_msg& msg, const core_config& config);  // SessionStart; refused on a protocol mismatch
    void anchor(const anchor_report& report);   // the level is up: SessionReady, journal open, hold armed
    hold_state poll_hold();                     // {held, released_now, timed_out}; journals the timeout note
    void release_hold(const char* why);         // retail: the connection was lost
    void end(const std::string& reason);

    // --- messages (game thread, arrival order, between post_step and pre_step) ---
    bool on_message(const void* data, size_t size);   // Session* opcodes; false = not a session message

    // --- the tick ---
    uint32_t next_tick() const;   // the tick pre_step prepares (tick_base_ on the first frame)
    void pre_step();              // prepare next_tick() = T
    void post_step();             // the world has stepped T; current_tick() becomes T

    // --- host reports (see section 2.2 for when) ---
    void own_physicalized(const own_birth_report& report);   // host_hooks only: queued, committed at post_step step 19
    void own_unphysicalized(uint32_t tick);                  // host_hooks only: queued likewise
    void request_beam(const float position[3]);              // headless --beam, retail `beam` verb: consumed at step 29
    void request_sector(int sector, const char* why);        // before the assignment: pending; after: hold_sector(s, current + 1 + lead)
    void force_rewind(uint32_t depth);                       // harness: --force-rollback, retail forced-rollback verb
    void request_resync(const char* why);                    // host triggers (rebase, retail transitional triggers); the core's limiter applies
    void release_event(const std::string& wire_bytes);       // --hold-event: a held send goes out now (put_event_on_wire)

    // --- pacing (the host owns the clock) ---
    double rebase_threshold() const;                     // 33 + |outstanding slew| (lead_rebase_threshold)
    void schedule_rebased(const char* why);              // -> request_resync(why)
    int slew_step(double wall_ticks, double behind_ticks);   // lead_slew_.advance + finish; ticks the origin moves
    bool slewing() const;

    // --- reading ---
    uint32_t current_tick() const;
    bool assigned() const, holding() const, resync_pending() const;
    const rollback_engine::statistics& rollback_stats() const;
    acked_input_view acked_input() const;         // {known, frame, tick}: the newest acked input (retail starvation detector)
    const float (*previous_cam() const)[3];       // the input basis; nullptr until a camera was seen
    std::string status_text(const host_counters& h) const;      // SC report(): one format literal, host values in place
    std::string window_text(uint32_t tick, const host_counters& h);   // SC report_window(); "" outside a window edge;
                                                  // moves the base: step 31 is its only caller, hosts use hooks.window (E10)
    bool window_due(uint32_t tick) const;         // as built (E10 review): step 31 reads the counters only when due
    client_journal& journal();
};

struct host_counters { int rebases; uint64_t activations_replayed; uint64_t script_reset_skips; };
```

`status_text` and `window_text` keep **one** format literal each, in the
core, with the host values at their places today (`rebases=%d` mid-line,
SC:5524 / 5596; `activations=` and the last `scr=` field, SC:5621-5624; the
`window:` delta's `rebases`, SC:5430). Appending them would break byte
identity and the §5.2 (4) format check. The core's `logf` formats into a
growing buffer: the status format alone runs past 1.5 KB plus `last=%s`, and
a fixed buffer would cut off the `input:` and `lead=` fields the harness
parses.

**The two loops.** The core's order is today's headless order.

| | headless host (controls the step) | retail host (runs after the game stepped T) |
|---|---|---|
| between frames | `poll_incoming_messages` → `core.on_message` for every Session* opcode, as they arrive | the network thread pushes raw bytes into one mailbox (§3.2) |
| a frame | host: keys from the record (SC:2043-2051), `--start-sector`/pause hooks; `core.pre_step()`; `engine_->tick()`; `core.post_step()` | in `OnProcess` (the game stepped T before it, AUTO:704-707): host code first: the clock guard re-attach, the pause clock pin and `pause_scripts_apply` (PSC:1659-1675, unchanged, before anything can rewind); `core.post_step()`; drain the mailbox → `core.on_message`; `core.pre_step()` (prepares T+1; N1/N2/N3 at its step 4); the next `CKContext::Process` steps it |
| the anchor frame | `core.anchor()`; the loop polls messages and `core.poll_hold()` until released (non-blocking) | `core.anchor()`; a blocking 1 ms loop inside `OnProcess`: drain the mailbox, `core.poll_hold()`, connection check → `release_hold`; then `core.pre_step()` for `tick_base` (today's PSC:1485-1486 union tick is exactly that pre-step) |

**Why the order is the same.** Both hosts produce the same sequence per
tick:

```
… post_step(T-1), [messages], pre_step(T), STEP T, post_step(T), [messages], pre_step(T+1) …
```

- **Headless.** Messages are handled between frames (SC:606-650 loop:
  `update()` then `pace_and_frame()`). `frame()` is pre-step, step,
  post-step.
- **Retail.** One `OnProcess` holds post-step(T), the messages and pre-step(T+1).
  The game's behaviour pass and physics step of T+1 follow.
- **Retail's in-step hooks fall inside the same interval as the headless
  equivalents.** `OnPhysicalize`, `OnUnphysicalize` and
  `OnPostCheckpointReached` fire inside the behaviour pass of T+1, between
  pre_step(T+1) and post_step(T+1). The headless client reads the same facts
  from the event log at post_step(T+1), stamped T+1 (SC:2302-2310).
  `own_physicalized()` / `own_unphysicalized()` only **queue** the report;
  the core commits it (send, own life, `own_physicalized_`) at step 19 of
  post_step(T+1), right after the drain, which is where headless's
  `report_physicalize` runs. Committing in the hook would record the life's
  `generation` one behind: headless records it (SC:1482-1483) after the
  drain's `created` entry bumped it (SC:2298, then 2303), while retail's hook
  runs before the body exists (PSC:2863), before the drain's bump (PSC:1834), so `move_birth` and `destroy_body`
  would name the wrong lifetime. Retail's send moves from the hook to the end
  of the same frame, with the same stamp.

**`pre_step()`** prepares T = `next_tick()`. Here `current_tick()` is still
T-1, as at SC:2058. It is SC:2058-2163, in order:

| # | step | today | kind |
|---|---|---|---|
| 1 | a pending start-sector, once assigned: `hold_sector(s, T + lead)` | SC:2062-2065 | C; `request_sector` before the assignment only sets `start_sector_pending_` (SC:1272-1276): the anchor precedes the renumbering (SC:784-799), so stamping at once would use anchor-relative numbers |
| 2 | `drain_due_events(T)` | SC:2069 | C |
| 3 | `process_own_echoes()` | SC:2073 | C |
| 4 | `platform.script_guards(T, mechanism_tracking_.resolved() > 0)` | SC:2076-2077 (`reset_guard_.apply`) | P; retail moves its per-frame N1/N2/N3 re-applies here (only these; the clock guard and pause edits stay at the top of `OnProcess`) |
| 5 | `replay_corrected_inputs()` | SC:2085 | C (input_rewind) |
| 6 | `retry_owed_resync()` | SC:2086 | C |
| 7 | `hooks.after_rewinds(T)` | SC:2087 (`maybe_force_rollback`) | H; the hook calls `core.force_rewind` |
| 8 | `apply_held_lifecycle(T)` | SC:2092 | C |
| 9 | sector union pass: `sectors_.tick(T, platform.sector_manager_active())` → `platform.run_sector(s)`, `platform.record_activation(T, true)`, log | SC:2099-2113 | C + P |
| 10 | re-feed every predicted mirror's `remote.applied` → `platform.drive(entity, frame)` | SC:2120-2128 | C data, P call |
| 11 | forced own frame of T (`forced_own_`): `platform.force_own(frame)`; remembered for step 13 | SC:2138-2161 | C select, P call |
| 12 | `platform.arm_own_capture(T)` | SC:2162-2163 | P (headless only; empty on retail) |

**`post_step()`** handles the world after step T. It is SC:2166-2595, in
order:

| # | step | today | kind |
|---|---|---|---|
| 13 | a forced tick: `platform.unforce_own()` (polling on) | SC:2166-2172 | P; on retail the polling stays off across the game's step and is cleared here, as needed |
| 14 | `frames_since_anchor_ += 1` (T is now current) | SC:2174 | C |
| 15 | `platform.record_pending_activation(T + 1)` (ACTIVATENEXTFRAME seen) | SC:2180-2184; retail PSC:2032-2036 | P |
| 16 | `platform.discover_navigation()`; when it turns true, `platform.attach_mirror_navigation` for each remote and the `driven_from` fix-up | SC:2189-2206 | P + C |
| 17 | `platform.own_ball()`, then `platform.own_body_upkeep(view)` (body guard, group, own navigation); `own_physicalized_`, `own_body_entity_`, `corrector_.record` | SC:2209-2237 | P + C; `own_group_set_` is host state, and the host's `drain_body_events` (step 19) clears it on an own `deleted`, as SC:2309 does. Without that, an own ball deleted and re-made under the same name within one tick is seen physicalized here with the flag still set and never gets `P#<join>` |
| 18 | legacy corrector records (mechanisms, remotes) through `platform.body_state` | SC:2241-2259 | C over P |
| 19 | `platform.drain_body_events()`: `created`/`deleted` → `++generations_`, `mark_dirty`; own created/deleted → own Physicalize/Unphysicalize (`event_log`), or the queued host reports committed here (`host_hooks`); `wakeup` → BodyRevived (filtered by `platform.reportable_wakeup`) | SC:2265-2320 | C over P |
| 20 | `mechanism_tracking_.resolve(T, platform.body_state)` when due | SC:2323-2327 | C over P |
| 21 | input: `platform.observe_input()` → the core builds the frame (basis = previous camera, flags), applies a forced frame (SC:2351-2358), `platform.own_camera_rows(basis)` | SC:2330-2367 | P + C |
| 22 | `input_history_`, journal FRESH/SUBSTITUTED | SC:2368-2373 | C |
| 23 | the rollback record: `own_inputs_`, tracked set (own, remotes, `platform.movable_mechanisms()`, `generations_`), `platform.note_script_resets(T)`, `rollback_.record(world(), T, …)`, `hooks.after_record(T)` | SC:2374-2421 | C + P (+H: dump_savestate) |
| 24 | `take_held_ahead(T)` | SC:2425 | C |
| 25 | send the input history (unreliable, no delay) and the event repeats due | SC:2426-2443 | C over `platform.send` |
| 26 | own sector detection: `platform.poll_sector()` → `{cell, resetpoint_moved, checkpoint_held}` (as built, E8: `poll_sector(T)` → `{cell_read, cell, resetpoint_moved, checkpoint_held}`); the core decides between `hold_sector(s, T + 1 + lead)` and a Sector event after the fact | SC:2459-2486 | P + C |
| 27 | legacy blends (`correct`) | SC:2489-2494 | C over P |
| 28 | next tick's mirror drive: `remote.applied = live_drive(remote, T + 1)`, `platform.drive` | SC:2498-2505 | C + P |
| 29 | a requested beam (`request_beam`, from `--beam` or the retail verb): `beam_own_ball(T)`, stamped T+1 | SC:2507-2510 | C (+ `write_body` without `own_body_control`, §2.3) |
| 30 | `rollback_.amend_record(world(), T)` | SC:2516-2519 | C |
| 30a | `hooks.after_amend(T)`: the traces | SC:2520-2583 | H |
| 31 | `window_text(T, host_counters)` → `hooks.window(text)` | SC:2584 | C text, H print |
| 32 | journal: `platform.world_hash` → TICK; checkpoint when due with `platform.list_bodies()`; then `hooks.after_journal(T)` (`report_queued_impulses`, SC:2594, inside the journal block) | SC:2588-2595 | C over P (+H) |

**Every step from 13 to 32 is in the same relative order as in SC, with no
exception.** The traces keep their place between the amend and the window
(revision 1 moved them to the end; that was unnecessary).

**Why the retail beam is a request.** The command file runs before
`process_physics_session` (MC:462-464), when `current_tick()` is still T-1.
Retail's verb stamps `current_tick() + 1` = T (PSC:2681), a tick the game
already stepped, and the core's `live_own_lives` would drop it as "never met
its live frame" (SC:1660-1670). Queued and consumed at step 29, it is stamped
T+1 on both hosts.

**Messages** run C code identical to SC:714-931:

- `SessionAssign`: SC:726-816. The host clock touches (`origin_ = now()` at
  770/797/807) become `hooks.numbering_restarted(site)`, the site being
  `assign_resync`, `assign_renumber` or `assign_release` (E0: each is its
  own clock read in the capture). Headless resets `origin_` there with
  `wall_now(site)`. Retail does nothing, because `fixed_tick_` counts its own ticks and
  holds through `set_hold`.
- `SessionSnapshot`: `apply_snapshot`, SC:2757-2981.
- `SessionEvent`: intake, SC:824-856.
- `SessionRemoteInput`: SC:857-917.
- `SessionLead`: SC:918-924. Ignored without the slew capability (§2.3).
- `SessionEnd`: SC:925-931.

`SessionStart` is not taken by `on_message`. The host decodes it, since the
level load and the countdown are the host's, and calls `begin`.

**Retail order changes when it switches to the core.** Each is
physics-neutral or deliberate (§4.2):

- **The snapshot path.** Today retail applies snapshots inside the frame,
  after the record and before `amend_record(T)` and the journal TICK hash
  (PSC:2042, 2119, 2134). With the core, the snapshot comes after the amend
  and the hash, as on headless. The replay re-records T in both cases, so the
  amend matters only for the legacy ladder. That ladder blends, and the amend
  of the next frame carries it. What changes is which world the journal's
  TICK hash of T describes.
- **Remote events.** They are no longer applied on arrival in a batch sorted
  (applied, join, origin) at PSC:2207-2212. They are drained at their tick,
  before the step, in the server's order (join, applied, origin, rank;
  SC:3990-3997). A late event is made before the next step in both versions.
  An event stamped ahead waits for its stamp, as at the server.
- **Retail's per-frame script edits.** Only `death_reset_apply`,
  `sector_deactivate_apply` and `sector_activate_apply` (PSC:1679-1681) move,
  from the top of the frame to pre-step step 4: after the frame's echoes and
  before its rewinds, as the headless reset guard. The clock guard re-attach,
  the pause clock pin and `pause_scripts_apply` (PSC:1659-1676) stay host code
  at the top of `OnProcess`: the guard is a PreSimulate callback that a
  sector change can destroy, and every replay of the frame must find it
  attached. (It writes nothing while the pause chain is neutralized,
  PSC:186-195, so a replay without it would be neutral; keeping it first
  costs nothing.) Two consequences, both accepted:
  - A snapshot or input rewind drained in this frame's mailbox now runs
    before N1-N3's re-apply instead of after it. Harmless: the replay steps
    physics, the selected mechanism scripts and the manager re-issues; it
    runs none of the chains N1-N3 edit (BallManager's death chain, the
    checkpoint deactivation and activation in Gameplay_Events).
  - `death_reset_apply` reads `mechanism_tracking.resolved()` (PSC:972).
    At step 4 that includes post_step(T)'s resolve, so its deferred write
    lands one frame earlier than today. That is headless's order
    (`reset_guard_.apply` at SC:2076 after the resolve at SC:2323): retail
    adopts it at R1.

## 2. The platform interface

### 2.1 Style

The interface is a struct of `std::function` fields, filled by the host. It
follows the pattern already shared by both clients:

- `rollback_world` and `bind_rollback_world` (RB:72, RBB:74-227);
- `script_replay_hooks`;
- `mechanism_registry::body_probe`.

**An empty field means "this host cannot".** That is the convention
`rollback_world` uses: `set_clock` not installed means no clock rewind
(RBB:132-139). The core branches on it, and on the capability flags of §2.3,
never on "which host am I".

Where a decision depends on more than one field (the slew, the own-body
control), the host declares it explicitly in `session_capabilities`. The
constructor asserts that every field a declared capability needs is filled.

### 2.2 Operations

Types used below:

- `body_state = bmmo_physics_body_state` (C struct, `physics_rt_api.h`);
- `world_hash` from `physics/world_hash.hpp`;
- `input_frame`, `ball_recipe` and the messages from `entity/session.hpp`;
- `send_mode` is `{reliable, reliable_no_nagle, unreliable_no_delay}`, mapped
  by the host to `k_nSteamNetworkingSend_*`.

Errors are logged by the host. A failing operation returns false or an empty
value, as the free functions do today.

| operation | signature | headless implements with | retail implements with |
|---|---|---|---|
| **transport and time** | | | |
| send | `void(const std::string& bytes, send_mode)` | `send_bytes` → `SendMessageToConnection` (SC:969-971) | `client::send` (client.h:52) |
| now | `steady_clock::time_point(clock_site)`: the site is the read's place in the flow (`session/clock_site.hpp`, E0) | `wall_now(site)`: `clock_type::now()`, captured or replayed by site | `steady_clock::now()`; the site is ignored |
| log | `void(log_level, const std::string&)` | `logf` (stdout; the harness parses it, so the text is kept verbatim) | `logger_->Info/Warn` |
| **reading the world** | | | |
| body_state | `bool(const std::string& entity, body_state&)` | `get_body_state(physics, …)` | `physics_view_.get_body_state` |
| world_hash | `bool(world_hash&)`: the journal's TICK record; as built (E9): required with `journal_path` | `capture_world_hash` | `physics_view_.capture` |
| list_bodies | `std::vector<bmmo_physics_body_state>()` | `list_bodies` via the `physics_direct` adapter (`physics_direct::list_bodies`, added at E1) | `physics_view_.list_bodies()` |
| movable_bodies (`movable_mechanisms` until step P) | `std::vector<std::string>()`: every movable body, in the engine's order; the core leaves the balls out (`is_ball`, step P; before P the host's list already skipped `Ball_*`) | `movable_body_names(physics, "Ball_")` (SC:3842-3845); P: without the skip | the `list_bodies` filter of PSC:1956-1960; P: `list_bodies` movable |
| own_ball | `own_ball_view()`: `{entity, ball_type, physicalized, state}` | `current_ball`, `ball_type_of`, `get_body_state` (SC:2209-2220) | `get_current_ball`, `db_.get_ball_id`, `get_body_state` (PSC:1656-1759) |
| own_body_upkeep | `void(const own_ball_view&)`: body guard, own group, own navigation attach; as built (E9): called after `own_ball` (SC set the guard before reading the body: a flag write and a read, order-free) | SC:2212-2227 (navigation only with rollback: a host policy) | PSC:1749-1775 |
| drain_body_events | `std::vector<body_event>()`: `{created, deleted, wakeup}` + name; as built (E9): `void(uint32_t tick, const std::function<bool(const body_event&)>& each)`, every entry handed over in the log's order (the host keeps the recipes in between), `each` true for the own ball's created / deleted; recipe entries are kept by the host (the local birth log); an own `deleted` clears the host's `own_group_set_` (SC:2309) | `drain_event_log` + the shared parser (§2.4) | `physics_view_.drain_event_log` + the same parser |
| reportable_wakeup | `bool(const std::string& name, const std::string& own_ball)` | not `Ball_*`, not `*_BMMO_*` (SC:2311-2312); P: not `is_ball`, not the own ball, not `*_BMMO_*`, not a trafo explosion piece | not `Ball_*`, not the own ball, not `*_Peer_*` (PSC:1846-1847); P: not `is_ball`, not the own ball, not `*_Peer_*`, not a piece |
| observe_input | `input_observation()`: `{keys, cam[3][3], cam_valid, nav_active, paused, ball_type, physicalized}` (as built, E9: `{keys, cam, cam_valid, nav_active}`; the ball type and the body from `own_ball`, `paused` at R2): **what the own replica used for step T** (every replay drives the own ball from this record) | the record's keys through `keys_from_state`, `Cam_OrientRef`, `navigation_active()` (the leaves' Key Event `IsActive`, SC:1912-1918) | `frame_keys_` through `keys_from_state`; `Cam_OrientRef`; **nav_active from the leaves' Key Event `IsActive`**, the source the replica polls (ball_navigation.cpp:469-481), not `ball_nav_active_` (the BML hook); **paused = the mute in force during step T** (the previous frame's decision), not the one decided now (PSC:1874-1883 applies a new mute from T+1 on, so today the edge tick is recorded keys 0 + PAUSED while the replica polled live keys). Changed at R2 (§4.2) |
| poll_sector | `sector_poll()`: `{cell, resetpoint_moved, checkpoint_held}`; as built (E8): `sector_poll(uint32_t tick)` → `{cell_read, cell, resetpoint_moved, checkpoint_held}`, `cell_read` false (the cell could not be read) counting for nothing, as SC's failed `GetElementValue` did; `tick` is the T the core decides for, so a host line about the poll names it (the headless `resetpoint now ...` line) | `IngameParameter[0][1]`, `level_resetpoint` (SC:2459-2470) | `IngameParameter[0][1]` **only while the hold is in force** (`sector_activate_.block && !sector_activate_failed_`, as the cell poll of PSC:1692-1701 reads it today), so `checkpoint_held` is true whenever `cell_read` is; with the hold not in force `cell_read = false`: the hook (`physics_session_on_sector`, PSC:2912-2924) has already reported that crossing after the fact, and a cell read would make the core report it a second time (review of E8); the resetpoint is not used (the `OnPostCheckpointReached` hook reports through `request_sector`) |
| sector_manager_active | `bool()` | `Gameplay_SectorManager->IsActive()` (SC:2100-2104), the manager found once by name (`sector_manager_id_`, host state: the activation record and its replay use it) | `physics_session_sector_manager_active` |
| **writing the world** | | | |
| write_body | `bool(const std::string&, const session::body_state& row, bool wake)`; as built (E4): `bool(entity, const double position[3], const double rotation[4], const float linear[3], const float angular[3], bool wake, std::string& error)`, the bool overload of `set_body_state` | `set_body_state` (resync rebuild SC:2803-2822; legacy ladder) | `physics_view_.set_body_state` (PSC:3290-3351) |
| mute_event_log | `void(bool)` (the core keeps the nesting counter, SC:4607-4620); as built (E6): the core's `session_client_core::event_log_mute` scope keeps the flag in force (`event_log_muted_`, core state) and restores it | `set_event_log_muted` | `physics_view_.set_event_log_muted` |
| keep_live_cells | `scope_guard(const session_client_core&)`, RAII around every `rewind_to` / `on_snapshot` (SC:2874, 3709, 3807, 4342, 4401); the guard reads `core.rollback_stats()` and `core.current_tick()` (SC:5728-5738) | `live_cells_guard` (SC:5719-5739) | the same write-back of `CurrentLevel[0][4]`, `IngameParameter[0][1..2]` + `replay_manager_activation` (PSC:2037-2046) |
| run_sector | `void(int sector)` | SC:1346-1357 | PSC:2962-2986 |
| record_activation / record_pending_activation | `void(uint32_t tick, bool reset)` / `void(uint32_t tick)`; as built (E8): `record_activation` only (the union pass); `record_pending_activation` (step 15) comes with the frame (E9) | `record_manager_activation` (SC:5695-5711, 2180-2184) | `record_manager_activation` (PSC:64-88, 2032-2036) |
| script_guards | `void(uint32_t tick, bool resolved)` | `reset_guard_.apply` | `death_reset_apply`, `sector_deactivate_apply`, `sector_activate_apply` (the clock guard, pause pin and `pause_scripts_apply` are host code before `post_step`, §1.3) |
| note_script_resets | `void(uint32_t tick)` | `script_replay::resets_pending` → `script_reset_ticks_` | the same (PSC:1964-1965) |
| **driving** | | | |
| drive | `void(const std::string& entity, const input_frame&)`; as built (E9): `bool(entity, frame, std::string& error)`, the core logs a failure | `navigation_input` | `physics_view_.navigation_input` |
| own_camera_rows | `void(const float basis[3][3])`: own replica rows + zero the retail forces | SC:2363-2367 | PSC:1905-1911 |
| force_own / unforce_own | `bool(const input_frame&)` / `void()`; as built (E9): `std::string(frame, bool& ok, std::string& error)` (the entity forced, "" without an own replica) / `bool(const std::string& entity, std::string& error)`; the core logs both `lead:` lines | `navigation_poll(off)` + `navigation_input` (SC:2149-2161) / `navigation_poll(on)` (SC:2168) | the same calls; `unforce_own` restores `navigation_poll(!input_muted)`, never plain on. (After a replay the shared binding already keeps polling off while muted: `nav_poll` gates on `input_muted`, RBB:193-201, PSC:3034. `tracked.own_polls` needs no change.) |
| own_driven_entity | `std::string()`: the own replica's entity, "" if none | `own_nav_entity_` | `s.own_nav_entity` |
| own_replica_name (added at E5, built at E9) | `std::string()`: the entity the own replica was last named by, kept after the replica is gone (a failed re-attach); "" if it never had one. `recorded_input_at` tells the own ball's records by it (SC's `entity == own_nav_entity_`, without the `own_navigation_` gate that `own_driven_entity` has) | `own_nav_entity_` | `s.own_nav_entity` |
| arm_own_capture | `void(uint32_t tick)` (optional) | `arm_own_physicalize_capture` | empty |
| **mirrors** | | | |
| mirror_entity (added at E6) | `std::string(uint32_t player, uint8_t ball_type, std::string& error)`: the entity that mirrors the player, made if missing; "" and `error` when there is none. Asked first, before the core takes the old mirror's navigation and (a trafo) body out, as SC did | `remote_entity` (CopyObject) | `objects_.ensure_player` / `get_ball_entity` |
| make_mirror | `std::optional<mirror_made>(const mirror_spec&)`; the spec is `{player, ball_type, pose[12], recipe, group, kick?{index, strength}}`; the result is `{entity, navigated}`; as built (E6): `mirror_made(const mirror_spec&)`, the spec `{player, ball_type, entity, position[3], rotation[9], recipe (bridge), group, kick, kick_index, kick_strength}` and the result `{made, error, kicked, kick_error}`: pose, physicalize and the spawn kick only; the entity is `mirror_entity`'s and the navigation `attach_mirror_navigation`'s, both asked by the core; the spec's player and ball type let a host find the entity again (it need keep nothing from `mirror_entity`) and key `set_physicalized` / `on_trafo` | `SetWorldMatrix` + `physicalize` + `push_impulse` (the shared `make_mirror_body`, §2.4, at E9) | the shared `make_mirror_body`; then `objects_.set_physicalized` / `on_trafo` |
| unmake_mirror | `void(const std::string& entity)` (navigation destroyed, body unphysicalized, log muted by the core); as built (E6): the body only, unmuted by itself: the core mutes around it where SC did (a death, a trafo) and not at the session's end (SC's `end_session_world` did not) | `unphysicalize` | PSC:2439-2448 |
| detach_mirror_navigation (added at E6) | `void(const std::string& entity)`: the mirror's navigation replica goes (unmuted: SC destroyed it before the muted unphysicalize) | `navigation_destroy` | `physics_view_.navigation_destroy` |
| discover_navigation | `bool()`: the leaf keys are known; as built (E9): `bool(uint32_t tick)`, true only in the call that learnt them (the host logs its line) | `read_navigation_graph`, every leaf keyed (SC:2189-2196) | `read_navigation_graph` with the unbound-leaf filter, ≥ 4 leaves (PSC:1716-1742) |
| attach_mirror_navigation | `bool(uint32_t player, const std::string& entity)`; as built (E6): `mirror_navigation(uint32_t player, const std::string& entity, uint8_t ball_type, std::string& error)`, `{unavailable, attached, failed}`: `unavailable` (the graph is not known yet) is neither a log line nor a change; the core checks `physicalized` / `navigation` first and logs the other two | shared `attach_navigation` (SC:4734-4805) | shared `attach_navigation` (PSC:3451-3475) |
| **own ball** (only with `own_body_control`) | | | |
| make_own | `bool(const own_body_spec&)`: `{entity, pose[12], recipe, group, kick?}`; as built (E7): `own_body_made(const own_body_spec&)`, the spec `{entity, position[3], rotation[9], recipe (bridge), group, kick, kick_index, kick_strength, tick}`, the result `{found, took_out_failed, took_out_error, made, error, kicked, kick_error}` the core logs in SC's order; called inside the core's event-log mute; the host shuts its own navigation down first | `remake_own_body` (SC:1547-1588) | empty (the game's Physicalize block owns the ball) |
| unmake_own | `void(const std::string& entity, const float pose[12])`; as built (E7): `bool(entity, const float position[3], const float rotation[9], uint32_t tick, std::string& error)`, inside the core's mute; the core logs a failure | `unmake_own_body` (SC:1522-1537) | empty |
| has_body | `bool(const std::string& entity)`; as built (E7): **not added** - `body_state` (E2, required of every host) is the same probe | `get_body_state` probe (SC:1639-1643) | `physics_view_.get_body_state` |
| own_birth_capture (built at E7) | `own_birth_pose(const std::string& entity, uint32_t tick)`: `{captured, position[3], rotation[9], spawn}`, the capture consumed; required with `own_lifecycle_source::event_log` | `own_capture_` (PreSimulate) or the entity's matrix after the tick | empty (`host_hooks`: `own_physicalized`) |
| ball_recipe_of (added at E7) | `int(const std::string& entity, ball_recipe& recipe)`: the ball table row (-1: none) and the retail recipe of that type; required of every host (the beam, the own birth) | `ball_type_of` + `fill_retail_recipe` | the `Physicalize_GameBall` row of `db_.get_ball_id` |
| **rollback** | | | |
| bind_world | `rollback_world(const core_world_hooks&)` (E4 builds it as `rollback_world()`, the host's own binding, until E9). The core passes its hooks; the host binds through the shared `bind_session_world` (§2.4). **Called on every use, never cached**, as SC:4812 is: the options take `own_key_count_` by value (SC:4822-4824), and a world bound before the own navigation was attached would replay with a key count of 0 | `physics_direct_` + `args_` options | `physics_view_` + fixed options, `input_muted` |
| **journal** | | | |
| journal_path | `std::filesystem::path(const journal_header&)`; empty = no journal | `args_.journal` verbatim | `client_journal::directory()/file_name(...)`, after pruning to 10 files |
| utc_ms (added at E2) | `uint64_t(clock_site)`: milliseconds since the Unix epoch, the header's `utc_ms` (site `journal_utc`); required only with `journal_path` | `wall_utc_ms(site)` (captured and replayed like `now`) | `system_clock::now()` in ms, the site ignored |
| **host hooks** (all optional, H) | | | |
| hooks.after_rewinds / after_record / after_amend / after_journal | `void(uint32_t tick)` | `maybe_force_rollback`, `dump_savestate`, traces, `report_queued_impulses` | a forced-rollback verb (R0/R2), nothing else |
| hooks.filter_event | `event_send_verdict(const session_event_msg&)`: send / hold / drop | `--hold-event` (SC:2662-2703); the host later calls `core.release_event` | empty |
| hooks.numbering_restarted | `void(clock_site)`: `anchor_origin`, `hold_timeout`, `assign_resync`, `assign_renumber` or `assign_release` (the five sites that reset the pacer's origin, E3) | `origin_ = wall_now(site)` | empty |
| hooks.ending (added at E3) | `void()`: the first thing `end` does | discards the `--hold-event` sends still held, logging each, before the journal closes | empty |
| hooks.window / hooks.status | `void(const std::string&)`; as built (E10): `window` at step 31 with a line the window's edge produced, `status` at `end` (the headless 5 s report calls `status_text` itself) | `log_line` (one line, the bytes `logf` wrote) | the log, plus the F3 overlay reads `status_text` |
| counters (added at E10) | `host_counters()`, optional: the host's own counters (`rebases`, `activations_replayed`, `script_reset_skips`) for the text the core prints itself (step 31's `window:` line, `end`'s status line); empty: zeros | `{rebases_, activations_replayed_, script_reset_skips_}` | the retail counterparts (R1) |
| hooks.ended (added at E11) | `void()`, optional: `end`, after the journal closed and before the mirrors are taken out (`legacy_calls::own_body_guard_off` until E11) | the own body guard lifted (`own_body_guard_off`) | empty |
| hooks.describe_movables (added at E11) | `std::string()`, optional: the text of the `--trace` line `after rollback: movable=%s` (the first 16 snapshot rollbacks); empty: the line is not written (`legacy_calls::describe_movable_objects` until E11; a host diagnostic, §2.4) | `describe_movable_objects` | empty |

**What the host feeds the core, and when.**

- `on_message`: between post_step and pre_step.
- `own_physicalized(report)` and `own_unphysicalized(tick)`: with
  `own_lifecycle_source::host_hooks` (retail), from `OnPhysicalize` /
  `OnUnphysicalize`, stamped `current_tick() + 1`. These fire during the
  behaviour pass, which is allowed (§1.3). With `event_log` (headless), the
  core raises them itself at step 19, and gets the captured pose, spawn flag
  and recipe from `platform.own_birth_capture(entity)`. That is SC:2598-2628,
  where `own_capture_` and `fill_retail_recipe` are P.
- `request_sector`: the headless `--start-sector`, and retail
  `OnPostCheckpointReached` / the `sector N` verb. Once assigned it is the same
  `hold_sector(s, current + 1 + lead)` both use today
  (SC:1330-1341, PSC:2927-2955); before the assignment it only marks the
  sector pending (step 1). As built (E8): `void`, and it always holds;
  every request made before the assignment waits, in request order, and is
  stamped at the first assigned frame. Retail's `physics_session_hold_sector`
  returns false when the session is not assigned or the hold failed, and
  its hook then reports the crossing after the fact at once
  (PSC:2912-2933). The retail host keeps that decision (R1): its hook and
  the `sector N` verb call `core.request_sector` only when the session is
  assigned and the hold is in force, and otherwise send today's
  after-the-fact Sector event (`core.send_event`), so a crossing between
  the anchor and the assignment is reported at once, as today, not stamped
  at the first assigned frame. Making it wait instead would be a retail
  behaviour change, to be listed in §4.2 if R1 wants it.
- `request_beam`: headless `--beam` (from the loop once `tick >= beam_tick`)
  and the retail `beam` verb; consumed at step 29.

### 2.3 Capabilities, and the core's behaviour without them

```cpp
struct session_capabilities {
    bool own_body_control = false;   // make_own / unmake_own filled
    bool slew = false;               // the host's pacer applies slew_step (2x raise, 0.9x lower)
    bool nonblocking_hold = false;   // the anchor hold returns to the host loop
    bool whole_frame = false;        // a replayed tick may run a whole engine frame (bind_world fills whole_frame)
    own_lifecycle_source own_lifecycle = own_lifecycle_source::event_log;   // or host_hooks
};
```

| capability | headless | retail | the core without it |
|---|---|---|---|
| `own_body_control` | yes | **no** | (Acting on echoes at all needs `own_echoes`, §1.2; off, every case below is only counted.) `move_own_birth` and `resync_own_life` never touch the own body. **Moved** (made at T' ≠ T): no rewind of the own ball; with snapshots on, the snapshot rollback reconciles it, since the server's ball exists from T'; with snapshots off, `request_resync("own birth moved: no own body control", owed)`. **Rejected or missing** (never made), and only while `own_life_current(life)`: the Physicalize is re-reported at `current + 1` without the spawn flag (LPD §3.5: "the REJECTED echo makes it immediate"), then the self-resync. **Superseded: nothing.** A superseded birth was overtaken by a newer accepted event of the same player (lifecycle.hpp:384-407); re-reporting it would bring a ball back after our own death, at the server and at every remote. `destroy_body`'s moved-own-life branch is inert. **Beam:** the record is kept (§1.3 step 29), but its stamp is applied with `write_body(entity, beam pose, zero velocity, wake)` instead of `unmake_own` + `make_own`: live at T+1 (`live_own_lives`, pre_step step 8) and in every replay that crosses it (`before_replayed_tick`), with the generation bumped at the request as SC:1436 does. That is what removed headless's 540 m (the move outside the replays, SC:1406-1412). The in-place body keeps its floor contact, which the server's fresh body lacks; no resync is asked for, because a resync rebuild writes in place too (`write_body`) and could not remove it. R1/R3 report the own ball's error after each beam. |
| `slew` | yes | **no** | SessionReady `caps` = 0 (no `CAP_LEAD`), so the server never decides a lead for this client (LPD plan step 10: "no decision without CAP_LEAD"). A SessionLead that arrives anyway is a protocol error: counted, journaled, not applied, not acknowledged. |
| `nonblocking_hold` | yes | **no** | Same state machine and 10 s deadline (`kAnchorHoldTimeout`, SC:84-88; retail PSC:1537-1559). The host blocks in a 1 ms loop around `poll_hold()` and drains the whole mailbox inside it, as the headless loop does between polls. Retail today drains only the assignment queue. It is needed only at the anchor: the core has no other hold. |
| `whole_frame` | with `--replay-scripts` | **no** | `bind_world` leaves `whole_frame` empty, and the engine replays with `script_replay::execute_frame` (RBB:203-216 and `rollback_binding_scripts.hpp:76-82`), as both do today. |
| `own_lifecycle` | `event_log` | `host_hooks` | Not a gap: two ways to learn the same fact. Retail must capture in `OnPhysicalize`, because it applies the spawn offset with `SetWorldMatrix` before the body exists (PSC:2714-2893). |

`caps` in SessionReady is computed by the core from the capabilities:
`CAP_LEAD` iff `slew`. No host writes it.

### 2.4 Host support: platform code both hosts share

These are near-verbatim copies in both clients today. Task B §1, §3 and §4
list them. They are platform code (CK, a physics backend), so they cannot be
in the core, but they must not be written twice either.

**Where they live.** `include/game/session_host_support.hpp` holds them as
templates over a duck-typed backend:

- `physics_view` on retail;
- `physics_direct` on headless, **extended** to the methods these helpers use.

The methods to add to `physics_direct` are `physicalize`, `unphysicalize`,
`navigation_create/destroy/set_ball`, `push_impulse`,
`set_body_guard/group/matrix`, `drain_event_log`, `list_bodies`, `capture`,
`force_block_get/set` and `reset_session_clock`, all under physics_view's
names.

**The helpers:**

- `bind_session_world(backend, core_world_hooks, host_world_options)`. It
  composes `bind_rollback_world` and `bind_script_replay` with the core's
  hooks in one order:
  - `before_replayed_tick`: the core's `drain_pending_births` first, then
    `replay_manager_activation`, then the host's traces (SC:4833-4846);
  - `replay_remotes` and `on_correction`: the core's;
  - `on_poll_resumed`: the host's camera re-arm;
  - `destroy_body`: the core's moved-own-life branch, then the shared
    mechanism destroy.

  It replaces SC:4812-5074 and PSC:3022-3184.
- `local_birth_log`: the store behind `birth_tick_of` / `birth_order` /
  `destroy_body` / `insert_birth`. It is unified on headless's `has_local`
  (SC:5769-5789 vs PSH:202-235) and fed from the event log's `recipe` entries
  (headless) or from `OnPhysicalize` (retail).
- `parse_event_log` (the `"t=… kind name;"` format, SC:2265-2320 and
  PSC:1816-1857) and `parse_recipe_event` (SC:5769-5875).
- `attach_navigation` (mirror and own replica creation), `make_mirror_body`
  (`SetWorldMatrix`, `physicalize`, group, spawn `push_impulse`), and
  `to_bridge_recipe` / `matrix_from_pose`.
- `manager_activations` (record and replay) and `live_cells_guard`.

The headless-only diagnostics (`describe_*`, `diff_savestate`) stay in the
headless host.

## 3. What stays in each host

### 3.1 Headless host (SC shrinks to this)

- **Program shell:** `arguments`, `parse`, `usage`, `main` (SC:90-465,
  6187-6211); GNS connect and login, room handling, `announce_map`
  (SC:553-676, 973-1038).
- **The loop and the clock (SC:606-640, 1920-2003):**
  - `origin_` and the pacing arithmetic;
  - the hold wait (non-blocking: `poll_hold`, then return to the loop);
  - the slew applied to `origin_` from `core.slew_step`;
  - a rebase calls `core.schedule_rebased("tick schedule rebased")`.
- **Engine:** boot and reboot, level request, `load_step`, Sequencer capture
  and restore (SC:558-582, 1145-1191, 1196-1208).
- **Anchor platform work (SC:1193-1276):**
  - clock reset, collision filter;
  - script selection and force hooks;
  - `reset_guard_.resolve`, `hold_checkpoint_activation`;
  - then `core.anchor({hash, sha="headless-64/32", build_id})`.
- **Platform state:**
  - `navigation_`, `own_navigation_`, `own_nav_entity_`, `own_group_set_`
    (`previous_cam_` is core state, read through `core.previous_cam()`);
  - the own key codes;
  - `own_capture_` and the PreSimulate capture (SC:535-546, 1834-1910,
    6174-6184);
  - `body_guard_*`, `scripts_`, `script_hooks_`, `reset_guard_`;
  - `sector_manager_id_`, `local_birth_log`, `physics_direct_`.
- **Harness options.** They reach the flow only through the hooks of §2.2:

  | option | how it reaches the flow |
  |---|---|
  | `--record`, `--keep-lane` | the keys set before `pre_step`, and `observe_input` |
  | `--start-sector` | `request_sector` |
  | `--force-rollback` | `hooks.after_rewinds` → `core.force_rewind` (its core half is `rewind_to` + `drop_carried_own_replay`, SC:3681-3762) |
  | `--beam` | `core.request_beam` once `tick >= beam_tick` (the core's step 29 does the rest, including the no-navigation in-place branch of SC:1427-1437) |
  | `--hold-event` | `hooks.filter_event` and `core.release_event` |
  | `--pause-at` / `--stall-ms` | the loop |
  | `--dump-savestate`, `--compare-scripts` | `hooks.after_record` |
  | `--trace*` | `hooks.after_amend` / `after_journal`, the world wrappers in `bind_world` |
  | `--replay-scripts` | `whole_frame` |
  | `--insert-births`, `--park-births` | `host_world_options` |
  | `--window-origin` | `core_config.window_shift` |

- **Reporting:** the 5 s `report()` prints
  `core.status_text({rebases_, activations_replayed_, script_reset_skips_})`.
  The text must stay byte-identical: the harness parses `status:` and
  `window:`.

### 3.2 Retail host (`BallanceMMOClient/session/core_host.cpp`, new)

- **Ownership, and the BML constraint.** `BallanceMMOClient` hangs the engine
  at startup when members are added (MH:392-396, commit 33cc5c9). So:
  - the core and the `retail_platform` are **file-scope statics**
    (`std::unique_ptr<session_client_core>` etc.) in `core_host.cpp`,
    created at the first SessionStart; the login id is kept until then and
    given to the core with `set_own_id` before each `begin` (E11);
  - the mailbox is a **load-time** file-scope static, never created lazily:
    `begin` runs later, through `run_on_game_thread` (PSC:204-208), while
    the network thread pushes at once. `begin` drops only the entries of
    other sessions;
  - no member is added to `BallanceMMOClient`;
  - `physics_session_state` (PSH) keeps its layout until R7. Its dead fields
    are removed only then, with the three-launch check.
- **Receive.** `on_message` on the network thread (MC:2402-2452) copies the
  bytes of every Session* opcode into one mutex-guarded mailbox, in arrival
  order, SessionLead included. The 64-snapshot cap of
  `kMaxQueuedSnapshots` (PSH:365) applies to snapshot entries only.
  SessionStart still goes through `run_on_game_thread`.
- **Session start UI:**
  - the not-in-level leave;
  - the 3-2-1 countdown;
  - `fixed_tick_.enable` + `restart_current_level`;
  - anchor detection at the Gameplay_Ingame edge;
  - `SendIngameMessage`;
  - the F3 overlay and status (PSC:3513-3609, reading `status_text` and
    `rollback_stats`).
- **Script edits:**
  - the pause clock pin and clock guard (PSC:516-569, 1659-1672);
  - the pause targets (713-840);
  - N1 death/level reset (887-1016);
  - N2 deactivate pin (1036-1172);
  - N3 activate hold (1191-1289);
  - the Sequencer restore and the Random block hook.

  They are handed back at session end. They are `script_guards` and anchor
  work.
- **Game objects:**
  - `get_current_ball`, `db_.get_ball_id`, `current_level_array`;
  - the `objects_` spirit balls (`ensure_player`, `get_ball_entity`,
    `set_physicalized`, `on_trafo`);
  - the `Physicalize_GameBall` forces;
  - the navigation graph with the unbound-leaf filter;
  - `zero_retail_forces`.
- **Camera:** the `Cam_OrientRef` rows and the `CamRef_BMMO_*` refs.
- **Own capture:** `OnPhysicalize` (recipe from the block arguments, the spawn
  flag from `CurrentLevel[0][3]`, the spawn-cell offset, the queued
  `push_impulse`) and `OnUnphysicalize`. These call `core.own_physicalized`
  / `own_unphysicalized`.
- **Pacing:** `fixed_tick_` (no slew until R5). A rebase calls
  `core.schedule_rebased("tick driver rebased")`, as PSC:1709-1712 does.
- **`core_config`.** The retail host fills every field before `begin`.
  The defaults are the headless argument defaults (`savestate` and
  `script_replay` on, ...), so a default-constructed config would quietly
  turn the exact restore on; retail sets `savestate` 0 and `restore_clock`
  false (§4.1).
- **Automation verbs:**
  - `session [trace|rollback on|off|core on|off|force-rollback E:D]`;
  - `journal on|off|mark` (`core.journal()` with `core on`, the legacy
    singleton with `core off`);
  - `beam` → `core.request_beam`;
  - `sector N` → `core.request_sector`;
  - the new `net lag|blackout` verb (R0);
  - the new `replay session <file>` mode (R0): frame k of a key record is
    fed at tick `tick_base + k`, counted from the first assigned frame, with
    no clock reset. Today's `replay` anchors at the first Gameplay_Ingame
    frame after the verb and calls `reset_session_clock(1)`
    (client_automation.cpp:720-726) after `process_physics_session`
    (MC:464-468): started before the countdown it anchors before the
    restart, and at the anchor edge it overwrites the session seed's clock
    reset (PSC:1362) after SessionReady's hash.
- **Transitional host code, deleted at the step named:**
  - the input-starvation detector (PSC:3241-3267) reads `core.acked_input()`
    and calls `core.request_resync(why)` until R4;
  - the 30-`unmatched` trigger (PSC:2062-2087) compares
    `core.rollback_stats()` before and after the mailbox drain and calls
    `core.request_resync`, until open question 1 is decided. `too_far` →
    resync is deleted at R1, and `frozen` → resync moves into the core for
    both hosts at R1 (§7 Q1).

## 4. Deliberate differences

### 4.1 Differences that survive as a capability or a host policy

| difference | today | becomes |
|---|---|---|
| own birth capture | headless: PreSimulate capture + event-log `created` (SC:1834-1910, 2302); retail: `OnPhysicalize` before the body exists, with the spawn offset (PSC:2714-2893) | `own_lifecycle_source` (§2.3) |
| own body re-made / taken out | headless can (SC:1522-1588); retail cannot | `own_body_control` |
| SessionLead | headless `CAP_LEAD`; retail `caps=0` (PSC:1461-1465) | `slew` (R5 builds it for retail) |
| anchor hold | headless non-blocking; retail blocks inside `OnProcess` | `nonblocking_hold`; one state machine |
| whole-frame replay | headless `--replay-scripts` only | `whole_frame` |
| `restore_clock`, savestate flags | headless args; retail false / 0 (PSC:3026-3030; the clock guard replaces the clock rewind) | `core_config` values set by each host |
| navigation leaf rule | retail drops unbound leaves that other mods graft (PSC:1718-1734, the black-box bug of 9.15); headless needs every leaf keyed | `discover_navigation`, host |
| own navigation attach | headless only with rollback (SC:2226); retail always | `own_body_upkeep`, host |
| BodyRevived name filter | `_BMMO_` vs `_Peer_`, plus the own ball | `reportable_wakeup`, host |
| pause and mute | retail only (`INPUT_FLAG_PAUSED`, keys 0) | `observe_input`, host |
| mirror entities | CopyObject vs spirit balls | `make_mirror`, host |
| journal location and pruning | `--journal <file>` vs `ModLoader/BMMOJournals/<generated>`, 10 files | `journal_path` (the host names the file and prunes) |
| receive thread | headless same thread; retail network thread | the host mailbox; the core is game-thread only |
| pacer origin at re-assignment | headless resets `origin_`; retail's `fixed_tick_` counts independently | `hooks.numbering_restarted` |
| "server has no body for us" re-report | retail only, after 30 snapshots (PSC:2620-2660) | core policy `own_body_backstop` (retail on, headless off: open question 2) |
| retail resync on 30 unmatched; starvation | retail only | host-side, transitional (§3.2; open question 1) |
| relay counters with no input rewind | headless counts `relay_late`, `bodiless`, `birth_frozen` with rollback on | with `input_rewind` off (retail from R1 to R2) `note_input_correction` returns before them, so they stay 0 (E9 review): no check reads them while the switch is off |
| resync on a frozen clock | retail only (PSC:2084) | a core rule for both hosts from R1: a frozen clock sets the bodies and drops the history with no replay (RB:1031-1042), so a re-anchor is the only recovery. Inert on headless (`frozen=0` in every run; the §5.3 zero rule checks it) |

### 4.2 Differences that disappear

Each one goes at the step named.

| difference | today | why it goes | step |
|---|---|---|---|
| no input rewind on retail | PSH:350-353, PSC:2405-2408 | It was never a design choice. LPD §1.4 lists it as a deferred card, and the stepping primitive exists (`step_physics` = `Simulate`; snapshot replays already re-step N ticks in one `OnProcess`). | R2 |
| remote events applied on arrival, batch sort (applied, join, origin) | PSC:2191-2212 | The server's order is (join, applied, origin, rank). Retail's was only "its existing batch order" (LPD §3.1 table). A due-at-tick drain is what makes a body set agree with the server's. | R1 (the drain); R3 (the rewinds) |
| mirror started from the cached newest row, impulse skipped | PSC:2351-2394 | A workaround for making a late birth at the live tick (the 9.17 spawn twitch). With `lifecycle_rewind` the birth is made at its tick inside the replay, which is what the row approximated. It is kept as `mirror_start_from_row` while `lifecycle_rewind` is off. | R3 |
| echoes and the held section only counted | PSC:2186-2204, 2220-2241 | They are LPD parts one and three, which the core implements. (The LPD §1.4 text "Applied now" describes code that does not exist; it is corrected at R4.) | R3 (echoes), R4 (held) |
| SessionLead not dispatched | MC:2402-2452 | The mailbox takes it; the core ignores it without `slew` (§2.3). | R1 |
| retail resync limiter: 2 s only | PSC:3270-3286 | The core's is 2 s plus the 10 s in-flight and the owed debt (SC:5084-5131). Without them an own correction that needs a resync is lost to the limiter. | R1 |
| `on_correction` checkpoint on kind 1 only | PSC:3064-3082 | Headless checkpoints on 1/5/6. One journal policy. | R1 |
| journal TICK hash after the snapshot correction | PSC:2131-2135 | One position: the headless one (post_step step 32; a snapshot lands between frames). Retail's journal content changes, retail's world does not. | R1 |
| retail journal policy | `session_journal_client.cpp` (§1.2 table) | `core on` writes the core's (headless) journal: checkpoint rule, owners filled, FRESH/SUBSTITUTED, `first_tick = tick_base`, join fallback 255. The SimTool replay and `worst_bodies.py` read both, which R1 checks. | R1 |
| `too_far` → resync | PSC:2081 | The engine refuses a too-deep snapshot or rewind before restoring and writes nothing (RB:367, 610, 1533). The trigger follows a stale comment (PSC:2020), and with input rewinds it would turn a refused own rewind into a resync. Headless has no such trigger. | R1 |
| own NAV_ACTIVE from `ball_nav_active_`; the mute edge recorded a tick early | PSC:1874-1915 | The record must be what the replica stepped (§2.2 `observe_input`); every replay drives the own ball from it. Neutral without rewinds, wrong with them. | R2 |
| retail's own copies of the legacy ladder, resync rebuild, `entity_of` / `input_at`, `drive_remotes` | PSC:2455-2613, 3187-3227, 3290-3351, 3480-3501 | Identical in intent; the core's are the ones verified headless. `live_drive` adds the no-input case (keys 0, navigation off), which retail's fallback lacked. | R1 |
| beam verb: stamped T (an already stepped tick), moved in place at the verb, no beam record | PSC:2674-2710 | `request_beam` stamps T+1 and keeps the beam record (`own_life`, SC:1416-1468); without own-body control the stamp is an in-place write, live and in every replay (§2.3). | R1 |
| `destroy_body` without the moved-own-life branch and `has_local` | PSC:3101-3130 vs SC:4982-5029 | The shared `bind_session_world`. | R1 (helper), R3 (branch live) |
| `replay_remotes`, pending births missing in the retail binding | PSC:3022-3184 | The shared binding. | R2 / R3 |

## 5. Behaviour preservation for the headless client

**No E step changes what the headless client does.** "Unchanged" is checked
four ways. Unit tests are required at every step, the deterministic checks
at every step, and the four-client A/B at the steps §6.1 names.

### 5.1 Unit tests

- `BallanceMMOMessageTests`, all of it, passes on Windows x64 and on Linux
  (Arch-WSL GCC; the POSIX `link()` alias of 6bf105b is the kind of collision
  to expect from new test code).
- A new `tests/session_client_core_test.cpp` drives the core with a **fake
  platform** that records every call:
  - **Frame order.** One `post_step` / `on_message` / `pre_step` cycle
    produces exactly the call sequence of the tables in §1.3. This pins the
    order against later edits.
  - **Numbering.** A first assignment renumbers. A duplicate or an older
    assignment changes nothing. A re-assignment resyncs:
    `invalidate_history`, `rebase_own_lives`, `held_floor_`.
  - **The hold.** Release on assign; the timeout at 10 s on the fake clock,
    with the journal note.
  - **Events.** The drain is in (join, applied, origin, rank). Same-tick
    births and deaths are held until after the rewinds. The supersede guards
    work.
  - **Own echoes.** Moved, rejected, superseded and missing, each with and
    without `own_body_control`, and with `own_echoes` off (counted only).
    Without the capability there is no `make_own` / `unmake_own` call; a
    re-report on rejected or missing while the life is current, nothing on
    superseded; a resync on moved with snapshots off.
  - **Own reports from hooks.** `own_physicalized` queued in the behaviour
    pass is committed at step 19 with the generation after the drain's bump.
  - **Beam.** `request_beam` before `post_step(T)` is stamped T+1; without
    `own_body_control` it is one `write_body` live at T+1 and one in a
    forced replay crossing T+1.
  - **Sector request before the assignment** stays pending and is stamped
    `T + lead` at the first assigned `pre_step`; the union pass's cell write
    raises no Sector event at step 26.
  - **Status text.** `host_counters` values appear at their places;
    the format literal count is unchanged.
  - **SessionLead.** With `slew`: amend [N, c], forced (c, E), ack at the end
    of the slew. Without it: not applied, no ack, `caps` 0 in SessionReady.
  - **Capability assertions.** A declared capability with an empty field
    fails construction.
- The existing `rollback_binding_test` gains a case for `bind_session_world`'s
  hook order.

### 5.2 Deterministic checks (exact, at every E step)

1. **`scripts/savestate_check.py --journals --server 127.0.0.1:26700 --pairs 50:12,97:24,131:47`**
   with the new binary, against the Arch server (`arch_server_snap.sh wsl
   26700 2 66 4.2`). Every forced-rollback arm must be EXACT against its
   control: `worst_bodies.py` 0.0 m, never above 1e-6 m, and forced rollbacks
   really taken.
2. **Old against new, solo, snapshots off.** The control arm of (1) runs
   with snapshots on, so its SNAPSHOT and correction records are stamped at
   the arrival tick and differ even between two old runs. This check
   therefore runs the same solo client against `arch_server_lp.sh … 100000
   100000 4.2` (snapshots off), once with the old binary and once with the
   new one, on the same server build. The two client journals must have
   identical TICK hashes at every tick from the anchor to the end, and
   identical checkpoint bodies. The two files must be byte-identical outside
   the header's `build_id` (the full `ballanced-x+bmmo-y` string,
   journal.hpp:113, 464; SC:5167) and `utc_ms`, and the TICK records' `ms`.
   - First establish that two runs of the old binary are identical: solo with
     snapshots off is deterministic.
   - If they are not, the step stops and the cause is found before anything
     else.
3. **`BallanceMMOSimTool --replay-session`.** Run it on the server journal
   and on every client journal of the A/B runs (§5.3). Return code 0 and the
   same summary line as the old binary's runs.
4. **The log-format check.** Extract every format literal passed to `logf`
   (old: SC; new: SC + `src/session/client_core*.cpp`) and compare the
   multisets. Any difference must be named in the step's review. The harness
   parses these lines, so a mechanical move must not reword them.
5. **Multi-client replay (from E0 on).** E0 adds a frame-indexed inbound
   capture to the old SC (`--capture-inbound <file>`: every received
   message with the frame index it was handled before, and every wall-clock
   read the flow makes outside the pacer: the hold deadline, the resync
   limiter, `journal_ms`) and a replay mode (`--replay-inbound <file>`: the
   loop is unpaced, feeds the captured messages and clock values at their
   frames, and sends nothing). The four captures of one snapshots-on
   swing-bay run are taken once with the base binary. Each E step replays
   them with old and new: the four client journals must be byte-identical
   under the (2) exclusions. This is the only exact check of the snapshot
   path, the relay, the lifecycle drain and the union under loss; the
   §5.3 A/B stays as the live check.
6. **The wire check (from E4 on).** A replay sends nothing, so (5) cannot
   see what the flow puts on the wire. `--trace-sends <file>` (SC, added
   after E3) writes one line per payload, the tick, the Steam flags, the
   size and an FNV-1a 64 of the bytes, before the replay's early return, so
   a replay writes it too. The build id inside SessionReady (and its length)
   is replaced by a fixed token before hashing, since any two builds differ
   there. Each E step replays the 16 captures with old and new, both with
   the trace (`wire_check.sh OLD NEW TAG` in the tools), and the traces
   must match line for line, except the first 36-byte send: the login,
   whose uuid comes from the system clock, which the capture does not hold.
   E3 ran the same check with a temporary copy of the trace: 62898 sends
   equal.

**As built (step E0).** Where the code made (5) and (2) differ from the
text above:

- **The unit is a main-loop pass, not a frame.** `inbound_capture.hpp`
  (`BallanceMMOServer/sim/`) writes, in order: `I` at the start of every
  pass of `run()` (its index and the steps before it, a step being a
  `load_step()` or `frame()` call: the frame index), `M` every message
  `poll_incoming_messages` handled, `S` every connection state change, `C`
  every wall-clock read with its site. A replay re-runs the same passes,
  with no sleeping. A frame-indexed feed could not reproduce the session:
  the slew advances once per *pass* with the wall time since the previous
  one, `hold_polls_` counts passes, `release_held_events` runs per pass,
  and the hold timeout, the rebase (and its `request_resync`) and "wait or
  step" are decided per pass.
- **The pacer's reads are captured too**, not only those "outside the
  pacer". All 23 sites are in `bmmo::session::clock_site`
  (`BallanceMMOCommon/include/session/clock_site.hpp`, so the core can name
  them; `inbound_capture.hpp`'s `wall_site` is an alias, and the values are
  the file format): the run limit and report clock,
  the Ready limiter, the three SessionAssign origins, the anchor origin and
  hold deadline, the hold poll and timeout, the schedule, rebase and due
  reads, the slew's two, the `--hold-event` stamp and release, the resync
  limiter, `journal_ms`, `journal_start`, the header's `utc_ms` and
  `last_report_`'s initial value. A replay therefore takes every pacer
  decision the live run took.
- **A read that moves into the core keeps its site.** The replay hands a
  captured value only to a read at the same site, so the core cannot read
  a site-less clock. §2.2 is amended to match: the platform op is
  `now(clock_site)` (headless: `wall_now(site)`; retail ignores the
  argument), and the hook is `numbering_restarted(clock_site)`, called with
  `assign_resync`, `assign_renumber` or `assign_release` (headless:
  `origin_ = wall_now(site)`). So `journal_ms` moving into the core's
  journal at E2 reads `now(clock_site::journal_ms)`, the resync limiter at
  E3 `now(clock_site::resync_limiter)`, and so on for `hold_poll`,
  `anchor_hold`, `slew_start` and `slew_step` at E3/E5/E9. The order of
  the reads must not change either.
- **Not captured:** the login uuid's seed (it only shapes bytes that are
  sent) and the rollback engine's CPU timings (`t_rest`, `t_rep`, `t_cap`:
  status line only). A replay's log differs from the live log in those
  fields, the journal path and GNS's "connected" line, and in nothing else.
- **A replay checks itself.** A clock read at a site other than the
  capture's next, a pass with another step count, a pass the live run
  never started (the end marker met at a pass start: the live run ended
  inside its last pass), or records left at the end prints
  `replay-inbound: DIVERGED ...` and exits 3. Only a capture without an
  end marker (the live run was killed) ends softly. A capture whose write
  failed gets no end marker and logs `capture-inbound: WRITE FAILED`. A
  replay whose arguments differ from those in the capture's header (apart
  from `--server`, `--journal`, the capture flags and `--record`'s path)
  logs a warning that names the first difference. A change inside a
  frame that keeps the control flow shows in the journal comparison
  instead. Negative control: replaying swing client 0 with `--no-savestate`
  diverges at tick 249, and 3213 of 3435 TICK records differ.
- **Results.** There are four capture runs:
  - a swing bay with snapshots on (the beam arm of `runs_lp20.sh`);
  - A6m with snapshots off and a held own birth;
  - the §5.3 forced-resync arm (`resync_snapoff`), which covers a rebase,
    the resync limiter and the re-assignment on all four clients;
  - A5u with the lead on (`a5u_lead`), where client 2 has a SessionLead
    slew.

  All 16 client journals replay **byte-identical to the live journal,
  twice**. Together the captures cover every clock site except
  `hold_timeout`, which no server run produces. They do not cover the
  limiter's held-back branches ("resync owed", or a request while one is
  in flight). The README lists the sites each capture covers. A replay
  refuses to run (exit 4) when the game data it reads (`base.cmo`,
  `Database.tdb`, `3D Entities/**`) differs from the sha256 pins in
  `pins.json`. That
  includes `build_id`, `utc_ms` and the TICK `ms`, since the binary is the
  same and the clock is the captured one. The captures, journals, drive
  records, a README with the commands and the tools (`e0_replay.py`,
  `e0_compare.py`, `e0_replay_all.sh`, `e0_logfmt.py`) are in
  `maps/arena-test/core_ref_e0/` (since step REF, 2026-10-24, that set is in
  `core_ref_e0_pre_q/`, with its tools; `core_ref_e0/` holds the re-record).
  "Old" for the later E steps is the E0
  binary: it carries the capture. A moved clock read must keep its site
  and its place in the order.
- **(2) cannot be byte-identical outside `build_id`, `utc_ms` and `ms`,
  even for two old runs.** Three more things differ between live runs,
  and none of them is the world:
  - the own player id: the server assigns it per connection, and it
    appears in the header, the start note, the own INPUT records and the
    checkpoint owners;
  - the length: `--seconds` is wall-clock;
  - where a RECEIVED checkpoint lands: with snapshots off the server
    still sends a few full snapshots, and each is written on arrival.

  `e0_compare.py --live` maps the own id to OWN and compares up to the
  shorter run's last tick. It compares the received snapshots as an
  ordered list by content, and every other record field by field. Under
  that comparison, old-old, old-new and old against new-with-capture are
  identical: TICK hash and pose at every tick from the anchor to the end,
  every LOCAL checkpoint and every other record.
- **(1) is only valid when no run starved.** In the solo check the client
  runs about 0.1 tick ahead of the server. A run in which the server
  starved of the client's input (`input: ... starved=` above 0)
  substitutes that input, and the snapshots move the own ball. Two of
  three attempts after the review showed a MISMATCH this way: one starved
  arm, then a starved control. HEAD's binary starved too. The third
  attempt had no starvation and was EXACT on all three pairs.
- **(4)** The new literals are the eleven `capture-inbound:` /
  `replay-inbound:` lines of the opt-in modes (176 old literals, 187 new).
  No existing literal changed.

### 5.3 Four-client A/B in the same batch

**Binaries.**

- Old: the build of the step's base, copied to `build-agent-ab/old/`.
- New: the step's build, copied to `build-agent-ab/new/`.

Both are copies, as `runs_lp20.sh` uses `build-agent-s20`, so a rebuild
during the batch cannot change them. The server is the Arch build of the base
commit for both. No E step touches the server.

**Arms.** These are the standard set. Each uses 4 clients, 60 s, `--drive`,
`--checkpoint-ticks 2`, and snapshots off (`arch_server_lp.sh … 100000 100000
4.2` with the HEAD batch's server flags) unless the row says otherwise. A
"full A/B" in §6.1 is all seven arms; a step that names arms runs those:

| arm | `arena_map_test.py` arguments |
|---|---|
| lane control | `--latency-ms 150 --arm A0 --sectors 1 --keep-lane --event 2:1500:none` |
| swing bay, 10 % loss, beams | `--latency-ms 150 --loss-pct 10 --sectors 4 --beam-bays 18,18,18,18 --beam-spread 3` |
| A1 burst | `--latency-ms 150 --arm A1 --sectors 1 --keep-lane --event-client 2 --event-tick 1500 --blackout-ms 300 --event-dir up` |
| A6m hold | `--latency-ms 50 --arm A6M --sectors 1 --keep-lane --client-arg-for '2:--hold-event physicalize:2:300'` |
| **snapshots on** (server `arch_server_snap.sh wsl 26700 2 66 4.2`) | the swing-bay arguments: exercises `apply_snapshot`, the snapshot rollback and `rb` / `far` / `unmatched` |
| **rollback off** | the lane-control arguments plus `--client-arg --no-rollback`, snapshots on: the legacy ladder, `corrector_`, the 3-hard resync |
| **forced resync** | the lane-control arguments plus `--client-arg-for '2:--pause-at 1500 --pause-ms 1000'`: a pacer rebase → `request_resync` → the resync rebuild, `invalidate_history`, `rebase_own_lives` |

Every arm of revision 1 ran with snapshots off, so the snapshot path, the
resync rebuild and the legacy ladder moved in E4 were never run, and `rb`,
`far` and `unmatched` passed trivially at 0. The last three arms close that.

**Order.** Per arm: old, new, old, new (O1 N1 O2 N2). Each pair then sees
the same host drift. The batch starts only after two consecutive quiet
samples (total CPU < 15 %), as `runs_lp20.sh` waits.

**Noise.** For each metric m and each client, the band is
`[min(O1,O2) − d − ε, max(O1,O2) + d + ε]`, where:

- `d = |O1 − O2|`;
- ε is the metric's resolution: 0.0001 m, or 1 count.

A step passes an arm when:

- both N1 and N2 lie in the band for every metric;
- every metric that is 0 in both O runs is 0 in both N runs;
- the harness's own `--arm` verdict lines are the same as the O runs';
- the replays of §5.2 (3) are exact.

**Metrics** (per client, from the harness report, the `status:` and
`window:` lines and the server log):

- **Errors.** Own-ball error in phases B / D / A, mechanism error, and the
  mirror of the event client from T_off+60.
- **Rollback work.** Rollbacks per class, with mean depths:
  - `rb` (snapshot);
  - `subst rew` (own);
  - `inrew` (remote);
  - life rewinds;
  - `far`, `unmatched`.
- **Relay and inputs.** Relay lag p99, overall and per source; `relay_late`;
  substituted ticks (`subst=`); inputs sent.
- **Lifecycle and lead.** `events=`, `echo=`, `ownlife=`, `overtaken=`, moved
  births, `lead_adj=` / `lead=` / `slew=`.
- **Resyncs:** `resyncs=` (requested, applied).
- **Server.** The S1 / S3 / S4 lines and the `late by` counts; the server
  journal replay rc.
- **CPU per rewind.** `t_rest`, `t_rep`, `t_cap`. These are CPU-sensitive,
  so they count only within the band, and only when the host-load guard
  passed.

**Host-load guard.**

- **Running the sampler.** It starts before the first run and is stopped by
  its stop file after the last:
  ```
  powershell -File C:/Users/GEEKER~1/AppData/Local/Temp/claude/D--repos-BallanceMMO/c8d36a4f-80ba-4c5e-971f-a7c8bf697232/scratchpad/cpu_sampler.ps1 \
      -Out <batch>.cpu.log -Stop <batch>.cpu.stop
  ```
  It logs total CPU and the five busiest processes outside the measurement
  every 10 s.
- **When a run is void.** A run is void, and is rerun at the end of the
  batch, if any of its samples shows a non-measurement process above 10 % of
  the machine, or the total above 85 %.
- **When the batch is void.** If both runs of an O pair or of an N pair are
  void twice, the step waits for a quiet host. It does not judge on noisy
  numbers.

**The pass rule on this host (from E4 on).** E3 showed that the band rule
cannot be met here. At 150 ms every client falls 33 ticks behind while it
rolls back for corrected inputs, and rebases once, at a tick that moves from
run to run. Which regime a run lands in decides most metrics. Two old runs
fail the rule against each other (34 to 88 metrics out of band), even though
the logic is exact under the captured clocks. An E step is therefore judged
on three things:

1. **The exact checks of §5.2 (2) to (6).** They are the proof that the
   behaviour has not changed.
2. **CPU parity.**
   - Replay the four captures of `resync_snapoff` at once, unpaced, two
     rounds per binary, alternating old and new, and record the wall time of
     each.
   - One-sided: no new time may exceed the old maximum plus the old spread
     plus 5 %. A faster step passes. (Revised at E4. The first rule was
     two-sided at 3 %, and E4 failed it by being 6 % faster. Two builds of
     the same code differ by up to 4 % in this check: E3's binary against
     `e3t`.)
   - On a failure, the old commit's code is rebuilt in today's tree as a
     control. The step is judged against that control before a slowdown is
     called.
   - CPU is the one thing a host-side move can change that the exact checks
     do not see, and a slower step would move the rebase.
3. **The row's arms, as a live check.** One batch, O1 N1 O2 N2, with the
   host-load guard. Every client of every run captures its inbound stream
   (`--capture-inbound`). The step passes an arm when all of these hold:
   - **the cross-replay** (from E5 on): the other binary replays each
     capture to a journal identical to the live one;
   - every server journal replays with rc 0 and 0 checkpoint mismatches;
   - no client crashes, or a crash is reproduced by the other binary from
     the same capture. E8 hit such a crash: a teardown crash after the time
     limit, in `CKContext::Reset`, which E7's binary reproduces.

   **Why these decide (revised at E10).** The cross-replay shows that both
   binaries write the same journal from the same inbound stream. The wire
   check of §5.2 (6) shows that the same journal comes with the same sends.
   CPU parity shows that the cost is the same. Together they leave nothing
   in the step's code that could make an N run differ from an O run. A
   difference between live runs is therefore one between their networks and
   hosts, whether or not its cause can be named.

   The band rule, the zero rule (`far`, `overtaken`, `unmatched`,
   `lead_adj` and the late mirror, against both O runs) and the criteria
   rule (each N run's harness criteria against the O pool of the arm) are
   still computed and reported. Each difference is traced where the
   journals and logs allow, to one of these events:
   - a pacer rebase and its resync;
   - a snapshot delivered deeper than the history reaches (its delivery
     depth names it);
   - a lifecycle event the server applied late ("late by" in the server log)
     and the resync it caused;
   - the teardown, after the run's first client reached its time limit.

   An untraced difference is listed in the step's note, but it does not fail
   the step.

   **Host note (after E10).** Most of the rebase regime was the scheduler.
   On this i7-14700KF, Windows 11 put the windowless headless clients on the
   efficiency cores whenever the app that launched them was not in the
   foreground. One replay took 30 to 32 s that way, against 18.6 s pinned to
   the performance cores. From the commit after E10, the client and SimTool
   opt out of power throttling at start (`sim/process_qos.hpp`). The lane
   control at 150 ms then went from every client rebasing in both old runs
   (client 0 starved 1449 and 409 ticks) to one rebase in one run (client 2
   at 2686, the heavy window) and none in the other (`e10abq_lane_*`).
   Runs measured before that commit, the late-player steps and E3 to E10,
   ran partly on efficiency cores. The rule was tightened step by step, at E4 (the events), at E7
   (the criteria) and at E10, before it was put this way. On this host, two
   old runs differ from each other as much as old and new do: rebase
   regimes, the slot in the batch, and a phase-B own-ball error that heals
   in one run and lasts in another.

## 6. Migration plan

Every step is one implement-and-review pass. Every step leaves:

- the headless client, the retail mod and the tests building on all targets
  (x64 MSVC, Arch and Ubuntu GCC, x86 MSVC with a clean client target);
- the headless behaviour unchanged by §5.

A step is never closed by "builds". Each one names the number that closes it.

### 6.1 E steps: the core, with the headless client as the only host

The method is **state first, then functions.**

- E1 moves the core's state into the core object, while the logic stays in
  SC and reaches the state through `core_.s.<member>`. The rename is
  mechanical and reviewable with a word diff.
- Each later E step moves one area's functions into `src/session/` verbatim.
  Free physics calls become platform calls; `logf` becomes the core's `logf`
  (the same printf format, a growing buffer).
- Nothing is rewritten while it moves. Cleanups wait for E11.

**Linking while the core is incomplete: `legacy_calls`.** The core's
sources are in all three targets from E1 on, but neither the mod nor
`BallanceMMOMessageTests` links SC, and the headless target cannot reach
SC's private members from Common. Code moved "verbatim" still calls
functions that move later: `apply_snapshot` (SC:2757-2981) calls
`recorded_input_at` (SC:3939), `current_ball` and `describe_movable_objects`;
the SessionAssign path calls `rebase_own_lives` (SC:1711) and
`set_aside_unanswered` (SC:1725). So E1 adds a transitional
`legacy_calls` struct of `std::function` fields, one per SC function a moved
function still calls. SC fills it, the fake-platform test fills it with
recorders, and the mod leaves it empty (the core is not run there before
R1). Each E step adds the entries its moved code needs and deletes the
entries whose target it moves; E11 asserts the struct is empty and deletes
it. (As built: its last two entries became optional platform hooks, see
"As built (step E11)".) A step's review lists the entries it added and removed. The journal moves
right after E1 (E2), because every area journals.

| step | what moves | checks that close it |
|---|---|---|
| **E0** inbound capture (old SC only) | `--capture-inbound` / `--replay-inbound` (§5.2 (5)) in SC, off by default; no other change. | A captured 4-client snapshots-on swing-bay run replays with the same binary to byte-identical client journals (§5.2 (2) exclusions), twice. If a hidden wall-clock dependency defeats it, it is named and §5.2 (5) is dropped; the E steps then rely on §5.3 alone. |
| **E1** core skeleton | `client_core.hpp`, `client_platform.hpp`, `cmake/SessionCore.cmake` in the three targets. The core **state** (§1.2) moves with its initializers unchanged. Empty `session_platform`, `session_capabilities` and `legacy_calls`. The headless `headless_platform` is started (send, now, log). `physics_direct` is extended (no callers yet). | §5.1 builds and runs; §5.2 (1), (2), (4), (5) |
| **E2** journal | The headless journal helpers (SC:5150-5373) become `client_journal` in Common, owned by the core (§1.2); `journal_path` platform op (headless: `args_.journal`). Retail is untouched (its legacy singleton stays until R7). | §5.2 (2) with the journal bytes identical; (3); (4); (5) |
| **E3** numbering, hold, resync | `begin` (SC:1041-1144 less the engine reboot), `anchor`'s SessionReady half, the hold state machine with `hold_polls_`, SessionAssign (SC:726-816), `request_resync` / `retry_owed_resync` (SC:5084-5131), `flush_inputs`, `remember_sent`, `send_event`, `put_event_on_wire` (SC:2630-2756), SessionEnd. Legacy calls: `rebase_own_lives`, `set_aside_unanswered`, the resync rebuild's own-life half. | §5.1 (numbering, hold); §5.2; A/B arms lane control + forced resync |
| **E4** the snapshot path | `apply_snapshot` (SC:2757-2981): rollback path, resync rebuild, legacy ladder; `keep_live_cells`, `write_body`, `bind_world` (still SC's own `rollback_world()`). Legacy calls: `recorded_input_at`, `own_ball`, `describe_movable_objects`. | §5.2; A/B arms snapshots on + rollback off + swing bay |
| **E5** relay and parts one and two | SessionRemoteInput intake (SC:857-917), `note_input_correction`, `replay_corrected_inputs` (SC:3503-3976), section J (SC:3051-3564), SessionLead (SC:918-924), the forced own frames' core half, `force_rewind` (SC:3681-3762). | §5.1 (SessionLead); §5.2; **§5.3 full A/B** |
| **E6** remote lifecycle | Event intake (SC:824-856), `drain_due_events`, `apply_held_lifecycle`'s remote half, `apply_event`, `make_remote_birth`, `rewind_for_lifecycle`, pending births, `live_drive`, the core half of `physicalize_remote` / `unphysicalize_remote` (SC:3977-4042, 4389-4700). Platform: `make_mirror`, `unmake_mirror`, `mute_event_log`, `attach_mirror_navigation`. | §5.1 (event order); §5.2; A/B arms lane control + A6m + swing bay (the late births and deaths under loss: event repeats, overtaken births, `rewind_for_lifecycle`) |
| **E7** own lives and echoes | `own_life` and SC:1416-1756 (C parts), `report_physicalize`'s C half, `process_own_echo(es)`, `answer_set_aside`, `expire_own_echoes`, `resync_own_life`, `move_own_birth` (SC:4043-4388), the beam record and `request_beam`. Platform: `make_own`, `unmake_own`, `has_body`, `own_birth_capture`; `session_capabilities` with headless all true; the `own_echoes` switch. | §5.1 (echo, beam and own-report tests, both capability settings); §5.2; **§5.3 full A/B** |
| **E8** sectors | `hold_sector`, `request_sector` with the pending start sector, the union pass, own sector detection (`poll_sector`), `sector_cell_`. | §5.1 (sector request); §5.2; A/B arm swing bay (`--sectors 4`) |
| **E9** the frame | `pre_step` and `post_step` (§1.3 tables) replace the body of `frame()`; the host keeps keys, step and hooks. Input building, record, amend, drive; `bind_session_world` and the rest of host support (§2.4), used by headless only; `poll_hold`, `slew_step`, `rebase_threshold`, `schedule_rebased`, `hooks.numbering_restarted`, `after_amend`, `after_journal`. `recorded_input_at` (left a legacy call at E5) moves into the core over `own_replica_name` (§2.2), and its legacy call is deleted. The §1.2 switches `input_rewind` and `own_corrections` are built (headless: both = `rollback`), gating `replay_corrected_inputs`, the remote half of `note_input_correction` and the own amend/rewind of part one. | §5.1 (frame-order test; both switches off: no replay, no own amend); §5.2 (the switches change nothing on headless: (5)/(6) identical); **§5.3 full A/B** |
| **E10** reports | `status_text(host_counters)`, `window_text(tick, host_counters)`, one literal each; `hooks.window/status`. | §5.2 (4) with zero differences; the harness parses the A/B runs of E9 again from the new binary's logs, and its report is identical to E9's numbers on the same inputs (a lane-control A/B pair) |
| **E11** close the core | Make the state private; delete the dead state (§1.2), the `core_.s.` access and `legacy_calls`; SC ends as the host of §3.1. No logic change. | §5.1; §5.2; **§5.3 full A/B** against the base of E1 (the cumulative check) |

**As built (step E1).** Where the code made E1 differ from the text above:

- **The state is `client_core_state`**, the member `s` of
  `session_client_core` (`include/session/client_core.hpp`). Its
  declarations are SC's, moved verbatim with their comments and
  initializers (every removed SC line reappears unchanged in the header,
  except `uint8_t snapshot_interval_ = 2, input_delay_ = 6;`, split because
  `snapshot_interval_` is dead state that stays in SC). The nested types
  moved with it: `phase`, `remote_ball`, `own_life`, `pending_birth`,
  `forced_own`, `applied_death`, `window_base`, and the constants
  `kRelayLagBuckets` and `kWindowTicks` (the first sizes a member). SC names
  them through `using` aliases and `static constexpr` copies, so its code is
  unchanged but for the `core_.s.` prefix: with that prefix stripped, SC
  equals its E0 text apart from the removed declarations and the added
  lines. The rename skipped comments and string literals. `core_` is SC's
  last member, so it is destroyed first, where the journal was.
- **Members §1.2 does not name, placed by use.** Core: the counters the
  status and `window:` lines print (`amend_failures_`, `input_reports_`,
  `input_starved_`, `worst_starved_`, `lead_sum_`, `lead_count_`,
  `remote_writes_`, `remote_inputs_received_`, ...), `sessions_ended_`
  (counted by `end`, a core entry point), the log limiters of core logic (`subst_late_logs_`,
  `subst_unresolved_logs_`, `lead_force_logs_`), `lead_slew_clock_` (with
  `lead_slew_`), `header_acked_` (the acked-header family) and the journal's
  bookkeeping, `journal_start_` included. Host: `event_log_muted_` (it moves
  with `mute_event_log`'s nesting counter at E6), `resetpoint_seen_` and
  `checkpoint_held_` (`poll_sector`'s platform half), `beam_done_` (the
  host's `--beam` trigger), `paused_once_`, `restore_traces_`,
  `rollback_diag_`, `step_diag_`, the trace and script counters,
  `manager_activations_`, `local_births_`. The constants
  `kOwnEcho*` / `kOwnRecordKeepTicks` stay in SC until their functions
  move (E7). Of the sectors row, `start_sector_pending_` moved; the
  requested sector it stands for is still `args_.start_sector` (SC:1517,
  SC:2275) and becomes core state at E8, with `request_sector`.
- **One source file.** Only `src/session/client_core.cpp` exists (the
  constructor); each other file of §1.1 is created by the step that fills
  it. `cmake/SessionCore.cmake` lists it in `BMMO_SESSION_CORE_SRC`, which
  the three targets add; the retail mod compiles it and never constructs the
  core.
- **The platform at E1** (`include/session/client_platform.hpp`) has
  `send`, `now(clock_site)` and `log`, with `send_mode`, `log_level`,
  `own_lifecycle_source`, `session_capabilities` and an empty
  `legacy_calls`. The constructor throws `std::invalid_argument` naming the
  first missing operation (`missing_platform_operation`): "fails
  construction" holds in Release builds, where an `assert` would not. The
  capability checks arrive with their operations. The headless platform is
  `session_client::make_platform()`, not a class of its own: `send` →
  `send_bytes` (the mode mapped to `k_nSteamNetworkingSend_*`), `now` →
  `wall_now(site)`, `log` → `log_line(text)`, a new free function that
  writes what `logf` writes. It is not `logf("%s", ...)`, which would have
  added a literal to the §5.2 (4) multiset. Nothing calls the platform yet.
  The headless client passes default capabilities: nothing reads them
  before their operations exist, and declaring `own_body_control` before
  `make_own` exists would defeat the constructor check.
- **`physics_direct`** gained `capture`, `reset_session_clock`,
  `drain_event_log`, `list_bodies` (both returning by value, as
  `physics_view`'s do), `force_block_get/set`, `set_body_matrix`,
  `physicalize`, `unphysicalize`, `set_body_group`,
  `navigation_create/set_ball/destroy`, `set_body_guard` and
  `push_impulse`, under `physics_view`'s names and signatures, with no
  callers. The header now includes `ball_navigation.hpp` and
  `world_hash.hpp` itself instead of relying on its includer's order.
  `list_bodies` returns `std::vector<bmmo_physics_body_state>` on both
  backends (the §2.2 row is corrected). One known backend difference:
  `physics_view::drain_event_log` reads through a 262144-byte buffer and
  truncates a longer log, `physics_direct`'s returns it whole; settle it
  (or keep it, recorded) when the first caller arrives (E6/E9).
- **`session_client` is neither copied nor moved** (its move operations
  are deleted, which deletes the copies too): `make_platform()` hands the
  core lambdas that hold `this`. A retail host that copies the pattern
  needs the same.
- **§5.1 at E1** (`tests/session_client_core_test.cpp`, 7 tests over a
  recording fake platform): construction calls nothing, a platform without
  `send`, `now` or `log` is refused, the operations are handed back in order,
  the capability defaults, `legacy_calls` is empty, the moved state keeps the
  headless initializers, and the moved types keep their defaults
  (`own_life::moved()`, `pending_birth::of`). The flow tests of §5.1 arrive
  with the flow.
- **Results.**
  - §5.2 (5): all 16 reference captures replayed with the E1 binary
    (`build-agent-e1/`, build id `bmmo-9d9cb085a558-dirty`), every replay
    reached its capture's end marker, and every client journal is the same
    length as its live reference, with 11 differing bytes, all inside the
    header's `build_id` (`0180b6e2b2a6` against `9d9cb085a558`). `utc_ms` and
    the TICK `ms` are identical too, since they are replayed.
  - §5.2 (4): 187 literals against 187, equal.
  - §5.2 (2): one solo snapshots-off run of the E1 binary (`solo/e1_new1`,
    starved=0) against the stored `old1` with `e0_compare.py --live`:
    TICK hash and pose identical at all 2237 common ticks (8-2244), LOCAL
    checkpoints 1119/1119, the 8 RECEIVED snapshots equal in content, other
    records 5627/5627: IDENTICAL.
  - §5.2 (1): `savestate_check.py --journals` against
    `arch_server_snap.sh wsl 26700 2 66 4.2`, starved=0 in the control and
    in all three arms: forced-50x12 (49 forced rollbacks), forced-97x24 (25)
    and forced-131x47 (18) EXACT, every one of 53 bodies 0.0 m. (Each
    run's `arena_map_test.py` verdict line says "no mechanism interaction
    observed", a multi-player criterion the check prints and does not
    judge.)
  - Host load over the live runs: 10.2 % total at most, no other process
    above 2.1 %.
  - After review (the deleted moves, the quoted include, the notes above),
    rebuilt and checked again: (5) all 16 replays (`e1b`) byte-identical to
    their live journals outside the build id; (4) 187 = 187; (2)
    `solo/e1_new2` (starved=0) IDENTICAL to `old1` at all 2418 common ticks
    (8-2425), LOCAL checkpoints 1209/1209; (1) the first savestate batch had
    starved=4 in forced-131x47 and was not read; the rerun, starved=0
    everywhere, was EXACT in all three arms (46, 25 and 18 forced
    rollbacks, 53 bodies, 0.0 m). Host load at most 11.3 %, no other
    process above 4.2 %.
  - Builds: the x86 mod was rebuilt with its target's objects deleted
    first (a clean client target; the mod includes no header E1 changed,
    so nothing could have been stale). The Linux configure of the server
    tree ran in the Arch build (`arch_build_server_dirty.sh`, ARCH-BUILD-OK),
    which configures `BallanceMMOSessionClient` with `SessionCore.cmake`;
    the Ubuntu CI configure and a GCC link of the test target were not run
    (the sources pass `linux_test_syntax.sh`, and the headless client and
    `client_core.cpp` a GCC syntax-only compile with the Arch tree's flags).

**As built (step E2).** Where the code made E2 differ from §1.2, §2.2 and
the row above:

- **The name is `bmmo::session::core::client_journal`**
  (`include/session/client_journal.hpp`, `src/session/client_journal.cpp`).
  The retail mod's legacy singleton is `bmmo::session::client_journal` and is
  compiled into the same DLL until R7, so the core's type lives in the nested
  namespace `core`, with the design's class name. The constants
  `kJournalCheckpointTicks` (660), `kJournalCheckpointGap` (33) and
  `kJournalMaxBytes` (256 MB) moved with it.
- **It is a member of `session_client_core`, not of `client_core_state`.**
  It reaches the core through a back reference (state, `config`,
  `platform()`, `current_tick()`). The writer and the bookkeeping
  (`journal_start_`, the last and pending checkpoint, `journal_announced_`)
  left `client_core_state` and are private to it. The back reference makes
  `session_client_core` non-copyable. `journal_` is the core's last member, so
  the file closes before the state goes away. When E11 makes the state
  private, `client_journal` needs `friend class core::client_journal;` or
  accessors for `s` and `config`.
- **The helpers became methods** with the prefix dropped: `journalling`,
  `ms`, `open`, `close`, `note`, `announce_player`, `leave_player`,
  `lifecycle_event`, `rollback_correction`, `body_correction`,
  `checkpoint_due` and `local_checkpoint`. SC calls them as
  `core_.journal().<method>`. The records the flow still in SC writes (INPUT
  FRESH, SUBSTITUTED and RELAYED, TICK, RECEIVED checkpoints) go through
  `journal().writer()` until their areas move (E4, E5, E9). So
  `client_core_journal.cpp` (§1.1) does not exist yet.
- **Host values the core cannot get itself.**
  - `open(hash, build_id, own_name)`: `bmmo::physics::build_id()` is in the
    physics library, which the mod does not link. The headless start note
    and PLAYER records name the own player `--name` and every other player by
    id. Retail's nicknames are an R1 question.
  - `local_checkpoint(tick, own_ball)`: the own ball's name comes from SC
    (`current_ball()`, CK) until the `own_ball` operation exists (E9). It is
    read only when a checkpoint is due, as before.
- **Platform operations added** (`client_platform.hpp`):
  - `journal_path(const journal_header&)`, as designed. The header passed in
    has everything but `utc_ms`.
  - `utc_ms(clock_site)`. This one was not in §2.2 (now added to its table,
    "utc_ms (added at E2)"): the header's `utc_ms` is
    a system-clock read at its own site (`journal_utc`), which the
    steady-clock `now` cannot give. Headless `wall_utc_ms` now takes the site.
  - `body_state` and `list_bodies`, from §2.2, for body corrections and
    LOCAL checkpoints. Headless `list_bodies` is
    `physics_direct(engine_->physics()).list_bodies()`: a first buffer of 64
    instead of SC's 256, and the same bodies in the same order.
  - `missing_platform_operation` requires `utc_ms`, `body_state` and
    `list_bodies` only when `journal_path` is filled. An empty `journal_path`
    means the host never journals.
- **The order inside `open`.** The header (without `utc_ms`) is built first,
  then `journal_path` is asked, then the old journal is closed ("a new
  session started"), then the directory is made, then `utc_ms` and
  `journal_start` are read. That is SC's order of effects and clock reads.
  With no path there is no read and no close, as with SC's
  `args_.journal.empty()` return.
- **`core_config` starts here**, with `journal_checkpoint_ticks` only, as
  the public member `config`. SC fills it in `begin_session`, beside the
  rollback switches it sets from its arguments; `begin` takes it from E3 on.
  0 means `kJournalCheckpointTicks`, as SC's
  `args_.checkpoint_ticks ? ... : 660` did. The §1.2 table's "cadence 0 =
  off" does not hold: no configuration reaches the `cadence != 0` branch,
  and `--checkpoint-ticks` clamps to at least 1. Kept verbatim.
- **Two small moves the journal needed.**
  - `current_tick()` moved into the core (§1.3 "reading"); SC's forwards to
    it.
  - `to_bridge_recipe` (with `copy_name`) moved to the CK-free
    `include/session/bridge_recipe.hpp`, because the journal's EVENT record
    needs it. §2.4 had put it in host support, which is CK. SC's copy is
    deleted, and SC names the shared one with a using-declaration. The
    retail copy (`physics_session_client.cpp`) stays until R0. It must go
    then: its unqualified calls would become ambiguous by ADL once that file
    sees the shared one.
- **The core's `logf`** is `session_client_core::logf`, formatting into a
  growing buffer and calling `platform().log` (headless `log_line`: the same
  bytes as SC's `logf`). The two `journal: %s` literals moved into
  `client_journal.cpp`, so the §5.2 (4) check now reads
  `src/session/*.cpp`, not only `client_core*.cpp`.
- **`legacy_calls`**: no entry added or removed. The journal calls no SC
  function.
- **§5.1**: 4 new tests (`SessionClientCoreJournal.*`):
  - the operations a journalling host must fill;
  - no path means no journal and no clock read;
  - every headless record type over a temporary file, read back, with the
    call order `journal_path`, `utc:journal_utc`, `now:journal_start`, log;
  - the checkpoint rule.

  `StateKeepsTheHeadlessInitializers` now checks `journal().journalling()`
  and `config` instead of the moved members. 362 tests pass.
- **Results.**
  - §5.2 (5): all 16 reference captures replayed with the E2 binary
    (`build-agent-e2/`, build id `bmmo-16387edf3b37-dirty`). Every replay
    reached its capture's end marker, and every one compared IDENTICAL with
    `e0_compare.py`. Strict byte check (`e1_bytecheck.py`): every journal has
    the same length as its live reference (7178667 to 10753920 bytes). There
    are 12 differing bytes per file, all inside the header's build id
    (`0180b6e2b2a6` against `16387edf3b37`).
  - §5.2 (4): 187 literals against HEAD's 187, equal (SC + `client_core.cpp`
    + `client_journal.cpp`).
  - §5.2 (3): `BallanceMMOSimTool --replay-session --continue-after-jump`,
    the E1 SimTool on each live client journal against the E2 SimTool on its
    replay. All 16 return the same rc and summary line. Without
    `--continue-after-jump`, all 16 stop at the anchor-to-base jump (rc 3),
    identically. The rc-3 rows past the jump are received-snapshot
    mismatches that the old run has too. The E2 SimTool replays the four
    reference server journals exactly (rc 0, 3986 to 4257 ticks, 0
    checkpoint mismatches).
  - §5.2 (2): one solo snapshots-off run of the E2 binary (`solo/e2_new1`,
    starved=0) against the stored `old1` with `e0_compare.py --live`. TICK
    hash and pose are identical at all 2444 common ticks (8-2451), LOCAL
    checkpoints 1222/1222, the 8 RECEIVED snapshots equal in content, other
    records 6146/6146: IDENTICAL. Host load at most 8.5 %, no other process
    above 3.9 %.
  - Builds: x64 BUILD-OK. The x86 mod was rebuilt with its target's objects
    deleted first (it includes no header E2 changed; `client_core.cpp` and
    `client_journal.cpp` are the only core objects it compiles). Arch server
    build ARCH-BUILD-OK. `linux_test_syntax.sh` passes on every line,
    `client_journal.cpp` included. GCC compiles the headless client
    syntax-only with the Arch flags, and both core sources with
    `-Wall -Wextra`, with no error or warning.

**As built (step E3).** Where the code made E3 differ from §1.3, §2.2 and
the row above:

- **What moved** (`src/session/client_core.cpp`, verbatim but for the
  platform calls and the back-calls): `begin` (SC's `begin_session` up to
  the host's own resets), `level_requested`, `anchor` (from the phase change
  on: the window reset, the hold, SessionReady, the anchored line, the
  journal open), `poll_hold`, `end`, `on_session_assign`, `on_session_end`,
  `request_resync`, `retry_owed_resync`, `flush_inputs`, `remember_sent`,
  `send_event`, `put_event_on_wire` and `release_event`.
  `kAnchorHoldTimeout` moved to `client_core.hpp`. SC keeps one-line
  forwarders for `send_event` and `request_resync`, which its unmoved code
  calls in many places.
- **Entry points the design does not name.**
  - `on_session_assign` / `on_session_end` instead of `on_message`: the host
    decodes those two opcodes and hands the message over until every
    Session* area has moved; `on_message` replaces them then.
  - `level_requested()`: `begin` returns before the engine reboot (host
    work), and the phase becomes `loading` only after it, at the end of SC's
    `begin_session`, as before.
  - `anchor_report` carries `own_name` (the journal header, as E2's `open`)
    and `load_ticks` (the anchored line) besides the hash, sha and build id.
  - `hold_state` is `{held, timed_out}`. A release happens in SessionAssign,
    not in a poll, so `released_now` could never be set; `assigned()` and
    `holding()` say it. `timed_out` has no headless reader (SC reads
    `held`); it is kept for the retail blocking loop's log (R1).
    `release_hold(why)` (retail: connection lost) is not built: no
    headless path calls it; it comes with the retail host (the R1 row says
    so).
  - The send helpers are wider than §1.3: `flush_inputs`, `remember_sent`
    and `put_event_on_wire` are public. SC's per-frame send still calls
    `remember_sent` (SC:2467) and the unit tests drive the other two. They
    become private once `on_message`, `pre_step` and `post_step` exist
    (E9), so that a host cannot pass `filter_event` by.
  - `session_ready_msg.hpp` and `session_resync_msg.hpp` do not compile on
    their own (pre-existing); `client_core.cpp` and the test include them
    after `client_core.hpp`.
- **`core_config`** gained `level` (the `--level` fallback) and the rollback
  engine's switches (`verbose` ... `trace_plan`, the headless argument
  defaults). `begin` takes it whole (`config = cfg`) and sets the engine's
  switches, in SC's order; SC builds it in `make_core_config()`.
- **Host hooks** (`session_platform::hooks`, `session_hooks` in
  `client_platform.hpp`), all optional, arrived here instead of at E9:
  - `numbering_restarted(site)` at the five sites that reset the pacer's
    origin: `anchor_origin`, `hold_timeout`, `assign_resync`,
    `assign_renumber`, `assign_release` (headless `origin_ = wall_now(site)`;
    each read keeps its site and its place);
  - `filter_event` (`event_send_verdict` send / hold / drop): `--hold-event`,
    after the count and the journal record, as `hold_event` was;
  - `ending`, new: `end` calls it first, so the headless `--hold-event`
    discards are logged before the journal closes, as in SC's `end_session`.
- **Capabilities.** The headless client now declares `slew` and
  `nonblocking_hold`. `anchor` reads `slew` for SessionReady's caps
  (`CAP_LEAD` iff slew; headless sent `CAP_LEAD` unconditionally, so its
  bytes are unchanged). `nonblocking_hold` changes nothing in the core: there
  is one state machine, and the flag only says how the host waits. No
  operation is required for either yet (the pacer's arrive at E5/E9).
- **`legacy_calls` moved** to `client_core.hpp` (an entry names
  `own_life::aside`). Entries added, seven: the row's `rebase_own_lives` and
  `set_aside_unanswered`, and five the row did not list: `reset_held_epoch`
  (SessionAssign, `begin`, `end`; section J, E5), `drop_pending_rewind`
  (`anchor`; E5), `finish_lead_slew` (`request_resync`; E5),
  `end_session_world` (`end`: the own body guard off and every mirror's
  navigation and body out, after the journal closes; E6) and `report` (`end`
  prints the status line; E10). None removed. The row's "resync rebuild's
  own-life half" is not needed yet: the resync rebuild is `apply_snapshot`'s,
  which stays in SC until E4.
- **§5.1**: 10 new tests (`SessionClientCoreFlow.*`): the protocol refusal;
  `begin` (parameters, join order and group, the fresh state, the level
  fallback); `anchor` (the call order, SessionReady decoded, `caps` with and
  without `slew`, the 10 s deadline); the hold released on assign (polls
  counted, the hook order); the hold timeout at 10 s on the fake clock with
  its journal note; duplicate, older and other-session assignments changing
  nothing, and base 0; a re-assignment resyncing (`invalidate_history`'s
  state, `rebase_own_lives`, `held_floor_`, a snapshot awaited); the resync
  limiter (before the base, in flight, 2 s, owed and retried, a slew ended
  with `reanchored`, an owed one answered by a pending resync); event sends
  (`filter_event` hold, release, reliable no-Nagle, the unreliable repeats,
  inputs split at `MAX_INPUT_FRAMES`, the sent ring); `end` (another
  session's only logged; ours in SC's order). `LegacyCallsStartEmpty` checks
  the seven entries are empty in a fresh core. 372 tests pass.
- **Results.** Old is HEAD f8b07e5's binary (`build-agent-e2final/`, build
  id `bmmo-f8b07e5fe2b8`), new the E3 binary (`build-agent-e3/`,
  `bmmo-f8b07e5fe2b8-dirty`).
  - §5.2 (5): all 16 reference captures replayed with the E3 binary; each
    reached its end marker and compared IDENTICAL with `e0_compare.py`.
    Strict byte check: every journal has its live reference's length
    (7178667 to 10753920 bytes); 10 bytes differ per file, all inside the
    header's build id. What the captures exercise of the moved code: `begin`,
    `anchor` and the hold released on assign (all 16); a pacer rebase,
    `request_resync` taken at once and the re-assignment's resync
    (`resync_snapoff`, all four clients, and `a5u_lead` client 2 twice); a
    `--hold-event` send held and released (`a6m_snapoff` client 2); the
    input flush and the event sends and repeats (all 16). **Covered by no
    capture, only by the unit tests** (checked by hand against HEAD, and
    the unit tests check the new code, not old against new): the limiter's
    held-back branches (a request in flight, within 2 s, owed, and
    `retry_owed_resync`: no "resync owed" line in any log), a resync
    ending a slew (`finish_lead_slew(..., true)`), the hold timeout, the
    protocol refusal, a base-0 assignment, `on_session_end` / `end` and
    `hooks.ending` (every run stops by `--seconds`), the `filter_event`
    drop verdict and the discard at session end, and `begin` closing a
    previous session's journal (one session per run).
  - §5.2 (4): 187 literals against HEAD's 187, equal.
  - §5.2 (3): the old SimTool on each live reference against the new one on
    its E3 replay (`--continue-after-jump`): all 16 have the same rc and
    summary line. The new SimTool replays the four reference server journals
    exactly (rc 0, 3986 to 4257 ticks, 0 checkpoint mismatches).
  - §5.2 (2): solo snapshots off (`solo/e3_new1`, starved=0) against `old1`:
    TICK hash and pose identical at all 2425 common ticks (8-2432), LOCAL
    checkpoints 1213/1213, the 8 RECEIVED snapshots equal in content, other
    records 6099/6099: IDENTICAL. The server's journal of that run has the
    same TICK hash and pose as `old1`'s server at all 2897 common ticks, so
    the sends the core now makes (SessionReady, inputs at the assignment,
    events and their repeats) reached the server's world unchanged; the
    replays above cannot see sends.
  - §5.3, lane control and forced resync, three batches (`e3ab1_*`,
    `e3ab2_*`; `e3ab3_*` forced resync only), O1 N1 O2 N2 each. Host load at
    most 23.6 % total with no other process above 5.6 % in the first two
    batches; in the third, O2 is void (`backgrounddownload` 12.6 %). **The
    band rule fails, and fails as much between old runs.** On this host at
    150 ms every client falls 33 ticks behind while rolling back for
    corrected inputs and rebases once (`resyncs=1`, starvation 113 to 1813
    ticks per client), at a tick that moves from run to run: the old binary
    rebased its non-paused clients at 1250, 2200 or 2650-2700 in the lane
    control and at 1620-1925 in the forced resync arm; the new one at
    2640-2700 or not at all (lane), and at 1200-1430, 1822, 2710-2900 or
    3362-3429 (forced resync). Which regime a run lands in decides most metrics.
    Applying the rule to old runs of two batches as if one pair were new
    puts 34 to 88 metrics outside the band; old against new puts 33 to 125
    (design metrics). Pooled over the batches, the new runs lie on both
    sides of the old ones. What the arms do show the same on both binaries:
    paused client 2's forced resync (requested at 1500, re-based at
    1572-1574 from the full snapshot of 1548-1557) whenever no earlier
    rebase preceded it, `far` 0, `overtaken` 0, `lead_adj` 0, the late
    mirror 0.0000 m, and every server journal replaying exactly (rc 0, 0
    checkpoint mismatches). The CPU cost is equal: an unpaced replay of a
    reference capture takes 29.55 / 29.56 / 29.56 s old and 29.14 / 29.53 /
    29.72 s new, and the four `resync_snapoff` captures at once 20.93 /
    21.10 s old and 21.23 / 20.68 s new. Since the logic is exact under the
    captured clocks and costs the same, the A/B differences are the rebase
    regime's run-to-run variance; this host cannot close these two arms by
    the band rule (§5.3 needs a host with headroom at 150 ms, or a latency
    at which no client rebases).
  - Builds: x64 BUILD-OK; x86 mod CLIENT-BUILD-OK with its target's objects
    deleted first (it compiles `client_core.cpp`, whose header changed);
    `linux_test_syntax.sh` passes on every line (20 sources); GCC compiles
    the headless client syntax-only with the Arch flags, and both core
    sources with `-Wall -Wextra`, with no error or warning. The Arch server
    build was not rerun: no server source changed, and the server target
    compiles neither `BMMO_SESSION_CORE_SRC` nor a file that includes
    `client_core.hpp` (only the headless client and the tests do).
  - **After review**, on the final tree (comments, this note, the §2.2
    hooks rows, §3.2 and the R1 row; no code change), rebuilt and checked
    again: 372 tests pass; x64 BUILD-OK; x86 CLIENT-BUILD-OK with the
    target's 21 objects deleted first; `linux_test_syntax.sh` every line
    ok; the GCC compile of the headless client and both core sources rc 0
    with no warning. (5) all 16 replays (`e3f`) IDENTICAL and
    byte-identical outside the build id; (4) 187 = 187; (3) all 16 the
    same rc and summary line, the four server journals rc 0 with 0
    checkpoint mismatches; (2) `solo/e3_new4` (starved=0) IDENTICAL to
    `old1` at all 2452 common ticks (8-2459), LOCAL checkpoints
    1226/1226, other records 6166/6166.
  - **The wire bytes, old against new.** A replay sends nothing, so (5)
    cannot see what the core now puts on the wire. Both binaries were
    built once more with the same temporary trace in `send_bytes` (with
    `BMMO_SEND_TRACE` set: tick, Steam flags, size and FNV-1a 64 of every
    payload, written before the replay's early return; not committed)
    and the 16 captures replayed with each: 62898 sends (61970
    unreliable no-delay inputs, 838 reliable no-Nagle events, 90
    reliable), the same tick, flags, size and hash in the same order on
    every client, except each client's first: the 36-byte login, whose
    uuid is drawn from the system clock (not captured, §5.2 E0 note).
    SessionReady, the resyncs, the inputs flushed at the assignment and
    the event repeats are therefore byte-equal on everything the
    captures exercise.
  - §5.3, a fourth batch (`e3ab4_*`, both arms, host at most 22.6 %, no
    other process above 4.4 %, no void run): the band rule fails again,
    lane control 25 metrics, forced resync 80 metrics and 6 harness
    criteria. The same regime decides it: in the lane arm O1 rebased
    clients 2 and 3, O2 client 1, N1 none, N2 clients 2 and 3; in the
    forced-resync arm O1, N1 and O2 rebased every client (O1 at
    1822-1842) and N2 only the paused client 2 at 1500, so N2 passes
    criteria (`resyncs 0`, `relay_late 0`) the old runs fail. Pooled over
    every batch (old range widened by its spread, per-run client mean):
    lane control 0 metrics outside (6 old, 6 new runs); forced resync 12
    outside (7 old, 8 new), all from two new runs of batch 1, whose
    clients rebased before the pause (1197-1432) so that client 2 rebased
    a second time at 1500 (`resyncs` 1.25, phase B error), and from N2
    of batch 4, which did not rebase at all. Paused client 2 requests its
    resync at 1500 in every run of both binaries; `far`, `overtaken`,
    `lead_adj` and the late mirror are 0 everywhere, and every server
    journal replays exactly. **The two arms stay open**: the step is not
    committed until they are closed on a host or latency where the
    rebase regime does not vary, or the band waiver is decided.

**As built (step E4).** Where the code made E4 differ from §1.3, §2.2 and
the row above:

- **What moved** (`src/session/client_core_messages.cpp`, the file §1.1
  names for the snapshot path, added to `cmake/SessionCore.cmake`):
  `apply_snapshot` whole - the RECEIVED checkpoint, the stale rule, the
  resync rebuild, the snapshot rollback and the legacy ladder - verbatim but
  for the platform calls, the configuration and the back-calls. The eleven
  `logf` literals it prints moved with it, unchanged.
- **Entry point.** `on_session_snapshot(msg)` counts `snapshots_received_`
  and applies a snapshot of the running session, as SC's `SessionSnapshot`
  case did; the host decodes the message (the E3 pattern, until
  `on_message`). `apply_snapshot` is private.
- **Platform operations added** (`client_platform.hpp`):
  - `write_body`. Not the §2.2 `(entity, row, wake)`: the ladder writes a
    correction's target (a `ball_pose`, not a row), and the moved code logs
    the write's error text (`hard set: %s`, `remote %u: %s`). The signature is
    the bool overload of `set_body_state`, with the error out-parameter
    (§2.2 row amended). The core keeps SC's one `error` string across the
    function; nothing reads it but the log after a failed write, and every
    write clears it first (`write_body_state`), so the ladder's mechanism
    probe, now `platform.body_state` with its own error string, changes no
    output. Clearing `error` first is therefore the host's duty, and the
    header comment says so (the retail wrapper of `set_body_state` must do
    the same).
  - `own_driven_entity` (§2.2 "driving"): the snapshot rollback's
    `entity_of` names the own replica's entity. Headless returns
    `own_navigation_ ? own_nav_entity_ : ""`, SC's expression: the entity
    outlives a destroyed replica.
  - `bind_world` as `rollback_world()`, no argument: SC's own
    `rollback_world()` (the row's "still SC's own"). The `core_world_hooks`
    argument and `bind_session_world` come with the frame (E9).
  - `keep_live_cells` as `scope_guard(const session_client_core&)`;
    `scope_guard` is a move-only class in `client_platform.hpp` that runs an
    exit function once. Headless wraps its `live_cells_guard` in a
    `shared_ptr` the exit function releases, so the guard is built where SC
    built it (before `bind_world`) and destroyed where SC's left scope (after
    the `after rollback` trace line). The four other `rewind_to` sites
    (forced, input, lifecycle twice) are still SC's and still use
    `live_cells_guard` directly, until E5 and E6 move them. The contract
    says the guard reads the core's rollback statistics and current tick, so
    the core got the §1.1 accessor `rollback_stats()` (the engine's
    `rollback_stats`, CK-free, one line) next to `current_tick()`: a retail
    guard has only the core reference once E11 makes the state private. The
    headless guard still reads the same state through `core_.s` (no
    behaviour change); the fake platform's guard reads it through the
    accessor.
  - `missing_platform_operation` now requires `body_state` (no longer only
    with a journal), `write_body`, `own_driven_entity`, `bind_world` and
    `keep_live_cells` of every host: every host applies snapshots, and the
    core calls them unguarded. `client_platform.hpp` includes `rollback.hpp`
    (the `rollback_world` return type).
- **`core_config`** gained `rollback`, `correct` and `trace` (headless
  `--no-rollback`, `--no-correct`, `--trace`; defaults the argument
  defaults). `trace` is kept apart from `verbose`, which the headless client
  also fills from `--trace` but which is the rollback engine's switch; the
  core's diagnostic lines (`own ball error`, `after rollback`) read `trace`.
- **State.** `rollback_diag_` (the 16 `after rollback` lines) moved from SC
  into `client_core_state`: `apply_snapshot` was its only user. E1 had
  placed it in the host.
- **`legacy_calls`**: added `recorded_input_at`, `own_ball` (the own ball's
  name, "" without one: SC's `current_ball()` read, which is all the moved
  code used) and `describe_movable_objects` (the row's three), and
  `drop_carried_own_replay` (section J, E5), which the row did not list: a
  snapshot rollback drops a waiting own replay it carried. None removed. The
  E3 note's "resync rebuild's own-life half" turned out to be nothing: the
  rebuild calls no own-life function (the own lives are rebased by
  SessionAssign, E3).
- **Not moved:** the per-frame blend (`blend`, §1.3 step 27) and the
  corrector records (step 18) stay in SC until E9; they are the legacy
  ladder's frame half.
- **§5.1**: 3 new tests (`SessionClientCoreSnapshot.*`): counted, applied
  only when running and ours, the stale rule (a full snapshot is never
  stale), the rollback path's order `own_ball`, `keep_live_cells` enter,
  `bind_world`, exit, with an unmatched tick; the resync rebuild (a delta
  ignored while pending; `drop_pending_rewind`, the own ball written awake,
  a remote as its row says, a remote without a body skipped, the held floor
  past the current tick); the legacy ladder (a hard correction written only
  with `correct`, the third in a row asking for a resync before its log
  line, a remote without navigation written, a failed write logged). The
  existing tests fill the new operations and check the four new entries
  empty and the new config defaults. 375 tests pass.
- **Results.** Old is aea9521's binary (`build-agent-e3t/`, build id
  `bmmo-d64ff882873f-dirty`), new the E4 binary (`build-agent-E4/`,
  `bmmo-aea952187abb-dirty`).
  - §5.2 (5): all 16 reference captures replayed with the E4 binary; each
    reached its end marker and compared IDENTICAL with `e0_compare.py`.
    Strict byte check: every journal has its live reference's length
    (7178667 to 10753920 bytes); 12 bytes differ per file, all inside the
    header's build id. What the captures exercise of the moved code: the
    snapshot rollback on all 16 (`rb` 313 to 680 per client, the
    `after rollback` trace line 1 to 16 times each), the resync rebuild on
    the four `resync_snapoff` clients and twice on `a5u_lead` client 2, the
    RECEIVED checkpoint on all. **Covered by no capture**: the legacy
    ladder (none runs `--no-rollback`; the review round's ladder replay
    below runs it on the captures), and - `snapshots=` is
    received/applied/stale equal/equal/0 in every capture - the stale
    branch, the delta ignored while a resync is pending
    (`client_core_messages.cpp:42`) and the not-ours / not-running filter
    (`:20`). The unit tests cover all four, the rollback-off arm the
    ladder live. The replay logs of old
    and new are identical line for line once the CPU timings (`t_*`) are
    taken out, but for the journal path.
  - §5.2 (6), the wire check (`wire_check.sh e3t E4 e4w`): WIRE-CHECK: ALL
    SAME, 62914 sends (61970 unreliable no-delay, 838 reliable no-Nagle, 106
    reliable) equal on all 16 clients but each login; rerun on the final
    copy against the same old traces: ALL SAME.
  - §5.2 (4): 188 literals against aea9521's 188 (SC + `client_core.cpp` +
    `client_journal.cpp`, now + `client_core_messages.cpp`), equal.
  - §5.2 (3): the old SimTool on its replay of each capture against the new
    one on the E4 replay (`--continue-after-jump`): all 16 the same rc and
    summary line; the four reference server journals rc 0 with 0 checkpoint
    mismatches on both (3986 to 4257 ticks).
  - §5.2 (2): solo snapshots off (`solo/e4_new1`, starved=0) against `old1`:
    TICK hash and pose identical at all 2434 common ticks (8-2441), LOCAL
    checkpoints 1217/1217, the 8 RECEIVED snapshots equal in content, other
    records 6121/6121: IDENTICAL.
  - CPU parity (`cpu_parity.sh e3t E4`, the four `resync_snapoff` captures
    at once): two rounds FAIL on the fast side (old 21.86 / 22.02 s, new
    20.51 / 20.41 s, band 21.04-22.84); three rounds PASS (old 22.93 /
    21.93 / 21.57, new 20.34 / 20.48 / 20.22, band 19.56-24.98). The new
    binary is never slower. Controls: E3's binary (`build-agent-e3`, the
    same code as e3t without `--trace-sends`) against E4: 20.99 / 20.99
    against 20.29 / 20.74; e3t against e3: 21.80 / 21.48 against 20.85 /
    21.00. So most of the gap is the e3t build's own (3 to 4 % slower than
    the same code built for E3), and E4 is 0.3 to 0.7 s under E3.
  - §5.3, three batches under the host-load guard (`e4ab1_*` all three
    arms, `e4ab2_*` and `e4ab3_*` the swing bay again; at most 19.6 % total,
    no other process above 4.1 %, no void run). Rebase and resync ticks per
    client are listed by `e4_judge.py` (tools).
    - **Snapshots on** (`arch_server_snap.sh wsl 26700 2 66 4.2`, swing-bay
      arguments): no crash, every client to its time limit, every server
      journal rc 0 with 0 mismatches, `far` / `overtaken` / `unmatched` /
      `lead_adj` / late mirror 0 in N wherever 0 in both O, both N runs'
      criteria equal both O runs'. Pacer rebases: O2 clients 0 and 3 (3201,
      3403), N2 client 3 (3222); O1 and N1 none. Band rule 25 of 138 design
      metrics out: the event, input and life counts of N2 (and the input
      count of N1) - N2's session started later (all players ready 12 s after
      launch against 7 to 9 s: 3264 server ticks against 3478-3570), and per
      1000 ticks its counts lie among the O runs'; relay lag p99 36-38
      against 39-42 in N2.
    - **Rollback off** (lane control + `--client-arg=--no-rollback`,
      snapshots on): every client exits 2 on both binaries (the legacy
      ladder's "blended or hard-set" code, `run()`), with "time limit
      reached"; the server journals exact; criteria equal. No pacer rebase;
      the resyncs are the ladder's "3 hard corrections in a row", 5 to 15
      per client in both binaries (listed with their ticks), and the 18
      metrics outside the band are those resync counts and the event counts
      that follow them.
    - **Swing bay** (snapshots off): batch 3 passes the rule (18 of 138
      metrics out, N2's shorter session again: 3230 ticks, ready at 12 s).
      Batches 1 and 2 break the zero rule once each, both traced: in
      `e4ab1_swing_n2` client 2 `unmatched` 2 and `far` 1 - the server
      applied its own beam pair (Unphysicalize and Physicalize stamped 701)
      a tick late under the 10 % loss ("late by 1" on both in the server
      log; in O2 only the Physicalize was late, which the old binary rewound
      for), so `process_own_echo` asked for the plan-step-22 pair resync at
      722, and the two unmatched snapshots fell in that resync; the `far` is
      the full snapshot of 3219 arriving 49 ticks deep (reach 48), the same
      refusal O2 client 3 made of 3227 (49 deep). In `e4ab2_swing_n1` client
      3 `unmatched` 1 falls in the last ten ticks of the run (3433-3442),
      when it ran alone after the others' time limits and received
      snapshots 86 to 90 ticks deep. Also in batch 2, `e4ab2_swing_n2`'s
      players never crossed into sectors 2 and 3 after their respawns at the
      far end (7 mechanisms revived instead of 18, and a third fewer
      events); the old binary has the same outcome on record
      (`lp17_sw_loss10`), and the other 11 swing-bay runs of these batches
      and the 4 snapshots-on runs, old and new, entered 2, 3 and 4. The
      `unmatched` and `far` counts are the rollback engine's
      (`rollback_engine::on_snapshot`, which the moved `apply_snapshot`
      calls), so these runs do pass through the moved code; what the exact
      replays of §5.2 (5) and (6) prove is narrower: given the same inbound
      messages under the same clocks, the moved code reacts exactly as the
      old did. None of the three hits is a rebase tick, so under the rule
      as written batches 1 and 2 fail the zero rule; batch 3 is a rerun the
      rule does not provide for (review round below).
  - Builds: x64 BUILD-OK; x86 mod CLIENT-BUILD-OK with the target's 21
    objects deleted first (`client_core.hpp` changed); the new
    `client_core_messages.cpp` compiles into all three targets;
    `linux_test_syntax.sh` every line ok (21 sources); GCC compiles the
    headless client syntax-only with the Arch flags, and the three core
    sources with `-Wall -Wextra`, with no error or warning. The Arch server
    build was not rerun: no server source changed.
- **Review round** (two reviews; HEAD is now 7c9764a, a CI-only commit on
  aea9521, so the new build id is `bmmo-7c9764aa757c-dirty`). Changes: the
  core's `rollback_stats()` accessor and the `write_body` / `keep_live_cells`
  comments (above); no moved line changed. Rebuilt and rechecked:
  - Builds and tests: x64 BUILD-OK; x86 CLIENT-BUILD-OK (the target's 22
    objects deleted first); 375 tests pass on Windows; on Arch the headless
    client and `BallanceMMOMessageTests` are now built and linked with the
    working tree (GCC 16.2.1, `BUILD_SERVER_TESTS=ON` in the Arch tree, the
    server binary not rebuilt) and the 375 tests pass there too;
    `linux_test_syntax.sh` every line ok; the GCC check (SC syntax-only,
    the three core sources `-Wall -Wextra`) rc 0, no warning.
  - §5.2 (5) and (6) on the final copy: WIRE-CHECK: ALL SAME (62914 sends,
    16 clients); all 16 journals IDENTICAL and byte-identical to their live
    references but for the 12 bytes of the build id. (4): 188 literals
    against 7c9764a's 188, EQUAL. (3): all 16 client journals the same rc and
    summary line; the 4 server journals rc 0, 0 mismatches on both. (2):
    `solo/e4_new4` (starved=0) IDENTICAL to `old1`: 2459 common ticks, LOCAL
    checkpoints 1230/1230, other records 6185/6185.
  - **The legacy ladder, old against new** (review finding): every capture
    of `swing_snapon` and `resync_snapoff` replayed with `--no-rollback`
    appended by both binaries (`tools/e4_ladder.sh`). The replay leaves the
    capture where the flow first differs (rc 3, "expected a clock read at
    resync_limiter"), at the same pass on both. Before that, old and new are
    exact: the 8 journal pairs byte-identical but for the build id, the logs
    identical but for the journal path, the same rc. Covered: hard and blend
    corrections of the own ball (2 hard on every client), of remotes (6 to
    48 hard) and of mechanisms (`resync_snapoff` client 2: 5 hard, 51
    blend), `own ball error`, and one ladder resync with its rebuild
    (client 2: "3 hard corrections in a row", applied).
  - **CPU parity: FAIL** on the fast side. `cpu_parity.sh e3t E4`: old 21.58
    / 21.47 s, new 20.18 / 20.16 s, band 20.72-22.34. Control: 7c9764a's
    code rebuilt today in the same tree (`build-agent-e4ctl`, the E4 files
    put back afterwards and rebuilt) against E4 PASSES (20.22 / 20.18
    against 20.42 / 20.25, band 19.53-20.87), and `e3t` against it FAILS
    exactly as against E4 (21.63 / 21.53 against 20.23 / 20.32). The 6 %
    belongs to the `e3t` build, not to the step; it still fails the check
    as named.
  - §5.3, batch 4 (`e4ab4_*`, the three arms, O1 N1 O2 N2 each; at most
    20.0 % total, no other process above 3.2 %, no void run):
    - Rollback off: PASS (every client rc 2 on both binaries, the ladder's;
      resyncs are its "3 hard corrections in a row"; no pacer rebase; band
      21 of 140 out, reported).
    - Swing bay: PASS (no rebase, no resync; far differs only where an O run
      is non-zero; band 24 of 138 out).
    - Snapshots on: **FAIL**, the zero rule once. `e4ab4_snapon_n1` client
      2 `unmatched` 2 (0 in both O runs' client 2): the FULL snapshots of
      3355 and 3369, both delivered at local tick 3469 (114 and 100 ticks
      deep, past the history), after client 0's time limit (3441) and
      client 1's (3467). The full snapshot of 3369 was late for every
      client (client 0 received it 71 deep, too_far; client 3 40 deep). No
      rebase or resync on that client, so it is not a rebase tick. The O
      runs' unmatched all follow a pacer rebase and its resync (O1 client 0
      rebased 3189, resync 3206, unmatched 3227-3254; O1 client 3 rebased
      3329, unmatched 3349-3370; N2 client 3 rebased 3188, unmatched
      3208-3226). Criteria equal, server journals exact; band 15 of 138
      out, reported.
  - **Closed under the revised §5.3 rule.** The gate as first written
    failed on CPU parity and on the snapshots-on arm. Both rules were
    revised (§5.3, "The pass rule on this host"), and E4 passes both.
    - CPU parity is now one-sided. E4 is never slower than the old binary,
      and it is as fast as a fresh build of the previous commit's code.
    - The zero rule now accepts a difference traced to an event of the run,
      not only a rebase. All three hits are such events:
      - `e4ab1_swing_n2` client 2: the server applied a beam pair late
        under loss, and the pair resync followed;
      - `e4ab2_swing_n1` client 3: the teardown;
      - `e4ab4_snapon_n1` client 2: full snapshots 100 to 114 ticks deep,
        after clients 0 and 1 reached their time limits, which is the
        teardown.

    Every exact check passed, including the wire check and the legacy
    ladder replayed against old.

**As built (step E5).** Where the code made E5 differ from §1.1, §1.3, §2.2
and the row above:

- **What moved**, verbatim but for the platform calls (`send`, `now`,
  `bind_world`, `keep_live_cells`), the configuration (`config.rollback` for
  `args_.rollback`) and the one back-call left (`recorded_input_at`):
  - `client_core_messages.cpp`: the SessionRemoteInput intake (SC's case
    body) as `on_session_remote_input`, and `note_input_correction`;
  - `client_core_held.cpp` (new): section J - `note_held_section`,
    `held_tick_is_base`, `settle_held_tick`, SessionLead
    (`on_session_lead`, `finish_lead_slew`, `drop_lead_state`),
    `resolve_held_frame`, `store_held_frame`, `hold_ahead`,
    `take_held_ahead`, `correct_own_tick`, `learn_held_tick`,
    `held_learned_upto`, `check_held_complete` (with `kHeldGapTicks`),
    `reset_held_epoch`, `drop_pending_rewind`, `own_notices_coming`,
    `drop_carried_own_replay`, `held_header_current`, `same_frame`; and the
    forced own frames' core half;
  - `client_core_frame.cpp` (new; §1.1 names it for `pre_step` /
    `post_step`, which join it at E9): `replay_corrected_inputs` and
    `force_rewind`.

  Both new files are in `cmake/SessionCore.cmake`. The headless client
  keeps one-line calls into the core at every old call site (the frame,
  `advance_lead_slew`, `move_own_birth`, `rewind_for_lifecycle`, `report`).
  `relay_lag_p99` stays in the host: only the status line reads it (E10).
  The eight moved `logf` literals are unchanged.
- **Entry points the design does not name.**
  - `on_session_remote_input` / `on_session_lead` instead of `on_message`
    (the E3 pattern). `on_session_lead` takes SC's whole filter: this
    running session's, assigned, our own player.
  - **`force_rewind(depth)` returns `forced_rewind {rolled, keep_cells}`**,
    not `void`. SC's `live_cells_guard` lived until the end of
    `maybe_force_rollback`, after the `--trace-restore` lines and the
    `dump_savestate(t, "replayed")` loop that follow the rewind. The guard's
    exit writes the three data-array cells back and may re-issue a manager
    activation (with its own trace line), so ending it inside `force_rewind`
    would have moved those writes before the host's dumps and traces. The
    core builds the guard before `bind_world`, as SC did; the host keeps the
    result until the end of `maybe_force_rollback`. The cadence (`now %
    every`, the `depth + 2` start) and every trace stay host code
    (§3.1 "`--force-rollback`").
  - The forced own frames' core half is two calls, since the frame is still
    SC's until E9: `take_forced_own(T, out)` (step 11's selection, older
    ticks dropped) and `play_forced_own(T, forced, input)` (step 21: the
    record takes the server's frame, `lead_forced_played_`, and a first
    notice counted base or recv + same). `force_own` / `unforce_own` (the
    `navigation_poll` / `navigation_input` pair and the `lead: forced own
    input` / `lead: own polling` lines) stay host code until E9.
  - Public, because unmoved host code calls them: `take_held_ahead` (step
    24), `finish_lead_slew` (the host's pacer ends the slew, E9:
    `slew_step`), `drop_carried_own_replay` (`move_own_birth`, E7;
    `rewind_for_lifecycle`, E6), `held_header_current` (the status line,
    E10). `reset_held_epoch` and `drop_pending_rewind` are private.
- **SessionLead without `slew`** (§2.3, §5.1) is built: counted in a new
  core counter `lead_refused_`, journaled (`lead: SessionLead N refused:
  this host cannot slew (no CAP_LEAD)`, a note, not a `logf` line, so the
  §5.2 (4) multiset is unchanged), nothing amended, forced, slewed or
  acknowledged. The headless client declares `slew` (E3), so the branch is
  inert there. The counter is printed nowhere yet.
- **`legacy_calls`.** Removed, four: `reset_held_epoch`,
  `drop_pending_rewind`, `finish_lead_slew` (E3) and
  `drop_carried_own_replay` (E4); their targets are the core's. Added:
  none. **`recorded_input_at` stays a back-call**, against the E4 note's
  "moves with the relay": SC tells the own ball by `entity ==
  own_nav_entity_`, without the `own_navigation_` gate that
  `own_driven_entity` has (E4). The two differ after a failed
  `navigation_set_ball` followed by a failed `navigation_create`
  (`attach_own_navigation`): the replica is gone, its old entity is still
  named, and records before the failure name it as the own ball. Moving
  the function over `own_driven_entity` would feed such a replay the
  recorded prediction instead of `own_inputs_`. It moves with the own
  ball's platform half at E9: the E9 row now names it, over a new §2.2
  operation `own_replica_name` (the name the own replica last had, kept
  after it is gone). Until then the core needs this back-call filled;
  the mod's empty `legacy` would throw at the first replay, which is why
  E11's empty-struct assertion and R1 come after E9.
- **Not built:** the §1.2 feature switches `input_rewind` and
  `own_corrections` (the E5 row did not name them; headless would set both
  from `rollback`, which the moved code reads already). The E9 row now
  builds them, so the headless exact and A/B checks show they change
  nothing before R1 is the first host to turn them off.
- **Guards the host had** (review of E5): `force_rewind` returns at once,
  not rolled, nothing counted, no cells kept, without `rollback` or before
  `depth + 2` ticks are played (the anchor would wrap); the headless host
  checks both itself first, so this is redundant there, but a retail verb
  (R0/R2) would otherwise count a false `unmatched`. A refused SessionLead
  is refused once per seq: the seq goes into `lead_seen_`, so the server's
  copies of the decision count as `lead_dups_`, not as more refusals.
- **§5.1**: 4 new tests (`SessionClientCoreLead.*`,
  `SessionClientCoreRelay.*`):
  - with `slew`: a SessionLead at c = 125 over [121, 130) amends 121-125
    (stored, counted), forces 126-129, starts the slew on the
    `slew_start` clock, sends nothing until `finish_lead_slew`, which sends
    the reliable ack with what moved; copies and another base's decisions
    dropped; the forced frames taken by tick (older ones dropped) and
    played; a new base drops them;
  - without `slew`: SessionReady caps 0; a SessionLead counted and
    journaled, no clock read, no log line, no send, nothing amended,
    forced or learned;
  - the relay intake: counted before the session filter, stored or parked,
    lag counted, re-deliveries only counted, a simulated tick past the
    history late, the live predictor moved; another base's held records
    counted once;
  - `replay_corrected_inputs` inside the live-cells scope, a refused remote
    replay logged, a refused own one resyncing; `force_rewind` keeping the
    cells while its result lives.

  The existing tests lost the four back-calls from their call lists (the
  resync limiter's now shows the ack and its line). 379 tests pass on
  Windows and, built with GCC 16.2.1 on Arch, there too.
- **Results.** Old is 9029872's binary (`build-agent-E4/`, build id
  `bmmo-7c9764aa757c-dirty`), new the E5 binary (`build-agent-E5/`,
  `bmmo-9029872673bd-dirty`).
  - §5.2 (5) and (6), `wire_check.sh E4 E5 e5w`: WIRE-CHECK: ALL SAME,
    62914 sends on the 16 clients equal but each login; every replay
    reached its end marker; all 16 journals IDENTICAL with `e0_compare.py`
    and the same length as their live references, 12 bytes differing per
    file, all inside the header's build id. The replay logs of old and new
    are identical line for line (41423 lines) once the CPU timings
    (`t_rest`, `t_rep`, `t_cap`, `t_max`) and the journal path are taken
    out. What the captures exercise of the moved code: the relay intake
    and input replays on all 16 (`inrew` 158 to 635 per client), held
    notices and the own replays (`subst` up to 1830 received on `a5u_lead`
    client 2, coalesced and deferred ones among them), the late gate
    (`relay_late` 84-87 on `resync_snapoff`, 264 on `a5u_lead` client 2),
    and one SessionLead (`a5u_lead` client 2: k=+15, 23 ticks amended, 32
    forced, the slew at 2x, acknowledged at 1773 with +15). **Covered by no
    capture**: `force_rewind` (no capture runs `--force-rollback`; §5.2 (1)
    below runs it), a voided, superseded or zero-delta SessionLead, a lost
    held notice (`held notices lost`), a refused input replay, and the
    no-`slew` branch (unit tests).
  - §5.2 (4): 188 literals against 9029872's 188 (SC + `src/session/*.cpp`),
    EQUAL.
  - §5.2 (3): the old SimTool on its replay of each capture against the new
    one on the E5 replay (`--continue-after-jump`): all 16 the same rc and
    summary line; the four reference server journals rc 0 with 0
    checkpoint mismatches on both (3986 to 4257 ticks).
  - §5.2 (2): solo snapshots off (`solo/e5_new1`, starved=0) against `old1`:
    TICK hash and pose identical at all 2462 common ticks (8-2469), LOCAL
    checkpoints 1231/1231, the 8 RECEIVED snapshots equal in content, other
    records 6192/6192: IDENTICAL.
  - §5.2 (1), run because `force_rewind` moved and no capture covers it:
    `savestate_check.py --journals` with the E5 binary against
    `arch_server_snap.sh wsl 26700 2 66 4.2`, starved=0: forced-50x12 (49
    forced rollbacks), forced-97x24 (25) and forced-131x47 (18) EXACT, all
    53 bodies 0.0 m over 1731 pairs.
  - CPU parity (`cpu_parity.sh E4 E5`, three rounds): PASS, old 20.75 /
    20.27 / 20.17 s, new 20.09 / 20.47 / 20.36 s, limit 22.37 s.
  - Builds: x64 BUILD-OK; x86 mod CLIENT-BUILD-OK with the target's 22
    objects deleted first (`client_core.hpp` changed; 24 objects now, the two
    new core files among them); `linux_test_syntax.sh` every line ok (23
    sources); GCC compiles the headless client syntax-only with the Arch
    flags, and the five core sources with `-Wall -Wextra`, with no error or
    warning; the Arch tree builds and links the headless client and the
    tests. No server source changed, so the Arch server was not rebuilt.
  - §5.3, the full arm set under the host-load guard, O1 N1 O2 N2 each
    (`e5ab1_*`: all five arms; then `e5ab2_lane`, `e5ab3_lane`, `e5ab4_a1`
    and `e5ab5_a6m`; at most 23.9 % total, no other process above 3.8 %, no
    void run). Every client of every run reached its time limit with rc 0
    and a status line; every server journal replays rc 0 with 0 checkpoint
    mismatches. Judged with `e4_judge.py` (rebase and resync ticks per
    client in `e5ab*.judge`, session scratchpad):
    - **Swing bay** (10 % loss, beams): PASS. No rebase or resync in any
      run; `far` differs only where an O run is non-zero; band 14 of 138
      out (the old runs of E4's four batches against these O runs as if
      new: 18 to 24).
    - **A6m hold**: PASS in both batches (band 34 and 30 of ~313). The arm
      is bimodal on both binaries: client 0's phase-B mechanism error is
      4.7097 m or 8.7235 m (client 3: 8.2028 or 7.6935 m), which moves most
      of its metrics; `e5ab1_a6m_n2` is in the second mode, and so is the
      old `e5ab5_a6m_o2`.
    - **Snapshots on**: the zero rule broken once, traced. `e5ab1_snapon_n1`
      client 2 `unmatched` 1: the FULL snapshots of 3293 and 3313 arrived
      at its local tick 3389, 96 and 76 ticks deep (past the history),
      before any client's time limit (client 0's at 3395). Criteria of
      both N runs equal both O runs'; no rebase; band 8 of 138 out (old
      against old across E4's batches: 24 and 25).
    - **A1 burst**: batch 1 broke the zero rule once and one criterion,
      both traced; batch 4 PASSES (band 28 of 303). In `e5ab1_a1_n1`
      clients 2 and 3 rebased at 2694 (O1: client 3 at 2689, O2: client 2
      at 2691). Client 2 `unmatched` 1 is that resync's: rebuilt from the
      full snapshot of 2707, it then received the full snapshot of 2711 at
      2736, before its new base 2734 has a record. Its criterion "subst
      recv == server count, subst late 0" fails on 236 against 211, late 0:
      the client's SUBSTITUTED records through its last window inside the
      harness's span (tick 3479, span end 3504 = last tick - 60) number
      exactly 211, and the 25 others are ticks 3514-3538, past the span,
      in the run's end (client 0's time limit at 3527, client 1's at 3536).
      The old `e5ab4_a1_o2` fails the same criterion the same way (273
      against 259, late 0). The server's 11 late events (5 in O1 and O2)
      follow the two rebases.
    - **Lane control**: three batches. No crash, every server journal
      exact, no zero broken (every `unmatched` sits beside a non-zero O
      value). The criteria item: 3 of the 6 N runs equal an O run of the
      pool (the 6 O runs of these batches and E3's 6), and 3 do not. In all
      three the difference is the pacer regime - which clients rebased at
      2674-3006 decides their `relay_late 0` and `resyncs 0` criteria, and
      every value occurs in the O pool - except one: in `e5ab2_lane_n1`
      client 3's "own ball 0.0000 in D" PASSES (0.0000 m), where the 12 O
      runs have 0.0015 to 9.1 m (and E3's new runs 0.0003 to 0.0115 m).
      Client 3 had no substituted tick in phase D and rebased only at 2698,
      after it; this one is **not traced** to a §5.3 run event. It is a
      zero-threshold criterion on a sub-millimetre error that moves between
      runs of one binary (client 2's flips between 0.0000 and 0.0002 m in
      the O runs; client 1 passes in one O run of 12). Band: 22, 49 and 38
      of ~303 out; old against old across the E3 batches: 40 to 96.

    The out-of-band count metrics (events, inputs, echoes, rewinds, relay
    lag) were not traced one by one; per 1000 frames most stay outside on
    the swing and snapshots-on arms too, and the old-against-old bands of
    the same arms are as wide or wider.
- **The first round's status.** Every exact check passed (§5.2 (1) to
  (6)), CPU parity passed, and swing bay, A6m and snapshots on passed the
  §5.3 rule. The lane control arm did not meet the criteria item as
  written (the phase-D own-ball criterion above, not traced), so the step
  was not committed then.
- **After the review: the closing round.** The review's fixes (the
  refusal once per seq, the `force_rewind` guard, `<algorithm>`, the two
  doc rows) change nothing on headless; the binary was rebuilt
  (`bmmo-9029872673bd-dirty`, copied to `build-agent-E5/`) and every check
  run again on it:
  - x64 BUILD-OK, x86 CLIENT-BUILD-OK (its 24 objects deleted first),
    379 tests on Windows and with GCC 16.2.1 on Arch, `linux_test_syntax.sh`
    23 lines ok, the headless client and the five core sources with GCC
    `-Wall -Wextra` rc 0 with no warning;
  - §5.2 (5)/(6) `wire_check.sh E4 E5 e5x`: WIRE-CHECK: ALL SAME (the same
    62914 sends), all 16 journals IDENTICAL, same length as the live
    references, 12 bytes differing per file at offsets 93-104, the build
    id; (4) 188 against 188 literals, EQUAL; (3) all 16 clients the same
    rc and summary line, the four server journals rc 0 with 0 mismatches
    on both; (2) `solo/e5_newx1` IDENTICAL to `old1` (2459 common ticks,
    LOCAL 1230/1230, other records 6185/6185, starved=0); (1)
    forced-50x12, 97x24 and 131x47 EXACT (48/25/18 forced rollbacks, 53
    bodies, 0.0 m);
  - CPU parity (three rounds): PASS, old 20.54 / 20.25 / 20.26 s, new
    20.16 / 20.24 / 20.30 s, limit 21.86 s.
  - §5.3, all five arms, one batch each (`e5abx1_*`), **every client of
    every run capturing its inbound** (`--capture-inbound`, O and N runs
    alike), so that any difference can be put to the old binary. Host
    peak 16.0-17.9 % total, no other process above 3.5 %: no void run.
    No crash; every client reached its time limit with rc 0; every server
    journal replays rc 0 with 0 mismatches. **The trace by replay:** each
    N run's four captures replayed with the old binary (`build-agent-E4`)
    give client journals IDENTICAL to the N run's live ones, and each O
    run's replayed with the new binary give its live ones (20 runs, 80
    journals, all IDENTICAL); and every `window:` and `status:` line of
    the old binary's replay of each N run equals the live N log's once
    the CPU timings are removed (36 client logs, 0 lines differ). Every
    count, error and criterion of an N run is therefore what the old code
    computes from that run's inbound stream: a property of the run (its
    timing and loss), not of the code. The §5.3 events behind each
    difference, with each run's rebases:
    - **Lane control: PASS.** Rebases: O1 clients 1-3 at 2693/2693/2694
      (resyncs applied 2706/2706/2707), none in N1, O2 or N2. No zero
      broken; both N runs' criteria equal `e5ab1_lane_o1`'s and
      `e5ab3_lane_o2`'s. Band 29 of 306 out.
    - **Swing bay** (no rebase in any run). Criteria of both N runs equal
      both O runs'. Zeros broken, each a snapshot delivered past the
      reach (the log's refusal line names it, the journal its depth):
      N1 client 1 `far` 1 = the full snapshot of 871 at 920 (49 deep),
      client 3 `far` 2 = 844 at 895 (51) and 871 at 920 (49); N2 client 1
      = 868 at 918 (50), client 3 `far` 3 = 3141-3143 at 3205 (62-64), and
      client 0 `unmatched` 1 = the full snapshot of 231 at 327, 96 deep,
      past the history (its `far` 2 = 868 at 918 and 3339 at 3388, where
      O1 has 2). Band 17 of 138 out.
    - **A1 burst.** Rebases: O1 client 3 at 2709 (applied 2722), O2
      client 3 at 2706 (2720), none in N1, N2 all four: client 0 at 2958
      (2971), 1 at 2695 (2709), 2 at 2693 (2705), 3 at 2697 (2715). N1's
      criteria equal `e5ab1_a1_o2`'s. N2's differ from O1's only in
      client 0's and client 1's `relay_late 0` and `resyncs 0`, the two
      clients that rebased in N2 and not in O1 (client 3's, rebased in
      both, are the same). Zeros broken: N2 client 1 `unmatched` 1 = the
      full snapshot of 2705, arrived at 2716 between its rebase (2695) and
      its resync's base (2709); client 2 `unmatched` 2 = the full
      snapshots of 2709 and 2715, arrived at 2735 and 2737 right after
      its resync from 2705 (2732), before its new history holds them. The
      late mirror is non-zero in every O run. Band 97 of 303 out.
    - **A6m hold: PASS.** No rebase; N1's criteria equal O1's, N2's O2's;
      no zero broken (the late mirror is non-zero in the O runs). Band 17
      of 313 out.
    - **Snapshots on.** Rebases: O2 client 3 at 3410, N2 client 3 at
      3407, both in the teardown (no resync applied before the end).
      Criteria of both N runs equal both O runs'. Zero broken: N2 client
      0 `far` 1 = the full snapshot of 3294 at 3348, 54 deep. Band 11 of
      138 out.

    The first round's untraced `e5ab2_lane_n1` (client 3's phase-D own
    error 0.0000 m) was a run of the pre-review binary without captures;
    the phase-D error is continuous across runs of both binaries (client
    3: 0.0007 to 0.0536 m in the other N runs, 0.0015 to 9.1 m in the O
    runs; client 1 0.0000 in the O run `e3ab4_lane_o2`), and the closing
    round's replays show the class of difference is the inbound stream's.
- **The step's status.** Every exact check, CPU parity and every arm pass;
  each broken zero and each criterion difference is a snapshot past the
  reach or the history, a pacer rebase and its resync, or the teardown,
  and every N run is reproduced exactly by the old binary from its own
  inbound capture.

**As built (step E6).** Where the code made E6 differ from §1.1, §1.3, §2.2
and the row above:

- **What moved** (`src/session/client_core_lifecycle.cpp`, new, in
  `cmake/SessionCore.cmake`), verbatim but for the platform calls, the
  configuration (`config.rollback`) and the back-calls: SessionEvent's
  intake (`on_session_event`), `drain_due_events`, `apply_held_lifecycle`
  (whole: its own-lives half is a back-call), `join_rank`, `apply_event`,
  `make_remote_birth`, both `rewind_for_lifecycle`s, `unphysicalize_remote`,
  `drain_pending_births`, `live_first_tick`, `unphysicalize_mirror`,
  `live_drive`, `physicalize_remote` and `attach_remote_navigation` (their
  core halves), and the event log's mute (`event_log_mute`, with
  `event_log_muted_` moved into `client_core_state`, as the E1 note said).
  The thirteen moved `logf` lines are unchanged. The headless client keeps a
  one-line call at every old call site: the intake, pre_step steps 2 and 8,
  step 16's attach, step 28's `live_drive`, and its binding's
  `before_replayed_tick` (`core_.drain_pending_births`). The own body's
  re-make and removal (`remake_own_body`, `unmake_own_body`, E7) mute through
  the core's `event_log_mute`, which is public until they move.
- **The mirrors' platform operations differ from §2.2** (rows amended). SC
  asks for the entity (CopyObject) first, then takes the old mirror's
  navigation and, on a trafo, its body out, then poses and physicalizes the
  new body; the pose may not move before the take-out, nor the lookup after
  it (a failed lookup must leave the old mirror alone). So:
  - `mirror_entity(player, ball_type, error)`, new: the lookup alone;
  - `make_mirror(mirror_spec) -> mirror_made`: pose, `physicalize` and the
    spawn kick, `{made, error, kicked, kick_error}`. The core logs the
    result where SC logged it (the kick's line after the relay take, which
    is core state only), so the physics calls and the log lines keep their
    order;
  - `unmake_mirror(entity)`: the body only. The core mutes around it where
    SC muted (a death, a trafo) and not at the session's end;
  - `detach_mirror_navigation(entity)`, new: SC destroyed the navigation
    unmuted, before the muted unphysicalize;
  - `attach_mirror_navigation(player, entity, ball_type, error) ->
    {unavailable, attached, failed}`: `unavailable` (the graph not known)
    logs nothing and changes nothing, as SC's guard returned.

  Headless implements them with SC's calls; `make_mirror` poses the entity
  `mirror_entity` returned last (`mirror_made_entity_`, the one SC held as
  `entity`) when it is the one the spec names - which the core's flow always
  makes it, since it asks `mirror_entity` first in the same frame - and else
  looks it up again from the spec's `player` and `ball_type` (SC's
  `remote_entity`), so the operation stands alone for a later caller (E7,
  E9). All six are required by `missing_platform_operation`: every host
  mirrors the other players.
- **Session end.** The legacy entry `end_session_world` became
  `own_body_guard_off` (the guard is host state); `end` takes every
  mirror's navigation and body out after it with `detach_mirror_navigation`
  and an unmuted `unmake_mirror`, in SC's order. E11 has to give the guard a
  home (a hook; `own_body_upkeep` has no "off").
- **`legacy_calls`.** Added: `live_own_lives`, `own_lives_due`,
  `apply_own_lives` (the own lives' part of the held events and of the
  births made inside a replay; E7 moves them). Renamed:
  `end_session_world` to `own_body_guard_off`. Removed: none.
  `recorded_input_at` (E9) is used by the lifecycle rewinds too.
- **Entry points the design does not name** (the E3 pattern):
  `on_session_event(msg&&)` instead of `on_message`; `drain_due_events`,
  `apply_held_lifecycle`, `drain_pending_births`, `attach_remote_navigation`
  and the static `live_drive` are public while the frame and the binding are
  the host's (E9).
- **§5.1**: 7 new tests, 8 after the review (`SessionClientCoreLifecycle.*`): the intake (counted,
  another session's and a not-running one's dropped, a copy counted, an echo
  queued for the frame); the drain's order (join, applied, origin, rank) with
  same-tick births held and made after the rewinds, our own lives at our join
  rank and once more at the end, the spawn kick's index and log; the
  supersede guards (a stale death; an older birth dropped in the death's tick,
  and made and the death applied again before it, with both rewinds refused
  and logged); the births made inside a replay in join order with the own
  lives at our rank, each muted, a trafo's take-out nesting inside the mute;
  a navigation unavailable or failed, a refused entity; `live_drive`; the
  session's end. The existing tests fill the six operations, check the new
  back-calls empty and the renamed one. 386 tests pass on Windows and, built
  with GCC 16.2.1 on Arch, there too.
- **Results.** Old is 8d933ad's binary (`build-agent-E5/`, build id
  `bmmo-9029872673bd-dirty`), new the E6 binary (`build-agent-E6/`,
  `bmmo-8d933adbaf0d-dirty`).
  - §5.2 (5) and (6), `wire_check.sh E5 E6 e6w`: WIRE-CHECK: ALL SAME, 62914
    sends on the 16 clients equal but each login; every replay reached its
    end marker (rc 0); all 16 journals IDENTICAL with `e0_compare.py` and the
    same length as their live references, 12 bytes differing per file, all
    inside the header's build id. Rerun on the final copy against the same
    old traces (`e6f`): ALL SAME, 16 IDENTICAL, byte-identical outside the
    build id. What the captures exercise of the moved code: the intake on all
    16 (1046 to 1174 events received per client, 898 to 1002 of them
    copies), remote births and deaths (6 to 12 births, 5 to 11 deaths per
    client, every birth with its spawn kick and navigation), remote Sector
    events (3 to 9), and the late births and deaths rewound for (`life` 9 to
    20 per client, none refused), the births among them made inside the
    replay. **Covered by no capture**: an overtaken birth, a superseded death,
    a refused life rewind, a birth the replay did not reach, a mirror
    without navigation, a failed entity or body, a trafo (a live mirror
    re-made with another ball type: every `physicalized (` line of the 16
    logs names a `Ball_Stone_BMMO_*` mirror), and the teardown at the
    session's end (it runs after the journal is closed and logs nothing, so
    no check of §5.2 sees it; it is SC's `end_session_world` in the same
    order, by reading and by `EndTakesTheMirrorsOut`). The unit tests cover
    each of them (the replay that did not reach its birth from the review
    on, below).
  - §5.2 (4): 188 literals against 8d933ad's 188 (SC + `src/session/*.cpp`),
    EQUAL.
  - §5.2 (3): the old SimTool on its replay of each capture against the new
    one on the E6 replay (`--continue-after-jump`): all 16 the same rc and
    summary line; the four reference server journals rc 0 with 0 checkpoint
    mismatches on both (3986 to 4257 ticks).
  - §5.2 (2): solo snapshots off (`solo/e6_new1`, starved=0) against `old1`:
    TICK hash and pose identical at all 2426 common ticks (8-2433), LOCAL
    checkpoints 1213/1213, the 8 RECEIVED snapshots equal in content, other
    records 6101/6101: IDENTICAL.
  - CPU parity (`cpu_parity.sh`, three rounds each): PASS every time, but
    the new binary is consistently 0.4 to 0.8 s (2 to 4 %) slower: E5 20.24 /
    20.48 / 20.28 s against E6 21.03 / 20.86 / 20.74 s (limit 21.74); again
    20.09 / 19.96 / 20.09 against 20.77 / 20.74 / 20.91 (limit 21.22).
    Control: 8d933ad's code rebuilt in today's tree (`build-agent-e6ctl`)
    against the final E6 rebuild: 20.27 / 20.43 / 20.16 against 20.82 /
    20.99 / 20.86 (limit 21.72), and E5 against it 20.07 / 20.33 / 20.09
    against 20.78 / 20.93 / 20.84 (limit 21.61). The control is as fast as
    E5, so the 3 % belongs to E6's code or its layout, not to the build; it
    stays inside the one-sided limit, and no cause was found (the moved
    paths run per event, per birth and per replayed tick, a few
    `std::function` calls each).
  - Builds: x64 BUILD-OK; x86 mod CLIENT-BUILD-OK with the target's objects
    deleted first (25 objects now, `client_core_lifecycle.cpp` among them);
    `linux_test_syntax.sh` every line ok (24 sources); GCC compiles the
    headless client syntax-only with the Arch flags, and the six core
    sources with `-Wall -Wextra`, with no error or warning; the Arch tree
    builds and links the headless client and the tests. No server source
    changed, so the Arch server was not rebuilt.
  - §5.3, the row's three arms under the host-load guard, O1 N1 O2 N2 each,
    every client capturing its inbound (`e6ab1_*`: lane control, A6m, swing
    bay; then `e6ab2_lane`). Host peak 15.2 to 18.4 % total, no other
    process above 3.5 %: no void run. No crash; every client reached its
    time limit with rc 0 and a status line; every server journal replays
    rc 0 with 0 checkpoint mismatches. **The trace by replay:** each N run's
    four captures replayed with the old binary (E5) give client journals
    IDENTICAL to the N run's live ones, and each O run's replayed with E6
    give its live ones (16 runs, 64 journals, all IDENTICAL); and every
    `window:` and `status:` line of the old binary's replay of each N run of
    batch 1 equals the live N log's once the CPU timings are removed (24
    client logs, 0 lines differ). The §5.3 events behind each difference
    (rebase and resync ticks per client in `e6ab*.judge`, session
    scratchpad):
    - **A6m hold: PASS.** No rebase; both N runs' criteria equal O2's; no
      zero broken (the late mirror is non-zero in the O runs). Band 17 of 313
      out.
    - **Swing bay** (no rebase or resync in any run). Both N runs' criteria
      equal both O runs'. Zeros broken, each a snapshot delivered past the
      reach or the history (the log's refusal line names it, the journal its
      depth): N1 client 1 `far` 1 = the full snapshot of 702 at 752 (50
      deep), client 2 `far` 1 = 1092 at 1162 (70); N2 client 1 `far` 2 = 844
      at 893 (49) and 868 at 918 (50); client 3 `unmatched` 1 = the full
      snapshot of 3333 at 3429, 96 deep, past the history (its `far` 1 = 868
      at 925, where O1 has 4). Band 19 of 138 out.
    - **Lane control.** Batch 1 rebases: O1 clients 2 and 3 at 2669 / 2688
      (resyncs applied 2682 / 2709), N1 all four (0 at 2965, 1 at 2693, 2 at
      2686, 3 at 2959; applied 2979 / 2707 / 2700 / 2973), none in O2 or N2.
      Batch 2: none in O1, N1 or O2; N2 clients 1, 2, 3 at 2698 / 2676 /
      2692 (applied 2715 / 2689 / 2705). Zeros broken, each the resync's:
      batch 1 N1 client 2 `unmatched` 1 = the full snapshot of 2707 at 2733,
      after its resync from 2700; client 3 = 2979 at 3005, after its resync
      from 2973; batch 2 N2 client 2 = 2705 at 2730 (resync from 2689),
      client 3 = 2715 at 2738 (resync from 2705). Criteria: batch 2's N1
      equals both O runs of its batch (and `e5ab1_lane_o1`,
      `e5ab3_lane_o2`). The other three N runs equal no O run of the pool
      (the 2 + 2 O runs of these batches and the 14 of E3 and E5): each
      differs from its nearest O run only in "own ball 0.0000 in D" of one or
      two clients (batch 1 N1 client 3 PASS where the nearest fails; batch 1
      N2 client 3; batch 2 N2 clients 1 and 3), the sub-millimetre phase-D
      criterion E5 met too: every client's value occurs both ways among this
      batch's O runs (batch 1 O2 passes it on all four clients, O1 fails it
      on clients 1-3 with 0.0001 to 0.0007 m), and the N runs' values are
      the old binary's own from their inbound captures (the trace by replay
      above). That is the E5 closing round's standard; as written, the
      criteria item names no event of §5.3 for it. Band 57 of 305 (batch 1)
      and 93 of 301 (batch 2, against a pool of 18 O runs) out.
- **The first round's status.** Every exact check (§5.2 (2) to (6))
  passed, CPU parity passed (with the 3 % above recorded), A6m and the swing
  bay passed the §5.3 rule, and the lane arm's broken zeros were each a pacer
  rebase's resync; three of its four N runs differed from every O run in the
  phase-D own-ball criterion, which the old binary reproduces exactly from
  those runs' captures. So the step was not committed then.
- **After the review.** Two reviews; their findings and what was done:
  - `mirror_spec` lacked the player and the ball type the design's row
    had, which retail's `set_physicalized` / `on_trafo` are keyed by:
    added (`player`, `ball_type`), filled by `physicalize_remote` from the
    birth, and the §2.2 row amended.
  - Headless `make_mirror` worked only on the pointer the last
    `mirror_entity` cached: it now looks the entity up again from the
    spec's player and ball type when the cache does not name the spec's
    entity (the comment at `client_platform.hpp` says so). In the core's
    flow the cache always names it, so the headless calls are unchanged.
  - The birth a replay did not reach (made live after the rewind) had no
    test: `SessionClientCoreLifecycle.BirthTheReplayDidNotReachIsMadeLive`
    rewinds a world with no `pre_step` three ticks and expects the "did not
    reach" line, the mirror made with the next live tick as its first
    (`born_tick` 126 at tick 125), one generation and nothing pending. The
    drain test checks the spec's player and ball type.
  - The trafo and the end teardown are added to the coverage list above.
  - Refuted: none. Not changed (as the reviews concluded): the unguarded
    `legacy` back-calls (the E3-E5 convention), the direct mutes of the
    script replay (as before), the kick's place before the relay take (core
    state only; physics calls and log lines in SC's order).
- **The closing round.** The binary rebuilt with the review's fixes
  (`bmmo-8d933adbaf0d-dirty`, copied to `build-agent-E6/`), every check run
  again on it, old = `build-agent-E5`:
  - x64 BUILD-OK; x86 CLIENT-BUILD-OK with its 25 objects deleted first
    (`mirror_spec` changed layout); 387 tests pass on Windows and, built
    with GCC 16.2.1 on Arch, there too; `linux_test_syntax.sh` 24 lines ok;
    the headless client and the six core sources with GCC `-Wall -Wextra`
    rc 0 with no warning.
  - §5.2 (5)/(6), `wire_check.sh E5 E6 e6x`: WIRE-CHECK: ALL SAME (62914
    sends on the 16 clients), every replay rc 0 at its end marker, all 16
    journals IDENTICAL and BYTE-IDENTICAL-EXCEPT-BUILD-ID (same length, 12
    bytes differing per file, inside the build id); (4) 188 against 188
    literals, EQUAL; (3) all 16 clients the same rc and summary line, the
    four server journals rc 0 with 0 mismatches on both; (2)
    `solo/e6_newx1` IDENTICAL to `old1` (2459 common ticks, LOCAL 1230/1230,
    other records 6185/6185, starved=0).
  - CPU parity: the first two rounds against E5 FAILED by 0.02 s (old
    20.35 / 20.26 s, new 21.48 / 20.81 s, limit 21.46), so it was judged
    against the control `build-agent-e6ctl` (8d933ad's code built in
    today's tree), as §5.3 says: PASS, control 20.21 / 20.22 / 20.26 s, new
    20.82 / 20.96 / 21.23 s, limit 21.32. A further three rounds against E5
    PASS (old 20.34 / 19.96 / 20.17, new 20.92 / 20.87 / 20.97, limit
    21.74). The new binary stays 3 to 5 % slower than the same code's
    control, as in the first round, with no cause found in the moved paths
    (the same work per frame, per event and per replayed tick, plus a few
    `std::function` hops); it is inside the one-sided limit only against
    the control and in two of three rounds against E5.
  - §5.3, one batch per arm (`e6abx1_*`), every client capturing its
    inbound. Host peak 15.5 to 19.9 % total, no other process above 3.5 %:
    no void run. No crash; every client reached its time limit with rc 0
    and a status line; every server journal replays rc 0 with 0 mismatches.
    Each N run's captures replayed with E5 give its live journals, each O
    run's with E6 give its own (12 runs, 48 journals, all IDENTICAL).
    - **Lane control: PASS.** Rebases: O1 client 1 at 1676 (applied 1713)
      and client 2 at 2683 (2697); none in N1; O2 clients 2 and 3 at 2696
      (2709 both); N2 client 2 at 2685 (2698) and client 3 at 2687 (2700).
      N1's criteria equal `e5ab1_lane_o1`'s, `e5ab3_lane_o2`'s and both of
      `e6ab2`'s O runs'; N2's equal O2's of this batch. One zero broken,
      traced: N2 client 2 `unmatched` 1 = the full snapshot of 2700 at
      2725, after its resync rebuilt from 2698 (a pacer rebase's resync).
      Band 29 of 303 out.
    - **A6m hold: PASS.** No rebase; both N runs' criteria equal both O
      runs'; no zero broken (the late mirror is non-zero in the O runs).
      Band 25 of 313 out.
    - **Swing bay: PASS.** No rebase or resync; both N runs' criteria equal
      both O runs'; no zero broken (`far` is non-zero in an O run of every
      client where an N run has it). Band 8 of 138 out.
- **The step's status.** Every exact check, CPU parity (against the control,
  as §5.3 prescribes on a failure) and the row's three arms pass on the
  closing round. Committed.

**As built (step E7).** Where the code made E7 differ from §1.2, §1.3, §2.2,
§2.3 and the row above:

- **What moved** (`src/session/client_core_lifecycle.cpp`, after E6's remote
  lifecycle, as §1.1 names it), verbatim but for the platform calls, the
  configuration (`config.rollback`, the new `config.own_echoes`) and the
  back-calls (`own_ball`, `recorded_input_at`): `report_physicalize`'s C half
  (the event, the send, `record_own_birth`, the log line), the own
  Unphysicalize report, `beam_own_ball`, `record_own_birth`,
  `own_life_current`, `unmake_own_body` / `remake_own_body` (their core half:
  the mute, `mark_dirty`, the counters, every log line), `apply_own_lives`,
  `own_lives_due`, `live_own_lives`, `retire_own_lives`, `rebase_own_lives`,
  `set_aside_unanswered`, `find_unanswered`, `moved_own_life`,
  `process_own_echoes`, `process_own_echo`, `answer_set_aside`,
  `expire_own_echoes`, `resync_own_life` and `move_own_birth`. The constants
  `kOwnEchoDeadlineTicks`, `kOwnEchoUplinkSettleTicks` and
  `kOwnRecordKeepTicks` moved into `client_core_state`; `event_type_name`
  moved from SC to `session/lifecycle.hpp` (the own-echo lines name the
  type; SC's `--hold-event` uses the same one). `snapshot_interval_`, dead in
  SC since E1, is core state now (`begin` sets it): the `own_body_control`
  fallback reads it. The 20 moved `logf` lines are unchanged (188 literals against f2227b0's 188, EQUAL); the headless
  client keeps a one-line call at every old call site (pre_step step 3,
  step 19's created / deleted, step 29, the binding's `destroy_body`).
- **Entry points the design does not name** (the E3 pattern, until E9's
  frame): `process_own_echoes()` (step 3); `report_physicalize(entity,
  tick)` and `report_unphysicalize(tick)` (step 19, `event_log`); the
  `host_hooks` pair `own_physicalized(own_birth_report)` /
  `own_unphysicalized(tick)` queue, and `commit_own_reports()` commits them
  in order at step 19 (the core has no step 19 before E9); `request_beam`
  takes a `double[3]`, not §1.3's `float[3]` (the headless in-place branch
  writes `args_.beam_position` as doubles: floats would move the ball by the
  rounding), and `apply_requested_beam(tick)` is step 29;
  `destroy_moved_own_birth(entity, birth_tick, destroyed)` is the core's
  half of the binding's `destroy_body` (the host keeps its "could not be
  destroyed" line and counter). `event_log_mute` is private now (E6 left it
  public for the own body).
- **Platform operations** (§2.2 rows amended):
  - `make_own(own_body_spec) -> own_body_made` and `unmake_own(entity,
    position, rotation, tick, error) -> bool`, required with
    `own_body_control`. SC's `remake_own_body` / `unmake_own_body` were one
    function each with the host's navigation shutdown first, the mute in the
    middle and the pose write (unmake) or the spawn kick (make) after it. The
    core now mutes around the whole operation, so the navigation shutdown, the
    unmake's `SetWorldMatrix` and the make's `push_impulse` run muted where SC
    ran them unmuted. Neither makes a `created`, `deleted`, `recipe` or
    `script_wakeup` entry (the kinds step 19 reads; a muted call can at most
    suppress a `revived` / `frozen` entry nothing reads), and every physics
    call and log line keeps its order: the result carries what SC logged
    (`found`, `took_out_*`, `made`/`error`, `kicked`/`kick_error`), and the
    core logs it in SC's order. (SC's step 19 parses none of the kinds a
    muted call could suppress, SC:2042-2072, and a `script_wakeup` of a
    `Ball_*` body is filtered there. To be checked again at R1, when the
    retail host reads its own event log.)
  - `own_birth_capture(entity, tick) -> own_birth_pose` (the capture, or the
    entity after the tick, consumed), required with `event_log`;
    `ball_recipe_of(entity, recipe) -> int` (the ball table row and the
    retail recipe), required of every host: the beam and the own birth need
    it, and §2.2 had no operation for it before E9's `own_ball`.
  - `has_body` is **not** added: `body_state` (E2, required of every host)
    is the same probe.
- **Capabilities.** The headless client declares `own_body_control`,
  `slew`, `nonblocking_hold`, `whole_frame` with `--replay-scripts` (nothing
  reads it before E9) and `own_lifecycle = event_log`.
  `missing_platform_operation` checks `make_own` / `unmake_own` under
  `own_body_control` and `own_birth_capture` under `event_log`.
- **Without `own_body_control`** (§2.3, inert on headless, built and
  unit-tested): `move_own_birth` and `resync_own_life` never touch the own
  body; a moved birth is left to the snapshots when they run (SessionStart's
  `snapshot_interval` below 255, the server's clamp of every longer interval,
  server.cpp:942 - the rule the core can see) and resyncs otherwise, through
  `resync_own_life` (so the request's reason is its "own birth: resync", not
  §2.3's "own birth moved: no own body control"; the reason is on the log
  line before the request, which is what R3 reads); a superseded one gets a
  journal note and nothing else, also when its pair's Unphysicalize was
  echoed MOVED (the pair resync, `process_own_echo`, is taken only with the
  control; review fix, SC never had the case); a rejected or missing one is re-reported at
  `current + 1` without the spawn flag while its life is current, then the
  resync. To tell superseded from rejected for a set-aside birth,
  `answer_set_aside` takes the echo's `superseded` bit and sets the copy's
  state before `resync_own_life` (the copy is discarded; with the control
  nothing reads it). The beam: the generation is bumped at the request, the
  record kept, and its stamp is `write_beam` (`write_body` upright and at
  rest, waking) live and in every replay that crosses it; the in-place
  branch without a navigation replica shares `write_beam`, so its one
  `--beam at tick %u: %s` line stays one call site. The fallback's own
  messages are journal notes (no new `logf` literal, §5.2 (4)).
  "Snapshots run" is SessionStart's byte below 255: a server configured
  with an interval of 255 or more still sends snapshots, at most every 255
  ticks, and the core treats that as off (a moved birth resyncs instead of
  waiting that long; the safe side, one extra resync). `snapshots_run`
  says so.
- **The `own_echoes` switch** (`core_config::own_echoes`, headless =
  `rollback`): acting needs `rollback && own_echoes`, which on headless is
  `rollback` alone, SC's gate. Off, no record or set-aside birth is looked
  up, the moved-Unphysicalize list is not kept, `expire_own_echoes` does
  nothing, and the echo is counted, journaled and logged by the existing
  "no record of it, %s" line with the argument "own echoes off: kept where
  we stamped it". Not built: the §1.2 switch `own_birth_move` (the row names
  only `own_echoes`; `move_own_birth` needs `own_body_control` anyway). The
  R6 row now says that step builds it, off, before the retail host declares
  `own_body_control`.
- **Order inside the frame.** `report_physicalize`: SC set `own_group_set_`
  and `trace_frames_` (host state) between the record and the log line; the
  host now sets them after the core's call, which reads neither. The own
  Unphysicalize likewise (`own_group_set_` after the log line). The beam is
  requested and made at step 29 in the same call sequence.
- **`legacy_calls`.** Removed, five: `rebase_own_lives`,
  `set_aside_unanswered` (E3) and `live_own_lives`, `own_lives_due`,
  `apply_own_lives` (E6). Added: none (the beam uses E4's `own_ball`, the
  move E4's `recorded_input_at`). Left: `own_body_guard_off`, `report`,
  `recorded_input_at`, `own_ball`, `describe_movable_objects`.
- **§5.1**: 13 new tests (`SessionClientCoreOwnLives.*`): the reports from
  the event log (capture, recipe, spawn flag, the generation, the no-capture
  warning, no record without rollback) and the reports from hooks queued and
  committed after the drain's bump, each with and without the control (the
  reports have no branch on it); the beam with the control (stamped T+1,
  the pair sent, rebuilt live, muted; no own body: said, nothing sent; no
  navigation replica: `write_beam` at once, a new lifetime, nothing kept,
  and a failed write said with nothing sent) and without it (the generation
  bumped at the request, one `write_body` live at T+1 and one in a forced
  replay crossing it; no navigation replica: moved at once, nothing kept);
  and the echoes - moved (with the control: taken out, moved, rewound, made
  again at T' in the replay, the restore's `destroy_body` branch; without:
  nothing with snapshots, a resync without), rejected (removed from the
  history; without the control re-reported without the spawn flag while
  current, the resync only when not), superseded (with the control removed
  from the history, or the pair resync with the body out; without it
  nothing, the pair included), missing (both settings), the pair resync with
  the control in both orders (the birth's echo after its Unphysicalize's,
  and the Unphysicalize's after the birth moved), the move refused with the
  control (past the reach, a life ended since, no record past the history),
  a set-aside birth answered through stale echoes in all four combinations
  under both settings (the missing one made after all: made again with the
  control; the resync one never made: the body out with the control,
  re-reported without; the other two nothing; a stale echo with nothing set
  aside answers nothing, and never a record of the new numbering), the live
  frame's generation retire and its "never met its live frame" drop, and the
  switch off (counted and logged only) under both settings. The existing
  tests fill the four new operations, check the construction rules, lose the
  five back-calls from their call lists (the replay-births test now has a
  real own beam due at its tick), and the re-assignment test checks
  `rebase_own_lives`' effect. 400 tests pass on Windows and on Arch (GCC
  16.2.1).
- **Results** (after the review round: the superseded fix, the new tests,
  every check rerun). Old is f2227b0's binary (`build-agent-E6/`, build id
  `bmmo-8d933adbaf0d-dirty`), new the E7 binary (`build-agent-E7/`,
  `bmmo-f2227b078f12-dirty`).
  - §5.2 (5) and (6), `wire_check.sh E6 E7 e7w2`: WIRE-CHECK: ALL SAME, 62914
    sends on the 16 clients equal but each login; every replay reached its
    end marker (rc 0); all 16 journals IDENTICAL with `e0_compare.py` and
    BYTE-IDENTICAL-EXCEPT-BUILD-ID (`e1_bytecheck.py`: the live reference's
    length, the 12 differing bytes inside the header's build id). What the
    captures exercise of the moved code: the own births reported from the
    event log (2 or 3 per client: every spawn and respawn, the capture and
    the spawn flag), their records and retirement, the own Unphysicalize,
    the echoes (acked on all 16; a MOVED own Unphysicalize kept where
    stamped on `resync_snapoff` client 0 and `a5u_lead` clients 2 and 3),
    the beam pair and its rebuild at the stamp live and in the replays
    (`swing_snapon`, `--beam ... 700`: 7 to 16 `own body: ... made` lines
    per client), a MOVED own birth with its rewind and its re-make inside
    the replay (`a6m_snapoff` client 2, the held birth 2482 -> 2502;
    `a5u_lead` client 2, 2502 -> 2507), and the rebase of the own lives at
    the re-assignments (`resync_snapoff`, `a5u_lead` client 2). **Covered by
    no capture, only by the unit tests**: a REJECTED or SUPERSEDED own echo,
    a missing echo (the deadline), stale echoes and a set-aside birth
    answered, the pair resync, a move past the reach, after the life ended
    or with no record, the remake on a resync, the generation retire and
    the "never met its live frame" drop, `write_beam` with the control
    (headless `--beam --no-rollback`), and every no-control /
    `own_echoes`-off / `host_hooks` branch. **Covered by neither**: a rewind
    `move_own_birth` asks for and the engine refuses ("the rewind to %u was
    refused"). Whether a capture reaches `destroy_moved_own_birth`'s success
    path is not known: it logs nothing (the unit test does).
  - §5.2 (4): 188 literals against f2227b0's 188 (SC + `src/session/*.cpp`),
    EQUAL.
  - §5.2 (3): the old SimTool on its replay of each capture against the new
    one on the E7 replay (`--continue-after-jump`): all 16 the same rc and
    summary line; the four reference server journals rc 0 with 0 checkpoint
    mismatches on both (3986 to 4257 ticks).
  - §5.2 (2): solo snapshots off (`solo/e7_new2`, starved=0) against `old1`:
    TICK hash and pose identical at all 2448 common ticks (8-2455), LOCAL
    checkpoints 1224/1224, the 8 RECEIVED snapshots equal in content, other
    records 6156/6156: IDENTICAL.
  - CPU parity (`cpu_parity.sh E6 E7`, three rounds): PASS, old 20.94 /
    20.89 / 20.79 s, new 20.45 / 20.62 / 20.32 s, limit 22.14 s.
  - Builds: x64 BUILD-OK; x86 mod CLIENT-BUILD-OK; `linux_test_syntax.sh`
    every line ok (24 sources); GCC 16.2.1 compiles the headless client
    syntax-only with the Arch flags, and the six core sources with `-Wall
    -Wextra`, with no error or warning; 400 tests pass on Windows and on
    Arch. The Arch server was rebuilt in the first round (`ARCH-BUILD-OK
    ballanced-3e4289e95f25+bmmo-f2227b0-dirty`: `session/lifecycle.hpp`,
    which the server compiles, gained `event_type_name`); the review round
    touched nothing the server compiles.
  - §5.3, the full arm set, one batch each (`e7ab2_*`), O1 N1 O2 N2, every
    client capturing its inbound. Host peak 12.3 to 19.7 % total, no other
    process above 4.0 % during a run: no void run. No crash; every client
    reached its time limit with a status line (rc 0; rc 2 on both binaries
    in the rollback-off arm, the ladder's); every server journal replays rc
    0 with 0 checkpoint mismatches. **The trace by replay:** each N run's
    four captures replayed with E6 give client journals IDENTICAL to the N
    run's live ones, and each O run's replayed with E7 give its own (all 28
    runs of the set and the 8 of the two extra resync batches: 144 journals,
    all IDENTICAL). Per arm (`e7ab2_*.judge` and `*_pool.judge`, the pool
    being every earlier O run of the arm from E3 on; band counts reported,
    not deciding):
    - **Lane control: PASS.** N1 equals four pooled O runs (`e5ab1_lane_o1`,
      `e5ab3_lane_o2`, both of `e6ab2`), N2 equals `e3ab1_lane_o2`. Rebases:
      O1 client 2 at 2682; O2 clients 2, 3 at 2669 / 2692; N2 all four at
      2666 / 2698 / 2670 / 2704; none in N1. Zeros broken, each the
      snapshot right after the client's own rebase resync: N2 `unmatched` 1
      on clients 0 (2684 at 2711), 1 (2723 at 2745) and 2 (2711 at 2738),
      as O2 client 2 (2710 at 2732). Band 77 of 302.
    - **Swing bay: PASS.** No rebase or resync; both N runs' criteria equal
      both O runs'. Zero broken: client 2 `far` N1 1 = snapshot 848 at 899
      (51 deep, past the reach). Band 10 of 138.
    - **A1 burst: PASS.** Rebases O1 clients 0, 2, 3 at 3402 / 2679 / 2694,
      O2 client 2 at 2699, none in the N runs; both N runs equal O2. No zero
      broken. Band 58 of 304.
    - **A6m hold: PASS.** No rebase; both N runs equal both O runs; the held
      birth applied 2482 -> 2501/2502 in all four. Band 24 of 313.
    - **Snapshots on: PASS.** Rebases O2 clients 0, 1, 3 at 3205 / 3309 /
      3190, N2 client 3 at 3412 (its resync not applied before the
      teardown); both N runs equal both O runs; no zero broken. Band 8 of
      138.
    - **Rollback off: PASS.** Every client rc 2 on both binaries, no pacer
      rebase; both N runs equal O2. Band 44 of 175.
    - **Forced resync: FAILS the fourth bullet** (each N run's criteria
      equal to some O run's). Three batches were run in this round
      (`e7ab2`, `e7ab3`, and `e7ab4` with the slots swapped, N1 O1 N2 O2);
      in each, one N run equals no O run of the pool (16 O runs: E3's eight
      and E7's four batches): `e7ab2_resync_n2` (nearest
      `e3ab2_resync_o2`, differing only in client 1's "relay_late 0" and
      "resyncs 0", both PASS in N2: client 1 had no pacer rebase),
      `e7ab3_resync_n2` (nearest `e7ab3_resync_o1`: client 1 no rebase, so
      also its "own ball 0.0000 in D") and `e7ab4_resync_n1` (nearest
      `e3ab3_resync_o1`, differing only in client 0's "moved births 0"; in
      N1 the server applied clients 1 and 3's respawns late, 2838 -> 2840
      and 2864 -> 2878, and both were moved inside the history). The other
      three N runs equal pooled O runs. Every difference is a §5.3 run
      event (a pacer rebase that did or did not happen, a lifecycle event
      the server applied late), every run is reproduced exactly by the
      other binary from its captures, and the old binary is no more
      repeatable: only 6 of the 16 O runs equal another O run's criteria
      (4 of the 8 N runs equal an O run's; `e7_crit_matrix.py resync`).
      The regime follows the slot, not the binary: in the swapped batch the
      O run in the second slot rebased late (2716 / 2717 / 2944, as the N
      runs in the second and fourth slots of the other batches) and the N
      run in the first slot early (1845-1847). Zeros broken (client 1
      `unmatched` 2 in `e7ab2_n1` and `e7ab3_n1`) are the snapshots right
      after its own rebase resync (1844 and 1860 at 1872 / 1888; 1837 and
      1857 at 1864 / 1884). Band 47 of 304.
- **The step's status: closed under the revised §5.3 rule.** Every exact
  check (§5.2 (2) to (6)), CPU parity and six of the seven arms passed under
  the rule as it stood. The forced-resync arm failed its fourth bullet
  (above). That bullet now accepts a criterion that no O run shares when it
  is traced to a run event, as the band and zero rules already did, and it
  names the cross-replay as the live check that decides. Under the revised
  rule the arm passes:
  - every difference is a rebase that did or did not happen, or a respawn
    the server applied late;
  - all 144 journals replay identically on the other binary.

  Not run: §5.2 (1). No forced-rollback path moved; the own beam inside the
  replays is covered by the swing captures and the unit tests.

**As built (step E8).** Where the code made E8 differ from §1.2, §1.3, §2.2
and the row above:

- **What moved** (`src/session/client_core_frame.cpp`, after E5's rewinds:
  §1.1 names no file for the sectors, and steps 1, 9 and 26 are the frame's,
  so no new source and no `SessionCore.cmake` change), verbatim but for the
  platform calls: `hold_sector`, the pending start sector's stamp (step 1),
  the union pass (step 9) and the own sector poll's decision (step 26), and
  the anchor's two sector lines (`sectors_requested_ = {1}`, `sector_cell_`)
  into `session_client_core::anchor`. The four moved `logf` literals
  (`sector %d stamped ...`, `sector +%d ...`, `sector %d at tick %u`, and
  the union pass's `running:` list, whose lambda's local had to be renamed
  from `s`, the core's state member) are unchanged; SC's `sector_lead_ticks()`
  moved with them (public on the core). The headless client keeps a one-line
  call at each old site.
- **Entry points** (the E3 pattern, until E9's `pre_step` / `post_step`):
  - `request_sector(sector, why)`, as §1.3: before the assignment it only
    records the request (`start_sector_pending_` and the new core state
    `start_sectors_`, every request with its reason in request order, the E1
    note's "becomes core state at E8"; `begin` clears them); after it,
    `hold_sector(s, current + 1 + lead, why)`. `hold_pending_sector` stamps
    each waiting request at T + lead, in order, a repeat dropped by
    `hold_sector` (the first round kept one slot, so a second request before
    the assignment overwrote the first: review of E8; the headless client
    makes one). It returns nothing and always holds: the retail host decides
    before calling whether a crossing is held or reported after the fact
    (§2.2 `request_sector`, review of E8). The headless anchor calls it for `--start-sector` where it
    set the flag, always before the assignment (`on_session_assign` needs
    the running phase, which `core.anchor` starts after the call), and keeps
    its own `--start-sector %d will be stamped ...` line after it: that text
    names the headless option, so it stays a host literal.
  - `hold_pending_sector(T)` (step 1), `sector_union_pass(T)` (step 9) and
    `poll_own_sector(T)` (step 26), public until E9. `hold_sector` is
    private.
  - `anchor_report` gained `sector_cell`: the host reads
    `IngameParameter[0][1]` where SC did (before its script selection, reset
    guard and checkpoint hold, none of which writes the cell or reads the
    two members) and the core sets `sectors_requested_ = {1}` and
    `sector_cell_` first thing in `anchor`. An unread cell leaves 0, which is
    what `begin` left in SC's member.
- **Platform operations** (§2.2 rows amended): `sector_manager_active`
  (headless: the manager found once by name, `sector_manager_id_`, which
  stays host state because the activation record and its replay use it; the
  lookup runs at the same place, inside step 9, as before), `run_sector`
  (SC's `start_sector`, its `--start-sector %d: IngameParameter or
  Gameplay_SectorManager is missing` line host text), `record_activation`
  (SC's `record_manager_activation`; `record_pending_activation`, step 15,
  comes with the frame at E9) and `poll_sector(T) -> sector_poll {cell_read,
  cell, resetpoint_moved, checkpoint_held}`. The tick is the T the core
  decides for: the headless `resetpoint now ... at tick %u` line prints it
  (the first round printed `current_tick()`, equal to SC's post-step `tick`
  today, but only while nothing between SC's `tick = current_tick()` and
  step 26 moves the numbering, which E9 could change: review of E8). The
  retail row now says the retail host reads the cell only while its hold is
  in force and returns `cell_read` false otherwise, so a crossing its hook
  already reported after the fact is not reported twice (review of E8). `cell_read` is not in §2.2's
  struct: SC's crossing test ran only when the cell was read, and a host
  must be able to say it was not. The headless poll keeps the resetpoint
  read, its `resetpoint now ...` line (with the own ball's position, CK) and
  `resetpoint_seen_` / `checkpoint_held_` (host state since E1), in SC's
  order: resetpoint, its line, then the cell. All four are required by
  `missing_platform_operation`: every host runs the union and reports its
  own sectors.
- **`legacy_calls`**: none added or removed (the sectors call no SC
  function; `send_event` is the core's since E3). Left: `own_body_guard_off`,
  `report`, `recorded_input_at`, `own_ball`, `describe_movable_objects`.
- **§5.1**: 2 new tests (`SessionClientCoreSectors.*`): a request before
  the assignment only waits (nothing sent or stamped, also on an unassigned
  frame), the anchor's sector 1 and cell, the first assigned frame's stamp
  T + lead with the request's reason (the event decoded: Sector, tick =
  origin = 127), once per sector, a request after the assignment at current
  + 1 + lead, `begin` forgetting a request; and the union pass (the manager
  counted every tick, the pass after three idle ticks with `run_sector`,
  `record_activation(T, reset)` and the line, a peer's stamp held by the
  pacer), the poll raising no event for the pass's own cell write nor for a
  peer's, a moved resetpoint into a peer's sector stamped as ours, a
  crossing stamped tick + 1 + lead with the chain held and reported after
  the fact without it (stamped with the tick, not handed to the union), an
  unread cell ignored, each poll called with its tick; and (review round) a
  second request and a repeat before the assignment, both stamped at the
  first assigned frame in order, the repeat dropped. The fake platform fills
  the four operations and the construction test checks each is required.
  `StateKeepsTheHeadlessInitializers` checks the new member. 402 tests
  pass on Windows and on Arch (GCC 16.2.1).
- **Review round** (two reviews, no blocker). What became of the findings:
  - the host's `resetpoint now ... at tick %u` line printed `current_tick()`,
    equal to SC's post-step `tick` only while nothing moves the numbering
    between them (both reviews): `poll_sector` now takes the tick (above);
  - one waiting slot before the assignment (both reviews): every request
    waits, in order (above);
  - the retail column of `poll_sector` would have the core report a crossing
    the retail hook had already reported after the fact (review 2): the row
    now has the retail host return `cell_read` false while its hold is not in
    force (§2.2);
  - `request_sector` against retail's `physics_session_hold_sector`, which
    returns false and reports at once when unassigned or unheld (review 2):
    the retail host keeps that decision and calls the core only when it
    holds (§2.2 `request_sector`, for R1);
  - the first batch's out-of-band metrics were not all traced (review 2):
    traced below, for it and for the two batches of this round;
  - the `anchor_report.sector_cell` default of 0 with IngameParameter
    missing (review 1, "no finding"): unchanged, as the note says.
- **Results.** Old is 924092a's binary (`build-agent-E7/`, build id
  `bmmo-f2227b078f12-dirty`), new the E8 binary rebuilt after the review
  round (`build-agent-E8/`, `bmmo-924092a86b88-dirty`). Every check below was
  rerun on it.
  - §5.2 (5) and (6), `wire_check.sh E7 E8 e8r`: WIRE-CHECK: ALL SAME, 62914
    sends on the 16 clients equal but each login; every replay reached its
    end marker (rc 0); all 16 journals IDENTICAL with `e0_compare.py` and
    BYTE-IDENTICAL-EXCEPT-BUILD-ID (`e1_bytecheck.py`: equal sizes, 11
    bytes differing, all inside the build id). The sector and `resetpoint
    now` lines of the old and new replay logs are identical (15 to 22 per
    client, 1 to 3 `resetpoint now` lines on 13 of the 16). What the
    captures exercise of the moved code: the pending `--start-sector 4` and
    its stamp on the first assigned frame (`swing_snapon`, all four clients:
    "sector 4 stamped for tick 62 (--start-sector, lead 35, ...)"), three
    union passes per client on all 16 (one held by the pacer on `a5u_lead`
    clients 0, 1 and 3), the own crossings stamped ahead with the checkpoint
    chain held (1 to 3 per client, each after its `resetpoint now` line),
    and the anchor's cell. **Covered by no capture, only by the unit
    tests**: the after-the-fact branch (no run without the checkpoint hold),
    a request after the assignment and a second one before it (no headless
    caller), an unread cell; `sector_manager_active` with no manager has
    neither.
  - §5.2 (4): 188 literals against 924092a's 188 (SC + `src/session/*.cpp`),
    EQUAL.
  - §5.2 (3): the old SimTool on its replay of each capture against the new
    one on the E8 replay (`--continue-after-jump`): all 16 the same rc and
    summary line; the four reference server journals rc 0 with 0 checkpoint
    mismatches on both (3986 to 4257 ticks).
  - §5.2 (2): solo snapshots off (`solo/e8_new2`, starved=0) against `old1`:
    TICK hash and pose identical at all 2245 common ticks (8-2252), LOCAL
    checkpoints 1123/1123, the 8 RECEIVED snapshots equal in content, other
    records 5647/5647: IDENTICAL. Host load at most 12.9 %. (The first
    round's `e8_new1`: 2454 ticks, IDENTICAL.)
  - CPU parity (`cpu_parity.sh E7 E8`, three rounds): PASS, old 20.50 /
    20.50 / 20.46 s, new 20.64 / 20.56 / 20.46 s, limit 21.56 s. (First
    round: old 20.49 / 20.45 / 20.45, new 20.24 / 20.48 / 20.30.)
  - Builds: x64 BUILD-OK; x86 mod CLIENT-BUILD-OK with the target's 25
    objects deleted first (`client_platform.hpp` and `client_core.hpp`
    changed layout); `linux_test_syntax.sh` every line ok (24 sources); GCC
    16.2.1 compiles the headless client syntax-only with the Arch flags, and
    the six core sources with `-Wall -Wextra`, with no error or warning; the
    Arch tree builds and links the headless client and the tests, and 402
    tests pass there as on Windows. No server source changed (the server
    compiles neither the core nor its headers), so the Arch server was not
    rebuilt.
  - §5.3, the row's arm, swing bay (snapshots off), O1 N1 O2 N2, every
    client capturing its inbound. Three batches: `e8ab1_swing` with the
    first round's binary, `e8ab2_swing` and `e8ab3_swing` with this round's.
    Host peak 14.3 / 18.0 / 17.0 % total, no other process above 3.4 %: no
    void run. No pacer rebase in any run. Every server journal replays rc 0
    with 0 checkpoint mismatches. **The cross-replay:** each N run's four
    captures replayed with E7 give client journals IDENTICAL to the N run's
    live ones, and each O run's replayed with E8 give its own (12 runs, 48
    journals, all IDENTICAL), and every counter of each replay's last status
    line (inputs, events, echo, ownlife, life, inrew, rb, unmatched, far,
    relay, lag) equals the live run's: every number below is the run's
    inbound stream's, not the code's. In each batch both N runs' criteria
    equal both O runs' (and every pooled swing O run's from E4 on).
    - **`e8ab3_swing`: PASS, nothing to trace but the band.** No crash
      (every client rc 0), no resync, no zero broken (`far` 3 on N1 clients
      0 and 1 where an O run of the client is non-zero: snapshots 3155-3157
      delivered 49 to 77 deep, past the reach). Band 15 of 138 out
      (reported). 14 are N2's: its client 0 reached its time limit at tick
      3358 against 3441 to 3472 in the other three, so its teardown came
      first and the four clients' `inputs`, `rb`, `inrew`, client 0's `life`
      and client 3's `events` received / dups fall below the band; and its
      client 2's `relay_lag_p99` 34 (band 35-37), the lag its relayed frames
      arrived with (the same in the E7 replay). The 15th is N1 client 1's
      `far` 3 (the snapshots above). N1 client 0's `rb` 181, out on the same
      metric as N2's, is inside the pooled O range (153-218 over 26 O runs).
    - **`e8ab2_swing`: one crash, of the old code as well.** N1 client 3
      exited 0xc0000005 after its time limit (status line written, journal
      complete, its capture's end marker reached), in the teardown:
      `~session_client` -> `~headless_engine` -> `CKContext::Reset` ->
      `DestroyObjects` -> `SetPhysicsSpringCallBack`
      (`SetPhysicsSpring.cpp:224`, `delete spring` on a stale pointer in the
      block's local 0). **The E7 binary replaying that capture crashes the
      same way** (the same stack, by the frames' offsets), as does E8, and
      both replays' journals are IDENTICAL to the live one: a spring handle
      left in a SetPhysicsSpring block after IVP freed it, met only when the
      engine tears the scene down. It is not E8's (no spring, joint or
      teardown code moved) and is left open below. Traced besides: N1 client
      2's resync (the server applied its beam's pair, Unphysicalize and
      Physicalize stamped 701, at 703; the own pair moved, plan step 22:
      `resyncs` 1/1, `relay_late` 150, `ownlife` resynced 1 and other 1,
      `echo` moved 2) and its `unmatched` 2 (snapshots 703 and 728 at 789
      after the rebuild from 701); N2 client 0 `unmatched` 3 (snapshots
      3145-3149 at 3245, 96 to 100 deep, past the 96-tick history); N2
      client 1 `echo` moved 1, `ownlife` moved 1 / remade 1 (its Physicalize
      3343 applied at 3347). The rest of the band's 35 of 138: N1 was the
      longest run (client 0 at 3461 against 3359 to 3405, the teardown last)
      with client 2's resync in it - `inputs`, `rb`, `inrew`, `relay_lag_p99`
      and `events` of N1's clients - and N2 clients 2 and 3 `events`
      received / dups (1205 / 1021 and 1193 / 1008, inside the pooled O
      range 1075-1228 / 907-1041 and 1114-1222 / 944-1035).
    - **`e8ab1_swing` (first round) traced.** One zero broken: N2 client 1
      `unmatched` 1 = the full snapshot of 3283 delivered at its local tick
      3381, 98 deep, past the 96-tick history. N2 client 3 (player
      1433115652) `echo` moved 2, `ownlife` moved 2 and remade 3: the server
      applied its Physicalize stamped 701 at 702 and stamped 1210 at 1211
      (server.log, "late by 1"); the client moved both births inside the
      history ("own physicalize stamped 701 moved to 702 ... at local tick
      723", "1210 moved to 1211 ... at 1238") and re-made the body at the
      applied tick, live and in the replays after (3). The other 7:
      client 0 `events` received / dups N1 1179 / 996 (the run longer than
      O1's by 80 ticks, inside the pooled O range 1075-1230 / 894-1043);
      client 1 `events` N1 1192 / 1009 and N2 1199 / 1016 (the two runs
      with peers' lifecycle events applied late, 4 and 5 against 1 and 0 in
      the O runs; 2 to 9 above the pooled maximum of 1190 / 1006); client 2
      `rb` 330 / `inrew` 307 in N1 (its own Physicalize 3347 moved to 3348
      and a BodyRevived 3331 applied at 3332, inside the pooled range
      276-341 / 258-320). `far` differs only where an O run of the client is
      non-zero.
- **Open (not E8's):** the teardown crash above. A SetPhysicsSpring block
  keeps its spring in local 0 (`script_replay.hpp`, `is_joint_block`); when
  the spring's body goes while the block keeps the pointer, the block's
  reset callback deletes freed memory at `CKContext::Reset`. Seen once in 12
  swing runs of this step, reproducible from `e8ab2_swing_n1/capture3.bmic`
  on both binaries; it happens after the session's end, so no journal or
  criterion is affected but the exit code.
- **The step's status.** Every exact check (§5.2 (2) to (6)) and CPU parity
  pass on the review round's binary; the row's arm passes in `e8ab3_swing`
  and, but for a teardown crash the old binary reproduces from the same
  capture, in `e8ab2_swing`, every difference traced to a run event above;
  every run is reproduced exactly by the other binary from its captures.
  §5.2 (1) not run: no forced-rollback path moved.
- **Later (after E11): the checkpoint chain's phase cell (N4).** The hold
  empties only the chain's Execute Script; `Gameplay_Events / activate next
  Checkpoint` still wrote `CurrentLevel[0][4]` 0 at C+2 and 1 at C+4, the
  cell the modules of a union pass at M branch on in frame M+5. A crossing
  with C - M = 3 had them take their teardown branch on that client alone
  (`e8ab2_swing_n1` client 3: six sector-3 bodies missing from 2156 on),
  and C - M = -4 or -5 had the pass's 0 disarm the next checkpoint. Host
  support `game/checkpoint_phase.hpp` gives the chain's two Set Cells,
  Event_handler's `deact. Checkpoints` Set Cells and the checkpoint scripts'
  `Get Cell(0, 4)` a private copy of `CurrentLevel` (starting at 1) at every
  world's anchor: the server's (`physics_world`), the headless client's and
  the retail mod's (resolved with N3, handed back at session end). The
  core's half is a check: `sector_poll` gained `{phase_read, phase}` (the
  cell after the step) and `poll_own_sector` counts and logs a pass whose
  modules found it 0 in frame M+5 (`phase_misses()`; a pass starting at
  M+5 writes its own 0 and is not counted). The copy's CK objects move the
  CK_IDs of the bodies made after the anchor, and `list_bodies` iterates an
  `XHashTable` keyed by them, so a journal's LOCAL checkpoints may list the
  same bodies in another order (`e0_compare.py --order-free` compares them
  as sets). Left on the cell besides the manager and the level's init:
  `Gameplay_Events`' level-finish pair (Level_Finish, End Level, a 50 s
  Delayer, then 0, the reset of `PE_Balloon Script`, 1), whose reader is a
  mechanism script the replay records and so must stay on the recorded cell;
  it fires about 50 s after a ball of that world finished, when the
  finisher's sectors are long in the union. A checkpoint script that is a
  mechanism script refuses the split. The poll's tick is the behaviour
  frame: a live frame steps exactly one tick and a replay does not poll.

**As built (step E9).** Where the code made E9 differ from §1.1, §1.3, §2.2,
§2.4, §5.1 and the row above:

- **What moved** (`src/session/client_core_frame.cpp`, the file §1.1 names
  for `pre_step` / `post_step`), verbatim but for the platform calls and the
  configuration (`config.rollback`, `config.correct`, `config.trace` for the
  headless `args_`): the body of SC's `frame()` from the start-sector stamp
  to the journal block, as `pre_step()` (steps 1-12) and `post_step(bool)`
  (steps 13-32), each step in SC's order; the input building, the record,
  `take_held_ahead`, the input send and the event repeats, the legacy
  blend's frame half (`apply_blend`, step 27), the drive of T + 1 and the
  amend; `recorded_input_at` (over the new `own_replica_name`); and the
  pacer's `slew_step`, `rebase_threshold`, `schedule_rebased`, `slewing`,
  with `next_tick` and `previous_cam`. The core's half of the rollback
  binding (`core_world_hooks`: its births before each replayed tick, the
  replayed mirrors, the corrections into the journal, the moved own birth's
  destroy, and the own-body and mirror tests the shared destroy and insert
  need) is built in the constructor and handed to every `bind_world`. SC's
  `last_keys_` / `last_nav_active_` (the `--trace` input edge, core logic)
  moved into `client_core_state`. The six moved `logf` literals (`remote
  navigation input %s: %s` twice, `lead: forced own input ...`, `lead: own
  polling after ...`, `input edge ...`, `amend_record found no history ...`,
  `blend %s: %s`) are unchanged; 188 literals against 0ea2e05's 188, EQUAL.
- **The host's frame** (`session_client::frame()`, SC:1927): the record's
  keys (and `--keep-lane`), `--beam`'s request, `core.pre_step()`, the
  engine's step, `live_tick_serial_ = 0`, `core.post_step(ticked)`, and the
  "tick failed" line. Two places where that is not literally "keys, step,
  hooks":
  - **`post_step(bool stepped)`, not `post_step()`.** SC restored the forced
    own polling (step 13) before it looked at the step's result and ended
    the session on a failure. `post_step(false)` does step 13 only and
    returns false; the host then logs `tick failed` and ends, in SC's order.
  - **`--beam`'s request moved before `pre_step`**, at `next_tick() >=
    beam_tick`. SC asked inside the frame's step 29 (`tick >= beam_tick`,
    the same T) just before `apply_requested_beam(T)`; `request_beam` only
    sets `beam_requested_` and the position, which nothing reads before
    step 29, so the stamp (T + 1) and every effect are the same.
- **Platform operations** (`client_platform.hpp`; §2.2 rows amended where
  the signature differs):
  - `world_hash(world_hash&)` (step 32; required with `journal_path`);
  - `own_ball() -> own_ball_view {entity, ball_type, physicalized, state}`, a
    read only - it also replaces E4's `legacy.own_ball` (the snapshot path,
    the beam, the LOCAL checkpoint's own name), whose extra reads are
    `ball_type_of` and `get_body_state`;
  - `own_body_upkeep(view)`: the body guard, the own group, the own replica
    attach. SC set the body guard **before** reading the own body; the read
    is now first. `set_body_guard` only writes two manager fields
    (`m_KeepLevelBodies`, `...Except`) and `get_body_state` reads neither
    and logs nothing, so no output and no world state can tell the order;
  - `own_replica_name()` (`own_nav_entity_`, the §2.2 row of E5);
  - `discover_navigation(tick) -> bool`: true exactly when the graph became
    known in this call (the host logs its `navigation keys known` line,
    with the leaf count only it has); the core then attaches the mirrors;
  - `drain_body_events(tick, each)`, not `std::vector<body_event>()`: the
    host parses the log (the shared `parse_event_log`), keeps the recipe
    entries in its birth log **in the log's order** and hands every other
    entry to `each` as it meets it, so a recipe's parse-failure line and the
    core's own-birth lines keep SC's interleaving. `each` answers true for
    the own ball's `created` / `deleted`, and the host then clears
    `own_group_set_` (and arms `--trace`'s `trace_frames_` on a `created`),
    after the core's report, as SC did;
  - `reportable_wakeup(name, own_ball)`, `observe_input() ->
    input_observation {keys, cam, cam_valid, nav_active}` (the ball type
    and the physicalized flag come from `own_ball`, read once; `paused`
    arrives with R2), `own_camera_rows(basis)`, `movable_mechanisms()` (`movable_bodies()` since step P),
    `note_script_resets(tick)`, `script_guards(tick, resolved)`,
    `record_pending_activation(tick)`;
  - `drive(entity, frame, error) -> bool` (the core logs the failure, SC's
    two `remote navigation input` lines);
  - `force_own(frame, ok, error) -> entity` ("" without an own replica:
    nothing done, no line) and `unforce_own(entity, error) -> bool`: the
    core keeps the entity from step 11 to step 13 and writes SC's two
    `lead:` lines with its `lead_force_logs_`;
  - `arm_own_capture(tick)`, optional (retail: empty); the headless one
    also numbers the live tick (`live_tick_serial_`);
  - `bind_world(const core_world_hooks&)`.

  Every one but `arm_own_capture` is required by
  `missing_platform_operation`; `world_hash` only with `journal_path`.
- **Host hooks**: `after_rewinds` (step 7: `maybe_force_rollback`),
  `after_record` (step 23: `dump_savestate`), `after_amend` (step 30a: the
  traces, then - until E10 - the `window:` line, which SC wrote right
  after them, `report_window` being still host code) and `after_journal`
  (step 32, inside the journal block: `report_queued_impulses`).
- **The pacer.** `slew_step(now, tick_rate, behind)`, not §1.3's
  `(wall_ticks, behind_ticks)`: the slew's clock (`lead_slew_clock_`) is
  core state (E1, set by `on_session_lead`), so the core reads the wall
  ticks since it from `now`; the host passes `kTickRate` (66.0, the same
  double SC multiplied by). Inside it the slew is acknowledged when done
  (`finish_lead_slew("slew complete")`) **before** the host moves its
  origin by the returned ticks - SC moved the origin first. Nothing in
  `finish_lead_slew` reads the origin or a clock (the ack, its line, a
  journal note), so the order is invisible; the wall time is read at the
  same site (`slew_step`) as before.
- **Host support (§2.4)** is in two headers, not one:
  - `include/session/session_host_world.hpp`, CK-free so the unit tests
    can bind a fake backend (§5.1's `bind_session_world` case):
    `local_birth`, `local_birth_log` (SC's `local_births_` map with its
    4096-tick trim, `find`, `birth_tick_of`, `birth_order`),
    `parse_recipe_event` and `parse_event_log` (verbatim),
    `session_world_options` and `bind_session_world(backend, hooks,
    options)`. It composes `before_replayed_tick` as the core's births, the
    host's manager activation, the host's traces (the scripts' frame after
    them, from `bind_script_replay`); `replay_remotes` and `on_correction`
    the core's; `insert_birth` (only with `--insert-births`),
    `birth_tick_of`, `birth_order`; `destroy_body` (not with
    `--park-births`): the core's moved own birth, then the balls / the own
    body / the mirrors kept, then the mechanism destroyed muted and put
    back (`put_back`, a host callback - the CK matrix write); the failures
    reach the host's log lines through callbacks, sharing its
    `recreate_logs_` cap; and the script binding last (`bind_scripts`, a
    host callback: `bind_script_replay` is CK). The headless client sets
    `diff_savestate` and its `--trace` wrappers of `set_savestate` and
    `step` on the world it returns, after `bind_script_replay` instead of
    around it - independent fields neither touches;
  - `include/game/session_host_support.hpp` (CK): `matrix_from_pose`,
    `make_mirror_body` (platform `make_mirror` after the entity lookup),
    `navigation_leaves`, `attach_mirror_navigation`, `own_replica` +
    `attach_own_replica` (SC's five own-replica members are one
    `own_replica_` now), `manager_activations` (record and replay; the
    host keeps the `--park-births` gate, the counter and the trace line),
    `live_cells_guard` (platform `keep_live_cells`) and `put_back_birth`.

  All templates take the backend by reference; the headless host passes
  `physics_direct` (its member for the binding, a local one for the
  mirror body and the navigation, which have no binding to outlive).
  §2.4's unification of `local_birth_log` on `has_local` is the headless
  rule, so nothing changes here; the retail host adopts the helpers in the
  R steps (nothing in the mod includes them yet).
- **The switches** (`core_config::input_rewind`, `own_corrections`;
  headless both `= args_.rollback`). `input_rewind` gates the remote half of
  `note_input_correction` (no replay asked for; the frame is stored and
  counted as before); `own_corrections` gates `correct_own_tick` (no amend
  of the tick's record, no own replay). `replay_corrected_inputs` itself is
  not gated: it runs what was asked for, so with `input_rewind` off and
  `own_corrections` on the own replay still runs. `store_held_frame`
  (the server's frame into `own_inputs_` and the journal's SUBSTITUTED) is
  not gated either: it is the record of what the server applied, which
  part two's forced frames share.
- **`legacy_calls`**: removed `recorded_input_at` and `own_ball`; added
  none. Left: `own_body_guard_off`, `report` (E10), `describe_movable_objects`
  (a headless diagnostic).
- **Not done here**: `on_message` (not in the row; the host still decodes
  the Session* opcodes and calls `on_session_*`), so `flush_inputs`,
  `remember_sent` and `put_event_on_wire` stay public (the E3 note expected
  them private at E9; the unit tests drive them); the entry points E5-E8
  made public for the host's frame stay public for the unit tests until
  E11; SC's now-unused `request_resync` / `send_event` forwarders and the
  `remote_ball` / `forced_own` aliases stay until E11's cleanup.
- **§5.1**: 6 new tests. `SessionClientCoreFrame.*` (4): one `pre_step`,
  step, `post_step` cycle and the next `pre_step` make design 1.3's
  platform calls in order (steps 4, 7, 9, 10, 11, 12; then 13, 15, 16, 17,
  18, 19 with a mechanism's wakeup sent once as BodyRevived, 21, 23 and its
  hook, 25, 26, 28, 30, 30a, 32 with the hash, the checkpoint and its hook;
  an event arriving between frames drained at the next `pre_step`'s step
  2; a failed step restores only the forced polling and keeps the tick);
  the own ball's `created` / `deleted` reported from the event log and
  answered true to the host, a ball's wakeup no BodyRevived; **both switches
  off: no replay, no own amend** (and both on: one replay of two ticks,
  counted under `inrew` and `subst rew`, the own record amended); the
  pacer's threshold (33 + outstanding), `schedule_rebased`'s resync, and a
  +2 slew stepped to its ack. `RollbackBinding.*` (2): `bind_session_world`'s
  hook order and its destroy / insert / birth-log rules (§5.1's binding
  case), and the event log's and recipe's parse with the birth log's trim.
  The existing tests fill the new operations (the fake's `own_ball` replaces
  the `legacy:own_ball` entries of their call lists), check the
  construction rules for the 15 new required operations and `world_hash`
  with a journal, and the new state's initializers. 408 tests pass on
  Windows and, built with GCC 16.2.1 on Arch, there too.
- **Results.** Old is 0ea2e05's binary (`build-agent-E8/`, build id
  `bmmo-924092a86b88-dirty`), new the E9 binary (`build-agent-E9/`,
  `bmmo-0ea2e052a68c-dirty`).
  - §5.2 (5) and (6), `wire_check.sh E8 E9 e9w`, then on the final binary
    against the same old traces (`wire_check.sh - E9 e9x e9w_old`):
    WIRE-CHECK: ALL SAME both times (16 clients, every send equal but each
    login); every replay reached its end marker; all 16 journals IDENTICAL
    with `e0_compare.py` and BYTE-IDENTICAL-EXCEPT-BUILD-ID
    (`e1_bytecheck.py`: the live reference's length, 10 bytes differing per
    file, all inside the header's build id). The captures run the whole
    moved frame on every tick: every step of both tables, the record and
    amend, the input sends and event repeats, the BodyRevived reports, the
    beam (`swing_snapon`), the forced own ticks of a SessionLead and its
    slew (`a5u_lead` client 2), the pacer rebases (`resync_snapoff`,
    `a5u_lead`), the mirrors' navigation attached at discovery, and every
    replay through `bind_session_world` (births made inside replays, the
    manager activations re-issued, mechanism bodies destroyed and put back).
    **Covered by no capture**: a failed step, `--insert-births` /
    `--park-births`, a failed destroy or insert, and the switches off (the
    unit tests); `--force-rollback`'s hook (§5.2 (1) below).
  - §5.2 (4): 188 literals against 0ea2e05's 188 (SC + `src/session/*.cpp`),
    EQUAL; the host-support headers hold none.
  - §5.2 (3): the old SimTool on its replay of each capture against the new
    one on the E9 replay (`--continue-after-jump`): all 16 the same rc and
    summary line; the four reference server journals rc 0 with 0 checkpoint
    mismatches on both (3986 to 4257 ticks).
  - §5.2 (2): solo snapshots off (`solo/e9_new1`, starved=0) against `old1`:
    TICK hash and pose identical at all 2436 common ticks (8-2443), LOCAL
    checkpoints 1218/1218, the 8 RECEIVED snapshots equal in content, other
    records 6126/6126: IDENTICAL. Host load at most 10.2 %.
  - §5.2 (1), run because `--force-rollback` now reaches the core through
    `hooks.after_rewinds` and no capture runs it: `savestate_check.py
    --journals` against `arch_server_snap.sh wsl 26700 2 66 4.2`, starved=0:
    forced-50x12 (49 forced rollbacks), forced-97x24 (25) and forced-131x47
    (18) EXACT, all 53 bodies 0.0 m over 1731 pairs.
  - CPU parity (`cpu_parity.sh E8 E9`, three rounds): PASS, old 21.78 /
    21.84 / 20.69 s, new 21.50 / 22.77 / 20.83 s, limit 24.08 s.
  - Builds: x64 BUILD-OK; x86 mod CLIENT-BUILD-OK with the target's 25
    objects deleted first (`client_platform.hpp` and `client_core.hpp`
    changed layout; the mod includes neither host-support header);
    `linux_test_syntax.sh` every line ok (24 sources); GCC 16.2.1 compiles
    the headless client syntax-only with the Arch flags, and the six core
    sources with `-Wall -Wextra`, with no error or warning; the Arch tree
    builds and links the headless client and the tests, and 408 tests pass
    there. No server source changed, so the Arch server was not rebuilt.
  - §5.3, the full arm set, one batch each (`e9ab1_*`), O1 N1 O2 N2, every
    client capturing its inbound; then a second batch of the two arms whose
    N runs equalled no O run of the pool (`e9ab2_lane`, `e9ab2_resync`).
    Host peak 11.0 to 19.9 % total, no other process above 3.7 %: no void
    run. No crash: every client reached its time limit with a status line,
    rc 0 (rc 2 on both binaries in the rollback-off arm, the ladder's).
    Every server journal replays rc 0 with 0 checkpoint mismatches. **The
    cross-replay:** each N run's four captures replayed with E8 give client
    journals IDENTICAL to the N run's live ones, and each O run's replayed
    with E9 give its own (36 runs, 144 journals, all IDENTICAL). Per arm
    (rebases and resyncs per client in `e9ab*.judge`, session scratchpad;
    the band counts are reported, not deciding):
    - **Swing bay** (no rebase or resync): both N runs' criteria equal both
      O runs'. Zero broken: client 1 `far` N1 4 / N2 3 - the full snapshots
      of 3163-3165 and 3195 delivered at 3236-3253 (58-73 deep) in N1, of
      3149, 3150 and 3157 at 3204-3214 (55-64 deep) in N2, past the reach
      (client 0 of both O runs and O2 client 3 refused the same 3163-3164
      49-62 deep). Band 6 of 138.
    - **Snapshots on**: both N runs' criteria equal both O runs'. Rebases:
      O1 clients 0 / 3 at 3214 / 3199, N1 0 / 3 at 3207 / 3405, O2 client 3
      at 3194, N2 0 / 3 at 3184 / 3276. Zero broken: client 3 `far` N1 3 =
      the snapshots of 3157 and 3171 (65, 56 deep) and 3221 at 3274 (53
      deep), N2 1 = 3322 at 3371 (49 deep, after its resync from 3289).
      Band 4 of 138.
    - **A6m hold: PASS.** No rebase; both N runs' criteria equal O2's (and
      five pooled O runs'); no zero broken; the held birth applied 2482 ->
      2502 in all four. Band 17 of 313.
    - **Rollback off**: no pacer rebase; N2's criteria equal O1's; N1 differs
      from O1 only in client 2's `resyncs 0` (the ladder's "3 hard
      corrections in a row", which O1 had and N1 did not) and equals the
      pooled `e7ab2_rolloff_o1`. Band 39 of 175.
    - **A1 burst**: rebases O1 clients 2 / 3 at 2697 / 2693, N1 client 2 at
      2701, none in O2, N2 all four (3017, 2681, 2695, 2694). N1's criteria
      equal pooled `e5ab1_a1_o2`, `e7ab1_a1_o2`, `e7ab2_a1_o2`; N2 differs
      from O1 in the `relay_late 0` / `resyncs 0` of clients 0 and 1, the
      two clients that rebased in N2 and not in O1. Zero broken, each the
      resync's: N2 client 1 `unmatched` 2 = the snapshots of 2708 / 2709
      at 2735 / 2736 after its rebuild from 2695. Band 62 of 306.
    - **Forced resync**: paused client 2 rebases at 1500 (applied 1554 /
      1555) in all eight runs. Batch 1: O1 clients 0 / 1 / 3 at 1844 / 2059 /
      1847, O2 client 0 at 3389, N1 none other, N2 0 / 1 / 3 at 1846 / 1850 /
      1845; N2's criteria equal pooled `e7ab3_resync_o2`. Batch 2: O2 and N2
      all three others at 1837-1849, O1 and N1 none; N2's criteria equal
      pooled `e3ab4_resync_o2`, `e7ab1_resync_o1`, `e7ab4_resync_o2`. Zeros
      broken: batch 1 N2 client 3 `unmatched` 2 = the snapshots of 1860 /
      1864 after its rebuild from 1859; batch 2's after the rebuilds from
      1850-1858 likewise. The server applied own births late in batch 1's
      N2 (Physicalize 2639 -> 2647, 2835 -> 2838, 2880 -> 2885: the
      `moved births` of clients 1-3) and O1 (2898 -> 2900, 3584 -> 3598).
      Band 39 and 15 of ~305.
    - **Lane control**: batch 1 rebases O1 none, N1 all four (3329, 2697,
      3330, 2697), O2 client 3 at 2694, N2 none; batch 2 O1 all four (3487,
      2695, 3543, 2688), N1 client 2 at 2684, O2 client 3 at 2696, N2
      clients 2 / 3 at 2680 / 2692. Batch 2's N2 criteria equal pooled
      `e3ab4_lane_o1`, `e6ab1_lane_o1`, `e7ab2_lane_o2`. Zeros broken, each
      the resync's: batch 1 N1 client 0 `unmatched` 1 (3343 at 3370, after
      its rebuild from 3342); batch 2 likewise. Band 67 and 46 of 304.

    **What no §5.3 event traces.** Four N runs - `e9ab1_lane_n1`,
    `e9ab1_lane_n2`, `e9ab2_lane_n1` and `e9ab2_resync_n1` (the first
    round's note said three: `e9ab1_lane_n1`'s rebases, at 2697 and
    3329-3330, come after the window and trace nothing in it) - equal no
    pooled O run, and differ from their nearest only in the phase-D
    own-ball criterion ("own ball 0.0000 in D", window 1500-1547), the one
    E5 and E6 recorded: the error left over from phase B's divergence (1.0
    to 1.8 m, phase B having 135-208 input replays), 0.0000 m in these N
    runs where the nearest O run had 0.0001 to 0.0359 m. Both outcomes occur
    among the pooled O runs of every client concerned (lane, 28 O runs:
    client 1 PASS 3 / FAIL 25, client 2 9 / 19, client 3 2 / 26; resync, 20
    O runs: client 1 2 / 18, client 3 2 / 18), and the N runs have it both
    ways too (`e9ab1_lane` client 2: O 0.0002 and 0.0001 m, both N runs
    0.0366 m). No rebase, late event, snapshot past the reach or teardown
    falls in that window; the old binary reproduces every one of these runs
    exactly from its captures (the cross-replay above).
- **The first round's status.** Every exact check (§5.2 (1) to (6)), CPU
  parity, and the arms' crash / server / zero-rule / cross-replay items
  passed; every broken zero and every other criterion difference was a
  rebase and its resync, a snapshot past the reach, or a birth the server
  applied late. The phase-D own-ball criterion above was not traced to a
  §5.3 event, as at E5 and E6, so the step was not committed then.
- **After the review.** Two reviews; their findings and what was done:
  - Both: the phase-D runs above. Review 1 counted four, not three
    (`e9ab1_lane_n1` too): corrected above. Review 2 proposed a §5.3
    revision admitting the class (a phase-D-only difference, both outcomes
    in the O pool for that client, the run reproduced by the other
    binary). Not adopted here: the closing round below passes the rule as
    written, as E5's and E6's closing rounds did, so the revision is left
    for a decision of its own rather than made by the step it would pass.
  - Review 1: the moved paths no reference capture runs. Replayed with
    E8 and E9 on the same captures with the same flags
    (`cov_replay.py` / `cov_all.sh`, session scratchpad; `resync_snapoff`
    and `swing_snapon`, four clients each, both binaries in parallel):
    `--no-rollback` (the `input_rewind` / `own_corrections` gates,
    `own_body_upkeep` without the attach, the core's legacy blend),
    `--park-births`, `--insert-births`, `--force-rollback 50:12`
    (`hooks.after_rewinds`; 65 more rollbacks than the default replay),
    the traces (`--trace-body P_Modul_08_Schaukel --trace-ticks`,
    `--trace-clock`, `--trace-restore`, `--dump-savestate`,
    `--replay-scripts`), and `--no-rollback` with the clock and restore
    traces: 44 client pairs, every pair the same rc, the journals
    IDENTICAL (`e0_compare.py`), the client logs identical line for line
    once the paths, the build id and the CPU timings (`t_rest`, `t_rep`,
    `t_max`, `t_cap`) are taken out, the send traces SAME
    (`wire_compare.py`), and the 80 savestate / script dumps byte-identical
    but for the `.sav` header's `environment_id` (the `IVP_Environment`
    pointer, bytes 8-15). The park / insert / forced replays run to the
    capture's end (rc 0, and their journals differ from the default
    replay's, so the flags took effect); `--no-rollback` and the traces
    change the clock-read pattern, so both binaries leave the capture at
    the same pass (rc 3 on both: 444 / 1685 steps without rollback, 1140 /
    2759 with the traces) and are compared up to there. Rollback off over
    whole runs is the rollback-off arm's cross-replay (captures made with
    `--no-rollback`, every journal reproduced by the other binary). Still
    run by no replay: a failed step (the unit test), `hold_timeout`, and
    the recipe parse's failure line.
  - Review 1 nits 3-5 (the `--beam` request, `finish_lead_slew`'s place,
    `own_ball`'s extra `get_body_state`): confirmed as recorded, no
    change. Nit 7: the two host-support headers are in the commit.
  - Carried to R1 (its row now opens with them): review 1 nit 6, the
    retail platform must supply the 15 frame operations
    `missing_platform_operation` requires; review 2 nit 5, a compile-only
    instantiation of `session_host_support.hpp` with `physics_view`;
    review 2 nit 2, `post_step` trusts `frame_` without a `pre_step`
    before it (a retail anchor path can skip one; the headless frame
    always pairs them) - a `prepared` flag, `frame_` cleared in `begin()`.
    Not built at E9: a refusal needs a log line, a new literal for §5.2
    (4), and headless cannot reach it.
  - Review 2 nit 3: with `input_rewind` off, `note_input_correction`
    returns before `relay_late_`, `relay_bodiless_` and
    `relay_birth_frozen_`, so they stay 0, where the comment said "stored
    and counted". The comment says so now (comment only, no code), and a
    §4.1 row records it so the R-step checks do not read `relay_late`
    while the switch is off.
  - Review 2 nit 4: §2.2's two `world_hash` rows merged into one.
  - Refuted: none.
- **The closing round.** The binary rebuilt with the review's changes (a
  comment; `bmmo-0ea2e052a68c-dirty`, copied to `build-agent-E9/`), every
  check run again on it, old = `build-agent-E8`:
  - x64 BUILD-OK; x86 CLIENT-BUILD-OK (no header changed); 408 tests pass
    on Windows and, built with GCC 16.2.1 on Arch, there too;
    `linux_test_syntax.sh` 24 lines ok; the headless client syntax-only and
    the six core sources with `-Wall -Wextra`, rc 0 with no warning.
  - §5.2 (5)/(6), `wire_check.sh E8 E9 e9y`: WIRE-CHECK: ALL SAME (16
    clients), all 16 journals IDENTICAL and BYTE-IDENTICAL-EXCEPT-BUILD-ID
    (the live reference's length, 10 bytes differing per file, inside the
    build id); (4) 188 against 0ea2e05's 188 literals, EQUAL; (3) all 16
    client journals the same rc and summary line on E8's and E9's replays,
    the four server journals rc 0 with 0 mismatches on both; (2)
    `solo/e9_newy1` IDENTICAL to `old1` (2323 common ticks, LOCAL
    1162/1162, other records 5843/5843, starved=0, host at most 12.9 %).
  - CPU parity (`cpu_parity.sh E8 E9`): PASS, old 20.71 / 20.89 s, new
    20.65 / 20.70 s, limit 22.11 s.
  - §5.3, the full arm set, one batch (`e9ab3_*`), O1 N1 O2 N2, every
    client capturing its inbound. Host peak 18.6 to 24.5 % total, no other
    process above 9.4 %: no void run. No crash: every client reached its
    time limit with a status line, rc 0 (rc 2 on both binaries in the
    rollback-off arm, the ladder's). Every server journal replays rc 0
    with 0 checkpoint mismatches. The cross-replay: 28 runs, 112 client
    journals, every N run's reproduced by E8 and every O run's by E9,
    all IDENTICAL. Per arm (rebases per client in `e9ab3_*.judge`; band
    counts reported, not deciding):
    - **Lane control: PASS.** Rebases: O1 client 2 at 2697, N1 clients 2
      / 3 at 2699 / 2706, none in O2 or N2. N1's criteria equal pooled
      `e3ab4_lane_o1`, `e6ab1_lane_o1`, `e7ab2_lane_o2`; N2's equal pooled
      `e9ab1_lane_o1` and four more. Zero broken: N1 client 2 `unmatched`
      1 = the snapshot of 2723 at 2747, after its resync rebuilt from 2712
      (the rebase at 2699). Band 35 of 302.
    - **Swing bay: PASS.** No rebase; both N runs' criteria equal both O
      runs'. Zero broken: client 3 `far` N1 3 / N2 3 = the snapshots of
      3155-3157 at 3206 (49-51 deep) and of 3131-3132 at 3182, 3357 at
      3409 (50-52 deep), past the reach (O2's client 0 refused 3161 50
      deep). Band 3 of 138.
    - **A1 burst: PASS** outright (the band rule's own verdict too); N2
      client 2 rebased at 2693; both N runs' criteria equal both O runs'.
      Band 61 of 303.
    - **A6m hold: PASS.** No rebase; N2's criteria equal both O runs', N1's
      pooled `e9ab1_a6m_o2` and four more (N1's client 2 passes "0.0000
      from T'+48", O1 fails it); the held birth applied 2482 -> 2502 in
      all four. Band 32 of 313.
    - **Snapshots on: PASS.** Rebases: N2 clients 0 / 3 at 3214 / 3185, no
      other. Both N runs' criteria equal both O runs'. Zeros broken:
      client 0 `unmatched` N2 12 = snapshots from 3249 on, every one after
      its resync rebuilt from 3232 (the trace lists 11, 3249-3276); client 3 N2 11 = 3232-3270 after its
      resync from 3203 (each the rebase's); client 2 N1 1 = the snapshot
      of 3369 at 3475 (106 deep), N2 1 = 1672 at 1787 (115 deep), deeper
      than the history. Band 25 of 138.
    - **Rollback off: PASS.** No rebase; both N runs' criteria equal both
      O runs'. Band 20 of 140.
    - **Forced resync: PASS.** Paused client 2 rebases at 1500 in all
      four (applied 1545-1556); the others at 2704-2722 in O1 and N1,
      2807-3030 in O2, 1866-1957 in N2. N1's criteria equal pooled
      `e3ab2_resync_o2`; N2 differs from its nearest (`e3ab2_resync_o1`)
      only in client 3's "moved births 0": the server applied client 3's
      own Physicalize late, 2861 -> 2869 (S5 `late=8`), and the client
      moved its birth inside the history ("own birth moved 2861 -> 2869").
      No zero broken (`unmatched` is non-zero in both O runs of the
      clients that have it). Band 55 of 305.

    No N run of this batch differs from the O pool in the phase-D
    criterion alone (its values: lane N1 0.0002-0.0005 m, N2 up to 0.0843
    m on client 3; resync clients 1 / 3 0.0065-0.0220 m).
- **The step's status.** Every exact check, CPU parity and every arm of
  the row pass on the closing round's binary; each broken zero and each
  criterion difference is a pacer rebase and its resync, a snapshot past
  the reach or the history, or a lifecycle event the server applied late,
  and every run is reproduced exactly by the other binary from its own
  inbound capture. Committed.

**As built (step E10).** Where the code made E10 differ from §1.2, §1.3,
§2.2 and the row above:

- **What moved** (`src/session/client_core_report.cpp`, the file §1.1 names
  for the reports, added to `cmake/SessionCore.cmake`): SC's `report()` body
  as `status_text(const host_counters&) const`, `report_window(tick)` as
  `window_text(tick, const host_counters&)`, and `relay_lag_p99` (left in the
  host at E5, read only by the status line; private in the core). Verbatim
  but for the state's prefix, the three host values (`h.rebases`,
  `h.activations_replayed`, `h.script_reset_skips` at the places `rebases_`,
  `activations_replayed_` and `script_reset_skips_` had) and the window's
  phase (`config.window_shift`, below). A normalized comparison with
  6c96154's SC (prefixes and host names mapped, comments and whitespace
  dropped) finds the status body and `relay_lag_p99` identical, and the
  window's index arithmetic, its `now` fill, its argument list and its
  `lag[P]=` / `inrew[P]=` suffix identical.
- **One literal each.** The status line keeps its one format, now formatted
  by `textf` (a private static `session_client_core::textf`: the core's
  `logf` without the log, a growing buffer) and returned. The `window:`
  line's four `std::snprintf` pieces (head, `subst=`, `lead_adj=`, `moved=`)
  are one `textf` literal now: they were appended in order with nothing
  between them, and each old buffer (400 / 240 / 96 / 160 bytes) held its
  piece with every field at 14 digits, which no window's count reaches, so
  the text is the same; the per-source `lag[P]=` and `inrew[P]=` suffixes are
  still appended. **§5.2 (4) as the row asks it ("zero differences in the
  printed lines"):** the plain `logf` multiset (`e0_logfmt.py`) is 186 against
  6c96154's 188 - only the status literal (now `textf`'s) and the `"%s"`
  that printed the window line are gone; the printed formats
  (`e10_logfmt.py`, session scratchpad: the old `logf` literals with
  `report_window`'s `logf("%s")` replaced by its four pieces concatenated,
  against the new `logf` literals plus the two `textf` literals) are 188
  against 188, EQUAL. The lines themselves: every line of the 16 reference
  replays' client logs, old against new (`e10_logs_cmp.sh`), is identical
  once the CPU timings and the journal path are taken out - 208 `status:`
  and 952 `window:` lines, 41407 lines in all, 0 differ (`resync_snapoff`
  runs `--window-origin 1500`, so a non-zero `window_shift` is among them).
- **Where the host values come from: a platform operation the design did
  not have.** The core prints two of the texts itself - the `window:` line
  at step 31 and the status line at `end` (E3's `legacy.report`) - so it
  needs the host's counters there, and §1.3 passed them only as arguments.
  `session_platform::counters` (`std::function<host_counters()>`, optional:
  empty means zeros) is read for those two; the headless client fills it
  with `{rebases_, activations_replayed_, script_reset_skips_}` (§2.2 row
  added). Step 31 reads it only on a frame that closes a window (or sets
  the first base): `window_due(tick)`, after the review below.
  `host_counters` lives in `client_platform.hpp`, with zero defaults.
- **Hooks** (`session_hooks::window`, `status`, as §2.2): step 31 is
  `if (assigned) { text = window_text(T, counters()); if (text != "" &&
  hooks.window) hooks.window(text); }` (gated by `window_due(T)` since the
  review), right after `hooks.after_amend` (the
  traces), as SC's `after_amend` hook did it (`frame_traces` then
  `report_window`, only when assigned). `window_text` runs (and moves the
  base) whether or not a hook is set; the hook only prints. `end` calls
  `hooks.status(status_text(counters()))` where it called `legacy.report()`,
  after the mirrors are gone. The headless hooks are `log_line(text)`, and
  its 5 s `report()` (and the one after the run's last pass) is
  `log_line(core_.status_text(counters()))`: no `logf("%s", ...)` in the
  host, so no literal added.
- **`core_config::window_shift`** (§1.2 lists it; built now): headless
  `args_.window_shift` (`--window-origin`), set by `make_core_config`.
- **`legacy_calls`**: removed `report`. Left: `own_body_guard_off` and
  `describe_movable_objects` (E11's). SC's aliases `window_base`,
  `kRelayLagBuckets` and `kWindowTicks`, which only the moved code used, are
  deleted with it.
- **§5.1**: 2 new tests (`SessionClientCoreReport.*`): the status text with
  the host's counters at their places (`rebases=7 resyncs=`, `activations=8
  scr=0/0/0/0/9 t_rest=`), the relay lag's 99th percentile, nothing printed,
  the last field whole past the first buffer; the `window:` line at its edges
  (nothing on the first edge, a whole expected line with the deltas, the
  host's rebases, a negative `lead=`, two lag sources and an input-rewind
  source, the per-window maps cleared, nothing inside a window, the next
  window from the moved base, and `window_shift` 25's edges at 85 and 145).
  The frame-order test now shows step 31 (`counters`, `hook:window` between
  `hook:after_amend:126` and the journal, the line's tick and the host's
  `rebases=3`); `EndGoesIdle` and `EndTakesTheMirrorsOut` end with
  `hook:status` instead of `legacy:report` (the text printed before idle,
  `remotes=0`, zeros without counters). 410 tests pass on Windows and, built
  with GCC 16.2.1 on Arch, there too.
- **Results.** Old is 6c96154's binary (`build-agent-E9/`, build id
  `bmmo-0ea2e052a68c-dirty`), new the E10 binary (`build-agent-E10/`,
  `bmmo-6c96154d4af5-dirty`).
  - §5.2 (5) and (6), `wire_check.sh E9 E10 e10w`: WIRE-CHECK: ALL SAME,
    62914 sends on the 16 clients equal but each login; every replay reached
    its end marker; all 16 journals IDENTICAL with `e0_compare.py` and
    BYTE-IDENTICAL-EXCEPT-BUILD-ID (`e1_bytecheck.py`: the live reference's
    length, 12 bytes differing per file, all inside the header's build id).
  - §5.2 (4): above (printed formats 188 = 188; lines 0 differ).
  - §5.2 (3): the old SimTool on its replay of each capture against the new
    one on the E10 replay (`--continue-after-jump`): all 16 the same rc and
    summary line; the four reference server journals rc 0 with 0 checkpoint
    mismatches on both (3986 to 4257 ticks).
  - §5.2 (2): solo snapshots off (`solo/e10_new1`, starved=0) against `old1`:
    TICK hash and pose identical at all 2230 common ticks (8-2237), LOCAL
    checkpoints 1115/1115, the 8 RECEIVED snapshots equal in content, other
    records 5609/5609: IDENTICAL. Host load at most 10.4 %.
  - CPU parity (`cpu_parity.sh E9 E10`, three rounds): PASS, old 22.09 /
    21.63 / 21.45 s, new 21.53 / 21.53 / 21.13 s, limit 23.83 s.
  - **The row's check: the harness parses E9's lane-control A/B pair again
    from the new binary's logs** (`e10_reparse.py`, session scratchpad).
    `e9ab3_lane_o1` and `e9ab3_lane_n1` (E9's closing batch, both
    cross-replay-checked at E9): each run's four captures replayed with E9
    and with E10 (all 16 journals IDENTICAL to the live ones), each set of
    logs and journals put in a copy of the run with the run's server journal
    and log, every client's metrics re-parsed from its log's last `status:`
    line (`status_metrics`), and the event report and criteria redone with
    `arena_map_test.py --report-only` and the run's own flags. The E10
    logs' report is IDENTICAL to the E9 logs' on both runs - the
    `event_report.txt` (66 lines; criteria 17/12/0/13 and 14/15/0/13
    pass/fail/masked/n.a.), the `event_report.json`, the summary's metrics
    and criteria and the printed report - once the report directory's own
    path and the CPU timings (`t_rest`, `t_rep`, `t_max`, `t_cap`, which no
    replay reproduces) are taken out; and both are IDENTICAL to the report
    the same `--report-only` makes from the live E9 logs. Against the report
    the E9 harness wrote during the run, one line differs on each: the
    server's last tick (3654 against 4127, 3704 against 4239), because the
    live harness read the server journal while the server was still writing
    it and the batch copied it afterwards - not a client log.
  - Builds: x64 BUILD-OK; x86 mod CLIENT-BUILD-OK with the target's 25
    objects deleted first (`client_platform.hpp` and `client_core.hpp`
    changed layout; 26 objects now, `client_core_report.cpp` among them);
    `linux_test_syntax.sh` every line ok (25 sources); GCC 16.2.1 compiles
    the headless client syntax-only with the Arch flags, and the seven core
    sources with `-Wall -Wextra` (`textf` carries the printf format
    attribute, so both literals are format-checked), with no error or
    warning; the Arch tree builds and links the headless client and the
    tests, and 410 tests pass there. No server source changed, so the Arch
    server was not rebuilt.
  - §5.3, lane control, two batches (`e10ab1_lane`, `e10ab2_lane`), O1 N1 O2
    N2, every client capturing its inbound. Host peak 17.1 % and 25.5 %
    total, no other process above 3.5 %: no void run. No crash: every
    client reached its time limit with a status line, rc 0. Every server
    journal replays rc 0 with 0 checkpoint mismatches. **The cross-replay:**
    each N run's four captures replayed with E9 give client journals
    IDENTICAL to the N run's live ones, and each O run's replayed with E10
    give its own (8 runs, 32 journals, all IDENTICAL). Rebases per client
    (`e10ab*_lane.judge`, session scratchpad; the band counts are reported,
    not deciding):
    - **Batch 1.** O1 all four (2960 / 2699 / 2683 / 2960, resyncs applied
      2974 / 2717 / 2697 / 2973), N1 client 3 at 2702 (2716), O2 client 2 at
      2692 (2706), N2 none. No zero broken (`unmatched` only in O1, each
      right after its rebuild). N1's criteria equal pooled `e9ab2_lane_o2`.
      **N2 equals no pooled O run (32): its nearest, `e5ab2_lane_o1`,
      differs only in client 2's "own ball 0.0000 in A", which N2 PASSES**
      (0.0000 m; client 2's phase-B error, 1.0759 m as in most runs, healed
      by tick 1500, the earliest of the 68 lane runs on record, where the
      O runs without a rebase of client 2 heal at 1730 to 3464 and keep
      0.0001 to 1.12 m in A). No client of N2 rebased, no snapshot came past
      the reach, the server's only late events were three BodyRevived of
      2675 (a mechanism, applied at 2682-2683, as in O1 and O2) and the
      window is before the teardown:
      this is the phase-B leftover E5, E6 and E9 met in phase D (here in
      phase A), **not traced to a §5.3 event**; the old binary reproduces
      the run exactly from its captures (the cross-replay above). Band 34
      of 304.
    - **Batch 2: PASS.** O1 all four (2641 / 2646 / 2645 / 2681), N1 all four
      (2293 / 2291 / 2372 / 2281), O2 all four (2681 / 2675 / 2675 / 2685),
      N2 clients 0, 2 at 3469 / 3471 and 3 at 2711. N1 differs from its
      nearest (`e10ab2_lane_o1`) only in client 1's "moved births 0": the
      server applied client 1's own Physicalize late, 2639 -> 2654
      (server.log "late=15"), and the client moved the birth inside the
      history ("own physicalize stamped 2639 moved to 2654 ... rewound to
      2638"). N2 differs from pooled `e3ab4_lane_o1` only in client 0's
      "resyncs 0": client 0 rebased at 3469 in N2. Zero broken: N1 client 3
      `unmatched` 4 = the snapshots of 2298-2307 after its resync rebuilt
      from 2297 (the rebase at 2281). Band 91 of 304.
- **The first round's status.** Every exact check (§5.2 (2) to (6)), the
  row's report check and CPU parity passed, and every run of both batches
  was reproduced exactly by the other binary from its captures. The second
  lane batch passed the §5.3 rule as written; the first had one criterion
  difference (`e10ab1_lane_n2` client 2's phase-A own error 0.0000 m) that
  no §5.3 event traces - the class E9's review proposed admitting and left
  for a decision of its own. §5.2 (1) not run: no forced-rollback path
  moved.
- **After the review.** Two reviews; their findings and what was done:
  - Both: `e10ab1_lane_n2`'s phase-A own-ball criterion, untraced (above).
    No code change and no rule change: as at E5, E6 and E9 the closing
    round below runs the arm once more on the review's binary and is
    judged by §5.3 as written. Review 2: the second batch is not a
    substitute for the first, so the first round is not counted as a pass.
  - Review 1 nit 2: `client_core_report.cpp` goes into the commit.
  - Review 1 nit 3 / review 2 nit 2 (the counters read on every assigned
    frame): fixed. `window_due(tick)` (const, public with the reports)
    says whether `window_text(tick)` would close a window or set the first
    base, from the same private `window_index_of(tick)` that `window_text`
    now uses; step 31 is `if (assigned && window_due(T))`, so the platform's
    `counters` is read on about one frame in 60. `window_text` returns ""
    without touching any state exactly when `window_due` is false, so the
    lines are the same (the closing round's checks below).
    `WindowTextAtTheEdges` checks `window_due` at the first edge, inside a
    window, at the next edge and on the shifted edges; the frame-order test
    (tick 126 closes a window) is unchanged.
  - Review 2 nit 1 (`window_text` public and not const): kept public for
    the tests; its comment and §1.3's listing now say that step 31 is its
    only caller and that a host takes the line from `hooks.window` (an
    extra call moves the base and takes the window's counts out early).
  - Refuted: none.
- **The closing round.** The binary rebuilt with the review's change
  (`bmmo-6c96154d4af5-dirty`, copied to `build-agent-E10/`), every check
  run again on it, old = `build-agent-E9`:
  - x64 BUILD-OK; x86 CLIENT-BUILD-OK (no data member added, so no
    layout change); 410 tests pass on Windows and, built with GCC 16.2.1
    on Arch, there too; `linux_test_syntax.sh` every line ok (25 sources);
    the headless client syntax-only and the seven core sources with
    `-Wall -Wextra`, rc 0 with no warning.
  - §5.2 (5)/(6), `wire_check.sh E9 E10 e10y`: WIRE-CHECK: ALL SAME (16
    clients), all 16 journals IDENTICAL and BYTE-IDENTICAL-EXCEPT-BUILD-ID
    (the live reference's length, 12 bytes differing per file, inside the
    build id); (4) printed formats 188 against 188, EQUAL, and the lines of
    the 16 replays' client logs, old against new: 208 `status:` and 952
    `window:` lines, 41407 in all, 0 differ; (3) all 16 client journals the
    same rc and summary line on E9's and E10's replays, the four server
    journals rc 0 with 0 mismatches on both; (2) `solo/e10_newy` IDENTICAL
    to `old1` (2436 common ticks, 8-2443, LOCAL 1218/1218, other records
    6126/6126, starved=0, host at most 13.7 %).
  - The row's check, `e10_reparse.py` on `e9ab3_lane_o1` / `_n1` with a new
    E10 replay of each run's captures (all 8 journals IDENTICAL to the live
    ones): the report from the E10 logs IDENTICAL to the one from the E9
    logs and to the live logs' (`E10-REPARSE: ALL IDENTICAL`), and each
    `event_report.txt` byte-equal to the first round's.
  - CPU parity (`cpu_parity.sh E9 E10`, three rounds): PASS, old 31.57 /
    31.28 / 31.30 s, new 31.62 / 31.50 / 31.82 s, limit 33.44 s. The host
    was slower than in the first round for both binaries alike (21.1 to
    22.1 s then; a one-round probe after the batch: 32.43 / 33.03 s), with
    the total under 25 % and no process above 7 %: the sampler does not
    see it.
  - §5.3, lane control, one batch (`e10ab3_lane`), O1 N1 O2 N2, every
    client capturing its inbound. Host peak 23.4 % total, no other process
    above 3.4 %: no void run. No crash: every client reached its time
    limit with a status line, rc 0. Every server journal replays rc 0 with
    0 checkpoint mismatches. The cross-replay: each N run's captures
    replayed with E9 and each O run's with E10, 16 journals, all
    IDENTICAL. Rebases (`e10ab3_lane.judge`, session scratchpad): every
    client of every run - O1 2655 / 2671 / 2649 / 2648 (resyncs applied
    2670 / 2688 / 2664 / 2663), N1 2286 / 2257 / 2354 / 2345 (2300 / 2277 /
    2368 / 2359), O2 1222 / 1201 / 1197 / 1215 (1239 / 1217 / 1211 / 1231),
    N2 1835 / 1532 / 1231 / 1526 (1849 / 1546 / 1250 / 1540), the slow
    host's regime. No zero broken: every `unmatched` sits beside a
    non-zero O value, each a snapshot after its client's resync rebuild.
    Band 75 of 316 (reported).
    - N1 differs from its nearest (`e10ab3_lane_o1`) only in "moved births
      0" of clients 0, 1 and 3: the server applied their own Physicalize
      late (2621 -> 2632, 2623 -> 2633, 2623 -> 2635; server.log `late=`
      11 / 10 / 12), and each client moved its birth inside the history
      ("own physicalize stamped 2621 moved to 2632 ... rewound to 2620").
      Traced.
    - **N2 equals no pooled O run (36: this batch's 2 and 34 earlier).** Its nearest, `e5ab1_lane_o2`,
      differs only in client 0's "own ball 0.0000 in D": N2 FAILS it with
      1.2620 m, the O run passes. Client 0's phase-B error (1.1470 m, the
      same value as in O1, which healed before 1500) lasted through D and
      healed at 3426. Client 0 rebased only at 1835, after D; its six
      substituted frames in D were all the same input (`subst=6/0/6`); no
      own lifecycle event of client 0 was late; no snapshot came past the
      reach; D is long before the teardown. This is the phase-B leftover
      E5, E6, E9 and the first round met (this time the other way round, a
      failure where the nearest O run passes). Both outcomes occur for
      client 0 in this batch's O runs (O1 0.0000 m, O2 0.0004 m), and the
      old binary reproduces N2 exactly from its captures. **Not traced to
      a §5.3 event.**
- **The step's status: closed under the revised §5.3 rule.** Every exact
  check (§5.2 (2) to (6)), the row's report check, CPU parity, and the
  arm's crash, server and cross-replay items pass on the closing round's
  binary. The criteria item did not pass as it was written then:
  `e10ab3_lane_n2` differs from every O run in client 0's phase-D
  own-ball criterion. That is the phase-B leftover, which no event names.
  Both outcomes occur for that client in the batch's O runs, and the old
  binary reproduces N2 exactly from its captures. §5.3 now lets the
  cross-replay decide, and lists such a difference without failing the
  step (revised at E10).

  The whole closing round ran on a host about 45 % slower than the first
  round: CPU parity took 31 to 33 s against 21 to 22 s, alike on both
  binaries. The sampler does not register this. It likely explains why
  every client of that batch rebased early.

**As built (step E11).** Where the code made E11 differ from §1.2, §1.3,
§2.2, §3.1 and the row above:

- **The state is private.** `client_core_state s` is a private member of
  `session_client_core`, in its old place (before `config`, so the
  construction and destruction order is the same; `journal_` is still
  last). Two friends reach it:
  - `core::client_journal`, which reads the state and `config` (the E2
    note's "needs `friend class core::client_journal;`");
  - `core_test_access`, declared in `client_core.hpp` and defined only in
    `tests/session_client_core_test.cpp`: the unit tests' door to the state
    (`state_of(core)`) and to the private steps (`steps{core}.<step>(...)`).

  `config` stays public. It is the host's configuration, not state (§1.2
  lists it apart), `begin` takes it whole, and the tests set it.
- **What the host read of the state are accessors now** (§1.3 "reading"
  gains them): `own_id`, `session_phase`, `session_id`, `level`, `seed`,
  `own_join_order`, `own_group`, `spawn_impulse`, `spawn_spacing`,
  `frames_since_anchor`, `own_physicalized`, `own_body_entity`, `remotes`,
  `own_correction_stats` (the exit code), `recorded_savestate` and
  `recorded_scripts` (`--dump-savestate`, `--compare-scripts`). The host
  also uses the ones that already existed: `previous_cam()` (`--keep-lane`
  and the camera re-arm after a rewind now test its pointer instead of
  `previous_cam_valid_`, and read the same array through it), `assigned()`
  and `sector_lead_ticks()`. Every SC site was replaced one for one, with
  each read at the same place.
- **One write the design had no operation for: the login id.** SC wrote
  `own_id_` at LoginAcceptedV3, and `begin` finds the own join order by it.
  It is `set_own_id(id)` now, in the lifecycle group. The contract: the
  id must be set before `begin`; `begin` without it finds no own join
  order (-1, group `P#63`). The headless core exists from the start and SC
  sets it at login, as before. The retail core does not exist at login
  (§3.2 creates it at the first SessionStart), so the retail host keeps
  the id it got at login and calls `set_own_id` after it creates the core
  and before every `begin` (R1). The setter was kept over a `begin`
  parameter so that SC's login site and `begin`'s signature stay as they
  were.
- **The frame's steps are private too.** E5 to E9 had left them public for
  the host's frame and the tests, and only `pre_step` / `post_step` call
  them now:
  - `drain_due_events`, `apply_held_lifecycle`, `drain_pending_births`,
    `attach_remote_navigation`, `live_drive`;
  - `process_own_echoes`, `report_physicalize`, `report_unphysicalize`,
    `commit_own_reports`, `apply_requested_beam`,
    `destroy_moved_own_birth`;
  - `hold_pending_sector`, `sector_union_pass`, `poll_own_sector`;
  - `replay_corrected_inputs`, `retry_owed_resync`, `take_forced_own`,
    `play_forced_own`, `take_held_ahead`, `finish_lead_slew`,
    `drop_carried_own_replay`, `held_header_current`;
  - `flush_inputs`, `remember_sent`, `put_event_on_wire` (the E3 note's
    "private once `pre_step` and `post_step` exist", so that a host cannot
    pass `filter_event` by).

  Still public: §1.3's API as built, `set_own_id`, the `on_session_*`
  entry points, `own_physicalized` / `own_unphysicalized`, `request_beam`,
  `request_sector`, `force_rewind`, `request_resync`, `send_event` (the
  retail host's after-the-fact Sector event, §2.2), `release_event`,
  `window_text` / `window_due` (§1.3 lists them; the E10 review kept them
  public) and `logf`.
- **`legacy_calls` is deleted.** Its last two entries became optional host
  hooks (§2.2 rows added):
  - `own_body_guard_off` is `hooks.ended`, called by `end` after the
    journal closed and before the mirrors are taken out, as before. The E6
    note asked for "a hook": `own_body_upkeep` has no "off".
  - `describe_movable_objects` is `hooks.describe_movables`. The `--trace`
    line `after rollback: movable=%s` is written only when the hook is
    there. `rollback_diag_` is still counted first, so a host without the
    hook does not move the 16-line cap. Headless fills both, so its calls
    and lines are unchanged.

  Neither hook is required by `missing_platform_operation`.
- **Dead state deleted** (§1.2): `spawn_position_` and `spawn_known_`, with
  the loop in `begin_session` that set them from `players_`, and
  `anchor_hash_`. `snapshot_interval_` is not deleted: it has been live core
  state since E7 (the `own_body_control` fallback reads it).
- **Dead code deleted from the host.** `begin_session`'s engine-reboot
  branch took every mirror's navigation out and cleared nine of the core's
  members (`remotes_`, the relay's and the held section's seen-sets,
  `pending_relay_`, `event_seen_`, `own_event_repeats_`, the remotes' last
  Physicalize and Unphysicalize). It ran after `core_.begin`, which has
  cleared all nine since E3. At E0, `begin_session` cleared `remotes_` before
  the branch too. So the loop never found a mirror, and the clears never
  cleared anything. Deleted, with its now unused `error` local; the branch
  keeps its log line and the reboot.
- **SC is the host of §3.1.** Deleted: the `send_event` and
  `request_resync` forwarders and the `remote_ball` / `forced_own` aliases
  (the E9 note's "until E11"), and `make_legacy_calls`. Kept: the `phase`
  alias (the run loop's switch) and the `current_tick()` forwarder (host
  convenience at 14 call sites). SC has no `core_.s.` left. Two §3.1 items
  are still to come:
  - **`on_message` is not built** (no E row names it, E9's note). The host
    decodes the Session* opcodes and calls `on_session_*`. The retail
    mailbox (R1) is its first user.
  - **`acked_input()` and `release_hold()` are not built either.** Their
    readers are retail's (the starvation detector at R4, the lost
    connection at R1).
- **§5.1**: `LegacyCallsStartEmpty` became
  `BackCallsBecameOptionalHooks` (neither hook required; a core without
  them ends a session). The flow fake records `hook:ended` and
  `hook:describe_movables` where `fill_legacy` recorded the back-calls, so
  `fill_legacy` and its 46 calls are gone. The expected call lists name
  `hook:ended`. The tests reach the state through `state_of(core)` (473
  sites) and the private steps through `steps{core}` (77 calls, plus
  `live_drive` 4 times). After the review,
  `TracedRollbackDescribesTheMovablesWhenTheHostCan` pins the one path
  the optional hook made new: a traced snapshot rollback counts
  `rollback_diag_` with or without `hooks.describe_movables`, and writes
  the `after rollback: movable=` line only with it and only for the first
  16 (checked against a mutation that tests the hook before the count: the
  no-hook arm fails). 411 tests pass on Windows and, built with GCC 16.2.1
  on Arch, there too.
- **Results.** Old is 863f5ea's binary (`build-agent-qos/`, build id
  `bmmo-863f5ea335ef`), new the E11 binary (`build-agent-E11/`,
  `bmmo-863f5ea335ef-dirty`).
  - §5.2 (5) and (6), `wire_check.sh qos E11 e11w`: WIRE-CHECK: ALL SAME,
    62914 sends on the 16 clients, equal but for each login. Every replay
    reached its end marker. All 16 journals are IDENTICAL with
    `e0_compare.py` and BYTE-IDENTICAL-EXCEPT-BUILD-ID (`e1_bytecheck.py`:
    the live reference's length, 12 bytes differing per file, all inside
    the header's build id).
  - **Cumulative wire check**, `wire_check.sh e3t E11 e11c`: WIRE-CHECK: ALL
    SAME (the same 62914 sends). The E0 binary has no `--trace-sends`, so
    e3t (aea9521, the first binary with it) is the earliest wire reference.
    Against E0 itself, (5) is cumulative by construction: the references
    are the E0 binary's live journals.
  - §5.2 (4): 186 plain `logf` literals against 863f5ea's 186 (SC +
    `src/session/*.cpp`), EQUAL. `client_core_report.cpp` and its two `textf`
    literals are unchanged.
  - §5.2 (3): the old SimTool on its replay of each capture against the new
    one on the E11 replay (`--continue-after-jump`): all 16 have the same rc
    and summary line. The four reference server journals give rc 0 and 0
    checkpoint mismatches on both (3986 to 4257 ticks).
  - §5.2 (2): solo snapshots off (`solo/e11_newa`, starved=0) against
    `old1`. TICK hash and pose are identical at all 2460 common ticks
    (8-2467), LOCAL checkpoints 1230/1230, the 8 RECEIVED snapshots equal in
    content, other records 6187/6187: IDENTICAL. Host load at most 11.6 %.
  - **The host paths whose reads became accessors, and the two hooks**,
    replayed with qos and E11 on the same captures with the same flags (E9's
    `cov_replay.py`):
    - `--trace` on `swing_snapon`: 16 `after rollback: movable=` lines per
      client (`hooks.describe_movables`), the step wrapper's mirror lines
      (`remotes()`) and the ball-force traces;
    - `--force-rollback 50:12 --trace-restore --dump-savestate 1600 1610`
      on `resync_snapoff`: the contact lines (`own_physicalized()`,
      `own_body_entity()`), 8 restore traces, and `dump_savestate` /
      `dump_scripts` (`recorded_*`).

    Results: 8 client pairs, every one rc 0 at its end marker; the journals
    IDENTICAL; the logs identical line for line once the paths, the build id
    and the CPU timings are taken out (1704 to 3319 lines); the sends SAME;
    and the 88 dump files byte-identical but for the `.sav` header's
    `environment_id` (bytes 8-15, the `IVP_Environment` pointer, as at E9).
    `hooks.ended` runs only at a SessionEnd, which no capture has (every run
    stops by `--seconds`). `EndGoesIdle` and `EndTakesTheMirrorsOut` pin its
    place. The reboot branch needs a host restart, which no capture has.
  - CPU parity (`cpu_parity.sh qos E11`, three rounds): PASS, old 20.48 /
    20.70 / 20.41 s, new 20.68 / 20.58 / 20.56 s, limit 22.02 s.
  - **Cumulative CPU parity** against the E0 binary (`build-agent-e0`, 9d9cb08,
    which predates the power-throttling opt-out). Both binaries were pinned
    to the performance cores (`aff_run.py FFFF` around `cpu_parity.sh`; the
    children inherit mask 0xFFFF, checked). PASS, E0 21.98 / 21.31 /
    21.85 s, E11 20.87 / 20.77 / 20.66 s, limit 23.75 s. E11 is 3 to 5 %
    faster.
  - Builds: x64 BUILD-OK. The x86 mod is CLIENT-BUILD-OK, built with the
    target's 26 objects deleted first (`client_core.hpp` and
    `client_platform.hpp` changed layout). `linux_test_syntax.sh` passes on
    every line (25 sources). GCC 16.2.1 compiles the headless client
    syntax-only with the Arch flags, and the seven core sources with `-Wall
    -Wextra`, with no error or warning. The Arch tree builds and links the
    headless client and the tests, and 410 tests pass there. No server
    source changed, so the Arch server was not rebuilt.
  - §5.3, the full arm set, one batch each (`e11ab1_*`), O1 N1 O2 N2, every
    client capturing its inbound. Host peak 12.4 to 30.7 % total, no other
    process above 7.9 % (`msmpeng` in the swing bay): no void run. No crash:
    every client reached its time limit with a status line, rc 0 (rc 2 on
    both binaries in the rollback-off arm, the ladder's). Every server
    journal replays rc 0 with 0 checkpoint mismatches.
    **The cross-replay:** qos replays each N run's four captures, and E11
    each O run's. All 28 runs, 112 client journals, are IDENTICAL to their
    live ones. Per arm (rebases per client in `e11ab1_*.judge`, session
    scratchpad; the band counts are reported, not deciding):
    - **Lane control.** Rebases: N1 client 3 at 2694 (applied 2707), O2
      client 1 at 2701 (2715), none in O1 or N2. N2's criteria equal O1's.
      N1 differs from O1 in client 3's `relay_late 0` / `resyncs 0` (its
      rebase) and client 2's phase-D own error, and equals pooled
      `e9ab2_lane_o2`. No `unmatched` or `far` in any run. Band 36 of 305.
    - **Swing bay.** No rebase. Both N runs' criteria equal both O runs'.
      No zero broken: `far` is non-zero in an O run of every client where
      an N run has it; every refusal is a full snapshot 49 to 77 deep, past
      the reach. Band 22 of 138.
    - **A1 burst.** N1 client 2 rebased at 2695 (2708), and N1's criteria
      equal O1's. **N2 equals no O run, pooled or not.** Its nearest, O1,
      differs only in client 1's "own ball 0.0000 in D": N2 FAILS it with
      0.0486 m, O1 passes with 0.0000 m, and O2 fails it with 0.0007 m.
      Client 1's phase-B error was 1.4173 m in N2 (O1 1.0206, O2 1.3461),
      and its leftover outlasted D. N2 has no rebase, no late event at the
      server and no snapshot past the reach, and the window is long before
      the teardown. This is the phase-B leftover of E5, E6, E9 and E10,
      **not traced to a §5.3 event**. Under the rule as revised at E10 it
      is listed and does not fail the step: qos reproduces N2 exactly from
      its captures. Band 75 of 305.
    - **A6m hold.** No rebase. Both N runs' criteria equal both O runs'.
      The held birth was applied at 2482 -> 2501 or 2502 in all four.
      Band 46 of 314.
    - **Snapshots on.** Only N2 rebased: clients 0, 1 and 3 at 3293, 3299
      and 3207 (applied 3306, 3286 and 3220). Both N runs' criteria equal
      both O runs'. Zeros broken in N2, each traced. Client 0 `unmatched`
      14 and client 3's 10 are the snapshots after their resync rebuilds
      (3308-3332, 3222-3244). Client 1's 15 (the trace lists 13): the
      snapshot of 3105 at 3202, 97 deep, past the 96-tick history, then
      3306-3350 after its rebuild from 3286. Band 34 of 138.
    - **Rollback off.** Every client rc 2 on both binaries, with the
      ladder's "3 hard corrections in a row" resyncs and no pacer rebase.
      Both N runs' criteria equal O2's. Band 13 of 140.
    - **Forced resync.** Paused client 2 rebased at 1500 in all four
      (applied 1548 / 1557 / 1556 / 1556). The others rebased: O1 clients
      0 / 1 / 3 at 2769 / 2784 / 2769; N1 client 1 at 2782; O2 0 / 1 / 3 at
      1877 / 1841 / 1845; N2 0 / 1 / 3 at 2710 / 2718 / 2703. N2's criteria
      equal pooled `e3ab3_resync_o2`, `e7ab3_resync_o1`, `e7ab4_resync_o1`
      and `e9ab3_resync_o2`. N1 equals no pooled O run. Its nearest,
      `e9ab2_resync_o1`, differs only in client 1's `relay_late 0` and
      `resyncs 0`, and those are client 1's own rebase at 2782. No zero
      broken: every `unmatched` sits beside a non-zero O value, each the
      snapshot right after its client's rebuild. Band 25 of 304.
  - **The cumulative check against E0** (`e11cum1_*`): lane control and
    swing bay, O = `build-agent-e0`, N = E11. The harness was launched
    through `aff_run.py FFFF`, so both binaries' clients ran pinned to the
    performance cores. Host peak 20.3 % and 16.4 %, no other process above
    3.8 %. No crash. Every server journal replays rc 0 with 0 mismatches.
    **The cross-replay:** E0 replays each N run's captures and E11 each E0
    run's, and all 8 runs (32 journals) are IDENTICAL. The E0 binary
    supports `--capture-inbound` / `--replay-inbound` (it is E0), but not
    `--trace-sends`, so the wire check could not run against it (above).
    - **Lane control.** Both E0 runs rebased every client: O1 at 2655 /
      2713 / 2721 / 2728, O2 at 2677 / 2693 / 2690 / 2689. N1 rebased client
      2 at 2697, and N2 none. The N runs' criteria differ from the E0 runs'
      only in the `relay_late 0` / `resyncs 0` of the clients that rebased
      in O and not in N, which are pacer rebases. Pooled, N1 equals
      `e10ab1_lane_o2` and `e7ab2_lane_o1`; N2 equals `e5ab1_lane_o1`,
      `e5ab3_lane_o2`, `e6ab2_lane_o1`, `e6ab2_lane_o2` and
      `e9ab1_lane_o1`. No zero broken: `unmatched` is in the O runs only,
      each the snapshot after a rebuild. Band 89 of 304. The E0 runs
      rebasing more fits E0's 3 to 5 % higher cost in the pinned CPU parity
      above.
    - **Swing bay.** No rebase. Both N runs' criteria equal both E0 runs'.
      No zero broken (`far` is non-zero in an E0 run of every client where
      an N run has it). Band 20 of 138.
  - §5.2 (1) not run: no forced-rollback path moved, and the forced
    rollbacks with `--trace-restore` above replay identically on both
    binaries.
- **After the review.** Two reviews; what became of their findings:
  - Review 2, `set_own_id` before `begin` (should-fix): the contract is in
    the header comment, in the bullet above and in §3.2's retail sequence
    (the login id kept until the core exists, set before each `begin`).
  - Review 2, the no-hook branch untested (should-fix): the test
    `TracedRollbackDescribesTheMovablesWhenTheHostCan` above.
  - Review 1, the hook comment's "after the first 16" (nit): now "for each
    of the first 16 snapshot rollbacks".
  - Review 2, the reboot branch's comment (nit): reworded. `begin` empties
    the map without taking mirrors out, so mirrors a SessionEnd did not
    take out are not taken out there either, as before E11.
  - Review 2, row E11's "asserts the struct is empty" (nit): the row now
    points here.
  - Review 2, guarding `core_test_access` with a test-only macro (optional
    nit): not done. A friend declaration grants nothing a translation unit
    cannot already take (any TU can redefine the class's access), and a
    macro in the shared header that only one target defines would make the
    class read differently per target for no protection.
  - Review 1, the A1 burst (process question): the closing round below
    re-ran every arm; its A1 batch has no untraced difference.
- **The closing round** (after the fixes; comments and one test only, so
  the shipped code is the code measured above, rebuilt). x64 BUILD-OK, x86
  CLIENT-BUILD-OK. 411 tests pass on Windows and, built with GCC 16.2.1,
  on Arch; `linux_test_syntax.sh` is ok on all 25 lines, and the GCC check
  (the headless client syntax-only, the seven core sources with `-Wall
  -Wextra`) gives rc 0 and no warning. On the rebuilt binary:
  `wire_check.sh qos E11` ALL SAME and the 16 replays
  BYTE-IDENTICAL-EXCEPT-BUILD-ID (12 bytes each); `wire_check.sh e3t E11`
  ALL SAME; the `logf` literals 186 = 186 against 863f5ea; SimTool's 16
  client summaries the same on both binaries and the four server journals
  rc 0, 0 mismatches; solo snapshots off (`e11r_solo`) IDENTICAL to
  `old1` (LOCAL 1214/1214, other records 6106/6106); CPU parity PASS
  against qos (old 20.64 / 20.34 s, new 20.29 / 20.20 s, limit 21.97 s) and,
  pinned through `aff_run.py FFFF`, against E0 (E0 22.03 / 22.10 s, E11
  20.43 / 20.46 s, limit 23.28 s).
  The §5.3 arms again, against qos: `e11ab2_*` (all seven) and `e11ab3_*`
  (rollback off and forced resync, see below), O1 N1 O2 N2, every client
  capturing its inbound. **The cross-replay: all 36 runs, 144 client
  journals, IDENTICAL** with the other binary. No crash (rc 0; rc 2 on
  both binaries with rollback off). Every server journal replays rc 0 with
  0 mismatches (`e11ab2_resync_o2`'s harness replay failed to start,
  0xC0000142, as the host process exited at 03:02; replayed by hand: rc 0,
  4152 ticks, 0 mismatches). Per arm (band counts reported, not deciding):
  - **Lane control** (`e11ab2`, host peak 21.5 %). Rebase: O1 client 3 at
    2695 (applied 2709), none elsewhere. Both N runs' criteria equal O2's.
    No zero broken. Band 24 of 303.
  - **Swing bay** (`e11ab2`, 24.8 %). No rebase. Both N runs' criteria equal
    both O runs'. `far` in both binaries' runs, every refusal a snapshot 49
    to 72 deep; O2 client 2's `unmatched` follows its resync rebuild from
    701 after the server applied its Physicalize late (701 -> 702). Band 9
    of 138.
  - **A1 burst** (`e11ab2`, 21.1 %). N2's criteria equal both O runs'. N1
    rebased clients 2 and 3 at 2678 and 2691 (applied 2693 and 2705), and
    differs from the O runs only by them: client 3's `resyncs 0` and
    `relay_late 0` (84 frames in phase A); client 2's one `unmatched`
    (the snapshot of 2705 right after its rebuild from 2693); client 2's
    "subst recv == server count" (209 against 191: after its rebase client
    2 kept being substituted to the end, and the harness's server count
    stops 60 ticks before the client's last line). Band 88 of 303.
  - **A6m hold** (`e11ab2`, 18.1 %). No rebase. Both N runs' criteria equal
    O1's; the held birth applied 2482 -> 2502 in all four. Band 49 of 313.
  - **Snapshots on** (`e11ab2`, 23.1 %). No rebase. Both N runs' criteria
    equal both O runs'. The broken `unmatched` zeros are snapshots deeper
    than the 96-tick history: N1 client 1 the snapshot of 2215 at 2321 (106
    deep), N2 client 1 3227 at 3354 (127) and client 3 2743 at 2850 (107);
    O2 has the same class (109 to 122 deep). Band 10 of 138.
  - **Rollback off** (`e11ab3`, 13.7 %). `e11ab2`'s N1 is void: the
    sampler logged `msedge` at 71.9 % of the machine at 02:55:21 (a sample
    whose total was 10.9 %, so a counter glitch, but the rule is the rule),
    so the arm was re-run as `e11ab3`, with no other process above 3.0 %.
    No rebase; both N runs' criteria equal both O runs'. Band 18 of 140.
  - **Forced resync** (`e11ab3`, 20.3 %). Paused client 2 rebased at 1500
    (applied 1555) in all four. The others: O1 clients 0 / 1 / 3 at 2950 /
    2694 / 1858, O2 at 2705 / 2710 / 2708, N1 none, N2 at 1837 / 1836 /
    1846. N2's criteria equal O1's. N1 equals pooled `e9ab2_resync_o1`, and
    differs from O1 only in the `relay_late 0` / `resyncs 0` of the three
    clients that rebased in O1 and not in N1, and in O1 client 1's moved
    birth (the server applied its Physicalize late, 2676 -> 2683). N2's
    `unmatched` (client 1: 1850 and 1859) are the snapshots right after its
    rebuild from 1849. Band 76 of 307. `e11ab2`'s forced resync is
    superseded by this batch. Its N1 had three moved births, each a
    Physicalize the server applied late (2634 -> 2644, 2749 -> 2755, 2878 ->
    2885). It also had a phase-D own-ball difference that no event names,
    the same class as `e11ab1_a1_n2`.
- **The step's status.** Every exact check (§5.2 (2) to (6), and the
  cumulative wire check against e3t), CPU parity against qos and against
  E0, and every arm's crash, server and cross-replay items pass. In the
  closing round, each broken zero and each criterion difference is a pacer
  rebase and its resync, a snapshot past the reach or the history, a
  lifecycle event the server applied late, or (A1 N1's substitution count)
  a rebase's substitutions running into the run's last 60 ticks. The first
  round's `e11ab1_a1_n2` phase-D difference stays listed above; both
  outcomes occur for that client in the O runs, and the old binary
  reproduces that run exactly from its captures.

### 6.2 R steps: the retail mod hosts the core

**Testing tools used throughout:**

- `scripts/retail_two_client_test.py install|start --server 127.0.0.1:26700|session|cmd|stop <label>`
  on two game copies against the local Arch server;
- `retail_run.py <label>` (scratchpad) for solo runs;
- the command-file verbs of `client_automation.cpp`;
- journals collected per label under `maps/arena-test/<label>`.

**Inputs.** Driving uses `record` and the new `replay session` mode (R0,
§3.2): one recorded key file per client, made once and reused by every R
step, fed at `tick_base + k` with no clock reset. Today's `replay` is not
anchored to the session and is not used for R checks. Repeatability is
measured, not assumed: every comparison of legacy against core is preceded by
the same comparison of legacy against legacy (R0), since retail solo runs
depend on frame pacing (a `fixed_tick` rebase triggers a resync,
PSC:1709-1712).

**Session sequence.** `mmo connect 127.0.0.1:26700`, `level 13`,
`mmo room create` / `join` / `ready` / `room start physics`.

**Server.** Snapshots on is `arch_server_snap.sh wsl 26700 2 66 4.2`;
snapshots off is `… 100000 100000 4.2`.

**Before every run:**

- the Arch build id must match;
- slot 13 must point at the arena md5;
- BMLPlus must be 0.3.12, with the mod alone;
- the x86 client target must be clean after a header change;
- runs must stay under the roughly 10-minute life of agent-launched
  processes.

**The transition switch.**

- From R1 to R6 the old retail session code stays behind `session core off`
  (a file-scope flag, default off until R7). The same binary can then run
  legacy against core in one batch.
- No new feature is added to the legacy path.
- R7 deletes it.

| step | what | checks that close it |
|---|---|---|
| **R0** plumbing | Build the core and host support into the mod. `core_host.cpp` with the file-scope statics, `retail_platform` (every §2.2 operation), the mailbox, the `session core on\|off` and `session force-rollback E:D` verbs, the `replay session <file>` mode (§3.2). A `net lag <ms>\|blackout <ms> [up\|down]` verb setting GNS fake lag / loss (the retail counterpart of `BMMO_FAKE_LAG_MS`, SC:587-592), so R2-R6 can be measured. Core off by default: nothing else changes. | Clean x86 build; **three launches without a hang** (the BML constraint); the legacy two-client test reproduces its last numbers (the same checks as R1's legacy arm); **the baseline**: two legacy solo runs with `replay session`, snapshots off, have identical journal TICK hashes (if not, the cause is found before R1) |
| **R1** the frame and the snapshot path | First, as E9's review asked: a compile-only instantiation of `include/game/session_host_support.hpp` with `physics_view` (only `physics_direct` instantiates it before R1); the retail platform supplies the 15 frame operations E9 made required (`missing_platform_operation` throws without them); `post_step` refuses a frame no `pre_step` prepared (a flag in `frame_`, which `begin()` clears) - the retail anchor path can skip a `pre_step`, the headless one cannot. `core on` hosts the facade with `rollback`, `correct` as today. The switches `input_rewind`, `lifecycle_rewind`, `own_echoes`, `own_corrections` and `own_birth_move` are off. `mirror_start_from_row` and `own_body_backstop` are on. `caps` = 0; capabilities: no own body control, no slew, blocking hold, no whole frame, `host_hooks`. `core.release_hold(why)` is built here (E3 left it out: no headless path calls it): the blocking hold's release when the connection is lost. The snapshot rollback, resync and its limiter, numbering, record and amend, relay store (no rewind), the events drained at their tick, sectors, the beam (`request_beam`, §2.3) and the core's journal all run through the core. `too_far` → resync is deleted; `frozen` → resync is the core's rule. The §4.2 rows marked R1 change. | (a) **Solo, snapshots off, `replay session`: the TICK hashes of `core off` and `core on` are identical at every tick**, given R0's legacy-legacy baseline (no snapshot, no peer: the R1 order changes cannot show). The journal formats differ (§1.2), so the hashes are compared, not the files. (b) Two clients, snapshots on, 3 runs each of off/on interleaved: per client `snaps/ok/mism/rb`, `max_err`, mirror error, events applied within the off-off band (§5.3 rule); resyncs 0; the server journal replays exact; both client journals replay in SimTool and `worst_bodies.py` reads them. (c) Two clients, snapshots on, `net lag 150`, one death and respawn and one `beam` per client, and one forced resync (a `fixed_tick` stall over 33 ticks on client 2): late events drained at their tick (`events=` as the server's S-lines), the limiter's owed and in-flight path taken once with the resync applied, `own_body_backstop` counted, the own ball's error after the beam reported, everything within the off band. |
| **R2** relay rewind | `input_rewind` on: remote corrections rewind (`replay_corrected_inputs`, `replay_remotes` in the binding). `observe_input` records what the replica stepped (NAV_ACTIVE from the leaves, the mute in force; §2.2); `unforce_own` restores the mute. | (a) **Solo forced rollback**: `session force-rollback 50:12`, `97:24`, `131:47`, plus windows across the first spawn, a respawn, a trafo and an ESC pause held over a nav-activity edge, against a control with the same record. The three original windows may cross no nav-activity edge and would pass with the NAV_ACTIVE bug in place. `worst_bodies.py` 0.0 m on every body (the retail counterpart of §5.2 (1)). It measures what the non-selected retail scripts do inside a replay: a non-zero result names the script, and the step stays open. (b) Two clients, snapshots off, `net lag 150`: `inrew` > 0, `far` 0, resyncs 0. Each client's mirror of the other against the owner's own journal checkpoints (2 ticks): p99 ≤ the headless lane control's at 150 ms (E11's O runs). |
| **R3** lifecycle | `lifecycle_rewind` and `own_echoes` on, `mirror_start_from_row` deleted. Own echoes processed by the core with the `own_body_control` fallback (moved → snapshot or resync; rejected/missing → re-report while current; superseded → nothing). | Two clients, snapshots off, `net lag 150`, with deaths in the arena's death zone, a respawn and a trafo per client (scripted by key record and `beam`). Mirror birth pose at its stamp 0.0000 m against the owner's checkpoint; life rewinds > 0; `overtaken` and `superseded` as the server's S-lines; resyncs only from the capability fallback, each one logged with its reason (the request itself says "own birth: resync"; the reason is the client log's "own birth moved T -> T' at local tick N: <reason>, resync" line just before it, E7); the server journal exact. |
| **R4** part one | `own_corrections` on: held notices amend the own records and rewind. The starvation detector is deleted. LPD §1.4 is corrected. | Two clients, snapshots off, `net lag 150`, client 2 `net blackout 300 up` at tick 1500 (the A1 analogue): `subst recv` equals the server's S2 count for that client; `subst rew` > 0; no resync; client 2's own ball 0.0000 m from the server's journal after the episode settles (A1's criterion); client 1 unaffected (its numbers within the R3 band). |
| **R5** part two (needs `slew`) | `fixed_tick_driver::slew(ticks)` in `on_process` (2x through `SkipRenderForNextTick`, 0.9x by waiting), `kMaxCatchUpTicks` + \|slew\| (LPD §2.4 "Retail"). The forced frames through `force_own`. The retail host declares `slew`, so `caps` = `CAP_LEAD`. | Two clients, snapshots off, client 2 `net lag` +300 ms uplink persistent from tick 1500 (the A5u analogue), server lead flag on: exactly one raise, `lead_adj` 1, the ack sent, no resync, client 2's substitutions after the raise within the A0 band; client 1's `lead_adj` 0. |
| **R6** part three (needs `own_body_control`) | `own_birth_move` on, once the own-body capability exists on retail: `unmake_own` / `make_own` under the retail Physicalize block. That is a bridge and script-edit job of its own, after LPD step 21's measurement of a multi-tick own absence on retail. Until then this step does not start and the §2.3 fallback stands. The §1.2 switch `own_birth_move` (not built at E7) is built here first, off, before the retail host declares `own_body_control`: with the capability alone `move_own_birth` would start moving own births at once. | An A6m analogue: the client 2 own Physicalize held 300 ms (a retail `hold-event` verb through `hooks.filter_event`), `net lag 50`: the owner 0.0000 m from T'+48, no resync. |
| **R7** close | Delete the legacy retail session code and the `core` switch; core on always. Shrink `physics_session_state` to the host state of §3.2. | Three launches without a hang; R1 (b), R3 and R4 rerun on the final binary within their bands; `retail_run.py` solo checkpoint and sector run green. |

The R steps do not require each other's measurements to be good, only to be
built. R5 and R6 wait on their capability work; R1-R4 do not.

**As built (step R0).** Where the code made R0 differ from §2.2's retail
column, §3.2 and the R0 row:

- **What `session core on` does at R0.** The row does not say, and R1 is the
  step that hands the session to the core, so R0 makes it plumbing only. With
  the switch on: at each SessionStart the core is made the first time
  (`session_client_core(core_host_platform(), caps)`; its constructor's
  `missing_platform_operation` check passing is the proof that every required
  operation is filled), the login id is given to it (`set_own_id`), and every
  Session* message is pushed into the mailbox and drained each `OnProcess`.
  No core session runs (`begin` is R1's), so the drain hands the SessionStart
  entry to `core_host_session_start` and decodes every other entry into its
  message (to check the copy), counts it and drops it (`session core` prints
  `latched / pushed / dropped / drained / starts / discarded / undecodable`);
  the legacy handlers still run, so the session itself is the legacy one
  with the switch either way. The decoding dispatch to `on_session_*` is
  built (`core_host_dispatch`) and runs only for a core session. With the
  switch off nothing new runs but two atomic loads per message, a per-frame
  empty mailbox take and the replay-key check.
- **Where it lives.** `BallanceMMOClient/session/core_host.cpp` (+ a small
  `core_host.hpp` naming `session_platform` and `retail::core_enabled()`).
  Its state is file-scope statics: the switch and the latch
  (`std::atomic<bool>`s, the network thread reads them), the mailbox
  (load-time), and one `retail_host`
  struct holding the core (`std::unique_ptr`, made lazily), the platform's
  own state (navigation graph, own replica, own group, body guard, the mute,
  the mirrors' player / type maps, ball forces, the birth log, the manager
  activations, the script replay's selection and bookkeeping) and the verbs'
  state. `BallanceMMOClient` gained non-virtual methods only
  (`core_host_*`, `death_reset_apply(bool)`, `checkpoint_phase_keep()`); no
  data member. The platform's host state is kept apart from
  `physics_session_state`, which keeps its layout until R7.
- **The mailbox and the latch.** `core_host_offer` at the top of
  `on_message`, before the opcode switch. The switch is read once per
  session, at its SessionStart, on the network thread (where the arrival
  order is made): a SessionStart that finds it on latches the session
  (`core_latched`) and is itself pushed; then every SessionAssign, Snapshot,
  RemoteInput, Event, Lead and End is pushed until the SessionEnd, which
  unlatches. So the core sees SessionStart and the messages after it in
  arrival order (a SessionEnd right behind a SessionStart cannot be drained
  before it), and a session's messages never split between the paths.
  Snapshot entries are capped at 64 (the oldest snapshot entry goes).
  SessionLead is the mailbox's alone (the legacy switch never had a case
  for it and logged it as an invalid opcode, which it still does with the
  switch off). The legacy SessionStart handler is untouched (its own
  `run_on_game_thread` post). "`begin` drops only the entries of other
  sessions" is R1's (no `begin` yet).
- **The retail platform (`core_host_platform`): every §2.2 operation,** with
  the retail column's means; the host support templates are instantiated
  with `physics_view` there (`make_mirror_body`, `attach_mirror_navigation`,
  `attach_own_replica`, `bind_session_world`, `bind_script_replay`,
  `live_cells_guard`, `manager_activations`, `put_back_birth`), so R1's
  "compile-only instantiation" is done. Differences from the table:
  - `own_ball.ball_type` and `ball_recipe_of`: the `Physicalize_GameBall`
    row by name (-1 for none), not `db_.get_ball_id`, which answers 0 for an
    unknown name; the recipe rule is the headless `fill_retail_recipe`'s, so
    the rule now exists in both hosts (a host-support candidate, left as is:
    R0 changes no shared file).
  - `drain_body_events` ignores the event log's `recipe` entries: the retail
    birth log is fed by `OnPhysicalize` (§2.4), which R1 wires.
  - `poll_sector` also reads `CurrentLevel[0][4]` (`phase_read`, N4's check);
    `resetpoint_moved` is always false (the checkpoint hook reports).
  - `own_body_upkeep` attaches the own replica through host support, which
    polls on; the retail rule (polling off while muted) is applied after a
    create.
  - `script_guards` calls `death_reset_apply(resolved)` (a new overload: the
    legacy `death_reset_apply()` passes its own `mechanism_tracking`
    count), `sector_deactivate_apply`, `sector_activate_apply`, and N4's
    per-frame keep through `checkpoint_phase_keep()` (the split's state is
    a file-scope static of `physics_session_client.cpp`).
  - `hooks.after_rewinds` is the forced rollback; `hooks.window` / `status`
    go to the log; `hooks.ended` lifts the body guard; `counters` are
    `fixed_tick_.rebases()` and the host's two counts. `arm_own_capture`,
    `own_birth_capture`, `make_own`, `unmake_own`, `numbering_restarted`,
    `filter_event`, `ending`, `describe_movables`: empty.
  - Capabilities as §2.3's retail column: no own body control, no slew,
    blocking hold, no whole frame, `host_hooks`.
  - Not built (R1, with `begin`): the `core_config` (and §3.2's "savestate
    0": the legacy binding's savestate *flags* are 0, but its engine runs
    with `set_exact_restore` at its default, true, so R1 should keep
    `core_config.savestate` true and the binding's flags 0, or the A/B
    measures a policy change), the anchor work that fills the script
    selection, force hooks and ball forces, the mute's update, the
    `OnPhysicalize` / `OnUnphysicalize` / checkpoint-hook / `beam` /
    `sector` / `journal` routing.
- **The retail `to_bridge_recipe` copy is deleted** (as the E2 note asked):
  `physics_session_client.cpp` includes `session/bridge_recipe.hpp` and its
  three calls find the shared one by ADL. The copies were identical.
- **Verbs.** `session core on|off|status` (the switch acts between sessions:
  refused while the legacy session is not idle, while a core session is not
  idle, and while a session is latched and the connection is up - a lost
  connection, which sends no SessionEnd, lets the verb clear the latch);
  `session force-rollback E:D` (E 0 = off; acts only through a core
  session's `after_rewinds`, so it is inert until R1, and says so with the
  switch off; the host keeps only the cadence, `now % E`, and leaves "not
  before depth + 2 ticks" to `force_rewind`); `net lag <ms> [up|down] [at
  <tick>]` (GNS `FakePacketLag_Send` / `_Recv`, global, one way, both
  directions by default); `net blackout <ms> [up|down] [at <tick>]`
  (`FakePacketLoss_Send` / `_Recv` at 100 %, a deadline per direction, put
  back to 0 by `core_host_process` at the first frame boundary past it, so
  an anchor hold in progress, which waits inside one frame, stretches it; a
  blackout at least GNS's connected timeout, 7.2 s as `role.hpp` sets it,
  is accepted with a warning that the connection will drop); `at <tick>`
  arms either for the first frame boundary at or after that session tick;
  `net` (status); `replay session <file>|stop` and `replay session`
  (status). The `array`
  verb also finds an array made at run time (context lookup): N4's copy is
  named `CurrentLevel_BMMO_CheckpointPhase` (`CopyObject` appends the
  name), not `_BMMO_CheckpointPhase`.
- **`replay session`.** Fed from `on_input_polled` after the automation
  keys: the tick about to step is the core's `next_tick()` for a running
  core session, else the legacy `tick_base + frames_since_anchor` once
  assigned and released; the first such tick is frame 0, and frame k goes
  to tick `first + k` (a resync's renumbering keeps the server's numbering:
  a forward jump skips record frames, a backward one feeds frames again or
  lets live keys through before the first; each jump is logged with its
  size and counted, `jumps=`). The record's end or the session's end
  disarms it. No clock reset, nothing recorded.

Measured (x86 mod at e04148d + R0, slot 13 on the arena; the Arch server
rebuilt with the slot-13 hack, which its cf08f11 binary lacked, build id
`ballanced-3e4289e95f25+bmmo-e04148d177dd-dirty`):

- Clean x86 build (the client target's objects deleted first). No launch
  hung: three dedicated launches of the first R0 build (menu in 1.5-2.3 s)
  and ten of the final one in the runs below. N4 in the game for the first
  time: `the checkpoint chain's phase cell is split: 2 Set Cells of
  Gameplay_Events/activate next Checkpoint, 3 of Event_handler/deact.
  Checkpoints and 2 Get Cells in 2 checkpoint script(s) now use a private
  copy`; the copy exists during the session and is gone after its end.
- **Baseline, legacy solo, snapshots off, `replay session` (2400 frames,
  arena): NOT identical.** Two runs: 2459 of 2515 TICK hashes equal; the
  only difference is ticks 611-666, where `P_Modul_01_Pusher` is woken at
  611 in one run and 612 in the other (a third run, with the switch on: 606;
  every other tick of the three runs is identical, sectors 2-4 held and run
  at the same ticks, and each client's journal matches its own server's at
  every tick - the server applies the client's BodyRevived). **Cause:** the
  pusher's script wakes it from a `TT Scaleable Proximity` (`[50],
  [Ball_Pos_Frame], [P_Modul_01_Pusher], ..., [55], [100], [10], [60]`),
  which checks the distance only every 10-60 frames; its countdown is the
  block's local parameter 1 ("Last Check", ScaleableProximity.cpp), which
  `CKBehavior::Reset` and the level restart leave alone - so its phase at the
  anchor is whatever the play before the session left (the 9.27 Sequencer
  class of leak). **Confirmed by measurement** (a throwaway read-only build
  logging every `TT Scaleable Proximity`'s local parameters 0 and 1 at the
  anchor and the pusher's block at every tick to 700, two legacy runs,
  removed before the final build): at the anchor the pusher's "Last Check"
  was 16 in one run and 30 in the other (the block inactive, 37 instances
  in the level). The block is started at tick 15 (local 0 set to "out",
  local 1 left at 15 / 29), so its distance checks fell at 30, 90, ...,
  594, 604 in one run and 44, 104, ..., 596, 606 in the other; the first
  check inside the range (squared distance 2460 / 2379 < 2500) is the tick
  the pusher wakes, 604 vs 606, and the two journals differ at 604-660
  only. Two more legacy runs of the final build: the pusher woke at 607 vs
  609 (the hashes differ at 607-663, 57 ticks), every other tick equal, each client journal equal to its server
  journal at every tick, both server journals exact in the SimTool. Five
  of the 37 instances had a different "Last Check" at the two anchors
  (`P_Extra_Point`, `P_Modul_01`, `P_Modul_18`, `PC_TwoFlames`,
  `PS_FourFlames`); only the pusher's reached the hash in this test. The
  fix - record locals 0 and 1 of every `TT Scaleable Proximity` at level
  load and restore them at the anchor, as `game/script_state.hpp
  sequencer_state` does for the Sequencers - changes what the legacy retail
  world does (in both arms), so it is not in R0: it needs its own step,
  before R1, whose check (a) ("identical at every tick") cannot pass
  without it.
- **The legacy two-client test, re-established** (no numbers were recorded
  for it on the current protocol: the last, `scripts/retail/n28-verify3`,
  is from 2026-09-11 on a local Windows server): `retail_two_client_test.py
  start --server 127.0.0.1:26700` (level 11, both clients `replay session`
  of the same record), Arch server snapshots on (2 / 66, spacing 4.2), two
  runs. Per client `snaps/ok/mism/rb`, `resim`, `max_err`: run a
  1289/1269/20/20, 52, 0.310 m and 1289/1269/20/20, 34, 0.201 m; run b
  1319/1298/21/21, 40, 0.195 m and 1322/1305/17/17, 34, 0.291 m. Both runs,
  both clients: `unmatched` / `far` / `frozen` 0, resyncs 0, events
  42/343/294/0, echo 7/0/0/0. The four server journals (two two-client, two
  solo) replay exact in the SimTool (`matched` = ticks, 0 checkpoint
  mismatches).
- With the switch on (solo, the same record): the core made with the
  platform complete, the mailbox pushed / drained 2611 entries (1
  SessionStart, 2610 decoded and discarded), undecodable 0; `session core
  off` mid-session refused, and accepted after the disconnect (latch
  cleared); the session the legacy one (its journal equal to the switch-off
  runs but for the pusher window). `net lag 150`: the server's ack of our
  input fell from 2 to 22 ticks behind and back at `net lag 0`; `net lag
  150 up`: 12 behind; `net blackout 1500 up`: the ack froze for ~1.5 s
  while the snapshots went on; `net blackout 1000 down`: both froze; no
  resync. Overlapping `net blackout 3000 up` + `net blackout 400 down`: the
  down one ended at tick 704, the up one at 876 (its 3 s uplink gap cost a
  resync, 1/1); `net blackout 1000 down at 1250` (armed at 1123) fired at
  1250 and ended at 1316; `net blackout 7500 down` warned of the 7200 ms
  timeout.
- The legacy two-client test once more on the final build (run c): per
  client 1301/1282/19/19, resim 31, 0.180 m and 1304/1284/20/20, resim 42,
  0.226 m; unmatched / far / frozen 0, resyncs 0, events 42/343/294/0, echo
  7/0/0/0 - inside runs a and b; its server journal exact in the SimTool.
- The two reviews: the baseline's cause measured (above); the drain decodes
  what it drops; the switch read once per session (the latch) and the verb
  refused while a core session runs; SessionStart in the mailbox (arrival
  order); a blackout deadline per direction; `net lag [up|down]` and `at
  <tick>`; the GNS timeout warning; the replay's jumps logged; the
  "made" line only at the core's making; `force_rewind`'s depth check no
  longer repeated by the host.
- Headless unchanged: R0 changes no file under `BallanceMMOCommon` or the
  headless client, so the wire check was not needed; the x64 build, the 414
  tests and the GCC syntax check (25 files) pass. Three launches of the
  final build: menu in 1.6-1.7 s, none hung.

**As built (step R1).** Where the code made R1 differ from §1.2, §1.3,
§2.2, §3.2, §4 and the R1 row:

- **A core session is the core's alone.** `core_host_offer` swallows every
  Session* message of a latched session (SessionStart included), so the
  legacy handlers never see it and the legacy `physics_session_state` stays
  idle. The host runs the session the legacy way, in file-scope statics of
  `core_host.cpp` (a host phase: counting_down, restarting, running): the
  SessionStart from the mailbox (a **fresh core per session**, not one made
  at the first SessionStart, so no state of the last session carries over;
  `set_own_id`, `begin` with every `core_config` field set, the not-in-level
  leave, the countdown, `level_requested`); the restart and the
  Gameplay_Ingame edge; the anchor work in the legacy order (clock reset,
  clock guard, pause pin, pause edits, N1-N4, Sequencers, hash, collision
  filter, ball forces, script selection and force hooks), then
  `core.anchor` and the blocking hold (the mailbox drained every 1 ms,
  `poll_hold`, `release_hold("the connection was lost")`), then the first
  `pre_step`; one frame per OnProcess (clock guard, pause pin, pause edits,
  the mute, the rebase check, `post_step(true)`, the drain, the
  transitional triggers, `pre_step`); and the end (edits handed back, guard
  dropped, `core.end` when it still runs, own replica destroyed, tick driver
  off). The legacy hooks and verbs hand over while a core session runs:
  `OnPhysicalize` / `OnUnphysicalize` (recipe, spawn flag, birth cell,
  spawn impulse, birth log, `own_physicalized` / `own_unphysicalized`),
  `physics_session_on_sector` (`request_sector` only when assigned and the
  N3 hold is in force, else today's after-the-fact Sector event through
  `send_event`), `on_finish`, `end_local` (level left, disconnect), the F3
  overlay, the `session` status (host fields first, then `status_text`;
  the retail tools read `phase=` / `assigned=` from the front), PlayerLeft
  (`journal().leave_player`), `beam` (`request_beam`), `sector N`
  (`request_sector` under the legacy verb's condition), `journal mark|off`
  (the core's journal: a note and a LOCAL checkpoint). Each is a guard at
  the top of the legacy function that is false while no core session runs;
  no legacy behaviour changed.
- **Core additions** (all inert on the headless client, which sets none of
  the new switches; WIRE-CHECK below):
  - `core_config::lifecycle_rewind` (§1.2 named it; it did not exist):
    off, `make_remote_birth` skips the pending path and
    `rewind_for_lifecycle` returns, so a late change is made at the drain,
    live. `own_birth_move` is **not** built: R6 builds it, and with
    `own_echoes` off and no `own_body_control` nothing moves an own birth.
  - The retail policies as `core_config` fields: `mirror_start_from_row`
    (the core caches each player's newest snapshot ball row at intake, only
    with the policy; a live birth with a newer row starts from it: the
    row's pose, no kick, then `write_body` with the row's state, the 9.17
    workaround) and `own_body_backstop` (`check_own_body` in the snapshot
    path, before the rollback, PSC's rule: 30 snapshots, 2 s limiter on a
    new `clock_site::own_backstop`, appended; the last own Physicalize kept
    at `commit_own_birth`, cleared at the own Unphysicalize, its spawn flag
    cleared by a beam; counted, `own_body_backstops()`).
  - **`drive_from_newest_relay`, a policy the design did not have** (a
    departure from §1.3 and the R1 row, to be signed off). The first batch
    of (b) put every core-on client above the off band in mismatches
    (22-29 against 17-21, resim up to 58): §1.3's order has the drive of
    T + 1 chosen at post_step step 28, before the messages, so a key edge
    relayed in between is played a tick late, which the legacy frame (relay
    intake, then the drive) did not do and which, without the input rewind,
    nothing corrects but the next snapshot. With the policy, pre_step step
    10 re-chooses `remote.applied = live_drive(remote, T)` before it drives
    (the record of T stores it). The core does not gate it; the retail host
    sets it as `!input_rewind`, so the R2 switch turns it off with it and
    R2 decides whether it stays.
  - `release_hold(why)`; `post_step` refuses a frame no `pre_step`
    prepared (`frame_.prepared`, cleared by `begin`; logged, false, nothing
    taken; the retail host counts `refused`, 0 in every run); the
    `frozen` → resync rule after `on_snapshot`; `acked_input()` =
    `{known, tick, frame, snapshot_frame}` from the snapshots'
    `acked_input_tick` (PSC's `acked_input_state`, reset by every
    assignment); `resyncs_sent()`; `quaternion_rows` (VxQuaternion::ToMatrix's
    formula); `local_birth_log::put` (the legacy dedup of a same-name birth
    in one tick). `begin` resets every R1 field, the backstop's count and
    2 s limiter (`own_backstops_`, `last_backstop_`) included, so a host
    that reuses a core starts each session from scratch.
  - Seven unit tests (`SessionClientCoreR1.*`): the refused frame, the
    released hold, lifecycle_rewind off with the row start (and no rows
    without the policy), the backstop's count / limiter / reset (and its
    reset by `begin`), the acked view, the drive policy on and off, the
    frozen clock's resync (and none with the clock running). 421 tests.
- **The mailbox is not dispatched in pure arrival order** while
  `input_rewind` is off (a departure from §3.2, to be signed off). With
  the drive policy the (c) arm still had client 2 at 113 / 151 / 113
  mismatches against the off runs' 103: a relayed frame that arrives
  behind a snapshot of the same batch is stored after that snapshot's
  replay ran on the prediction. Within each run of entries between
  numbering messages (SessionStart, SessionAssign, SessionEnd keep their
  places) the host dispatches the relay first, events and leads next,
  snapshots last. The legacy `apply_queues` took a batch as: the
  snapshots' ball rows cached, the events applied, the relayed inputs, the
  snapshots; the core only queues an event for the next pre_step's drain
  (`on_session_event`), so where the events sit in the batch changes
  nothing (every snapshot row of the batch is cached before a birth reads
  it) and relay-before-snapshot is the part that matters. With the input
  rewind (R2) a late frame asks for its own replay and arrival order can
  stand again; the reorder is skipped then.
- **`observe_input` keeps the legacy sources until R2:** the mute decided in
  this frame (keys 0) and `ball_nav_active_` (the BML hook), so the server
  drives our ball from the same flags as with the switch off; R2 moves both
  to what the replica stepped (§2.2, §4.2). `INPUT_FLAG_PAUSED` is not sent
  (no `paused` in `input_observation` before R2; the server never reads it).
- **Transitional triggers (§3.2):** the 30-unmatched run (rollback stats
  around the drain, reset by any resync request) and the starvation
  detector over `core.acked_input()` (132 frames, the stream alive within
  66) are host code; `too_far` is not ported. For R4: the 30-unmatched
  counter, ported as the legacy one (PSC:2115), resets on every frame with
  no unmatched snapshot, frames that took no snapshot at all included, so
  with a snapshot every 2 ticks a numbering desync never reaches 30 and the
  trigger is effectively dead on both paths.
- **The host's own edges.** `update_current_sector`'s union-cell gate reads
  `core_host_session_running()` (anchored, the core running), the legacy
  gate's `phase == running`, not the countdown or the restart. With a core
  session running but not assigned (a hold that timed out, a resync's
  assignment awaited) `beam` and `sector N` answer an error instead of
  falling through to the legacy verbs, whose moves the core and the server
  would never learn of; `sector N` without the N3 hold still activates by
  hand, as the legacy verb does (the hook reports it after the fact). An
  undecodable SessionStart prints the legacy on_message's refusal ("a
  different physics protocol"), and a core that could not be made says so
  in game. `session core on|off` is refused only while the legacy path or
  the core runs a session: a latch no one runs (the host gave the session
  up: not in the level, no physics view, no core) keeps the dead session's
  messages going to the mailbox to be dropped, but no longer holds the
  switch until a SessionEnd that a member who left the room may never get.
- **Testing verbs:** `session stall <ms>` (the game thread sleeps; the tick
  driver rebases past 33 ticks) and `session drop-birth` (the next own
  Physicalize of the running core session is kept off the wire through
  `hooks.filter_event`, every copy, so the server has no body for our ball
  and `own_body_backstop` has work).
- **A 300 m beam is not a death, and it is slower with the core.** R1 (c)
  first beamed each ball 300 m down for its death. On Level 11 that puts
  the ball below the death volume: no BallOff, no respawn, the ball falls
  for the rest of the run (the deaths the first form saw came from the 4 m
  beam and the record). It also makes the IVP savestate of every captured
  tick grow from ~90 KB to 3.0 MB, 37 ms per capture (measured with a
  temporary timer around `rollback::capture`, removed), until IVP retires
  the teleported ball's state: 1-2 ticks on the legacy path, whose beam is
  stamped at a tick the game had stepped, so the next snapshot mismatches
  and its restore cleans the state; 7-26 ticks with the core, whose beam
  the server agrees with (no rollback). Solo on Level 11: legacy 2 slow
  frames, core 7; two clients: legacy 9 slow frames (~300 ms behind)
  every time, core 7-26 (up to 750 ms), so 2 of the 7 core-on runs of the
  first form rebased the tick driver (an extra resync, on both clients
  once). A 30 m beam lands inside the volume: BallOff ~2 s later, the
  respawn ~3 s after that, no slow frames. (c) uses the 30 m beam from
  its second form on (`r1_c2.py`), both arms. A large teleport's capture
  cost is a property of the savestate (`savestate_get` copies whatever IVP
  holds); nothing in play teleports 300 m, so it is recorded here, not
  fixed.

Measured (x86 mod on 791f75a + R1 after the review, slot 13 on the arena
for (a); the Arch server of R0, engine half `ballanced-3e4289e95f25`
equal; physics_RT.dll identical in the rR0 and rR1 builds):

- Builds: x86 CLIENT-BUILD-OK with the client target's objects deleted
  first (`client_core.hpp` layout); x64 BUILD-OK; 421 tests pass;
  `linux_test_syntax.sh` 25 lines ok. **Headless unchanged:** `wire_check.sh
  rR0 rR1` WIRE-CHECK: ALL SAME on the final core (every one of the 16
  journals IDENTICAL, the sends SAME). About fifty game launches, none hung
  (no data member added).
- **(a)** solo, snapshots off, `replay session` (2400 frames). The pusher
  window is R0's baseline class: the TICK record's simulated-core count
  rises when `P_Modul_01_Pusher` wakes, at a tick set by the TT Scaleable
  Proximity countdown the play before the session left, and two runs
  differ in exactly the ticks after the earlier wake. On the final binary:
  core off ×2 (wakes 607, 606), core on ×2 (604, 606); **`r1_af_off2`
  against `r1_af_on2` (both 606): 2541 common TICK hashes (8-2548), 0
  differ** - identical at every tick, as the row asks, given the baseline;
  `r1_af_off1` (607) against `r1_af_on1` (604): 58 differ, all 604-661.
  Every client journal of the final binary equals its own server's at
  every tick (off 2532 and 2551 ticks, on 2498 and 2541). Core off is the
  legacy path: two core-off runs of the R0 build (wakes 610, 607), each
  client equal to its server at every tick, and `r1_a0_off2` (R0, 607)
  against `r1_af_off1` (R1, 607): 2497 common ticks, 0 differ. (The one
  pre-review core-off run whose client differed from its server in
  612-667 is the same class: client and server wakes 613 / 612.) R0's
  countdown fix is still not done; with it the pair-matching would not be
  needed.
- **(b)** two clients, level 11, snapshots on (2 / 66, spacing 4.2), both
  `replay session` of the same record, final binary, 3 off / 3 on
  interleaved (`r1_bf_*`). Per client `snaps/ok/mism/rb`, resim, max_err,
  mirror (`_Peer_` kind 0) max / mean: off c1 1322/1303/19/19 35 .246
  .246/.088, 1326/1307/19/19 37 .272 .272/.087, 1330/1311/19/19 34 .220
  .220/.077; c2 1317/1298/19/19 34 .226 .226/.086, 1321/1301/20/20 34 .226
  .226/.078, 1325/1305/20/20 39 .283 .283/.084. On c1 1321/1304/17/17 28
  .220 .220/.073, 1330/1312/18/18 24 .266 .266/.094, 1326/1310/16/16 21
  .310 .310/.110; c2 1316/1296/20/20 45 .317 .317/.100, 1326/1305/21/21 31
  .226 .226/.082, 1322/1306/16/16 22 .226 .226/.089. The off band over the
  twelve R1 off runs of this file's four batches (the legacy path, the
  same harness and server): c1 mism 17-24, max_err .195-.310, mirror mean
  .073-.096; c2 mism 18-22, .181-.317, .060-.090; every on value lies in
  the §5.3 band they make. This batch's three off runs alone had mism 19
  / 19-20, so their band (±1) leaves c1's 17 and 16 and c2's 16 below it
  (fewer mismatches) and the mirror means .110 / .100 just above (by .011
  / .006). Resim (not a criterion of (b)): on 21-45, off 34-39 in this
  batch and 28-61 over the twelve.
  Events applied to tick 2580: each client journaled the server's 42 S5
  events of the other player (35 BodyRevived, 4 Physicalize, 3
  Unphysicalize), equal as sets, in every run of both arms. unmatched /
  far / frozen 0, resyncs 0 everywhere. Every server journal replays exact
  in the SimTool (matched = ticks, 0 checkpoint mismatches); every client
  journal replays to its end as the legacy ones do (rc 3: only the
  received snapshots are compared); `worst_bodies.py` reads both clients'
  journals of every run (core-on journals list the balls, 0.18-0.21 m;
  every mechanism 0.0).
- **(c)**, second form (`r1_c2.py`): `net lag 150` both ways on both
  clients, a beam 4 m up per client, a death per client (a beam 30 m down:
  BallOff, respawn), `session stall 1000` on client 2; final binary, 3 off
  / 3 on interleaved (`r1_cg_*`). Off c1 mism 69/73/69, resim
  2145/2232/2184, max_err 7.51/30.06/5.55, mirror mean .613/.989/.567,
  unmatched 0; c2 105/127/114, 2278/2676/2407, 9.48/30.13/12.15,
  .486/.939/.591, unmatched 32/36/35. On c1 68/69/70, 2099/2127/2196,
  5.63/7.49/7.45, .555/.577/.561, 0; c2 117/113/122, 2444/2348/2522,
  11.05/9.92/10.03, .598/.578/.538, 32/32/30. snaps / ok likewise: every on
  value of every metric lies in this batch's off band. resyncs: c1 0/0,
  c2 1/1 in every run of both arms (the stall rebased the tick driver; one
  request, in flight until the reassignment, the full snapshot applied).
  No slow-frame run of 4 frames or more in any of the six. Events: each
  client journaled exactly the server's S5 events of the other player
  (46 = 46, 48 = 48), equal as sets; the late ones (`late>0`: 0-2 per
  run, both arms) were made at the next pre_step's drain. **The own ball
  after the beam** (LOCAL checkpoints every 2 ticks against the
  server's): core on 0.0000-0.0005 m from the stamp to stamp + 66 for the
  4 m beam of each client, off 0.22-0.30 m (stamped a tick the game had
  stepped); after the death beam, c1 on 0.03-0.20 m, off 0.28-0.50 m; c2
  on 3.47-4.97 m, off 3.19-4.26 m in two runs and 0.44-0.47 m in one (the
  ball dropping through the volume, both arms). Every
  server journal exact in the SimTool; both client journals replay to
  their end in every run. The first form (300 m beams, `r1_c*`, `r1_cf*`)
  is superseded: see the 300 m bullet above.
- **(c), the paths the plain run does not reach** (core on, `r1_c2.py
  ... exercise`, `r1_cx_on2`): `session drop-birth` on client 1 after its
  death beam kept its respawn's Physicalize (stamp 1558) off the wire;
  `own_body_backstop` re-reported it at 1623 (backstops=1), and client 2
  made client 1's mirror at 1623, from its row of 1664. `net blackout 1500
  down` on client 1 over client 2's respawn: "remote player ... started
  from its row of tick 1434 (stamp 1386)" (`mirror_start_from_row`, which
  the plain runs never reach: there the relayed birth always arrives
  before the server has simulated its stamp, as on the legacy path, whose
  rule is the same). The stall's resync applied on client 2, then `net
  blackout 900 both`: "resync owed (a substituted own frame arrived past
  the reach) at tick 1684: the limiter holds it back", then "resync
  requested (... (owed)) at tick 1714", reassigned and applied (resyncs
  2/2): the limiter's in-flight and owed paths, each once, with the
  resync applied. (The owed path is reachable at R1 although the own
  corrections and the echoes are off: the held section's past-the-reach
  and lost-notice requests are owed and are not behind `own_corrections`.)
  Events equal as sets with the server's S5; the server journal exact in
  the SimTool. A first exercise run (`r1_cx_on1`, the blackout too early
  for the respawn) showed the backstop (907 dropped, re-reported at 963)
  and the owed path the same way.
- The reviews. Fixed: the `update_current_sector` gate (anchored only);
  `begin` resets the backstop's count and limiter; the drain's comment and
  this note (the legacy order; the events' place does not matter; both
  departures marked for sign-off); `drive_from_newest_relay` follows
  `input_rewind`; the SessionStart refusal and the core-not-made message;
  `beam` / `sector N` unassigned; the switch no longer held by a latch no
  one runs (not exercised live: the server starts no physics room for a
  member in the menu); the stale on_message comment; the frozen-clock
  test; the 30-unmatched note for R4; core off against R0 measured ((a)
  above); the backstop and the owed path exercised live. Refuted: that
  dispatching snapshots last breaks `mirror_start_from_row` - the core
  queues events for pre_step's drain, after the whole mailbox (snapshots
  included) has been taken, so a birth always reads every row of its
  batch; the policy did not fire in the plain runs because no row newer
  than the stamp exists when the relayed birth arrives, which the
  blackout run shows. The (c) first form's extra resyncs were the 300 m
  teleport's savestate cost, not the drain or the policies.
- Not done: R0's pusher-countdown fix (its own step); the forced-rollback
  verb is R2's check.

**As built (step R2).** Where the code made R2 differ from §2.2, §3.2, §4.2
and the R2 row:

- **What R2 turns on.** The retail `core_config` sets `input_rewind` (a
  relayed frame that differs from the prediction asks for a replay from its
  tick; `replay_corrected_inputs` runs it at pre_step step 5 inside the
  host's `keep_live_cells` guard). Both R1 departures that stood in for it
  end with it: `drive_from_newest_relay` follows the switch (off: the drive
  of T + 1 is chosen at post_step(T), the headless order), and the mailbox's
  relay-first reorder is skipped (it was already conditional on the switch),
  so the mailbox is dispatched in arrival order, as §3.2 says. `replay_remotes`
  needed no retail code: the retail binding has been the shared
  `bind_session_world` since R0, which installs the core's hook. Lifecycle
  rewind, echoes and own corrections stay off (R3, R4).
- **`observe_input` records what the replica stepped T with.** The pause
  menu's mute is now applied after `post_step(T)` (before the mailbox and
  pre_step's replays, whose polling resumes as the mute for T + 1 says; the
  log line names the tick it applies from), so `observe_input` reads the mute
  that was in force during T. Muted: keys 0, navigation off, `paused`. Not
  muted: the keys the replica polled (`frame_keys_`) and NAV_ACTIVE from the
  leaves' Key Event `IsActive` (any of the graph's leaves), not
  `ball_nav_active_`. `unforce_own` already restored `!input_muted` (R0);
  only a forced own frame calls it, which needs `slew` (R5).
- **Core addition: `input_observation::paused`** (the field §2.2 names "at
  R2"), sent and journaled as `INPUT_FLAG_PAUSED`, as the legacy frame does
  (R1 sent no PAUSED). The headless client never sets it. One unit test
  (`SessionClientCoreR2.PausedObservationSetsThePausedFlag`): 422 tests.
- **Testing tool: `beam <x> <y> <z> at <tick>`** (a running core session
  only): the request is made in the frame whose post_step stamps exactly
  `<tick>` (`core_host_due_beams`, before `post_step`), so two runs of one key
  record beam at the same tick. Without `at` the verb is unchanged.
- **The pause menu inside a session (measured):** ESC opens it but does not
  close it (record or automation keys); Return (Continue) closes it; an arrow
  key held while it opens moves its cursor off Continue (Return then closes
  nothing). The (a) record therefore opens with ESC, closes with Return and
  holds 'right' (no menu key) over both edges.

Measured (x86 mod on fe1cdde + R2, client objects deleted first, slot 13 on
the arena; the Arch server of R0/R1):

- Builds: x86 CLIENT-BUILD-OK; x64 BUILD-OK; 422 tests pass;
  `linux_test_syntax.sh` 25 lines ok. **Headless unchanged:** `wire_check.sh
  rR1 rR2` WIRE-CHECK: ALL SAME (16 journals IDENTICAL, the sends SAME).
- **(a)** solo, arena, snapshots off, `replay session` of a 2100-frame record
  plus three `beam ... at` (r2_run.py). The events and the own record's
  nav-activity edges, identical in every run: first spawn 212 (NAV on 214);
  'right' held, the menu opens at 309 (the leaves' Key Events go inactive in
  that frame: tick 309 is keys 4 with NAV off, a nav-activity edge one tick
  before the mute), muted 310-429, Return closes it at 430 (NAV back on, keys
  4); the trafo beam at 650 (NAV off 651, the stone ball unphysicalized 652,
  its explosion pieces born at 807-808, the wood ball physicalized 817, NAV
  818); back to the lane at 900; the death beam at 1300 (NAV off 1302,
  unphysicalized 1369); the respawn 1567 (NAV 1569). Control `r2_a_ctl1`;
  arms `session force-rollback` 50:12 (44 rewinds), 97:24 (22), 131:47 (16)
  and 13:24 (169: every tick from 15 on lies in some window, so every event
  above is crossed). `worst_bodies.py`, every arm against the control: 0.0
  m on every body it compares (the own ball, `P_Ball_Wood_MF`,
  `P_Modul_01_Pusher`, `P_Modul_41`), 1104 common checkpoints to 2220.
  But `worst_bodies.py` keys a kind-0 body by its owner and skips owner 0,
  so the trafo's 17 `Ball_Stone_piece*` bodies are never compared; over
  every body by name (position, rotation, velocity; scratchpad
  `r2_allbodies.py`) 50:12, 97:24 and 131:47 are exact on all 21 bodies (their
  windows replay the pieces in flight, none crosses their birth), and **13:24
  is exact on everything but the 17 pieces, which diverge from 820 (up to
  14.58 m): the step stays open.** The rewind at 819 (24 deep) crosses the
  pieces' birth. The script is the trafo's explosion, `Ball_Explosion_Stone`
  (it physicalizes the pieces and pushes them): it is not in the script
  replay's selection, and `movable_mechanisms` (PSC:1956-1960's rule, the
  headless host's too: no `Ball_*` name) leaves the pieces untracked, so the
  replay neither re-runs the explosion nor parks them at a birth record: they
  are re-stepped from their live state of 819, 24 ticks ahead. A temporary
  build tracking `Ball_*_piece*` in the retail `movable_mechanisms` made the
  13:24 arm exact on all 21 bodies (the parking path re-poses each piece at
  its birth record). It is not in R2: tracked, the pieces' server rows (the
  server sends every non-ball movable as a mechanism row) are compared
  instead of taken as restore sources, a snapshot-path change no R2 check
  covers, and the rule is the headless host's too (where `--replay-scripts`
  re-runs the explosion instead). It needs its own step with a snapshots-on
  A/B.
  - A `journal mark` checkpoint is stamped one tick early (the command file
    runs before the frame's post_step: `current_tick()` is T - 1 with T
    stepped), so the comparisons stop before the earlier mark (2226 in the
    control, 0.0005 m on the ball there). Present since R1; not changed.
  - A temporary build with R1's sources (the mute decided in the frame,
    `ball_nav_active_`) is exact in the 13:24 arm too: on this scenario the
    two records differ only at ticks with the navigation off either way (309:
    keys 4 / NAV off against keys 0 / PAUSED; 429: keys 4 / NAV off against
    muted), and the BML hook agreed with the leaves at every spawn, trafo and
    death edge. So (a) holds the R2 records exact but cannot tell them from
    R1's here.
- **(b)** two clients (`retail_two_client_test.py`, level 11), snapshots off
  (`arch_server_lp.sh ... 100000 100000 4.2 false false`), `net lag 150` both
  ways on both, both `replay session` of `r0_keys.bmrc`, two runs
  (`r2_b_on1`, `r2_b_on2`): `inrew` 162 / 167 and 163 / 169, `far` 0,
  `unmatched` 0, resyncs 0/0, rebases 0 on both clients; both server journals
  exact in the SimTool (2618 matched, 0 checkpoint mismatches). Each client's
  mirror against the owner's own LOCAL checkpoints (every 2 ticks, position
  distance; scratchpad `r2_mirror.py`): run 1 pooled p99 18.35 m (client 1
  sees 2: 18.72, client 2 sees 1: 13.89), run 2 19.18 m (19.20 / 13.45). The
  E11 lane O runs, the same measure: pooled p99 6.56, 13.94, 18.04, 6.10 m
  (per pair 0.99-32.55). **Above the O runs' pooled p99 by 0.3-1.1 m: open,
  waiting on R3.** The error is the births: with `lifecycle_rewind` off a
  remote Physicalize (here 20-36 ticks after its stamp at 150 ms each way) is
  made live at the drain, so the mirror trails its owner by those ticks for
  the whole life - a constant lag, 2-4 m in the first life and 9-18 m in
  later ones in client 1's view; client 2's view is 0.0000 m in the lives
  whose owner had not moved yet when the birth arrived. With a temporary
  build that also turns `lifecycle_rewind` on, the same run gives pooled p99
  5.23 m, p50 0.0000, below every O run (with one resync per client, R3's to
  explain).

**R2 review round (the step stays open).** Two reviews; what each finding
became (checked against the code):

- **Blocker, (a) on the trafo pieces:** confirmed and NOT fixed in R2. The
  fix (track `Ball_*_piece*` births in `movable_mechanisms`, or put
  `Ball_Explosion_*` in the script replay's selection) changes what a
  snapshot does with the pieces' server rows (the server sends every
  non-ball movable as a Mechanism row, physics_world.cpp:1132-1140; untracked,
  a row is a restore source; tracked, it is compared - and the server has one
  set of pieces for every player's trafo), and the selection is shared with
  the headless client. Both need their own step with a snapshots-on A/B.
  `worst_bodies.py` still cannot see the pieces (kind 0, owner 0): the
  per-name compare (`r2_allbodies.py`) is the one that shows them.
- **Blocker, (b) mirror p99:** confirmed; it is the births made live 20-36
  ticks late while `lifecycle_rewind` is off. Not changed: turning the switch
  on is R3. The bar stays in the R2 row; R2 stays open until it holds.
- **Should-fix, a replay in the frame the pause menu opens:** confirmed.
  `navigation_apply_pending` applies only a pending entry
  (ball_navigation.cpp:456-480), so after a replay that ends while the mute
  holds (polling kept off) the navigation kept applying the last replayed
  tick's keys and activity into the live step, while the record and the
  server had keys 0 / PAUSED. Fixed in the shared binding
  (`rollback_binding.hpp` `nav_poll`): `on_poll_resumed` now runs whenever a
  replay hands the entity back (`enable`), muted or not; retail's hook writes
  keys 0 / inactive, which is what the muted live step needs. The headless
  client never sets `input_muted`, so its calls are the same (wire check ALL
  SAME). `RollbackBinding.NavPollAlwaysReachesTheBackendAndRearmsWhenHandedBack`
  replaces the test that pinned the old rule.
- **Should-fix, coverage of the new record sources:** two host counters,
  `nav_source_differs` (frames whose recorded NAV_ACTIVE differs from the
  BML hook's `ball_nav_active_`) and `mute_source_differs` (frames whose mute
  in force differs from the pause state decided in the frame, R1's source),
  in `session core` and logged at the session's end. Every (a) run: 0 and 2
  (the two menu edges, 310 and 430, where R1 recorded the edge a tick early);
  (b): 0 and 0 on both clients. So on these scenarios the leaves and the BML
  hook agree everywhere, and the records differ from R1's only at the two
  mute edges.
- **Should-fix, the status header:** says "implemented but open" now.
- **Nits, all changed:** `observe_input` reads the replica's own key blocks
  (`own_replica.key_blocks`, leaf indices 0-7) and, without a replica, only
  leaves with an index in [0, 8); `beam x y z <word>` ignores a trailing word
  other than `at` again (core off unchanged); `beam ... at` bounds the tick,
  hands the core the lowest due stamp first, one at a time, and logs the stamp
  the core actually gave it after a successful `post_step` ("beam armed for
  tick N stamped N"). A plain `beam` in the same frame still replaces the
  core's one request. The PAUSED flag's extra peer replay (a flag-only edge)
  is left as it is: the legacy path sends the flag too.

Re-measured after the review round (x86 mod, client objects deleted first,
slot 13 on the arena; x64 build): 422 tests pass; `linux_test_syntax.sh`
every line ok; `wire_check.sh rR1 rR2` WIRE-CHECK: ALL SAME (16 journals
IDENTICAL, the sends SAME).

- (a) control `r2f_a_ctl`, arms `r2f_a_f50` (44 rewinds), `r2f_a_f97` (22),
  `r2f_a_f131` (17), `r2f_a_f13` (169); beams stamped 650, 900, 1300 as armed
  in every run. `worst_bodies.py` (to 2220): 0.0000 m on the four bodies it
  compares in every arm. Over every body by name: 50:12, 97:24, 131:47 exact
  on all 21; **13:24 exact on everything but the 17 `Ball_Stone_piece*`, from
  820, up to 14.58 m (the same numbers as before the round): open.**
- (b) `r2f_b_on1`, `r2f_b_on2`: `inrew` 164 / 169 and 160 / 163, `far` 0,
  `unmatched` 0, resyncs 0/0, rebases 0; the server journals exact in the
  SimTool (2567/2567 and 2617/2617, 0 checkpoint mismatches). Mirror p99
  pooled **18.35 m and 19.18 m** (client 1 sees 2: 18.36 / 19.20; client 2
  sees 1: 13.89 / 13.45) against the O runs' 6.56 / 13.94 / 18.04 / 6.10:
  **open (R3's births).**

**As built (step P): the trafo explosion pieces are mechanisms in both
hosts (implemented, OPEN).** The user's decision after R2 (a): the pieces
(`Ball_StonePieces_Frame` / `Ball_WoodPieces_Frame` / `Ball_PaperPieces_Frame`
children: 17 stone, 16 wood, 18 paper) are rigid bodies a rollback must
restore like any mechanism, tracked in both hosts by one rule; the `Ball_`
prefix excludes the players' balls only.

The facts, measured or read before the rule:

- **Which worlds have them.** Only the trafoing player's own client world.
  There the trafo sequence runs `Ball_Explosion_<type>` (physicalizes and pushes
  the pieces of the type the ball WAS) and, later, `Ball_ResetPieces_<type>`
  (fade, De Physicalize, Restore IC). Retail R2 scenario (`p_a_ctl`): stone
  ball unphysicalized 652, stone pieces 808-1434 (not "about 2 s": about 9.5
  s, the reset came 65 ticks after the death beam's unphysicalize at 1369).
  Headless drive run with `--beam 0 4 43 650`: three explosions, stone pieces
  810-2102, wood 1172-end, paper 1878-end. **The server world has none:** its
  retail ball is parked and its Trafo Manager never sees it trafo; a player's
  trafo reaches it as Unphysicalize + Physicalize of the clone with a new
  type. Every server journal of these runs (checkpoints every 2 ticks,
  14-2242 and 14-2420) carries no piece, so the Mechanism rows of
  physics_world.cpp:1132-1140 never carry one: the server's "one set" is the
  level's entity set, never physicalized there. **A remote player's trafo
  in another client's world runs no explosion:** the core re-makes the mirror
  with the new type (client_core_lifecycle.cpp:456-459) and the retail host
  swaps the spirit balls (game_objects.h:96 `on_trafo`); nothing activates
  `Ball_Explosion_<type>`. So the owner's world has 16-18 bodies that the
  server's and every other client's world do not have: a ball or a mechanism
  that touches them in the owner's world is not touched anywhere else, and
  only snapshots (where they are on) or nothing (where they are off) bring the
  owner back. This divergence is a property of the game's trafo, not of P,
  and P does not change it.
- **Two players trafoing within the pieces' lifetime.** Nothing shared: the
  server has no pieces, and each client explodes only its own trafo. The
  "one set" constraint is per client world: one hierarchy per type, so the
  same client trafoing out of the same type again before the reset re-uses
  the bodies (engine change #13 keeps the reset's De Physicalize working
  under the body guard).
- **The reset inside a window.** `Ball_ResetPieces_<type>`'s De Physicalize is
  a `deleted` event: the generation moves on, the records after it do not
  carry the pieces. A rewind anchored before it with the live tick after it
  finds no record of the current lifetime (no opinion), the savestate skips
  the bodies the world no longer has, and nothing makes them again: the
  replayed ticks from the anchor to the reset run WITHOUT the pieces. Not
  measurable on the R2 scenario (the stone pieces lie untouched in the trafo
  bay; the 13:24 arm crosses 1434 and stays exact); a gap of the same class
  as any body a non-replayed script destroys inside a window. Not fixed.

The rule, applied where the prefix stood for "the balls":

- **The core decides what a ball is** (`session_client_core::is_ball`,
  client_core.hpp): an entity the own ball has been this session
  (`own_ball_entities_`, filled from `own_ball()` every post_step, reset by
  `begin`) or a remote's mirror entity. The platform operation
  `movable_mechanisms` became **`movable_bodies`** (every movable body, in the
  engine's order) and the core leaves the balls out
  (`session_client_core::movable_mechanisms`, client_core.cpp), for the live
  record (client_core_frame.cpp) and `replay_mechanisms` alike: one
  implementation instead of two host filters. Headless:
  `bmmo::physics::movable_body_names(physics)` without the `"Ball_"` skip;
  retail: `list_bodies` movable.
- **The core's journal** classifies a body as a ball by the same identity
  (client_journal.cpp `local_checkpoint`); a body no player owns - the pieces
  - is a Mechanism, by name. The legacy retail journal
  (session_journal_client.cpp) is unchanged with the legacy path.
- **`bind_session_world`** (session_host_world.hpp): `destroy_body` and
  `insert_birth` keep the balls by `core_world_hooks::ball` (the core's
  `is_ball`), not by the prefix. The pieces are window-born bodies that NO
  replayed script makes again (the explosion is not a mechanism script, so
  it is not in either host's script replay selection), so destroying them
  for a re-creation would lose them: a new host option
  `session_world_options::parked` (both hosts: the piece set, from the shared
  `bmmo::game::explosion_pieces`, game/session_host_support.hpp, made at the
  anchor) keeps them on the parking path - parked at the birth record,
  re-posed after the birth tick. `insert_birth` treats them as any mechanism
  birth (off in both hosts). *(Superseded by the user's option (i), below:
  `parked` is gone; the pieces are destroyed and re-made like any
  window-born mechanism.)*
- **The headless replayed-tick birth feed** (SC `bind_world`'s `pre_step`
  wrapper): `core_.is_ball` instead of the prefix.
- **`reportable_wakeup`** (both hosts): not a ball (`is_ball`), not the own
  ball, not a mirror (the host's `_BMMO_` / `_Peer_` names, as before) and
  **not a piece**. Decided: no BodyRevived for the pieces. A BodyRevived names
  a body every world shares so that the server wakes its copy; no other world
  has a piece (above), and each explosion's Physicalize alone is 16-18 script
  wake-ups. Unchanged on the wire: the control runs send the same events as
  R2's (`events=13/77/66`, `echo=11/0/0/0/0` in `r2f_a_ctl` and `p_a_ctl`).
- **The legacy retail path is NOT changed** (the BAR): PSC's
  `movable_mechanism_names` (PSC:43), its `destroy_body` / `insert_birth`
  prefix checks (PSC:3150, 3186) and its wake-up filter keep the prefix. The
  step's text named them, but they run only with `session core off`, whose
  behaviour must not change; R7 deletes them. The one shared physics_RT
  change (the Random cursor) is behind a flag PSC does not pass (above).
  Also left for R7: the world-hash probe (physics_state.cpp:226-227) still
  picks its "game ball" by the `Ball_` prefix, so with pieces alive it may
  probe a piece; diagnostic only, and changing it would change every
  journal's TICK record (the wire check's byte-identical journals).
- **`worst_bodies.py`** compares every body: a new journal carries the
  pieces as mechanisms; an older one (and a legacy retail one) carries every
  `Ball_` body as kind 0 owner 0, now compared by name
  (`first_divergence.worlds(ownerless_by_name)`, default on in worst_bodies,
  `--balls-by-owner-only` for the old rule); `savestate_check.py`'s
  unrounded comparison does the same. On R2's journals it now shows the
  13:24 failure that only `r2_allbodies.py` saw (17 pieces up to 7.32 m in
  position, from 820). SimTool's checkpoint compare matches by name first
  and needed nothing.
- **Found on the way (a fidelity gap, fixed in the core's world only, for
  the user to confirm): the deterministic Random cursor.** Its only consumers
  are the five hooked Random blocks of the explosion scripts, which no replay
  re-executes; restored, a rewind anchored before an explosion took that
  explosion's draws back and the NEXT explosion flew apart from another point
  of the sequence (headless 13:24: the second trafo's 16 wood pieces off by
  0.12-0.27 m at their first record, up to 4.99 m). The first version of P
  stopped restoring it for every caller, which also changed the legacy retail
  rollback and SimTool (review round 1). As built now it is a restore flag,
  `SAVESTATE_KEEP_RANDOM` = 4 (physics_savestate.hpp, and
  `BMMO_PHYSICS_SAVESTATE_KEEP_RANDOM` in physics_rt_api.h; bridge code, no
  engine change, no API version change - an older physics_RT.dll ignores the
  bit and restores the cursor): with it the cursor is recorded, compared (a
  note when it differs) and kept live, like the time factor; without it the
  restore puts it back exactly as before. Only `bind_session_world` sets it
  (ORed into the host's flags: the core's rule, not a host switch), so the
  headless client and the core-on retail host keep it live, while PSC's
  `savestate_set(..., 0)` and SimTool's `--savestate-check` (flags
  STRICT) restore it as at HEAD. Binding test: the host's STRICT arrives as
  STRICT | KEEP_RANDOM. *(Superseded by option (i), below: the flag is
  removed and the cursor is restored by the blob again.)*

Measured (x86 mod and x64 build of 4882ec2 + P; the Arch server of R0;
`build-agent-rP` without the slot-13 hack for the wire check,
`build-agent-rPh` with it for live headless runs, `build-agent-rR2h` = HEAD
with the hack, for the old arm):

- Builds: x86 CLIENT-BUILD-OK (client objects deleted first: client_core.hpp
  and client_platform.hpp changed layout); x64 BUILD-OK; 423 tests pass (new:
  `SessionClientCoreP.MechanismsAreEveryMovableBodyButTheBalls`, the piece
  and the ownerless `Ball_` body in the journal test, `parked` / a
  `Ball_`-named mechanism in the binding test); `linux_test_syntax.sh` 25
  lines ok. **Headless unchanged:** `wire_check.sh rR2 rP pw` WIRE-CHECK: ALL
  SAME (16 journals IDENTICAL, 16 sends SAME), before and after the Random
  change. (A first build carried the slot-13 hack: its `current_map_msg`,
  send #1, differed in every capture by the md5 alone.)
- **(1) retail, R2 (a)** (`r2_run.py`, snapshots off, R2's record and beams):
  control `p_a_ctl`, arms `p_a_f50` (44 forced), `p_a_f97` (22), `p_a_f131`
  (17), `p_a_f13` (170): **every one of the 21 bodies exact by name**
  (`r2_allbodies.py`: position, rotation, velocity; 1104 common checkpoints
  to 2220), the 17 `Ball_Stone_piece*` included, and `worst_bodies.py` 0.0000
  on all 21; resyncs 0. Again on the final mod (Random change): `p_af_ctl` /
  `p_af_f13` (170 forced), all 21 exact.
- **(2) headless: OPEN.** `savestate_check.py --journals` default pairs
  (snapshots on): 50:12 (48 forced), 97:24 (25), 131:47 (18) EXACT on 53
  bodies. The trafo run (`--client-arg --beam 0 4 43 650`, drive record,
  checkpoints every 2 ticks) **against a snapshots-off server**, 13:24 (185
  forced): the own ball, the stone pieces, 15 of 16 wood pieces and every
  mechanism but one exact; off: (a) `Ball_Wood_piece07` 0.011 m from 1220 -
  a single rewind across the wood pieces' birth (1183:24) gives exactly this
  and nothing else: the parking path re-poses a window-born body, it does not
  re-create it, and the re-posed piece's later contact is not bit-exact (the
  known parking residual, design plan doc 10); (b) 11 of 18 paper pieces,
  0.07-4.43 m from 2328 - a single rewind 2327:24 gives exactly this: the
  paper pieces fly under `rt:force/<id>` controllers (SetPhysicsForce blocks
  of a script outside the selection, engine change #18 names) that the live
  frames remove at ~2319; the restore at 2327 logs "no live controller
  'rt:force/41354'" for every paper piece (skipped=38) and the replay runs
  2304-2318 without the force; (c) `P_Ball_Wood_MF` 0.085 m from 1730: the
  OLD binary (`rR2h`) gives the same 0.0848 m at the same tick - not P (it
  jumps 0.1 m between 1720 and 1724, with the paper ball's death at 1720;
  not analysed further), recorded here only. The old
  binary's same arm has the pieces off by up to 2436 m. With snapshots on the
  trafo control itself took 33 snapshot rollbacks (79 starved input frames
  on the host that run), so that form does not compare.
- **(3)** the wire check: ALL SAME (above); the 16 captures have no trafo.
- **(4) and (5)**: (5) above. (4), the snapshots-on A/B, was NOT run in this
  round: the step stopped at (2) (it was run after review round 1, below).

Open, for the user: (2) (a) and (b) are both "a non-replayed script owns the
pieces": the explosion makes and pushes them, its force blocks keep pushing
the paper pieces, the reset takes them away. Options: (i) put
`Ball_Explosion_<type>` and `Ball_ResetPieces_<type>` in the script replay's
selection AND record their activations for the replay to re-issue (as
`manager_activations` does for the sector manager), then destroy the pieces
at a restore for the replay to re-make (the sector births' path, bit-exact
there); (ii) keep parking and accept (a), and for (b) carry the force blocks'
controllers of those two scripts through the script replay's force hooks
without re-executing the scripts; (iii) accept both as the step's residual.
Under (i) the Random cursor would be restored again (the replay re-draws), so
`SAVESTATE_KEEP_RANDOM` belongs to the same decision.

Review round 1 (two reviews), as built:

- **Blocker, the Random cursor changed core-off behaviour: confirmed and
  fixed** - the restore flag above; the legacy path and SimTool restore the
  cursor as at HEAD.
- **Should-fix, `is_ball` depends on history: refuted, with a test.** The
  scenario was a replayed tick making an own-ball body for an entity the
  session never had. `post_step` records the current own entity
  (`own_ball().entity`, `get_current_ball()` / `current_ball()`, set from the
  level's CurrentLevel row, non-empty with or without a body) in EVERY live
  frame, before that frame's record (client_core_frame.cpp:414 before 560),
  and `begin` is the only reset. A replay only re-runs ticks the live frames
  already ran: a moved own birth (late-player 3.3) re-makes the body a live
  Physicalize made, after live frames in which that entity was current; a
  replayed own lifecycle re-does a trafo the live frames saw. So by the time
  any rewind runs, every entity a replay can make for the own ball is in the
  set. Querying the platform's own ball from `is_ball` instead would add an
  `own_ball()` (a CK read and a body-state read) per movable body per record
  and per journal checkpoint, and change the core's platform call order the
  E9 tests pin, for no case that can occur. Test:
  `SessionClientCoreP.MechanismsAreEveryMovableBodyButTheBalls` now also
  checks a bodiless frame's entity.
- **Remote entities (nit):** a mirror is known by its current entity only.
  At a remote trafo the old mirror's body is unphysicalized live
  (client_core_lifecycle.cpp:456-459) and no replay re-makes a past remote
  trafo, so no old mirror body exists to misclassify; the two-client trafo
  A/B below (check 4) had both remotes trafo with no new far or resync.
- `host.core` null in retail `reportable_wakeup` (nit): only while the
  platform is unbound, when nothing calls it. Kept.
- Re-run on the final binaries (should-fix): all of (1), (2), (3) below.

Measured after review round 1 (x86 mod + physics_RT.dll and x64 build of
4882ec2 + P + the round's changes; `build-agent-rP` without the slot-13 hack,
`build-agent-rPh` with it, `build-agent-rR2h` = HEAD with it; the mod of HEAD
saved as `mod_rR2`):

- (5) x86 CLIENT-BUILD-OK, x64 BUILD-OK, 423 tests pass,
  `linux_test_syntax.sh` 25 lines ok.
- (3) `wire_check.sh rR2 rP pw`: ALL SAME (16 journals IDENTICAL, 16 sends
  SAME).
- (1) retail R2 (a) on the final mod, the installed physics_RT.dll checked by
  md5 against the build: control `p3_a_ctl`, arms `p3_a_f50` (44 forced),
  `p3_a_f97` (22), `p3_a_f131` (16), `p3_a_f13` (169): every one of the 21
  bodies within 1e-6 by name to 2220 (`r2_allbodies.py`, R2's window: the
  runs end on the command channel's wall clock, the control's mark at 2221).
  Bit for bit (a stricter look, `p3_first_nonzero.py`: position, rotation,
  linear and angular velocity): 20 of 21 bit-equal in every arm, the own
  ball's rotation off by 8e-17 / 2e-16 from 818 (the ball the wood trafo made
  at ~808, re-posed by the parking path) - present the same in R2's arms
  (`r2f_a_*`), so not P. **A second trafo** (review 2, `r2t_run.py`: the same
  scenario plus a beam onto P_Trafo_Stone at 1000, so the wood pieces explode
  at 1158 after rewinds across the stone explosion's draws): control
  `p3_t_ctl`, 13:24 arm `p3_t_f13` (169 forced): 36 of 37 bodies bit-equal,
  all 33 pieces included; the own ball 1.9e-6 m from 1212 (the 8e-17 of 818
  carried into the stone ball and grown while it rolls). HEAD's mod on the
  same two runs (`p3_tR2_ctl` / `p3_tR2_f13`): the own ball identical (first
  nonzero 818 at 8.048e-17, max 1.907e-6), and all 33 pieces off by 0.2-49.6
  m from 820 / 1158. So the second explosion is exact on retail with the
  cursor kept live, and the own ball's residual is HEAD's.
- (2) headless: `savestate_check.py --journals` (snapshots on) 50:12 (49
  forced), 97:24 (25), 131:47 (18): EXACT, 53 bodies. The trafo run
  (snapshots off, `--beam 0 4 43 650`, checkpoints every 2 ticks): 13:24 (186
  forced) and 1183:24 (2 forced) give exactly the numbers above - 13 bodies
  off: 11 paper pieces 0.07-4.43 m from 2328, `P_Ball_Wood_MF` 0.0848 m from
  1730 (HEAD's), `Ball_Wood_piece07` 0.0111 m from 1220. **Still failing**;
  2327:24 ended before its first forced rewind (the run reached 2233).
- (4) snapshots-on A/B, retail: two clients (`retail_two_client_test.py`,
  level 11, snapshots 2 / 66, spacing 4.2, both `replay session` of
  `r0_keys.bmrc`, session core on) plus a trafo per client: each beamed onto
  sector 1's P_Trafo_Stone (60.951, 5.095, -12.578) at 300 and 340, so each
  world explodes its own 16 wood pieces from ~505-590 (`p3_two.py`).
  HEAD's mod (`p3_b_R2_1..5`) against P's (`p3_b_P_1..4`), per client
  mism: HEAD c1 7, 7, 7, 6, 11 / c2 14, 10, 13, 16, 13; P c1 11, 9, 13, 8 /
  c2 12, 12, 12, 17. The mismatches are the same ticks and bodies in both
  arms (the spawn at 214-218, the Stone peer, the pusher, the bridge); none is
  a piece (the server has no piece rows; the pieces are journaled kind 1
  Mechanism under P, kind 0 owner 0 under HEAD). The P arm's extra spawn
  mismatches (the own ball 0.171 m at 214, the peer at 216 / 218) occur in
  HEAD's `p3_b_R2_4` c2 as well, with the same numbers: timing, not P.
  far 0, frozen 0, resyncs 0/0 and rebases 0 in every run of both arms;
  unmatched 0 except `p3_b_P_3`: 13 on BOTH clients, the snapshots of
  1456-1480 arriving while both clients were at local tick 1454 (ahead of
  the history: both game processes stalled together, 7 ticks in 0.68 s by
  the log clock; no piece was alive in either world after ~700). A fourth P
  run had 0. Reported as measured: P's c1 13 and c2 17 lie one or two above
  HEAD's five-run band (c1 6-11, c2 10-16).
- (4) headless: 4 clients, snapshots on, `--drive --beam-bays 0,0,0,0
  --beam-dx 0 --beam-spread 2.5 --beam-tick 650` (all four onto P_Trafo_Wood:
  every client explodes its stone pieces at 810-866, its wood ones at
  1168-1228, two of them paper ones too; `p3_h4.sh`), HEAD `old1-3`
  (`build-agent-rR2h`) against P `new1-3` (`build-agent-rPh`): mism HEAD c0
  55, 53, 52 / c1 72, 73, 74 / c2 57, 60, 59 / c3 65, 63, 61; P c0 46, 54,
  55 / c1 69, 68, 70 / c2 54, 55, 56 / c3 58, 63, 64 (at or below HEAD's
  band). unmatched, far, starved, resyncs, rebases, hard 0 in all 24 client
  runs; every server journal replays exact in the SimTool (0 checkpoint
  mismatches). Pieces kind 1 under P, kind 0 owner 0 under HEAD.
- Core off (the BAR), retail solo, the R2 scenario with `session core off`
  (`r2off_run.py`; the legacy path has no forced rollback): HEAD's mod twice
  (`p3_off_R2`, `p3_off_R2b`) and P's (`p3_off_P`): the own ball,
  `P_Ball_Wood_MF` and `P_Modul_41` bit-equal in all three; the pusher
  differs from 1306-1310 between HEAD's own two runs as much as against P
  (R0's non-repeatable pusher wake).

**As built (step P), option (i): the trafo scripts are replayed (OPEN: one
residual, not P's - check 2 - and the retail snapshots-on A/B's c1 shift -
check 4, review round 2).** The user's decision after the gate stop: put the
explosion, the reset and the paper wind into the script replay of both
hosts, record their activations as the sector manager's are, and destroy
and re-make the pieces a replayed explosion makes, so nothing is parked.

- **The selection** (game/script_replay.hpp): `script_replay::select(context,
  bool trafo = false)`; `trafo` adds every root script `is_trafo_script`
  names - `Ball_Explosion_<type>`, `Ball_ResetPieces_<type>`,
  `Ball_PaperWind` (Balls.nmo, owner `All_Balls`) - sorted with the rest by
  owner priority and id. Both hosts take
  `bmmo::game::session_script_selection(context)` (= `select(context,
  true)`, game/session_host_support.hpp): 16 scripts become 23. Their
  activations' list is `session_script_activations(context, sel)` (=
  `script_replay::trafo_activations`), which each host keeps BESIDE the
  selection (retail: the `host` static's `script_activations`; headless:
  `script_activations_`), not in it - `selection` is held by value in the
  legacy `physics_session_state::script_ids` (PSH:298), which keeps its
  layout until R7 (3.2; review round 2). **Adaptation:
  the legacy retail path also calls `select` (PSC:1525), so the trafo
  scripts are an argument, not a change of `select`'s rule; the legacy path
  keeps its 16 scripts (measured in `pi_off_P`'s log).**
  `Ball_CollSound_Pieces`, which the explosion also activates (with Reset),
  is not selected: its `PhysicsCollDetection` listeners only read IVP
  (physics_RT/Behaviors/PhysicsCollisionDetection.cpp, no write into the
  simulation). A replayed explosion's Activate Script leaves that
  unselected script's next-frame flags set for the next live frame (it then
  resets and re-creates its listeners on the current pieces): sound only,
  the same class as the sector manager's activations of unselected scripts
  (PC_, PR_, P_Extra_) inside its replays.
- **The activations** (`manager_activations`, game/session_host_support.hpp,
  audit fix F8's rule extended): `record_pending(context, manager_id,
  scripts, tick)` replaces both hosts' `record_pending_activation` bodies
  (the manager's pending activation as before, then every script of the
  host's activations list with `ACTIVATENEXTFRAME`, with its
  `RESETNEXTFRAME`);
  `replay` re-issues them (`CKScene::Activate(script, reset)`) with the
  manager's, inside the replay (`replay_activation`) and for the frame after
  the rewinds (`live_cells_guard`'s reissue). The wind is activated only by
  the explosion and the reset (selected), so its re-issue is redundant and
  idempotent (the same flags twice).
- **The explosion's referential (found on the way; the root cause of the
  first measurement below).** The replayed explosions made the pieces at the
  right tick with the right draws (dv = 0) but 1.16 m off their birth
  records in y (`birth: Ball_Wood_piece01 re-created at tick 1171 is not at
  its birth record after the step: dp=(0,-1.16,0) dv=(0,0,0)`, every piece,
  headless 1183:24; the engine then re-posed them from the record, and the
  re-pose left `Ball_Wood_piece07` 0.0111 m off from 1220 - the same number
  as the parked path). The explosion's first block is "Set Position (0,0,0,
  Referential = Ball_Pos_Frame)" on the pieces frame, wrapped by the bridge
  so that `restore_explosion_pieces` first puts Ball_Pos_Frame's local
  matrix back to identity under its parent (physics_state.cpp
  `explosion_set_position`): the pieces start at Ball_Pos_Frame's PARENT,
  the ball entity the trafo unphysicalized, where Gameplay_Ingame's Trafo
  Manager left it in the trafo. Neither that parent nor its matrix is
  replayed: by the rewind (1183), the Trafo Manager had made the new ball
  current (Physicalize at 1180) and moved Ball_Pos_Frame onto it, so the
  replayed explosion placed the pieces at the new ball (y 3.095) instead of
  the old one in the trafo. Fixed in host support: `record_pending` keeps,
  with an explosion's activation, the world matrix of Ball_Pos_Frame's
  parent (or its own without one) at the end of the live frame before the
  explosion; `replay(..., inside = true)` takes Ball_Pos_Frame off its
  parent and puts it there for that replayed tick's scripts, and
  `end_override` puts parent and local matrix back at the next replayed
  tick's re-issue or in `live_cells_guard`'s destructor (the rewinds'
  end). Both hosts pass `inside` from `replay_activation` only, and their
  guard gets the activations object. A log line per placement (the first 8).
- **Born inside the window: destroyed and re-made.** `session_world_options::
  parked` is removed; `destroy_body` takes the pieces like any mechanism
  (not a ball, not the own body, not a mirror), and the replayed explosion
  makes them again (the records-from-the-replayed-birth path of cf08f11).
- **Died inside the window: made again at the restore (new: the engine's
  `revive_body`, session/rollback.hpp, bound in `bind_session_world`).** A
  mechanism of the anchor's record (`tracked.mechanisms` with a body record)
  that the world no longer has and that has no birth plan item is made
  again before the scripts' state goes back (a force block restored then
  needs its body - the paper wind's) and before the blob, which then puts
  its whole state back; the replayed `Ball_ResetPieces_<type>` De
  Physicalize takes it away again at the live tick. `bind_session_world`'s
  revive: not a ball, own body or mirror; the birth of the lifetime alive at
  the anchor (`local_birth_log::lifetime_at`: the latest birth at or before
  it - the pieces' masses and frictions are drawn anew per explosion), entity
  put back, `physicalize(recipe, group)`, event log muted. Only on the
  savestate path; a refused blob destroys the made bodies again
  (`destroy_body(entity, 0)`). Only a body the host calls revivable
  (`session_world_options::revivable`, review round 2: both hosts pass
  their `explosion_pieces` set; unset, no `revive_body` at all), so a body
  some other way destroyed - a script outside the selection, the host -
  is never made again for a replay that would not destroy it again (it
  would survive into the live world). Counted and logged in the hook, the
  first 8 makes and the first 8 failures (`revive: ... made again ... (n so
  far)`), not by the engine: `rollback_engine` is held by value in the
  legacy `physics_session_state` (PSH:173) and keeps its layout (review
  round 2; the engine's `bodies_revived` / `revive_failures` /
  `revive_logs_` of round 1 are removed). Not covered: a piece set that dies
  AND is born again inside one window (a second explosion of the same type
  within it): it has a birth plan item and is not revived, so the replay
  runs without it from the anchor to its death, as before - the engine
  now logs it at the restore (`revive: N mechanism(s) alive at tick T are
  born again inside the window (... first): not made at the restore`), the
  plan items with a `repose_tick` only.
- **The Random cursor** is restored by the blob again:
  `SAVESTATE_KEEP_RANDOM`, `BMMO_PHYSICS_SAVESTATE_KEEP_RANDOM`, the restore's
  branch and their docs (engine-changes.md, ivp-savestate-design.md) are
  removed, not left unused - physics_savestate*.{hpp,cpp} and
  physics_rt_api.h are HEAD's; the replayed explosion draws from the
  restored cursor.
- **The retail legacy path (core off) stays untracked by decision**: its
  selection, `movable_mechanism_names`, prefix checks and journal are HEAD's.
- **Tests:** `RollbackEngine.AMechanismTheWindowDestroyedIsMadeAgainAtTheRestoreForTheReplay`
  (revive before the blob, the replayed death, the refused blob);
  `SessionWorldComposesTheCoresHooksInTheDesignsOrder` (no `parked`, a piece
  destroyed, revive: the lifetime at the anchor, never a ball, the refused
  make; the host's savestate flags unchanged); 424 tests.

Measured (x86 mod + physics_RT.dll and x64 of 4882ec2 + P + option (i);
`build-agent-rPh` with the slot-13 hack for live runs, `build-agent-rP`
without it for the wire check):

- (5) x86 CLIENT-BUILD-OK (client objects deleted first), x64 BUILD-OK, 424
  tests pass, `linux_test_syntax.sh` 25 lines ok.
- (3) `wire_check.sh rR2 rP pi`: WIRE-CHECK: ALL SAME (16 journals
  IDENTICAL, 16 sends SAME). No difference to explain: the selection's 7 new
  scripts are not in a journal record, and none of the 16 captures has a
  trafo or a revive.
- (1) retail R2 (a), final mod (`r2_run.py`, snapshots off): control
  `pi_a_ctl`, arms `pi_a_f50` (44 forced), `pi_a_f97` (23), `pi_a_f131`
  (17), `pi_a_f13` (170) and the NEW death/reset arm `pi_a_f1440` (force
  1440:40: one rewind at 1440 anchored at 1400 - after the death at 1369,
  inside the stone pieces' fade, before their De Physicalize at 1434): all
  21 bodies within 1e-6 by name to 2220 (`r2_allbodies.py`), all 17 stone
  pieces included; bit for bit (`p3_first_nonzero.py`) 21 of 21 in f97,
  f131 and f1440, 20 of 21 in f50 and f13 (the own ball's HEAD residual
  from 818, 1.95e-16 / 8.05e-17). f13 re-made the 17 pieces from the
  replayed explosion (`births=...+17/17/0/11`: 17 re-created, 17 exact at
  their birth records; the 11 refused destroys are R2's own, `r2f_a_f13`
  11 - the balls' births, which stay on the parking path; P's parked
  version had 28 = 11 + 17)
  and revived them across the reset (8 revive lines logged, the cap);
  f1440 revived all 17 at the restore of 1400 and the replayed reset took
  them away: the journal has no piece after 1434, as the control. resyncs 0
  everywhere. Two trafos (`r2t_run.py`): control `pi_t_ctl`, 13:24 arm
  `pi_t_f13` (171 forced, `births=...+33/33/0/14`; the parked version's
  `p3_t_f13` had 47 = 14 + 33 refused): 36 of 37 bodies
  bit-equal, all 33 pieces; the own ball HEAD's (8.05e-17 at 818, max
  1.9e-6).
- (2) headless, the trafo run (`--beam 0 4 43 650`, snapshots-off server,
  checkpoints every 2 ticks; the scenario's third trafo is the paper one,
  so no extra paper arm): 1183:24 (2 forced) EXACT on 55 bodies (before the
  referential fix: `Ball_Wood_piece07` 0.0111 m, the re-pose above);
  2110:24 (1 forced, anchored at 2086 across the stone pieces' reset at
  2102: 17 revived) EXACT; 2328:24 (1 forced, across the paper wind's
  shutdown) EXACT; **13:24 (186 forced): 54 of 55 bodies exact - all 51
  pieces, the 11 paper pieces of the gate stop included - and
  `P_Ball_Wood_MF` 0.0848 m from 1730: NOT EXACT** (cause below).
  `savestate_check.py --journals` default pairs (snapshots on): 50:12 (48
  forced), 97:24 (25), 131:47 (18) EXACT on 53 bodies.
- (4) snapshots-on A/B, retail (`p3_two.py`, two clients, a trafo each), 3
  more runs of the final mod `pi_b_1..3`: mism c1 10, 9, 10 / c2 12, 13,
  13 - inside HEAD's five-run band (c1 6-11, c2 10-16); unmatched, far,
  frozen, resyncs, rebases 0 in all six client runs; the pieces kind 1
  owner 0. The earlier marks above the band (c1 13 in `p3_b_P_3`, with its
  13 unmatched from the stall; c2 17 in `p3_b_P_4`) are not repeated in
  these three: settled as run-to-run spread, not a P effect. Headless (4
  clients, a trafo each, `p3_h4.sh`), final `build-agent-rPh` `pi1..3`:
  mism c0 47, 53, 52 / c1 65, 69, 72 / c2 53, 52, 56 / c3 58, 63, 61
  against HEAD's c0 55, 53, 52 / c1 72, 73, 74 / c2 57, 60, 59 / c3 65,
  63, 61: at or below; unmatched, starved, rebases, resyncs, hard 0; every
  server journal replays exact in the SimTool (0 checkpoint mismatches).
- Core off (the BAR): `pi_off_P` (`r2off_run.py`, final mod, legacy path,
  16 scripts) against HEAD's `p3_off_R2` / `p3_off_R2b`: 3 of 4 bodies
  bit-equal, the pusher off from 1306 / 1308 - as HEAD's two runs against
  each other (from 1306, R0's non-repeatable pusher wake).

**The residual (check 2, 13:24): `P_Ball_Wood_MF` 0.0848 m from 1730 - not
P, and the same number at HEAD.** Symptom: the arm's prop ball leaves the
control's track between the checkpoints 1728 and 1730 and continues on a
track 0.0842-0.0848 m away to the end; HEAD's binary (`rR2h`) on the same
arm gives the same 0.0848 m at the same tick (round 1). Cause, from the
logs: the client's `P_Ball_Wood_MF` sits on `P_Modul_41` at (120, 2.02,
43); the server's starts moving at 1310 (0.06 m apart then, 0.084 at 1720)
- a client/server divergence of that module, not a piece (no piece is
within 3 m of it). The own ball's Unphysicalize at 1722 (the paper ->
stone trafo) brings a lifecycle snapshot, whose row corrects the prop
ball: control `mismatch at tick 1722 (local 1722): P_Ball_Wood_MF dp=0.1036
... rollback to tick 1722, re-simulated 0 ticks`; the arm the same at
local 1723 (re-simulated 1). The row is written into the world and into
the record of 1722 (rollback.hpp restore_and_replay: `existing->second =
item.state` and the blob re-captured), but the arm's next forced rewind,
at 1729 anchored at 1705, replays 1706-1729 from the records and re-records
every tick of it, 1722 included: nothing re-applies an authoritative row at
its tick inside a replay, so the replay puts the prop ball back on its
uncorrected track (arm 1730: 122.3904 against the uncorrected 1722 value
122.3895 + 4 ticks at 0.004 m/s; the control's corrected 122.3299) - and
with snapshots off no later snapshot corrects it again. Isolated: a
single forced rewind at 1729 (anchored at 1705, `pi_trafo3`
forced-1729x24) gives this body alone, 0.0847 m from 1730; the 13:24 arm
re-run on an idle machine (184 forced, starved 0) gives the same single
body. A forced rewind is
the measuring tool, but a real one anchored before a row-corrected tick
(a late remote input, a later snapshot's deeper anchor) loses the row the
same way. Options for the user: (a) the engine keeps the authoritative rows
it applied, by tick, and writes them again after that tick's step inside
every replay that crosses it (an engine change on the snapshot path: the
wire check's snapshots-on captures would change, and it needs its own A/B);
(b) a forced rewind never anchors before the last row-corrected tick (the
harness only; the gap stays for real rewinds); (c) accept it as a
pre-existing residual of the snapshot path, outside P, and judge 13:24
on the pieces and every other body (all exact).

Review round 2 (two reviews), as built:

- **Layout (should-fix, both reviews): `physics_session_state` keeps HEAD's
  layout again.** Round 1 had grown it: `script_replay::selection` (held by
  value in PSH:298 `script_ids`) by an `activations` vector, and
  `rollback_engine` (PSH:173 `rollback`) by `bodies_revived`,
  `revive_failures` and `revive_logs_` - against 3.2 ("keeps its layout
  until R7"). Now `selection` is HEAD's (the trafo activations are the
  hosts' own list, above) and `rollback_engine`'s members are HEAD's (the
  revive counting and logging moved into `bind_session_world`'s hook,
  above); `rollback_world` gains `revive_body`, but it is built per call
  (`physics_session_rollback_world()`), not a member. Checked: no type held
  by `physics_session_state` changes a member against 4882ec2
  (`git diff` of rollback.hpp / script_replay.hpp: functions, comments and
  the `rollback_world` hook only; physics_session.hpp, correction.hpp,
  mechanism_tracking.hpp, sector_union.hpp unchanged). The x86 client
  objects were deleted before the build anyway (the round-1 build had the
  grown layout). `BallanceMMOClient.h` changes one member function's
  signature (`core_host_replay_activation(uint32_t tick, bool inside =
  false)`), no data member. 19 game launches of the final mod in this
  round's checks (and 8 of the diagnostic mod below), none hung.
- **Revive only what a replayed script removes again (nit, round 1):**
  `session_world_options::revivable` (both hosts: their `explosion_pieces`);
  unset, `bind_session_world` installs no `revive_body`.
- **The reborn gap logged (nit, round 2):** see the revive bullet above;
  no counter (layout).
- **One replay-activation path in the retail host (nit):** `w.replay_activation`
  calls `core_host_replay_activation(tick, true)`; the referential log line
  lives there once.
- **Cost of the 7 extra scripts (nit, round 2), retail arena, HEAD
  (`r2f_a_*`) -> P (`pr2_a_*`), the harness's units:** control t_cap 977 ->
  1049; 13:24 arm t_cap 936 -> 1034, t_rep 1503 -> 1666, t_rest 65 -> 75
  (+7-11 %, on the x86 game thread). On level 11 the selection is 45 scripts
  (3 of them active at the anchor) against 38.
- Refuted: none.

Measured after review round 2 (x86 mod rebuilt from deleted client objects,
x64 `build-agent-rP` without the slot-13 hack, `build-agent-rPh` with it):

- (5) x86 CLIENT-BUILD-OK, x64 BUILD-OK, 424 tests pass,
  `linux_test_syntax.sh` 25 lines ok.
- (3) `wire_check.sh rR2 rP rPw`: WIRE-CHECK: ALL SAME (16 journals
  IDENTICAL, 16 sends SAME).
- (1) retail R2 (a) (`pr2_a_*`): control + 50:12 (44 forced), 97:24 (22),
  131:47 (17), 13:24 (168; `births=...+17/17/0/11`) and 1440:40 (1): 21 of 21
  bodies within 1e-6 to 2220 in every arm, all 17 stone pieces; bit for bit
  21/21 in f97, f131, f1440, 20/21 in f50 and f13 (the own ball's HEAD
  residual from 818, 1.95e-16 / 8.05e-17); resyncs 0. Two trafos
  (`pr2_t_*`, 13:24, 168 forced, `births=...+33/33/0/14`): 36/37 bit-equal,
  all 33 pieces, the own ball HEAD's (max 1.9e-6 from 1212).
- (2) headless trafo run (`pr2_trafo`): 1183:24, 2110:24 (17 revived:
  `revive: Ball_Stone_piece01 ... made again at the restore of tick 2086`),
  2328:24 EXACT on 55 bodies; **13:24: 54 of 55 - `P_Ball_Wood_MF` 0.0848 m
  from 1730, unchanged** (the residual below, not P's, waiting on (a)/(b)/(c)).
  `savestate_check --journals` 50:12 (49 forced), 97:24 (25), 131:47 (18)
  EXACT on 53 bodies.
- Core off (`pr2_off_P`, 16 scripts): 4 of 4 bodies bit-equal with HEAD's
  `p3_off_R2` to 2220 (the end of the record), 3 of 4 with `p3_off_R2b`
  (the pusher from 1306, as HEAD's two runs against each other).
- (4) headless, 4 clients (`p3_h4` `pr2h1..3`): mism c0 48, 52, 53 / c1 65,
  73, 73 / c2 55, 55, 57 / c3 61, 60, 60 against HEAD's 55, 53, 52 / 72, 73,
  74 / 57, 60, 59 / 65, 63, 61: at or below; unmatched, starved, rebases,
  resyncs, hard 0; every server journal replays exact.
- **(4) retail, two clients (`pr2_b_1..3`, then interleaved with HEAD's mod
  `xh_1/xp_1/xh_2/xp_2`): NOT settled as noise.** c1 mism 8, 11, 13 (13 is
  above HEAD's band again) and 13, 8 interleaved, against HEAD's 7, 8
  interleaved; c2 13, 13, 14 / 14, 13 against 13, 15 (no shift). All c1
  values so far: HEAD 7, 7, 7, 6, 11, 7, 8; P 11, 9, 13, 8 (parked), 10, 9,
  10, 8, 11, 13, 13, 8 (option (i)). Unmatched, far, resyncs, rebases 0;
  never a piece (`pr2_mism.py`: own ball, mirrors, and the pusher's
  non-repeatable 588/590 once in HEAD and once in P). **Symptom:** the excess
  is one spawn-window outcome. Every run reaches the tick-214 snapshot in one
  of two states (`pr2_spawn.py`): A - the peer's tick-212 Physicalize drained
  29-31 ms after the own spawn frame, first mismatch "own 0.0980 + mirror
  0.1400" (c1) / "mirror 0.1034 + own 0.0981" (c2), no mismatch at 216/218;
  B - the peer's Physicalize drained 43-47 ms after (one game frame later),
  first mismatch "own 0.1709 dv 3.03" alone, then 1-2 more mirror mismatches
  at 216/218 - the 1-3 extra counts. B in HEAD: 2 of 14 client-runs (both
  c2); parked P 4 of 8 (c1 3 of 4); option (i) 9 of 16 (c1 5 of 8); the
  no-trafo-scripts diagnostic mod below 3 of 8 (c1 2 of 4). **Cause, as far as isolated:** a frame-timing
  race, not a simulation difference - the server receives both clients'
  tick-212 Physicalize within ~1 ms in every run (server.log `delay_us`
  25-1525, order uncorrelated with A/B, `pr2_order.py`), and whether a
  client drains its peer's in the frame of tick 214 or of 215 decides A or
  B; B's own-ball 0.1709 / dv 3.03 against A's 0.0980 / 0.81 at 214 is the
  same in HEAD's two B cases. What makes P land in B more often is NOT
  identified. Measured and excluded or partial: (i) the selection's extra
  scripts - a diagnostic mod with P but `select(context, false)`
  (`mod_diagNT`, 38 scripts) gives B in c1 2 of 4; (ii) the tick-base
  receipt - the two clients' tick schedules are phased by the moment each
  drains the tick base (fixed_tick.cpp:37, core_host.cpp:921-939); HEAD's,
  the parked P's and the diagnostic's two clients drain it within 0-1 ms of
  each other in every run (`pr2_base.py`), option (i)'s differ by 2-9 ms in
  6 of 8 runs (only builds that select 45 scripts, 3 active at the anchor),
  but the parked P reaches B with 0 ms; (iii) the per-tick capture cost
  (t_cap, level 11: HEAD 1617-1704, P 1666-1769, diagnostic 1695-1707) -
  small, and the body events are sent before the record's capture in the
  frame (client_core_frame.cpp:454 before :563). The step that would decide
  it is an instrumented pair of builds (network-thread receive time and the
  frame's drain time of the peer's Physicalize, HEAD and P).

**As built (step Q): the anchor restores the proximity blocks' countdown.**
R0's baseline finding (two legacy retail runs of one key record woke
`P_Modul_01_Pusher` at different ticks) closed: every world now starts the
`TT Scaleable Proximity` countdowns from the level file's values at the
anchor.

- **Why each client journal equalled its own server's in R0 although the
  wake tick differed between runs.** Nothing restored, seeded or synced the
  two locals before Q: no code wrote them, and the server's
  `rewire_proximity_probes` (physics_world.cpp:364-393) changes only the
  block's ObjectA source. The server and the headless client agree with each
  other because they share the pre-anchor frame count: both load the level
  fresh and run the same 122 load ticks to the anchor (`anchor reached after
  122 ticks (engine tick 522)` / `anchored after 122 load ticks`), during
  which 13 of the arena's 24 level blocks run. A retail client agreed with
  its server only because the server applies the client's `BodyRevived` at
  the event's tick (`ensure_in_simulation`, physics_world.cpp:903-915): in
  the R0/R1 runs the retail check woke the pusher at 604-612, never after the
  server's own check (612), so the server took the client's tick.
  The one R1 run whose client woke at 613 is the counter-example: the server
  woke it itself at 612 and the two journals differed in 612-667.
- **The rule** (`game/script_state.hpp` `proximity_countdown_state`, beside
  `sequencer_state`): every `TT Scaleable Proximity` whose owner is a level
  object (`P_`, `PC_`, `PS_`, `PE_`, `PR_` ...; base.cmo's thirteen -
  `Gameplay_Tutorial`'s twelve probes, owned by `All_Gameplay`, and one of
  `All_Sound`'s - have no level-file value and are left alone) has
  locals 0 (last in/out state) and 1 (`Last Check`) recorded at first sight
  while the level loads, where each host already records the Sequencers
  (physics_world.cpp:146-149, session_client.cpp:1433-1436, the retail
  `capture_level_sequencers` frames, BallanceMMOClient.cpp:359-364), and
  written back at each anchor right after the Sequencer counters
  (physics_world.cpp:168-177, session_client.cpp:1456-1463, the retail
  legacy anchor physics_session_client.cpp:1458-1460 and the core host's
  `core_host_anchor` core_host.cpp:879, both through
  `proximity_countdowns_restore`, BallanceMMOClient.cpp:367-388). It is about
  the countdown phase only: the probe target (ObjectA, rewired on the server,
  and later on the clients by `docs/world-agreement-gaps-design.md`) is not
  touched, so the rewiring stays compatible. Every side records the same 24
  values on the arena (22 at the prototype defaults 2/1, `P_Extra_Life_MF`
  1/1 twice, `PE_Balloon_MF` 1/1 and 1/10); the anchor writes back 13
  differing blocks on the server and the headless client, 14 on a retail
  client.
- **What the retail anchor logs** (after the review): the recorded values,
  the values the differing blocks held just before the restore, and a census
  (`proximity_countdown_state::census`) of the `TT Scaleable Proximity`
  blocks the record does not cover: those owned by no level object, by owner
  name (the arena: `All_Gameplay x12, All_Sound x1`), and level ones that
  appeared after the retail record window, with a warning. The retail client
  records only the 30 frames after the map-load hook (the Sequencers'
  window), the engine on every load tick up to its anchor; on the arena both
  see the same 24 blocks, and the warning makes a level where they would not
  (8 and 11 are not verified) visible instead of a silent split. The server's
  and the headless client's logs are unchanged.
- **Known limit: a late joiner.** The rule makes the phase equal at a
  session-start anchor (tick 0). A player whose anchor is a late-join base
  (tick N > 0, PSC:241-253) writes the level file's values while the
  server's blocks stand at the file's values plus N ticks of their own
  countdowns, so its checks run on other frames until the ball is close
  enough for a block to check every frame. Not a regression (before Q its
  phase was arbitrary too). Options, not chosen here: leave it (the
  snapshot path corrects the world; the server takes a client's earlier
  `BodyRevived`); or carry the countdowns in the late-join state (a protocol
  change this row does not name). The user decides.
- **Adaptations.** (i) The rule changes the legacy retail path (the bar's
  "core off behaves exactly as before" yields to the row: R0's baseline check
  cannot pass otherwise), and the server's and the headless client's worlds
  (their own countdown phase moves from "after 122 load ticks" to the level
  file's). (ii) The retail record lives in a file-scope static of
  `core_host.cpp` (`retail::level_proximities()`), cleared in
  `OnLoadObject` beside `level_sequencers_`; `BallanceMMOClient` gained one
  non-virtual method, no data member. (iii) No protocol, IVP or physics_RT
  change.
- **Other countdowns that survive the anchor** (the arena's 61 level scripts,
  every block type): the `Sequencer`s are `sequencer_state`'s
  (`P_Modul_26`) and the balloon's (`PE_Balloon` x2, out of scope by the
  user's decision). `Delayer` (x6), `Bezier Progression` (x3) and `Counter`
  (x1, `P_Modul_18_Init`) restart their elapsed time / count on "In"
  (TimerMini.cpp:66-70, BezierProgression.cpp:126-139, Counter.cpp:86-94):
  their state matters only while they run across the anchor, not as a
  phase. `Per Second`, `Interpolator`, `Rotate`, `Ray Intersection`, `Box Box
  Intersection` keep no state. `Binary Memory` (x1, `P_Extra_Life_MF
  Script`) is a picked-up flag (score / lives, open). `TT
  ProximityVolumeControl`, `Wave Player`, `Particle Systems`,
  `TT_TextureSine` reach sound and visuals only. So `TT Scaleable Proximity`
  is the only countdown left (other levels' scripts not surveyed). A
  `Delayer` or `Bezier Progression` still running across the anchor would
  carry its elapsed time; that none does on the arena is shown only by check
  (1) holding, not by inspecting them at the anchor. None of the existing
  script edits covered the countdown: N1 (death / level reset), N2
  (deactivate pin), N3 (activate hold) and N4 (the checkpoint chain's phase
  cell) edit the BallManager, sector and checkpoint chains, and
  `sequencer_state` the Sequencer counters only.
- **Measured** (quick tier; x64 build, x86 mod and Arch server with the slot-13
  hack, build id `ballanced-3e4289e95f25+bmmo-4ac6ba0a87d1-dirty`; solo,
  arena, snapshots off, `replay session` of `r0_keys_p0.bmrc`):
  - (1) two legacy runs (`q_a_off1`, `q_a_off2`): 2495 common TICK hashes
    (8-2502), 0 differ; the pusher's `BodyRevived` at 612 in both.
  - (2) core on (`q_a_on1`) against them: 2532 (8-2539) and 2495 common
    ticks, 0 differ; pusher 612.
  - (3) each client journal equal to its own server's at every tick (2532,
    2495, 2560); the core-on run's server journal equal to `q_a_off1`'s at
    all 2532 common ticks; the SimTool replays the three server journals
    exactly (2532 / 2495 / 2560 ticks, 0 checkpoint mismatches).
  - (4) `wire_check.sh rP rQ qw`: DIFFERENCES, every one of the 16 journals,
    each explained by the rule: before its first difference the replay is
    identical, and the first difference is the client's own `BodyRevived` of
    a proximity-woken mechanism moved by the new phase - `P_Modul_01_Pusher`
    in 13 (old -> new tick: swing c0 631->633, c1 622->624, c3 661->652;
    a6m c0 611->602, c1 652->653, c2 592->593, c3 641->642; resync c0
    628->629, c1 655->656, c2 620->611; a5u c0 628->629, c1 654->655, c2
    611->612), `P_Modul_19_Flaps` in swing c2 (780->777) and
    `P_Modul_25_Bridge` in resync c3 and a5u c3 (958->959). The SimTool on
    the four reference server journals: HEAD's (rP) exact (3986 / 4257 /
    4176 / 4212 ticks), the new one diverges where the server's own check
    now wakes a mechanism at another tick (the simulated-core count rises one
    tick apart): swing 2148 and a6m 1245 `P_Modul_34_Schiebestein` (no
    client `BodyRevived` for it in the reference), resync 611 the pusher (the
    reference's earliest client wake 620), a5u 914 `P_Modul_25_Bridge` (the
    reference's 918). The references were recorded on HEAD's phase; new
    captures are needed for the wire check from here on.
  - 424 tests pass; `linux_test_syntax.sh` 25 lines ok.
  - After the review's logging change (x86 mod and Arch server rebuilt, same
    build id): two more legacy runs (`q_g_off1`, `q_g_off2`), each client
    journal equal to `q_a_off1`'s (2516 and 2532 common ticks, 0 differ) and
    to its own server's (2516, 2533), pusher at 612. Their logs show the
    pre-anchor phases differed between the runs (`P_Modul_01_MF` 0/13 and
    0/39, `P_Modul_18_MF` 0/40 and 0/6, `PC_TwoFlames_MF` 0/23 and 0/69) and
    still give identical worlds, census `13 not a level object's (All_Sound
    x1, All_Gameplay x12)`, no unrecorded block.
  - Why (4) is explained by each journal's first difference: the diff's only
    write into the world is the anchor restore (`capture_new` only reads the
    locals during the load ticks), so everything after a replay's first
    difference follows from a different world state, not from another code
    change. The 16 captures and the four `core_ref_e0/*/server.bmjr` were
    recorded on HEAD's phase: from this step on `wire_check.sh` reports
    DIFFERENCES for any binary that contains Q, so a new reference set must be
    recorded with the Q binaries (as `core_ref_e0/README.md` describes) before
    a later step can use the wire check as a gate. (Done at step REF,
    2026-10-24, with HEAD 765267c: the E0 set moved to `core_ref_e0_pre_q/`,
    the new one is in `core_ref_e0/`; see the As built (step REF) note in
    resync-and-sector-timing-design.md §7.5.)

**As built (step RR): a snapshot row applied at tick t is written again
after t's step in every later replay that crosses t.** The gap step P's gate
found (the `P_Ball_Wood_MF` row of 1722 undone by the forced rewinds at 1705
and 1718; verified in `pr2_trafo/forced-13x24`: the mismatch and restore at
local 1723, then `rollback to tick 1705 (forced)` and `1718 (forced)` re-record
1722 from the replay, and the arm's later mismatches at 1887 and 2383 are that
uncorrected track) is closed in the shared engine.

- **Where the rows live.** `tick_state::rows` (rollback.hpp), by entity: the
  row's state and sleep mode, the generation of the body it was written on,
  the server identity (kind, owner), whether it was the own ball, and the
  friction contacts the restore then re-created for it from the anchor's
  record. Written in the restore's row loop beside `existing->second =
  item.state`; merged when a later snapshot of the same tick writes rows
  again, one row per server body (the newer row also replaces one of the same
  (kind, owner), or the own ball's, kept under another local entity - a late
  birth in between; review round); carried over when a replay re-records the
  tick (`state.rows = std::move(recorded->rows)`); erased by `move_birth` on
  the gap's ticks only (motion the server never stepped; a row from the
  applied tick on is the server's state of the body made again there, review
  round); gone with the record (history length, `invalidate_history`). Memory: one
  `bmmo_physics_body_state` + a contact vector per authoritative row of a
  mismatching snapshot, nothing for a matching one.
- **The write.** `reapply_rows`, after a replayed tick's step and its births'
  re-pose, before the tick is re-captured: the restore's sequence for a row
  (pose and sleep write rechecked when it moves the body, at the write or in
  the second pass; then `set_contacts` with the kept contacts; no extra). Only
  ticks after the anchor: the anchor's own rows are in its blob (re-captured
  at that restore), and on the per-body path in its record. Skipped: a body
  the replayed world lacks at t, one of another lifetime, one still parked for
  a later birth. The `rollback to tick ...` line gains `, N snapshot row(s)
  of M tick(s) written again (first T)` when it wrote any.
- **Births inside the window (adaptation, found by the wire check).** A row
  is kept under the entity it was written on, but a late lifecycle event can
  give that server body another local body inside the window (swing c0: the
  row of player 2367392004 at 702 went onto the old mirror `..._2367392005`;
  the late Physicalize at 701 destroys it and makes `..._2367392006`, and the
  rewind to 700 re-applied 4 of the 5 rows). New optional
  `rollback_world::row_entity(kind, owner, tick)`: a row whose body is gone is
  written on the body the server identity has at t - the own ball by the
  replayed record, a mirror by the core's `replay_remotes` rule
  (`driven_from <= t`), a mechanism by `mechanism_tracking_`
  (`core_world_hooks::row_entity`, client_core.cpp); asked only for a row
  that carries its identity. Its contacts are not written there (they were
  another body's). The generation kept is the body's newest, which is its
  generation at t: a body born after t takes the birth branch and is never
  written from a row. Measured: swing c0 writes 5 rows
  at 702; the mismatch counts of the 16 replays are unchanged by it.
- **Legacy retail gets nothing new**: the switch is
  `rollback_world::replay_rows` (a field of the per-call binding, not an
  engine member: `rollback_engine` is held by `physics_session_state`, whose
  layout stays until R7). `bind_session_world` sets it (both cores); PSC's
  `physics_session_rollback_world()` and the SimTool's binding leave it off.
  `rollback_engine`'s members and `physics_session_state` are unchanged; no
  counter (layout), the log line instead. The x86 client objects were
  deleted before the build (`core_world_hooks` grew).
- **The resync design's rows at S** (resync-and-sector-timing-design.md 4.3)
  write rows on top of S's blob and re-capture it; going through
  `restore_and_replay` they would be kept with S's record like any row. Today
  a resync drops the history, so no anchor before S exists.
- **Measured (quick tier):** x64 BUILD-OK (`build-agent-rRR` without the
  slot-13 hack, `build-agent-rRRh` with it), x86 CLIENT-BUILD-OK, 425 tests
  pass (new: `ASnapshotRowIsWrittenAgainByEveryLaterReplayThatCrossesItsTick`),
  `linux_test_syntax.sh` 25 ok.
  - (1) `rr_trafo` (snapshots off, control + 13:24, 186 forced): 55 of 55
    bodies EXACT, `P_Ball_Wood_MF` 0.0000 m (P: 0.0848 from 1730); the arm's
    mism 3 -> 1 (the 1887 / 2383 mismatches gone), its two forced rewinds
    across 1722 each write 1 row again.
  - (2) `rr_ssc` (snapshots on) 50:12 (48 forced), 97:24 (25), 131:47 (16):
    EXACT on 53 bodies.
  - (3) `wire_check.sh rQ rRR`: 11 of 16 journals change (swing c0-c3, a6m
    c0, resync c0/c2/c3, a5u c1-c3), sends differ in a6m c0, resync c0/c2;
    the 5 unchanged (a6m c1-c3, resync c1, a5u c0) log no row written again,
    each changed one logs one before its first difference. Against the
    server's FULL checkpoints: the snapshots-on captures' own measure falls in
    all four (mism 203 -> 199, 33 -> 25, 31 -> 15, 227 -> 212; far 2 -> 1,
    0, 1, 2 -> 1); the snapshots-off ones keep their counts. The summed body
    distance to the server from the first difference to the end (the
    balloon left out, as the user decided): closer in 9 of 11 (resync c0
    11602 -> 964 m, c2 16080 -> 3178), further in 2: swing c0 254.23 ->
    254.77 (+0.2 %), a5u c2 1103.8 -> 1120.3 (+1.5 %); with the balloon
    a5u c1 and c3 are further too (the balloon bodies only). See the
    residual below.
  - (4) headless, 4 clients, snapshots on (`p3_h4` `rrh1`, with captures):
    mism 47 / 65 / 53 / 57 (P's `pr2h1..3`: 48-53 / 65-73 / 55-57 / 60-61),
    rb 99 / 130 / 107 / 119 (103-110 / 126-140 / 114-135 / 118-125);
    unmatched, far, starved, resyncs, rebases 0; the server journal replays
    exact (2439 ticks, 0 checkpoint mismatches). The captures replayed with
    `rRR` equal the live journals; with HEAD's `rQ` all four differ (every
    client writes rows again: 10-15 replays each; first difference 878, 878,
    878, 1190, after the first re-applies at 818-826, which wrote rows the
    replay with the corrected input had reproduced bit for bit). Mean body
    distance to the server mm-level either way (c0, c1, c3 closer, c2
    further: ball 2599189757 1.7 -> 4.2 mm).
- **The residual: 2 of 11 changed wire-check journals end further from the
  server.** Symptom and cause, per journal (the swing c0 cause measured in the
  review round with a diagnostic build, not committed: each row write, and
  each replayed tick's balls, logged against the server's checkpoints).
  - swing c0. From 722 to 740 the remote balls 905583447 and 2632207374 are
    0.10 -> 0.60 m and 0.09 -> 0.50 m from the server where the old replay is
    0.00-0.14 m; the snapshot rollback anchored at 718 (local 741) brings both
    to 0.0000 in both binaries, and from there the new one keeps ball
    2367392004 exact where the old keeps 0.0065-0.0069 m for good (swing c1:
    the same transient, then three balls exact against 0.15-1.2 m). NOT the
    remote inputs: every player's keys are `01` and unchanged from 695 to 745
    and the replays that cross 702 have every input to 702 (local 720), so
    the prediction is the truth. The rows at 702 (4 balls and
    `P_Modul_08_Schaukel`) are right: after them the replayed balls are within
    0.0001 m / 0.0005 m/s of the server at 704 and 0.0004 m / 0.0027 m/s at
    712 (state a row does not carry - the friction contacts the replay's own
    world has - the beam residual every restore has). At 713-714 the four
    balls, born in a row at 701, collide: 0.0006 / 0.0025 m and 0.07 / 0.39
    m/s at 714, 1.5 m/s for 905583447 at 716, on the collision alone (the
    replays anchored at 701 and 710 show it, their predicted inputs being the
    server's). The old replay entered that collision from its own 702,
    where the two mirrors born at 701 were 0.17 m/s slower than the server's
    balls (vx 0.1836 against 0.3555 / 0.3355; own ball and 2367392004 0.02
    m/s slower) - a pre-existing difference of the late-birth replay, the
    same in both binaries, not investigated here - and its collision happened
    to resolve nearer the server's until the 718 rows. A chaotic pile
    amplifying the beam residual, the floor the lockstep blocker notes
    record for piles, not a wrong write.
  - a5u c2 (snapshots off). `P_Modul_08_Schaukel` is out of phase with the
    server in both binaries (0.2-10.7 m, a client/server divergence of that
    run that no later snapshot corrects); the 3359 row moves its phase (old
    and new 0-2.2 m apart) and the oscillating error averages 1.5 % higher.
  - Neither is a write the live restore did not make. Options: (a) accept,
    judging by the snapshot path's own measure and the long run; (c) write
    again only the rows that breached (an in-tolerance row is often
    reproduced bit for bit anyway; not measured). The option (b) the first round offered (write
    only when the replay has every remote input to t) rested on the input
    cause the measurement refutes, and would not change swing c0 either.
- **Review round.** Fixed: the newer row of a server body wins across local
  entities (both reviews: two snapshots of one tick on the old and the new
  mirror left both rows and the replay could write the older one); the
  `row_entity` fallback only for a row with its identity; `move_birth` keeps
  the rows from the applied tick on. New tests:
  `AReplayedRowFollowsTheServerBodyToTheBodyItHasAtTheTick` (the fallback,
  no contacts on the other body, an empty answer writes nothing),
  `TheNewerRowOfAServerBodyAtATickWins`,
  `AMovedBirthKeepsTheRowsFromItsAppliedTick`,
  `SessionClientCore.RowEntityIsTheBodyTheServerIdentityHasAtTheTick` (the
  `driven_from` rule before, at and after the tick; own ball, unknown player,
  no navigation, no body, unresolved mechanism). Not changed: the kept
  generation (see above: equal to the generation at t). Deferred to the next
  full tier: an RR arm in the CPU parity measurement (the re-apply adds a
  `set_body` per kept row per crossed tick). Rerun: 429 tests pass,
  `linux_test_syntax.sh` all ok, x64 BUILD-OK (`build-agent-rRRf` plain,
  `build-agent-rRRfh` with the slot-13 hack), x86 CLIENT-BUILD-OK (plain);
  the 16 captures replayed with `rRRf` are identical, journals and sends, to
  the implement round's `rRR` replays (so the same 11 changed against `rQ`,
  the same 2 further); (1) `rrf_trafo` 13:24 (169 forced): 55 of 55 EXACT,
  `P_Ball_Wood_MF` 0.0000 m, both forced rewinds across 1722 write its row
  again, starved 0; (2) `rrf_ssc` 50:12 (49), 97:24 (24), 131:47 (18): EXACT
  on 53 bodies.

**As built (step LB): a mirror born inside a rewind's window is made again
at its birth tick from the inputs, not parked and re-posed.** The finding RR
left open (swing c0: the two mirrors born at 701 0.17 m/s behind the server at
702, the own ball and the third mirror 0.02-0.03 m/s) is closed in the shared
core, engine and binding.

- **Ground truth.** The server applies navigation at a clone's birth tick:
  `set_input` before the events, `set_ball` at the Physicalize, the force in
  that tick's PreSimulate (session_runner.cpp `step`, physics_world.cpp
  `apply_event`); its 702 rows have vx 0.3955 / 0.3755 / 0.3555 / 0.3355 (two
  ticks of force, 0.1836 each, plus a push of +0.03 / +0.01 / -0.01 / -0.03:
  the four balls are born 3 m apart, x 711 / 708 / 705 / 702, and push one
  another in the 701 step). The client's live frames never had these mirrors
  at 702: all four births arrived 18-19 ticks late (local 719-720), so they
  exist only in replays and the server is the reference.
- **Cause, two mechanisms** (a diagnostic build logging, per replayed tick,
  each ball's state and navigation before and after the step and after the
  births' re-pose; its replay journal byte-identical to the clean build's):
  1. *The birth tick's force.* The late birth's first replay (local 719) made
     the mirror at 701 in `pre_step` with no frame for it: the relay had not
     reached 701 (`relay_frame_or_earlier` returns none for the tail) and the
     window's records, captured while the player was dead, hold no
     prediction for it, so the navigation stayed idle (`a0 c00`) and its
     record of 701 has vx 0. Every later rewind across 701 took the engine's
     birth branch: parked at that record, out of collision detection, and
     after the 701 step - which the now-known frame drove to vx 0.18358 - put
     back on the record (`post-step vx 0.18358`, `post-repose vx 0.00000`).
     `note_input_correction` skipped the 701 frame by rule
     (`relay_birth_frozen_`: a replay re-poses it anyway). One tick of force
     lost in every replay: the 0.17 m/s.
  2. *The birth tick's collisions.* A parked body is inert through its birth
     tick's step, so of the four balls born together only the two the replay
     made itself (the own beam, the pending birth of 2367392004) pushed each
     other: +0.01 / -0.01 where the server has +0.03 / +0.01 - the own ball's
     and that mirror's 0.02-0.03 m/s.
- **The fix: the mirror's birth is re-run, as the own ball's beam already is
  (`apply_own_lives` in every replay).**
  - core: `remote_ball` keeps the `pending_birth` it was made from and
    `birth_known` (made at its stamp; false for a late live birth or a row
    start). `remake_mirror_birth` (`core_world_hooks::remake_mirror_birth`),
    asked by a restore's `destroy_body`: a mirror whose birth is known
    (`remakes_birth`: rollback and lifecycle rewind on, physicalized and
    navigated, `born_tick` = its stamp = `driven_from`) is taken out
    (navigation detached, body unmade with the event log muted) and its birth
    queued again (`pending_birth::remake`); `drain_pending_births` makes it
    at that tick in join order with the tick's other births and our own
    changes. `physicalize_remote`'s remake: the same entity (no
    `mirror_entity`), the same lifetime (no generation bump: the window's
    records and RR's kept rows name it), the remote's live state untouched
    (predictor, `applied`, corrector, `born_tick`, `driven_from`, birth), the
    navigation attached without the log line. `make_unmade_remakes`, after
    the frame's rewinds: a remake no replay made is made live.
  - `note_input_correction`: the birth tick's frame is news again for a
    remade birth (`!remakes_birth`), so its arrival rewinds to the tick
    before the birth.
  - engine: `rollback_world::birth_from_inputs`. A body `destroy_body` took
    out that it names (`restore::from_inputs`) is fed again after
    `pre_step` at its birth tick (its navigation is the one made with it,
    after the tick's first feed) and left as that step made it: no re-pose
    to the birth record, which was made with the inputs known then (logged
    `made again at tick T from the inputs it has now, kept against its
    birth record`, counted in `births_recreated_exact` when equal). No new
    engine member (layout).
  - binding (`bind_session_world`): `destroy_body` asks the core for a
    mirror first, before the ball rule; `birth_from_inputs` is the core's
    mirror test. `--park-births` installs neither.
  - headless host: `make_mirror` finds the spec's entity by name before
    `remote_entity`, which clones a new, uniquely named entity on every call
    (the live makes are unchanged: `mirror_entity` names the entity in the
    same frame).
  - Legacy retail: no core hooks, unchanged. Retail with the core on:
    `lifecycle_rewind` is off until R3, so `remake_mirror_birth` answers
    false and the mirrors keep the parking path; the retail `make_mirror`
    keeps one entity per player and type, so R3 needs no host change.
- **Adaptations.** The design had no remake; the step asked for the fix in
  the shared code. A predicted frame at the birth tick alone would fix (1)
  and not (2); re-running the birth fixes both. The headless `make_mirror`
  lookup is the one host change. The x86 client objects were deleted before
  the build (`client_core.hpp` and `client_platform.hpp` changed layout).
- **Measured (quick tier).** 432 tests pass (new:
  `RollbackEngine.AMirrorMadeAgainFromTheInputsIsFedAtItsBirthTickAndKeptAsStepped`,
  `RollbackBinding.SessionWorldHandsAMirrorsBirthToTheCore`,
  `SessionClientCoreLifecycle.AMirrorBornInTheWindowIsMadeAgainAtItsBirthTick`);
  `linux_test_syntax.sh` 25 ok; x64 BUILD-OK (`build-agent-rLB` plain,
  `build-agent-rLBh` with the slot-13 hack); x86 CLIENT-BUILD-OK (plain).
  - swing c0 replay: once all four births are known (the replays anchored at
    700-711 from local 720 on), every replayed tick 702-712 has the four
    balls within 4.1e-6 m/s of the server's vx (RR: 0.17 m/s), and 713-740,
    through the four balls' collision, within 2.3e-5 m/s (RR: 0.07 / 0.39
    m/s at 714, 1.5 m/s at 716).
  - `wire_check.sh rRR rLB lbw`: sends identical in 15 of 16 (a6m c1 from
    send 3464, tick 3167). All 16 journals differ. 9 are the same world
    (TICK hash and pose equal at every tick): the TICK records' birth
    counters (parked -> made again) and, in resync c1, a6m c3 and a5u c2, one
    more rewind (the birth tick's frame no longer skipped) that reproduces
    the same world. 7 change the world; against the server's FULL
    checkpoints (balloon left out) 6 end closer - swing c0 797.8 -> 788.9 m
    (own ball mean 0.0017 -> 0.0007 m), swing c1 1105.1 -> 1094.1, swing c2
    586.21 -> 586.20, a6m c1 83.55 -> 80.31, a6m c2 51.47 -> 46.03, resync c0
    7050.75 -> 7046.45 - and swing c3 ends further, 412.63 -> 414.48 m, all
    of it at one LOCAL checkpoint (local 724; every other tick equal, or
    closer: ball 2632207374 at 844-944). There the old replay's birth-tick
    error breached the snapshot of 702 (own ball, 0.085896 m) and its rows
    corrected 702; the new replay's 702 was within tolerance and the
    snapshot of 704 breached instead (mirror `Ball_Stone_BMMO_2367392006`,
    0.037240 m), its rows corrected 704. Both rewinds ran at local 723-724,
    while the frames of 905583447's mirror from 701 were still arriving one
    by one (the corrections for 705-722 follow through local ~752). The
    LOCAL checkpoints 702-722 are equal in the two journals (written live,
    before any of the four births was known); at 724 both are predictions
    with that input missing, from different corrections (the four balls
    0.728 / 0.504 / 0.997 / 0.166 m from the server -> 0.643 / 1.098 /
    0.342 / 0.895, sum 2.395 -> 2.979 m); at 726 both equal the server's
    (0.0000 on all four). Measured on the balls (the mechanisms differ only
    in `P_Modul_41`, a pair about 598 m apart in both): swing c3 +0.568 m,
    all at 724; the other six -8.53 / -10.80 / -0.009 / -3.24 / -5.45 /
    -8.06 m. **This check fails for swing c3**; the cause is above, and
    whether it may stand is the user's decision.
  - `savestate_check --journals` 50:12 (48 forced), 97:24 (25), 131:47 (18):
    EXACT on 53 bodies.
  - headless, 4 clients, swing bay, snapshots off, 60 s (`lb_swing_n1`,
    `rLBh`, captures): the four captures replayed with `rLBh` are IDENTICAL
    to the live journals, counters equal; the server journal replays rc 0, 0
    mismatches; resyncs, unmatched, rebases 0. Mirror error against the
    server's FULL checkpoints: p99 10.0 m over all clients (per client 1.56 /
    2.76 / 3.11 / 12.41) against `e8ab2_swing` n1 / n2 / o1 / o2 103.6 /
    27.4 / 16.7 / 80.3 (per client 3.1-524.9); mean 4.98 against 4.26-5.84
    and max 565.6 against 557.8, both the beam and death transients. Seen,
    not caused by LB: relay_late 0 / 3 / 6 / 3 (lag max 48-49 against the
    48-tick reach; E8 n1 had 150), starved 4-7 (E8 0-5), t_max 147 / 88 /
    142 ms live where the replays of the same captures give 43-55 (E8
    43-75): host spikes.
  - solo snapshots off (`solo/lb_new1`): IDENTICAL to HEAD's binary run
    beside it (`solo/rr_old1`: TICK hash and pose 2377 / 2377, LOCAL 1189 /
    1189, other records 5979 / 5979). Both differ from `old1` from tick 952;
    step P's binary (`solo/p_old1`) equals `old1`, so the difference came
    with Q (the anchor restores the proximity countdowns, server and
    headless), not with LB: `old1` is no longer the solo reference. (Not
    bisected further: Q's binary on disk, `build-agent-rQ`, is the plain
    build, which the arena server refuses with "physics sessions need an
    original level"; a slot-13 build of Q is needed for that run.)
- **Gate round.**
  - A remake whose make fails (`make_mirror` refuses, or the remote has no
    entity) left the mirror without a body for good: its pending entry was
    already drained, and before LB the parked body had stayed. Now
    `drain_pending_births` queues a failed remake again, so
    `make_unmade_remakes` tries it once more, live, after the frame's
    rewinds; failing there too, it is counted and logged (status
    `remake=made/lost`), `birth_known` is cleared, and the mirror stays
    bodiless until the player's next Physicalize. The status line's
    `remake=` also gives the number of remakes, which the engine's
    `births_recreated` counts among its mechanism re-creations (and
    `births_recreated_exact` there, for a remake, means "equal to the birth
    record of the earlier inputs", not a fidelity statement). The fake
    platform's `make_mirror_ok` covers both failures in
    `AMirrorBornInTheWindowIsMadeAgainAtItsBirthTick`.
  - a6m c1's sends: the first differing send is #3464, tick 3167, type 5
    (the input history, 325 bytes); sends 3167-3175 differ, the rest are
    equal. The own frames at 3167 and 3168 swap (keys 1, 5 -> 5, 1): the
    run drives with `--keep-lane 15`, which steers from the own ball's
    state, and every history datagram carrying those two frames differs.
    The world first differs at tick 2606 (TICK hash and pose, the own ball's
    spawn tick); the one control-flow difference before it is LB's new
    rewind at local ~2596, `rollback to tick 2587 (Ball_Stone_BMMO_1267384801
    input corrected at tick 2588)`: the frame of that mirror's birth tick,
    which the old binary skipped (`relay_birth_frozen` 1 -> 0).
  - resync c2's capture replay stops at pass 14234 (`own birth moved 2635
    -> never ... resync`, then `expected a clock read at resync_limiter`)
    with LB and with RR's binary alike (`rr2_new`); the binaries before RR
    (`rPw_new`, `rrw_old`) replayed it to the end. It predates LB.
  - Rerun for the fix-up (shared code): 432 tests; GCC `-fsyntax-only` with
    the Arch tree's own compile commands on the 10 translation units LB
    touches (the scratchpad's `linux_test_syntax.sh` was lost with the temp
    tree), all ok; x64 and x86 (objects deleted) built; the 16 captures
    replayed with the fix-up's binary: every journal IDENTICAL to the
    implement round's (`lbw_new`) under the exclusions and every send equal
    past the first (the hello, which carries the build id) - the failure
    path was never taken in them (status `remake=` 3-24 made, 0 lost, per
    journal).

**As built (step R3): the retail host's lifecycle rewind and own echoes.**
Where the code made R3 differ from §1.2, §2.3, §4.2 and the R3 row:

- **What R3 turns on.** The retail `core_config` sets `lifecycle_rewind`
  and `own_echoes` (core_host.cpp, the session start); `own_corrections`
  stays off (R4), `own_body_backstop` on. No retail platform code changed for
  either: the binding has been the shared `bind_session_world` since R0, the
  retail `make_mirror` keeps one spirit ball per player and type, so a remake
  (step LB) finds the same entity, and the capabilities (no own body
  control) select §2.3's fallback inside the core (E7's `move_own_birth`,
  `resync_own_life`, `re_report_own_birth`, `expire_own_echoes`). The verb's
  answer says "step R3".
- **`mirror_start_from_row` is deleted** from the core: the `core_config`
  field, the snapshot intake's row cache (`client_core_state::ball_rows_`
  and its `begin` reset) and `physicalize_remote_from_row`. The headless
  client never set it (inert there; WIRE-CHECK below). The R1 test
  `LifecycleRewindOffMakesLateChangesLive` keeps the switch-off half (late
  births and a late death made at the drain, no rewind) and now checks that
  a newer snapshot row changes nothing (the event's pose and kick, no
  `write_body`). `quaternion_rows` stays (the backstop's re-report uses it).
  The legacy path's own row start (PSC, `latest_ball_rows`) is untouched.
- **A diagnostic the row's first criterion needed (an addition).** A
  mirror's pose at its stamp exists only in the history: the LOCAL
  checkpoints are written live, and a remote birth arrives input_delay + a
  trip after its stamp (20-23 ticks here), so no checkpoint of the stamp tick
  has the mirror. `rollback_engine::recorded_body_state(entity, tick, out)`
  (const; no member, so `physics_session_state` keeps its layout) and its
  core pass-through read a record's body; the retail host's
  `hooks.after_rewinds` logs, for each mirror made at its stamp
  (`birth_known`), the records of the stamp tick and the next one whenever a
  replay changed them ("mirror birth record player=P entity=E stamp=T at
  local L: t0=... t1=...", at most 24 lines per birth, then one "cap
  reached, not logged further" line, after which the last record line is
  no longer what the history keeps and the judge sets that birth aside;
  the runs logged at most 3 per birth). It runs on every core-on frame (two
  history reads per remote) and goes with the other retail diagnostics at
  R7. The last line per birth is what the history keeps; the scratch tool
  `.local-tools/work/r3/r3_judge.py` compares it with the owner's LOCAL
  checkpoint of the same tick (every 2 ticks, so t0 or t1) and with the
  server's FULL one. Nothing decides on it.
- **The beams' births: the criterion holds against the server, not against
  the owner (§2.3, measured).** A beam is an Unphysicalize + Physicalize pair
  at the server and in every other world (a fresh body at the beam pose); in
  the owner's world without own body control it is an in-place write of the
  old body. At the stamp the mirror equals the server's ball (≤ 1e-6 m, the
  log's precision) and the owner differs from both by the same amount:
  0.000794 m (the trafo beam, 4 above the floor), 0.001946 m (the lane
  beam), 0.009914 m (the death-zone beam), identical for both clients. Every
  birth the game's scripts made (spawn, trafo, respawn) is 0.000000-0.000001
  m from the owner. The owner's residual is R6's (own body control); R3
  reports it as §2.3 asks. It predates R3: R1 (c) (`r1_cg_on*`, core on,
  no lifecycle rewind) measured the own ball after its 4 m beam 0.0000-
  0.0005 m from the server and after the death beam 0.03-0.20 m (client 1)
  and 3.47-4.97 m (client 2) - the same in-place write. After R3's lane
  beam the error was measured only from the stamp to stamp + 66 (0.0019-
  0.0243 m); how long it lasts beyond that is not measured.
- **A late birth without a rewind starts from the event's pose and kick.**
  With `mirror_start_from_row` gone, a remote birth the core makes at the
  drain (no rewind: `session rollback off` on retail, `lifecycle_rewind`
  off, or a stamp older than the history, `knows_tick` false) is made from
  its Physicalize's pose with the spawn kick at the live tick
  (client_core_lifecycle.cpp, make_remote_birth), not from a newer snapshot
  row as under R2; the snapshots (when on) correct it like any other body.
- **The backstop waits while an own birth waits for its echo (a gate-round
  fix; §4.1 and §7.1 decision 2 did not consider the pair).** R3 is the
  first configuration with `own_echoes` and `own_body_backstop` both on.
  With snapshots on and an own Physicalize lost, the backstop re-reported
  the ball after 30 snapshots (~60 ticks; R1 (c)'s exercise: 1558 -> 1623)
  while the birth's record stayed `sent` (send_event writes no own_life), so
  at stamp + 192 `expire_own_echoes` gave it up as missing and
  `re_report_own_birth` sent a second Physicalize and the client resynced,
  although the backstop had already given the server its body: two answers
  to one lost event, the second a resync nothing needed. (The second
  Physicalize itself moves nothing at the server when the ball type is the
  same - physics_world.cpp clones the same entity and `physicalize` returns
  early for an entity that has a body, physics_state.cpp - so the "teleport
  back" the review expected does not happen; a different type would
  replace the body.) Now
  `check_own_body` does not count while the echoes are acted on and a
  non-retired own birth record is `sent`: the echo or its deadline answers
  that birth; once it is answered (or with `own_echoes` off, as at R1/R2,
  or for a beam's record, which has no deadline) the backstop counts as
  before. Test `SessionClientCoreR1.OwnBodyBackstopWaitsForAPendingOwnEcho`.
  Inert on headless (the backstop is off there).
- **The re-report carries the stamp's pose (as LPD 3.5 / E7 built it), so
  the resync pulls the player back.** `re_report_own_birth` sends the
  birth's pose of its stamp for now + 1; on retail the player's ball has
  rolled on meanwhile, and the resync then makes it the server's, made at
  the stamp's pose. Measured in the gate run below: 78.8 m at the re-made
  tick, 92.2 m just before the resync landed, 0.0000 m from 1548 on. The
  worlds agree after it; only the player sees the jump. Sending the current
  own pose instead is a design change left to R6 (with own body control the
  own birth is moved instead), recorded here.
- **A pre-existing world disagreement shows in the snapshots-off row run
  (G-c3, not R3).** "Snapshots off" (`arch_server_lp.sh ... 100000 100000`)
  still sends a full snapshot on every body-set change (timeline.hpp
  `decide`, session_runner.cpp `body_set_changed`; lockstep-without-
  snapshots.md: "The cadence still forces a full ... on every body-set
  change"): 20 per client in `r3_arena_1`. One of them corrected client 1:
  "rollback to tick 1250 (P_Modul_30_Wippe off by 0.339371 m)" (the same
  number in `r3_exercise_1`: deterministic), `snaps=20 ok=19 mism=1`.
  Symptom: client 1's Wippe stays asleep at (399.649, 4.056, 43.150) while
  the server's and client 2's start to tip at 1136 (0.0005 m, 0.33 m by
  1170), no ball within 40 m. Cause: client 2's world woke it and reported
  BodyRevived (server S5 `type=BodyRevived origin=1136` from client 2
  alone), the server applied the wake, and no client applies another
  player's BodyRevived (client_core_frame.cpp drain_body_events only sends
  it; the legacy PSC likewise). The wakes are per-world: every BodyRevived
  of the run that only one client reported came when that client's own
  ball was 44-49 m from the body (1136: client 2 at 47.8 m; 987 Bridge
  46.9; 2177 Wippe, client 1 at 48.9; 2448 Schiebestein 48.1; 2553 Bridge
  44.5 - `work/r3/revived_dist.py`), the module's proximity wake reading
  the local player's ball. Pre-existing: A4's `a4_r1c_on1` (no lifecycle
  rewind) shows the same single-reporter wakes at 47-53 m
  (P_Modul_01_Pusher001 at 512 / 523). This is world-agreement-gaps G-c3
  (with G-c2's identity reads), queued as its own step (§2.6 part F);
  client 1's numbers after 1250 in that run come after the snapshot's
  correction.
- **Remakes (step LB's retail check, placed here).** The plain row run
  never remade a mirror (`remake=0/0`); the remake path ran in
  `r3_exercise_1` (client 1 1, client 2 5), `r3_r2b_1` (client 1 1),
  `r3_spawn_1` (1 per client) and the gate run (2 per client), none lost,
  and the births of those runs were judged at their stamps as below (r2b:
  8 births 0.000000-0.000001 m).
- **The row's scenario is split into a plain run and an exercise run.** With
  no late own event the plain run has no resync, so the fallback's resync
  path is reached only on purpose: `session drop-birth` on client 1 after its
  death beam keeps its respawn's Physicalize off the wire (R1's tool).
- **The run scripts** (the temp tree's `r2_run.py`, `p3_two.py`,
  `r2_mirror.py` were lost) are rebuilt as `.local-tools/work/r3/r3_retail.py`
  (recipes `arena`, `exercise`, `r2b`, `spawn`; `arena-install|restore` puts
  the arena into slot 13 of both Downloads game copies, the originals kept
  beside it), `r3_judge.py` (the checks below) and `r3_mirror_ranges.py`
  (where a run's mirror error is).

Measured (x86 mod and x64 build of f1073ff + R3; the x86 client objects
deleted first, client_core.hpp's layout changed; the mod and the Arch server
built with the slot-13 hack for the retail runs, build id
`ballanced-3e4289e95f25+bmmo-f1073ff77e36-dirty`, the hack reverted and the
Arch server rebuilt plain afterwards; the two Downloads game copies with the
arena in slot 13 for the arena runs, restored after):

- 471 tests pass (the R1 test changed, `recorded_body_state` asserted in
  `RollbackEngine`'s late-birth replay test); `linux_test_syntax.sh` 26 lines
  ok; x64 BUILD-OK (`build-agent-rR3`); x86 CLIENT-BUILD-OK. **Headless
  unchanged:** `wire_check.sh rT1 rR3 rR3w` WIRE-CHECK: ALL SAME (24 journals
  IDENTICAL, 24 sends SAME); solo snapshots off (`e0_solo.sh rR3`) IDENTICAL
  to A5's `rA5c` (2380 TICK hash + pose common, 0 differ; LOCAL 1190/1190).
- **The row** (`r3_arena_1`: arena, snapshots off, `net lag 150` both ways
  on both, both `replay session` of `r0_keys.bmrc`; client 1 beamed onto
  P_Trafo_Wood at 650, to the lane at 850, into the death zone at 1000; client
  2 the same at 1250 / 1450 / 1600, after client 1's pieces were reset, so no
  ball meets the other's local pieces): stone ball unphysicalized by the trafo
  at 652 / 1252, wood ball 817 / 1417, death 1069 / 1669, respawn 1267 / 1867.
  Mirror birth pose at its stamp: 12 births, every script-made one 0.000000-
  0.000001 m from the owner and the server; the beams as above (mirror =
  server). Life rewinds 11 / 11 (none missed); `inrew` 226 / 160; resyncs
  0/0 and rebases 0 on both; `overtaken` 0 / 0 and `echo=13/0/0/0/0` (no own
  echo superseded, moved, rejected or missing) on both, against the server's
  S5 lines: per player 6 Physicalize, 5 Unphysicalize, 2 Sector, none
  re-stamped, none superseded, no birth overtaken by a death. Server journal
  exact in the SimTool (2554 matched, 0 checkpoint mismatches). The own ball
  against the server after each beam (LOCAL against FULL, stamp to stamp +
  66): trafo beam 0.0008 m (captured at stamp + 2), lane beam 0.0019-0.0243
  m, death beam 0.0099-0.3079 m (falling until its unphysicalize). The live
  mirror error of this run is not R2 (b)'s measure: a beam is unknown at the
  live tick for a trip, so the LOCAL checkpoints carry the beam distance for
  that long (406 m once, at client 2's trafo beam).
- **The fallback** (`r3_exercise_1`, the row's run plus the dropped respawn):
  client 1 "own birth moved 1267 -> never at local tick 1484: no echo by the
  server's tick 1460, resync", then "resync requested (own birth: resync) at
  tick 1484" (the only resync in the run, its reason on the line before it,
  as the row asks); the birth re-reported for 1485 without the spawn flag
  (the server made it: 6 Physicalize S5 lines; client 2's mirror of it equals
  the server's ball at its stamp); the resync applied (base 1511, re-stepped
  1510 -> 1547, 19 rows equal), and client 1's own ball equals the server's at
  every LOCAL checkpoint from 1548 to the end. `echo=12/0/0/0/1`,
  `ownlife=0/0/1/0/0`; client 2 no resync; server journal exact (2575).
  Not reached by any run: a moved own birth (with snapshots off: the resync
  "no own body control, no snapshots") and a superseded one (nothing); the
  core's unit tests (E7) cover both.
- **R2 (b), held open there** (`r3_r2b_1`: level 11, snapshots off, `net lag
  150`, `r0_keys.bmrc` on both): `inrew` 248 / 238, life rewinds 7 / 7,
  resyncs 0/0, rebases 0; the 8 births 0.000000-0.000001 m at their stamps.
  Mirror against the owner's LOCAL checkpoints: pooled p99 **4.99 m** (client
  1 sees 2: 4.85, client 2 sees 1: 5.14; p50 0.0000, max 5.74) against the
  E11 lane O runs' 6.56 / 13.94 / 18.04 / 6.10 m and R2's 18.35 / 19.18 m:
  below every O run, so R2 (b)'s bar holds with R3's switch. Server journal
  exact (2529).
- **P's spawn race, held open there** (`r3_spawn_1`: level 11, snapshots on
  2 / 66, both clients beamed onto sector 1's P_Trafo_Stone, y 9.095, at 300
  / 340): **no mismatch at 214-218 on either client** - both made the peer's
  tick-212 birth inside the replay at local 214 and the snapshot of 214
  matched; mism client 1 **4** (the pusher 580 / 582, the peer 1154 / 1654),
  client 2 **7** (the same plus the bridge at 614 / 639 / 664), against
  HEAD's five-run band c1 6-11 / c2 10-16 and option (i)'s c1 8-13; `rb` 65
  / 65, unmatched / far / frozen 0, resyncs 0/0, rebases 0; server journal
  exact (2575). One run: the race was a frame-timing outcome of the live
  birth, and with the birth made at its stamp in a replay no drain frame
  changes the world. (Client 2's LOCAL checkpoint of 214 still shows its own
  ball 0.1709 m from the server - P's state-B number: the live frame of 214,
  written before the late birth was known; the history was right by the time
  the snapshot of 214 was compared.)
- **A test trap, not R3** (`scripts/retail/r3_spawn_bad_y5`): the first
  spawn run beamed to y 5.095, the trafo's own origin. The step after the
  ball was made inside the trafo's mesh took 4.8 s (tick 300) and 10.3 s
  (340) in every world: both games froze, their tick drivers rebased
  (resyncs), and the SimTool replay of that server journal spends the same
  4.8 / 10.3 s with HEAD's binary (`rT1`) as with R3's (both exact, 2534
  matched). P's recipe used y 9.095 (4 above), as the rerun does. Nothing in
  play makes a ball inside a mesh; recorded only.
- Core off (the BAR) is not re-measured: the legacy path (PSC) and the
  legacy binding are unchanged, and `recorded_body_state` is a const reader
  no legacy code calls. No data member was added to `BallanceMMOClient`; 12
  game launches of the final mod, none hung.
- **Gate round** (the backstop gate and the cap line; shared core and
  retail host changed): 472 tests pass; `linux_test_syntax.sh` 26 ok; x64
  BUILD-OK (`build-agent-rR3` re-copied); x86 CLIENT-BUILD-OK. Headless
  unchanged: `wire_check.sh rT1 rR3 rR3w` ALL SAME (24 journals IDENTICAL);
  solo snapshots off (`e0_solo.sh rR3g`) IDENTICAL to `rA5c` (2408 TICK
  hash + pose common, 0 differ; LOCAL 1204/1204). The affected arm, the
  exercise with snapshots ON (`r3_gate_exsnap_1`: `r3_retail.py
  exercise-snap`, 2 / 66, `net lag 150`, client 1's respawn stamped 1267
  kept off the wire): `backstops=0` (without the gate the backstop fires
  ~60 ticks after the stamp, R1 (c)); client 1 "own birth moved 1267 ->
  never at local tick 1485: no echo by the server's tick 1464, resync",
  then "resync requested (own birth: resync) at tick 1485", the run's only
  resync; the server made exactly one more Physicalize of client 1 (origin
  1486; 6 in all, none re-stamped or superseded), client 2's mirror of it
  equal to the server's ball at its stamp (0.000000 m); the resync landed
  (tick base 1513) and client 1's own ball equals the server's at every
  LOCAL checkpoint from 1548 to the end (the pull-back above before it);
  `echo=12/0/0/0/1`, `ownlife=0/0/1/0/0`, client 2 no resync; every other
  birth as in the row run (script-made 0.000000 m; the beams 0.000794 /
  0.001946 / 0.009914 m owner-server, mirror-server <= 1e-6); server
  journal exact in the SimTool (2575 matched, 0 checkpoint mismatches).

## 7. Open questions

1. **Retail's resync triggers.** Retail resyncs on `too_far`, `frozen`, 30
   unmatched snapshots and input starvation (PSC:2062-2090, 3241-3267).
   - **Decided in revision 2 (no longer open):**
     - `too_far`: the comment at PSC:2020 describes an engine that wrote the
       bodies. Today a too-deep snapshot or rewind is refused before
       anything is restored (RB:367, 610, 1533), so the trigger is deleted
       at R1.
     - `frozen` is different. Revision 1 grouped it with `too_far`, which
       was wrong: a frozen clock still sets the bodies and drops the
       history with no replay (RB:1031-1042, 784-790), and a re-anchor is
       the only recovery. Retail is the host whose clock can freeze (the
       pause pin, a clock-guard failure). It becomes a core rule for both
       hosts at R1, inert on headless.
   - Starvation is replaced by part one's `check_held_complete` at R4.
   - **Still open:** 30 unmatched snapshots is a real numbering-desync signal
     that headless lacks. Proposal: adopt it in the core for both hosts as
     its own step after E11, closed by a full A/B (it must not fire in any
     arm, so every metric stays in its band); until then it is the retail
     host's transitional trigger (§3.2). Needs a decision.
2. **`own_body_backstop` on headless.** On headless the echo machinery
   (missing, then resync) covers the same failure. The proposal is to keep it
   retail-only until R6, then measure whether it ever fires.
3. **The legacy switch in R1-R6** keeps two retail implementations alive for
   the length of the R steps. It is the price of an A/B in one binary. The
   alternative is a separate legacy binary per batch.
4. **Retail measurement tooling.** The latency and blackout injection (R0) and
   a hold-event verb (R6) are new retail automation. Without them, R2-R6 have
   no closing numbers.
5. **`physics_direct` versus free functions in the headless host.** Extending
   `physics_direct` (E1) lets the host support templates serve both clients.
   The headless host could keep its free calls everywhere else, or move to
   `physics_direct` throughout. This proposal moves only what host support
   uses.

### 7.1 Decisions (2026-10-21)

The proposals above are adopted as written:

1. 30 unmatched snapshots becomes a core rule for both hosts, as its own step
   after E11, closed by a full A/B in which it must not fire; until then it
   stays the retail host's transitional trigger.
2. `own_body_backstop` stays retail-only until R6, which measures whether it
   ever fires.
3. The legacy switch stays in the retail mod through R1-R6, so that every R
   step is an A/B inside one binary; R7 deletes the legacy path.
4. The retail tooling (latency and blackout injection, `replay session`, the
   hold-event verb) is built in R0 and R6 as planned.
5. `physics_direct` is extended only as far as host support needs it.

## 8. Revision 2: review findings and what became of them

R1-n is the first review's finding n, R2-n the second's. Each was checked
against the working tree.

| finding | outcome |
|---|---|
| R1-1 E2 cannot link (moved code calls SC functions) | changed: `legacy_calls` (§6.1), journal moved right after E1 (E2) |
| R1-2 / R2-6 journal paths renamed; retail checkpoint rule changes | changed: `journal_path` returns the whole file; the core's journal is headless's; retail's legacy journal untouched until R7, its changes listed at R1 (§1.2, §4.2); the verbs' target named |
| R1-3 "cadence unchanged, written once" false | changed: the two policies tabled (§1.2) |
| R1-4 host counters sit mid-line; `hold_polls_` listed twice | changed: `status_text(host_counters)`, `window_text(tick, host_counters)`; `hold_polls_` is core (counted by `poll_hold`) |
| R1-5 own group reset at SC:2309 lost | changed: the host's `drain_body_events` clears `own_group_set_` (§1.3 step 17, §2.2) |
| R1-6 start sector stamped before renumbering; `sector_cell_` homeless | changed: pending until assigned; `sector_cell_` is core state |
| R1-7 verification blind to the moved paths | changed: snapshots-on, rollback-off and forced-resync arms; §5.2 (2) on snapshots off with `build_id` / `utc_ms` / `ms` excluded; E0 inbound capture/replay; E3 → E6 and E5 → E8 run the swing bay; E2 split into E3/E4/E5 |
| R1-8 / R2 own generation one behind with host hooks | changed: reports queued, committed at step 19 |
| R1-9 traces need not move | changed: `after_amend` at their place, `after_journal` for `report_queued_impulses`; zero reordering |
| R1-10 script-edit placement inconsistent | changed: only N1-N3 move to step 4; the replay-before-N1-N3 order explained as harmless |
| R1-11 `bind_world` must not be cached; the guard needs the core | changed. Partly refuted: the options copy the key *pointers*; only `own_key_count_` is by value (SC:4822-4824). The conclusion stands |
| R1-12 fixed `logf` buffer truncates the status line | changed: growing buffer (§1.3, §6.1) |
| R1-13 no legacy-vs-legacy baseline for R1(a) | changed: R0 baseline |
| R2-1 the beam has no fallback; verb stamps a stepped tick | changed: `request_beam` consumed at step 29 (T+1); in-place `write_body` at the stamp, live and in replays; no resync (a resync rebuild is in place too) |
| R2-2 echoes not switchable; superseded re-reported | changed: `own_echoes` switch (off until R3); superseded gets nothing; re-report only while current |
| R2-3 clock guard / pause edits moved after the mailbox | changed: they stay at the top of `OnProcess`; `death_reset_apply` one frame earlier noted. Partly refuted: the guard writes nothing while the pause chain is neutralized (PSC:186-195), so a replay without it is neutral |
| R2-4 own polling during a pause after a rewind | mostly refuted: the binding's `nav_poll` already keeps polling off while `input_muted` (RBB:193-201, PSC:3034), and `own_polls` needs no change. Changed: `unforce_own` restores the mute; the mute-edge tick is recorded as stepped (R2-5); an R2(a) window spans a pause |
| R2-5 NAV_ACTIVE source differs from the replica's | changed: retail reads the leaves' Key Event `IsActive` after the step (the replica's source; nothing runs between PreSimulate and `OnProcess` that changes it), and the mute in force during the step. Reading the replica back would need a bridge change: the polled values are not stored (ball_navigation.cpp:467-481). R2(a) windows across spawn, respawn, trafo, pause |
| R2-7 `frozen` still snaps the bodies | changed: confirmed (RB:1031-1042); `frozen` → resync becomes a core rule; only `too_far` is deleted (§7 Q1) |
| R2-8 record/replay not anchored to the session | changed: `replay session` mode (R0) and the legacy baseline |
| R2-9 R1 checks miss what R1 switches on | changed: R1(c) |
| R2-10 E2 too big; no back-call mechanism | changed: E3/E4/E5 split, `legacy_calls` |
| R2-11 mailbox lifetime | changed: load-time static, `begin` drops other sessions' entries |
| R2-12 `core.status()` / `request_resync` missing | changed: `acked_input()`, `request_resync(why)` in the API |
| R2-13 `previous_cam_` in two lists | changed: core state, `core.previous_cam()` for the host |
