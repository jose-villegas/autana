"""Validate posed skin placement and white reflectance on the shared triangle rig."""
import pathlib
import sys
import unittest

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.gltf_skin import SkinnedAsset
from r3d.skin_reference import posed_skin, pose_frame
from tests.skin_probe import POSITIONS, rig


class SkinReferenceTests(unittest.TestCase):
    def test_pose_frame_keeps_exported_values(self):
        frame = {"frame": 3, "camera": {"world": np.eye(4).tolist(), "lens": 0.7, "near": 0.1},
                 "skins": [{"name": "subject", "clip": "move", "time": 0.375,
                            "world": np.eye(4).tolist(), "blend": 1}]}
        with self.subTest("selection"):
            (skin,), camera = pose_frame([frame], 3, "subject")
            self.assertEqual(skin["time"], 0.375)
            self.assertEqual(camera["lens"], 0.7)
        frame["skins"][0]["blend"] = 0.5
        with self.assertRaisesRegex(ValueError, "blend"):
            pose_frame([frame], 3, "subject")

    def test_pose_frame_mirrors_both_matching_skins_into_source_frame(self):
        camera_world, first_world, second_world = np.eye(4), np.eye(4), np.eye(4)
        camera_world[:3, 3], first_world[:3, 3], second_world[:3, 3] = (1, 2, 3), (4, 5, 6), (7, 8, 9)
        first = {"name": "subject", "clip": "move", "time": 0, "world": first_world.tolist(), "blend": 1}
        second = {**first, "world": second_world.tolist()}
        frame = {"frame": 0, "camera": {"world": camera_world.tolist(), "lens": 0.7, "near": 0.1},
                 "skins": [first, second, {**first, "name": "other", "blend": 0}]}
        skins, camera = pose_frame([frame], 0, "subject")
        camera_source = np.asarray(camera["world"])
        np.testing.assert_allclose(camera_source[:3, 3], (1, 2, -3))
        np.testing.assert_allclose(np.asarray([skin["world"] for skin in skins])[:, :3, 3],
                                   [(4, 5, -6), (7, 8, -9)])
        # The engine camera looks down its +z; mirrored, it looks down the source frame's -z, its local -z.
        np.testing.assert_allclose(-camera_source[:3, 2], (0, 0, -1))
        np.testing.assert_allclose(camera_source[:3, 1], (0, 1, 0))
        with self.assertRaisesRegex(ValueError, "no.*skin"):
            pose_frame([frame], 0, "missing")

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
