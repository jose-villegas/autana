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
    from r3d.light_preview import aim_sun, path_poses, save_rotation, toward
except ImportError:
    np = None

from tests.r3d_env import needs_mitsuba  # noqa: E402

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


@unittest.skipIf(np is None, "the r3d environment is not installed")
@needs_mitsuba
class AimSun(unittest.TestCase):
    def test_a_turned_sun_shines_as_one_built_at_that_rotation(self):
        """A floor lit by a sun turned in place matches a floor built with the sun there: what --live relies on."""
        from types import SimpleNamespace

        from r3d import mitsuba_reference
        from r3d.geometry import corner_normals

        p = np.array([[-50.0, 0.0, -50.0], [50.0, 0.0, -50.0], [50.0, 0.0, 50.0], [-50.0, 0.0, 50.0]])
        source = SimpleNamespace(p=p, tri_v=np.array([[0, 2, 1], [0, 3, 2]]), tri_t=np.zeros((2, 3), np.int64),
                                 tri_m=np.zeros(2, np.int64), uv=np.zeros((1, 2)), names=["m"], materials={},
                                 textures=[None], colors=None)
        source.corner_normals = corner_normals(source.p, source.tri_v)

        def tracer(direction):
            light = {"type": "directional", "direction": list(direction), "color": [1.0, 1.0, 1.0], "intensity": 1.0}
            return mitsuba_reference.prepare(source, [light], [], keep_textures=True)

        pose = np.array([0.0, 60.0, 80.0, 0.0, -0.6, -0.8])
        turned_to = toward([40.0, -70.0, 0.0])
        turned = tracer(toward([10.0, 30.0, 0.0]))
        aim_sun(turned, 0, turned_to)
        expected = tracer(turned_to).trace(pose, 16, 16, 0.6, 1.0, 16, 0, 2)[0]
        np.testing.assert_allclose(turned.trace(pose, 16, 16, 0.6, 1.0, 16, 0, 2)[0], expected, rtol=1e-3, atol=1e-4)
        self.assertGreater(expected.mean(), 0.0)


if __name__ == "__main__":
    unittest.main()
