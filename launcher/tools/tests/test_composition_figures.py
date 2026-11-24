"""Tests for launcher/tools/r3d/cards/composition_figures.py.

    python -m unittest discover -s launcher/tools/tests
"""
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
sys.path.insert(0, str(ROOT / "launcher/tools/r3d/cards"))

try:
    import composition_figures
    from r3d.import_settings import load_scene
    from r3d.poses import flight_camera
except ImportError as missing:
    raise unittest.SkipTest(f"needs launcher/tools/r3d/requirements.txt: {missing}")

DEMO_SCENE = ROOT / "launcher/demo/sponza/sponza.scene.toml"
# The figure's pose, as render_doc_images.sh passes it.
POSE_MS = 8000


class PathPoseTest(unittest.TestCase):
    def test_the_pose_comes_from_the_flythrough_when_the_scene_camera_has_no_path(self):
        scene = load_scene(DEMO_SCENE)
        flight = flight_camera(scene)
        self.assertIsNotNone(flight)
        self.assertIsNot(scene.camera.component, flight)
        _pose, lens = composition_figures.path_pose(scene, POSE_MS, *composition_figures.PANEL_SIZE)
        self.assertEqual(lens, flight.half_fov_short_tan)


if __name__ == "__main__":
    unittest.main()
