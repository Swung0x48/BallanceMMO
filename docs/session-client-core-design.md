# Session-client core: one implementation of the client flow, two hosts

Status: design, revision 2 (two reviews answered; §8 lists every finding and
what became of it). Steps E0 (§5.2, "As built (step E0)"), E1, E2, E3, E4
and E5 (§6.1, "As built (step E1)" ... "As built (step E5)") are built.
Nothing else is yet.

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

- `mirror_start_from_row`, needed only while `lifecycle_rewind` is off;
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
    std::string window_text(uint32_t tick, const host_counters& h);   // SC report_window(); "" outside a window edge
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
| 26 | own sector detection: `platform.poll_sector()` → `{cell, resetpoint_moved, checkpoint_held}`; the core decides between `hold_sector(s, T + 1 + lead)` and a Sector event after the fact | SC:2459-2486 | P + C |
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
| world_hash | `bool(world_hash&)` | `capture_world_hash` | `physics_view_.capture` |
| list_bodies | `std::vector<bmmo_physics_body_state>()` | `list_bodies` via the `physics_direct` adapter (`physics_direct::list_bodies`, added at E1) | `physics_view_.list_bodies()` |
| movable_mechanisms | `std::vector<std::string>()` | `movable_body_names(physics, "Ball_")` (SC:3842-3845) | the `list_bodies` filter of PSC:1956-1960 |
| own_ball | `own_ball_view()`: `{entity, ball_type, physicalized, state}` | `current_ball`, `ball_type_of`, `get_body_state` (SC:2209-2220) | `get_current_ball`, `db_.get_ball_id`, `get_body_state` (PSC:1656-1759) |
| own_body_upkeep | `void(const own_ball_view&)`: body guard, own group, own navigation attach | SC:2212-2227 (navigation only with rollback: a host policy) | PSC:1749-1775 |
| drain_body_events | `std::vector<body_event>()`: `{created, deleted, wakeup}` + name; recipe entries are kept by the host (the local birth log); an own `deleted` clears the host's `own_group_set_` (SC:2309) | `drain_event_log` + the shared parser (§2.4) | `physics_view_.drain_event_log` + the same parser |
| reportable_wakeup | `bool(const std::string& name, const std::string& own_ball)` | not `Ball_*`, not `*_BMMO_*` (SC:2311-2312) | not `Ball_*`, not the own ball, not `*_Peer_*` (PSC:1846-1847) |
| observe_input | `input_observation()`: `{keys, cam[3][3], cam_valid, nav_active, paused, ball_type, physicalized}`: **what the own replica used for step T** (every replay drives the own ball from this record) | the record's keys through `keys_from_state`, `Cam_OrientRef`, `navigation_active()` (the leaves' Key Event `IsActive`, SC:1912-1918) | `frame_keys_` through `keys_from_state`; `Cam_OrientRef`; **nav_active from the leaves' Key Event `IsActive`**, the source the replica polls (ball_navigation.cpp:469-481), not `ball_nav_active_` (the BML hook); **paused = the mute in force during step T** (the previous frame's decision), not the one decided now (PSC:1874-1883 applies a new mute from T+1 on, so today the edge tick is recorded keys 0 + PAUSED while the replica polled live keys). Changed at R2 (§4.2) |
| poll_sector | `sector_poll()`: `{cell, resetpoint_moved, checkpoint_held}` | `IngameParameter[0][1]`, `level_resetpoint` (SC:2459-2470) | `IngameParameter[0][1]`; `checkpoint_held = sector_activate_.block && !failed`; the resetpoint is not used (the `OnPostCheckpointReached` hook reports through `request_sector`) |
| sector_manager_active | `bool()` | `Gameplay_SectorManager->IsActive()` (SC:2100-2104) | `physics_session_sector_manager_active` |
| **writing the world** | | | |
| write_body | `bool(const std::string&, const session::body_state& row, bool wake)`; as built (E4): `bool(entity, const double position[3], const double rotation[4], const float linear[3], const float angular[3], bool wake, std::string& error)`, the bool overload of `set_body_state` | `set_body_state` (resync rebuild SC:2803-2822; legacy ladder) | `physics_view_.set_body_state` (PSC:3290-3351) |
| mute_event_log | `void(bool)` (the core keeps the nesting counter, SC:4607-4620) | `set_event_log_muted` | `physics_view_.set_event_log_muted` |
| keep_live_cells | `scope_guard(const session_client_core&)`, RAII around every `rewind_to` / `on_snapshot` (SC:2874, 3709, 3807, 4342, 4401); the guard reads `core.rollback_stats()` and `core.current_tick()` (SC:5728-5738) | `live_cells_guard` (SC:5719-5739) | the same write-back of `CurrentLevel[0][4]`, `IngameParameter[0][1..2]` + `replay_manager_activation` (PSC:2037-2046) |
| run_sector | `void(int sector)` | SC:1346-1357 | PSC:2962-2986 |
| record_activation / record_pending_activation | `void(uint32_t tick, bool reset)` / `void(uint32_t tick)` | `record_manager_activation` (SC:5695-5711, 2180-2184) | `record_manager_activation` (PSC:64-88, 2032-2036) |
| script_guards | `void(uint32_t tick, bool resolved)` | `reset_guard_.apply` | `death_reset_apply`, `sector_deactivate_apply`, `sector_activate_apply` (the clock guard, pause pin and `pause_scripts_apply` are host code before `post_step`, §1.3) |
| note_script_resets | `void(uint32_t tick)` | `script_replay::resets_pending` → `script_reset_ticks_` | the same (PSC:1964-1965) |
| **driving** | | | |
| drive | `void(const std::string& entity, const input_frame&)` | `navigation_input` | `physics_view_.navigation_input` |
| own_camera_rows | `void(const float basis[3][3])`: own replica rows + zero the retail forces | SC:2363-2367 | PSC:1905-1911 |
| force_own / unforce_own | `bool(const input_frame&)` / `void()` | `navigation_poll(off)` + `navigation_input` (SC:2149-2161) / `navigation_poll(on)` (SC:2168) | the same calls; `unforce_own` restores `navigation_poll(!input_muted)`, never plain on. (After a replay the shared binding already keeps polling off while muted: `nav_poll` gates on `input_muted`, RBB:193-201, PSC:3034. `tracked.own_polls` needs no change.) |
| own_driven_entity | `std::string()`: the own replica's entity, "" if none | `own_nav_entity_` | `s.own_nav_entity` |
| own_replica_name (added at E5, built at E9) | `std::string()`: the entity the own replica was last named by, kept after the replica is gone (a failed re-attach); "" if it never had one. `recorded_input_at` tells the own ball's records by it (SC's `entity == own_nav_entity_`, without the `own_navigation_` gate that `own_driven_entity` has) | `own_nav_entity_` | `s.own_nav_entity` |
| arm_own_capture | `void(uint32_t tick)` | `arm_own_physicalize_capture` | empty |
| **mirrors** | | | |
| make_mirror | `std::optional<mirror_made>(const mirror_spec&)`; the spec is `{player, ball_type, pose[12], recipe, group, kick?{index, strength}}`; the result is `{entity, navigated}` | `remote_entity` (CopyObject) + the shared `make_mirror_body` (§2.4) | `objects_.ensure_player` / `get_ball_entity` + the shared `make_mirror_body`; then `objects_.set_physicalized` / `on_trafo` |
| unmake_mirror | `void(const std::string& entity)` (navigation destroyed, body unphysicalized, log muted by the core) | SC:4415-4428, 4636-4643 | PSC:2439-2448 |
| discover_navigation | `bool()`: the leaf keys are known | `read_navigation_graph`, every leaf keyed (SC:2189-2196) | `read_navigation_graph` with the unbound-leaf filter, ≥ 4 leaves (PSC:1716-1742) |
| attach_mirror_navigation | `bool(uint32_t player, const std::string& entity)` | shared `attach_navigation` (SC:4734-4805) | shared `attach_navigation` (PSC:3451-3475) |
| **own ball** (only with `own_body_control`) | | | |
| make_own | `bool(const own_body_spec&)`: `{entity, pose[12], recipe, group, kick?}` | `remake_own_body` (SC:1547-1588) | empty (the game's Physicalize block owns the ball) |
| unmake_own | `void(const std::string& entity, const float pose[12])` | `unmake_own_body` (SC:1522-1537) | empty |
| has_body | `bool(const std::string& entity)` | `get_body_state` probe (SC:1639-1643) | `physics_view_.get_body_state` |
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
| hooks.window / hooks.status | `void(const std::string&)` | `logf` | the log, plus the F3 overlay reads `status_text` |

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
  sector pending (step 1).
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
    created at the first SessionStart;
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
  `maps/arena-test/core_ref_e0/`. "Old" for the later E steps is the E0
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
   host-load guard. Each run lists its clients' rebase ticks. The step
   passes an arm when all of these hold:
   - no client crashes, and every client reaches the end;
   - every server journal replays with rc 0 and 0 checkpoint mismatches;
   - `far`, `overtaken`, `unmatched`, `lead_adj`, and the late mirror stay 0
     in both N runs wherever they are 0 in both O runs;
   - each N run's harness criteria equal those of an O run of the same arm,
     in this batch or an earlier batch of the same arm.

   The band rule is still computed and reported, but it does not decide the
   step. A metric outside the band, or a zero broken, must be traced, from
   the journals and logs, to an event of the run rather than of the code.
   The events (widened at E4 from rebase ticks alone) are:
   - a pacer rebase and its resync;
   - a snapshot delivered deeper than the history reaches (its delivery
     depth names it);
   - a lifecycle event the server applied late ("late by" in the server log)
     and the resync it caused;
   - the teardown, after the run's first client reached its time limit.

   Any difference not traced to one of these fails the step. The traces
   are listed in the step's note.

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
it. A step's review lists the entries it added and removed. The journal moves
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
| **R1** the frame and the snapshot path | `core on` hosts the facade with `rollback`, `correct` as today. The switches `input_rewind`, `lifecycle_rewind`, `own_echoes`, `own_corrections` and `own_birth_move` are off. `mirror_start_from_row` and `own_body_backstop` are on. `caps` = 0; capabilities: no own body control, no slew, blocking hold, no whole frame, `host_hooks`. `core.release_hold(why)` is built here (E3 left it out: no headless path calls it): the blocking hold's release when the connection is lost. The snapshot rollback, resync and its limiter, numbering, record and amend, relay store (no rewind), the events drained at their tick, sectors, the beam (`request_beam`, §2.3) and the core's journal all run through the core. `too_far` → resync is deleted; `frozen` → resync is the core's rule. The §4.2 rows marked R1 change. | (a) **Solo, snapshots off, `replay session`: the TICK hashes of `core off` and `core on` are identical at every tick**, given R0's legacy-legacy baseline (no snapshot, no peer: the R1 order changes cannot show). The journal formats differ (§1.2), so the hashes are compared, not the files. (b) Two clients, snapshots on, 3 runs each of off/on interleaved: per client `snaps/ok/mism/rb`, `max_err`, mirror error, events applied within the off-off band (§5.3 rule); resyncs 0; the server journal replays exact; both client journals replay in SimTool and `worst_bodies.py` reads them. (c) Two clients, snapshots on, `net lag 150`, one death and respawn and one `beam` per client, and one forced resync (a `fixed_tick` stall over 33 ticks on client 2): late events drained at their tick (`events=` as the server's S-lines), the limiter's owed and in-flight path taken once with the resync applied, `own_body_backstop` counted, the own ball's error after the beam reported, everything within the off band. |
| **R2** relay rewind | `input_rewind` on: remote corrections rewind (`replay_corrected_inputs`, `replay_remotes` in the binding). `observe_input` records what the replica stepped (NAV_ACTIVE from the leaves, the mute in force; §2.2); `unforce_own` restores the mute. | (a) **Solo forced rollback**: `session force-rollback 50:12`, `97:24`, `131:47`, plus windows across the first spawn, a respawn, a trafo and an ESC pause held over a nav-activity edge, against a control with the same record. The three original windows may cross no nav-activity edge and would pass with the NAV_ACTIVE bug in place. `worst_bodies.py` 0.0 m on every body (the retail counterpart of §5.2 (1)). It measures what the non-selected retail scripts do inside a replay: a non-zero result names the script, and the step stays open. (b) Two clients, snapshots off, `net lag 150`: `inrew` > 0, `far` 0, resyncs 0. Each client's mirror of the other against the owner's own journal checkpoints (2 ticks): p99 ≤ the headless lane control's at 150 ms (E11's O runs). |
| **R3** lifecycle | `lifecycle_rewind` and `own_echoes` on, `mirror_start_from_row` deleted. Own echoes processed by the core with the `own_body_control` fallback (moved → snapshot or resync; rejected/missing → re-report while current; superseded → nothing). | Two clients, snapshots off, `net lag 150`, with deaths in the arena's death zone, a respawn and a trafo per client (scripted by key record and `beam`). Mirror birth pose at its stamp 0.0000 m against the owner's checkpoint; life rewinds > 0; `overtaken` and `superseded` as the server's S-lines; resyncs only from the capability fallback, each one logged with its reason; the server journal exact. |
| **R4** part one | `own_corrections` on: held notices amend the own records and rewind. The starvation detector is deleted. LPD §1.4 is corrected. | Two clients, snapshots off, `net lag 150`, client 2 `net blackout 300 up` at tick 1500 (the A1 analogue): `subst recv` equals the server's S2 count for that client; `subst rew` > 0; no resync; client 2's own ball 0.0000 m from the server's journal after the episode settles (A1's criterion); client 1 unaffected (its numbers within the R3 band). |
| **R5** part two (needs `slew`) | `fixed_tick_driver::slew(ticks)` in `on_process` (2x through `SkipRenderForNextTick`, 0.9x by waiting), `kMaxCatchUpTicks` + \|slew\| (LPD §2.4 "Retail"). The forced frames through `force_own`. The retail host declares `slew`, so `caps` = `CAP_LEAD`. | Two clients, snapshots off, client 2 `net lag` +300 ms uplink persistent from tick 1500 (the A5u analogue), server lead flag on: exactly one raise, `lead_adj` 1, the ack sent, no resync, client 2's substitutions after the raise within the A0 band; client 1's `lead_adj` 0. |
| **R6** part three (needs `own_body_control`) | `own_birth_move` on, once the own-body capability exists on retail: `unmake_own` / `make_own` under the retail Physicalize block. That is a bridge and script-edit job of its own, after LPD step 21's measurement of a multi-tick own absence on retail. Until then this step does not start and the §2.3 fallback stands. | An A6m analogue: the client 2 own Physicalize held 300 ms (a retail `hold-event` verb through `hooks.filter_event`), `net lag 50`: the owner 0.0000 m from T'+48, no resync. |
| **R7** close | Delete the legacy retail session code and the `core` switch; core on always. Shrink `physics_session_state` to the host state of §3.2. | Three launches without a hang; R1 (b), R3 and R4 rerun on the final binary within their bands; `retail_run.py` solo checkpoint and sector run green. |

The R steps do not require each other's measurements to be good, only to be
built. R5 and R6 wait on their capability work; R1-R4 do not.

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
