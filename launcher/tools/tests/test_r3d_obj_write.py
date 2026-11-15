"""r3d.obj.write_obj: what it writes, load_obj reads back.

    python -m unittest discover -s launcher/tools/tests -p test_r3d_obj_write.py
"""

import pathlib
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.obj import load_obj, write_obj  # noqa: E402

POSITIONS = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0), (0.0, 1.0, 0.0)]
UVS = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]


class WriteObj(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = pathlib.Path(self.dir.name) / "quad.obj"

    def tearDown(self):
        self.dir.cleanup()

    def test_objects_materials_and_uvs_read_back(self):
        groups = [("left", "stone", [(0, 1, 2)]), ("left", "wood", [(0, 2, 3)]), ("right", "stone", [(3, 2, 1)])]
        write_obj(self.path, "quad.mtl", POSITIONS, groups, uvs=UVS)
        positions, uvs, tri_v, tri_t, tri_m, materials, tri_o, objects = load_obj(self.path)
        np.testing.assert_allclose(positions, POSITIONS)
        np.testing.assert_allclose(uvs, UVS)
        self.assertEqual([[0, 1, 2], [0, 2, 3], [3, 2, 1]], tri_v.tolist())
        self.assertEqual(tri_v.tolist(), tri_t.tolist())  # each corner's uv is its position's
        self.assertEqual(["stone", "wood", "stone"], [materials[m] for m in tri_m])
        self.assertEqual(["left", "right"], objects)  # one o line per object, however many materials
        self.assertEqual([0, 0, 1], tri_o.tolist())

    def test_a_banner_and_no_uvs_or_objects(self):
        write_obj(self.path, "quad.mtl", POSITIONS, [(None, "stone", [(0, 1, 2)])],
                  banner=("made here", ""), position_decimals=5)
        text = self.path.read_bytes().decode()
        self.assertTrue(text.startswith("# made here\n#\nmtllib quad.mtl\nv 0.00000 0.00000 0.00000\n"))
        self.assertNotIn("\r", text)
        self.assertIn("\nf 1 2 3\n", text)
        _, uvs, _, _, _, _, _, objects = load_obj(self.path)
        self.assertEqual(0, len(uvs))
        self.assertEqual(["quad"], objects)  # no o line: the file's own name


if __name__ == "__main__":
    unittest.main()
