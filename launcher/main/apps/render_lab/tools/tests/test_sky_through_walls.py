"""Sky through the walls of the scene's region-visibility bakes, counted along the camera path.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests

Builds the host renderer once, flies each scene in MOST_HOLED_FRAMES over its
camera path (FRAMES frames, DT_MS apart), and counts per frame the pixels of
the clear colour that are not connected to the top row: sky showing where a
wall should be. A frame with more than HOLE_PIXELS of them is holed; a bake
may have no more holed frames than MOST_HOLED_FRAMES allows. The roof
opening seen straight up is enclosed sky too, so the ceilings include it.

Needs numpy, scipy and Pillow (launcher/tools/r3d/requirements.txt).
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import tomllib
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
SCENE = TOOLS.parent / "meshes" / "sponza.scene.toml"
sys.path.insert(0, str(TOOLS.parents[3] / "tools" / "render"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import host_render  # noqa: E402

try:
    import numpy as np
    from scipy import ndimage

    from render_compare import expand_565, read_video
except ImportError:  # pragma: no cover - the tool tests' environment has them
    np = None

FRAMES, DT_MS = 600, 50
HOLE_PIXELS = 50
# Per scene, the holed frames of its current bake: the ceiling a rebake must not go above.
MOST_HOLED_FRAMES = {"sponza": 8, "sponza-flat": 8, "sponza-lite": 8}


def clear_colour():
    """The scene's camera background as the 16-bit framebuffer shows it."""
    with SCENE.open("rb") as source:
        scene = tomllib.load(source)
    rgb = next(item["camera"]["background"] for item in scene["objects"] if "camera" in item)
    return expand_565(((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF))


def sky_pixels(frame, sky):
    """(enclosed, open): the pixels of colour `sky` in an (h, w, 3) RGB frame not connected to its top row, and
    those that are."""
    mask = (frame == np.array(sky)).all(axis=2)
    labels, _ = ndimage.label(mask)
    top = labels[0][labels[0] > 0]
    enclosed = mask & ~np.isin(labels, top)
    return int(enclosed.sum()), int(mask.sum() - enclosed.sum())


@unittest.skipIf(np is None, "needs numpy, scipy and Pillow")
class SkyCount(unittest.TestCase):
    def test_sky_enclosed_by_a_wall_counts_and_sky_open_to_the_top_does_not(self):
        sky = (156, 195, 231)
        frame = np.zeros((8, 8, 3), dtype=np.uint8)
        frame[:3] = sky  # open sky along the top
        frame[5:7, 2:5] = sky  # a hole in the wall below it
        self.assertEqual(sky_pixels(frame, sky), (6, 24))


@unittest.skipIf(np is None, "needs numpy, scipy and Pillow")
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
        # Sky open to the top in most frames: the colour matched, so a frame with no holes counted none.
        self.assertGreater(sum(open_ > 0 for _, open_ in counts), FRAMES // 2, f"{scene}: no sky found")
        holed = [frame for frame, (enclosed, _) in enumerate(counts) if enclosed > HOLE_PIXELS]
        self.assertLessEqual(len(holed), most, f"{scene}: holed frames {holed}")


if __name__ == "__main__":
    unittest.main()
