"""Validate posed skin placement and white reflectance on the shared triangle rig."""
import pathlib
import sys
import unittest

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.gltf_skin import SkinnedAsset
from r3d.skin_reference import posed_skin
from tests.skin_probe import POSITIONS, rig


class SkinReferenceTests(unittest.TestCase):
    def test_triangle_placement(self):
        asset = SkinnedAsset(*rig())
        world, colors, triangles = posed_skin(asset, "move", 0, (10, 20, 30), 90, 2)
        expected = [(10 + 2 * z, 20 + 2 * y, 30 - 2 * x) for x, y, z in POSITIONS]
        np.testing.assert_allclose(world, expected)
        np.testing.assert_array_equal(triangles, [[0, 1, 2]])
        np.testing.assert_array_equal(colors, np.ones((3, 3)))

    def test_white_overrides_every_albedo(self):
        asset = SkinnedAsset(*rig())
        asset.colors = [(0.1, 0.2, 0.3, 0.5)] * len(POSITIONS)
        _, colors, _ = posed_skin(asset, "move", 0.3, (0, 0, 0), 0, 1, white=True)
        np.testing.assert_array_equal(colors, np.ones((3, 3)))


if __name__ == "__main__":
    unittest.main()
