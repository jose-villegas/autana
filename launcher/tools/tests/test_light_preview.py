"""Checks that light_preview.py --save rewrites only the named light's rotation line, and that it reads back as the
direction previewed."""

import pathlib
import sys
import tempfile
import tomllib
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.import_settings import load_light
    from r3d.light_preview import path_poses, save_rotation, toward
except ImportError:
    np = None

SCENE = """# two lights
[[objects]]
name = "moon"
rotation = [10.0, 20.0, 0.0]

[objects.light]
type = "directional"
color = [1.0, 1.0, 1.0]
intensity = 1.0

[[objects]]
name = "sun"
rotation = [22.5, -37.0, 0.0]   # toward (old)

[objects.light]
type = "directional"
color = [1.0, 1.0, 1.0]
intensity = 3.0

[[objects]]
name = "lamp"

[objects.light]
type = "directional"
color = [1.0, 1.0, 1.0]
intensity = 1.0
"""


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SaveRotation(unittest.TestCase):
    def test_only_the_named_lights_line_changes(self):
        saved = save_rotation(SCENE, "sun", [15.0, -90.0, 0.0])
        changed = [(a, b) for a, b in zip(SCENE.splitlines(), saved.splitlines()) if a != b]
        self.assertEqual(len(SCENE.splitlines()), len(saved.splitlines()))
        self.assertEqual(len(changed), 1)
        self.assertTrue(changed[0][1].startswith("rotation = [15.0, -90.0, 0.0]   # toward ("))

    def test_a_light_without_a_rotation_gets_one(self):
        saved = save_rotation(SCENE, "lamp", [5.0, 0.0, 0.0])
        self.assertEqual(len(saved.splitlines()), len(SCENE.splitlines()) + 1)
        self.assertIn('name = "lamp"\nrotation = [5.0, 0.0, 0.0]', saved)

    def test_line_endings_are_kept(self):
        crlf = SCENE.replace("\n", "\r\n")
        saved = save_rotation(crlf, "sun", [15.0, -90.0, 0.0])
        self.assertEqual(saved.count("\r\n"), crlf.count("\r\n"))
        self.assertNotIn("\n", saved.replace("\r\n", ""))

    def test_the_saved_rotation_reads_back_as_that_direction(self):
        rotation = [12.5, -80.0, 0.0]
        sun = tomllib.loads(save_rotation(SCENE, "sun", rotation))["objects"][1]
        self.assertEqual(sun["rotation"], rotation)
        light = load_light(sun["light"], sun["rotation"], "sun")
        np.testing.assert_allclose(light["direction"], toward(rotation))

    def test_an_unknown_light_is_an_error(self):
        with self.assertRaises(ValueError):
            save_rotation(SCENE, "star", [0.0, 0.0, 0.0])


KEYS = """node = "cam"
animation = "walk"

[[keys]]
t = 0
eye = [0, 10, 0]
look_at = [100, 10, -100]

[[keys]]
t = 4
eye = [40, 30, 0]
look_at = [40, -70, 1]
"""
ANIM = """source = "walk.keys.toml"
animation = "walk"
"""


@unittest.skipIf(np is None, "the r3d environment is not installed")
class PathPoses(unittest.TestCase):
    def test_a_key_time_gives_that_keys_eye_and_view_direction(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = pathlib.Path(folder)
            (folder / "walk.keys.toml").write_text(KEYS)
            (folder / "walk.anim.toml").write_text(ANIM)
            start, end = path_poses(folder / "walk.anim.toml", "cam", [0.0, 4.0 - 1e-6])
        for pose, eye, look in ((start, [0, 10, 0], [100, 10, -100]), (end, [40, 30, 0], [40, -70, 1])):
            np.testing.assert_allclose(pose[:3], eye, atol=1e-3)
            want = np.subtract(look, eye) / np.linalg.norm(np.subtract(look, eye))
            np.testing.assert_allclose(pose[3:] / np.linalg.norm(pose[3:]), want, atol=1e-4)

    def test_a_node_without_tracks_is_an_error(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = pathlib.Path(folder)
            (folder / "walk.keys.toml").write_text(KEYS)
            (folder / "walk.anim.toml").write_text(ANIM)
            with self.assertRaises(ValueError):
                path_poses(folder / "walk.anim.toml", "other", [0.0])


if __name__ == "__main__":
    unittest.main()
