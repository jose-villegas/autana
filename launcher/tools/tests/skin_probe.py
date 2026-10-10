"""A two-root rig for Python skin validation and mapped C reader probes."""

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from anim import tracks_asset
from asset.asset_pack import build_pack
from gltf import gltf_read, gltf_write
from r3d import skin_asset

IDENTITY = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
POSITION_SCALE = 16
POSITIONS = [(0, 0, 0.5), (1, 0, -0.5), (0, 1, 1)]
NORMALS = [(0, 0, 1)] * 3
JOINTS = [(0, 1, 2, 3)] * 3
WEIGHTS = [(128 / 255, 127 / 255, 0, 0), (64 / 255, 64 / 255, 64 / 255, 63 / 255),
           (100 / 255, 80 / 255, 50 / 255, 25 / 255)]


def rig():
    nodes = [{'name': 'armature', 'children': [1, 3]}, {'name': 'a', 'children': [2]},
             {'name': 'child'}, {'name': 'b', 'children': [4]}, {'name': 'tip'},
             {'name': 'mesh', 'mesh': 0, 'skin': 0}]
    channels = [{'node': j, 'path': 'translation', 'times': [0, 1],
                 'values': [(0, 0, 0), (j / 4, j / 8, j / 16)]} for j in range(1, 5)]
    channels += [{'node': j, 'path': 'rotation', 'times': [0, 1],
                  'values': [(0, 0, 0, 1), (0.5, 0.5, 0.5, 0.5)]} for j in range(1, 5)]
    primitive = dict(positions=POSITIONS, indices=[0, 1, 2], normals=NORMALS,
                     joints=JOINTS, weights=WEIGHTS)
    return gltf_read.parse_glb(gltf_write.build_glb(nodes, [{'name': 'move', 'channels': channels}],
               meshes=[{'name': 'triangle', 'primitives': [primitive]}],
               skins=[{'joints': [1, 2, 3, 4], 'inverse_binds': [IDENTITY] * 4}]))


def mesh_entry():
    from r3d.lit_mesh import bake_lit_mesh, mesh_blob
    return mesh_blob(bake_lit_mesh(POSITIONS, [(255, 255, 255)] * 3, [(0, 1, 2)], [0],
                                   position_scale=POSITION_SCALE))


def entries(influences=4):
    document, binary = rig()
    name, skeleton, skin = skin_asset.bake(document, binary, 5, mesh_entry(), influences)
    tracks, duration, root = tracks_asset.clip_tracks(document, binary, document['animations'][0])
    return [(name, b'SKEL', skeleton), ('probe.skin', b'SKIN', skin),
            ('move', b'TRCK', tracks_asset.encode(tracks, duration, root))]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-o', '--out', required=True)
    args = parser.parse_args()
    probe_entries = entries()
    _, kind, skin_two = entries(2)[1]
    probe_entries.append(('probe2.skin', kind, skin_two))
    pathlib.Path(args.out).write_bytes(build_pack(probe_entries))
