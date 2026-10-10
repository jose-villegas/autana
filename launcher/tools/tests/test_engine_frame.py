"""Asset-pack entries cross the source-to-engine boundary once."""

import math
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from anim import tracks_asset
from asset.engine_frame import INT16_MIN, POSITION
from gltf import gltf_write
from r3d import build_pack, mesh_asset, scene_asset
from r3d.gltf_skin import mat_from_trs, transform_vector
from test_r3d_import import renderer, write_import

POINT = (2, 3, -7)
NORMAL = (0.0, 0.0, -1.0)
UNIT_SCALE = (1.0, 1.0, 1.0)
ORIGIN = (0.0, 0.0, 0.0)
MIRROR = (1.0, 1.0, -1.0, 1.0)


def mesh_entry(point=POINT):
    positions = POSITION.pack(*point)
    cluster_at = mesh_asset.BLOB_HEADER.size + len(positions) + (-len(positions) % 4)
    node_at = cluster_at + mesh_asset.CLUSTER.size
    header = mesh_asset.BLOB_HEADER.pack(1, 0, 1, 1, 1, mesh_asset.BLOB_HEADER.size,
                                         0, 0, cluster_at, node_at, 0)
    cluster = mesh_asset.CLUSTER.pack(0, 1, 0, 0, 0, 0, point[2], point[0], point[1], -1, 0)
    node = mesh_asset.NODE.pack(0, 0, point[2], point[0], point[1], -1, 0, 1, 1)
    return header + positions + bytes(-len(positions) % 4) + cluster + node


class EngineFrameTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = pathlib.Path(self.folder.name)

    def test_packed_point_and_bounds_are_mirrored_without_reordering(self):
        source = self.root / "probe.mesh"
        raw = mesh_entry()
        source.write_bytes(raw)
        _, kind, converted = build_pack.pack_entry("probe", source)
        self.assertEqual(kind, mesh_asset.TYPE)
        header = mesh_asset.BLOB_HEADER.unpack_from(converted)
        self.assertEqual(POSITION.unpack_from(converted, header[5]), (2, 3, 7))
        self.assertEqual(mesh_asset.CLUSTER.unpack_from(converted, header[8])[4:10], (0, 0, 1, 2, 3, 7))
        self.assertEqual(mesh_asset.NODE.unpack_from(converted, header[9])[:6], (0, 0, 1, 2, 3, 7))
        self.assertEqual(converted[:mesh_asset.BLOB_HEADER.size], raw[:mesh_asset.BLOB_HEADER.size])
        self.assertEqual(source.read_bytes(), raw)

    def test_unrepresentable_mirror_is_rejected(self):
        source = self.root / "probe.mesh"
        source.write_bytes(mesh_entry((2, 3, INT16_MIN)))
        with self.assertRaisesRegex(ValueError, "int16"):
            build_pack.pack_entry("probe", source)

    def test_scene_placement_is_mirror_matrix_mirror(self):
        source = self.root / "probe.scene.toml"
        write_import(self.root)
        source.write_text('[[objects]]\nname = "camera"\nposition = [2, 3, -7]\n'
                          'rotation = [15, 25, 35]\n[objects.camera]\nhalf_fov_short_tan = 1\nnear_z = 1\n' + renderer())
        original = scene_asset.decode(scene_asset.bake(source))["entities"][0]
        _, _, entry = build_pack.pack_entry("probe", source)
        converted = scene_asset.decode(entry)["entities"][0]
        self.assertEqual(converted["position"], [2.0, 3.0, 7.0])
        for row in range(3):
            for column in range(3):
                self.assertEqual(converted["matrix"][row][column],
                                 MIRROR[row] * original["matrix"][row][column] * MIRROR[column])

    def test_cubic_translation_and_rotation_tangents_are_mirrored(self):
        channels = [
            {"node": 0, "path": "translation", "times": [0.0], "interpolation": "CUBICSPLINE",
             "values": [(1.0, 2.0, 3.0), (4.0, 5.0, 6.0), (7.0, 8.0, 9.0)]},
            {"node": 0, "path": "rotation", "times": [0.0], "interpolation": "CUBICSPLINE",
             "values": [(1.0, 2.0, 3.0, 4.0), (0.0, 0.0, 0.0, 1.0), (5.0, 6.0, 7.0, 8.0)]}]
        glb = self.root / "probe.glb"
        glb.write_bytes(gltf_write.build_glb([{"name": "camera"}], [{"name": "turn", "channels": channels}]))
        clip = self.root / "probe.anim.toml"
        clip.write_text('source = "probe.glb"\nanimation = "turn"\n')
        original, duration = tracks_asset.decode(tracks_asset.bake(clip))
        _, _, converted = build_pack.pack_entry("probe", clip)
        tracks, engine_duration = tracks_asset.decode(converted)
        self.assertEqual(duration, engine_duration)
        for source, actual, signs in zip(original, tracks, (MIRROR[:3], (-1, -1, 1, 1))):
            self.assertEqual(actual["times"], source["times"])
            self.assertEqual(actual["interpolation"], source["interpolation"])
            self.assertEqual(actual["values"], [tuple(v * s for v, s in zip(row, signs))
                                                for row in source["values"]])

    def packed_rotation(self, rotation):
        channel = {"node": 0, "path": "rotation", "times": [0.0], "values": [rotation]}
        glb = self.root / "probe.glb"
        glb.write_bytes(gltf_write.build_glb([{"name": "camera"}], [{"name": "turn", "channels": [channel]}]))
        clip = self.root / "probe.anim.toml"
        clip.write_text('source = "probe.glb"\nanimation = "turn"\n')
        _, kind, converted = build_pack.pack_entry("probe", clip)
        self.assertEqual(kind, tracks_asset.TYPE)
        tracks, _ = tracks_asset.decode(converted)
        return tracks[0]["values"][0]

    def test_quaternion_rotation_is_mirror_matrix_mirror(self):
        for axis in ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)):
            for angle in (0.3, 1.2, -2.1):
                rotation = (*[a * math.sin(angle / 2) for a in axis], math.cos(angle / 2))
                converted = self.packed_rotation(rotation)
                original = mat_from_trs(ORIGIN, rotation, UNIT_SCALE)
                actual = mat_from_trs(ORIGIN, converted, UNIT_SCALE)
                for row in range(4):
                    for column in range(4):
                        self.assertAlmostEqual(actual[row * 4 + column],
                                               MIRROR[row] * original[row * 4 + column] * MIRROR[column], places=6)

    def test_camera_forward_is_mirrored_gltf_minus_z(self):
        rotation = (0.0, math.sin(0.4), 0.0, math.cos(0.4))
        engine_matrix = mat_from_trs(ORIGIN, self.packed_rotation(rotation), UNIT_SCALE)
        gltf_matrix = mat_from_trs(ORIGIN, rotation, UNIT_SCALE)
        engine_forward = transform_vector(engine_matrix, (0.0, 0.0, 1.0))
        source_forward = transform_vector(gltf_matrix, NORMAL)
        for actual, source, sign in zip(engine_forward, source_forward, MIRROR):
            self.assertAlmostEqual(actual, source * sign)


if __name__ == "__main__":
    unittest.main()
