# BMMO Mechanism Arena

A custom Ballance level built to exercise the collision-overhaul physics server:
it contains **one of every element type** the
[BallanceBlenderHelper](https://github.com/yyc12345/BallanceBlenderHelper)
plugin knows — all 23 sector components plus the level start, level end,
checkpoints and resetpoints — laid out one per bay so no two mechanisms
overlap.

| file | what it is |
| --- | --- |
| `build_arena_map.py` | the generator; run it inside Blender with bbp_ng enabled |
| `bmmo_mechanism_arena.blend` | the Blender source it produced |
| `BMMO_Mechanism_Arena.nmo` | the exported level |
| `arena_overview.png`, `sector1_view.png` | renders |
| `arena-test/` | output of `scripts/arena_map_test.py` |

## Layout

A flat arena, 1000 x 60 units, running along +X at z = 0 - plus a start plate
that reaches further back and to the side, and a death zone under all of it
(both below) - with **two parallel lines**:

- **the driving lane, y = 15** — `PS_FourFlames_01` at the start,
  `PC_TwoFlames_01..03` and `PR_Resetpoint_01..04` on the sector boundaries,
  `PE_Balloon_01` at the end. A ball can drive the whole arena and cross every
  checkpoint without being blocked.
- **the mechanism row, y = 43** — the 23 sector components, one every 40 units,
  which is wider than the largest footprint (`P_Modul_03`, ~32 units) so
  nothing intersects anything else. Six components per sector over four
  sectors.

They are separate because the mechanisms stand *on* the road rather than sunk
into it (below), which makes them walls: a lane running through them just stops
the ball at the first one. The deepest footprint still clears the lane by more
than 12 units, and the harness beams a ball to a bay when it wants contact.

A `Phys_FloorRails` double rail runs between the two at y = 29, so the map
carries the rail surface as well as `Phys_Floors`.

Two of the elements are **relocated** rather than given a bay of their own:
`P_Box` rests on the `P_Modul_30` see-saw and `P_Ball_Wood` on the
`P_Modul_41` tilting block, so a run exercises mechanism-against-loose-body
contact without anyone having to drive into them.

The road is solid 5-unit-thick Ballance floor blocks (surface at z = 0,
underside at z = -5), and **every mechanism sits at its retail height**, with
an opening cut in the road under the part of it that goes below the surface.

Getting that right took three tries, and the two wrong ones are worth knowing:

1. **Thickness-less floor plates.** Almost every `P_Modul_*` mesh reaches 5
   units below its mount point, because retail buries its base plate inside a
   5-thick floor block. On plates with no thickness those base plates stick up
   through the road, so every such mechanism begins the session interpenetrating
   a static body — it settles nonsense, and anything measured from that settle
   is nonsense too.
2. **Standing them all on the surface.** Lifting each mechanism until its
   lowest point clears the road fixes the clipping, but it raises the mechanism
   by however deep its buried base is — 5 units for most, 10 for `P_Modul_17`,
   15.8 for `P_Modul_03` — which carries the *working* parts up with it, out of
   reach of a rolling ball. The contact measurements caught it: `P_Modul_26`'s
   sack never came within 8.9 m of a ball and `Modul17`'s arms within 5.0 m.

What works is the retail height plus a hole sized to the **buried part only**
(`sunken_footprint()`), not to the mechanism's whole footprint. A swing's post
is about a metre across though the swing spans 25; cutting the whole 25 would
leave a hole a ball could drop through. The only gap left is the
`RECESS_MARGIN` ring of 0.6 units, far narrower than a ball (radius 2).

Three checks in `build_arena_map.py` keep it that way:

- `check_clipping()` — every component vertex below the road against every
  floor block, **per vertex, not per bounding box**: a mechanism legitimately
  has a box that spans floor blocks its geometry never touches, so a box test
  reports phantom overlaps here. For the map as committed it reports **426
  vertices over 9 components** (`P_Modul_01/17/18/19/25/29/34/41`,
  `PE_Balloon`): the base plates retail itself buries in the 5-thick floor
  block (see the openings below). Compare against that count; it is not zero
  and is not meant to be.
- `check_reach()` — how many components have geometry in the band a rolling
  ball sweeps (z = 0 .. 4.5). Returns 32/32.
- `check_depth_zone()` — the death zone against everything else (below):
  18.97 units of clearance under the lowest component vertex, 150 past the
  footprint on every side, hidden, and still exactly one `PE_Levelende`.

A human playing the arena reached the end and the session recorded a `Finish`
event at tick 5887, so the level completes.

### Stoppers

Three mechanisms need `Phys_FloorStopper` slabs and will not sit right without
them. The see-saw is the obvious one: with nothing to come to rest against, its
plank simply falls into its own opening — measured over a session, the plank's
height ranged over **4.02 m** (0.04 .. 4.06) and it was moving for 201 sampled
ticks at up to 10 m/s. With the stoppers in place the same run keeps it at
**0.10 m** of range (3.96 .. 4.06), moving for 72 ticks and only when a ball
actually rides it.

`STOPPERS` was measured the same way as `MOUNT_DZ`: every `Phys_FloorStopper`
vertex within 18 units of each mechanism across the 13 retail levels, clustered
into slabs and rotated into the mechanism's own frame. Retail merges every
stopper in a level into **one** object, so its bounding box says nothing and
the geometry has to be clustered.

| mechanism | instances with stoppers | slabs, in the mechanism's frame |
| --- | --- | --- |
| `P_Modul_30` (see-saw) | 17/18 | (±6.9, ±2.5, −0.25), along x |
| `P_Modul_25` (倒板) | 29/30 | (−9.6, ±2.5, −0.75), along x |
| `P_Modul_37` (T板) | 21/23 | (−11.3 and −16.3, ±4.2, −0.75), along y |
| `P_Modul_19` | 7/8 | 4 slabs at z −0.75 |

`P_Modul_41` and `P_Modul_03` never have them in retail, and do not get them
here. Every slab is the same part: a vertical quad 1 unit long and 0.5 tall
with no thickness.

### Mount heights

`MOUNT_DZ` in the generator is not guesswork. It was measured out of the 13
retail levels: for ~1250 component placements, the z of the component's origin
above the floor surface directly beneath it, taking the mode per type. That is
why the trafos sit at +0.095, extra points at +2.5 and `P_Modul_03` at +10.76.
`mount_dz()` raises that figure only where the retail value would sink the
mechanism into the road.

### The death zone

Retail Ballance kills a ball in `BallManager` (`Gameplay_Ingame`; the level
flow table in `docs/collision-overhaul-design.md`): every frame, a `Box Box
Intersection` of the ball against each member of the **`DepthTestCubes`**
group. A hit takes the controls away, unphysicalizes the ball a second later,
resets the current sector and physicalizes a new ball at the sector's
resetpoint. A level without that group has no way to die, and until
2026-10-20 this one had none: a ball that left the floor fell for ever - in
one 84 s run to y = -10391, still falling when the session ended.

All 13 retail levels carry the group, and its members look alike: 3-9
**hidden** boxes (`Quader01`, `DepthTestCube_01`, `Depth_TestCube01`, ...),
hundreds of units across and 20-200 deep, their tops mostly 1-25 units under
the floor above them. The arena has one, `DepthCubes_01`: hidden, the
footprint of everything plus `DEPTH_MARGIN` = 150 on every side - a ball
driven off an edge at full speed still covers ~30 units sideways on the way
down - 100 deep, with its top at z = -25, 20 under the slab and 19 under the
deepest component vertex (`P_Modul_17`'s base).

**bbp_ng trap:** `VirtoolsGroupConvention.set_to_object()` files a
`DEPTH_CUBE` under **`PE_Levelende`** (its parser reads `DepthTestCubes`
back), so going through it gives the level a second level end and still no
death zone. `add_depth_cube()` sets the group by hand, and
`check_depth_zone()` checks there is exactly one `PE_Levelende` member.

### The start plate

Every spawn and respawn is kicked at 3 m/s (the spawn impulse, design 9.10) in
a direction that `session/spawn_impulse.hpp` derives from the join order and
the tick, so the floor round the spawn has to hold a passive ball kicked any
way at all. It used to end right under it: the first bay's floor begins at
x = -20 - the spawn - and the old apron stopped 20 units short of it, so the
resetpoint sat on the floor's back edge with a gap behind. A ball that spawned
0.09 behind that edge, or was pushed back by a same-tick spawn, dropped
through.

The plate is now solid from x = -60 (`APRON_BACK`, 40 behind the spawn) to the
first bay, and reaches `APRON_SIDE` = 25 units south of the arena (y = -25)
beside the plate and the first bay, so no edge is nearer the spawn than 40.
The bays, checkpoints, resetpoints and every component are where they were:
against the previous `.nmo`, the only objects that differ are the floor
blocks behind and south of the spawn and the new `DepthCubes_01`.

The roll-outs come from `BallanceMMOSimTool --spawn-test N`, which
physicalizes N balls at the resetpoint in one tick with the spawn flag - the
way N real clients report a spawn - with `--drop-settle 0..63` moving the
spawn tick through every rotation of the direction table (64 runs per row):

| balls | type | farthest from the spawn | closest to an edge of the plate | fell |
| --- | --- | --- | --- | --- |
| 1 | stone | 8.89 | 31.25 | 0 |
| 4 | stone | 8.89 | 31.25 | 0 |
| 8 | stone | 16.57 | 26.75 | 0 |
| 4 | wood | 5.15 | 35.00 | 0 |
| 8 | wood | 15.04 | 25.00 | 0 |
| 1 | paper | 3.08 | 37.75 | 0 |
| 4 | paper | 25.57 | 16.25 | 0 |
| 8 | paper | 44.64 | - | 21 of 512 |

A lone kicked ball stops within 8.9 of the spawn whichever way it goes, and
balls spawned at the very same point do not push each other at birth: a
coincident pair has no contact normal, and IVP leaves a pair that is moving
apart alone (the player collision filter in `physics_state.cpp`). What spreads
them further is colliding afterwards: eight stone balls - a full room - fan
out to 16.6, and paper balls, which barely roll on their own, stay close
enough to bounce off each other. Eight paper balls spawned in one tick is the
one case the plate does not hold, and no session produces it: slot 13 spawns
every ball as stone.

**The harness does not kick most spawns.** The headless session client flags
a Physicalize as a spawn only if its ball is still within 0.05 m of the
resetpoint when it reports it (`report_physicalize`, `session_client.cpp`),
and with four balls spawning in the same tick it has already moved ~0.09 m by
then (the client logs `physicalize pose taken from the entity after the tick
(0.091 m from the resetpoint)`), so in a 4-player harness run the four initial
spawns carry no kick at all - only a lone respawn at the start does (it
compares against the level's first resetpoint, so a respawn further on is
never kicked). A real client reports
before the body exists (`physics_session_client.cpp`) and always flags. That
is why the plate is measured with the SimTool rather than with harness runs.

## Rebuilding the map

With Blender running and `bbp_ng` enabled (its **Ballance texture folder**
preference must point at a game `Textures/` directory):

```python
ns = {'__name__': 'build_arena_map'}
exec(compile(open(r'D:\repos\BallanceMMO\maps\build_arena_map.py').read(), 'build_arena_map.py', 'exec'), ns)
ns['build']()
bpy.ops.bbp.export_virtools(filepath=r'D:\repos\BallanceMMO\maps\BMMO_Mechanism_Arena.nmo',
                            export_mode='4', texture_save_opt='1',
                            use_compress=True, compress_level=5, successive_sector=True)
```

Then run the three checks (`ns['check_clipping']()`, `ns['check_reach']()`,
`ns['check_depth_zone']()`) and compare with the figures above. Export mode 4
writes every object, hidden ones included, which is what keeps the death zone.

## Installing it

The retail `Levelinit` script only resolves level numbers 1..13 — a
`Level_14.NMO` is simply never loaded — and the server refuses a physics room
whose map is not a hash-verified original level (`server.cpp`, `is_original_level()`).
So a custom map has to occupy one of the 13 slots:

```bash
cd "<game>/3D Entities/Level"
cp Level_13.NMO Level_13.NMO.orig
cp /d/repos/BallanceMMO/maps/BMMO_Mechanism_Arena.nmo Level_13.NMO
```

Restore with `mv Level_13.NMO.orig Level_13.NMO`.

Nothing else is needed: a custom map is geometry plus Virtools groups only. The
engine instantiates each mechanism's script from the matching prefab under
`3D Entities/PH/`, which is what makes `P_Modul_01_MF Script`,
`P_Modul_18_MF Script`, `PC_TwoFlames_MF Script` and friends show up in the
headless engine's active-roots list once the level is in game.

Note that the level slot decides the starting ball: in slot 13 the ball spawns
as `Ball_Stone`.


## Playing it yourself, alongside the headless clients

A real game client can join a session on this map, but three things have to be
right and none of them is obvious.

1. **The mod needs BMLPlus 0.3.12.** A game copy running plain BML 0.3.10
   silently *skips* the mod — the only sign is one line in `ModLoader.log`:
   `Mod BallanceMMOClient[BallanceMMOClient] requires BML 0.3.12`. The matching
   runtime ships in the SDK at `<sdk>/bin/BMLPlus.dll` (file version
   `0.3.12+6c9a693`); drop it into the game's `BuildingBlocks`.
2. **Run only the BMMO mod.** With BMLPlus 0.3.12, a `DebugUtilities` built
   for 0.3.10 fails to register a command and mod init stops there — the game
   never reaches the start menu and leaves no crash dump. Move the other
   `.bmodp` files out of `ModLoader/Mods` and put the mod's four runtime DLLs
   (`GameNetworkingSockets`, `abseil_dll`, `libcrypto-3`, `libprotobuf`) in
   there beside it.
3. **The server only starts a physics room on a hash-verified original level**
   (`server.cpp`, `is_original_level()`), and all room members must report the
   same map. The headless clients fabricate the canonical hash for their
   `--level`; a real client computes the file's hash honestly, so the two
   disagree and the room is refused. For a local play session, point slot 13's
   entry in `original_map_hashes` (`BallanceMMOCommon/include/entity/map.hpp`)
   at this map's hash and rebuild the server and session client:

   ```
   md5sum "<game>/3D Entities/Level/Level_13.NMO"   # c58a76b97bbb1b66bd824da72c9ec481
   ```

   That is the map as committed (with the death zone and the start plate,
   2026-10-20). Every rebuild changes the hash, so re-point the entry - and
   rebuild - whenever a new `.nmo` goes into the slot.

   **Keep that edit local.** It is a test convenience, not a feature.

Then start a server and some bots (one of them `--host --expect N`, counting
yourself), and in game:

```
/mmo connect 127.0.0.1:26676
/mmo auto level 13      # drives the menus into the slot, no level unlock needed
/mmo room list
/mmo room join <id>
/mmo room ready         # last - the host starts the moment everyone is ready
```

Load the level **before** readying: the host sends Start as soon as the last
member readies, and if you are still in the menu the map check rejects it — and
the headless host does not retry.

## Testing the server with it

```bash
# the broad one: every sector, four players, balls put on the mechanisms
python scripts/arena_map_test.py --bin build-agent/BallanceMMOServer \
    --root "C:/Users/geekerwan/Desktop/Ballance-Agent" --level 13 \
    --sectors 1,2,3,4 --players 4 --seconds 60 --drive --beam-bays auto

# the deep one: two players driving the whole arena, crossing every checkpoint
python scripts/arena_map_test.py --bin build-agent/BallanceMMOServer \
    --root "C:/Users/geekerwan/Desktop/Ballance-Agent" --level 13 \
    --sectors 1 --players 2 --seconds 60 --drive
```

Per sector the harness starts a local `BallanceMMOServer` with physics enabled
plus N headless `BallanceMMOSessionClient`s, drives the balls with generated
input records, and then checks four things:

1. every client exits 0 with no hard corrections, no resyncs and no sustained
   input starvation;
2. the server's own `.bmjr` journal replays bit-for-bit under
   `BallanceMMOSimTool --replay-session`;
3. the clients actually had local mechanism bodies (`mechanisms=<known>/<resolved>`
   with a non-zero *second* number — zero mechanism corrections on its own is
   **not** a pass, it is also exactly what a client with no local mechanisms
   reports; see the 9.28 level-reset postmortem). `known > resolved` is normal
   whenever the players straddle a checkpoint, because the server runs the
   union of their sectors;
4. the run produced the interactions it is meant to test — it reports which
   mechanism bodies were corrected, which peer balls were corrected, and which
   ball types were physicalized, and fails if either list is empty.

Knobs worth knowing:

- `--start-sector` (driven by `--sectors`) is what makes mechanism coverage
  complete: Ballance only runs the mechanisms of the sector the ball is in.
- `--beam-bays auto` drops each client's ball just short of a mechanism of the
  active sector, so contact does not depend on anyone driving the full arena.
  It inflates the reported `max_err` by design — the teleport itself is a
  several-hundred-metre "error" to the rollback engine.
- `--drive` holds "up", staggers each player's start by 40 ticks and has the
  leader brake briefly every 500, so the field concertinas and the balls run
  into each other. Do not add `--steer`: a pre-recorded steer has no feedback,
  so the lateral input accumulates and the balls end up 15-20 units off line.

### Multi-body pile-ups

Sending every client to the *same* bay with a small `--beam-spread` puts four
balls and a multi-body mechanism in contact at once, which is the case a
one-ball-one-mechanism run never reaches. Three of them, all passing with the
journal bit-exact:

```bash
# four balls onto P_Modul_29's nine plates (plus the see-saw next door)
python scripts/arena_map_test.py --bin build-agent/BallanceMMOServer \
    --root "<game>" --level 13 --sectors 2 --players 4 --seconds 50 \
    --drive --beam-bays 9,9,9,9 --beam-spread 2 --checkpoint-ticks 2

# four balls onto P_Modul_03's nine bodies
... --sectors 4 --beam-bays 20,20,20,20 --beam-spread 2

# four balls and the crate onto the see-saw
... --sectors 2 --beam-bays 10,10,10,10 --beam-spread 2
```

`collision_report.py` reports the busiest single tick of each run, which is
what says the contacts really were simultaneous rather than one after another:

| scenario | bodies touched | busiest tick |
| --- | --- | --- |
| 4 balls -> `P_Modul_29` | all 9 plates + `P_Modul_30_Wippe` | **9 mechanism bodies moving near a ball at once** |
| 4 balls -> `P_Modul_03` | all 9 (floor, gate, 7 walls) | **6 ball pairs touching at once** — every pair of 4 balls |
| 4 balls + crate -> see-saw | `P_Box_MF`, `P_Modul_30_Wippe`, 5 plates | 6 ball pairs at once |

All three report 6/6 ball pairs in contact over the run and 7-8 peer balls
corrected.

### Prop balls

`P_Ball_*_MF` are real movable bodies, so they are placed where something can
actually act on them:

| prop | where | why |
| --- | --- | --- |
| `P_Ball_Wood` | on the tilting block | the block carries it |
| `P_Ball_Paper` | on `P_Modul_29`'s plate row | the plates carry it |
| `P_Ball_Stone` | on the road east of the see-saw | 10 kg, and a player can hit it |

The stone one started on top of the see-saw, which was wrong: perched on the
plank it sat at z 3.81, above the band a rolling ball sweeps, so only the
see-saw could ever move it. On the road it is at z 0..4 and
`collision_report.py` measures a player ball closing to **2.88 m** of it —
contact for two radius-2 bodies is 4.0 — and disturbing it for 178 ticks.

**The openings come from retail, not from the placeholder mesh.** They used to
be cut from `sunken_footprint()`, which measures the PH placeholder in its
*authored, tilted* pose while at runtime the stoppers hold the plank level. The
see-saw's hole came out at ±13 where retail cuts ±7.4, so the plank ended short
of the road and left a lip: a ball driven east off the see-saw stopped about
0.8 m short, and only `--beam-dx` could reach the stone prop beyond it.

`RECESS_GAP_X` now carries retail's own gap, harvested the same way as
`MOUNT_DZ` — for every mechanism instance in the 13 levels, how close its own
floor comes to the mechanism's origin in the mechanism's frame. The answer is
that retail cuts a hole for **three** mechanisms (`P_Modul_30` 7.40,
`P_Modul_08` 7.50, `P_Modul_29` 6.86) and buries every other base plate in the
5-thick floor block, so this map does the same. `check_clipping()` reports
those buried vertices rather than failing on them; the guard with a right
answer is `check_reach()`.

With the narrower hole a ball drives across the see-saw and off the far side:
the stone prop's closest approach goes from 4.82 m with no contact to 4.01 m
and 43 disturbed ticks (contact is 4.0), and the see-saw itself from 20 to 77
disturbed ticks — without `--beam-dx`.

### Against a Linux server (Arch WSL)

The server does not have to be the Windows one. Built in Arch WSL (gcc 16.2.1,
system openssl/protobuf, no vcpkg) it runs the same physics sessions for
Windows clients, and its journal replays bit-for-bit in the **Windows**
SimTool, which is a cross-platform determinism check for free.

Two things to get right:

- **The engine half of the build id must match the clients'**, so pass it:
  `-DBMMO_BUILD_ID=ballanced-<engine sha12>+bmmo-<anything>`. The server only
  compares the `ballanced-` half.
- **Point `journal_dir` at `/mnt/d/...`** so the Windows side can replay the
  journal without copying it, and give the harness
  `--external-server HOST:PORT --journal-dir <that path>`.

WSL2 mirrored networking means the clients reach it on `127.0.0.1`.

### Latency and stalled clients

`--latency-ms` puts the named clients behind a UDP delay proxy;
`--stall-client/--stall-at/--stall-ms` freezes one mid-session. Four players on
the Arch server, the delay applied to clients 1 and 3 only:

| added latency each way | resyncs | hard corrections | worst client's resim ticks | journal |
| --- | --- | --- | --- | --- |
| 0 ms | 0 | 0 | 7 | bit-exact |
| 150 ms | 0 | 0 | 2086 | bit-exact |
| 300 ms | 0 | 0 | 4411 | bit-exact |

Latency does not cost correctness — no resyncs, no hard corrections, every
journal bit-exact. What it costs is **re-simulation**, and not only for the
slow players: at 150 ms the two *direct* clients were also re-simulating
hundreds of ticks, because everyone has to roll back to take the late inputs.
One bad link raises the CPU cost for the whole room.

A client **freezing** is different from a slow link. A 700 ms stall
(`--stall-ms 700`, about 46 ticks) put that client past the rollback window and
it re-anchored: one resync, which is the designed recovery. The other three
were unaffected and the journal still replayed bit-exact.

### Trafo fragments and checkpoints

Four players beamed onto the wood trafo together transform at the same moment,
which is the case the fragment work (design 9.10/9.14) is about:
`ball types physicalized: {'1': 9, '2': 4, '0': 4}` — nine stone spawns, then
four wood and four paper transformations — with **16 peer balls corrected**
across all three ball types, and the journal bit-exact.

Four players crossing all three checkpoints together (beamed onto the driving
lane with `--beam-y 15`, since the checkpoints are 28 units off the mechanism
row) also passes bit-exact, reaching `sectors entered: 1, 2, 3, 4`.

## Results (2026-10-20): the death zone and the start plate

The plain drive run - `--sectors 1 --players 4 --seconds 50 --drive`, local
server, `build-agent/` (`ballanced-e620b487c59e+bmmo-d2ab09a22004-dirty`) -
on the map at two stages. All four balls spawn together at tick 212 in every
run, so every run gets the same four directions; and in every run the
server's journal replayed bit-exact with `hard=0`, `resyncs=0`, `starved=0`.

| map | at the start | further on | replay | mechanism bodies touched |
| --- | --- | --- | --- | --- |
| death zone only, old start | 2 balls went over the back edge, died 107-108 ticks later and respawned; one went over again, died at tick 710 and respawned again | the others drove off the far end, died and respawned | 2799/2799 | 12 |
| as committed (twice: a copy of the game directory, then the installed one) | no ball left the plate | all 4 drove off the far end (x ~ 970) 33-36 s in, died 104-106 ticks after leaving the floor and respawned 204 ticks later at the resetpoint each had last crossed (x = 700, 700, 220, -20) | 3033/3033, 2847/2847 | 25, 26 |
| as committed, `--checkpoint-ticks 2` | the same | the same, checked every 2 ticks: all 8 lives, no ball under the floor without a death | 2832/2832, 1416 checkpoints | 25 |

On the map before either fix, a 4-player drive run that went on for 84 s had
one ball through the gap behind the spawn and three off the far end, and all
four were still falling when it ended, the lowest at y = -10391.

Without `--drive` the balls stay where the spawn leaves them. With the death
zone but the old start, two of the four same-tick balls went through the gap,
and one of them died three times over: its respawns landed 0.09 behind the
edge, or beside a ball that pushed it back, and rolled off without any kick.
As committed, all four come to rest 4.7-9.5 units from the spawn, on the plate
(replay 1520/1520). That run reports FAIL only because a passive ball never
reaches a mechanism.

## Results (2026-10-19, VS 18 build in `build-agent/`)

Every run below passed, and in every one of them the server's journal replayed
bit for bit with `checkpoint_ticks: 2` — 1350-1520 full body checkpoints per
session, **0 bodies differing** — with `hard=0`, `mech_hard=0` throughout.

**Regression, 4 sectors x 4 players x 50 s: 4/4 PASS.**

**Ball against ball.** Measured with `collision_report.py` over nine
4-player sessions: **54/54 ball pairs made contact**, 2754 contact ticks,
closest approach **0.14 m** — two radius-2 balls squeezed almost concentric, so
these are real collisions, not near misses.

**Ball against mechanism.** Across the regression plus the targeted runs,
**27 distinct mechanism bodies over 12 mechanism types** were predicted locally
and reconciled with the server. Four-ball pile-ups onto the two multi-body
mechanisms both pass with every body involved:

| run | bodies touched | peer balls corrected |
| --- | --- | --- |
| 4 balls onto `P_Modul_29` | all 9 plates + `P_Modul_30_Wippe` | 8 |
| 4 balls onto `P_Modul_03` | all 9 (floor, gate, 7 walls) | 8 |

**The ventilator, with a paper ball.** `P_Trafo_Paper` sits 6 units upstream of
`P_Modul_18`, so a ball crossing it turns to paper and then meets the fan —
one ventilator lifts paper, and only paper (six are needed for wood,
thirty-two for stone). With `--drive-stop` releasing the controls so the ball
halts in the column instead of crossing it at 15 units a second, the paper ball
**rose to y = 9.00** against a resting height of 2.0. `BallanceMMOSimTool
--drop P_Modul_18_01 0` shows the same thing from the other side:
`P_Modul_18_Particle Script` becomes active the moment the paper ball arrives.

**Deep run — 2 players driving the whole arena: PASS.** Both balls crossed all
three checkpoints, 24 mechanism bodies corrected, 18 mechanisms revived.

**Solo control — 1 player: PASS**, whole-run max error 0.086 m.

### One failure, and what it was

One sector-3 run in a back-to-back suite failed with resyncs and up to 46 %
input starvation. It is a host capacity limit, not a protocol fault, and the
numbers say so plainly: the worst client reported `frames=414` against
`tick=2983` — it was not stepping its own physics at all — while the server's
journal still replayed `3034 matched`, 1517 checkpoints, 0 differing, and no
client took a hard correction. One server plus four headless clients is five
full Ballance engines at 66 Hz on one box, and sector 3 carries the
`P_Modul_26` rope-and-sack joint chain. Run alone, the same case passes twice
out of two. The harness now waits `--gap` seconds between cases so the previous
session's processes are gone before the next server binds; with that, the
regression is 4/4.

### What the runs turned up

At every sector boundary, roughly 6 ticks after the *trailing* player crosses
it, that client takes one rollback of 0.6-2.7 m on a mechanism of the sector it
just entered — and the solo control, crossing the same checkpoints, never does.
The cause is that the server runs the union of its players' sectors while each
client runs only its own, so the second player starts that sector's mechanisms
tens of ticks late. It is written up — together with the A/B that led to a
server-side change for it being measured, found to buy nothing, and reverted —
in [docs/mechanism-phase-at-sector-activation.md](../docs/mechanism-phase-at-sector-activation.md).
No fix for it is in the tree.

One map-design trap worth repeating: the game names a component's physics body
after its **element type**, so two instances of one type are two bodies with
the same name. An earlier version of this map had two `P_Ball_Wood` 320 m
apart and the runs produced a phantom `P_Ball_Wood_MF` correction of exactly
320.004 m. The map now relocates rather than duplicates (`RELOCATE` in the
generator), so every element type appears once.
