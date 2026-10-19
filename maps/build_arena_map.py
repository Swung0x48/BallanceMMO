"""Build "BMMO Mechanism Arena": a Ballance level holding every element type
BallanceBlenderHelper knows, laid out as one bay per mechanism along a flat
arena so nothing overlaps anything else.

Mount heights (MOUNT_DZ) are not invented: they were measured from the 13
retail levels - the z of each component's origin above the floor surface
directly beneath it, taking the mode over ~1250 placements.

The floor is solid 5-unit-thick Ballance floor blocks (surface at z = 0,
underside at z = -5) and is **unbroken**: every mechanism is lifted so that it
stands completely on top of the road instead of being sunk into it.

That is a deliberate departure from the retail levels, where almost every
P_Modul_* mesh reaches exactly z = -5 below its mount point because its base
plate is buried in a 5-thick floor block.  Reproducing that faithfully needs
either a hole in the floor under each mechanism (which the balls fall into) or
a thickness-less floor (which lets the base plates stick through the road, so
every mechanism starts the session interpenetrating a static body and its
settling physics is meaningless).  For a test bench, standing them on the
surface is better than both: nothing clips, nothing can be fallen into, and
every mechanism is fully visible.  MOUNT_DZ below is still the measured retail
height; mount_dz() raises it only where the mechanism would otherwise sink.
"""
import bpy
import math
import mathutils

from bl_ext.user_default.bbp_ng import (
    UTIL_bme, UTIL_naming_convention, PROP_ballance_element,
    PROP_ballance_map_info, PROP_ptrprop_resolver, PROP_virtools_group,
)

NC = UTIL_naming_convention
BT = NC.BallanceObjectType
BET = PROP_ballance_element.BallanceElementType
Info = NC.BallanceObjectInfo

# origin z above the floor surface under it, measured from the retail levels
MOUNT_DZ = {
    'P_Extra_Life': 2.562,
    'P_Extra_Point': 2.5,
    'P_Trafo_Paper': 0.095,
    'P_Trafo_Stone': 0.095,
    'P_Trafo_Wood': 0.095,
    'P_Ball_Paper': 2.0,
    'P_Ball_Stone': 2.0,
    'P_Ball_Wood': 2.0,
    'P_Box': 2.5,
    'P_Dome': 0.0,
    'P_Modul_01': 0.0,
    'P_Modul_03': 10.76,
    'P_Modul_08': -1.5,
    'P_Modul_17': 3.968,
    'P_Modul_18': -0.297,
    'P_Modul_19': 0.0,
    'P_Modul_25': 0.0,
    'P_Modul_26': 2.685,
    'P_Modul_29': -0.17,
    'P_Modul_30': -0.5,
    'P_Modul_34': 0.0,
    'P_Modul_37': 0.0,
    'P_Modul_41': 0.0,
    'PC_TwoFlames': 0.0,
    'PE_Balloon': 0.0,
    'PR_Resetpoint': 3.326,
    'PS_FourFlames': 0.0,
}

# the 23 sector-bound components, in bay order; 6 per sector
BAYS = [
    'P_Trafo_Wood', 'P_Extra_Point', 'P_Modul_18', 'P_Modul_41', 'P_Modul_01', 'P_Box',
    'P_Trafo_Stone', 'P_Extra_Life', 'P_Modul_25', 'P_Modul_29', 'P_Modul_30', 'P_Ball_Wood',
    'P_Trafo_Paper', 'P_Modul_34', 'P_Modul_37', 'P_Modul_26', 'P_Modul_17', 'P_Ball_Paper',
    'P_Modul_08', 'P_Modul_19', 'P_Modul_03', 'P_Dome', 'P_Ball_Stone',
]

