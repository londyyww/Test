"""Preview renders for the KS-74 blockout (Cycles, CPU).

    python render_previews.py            # or: blender -b -P render_previews.py

Writes previews/*.png. `side_left_ref.png` uses the exact framing of ../concept/ref_side_a.png
(2081x756) so the two can be overlaid to check proportions.
"""
import math
import os

import bpy
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "previews")
os.makedirs(OUT, exist_ok=True)
bpy.ops.wm.open_mainfile(filepath=os.path.join(HERE, "KS74_Zarnitsa_Blockout.blend"))
scene = bpy.context.scene

K = 0.943 / 2002.0
OX, OY = 650.0, 208.0

scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = 48
scene.cycles.use_denoising = True
scene.render.film_transparent = False
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"

world = bpy.data.worlds.new("Studio")
scene.world = world
if world.node_tree is None:
    world.use_nodes = True
bg = world.node_tree.nodes["Background"]
bg.inputs["Color"].default_value = (0.018, 0.019, 0.021, 1)
bg.inputs["Strength"].default_value = 1.0

CENTER = Vector(((1040 - OX) * K, 0.0, (OY - 380) * K))


def light(name, loc, energy, size, color=(1, 1, 1)):
    d = bpy.data.lights.new(name, "AREA")
    d.energy = energy
    d.size = size
    d.color = color
    o = bpy.data.objects.new(name, d)
    scene.collection.objects.link(o)
    o.location = loc
    look(o, CENTER)
    return o


def look(ob, target):
    ob.rotation_euler = (Vector(target) - ob.location).to_track_quat("-Z", "Y").to_euler()


light("Key", CENTER + Vector((0.4, 1.2, 1.0)), 70, 1.0, (1.0, 0.97, 0.93))
light("KeyR", CENTER + Vector((0.3, -1.2, 1.0)), 45, 1.0, (1.0, 0.97, 0.93))
light("Fill", CENTER + Vector((-1.2, 0.6, -0.2)), 15, 1.5, (0.85, 0.9, 1.0))
light("Rim", CENTER + Vector((0.2, 0.0, 1.4)), 40, 0.8)

cam_data = bpy.data.cameras.new("Cam")
cam = bpy.data.objects.new("Cam", cam_data)
scene.collection.objects.link(cam)
scene.camera = cam


def render(name, w, h):
    scene.render.resolution_x, scene.render.resolution_y = w, h
    scene.render.filepath = os.path.join(OUT, name + ".png")
    bpy.ops.render.render(write_still=True)
    print("rendered", name)


def ortho(side):
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = 2081 * K
    cx, cz = (1040.5 - OX) * K, (OY - 378) * K
    cam.location = (cx, 2.0 if side == "left" else -2.0, cz)
    cam.rotation_euler = (math.radians(90), 0, math.radians(180 if side == "left" else 0))


def persp(offset, lens=55):
    cam_data.type = "PERSP"
    cam_data.lens = lens
    cam.location = CENTER + Vector(offset)
    look(cam, CENTER + Vector((0, 0, 0.01)))


ortho("left")
render("side_left_ref", 2081, 756)
ortho("right")
render("side_right", 2081, 756)
persp((0.75, 1.55, 0.5), 50)
render("hero_front_left", 1920, 1080)
persp((-0.95, -1.45, 0.45), 50)
render("rear_right", 1920, 1080)

# exploded view: modular parts pulled off along their swap direction
moves = {"KS74_Magazine": (0.03, 0, -0.13), "KS74_Grip": (-0.02, 0, -0.1), "KS74_Stock": (-0.16, 0, 0),
         "KS74_MuzzleBrake": (0.12, 0, 0), "KS74_DustCover": (0, 0, 0.09), "KS74_Handguard_Upper": (0, 0, 0.1),
         "KS74_ClampRail": (0, 0, -0.12), "KS74_SideRail": (0, 0.09, 0.0)}
for n, d in moves.items():
    bpy.data.objects[n].location += Vector(d)
persp((0.6, 1.9, 0.6), 50)
render("exploded", 1920, 1080)
