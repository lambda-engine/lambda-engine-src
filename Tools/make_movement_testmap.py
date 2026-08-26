"""
Writes surf_test.vmf: a sealed dev room with a surf ramp and a func_ladder, then the map is compiled with the
Source SDK's own vbsp. It exists so the movement the engine ports - surfing along a too-steep slope, climbing a
ladder - has somewhere to be tested without hand-authoring a map in Hammer.

    python Tools/make_movement_testmap.py [out.vmf]

The room is 2048x2048 across and 768 tall inside. The west wall carries the surf ramp - its face runs at 60
degrees from the floor, so its normal's z is 0.5, well under the 0.7 a surface needs to be stood on. The east
wall carries a ladder up to a platform.
"""

import sys

MAT = "DEV/DEV_MEASUREGENERIC01"

_id = [0]


def nid():
    _id[0] += 1
    return _id[0]


def face(p1, p2, p3, material=MAT, uaxis="[1 0 0 0] 0.25", vaxis="[0 -1 0 0] 0.25"):
    def pt(p):
        return f"({p[0]} {p[1]} {p[2]})"
    return f'''\t\tside
\t\t{{
\t\t\t"id" "{nid()}"
\t\t\t"plane" "{pt(p1)} {pt(p2)} {pt(p3)}"
\t\t\t"material" "{material}"
\t\t\t"uaxis" "{uaxis}"
\t\t\t"vaxis" "{vaxis}"
\t\t\t"rotation" "0"
\t\t\t"lightmapscale" "16"
\t\t\t"smoothing_groups" "0"
\t\t}}
'''


def box(x1, y1, z1, x2, y2, z2, material=MAT):
    # The exact point pattern Hammer writes for an axis-aligned box: three points per face, clockwise seen from
    # outside the brush.
    faces = [
        face((x1, y2, z2), (x2, y2, z2), (x2, y1, z2), material),                                  # top
        face((x1, y1, z1), (x2, y1, z1), (x2, y2, z1), material),                                  # bottom
        face((x1, y2, z2), (x1, y1, z2), (x1, y1, z1), material, "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"),   # west
        face((x2, y2, z1), (x2, y1, z1), (x2, y1, z2), material, "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"),   # east
        face((x2, y2, z2), (x1, y2, z2), (x1, y2, z1), material, "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"),   # north
        face((x2, y1, z1), (x1, y1, z1), (x1, y1, z2), material, "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"),   # south
    ]
    return f'\tsolid\n\t{{\n\t\t"id" "{nid()}"\n' + "".join(faces) + "\t}\n"


def wedge_west_ramp(x_wall, x_foot, y1, y2, z_top, material=MAT):
    # A triangular prism against the west wall: its face rises from the floor at x_foot to z_top at the wall.
    # Five planes; the slope's points follow the same clockwise-from-outside rule as the box faces.
    faces = [
        face((x_wall, y1, 0), (x_foot, y1, 0), (x_foot, y2, 0), material),                          # bottom (down)
        face((x_wall, y2, z_top), (x_wall, y1, z_top), (x_wall, y1, 0), material,
             "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"),                                                  # back (west)
        face((x_wall, y2, z_top), (x_foot, y2, 0), (x_foot, y1, 0), material,
             "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"),                                                  # slope (up-east)
        face((x_foot, y2, 0), (x_wall, y2, z_top), (x_wall, y2, 0), material,
             "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"),                                                  # north cap
        face((x_wall, y1, z_top), (x_foot, y1, 0), (x_wall, y1, 0), material,
             "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"),                                                  # south cap
    ]
    return f'\tsolid\n\t{{\n\t\t"id" "{nid()}"\n' + "".join(faces) + "\t}\n"


def entity(classname, origin=None, extra=None, solids=""):
    body = f'\t"id" "{nid()}"\n\t"classname" "{classname}"\n'
    if origin:
        body += f'\t"origin" "{origin}"\n'
    for k, v in (extra or {}).items():
        body += f'\t"{k}" "{v}"\n'
    return "entity\n{\n" + body + solids + "}\n"


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "surf_test.vmf"

    # The room: interior -1024..1024 in x and y, floor at 0, ceiling at 768, walls 64 thick.
    world_solids = []
    world_solids.append(box(-1088, -1088, -64, 1088, 1088, 0))       # floor
    world_solids.append(box(-1088, -1088, 768, 1088, 1088, 832))     # ceiling
    world_solids.append(box(-1088, -1088, 0, -1024, 1088, 768))      # west wall
    world_solids.append(box(1024, -1088, 0, 1088, 1088, 768))        # east wall
    world_solids.append(box(-1024, 1024, 0, 1024, 1088, 768))        # north wall
    world_solids.append(box(-1024, -1088, 0, 1024, -1024, 768))      # south wall

    # The surf ramp: 400 wide at the foot, 692 tall at the wall - 60 degrees, normal.z = 0.5.
    world_solids.append(wedge_west_ramp(-1024, -624, -768, 768, 692))

    # The platform the ladder climbs to, on the east wall - stopping short of the ladder shaft, or the climber
    # bonks its underside on the way up, and running the ladder past the lip so there is ladder to hold above it.
    world_solids.append(box(832, -192, 368, 928, 192, 384))

    vmf = []
    vmf.append('versioninfo\n{\n\t"editorversion" "400"\n\t"editorbuild" "8000"\n\t"mapversion" "1"\n'
               '\t"formatversion" "100"\n\t"prefab" "0"\n}\n')
    vmf.append('visgroups\n{\n}\n')
    vmf.append('viewsettings\n{\n\t"bSnapToGrid" "1"\n\t"bShowGrid" "1"\n\t"nGridSpacing" "64"\n}\n')
    vmf.append('world\n{\n\t"id" "%d"\n\t"mapversion" "1"\n\t"classname" "worldspawn"\n'
               '\t"skyname" "sky_day01_01"\n' % nid() + "".join(world_solids) + "}\n")

    # The ladder: a thin func_ladder brush standing off the east wall, floor to platform.
    ladder = f'\tsolid\n\t{{\n\t\t"id" "{nid()}"\n' + "".join([
        face((1000, 64, 448), (1016, 64, 448), (1016, -64, 448)),
        face((1000, -64, 0), (1016, -64, 0), (1016, 64, 0)),
        face((1000, 64, 448), (1000, -64, 448), (1000, -64, 0), MAT, "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"),
        face((1016, 64, 0), (1016, -64, 0), (1016, -64, 448), MAT, "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"),
        face((1016, 64, 448), (1000, 64, 448), (1000, 64, 0), MAT, "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"),
        face((1016, -64, 0), (1000, -64, 0), (1000, -64, 448), MAT, "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"),
    ]) + "\t}\n"

    entities = []
    entities.append(entity("info_player_start", "0 0 1", {"angles": "0 0 0"}))
    entities.append(entity("light", "0 0 700", {"_light": "255 255 255 400"}))
    entities.append(entity("light", "-700 0 500", {"_light": "255 255 255 300"}))
    entities.append(entity("light", "900 0 500", {"_light": "255 255 255 300"}))
    entities.append(entity("func_ladder", solids=ladder))

    open(out, "w", encoding="ascii").write("".join(vmf) + "".join(entities))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