# Elements that are moved off their own bay onto (or next to) another bay's
# mechanism, so a run exercises contact that would otherwise need luck:
# type -> (host bay, dx, dy, placement).
#
#   'on_top'  rest it on the host mechanism's highest point
#   'road'    stand it on the road at its own mount height, dx/dy from the bay
#
# These are RELOCATIONS, not copies: the element keeps one instance and takes
# the host's sector, and its own bay is left as empty road.  Do not turn them
# into extra instances - the game names a component's body after its element
# type, so two P_Ball_Wood mean two bodies both called P_Ball_Wood_MF and the
# session has to guess which of ours a snapshot row means.  An earlier version
# did that, 320 m apart, and produced a phantom 320.004 m correction on
# P_Ball_Wood_MF, the exact distance between the two.
RELOCATE = {
    # on the tilting block, so the block carries a loose body
    'P_Ball_Wood': (3, 0.0, 0.0, 'on_top'),
    # beside the see-saw rather than on it: perched on top at z 3.81 the crate
    # sat above the band a rolling ball sweeps (0 .. 4.5) and no ball could
    # ever reach it.  On the road a ball can hit it AND shove it into the
    # see-saw, which is the interaction that was wanted in the first place.
    'P_Box': (10, -16.0, 0.0, 'road'),
    # immediately upstream of the ventilator: a ball crossing it turns to paper
    # and then meets the fan, which is the only ball type one ventilator can
    # lift (six are needed for wood, thirty-two for stone).
    'P_Trafo_Paper': (2, -6.0, 0.0, 'road'),
}

BAY_SPAN = 40.0          # > the widest footprint (P_Modul_03, ~32 units)
BAYS_PER_SECTOR = 6
ARENA_WIDTH = 60.0
# The mechanisms stand on the road rather than sunk into it, which makes them
# walls: a ball driven along a lane through them simply stops at the first one.
# So the driving lane and the mechanism row are separate.  PLAY_Y carries the
# start, the checkpoints, the resetpoints and the level end - a ball can drive
# the whole arena and cross every checkpoint without being blocked - and MECH_Y
# carries the 23 mechanisms, close enough to reach (the harness beams a ball to
# a bay when it wants contact) and far enough not to foul the lane: the deepest
# footprint, P_Modul_19's, still clears PLAY_Y by more than 12 units.
PLAY_Y = 15.0
MECH_Y = 43.0
RAIL_Y = 29.0
RECESS_MARGIN = 0.6      # clearance between a mechanism and the floor around it
FLOOR_THICKNESS = 5.0
MIN_STRIP = 0.35         # drop slivers rather than emit degenerate blocks


def scene():
    return bpy.context.scene


def reset():
    bpy.ops.wm.read_homefile(use_empty=True)
    v = PROP_ptrprop_resolver.PropsVisitor(scene())
    if len(v.get_ioport_encodings()) == 0:
        v.preset_ioport_encodings()


_floor_n = 0


def add_floor_block(x0, y0, length, width):
    """A solid Ballance floor block: surface at z = 0, 5 units thick."""
    global _floor_n
    if length < MIN_STRIP or width < MIN_STRIP:
        return None
    obj = UTIL_bme.create_bme_struct_wrapper('floor_flat', {
        'height_': FLOOR_THICKNESS, 'length_': length, 'width_': width,
        'face_': (True, True, True, True, True, True), 'is_sink_': False,
    })
    NC.VirtoolsGroupConvention.set_to_object(obj, Info.create_from_others(BT.FLOOR), None)
    scene().collection.objects.link(obj)
    obj.matrix_world = mathutils.Matrix.Translation((x0, y0, 0.0))
    _floor_n += 1
    obj.name = f'A_Floor_{_floor_n:03d}'
    return obj


def floor_with_recess(x0, y0, length, width, recess):
    """Tile [x0,x0+length] x [y0,y0+width] with solid blocks, leaving `recess`
    (an (rx0, rx1, ry0, ry1) rectangle, or None) open."""
    x1, y1 = x0 + length, y0 + width
    if recess is None:
        add_floor_block(x0, y0, length, width)
        return
    rx0, rx1, ry0, ry1 = recess
    rx0, rx1 = max(rx0, x0), min(rx1, x1)
    ry0, ry1 = max(ry0, y0), min(ry1, y1)
    if rx1 <= rx0 or ry1 <= ry0:
        add_floor_block(x0, y0, length, width)
        return
    add_floor_block(x0, y0, rx0 - x0, width)            # west of the recess
    add_floor_block(rx1, y0, x1 - rx1, width)           # east
    add_floor_block(rx0, y0, rx1 - rx0, ry0 - y0)       # south
    add_floor_block(rx0, ry1, rx1 - rx0, y1 - ry1)      # north


