"""KS-74 «Зарница» (LAST LIGHT) — modular blockout generator.

Run with Blender 5.x:
    blender -b -P build_blockout.py
or with the `bpy` Python module:
    python build_blockout.py

Writes next to this file:
    KS74_Zarnitsa_Blockout.blend   editable source (live bevel modifiers, quads/ngons)
    KS74_Zarnitsa_Blockout.fbx     Unreal import (triangulated, modifiers applied, SOCKET_ empties)
    KS74_Zarnitsa_Blockout.glb     quick viewing
    KS74_Zarnitsa_parts.json       part list with sizes, pivots and sockets

All side-profile coordinates are pixels measured on ../concept/ref_side_a.png
(2081x756). 2002 px (butt to muzzle) = 943 mm, the AK-74 overall length.
Axes: +X muzzle (Unreal forward), +Z up, +Y = rifle's left side (side rail, stock plate).
"""
import json
import math
import os
import sys

import bpy
import bmesh
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
NAME = "KS74_Zarnitsa_Blockout"

K = 0.943 / 2002.0        # metres per reference pixel
OX, OY = 650.0, 208.0     # root origin in reference px: bore axis directly above the pistol grip
MM = 0.001


def X(px):
    return (px - OX) * K


def Z(py):
    return (OY - py) * K


# ----------------------------------------------------------------------------- scene
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.unit_settings.system = "METRIC"
scene.unit_settings.scale_length = 1.0
scene.unit_settings.length_unit = "MILLIMETERS"
COL = bpy.data.collections.new("KS74_Blockout")
scene.collection.children.link(COL)
CUT = bpy.data.collections.new("_cutters")
scene.collection.children.link(CUT)

# ----------------------------------------------------------------------------- materials
MATS = {}


def material(key, rgb, metal, rough):
    m = bpy.data.materials.new("M_KS74_" + key)
    if m.node_tree is None:
        m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*rgb, 1.0)
    bsdf.inputs["Metallic"].default_value = metal
    bsdf.inputs["Roughness"].default_value = rough
    m.diffuse_color = (*rgb, 1.0)
    m.metallic = metal
    m.roughness = rough
    MATS[key] = m


material("Steel", (0.045, 0.048, 0.052), 1.0, 0.42)
material("SteelWorn", (0.16, 0.165, 0.155), 0.85, 0.55)
material("SteelBare", (0.42, 0.43, 0.44), 1.0, 0.3)
material("PaintOD", (0.12, 0.125, 0.07), 0.0, 0.72)
material("PolymerOD", (0.16, 0.19, 0.09), 0.0, 0.6)
material("PolymerBlack", (0.025, 0.025, 0.027), 0.0, 0.55)
material("PolymerMag", (0.17, 0.055, 0.04), 0.0, 0.55)
material("Wood", (0.17, 0.065, 0.025), 0.0, 0.5)
material("Tape", (0.012, 0.012, 0.013), 0.0, 0.45)
material("Cloth", (0.34, 0.30, 0.22), 0.0, 0.95)
material("Cord", (0.07, 0.08, 0.05), 0.0, 0.9)

# ----------------------------------------------------------------------------- bmesh builders


def add_prism(bm, pts, y0, y1, taper=None):
    """Extrude a side-view polygon (reference px) across y0..y1 metres.
    taper=(x_px_a, width_mm_a, x_px_b, width_mm_b) overrides the width along X."""
    yc = (y0 + y1) / 2
    lo, hi = [], []
    for x, y in pts:
        if taper:
            xa, wa, xb, wb = taper
            t = min(1.0, max(0.0, (x - xa) / (xb - xa)))
            h = (wa + (wb - wa) * t) * MM / 2
            a, b = yc - h, yc + h
        else:
            a, b = y0, y1
        lo.append(bm.verts.new((X(x), a, Z(y))))
        hi.append(bm.verts.new((X(x), b, Z(y))))
    bm.faces.new(lo[::-1])
    bm.faces.new(hi)
    n = len(pts)
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new((lo[i], lo[j], hi[j], hi[i]))


def add_box(bm, x0, x1, py0, py1, y0, y1):
    add_prism(bm, [(x0, py0), (x1, py0), (x1, py1), (x0, py1)], y0, y1)


