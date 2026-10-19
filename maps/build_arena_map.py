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

# Loose bodies parked on top of a moving mechanism, so the run exercises
# mechanism-against-mechanism contact without anyone having to drive into them.
# These are RELOCATIONS, not copies: the element still owns its bay index (and
# so its sector), it is simply placed on the host mechanism instead of on its
# own patch of road, and its own bay is left as empty road.
#
# Do not turn these into extra instances.  The game names a component's body
# after its element type, so two P_Ball_Wood in one level mean two bodies both
# called P_Ball_Wood_MF, and the session has to guess which of ours a snapshot
# row means.  An earlier version of this map did that, 320 m apart, and the
# runs duly produced a phantom 320.004 m correction on P_Ball_Wood_MF - the
# exact distance between the two - even though mechanism_tracking.hpp's
# identity guard is supposed to drop a pick beyond 20 m.
RELOCATE = {
    'P_Box': 10,        # rests on the P_Modul_30 see-saw (bay 10)
    'P_Ball_Wood': 3,   # rests on the P_Modul_41 tilting block (bay 3)
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
    """Where this element's origin goes above the road surface.

    The retail height, except for the mechanisms that retail sinks into the
    floor block: those are lifted until their lowest point rests on the
    surface, so the road can stay solid and unbroken."""
    z0 = element_bbox(type_name)[4]
    return max(MOUNT_DZ[type_name], -z0)


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

    # --- the road: one unbroken slab from the start apron to the end -------
    x_balloon = x_end + BAY_SPAN * 0.75
    road_x0 = x_start - BAY_SPAN
    road_x1 = x_end + BAY_SPAN * 1.5
    # tiled rather than one huge quad so no single floor mesh is absurdly big
    span = BAY_SPAN * 4
    x = road_x0
    while x < road_x1 - 1e-6:
        add_floor_block(x, y0, min(span, road_x1 - x), ARENA_WIDTH)
        x += span

    # --- level start + its resetpoint --------------------------------------
    add_component('PS_FourFlames', 1, (x_start, PLAY_Y, mount_dz('PS_FourFlames')),
                  90.0, 'PS_FourFlames_01')
    # the start pair sits 3.65 up, not 3.326 - that is how the retail start reads
    add_component('PR_Resetpoint', 1, (x_start, PLAY_Y, 3.65), 180.0, 'PR_Resetpoint_01')

    # --- the mechanisms ----------------------------------------------------
    for i, type_name in enumerate(BAYS):
        host = RELOCATE.get(type_name)
        if host is None:
            sector = i // BAYS_PER_SECTOR + 1
            pos = (i * BAY_SPAN, MECH_Y, mount_dz(type_name))
        else:
            # on top of its host mechanism, and in the host's sector so the two
            # are activated together
            sector = host // BAYS_PER_SECTOR + 1
            host_top = element_bbox(BAYS[host])[5] + mount_dz(BAYS[host])
            z0 = element_bbox(type_name)[4]
            pos = (host * BAY_SPAN, MECH_Y, host_top - z0 + 0.05)
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
    """Every mechanism against every floor block: report real overlaps.

    A mechanism may sit *in* a recess, so only an actual intersection of the
    two bounding boxes counts, and a shared face (the body resting on the
    surface) does not."""
    floors, comps = [], []
    for ob in bpy.data.objects:
        if ob.type != 'MESH':
            continue
        with PROP_virtools_group.VirtoolsGroupsHelper(ob) as gp:
            groups = set(gp.iterate_groups())
        co = [ob.matrix_world @ v.co for v in ob.data.vertices]
        box = (min(c.x for c in co), max(c.x for c in co),
               min(c.y for c in co), max(c.y for c in co),
               min(c.z for c in co), max(c.z for c in co))
        if 'Phys_Floors' in groups and ob.name.startswith('A_Floor'):
            floors.append((ob.name, box))
        elif any(g.startswith(('P_', 'PC_', 'PE_', 'PS_', 'PR_')) for g in groups):
            comps.append((ob.name, box))

    # 2 mm: a body resting on the surface shares a face with it, and float
    # rounding puts that shared face a few microns either side of z = 0.
    eps = 2e-3
    hits = []
    for cname, c in comps:
        for fname, f in floors:
            ox = min(c[1], f[1]) - max(c[0], f[0])
            oy = min(c[3], f[3]) - max(c[2], f[2])
            oz = min(c[5], f[5]) - max(c[4], f[4])
            if ox > eps and oy > eps and oz > eps:
                hits.append({'component': cname, 'floor': fname,
                             'overlap': [round(ox, 3), round(oy, 3), round(oz, 3)]})
    return {'floors': len(floors), 'components': len(comps), 'clipping': hits}