def element_bbox(type_name):
    with PROP_ballance_element.BallanceElementsHelper(scene()) as creator:
        me = creator.get_element(BET[type_name])
        xs = [v.co.x for v in me.vertices]
        ys = [v.co.y for v in me.vertices]
        zs = [v.co.z for v in me.vertices]
    return (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs))


def mount_dz(type_name):
    """Where this element's origin goes above the road surface: the retail
    height, unchanged.

    An earlier version lifted every mechanism until its lowest point cleared
    the road, so the road could stay unbroken.  That is wrong, and the contact
    measurements showed it: a mechanism gets lifted by however deep its buried
    base is - 5 units for most, 10 for P_Modul_17, 15.8 for P_Modul_03 - which
    carries the *working* parts that far up too, out of reach of a rolling
    ball.  P_Modul_26's sack never came within 8.9 m of a ball, Modul17's arms
    within 5.0 m.  The parts retail buries get a hole instead."""
    return MOUNT_DZ[type_name]


def sunken_footprint(type_name, rz_deg=0.0):
    """XY bounds of the geometry that sits below the road at the retail height,
    or None when the element sits entirely on top of it.

    Only that part needs an opening - not the element's whole footprint.  A
    swing's post is a metre across even though the swing itself spans 25, and
    cutting the whole 25 would leave a hole a ball could drop through."""
    dz = MOUNT_DZ[type_name]
    with PROP_ballance_element.BallanceElementsHelper(scene()) as creator:
        me = creator.get_element(BET[type_name])
        below = [(v.co.x, v.co.y) for v in me.vertices if v.co.z + dz < -0.05]
    if not below:
        return None
    a = math.radians(rz_deg)
    ca, sa = math.cos(a), math.sin(a)
    xs = [x * ca - y * sa for x, y in below]
    ys = [x * sa + y * ca for x, y in below]
    return (min(xs), max(xs), min(ys), max(ys))


def recess_for(type_name, cx, cy, rz_deg=0.0):
    """The floor opening this element needs at (cx, cy), or None.

    Sized to the geometry that goes below the road, plus a margin.  That margin
    is the only gap left around the element, and at RECESS_MARGIN it is far
    narrower than a ball (radius 2), so nothing can drop through."""
    foot = sunken_footprint(type_name, rz_deg)
    if foot is None:
        return None
    x0, x1, y0, y1 = foot
    m = RECESS_MARGIN
    return (cx + x0 - m, cx + x1 + m, cy + y0 - m, cy + y1 + m)


def add_component(type_name, sector, loc, rz_deg=0.0, name=None):
    ty = BET[type_name]
    if ty is BET.PS_FourFlames:
        info = Info.create_from_others(BT.LEVEL_START)
    elif ty is BET.PE_Balloon:
        info = Info.create_from_others(BT.LEVEL_END)
    elif ty is BET.PC_TwoFlames:
        info = Info.create_from_checkpoint(sector)
    elif ty is BET.PR_Resetpoint:
        info = Info.create_from_resetpoint(sector)
    else:
        info = Info.create_from_component(type_name, sector)

    with PROP_ballance_element.BallanceElementsHelper(scene()) as creator:
        obj = bpy.data.objects.new('', creator.get_element(ty))
        NC.YYCToolchainConvention.set_to_object(obj, info, None)
        NC.VirtoolsGroupConvention.set_to_object(obj, info, None)
        scene().collection.objects.link(obj)
    obj.matrix_world = mathutils.Matrix.LocRotScale(
        mathutils.Vector(loc), mathutils.Euler((0.0, 0.0, math.radians(rz_deg)), 'XYZ'), None)
    if name:
        obj.name = name
    return obj