def add_lathe(bm, prof, zc_px, yc=0.0, segs=32):
    """Solid of revolution about an X-parallel axis; prof = [(x_px, radius_mm), ...]."""
    rings = []
    for xpx, r in prof:
        rings.append([bm.verts.new((X(xpx), yc + r * MM * math.cos(a), Z(zc_px) + r * MM * math.sin(a)))
                      for a in (2 * math.pi * i / segs for i in range(segs))])
    for a, b in zip(rings, rings[1:]):
        for i in range(segs):
            j = (i + 1) % segs
            bm.faces.new((a[i], a[j], b[j], b[i]))
    bm.faces.new(rings[0])
    bm.faces.new(rings[-1])


def add_cyl(bm, center, axis, length, r, segs=16):
    rot = {"X": Matrix.Rotation(math.pi / 2, 4, "Y"), "Y": Matrix.Rotation(math.pi / 2, 4, "X"),
           "Z": Matrix.Identity(4)}[axis]
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segs, radius1=r, radius2=r,
                          depth=length, matrix=Matrix.Translation(center) @ rot)


def rounded_rect(yh, zt, zb, r, n=5):
    """CCW outline in the (Y, Z) plane, metres."""
    pts = []
    for cy, cz, a0 in ((yh - r, zb + r, -90), (yh - r, zt - r, 0), (-yh + r, zt - r, 90), (-yh + r, zb + r, 180)):
        for k in range(n + 1):
            a = math.radians(a0 + 90 * k / n)
            pts.append((cy + r * math.cos(a), cz + r * math.sin(a)))
    return pts


def u_path(yh, zt, zb, r, n=5):
    """Open U (left top -> bottom -> right top), CCW sense, for wraps tucked under the upper handguard."""
    pts = [(-yh, zt)]
    for cy, cz, a0 in ((-yh + r, zb + r, 180), (yh - r, zb + r, 270)):
        for k in range(n + 1):
            a = math.radians(a0 + 90 * k / n)
            pts.append((cy + r * math.cos(a), cz + r * math.sin(a)))
    pts.append((yh, zt))
    return pts


def add_band(bm, outline, closed, x0, x1, thick):
    """Thin shell following an outline in the (Y, Z) plane between x0..x1 px (clamps, tape wraps)."""
    n = len(outline)

    def normal(i):
        p, q = outline[(i - 1) % n], outline[(i + 1) % n]
        if not closed and i == 0:
            p = outline[0]
        if not closed and i == n - 1:
            q = outline[-1]
        dy, dz = q[0] - p[0], q[1] - p[1]
        L = math.hypot(dy, dz)
        return dz / L, -dy / L

    loops = {}
    for side, xx in (("a", X(x0)), ("b", X(x1))):
        inner, outer = [], []
        for i, (y, z) in enumerate(outline):
            ny, nz = normal(i)
            inner.append(bm.verts.new((xx, y, z)))
            outer.append(bm.verts.new((xx, y + ny * thick, z + nz * thick)))
        loops[side] = (inner, outer)
    (ia, oa), (ib, ob) = loops["a"], loops["b"]
    segs = range(n) if closed else range(n - 1)
    for i in segs:
        j = (i + 1) % n
        bm.faces.new((oa[i], oa[j], ob[j], ob[i]))
        bm.faces.new((ia[j], ia[i], ib[i], ib[j]))
        bm.faces.new((ia[i], ia[j], oa[j], oa[i]))
        bm.faces.new((ib[j], ib[i], ob[i], ob[j]))
    if not closed:
        for k in (0, n - 1):
            bm.faces.new((ia[k], oa[k], ob[k], ib[k]))


def strip(pts, w):
    """Polygon of constant width w (px) around a polyline (px)."""
    left, right = [], []
    for i, (x, y) in enumerate(pts):
        a = pts[max(i - 1, 0)]
        b = pts[min(i + 1, len(pts) - 1)]
        dx, dy = b[0] - a[0], b[1] - a[1]
        L = math.hypot(dx, dy)
        nx, ny = -dy / L, dx / L
        left.append((x + nx * w / 2, y + ny * w / 2))
        right.append((x - nx * w / 2, y - ny * w / 2))
    return left + right[::-1]


