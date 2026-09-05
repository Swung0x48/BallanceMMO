# Plan: the session black box (server journal + offline replay)

Status: **plan only, nothing implemented yet.** Written 2026-09-04 for whoever
picks it up. No engine change, no protocol change, no client change is needed
for the recorder itself — it is server-side plus tooling, so rolling it out
means replacing the server binary only.

## Why

A physics-room bug that only happens in real multiplayer cannot be reproduced
today. `.bmrc` tick records (`record start` in the client automation, replayed
with `BallanceMMOSimTool --replay`) are solo-only: `record start` resets the
session clock, and it captures one player's keyboard, not the session. All we
have for a live session are the client's log (`session trace on`: `mismatch at
tick N`, `rollback to tick N (<entity> off by X)`) and the server's log
(`physics.debug_trace`). Those show the symptom, not a world I can re-run.

The server is authoritative and deterministic: given the anchored world, every
player's input frame per tick, and the client-reported lifecycle events, its
simulation is reproducible bit for bit (that is the whole premise of design
section 8, and Windows/Linux already agree — 9.13). So a journal of exactly
those inputs replays the session offline.

## What the world consumes (the determinism contract)

`bmmo::sim::physics_world`'s entire input surface, in the order it is used:

| call | when | journal record |
|---|---|---|
| `physics_world::create(world_options)` | session boot | header (level, seed, spawn_impulse, input_delay) |
| `add_player(id, join_order)` | boot and late join | `PLAYER` |
| `remove_player(id)` | leave | `PLAYER` |
| `set_input(id, frame)` | every tick, every player | `INPUT` |
| `apply_event(id, lifecycle_event)` | client events due this tick | `EVENT` |
| `tick()` | every tick | `TICK` (fingerprint) |

If a future change gives the world another input, it must be journalled too or
replays silently diverge. Say so in a comment next to `physics_world`'s
declaration.

## Capture points

All in `BallanceMMOServer/sim/session_runner.cpp`, on the simulation thread
(no locking needed):

- `create_session()` — after `physics_world::create` succeeds: open the file,
  write the header (session id, level, seed, `input_delay`, `spawn_impulse`,
  `anchor_hash`, `anchor_surfaces`, `bmmo::physics::build_id()`, UTC ms,
  snapshot cadence) and a `PLAYER` record per initial member.
  **The spawn impulse must be the session's, not the config's** — see commit
  9d4e393; the header is what a replay builds `world_options` from.
- `add_player()` / `remove_player()` — one `PLAYER` record each.
- `step()` — this is the important one:
  - the `applied` vector (`buffer.take(tick, fresh)`) is what the world
    actually consumed: journal *that*, not the wire messages, which can be
    lost, duplicated or out of order;
  - the events dequeued from `s.events` — journal them **after**
    `e.event.tick = e.tick`, because the spawn-impulse direction is derived
    from that field;
  - after `world->tick()`: a `TICK` record with
    `bmmo::physics::capture_world_hash` (`hash`, `pose`, `cores`) and the
    milliseconds since session start (for lining up with client log
    timestamps);
  - every `checkpoint_ticks` (default 660 = 10 s) a `CHECKPOINT` from
    `world->snapshot(true, bodies)`.
- `destroy_session()` / the failure path — a `NOTE` record with the reason and
  close.

## File format (`BallanceMMOServer/sim/session_journal.hpp/.cpp`)

Little-endian, field-by-field serialization (never a struct dump: the writer is
Linux x64, the reader is usually Windows x64). Header `"BMMOJRNL"` + `uint32`
version. Then a stream of `uint8` tag + payload:

| tag | payload |
|---|---|
| `PLAYER` | `u32 id, u8 join_order, u8 added` |
| `INPUT` | `u32 tick, u32 id, u8 repeat` — `repeat=1` means "same frame as this player's previous one" (the common case when nobody moves the camera); otherwise `keys u8, cam_right/up/dir 9×f32, ball_type u8, flags u8` |
| `EVENT` | `u32 tick, u32 id, u8 type, u8 ball_type, u8 flags, f32 pos[3], f32 rot[9], i32 sector, str name, recipe` |
| `TICK` | `u32 tick, u64 hash, u64 pose, i32 cores, u32 ms` |
| `CHECKPOINT` | `u32 tick, u32 count, count × body_state (kind u8, owner u32, str name, f64 pos[3], f64 rot[4], f32 lin[3], f32 ang[3], u8 flags)` |
| `NOTE` | `u32 tick, str text` — session start/end reason, room name, member names, anything a human wants in the box |

`str` = `u16` length + bytes. The recipe is `bmmo_physics_ball_recipe`'s fields
in declaration order (fixed-size name arrays written as `str`).

Size: ~47 B per player-tick worst case, 9 B when repeated, plus 25 B/tick and a
few KB per checkpoint — about 3 KB/s per player. Cap it: config
`physics.journal_max_mb` (default 256), and when the cap is hit write a final
`NOTE` and stop recording that session rather than the file growing forever.

Flush at every checkpoint and at every event, so a server crash still leaves a
usable box (that is the point of the name).

Reader: `bool read_journal(path, journal&, std::string& error)` returning a
`journal { header, initial_players, std::vector<journal_tick> }`, where
`journal_tick` groups the records that preceded each `TICK`. Reject a truncated
tail cleanly (keep the ticks read so far, report how many bytes were dropped).

## Config

```yaml
physics:
  journal_dir: journals      # empty = off
  journal_max_mb: 256
  journal_checkpoint_ticks: 660
