"""LCOL layout, scene selection and pack validation share the runtime rules."""
import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from asset.asset_pack import PackError, padded
from build.c_header import read_define
from r3d import build_pack, lcol_asset, mesh_asset, scene_asset
from r3d.import_settings import SettingsError
from test_scene_asset import authored_entry


class LcolTests(unittest.TestCase):
    def test_mesh_header(self):
        words = tuple(range(1, 12))
        mesh = mesh_asset.BLOB_HEADER.pack(*words)
        header = lcol_asset.read_mesh_header(mesh + b"payload")
        self.assertEqual(header.vertices, words[0])
        self.assertEqual(header.triangles, words[1])
        self.assertEqual(header.colors_at, words[6])
        self.assertEqual(header.face_at, words[10])

    def test_short_mesh_header(self):
        for size in range(mesh_asset.BLOB_HEADER.size):
            with self.subTest(size=size), self.assertRaisesRegex(PackError, "LMSH header is truncated"):
                lcol_asset.read_mesh_header(bytes(size))

    def test_round_trip(self):
        owner = lcol_asset
        for encoding, stride in ((owner.RGB888, 3), (owner.RGB565, 2)):
            sets = [bytes(range(4 * stride)), bytes(reversed(range(4 * stride)))]
            entry = owner.encode(encoding, 4, sets)
            self.assertEqual(owner.decode(entry), (encoding, 4, sets))
            for broken in (entry[:owner.HEADER.size - 1], entry[:-1], entry + b"x",
                           owner.HEADER.pack(owner.VERSION, encoding, 0, 4)):
                with self.assertRaises(ValueError):
                    owner.decode(broken)
            for at, value in ((0, owner.VERSION + 1), (2, 2)):
                broken = bytearray(entry)
                struct.pack_into("<H", broken, at, value)
                with self.assertRaises(ValueError):
                    owner.decode(broken)

    def test_shared_names(self):
        owner = lcol_asset
        vectors = pathlib.Path(__file__).resolve().parents[2] / "test/lcol_name_vectors.h"
        for mesh, name in zip(read_define(vectors, "LCOL_TEST_MESHES"), read_define(vectors, "LCOL_TEST_NAMES")):
            self.assertEqual(owner.name_for_mesh(mesh), name)
        for mesh in read_define(vectors, "LCOL_TEST_INVALID"):
            with self.assertRaises(ValueError):
                owner.name_for_mesh(mesh)

    def test_scene_v4_round_trip(self):
        self.assertEqual(scene_asset.VERSION, 4)
        scene = authored_entry()
        at = scene_asset.HEADER.unpack_from(scene)[1 + len(scene_asset.PARTS) + scene_asset.PARTS.index("renderers")]
        struct.pack_into("<H", scene, at + 2, 7)
        self.assertEqual(scene_asset.decode(scene)["renderers"][0]["colour_set"], 7)

    def test_pack_refusals(self):
        owner = lcol_asset
        for encoding, count, stride in ((owner.RGB888, 4, 3), (owner.RGB565, 2, 2)):
            positions = mesh_asset.BLOB_HEADER.size
            triangles = positions + 4 * struct.calcsize("<3h")
            clusters = triangles + 2 * struct.calcsize("<3H")
            nodes = padded(clusters + mesh_asset.CLUSTER.size, struct.calcsize("<I"))
            mesh = mesh_asset.BLOB_HEADER.pack(4, 2, 1, 1, 1, positions, 0, triangles, clusters, nodes, 0)
            colors = owner.encode(encoding, count, [bytes(count * stride)] * 2)
            scene = authored_entry()
            struct.pack_into("<H", scene, struct.calcsize("<H") * (1 + scene_asset.PARTS.index("skinned_renderers")), 0)
            at = scene_asset.HEADER.unpack_from(scene)[1 + len(scene_asset.PARTS) + scene_asset.PARTS.index("renderers")]
            scene_asset.RENDERER.pack_into(scene, at, 0, 0, b"mesh")
            rows = [("scene", scene_asset.TYPE, scene), ("mesh", mesh_asset.TYPE, mesh),
                    ("mesh.colors", owner.TYPE, colors)]
            build_pack.validate_colour_sets(rows)
            for failure in ("range", "missing", "count", "own", "encoding", "colourless", "mesh_type", "mesh_missing", "skinned"):
                changed = [(name, kind, bytearray(data)) for name, kind, data in rows]
                if failure == "range":
                    struct.pack_into("<H", changed[0][2], at + 2, 2)
                elif failure == "missing":
                    changed.pop()
                elif failure == "count":
                    struct.pack_into("<I", changed[2][2], 8, count + 1)
                elif failure == "encoding":
                    struct.pack_into("<H", changed[2][2], 2, 1 - encoding)
                elif failure == "mesh_type":
                    changed[1] = ("mesh", b"OTHER", mesh)
                elif failure == "mesh_missing":
                    changed.pop(1)
                elif failure == "skinned":
                    struct.pack_into("<H", changed[0][2], struct.calcsize("<H") * (1 + scene_asset.PARTS.index("skinned_renderers")), 2)
                else:
                    struct.pack_into("<H", changed[0][2], at + 2, scene_asset.COLOUR_SET_OWN)
                    if failure == "own":
                        changed[1][2][:mesh_asset.BLOB_HEADER.size] = mesh_asset.BLOB_HEADER.pack(
                            4, 2, 1, 1, 1, positions, positions, triangles, clusters, nodes, 0)
                    if failure == "colourless":
                        changed.pop()
                messages = {"range": "LCOL set, count", "missing": "requires an LCOL",
                            "count": "sets do not match", "own": "own colours require a coloured mesh without LCOL",
                            "encoding": "sets do not match", "colourless": "own colours require",
                            "mesh_type": "renderer mesh has the wrong type",
                            "mesh_missing": "renderer mesh is missing",
                            "skinned": "skinned renderer requires a coloured mesh"}
                error = SettingsError if failure in ("missing", "own", "colourless", "mesh_type", "mesh_missing", "skinned") else ValueError
                with self.subTest(failure=failure), self.assertRaisesRegex(error, messages[failure]):
                    build_pack.validate_colour_sets(changed)

    def test_coloured_prototype_refused(self):
        owner = lcol_asset
        for index in (6, 10):
            words = [4, 2, 1, 1, 1, mesh_asset.BLOB_HEADER.size, 0, 0, 0, 0, 0]
            words[index] = mesh_asset.BLOB_HEADER.size
            mesh = mesh_asset.BLOB_HEADER.pack(*words)
            entry = owner.encode(owner.RGB888, 4, [bytes(4 * owner.STRIDES[owner.RGB888])])
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, "mesh colour ownership"):
                owner.validate_mesh(entry, mesh, 0)


if __name__ == "__main__":
    unittest.main()