def rail_teeth(bm, x0, x1, add_tooth, pitch_px=10.01 * MM / K, tooth_mm=4.68):
    """Picatinny teeth along X (10.01 mm pitch); add_tooth(bm, x_start_px, x_end_px) builds one tooth."""
    n = int((x1 - x0) // pitch_px)
    tooth = tooth_mm * MM / K
    xs = []
    for i in range(n):
        a = x0 + i * pitch_px + (pitch_px - tooth) / 2
        add_tooth(bm, a, a + tooth)
        xs.append(a + tooth / 2)
    return xs, pitch_px


def box_uv(me, scale=10.0):
    """Per-face box projection so the blockout imports with valid UVs (triplanar-style)."""
    uv = me.uv_layers.new(name="UVMap")
    for poly in me.polygons:
        ax = max(range(3), key=lambda i: abs(poly.normal[i]))
        u, v = [(1, 2), (0, 2), (0, 1)][ax]
        for li in poly.loop_indices:
            co = me.vertices[me.loops[li].vertex_index].co
            uv.data[li].uv = (co[u] * scale, co[v] * scale)


# ----------------------------------------------------------------------------- object assembly
PARTS = []


def make(name, bm, mat, bevel_mm=0.8, segs=2, origin=None, parent=None, cutters=(), angle=40, note=""):
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    o = Vector(origin) if origin else Vector((0, 0, 0))
    me.transform(Matrix.Translation(-o))
    ob = bpy.data.objects.new(name, me)
    COL.objects.link(ob)
    ob.location = o
    me.materials.append(MATS[mat])
    for c in cutters:
        mod = ob.modifiers.new("cut", "BOOLEAN")
        mod.operation = "DIFFERENCE"
        mod.solver = "EXACT"
        mod.object = c
        bpy.context.view_layer.update()
        ev = ob.evaluated_get(bpy.context.evaluated_depsgraph_get())
        new = bpy.data.meshes.new_from_object(ev)
        ob.modifiers.remove(mod)
        old, ob.data = ob.data, new
        bpy.data.meshes.remove(old)
        new.name = name
    me = ob.data
    box_uv(me)
    me.shade_smooth()
    me.set_sharp_from_angle(angle=math.radians(angle))
    if bevel_mm:
        bv = ob.modifiers.new("Bevel", "BEVEL")
        bv.width = bevel_mm * MM
        bv.segments = segs
        bv.limit_method = "ANGLE"
        bv.angle_limit = math.radians(angle)
        bv.use_clamp_overlap = True
    p = parent or ROOT
    ob.parent = p
    ob.matrix_parent_inverse = Matrix.Translation(-p.location) if p is not ROOT else Matrix.Identity(4)
    PARTS.append((ob, note))
    return ob


def cutter(name, build):
    bm = bmesh.new()
    build(bm)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    CUT.objects.link(ob)
    return ob


def P3(xpx, ypx, y_mm=0.0):
    return (X(xpx), y_mm * MM, Z(ypx))


ROOT = bpy.data.objects.new("KS74_Root", None)
ROOT.empty_display_type = "ARROWS"
ROOT.empty_display_size = 0.05
COL.objects.link(ROOT)

RCV_H = 15.0  # receiver half width, mm

# --- receiver + rivets (both sides) and the hex bolt that replaced a lost rivet
port_cut = cutter("cut_port", lambda bm: add_box(bm, 832, 998, 154, 184, -25 * MM, -11 * MM))
bm = bmesh.new()
add_prism(bm, [(540, 150), (1136, 150), (1136, 266), (560, 266), (540, 258)], -RCV_H * MM, RCV_H * MM)
for x, y in [(566, 180), (735, 245), (990, 242), (1090, 175), (1105, 240), (1120, 200)]:
    add_cyl(bm, P3(x, y, RCV_H + 0.4), "Y", 1.6 * MM, 2.0 * MM, 12)
for x, y in [(566, 180), (566, 240), (735, 245), (990, 242), (1090, 200), (1105, 240)]:
    add_cyl(bm, P3(x, y, -RCV_H - 0.4), "Y", 1.6 * MM, 2.0 * MM, 12)
add_cyl(bm, P3(566, 240, RCV_H + 1.0), "Y", 2.6 * MM, 2.8 * MM, 6)
receiver = make("KS74_Receiver", bm, "Steel", 0.8, cutters=[port_cut],
                note="Stamped receiver, factory 2031. Ejection port cut on the right (-Y).")

# --- dust cover (green repaint) + welded top rail
bm = bmesh.new()
add_prism(bm, [(538, 152), (545, 128), (562, 106), (588, 94), (1132, 93), (1132, 152)], -16 * MM, 16 * MM)
dust = make("KS74_DustCover", bm, "PaintOD", 6.0, 4, origin=P3(1132, 152),
            note="Owner I repaint. Pivot at the front lock into the rear-sight block.")
bm = bmesh.new()
add_box(bm, 598, 956, 84, 96, -7.8 * MM, 7.8 * MM)
rail_teeth(bm, 598, 956, lambda b, a, c: add_box(b, a, c, 72, 84, -10.6 * MM, 10.6 * MM))
for x in (604, 950):
    add_box(bm, x - 5, x + 5, 92, 99, -9 * MM, 9 * MM)  # weld beads
make("KS74_DustCover_TopRail", bm, "Steel", 0.4, parent=dust,
     note="Welded Picatinny strip, 10 mm slot pitch. Socket Optic_Top.")

# --- rear sight
bm = bmesh.new()
add_prism(bm, [(1128, 152), (1128, 104), (1138, 96), (1226, 96), (1240, 120), (1244, 152)], -13 * MM, 13 * MM)
add_box(bm, 1140, 1222, 84, 97, -9 * MM, 9 * MM)
add_box(bm, 1170, 1190, 79, 99, -11 * MM, 11 * MM)
make("KS74_RearSight", bm, "Steel", 0.6, note="AK-74 leaf sight block.")

# --- left side-mount rail (bolted)
bm = bmesh.new()
add_box(bm, 676, 1024, 166, 196, RCV_H * MM, 19.5 * MM)
xs, pitch = rail_teeth(bm, 680, 1024, lambda b, a, c: add_box(b, a, c, 158, 204, 19.5 * MM, 23.5 * MM))
for tx in (xs[1], xs[len(xs) // 2], xs[-2]):
    add_cyl(bm, P3(tx + pitch / 2, 181, 21.5), "Y", 3.5 * MM, 3.0 * MM, 6)
make("KS74_SideRail", bm, "Steel", 0.4, note="Bolted left-side Picatinny. Socket Optic_Side.")

# --- bolt carrier (visible in the port) + charging handle, right side
bm = bmesh.new()
add_box(bm, 832, 1000, 155, 183, -12 * MM, 4 * MM)
add_box(bm, 1004, 1048, 152, 168, -24 * MM, -10 * MM)
add_cyl(bm, P3(1036, 160, -28), "Y", 12 * MM, 6.5 * MM, 16)
make("KS74_BoltCarrier", bm, "SteelWorn", 0.6, origin=P3(1036, 160, -28),
     note="Bolt carrier + charging handle (right). Animate along -X ~ 125 mm travel.")

# --- selector (right)
bm = bmesh.new()
add_prism(bm, [(588, 196), (598, 180), (864, 168), (868, 180), (612, 192), (606, 222), (588, 222)],
          -(RCV_H + 2.2) * MM, -(RCV_H + 0.2) * MM)
make("KS74_Selector", bm, "SteelBare", 0.4, origin=P3(596, 200, -RCV_H - 1),
     note="AK selector lever, right side. Pivot at its rear axle (rotate about Y).")

# --- fire control group
bm = bmesh.new()
add_prism(bm, strip([(706, 260), (712, 330), (728, 347), (842, 347), (858, 330), (862, 260)], 8), -6 * MM, 6 * MM)
make("KS74_TriggerGuard", bm, "Steel", 0.5)
bm = bmesh.new()
add_prism(bm, [(770, 266), (768, 296), (760, 322), (766, 326), (780, 300), (783, 266)], -3.5 * MM, 3.5 * MM)
make("KS74_Trigger", bm, "SteelBare", 0.4, origin=P3(776, 268), note="Pivot at trigger pin.")
bm = bmesh.new()
add_prism(bm, [(864, 264), (878, 264), (876, 362), (862, 356)], -7 * MM, 7 * MM)
make("KS74_MagCatch", bm, "Steel", 0.5)

# --- pistol grip + tape wrap
bm = bmesh.new()
add_prism(bm, [(598, 264), (712, 264), (702, 305), (645, 505), (632, 520), (545, 506), (527, 489)], -15 * MM, 15 * MM)
grip = make("KS74_Grip", bm, "PolymerBlack", 5.0, 3, origin=P3(650, 265),
            note="Swappable grip. Socket Grip at its top.")
bm = bmesh.new()
add_prism(bm, [(577, 320), (701, 320), (648, 508), (633, 524), (543, 509), (523, 490)], -16.6 * MM, 16.6 * MM)
make("KS74_Grip_Tape", bm, "Tape", 5.0, 3, parent=grip, note="Owner IV electrical-tape wrap.")

# --- stock group
bm = bmesh.new()
add_prism(bm, [(556, 156), (52, 163), (46, 172), (46, 436), (54, 446), (556, 270)], 0, 0, taper=(46, 42, 548, 32))
stock = make("KS74_Stock", bm, "Wood", 5.0, 3, origin=P3(548, 205),
             note="Fixed walnut stock (owner II). Swap point = rear trunnion, socket Stock.")
bm = bmesh.new()
add_prism(bm, [(48, 162), (34, 165), (30, 175), (30, 438), (36, 448), (48, 447)], -21.5 * MM, 21.5 * MM)
make("KS74_Stock_Buttplate", bm, "Steel", 2.0, parent=stock)
bm = bmesh.new()
add_prism(bm, [(98, 154), (354, 151), (354, 348), (98, 438)], 0, 0, taper=(98, 50, 354, 45))
make("KS74_Stock_Wrap", bm, "Cloth", 7.0, 3, parent=stock, note="Canvas cheek rag. Sculpt folds on top of this.")
bm = bmesh.new()
add_prism(bm, [(66, 158), (118, 157), (118, 441), (66, 445)], 0, 0, taper=(66, 47, 118, 46))
make("KS74_Stock_Cord", bm, "Cord", 4.0, 3, parent=stock, note="Cord lashing over the wrap.")


def stock_half(x):
    return (42 + (32 - 42) * (x - 46) / (548 - 46)) / 2


bm = bmesh.new()
yp = stock_half(470) + 0.8
add_prism(bm, [(388, 186), (545, 183), (548, 272), (392, 284)], (yp - 1) * MM, (yp + 1) * MM)
for x, y in [(402, 205), (530, 200), (405, 268), (532, 256)]:
    add_cyl(bm, P3(x, y, yp + 1.4), "Y", 1.4 * MM, 2.4 * MM, 6 if x > 500 else 12)
make("KS74_Stock_Plate", bm, "SteelWorn", 0.4, parent=stock, note="Cut steel repair plate, left side, mixed fasteners.")

# --- magazine group (pivot = front locking lug, AK rock-in)
bm = bmesh.new()
add_prism(bm, [(1006, 262), (1030, 350), (1052, 430), (1080, 510), (1112, 575), (1146, 606),
               (1032, 702), (992, 640), (958, 560), (928, 470), (902, 380), (878, 262)], -13 * MM, 13 * MM)
mag = make("KS74_Magazine", bm, "PolymerMag", 2.0, origin=P3(1006, 266),
           note="30-rd 5.45 polymer mag. Origin at the front lug for the rock-in reload.")
bm = bmesh.new()
add_prism(bm, [(1140, 600), (1158, 612), (1040, 712), (1026, 700)], -14 * MM, 14 * MM)
make("KS74_Magazine_Floorplate", bm, "SteelWorn", 1.0, parent=mag)
bm = bmesh.new()
add_prism(bm, [(945, 538), (1080, 495), (1099, 538), (964, 586)], -14.4 * MM, 14.4 * MM)
make("KS74_Magazine_Tape", bm, "Tape", 1.5, parent=mag, note="Tape band / pull tab.")

# --- handguards
bm = bmesh.new()
add_prism(bm, [(1134, 164), (1514, 164), (1520, 172), (1520, 250), (1508, 262), (1260, 276), (1168, 276), (1138, 262)],
          -25 * MM, 25 * MM)
hg_low = make("KS74_Handguard_Lower", bm, "Wood", 7.0, 3, origin=P3(1134, 220),
              note="Cracked walnut lower handguard (owner III). Socket Hand_L underneath.")
bm = bmesh.new()
for x0, x1 in ((1222, 1262), (1284, 1346)):
    add_band(bm, u_path(25.0 * MM, Z(167), Z(276), 7 * MM), False, x0, x1, 1.2 * MM)
make("KS74_Handguard_Lower_Tape", bm, "Tape", 0.3, parent=hg_low, note="Tape over the crack, ends tucked under the upper cover.")
bm = bmesh.new()
add_prism(bm, [(1204, 166), (1240, 104), (1256, 96), (1506, 96), (1516, 106), (1516, 166)], -22 * MM, 22 * MM)
hg_up = make("KS74_Handguard_Upper", bm, "PolymerOD", 9.0, 4, origin=P3(1204, 140),
             note="Olive polymer replacement upper cover; lifts off with the gas tube.")
bm = bmesh.new()
add_lathe(bm, [(1500, 10), (1622, 10), (1628, 8.5)], 140)
make("KS74_GasTube", bm, "Steel", 0.6, parent=hg_up)
bm = bmesh.new()
add_box(bm, 1506, 1528, 158, 264, -26 * MM, 26 * MM)
make("KS74_Handguard_Retainer", bm, "Steel", 2.0)

# --- barrel group
bm = bmesh.new()
add_lathe(bm, [(1130, 9), (1500, 9), (1500, 12), (1600, 12), (1600, 9), (1832, 9)], OY)
make("KS74_Barrel", bm, "Steel", 0.5, note="Visible barrel, receiver face to thread shoulder; 24x1.5 LH thread at the front.")
bm = bmesh.new()
add_prism(bm, [(1596, 118), (1632, 118), (1640, 128), (1688, 182), (1694, 232), (1600, 234)], -13 * MM, 13 * MM)
make("KS74_GasBlock", bm, "Steel", 1.5)
bm = bmesh.new()
add_box(bm, 1730, 1768, 182, 236, -13 * MM, 13 * MM)
add_box(bm, 1732, 1766, 232, 262, -7 * MM, 7 * MM)
add_box(bm, 1752, 1830, 166, 192, -11 * MM, 11 * MM)
add_prism(bm, [(1756, 170), (1762, 86), (1778, 86), (1776, 170)], -10 * MM, 10 * MM)
add_prism(bm, [(1806, 170), (1808, 86), (1824, 86), (1830, 170)], -10 * MM, 10 * MM)
add_box(bm, 1760, 1826, 76, 92, -10 * MM, 10 * MM)
add_cyl(bm, P3(1792, 135), "Z", 70 * K, 1.6 * MM, 10)
make("KS74_FrontSight", bm, "Steel", 1.0)
bm = bmesh.new()
add_lathe(bm, [(1522, 3), (1772, 3)], 252, segs=12)
make("KS74_CleaningRod", bm, "Steel", 0.0)

windows = cutter("cut_brake", lambda bm: (
    add_box(bm, 1948, 1994, 193, 223, -25 * MM, 25 * MM),
    add_box(bm, 1999, 2013, 196, 220, -25 * MM, 25 * MM),
    add_cyl(bm, P3(2030, OY), "X", 0.05, 4 * MM, 16)))
bm = bmesh.new()
add_lathe(bm, [(1828, 11), (1842, 11), (1842, 15), (2018, 15), (2040, 13)], OY)
make("KS74_MuzzleBrake", bm, "SteelWorn", 0.6, origin=P3(1832, OY), cutters=[windows],
     note="Two-chamber brake. Origin = thread shoulder = socket Muzzle (suppressor swap).")

# --- improvised accessory rail, hose clamps, flashlight
bm = bmesh.new()
add_prism(bm, [(1240, 275), (1416, 266), (1416, 290), (1240, 290)], -9 * MM, 9 * MM)
rail_teeth(bm, 1242, 1416, lambda b, a, c: add_box(b, a, c, 290, 298, -10.6 * MM, 10.6 * MM))
crail = make("KS74_ClampRail", bm, "Steel", 0.4, origin=P3(1328, 290),
             note="Owner III rail held by two hose clamps. Socket Underbarrel.")
bm = bmesh.new()
outline = rounded_rect(26 * MM, Z(94), Z(290), 10 * MM)
for x0, x1 in ((1262, 1284), (1388, 1408)):
    add_band(bm, outline, True, x0, x1, 1.2 * MM)
    add_box(bm, x0 - 3, x1 + 3, 196, 226, 26 * MM, 32 * MM)  # worm-gear housing
make("KS74_HoseClamps", bm, "SteelBare", 0.3, parent=crail)
bm = bmesh.new()
add_lathe(bm, [(1228, 6), (1236, 7), (1405, 7), (1410, 8.2), (1440, 8.2)], 314, segs=24)
add_box(bm, 1300, 1356, 296, 306, -8 * MM, 8 * MM)
make("KS74_Flashlight", bm, "PolymerBlack", 0.6, parent=crail, note="Placeholder weapon light.")

# ----------------------------------------------------------------------------- sockets (UE: SOCKET_ prefix, child of a mesh)
SOCKETS = {
    "Optic_Top": (P3(777, 72), (0, 0, 0)),
    "Optic_Side": (P3(850, 181, 23.5), (math.pi / 2, 0, 0)),
    "Muzzle": (P3(1832, OY), (0, 0, 0)),
    "MuzzleFlash": (P3(2040, OY), (0, 0, 0)),
    "Underbarrel": (P3(1328, 298), (math.pi, 0, 0)),
    "Tactical_L": (P3(1380, 220, 25), (math.pi / 2, 0, 0)),
    "Magazine": (P3(1006, 266), (0, 0, 0)),
    "Stock": (P3(548, 205), (0, 0, 0)),
    "Grip": (P3(650, 265), (0, 0, 0)),
    "Hand_L": (P3(1330, 278), (0, 0, 0)),
    "Eject": (P3(915, 168, -16), (-math.pi / 2, 0, 0)),
}
for sname, (loc, rot) in SOCKETS.items():
    e = bpy.data.objects.new("SOCKET_" + sname, None)
    e.empty_display_type = "SINGLE_ARROW"
    e.empty_display_size = 0.03
    e.location = loc
    e.rotation_euler = rot
    COL.objects.link(e)
    e.parent = receiver

# cutters are baked into the meshes; drop them from the file
for c in list(CUT.objects):
    bpy.data.objects.remove(c)
bpy.data.collections.remove(CUT)

# ----------------------------------------------------------------------------- report + export
bpy.context.view_layer.update()
dg = bpy.context.evaluated_depsgraph_get()
lo, hi = Vector((1e9,) * 3), Vector((-1e9,) * 3)
parts = []
tris = 0
for ob, note in PARTS:
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    pts = [ob.matrix_world @ v.co for v in me.vertices]
    tris += sum(len(p.vertices) - 2 for p in me.polygons)
    a = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    b = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    ev.to_mesh_clear()
    lo = Vector(map(min, lo, a))
    hi = Vector(map(max, hi, b))
    parts.append({"name": ob.name, "parent": ob.parent.name, "material": ob.data.materials[0].name,
                  "pivot_mm": [round(c * 1000, 1) for c in ob.matrix_world.translation],
                  "size_mm": [round((b[i] - a[i]) * 1000, 1) for i in range(3)], "note": note})
size = [round((hi[i] - lo[i]) * 1000, 1) for i in range(3)]
report = {
    "asset": NAME, "units": "mm in this report; Blender scene in metres (1 Blender m = 100 UE units)",
    "axes": "+X muzzle (UE forward), +Z up, +Y rifle-left (UE -Y after FBX import)",
    "origin": "KS74_Root = bore axis directly above the pistol grip",
    "overall_size_mm": size, "triangles_with_bevels": tris,
    "sockets": {k: [round(c * 1000, 1) for c in v[0]] for k, v in SOCKETS.items()},
    "parts": parts,
}
with open(os.path.join(HERE, "KS74_Zarnitsa_parts.json"), "w", encoding="utf-8") as f:
    json.dump(report, f, ensure_ascii=False, indent=2)

bpy.ops.wm.save_as_mainfile(filepath=os.path.join(HERE, NAME + ".blend"), compress=True)
bpy.ops.export_scene.fbx(filepath=os.path.join(HERE, NAME + ".fbx"), object_types={"EMPTY", "MESH"},
                         use_mesh_modifiers=True, mesh_smooth_type="FACE", use_triangles=True,
                         add_leaf_bones=False, apply_unit_scale=True, bake_space_transform=False)
bpy.ops.export_scene.gltf(filepath=os.path.join(HERE, NAME + ".glb"), export_format="GLB", export_apply=True)
print(f"overall {size} mm, {len(parts)} parts, {tris} tris")