def add_rail(name, x0, y, length):
    """A retail double rail, laid along +X with its tube bottoms on the floor.

    Built straight from UTIL_rail_creator rather than through bbp.add_straight_rail:
    that operator ends by driving the selection operators, which need a window
    context this session does not have.
    """
    import bmesh
    from bl_ext.user_default.bbp_ng import UTIL_rail_creator, PROP_bme_material

    bm = bmesh.new()
    UTIL_rail_creator.create_straight_rail(bm, False, 0.35, 3.75, length, 0, True, True)
    mesh = bpy.data.meshes.new('Rail')
    bm.to_mesh(mesh)
    bm.free()
    mesh.shade_smooth()
    with PROP_bme_material.BMEMaterialsHelper(scene()) as bmemtl:
        mesh.materials.clear()
        mesh.materials.append(bmemtl.get_material('Rail'))
        mesh.validate_material_indices()

    obj = bpy.data.objects.new(name, mesh)
    NC.VirtoolsGroupConvention.set_to_object(obj, Info.create_from_others(BT.RAIL), None)
    scene().collection.objects.link(obj)
    rot = mathutils.Matrix.Rotation(math.radians(-90), 4, 'Z')   # local +Y -> world +X
    obj.matrix_world = rot
    zmin = min((obj.matrix_world @ v.co).z for v in mesh.vertices)
    obj.matrix_world = mathutils.Matrix.Translation((x0, y, -zmin)) @ rot
    obj.name = name
    return obj


def build():
    global _floor_n
    reset()
    _floor_n = 0

    sector_count = (len(BAYS) + BAYS_PER_SECTOR - 1) // BAYS_PER_SECTOR
    x_last = (len(BAYS) - 1) * BAY_SPAN
    x_start = -BAY_SPAN / 2
    x_end = x_last + BAY_SPAN / 2
    y0 = 0.0

    # --- the road: solid slab, with an opening under each buried base ------
    x_balloon = x_end + BAY_SPAN * 0.75
    road_x0 = x_start - BAY_SPAN
    road_x1 = x_end + BAY_SPAN * 1.5
    # the start apron carries nothing sunken
    add_floor_block(road_x0, y0, x_start - BAY_SPAN / 2 - road_x0, ARENA_WIDTH)
    for i, type_name in enumerate(BAYS):
        cx = i * BAY_SPAN
        # a relocated element is not on this bay, so this bay only has to open
        # up for what its own mechanism buries - and nothing at all when its
        # element moved away
        floor_with_recess(cx - BAY_SPAN / 2, y0, BAY_SPAN, ARENA_WIDTH,
                          None if type_name in RELOCATE
                          else recess_for(type_name, cx, MECH_Y))
    floor_with_recess(x_end, y0, road_x1 - x_end, ARENA_WIDTH,
                      recess_for('PE_Balloon', x_balloon, PLAY_Y, -180.0))

    # --- level start + its resetpoint --------------------------------------
    add_component('PS_FourFlames', 1, (x_start, PLAY_Y, mount_dz('PS_FourFlames')),
                  90.0, 'PS_FourFlames_01')
    # the start pair sits 3.65 up, not 3.326 - that is how the retail start reads
    add_component('PR_Resetpoint', 1, (x_start, PLAY_Y, 3.65), 180.0, 'PR_Resetpoint_01')

    # --- the mechanisms ----------------------------------------------------
    for i, type_name in enumerate(BAYS):
        move = RELOCATE.get(type_name)
        if move is None:
            sector = i // BAYS_PER_SECTOR + 1
            pos = (i * BAY_SPAN, MECH_Y, mount_dz(type_name))
        else:
            host, dx, dy, how = move
            # the host's sector, so the two are activated together
            sector = host // BAYS_PER_SECTOR + 1
            if how == 'on_top':
                host_top = element_bbox(BAYS[host])[5] + mount_dz(BAYS[host])
                z = host_top - element_bbox(type_name)[4] + 0.05
            else:
                z = mount_dz(type_name)
            pos = (host * BAY_SPAN + dx, MECH_Y + dy, z)
        add_component(type_name, sector, pos, 0.0, f'{type_name}_01')

    # --- checkpoint + resetpoint on each sector boundary -------------------
    for s in range(2, sector_count + 1):
        boundary = (s - 1) * BAYS_PER_SECTOR * BAY_SPAN - BAY_SPAN / 2
        add_component('PC_TwoFlames', s - 1, (boundary, PLAY_Y, mount_dz('PC_TwoFlames')),
                      0.0, f'PC_TwoFlames_{s - 1:02d}')
        add_component('PR_Resetpoint', s, (boundary, PLAY_Y, mount_dz('PR_Resetpoint')),
                      180.0, f'PR_Resetpoint_{s:02d}')

    # --- rails: a side lane so the test exercises Phys_FloorRails ----------
    add_rail('A_Rail_01', 0.0, RAIL_Y, 160.0)

    # --- level end ---------------------------------------------------------
    # Lifted clear of the road like everything else, so its pillar does not run
    # through the slab.  The pad floats a little as a result, which does not
    # stop the level being finished - a human playing it reached the end and
    # the session recorded a Finish event.
    add_component('PE_Balloon', sector_count, (x_balloon, PLAY_Y, mount_dz('PE_Balloon')),
                  -180.0, 'PE_Balloon_01')

    info = PROP_ballance_map_info.get_raw_ballance_map_info(scene())
    info.mSectorCount = sector_count
    PROP_ballance_map_info.set_raw_ballance_map_info(scene(), info)

    return sector_count


