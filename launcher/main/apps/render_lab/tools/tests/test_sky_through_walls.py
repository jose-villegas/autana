"""Sky through the walls of the scene's region-visibility bakes, counted along the camera path.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests

Builds the host renderer once, flies each scene in MOST_HOLED_FRAMES over its
camera path (FRAMES frames, DT_MS apart), and counts per frame the pixels of
the clear colour that are not connected to the top row: sky showing where a
wall should be. A frame with more than HOLE_PIXELS of them is holed; a bake
may have no more holed frames than MOST_HOLED_FRAMES allows. The roof
opening seen straight up is enclosed sky too: the current bakes' holed
frames are the few that look up through it.

Needs the r3d environment (launcher/tools/r3d/requirements.txt).
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
SCENE = TOOLS.parent / "meshes" / "sponza.scene.toml"
sys.path.insert(0, str(TOOLS.parents[3] / "tools" / "render"))
sys.path.insert(0, str(TOOLS.parents[3] / "tools"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import host_render  # noqa: E402

try:
    import numpy as np
    from scipy import ndimage

    from r3d.import_settings import load_scene
    from render_compare import expand_565, read_video
except ImportError:  # pragma: no cover - the tool tests' environment has them
    np = None

FRAMES, DT_MS = 600, 50
HOLE_PIXELS = 50
# Per scene, the holed frames of its current bake: the ceiling a rebake must not go above.
MOST_HOLED_FRAMES = {"sponza": 8, "sponza-flat": 8, "sponza-lite": 8}


def clear_colour():
    """The scene's camera background as the 16-bit framebuffer shows it."""
    rgb = load_scene(SCENE).camera.component.background
    return expand_565((rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF))


def sky_pixels(frame, sky):
    """(enclosed, open): the pixels of colour `sky` in an (h, w, 3) RGB frame not connected to its top row, and
    those that are."""
    mask = (frame == np.array(sky)).all(axis=2)
    labels, _ = ndimage.label(mask)
    top = labels[0][labels[0] > 0]
    enclosed = mask & ~np.isin(labels, top)
    return int(enclosed.sum()), int(mask.sum() - enclosed.sum())


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SkyCount(unittest.TestCase):
    def test_sky_cut_off_from_the_top_row_counts_and_sky_open_to_it_does_not(self):
        sky = (156, 195, 231)
        frame = np.zeros((8, 8, 3), dtype=np.uint8)
        frame[:3] = sky  # open sky along the top
        frame[5:7, 2:5] = sky  # a hole in the wall below it
        frame[4:6, 0] = sky  # a hole at the left edge
        frame[7, 6:8] = sky  # a hole at the bottom edge
        self.assertEqual(sky_pixels(frame, sky), (10, 24))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SkyThroughWalls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = pathlib.Path(tempfile.mkdtemp(prefix="sky_through_walls_"))
        run, cls.binary = host_render.run(cls.out, "--build-only")
        cls.build_output = run.stdout + run.stderr

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def test_each_bake_shows_no_more_sky_through_walls_than_its_ceiling(self):
        for scene, most in MOST_HOLED_FRAMES.items():
            with self.subTest(scene=scene):
                self.assert_few_holed_frames(scene, most)

    def assert_few_holed_frames(self, scene, most):
        self.assertTrue(self.binary.exists(), self.build_output[-2000:])
        video = self.out / f"{scene}.avi"
        run = subprocess.run([str(self.binary), "--quarter", "1", "--no-hud", "--scene", scene, "--frames", str(FRAMES),
                              "--dt", str(DT_MS), "-o", str(self.out / f"{scene}.bmp"), "--video", str(video)],
                             capture_output=True, text=True, timeout=600)
        self.assertEqual(run.returncode, 0, run.stderr[-2000:])
        sky = clear_colour()
        counts = [sky_pixels(frame, sky) for frame in read_video(video)[1]]
        video.unlink()
        self.assertEqual(len(counts), FRAMES)
        # The path looks up at the open roof, so sky open to the top must turn up: that proves the clear colour
        # matched, so a count of no holes is real.
        self.assertTrue(any(open_ > HOLE_PIXELS for _, open_ in counts), f"{scene}: no sky found")
        holed = [frame for frame, (enclosed, _) in enumerate(counts) if enclosed > HOLE_PIXELS]
        self.assertLessEqual(len(holed), most, f"{scene}: holed frames {holed}")


if __name__ == "__main__":
    unittest.main()
