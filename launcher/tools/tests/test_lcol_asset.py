"""LCOL layout, scene selection and pack validation share the runtime rules."""
import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from build.c_header import read_define
from r3d import build_pack, mesh_asset, scene_asset
from test_scene_asset import authored_entry


class LcolTests(unittest.TestCase):
    def owner(self):
        from r3d import lcol_asset
        return lcol_asset

    def test_round_trip(self):
        owner = self.owner()
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
        owner = self.owner()
        vectors = pathlib.Path(__file__).resolve().parents[2] / "test/lcol_name_vectors.h"
        for mesh, name in zip(read_define(vectors, "LCOL_TEST_MESHES"), read_define(vectors, "LCOL_TEST_NAMES")):
            self.assertEqual(owner.name_for_mesh(mesh), name)
        for mesh in read_define(vectors, "LCOL_TEST_INVALID"):
            with self.assertRaises(ValueError):
                owner.name_for_mesh(mesh)

    def test_scene_v3_round_trip(self):
        self.assertEqual(scene_asset.VERSION, 3)
        scene = authored_entry()
        at = scene_asset.HEADER.unpack_from(scene)[1 + len(scene_asset.PARTS) + scene_asset.PARTS.index("renderers")]
        struct.pack_into("<H", scene, at + 2, 7)
        self.assertEqual(scene_asset.decode(scene)["renderers"][0]["colour_set"], 7)

    def test_pack_refusals(self):
        owner = self.owner()
        for encoding, count, stride in ((owner.RGB888, 4, 3), (owner.RGB565, 2, 2)):
            mesh = mesh_asset.BLOB_HEADER.pack(4, 2, 1, 1, 1, 44, 0, 68, 80, 104, 0)
            colors = owner.encode(encoding, count, [bytes(count * stride)] * 2)
            scene = authored_entry()
            at = scene_asset.HEADER.unpack_from(scene)[1 + len(scene_asset.PARTS) + scene_asset.PARTS.index("renderers")]
            scene_asset.RENDERER.pack_into(scene, at, 0, 0, b"mesh")
            rows = [("scene", scene_asset.TYPE, scene), ("mesh", mesh_asset.TYPE, mesh),
                    ("mesh.colors", owner.TYPE, colors)]
            build_pack.validate_colour_sets(rows)
            for failure in ("range", "missing", "count", "own", "encoding", "colourless"):
                changed = [(name, kind, bytearray(data)) for name, kind, data in rows]
                if failure == "range":
                    struct.pack_into("<H", changed[0][2], at + 2, 2)
                elif failure == "missing":
                    changed.pop()
                elif failure == "count":
                    struct.pack_into("<I", changed[2][2], 8, count + 1)
                elif failure == "encoding":
                    struct.pack_into("<H", changed[2][2], 2, 1 - encoding)
                else:
                    struct.pack_into("<H", changed[0][2], at + 2, scene_asset.COLOUR_SET_OWN)
                    if failure == "colourless":
                        changed.pop()
                with self.subTest(failure=failure), self.assertRaises(ValueError):
                    build_pack.validate_colour_sets(changed)


if __name__ == "__main__":
    unittest.main()
