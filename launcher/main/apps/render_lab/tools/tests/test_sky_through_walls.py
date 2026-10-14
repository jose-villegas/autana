"""Sky enclosed by walls over one whole camera flythrough at the device frame step.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests -p test_sky_through_walls.py

Builds the host renderer once and samples the scene camera clip with track_host
at DT_MS intervals to determine the frame count. ANY clear-colour pixel not
connected to the top row makes a frame holed, including four-pixel pinholes.
The roof opening also counts when enclosed; each scene has its own ceiling.
Needs the r3d environment (launcher/tools/r3d/requirements.txt).
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
SCENE = TOOLS.parents[3] / "demo" / "sponza" / "sponza.scene.toml"
sys.path.insert(0, str(TOOLS.parents[3] / "tools" / "render"))
sys.path.insert(0, str(TOOLS.parents[3] / "tools"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import host_render  # noqa: E402

try:
    import numpy as np
    from scipy import ndimage

    from anim import track_host
    from r3d.poses import parse_pose_rows
    from r3d.import_settings import load_scene
    from render_compare import expand_565, read_video
except ImportError:  # pragma: no cover - the tool tests' environment has them
    np = None

DT_MS = 33
# Placeholder ceilings; measure each scene's locked rebake before setting its regression budget.
MOST_HOLED_FRAMES = {
    "sponza": 13,
    "sponza-lite": 13,
    "sponza-flat": 13,
    "sponza-fitted": 13,
    "sponza-fitted-full": 13,
    "sponza-flat-fitted": 13,
}


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
        scene = load_scene(SCENE)
        camera = scene.camera.component
        size = next(job.renderer.fit.size for job in scene.renderers if job.renderer.fit is not None)
        cls.frames = len(parse_pose_rows(track_host.poses(
            camera.path.animation, camera.path.node, DT_MS, *size, camera.half_fov_short_tan, camera.near_z))[4])
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
        run = subprocess.run([str(self.binary), "--quarter", "1", "--no-hud", "--scene", scene, "--frames", str(self.frames),
                              "--dt", str(DT_MS), "-o", str(self.out / f"{scene}.bmp"), "--video", str(video)],
                             capture_output=True, text=True, timeout=600)
        self.assertEqual(run.returncode, 0, run.stderr[-2000:])
        sky = clear_colour()
        counts = [sky_pixels(frame, sky) for frame in read_video(video)[1]]
        video.unlink()
        self.assertEqual(len(counts), self.frames)
        # The path looks up at the open roof, so sky open to the top must turn up: that proves the clear colour
        # matched, so a count of no holes is real.
        self.assertTrue(any(open_ > 0 for _, open_ in counts), f"{scene}: no sky found")
        holed = [frame for frame, (enclosed, _) in enumerate(counts) if enclosed > 0]
        print(f"{scene}: {len(holed)}/{self.frames} holed frames (ceiling {most})", flush=True)
        self.assertLessEqual(len(holed), most, f"{scene}: {len(holed)} holed frames exceeds {most}")


if __name__ == "__main__":
    unittest.main()
