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
| core_ref_e0_pre_q/resync_snapoff | c2 | 1572 | 1555 | 1572 | 17 |
| core_ref_e0_pre_q/resync_snapoff | c0 | 2821 | 2794 | 2821 | 27 |
| core_ref_e0_pre_q/a5u_lead (lead on) | c2 | 2737 | 2695 | 2737 | 42 |
| r1_c_on1 (retail, L11) | c2 | 1527 | 1499 | ~1527 | 28 |
| r2_x_t1_b (retail, L11) | c1, c2 | 524 | 496 | ~524 | 28 |

The snapshot arrives with the assignment, so B is about `base` and B - S is
about L.

**Measured Δ.** All six resyncs of the arena references (the E0 set,
`core_ref_e0_pre_q/`) are "tick schedule
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

**Measured, core_ref_e0_pre_q/a5u_lead** (the E0 set; the REF set's
numbers are in the As built (step REF) note under §7.5):

- The server logs `sector +4 at tick 1714 (player 2518006732, stamped 1714)`
  (server.log:203).
- Clients 0, 1 and 3 received the stamp at local ticks 1717-1722 (each one
  logs `LATE: the server started it already`) and started the sector at
  1723 (`held by the pacer`).
- So the whole sector runs 9 ticks out of phase in three of four worlds for
  the rest of the session. Audit #5 measured the result as Schaukel 2.873 m,
  P_Modul_03 1.36 m and PE_Balloon up to 6.8 m.

**A late stamp that changes nothing.** In core_ref_e0_pre_q/resync_snapoff,
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

**As built (step A3, 2026-10-24).**

- **Wire (§5 adapted).** Protocol 10. The state is not a `full = 2`
  snapshot but its own message, `SessionResyncState`
  (`message/session_resync_state_msg.hpp`, opcode appended): session, tick
  S, base, the rows (the full-snapshot row format, names included), every
  player's server clone name, every player's navigation state
  (`bmmo_physics_nav_state`), the union bytes, the portable script record
  and the server's savestate blob as u32-length sections, plus the two
  sections' unpacked sizes. The server packs the record and the blob with
  `CKPackData` (zlib, level 6) and the client unpacks with its CK's
  `CKUnPackData` (`client_platform::unpack_bytes`, both hosts). Unpacked,
  the state was 150-250 KB, which GameNetworkingSockets' default 256 KB/s
  send rate delivered in 40-65 ticks: D reached 72 and every relayed frame
  of the window left the reach. Packed it is 29-50 KB on the arena
  (blob 108-145 KB -> 20-38 KB, record 40 KB -> 5 KB) and D is 12-53.
  `SessionAssign` carries `u8 flags` on every copy.
- **Capability.** `SessionReady.caps` gains `CAP_RESYNC_STATE`, set by a
  core with rollback on (the apply is a re-step; `unpack_bytes` is a
  required platform operation). A resync of a member without it - the
  legacy retail path, a core with rollback off - is answered as before
  protocol 10: no flag, `request_full_snapshot` to everyone (server.cpp
  `resync_member`). The server's "resynced at tick"
  log line keeps its format (the harness reads it); "the state of tick S
  follows" is a second line.
- **Server.** `physics_world::capture_resync_state` (rows numbered without
  touching the body-set bookkeeping, clones, navs, union, the portable
  record over the selection the world makes at `anchor()`, the savestate),
  `session_runner::request_resync_state` / `emit_resync_states` after the
  step that runs base-1 (journal NOTE per state and per missed base, which
  answers again; the miss test is `tick_index() >= base`, the index being
  the next tick to step). Emitting costs 3.9-5.5 ms. A state larger than
  the message's caps (rows, 4 MB sections) is logged as cut. The server never reads it back
  and is independent of snapshots.
- **The travelling savestate (bridge note, no engine change).** physics_RT
  API v20 `savestate_set_travelling`, behind a `savestate_travel` struct
  (renames, excluded prefixes) on a `set_savestate` overload instead of a
  flag bit: the environment-id check is waived, objects are matched by
  name after renames and by occurrence order for shared names
  (`live_world::find_by_occurrence`), keep-local prefixes are skipped and
  counted, a callback list that differs in length is a note, not a
  refusal; the PreSimulate queue is not compared (the live one is kept).
  Restore flags stay 0 (unknown bodies dropped as before). Every clone is
  renamed, mirrors included: the server names a player's clone
  `Ball_*_BMMO_<id+1>`, a client's mirror can be `<id+2>` (the CK name
  bump), and the own clone becomes the own ball (`resync_renames`). x64
  and x86 blobs are identical past the 16-byte header; the x86 SimTool
  restores an x64 blob byte-identically (§9 Q1 decided: the server's blob
  travels).
- **Navigation travels.** The anchor's own navigation (keys held, the
  `bmmo:force/<leaf>` controllers made with the camera of the key press)
  is not the server's; restoring the server's ball with the client's
  controllers put the own ball 0.88 m off. The message carries every
  player's nav state and the apply installs them (mapped to local
  entities) in the anchor record.
- **Keep-local.** `PE_Balloon` and `PE_Box_slide` (`resync_rules.hpp`):
  the slide is jointed to the keep-local balloon platform by the
  `PE_Balloon Script` slider and spring, and restoring it alone moved it
  10 mm -> 44 m.
- **Client core.** `client_core_resync.cpp` (new): `anchor_resync`,
  `on_session_resync_state`, `try_apply_resync`, `poll_resync_hold`.
  Rows map through the registry, with a name fallback when the body
  exists (the registry's 20 m identity guard refused a far-diverged sack
  forever). A missing body defers (`resync_deferred`) and the apply is
  retried from `pre_step`. Fixed parts are rechecked, not destroyed;
  `forget_births_after` is limited to the rowed entities. Lifecycle events
  stamped before the pending anchor are skipped (the state covers them);
  snapshots are dropped while the exact resync is pending. The apply runs
  `rollback_.resync` inside the live-cells guard with the union
  re-derivation of §2.8, then restores the union and adopts the cells.
- **Resync hold.** Spent once timed out (`resync_hold_spent_`): re-engaging
  after a timeout stalled a client for 14 s and its next base came back
  946 ticks early. The timeout asks for a new resync at once (every other
  trigger waits while one is pending), and a refused state's request that
  the 2 s limiter holds back stays owed while the exact resync is pending
  with nothing in flight (`retry_owed_resync`). In the final runs D never
  came near the reach (max 50 of 95): holds 0 everywhere; the hold is
  covered by unit tests only.
- **Δ > 0.** `--resync-at` cannot make it: the server's base is its tick
  plus the member's lead, and a running client is that lead ahead minus the
  downlink trip it started with, so Δ is about the change of the downlink
  trip since the start (-5/-6 at a steady 150 ms). The running-client arm
  with Δ > 0 is therefore `--event 0:600:step=300:dir=down` beside
  `--client-arg-for '0:--resync-at s2+3'` (`core_ref_r3/resync_at150_down`,
  Δ +14). It showed a gap the design had not named: the old numbering had
  sent its frames for base..R, the server's buffer keeps a frame it has at
  or after the base (`input_buffer::reset`) and takes the first copy of a
  tick (`submit`), so it applied those while the client played new ones
  (14 own frames keys 1 vs 0, the own ball 0.77 m off after the apply).
  The re-assign now takes those frames from the sent ring and plays them
  again at their ticks as forced own frames (`resync_resent_`); the inputs
  then agree (0 of 74 differ) and the own ball is bit-exact from B+2. The
  pass undo itself (a pass inside (S, R]) is covered by a unit test only.
