"""Export a binary glTF to FBX, inside Blender.

    blender --background --factory-startup --python launcher/tools/gltf/glb_to_fbx.py -- IN.glb OUT.fbx [--fps N]

Imports the .glb with its animations as actions and writes an FBX with
Blender's defaults (centimetres, Y up), the form an FBX arrives in. The
animations are keyed on whole frames of --fps (default 30), so pass the rate
the .glb was keyed at or its last key is lost. The bone shapes Blender's
importer adds are not exported. It makes the FBX fixtures the converter's
round-trip tests read; the .glb is only read.
"""
import argparse
import sys

import bpy

DEFAULT_FPS = 30


def arguments():
    parser = argparse.ArgumentParser(prog="glb_to_fbx.py", description=__doc__.split("\n\n")[0])
    parser.add_argument("glb")
    parser.add_argument("fbx")
    parser.add_argument("--fps", type=int, default=DEFAULT_FPS)
    return parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])


def drop_bone_shapes():
    shapes = set()
    for obj in bpy.data.objects:
        if obj.type == "ARMATURE":
            for bone in obj.pose.bones:
                if bone.custom_shape:
                    shapes.add(bone.custom_shape)
                    bone.custom_shape = None
    for shape in shapes:
        bpy.data.objects.remove(shape)


def main():
    args = arguments()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.render.fps = args.fps
    bpy.ops.import_scene.gltf(filepath=args.glb)
    drop_bone_shapes()
    bpy.ops.export_scene.fbx(filepath=args.fbx, add_leaf_bones=False)


main()
