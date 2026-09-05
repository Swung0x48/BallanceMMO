# Building and deploying (collision overhaul)

This is the practical companion to the [design doc](collision-overhaul-design.md)
and the [protocol doc](rooms-and-sessions-protocol.md) section 3.1: how to
actually compile the client mod and the server on this branch, what to ship
to players, and what a server operator needs to set up. The collision
overhaul adds one moving part beyond the base [README](../README.md)
instructions: a `physics_RT` plugin (an open-source rebuild of the retail
engine's physics module, with the BallanceMMO bridge and the engine changes
in [engine-changes.md](engine-changes.md) baked in) that the client needs for
server-authoritative physics rooms, and that the server builds a static
equivalent of for its headless simulation.

## Building the client mod

Same prerequisites as the base README (VS2022 with the C++ workload and the
English language pack, CMake, Ninja), plus an extracted BMLPlus SDK
(`include/BML/BMLAll.h` and `lib/BMLPlus.lib`). Build from Git Bash or
`cmd.exe`, not PowerShell — PowerShell's UTF-8 console breaks ninja's header
dependency tracking, which silently produces stale incremental builds.

```bash
cmake -S . -B build-client -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVCPKG_TARGET_TRIPLET=x86-windows -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON \
  -DVIRTOOLS_SDK_FETCH_FROM_GIT=ON \
  -DBMMO_BMLPLUS_SDK_ROOT="<path to the extracted BMLPlus SDK>"

cmake --build build-client --target physics_RT BallanceMMOClient
```

Run this from an x86 MSVC developer environment (`vcvarsall.bat x86`). CMake
pulls Boost/OpenSSL/GameNetworkingSockets through vcpkg and the Virtools SDK
from GitHub automatically. `physics_RT` is a separate target
(`cmake/PhysicsRTPlugin.cmake`) that compiles `submodule/Ballanced` into a
drop-in replacement for the retail engine's physics module; build it
explicitly alongside `BallanceMMOClient` or physics sessions won't be
available. `BMMO_PHYSICS_FP_STRICT` and `BMMO_PHYSICS_PORTABLE_MATH` (strict
IEEE float semantics, OpenLibm transcendentals) default to `ON` and should
stay that way — they're what makes the client and the headless server
integrate bit-identically.

Artifacts:

- `build-client/BallanceMMOClient/BallanceMMOClient.bmodp`
- `build-client/BuildingBlocks/physics_RT.dll`

## Building the server

```bash
cmake -S . -B build-server -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF

cmake --build build-server --target BallanceMMOServer BallanceMMOSessionClient BallanceMMOSimTool
```

Do not clear `CMAKE_CXX_FLAGS_RELEASE`: the IVP sources key their assertions
and their debug output off `NDEBUG`, so a release tree without it builds a
server that `BREAKPOINT`s the whole process on an assertion IVP's own bounds
can trip (`worst_case_speed > max_coll_speed`, seen on a trafo into the stone
ball) and prints a statistics line every second - besides running unoptimized.
The configure step warns when the flag is missing. The simulation itself is
unaffected: every world hash of a 2500-tick `BallanceMMOSimTool` replay is
identical with and without `/O2 /Ob2 /DNDEBUG`.

Works the same way on Linux (`GCC`/`Ninja`, `libssl-dev`, `libprotobuf-dev`,
`protobuf-compiler`) and on Windows (x64 MSVC developer environment). Add
`-DBUILD_SERVER_TESTS=ON` and build `BallanceMMOMessageTests` to run the unit
tests (includes the message-layer, room-manager, session-protocol and
rollback-engine suites).

`BMMO_BUILD_SIM` (headless Ballanced simulation runtime, needed for physics
sessions) defaults to `ON`, so a server build now also compiles a static copy
of the whole engine (`cmake/BallancedHeadless.cmake`) — expect this to take
noticeably longer than a pre-collision-overhaul server build. Set it to
`OFF` only if you specifically need a physics-session-less build.

Artifacts land in `build-server/BallanceMMOServer/`:
`BallanceMMOServer`, `BallanceMMOSessionClient` (headless session client —
join a room and play back a recorded input file, used for load testing and
CI-style verification without a real game client), `BallanceMMOSimTool`
(offline replay/diagnostics), plus their shared-library dependencies
(`GameNetworkingSockets`, `yaml-cpp`, …) copied alongside them and the
`start_ballancemmo_loop` helper script — the whole output directory is the
deployable unit. On Linux, `cmake --build build-server --target install`
additionally installs everything under `CMAKE_INSTALL_PREFIX/bin` and `lib`.

## Distributing the client mod

Players need, under their game install:

1. BML or BMLPlus itself (`BML.dll` under `BuildingBlocks`).
2. `BallanceMMOClient.bmodp` (plus its runtime DLL dependencies, if built
   against shared vcpkg libraries) under `ModLoader/Mods`.
3. **`physics_RT.dll` under `BuildingBlocks`, overwriting the game's own
   copy.** This is the collision-overhaul-specific step: without it the mod
   still loads and shadow-ball rooms still work (it detects the retail DLL
   has no BallanceMMO bridge and reports "Physics session unavailable"
   instead of failing outright), but physics rooms are unavailable. There is
   no installer that does this automatically yet — call it out explicitly in
   release notes / install instructions.

Client and server must agree on the `physics_RT` build. Both sides compile in
a `ballanced-<engine commit>+bmmo-<repo commit>` build id, resolved by
`cmake/BuildId.cmake` before every build rather than once at configure time,
and the server refuses a physics session whose client reports a different
engine commit than its own. A build from an export with no git history says
`unknown` and is not compared; pass `-DBMMO_BUILD_ID=ballanced-<rev>+bmmo-<rev>`
to give such a build the id it should have.

