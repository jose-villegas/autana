"""Export a skinned model from a .blend to a binary glTF, inside Blender.

    blender --background --factory-startup --python launcher/tools/gltf/blend_skin_to_glb.py -- \\
        IN.blend OUT.glb [--armature NAME] [--clips A,B,...] [--fps N] [--influences N]

Exports the armature (the file's only one, or --armature) and every mesh its
Armature modifier deforms: deform bones only, modifiers applied, every action
in --clips (all of the file's when left out, by name) baked at --fps as its own
glTF animation, at most --influences joints per vertex, no materials, cameras
or lights. The .blend is only read.
"""
import argparse
import sys

import bpy


def arguments():
    parser = argparse.ArgumentParser(prog="blend_skin_to_glb.py", description=__doc__.split("\n\n")[0])
    parser.add_argument("blend")
    parser.add_argument("out")
    parser.add_argument("--armature", help="the armature to export; needed when the file has more than one")
    parser.add_argument("--clips", help="comma-separated action names, in the order to write them")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--influences", type=int, default=4)
    return parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])


def armature(name):
    rigs = [obj for obj in bpy.data.objects if obj.type == "ARMATURE"]
    if name:
        rigs = [obj for obj in rigs if obj.name == name]
    if len(rigs) != 1:
        found = ", ".join(obj.name for obj in bpy.data.objects if obj.type == "ARMATURE") or "none"
        sys.exit(f"name one armature with --armature; the file has: {found}")
    return rigs[0]


def skinned_meshes(rig):
    return [obj for obj in bpy.data.objects if obj.type == "MESH" and any(
        modifier.type == "ARMATURE" and modifier.object == rig for modifier in obj.modifiers)]


def clips(names):
    if not names:
        return sorted(bpy.data.actions, key=lambda action: action.name)
    missing = [name for name in names.split(",") if name not in bpy.data.actions]
    if missing:
        sys.exit(f"no action {', '.join(missing)}; the file has: {', '.join(a.name for a in bpy.data.actions)}")
    return [bpy.data.actions[name] for name in names.split(",")]


def main():
    args = arguments()
    bpy.ops.wm.open_mainfile(filepath=args.blend)
    bpy.context.scene.render.fps = args.fps
    rig = armature(args.armature)
    meshes = skinned_meshes(rig)
    if not meshes:
        sys.exit(f"no mesh is deformed by {rig.name}")
    # Every clip as a muted NLA track, so the exporter writes one animation per action.
    if rig.animation_data is None:
        rig.animation_data_create()
    rig.animation_data.action = None
    for track in list(rig.animation_data.nla_tracks):
        rig.animation_data.nla_tracks.remove(track)
    for action in clips(args.clips):
        track = rig.animation_data.nla_tracks.new()
        track.name = action.name
        track.strips.new(action.name, int(action.frame_range[0]), action)
        track.mute = True
    for obj in bpy.data.objects:
        obj.select_set(obj == rig or obj in meshes)
    bpy.ops.export_scene.gltf(
        filepath=args.out, export_format="GLB", use_selection=True,
        export_apply=True, export_def_bones=True, export_animation_mode="NLA_TRACKS",
        export_force_sampling=True, export_frame_step=1, export_optimize_animation_size=False,
        export_anim_slide_to_zero=True, export_reset_pose_bones=True,
        export_skins=True, export_influence_nb=args.influences, export_all_influences=False,
        export_cameras=False, export_lights=False, export_materials="NONE")


main()
