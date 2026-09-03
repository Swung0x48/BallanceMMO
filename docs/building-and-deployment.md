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

Client and server must agree on the exact `physics_RT` build. On join, the
client reports its DLL's sha256 and a `ballanced-<engine commit>+bmmo-<repo
commit>` build id; see *Deploying the server* below for how the server can
enforce this.

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
  require_physics_sha: ""         # empty = accept any physics_RT build
```

- `game_root` must point at a complete copy of the game's *data* (not an
  installed, runnable copy — the headless engine reads the composition file
  and asset folders directly, it never loads or executes any Windows DLL).
  The same directory tree works whether the server itself runs on Windows or
  Linux. Without it, `physics.enabled` rooms fall back to shadow-ball mode.
- Set `require_physics_sha` to the sha256 of the exact `physics_RT.dll` you
  distribute to lock out mismatched or tampered clients; leave it empty
  during development. Headless clients (`BallanceMMOSessionClient`, load
  testing / CI) report a `headless-*` build id and are always exempt from
  this check.
- Start the server via `start_ballancemmo_loop` (handles logging and
  restarts on crash), not the executable directly. After editing
  `config.yml`, type `reload` in the server console rather than restarting
  the process.
- The server validates client-reported physics events at the rate/shape
  level (design section 9.4): more than 20 events/second per player are
  dropped, malformed `Physicalize` payloads are rejected outright, and
  suspicious-but-plausible ones (pose far from every spawn slot,
  non-monotonic sector) are only logged and counted — check the console
  `sessions` command for `flagged`/`rejected` counters.
