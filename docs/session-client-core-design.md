# Session-client core: one implementation of the client flow, two hosts

Status: design, revision 2 (two reviews answered; §8 lists every finding and
what became of it). Nothing here is built yet.

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
  770/797/807) become `hooks.numbering_restarted()`. Headless resets `origin_`
  there. Retail does nothing, because `fixed_tick_` counts its own ticks and
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
| now | `steady_clock::time_point()` | `clock_type::now()` | `steady_clock::now()` |
| log | `void(log_level, const std::string&)` | `logf` (stdout; the harness parses it, so the text is kept verbatim) | `logger_->Info/Warn` |
| **reading the world** | | | |
| body_state | `bool(const std::string& entity, body_state&)` | `get_body_state(physics, …)` | `physics_view_.get_body_state` |
| world_hash | `bool(world_hash&)` | `capture_world_hash` | `physics_view_.capture` |
| list_bodies | `std::vector<bmmo_physics_body_info>()` | `list_bodies` via the `physics_direct` adapter (`physics_direct.hpp:95-102`) | `physics_view_.list_bodies()` |
| movable_mechanisms | `std::vector<std::string>()` | `movable_body_names(physics, "Ball_")` (SC:3842-3845) | the `list_bodies` filter of PSC:1956-1960 |
| own_ball | `own_ball_view()`: `{entity, ball_type, physicalized, state}` | `current_ball`, `ball_type_of`, `get_body_state` (SC:2209-2220) | `get_current_ball`, `db_.get_ball_id`, `get_body_state` (PSC:1656-1759) |
| own_body_upkeep | `void(const own_ball_view&)`: body guard, own group, own navigation attach | SC:2212-2227 (navigation only with rollback: a host policy) | PSC:1749-1775 |
| drain_body_events | `std::vector<body_event>()`: `{created, deleted, wakeup}` + name; recipe entries are kept by the host (the local birth log); an own `deleted` clears the host's `own_group_set_` (SC:2309) | `drain_event_log` + the shared parser (§2.4) | `physics_view_.drain_event_log` + the same parser |
| reportable_wakeup | `bool(const std::string& name, const std::string& own_ball)` | not `Ball_*`, not `*_BMMO_*` (SC:2311-2312) | not `Ball_*`, not the own ball, not `*_Peer_*` (PSC:1846-1847) |
| observe_input | `input_observation()`: `{keys, cam[3][3], cam_valid, nav_active, paused, ball_type, physicalized}`: **what the own replica used for step T** (every replay drives the own ball from this record) | the record's keys through `keys_from_state`, `Cam_OrientRef`, `navigation_active()` (the leaves' Key Event `IsActive`, SC:1912-1918) | `frame_keys_` through `keys_from_state`; `Cam_OrientRef`; **nav_active from the leaves' Key Event `IsActive`**, the source the replica polls (ball_navigation.cpp:469-481), not `ball_nav_active_` (the BML hook); **paused = the mute in force during step T** (the previous frame's decision), not the one decided now (PSC:1874-1883 applies a new mute from T+1 on, so today the edge tick is recorded keys 0 + PAUSED while the replica polled live keys). Changed at R2 (§4.2) |
| poll_sector | `sector_poll()`: `{cell, resetpoint_moved, checkpoint_held}` | `IngameParameter[0][1]`, `level_resetpoint` (SC:2459-2470) | `IngameParameter[0][1]`; `checkpoint_held = sector_activate_.block && !failed`; the resetpoint is not used (the `OnPostCheckpointReached` hook reports through `request_sector`) |
| sector_manager_active | `bool()` | `Gameplay_SectorManager->IsActive()` (SC:2100-2104) | `physics_session_sector_manager_active` |
| **writing the world** | | | |
| write_body | `bool(const std::string&, const session::body_state& row, bool wake)` | `set_body_state` (resync rebuild SC:2803-2822; legacy ladder) | `physics_view_.set_body_state` (PSC:3290-3351) |
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
| bind_world | `rollback_world(const core_world_hooks&)`. The core passes its hooks; the host binds through the shared `bind_session_world` (§2.4). **Called on every use, never cached**, as SC:4812 is: the options take `own_key_count_` by value (SC:4822-4824), and a world bound before the own navigation was attached would replay with a key count of 0 | `physics_direct_` + `args_` options | `physics_view_` + fixed options, `input_muted` |
| **journal** | | | |
| journal_path | `std::filesystem::path(const journal_header&)`; empty = no journal | `args_.journal` verbatim | `client_journal::directory()/file_name(...)`, after pruning to 10 files |
| **host hooks** (all optional, H) | | | |
| hooks.after_rewinds / after_record / after_amend / after_journal | `void(uint32_t tick)` | `maybe_force_rollback`, `dump_savestate`, traces, `report_queued_impulses` | a forced-rollback verb (R0/R2), nothing else |
| hooks.filter_event | `event_send_verdict(const session_event_msg&)`: send / hold / drop | `--hold-event` (SC:2662-2703); the host later calls `core.release_event` | empty |
| hooks.numbering_restarted | `void()` | `origin_ = now()` | empty |
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
| **E9** the frame | `pre_step` and `post_step` (§1.3 tables) replace the body of `frame()`; the host keeps keys, step and hooks. Input building, record, amend, drive; `bind_session_world` and the rest of host support (§2.4), used by headless only; `poll_hold`, `slew_step`, `rebase_threshold`, `schedule_rebased`, `hooks.numbering_restarted`, `after_amend`, `after_journal`. | §5.1 (frame-order test); §5.2; **§5.3 full A/B** |
| **E10** reports | `status_text(host_counters)`, `window_text(tick, host_counters)`, one literal each; `hooks.window/status`. | §5.2 (4) with zero differences; the harness parses the A/B runs of E9 again from the new binary's logs, and its report is identical to E9's numbers on the same inputs (a lane-control A/B pair) |
| **E11** close the core | Make the state private; delete the dead state (§1.2), the `core_.s.` access and `legacy_calls`; SC ends as the host of §3.1. No logic change. | §5.1; §5.2; **§5.3 full A/B** against the base of E1 (the cumulative check) |

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
| **R1** the frame and the snapshot path | `core on` hosts the facade with `rollback`, `correct` as today. The switches `input_rewind`, `lifecycle_rewind`, `own_echoes`, `own_corrections` and `own_birth_move` are off. `mirror_start_from_row` and `own_body_backstop` are on. `caps` = 0; capabilities: no own body control, no slew, blocking hold, no whole frame, `host_hooks`. The snapshot rollback, resync and its limiter, numbering, record and amend, relay store (no rewind), the events drained at their tick, sectors, the beam (`request_beam`, §2.3) and the core's journal all run through the core. `too_far` → resync is deleted; `frozen` → resync is the core's rule. The §4.2 rows marked R1 change. | (a) **Solo, snapshots off, `replay session`: the TICK hashes of `core off` and `core on` are identical at every tick**, given R0's legacy-legacy baseline (no snapshot, no peer: the R1 order changes cannot show). The journal formats differ (§1.2), so the hashes are compared, not the files. (b) Two clients, snapshots on, 3 runs each of off/on interleaved: per client `snaps/ok/mism/rb`, `max_err`, mirror error, events applied within the off-off band (§5.3 rule); resyncs 0; the server journal replays exact; both client journals replay in SimTool and `worst_bodies.py` reads them. (c) Two clients, snapshots on, `net lag 150`, one death and respawn and one `beam` per client, and one forced resync (a `fixed_tick` stall over 33 ticks on client 2): late events drained at their tick (`events=` as the server's S-lines), the limiter's owed and in-flight path taken once with the resync applied, `own_body_backstop` counted, the own ball's error after the beam reported, everything within the off band. |
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