def check_clipping():
    """Every component vertex that is below the road, against every floor block.

    Bounding boxes are no good here.  A mechanism sits in a hole cut to the
    footprint of its *buried* part only, so its box legitimately spans floor
    blocks that its geometry never touches - P_Modul_30 is 9.7 units deep
    overall but only 5 of that goes below the road.  So the test is per vertex:
    any vertex below the road surface that falls inside a floor block's
    footprint is real interpenetration."""
    floors, comps = [], []
    for ob in bpy.data.objects:
        if ob.type != 'MESH':
            continue
        with PROP_virtools_group.VirtoolsGroupsHelper(ob) as gp:
            groups = set(gp.iterate_groups())
        if 'Phys_Floors' in groups and ob.name.startswith('A_Floor'):
            co = [ob.matrix_world @ v.co for v in ob.data.vertices]
            floors.append((ob.name, min(c.x for c in co), max(c.x for c in co),
                           min(c.y for c in co), max(c.y for c in co),
                           min(c.z for c in co), max(c.z for c in co)))
        elif any(g.startswith(('P_', 'PC_', 'PE_', 'PS_', 'PR_')) for g in groups):
            comps.append((ob.name, [ob.matrix_world @ v.co for v in ob.data.vertices]))

    eps = 2e-3
    hits = {}
    for cname, verts in comps:
        for c in verts:
            if c.z > -eps:
                continue                      # on or above the road: fine
            for (fname, x0, x1, y0, y1, z0, z1) in floors:
                if x0 + eps < c.x < x1 - eps and y0 + eps < c.y < y1 - eps \
                        and z0 - eps < c.z < z1 + eps:
                    key = (cname, fname)
                    hits[key] = hits.get(key, 0) + 1
                    break
    return {'floors': len(floors), 'components': len(comps),
            'clipping': [{'component': c, 'floor': f, 'vertices_inside': n}
                         for (c, f), n in sorted(hits.items())]}


# A ball of radius 2 rolling on the road sweeps z = 0 .. 4; give it a little
# slack for bounces and for riding up on something.
BALL_SWEEP_Z = 4.5


def check_reach():
    """Which mechanisms a rolling ball could actually touch.

    The point of the map is contact, so a mechanism whose geometry is all above
    the band a ball sweeps is dead weight - that is exactly what lifting them
    clear of the road did to P_Modul_26, P_Modul_17 and P_Modul_03.  Elements
    that are *meant* to be out of reach (a balloon, a sack hanging from a rope)
    will still show up here; the number to watch is how many of the 23 bays
    have something in the band, not whether every body does."""
    out = []
    for ob in bpy.data.objects:
        if ob.type != 'MESH' or ob.name.startswith(('A_Floor', 'A_Rail')):
            continue
        zs = [(ob.matrix_world @ v.co).z for v in ob.data.vertices]
        in_band = [z for z in zs if -0.05 <= z <= BALL_SWEEP_Z]
        out.append({
            'name': ob.name,
            'z': [round(min(zs), 2), round(max(zs), 2)],
            'reachable': bool(in_band),
        })
    out.sort(key=lambda r: r['name'])
    return {
        'reachable': sum(1 for r in out if r['reachable']),
        'total': len(out),
        'out_of_reach': [f"{r['name']} z={r['z'][0]}..{r['z'][1]}" for r in out if not r['reachable']],
    }
