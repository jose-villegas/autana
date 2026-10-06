#!/usr/bin/env python3
"""A test scene baked by the scene entry's writer, and the pack the host C
suite suite_scene.c reads to hold the firmware's reader to it.

    python launcher/tools/tests/scene_probe.py -o PACK

The pack holds SCNE "probe_scene", baked by scene_asset.bake() from the scene
file probe_scene.scene.toml written here (its stem is the id), and the TRCK "probe" its camera flies (anim_probe.py's
clip, node "lamp"). The suite adds meshes of its own for the two renderers,
"red" and "green", and checks the loaded scene against what SCENE below says. The scene
is invented here, so nothing depends on one an app ships.
"""

import argparse
import pathlib
import sys
import tempfile

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "tests"))

from anim import tracks_asset  # noqa: E402
from anim_probe import probe_entry, probe_glb  # noqa: E402
from asset.asset_pack import build_pack  # noqa: E402
from r3d import scene_asset  # noqa: E402
from test_r3d_import import write_import  # noqa: E402

# What suite_scene.c checks: the placed renderer comes first, so an array
# reversed or shifted is caught; it is turned a quarter about y and scaled
# unevenly, so its matrix is not its own transpose and differs from its
# position.
SCENE = """
[[objects]]
name = "red"
position = [1.0, 2.0, 3.0]
rotation = [0.0, 90.0, 0.0]
scale = [2.0, 4.0, 0.5]
[objects.mesh_renderer]
mesh = "red.import.toml"

[[objects]]
name = "camera"
[objects.camera]
half_fov_short_tan = 0.75
near_z = 1.5
background = 0x336699
path = { animation = "probe.anim.toml", node = "lamp" }

[[objects]]
name = "green"
[objects.mesh_renderer]
mesh = "green.import.toml"
"""


def probe_pack():
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        for mesh in ("red", "green"):
            write_import(root, f"{mesh}.import.toml", output=f'[output]\ndirectory = "."\nname = "{mesh}"\n')
        (root / "probe.glb").write_bytes(probe_glb())
        (root / "probe.anim.toml").write_text('source = "probe.glb"\nanimation = "clip"\n')
        scene = root / "probe_scene.scene.toml"
        scene.write_text(SCENE)
        entry = scene_asset.bake(scene)
        scene_id = scene_asset.scene_id(scene)
    return build_pack([(scene_id, scene_asset.TYPE, entry), ("probe", tracks_asset.TYPE, probe_entry())])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("-o", "--out", required=True, help="the pack to write")
    args = parser.parse_args(argv)
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(probe_pack())
    return 0


if __name__ == "__main__":
    sys.exit(main())
