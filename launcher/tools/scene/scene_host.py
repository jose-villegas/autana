"""The engine sources a host tool links to run a scene's animation and skin systems without drawing, and the one
scene entry such a tool loads.

    from scene import scene_host
    sources = [my_host.c, *scene_host.SOURCES]
    command = [cc, *firmware_includes, *scene_host.FLAGS, *sources]
"""

import pathlib
import sys
from types import SimpleNamespace

from r3d import scene_asset

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "device"))
from device_profile import load, require  # noqa: E402

TOOLS = pathlib.Path(__file__).resolve().parents[1]
MAIN = TOOLS.parent / "main"
TLSF = TOOLS.parents[1] / "third_party/tlsf"
SOURCES = [pathlib.Path(__file__).with_name("scene_draw_stand_ins.c"), *[MAIN / name for name in (
    "anim/anim_track.c", "anim/anim_tracks.c", "anim/anim_binding.c", "anim/anim_skeleton.c",
    "anim/pose/anim_pose.c", "anim/playback/anim_playhead.c", "render/r3d_skin.c",
    "render/skin/r3d_skinning.c", "render/skin/r3d_skin_light.c", "render/r3d_lit_mesh.c", "asset/asset_pack.c",
    "asset/asset_file.c", "core/job.c", "core/pending.c", "anim/playback/anim_layer.c", "render/render_view.c",
    "asset/asset_store.c", "asset/asset_store_async.c", "asset/asset_store_file.c", "core/app_heap.c",
    "app/app_arena.c", "render/batch/r3d_batch.c")],
    # Every scene source but the draw and the shell half, which the stand-ins replace.
    *sorted(path for path in (MAIN / "scene").rglob("*.c") if path.name not in ("scene_draw.c", "scene_shell.c")),
    MAIN.parent / "packages/hash/src/name_hash.c", TLSF / "tlsf.c"]
# What those sources need past the firmware's include roots: the host stand-ins for the IDF headers (esp_log.h among
# them), the allocator app_heap.c builds on, and the board's PSRAM, which app_arena.c sizes the app region from.
FLAGS = [*(flag for root in (MAIN.parent / "test/stubs", TLSF, TLSF / "include") for flag in ("-I", str(root))),
         f"-DAPP_ARENA_HOST_PSRAM_BYTES={require(load(), 'DP_PSRAM_BYTES', int)}"]
SCENE_ID = "scene"
ENTITY_NAME = "subject"


def one_entity_scene(mesh_id=None):
    """SCNE bytes of one entity, ENTITY_NAME at the origin, drawing mesh_id when given."""
    subject = SimpleNamespace(name=ENTITY_NAME, kind="mesh_renderer" if mesh_id else "empty", position=(0, 0, 0),
                              matrix=((1, 0, 0), (0, 1, 0), (0, 0, 1)))
    renderers = [SimpleNamespace(object=subject, asset_name=mesh_id, renderer=None)] if mesh_id else []
    return scene_asset.encode(SimpleNamespace(objects=[subject], renderers=renderers))