```

File name `session_<id>_level<N>_<UTC yyyymmddhhmmss>.bmjr`. On the deployed
server that is `~/bmmo/journals/`; fetch with the `bmmo_deploy_ed25519` key
(see the memory note on deployment).

## Replay: `BallanceMMOSimTool --replay-session <file.bmjr>`

In `sim_tool.cpp`, a sibling of `run_replay`:

1. Read the header; warn (do not abort) when `bmmo::physics::build_id()`'s
   engine half differs from the header's, and when the game root's anchor
   surfaces differ from `header.anchor_surfaces` (wrong game data — that is
   the first thing to check when everything diverges at tick 0).
2. Build `world_options` from the header (`level`, `seed`, `spawn_impulse`,
   `trace` from a flag), `physics_world::create`, compare the anchor hash.
3. Add the initial players in recorded order (join orders matter: they index
   the nocoll groups and the spawn direction table).
4. Per journal tick: player add/remove, `set_input` for each recorded input,
   `apply_event` for each event, `world->tick()`, then compare
   `capture_world_hash` against the record. On a checkpoint compare every body
   (name, position, rotation, velocities) and print the worst offender.
5. Report the first divergent tick with the probe delta, then keep going and
   print a summary (`ticks=N matched=M first_divergence=T`), like
   `--replay` does for `.bmrc`.

Flags worth having: `--ticks N` (stop early), `--from A --to B` for
`describe_cores_exact` dumps around the interesting tick, `--dump-entity NAME`,
`--stop-on-divergence`, and `--list` (print the header, the members, the tick
range, the events and the notes without simulating — the fast triage pass).

## Pairing with the client trace: `scripts/journal_trace.py`

A Python reader of the same format (the format section above is the spec) plus
a parser for the client's `ModLoader.log` lines that carry a tick:
`mismatch at tick N (local M): …`, `rollback to tick N (<entity> off by X)`,
`Physics session: own ball … physicalized at tick N`, `resim tick N: …`.

```
journal_trace.py <file.bmjr> [--log ModLoader.log ...] [--around TICK] [--window 40]
```

Prints one merged timeline: per tick, the server's inputs/events/hash on the
left and each client's log lines for the same tick on the right, so "the server
had these inputs, the client rolled back here" is one screen instead of two
files. Also print the wall-clock mapping (journal `ms` + header UTC ↔ log
timestamps) so a player's "it looked wrong around 21:37" lands on a tick.

## Tests

1. **Round trip** (gtest, `BallanceMMOServer/tests`, runs in
   `BallanceMMOMessageTests`): synthetic journal with every record type,
   including repeated inputs and a truncated tail; read it back and compare.
2. **End to end, local**: a local server with `journal_dir` set, one
   `BallanceMMOSessionClient --record <a.bmrc>` player (and, better, the retail
   client too), a couple of minutes with a death and a trafo; then
   `--replay-session` must report `first_divergence=-1` over the whole file.
3. **Cross-platform**: replay a journal from the Linux server on Windows —
   also zero divergences, which doubles as a standing check of the Win/Linux
   bit-exactness the project already depends on (9.13).
4. Re-run the existing suite: unit tests, `rec_m3b.bmrc` replay,
   `--spawn-test 3`, the two-explosion `--body-guard` check of 9.14.

## Order of work

1. `session_journal.hpp/.cpp` + the round-trip test (self-contained, no server).
2. Hook `session_runner` + config + `config_manager` comment block.
3. `--replay-session` in the SimTool.
4. `scripts/journal_trace.py`.
5. End-to-end test, then a section in `docs/collision-overhaul-design.md` (9.15)
   and a line in `docs/building-and-deployment.md` about the config keys.

## Traps

- Build from Git Bash, never the PowerShell tool (ninja records no header
  dependencies otherwise), and after touching any engine header do a clean
  rebuild of the target — a stale object silently keeps an old build id or an
  old class layout. See the build-tree memory note.
- The journal must not change the simulation: it only reads what is already
  computed. `capture_world_hash` per tick is the one added cost — measure it
  with 8 players before defaulting the recorder to on.
- Keep the writer on the simulation thread; do not take the server's locks.
- A session that never reaches the anchor (boot failure) should still leave a
  header and a `NOTE`, or the box is empty exactly when it is most needed.
