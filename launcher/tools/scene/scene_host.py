"""The engine sources a host tool links to run a scene's animation and skin systems without drawing, and the one
scene entry such a tool loads.

    from scene import scene_host
    sources = [my_host.c, *scene_host.SOURCES]
"""

import pathlib
from types import SimpleNamespace

from r3d import scene_asset

TOOLS = pathlib.Path(__file__).resolve().parents[1]
MAIN = TOOLS.parent / "main"
SOURCES = [pathlib.Path(__file__).with_name("scene_draw_stand_ins.c"), *[MAIN / name for name in (
    "anim/anim_track.c", "anim/anim_tracks.c", "anim/anim_binding.c", "anim/anim_skeleton.c",
    "anim/pose/anim_pose.c", "anim/playback/anim_playhead.c", "render/r3d_skin.c",
    "render/skin/r3d_skinning.c", "render/skin/r3d_skin_light.c", "render/r3d_lit_mesh.c", "asset/asset_pack.c",
    "asset/asset_file.c", "core/job.c", "scene/scene.c", "scene/scene_animation.c", "scene/scene_light.c", "scene/scene_asset.c", "scene/scene_hierarchy.c",
    "anim/playback/anim_layer.c", "scene/camera/camera.c", "render/render_view.c", "asset/asset_store.c", "asset/asset_store_file.c")],
    MAIN.parent / "packages/hash/src/name_hash.c"]
SCENE_ID = "scene"
ENTITY_NAME = "subject"


def one_entity_scene(mesh_id=None):
    """SCNE bytes of one entity, ENTITY_NAME at the origin, drawing mesh_id when given."""
    subject = SimpleNamespace(name=ENTITY_NAME, kind="mesh_renderer" if mesh_id else "empty", position=(0, 0, 0),
                              matrix=((1, 0, 0), (0, 1, 0), (0, 0, 1)))
    renderers = [SimpleNamespace(object=subject, asset_name=mesh_id)] if mesh_id else []
    return scene_asset.encode(SimpleNamespace(objects=[subject], renderers=renderers))
