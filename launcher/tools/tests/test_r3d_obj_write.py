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
# Uvs a texel apart on a large texture, differing only past the third decimal.
FINE_UVS = [(0.12345, 0.54321), (0.12351, 0.54329), (0.12399, 0.54377), (0.5, 0.5)]
# write_obj's default uv precision, and the coarser and finer settings the tests give it.
DEFAULT_UV_DECIMALS = 5
COARSE_UV_DECIMALS = 2
FINER_UV_DECIMALS = 4


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


    def test_a_group_without_an_object_closes_the_object_before_it(self):
        """None writes no o line, so its faces stay in the file's current object; the next named group opens its
        object again, the same name or not."""
        groups = [("left", "stone", [(0, 1, 2)]), (None, "wood", [(0, 2, 3)]), ("left", "stone", [(3, 2, 1)])]
        write_obj(self.path, "quad.mtl", POSITIONS, groups)
        text = self.path.read_text(encoding="utf-8")
        self.assertEqual(["o left", "o left"], [line for line in text.splitlines() if line.startswith("o ")])
        self.assertLess(text.index("usemtl wood"), text.rindex("o left"))
        _, _, tri_v, _, _, _, tri_o, objects = load_obj(self.path)
        self.assertEqual(["left", "left"], objects)
        self.assertEqual([0, 0, 1], tri_o.tolist())  # the unnamed group's triangle belongs to the first
        self.assertEqual([[0, 1, 2], [0, 2, 3], [3, 2, 1]], tri_v.tolist())

    def test_uvs_that_differ_below_three_decimals_read_back_distinct(self):
        write_obj(self.path, "quad.mtl", POSITIONS, [(None, "stone", [(0, 1, 2)])], uvs=FINE_UVS)
        uvs = load_obj(self.path)[1]
        np.testing.assert_allclose(uvs, FINE_UVS, atol=0.5 * 10.0 ** -DEFAULT_UV_DECIMALS)
        self.assertEqual(len(FINE_UVS), len({tuple(uv) for uv in uvs.tolist()}))

    def test_uv_decimals_sets_how_many_decimals_a_uv_is_written_with(self):
        for decimals in (COARSE_UV_DECIMALS, FINER_UV_DECIMALS):
            with self.subTest(decimals=decimals):
                write_obj(self.path, "quad.mtl", POSITIONS, [(None, "stone", [(0, 1, 2)])], uvs=FINE_UVS,
                          uv_decimals=decimals)
                lines = [line for line in self.path.read_text(encoding="utf-8").splitlines() if line.startswith("vt ")]
                u, v = FINE_UVS[0]
                self.assertEqual(f"vt {u:.{decimals}f} {v:.{decimals}f}", lines[0])
                self.assertEqual(len(FINE_UVS), len(lines))


if __name__ == "__main__":
    unittest.main()