## Deploying the server

Physics rooms need `config.yml`'s `physics` section filled in (a template is
written on first run):

```yaml
physics:
  enabled: true
  game_root: C:/path/to/a/full/Ballance/data/directory   # base.cmo, Textures, Sounds, 3D Entities, ...
  snapshot_interval: 2
  input_delay: 6
  maximum_physics_rooms: 1        # one physics world costs roughly one core
  event_rate_limit: 20            # client events per second per player; 0 = no limit
  spawn_impulse: 3.0              # m/s kick applied to every spawn Physicalize; 0 = off; solo sessions force 0
  journal_dir: journals           # session black box: one .bmjr per session; empty = record nothing
  journal_max_mb: 256             # cap per session file; recording stops there (a final NOTE says so)
  journal_checkpoint_ticks: 660   # ticks between full body checkpoints in the journal; 0 = none
```

- `game_root` must point at a complete copy of the game's *data* (not an
  installed, runnable copy — the headless engine reads the composition file
  and asset folders directly, it never loads or executes any Windows DLL).
  The same directory tree works whether the server itself runs on Windows or
  Linux. Without it, `physics.enabled` rooms fall back to shadow-ball mode.
- Nothing has to be configured to keep mismatched builds out: the engine
  commit is compared on join. It is not an integrity check - the client
  reports its own id - only a guard against the two sides having been built
  from different sources, which otherwise shows up as a world mismatch or as
  a session that hard-corrects forever.
- Start the server via `start_ballancemmo_loop` (handles logging and
  restarts on crash), not the executable directly. After editing
  `config.yml`, type `reload` in the server console rather than restarting
  the process.
- The server validates client-reported physics events at the rate/shape
  level (design section 9.4): more than `event_rate_limit` events/second per
  player are dropped, malformed `Physicalize` payloads are rejected outright,
  and suspicious-but-plausible ones (pose far from the level's resetpoint
  (2.5 m) and from the player's last known position (5 m), non-monotonic
  sector) are only logged and counted — check the console
  `sessions` command for `flagged`/`rejected` counters. The cap defaults to
  20; `event_rate_limit: 0` turns it off, which is worth doing on levels
  where a sector reset wakes more than 20 mechanisms at once (each one is a
  `BodyRevived` event and they all land in the same second).

## Session journals (the black box)

With `journal_dir` set, every physics session writes
`<journal_dir>/session_<id>_level<N>_<UTC yyyymmddhhmmss>.bmjr`: everything the
session's world consumed (members, every applied input frame, every lifecycle
event) plus a fingerprint per tick and a full body checkpoint every
`journal_checkpoint_ticks`, so a bug that only happens in a live room can be
replayed offline bit for bit. The [design doc](collision-overhaul-design.md)
section 9.15 has the format and the capture points.

`journal_dir` is resolved against the **server's own directory**, not its
working directory — the headless engine chdirs into the game's `Bin`. On the
deployed server that means `~/bmmo/journals/`. The start banner ends with
`journal <dir>` (or `journal off`), and the console `sessions` command prints
each running session's file, so you can fetch the right one while the room is
still up (`scp -i ~/.ssh/bmmo_deploy_ed25519 …`). Measured on a two-player
Level 1 session: about 120 B/tick, 8 KB/s, so the 256 MB cap is roughly 9.5
hours of one session; recording costs about 0.025 ms/tick.

Replay it on any machine that has a copy of the game data (a Windows box with
the same `game_root` tree is the usual one — the journal itself is
platform-independent, field-by-field little-endian):

```
BallanceMMOSimTool --replay-session session_1_level1_20260905080958.bmjr --list
BallanceMMOSimTool --root C:/path/to/Ballance/data \
    --replay-session session_1_level1_20260905080958.bmjr --report-every 500
```

`--list` is the triage pass: it prints the header, the members, the tick range,
every event/note/correction and the checkpoint ticks without booting the engine
(no `--root` needed) and streams the file, so it stays usable on a journal at
the cap. The replay prints
`summary: ticks=N matched=M first_divergence=T checkpoints=C checkpoint_mismatches=D`
and exits 0 when nothing diverged, 3 when something did. A journal left behind
by a crash is expected to be truncated: both readers keep everything before the
cut and report the dropped bytes.

`python scripts/journal_trace.py <a.bmjr> [b.bmjr …] [--log ModLoader.log …]`
is the merged timeline — the server's inputs and events, each client's own tick
hash and corrections, and the log lines that carry the same tick, one block per
tick, plus the wall-clock mapping so a player's "it looked wrong around 21:37"
lands on a tick. `--log` takes this server's own log as readily as a player's
`ModLoader.log`: it reads BMLPlus, ISO and BMMO's own console stamps
(`[09-05 04:09:19]`, dated from the journal header, so a log that runs across
New Year still lands on the right day). `--diff` compares two journals tick by
tick (exit 3 on a divergence, 1 when it had nothing to compare).

The client records too, which is what makes a player's report actionable: the
BML property `Gameplay`/`SessionJournal` (on by default, read when a session
starts) writes the same format to `<game>/ModLoader/BMMOJournals`, keeping the
newest 10 files. `/mmo journal status` prints the current file,
`/mmo journal mark <text>` drops a note plus a body checkpoint at the current
tick ("ask the player to type that the moment it looks wrong"), and
`/mmo journal on|off` moves the same property. Ask for the `.bmjr` next to the
player's `ModLoader.log`; `journal_trace.py` takes both at once. A client
journal is best effort by nature (it does not know when the server applied its
events), so it explains what that client saw — the server's journal is the
authority.
