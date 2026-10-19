"""Build "BMMO Mechanism Arena": a Ballance level holding every element type
BallanceBlenderHelper knows, laid out as one bay per mechanism along a flat
arena so nothing overlaps anything else.

Mount heights (MOUNT_DZ) are not invented: they were measured from the 13
retail levels - the z of each component's origin above the floor surface
directly beneath it, taking the mode over ~1250 placements.
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
BAY_SPAN = 40.0          # > the widest footprint (P_Modul_03, ~32 units)
BAYS_PER_SECTOR = 6
LANE_Y = 30.0            # arena centre line
ARENA_WIDTH = 60.0
PLATE_SPAN = 200.0


def scene():
    return bpy.context.scene


def reset():
    bpy.ops.wm.read_homefile(use_empty=True)
    v = PROP_ptrprop_resolver.PropsVisitor(scene())
    if len(v.get_ioport_encodings()) == 0:
        v.preset_ioport_encodings()


def add_floor_plate(name, x0, y0, length, width):
    """A flat Ballance floor: a single top-facing quad, surface at z = 0.

    Flat (thickness-less) plates are what the retail maps use for open areas,
    and they keep the mechanisms that mount *below* the surface (the
    ventilator sinks 4.6 units) from intersecting a floor block.
    """
    obj = UTIL_bme.create_bme_struct_wrapper('floor_flat', {
        'height_': 5.0, 'length_': length, 'width_': width,
        'face_': (True, False, False, False, False, False), 'is_sink_': False,
    })
    NC.VirtoolsGroupConvention.set_to_object(obj, Info.create_from_others(BT.FLOOR), None)
    scene().collection.objects.link(obj)
    obj.matrix_world = mathutils.Matrix.Translation((x0, y0, 0.0))
    obj.name = name
    return obj


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
    # retail rail section: radius 0.35, span 3.75; grows along local +Y
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
    reset()

    sector_count = (len(BAYS) + BAYS_PER_SECTOR - 1) // BAYS_PER_SECTOR
    x_first = 0.0
    x_last = (len(BAYS) - 1) * BAY_SPAN
    x_start = x_first - BAY_SPAN / 2
    x_end = x_last + BAY_SPAN / 2

    # --- floor: flat plates spanning the whole arena -----------------------
    plate_x0 = x_start - BAY_SPAN
    plate_x1 = x_end + BAY_SPAN
    n_plates = int(math.ceil((plate_x1 - plate_x0) / PLATE_SPAN))
    for i in range(n_plates):
        x0 = plate_x0 + i * PLATE_SPAN
        length = min(PLATE_SPAN, plate_x1 - x0)
        add_floor_plate(f'A_Floor_{i + 1:02d}', x0, LANE_Y - ARENA_WIDTH / 2,
                        length, ARENA_WIDTH)

    # --- level start + its resetpoint --------------------------------------
    add_component('PS_FourFlames', 1, (x_start, LANE_Y, MOUNT_DZ['PS_FourFlames']),
                  90.0, 'PS_FourFlames_01')
    # the start pair sits 3.65 up, not 3.326 - that is how the retail start reads
    add_component('PR_Resetpoint', 1, (x_start, LANE_Y, 3.65), 180.0, 'PR_Resetpoint_01')

    # --- one mechanism per bay ---------------------------------------------
    counts = {}
    for i, type_name in enumerate(BAYS):
        sector = i // BAYS_PER_SECTOR + 1
        counts[type_name] = counts.get(type_name, 0) + 1
        add_component(type_name, sector,
                      (i * BAY_SPAN, LANE_Y, MOUNT_DZ[type_name]), 0.0,
                      f'{type_name}_{counts[type_name]:02d}')

    # --- checkpoint + resetpoint on each sector boundary -------------------
    for s in range(2, sector_count + 1):
        boundary = (s - 1) * BAYS_PER_SECTOR * BAY_SPAN - BAY_SPAN / 2
        add_component('PC_TwoFlames', s - 1, (boundary, LANE_Y, MOUNT_DZ['PC_TwoFlames']),
                      0.0, f'PC_TwoFlames_{s - 1:02d}')
        add_component('PR_Resetpoint', s, (boundary, LANE_Y, MOUNT_DZ['PR_Resetpoint']),
                      180.0, f'PR_Resetpoint_{s:02d}')

    # --- rails: a side lane so the test exercises Phys_FloorRails ----------
    add_rail('A_Rail_01', x_first, LANE_Y - 20.0, 160.0)

    # --- level end ---------------------------------------------------------
    add_component('PE_Balloon', sector_count, (x_end, LANE_Y, MOUNT_DZ['PE_Balloon']),
                  -180.0, 'PE_Balloon_01')

    info = PROP_ballance_map_info.get_raw_ballance_map_info(scene())
    info.mSectorCount = sector_count
    PROP_ballance_map_info.set_raw_ballance_map_info(scene(), info)

    return sector_count


def inventory():
    groups = {}
    objs = []
    for ob in bpy.data.objects:
        if ob.type != 'MESH':
            continue
        with PROP_virtools_group.VirtoolsGroupsHelper(ob) as gp:
            gs = sorted(gp.iterate_groups())
        for g in gs:
            groups[g] = groups.get(g, 0) + 1
        loc = ob.matrix_world.translation
        objs.append({'name': ob.name, 'groups': gs, 'polys': len(ob.data.polygons),
                     'loc': [round(loc.x, 3), round(loc.y, 3), round(loc.z, 3)]})
    return {'groups': groups, 'objects': objs}
