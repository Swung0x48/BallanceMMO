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

A flat arena, 1000 x 60 units, running along +X at z = 0. The 23 sector
components sit in a row at y = 30, one every 40 units — wider than the largest
footprint (`P_Modul_03`, ~32 units), so nothing intersects anything else.
Six components per sector over four sectors, with `PC_TwoFlames_01..03` and
`PR_Resetpoint_01..04` on the boundaries, `PS_FourFlames_01` at the start and
`PE_Balloon_01` at the end. A `Phys_FloorRails` double rail runs alongside so
the test covers the rail surface as well as `Phys_Floors`.

The floor is made of `floor_flat` plates (a single top-facing quad each, the
same construct the retail maps use for open areas) rather than solid floor
blocks. That matters: several mechanisms mount *below* the floor surface — the
ventilator sinks 4.66 units — and a thickness-less plate keeps them from
starting the session interpenetrating a static body.

### Mount heights

`MOUNT_DZ` in the generator is not guesswork. It was measured out of the 13
retail levels: for ~1250 component placements, the z of the component's origin
above the floor surface directly beneath it, taking the mode per type. That is
why the ventilator sits at -0.297, the trafos at +0.095, extra points at +2.5
and `P_Modul_03` at +10.76.

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

## Testing the server with it

```bash
python scripts/arena_map_test.py --bin build-agent/BallanceMMOServer \
    --root "C:/Users/geekerwan/Desktop/Ballance-Agent" --level 13 \
    --sectors 1,2,3,4 --players 2 --seconds 40 --drive
```

Per sector it starts a local `BallanceMMOServer` with physics enabled plus two
headless `BallanceMMOSessionClient`s, drives the balls down the arena with a
generated input record, and then checks three things: both clients exit 0 with
no hard corrections, resyncs or input starvation; the server's own `.bmjr`
journal replays bit-for-bit under `BallanceMMOSimTool --replay-session`; and the
sector's mechanisms were actually physicalized.

`--start-sector` is what makes the coverage complete — Ballance only runs the
mechanisms of the sector the ball is in, so each sector needs its own session.

`--drive` holds "up" only. Do not add `--steer`: a pre-recorded steer has no
feedback, so the lateral input accumulates and the ball ends up running 15-20
units off the bay centre line, past the very mechanisms the run is meant to hit.

## Results (2026-10-19, HEAD 29b363d, VS 18 build in `build-agent/`)

Four sessions, one per sector, two headless clients each, 45 s of driving:

| sector | mechanisms tracked | hard corrections | resyncs | journal replay |
| --- | --- | --- | --- | --- |
| 1 | 15/15 | 0 | 0 | 2699/2699 bit-exact |
| 2 | 12/12 | 0 | 0 | 2708/2708 bit-exact |
| 3 | 19/19 | 0 | 0 | 2708/2708 bit-exact |
| 4 | 42/42 | 0 | 0 | 2711/2711 bit-exact |

All eight clients exited 0 with `mech_hard=0`, `mech_max_err=0.0000`,
`unmatched=0`, `starved=0`, `rebases=0` and no checkpoint mismatches. The map
plays: the ball spawns as `Ball_Stone`, rolls onto `P_Trafo_Wood_01` at x = 0
and becomes `Ball_Wood`, then runs the arena at ~15.8 units/s at a steady height
of 2.02, crossing the checkpoints (`world: sector +2 at tick 1067`).

### One thing worth following up

In three of the four runs the host client took a single one-tick rollback on
`P_Modul_29`, about 100 ticks **after its sector was activated** — not at the
session start:

```
mismatch at tick 1167: P_Modul_29_Platte01 dp=0.4501 ... Platte03 dp=2.2352 ...
rollback to tick 1167 (P_Modul_29_Platte05 off by 3.328804 m), re-simulated 1 ticks
```

The client had already started the plate row's drop (local y -1.14, -2.12,
-2.99 along the row) while the server still had every plate at -0.770: a
mechanism **phase** offset that grows with plate index, not a position error.
Rollback absorbed it in one tick and the following ticks are back to
0.014-0.046 m, so it never became a hard correction and the server's own
journal is still bit-exact. Design 9.25-9.27 aligned the mechanism start phase
at the session anchor; this is the same class of thing one sector-activation
later, and the arena reproduces it on demand.
