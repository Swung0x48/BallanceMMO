# Resync re-step with script state, and late sector events

Track 3 of the post-audit work. It closes audit gaps #3, #4 and #5
(`scratchpad/audit/rollback-coverage-audit.md` §1, fixes F3 and F4), the
late-player plan's open steps 13, 14, 23 and 25 (`docs/late-player-design.md`
§6), and the gaps found while designing it (§1.3). Written against HEAD
4882ec2 (R2) with step P in the working tree. **Every line number is the
working tree's of 2026-10-23** (step P is still moving `client_core*.cpp`,
`rollback.hpp`, `script_replay.hpp`, `session_host_support.hpp`,
`session_client.cpp` and `core_host.cpp`; re-check them at implementation).
Scratch evidence is in `scratchpad/design/track3/` (the session scratchpad).
Revised after two reviews; §10 lists every finding and what became of it.

Short names used below:

- **S**: the tick of the state a resync applies (`base - 1`, §2.2).
- **base**: the `first_tick` of the SessionAssign that answers the resync.
- **R**: the client's tick in its *old* numbering when that SessionAssign is
  taken. **Δ = R - (base - 1)**.
- **B**: the client's current tick when the resync state is applied.
- **L**: the lead the server re-anchors the member at (`base` minus the
  server's `next_tick` when it handled the request). **Lc**: the client's
  lead before the resync. **h**: the one-way trip in ticks.
- **Shared tick**: a tick number in the session's one numbering. The client's
  old and new numberings are both shared numberings; a re-assign moves the
  client's *position* in it by -Δ, it does not rename ticks.
- **H**, **RC**, **SV**: the headless client (`sim/session_client.cpp`), the
  retail core host (`BallanceMMOClient/session/core_host.cpp`), the server
  world (`sim/physics_world.cpp` under `sim/session_runner.cpp` and
  `server.cpp`).

## 1. The problem and the rule

### 1.1 Gaps 3 and 4: a resync writes the server's past at the client's present

**What the code does today.**

1. `request_resync` sends SessionResync (client_core.cpp:455-492).
2. The server answers in `handle_session_resync` (server.cpp:1620-1658):
   - it assigns `base = current_tick + L` (`resync_tick_base`,
     server.cpp:1280-1284, or `L_eff` on the per-player path). Its
     `current_tick` is the runner's published `next_tick`
     (session_runner.cpp:104-108);
   - it re-sends every member's last Physicalize and the whole
     `sector_history`;
   - it calls `request_full_snapshot(s.id)` (server.cpp:1654). That forces
     one full snapshot to **every** member at the server's next tick
     (session_runner.cpp:682-687, 715-723, 931-932; server.cpp:1162-1191).
     That tick is S, and S < base.
3. The client's re-assign branch (client_core.cpp:369-397) does the
   following:
   - renumbers from `base` (`tick_base_`, `frames_since_anchor_ = 0`) and
     sets `held_floor_ = base`;
   - clears the input history, the correctors, `own_inputs_` and
     `mechanism_tracking_`;
   - calls `invalidate_history()` and `rebase_own_lives()`;
   - sets `resync_pending_` and `have_snapshot_ = false`.
4. The first full snapshot then takes the resync branch of `apply_snapshot`
   (client_core_messages.cpp:69-136):
   - `invalidate_history()` again;
   - `write_body` of every row onto the live world at B
     (client_core_messages.cpp:94-113), which wakes every body
     (`owner == own_id ? true : wake`);
   - nothing re-steps S+1..B, and no script is touched;
   - `held_floor_` moves past B (client_core_messages.cpp:127-129).

**What that leaves.** Every body is put back to tick S, but the world clock,
the mechanism scripts and the union stay at B. There are two consequences:

- **The scripts keep the client's own phase for the rest of the session.**
  Every Delayer, Sequencer and Proximity countdown, and every in-flight
  delayed link, is the client's pre-resync value, B - S ticks off the bodies
  it drives (gap 3).
  - Later snapshot rollbacks restore the bodies from server rows but the
    scripts from the client's own records, so the error is re-baked at
    every rewind.
  - Measured on Level 11: P_Modul_26_Sack is 4.48 m off from 1528 to 2610
    (`r1_c_on1` c2), 8.27 m (`r1_c_off1` c2, legacy) and 5.70 m
    (`r2_x_t1_b`, both clients), with 41-72 sack corrections after each
    resync. Clients that did not resync stay exact.
- **A one-shot effect between S and B is undone for good** (gap 4).
  - In `r2_x_t1_b` c1, Pusher001 dropped at 508. The resync to base 524
    from S = 496 put it back asleep at its pre-drop pose.
  - It stays 0.086 m off the server from 524 to 2654. That is below the
    widened tolerance. A sleeping in-tolerance row is not authoritative
    (rollback.hpp:603-610), so nothing ever repairs it.

**A third defect that the same write hides.** The ticks S+1..base-1 are not
in the client's new numbering at all.

- The server steps them with the substitute it holds for this player
  (`input_buffer::reanchored`, session_runner.cpp:1130-1135). Its held
  notices never name them (server.cpp:1640-1651, "applied held under no
  numbering the client has").
- The late-player design records this as the reason a resync with
  snapshots off is not exact (late-player-design.md §4, last row of the
  part-3 table), and plans step 25 "Exact resync" against it.

**Measured B - S** (logs in `maps/arena-test/`):

| run | client | base | S | B at apply | B - S |
|---|---|---|---|---|---|
| core_ref_e0/resync_snapoff | c2 | 1572 | 1555 | 1572 | 17 |
| core_ref_e0/resync_snapoff | c0 | 2821 | 2794 | 2821 | 27 |
| core_ref_e0/a5u_lead (lead on) | c2 | 2737 | 2695 | 2737 | 42 |
| r1_c_on1 (retail, L11) | c2 | 1527 | 1499 | ~1527 | 28 |
| r2_x_t1_b (retail, L11) | c1, c2 | 524 | 496 | ~524 | 28 |

The snapshot arrives with the assignment, so B is about `base` and B - S is
about L.

**Measured Δ.** All six resyncs of the arena references are "tick schedule
rebased" resyncs after a stall, and all have Δ < 0: about -18 to -49 (the
last tick logged before `resynced: tick base`, an estimate;
`scratchpad/design/track3/resync_delta.txt`). `r2_x_t1_b`'s old numbering
ran to 518 against base 524 (Δ = -5). A resync from a client that keeps
running (hard corrections, a substituted own frame past the reach) has
Δ ≈ Lc + h - L + 1, which is positive when Lc ≥ L. No reference exercises
that case; §7.3 adds an arm for it. Δ ≠ 0 is the fourth defect: §1.3 G-d.

### 1.2 Gap 5: a late remote Sector stamp starts the sector at the live tick

**What the code does.** `apply_event` (client_core_lifecycle.cpp:328-337)
calls `sectors_.stamp()` for every remote Sector event, late or not.

- A stamp at or before the current tick is counted as late
  (sector_union.hpp:58-65). `tick()` then applies it at the next tick the
  pacer allows (sector_union.hpp:82-104).
- The core records that tick (client_core_frame.cpp:188-201,
  `record_activation`), and every later replay re-issues the activation
  there.
- No rewind is made. A late Physicalize or Unphysicalize, by contrast, gets
  one (client_core_lifecycle.cpp:183-207).

**Measured, core_ref_e0/a5u_lead:**

- The server logs `sector +4 at tick 1714 (player 2518006732, stamped 1714)`
  (server.log:203).
- Clients 0, 1 and 3 received the stamp at local ticks 1717-1722 (each one
  logs `LATE: the server started it already`) and started the sector at
  1723 (`held by the pacer`).
- So the whole sector runs 9 ticks out of phase in three of four worlds for
  the rest of the session. Audit #5 measured the result as Schaukel 2.873 m,
  P_Modul_03 1.36 m and PE_Balloon up to 6.8 m.

**A late stamp that changes nothing.** In core_ref_e0/resync_snapoff,
clients 0, 1 and 3 log a late stamp for sector 4 at 1813. Sector 4 was
already requested from another player's stamp at 1810, so the union
inserted nothing (sector_union.hpp:70-73). The rule in §2.7 keeps that case
a no-op.

**The own Sector moved by the server** (late-player plan step 23). The
server applies an own Sector event at `max(origin, N)` and echoes MOVED when
that is later than the stamp. The core counts the echo and keeps the sector
where it stamped it (client_core_lifecycle.cpp:1062-1065, log at :1112).
The owner then runs the sector at the origin tick while the server and
every other member run it at the applied tick: the same out-of-phase sector
as a late remote stamp, in the owner's world. B1 handles both (§2.7).

### 1.3 Gaps found while designing this

These are reported per principle (d). G-a, G-b, G-d and G-e are fixed by
this design. G-c is outside this track.

**G-a. The host's tick-keyed logs survive a renumbering.**

- **Symptom:** none measured. It becomes reachable once records under the
  new numbering are replayed (§2.6). A replay into `[base, B]` would
  re-issue a manager or script activation recorded by a frame of the old
  numbering that the new numbering re-runs (Δ > 0) for the same tick number.
- **Cause:** at a re-assign nothing clears these logs:
  - `manager_activations` (game/session_host_support.hpp:203-312);
  - H's `script_reset_ticks_` (session_client.cpp:1072);
  - RC's `host.activations`.
  They are cleared only at session start (session_client.cpp:1384-1391;
  core_host.cpp:425-426).
- **Fix:** §2.6.1 step 4. The birth logs are *not* cleared there: a birth's
  tick is a shared tick, and the reconcile needs it (§2.6.3).

**G-b. A late joiner's mechanisms keep their own phase for good.**

- **Symptom** (`maps/arena-test/lp1_join/sector1/client3.log`): client3 is
  assigned base 1640. It runs sector 2 at 1642 and sector 3 at 1649; the
  server ran them at 825 and 1270. The forced full snapshot is `unmatched`
  (the status line shows `unmatched=6`) because its tick precedes every
  record.
- **Cause:** this is gap 3 again. The joiner's module scripts start at its
  own pass ticks, and nothing ever brings them to the server's state.
- **Fix:** the same path as a resync (§2.9, step A5).
- **Prerequisites:**
  - audit #16: the owed pass at relative tick 2 kills sector 1;
  - the pooled-module option B (audit #9), for any level that pools objects.
    The arena pools none.

**G-c. Client proximity sensors watch the own ball only.**

- The server rewires every `TT Scaleable Proximity` that watches
  `Ball_Pos_Frame` to a per-block frame on the nearest player ball
  (`rewire_proximity_probes`, physics_world.cpp:351-384;
  `update_proximity_probes`, :386-425).
- No client does this: a grep for `Scaleable Proximity` finds only
  physics_world and script_replay. A client's sensors therefore read
  `Ball_Pos_Frame`, the child of its own ball.
- A remote player arriving at a P_Modul_01 Pusher, a P_Modul_29 bridge
  trigger or a fan wakes it in the server's world and not in that client's.
  BodyRevived goes only client to server (physics_world.cpp:890-901).
- This breaks principle (a). Its options are in §9 Q6.
- For this design it means only one thing: the Proximity state that a
  resync imports from the server is correct at S, and the client then runs
  it under its own-ball semantics.

**G-d. The resync's anchor is not at the tick its label says (Δ ≠ 0).**

- **Cause:** at the re-assign the client's world is the state after its old
  tick R. The next frame is numbered `base`, so the world is used as if it
  were the state after `base - 1`. Today the full snapshot's write papers
  over it for the bodies and nothing does it for the structure.
- **Δ > 0** (a running client): everything the client's world did at shared
  ticks in `(base-1, R]` is in it early. A sector pass it ran there (an own
  crossing stamped within about h ticks before the re-assign is enough) has
  built modules the server's state at S does not have; remote balls born
  there exist, remote balls that died there are gone.
- **Δ < 0** (a stall, every measured case): the shared ticks `(R, base-1]`
  never ran in the client's world. Their sector stamps and lifecycle events
  are drained at `base` as late, so the server's state at S has bodies the
  anchor world lacks.
- **Fix:** the reconcile of §2.6.3.

**G-e. The trafo scripts are in the selection, and the server never runs a
trafo.** `session_script_selection` selects the trafo scripts
(session_host_support.hpp:324; script_replay.hpp:207-223, the `trafo`
argument). The server parks the retail ball, so its Trafo Manager never
fires, and it runs no explosion, wind or reset
(`docs/trafo-replication-design.md` §1.1). Importing those scripts from the
server would write its idle state over a client whose own pieces are alive.
Fix: §2.4, `keep_local`, until the trafo replication lands (§8 T2).

### 1.4 The invariants

- **I1 (resync).** After a resync applied at B, the client's world at every
  tick t >= B is the world obtained by stepping the server's state at S
  with the inputs the server applied for S+1..t. Here "state" means:
  - the body set and the body rows;
  - the selected scripts' runtime state (except `keep_local`, I3 and §2.4);
  - the three data-array cells;
  - the sector union with its pacer.

  No pre-resync phase or tick offset survives, whatever Δ was. The one
  difference allowed is IVP internal state that a body row does not carry:
  contacts, mindists and impulse queues, the class audit §3 calls "pile
  residual". It is bounded and measured in step A2 (§9 Q1).
- **I2 (sector timing).** In every world, a sector's pass starts at the tick
  the union rule (sector_union.hpp:75-104) gives for the server's applied
  stamps and the manager's activity. A stamp that reaches a client late, and
  an own stamp the server moved, change when the client learns of it, never
  where the pass lands. This makes the late-player-design §2.9 margin
  unnecessary (plan step 14).
- **I3 (exception c).** The level-end balloon and its airship (bodies and
  scripts whose owner name starts `PE_Balloon`) stay local to each player.
  - A resync never imports their server state into a world that has its own
    (§2.4, §2.6.3).
  - No check compares them.
  - Their activation follows their sector's pass in that world, like every
    other module's.

## 2. Design

### 2.1 Parts

| part | what | where | wire |
|---|---|---|---|
| B1 | The sector union becomes rollback state with a stamp log. A late remote Sector stamp, or a moved own one, rewinds to stamp-1 and re-derives the passes inside the replay | core (`sector_union.hpp`, `client_core_lifecycle.cpp`, `client_core_frame.cpp`), host support (`manager_activations`, `live_cells_guard`) | none |
| A1 | A portable script record: v2's payload keyed by names instead of CK_IDs | `game/script_replay.hpp` (host support, CK) | none by itself |
| A2 | The server's resync state: a full snapshot of tick base-1, sent to the resynced member only, carrying the portable record and the union state | `physics_world`, `session_runner`, `server.cpp` | protocol 10 |
| A3 | The client applies the resync state as tick base-1, reconciles its anchor's structure to it, and re-steps base..B as a snapshot rollback | core (`client_core.cpp`, `client_core_messages.cpp`, `rollback.hpp`) and both hosts' platform ops | protocol 10 |
| A5 | A late joiner's first assignment takes the same path | server, core | protocol 10 |

### 2.2 Choosing S: base-1, not the server's current tick

The resync state is the server's state at the end of tick `base - 1`. It is
taken after the server steps `base - 1` and sent to the resynced member
only. This is late-player-design step 25 ("the resynced member's full
snapshot taken when the server steps B-1, to that member only").

| | S = server's tick at the request (today) | S = base - 1 (this design) |
|---|---|---|
| Own ball in S+1..base-1 | Stepped by the server on substitutes the client never learns. A re-step would need them shipped, but they are not even known when the snapshot is taken: the substitute changes as stale frames arrive (timeline.hpp:93-113) | Inside the state: there is no such tick in the window |
| Own ball in base..B | The client's frames under the new epoch | Same. The server's held notices name every substituted tick (the epoch starts at base), so the records are amended before or after the apply as usual |
| Remote balls in the window | Relay ring (`remote.inputs`, 128 ticks, input_relay.hpp:47) | Same |
| Window depth B - S | About L (17-42, §1.1) | About L, because the client runs L ahead of the server whatever S is. The state of base-1 leaves the server when the client is at about base-1+L-h and arrives h later |
| When the client is right again | About h after the request | About L+h after the request: L ticks later than today. A resync is already a visible discontinuity, and the client predicts on its old world meanwhile, as it does today until the snapshot |

The window cannot be avoided: the client is always L ahead of the server's
past. The re-step's depth is therefore L plus delivery jitter (§2.6.4,
"reach").

### 2.3 Server: the resync state

**`server.cpp handle_session_resync` (:1620-1658).**

- Replace `runner_->request_full_snapshot(s.id)` with
  `runner_->request_resync_state(s.id, c, assigned)`.
- The re-sends of the last Physicalizes and `sector_history` stay as they
  are.
- The SessionAssign carries `flags = ASSIGN_FLAG_STATE_FOLLOWS` (§5), and
  **so does every unreliable repeat of it**: `assign_repeat`
  (server.cpp:633-638) gains `uint8_t flags`, and `send_assign_repeats`
  (:1466-1484) sends it. Today a repeat is rebuilt from `first_tick` alone
  (:1476-1478), and the client applies whichever copy arrives first
  (:1405-1407).
- The other members get no forced full snapshot. This also implements
  late-player-design §3.6 #8. Full snapshots on a body-set change stay
  broadcast: those are session state.

**`session_runner`.**

- `request_resync_state` is posted to the runner like every command. It
  stores `{player, base}` in `session_state`; a newer request for the same
  player replaces it.
- **The base may already be past.** `base` is computed on the network thread
  from the published `next_tick`, and the post is asynchronous, so the
  runner can have stepped `base - 1` before the command lands. The runner
  checks it in the command itself: if `world->tick_index() > base - 1`, it
  emits nothing and posts `resync_base_missed(player, base)` back. The
  server then answers the member again (a new `handle_session_resync` pass:
  a new base from the runner's current tick, a new assign with the flag)
  and logs `resync base <b> for <name> was stepped before the request
  reached the runner: re-assigned at <b'>`. The client takes the newer
  assign (a newer base answers every resync, client_core.cpp:365-368). A
  state for a base the runner already passed is never sent, so the client's
  check `tick == base - 1` (§2.6.4 step 1) never refuses a valid answer.
- After the step of tick `base - 1`, at the place `emit_snapshot` runs
  (session_runner.cpp:931-932), `emit_resync_state(s, player)` builds:
  - `session_snapshot` with `full = 2`, `target = player`,
    `tick = base - 1`, and the rows from `world->snapshot(true, ...)`;
  - `resync.base = base`;
  - `resync.union_state = world->union_state()` (§2.5);
  - `resync.scripts = world->capture_scripts_portable()` (§2.4).
- `server.cpp on_session_snapshot` sends a targeted snapshot to its member
  alone, reliable.
- The server log gains:
  `resync state for <name>: tick <S>, <n> bodies, <k> script bytes, union {..}`.

**`physics_world`.**

- At `anchor()` (physics_world.cpp:153-292), after `rewire_*`, it selects
  `script_selection_ = bmmo::game::session_script_selection(context)`. That
  is the same function both clients use (session_host_support.hpp:324),
  trafo scripts included, so the three selections have the same keys.
- It builds `script_hooks_.capture_force` over
  `bmmo::physics::force_block_get`, as `--savestate-check --frames` already
  does on this world (sim_tool.cpp:1750-1774).
- `capture_scripts_portable()` reads only.
  - `capture` reads every parameter with `GetReadDataPtr(FALSE)`
    (script_replay.hpp:746). The force hook reads the controller.
  - So the server's world, its hash and its journal do not change. The
    check is in §7.6.
- The server never depends on the record: it only sends it. That keeps the
  rule "the server never depends on snapshots".

**Late join (A5).** `handle_session_ready` for a late joiner
(server.cpp:1599-1614) does the same, with `assigned` as base.

### 2.4 The portable script record (`script_replay`, host support)

**Why v2 cannot travel.** The v2 record keys every script and every block by
CK_ID: `put_u32(id)` (script_replay.hpp:377, :782). The restore refuses any
other id (:460, :863). CK_IDs differ between the server's engine, the
headless engine and retail CK2.dll.

Two kinds of parameter carry process-local values as their bytes:

- Object-typed locals and outputs carry CK_IDs, for example
  `Physicalize local 0 Target (3D Entity) = 47336`
  (`audit/d_P_Modul_26_MF_Script.txt`).
- Message-typed and attribute-typed parameters carry registry indices, for
  example PE_Balloon_Script local 25 `Message = 38 (Level_Finish)`
  (`audit/d_PE_Balloon_Script.txt`). The index depends on the order in which
  the process registered message types. The only Message locals found so
  far are in a `keep_local` script or are file constants (`51 (NULL)`), so
  no failing case is known; they are encoded by name anyway, and A1 lists
  every such parameter per level.

**Format (`kPortableMagic = 'BMSP'`, `kPortableVersion = 1`).**

```
u32 magic, u32 version
u32 script_count
  per script: str owner_name, str script_name, u8 present
    block (pre-order, sub-behaviour index order), recursively:
      u32 runtime_flags & kRuntimeFlags, u32 in_mask, u32 out_mask
      u16 locals, then per parameter one of
          u8 0                      opaque (is_opaque) or empty: not written
          u8 1, u32 size, bytes     a value
          u8 2, u32 class_id, str name   an object reference (GetParameterClassID() != 0), name "" = null
          u8 3, u8 kind, str name   a registry index by name: kind 1 message type, 2 attribute type
      u16 outputs, the same
      u8 has_force [u8 exists, 6 x f64]     as v2 (object space: portable)
      u8 has_joint [u8 handle_present]      as v2
      u16 children, per child u8 present + block
      u16 delayed links, u16 countdown each  as v2
3 x (u8 present, u32 value)     the three cells, as v2
```

**Keys.** A script is identified by `(owner name, script name)`.

- Checked on L1-L12 and the arena (L13) with SimTool `--list-scripts "*"`
  at tick 600 (`scratchpad/design/track3/ls*.txt`, `uniq.py`).
- Every selected script's key is unique on every level. The selected counts
  are 12, 31, 22, 17, 21, 28, 35, 31, 43, 45, 45, 45, 23, including the 7
  trafo scripts.
- The sector-placement design's copies keep their script names and get
  unique owner names (`docs/sector-placement-design.md` §4, "For F3"), so
  the key stays unique under it.
- Inside a script, a block is identified by its walk position. The restore
  validates it by every count it meets: locals, outputs, children, delayed
  links. That is v2's structural check (:867-925) without the id.

**`localize(context, sel, portable, local_anchor, keep_local, out, error)`.**
It writes a **local v2 record** and touches no world state.

- For each script of the local selection, it finds the portable entry by
  key and emits v2 bytes with the local CK_IDs from its own walk.
  - A value is copied as is.
  - An object reference: if the local parameter's current object has the
    recorded name, its local bytes are kept (every constant target).
    Otherwise it resolves by `GetObjectByNameAndClass(name, class_id)`.
  - A registry entry resolves by name in the local registry.
  - A name the local world lacks keeps the local value and counts as
    `unportable`. These are the server's private objects: `BMMO_Prox_*`,
    the `BMMO_Ball_*` arrays and player clones. Proximity frames are wired
    to inputs, which are not captured.
- **`keep_local(owner, script)`** scripts copy their bytes from
  `local_anchor` instead. The predicate is a core rule (one implementation,
  `session/resync_rules.hpp`, CK-free):
  - owner name starting `PE_Balloon` (I3), always;
  - the 7 trafo scripts (`Ball_Explosion_*`, `Ball_ResetPieces_*`,
    `Ball_PaperWind`; script_replay.hpp:207), **until the trafo
    replication lands** (G-e; §8 T2 removes them). Their pieces have no
    rows and keep the anchor's state with them (§2.6.3).
- A key in one selection and not the other, or a count that differs, fails
  the whole call with the place named. Half a record never runs: this is
  v2's rule.

The result goes through the existing `restore` path unchanged, with its
force hook, joint presence rule (script_replay.hpp:890-905) and delayed-link
re-arm.

**Size.**

- A v2 record on the arena is 27,563-27,585 bytes
  (`maps/arena-test/core_ref_e0/cov_e9/.../dump0/*.scr`).
- Level 11 has 38 mechanism scripts, 607 blocks, 3079 parameters, 22 force
  blocks and 100 opaque parameters (SimTool `--level 11 --savestate-check
  --frames`, `ssc_l11.txt`; that run selects without the 7 trafo scripts).
- Names on object references add about 20 bytes each. The expected size is
  60-90 KB, to be measured in step A1.
- A reliable GNS message may carry 512 KB
  (`k_cbMaxSteamNetworkingSocketsMessageSizeSend`).

**As built (step A1, 2026-10-24).**

- **Where.** `game/script_replay.hpp`: `capture_portable`, `localize`,
  `portable_report`, `kPortableMagic`/`kPortableVersion`, `index_record`
  (a v2 record's per-script spans: localize's anchor, the check's splice); the v2 capture's
  flag, force/joint and delayed-link writers are factored out
  (`put_activation`, `put_force_and_joint`, `put_delayed_links`) and shared
  by both walks, so v2's bytes are unchanged (the wire check below).
  `session/resync_rules.hpp` (new, CK-free): `keep_local_script(owner,
  script)` and `is_trafo_script_name`, which `script_replay::is_trafo_script`
  now calls (one place for the trafo names). `game/session_host_support.hpp`:
  `session_script_activations` is the trafo scripts plus every keep-local
  script of the selection, so both hosts record and re-issue
  `PE_Balloon Script`'s pending activations (§2.8). SimTool
  `--portable-scripts-check`. Test `ResyncRules.KeepLocalScripts`.
- **Adaptations** (no principle touched):
  - `resync_rules.hpp` is created here, with `keep_local_script` only: A1's
    `localize` and activation list need the predicate; §2.10 lists the file
    under A3, which adds the classification and attribution to it.
  - Kind 2 records the object's **own** class, not the parameter's. The
    lookup is `GetObjectByNameAndParentClass(name, <the local parameter's
    class>)`: the objects of the recorded class if there are any, else the
    others, and only a unique match resolves. Several matches keep the local
    value and count as `ambiguous` in the localize report (never a
    creation-order pick); an object outside the parameter's class is never
    a candidate. Class 0 with "" is no object and localizes to 0 (never a
    stale local id); a class with "" is an object without a name
    (unportable: the local value is kept). The local value is also kept
    when it already holds an object of the recorded name (every constant
    target): a name names one object (capture counts the exceptions as
    ambiguous) or its copies are interchangeable.
  - `localize` refuses a local selection in which two scripts share a key,
    as `capture_portable` refuses it on the capturing side; kind 4 also
    resolves only a unique (owner, script).
  - **Kind 4, `u8 4, str owner, str script`: a reference to a root script,
    by the record's own key.** Module copies share script names, so a name
    alone finds the first copy. The first run, with names only, failed the
    perturbed localize on L1-L12 (the arena passed): L2 had 24 ambiguous
    references (`P_Modul_18_MF Script` locals 9, 11 and 12 on all 8
    copies; L2 `P_Modul_18_MF Script` local 9 resolved to 45881 instead of
    53524), L11 14 (also `P_Modul_08_MF Script / Physicalize and Swing`
    local 41 and the manager's `Op` res), L1 1 (the manager's `Op` res).
    With kind 4, ambiguous = 0 on every level.
  - `localize` takes `local_anchor` as a pointer (null: no keep-local
    copies) and `keep_local` as a `std::function` (empty: nothing kept
    local).
  - **The perturbation.** Restoring an older record would tear down the
    joints and forces made since (the restore's joint-presence rule) and
    damage the world for the later checkpoints. The check therefore zeroes
    every object reference and sets every registry entry to -1 in the live
    world, so `localize` has to find every one by name alone. It also
    localizes unperturbed, with and without the keep-local rule; the
    keep-local arm is anchored on the PREVIOUS checkpoint's v2 record and
    must equal the current v2 record with the keep-local entries spliced in
    from that anchor.
  - **What one process cannot see.** Capture and localize run in one
    process, so every CK_ID, class id and registry index agrees and a
    process-local value copied as kind-1 bytes would still compare equal.
    The check therefore lists every kind-1 value whose type is index-like
    (`CKPGUID_CLASSID`, `CKPGUID_PARAMETERTYPE`, `CKPGUID_ID`, or a
    structure type, whose data is its members' CK_IDs): the list is empty on
    every level. Values of other types (an Integer that happens to carry an
    id) are not detected; cross-world use is first exercised by A3's
    resync checks.
  - The level's sector count is read from the PH table's `Sector` column.
    The `Sector_NN` groups are gone once Levelinit has read them.
- **The check, L1-L12 and the arena** (`--portable-scripts-check
  --drop-settle 150`; anchor+30, then one checkpoint per sector 2..N, 150
  ticks apart). At every checkpoint of every level, each of these is
  byte-identical to its expectation: `localize` without the keep-local
  rule, after the scrub, and the restore of the scrubbed localization
  re-captured, each equal to the direct v2 record; with the rule (8 scripts
  copied from the previous checkpoint's anchor on every level), equal to v2
  with those 8 entries spliced from the anchor. At the last sector's
  checkpoint one of the 8 differs between anchor and now on every level
  (the balloon's activation), so the copy is the anchor's, not the live
  bytes. 0 unportable, 0 ambiguous (capture and localize), 0 without a
  name, 0 server-private (BMMO), 0 index-like: the server's private objects
  (`BMMO_Prox_*`, the ball arrays, the clones) are in no local or output
  parameter of a selected script. (Numbers after the review fix-up, which
  made the lookups unique-only and added the anchor arm and the index-like
  list: the same sizes and counts as the first run.)

  | level | sectors | selected | checkpoints | blocks | object refs | registry | v2 max (B) | portable max (B) | balloon pending after the frame of |
  |---|---|---|---|---|---|---|---|---|---|
  | 1 | 4 | 12 | 4 | 267 | 277 | 2 | 20,083 | 23,540 | 334 (sector 4 stamped 330) |
  | 2 | 5 | 31 | 5 | 620 | 509 | 20 | 50,213 | 56,665 | 484 (sector 5 stamped 480) |
  | 3 | 5 | 22 | 5 | 425 | 404 | 8 | 33,415 | 38,635 | 484 (480) |
  | 4 | 5 | 17 | 5 | 387 | 364 | 8 | 30,637 | 35,125 | 484 (480) |
  | 5 | 5 | 21 | 5 | 381 | 359 | 6 | 29,888 | 34,580 | 484 (480) |
  | 6 | 5 | 28 | 5 | 598 | 519 | 18 | 49,214 | 55,993 | 484 (480) |
  | 7 | 5 | 35 | 5 | 578 | 566 | 10 | 48,046 | 56,112 | 484 (480) |
  | 8 | 5 | 31 | 5 | 517 | 485 | 6 | 39,459 | 45,959 | 484 (480) |
  | 9 | 5 | 43 | 5 | 711 | 674 | 12 | 58,386 | 67,848 | 484 (480) |
  | 10 | 5 | 45 | 5 | 714 | 673 | 12 | 58,099 | 67,816 | 484 (480) |
  | 11 | 6 | 45 | 6 | 697 | 627 | 12 | 55,056 | **64,091** | 634 (sector 6 stamped 630) |
  | 12 | 8 | 45 | 8 | 852 | 708 | 24 | 70,440 | 79,927 | 934 (sector 8 stamped 930) |
  | 13 (arena) | 4 | 23 | 4 | 432 | 445 | 4 | 34,341 | 40,252 | 334 (sector 4 stamped 330) |

  The selected counts are §2.4's. The portable record is 15-17 % larger
  than v2. L11's is 64 KB, inside §2.4's 60-90 KB estimate and far under
  the 512 KB message limit.
- **The registry list, explained.** Only two kinds occur, both Message
  types; no attribute-typed parameter is in any selection:
  - `P_Modul_18_Particle Script / PlanarParticleSystem` local 11 ("Message
    To Deflectors") = 51 (NULL), on every P_Modul_18 copy (two blocks per
    copy). It is a file constant.
  - `PE_Balloon Script` locals 25 and 26 ("Message") = 38 (Level_Finish).
    The script is keep-local.

  Both travel by name all the same.
- **Q8 settled: as designed.** On all 13 levels `PE_Balloon Script` shows
  `CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME` (inactive until
  then) after the frame of T+4, for the last sector stamped at T. The
  manager's `Activate Sector / activate Type 1` runs `Activate Script`
  (Reset? TRUE). That block calls `CKScene::Activate(script, reset)`, which
  only sets the next-frame flags. `--describe-script Gameplay_SectorManager`
  on the arena shows the block (id 31122). `manager_activations::replay`
  re-issues through the same call, so a re-issue on top of the replayed
  manager's own is idempotent.
- **The headless client is unchanged.** The wire check ran with build rT0
  against rA1, on the reference set in `core_ref_e0_pre_q/`, where the four
  labels have lived since the REF re-record: 16 of 16 send traces SAME,
  16 of 16 journals IDENTICAL, `WIRE-CHECK: ALL SAME`. So the factored v2
  capture leaves v2's bytes as they were. The captures reach the arena's
  sector 4, so the balloon's activation is recorded in them; whether a
  replay window in them re-issued it is not observed (`scripts_reissued`
  is neither logged nor journaled). The re-issue is idempotent by
  construction (`CKScene::Activate` only sets the flags the replayed
  manager sets too), and its live effect is A5's check.

### 2.5 The union as state, and the stamp log

**`sector_union::state`** (`sector_union.hpp`, pure) is a value type:

- `active`, `requested`;
- `origin` (sector → tick, player);
- `pending` stamps;
- the pacer's `idle`;
- `last_tick`, `ticked`.

It has `state()`, `restore(state)`, `unstamp(player, sector)` and a byte
serializer in the same header. The counters (`passes_`, `late_`) stay live:
they are diagnostics.

**The stamp log `s.sector_stamps_`** (core, new) holds one entry per
`(player, sector)`: the tick the server applies it at, and an arrival
sequence number. Every stamp goes in:

- the own ones from `hold_sector` (client_core_frame.cpp:142-153) and from
  the unheld crossing report (:241-248);
- the remote ones from `apply_event` (client_core_lifecycle.cpp:328-337);
- the re-sent `sector_history` of a resync or a join (a repeat of an
  existing entry is dropped);
- a MOVED own Sector echo rewrites its entry's tick to the applied tick
  (§2.7).

It lives for the session (at most sectors × members entries) and survives a
re-assign: its ticks are shared ticks. It is the one source a re-derivation
takes stamps from (§2.8).

**The union records `s.union_records_[t]`**: the union's state after
`tick(t)`, kept beside the rollback record of t.

**The server's union at S** is `physics_world::sectors_`
(physics_world.hpp:311).

- The server calls `request()` at the tick it dequeues the event
  (physics_world.cpp:884), so its `pending` is always empty.
- Its `origin` ticks are the applied ticks, which every relayed Sector
  event carries as `tick` (protocol 9). Clients stamp with that same value
  (client_core_lifecycle.cpp:334).

**The merge** (the client's union at S after a resync): the server's state,
with `pending` = every stamp-log entry whose sector the server's state has
not requested. Those are events the server has accepted or will accept and
has not dequeued by S; their ticks are > S. The rule is deterministic,
because the stamp log is what every member received.

### 2.6 Client core: the resync (A3)

#### 2.6.1 At the re-assign

In the re-assign branch (client_core.cpp:369-397), in this order:

1. Renumber, clear and `invalidate_history()` exactly as today. This is the
   only invalidation of a resync.
2. **Note Δ.** `s.resync_delta_ = R - (base - 1)`, with R the old
   `current_tick()` read before the renumbering. Log
   `resynced: tick base <base> (old tick R, delta Δ)`.
3. **The synthetic anchor.**
   - `s.rollback_.record(world(), base - 1, tracked_now(), {})`: the
     client's world as it stands, labelled base-1. `tracked_now()` is the
     tracked-set builder of post_step (client_core_frame.cpp:524-560),
     factored out. Its mechanisms come from `movable_mechanisms()`
     (client_core.cpp:78-83, the platform's movable bodies), not from the
     registry, which step 1 has just cleared. `note_script_resets(base-1)`
     runs first, as in post_step (:562).
   - `s.union_records_[base - 1] = s.sectors_.state()`: the anchor world's
     own union, which is what it has built.
   - The world at this moment is what the frames numbered `base, base+1, ...`
     will step from. Its *structure* is that of shared tick R, not base-1
     (G-d); the apply reconciles it (§2.6.3).
4. **`platform_.forget_activations_from(base)`** (G-a). The host drops its
   manager and script activation records and its script reset ticks at
   ticks >= base. They are the old numbering's frames, which the new
   numbering re-runs (Δ > 0) or never runs. The host's **birth log is
   kept**: a birth's tick is a shared tick.
5. `resync_pending_ = true` and `s.resync_base_ = base`. Keep
   `held_floor_ = base`: notices for `base..` amend records that exist now.
6. `numbering_restarted(clock_site::assign_resync)` as today. The pacer's
   origin does not change, and neither does the limiter.

The core also keeps two short shared-tick logs across the re-assign, both
new and both trimmed to the last `history_ticks + 128` ticks:

- `s.pass_log_`: every union pass this world ran (tick, sector), live or
  re-derived;
- `s.life_log_`: every remote lifecycle event applied (applied tick, player,
  type, the event).

#### 2.6.2 While pending

- The frames run normally. Records `base..B` accumulate with the inputs
  they stepped, which held notices amend.
- Input rewinds and lifecycle rewinds run as usual. They only improve the
  inputs the re-step will use.
- A late lifecycle event whose rewind target precedes the synthetic anchor
  (Δ < 0 drains every event of `(R, base-1]` at `base`) is applied live
  without a rewind and without counting a miss: the resync state covers it.
  A late Sector stamp is §2.7 case 4.
- Every snapshot other than `full = 2` with `tick == resync_base_ - 1` is
  dropped and counted (`snapshots_while_resync`). Today any full snapshot
  is taken as the resync (client_core_messages.cpp:70), including a
  body-set-change broadcast that describes a different tick.

#### 2.6.3 The reconcile: the anchor's structure becomes S's

The rows and the scripts replace the anchor's physics state. Its structure,
which bodies and passes exist, must be made S's too, from shared ticks
(G-d). At the apply, the core compares the anchor world against the
server's state at S and classifies every body. One rule covers both signs of
Δ and the late joiner:

> A body exists at S if and only if the server has a row for it at S, or it
> is a body without rows (a fixed part, a keep-local body) whose producer
> exists at S. Whatever the anchor world did at shared ticks after S is
> undone at the restore and done again by the replay at its shared tick.

| anchor world against the server's state at S | when | treatment at the restore | in the replay |
|---|---|---|---|
| body in the anchor, row at S | always | the row (authoritative, §4.1) | stepped |
| movable mechanism in the anchor, no row, its shared birth tick > S | Δ > 0: born by a pass the client ran in `(S, R]` | destroyed (`put_back_birth` from the host birth log) | re-made by the replayed module scripts after the re-derived pass |
| fixed part in the anchor whose pass the re-derivation re-runs | Δ > 0, the same pass | destroyed with its module | re-made with it |
| movable mechanism with a row at S, not in the anchor, live at B | Δ < 0 or A5: born by a live pass at a tick >= base of a sector the server has active at S | from the row, rechecked into the tree at the restore (no deferral) | stepped from S; the live pass is forgotten (§2.8) |
| fixed part not in the anchor, live at B, attributed to a pass the re-derivation forgets | the same | kept, rechecked at the restore (no deferral) | stepped |
| a row with no live body | the client has not run that pass yet | — | the apply waits (§2.6.4 step 2) |
| mirror in the anchor, no row at S, its Physicalize in `life_log_` with tick in `(S, R]` | Δ > 0 | destroyed | the Physicalize goes into `pending_births_` and is made at its tick (`drain_pending_births`) |
| mirror row at S, mirror absent from the anchor, its Unphysicalize in `life_log_` with tick in `(S, R]` | Δ > 0 | made from the row (`physicalize_remote` with the row's state) | the Unphysicalize is applied at its tick (`pending_deaths_`, new, drained beside `pending_births_`) |
| mirror row at S, absent from the anchor, live at B | Δ < 0: born late at `base` | from the row | stepped |
| mirror in the anchor, no row at S, already gone live | Δ < 0: died late at `base` | nothing | — |
| own ball | always | the server's own row when it has one; otherwise the synthetic record (the own lifecycle is the client's input) | own lives due in `(S, B]` are applied at their stamps (`apply_own_lives`, as in every replay) |
| keep-local bodies (`PE_Balloon*`, trafo pieces) whose sector the replay does not re-pass | always | the anchor's own state | stepped |
| `PE_Balloon*` bodies whose sector the re-derivation re-passes | Δ > 0 | destroyed with the sector's modules | re-made by the re-passed module (I3: they follow the pass) |
| any other body in the anchor with no row and none of the reasons above | should not happen | kept from the anchor | counted `resync_unexplained`, logged by name; the gate requires 0 |

**Attribution of a window birth to a pass.** A fixed part has no row, so
whether it exists at S follows from the pass that made it. A pass at M
physicalizes its modules in frame M+5 (client_core_frame.cpp:218-226). The
birth log's entry at tick b is attributed to the pass in `pass_log_` with
M + 5 = b, else to the newest pass with M + 5 <= b. A birth no pass explains
is counted `resync_birth_unattributed` (gate: 0) and treated as present.

**Why the rows decide, not the anchor's union.** Marking the anchor's own
passes as passed in the merged union (keeping the sectors the client built
early) would leave their module scripts in the server's pre-pass state, so
they would never run. The server's state at S is the only one whose
scripts, rows and union agree; the anchor is brought to it.

**The host logs after the reconcile.** The births of the old numbering at
ticks > S are dropped once the restore has used them; the replay logs the
births it makes (session_client.cpp:2745-2760; retail
`OnPhysicalize`, core_host.cpp:1935, to be confirmed for replayed ticks in
step A3).

#### 2.6.4 The apply

`apply_resync` replaces client_core_messages.cpp:69-136:

1. **Validate:**
   - `snapshot.full == 2`;
   - `snapshot.tick == S == resync_base_ - 1`;
   - `resync.base == resync_base_`;
   - `knows_tick(S)`.

   A mismatch drops the message and counts it. A stale state is one for a
   base the client has left.
2. **Wait for the server's body set.** Every row of a movable mechanism must
   name a body that is live now. A pass at M has its bodies in frame M+5;
   the owed passes come from the `sector_history` re-sent before the state,
   on the same reliable channel, and are stamped live (§2.7 case 4). Until
   then the state is kept and the apply is retried each frame, counted as
   `resync_deferred` with its ticks.
3. **The reach.** `D = current - S` must be <= `R_resync`, which is
   `history_ticks - 1 = 95`. Records `base..B` must exist, and
   `own_inputs_` keeps 128 (client_core_frame.cpp:524). The hold of §2.6.6
   keeps D inside it. A re-step deeper than `max_resim_ticks` (48) is
   allowed for a resync only (§9 Q2 sets the cap).
4. **Localize the scripts.**
   - `platform_.localize_scripts(resync.scripts, anchor_scripts, out, error)`,
     with the core's `keep_local` predicate.
   - `anchor_scripts` is the synthetic record's own v2 bytes
     (`recorded_scripts(S)`).
   - A failure logs `resync: the server's script state was refused at
     <where>` and counts it. The gate requires 0 (§7). The apply goes on
     with the rows and the client's own scripts: today's behaviour plus the
     re-step. It is never silent.
5. **The union.** `s.union_records_[S]` = the merge (§2.5).
6. **The reconcile plan** (§2.6.3), built by the core from the rows, the
   anchor record's bodies, `life_log_`, `pass_log_`, the host's
   `birth_tick_of` and the server's union.
7. **`s.rollback_.resync(world(), snapshot, plan, current, entity_of,
   input_at, localized_scripts)`**: a new mode of `restore_and_replay`
   (§4.1), run inside `keep_live_cells` in **adopt** mode (§4.6), with the
   union re-derived inside the replay (§2.8).
8. **After it:**
   - `resync_pending_ = false`, `++resyncs_done_`, `++resyncs_restepped_`;
   - `drop_carried_own_replay(S, own_row)`;
   - the live union becomes the replayed union at B, and `s.sector_cell_`
     is re-read from the cell (§4.6);
   - log `resync: re-stepped from the server's tick S to B (D ticks, delta
     Δ; n rows, u unmapped, k script bytes, p unportable; destroyed d,
     made m, rechecked r; union a..)`;
   - journal record RESYNC (§5).

`held_floor_` is not moved at the apply, because the history now reaches
back to S. The rule "no held notice of a tick at or below the snapshot is
late" (client_core_messages.cpp:115-129) becomes: a notice for a tick
>= base finds its record and amends it.

#### 2.6.5 The own ball

- Its row at S is authoritative like any other.
- An own birth or beam stamped after S is applied at its stamp by the
  existing paths (`apply_own_lives`, the own birth's parking and re-pose).
  `rebase_own_lives` keeps live every own life the new numbering has not
  reached (client_core.cpp:385-389); under Δ > 0 that includes lives the old
  numbering already applied in `(S, R]`, which the restore takes back to the
  row of S and the replay applies again at their stamps.
- If the server has no own row at S (its copy of the Physicalize is in
  flight), the own ball comes from the synthetic record, which is the
  client's own world. That is right, because the own lifecycle is the
  client's input.

#### 2.6.6 The resync hold

While `resync_pending_` and `current - S` reaches `R_resync`, the world must
not step: D would leave the history.

- **Core:** `poll_resync_hold()` (new, beside `poll_hold`,
  client_core.cpp:284-300), with its own state (`resync_held_`, deadline
  2 s), clock sites `resync_hold`, `resync_release` and
  `resync_hold_timeout` (appended to clock_site.hpp:21-46), its own log
  lines and journal notes ("resync hold ... ticks", "resync hold timed
  out"). It never touches the anchor hold's `held_`.
- **Timeout:** the hold releases, `resync_pending_` stays, and
  `resync_in_flight_` is cleared so the next trigger asks again (a new
  assign starts a new anchor). Logged with the reason.
- **H:** the frame loop polls it where it polls `poll_hold`
  (session_client.cpp:1843) and steps nothing while held.
- **RC:** there is no mid-session blocking hold today; the anchor's is
  inline in the anchor sequence (core_host.cpp:913-937). The resync hold is
  a second loop of the same shape at the end of `core_host_frame`, after
  the mailbox drain and before `pre_step(T + 1)`: `fixed_tick_.set_hold(true)`,
  then `core_host_drain()` and a 1 ms sleep while held, then
  `fixed_tick_.set_hold(false)` and `host.last_rebases =
  fixed_tick_.rebases()`. That bracket is what keeps the tick driver from
  rebasing during the wait (core_host.cpp:939-940), which would call
  `schedule_rebased` (:969-972) and so ask for another resync. The state
  lives in the file-scope host state, not in `BallanceMMOClient`.

### 2.7 Client core: the late Sector stamp and the moved own Sector (B1)

**Records.** `post_step` records `s.union_records_[T] = s.sectors_.state()`
next to the rollback record (client_core_frame.cpp:563). These records are:

- dropped with `invalidate_history` (the synthetic anchor's is taken after);
- trimmed with the history (96);
- re-recorded per replayed tick when a replay re-derives the union (§2.8).

**`apply_event` Sector** (client_core_lifecycle.cpp:328-337) writes the stamp
into the stamp log first, then applies the first case that matches:

1. **On time:** `event.tick > current_tick()`. `stamp()` as today.
2. **Late but redundant:** the sector is already requested with an origin
   tick <= `event.tick`, or a pending stamp of it is <= `event.tick`.
   `stamp()` as today, which is a no-op. This is the resync_snapoff 1813
   case.
3. **Late, inside the reach:** `knows_tick(event.tick - 1)` and
   `current - (event.tick - 1) <= max_resim_ticks`. The stamp is added to
   the frame's **late set**; nothing else happens in the drain.
4. **Late, beyond the reach or the history, or a rewind that failed:**
   - **Not during a resync:** `stamp()` live, as today, so the sector
     starts. Then `request_resync("a late Sector event beyond the reach",
     true)`. The resync's union state and scripts bring the sector to the
     server's tick (§2.6). Count `sector_rewinds_missed_`.
   - **While a resync is pending, or for a late joiner's catch-up:**
     `stamp()` live and nothing else. The pending state carries the
     server's union, and the apply waits for the pass (§2.6.4 step 2).

**A MOVED own Sector echo** (applied T' > origin T) rewrites the stamp log
entry to T' and:

- if T > `current_tick()` (the own pass has not run): `unstamp` the own
  pending stamp and `stamp(T')` live;
- otherwise it joins the frame's late set with anchor tick T (the own pass
  at T has to go and the one at T' has to come).

**The frame's Sector rewind.** After `drain_due_events` and
`process_own_echoes` (client_core_frame.cpp:303-308), if the late set is
not empty:

- `A = min` over the set's ticks (one rewind for any number of late stamps
  and moves in one frame, whatever order they arrived in);
- `rewind_to(world(), A - 1, now, input_at, "sector K stamped T (late)")`
  with **union re-derivation on** (§2.8);
- on success count `sector_rewinds_` and log `remote player P entered
  sector K, stamped for tick T (late by N: rewound to A-1)` per stamp;
- if `rewind_to` refuses (rollback.hpp:1623-1640), every stamp of the set
  takes case 4: stamped live, resync requested.

This is before `replay_corrected_inputs` and before the live union pass
(:318, :330): the order a late Physicalize already uses, which keeps the
next-frame flags of a live pass out of the restore (:309-317). A lifecycle
rewind of the same frame runs first as today; its ordinary replay re-issues
the recorded passes, and the Sector rewind then re-derives from A - 1.

The rule does not apply to:

- the own N3 activation (`hold_sector`), stamped ahead, except through a
  MOVED echo;
- the server's hold of the level's own checkpoint chain
  (physics_world.cpp:243-256), which no client stamp touches.

### 2.8 The union inside a replay

A replay re-derives the union only when it is a Sector rewind (B1) or a
resync re-step (A3). Every other replay re-issues the recorded activations
exactly as today. Whether to re-derive always is §9 Q4.

**The start.** `u = union_records_[A - 1]` (for A3, the merged record of S),
with `u.pending` replaced by every stamp-log entry whose sector `u` has not
requested. Taking the pending stamps from the log, not from the record, is
what keeps every stamp that reached the live union after the record was
taken: an own `hold_sector` stamp, an on-time remote stamp drained in
`[A, now]`, and the other late stamps of the set.

**Each replayed tick t.** `world_hooks_.before_replayed_tick(t)` runs
`drain_pending_births(t)`, the pending deaths (§2.6.3), then:

```
pass = u.tick(t, platform_.sector_manager_active())   // manager state after the replayed step t-1
if pass:  platform_.set_union_pass(t, pass.sector)    // the record replay_activation(t) re-issues next
          s.sector_cell_ = pass.sector; recent_passes_ and pass_log_ updated
          log "sector +K at tick t (... re-derived in a rewind at live tick N)"
else:     platform_.set_union_pass(t, 0)              // forget a union pass the live frames made at t
union_records_[t] = u.state()
```

That is the server's order: events, then `update_sectors`, then the step
(physics_world.cpp:929-931). The host's `replay_activation(t)` runs after the
core's hook (session_host_world.hpp:274-278).

**`set_union_pass(tick, sector)`** is a new platform op. Both hosts
implement it with host support's `manager_activations::set_pass` /
`forget_pass`.

- **Two slots per tick.** Today `by_tick_` maps a tick to one
  `manager_activation`, and `record()` overwrites it
  (session_host_support.hpp:213-222). The pending activation of frame T is
  recorded at T+1 (client_core_frame.cpp:395), the tick a union pass of T+1
  is recorded at too, so one of them is lost today when both happen. The
  entry becomes `{optional pending, optional pass}`: `record_pending`
  writes `pending`, the union's `record_activation` and `set_pass` write
  `pass`, `forget_pass` clears `pass` only.
- **Re-issue order** in `replay()` (:257-297): `pending` first (its cells,
  then the manager activated with its reset flag), then `pass` (its cells,
  then the manager activated with reset). That is the live order: frame T's
  scripts wrote the pending cells and flags, and the pass of T+1 overwrites
  the cells before frame T+1 consumes them.
- The cells a pass slot carries are `activate = sector`, `deactivate = 0`
  and `reset = true`. Those are what `run_sector` writes
  (session_client.cpp:1566-1576, core_host.cpp:1399-1412), so the record
  equals the live one bit for bit.

**Forgetting is required.** Without it, a live pass that the re-derivation
moved (for example a sector 4 pass delayed because a late sector 3 pass now
occupies the manager) would be re-issued at its old tick. The manager would
run twice for the sector, and the second Set World Matrix and module reset
would tear the sector's modules down.

**Keep-local modules of a forgotten pass.** When a re-derivation forgets a
live pass because the sector was already active at S (A3 under Δ < 0, A5),
the balloon's module scripts, which `keep_local` restores from the anchor
(where they never ran), would never be activated. The fix is to record the
`keep_local` scripts' own pending activations like the trafo scripts'
(`session_script_activations`, session_host_support.hpp:330-332;
`record_pending`, :227-251): the replay then re-issues the balloon's
activation at the tick the live run had it, independently of the forgotten
manager pass, and the replayed scripts re-make its bodies. Where the
manager also re-issues it, the flags are set twice, which is idempotent
(the trafo case, session_host_support.hpp:170-179). That the manager's walk
activates `PE_Balloon Script` through next-frame flags is checked in step A1
(§9 Q8 has the fallback).

### 2.9 Late joiner (A5)

**The flow.** The joiner's first SessionAssign carries
`ASSIGN_FLAG_STATE_FOLLOWS`, on the reliable copy and on every repeat
(§2.3).

- The core takes the first-assign branch (client_core.cpp:398-436). With
  the flag set, it also records the synthetic anchor at `base - 1` (§2.6.1
  steps 2-5) and sets `resync_pending_`.
- The world at that moment is the anchored level start, which is what
  frame `base` steps from. Its Δ is the whole distance to the level start;
  only the Δ < 0 rows of §2.6.3 can apply.
- The joiner's owed passes run live, stamped late (§2.7 case 4).
- The apply waits for them (§2.6.4 step 2), then adopts the server's rows,
  scripts and union at `base - 1` and re-steps.

**After the apply.**

- The joiner's own owed passes in `(S, B]` are forgotten by the
  re-derivation, because the server's union has those sectors active.
- Their bodies are kept, posed by rows and rechecked at the restore (§2.6.3,
  §4.1).
- Their module scripts take the server's mid-run state.
- Their joints are the joiner's own (the localized record says "handle
  present" and the live block has one: script_replay.hpp:896-905 leaves it).
  They were built from the same module frames at the same PH rows, which on
  a level without pooling (#9) is the same constraint.
- The balloon: its activation is re-issued from the joiner's own record
  (§2.8, last paragraph), so a joiner that arrives after any member reached
  the last sector still has its own balloon.

**Prerequisites:**

- **Audit #16**, without which sector 1 never starts in the joiner's world.
  The apply would then wait until the hold times out.
- **#9 option B** for pooled levels. Pass order moves pooled frames and
  hinges (audit2 §5).

### 2.10 Where it runs

| world | file | change |
|---|---|---|
| SV | `sim/physics_world.{hpp,cpp}` | `script_selection_` and force hooks at `anchor()`; `capture_scripts_portable()`; `union_state()` |
| SV | `sim/session_runner.{hpp,cpp}` | `request_resync_state(session, player, base)` with the missed-base check; `emit_resync_state` after the step of base-1; snapshot `target` |
| SV | `server.cpp` | `handle_session_resync` and late `handle_session_ready` use it; the assign and its repeats carry the flag; targeted send; `resync_base_missed` re-answers |
| core | `session/sector_union.hpp` | `state`, `restore`, `unstamp`, serializer, merge |
| core | `session/resync_rules.hpp` (new, CK-free) | `keep_local`, the §2.6.3 classification, birth attribution |
| core | `session/client_core.hpp`, `src/session/client_core.cpp`, `client_core_messages.cpp`, `client_core_lifecycle.cpp`, `client_core_frame.cpp` | Δ, synthetic anchor, `pass_log_`, `life_log_`, `apply_resync`, wait, resync hold, stamp log, union records, Sector rule and moved echo, replay re-derivation, `pending_deaths_`, cell re-read, counters, logs |
| core | `session/rollback.hpp` | `resync()` mode of `restore_and_replay` with the reconcile plan (§4.1); `recorded_scripts` exists already |
| core | `session/client_platform.hpp` | ops `localize_scripts`, `set_union_pass`, `forget_activations_from`, `forget_births_after`, `sector_cell_adopted` (required, `missing_platform_operation`); clock sites appended (clock_site.hpp:21-46, appended so captures keep their numbers) |
| core | `session/client_journal.{hpp,cpp}`, `session/journal.hpp` | RESYNC record, journal version (§5) |
| host support | `game/script_replay.hpp` | `capture_portable`, `localize` (registry entries by name) |
| host support | `game/session_host_support.hpp` | two-slot `manager_activation`, `set_pass`, `forget_pass`, `forget_from`; the keep-local scripts in `session_script_activations`; `live_cells_guard` adopt mode |
| host support | `session/session_host_world.hpp` | `local_birth_log::forget_after` |
| H | `sim/session_client.cpp` | the ops over `scripts_`, `manager_activations_`, `local_births_`, `script_reset_ticks_` (one line each into host support); resync hold poll; `--resync-at TICK` (test option, §7.3) |
| RC | `session/core_host.cpp` | the same over `host.script_ids`, `host.activations` and `host.births`, in the file-scope host state (no data member on `BallanceMMOClient`); `sector_cell_adopted` does `run_sector`'s bookkeeping (`sector_cell_is_own_`, :1412); the resync hold loop |
| tools | `sim_tool.cpp`, `scripts/first_divergence.py`, `scripts/worst_bodies.py`, `scripts/journal_trace.py` | §7.2, §8 T0/T1 |

Every decision is in the core:

- when to anchor, apply, wait, hold, rewind or re-derive;
- what exists at S, what is authoritative, what is re-made;
- what is local (I3, G-e).

The hosts translate between the core and CK.

The legacy retail path (`physics_session_client.cpp`) gets nothing. This
follows the user's decision (audit §5) and R7's deletion of that path.

## 3. Determinism

**The union.**

- `sector_union` is a pure function of the stamps and of the manager's
  activity per tick. The manager's activity in turn depends only on:
  - the passes;
  - the re-issued activations of unselected scripts;
  - script frames.

  None of these depends on physics.
- After B1, a client applies every stamp at its applied tick: live when it
  is on time, inside a replay from A - 1 otherwise. A moved own stamp is
  applied at the server's tick the same way.
- The server applies it at the same tick, since that tick is the event's
  `tick`. So the pass ticks agree in every world (I2).
- A re-derivation takes its pending stamps from the stamp log, the same set
  in every world that has received them, so no stamp that reached the live
  union is lost to the replay, and the order in which late stamps arrived
  does not matter (one rewind at the minimum).
- Late but redundant stamps change nothing on any side.

**Out-of-order passes.**

- The rule picks the lowest requested sector that is not yet active, at
  each idle manager. A world in which sector 4 was requested before 2 runs
  4 first.
- Every member sees the same stamps and so runs the same order
  (lockstep-without-snapshots.md "+4 @352 then +2 @827").
- A rewind that adds a stamp re-derives the order inside the window and
  forgets the passes that moved (§2.8).

**Resyncs.**

- The state at S is the server's end-of-tick state:
  - rows from `snapshot(true)`;
  - scripts captured after the step of S;
  - the union after its `tick(S)`.
- The client's records of tick T are end-of-tick too (client_core_frame.cpp:
  563), so the anchor means the same thing on both sides.
- **The anchor's tick is made true.** Whatever Δ was, the reconcile
  (§2.6.3) leaves at S exactly the server's body set: undoing the client's
  shared ticks after S (Δ > 0), adding what it lacked (Δ < 0, A5). Every
  structural change after S is then made by the replay at its shared tick,
  from the same stamps and lifecycle events the server applied.
- The re-step's inputs are:
  - for the own ball: its frames for base..B, amended by the held notices
    of the new epoch, which is what the server applies;
  - for each remote: the relay ring, then the prediction where the relay
    has not reached. Later relay corrections replay as ordinary input
    rewinds over the re-recorded history.
- What still differs from the server is IVP's internal state of the rows'
  bodies: the synthetic anchor's blob is the client's own world (and has
  nothing for bodies it lacked), and the rows go on top of it. That is §9
  Q1.

**Late joiners.**

- Before A5, a joiner's world is built from owed passes at its own ticks,
  in ascending order, and never converges (G-b).
- After A5, everything before S is replaced by the server's state at S,
  except two things:
  - the joints its own passes built, which are equal where nothing is
    pooled;
  - the pooled placements, which #9 option B makes order-free.

**The script record across engines.**

- The keys are names, which are unique on every level (§2.4). The walk
  order is the file's graph, which is the same in every engine.
- N1, N3, N4 and the trafo wrappers edit unselected scripts or block
  functions, never the selected graphs' structure.
- Registry indices travel by name (§2.4), so they do not depend on each
  process's registration order.
- The server's rewiring changes input parameter sources only. Inputs are not
  captured. Where the server's private object sits behind a captured output
  (the `BMMO_Ball_*` reads, physics_world.cpp:428-527), `localize` keeps the
  client's value and counts it.
- The step A1 check enumerates these per level, and step A4 enumerates them
  on retail.

## 4. Rollback interplay

### 4.1 The body plan in resync mode

`resync()` is `restore_and_replay` at `anchor = S` with the reconcile plan
(§2.6.3) given by the core. Differences from `on_snapshot`:

| body | source at S | difference from a snapshot rollback |
|---|---|---|
| a body with a row, not keep-local | the row, `from_row`, its sleep flag as the server has it | every row is authoritative, including sleeping rows inside the tolerance (rollback.hpp:603-610 does not apply); no compare, no tolerance |
| a body with a row that the records show born in `(S, B]` (Δ < 0, A5) | the row, `from_row`, with `force_recheck` | a row outranks a birth, the reverse of rollback.hpp:728-734: the server says the body exists at S |
| a live body the anchor's blob does not know, present at S (rowed, or a fixed part of a forgotten pass) | the row, or its live pose for a fixed part | rechecked at the restore. Today `set_savestate` leaves it out of collision detection and defers the recheck to its birth tick (rollback.hpp:952-966); in resync mode only a body the plan destroys keeps a deferral |
| a body the plan destroys (Δ > 0 births after S, their modules' fixed parts, a re-passed balloon) | destroyed (`destroy_body`, `put_back_birth`) | re-made by the replay |
| a mirror the plan makes from a row (Δ > 0 death after S) | made from the row | its death is re-applied at its tick |
| a body with no row (keep-local, a client-only body) | the synthetic record (client's own) | as today's "recorded" branch |
| a body born in `(S, B]` with no row and not present at S | birth path: destroyed and re-made by the replayed scripts, or parked | unchanged |
| a mechanism alive at S and destroyed in the window | `revive_body` (rollback.hpp:855-880) | unchanged |

**Joints of the bodies present at S but unknown to the blob.** Their
constraints were built by the live frames after the anchor; the localized
record says "handle present" and the live block has one, so the restore
leaves them (script_replay.hpp:896-905). What `set_savestate` does with a
constraint whose cores it does not know is not established: step A2's arm S
(§7.2) measures it, and its pass is the gate for A3 and A5 (§9 Q1).

**The rows' entities.**

- Mechanisms map through `mechanism_tracking_`. The full snapshot's
  dictionary is noted first and resolved once, as client_core_messages.cpp:
  84-92 does today.
- A row with no local entity counts as `resync_rows_unmapped`. The wait of
  §2.6.4 step 2 makes this 0 for sectors. A pooled level before #9 B can
  still show it, and it is reported.

### 4.2 Window-born births

The ordinary machinery applies:

- `destroy_body` at the restore;
- the re-made body from the replayed module script;
- `births_recreated_exact`;
- `replay_mechanisms` adding a body the replay made earlier
  (rollback.hpp:1349-1370). This includes a pass that the re-derivation
  moved earlier, as in B1's a5u case: sector 4's bodies, born live at
  1728, are re-made in the replay at 1719.

### 4.3 Savestate naming

- The synthetic record's blob is captured by the client from its own world,
  so every name in it is local. `set_savestate` meets the object set it
  recorded, except for bodies born after it, which §4.1 lists.
- No server blob is sent. The server's names (clones, `BMMO_*`) never reach
  a client blob.
- Rows are written on top of the blob, and the anchor's blob is re-captured
  as on any row write (rollback.hpp:1083-1088). A later rewind to S restores
  the corrected state.

### 4.4 Script replay selection

- All three worlds use `session_script_selection` (trafo scripts included).
- `localize` keeps the `keep_local` scripts local (§2.4).
- `replay_activation = true` on both hosts (session_client.cpp:2690,
  core_host.cpp:1571). Every selected script, inactive ones included, is
  written back from the localized record, and the cells with it
  (script_replay.hpp:476-481). This includes the manager, whose walk state
  then continues the server's.
- The keep-local scripts join the recorded activations (§2.8).

### 4.5 The records

| record | resync | late or moved Sector |
|---|---|---|
| rollback history | synthetic anchor at base-1, then live `base..B`, re-recorded by the re-step | unchanged; `A-1..now` re-recorded |
| anchor scripts | replaced by the localized server record before the restore | unchanged |
| `union_records_` | `[S]` replaced by the merge; `(S, B]` re-derived | `[A, now]` re-derived |
| stamp log | kept across the re-assign; the re-sent history dedupes | gains the stamp; a moved echo rewrites its tick |
| `pass_log_`, `life_log_` | kept across the re-assign; used by the reconcile | `pass_log_` follows the re-derivation |
| host activation records | `forget_activations_from(base)` at the assign; union pass slots in `(S, B]` set or forgotten by the re-derivation | pass slots in `[A, now]` set or forgotten |
| host birth log | kept at the assign; old-numbering births after S dropped after the restore | unchanged |
| own inputs, relay ring, sent ring | kept (cleared at the assign as today: `own_inputs_` from base on) | kept |
| journal | RESYNC record + the RECEIVED checkpoint (flag RESYNC) | the NOTE "sector K stamped T: rewound" |

### 4.6 The live-cells guard

**Today.** `keep_live_cells` (session_host_support.hpp:344-386) writes the
three cells back to their live values after a rewind. Its premise is that
the live chains which wrote them are not replayed.

**Under re-derivation.** When the replay re-derived the union (A3, or a B1
rewind that set or forgot a pass), the replayed values at the end are the
right ones, cell by cell:

| cell | live writers | adopt rule |
|---|---|---|
| `CurrentLevel[0][4]` (phase) | the union pass (0 at M..M+3, 1 at M+4) | replayed value. A live value from a pass at the wrong tick would hand a module the wrong first-frame branch, the phase miss client_core_frame.cpp:225-234 detects |
| `IngameParameter[0][1]` (activate) | the union pass; the level's checkpoint chain on an own crossing | replayed value. Every chain write of a live frame was polled in that frame's post_step (`poll_own_sector`) before the next pre_step's rewinds, so it is already a stamp in the log; dropping the write loses no crossing |
| `IngameParameter[0][2]` (deactivate) | the union pass (0); the death chain's sector reset, neutralized by the script guards | replayed value |

**After every re-deriving rewind** (the guard's destructor, when
`core.last_rewind_rederived()`):

- the cells are not written back;
- the core re-reads `IngameParameter[0][1]` into `s.sector_cell_`, so
  `poll_own_sector`'s comparison (client_core_frame.cpp:235) does not take
  the replayed value for an own crossing. Without this, a5u_lead's clients
  0, 1 and 3 would stamp an own sector 4 they never entered, and their
  real crossing would then be dropped as already requested;
- the host's `sector_cell_adopted(cell)` does the bookkeeping `run_sector`
  does (RC: `sector_cell_is_own_` false when the cell is not the player's
  sector, core_host.cpp:1412; H: nothing);
- the activation of the frame about to run is re-issued (`reissue_`,
  :365) as for every rewind.

Every other rewind keeps today's write-back.

## 5. Wire, protocol and journal

| item | change |
|---|---|
| `PHYSICS_PROTOCOL_VERSION` (entity/session.hpp:51) | 9 → **10**. The sector-placement design (`docs/sector-placement-design.md` §5) and pooled-module option B also ask for 10; whichever lands second takes the next number (§9 Q5). The comment gets: "10: a resync or a late join anchors at base-1: SessionAssign flags (STATE_FOLLOWS), a full snapshot `full = 2` to that member only with the union state and the portable script record" |
| `session_assign_msg` | `+ u8 flags`; `ASSIGN_FLAG_STATE_FOLLOWS = 1`. The reliable copy and every unreliable repeat carry it (server.cpp `assign_repeat.flags`) |
| `session_snapshot_msg` | `full = 2` (RESYNC): reliable, one member. After the rows: `u32 base`, `u16 union_len` + union bytes, `u32 scripts_len` + portable record. Rows carry names as for `full = 1`. Total under 512 KB (`MAX_BODIES_PER_SNAPSHOT` 1024 rows at about 150 B each, plus the record) |
| `session_resync_msg` | unchanged |
| sector union bytes | `u8 version`, `u32 last_tick`, `u8 ticked`, `u8 idle`, active and requested (u8 count + i32 each), origin (u8 count + (i32 sector, u32 tick, u32 player)), pending (u16 count + (u32 tick, u32 player, i32 sector)) |
| journal | `kJournalVersion` lives in the shared `journal.hpp:74`, so a bump changes every file's header, `server.bmjr` included. It goes 3 → 4 (or the next free number: the sector-placement design also takes 4, §9 Q5). New tag 8 RESYNC: `u32 S, u32 base, i32 delta, u32 applied_at, u8 outcome (applied, refused_scripts, timed_out), union bytes, record bytes`. `JOURNAL_CHECKPOINT_RESYNC = 8` on the RECEIVED checkpoint of the state. `kJournalMaxTag` follows. `kJournalOldestReadable` stays 2, so the E0 set still reads |
| server journal | a NOTE per resync state sent and per missed base. Nothing the world computes changes |
| inbound capture (`.bmic`) | unchanged format. The new messages are captured as bytes |
| B1 | no wire change, no protocol bump. A mix of B1 and pre-B1 clients still agrees with the server where B1 runs |

## 6. What must not change

- **The non-resync snapshot path** (client_core_messages.cpp:137-287):
  - compare;
  - tolerance and its blind widening;
  - restore;
  - `too_far`;
  - frozen;
  - the legacy blend ladder.
- **The resync request:**
  - its reasons;
  - the 2 s / 10 s limiter (client_core.cpp:455-492);
  - the owed path (:497-502).
- **The renumbering at the assign:** `numbering_restarted(assign_resync)`
  and every host pacer origin. Only the resync hold adds sites, appended.
- **The anchor hold** (`held_`, `poll_hold`, `kAnchorHoldTimeout`): the
  resync hold is a separate state.
- **The server's world.**
  - It computes the same world with or without resyncs.
  - The server journal replays bit-exact.
  - The server never reads a snapshot.
  - `update_sectors`, the N2 hold and the N4 split are unchanged.
- **On-time Sector stamps:** they are stamped and passed exactly as today.
  Ticks and logs are identical in the 13 references that have no late,
  non-redundant stamp (§7.5).
- **The own N3 activation:** held, stamped ahead and sent ahead.
- **The server's hold of the level's own checkpoint chain.**
- **Ordinary rollbacks:** they re-issue recorded activations, and write the
  live cells back. Re-derivation and adopt mode are only for B1's rewinds
  and A3's re-step. The two-slot activation record re-issues exactly what
  the one slot did when only one activation was recorded at a tick.
- **IVP and physics_RT:** no engine change. The force and joint hooks and
  CK's public parameter API exist on all three worlds.
- **Exception (c):** `PE_Balloon*` scripts and bodies are never imported
  from the server into a world that has its own, and never compared. Their
  timing follows their sector's pass in that world, which moves in B1 like
  every module's and is not required to match.
- **The trafo, until track 2:** the trafo scripts and pieces stay the
  owner's, as today (G-e).

## 7. Verification

The tooling comes first (step T0):

- `first_divergence.py` and `worst_bodies.py` gain `--from TICK` and
  `--exclude PREFIX` (repeatable; `PE_Balloon` in every check below, and
  the trafo pieces' prefixes until T2).
- Step P is editing both files in the working tree, so T0 lands after P.

**As built (step T0, 2026-10-24).**

- `first_divergence.py` gains `--from TICK` (the drift and the body set
  are both compared from TICK on) and `--exclude PREFIX` (repeatable).
  `worst_bodies.py` already had `--from`/`--until` and gains `--exclude`.
  An excluded body is neither compared nor reported MISSING. The match is
  on names only, applied after the ownerless-by-name keying, so it reaches
  map bodies and trafo pieces keyed by name. A ball keyed by its owner is
  never excluded.
- **Options off, a5u_lead:** the output is byte-identical to the old
  scripts' (`first_divergence` server vs the four clients; `worst_bodies`
  with client 2 as control vs clients 0, 1 and 3, and with `--from 1714`).
  `--exclude PE_Balloon` removes exactly the 17 PE_Balloon rows.
- **The capture used.** On 2026-10-24 the re-record moved the E0 set to
  `maps/arena-test/core_ref_e0_pre_q/`. `core_ref_e0/` now holds the
  partial re-record (`solo`, `swing_snapon`, `cov_e9`, `pins.json`). The a5u_lead journals there
  match the README's sha256 (client0 `db6722d1…`, client2 `83bb3777…`).
- **With the options** (`--from 1714 --prefix P --exclude PE_Balloon`):
  - clients 0, 1 and 3 first diverge at 1728, on P_Modul_08_Schaukel by
    0.25 m;
  - their body set differs from 1720 on (P_Modul_03_*, PE_Box_slide,
    P_Modul_19_Flaps): sector 4 started at 1723, not 1714 (§1.2);
  - client 2, which stamped sector 4, first diverges at 2214, on
    PE_Box_slide by 0.0604 m.
- **The grep is `scripts/moved_own_sector.py`.** The core prints a moved
  own Sector as `own sector stamped tick T echoed moved by the server
  (applied A, …)`:
  - the type name is lowercase (`event_type_name`, lifecycle.hpp:41);
  - the line is `logf` at client_core_lifecycle.cpp:1213.

  The legacy retail path prints no echo line. The script therefore also
  reads the server's S5 line (server.cpp:1946), where `type=Sector` with
  `origin != applied` is a moved own Sector for every client build. The
  last line it prints is `MOVED-OWN-SECTOR: NONE | FOUND n`.
- **The grep's result:**
  - The 16 captures have no moved own Sector. Every Sector S5 line is on
    time: a5u_lead 10, a6m_snapoff 12, resync_snapoff 10, swing_snapon 6.
  - All 2987 logs under `maps/arena-test` give NONE: 1132 Sector relays,
    all on time.
  - The other types show that both patterns match. The clients log 40
    unphysicalize and 1 physicalize `echoed moved` lines. The server logs
    41 Unphysicalize, 54 Physicalize and 898 BodyRevived moved relays. An
    own Physicalize moved inside the history logs `own birth moved`
    instead.
- **Pre-existing, unchanged:** between two clients of a four-player
  session, `worst_bodies` reports every ball MISSING. A single tick where
  the arm lacks a ball (start spawn, death) marks it `inf` for the whole
  run.

### 7.1 Unit tests (`BallanceMMOMessageTests`)

- **`sector_union_test`:**
  - `state` / `restore` / `unstamp` / serializer round trip;
  - a late stamp applied to `state(T-1)` and re-ticked with the same
    manager activity gives the same pass ticks as an on-time run;
  - a redundant late stamp changes nothing;
  - the merge rule (§2.5) with stamps on both sides of S.
- **`session_client_core_test`** (fake platform):
  - a late remote Sector inside the reach makes exactly one `rewind_to` to
    stamp-1, calls `set_union_pass(stamp, K)` before the replayed tick's
    re-issue, makes no live pass at the arrival tick, and logs the line;
  - **stamps inside the window survive:** an own `hold_sector` stamp for a
    tick in the window and an on-time remote stamp drained in `[T, now]`
    both pass at their ticks after the rewind (the reviews' scenario: own 4
    at 1736, late remote 3 at 1695 arriving at 1705);
  - two late stamps in one drain, delivered in descending tick order, make
    one rewind to the lower one;
  - `rewind_to` refused: every stamp of the set stamped live, one resync;
  - a MOVED own Sector echo: the own pass moves from T to T', one rewind to
    T-1 (or a live re-stamp when T is ahead);
  - after a re-deriving rewind that moved the cell, no own Sector event is
    sent (the `sector_cell_` re-read), and a real crossing afterwards is;
  - beyond the reach it makes one `request_resync` and stamps live; while
    pending it requests nothing;
  - a re-assign with `STATE_FOLLOWS` records the synthetic anchor at
    base-1, logs Δ, calls `forget_activations_from(base)` and not the
    birth log's, keeps `held_floor_ = base`, and calls
    `numbering_restarted` once;
  - a `full = 1` snapshot while pending is dropped and counted;
  - the resync state applied with `D = 0`, `D = 20` and `D = 95` (the last
    with the resync hold engaged) gives one restore and D replayed ticks;
  - **the reconcile, Δ = +9:** a pass of sector K at R-3 (its bodies
    destroyed at the restore and re-made after the re-derived pass), a
    remote birth at R-5 (destroyed, made again at its tick), a remote death
    at R-2 (made from the row, killed again at its tick);
  - **the reconcile, Δ = -6:** a pass and a remote birth of `(R, S]`
    drained at base (rows from S, rechecked, no deferral; the live pass
    forgotten);
  - keep-local rows are skipped; a forgotten pass's balloon activation is
    re-issued from its record.
- **`rollback_engine_test`:**
  - `resync()` against a synthetic world: the bodies at B equal a straight
    run from the row state at S with the same inputs;
  - a row outranks a window birth and is rechecked at once;
  - a body with no row comes from the record;
  - the scripts of the anchor are replaced before the restore.
- **`session_host_support_test`** (or the binding test): two slots at one
  tick re-issue pending then pass; `forget_pass` keeps the pending slot.
- **`session_messages_test`:**
  - assign flags round trip;
  - `full = 2` round trip with both trailing sections;
  - a protocol 9 SessionStart refused.
- **`session_journal_test`:** a RESYNC record round trip; v3 files still
  read.

### 7.2 SimTool (offline, `--root C:/Users/geekerwan/Desktop/Ballance-Agent`)

**`--portable-scripts-check` (new, step A1).** On each of L1-L12 and the
arena (13):

- **Procedure:**
  - at anchor+30 and after each `--activate` of sectors 2..N, capture a
    local v2 and a portable record;
  - restore an older local record (perturb);
  - `localize` and `restore`;
  - capture the local v2 record again.
- **Pass:**
  - the bytes are identical to the first local capture on every level;
  - `unportable` = 0 on the arena;
  - on L1-L12, every unportable reference and every registry-typed
    (Message, attribute) parameter is listed by script, block and
    parameter, and each one is explained;
  - the portable size per level is logged.
- **The balloon's activation (§2.8, Q8):** after `--activate` of the
  balloon's sector, `PE_Balloon Script` shows as a pending activation
  (`CKBEHAVIOR_ACTIVATENEXTFRAME`) after one of the pass's frames, on the
  arena and on every level that has it.

**`--resync-check S B` (new, step A2).** Built on the `--savestate-check`
binding.

- **Procedure:**
  - run straight to B+600 and keep blob, rows, portable scripts and union
    at S;
  - **arm E:** restore the blob of S, the rows and the localized scripts at
    S, then re-step to B+600;
  - **arm R:** restore the blob of a perturbed run's tick S (scripts of
    S-17 restored at S-17 and stepped), then rows and scripts, then
    re-step. This is what a client's anchor is;
  - **arm S (the anchor lacks a sector, Δ < 0 and A5):** the blob of a run
    in which sector K was activated 9 ticks *after* the straight run's (so
    its bodies are unknown to the blob at S), rows and scripts of the
    straight run at S, the reconcile's rechecks, then re-step;
  - **arm A (the anchor has a sector too early, Δ > 0):** the blob of a run
    in which sector K was activated 9 ticks *before* the straight run's
    pass after S, then the reconcile's destruction, rows, scripts,
    re-derived pass, re-step.
- **Scenes:**
  - the arena: P_Modul_08, P_Modul_01, P_Modul_03 (Schaukel);
  - Level 11 sector 1: P_Modul_26 sack and rope, P_Modul_01 Pusher001.
- **Pass:**
  - arm E is bit-exact against the straight run at every tick B..B+600;
  - arm A is bit-exact for the re-made sector's bodies (they are made from
    nothing in both runs) and within arm R's band for the rest;
  - arms R and S: the per-body maximum error over B..B+600 is reported,
    and **≤ 1 mm for Pusher001, the sack and the arm-S sector's bodies is
    the go/no-go gate for A3** (§9 Q1). Arm S also reports every
    constraint of the sector's joints after the restore (present, attached
    to the right cores).

**`--late-sector-check K T A` (new, step B1).** The client binding on the
SimTool world.

- **Procedure:**
  - the control activates sector K by a union stamp at T;
  - the arm stamps it at T but delivers the stamp at A > T. That is a live
    pass at about A, then the late rewind to T-1 with re-derivation.
- **Cases:** the arena, K ∈ {2, 3, 4}, A - T ∈ {3, 9, 40}; one case where a
  later sector's live pass must be forgotten; one with an own stamp of a
  later sector made inside the window; one moved own stamp (T, T+9).
- **Pass:**
  - the savestate (the diff ignoring order bookkeeping) and the scripts are
    identical to the control at every tick from A to A+600;
  - `births_missing` = 0;
  - the forgotten-pass count is as expected;
  - no own Sector event is produced by the cell.

**Server journal replay.** `--replay` of the A3 runs' `server.bmjr` is
bit-exact (§7.6).

### 7.3 Headless harness (arena, 4 clients, same-batch old/new per §5.3 of the core design)

**a5u_lead recipe** (B1): `--latency-ms 150 --arm A5u --sectors 1
--keep-lane --event 2:1500:step=300:dir=up`, server `arch_server_lp.sh wsl
26700 100000 100000 4.2 true true`. Three runs. Pass:

- every client logs `sector +4 at tick 1714` (the server's tick);
- clients 0, 1 and 3 each log one Sector rewind, and none of them sends an
  own Sector 4 before its own crossing (1798/1807 in the reference);
- `first_divergence.py server.bmjr client*.bmjr --prefix P_ --exclude
  PE_Balloon --from 1714` gives no sector-4 body beyond the stamping client
  2's own maximum + 1 mm;
- `phase_misses` = 0;
- the server journal replays exactly.

**resync_snapoff recipe** (A3, Δ < 0): `--latency-ms 150 --arm A0 --sectors 1
--keep-lane --event 2:1500:none --client-arg-for '2:--pause-at 1500
--pause-ms 1000'`, snapshots off. Three runs. Pass, for every resynced
client:

- it logs `resync: re-stepped from the server's tick S to B` with
  `S = base - 1`, its Δ and `D ≤ 95`;
- `unportable` = 0, `rows_unmapped` = 0, scripts refused = 0,
  `resync_unexplained` = 0, `resync_birth_unattributed` = 0;
- after B, P_Modul_08, P_Modul_01 and P_Modul_03 are within 1 mm of the
  server to the end (`--from B+1 --exclude PE_Balloon`);
- the other members' `snapshots applied` delta is 0, since they get no
  broadcast;
- the resync count is unchanged against old (no new resyncs).

**Running-client resync** (A3, Δ > 0), new: `--client-arg-for
'0:--resync-at T'`, with T chosen 3 ticks after client 0's own crossing
stamp of sector 2 (its log's `sector 2 stamped for tick X`), at 0 ms and
at 150 ms. `--resync-at` is a headless test option that calls
`request_resync("test")` at T while the client keeps stepping. Three runs
each. Pass: the logged Δ is > 0; the sector's pass is at the server's tick
in client 0's log after the apply, with no second pass of it and
`phase_misses` = 0; the same body criteria as resync_snapoff.

**a5u_lead recipe again** (A3): client 2 resyncs twice there; the same pass
criteria apply to client 2.

**Regression arms:**

- swing_snapon: `--latency-ms 150 --loss-pct 10 --sectors 4 --beam-bays
  18,18,18,18 --beam-spread 3`;
- a6m_snapoff;
- the lane arm at 10% loss.

Pass:

- every per-client number is within the old binary's band (§5.3 rule);
- `sector_rewinds` = 0 where no stamp is late;
- the server's CPU per step is not above old + 5%;
- the resync state's emit time is logged;
- `resync_base_missed` is logged with a re-assign whenever it occurs, and
  never leaves a client pending.

**Late join** (A5): the lp1_join recipe (client 3 joins after sector 3),
and a variant in which client 3 joins after another member has started the
arena's last sector (the balloon's). Also once at 10% loss. Pass:

- client 3 reaches mechanisms 52/52 (with #16 fixed);
- it re-steps from base-1 (the assign was taken with `STATE_FOLLOWS`
  whichever copy arrived first);
- after B its sector 2 and 3 modules are within 1 mm of the server
  (`--exclude PE_Balloon`);
- in the variant, client 3's own balloon bodies exist and its balloon
  script runs (the journal shows `PE_Balloon_Platte*` physicalized after
  the apply).

### 7.4 Retail runs (Level 11, two clients, core on, against the Arch server of the step's commit)

**Recipes:** the `r1_c_on1` recipe (`r1_c2.py`, a stall that rebases the
tick driver of client 2) and the `r2_x_t1_b` recipe (`r2_run.py`, net lag
with own frames substituted past the reach on both clients). Three runs
each. Pass:

- every resynced client logs the re-step with `unportable = 0`. This is the
  cross-engine evidence: retail CK2 against the server's engine;
- P_Modul_26_Sack and P_Modul_26_Rope are within 1 mm of the server after B
  (`worst_bodies.py`/`first_divergence.py` against `server.bmjr`);
- 0 correction records naming the sack after B (snapshots on);
- in `r2_x_t1_b`, client 1's Pusher001 is within 1 mm of the server from
  B to the end;
- every resync hold is logged with its length, and no rebase is counted
  during one;
- clients that did not resync are unchanged, exact as before;
- a B1 retail check: one client's `net lag 300 up` across a first crossing.
  The other client logs `sector +K at tick <the server's>`, and neither
  sends an own Sector event it did not earn.

### 7.5 The 16 reference captures (`maps/arena-test/core_ref_e0/`)

Replay the captures with the B1 binary, twice. The expected results, with
the evidence for each in §1.1 and §1.2:

| capture | B1 result | why |
|---|---|---|
| swing_snapon/client0-3 | byte-identical | no late stamp, no resync (logs) |
| a6m_snapoff/client0-3 | byte-identical | the same |
| resync_snapoff/client0, client1, client3 | byte-identical | their late stamp for sector 4 at 1813 is redundant (1810 already); each one's resync is A3's business |
| resync_snapoff/client2 | byte-identical | resync only |
| a5u_lead/client2 | byte-identical | the stamping client; its two resyncs are A3's |
| **a5u_lead/client0, client1, client3** | **change** | sector 4 at 1714 instead of 1723: one Sector rewind; the TICK records differ from 1714 on |

A capture with a MOVED own Sector echo would also change; the 16 have none
(no `own Sector ... moved` line in their logs), which T0 confirms with a
grep.

After A3 (protocol 10):

- every capture is refused at SessionStart (client_core.cpp:113-117). That
  is the intended "no path for old builds".
- **Re-capture** the four runs, and the running-client resync arm, with the
  A3 binaries as `core_ref_r3/`. That set is what later refactor steps
  replay.
- The E0 set's journals that A3 would have changed are all four of
  resync_snapoff (each client resynced: c2 at 1500, c0 at 2781, c1 at 2779,
  c3 at 3003) and a5u_lead/client2 (two resyncs). The three a5u journals
  above are already changed by B1.

### 7.6 Server

- `--replay` of every A3 and A5 run's `server.bmjr` is bit-exact.
- One run with a forced resync and one without, from the same inputs (the
  solo baseline with `--drive`), give identical server TICK hashes. The
  capture is read-only.

## 8. Steps

Each step is one implement → review (code-review, high) → gate pass. A step
closes only on its checks. A failed check is investigated to its cause and
reported as symptom, cause and options (principle d).

| # | step | files | checks that close it |
|---|---|---|---|
| T0 | Tools | `scripts/first_divergence.py`, `scripts/worst_bodies.py` (`--from`, `--exclude`); after step P lands | both run on core_ref_e0/a5u_lead and give today's numbers with the options off; the moved-own-Sector grep of §7.5 |
| B1 | Union as rollback state; late Sector rewind; moved own Sector | `sector_union.hpp`; core (stamp log, `union_records_`, `pass_log_`, §2.7, §2.8, `last_rewind_rederived`, the cell re-read); `client_platform.hpp` (`set_union_pass`, `sector_cell_adopted`); `session_host_support.hpp` (two slots, `set_pass`, `forget_pass`, guard adopt mode); H and RC ops; `sim_tool.cpp --late-sector-check` | §7.1 union, core and host-support tests; §7.2 `--late-sector-check` exact; §7.5 B1 table; §7.3 a5u_lead ×3; regression arms in band; retail B1 check |
| A1 | Portable script record | `script_replay.hpp` (`capture_portable`, `localize`, registry entries by name); keep-local scripts in `session_script_activations`; `sim_tool.cpp --portable-scripts-check` | §7.2 on L1-L12 and the arena, bytes identical, the unportable and registry lists explained; L11 size logged; the balloon activation check (Q8 settled) |
| A2 | Resync fidelity offline | `sim_tool.cpp --resync-check` | arm E exact; arm A as specified; arms R and S ≤ 1 mm for Pusher001, the sack and the arm-S bodies (else Q1 is decided before A3) |
| A3 | Exact resync, wire to world | `entity/session.hpp` (10), `session_assign_msg` and the repeats, `session_snapshot_msg`, `physics_world`, `session_runner` (missed base), `server.cpp`, core (§2.6, `resync_rules.hpp`, `life_log_`, `pending_deaths_`), `rollback.hpp` (`resync`), `client_platform.hpp` (`localize_scripts`, `forget_activations_from`, `forget_births_after`, clock sites), `session_host_world.hpp` (`forget_after`), journal, H and RC ops, both resync holds, `--resync-at` | §7.1 all; §7.6; §7.3 resync_snapoff ×3, running-client resync ×3 at 0 and 150 ms, a5u_lead; regression arms; re-captured `core_ref_r3/` |
| A4 | Retail gate | none (runs); fixes found go back to A3's files | §7.4 |
| A5 | Late joiner via the state | `server.cpp` (late ready), core first-assign branch | needs audit #16 (and #9 option B for pooled levels); §7.3 late join, its last-sector variant and its 10% loss run; A3's arms unchanged |
| B2 | Lift the Sector-margin cap on raises | `session/lead.hpp:209-213` (the `sector_bound` term) and its test | late-player-design step 14: A5u with sectors 4, client 2's and the others' mechanism error ≤ control; `lead_test` updated. After A3, under protocol 10, because a pre-B1 client would suffer from a raise past its margin |
| T1 | Client journals replay through a resync | `sim_tool.cpp --replay-session` reads RESYNC (rows, scripts and union at S, the reconcile, then continues) | a resynced client's journal replays exact against itself after the RESYNC record |
| T2 | Trafo scripts out of `keep_local` | `session/resync_rules.hpp`; after `docs/trafo-replication-design.md` lands with its `trafo_live` join/resync state (its §4.7, which itself waits for this track's A3) | the trafo design's H1 arm with a forced resync of the trafoing client during its pieces' life: pieces bit-equal to the server's after the apply; A3's arms unchanged |

**Ordering with the other tracks.** A3 must land before the trafo
replication's T3 (that design's §8 T0 lists F3, which is this track's A3).
Until T2, a resync inside a trafo's 19.6 s keeps the owner's own trafo
state: the pre-existing owner-only gap (audit #2), not a new one.

## 9. Open questions

**Q1. Is a rows-only anchor exact enough, or must the server's IVP blob
travel?**

- A row carries pose, velocity and sleep. The client's own blob supplies
  the contacts, mindists, impulse queues and calm references, all from its
  pre-resync world, and nothing for the bodies it lacked (arm S).
- For hanging bodies (the sack) this should vanish. For bodies in contact
  (Pusher001 in its Rinne, piles) it is the known pile class. For bodies
  unknown to the blob, what `set_savestate` does with their constraints is
  not known yet.
- **Settle:** with A2's arms R and S.
  - If they meet ≤ 1 mm, rows stay.
  - If not, a server blob would be needed. It must be measured first, by a
    SimTool cross-world restore test:
    - name mapping: clone vs own ball, `_BMMO_`;
    - the x64-server vs x86-retail blob format;
    - size (101,375 bytes on L11 sector 1 alone, `ssc_l11.txt`).
  - It is decided before A3, never waived.

**Q2. The reach of the re-step: 95 or 48?**

- Off the per-player path, L ≤ 44 - input_delay (late_tick.hpp:130-134,
  149-151), so D stays under 48. With `lead_adjust`, L_eff reaches 80
  (lead.hpp:45).
- **Settle:** measure `t_max` and `rewind_us` of a forced 80-tick re-step on
  retail L11 (`session force-rollback`-style verb on the re-step path) at
  the deployed snapshot interval.
  - If one frame stays under 100 ms, the cap is 95.
  - Otherwise the cap is 48, with the resync hold engaging for raised
    members.

**Q3. How often does the apply wait for the body set (§2.6.4 step 2), and
for how long?**

- **Settle:** count `resync_deferred` and its ticks in every A3/A5 arm.
- Expected: for resyncs, the passes of `(R, S]` (Δ < 0); for late joiners,
  the owed passes, about 7-10 ticks each per lp1_join's 1642/1649.
- Any wait longer than the owed passes explain is investigated.

**Q4. Should every replay re-derive the union, not only B1's and A3's?**

- That would be simpler and uniform, but it puts every rollback on the new
  path.
- **Settle:** run the swing, lane and a6m arms with re-derivation forced on
  and count passes moved or forgotten.
  - If the count is 0 on every arm, every replay re-derives and the
    re-issue of union passes from records goes away.
  - If it is non-zero, the moved pass is a bug in the records to find
    first.

**Q5. Protocol and journal numbers** (this track, the sector-placement
design and pooled-module option B all ask for protocol 10; this track and
the sector-placement design both ask for journal version 4).

- **Settle:** at each commit; the second change takes the next number.
- Their message and journal fields are independent.

**Q6. Client proximity sensing (G-c).**

- This is outside this track. The proposed principle-(a) fix rewires each
  client's `TT Scaleable Proximity` like the server's: the nearest of the
  own ball and the mirrors, deterministic from the same poses.
- **Settle first:**
  - a headless two-client arena run with client 1's ball at P_Modul_01 and
    client 0's elsewhere;
  - compare the Pusher's wake tick in client 0's journal with the
    server's;
  - expected: client 0 never wakes it until a snapshot.
- Owner and order to be set by the user. Until then I1 holds for the
  imported Proximity state at S and not after it.

**Q7. The retail resync hold blocks a frame.**

- This applies only when D would pass the reach.
- **Settle:** the A4 runs log every hold with its length. A hold seen in
  normal retail play becomes a non-blocking variant (`nonblocking_hold`
  capability) before R7.

**Q8. Does the manager activate `PE_Balloon Script` through next-frame
flags?** §2.8 relies on it to re-issue a forgotten pass's balloon.

- **Settle:** the A1 check (§7.2). SimTool `--describe-script
  Gameplay_SectorManager` names the block that activates module scripts.
- If it does: as designed.
- If it does not (an immediate activation inside the frame): the host gains
  `activate_local_modules(sector, tick)`, which the re-derivation calls at
  the forgotten pass's tick for the `keep_local` scripts of that sector,
  with reset, as the manager's block does; the A5 last-sector variant is
  its check.
- **Settled (step A1): as designed.** The manager's Activate Script sets the
  next-frame flags (reset included) on every level; §2.4, "As built (step
  A1)".

**Q9. How often is Δ > 0 in practice, and how large?**

- The references have only stall resyncs (Δ < 0). Hard-correction and
  substitution resyncs of a running client are the Δ > 0 case.
- **Settle:** the Δ now logged at every re-assign (§2.6.1 step 2), over the
  A3 harness arms and the A4 retail runs. A Δ beyond `history_ticks` cannot
  be reconciled from `life_log_`; if one is seen, `life_log_` and
  `pass_log_` are sized from the measured maximum.

## 10. Review

Two reviews of the first draft (correctness/determinism lens). Every
finding was checked against the code; all were confirmed. None is refuted;
three are taken in a different form, with the reason.

| # | finding | verdict | where |
|---|---|---|---|
| R1.1, R2.1 | a Sector rewind drops stamps that reached the live union after the anchor record; two late stamps in descending order; no failure path | confirmed (client_core_frame.cpp:142-153 stamps the live union only; the draft re-derived from the record plus one stamp) | §2.5 stamp log, §2.7 frame's Sector rewind with one anchor and a failure path, §2.8 pending from the log, §7.1 tests |
| R1.2 | the synthetic anchor is not at base-1 (Δ ≠ 0) | confirmed (client_core.cpp:369-397); the six measured resyncs all have Δ < 0, the running-client case has Δ > 0 | G-d, §2.6.1 (Δ logged), §2.6.3 reconcile, §7.3 running-client arm, Q9. **Different form:** the reviewer's fix (mark the anchor's own passes as passed) would leave those sectors' module scripts in the server's pre-pass state, never to run; the reconcile instead brings the anchor to the server's body set at S and replays the rest |
| R1.3, R2.9 | resync births: rowed parts simulated without their unrowed fixed parts until their birth tick; joints; no offline arm | confirmed (rollback.hpp:952-966) | §2.6.3 rows and attribution, §4.1 rechecks at the restore, joints paragraph, §7.2 arms S and A. **Different form for R2.9's criterion:** "exact against the straight run" cannot hold for bodies whose contacts the anchor's blob lacks (that is Q1's class); arm A, whose bodies are made from nothing in both runs, is required bit-exact, arm S ≤ 1 mm |
| R1.4, R2.6 | adopt mode leaves `sector_cell_` stale: phantom own crossing, a real one then dropped; retail's `sector_cell_is_own_` | confirmed (client_core_frame.cpp:193, :235; core_host.cpp:1412) | §4.6 per-cell rule, cell re-read, `sector_cell_adopted`; §7.1, §7.3 |
| R1.5, R2.2 | the trafo scripts imported from a server that runs no trafo | confirmed (trafo-replication-design.md §1.1; script_replay.hpp:207-223) | G-e, `keep_local`, §8 T2 and ordering |
| R1.6, R2.8 | a moved own Sector echo is kept at the origin | confirmed (client_core_lifecycle.cpp:1062-1065, :1112) | §1.2, §2.5 log rewrite, §2.7 moved echo (plan step 23 folded in) |
| R1.7, R2.7 | one activation record per tick: a pass overwrites a pending activation | confirmed (session_host_support.hpp:213-222; client_core_frame.cpp:395) | §2.8 two slots and re-issue order; §7.1 |
| R1.8, R2.11 | `base - 1` can be stepped before the runner sees the request | confirmed (session_runner.cpp:104-108, asynchronous post) | §2.3 missed-base check and re-assign |
| R1.9 | line numbers labelled HEAD were working-tree numbers | confirmed | header: every number is the working tree's |
| R1.10 | the re-assign also clears `mechanism_tracking_` and `have_snapshot_`; the anchor's tracked set must not use the registry | confirmed (client_core.cpp:391-393) | §1.1 step 3, §2.6.1 step 3 |
| R2.3 | a late joiner never gets its balloon | confirmed; it also hits a Δ < 0 resync that runs the last sector's pass live | §2.8 keep-local activations, §2.9, Q8, §7.3 last-sector variant |
| R2.4 | SessionAssign repeats carry no flags | confirmed (server.cpp:1476-1478) | §2.3, §5, §7.3 at 10% loss |
| R2.5 | retail has no mid-session blocking hold; a naive one rebases the tick driver and asks for another resync | confirmed (core_host.cpp:913-940, :969-972; session_client.cpp:1843) | §2.6.6 |
| R2.10 | registry-typed parameters are process-local | **different form:** no failing case exists today (the only Message locals are in a keep-local script or are constants); encoded by name anyway, and listed per level by A1 | §2.4 format kind 3, §7.2 |
| R2.12 | the journal version is shared with the server's journal | confirmed (journal.hpp:74) | §5, Q5 |

## 11. Decisions (user, 2026-10-23)

- Every gap found here is to be fixed (the user's rule (d)); the balloon and
  its airship stay local (exception (c), I3).
- **G-c, client proximity sensing** (Q6): to be fixed as its own step after
  this track and the trafo replication: each client rewires its
  `TT Scaleable Proximity` blocks like the server, to the nearest of the own
  ball and the mirrors, deterministic from the same poses.
- **Q5, numbers:** protocol 10 and journal version 4 are taken by whichever
  of this track, the trafo replication and the sector placement lands first;
  the next takes the next number.
