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
from asset.asset_pack import DEFAULT_ALIGN, build_pack  # noqa: E402
from r3d import scene_asset, scene_name  # noqa: E402
from test_r3d_import import write_import
from types import SimpleNamespace
from unittest import mock
from skin_probe import rig, POSITIONS, POSITION_SCALE
from test_engine_frame import mesh_entry
from r3d import mesh_asset, skin_asset
from anim import skeleton_asset
from asset.engine_frame import to_engine
from build.c_header import read_define  # noqa: E402
BINDING_OWNER = TOOLS.parent / "main/anim/anim_binding.h"

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


def camera_entry():
    tracks, duration = tracks_asset.decode(probe_entry())
    target = read_define(BINDING_OWNER, "ANIM_TARGET_SELF")
    tracks = [dict(t, path=target) for t in tracks if t["path"] == "lamp" and t["component"] == tracks_asset.TRANSFORM]
    return tracks_asset.encode(tracks, duration)


def authored_entries(root):
    source = write_import(root, 'rig.import.toml', source='[source]\npath = "rig.glb"\ncredit = "test"\n')
    write_import(root, 'backdrop.import.toml', output='[output]\nname = "backdrop"\n')
    objects = ['[[objects]]\nname = "backdrop"\n[objects.mesh_renderer]\nmesh = "backdrop.import.toml"\n']
    for name, clip in (("first", "idle"), ("second", "walk")):
        objects.append(f'[[objects]]\nname = "{name}"\n[objects.skinned_renderer]\nmesh = "rig.import.toml"\n'
                       f'[objects.animator]\nclips = ["{clip}"]\ndefault_clip = "{clip}"\n')
    for name in ('sun', 'fill'):
        objects.append(f'[[objects]]\nname = "{name}"\n[objects.light]\ntype = "directional"\n'
                       'color = [0.8, 0.7, 0.6]\nintensity = 1.5\n')
    path = root / 'authored.scene.toml'
    path.write_text('tonemap_white = 0.5\n[ambient]\ncolor = [0.1, 0.2, 0.3]\nintensity = 0.4\n' + ''.join(objects))
    document, binary = rig()
    with mock.patch('r3d.skin_pack.load_rig', return_value=(source, document, binary, 5)):
        scene = scene_asset.bake(path)
    positions = [skin_asset.quantize_position(p, POSITION_SCALE) for p in POSITIONS]
    mesh = bytearray(mesh_entry(points=positions))
    header = list(mesh_asset.BLOB_HEADER.unpack_from(mesh))
    padding = -header[9] % DEFAULT_ALIGN
    mesh[header[9]:header[9]] = bytes(padding)
    header[9] += padding
    mesh.extend(bytes(-len(mesh) % DEFAULT_ALIGN))
    header[4], header[6], header[7] = POSITION_SCALE, len(mesh), header[5]
    mesh_asset.BLOB_HEADER.pack_into(mesh, 0, *header)
    mesh.extend(bytes([64, 128, 192]) * len(positions))
    name, skeleton, skin = skin_asset.bake(document, binary, 5, mesh)
    tracks, duration, root_kind = tracks_asset.clip_tracks(document, binary, document['animations'][0])
    clip = tracks_asset.encode(tracks, duration, root_kind)
    entries = [('authored', scene_asset.TYPE, scene), ('backdrop', mesh_asset.TYPE, bytes(mesh)),
               (name, skeleton_asset.TYPE, skeleton),
               ('idle', tracks_asset.TYPE, clip), ('walk', tracks_asset.TYPE, clip)]
    for name in ('first', 'second'):
        key = 'authored.' + name
        entries.extend([(key, mesh_asset.TYPE, bytes(mesh)), (key + '.skin', skin_asset.TYPE, skin)])
    return [(name, kind, to_engine(kind, data)) for name, kind, data in entries]


def probe_pack():
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        for mesh in ("red", "green"):
            write_import(root, f"{mesh}.import.toml", output=f'[output]\nname = "{mesh}"\n')
        (root / "probe.glb").write_bytes(probe_glb())
        (root / "probe.anim.toml").write_text('source = "probe.glb"\nanimation = "clip"\n')
        scene = root / "probe_scene.scene.toml"
        scene.write_text(SCENE)
        entry = scene_asset.bake(scene)
        scene_id = scene_name.scene_id(scene)
        authored = authored_entries(root)
    return build_pack([(scene_id, scene_asset.TYPE, entry), ("probe", tracks_asset.TYPE, camera_entry()), *authored])


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
