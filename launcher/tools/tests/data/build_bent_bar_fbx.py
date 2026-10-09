"""Build skinned_probe.fbx, the FBX the converter tests read, inside Blender.

    blender --background --factory-startup --python launcher/tools/tests/data/build_bent_bar_fbx.py -- OUT.fbx

A 0.2 m by 0.2 m by 2 m bar standing on the origin, skinned to two bones:
"base" (z 0 to 1) and "tip" (z 1 to 2, a child of base). The bar's lower half
is weighted to base, its upper half to tip, and the vertices around z 1 share
both. Five still bones, "extra0" to "extra4", each hold a hundredth of every
vertex, so each vertex has seven influences and the converter must keep four. The action "bend" turns tip 90 degrees about X over frames 1 to 30 at
30 fps. Exported with Blender's FBX defaults, so the file is in centimetres
with Y up, which the converter must bring back to metres.
"""
import math
import sys

import bpy
import bmesh

BAR_WIDTH = 0.2
BAR_HEIGHT = 2.0
SPLIT = BAR_HEIGHT / 2
BEND_DEGREES = 90
LAST_FRAME = 30
EXTRA_BONES = 5
EXTRA_WEIGHT = 0.01


def bar():
    mesh = bpy.data.meshes.new("bar")
    shape = bmesh.new()
    bmesh.ops.create_cube(shape, size=1.0)
    for vertex in shape.verts:
        vertex.co = (vertex.co.x * BAR_WIDTH, vertex.co.y * BAR_WIDTH, (vertex.co.z + 0.5) * BAR_HEIGHT)
    bmesh.ops.subdivide_edges(shape, edges=shape.edges[:], cuts=1, use_grid_fill=True)
    shape.to_mesh(mesh)
    shape.free()
    obj = bpy.data.objects.new("bar", mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def armature():
    data = bpy.data.armatures.new("rig")
    rig = bpy.data.objects.new("rig", data)
    bpy.context.collection.objects.link(rig)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    base = data.edit_bones.new("base")
    base.head, base.tail = (0, 0, 0), (0, 0, SPLIT)
    tip = data.edit_bones.new("tip")
    tip.head, tip.tail = (0, 0, SPLIT), (0, 0, BAR_HEIGHT)
    tip.parent = base
    for index in range(EXTRA_BONES):
        extra = data.edit_bones.new(f"extra{index}")
        extra.head, extra.tail = (0, 0, 0), (0, 0.1, 0)
        extra.parent = base
    bpy.ops.object.mode_set(mode="OBJECT")
    return rig


def skin(obj, rig):
    base, tip = obj.vertex_groups.new(name="base"), obj.vertex_groups.new(name="tip")
    extras = [obj.vertex_groups.new(name=f"extra{index}") for index in range(EXTRA_BONES)]
    main = 1.0 - EXTRA_BONES * EXTRA_WEIGHT
    for vertex in obj.data.vertices:
        along = vertex.co.z / BAR_HEIGHT
        upper = min(max(along * 2 - 0.5, 0.0), 1.0)  # 0 below a quarter, 1 above three quarters
        base.add([vertex.index], main * (1.0 - upper), "REPLACE")
        tip.add([vertex.index], main * upper, "REPLACE")
        for group in extras:
            group.add([vertex.index], EXTRA_WEIGHT, "REPLACE")
    obj.parent = rig
    obj.modifiers.new("Armature", "ARMATURE").object = rig


def bend(rig):
    bpy.context.scene.render.fps = 30
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="POSE")
    pose = rig.pose.bones["tip"]
    pose.rotation_mode = "XYZ"
    for frame, degrees in ((1, 0), (LAST_FRAME, BEND_DEGREES)):
        pose.rotation_euler = (math.radians(degrees), 0, 0)
        pose.keyframe_insert("rotation_euler", frame=frame)
    bpy.ops.object.mode_set(mode="OBJECT")
    rig.animation_data.action.name = "bend"


def main():
    out = sys.argv[sys.argv.index("--") + 1]
    bpy.ops.wm.read_factory_settings(use_empty=True)
    obj, rig = bar(), armature()
    skin(obj, rig)
    bend(rig)
    bpy.context.scene.frame_start, bpy.context.scene.frame_end = 1, LAST_FRAME
    bpy.ops.export_scene.fbx(filepath=out, add_leaf_bones=False)


main()
