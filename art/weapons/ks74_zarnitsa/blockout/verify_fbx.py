"""Round-trip check: import the exported FBX into an empty scene and report what Unreal would receive."""
import os
import bpy
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=os.path.join(HERE, "KS74_Zarnitsa_Blockout.fbx"))
meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
sockets = sorted(o.name for o in bpy.context.scene.objects if o.name.startswith("SOCKET_"))
pts = [o.matrix_world @ Vector(c) for o in meshes for c in o.bound_box]
size = [round((max(p[i] for p in pts) - min(p[i] for p in pts)) * 1000, 1) for i in range(3)]
mats = sorted({m.name for o in meshes for m in o.data.materials})
ngons = sum(1 for o in meshes for p in o.data.polygons if len(p.vertices) > 3)
print("FBX meshes:", len(meshes), "| size mm (X,Y,Z):", size, "| non-tri faces:", ngons)
print("sockets:", sockets, "| socket parents:", sorted({o.parent.name for o in bpy.context.scene.objects if o.name.startswith("SOCKET_")}))
print("materials:", mats)