- **Review fix-up (two reviews).**
  - The apply runs after the drain the state arrived in (`poll_resync_hold`,
    which both hosts call before `pre_step`), not on arrival.
  - A held notice for a tick of the pending exact resync's epoch amends the
    own RECORD (the re-step feeds the own ball from the record, not from
    `own_inputs_`), with no rewind of its own; one for a tick not simulated
    yet is amended when its live frame is recorded (`resync_amends`). Stored
    in `own_inputs_` alone it left a5u_lead's own ticks 2779-2788 at keys 0
    where the server applied 5, and the own ball 0.17 m off for good.
  - After the apply `held_floor_` is S, not base: the history reaches S,
    so a notice for base itself is replayed like any later one (§2.6.4's
    rule, which `held_floor_ = base` excluded).
  - Inside the re-step window (S, applied tick] a correction - a held
    notice, a relayed frame - rewinds as deep as the re-step
    (`window_rewind_reach`, `rewind_to`'s `reach`), not max_resim_ticks:
    with D 51-53 the server's substitutions of the new epoch's first ticks
    each asked for the next resync (a5u_lead: 6 resyncs vs B1's 2).
  - A correction noted before the apply is subsumed by its re-step
    (dropped, counted `subsumed`).
  - A mirror dead after S is made from its row before the renames and the
    navigation are built, and its death is queued whatever its tick and
    drained live by `pre_step` when it lies past the apply.
  - The bridge reports a name list that overflows the caller's buffer in
    the summary, not as a failed restore.
  - Refuted: `restore_and_replay` cannot refuse after it changed the anchor
    record (its only `false` is before), so the anchor never keeps the
    server's scripts for a re-step that did not run.
- **Test hooks.** SimTool `--resync-at TICK | s<K>+<D>`, and
  `--record-by-tick` for §7.6's recorded-frame comparison.
- **Measured (final binaries, fix-up included; snapshots off, 150 ms,
  arena, 4 clients; `core_ref_r3/`).** Every apply: unportable,
  unmapped, refused, unexplained, unattributed 0; P_Modul_01/08 within
  0.000003 m to the end; phase_misses 0; every client's snapshot count
  equal (no broadcast for a resync). The own ball is bit-exact from B+2..B+4
  in every apply but one and stays so 20-160 ticks (the one: c0's second
  resync in resync_at150_down, Δ +4, whose mirrors were off from the
  relayed frames its +300 ms downlink brought past the reach, 81 in that
  minute, in the lane pile the own ball shares); what follows is (a) the own
  frames the server substitutes while the notices are on their way (the
  lead client of a5u_lead), (b) G-c (leader-only wakes, roadmap §4 item 7):
  P_Modul_03 1.35 m and P_Modul_19 1.8 mm in every non-leader, resynced or
  not, from the leader's BodyRevived, and the non-leaders' own balls after
  it, and (c) mirror prediction in the lane pile where relayed frames come
  past the 48-tick reach. resync_snapoff: 4 resyncs (E0 4), each exact from
  B+2..B+4 for 120-160 ticks, c2 exact again at the end. resync_at150
  (Δ -5): c0 716/850 bit-exact after the apply, [B+60, B+600] 228/271
  (before the fix-up: 38/263 within 1 mm, max 6.5 m). a5u_lead: c2 1 resync
  (B1 2), D 50, exact 2946-3094 and at the end. resync_at150_down (Δ +14):
  exact 924-942, then the lane pile with the recipe's late relayed frames.
  Server replays exact; the 16 new captures replay byte-identical with
  identical sends. Solo (§7.6): the server's
  checkpoints, pre-A3 vs A3, 0/1434 differ; a solo resync at tick 1500
  (Δ -7, D 56) leaves the server identical through tick 2407 and the
  client bit-exact to it after the apply (34 bodies); the first server
  difference, at 2408, is an applied input frame (the client's NAV_ACTIVE
  flag phase after its renumbering), not the server's world.
- **Open.** The resync hold has not run live (D at most 50 of 95). The pass
  undo of Δ > 0 needs a pass inside (S, R], which the live recipe does not
  place; a unit test covers it.

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

**As built (step B1, 2026-10-24).**

- **Where.**
  - `session/sector_union.hpp`: `state_type`, `state()`, `restore()`,
    `unstamp()`, `covers()` (case 2), `with_pending()` (the merge of §2.5 and
    the start of §2.8: one function), `serialize`/`deserialize` (§5's bytes,
    `kStateVersion` 1; a state the format cannot hold - more than 255
    sectors or origins, more than 65535 stamps - serializes to nothing,
    which `deserialize` refuses, never to a truncated other state),
    `birth_moved()` (the attribution below); `sector_pacer::restore`.
  - `session/sector_union.hpp` `sector_rederivation`: the re-derivation
    rule itself, CK-free and the one implementation - `start` (the record
    with `with_pending`, the window's live passes moved out of the pass
    log), `tick` (the pass to put in the slot, the slot to empty, the live
    pass forgotten), `abandon` (a refused rewind: the live passes back),
    `through(now)`, `birth_moved`. The core's `rewind_for_sectors` /
    `rederive_union_tick` and SimTool's `--late-sector-check` both drive
    it, so the offline check runs the clients' code; each does only its own
    side effects (the platform's or the activations' pass slot, logs,
    records). A3's resync re-step, which §2.8 says also re-derives, reuses
    it.
  - The core: the stamp log `sector_stamps_`, `union_records_` (recorded in
    post_step beside the rollback record, cleared at every
    `invalidate_history`, trimmed to `history_ticks`), `pass_log_` (live and
    re-derived passes, `history_ticks + 128`), the frame's late set
    (`sector_late_`), `rewind_for_sectors` (pre_step, after the drain, the
    echoes and the script guards, before `replay_corrected_inputs`),
    `rederive_union_tick` (in `before_replayed_tick`, after the births),
    `move_own_sector`, `last_rewind_rederived()`, the cell re-read; counters
    on the status line before `input:` (`sectors: rew=made/missed depth=
    forgot= births_moved= phase_misses=`).
  - `client_platform.hpp`: `set_union_pass`, `sector_cell_adopted` (both
    required); `core_world_hooks::birth_moved`.
  - Host support: `activation_slots` (CK-free, `session_host_world.hpp`) and
    `manager_activations` on it (`record` = the live pass slot,
    `record_pending` = the pending slot, `set_pass`, `forget_pass`, re-issue
    pending then pass); `live_cells_guard` adopt mode
    (`core.last_rewind_rederived()`); `bind_session_world` hands
    `birth_moved` on.
  - `rollback.hpp`: `rollback_world::birth_moved`, `rollback_stats::births_moved`.
  - Hosts: one line each (H: `manager_activations_`; RC: `host.activations`,
    and `sector_cell_adopted` clears `sector_cell_is_own_` for a cell that is
    not `current_sector_`, run_sector's bookkeeping).
  - `sim_tool.cpp --late-sector-check K T A` (§7.2).
- **Adaptations** (no principle touched):
  - The value type is `sector_union::state_type`: `state` is the accessor.
  - **An after-the-fact own crossing is not in the stamp log.** §2.5 lists
    the unheld crossing report (client_core_frame.cpp poll_own_sector
    without the hold; the retail hook's report) among the log's stamps. It
    never entered the live union (the level's chain ran the manager), so a
    re-derivation that took it would run a manager pass the live world never
    ran. Only stamps handed to the union are logged; a MOVED echo of an
    unlogged own Sector is kept where stamped, as before.
  - **Case 4's "late joiner's catch-up"** is a stamp at or before this
    numbering's first tick (`event.tick <= tick_base_`): stamped live, no
    resync (it never has a record). A stamp beyond the history otherwise
    resyncs once (the request in flight covers the rest).
  - **`sector_cell_adopted(int& cell)` returns the cell**: the core cannot
    read CK. The host reads `IngameParameter[0][1]` after the guard has
    ended (the guard's re-issue of the next frame's activation included) and
    the core takes it as `sector_cell_`.
  - **`births_moved` (engine, not in the design).** A window-born body of a
    live pass the re-derivation moved or forgot is destroyed at the restore
    like any window birth and is not made at its live birth tick; without
    the hook the engine counted it in `births_missing`, which §7.2 requires
    to be 0. The core's hook attributes the birth to the window's live pass
    at M with M+5 = b ONLY (its physicalize frame) and asks whether the
    re-derivation has that pass at M; a body born at any other tick belongs
    to no pass and, missing, stays a missing birth (the fix-up round
    dropped the first version's fallback "else the newest pass with
    M+5 <= b", which could have counted a real missing birth - a trafo
    piece of a later tick - as moved). The engine logs the first ones
    (`birth: X is not back after its birth tick T: its sector pass was
    moved or forgotten by the replay`). The body made later by the moved
    pass is recorded from then on by the existing `replay_mechanisms` rule.
    Measured with the strict rule: the moved own Sector case, 11 bodies,
    every one born at 305 = 300+5 (P_Modul_19_Flaps, P_Modul_03_*); the
    forgotten-pass case, 29.
  - **A moved own Sector is acted on with the own echoes only**
    (`acting_on_echoes`). The retail core runs `own_echoes` off until R4, so
    a retail client keeps its own moved Sector at its stamp, as before B1.
    The late remote stamp needs `rollback` only and runs on retail.
  - Re-derivation runs only in the frame's Sector rewind; every other replay
    re-issues the recorded slots (Q4 stays open).
  - **The balloon of a forgotten pass.** A1 records `PE_Balloon Script`'s
    pending activation (a keep-local script, §2.8) at the tick the live pass
    had it, and every replay re-issues it from that record. When a B1
    re-derivation moves the pass (the moved own Sector case), the balloon is
    therefore activated at the old pass's tick from the record and again by
    the moved pass's walk; its proximity countdown then runs a different
    phase from the on-time world (`Last Check` 9 vs 60 at A, for the rest of
    the run). It is exception (c), never compared (I3), and the balloon
    bodies stay equal in the check; left as it is.
- **Checks** (rB1, the working tree on fcb4d37):
  - Unit tests 446/446 (new: `SectorUnionState` ×4, `ActivationSlots`,
    `SessionClientCoreSectorRewind` ×8; `DrainsInTheServersOrder...` now
    expects the B1 rule for its late stamps, none of which has a record:
    stamped live and one resync). Arch GCC 16.2 and MSYS GCC 13.3 syntax
    checks: every file ok.
  - **`--late-sector-check`, the arena** (each case with `--late-reference`;
    T = root+60, 601 boundaries A..A+600): K ∈ {2, 3, 4} × A−T ∈ {3, 9, 40};
    a later live pass forgotten (K 3 late at 60 → 100, sector 4 on time at
    62: forgotten 1); an own stamp inside the window (sector 4 at 80:
    forgotten 0); a moved own stamp (4 at 60 moved to 69, echo at 75:
    forgotten 1, births_moved 11). Every case: blobs equal ignoring the order
    bookkeeping at 601/601 boundaries (byte-identical 0: unit numbering only,
    PE_Balloon's in the B1 arm, A_Floor's in the ordinary-rewind reference),
    union identical 601/601, births_missing 0, no Sector event from the cell,
    and every script difference one the ordinary-rewind reference has at the
    same boundary (verdict PASS-EXCEPT-REFERENCE). The one difference: the
    pre-existing residual below.
  - **Wire check** `wire_check.sh rA2 rB1 rB1w` on the REF set: §7.5's B1
    table exactly. 13 captures byte-identical in sends and journals;
    a5u_lead c0, c1, c3 change: sector +4 at 1713, re-derived in a rewind at
    live tick 1721 (`rollback to tick 1712 ... re-simulated 9 ticks`), the
    own crossings stamped at their own ticks (1790, 1807, 1798); their TICK
    records differ from 1722 on (the TICKs of 1713..1721 are the live
    frames' as written before the stamp arrived) and their sends from 1728.
  - **a5u_lead live** (`b1_a5u_lead`, snapshots off, patience and lead on,
    captures): the server and every client log `sector +4 at tick 1713`
    (c0 re-derived at live 1719, c1 and c3 at 1718; one Sector rewind each,
    depth 6-7); no client sent a Sector 4 before its own crossing (c0 1790,
    c3 1798, c1 1807); `phase_misses` 0; server journal replay 3292/3292,
    0 checkpoint mismatches; harness verdict as the REF capture's (the arm's
    starvation and c2's resyncs). `first_divergence --from 1713 --exclude
    PE_Balloon`: sector-4 bodies against the server over 1713-1837:
    P_Modul_03_*, P_Modul_08, PE_Box_slide exact in every client; one LOCAL
    checkpoint (1718) per rewinding client lacks the P_Modul_03 bodies (it
    was written live before the stamp arrived); then the leader-only wakes
    below (P_Modul_19_Flaps 1.79 mm from 1728, P_Modul_03_Floor 0.066 m at
    1838) in c0, c1 and c3 alike.
  - **Lane control** (snapshots off, rA2 then rB1 in one batch): passes
    +2@824 +3@1270 +4@1713 on the server and every client in both; resyncs
    1/1 per client in both; rewinds 396-518 vs 407-532, t_max 45-63 vs
    46-52 ms; sector rewinds 0, forgotten 0, phase misses 0; first
    divergence identical in pattern (non-leaders P_Modul_01_Pusher 0.0895 m
    at 614, c2 after its resync).
  - **Solo snapshots off** (`e0_solo.sh rB1`): `e0_compare --live` against
    old1 IDENTICAL (2406 ticks, 0 differ).
  - **Retail** (`scripts/retail/b1_retail_1`, Level 11, core on, Arch
    server snapshots off): client 1 `net lag 300 up`, then `sector 2`
    (stamped 437, lead 9); the server ran +2 at 437 (applied = origin);
    client 2 got the relay late and logged `sector +2 at tick 437 ...
    re-derived in a rewind at live tick 448` (one rewind, 12 ticks); one
    Sector event in the session (client 1's), none from client 2.
- **Residuals, pre-existing and root-caused** (each its own item):
  - **P_Modul_18_Particle Script's `CKBEHAVIOR_DEACTIVATENEXTFRAME`.** The
    on-time world keeps the bit (0x400000) on the inactive script for the
    rest of the run; any world that replayed the tick it was set in has it
    cleared. Cause: `script_replay::execute_frame` consumes the next-frame
    flags of every selected script, while the live CK frame consumes them
    only for scripts whose owner is in the behaviour manager's object list,
    which this inactive script's owner has left (A2's G1 mechanism). Shown
    by the ordinary-rewind reference of every `--late-sector-check` case at
    the same 601 boundaries, so not B1's; it is the "engine script check"
    line the sector-activation replay notes recorded on 2026-10-22. Why
    the reference stands for the build before B1: the code it runs is
    rA2's - `script_replay::execute_frame` (script_replay.hpp:503-511, where
    the flags are consumed) is unchanged by B1, the reference arm's
    re-derivation is off (the `birth_moved` hook answers false), and B1's
    only change on that path, the two activation slots, re-issues what the
    one-slot map did on every tick where the frame's pending activation and
    a union pass do not share the tick; the rewound line's `coincident=`
    counts those ticks and is 0 in all 12 references (and arms). rA2's own
    SimTool has no mode that replays the session selection with
    `execute_frame` and compares it (`--savestate-check --frames` replays
    the 16 mechanism scripts only; run on rA2 with the arena drop scene it
    shows the balloon's proximity float, not this script), so the
    equivalence is the evidence. Queued as **F** in
    `rollback-engine-gaps-design.md` (roadmap §4 item 8): `execute_frame`
    takes a script's next-frame flags only where the live frame would
    (shared script replay, both hosts); `--late-sector-check` then exits 0.
  - **Leader-only wakes (G-c, §1.3; Q6, decision §11).** A mechanism woken
    by a proximity sensor or a Physics WakeUp of the leading ball wakes in
    the server's world (the client's BodyRevived, or the server's own
    rewired sensor) and in the leader's, never in the other clients'
    (BodyRevived goes client to server only; their sensors watch their own
    ball). In `b1_a5u_lead` the leader c2's BodyRevived for
    P_Modul_19_Flaps at 1728 and P_Modul_03_Floor at 1838 are exactly where
    c0, c1 and c3 depart, all three by the same amount; the pre-B1 REF
    a6m_snapoff shows the same (P_Modul_19_Flaps 1.0 mm in the three
    non-leaders from the leader's BodyRevived at 1703, and every REF
    capture's non-leaders on P_Modul_01_Pusher at ~600). Its size over
    1713-2700 (`sector_body_max.py`, both B1 runs alike): the P_Modul_03
    gate and walls up to 1.35-1.37 m in c0, c1 and c3 (1936-1970, the
    module the leader woke at 1838 moving on the server and in c2 only),
    P_Modul_19_Flaps 1.79 mm; c2 within 0.064 mm. So §7.3's "no sector-4
    body beyond client 2's own maximum + 1 mm" fails on G-c alone; the fix
    is world-agreement (roadmap §4 item 7: client sensors rewired to the
    nearest ball, own or mirror).
- **Review fix-up** (two reviews): the re-derivation moved into
  `sector_rederivation` (above) and SimTool's copy of it deleted; the
  strict birth attribution; `serialize` refuses what it cannot hold;
  the engine logs moved births; `--late-sector-check` prints
  `coincident=`; the drain's Sector comment says a reachable late stamp
  rewinds even while a resync is pending (§2.7 case 3). Not changed, with
  the reason: (a) `move_own_sector` still rewinds for a moved own stamp
  whose sector another member's earlier stamp already requested - the
  re-derivation gives the same passes, so it costs a rewind, never a
  difference, and a moved own Sector is acted on only with the own echoes
  on (headless, rare); (b) a late joiner's catch-up stamp that arrives in
  the same drain as a reachable late stamp is re-derived from the anchor
  tick + 1 instead of the live tick + 1 - the server ran that pass before
  this numbering began, so neither tick is the server's, and dropping it
  from the merge would lose the stamp when the union is restored from the
  re-derivation. Rerun (rB1 rebuilt, the same working tree plus the
  fix-up): unit tests 448/448 (new: `RederivationMovesForgetsAndAbandons`,
  `SerializeRefusesWhatTheFormatCannotHold`; `BirthAttributionToAMovedPass`
  to the strict rule); Arch GCC 16.2 and MSYS GCC 13.3 every file ok; x64
  BUILD-OK, x86 CLIENT-BUILD-OK (client objects deleted first); wire check
  `rA2 rB1 rB1f`: its whole output identical to the first round's (13
  captures byte-identical, a5u_lead c0/c1/c3 the same differences);
  `--late-sector-check` all 12 cases as before (601/601 equal ignoring
  order, union 601/601, births_missing 0, forgotten 1/0/1 where expected,
  births_moved 11 and 29, PASS-EXCEPT-REFERENCE with no script difference
  the reference lacks, `coincident=0` everywhere); a5u_lead live
  (`b1f_a5u_lead`, snapshots off): server and every client `sector +4 at
  tick 1713`, c0/c1/c3 one Sector rewind each (live 1721, depth 9),
  own crossings stamped 1790/1807/1799, `phase_misses` 0, server replay
  3594/3594, first divergence of c0/c1/c3 P_Modul_03_Floor 0.0656 m at
  1838 (G-c), the LOCAL checkpoints 1718 and 1720 written before the stamp
  arrived lack the P_Modul_03 bodies, c2 at 2800 after its resync of 2760
  (A3's); solo snapshots off (`rB1f`) IDENTICAL to old1 and old2; retail
  (`scripts/retail/b1f_retail_1`, Level 11, core on): client 2 `sector +2
  at tick 446` = the server's, one 12-tick rewind, one Sector event in the
  session (client 1's). The lane-control arm was not rerun: it makes no
  Sector rewind (0 in both first-round runs), the only code the fix-up
  changed on a live path.
- **Not run at this step** (quick tier: one run of each check the change
  can reach): the a5u_lead recipe's second and third runs and the swing and
  a6m regression arms of §8's row.

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

**As built (step A5, 2026-10-24).** Audit #16 was built first, as the
sector-placement design's step S4 (its §2.6 As built note: the gate is
`gated_sector_union`; the legacy path keeps the gate-free union).

- **Server** (`server.cpp handle_session_ready`): a late joiner with
  `CAP_RESYNC_STATE` gets its assignment (and every repeat) with
  `ASSIGN_FLAG_STATE_FOLLOWS`, the last Physicalizes and the sector history
  as before, then `request_resync_state(session, joiner, assigned)` instead
  of the full snapshot broadcast; it logs `the state of tick S follows to
  the late joiner X alone (protocol 10)`. A joiner without the capability
  gets the broadcast as before. A missed base is answered by
  `resync_member`, as any resync's.
- **Core, first assignment** (`client_core.cpp on_session_assign`): with
  the flag, rollback on and a numbered base, the ordinary first-assign
  renumbering runs, then `resync_pending_`, `resync_exact_` and
  `anchor_resync(base, old tick)`: the anchored level start recorded as
  base - 1, Δ = -(base - 1) (`late join: the server's state of tick S
  follows`). The owed passes then run live from the catch-up stamps (B1's
  case 4), after the level's own walk (S4), and the apply waits for their
  bodies (A3's wait).
- **Adaptation (not in the design): the level's own walk is dropped from the
  re-step.** Init Ingame's activation of the manager is a pending
  activation the live frames record (`manager_activations::record_pending`)
  and every replay re-issues at its tick; after the restore of the
  server's state at S it would walk sector 1 again over the server's
  mid-run modules (Set World Matrix and module reset). At the apply, when
  the gate opened by the walk's edge inside (S, now], the core drops the
  manager's pending activations of base .. the walk's tick (new required
  platform op `forget_pending_activations`, host support
  `activation_slots::forget_pending`, one line per host), and logs `the
  level's own walk (seen at tick N) is in the server's state of tick S: k
  manager activation(s) of base..N not re-issued`. Review fix-up: the drop
  is confined to the late joiner (`resync_join_`) and to a walk seen in the
  current numbering (`walk_open().known`, cleared by every renumbering
  assignment, sector-placement design §2.6 As built). Without the first, a
  running member's A3 resync whose S lies before the server's walk (a
  request in the session's first ticks) would drop the Init Ingame
  activation the server's state at S has not run yet - audit #16 again on a
  start member. Without the second, a join superseded by an exact re-assign
  (the base missed, `resync_member`) would compare a walk tick of the old
  numbering with the new window and drop the manager's other activations
  (a death's sector reset) at those ticks; the walk's own activation went
  with the re-assign's `forget_activations_from(base)`. `resync_join_` is
  also cleared wherever the pending resync ends otherwise (a full snapshot,
  a re-assign without the flag). Unit tests: a running member's resync with
  the walk inside its window, and a superseded join, drop nothing; a
  re-assign without the flag ends the join.
- **Protocol 10 inside the resync track.** The protocol number does not
  change: an A3/A4 core joining late would get the flag on its first
  assignment, ignore it, drop the state as stale and get no full snapshot.
  No such build was released (protocol 10 is the unreleased resync
  track's).
- **Gap found and fixed here (not in the design): a joiner's catch-up
  wakeups reached the server.**
  - Symptom (`a5_join_1`, the first build, snapshots off): the joiner equal
    to the members after the apply, but every world, the members included,
    off the server on `P_Ball_Paper_MF` and the `P_Modul_29` plates (26 mm)
    and `P_Ball_Wood_MF` (1.9 mm) from 1646-1652 on, the same bodies and
    the same maxima in each client.
  - Cause: the joiner's world before the apply is the level start plus its
    catch-up passes; their physicalizes wake bodies, and every Physics
    WakeUp of a non-ball body is reported as BodyRevived
    (client_core_frame.cpp post_step). The joiner sent BodyRevived for
    `P_Ball_Wood_MF` (1643), `P_Modul_41` (1644), `P_Ball_Paper_MF`,
    `P_Box_MF`, `P_Ball_Stone_MF` (1650), `Modul17_Dreharme`,
    `P_Modul_26_Sack`, `P_Modul_26_Rope` (1658); the server woke them at
    those ticks (its `S5 event ... applied=1643..1658`), where nothing in
    the shared world did, and no client world did.
  - Pre-existing: the pre-A5 `lp1_join` shows the same bodies off by the
    same maxima from 1646/1648; the join's full snapshot broadcast did not
    precede the wakes.
  - Fix: while a late joiner's exact resync is pending (`resync_join_`,
    set by the first assignment, cleared by the apply) its wakeups are not
    reported (`late join: wakeups before the apply are not reported`, the
    status line's `join_wakes_withheld=`). After it, the bodies are exact
    in every world (below). A3's pending window keeps reporting: a running
    client's own-ball wakes there can be the shared world's.
- **Checks** (quick tier; snapshots off, 150 ms, arena, 4 clients, captures;
  `build-agent-rA5`, the Arch server rebuilt from this tree; one run each).
  - **Late join** (`a5_join_2`, `--keep-lane --join-late 3:1500`): base
    1638, the walk seen at 1641, the owed passes live at 1647 and 1654, the
    state waited 8 frames, re-stepped 1637 -> 1659 (22 ticks, Δ -1637; 26
    rows, 0 unmapped, 40125 script bytes localized, 0 unportable, blob
    restored; unexplained and unattributed 0), one walk activation dropped,
    the two live passes forgotten; mechanisms 52/52 (lp1_join: 52/45).
    After B every map body is the server's (P_Modul_29, P_Ball_*_MF 0.000000
    m) through 1839, then only G-c: `P_Modul_19_Flaps` 1.001 mm from 1740
    and `P_Modul_03_*` from 1840 (the leader's wakes, the same in client 0,
    1.37 m), and the keep-local `PE_Box_slide`. The joiner's world hash
    equals the server's at 1770-1827 (58 ticks; before, its mirrors are
    predicted); client 0's never (its own `P_Modul_34` 1.8 mm from 1346).
    Clients 1 and 2 resynced once each on a host stall (`tick schedule
    rebased`).
  - **Last-sector variant** (`a5_joinlast_1`, `--join-late 3:1800`, after
    sector 4 at 1714): base 1936, walk 1939, passes 1945/1952/1959 live,
    waited 10 frames, re-stepped 1935 -> 1964 (29 ticks), three passes
    forgotten; from 1966 every `P_Modul` body within 0.061 mm of the server
    to the end (sector 4's included: the leader's wake precedes S). The
    balloon's 18 bodies, born in the window by the joiner's own sector-4
    walk, were destroyed at the restore and made again by the replayed
    balloon script (births 18/18 remade, missing 0) and exist to the end.
    Mechanisms 52/48: the four `PE_Balloon_Ballon*` are 21-23 m from the
    server's rows, past the registry's 20 m identity guard (exception (c)).
    The joiner's balloon sinks (Ballon01 y 6.7 -> -15) where the members'
    stays up (y 1-3): pre-existing (`lp1_join`'s joiner: -13; `a5_join_2`'s
    joiner, whose sector 4 came on time: -15), the balloon, left by the
    decision of 2026-10-24.
  - **10 % loss** (`a5_joinloss_1`): the assignment came at 1775 (after
    sector 4), taken with the flag; walk 1778, passes 1795/1802/1809,
    waited 10 frames, re-stepped 1774 -> 1814 (40 ticks), the same zero
    counts; after B every map body exact but `P_Modul_03_*` from 1840 (G-c,
    client 0 alike); hash equal at 1815-1827 and 1974-1978; 52/48 (the
    balloons, as above).
  - **A3's resync_snapoff** (`a5_resync_snapoff_1`): 4 resyncs, all `tick
    schedule rebased`, as in `core_ref_r3/resync_snapoff` (4); each S =
    base - 1, D 11-37, every count 0, scripts localized; after B
    `P_Modul_01/08/03` exact (client 1's `P_Modul_03` from 1972: G-c, as in
    the reference).
  - Unit tests 466/466 at the implement round, 468/468 after the review
    fix-up (new: `ALateJoinersFirstAssignmentTakesTheStatePath`,
    `TheWalkDropIsTheLateJoinersAlone`, `ActivationSlots.ForgetPending`,
    `ALateJoinersWakeupsBeforeTheApplyAreWithheld`, the S4 gate cases; A3's
    apply asserts no walk is dropped); GCC 16.2 26 files ok; x64 BUILD-OK,
    x86 CLIENT-BUILD-OK (client objects deleted: `client_core.hpp`,
    `client_platform.hpp` layout); wire check `rA4 rA5`: ALL SAME (24
    journals identical); solo snapshots off IDENTICAL to old1 (a first run
    lost to host stalls, rerun).
- **Review fix-up gate round** (core code changed: the drop's condition,
  `forget_walk_tick`, `resync_join_` clears): unit tests 468/468; GCC 26
  files ok; x64 BUILD-OK, x86 CLIENT-BUILD-OK (client objects deleted:
  `sector_union.hpp`'s `walk_opened`); the Arch server rebuilt; wire check
  `rA4 rA5` ALL SAME (24 journals identical); one late-join run
  (`a5_join_fix` sector 1, the `a5_join_2` recipe): base 1639, walk 1642,
  the drop as before (`1 manager activation(s) of 1639..1642 not
  re-issued`), re-stepped 1638 -> 1660 (22 ticks, 0 unportable,
  unexplained 0, unattributed 0), 8 wakes withheld, `walk_timeouts=0`
  everywhere; after the apply every map body equal to the server's through
  1839 but `P_Modul_19_Flaps` 1.001 mm from 1740, then `P_Modul_03_*` from
  1840 (G-c, client0 1.37 m), the balloon's bodies 21-22 m from 1996 (as in
  `a5_join_2`, 21.6-24.0 m; the harness's count 52/48 there against
  `a5_join_2`'s 52/52 is the 20 m guard at its measuring moment,
  exception (c)); the joiner's world hash equal to the server's at 1776
  and 1799-1827; the server's journal replays matched (0 checkpoint
  mismatches). The other arms were not rerun: the fix-up cannot reach
  them (the drop fires only on the joiner's resync, which they share with
  this arm; A3's never had the walk in its window).
- **Not run** (quick tier): the second and third runs of each arm, the
  regression arms, retail.

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
  `maps/arena-test/core_ref_e0_pre_q/`, and T0's numbers here are that
  set's: its a5u_lead journals match its README's sha256 (client0
  `db6722d1…`, client2 `83bb3777…`). `core_ref_e0/` was re-recorded
  afterwards (As built (step REF) under §7.5).
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

**As built (step A2, 2026-10-24).**

- **Where.** `BallanceMMOServer/sim/sim_tool.cpp` only: `--resync-check S B`
  (`run_resync_check` and its `rc_*` helpers). Nothing shared changed.
- **The harness.** One world (`physics_world`, the server's), one process.
  Every run starts from a ROOT: `--resync-root` whole world ticks (240)
  after player 1 joins, so the retail ball is parked and the retail blocks
  that retry every frame are queued (the savestate refuses a PreSimulate
  queue that differs from its own). A run from the root is put back the way
  a rollback restores, plus what a root needs: the scripts' v2 record with
  `replay_activation`; every body the previous run made destroyed (joint
  handles released, entity put back where its Physicalize found it: the
  binding's `destroy_body` + `release_joints`); the savestate; the world's
  counters; and, harness-only, every 3D entity's world matrix, visibility
  and parent, every data-array cell, and the restored-active scripts
  re-registered with the behaviour manager (finding G1 below). A warm-up
  run from the live root is compared with the straight run from the
  restored root at every boundary: **identical** in every scene but the
  pooled one (finding G2). Ticks on the command line count from the root;
  "S" is boundary S (the server's end-of-tick state of S-1).
- **The apply** follows `restore_and_replay`'s order: counters from the
  anchor with the server's union; the server's portable record localized
  against the anchor's v2 and restored with `replay_activation`; the
  anchor run's bodies of a pass the server's union lacks at S destroyed
  (before the blob, like the engine's births; `--resync-destroy-after-blob`
  is the A/B); the anchor's blob, lenient; the rows (wake/freeze as the row
  says, rechecked when they move the body or the blob did not know it);
  live bodies the blob did not know and no row covers rechecked at once;
  the anchor's recorded contacts re-made for the rowed bodies. The re-step
  is the server world's whole tick, S..B+600 (not the client's replay
  recipe: that recipe is `--savestate-check --frames`'s and the sector
  replay's, the union re-derivation B1's).
- **Adaptations.**
  - **A row the body already has, bit for bit, in a body the blob knows,
    is not written** (`--resync-write-equal-rows` is the A/B). The write is
    a beam, and a beam loses what no row carries: the hull manager's
    gradient and values, a sleeping core's PSI time stamp and delta, the
    entity matrix rebuilt from the quaternion (low bits). Measured, arm E on
    Level 11 with every row written: 0 of 630 ticks exact, Pusher001
    0.24 mm, a stone prop 4.3 cm; with the rule: 630 of 630. **A3's
    `resync()` takes this rule** (the "every row is authoritative" of §4.1
    then means "every row that differs"). Consequence: **arm E writes no
    row at all** (16 or 33 rows "equal"), so arm E checks the blob, the
    scripts, the counters and the reconcile, not `set_body`; the row write
    is exercised by arms R and S (and `--resync-write-equal-rows`). That a
    row write is not a no-op is itself a gap of the shared binding (finding
    G3 below).
  - **Arm R is not the arm the procedure above words.** The procedure's
    anchor is "a perturbed run's tick S (scripts of S-17 restored at S-17
    and stepped)". Built as worded (`--resync-perturb-scripts`: the
    selected scripts captured and restored with `replay_activation` at
    S-17, no body touched) the anchor does not diverge: Level 11 and the
    arena both 630 of 630 blobs exact, gate 0 - it is arm E again and
    measures nothing about an anchor that differs. The arm as built moves
    the anchor instead: at S-17 every movable body is moved by M in x and z
    and the awake ones are sped up by 5M m/s (`--resync-perturb M`, 1 cm by
    default), then stepped to S, so the anchor's contacts, mindists, queues
    and calm references at S are its own. M is the arm's free parameter;
    it was swept (1, 5 and 20 cm). Arm R's failures below are this
    substitute's; Q1's decision stands on arm S alone.
  - Arms S and A shift the stamp of sector K by 9 ticks; the tool checks
    that the case holds (S: K born in the straight run before S and in the
    arm's after it; A: K passed by the server after S and born in the arm's
    before it). Arena K = 4 (P_Modul_08, P_Modul_03, P_Modul_19, the level
    end); Level 11 K = 5 (two P_Modul_08), and K = 2 for the pooled case.
  - The level end's bodies (`PE_`: the balloon, its airship, its slide) are
    reported and not judged (I3); the whole-frame re-step does not re-issue
    their keep-local activations (§2.8), so arm S's balloon falls apart
    (its joints are reset by the anchor's keep-local record, its bodies
    unknown to the blob).
  - Lower-case arms `r`, `s`, `a` are R, S and A applied with the **server's
    blob of S** instead of the anchor's: the Q1 alternative, measured in
    the same runs. With `--resync-server-blob FILE` they restore another
    process's blob instead (the cross-world restore test below).
  - The verdict is strict about its own inputs: a gate body never compared
    fails its arm; a `--resync-watch` name that is not a movable body, arm
    R's perturbation at or before the root, and A before R in
    `--resync-arms` (A is judged against R's band) refuse the run. The
    harness line (warm-up vs restored root) is informational: every arm and
    the straight run start from the same restored root.
  - A body name two bodies share gets a `#n` suffix in the pose capture by
    listing order; in arm S that order differs between worlds, so a shared
    name could swap suffixes. These scenes have none; Level 8 (two
    `P_Modul_30_Wippe`) must be judged by entity id, not suffix.
- **Results** (gate: Pusher001, both sacks, and K's movable bodies over
  (B, B+600], 1 mm; depth B-S = 30):

  | scene | arm | gate worst | other worst | blobs exact |
  |---|---|---|---|---|
  | L11 s1, ball on Pusher001, S=150 | E | 0 | 0 | 630/630 |
  | | R 1 cm | 0.038 mm (Pusher001) | 3.1 mm (P_Modul_25_Bridge001) | 0 |
  | | R 5 cm | **8.4 mm** (Pusher001) | 18.7 mm (ball) | 0 |
  | | R 20 cm | **7.8 mm** (Pusher001) | 68.9 mm (ball) | 0 |
  | | r (any) | 0 | 0 | 630/630 |
  | L11 K=5 late (S=30) | S | 0.0025 mm (Schaukel) | 0 | 0 |
  | | s | 0 | 0 | 630/630 |
  | L11 K=5 early (S=38) | E / R 1 cm | 0 / 0.25 mm (Pusher001) | 0 / 0.49 mm | 630 / 0 |
  | | A | K bit-exact | 0 | 0 (order bookkeeping) |
  | | a | K bit-exact | 0 | 630/630 |
  | arena, ball on the Schaukel, S=150 | E | 0 | 0 | 630/630 |
  | | R 1 / 5 / 20 cm | 0.002 / 0.009 / **133 mm** (Schaukel) | 0.02 / 0.3 / 6893 mm (ball) | 0 |
  | | r | 0 | 0 | 630/630 |
  | arena K=4 late, ball dropped on its see-saw (S=30) | S | **14.4 mm** (Schaukel) | 11.6 mm (ball) | 0 |
  | | s | 0 | 0 | 0 (the level end only) |
  | arena K=4 early (S=38) | E, R | 0, 0.00007 mm | 0, 0.0002 mm | 630, 0 |
  | | A, a | K bit-exact | 0 | 0, 630 |
  | L11 K=2 (pooled with s1) early | E | 0 | **19.9 mm** (P_Modul_25_Bridge001) | 0 |
  | | A / a | K bit-exact | 30.5 / 46.3 mm (P_Modul_25_Bridge) | 0 |

  Constraints of K after arm S's apply: every non-level-end body's named
  controllers equal the server's (arena 12 of 12, Level 11 2 of 2).
- **Q1, decided: rows-only is not exact enough; the server's IVP savestate
  travels** (the user's standing default, roadmap §2). Arm S fails the
  gate on the arena (14.4 mm), arm R (the substitute above) from a 5 cm
  anchor error on (Level 11 8.4 mm, the arena 133 mm at 20 cm); the
  server's blob is exact in every non-pooled scene, the local level-end
  bodies aside (the pooled L11 K=2 case is G2: `a` 46.3 mm; the arena's
  `s` has 0 of 630 identical blobs because of the level end alone, every
  judged body 0). Causes:
  - **Arm S.** A body the anchor's blob does not know enters IVP by its
    live insertion plus the apply's recheck: its place in
    `m_MovableObjects` (`[Schaukel, Ball]` against the server's
    `[Ball, Schaukel]`), the simulation units' numbering (A_Floor_001 unit
    67 against 55) and its collision pairs (made at S by the recheck, not
    at its birth: the falling ball's `current_speed` and hull gradient
    rebuilt to 0) are the anchor's, so IVP processes the same bodies in
    another order: 2.5 µm with no contact (Level 11), 14.4 mm once the
    ball lands on the see-saw (arena).
  - **Arm R.** A row carries pose, velocity and sleep. After a rows-only
    apply from a 5 cm anchor the bodies' poses equal the server's, and
    still 403 collision records (mindist lengths, recalc stamps), 33
    contact points (Pusher001-Rinne001, ball-Pusher001), 11 friction
    systems, 30 hull managers and 16 cores (calm references, PSI fields)
    differ (`--savestate-trace`, the applied world against the server's):
    the anchor's own contact state at its own poses.
  - **The measurement of the alternative** (A3 builds it):
    - size of the server's blob at S: arena s1+s4 with one ball 101,171
      bytes (zlib 8,473), Level 11 s1 with one ball 146,296 (zlib 28,000),
      Level 8 s1 68,751 (zlib 5,434);
    - **the cross-world restore test (x64, two processes).** Process 1
      joins as player 1 and writes its server blob of S
      (`--savestate-trace P` -> `P.server.blob`); process 2 runs the same
      scene as player 2 (its own clone `Ball_Stone_BMMO_2`, as a client's
      own ball is spelled otherwise than the server's clone) and restores
      that file in the `r`/`s` arms (`--resync-server-blob`). Scenes: L11
      ball on Pusher001 (S=150, arm r) and the arena K=4 late (S=30,
      arm s). Results:
      - **as it is, the blob is refused**: "the savestate was captured in
        another environment". The blob's header carries the capturing
        `IVP_Environment` pointer (`world_record::environment_id`,
        physics_savestate_capture.cpp `out.environment_id`), and the
        restore's first precondition compares it with the live one
        (physics_savestate_restore.cpp `restorer::run`). The blob was
        designed never to leave its process. With the blob refused the
        apply leaves the anchor's live IVP state under the rows: L11
        (anchor perturbed 1 cm) 45 mm (sack), arena 20.5 mm (Schaukel). **A3 needs a restore flag that waives the environment
        identity for a travelling blob** (shared savestate code);
      - past the identity (`--resync-blob-adopt-environment` writes process
        2's identity into the header, what that flag would do) and with
        the one rename `Ball_Stone_BMMO_1 -> Ball_Stone_BMMO_2`
        (`--resync-blob-rename`, one occurrence): the renamed foreign blob
        is **byte-identical** to process 2's own server blob of S (146,296
        and 101,171 bytes), and the arms are exact: L11 `r` 630 of 630,
        gate 0; arena `s` gate 0 and rest 0 (the level end's 58 skipped
        records as in-process);
      - without the rename: the own ball's record is not found, it stays
        the anchor's ("unknown", its records skipped: 55 on L11, 107 on the
        arena): L11 5.9 mm (Pusher001), arena 1.9 mm (Schaukel) - fails;
      - so the names: exactly one name maps, the resynced member's own
        clone to its own ball; every other body, the mirrors' clones
        included, has the same name in both worlds. The entity ids agreed
        between the two processes (same load order); on a retail client
        they will not, and they matter only for a name several bodies share
        (`live_world::find_by_name`): 0 in these scenes, so a retail blob
        must not rely on them for Level 8's two `P_Modul_30_Wippe` (by
        then unique names from the sector placement's copies, or a mapping
        by occurrence order);
    - format: fixed-width little-endian fields; an edge is named by its
      byte offset in the compact surface, whose layout is int-based and
      16-byte aligned (`ivp_compact_ledge.hxx`), so x64 and x86 agree if
      their surface builders build the same surfaces (the world hash's
      surface field says so at every handshake). **Not executed**: the x86
      SimTool tree (`build-sim-x86`) was configured with a Visual Studio
      2022 toolchain that is no longer installed and the x86 client tree
      has no SimTool target, so the x64-server vs x86-retail blob test is
      **open and is A3's first gate**;
    - what a travelling blob cannot carry: the keep-local bodies (the
      balloon: arm `s` skipped 58 records, its constraints, because the
      anchor's keep-local record had reset them) and the client-only ones
      (trafo pieces) - A3 restores with KEEP_UNKNOWN and keeps the anchor's
      own records of the keep-local bodies.
- **Findings, queued as their own items** (pre-existing, root-caused):
  - **G1. A restored-active script that finished on its own never runs in
    the live frames again.** `script_replay::restore` sets
    `CKBEHAVIOR_ACTIVE` with `ModifyFlags` (script_replay.hpp
    `restore_behavior`), but a script whose graph finished cleared its
    scene descriptor's `CK_SCENEOBJECT_ACTIVE` (CK2 `CKBehavior.cpp`
    `CheckBehaviorActivity`) and an owner with no active script left the
    behaviour manager's object list (`CKBeObject::ExecuteBehaviors` ->
    `RemoveObjectNextFrame`); only a scene activation adds it back
    (`CKScene::ActivateScript` -> `AddObjectNextFrame`). A replay's
    `execute_frame` runs it (it calls `Execute` directly); the first live
    frame after the replay does not. Seen at the harness's root restore:
    Level 11's `P_Modul_01_MF Script` (both copies) finished in the
    warm-up run; restored active, its proximity's Last Check stayed 8 and
    the far pusher never woke (73 m on a stone prop at the end). Any
    rollback whose corrected history keeps alive a script the live run had
    finished has it. Fix: the restore re-registers what it activates
    (shared code, both hosts).
  - **G2. A pass of a pooled sector inside the window** (#9; the sector
    placement design): on Level 11 the pass of sector 2 re-places sector
    1's `P_Modul_25`/`P_Modul_37` bridges (moved frames, rebuilt hinges),
    which neither the blob nor the scripts put back: arm E 19.9 mm, A
    30.5 mm, a 46.3 mm, the harness's own root 48.6 m. Not a resync
    defect; closed by the sector placement step.
  - **G3. A row write is not a no-op.** The binding's `set_body` (the
    physics_RT body write behind `rollback_world::set_body`) is a beam:
    writing a body's own pose, velocity and sleep back onto it rebuilds
    the hull manager's gradient and values, resets a sleeping core's PSI
    time stamp and delta, and rebuilds the entity matrix from the
    quaternion (low bits). Measured: arm E on Level 11 with every row
    written (`--resync-write-equal-rows`), 0 of 630 ticks exact, Pusher001
    0.24 mm, a stone prop 4.3 cm. The same write serves every snapshot
    correction of the live rollback, not only the resync, so a correction
    that agrees with the client still perturbs it. The equal-row skip is
    the resync's workaround; the fix (a write that leaves what the row
    does not change) is its own item, shared code.

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

- every client logs `sector +4 at tick 1713` (the server's tick in the REF
  set; 1714 was the E0 set's);
- clients 0, 1 and 3 each log one Sector rewind, and none of them sends an
  own Sector 4 before its own crossing (1798/1807 in the reference);
- `first_divergence.py server.bmjr client*.bmjr --prefix P_ --exclude
  PE_Balloon --from 1713` gives no sector-4 body beyond the stamping client
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

**As built (step A4, 2026-10-24): the full-tier gate of the exact resync.**

- **Binaries.** New = HEAD 93c13f6 (`build-agent-rA4`), and for the retail
  runs, Q2 and the re-captured set `build-agent-rA4f` / the x86 mod of the
  same tree with the one code change below. Old = `build-agent-rB1`.
  Protocol 10 refuses protocol-9 peers, so each old run was served by a
  protocol-9 Arch server built from 64be32e in its own tree
  (`~/bmmo-build-arch-b1`, id `...+bmmo-64be32ec068d`) and each new run by
  the Arch build of HEAD (`...+bmmo-93c13f603ffe`); the B1 Arch binary had
  been overwritten at A3.
- **The one code change (a test path).** `force_rewind` passes a deeper
  `reach` to `rewind_to` when the depth is above `max_resim_ticks` (bounded
  by the resync reach), so `session force-rollback E:80` re-steps 80 ticks
  as a resync does (client_core_frame.cpp `force_rewind`). Before it a
  forced rewind deeper than 48 was refused as too far, and Q2 had no verb.
  A depth up to 48 is the rewind it was: `wire_check.sh rA4 rA4f`
  WIRE-CHECK: ALL SAME on the 16 protocol-10 captures. No live run but Q2's
  forces a rewind, so the A/B batches (run with rA4) stand. The headless
  `--force-rollback E:D` goes through the same function, so a D of 49-95
  now re-steps there too (it was a counted miss); no reference recipe
  forces deeper than 24. A depth whose anchor has left the history (96 and
  more) is still a counted miss (the engine's `unmatched`). Unit test
  `SessionClientCoreForcedRewind.DeeperThanMaxResimReStepsUpToTheResyncReach`
  (80 ticks re-stepped, 96 missed). §7.6's forced-vs-unforced solo server
  comparison was not rerun: A3 ran it, and the forced depths it uses (≤ 48)
  are unchanged.
- **Adapted: the r2_x_t1_b recipe.** Its resync came from own frames late
  past the reach at ~435 in R2's run, and `a4_r2x_1` (the recipe as
  written) had no resync. The recipe never produced that trigger: R2's run
  (`maps/arena-test/r2_x_t1_b/server.log`) shows a one-off pause of about
  0.6 s shared by both clients and the server at tick 435 - the server held
  BOTH players' frames 435..443 at once from source 434 (`S2 held`), its
  relay of a BodyRevived stamped 435 took 621478 µs (41.4 ticks), and both
  clients got the held notice for 435 at 496/497 (each client's window of
  480 relayed 35 frames instead of ~60). Net lag 150 alone delays every
  frame by the same amount and never pushes one past the reach, so no
  build change made the trigger go away (B1 and A3 included). The recipe keeps net lag 150 both ways on
  both clients and the key record, and adds `net blackout 900 both at 430`
  on both (500 ms up gave no resync), which reproduces that mechanism on
  purpose (`a4_r2x_b1` server: both players held 431..480 from source 430):
  both clients resync with "a
  substituted own frame arrived past the reach" at 502-503, S 507-510, the
  pusher dropping at 512 (the original's S 496, drop 508). The run scripts
  were rebuilt from `retail_two_client_test.py` and the R1/R2/B1 notes
  (`.local-tools/work/a4/a4_retail.py`; `r0_keys.bmrc` re-made from
  `r0_keys_p0.bmrc`, identical keys, level 11 - the R2 journal's own key
  edges match frame for frame); client LOCAL checkpoints every 2 ticks
  (`BMMO_JOURNAL_CHECKPOINT_TICKS=2`).
- **Headless arms** (arena, 4 clients, snapshots off unless named; every
  server journal replays exactly in the harness; cpu sampler max total
  18-27 %, worst outside process 3.8 %):
  - resync_snapoff O1 O2 / N1 N2 N3: resyncs per run old 4, 4; new 4, 1, 4.
    Every new apply: S = base - 1, D 11-38, unportable, unmapped, refused,
    unexplained, unattributed, deferred 0; P_Modul_08, P_Modul_01 and
    P_Modul_03 within 0.000003 m of the server from B+1 to the end in every
    resynced client (old, legacy full snapshot: P_Modul_08 4.7-6.1 m, the
    sack 4.8-6.9 m); every member's snapshot count unchanged by a resync.
  - running-client resync, 0 ms ×3 (Δ -7, D 15-16) and 150 ms ×3 (Δ -5/-6,
    D 33-34), plus the Δ > 0 arm ×3 (Δ +14, +15, +14; resent own frames
    14-15): every apply clean as above; client 0's passes at the server's
    ticks, none twice; phase_misses 0; the modules exact after B but
    P_Modul_03 (G-c, below).
  - a5u_lead O1 O2 / N1 N2 N3: sector +4 at 1713 everywhere, one Sector
    rewind on c0, c1 and c3, own Sector 4 stamps 1789-1808 after their
    crossings, phase_misses 0. Client 2 resynced 1, 3, 3 times (old 3, 2),
    D 49-53; between consecutive resyncs and after the last its modules are
    within 0.000002 m. One apply of 7 has `unmapped 1` (below). Criterion
    3 (`.local-tools/work/a4/sector_bodies*.py`, the bodies the server
    first records at or after 1713, PE_Balloon excluded, max from 1713):
    clients 0, 1 and 3 in N1-N3 and O1-O2 alike have P_Modul_03_Gate
    1.360-1.366 m (G-c), P_Modul_19_Flaps 1.794 mm at 1736 settling to
    0.804 mm from 1744 (G-c: the leader's BodyRevived at 1728, as B1's
    note found), PE_Box_slide 0.396 m at 2132 (keep-local: jointed to the
    balloon platform, `resync_rules.hpp`), P_Modul_08_Schaukel 0; client 2
    has Flaps 0 and Schaukel 6.6-6.9 m only before a resync (11.1-12.0 m
    old). So, as at B1, the criterion holds against client 2's own
    maximum and fails per body on G-c alone (roadmap §4 item 7).
  - Regression lane / swing (10 % loss) / a6m, O1 N1 O2 N2: no new resync
    (lane: old 1-2 per client, new 0-1; swing, a6m 0), `sector_rewinds` 0
    in a6m and swing, max_err new 0.08-5.0 m against old 11.4-24.3 m in the
    lane, swing and a6m inside the old band but for t_max in swing N2 (c1
    77 ms against 56-57, c3 70 against 55-65). Cause
    (`.local-tools/work/a4/swing_tmax.py`): in every swing run, old and
    new, the t_max of clients 1-3 is one rewind in the ticks 2145-2200
    just after the sector-3 pass, where the replay builds the new sector's
    mechanisms and costs 1.76-2.06 ms per re-simulated tick (old
    1.85-2.02, new 1.76-2.06: equal), so t_max ≈ that rewind's depth ×
    ~1.85 ms. In N2 that rewind was 42 deep on c1 (2174, 2175) and 43 on
    c3 (2174): player 3183853383's relayed frames reached them up to 41-42
    ticks late in the windows of 2200/2220, against 31-36 in O1, O2 and
    N1 - a loss burst of the 10 % loss arm (the lag ladder 27..41 of one
    recovered run of frames), not a code change. The cpu sampler over that
    run shows 8-14 % total and no outside process above 3 %; the old runs'
    own deepest rewinds elsewhere reach 43-48.
  - The resync state's emit time: 54 states, 3.9-5.6 ms (median 5.1), 29-52
    KB on the wire; `resync_base_missed` never logged.
- **CPU.** No capture replays on both sides of protocol 10, so parity is
  judged by the live and the server numbers below; `cpu_parity.sh rA3
  rA4` on core_ref_r3_a3 resync_snapoff (PASS, 22.76/22.90 s against
  22.98/22.89 s) compares two builds of the same commit 93c13f6 and is
  only a sanity check of the tool on the protocol-10 set. Live client
  cost per re-simulated tick old vs new lane 1.289/1.201, swing
  1.067/1.044, a6m 1.680/1.692, resync 1.203/1.157, a5u 1.356/1.287 ms,
  capture per tick 0.816-0.856 both; the server per step (SimTool replay of
  each arm's server journals by its own build, pinned, two rounds) lane
  288/287, swing 312/309, a6m 293/298 µs per tick (+1.7 %, within 5 %).
- **Retail (Level 11, two clients, core on, the HEAD Arch server).**
  - r1_c_on1 (r1_c2.py's second form: net lag 150, beams 4 m up and 30 m
    down, `session stall 1000` on client 2), snapshots on ×3 and off ×1:
    client 2 resyncs "tick driver rebased" with Δ -45/-50, D 11-12;
    `a4_r1c_on3` also resynced both clients for "a substituted own frame
    arrived past the reach" (Δ -5, D 47 and 54). Every apply unportable 0
    and unmapped 0 (retail CK2 against the server's engine: the x86 mod
    restores the x64 server's blob); the sack and rope within 0.000003 m of
    the server from B+1 to the end (R1's legacy: 4.48 m); 0 correction
    records name the sack after B; holds 0; no rebase but the stall's.
    Client 1 (no resync where it had none) exact on every module.
  - r2_x_t1_b (adapted, above) ×3, snapshots off: both clients resync, Δ
    +15..+17, D 31-34, unportable and unmapped 0; client 1's Pusher001
    0.000000 m from B+1 to the end; the sack and rope within 0.000003 m
    (one 0.08 m at the `journal mark` tick, stamped a tick early - R2's
    known mark artifact); client 2's Pusher001 0.78-0.83 mm at B+1,
    decaying to bit-exact by B+30 (below).
  - B1 check (`a4_b1_1`): client 1 `net lag 300 up`, `sector 2` stamped
    382; client 2 logs `sector +2 at tick 382 ... re-derived in a rewind at
    live tick 393`, the server's 382; the session has one Sector event,
    client 1's.
  - Every retail server journal replays exactly in the SimTool (2435-2534
    ticks, 0 checkpoint mismatches).
- **Residuals, pre-existing, root-caused, each its own queued item.**
  - **The own row unmapped (2 of 81 applies).** a5u_lead N2 c2's third
    apply (S 3260): its own ball unphysicalized at 3286, after S and before
    the apply, so it has no body when the state comes and its row is not
    mapped (client_core_resync.cpp:203-205); the re-step lacks the own ball
    for (S, 3286]. That is gap L, a life that ends inside the window
    (rollback-engine-gaps-design.md §2.3, roadmap item 8); the same build
    before A3 shows it in every rewind across an own death (`a4_a5u_o1` c2:
    "object 'Ball_Stone' is in the savestate but not in the world" anchor
    2274, death 2302). core_ref_r3/swing_snapon c3 (S 3249): its own
    Unphysicalize stamped 3227 was applied by the server at 3250 and kept
    at 3227 here ("only an own Physicalize is moved here") - late-player
    plan step 22 (own death move, roadmap stage B). In both the modules are
    within 0.000001 m after B.
  - **G-c** (world-agreement design): P_Modul_03 1.36 m in every non-leader
    client, resynced or not, old and new alike.
  - **Retail own echoes (R4):** client 2's own BodyRevived stamped 521 was
    applied by the server at 525 and kept at 521 (the retail core runs
    `own_echoes` off), so client 2 woke Pusher001 four ticks early: the
    0.8 mm above, gone when the pusher rests.
  - **N2 exists only in the retail mod (new finding).**
    a4_resync_n2 client 1 (no resync): its own crossing of the sector-4
    checkpoint fell in the frame of the union's pass (pass 1786, own stamp
    1822 = 1786 + 1 + 35). The held chain's Execute Script is neutralized
    (sector_activation.hpp), but its two Set Cells still write
    IngameParameter[0][1] = 4 and [0][2] = 3 in that frame, before the
    manager, activated by `run_sector` with 4 / 0 (session_client.cpp
    `start_sector`), reads them at its Start (Get Cell [0][1], Get Cell
    [0][2]): the manager deactivated sector 3 in this world only. The
    [0][1] write was harmless here (4, what `run_sector` wrote); the [0][2]
    write is the cause. The retail mod has pinned exactly that Set Cell's
    Value to a session constant 0 since design 9.25 (N2,
    physics_session_client.cpp:1013-1167 `sector_deactivate_resolve` /
    `_apply` / `_restore`, applied on core-on retail too, core_host.cpp:870
    and :1420), so retail, legacy and core-on, is not exposed; the headless
    client (session_client.cpp:1605-1629 holds the chain and splits the
    phase cell, never pins [0][2]) and the server's world
    (physics_world.cpp:270-295, the same) lack it, and the server's world
    can hit the same race when a clone crosses a checkpoint in the frame of
    an `update_sectors` pass (not observed at A4). The replay re-issues the pass with the recorded cells 4 / 0 and
    never runs the own chain, so a rewind across the pass (1812 -> 1769)
    found the live world without 15 sector-3 bodies (`skipped=15`), the
    re-executed scripts differed from the live record (`Deactivate Sector
    3 vs 0`, P_Modul_26_MF Script flags), and P_Modul_26_Sack came back
    without its rope joint and fell (8498 m at the end). Only a live pass
    whose frame also sees the own crossing races (own stamp = pass + 1 +
    lead; 1 of 148 headless client-runs at A4; crossings one frame before
    or after are safe, and so is a pass made inside a rewind, as in
    core_ref_r3/resync_snapoff client 1). Reproduced before A3/B1:
    `r4_lane_loss10` client 1 (build 8042b03, 2026-10-21), the same diff
    signature and the sack at 7439 m. Queued as its own step (roadmap §4
    item 7, "N2 on every world"): N2's rewire moves into a shared `game/`
    header next to `sector_activation.hpp`, the retail mod's copy becomes a
    use of it (one implementation), and the headless client and the
    server's world apply it at their anchor. The [0][1] Set Cell must keep
    working: the own sector poll reads that cell (`poll_sector`, and the
    retail mod's N2 leaves it for the same reason). The same step checks
    the one case [0][1] could still matter, an own crossing of sector K in
    the frame of a pass of another sector J (the chain's K over
    `run_sector`'s J), on every world.
- **Q2, Q3, Q7, Q9 settled** (§9): cap 95; no wait; no hold; Δ -50..+17.
- **The reference set re-captured** as `core_ref_r3/` (§7.5, "As built
  (step A4)"); A3's four captures moved to `core_ref_r3_a3/`.

### 7.5 The 16 reference captures (`maps/arena-test/core_ref_e0/`)

Replay the captures with the B1 binary, twice. The expected results, with
the evidence for each in §1.1 and §1.2:

| capture | B1 result | why |
|---|---|---|
| swing_snapon/client1-3 | byte-identical | no late stamp, no resync (logs) |
| swing_snapon/client0 | byte-identical | no late stamp; its one resync (own beam pair moved 701 -> 704, at 724) is A3's business |
| a6m_snapoff/client0-3 | byte-identical | no late stamp, no resync; c2's moved own Physicalize is not a Sector event |
| resync_snapoff/client3 | byte-identical | its late stamp for sector 4 at 1813 is redundant (1800 already); its resync is A3's business |
| resync_snapoff/client0, client1, client2 | byte-identical | no late stamp; resync only |
| a5u_lead/client2 | byte-identical | the stamping client; its late stamp for sector 4 at 1798 is redundant (1713 already); its two resyncs are A3's |
| **a5u_lead/client0, client1, client3** | **change** | sector 4 at 1713 instead of 1722: one Sector rewind; the TICK records differ from 1722 on (those of 1713-1721 were written live before the stamp arrived; as built, step B1) |

(Re-derived at step REF against the re-recorded set; the E0 set's version
of this table had sector 4 at 1714/1723, the redundant resync_snapoff stamps
on c0, c1 and c3 against 1810, and no resync in swing_snapon.)

A capture with a MOVED own Sector echo would also change; the 16 have none
(no `own Sector ... moved` line in their logs), which T0 confirms with a
grep (the REF set too).

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
- In the REF set (below) the same journals resync (resync_snapoff: c2 at
  1500, c1 at 1815, c0 at 1833, c3 at 1835; a5u_lead/client2 at 2655 and
  3083), and swing_snapon/client0 resyncs once (an own-birth resync at 724),
  so A3 would change those six.

**As built (step REF, 2026-10-24): the reference set was re-recorded.**

- **Why.** Steps Q, RR and LB changed what a replay of the E0 captures
  produces, so they no longer matched any current binary (the session-core
  design's As built (step Q) note). The E0 set is kept unchanged in
  `maps/arena-test/core_ref_e0_pre_q/`. **Every citation of `core_ref_e0/`
  numbers dated before 2026-10-24 refers to that set** (here §1.1, §1.2 and
  T0 now say so; the audits' too).
- **What.** `maps/arena-test/core_ref_e0/` holds a fresh set recorded with
  HEAD 765267c (A2): the four 4-client arms (swing_snapon, a6m_snapoff,
  resync_snapoff, a5u_lead) with the E0 README's harness arguments,
  `--capture-inbound` and `--trace-sends` on every client, under the cpu
  sampler; the 4 server journals; the solo snapshots-off baseline `old1`
  and its twin `old2`; `pins.json` (58 files, identical to the E0 pins).
  `cov_e9/` stays (rollback-engine-gaps cites it).
- **Binaries.** Client: the slim copy `build-agent-rA2` (A2's code; its
  journal header carries the stale engine id
  `ballanced-3e4289e95f25+bmmo-bf3fa76b43d4`). Server: the Arch WSL build of
  765267c, `ballanced-3e4289e95f25+bmmo-765267c59e28-dirty`, map.hpp
  unmodified; the Arch GCC syntax check passed 25/25.
- **Verified.** old1 and old2 `RESULT: IDENTICAL` (2406 common ticks, 0
  differ: the determinism precondition). The 16 captures replay to journals
  byte-identical to the live ones except the build id
  (`BYTE-IDENTICAL-EXCEPT-BUILD-ID: 16 pairs`); `wire_check.sh - rA2`
  compares the replays' sends with the live send traces: 16/16 SAME, 16/16
  journals IDENTICAL, `WIRE-CHECK: ALL SAME`. The 4 server journals and the
  two solo server journals replay in SimTool with 0 checkpoint mismatches
  (swing 3912/3912, a6m 3985/3985, resync 4160/4160, a5u 3904/3904 ticks).
- **The facts a gate judges against** (the logs; the README has the table):
  - swing_snapon: sectors +4 62, +2 1671, +3 2117 everywhere, no late
    stamp. **New:** c0's own beam pair (stamped 701) reached the server late
    at 10 % loss and was applied at 704; a moved own pair resyncs by design
    (`client_core_lifecycle.cpp:1226`, `:1244`, plan step 22), at 724 from
    the full snapshot of 725. The harness verdict is FAIL for that resync
    (the E0 run drew no moved pair); the capture is exact and adds resync
    coverage to the snapshots-on arm.
  - a6m_snapoff: sectors 784, 1230, 1673 everywhere; c2's held Physicalize
    moved 2482 -> 2501; no resync; verdict PASS.
  - resync_snapoff: sector 4 at 1800 on the server and all four clients (E0:
    1810); only c3 logs a late, redundant stamp (1813); one rebase and one
    resync per client (above).
  - a5u_lead: the server and c2 start sector 4 at 1713; c0, c1 and c3 get
    the stamp late and start it at 1722 (E0: 1714 / 1723): the same 9-tick
    phase shift §1.2 describes. c2: lead +15 acknowledged at 1784, two
    resyncs, three rebases.
  - First rewinds anchored at 220 (a6m) or 230 (the others) on every
    client, as in the E0 set; the moved-own-Sector grep finds 0.
- **The tools** live in `.local-tools/` (untracked): `e0_run.sh` and
  `e0_solo.sh` default to `build-agent-rA2`; `wire_check.sh` points at
  `core_ref_e0` again, creates `<label>/replay/` itself (a fresh set has
  none), and its `-` form without OLDDIR compares against the live
  `client<i>.send`; `e0_run.sh` refuses an empty label and an existing
  capture unless `FORCE=1`. The set's README (`core_ref_e0/README.md`, also
  untracked, as the data is) has the commands, the per-capture facts and
  the 18 journals' sha256 (e.g. a5u_lead/client0 `d529ad28…`).

**As built (step A4, 2026-10-24): `core_ref_r3/`, the protocol-10 set.**

- **What.** The four REF arms (swing_snapon, a6m_snapoff, resync_snapoff,
  a5u_lead) and the two running-client arms (resync_at150 and the Δ > 0
  arm resync_at150_down), each 4 clients with `--capture-inbound` and
  `--trace-sends`, under the cpu sampler, plus the solo snapshots-off
  baseline `old1` / `old2`; recorded with `build-agent-rA4f` against the
  Arch build of 93c13f6. A3's four captures (rA3) are kept unchanged in
  `core_ref_r3_a3/`, which A3's note cites. The set's README (untracked,
  with the data) has the commands, the per-capture facts and the 26
  journals' sha256 (e.g. a5u_lead/client0 `f6c9b618…`).
- **Verified.** The 24 captures replay `BYTE-IDENTICAL-EXCEPT-BUILD-ID: 24
  pairs`; `wire_check.sh - rA4f`: 24/24 sends SAME against the live traces,
  24/24 journals IDENTICAL, WIRE-CHECK: ALL SAME. The six server journals
  replay exactly (4004, 4204, 4178, 4193, 4058, 4182 ticks). `old1` against
  `old2` and against A3's solo: RESULT: IDENTICAL.
- **The facts a gate judges against.** Sector starts: swing +4 62, +2 1702,
  +3 2143; a6m 784 / 1230 / 1673; resync_snapoff 824 / 1270 / 1786;
  a5u_lead 824 / 1270 / 1713; resync_at150 824 / 1270 / 1713;
  resync_at150_down 824 / 1417 / 1917. Resyncs (S -> B, Δ): swing c3
  3249 -> 3308 (-20, the own row unmapped: plan step 22); resync_snapoff c2
  1571 -> 1583 (-50), c3 1884, c1 2226, c0 2229 (-17/-18); a5u_lead c2
  2733 -> 2786 and 3232 -> 3285 (-37, -38); resync_at150 c0 886 -> 920
  (-5); resync_at150_down c0 884 -> 916 (+16) and 1124 (-18), c1-c3 at
  1092-1121 (-19..-22).
- **The tools.** `.local-tools/wire_check.sh` and `cpu_parity.sh` default to
  `core_ref_r3` (the e0 copies: `.local-tools/work/a4/*.e0.bak`); the
  capture scripts are `.local-tools/work/a4/r3_run.sh`, `r3_solo.sh` and
  `r3_batch.sh`.

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
| T0 | Tools | `scripts/first_divergence.py`, `scripts/worst_bodies.py` (`--from`, `--exclude`); after step P lands | both run on core_ref_e0_pre_q/a5u_lead (then core_ref_e0/a5u_lead) and give today's numbers with the options off; the moved-own-Sector grep of §7.5 |
| B1 | Union as rollback state; late Sector rewind; moved own Sector | `sector_union.hpp`; core (stamp log, `union_records_`, `pass_log_`, §2.7, §2.8, `last_rewind_rederived`, the cell re-read); `client_platform.hpp` (`set_union_pass`, `sector_cell_adopted`); `session_host_support.hpp` (two slots, `set_pass`, `forget_pass`, guard adopt mode); H and RC ops; `sim_tool.cpp --late-sector-check` | §7.1 union, core and host-support tests; §7.2 `--late-sector-check` exact; §7.5 B1 table; §7.3 a5u_lead ×3; regression arms in band; retail B1 check |
| A1 | Portable script record | `script_replay.hpp` (`capture_portable`, `localize`, registry entries by name); keep-local scripts in `session_script_activations`; `sim_tool.cpp --portable-scripts-check` | §7.2 on L1-L12 and the arena, bytes identical, the unportable and registry lists explained; L11 size logged; the balloon activation check (Q8 settled) |
| A2 | Resync fidelity offline | `sim_tool.cpp --resync-check` | arm E exact; arm A as specified; arms R and S ≤ 1 mm for Pusher001, the sack and the arm-S bodies (else Q1 is decided before A3) |
| A3 | Exact resync, wire to world | `entity/session.hpp` (10), `session_assign_msg` and the repeats, `session_snapshot_msg`, `physics_world`, `session_runner` (missed base), `server.cpp`, core (§2.6, `resync_rules.hpp`, `life_log_`, `pending_deaths_`), `rollback.hpp` (`resync`), `client_platform.hpp` (`localize_scripts`, `forget_activations_from`, `forget_births_after`, clock sites), `session_host_world.hpp` (`forget_after`), journal, H and RC ops, both resync holds, `--resync-at` | §7.1 all; §7.6; §7.3 resync_snapoff ×3, running-client resync ×3 at 0 and 150 ms, a5u_lead; regression arms; re-captured `core_ref_r3/` |
| A4 | Retail gate | none (runs); fixes found go back to A3's files | §7.4 |
| A5 | Late joiner via the state | `server.cpp` (late ready), core first-assign branch | needs audit #16 (and #9 option B for pooled levels); §7.3 late join, its last-sector variant and its 10% loss run; A3's arms unchanged |
| B2 | Lift the Sector-margin cap on raises | `session/lead.hpp:209-213` at 72108fd (the `sector_bound` term; `lead_sector_bound` as built) and its test | late-player-design step 14: A5u with sectors 4, client 2's and the others' mechanism error ≤ control; `lead_test` updated. After A3, under protocol 10, because a pre-B1 client would suffer from a raise past its margin |
| T1 | Client journals replay through a resync | `sim_tool.cpp --replay-session` reads RESYNC (rows, scripts and union at S, the reconcile, then continues) | a resynced client's journal replays exact against itself after the RESYNC record |
| T2 | Trafo scripts out of `keep_local` | `session/resync_rules.hpp`; after `docs/trafo-replication-design.md` lands with its `trafo_live` join/resync state (its §4.7, which itself waits for this track's A3) | the trafo design's H1 arm with a forced resync of the trafoing client during its pieces' life: pieces bit-equal to the server's after the apply; A3's arms unchanged |

**Ordering with the other tracks.** A3 must land before the trafo
replication's T3 (that design's §8 T0 lists F3, which is this track's A3).
Until T2, a resync inside a trafo's 19.6 s keeps the owner's own trafo
state: the pre-existing owner-only gap (audit #2), not a new one.

**As built (step B2, 2026-10-24).**

- **What changed.** `lead_decide`'s Sector term is `lead_sector_bound`
  (`session/lead.hpp`): the worst margin less a tick, plus
  `kLeadSectorReachTicks` (48) for a player whose SessionReady declared
  `CAP_RESYNC_STATE`; never below 0; no sample, no bound, as before. The
  runner sets `lead_player::rewinds_sectors` from the caps
  (`session_runner.cpp` `set_player_lead`), and the S1 line prints
  `caps=lead+sector-rewind`. Nothing else moves: the total cap, the
  pooled samples, the shift on an acknowledgment, the deferral and the
  log labels (`cap=sector`) are as they were.
- **Adapted: a reach bound, not a removal.** Late-player §2.9 says the
  bound "stays at lead.hpp:211-212 as a reach bound that costs p only";
  this row says "lift". A late remote Sector inside the reach rewinds
  (`sector_rewind_reachable`: a rewind to stamp - 1 of at most
  `max_resim_ticks`); past it the client starts the sector live and
  resyncs - exact since A3, but a state transfer that a raise should not
  make routine. So the margin's cap is lifted by exactly the reach. The
  48 is `rollback_thresholds{}.max_resim_ticks`, which both hosts use
  unchanged; `lead_test` asserts the two agree. With the arena's margins
  (11-15) the bound is 58-62, above every total-cap room (80 less a
  start lead of about 27-35), so in practice only the total cap binds.
- **Adapted: gated on `CAP_RESYNC_STATE`.** The row's reason ("a pre-B1
  client would suffer from a raise past its margin") is met by the
  capability rather than the protocol number: the cap means a session core
  with the rollback on, which is exactly the client that has B1's rewind.
  A core with the rollback off (which may still declare `CAP_LEAD`) and the
  legacy retail path (which declares neither) keep the margin bound. No
  protocol change.
- **Verified (quick tier).** `BallanceMMOMessageTests` 471/471, three new
  (`LeadDecide.ASectorRewindingClientIsNotCappedByTheMargin`,
  `LeadDecide.TheRewindReachBoundsARaise`,
  `LeadSector.TheBoundIsTheMarginPlusTheClientsRewindReach`); GCC syntax
  26/26; the wire check `rA5 -> rB2` ALL SAME (24/24 sends, 24/24
  journals identical; `lead.hpp` is compiled into the server and the tests
  only). Live, A5u with `--sectors 4` (the a5u_lead recipe with sector 4
  as the start), snapshots off, one run each: `b2_a5u_lead` (patience and
  lead on) against `b2_a5u_ctl` (both off). Client 2 was raised once,
  `k=16` decided and sent uncapped at 1685 with its margin at 15 (the old
  bound would have sent 14); the server journal replays exactly in both.
  Phase A (from 1548), PE_Balloon excluded (I3) and each resync's own
  window excluded - from the request S + 1 to the end of its re-step
  (B + 47..53; late-player §5.2 A5u: "outside a resync's own 48 ticks",
  and a resync of client 2 is its own cost; the re-stepped rows run on
  predicted remote inputs past the newest relayed frame, B + 2, and are
  corrected from the first live tick on) - the worst mechanism per
  client, lead / control:
  c0 1.3661 / 1.3661, c1 1.3662 / 1.3602, c3 1.3602 / 1.3662 (the same
  `P_Modul_03_Gate` excursion from 1850 in both runs, on every client but
  the leader c2, the 6 mm between runs only the tick it is sampled at:
  G-c of `world-agreement-gaps-design.md`. On the server the elevator's
  proximity block (`P_Modul_03_MF`, one of the countdowns the world
  records) fires for client 2's ball, the nearest at 34.6 m at 1850, and
  the gate drops from y 4.17 to 3.00 by 1950; the other clients' blocks
  watch their own balls only, which arrive about 100 ticks later. Queued
  as its own step, roadmap §4 item 7), c2 0.0525 / 0.0542
  (`PE_Box_slide` at 2214), and after client 2's resyncs 0.0003 / 0.0034.
  The harness's raw phase-A `mech` (no exclusions) reads c2 6.97 against
  4.54: `P_Modul_08_Schaukel` at 2748, inside client 2's resync window
  2662-2790. That window is longer by the raise: client 2's schedule
  rebased at 2661 in both runs (the frame loop 33 ticks behind wall time
  under 38-39-tick rewinds per frame for the three respawns at 2596-2612),
  and Δ is -38 with its +16 offset against -22 without. In the control all
  four clients rebased and resynced there; with the flags on only client 2
  did. The rebase itself is pre-existing and flag-independent (the CPU
  cliff, queued under roadmap §4 item 8).
- **That run does not reach B2's path: A5u's client 2 crosses first.** The
  harness's staggered drive brings client 2 to every sector first (824,
  1270; the others 856-872, 1324-1341), so no remote Sector stamp ever adds
  a sector to client 2's union: `!covers` fails, nothing rewinds, whatever
  the raise. Moving the step to 500 (`b2_early_lead`, raise `k=16` effective
  746, before every crossing) shows it: client 2's three late stamps for
  sector 2 print "(LATE: the server started it already)" from the covered
  branch (`client_core_lifecycle.cpp:384-385`), `sectors: rew=0/0`. So the
  path is checked with the step on the last crosser, client 1 (`--event
  1:500:step=300:dir=up`, otherwise the same recipe, snapshots off):
  `b2_c1_lead` against `b2_c1_ctl`. Client 1 was raised `k=17` at 685
  (effective 748) with its margin at 14 - the old bound would have sent 13
  - and client 2's first crossings then reached it late and were rewound:
  "stamped for tick 824 (late by 3: rewound to 823)" and "1270 (late by 6:
  rewound to 1269)", `sectors: rew=2/0 depth=9`, nothing missed; both
  server journals replay exactly (3580 and 3560 ticks). The worst
  mechanism per client over phase A (from 548), PE_Balloon and each
  resync's own window excluded as above, lead / control: c0 2.7374 /
  2.7374, c1 2.7374 / 2.7374, c2 0.0058 / 0.0802, c3 2.7374 / 2.7374.
  The 2.7374 is `P_Modul_34_Schiebestein` at 1294, identical in both runs
  on every client but the first crosser c2 (0.0024): the signature of the
  gate's G-c above, attributed to it and not traced further; next the
  gate itself, 1.36. Client 1 after its last resync 0.0813 / 0.0802, both
  `PE_Box_slide` (a pushed box's pre-existing 8 cm, 0.0807 on control c3
  too).
- **What the raise costs the raised client (reported, not failed).** The
  four extra ticks of lead put client 1's relay lag at p99 47 / max 52
  against the reach of 48: 435 relayed frames past the reach (control 81,
  where every member lagged 43 waiting for client 1), and two "held notice
  ... arrived past the reach of 48 ticks" resyncs (1300, 2883) besides the
  rebase's (2682). Its re-step after the rebase is 53 ticks on predicted
  remote inputs (Δ -36, control -19), so the transient inside that window
  is larger: raw phase-A `mech` c1 23.18 (the balloon) and, balloon
  excluded, `P_Modul_08_Schaukel` 6.72 at 2810 inside 2683-2813 (control
  3.99 at 2736 inside its 2665-2757); 0.000 from the first live tick. This
  is late-player open question 8 - a raise past the reach before plan
  step 12 (the per-client reach, roadmap §4 item 12) - and, like A5u's
  relay_late there, p's own cost; the others' worlds are untouched (c0, c2,
  c3 at or below control). B2 makes it reachable at a smaller step than
  before (the old margin cap, 13 here, held the lag at about 43).
- **No slack at the reach.** At `k = worst + 47` a stamp one tick worse
  than the worst of the eight samples rewinds 49 ticks, which
  `sector_rewind_reachable` refuses: that one event then costs a resync
  (exact) instead of a rewind. Left so: on the arena the total cap binds
  well before it (above), and the margin samples move by a tick or two
  (11-15); revisit with step 12, which makes the reach per client.

**As built (step T1, 2026-10-24).**

- **What changed (SimTool only).** `--replay-session` of a client journal
  restores every RESYNC record whose outcome is applied (or applied with the
  scripts refused) at its tick S: after the replay has stepped S as before,
  it compares its own world with the state ("before the restore": the blob
  past its 16-byte header - magic, version and the capturing environment's
  identity - and the rows of the RECEIVED|FULL|RESYNC checkpoint), then
  makes the world S's - the balls (a rowed player not physicalized is made
  from its last birth the replay applied, at the row, no spawn kick; one
  with no row is unphysicalized), the server's portable record localized
  against the replay's own v2 with no script kept local and restored with
  `replay_activation` (restored-active scripts re-registered with the
  behaviour manager), the server's blob as a travelling one (nothing
  excluded; a clone renamed only where this world's CK name is not the
  blob's), rows written only where the restored body differs (a body the
  blob did not know; unknown unrowed bodies rechecked), and the world's
  counters (tick S + 1, the RNG cursors as the blob left the generators, the
  callback not pending, the server's union) - and goes on from S + 1 with
  the journal's records. The RESYNC checkpoint is compared after the
  restore (it checks the restore). A hole in the numbering within 256 ticks
  before a state's S is that resync's renumbering and does not stop the
  replay. `--list` prints the record (it stopped at it before: tag 8 was not
  in its switch, "record N (tag 8) does not parse", the rest of the file
  dropped). `--ignore-resync` is the pre-T1 replay for an A/B. Journals
  with RESYNC records get one more line, `resync summary: ...`; the summary
  line itself is unchanged, so it still compares across builds.
- **Adapted: the reconcile is the server world's, and partly counted.** The
  replay's world is a server world (`physics_world`), so the client's
  reconcile (2.6.3: births destroyed, mirrors re-made, keep-local kept) does
  not apply: nothing is keep-local and the blob carries everything. The
  balls are reconciled as above; a mechanism body the server has at S and
  the replay does not (a module the replay never physicalized), or the other
  way round, is counted and named, and the replay then says it is not the
  server's world after S - it cannot be made at S. Every journal below had
  0 of both, the late joiner included.
- **Adapted: what the record does not carry stays the replay's.** The
  journal record (5) has the union, the scripts and the blob; the wire's
  `SessionResyncState` also has every player's navigation (keys held, force
  controllers made by the press camera), the clone names and the packed
  sizes. The navigation stays the replay's own, which is the server's when
  the replay had the inputs before S; no format change (the step names
  none, and the wire check compares the clients' journals byte for byte).
  The clone names are read from the blob (`object` lines with `_BMMO_`).
  One accessor is added for the rows' ball entities,
  `physics_world::player_ball_entity` (read only).
- **Verified (quick tier).** `BallanceMMOMessageTests` 471/471; GCC syntax
  26/26; the wire check `rB2 -> rT1` ALL SAME (24/24 sends, 24/24
  journals identical). The step's check, `core_ref_r3/resync_snapoff`
  client 2 (S 1571, Δ -50, D 12), `--continue-after-jump
  --write-journal`, the written journal's TICK records against
  `server.bmjr`'s: before the restore the replay differs only in the IVP
  clock, one tick ahead (below), rows 27/27 equal; from S on the world hash
  and pose equal the server's at every tick 1571..3553 (1983 ticks), the 13
  received checkpoints after S 0 mismatches, rc 0. 3554..3570 differ: the
  journal's last relayed frames are for 3544 (its horizon, the relay lag).
  With `--ignore-resync` 0 of those 2000 ticks are identical (the clock
  offset stays). A journal without a RESYNC record replays as before:
  stdout of rB2 and rT1 identical on `a6m_snapoff` client 0 (with and
  without `--continue-after-jump`, and `--list`), `swing_snapon` client 1
  and `a6m_snapoff/server.bmjr` (4204/4204 matched). Further journals, hash
  and pose equal to the server's from S: `a5u_lead` c2 2733..3560 (two
  records; before the second, 3232, the replay's blob is IDENTICAL past the
  header and 56/56 rows equal - the replay ran the server's world from the
  first record), `resync_snapoff` c3 1884..3596 (to the end), c0 2229..3514,
  c1 2226..3542, `resync_at150` c0 886..3542, `swing_snapon` c3 3249..3534
  (snapshots on: 114 received snapshots after S, 0 mismatches), and the late
  joiner `a5_join_1` c3 (S 1636, Δ -1636; before: 20 of 26 rows differ, up
  to 685 m) 1636..3477, to the end.
- **Review fix-up: a restore that is not S's is a divergence.** A restore
  refused, or one that leaves the world not S's - a mechanism only one
  side has, a rowed ball with no birth or whose (un)physicalize failed, the
  scripts not restored (refused, not captured or empty), a clone with more
  than one blob name it could be, an unmapped row, or a row written over a
  body the blob restored (the blob did not make it the server's row; the
  write would hide it from the RESYNC checkpoint, which is compared after
  it) - is printed `RESYNC INCOMPLETE` / `NOT RESTORED`, makes rc 3 and
  stops `--stop-on-divergence`, as a second record whose blob differs
  already did. The RESYNC checkpoint of the restored tick is left out of
  the "after the first restore" counts (it checks the row write). A refused
  restore that excused a jump in the numbering stops the replay unless
  `--continue-after-jump` (the jump's old rule). The clone rename is made
  only with exactly one candidate and never while a rowed ball is unmade.
  `timed_out` and `no_state` (an applied record without a blob) are counted
  apart. The tick comparison is `scripts/replay_tick_compare.py SERVER
  REPLAY --from S --to T` (exit 0 = identical). Re-run on the fix-up build:
  every journal above restores with 0 of all of these, `resync_snapoff` c2
  1571..3553 1983/1983 identical (12 received checkpoints after S plus S's
  own, 0 mismatches, rc 0), `a5u_lead` c2, `resync_snapoff` c3,
  `resync_at150` c0, `swing_snapon` c3 and `a5_join_1` c3 identical over
  the ranges above (the last two rc 3 from received checkpoints before
  their S, as before); the journals without a RESYNC record byte-identical
  to rB2.
- **Found, pre-existing, not T1's (queued: `docs/roadmap.md` §4 item 9).**
  - *The client journal replay's clock is one tick ahead before its first
    restore.* A client journal opened at the anchor has its founding
    PLAYER records and start note in group 0; `run_replay_session` steps
    the world once per group up to the last simulated one, and numbers the
    world from the first simulated group (27), so group 0 steps an empty
    tick 27 and group 27 is the world's 28. Positions are unaffected
    (resync_snapoff c2's 6 received checkpoints before S all agree, with
    and without T1), the IVP clock is
    one step ahead, so no TICK hash of a client journal's replay equals the
    server's before a restore. A restore sets the clock (and the tick) to
    the server's. Not changed: a journal without a RESYNC record must replay
    as before.
  - *An own BodyRevived the server applies late is in the client journal at
    its stamp.* `resync_at150_down` c0 (2 records, Δ +16 then -18) is exact
    884..1097 and 1124..1447, then has 16-tick windows of differing hashes
    (1448..1463, 1556..1571, 1635..1650, ...) and `P_Modul_34_Schiebestein`
    0.637 mm off from 1917 (13 received checkpoints after S differ by it).
    c0's own BodyRevived events (stamped 1422, 1556, 1622, 1822, 1923,
    2022) were applied by the server 16-26 ticks later (1448, 1575, 1638,
    1838, 1941, ...: its uplink in that recipe), and the server never
    echoes a BodyRevived to its owner (server.cpp:2046-2050), so the client
    journal knows only the stamp; the replay wakes the bodies early, they
    settle on another tick, and the sliding stone settles 0.6 mm apart.
    Options: (a) the server echoes a moved BodyRevived to its owner and the
    core journals it without acting on it (a server and core change, the
    owner's own world is unaffected); (b) SimTool takes those ticks from
    the server journal when it is given (tool only, needs both files).
    Before the second record, 1098..1123 differ too: the server substituted
    c0's own frames 1098-1119 (keys 5; the client's FRESH frames say 1) and
    the journal has no SUBSTITUTED record for them - the RESYNC record at
    1124, whose state covers them, is what puts that right. (c0 had fallen
    behind: the schedule rebased at 1067 and asked for that resync, its
    FRESH records stop at 1106, its old tick at the apply, and its status
    counts 0 stale-epoch and 3 base notices - the notices for 1098-1106
    never reached its old epoch; whether the server sent them is queued
    with the two above.)

**As built (step RL1, 2026-10-27): the resync's base on the fixed deadline
schedule.** Found by the user's in-game trial (relay lag 21 -> 36-37 on the
other client after a stall-triggered resync): the base was the runner's
current tick + `resync_lead_ticks`, while the deadlines are fixed at the
start, so a resync handled while the server waited on that member gave its
frames about 6 ticks of deadline budget instead of 22 and the server then
relayed every member about 15 ticks later for the rest of the session. The
base of a resync, a resync at `L_eff` and a late join is now
`max(tick_scheduler::tick_at(now, lead), current_tick + 2)`
(`session_runner::reanchor_tick`, `sim/late_tick.hpp` `reanchor_base_tick`):
deadline(base) - now = lead + D, the start's budget. Server only, no
protocol change; A3's state at S is unchanged, only S moves. resync_snapoff:
the other clients' lag 36-37 -> 20-21, the server's S1 q50 for c2 6.05 ->
22.15; resync_at150: c0 26.77 -> 22.3. The full record is the late-player
design §2.8, "As built (step RL1)".

**As built (step RL1b, 2026-10-27): RL1's review follow-ups.** The exact
resync's client re-step depth moved resync_snapoff 12 -> 28 ticks and
resync_at150 34 -> 29: the 12 was the defect's shrunk budget, 28-29 is what a
member at the start's budget re-steps, inside the reach guard as before. The
held window [C, base) is lead + D - e(C), unbounded by a server backlog (the
member's on-time frames below the base are discarded). Code: the schedule's
tick is signed before the schedule's start (a re-anchor in the first
start_base periods gets the start's budget, not up to start_base periods
more), a member without CAP_RESYNC_STATE (no base-missed re-answer) keeps a
floor of current_tick + 3, and both start paths store the schedule through
one helper. Server only, no protocol change. The record is the late-player
design §2.8, "As built (step RL1b)".

**As built (step B2SW, 2026-10-27): the sweep of roadmap §4 item 2 - T2, G1,
N2 on every world, T1's findings.**

- **T2 (the trafo scripts out of `keep_local`): covered by T3, closed without
  code.** Measured first, as the row asks (`maps/arena-test/b2sw_A`, headless,
  4 clients, 150 ms, snapshots off, the server's `trafo_inputs` and
  placement on, build `rB2SWb`): client 0 transforms at 650 (stone, 50
  pieces) and is forced to resync at 1000 (`--resync-at`, S 1021, re-step to
  1049, inside the pieces' life); client 2 takes TR3b's deferred H3b (two
  downlink blackouts, `trafo_missed` 5, two resyncs at S 1145 and 2304).
  `tr3/svc.py --pieces-only` against the server journal: client 0 from 1050
  on 50/50 pieces bit-equal, 0 missing / 0 extra ticks; client 2 between its
  resyncs (1180-2137) 50/50 and after the second (2339 on) 33/33 bit-equal,
  its extra ticks only inside the second window before the apply (the missed
  reset's pieces, TR3's H3b signature); clients 1 and 3 50/50; the server
  journal replays 3270/3270. Why: with the option on the originals are never
  selected (trafo design 4.3), so `keep_local_script`'s trafo-name rule has
  nothing left to keep - the sets' copies (`_Trafo<player>`) are localized
  from the server's portable record like every other script - and with the
  option off the server runs no trafo, so keeping the originals local is the
  only right answer. `resync_rules.hpp` is unchanged (its comment's "until
  step T2" now means "while the trafo inputs are off").
- **G1 (a restored-active script that finished never runs again): fixed in
  shared code.** `script_replay::restore` (game/script_replay.hpp), under
  the new switch `live_membership()`, re-registers every selected root script
  it writes back active (or with ACTIVATENEXTFRAME) whose owner the live
  frame would not run: the scene flag `CK_SCENEOBJECT_ACTIVE` set first, then
  `CKScene::Activate(script, FALSE)` - exported by the retail CK2.dll as by
  the reimplementation; disassembled at this step (`CK2.dll`
  `?Activate@CKScene` -> ActivateScript at 0x2402eeb0): it adds the owner for
  the next frame (when active in the scene or the level) and, the scene flag
  being set, calls no CKM_BEHAVIORACTIVATESCRIPT callback - then the record's
  own next-frame flags put back. The behaviour manager's list is read through
  its public members only (`m_BeObjects`, the pending table walked, not
  hashed). `script_binding_options::live_membership` turns it on around the
  restore; the core hosts (`core_host.cpp`, `session_client.cpp`) and the
  SimTool bindings set it, the legacy retail path does not. SimTool's
  `--resync-check` root and apply and `--replay-session`'s resync restore use
  it too; the harness's own re-registration (`rc_unregistered_scripts`) now
  only counts what is left (and fixes it with `--resync-reregister`, the
  A/B). Evidence: L11 `--resync-check 150 180 --resync-arms E` `--verbose`:
  the root restore re-registers the same 5 scripts the harness did before
  (P_Modul_01_MF Script x2, P_Modul_25 script x2, P_Modul_37_MF Script), none
  left, "warm-up run vs the run from the restored root: identical"; the A2
  matrix cases l11_ER, l11_A5, arena_ER, arena_A4: every RESULT line equal to
  rR12's (12 arms). No `--resync-check` arm met the case at its apply (none
  needed the flag there), so the root is the case that needed it.
- **N2 on every world: built.** The pin is `game/sector_deactivate_pin.hpp`
  (`pin_deactivate_cell`, `repin_deactivate_cell`, `unpin_deactivate_cell`,
  `pin_checkpoint_deactivation`); the retail mod's
  `sector_deactivate_resolve_one` / `_apply` / `_restore` are uses of it (the
  table keeps its layout: BallanceMMOClient's members), and the headless
  client (`session_client.cpp`, after the checkpoint hold) and the server's
  world (`physics_world.cpp`, after the hold; `world_options::
  pin_checkpoint_deactivation`, off only for SimTool's `--n2-pin off`) pin
  both chains at their anchor. The headless client keeps its pins as the
  retail mod keeps its own: re-pinned in its frame guard (`script_guards`,
  beside the reset guard and the phase split, `repin_checkpoint_deactivation`)
  and handed back when the next session begins (`unpin_checkpoint_
  deactivation`, beside the phase split's restore: base.cmo's chains survive
  a level exit, and a pin made on top of the last would take its constant
  for the retail source). The server's world pins once and keeps nothing: it
  is booted for its session and destroyed with it (`physics_world::create`),
  and no code of it hands an input back - its phase split is applied once
  for the same reason (review round). Adapted: the chains are found as the retail
  mod finds them, a direct sub-behaviour of the script (BML's `FindFirstBB`
  default is not hierarchical): Event_handler has a nested behaviour named
  "reset Level" too, which a depth-first search met first (the first build
  pinned 1 of 2 on every level; now 2 of 2 on L1-L13 and in both retail
  clients). **The a4_resync_n2 case, reproduced offline** (its capture is
  protocol 10): SimTool `--level 13 --drop P_Ball_Wood_MF 0 --drop-at 220
  3.65 15 --drop-sector 1 --drop-move 2 T --cells-trace` (a ball dropped on
  the sector-2 checkpoint flame, crossing at tick 80; a pass of sector 2 at
  T): with `--n2-pin off` and T = 80 (the pass's manager starts in the
  crossing's frame) the chain writes [0][2] = 1 before the manager reads it
  and sector 1 is deactivated (bodies 57 -> 52, 66 at the end instead of
  71); with the pin, [0][2] stays 0 and the world ends with 71 at every T in
  76-81 (the A4 race, the server's world). **Found: [0][1] does matter**
  (the case the A4 note asked to check): a crossing of sector K's checkpoint
  in the frame of a pass of another sector J writes [0][1] = K after
  `run_sector_pass` wrote J, and the manager activates K instead of J - the
  same scene with `--drop-move 3 80`: union {1,3} in the world's
  bookkeeping, sector 2's 14 bodies made (71) instead of sector 3's 6 (63 at
  T 79 and 81). Pre-existing on every world, the retail mod included (N2
  leaves [0][1] for the own sector poll); queued in roadmap §4 item 2.
  Wire check `rR12 -> rB2SWb`: sends 12/12 SAME; journals 5/12 identical,
  7/12 (swing_snapoff c0-c3, l8_cross c0, c1, c3) identical order-free -
  body order only: the pin's two `BMMO_SectorKeep` parameters, made at the
  headless anchor, shift the CK ids of everything made after it (with the
  first build's one pin only swing_snapoff differed). The five server
  journals of `core_ref_s7` replay matched (4111, 3914, 3019, 2813, 2855).
- **T1's findings.** (a) Not built: it is the user's choice (below). (b)
  **Built**: `run_replay_session` applies a group before the world's first
  simulated one (a client journal's anchor era: PLAYER records and notes at
  tick 0) without stepping the world (`--legacy-first-step` is the old
  replay, for the A/B; one `note:` line counts such groups; the `summary:`
  line is unchanged). A/B on `core_ref_s7/resync_snapoff` client 2
  (S 1587): `replay_tick_compare.py` against the server journal, before S
  1560/1560 ticks identical in hash and pose (old: 0/1560, first difference
  27, the IVP clock one step ahead), after S identical in both (1978). The
  RESYNC record's before-restore blob comparison now differs only in three
  ball clones' CK entity ids (one lower than the server's: the skipped empty
  step had made one CK object the server makes elsewhere; ids are not
  physics); it used to differ in the clock. (c) **Established: the server
  sends no notice for those frames, by design.** `core_ref_r3/
  resync_at150_down` server.log: "Arena0 resynced at tick 1125 (... held
  counted through tick 1097)" - the resync's held window [C, base) (RL1b:
  lead + D - e(C)) substitutes 1098..1124 without notices, and the state at
  S = 1124 is the server's world after them; the notices before it
  (1054..1097, epoch 885) all went out. So a client journal has no
  SUBSTITUTED record for them and its replay feeds the client's FRESH
  frames there; the RESYNC record at 1124 puts it right. Nothing to fix in
  the session; a replay that wants those ticks exact needs the server
  journal (T1's option (b) for the BodyRevived case).
- **T1 (a) is the user's choice** (reported, not built). Symptom: an own
  BodyRevived the server applies late (16-26 ticks at 150 ms) is in the
  owner's journal only at its stamp, so a SimTool replay of that journal
  wakes the body early (resync_at150_down c0: 16-tick windows of differing
  hashes, P_Modul_34_Schiebestein 0.637 mm). Cause: the server never echoes
  a BodyRevived to its owner (server.cpp's echo covers the lifecycle types
  only). Options: (i) build the echo now (the server sends the owner its
  moved BodyRevived, the core journals it without acting; a protocol change
  - an existing message to a new recipient - that the step does not name,
  and dead code once world agreement's G5 retires BodyRevived under its
  world bit); (ii) wait for the user's world-agreement Q9: if G5 lands,
  BodyRevived is gone and so is the finding; (iii) SimTool takes those
  ticks from the server journal when given both files (tool only).

## As built (step B2SX, 2026-10-27): the [0][1] cell, and T1 (a) recorded

- **The [0][1] cell (N2's finding), the user's option (a).** The cause
  (B2SW): an own crossing of sector K's checkpoint (a clone's, on the
  server) in the frame of a union pass of another sector J - the pass writes
  IngameParameter[0][1] = J, the chain's Set Cell writes K before the sector
  manager's Start reads it, and the manager activates K. The fix:
  `game/sector_cell_split.hpp` (next to N2's `sector_deactivate_pin.hpp`)
  gives Gameplay_Events / activate Sektor's [0][1] Set Cell a private copy of
  IngameParameter (`_BMMO_SectorCell`, named by the session's parameter
  `BMMO_SectorCell` on the block's Target), as the phase cell's split did;
  the manager then reads only run_sector_pass's [0][1]. The own sector poll
  reads the copy: `sector_poll::cell_private` (client_platform.hpp), and the
  core compares a private cell with its last poll (`chain_cell_`, from the
  anchor's cell), never with a pass's sector (client_core_frame.cpp
  `poll_own_sector`) - a pass no longer writes the cell the poll watches.
  On every world, once at its anchor and only where the hold is in force
  (without it the chain runs the manager, which needs its own [0][1]): the
  server's world (`physics_world`, `world_options::split_sector_cell`, off
  only for SimTool's `--n2-cell off`), the headless client (kept in its
  frame guard, handed back when the next session begins), the retail mod
  with the session core on (`core_host.cpp`, a file-scope split: kept in
  `script_guards`, handed back at session end; the legacy path, core off,
  keeps the shared cell it polls). **Evidence**: the B2SW scene (SimTool
  `--level 13 --drop P_Ball_Wood_MF 0 --drop-at 220 3.65 15 --drop-sector 1
  --drop-move 3 T --ticks 120 --cells-trace`): T 79, 80, 81 make sector 3's
  bodies (63 at the end), and the chain's copy turns 2 at the crossing tick
  80 (the poll's cell) while [0][1] keeps the pass's 3; `--n2-cell off` at
  T 80 still makes sector 2's 14 (71). The core:
  `SessionClientCoreSectors.PrivateChainCellIsComparedWithThePollsNotThe
  Passes` - the own crossing of 2 in the frame of the pass of 3 is stamped
  at its own poll and its pass runs at its stamp; a pass changes nothing
  for the poll; an own crossing into a sector a peer started is seen
  without a moved resetpoint (a shared cell missed it: the pass had
  written the same sector). Wire check `rB2SW -> rB2SX` over `core_ref_s7`:
  sends 12/12 SAME; journals: resync_snapoff 4/4 identical, swing_snapoff
  and l8_cross 8/8 identical order-free only - the split's two CK objects,
  made at the headless anchor after N2's, shift the CK ids of every body
  made after them (list_bodies' order). The captures' own sectors are
  scripted (`--sector-at`), so the wire check does not cross a checkpoint
  through the poll; the gate round's one live headless session did
  (`maps/arena-test/b2sx_stallg`, level 13, 2 clients, 150 ms, snapshots
  off, client 0 `--beam 220 7.65 15 400`): both clients logged the split,
  client 0's poll saw its own crossing of checkpoint 2 and stamped it ("sector
  2 stamped for tick 473 (checkpoint, lead 35, input_delay 17)"), and both
  worlds ran the pass at 473 ("sector +2 at tick 473 ... stamped 473");
  server journal replay 2763/2763. The retail two-copy run is deferred to
  the final verification (the 15-minute budget). [Run at step RTX
  (rollback-engine-gaps design, "As built (step RTX)"; `scripts/retail/
  rtx_1`, L11, core on, snapshots off, trafo inputs and placement on):
  both copies logged the sector-cell split; client 1, beamed onto the
  sector-2 checkpoint at 785, logged "sector 2 stamped for tick 828
  (checkpoint, lead 9, input_delay 6), event sent ahead" - its poll's tick
  819 plus the lead - and both clients and the server ran "sector +2 at
  tick 828 (player 4289819235, stamped 828)" (server S5 late=0); client 2's
  own crossing into the sector the peer had started, beamed at 985, was
  seen by its poll and stamped 1028 ("remote player ... entered sector 2,
  stamped for tick 1028" on client 1; server late=0). One finding there,
  pre-existing and queued in roadmap item 4: a rewind whose window holds
  the own crossing leaves the record's `IngameParameter[0][2]` at its
  pre-crossing 1 until the next union pass writes 0 in every world (rtx_1:
  819-828; the peer, with no own crossing, held 1 there too; no reader
  today).]
- **T1 (a), the user's option (ii)** (2026-10-27): not built; it waits for
  the user's answer to world agreement's Q9. The cause stays as recorded at
  T1: `server.cpp`'s relay skips the owner for BodyRevived ("relayed to
  the others and never echoed", `on_event`'s `m != owner || msg.type !=
  BodyRevived`), so a moved BodyRevived is in its owner's journal only at
  its stamp and a SimTool replay of that journal wakes the body early
  (`resync_at150_down` c0: P_Modul_34_Schiebestein 0.637 mm, 16-tick
  windows). If world agreement's G5 lands, BodyRevived is retired and the
  finding with it; otherwise the echo (option (i)) or SimTool reading the
  server journal (option (iii)) is the user's to choose then.

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
- **Decided (step A2): the server's blob travels** (the user's standing
  default, roadmap §2). Rows-only fails the gate: arm S 14.4 mm on the
  arena's see-saw (a body unknown to the anchor's blob takes another place
  in IVP's orders and pairs), arm R 8.4 mm on Pusher001 from a 5 cm
  anchor (a substitute perturbation; the anchor's own contacts, mindists
  and hull state); the server's blob is exact in every non-pooled scene,
  the local level-end bodies aside. The cross-world restore test (x64, two
  processes): size measured; the name mapping measured (one rename, the
  own clone; with it the foreign blob is byte-identical and exact, without
  it 1.9-5.9 mm); and a blob of another process is **refused** today (its
  header names the capturing environment), so A3 adds a restore flag for
  a travelling blob. **Still open, A3's first gate:** the x64-server vs
  x86-retail format, not executed (no x86 SimTool build). Details in §7.2,
  "As built (step A2)".

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
- **Settled (step A4): the cap stays 95.** Retail L11, x86 mod, snapshots
  2 / 66, the key record, `session force-rollback 150:80` on client 1
  (`a4_q2_80`; the verb re-steps past 48 since A4's `force_rewind` reach):
  15 forced 80-tick rewinds, the longest 52 ms (`t_max`), about 45 ms on
  average (0.57 ms per re-simulated tick), no rebase, client 1's modules
  exact against the server throughout. The deepest live re-steps seen at
  A4: D 59 headless, 54 retail. A forced rewind is local only; a real
  apply also restores the server's blob, runs the reconcile and installs
  the server's scripts and navigation. `t_max` times all of that (the
  rewind timer of `restore_and_replay` spans the travelling blob, the
  reconcile's destroys and the replay), and the real retail applies stay
  below the forced 80: `a4_r1c_on3` session t_max 32 ms on client 1 (its
  D 54 apply the deepest rewind it ran) and 42 ms on client 2 (D 47 and
  D 11). The frame's other work (one live step, the render) is not in
  `t_max`; it is the frame time a rewind adds.

**Q3. How often does the apply wait for the body set (§2.6.4 step 2), and
for how long?**

- **Settle:** count `resync_deferred` and its ticks in every A3/A5 arm.
- Expected: for resyncs, the passes of `(R, S]` (Δ < 0); for late joiners,
  the owed passes, about 7-10 ticks each per lp1_join's 1642/1649.
- Any wait longer than the owed passes explain is investigated.
- **Settled for resyncs (step A4):** 0 waits in all 81 applies (67
  headless across the A4 arms and core_ref_r3, 14 retail): `waited 0
  frame(s)`, `resync_deferred` 0. The late joiner's count is A5's.

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
- **Settled (step A4): no hold.** No resync hold engaged in any A4 run
  (`holds=0/0 hold_max=0` on every client of the 10 retail runs and the
  headless arms): D was at most 54 on retail and 59 headless, against the
  reach of 95. The blocking hold stays as built; it remains covered by unit
  tests only.

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
- **Settled (step A4): -50 ≤ Δ ≤ +17, the sizes stay.** 81 applies: headless
  -50..+16 (Δ > 0 in 4, all from the downlink step of resync_at150_down,
  +14..+16; stalls -17..-50; `--resync-at` -5..-7), retail -50..+17 (Δ > 0
  in 8 of 14: the adapted r2_x_t1_b's both-way blackout, +15..+17; the
  stall -45/-50). |Δ| stays far inside `history_ticks` (96).

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
