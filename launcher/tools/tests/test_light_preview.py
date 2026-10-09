"""Checks that light_preview.py --save rewrites only the named light's rotation line, and that it reads back as the
direction previewed."""

import pathlib
import sys
import tomllib
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.import_settings import load_light
    from r3d.light_preview import save_rotation, toward
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


if __name__ == "__main__":
    unittest.main()
