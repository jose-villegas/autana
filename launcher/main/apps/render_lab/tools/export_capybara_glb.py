"""Export ../assets/capybara.blend to ../assets/capybara.glb, inside Blender.

    blender --background --factory-startup --python launcher/main/apps/render_lab/tools/export_capybara_glb.py

Deform bones only, modifiers applied, every action baked at 30 fps as its own
glTF animation, four influences per vertex, no materials. The .blend is only
read.
"""
from pathlib import Path

import bpy

ASSETS = Path(__file__).resolve().parents[1] / "assets"
CLIPS = ("idle", "walk", "walk_fast", "gallop", "half_bound")

bpy.ops.wm.open_mainfile(filepath=str(ASSETS / "capybara.blend"))
bpy.context.scene.render.fps = 30
rig = bpy.data.objects["capyrig"]
# Every clip as a muted NLA track, so the exporter writes one animation per action.
rig.animation_data.action = None
for track in list(rig.animation_data.nla_tracks):
    rig.animation_data.nla_tracks.remove(track)
for name in CLIPS:
    action = bpy.data.actions[name]
    track = rig.animation_data.nla_tracks.new()
    track.name = name
    track.strips.new(name, int(action.frame_range[0]), action)
    track.mute = True
for obj in bpy.data.objects:
    obj.select_set(obj.name in ("capy", "capyrig"))
bpy.ops.export_scene.gltf(
    filepath=str(ASSETS / "capybara.glb"), export_format="GLB", use_selection=True,
    export_apply=True, export_def_bones=True, export_animation_mode="NLA_TRACKS",
    export_force_sampling=True, export_frame_step=1, export_optimize_animation_size=False,
    export_anim_slide_to_zero=True, export_reset_pose_bones=True,
    export_skins=True, export_influence_nb=4, export_all_influences=False,
    export_cameras=False, export_lights=False, export_materials="NONE")
