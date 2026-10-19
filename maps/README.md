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

A flat arena, 1000 x 60 units, running along +X at z = 0, with **two parallel
lines**:

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

The road is one unbroken slab of solid 5-unit-thick Ballance floor blocks
(surface at z = 0, underside at z = -5), and **every mechanism stands entirely
on top of it**.

That last part is a deliberate departure from retail. Almost every `P_Modul_*`
mesh reaches exactly 5 units below its mount point, because the retail levels
bury its base plate inside a 5-thick floor block. Reproducing that faithfully
needs either a hole in the road under each mechanism — which the balls fall
into — or a thickness-less floor, which lets those base plates stick up through
the road so every mechanism begins the session interpenetrating a static body.
The first version of this map did the latter, and it was wrong: a mechanism in
permanent contact with the road settles nonsense, and anything measured from
that settle is nonsense too. Standing them on the surface beats both: nothing
clips, nothing can be fallen into, and every mechanism is fully visible.

`build_arena_map.py` has a `check_clipping()` that compares every component
against every floor block and reports real overlaps; it returns zero for the
map as committed.

The cost is that the level cannot be finished — `PE_Balloon` is lifted clear of
the road with the rest, so its pad floats and the ball can never reach it. No
test here depends on finishing.

### Mount heights

`MOUNT_DZ` in the generator is not guesswork. It was measured out of the 13
retail levels: for ~1250 component placements, the z of the component's origin
above the floor surface directly beneath it, taking the mode per type. That is
why the trafos sit at +0.095, extra points at +2.5 and `P_Modul_03` at +10.76.
`mount_dz()` raises that figure only where the retail value would sink the
mechanism into the road.

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

## Results (2026-10-19, HEAD + the Sector-snapshot change, VS 18 build in `build-agent/`)

**Broad run — 4 sectors x 4 players x 60 s: 4/4 PASS.** Every journal bit-exact
(3692-3698 ticks each), `hard=0`, `mech_hard=0`, `resyncs=0`, `starved=0`,
`mechanisms` resolved N/N in every sector (3, 12, 7 and 30). Across the four
sectors the clients predicted and had corrected **26 distinct mechanism
bodies** — all nine `P_Modul_29` plates, all seven `P_Modul_03` walls plus its
floor and gate, `P_Modul_08_Schaukel`, `P_Modul_19_Flaps`, `P_Modul_25_Bridge`,
`P_Modul_34_Kiste`/`_Schiebestein`, `P_Modul_01_Pusher`, `P_Ball_*_MF` — and
8-12 peer balls per sector. All three ball types were physicalized (stone,
wood and paper), so every trafo fired.

**Deep run — 2 players driving the whole arena: PASS** (with 5 starved ticks in
3681 on one client, 0.14 %, noted as a warning). Both balls crossed all three
checkpoints, 24 mechanism bodies were corrected, 18 mechanisms revived, journal
bit-exact.

**Solo control — 1 player, same drive: PASS**, whole-run max error 0.086 m.

### What the runs turned up

At every sector boundary, roughly 6 ticks after the *trailing* player crosses
it, that client takes one rollback of 0.6-2.7 m on a mechanism of the sector it
just entered — and the solo control, crossing the same checkpoints, never does.
The cause is that the server runs the union of its players' sectors while each
client runs only its own, so the second player starts that sector's mechanisms
tens of ticks late. It is written up, with the A/B that shows what the
server-side change for it is and is not worth, in
[docs/mechanism-phase-at-sector-activation.md](../docs/mechanism-phase-at-sector-activation.md).

One map-design trap worth repeating: the game names a component's physics body
after its **element type**, so two instances of one type are two bodies with
the same name. An earlier version of this map had two `P_Ball_Wood` 320 m
apart and the runs produced a phantom `P_Ball_Wood_MF` correction of exactly
320.004 m. The map now relocates rather than duplicates (`RELOCATE` in the
generator), so every element type appears once.
