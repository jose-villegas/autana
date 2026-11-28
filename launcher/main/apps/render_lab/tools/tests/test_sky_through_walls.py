"""Sky enclosed by walls over one whole camera flythrough at the device frame step.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests -p test_sky_through_walls.py

Builds the host renderer once, samples the scene camera clip with sample_camera_rows at DT_MS
intervals for the frame count, then renders that flythrough. A frame is holed when a clear-colour
render pixel not connected to its top row is a hole: one where the source mesh shows no sky, in none
of the SUBPIXELS x SUBPIXELS rays through it or an enclosed pixel beside it. Sky the source shows, such
as the roof opening closed in by a beam or a notch in its rim, drawn up to a pixel wider, is no hole.
Needs the r3d environment (launcher/tools/r3d/requirements.txt) and the scene's source mesh
(launcher/demo/sponza/source/sponza.obj, Git LFS).
"""
import pathlib
import shutil
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
SCENE = TOOLS.parents[3] / "demo" / "sponza" / "sponza.scene.toml"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS.parents[3] / "tools"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import host_render  # noqa: E402

try:
    import numpy as np
    from r3d.import_settings import load_scene
    from r3d.ray_query import RayQuery
    from sky_through_walls import MOST_HOLED_FRAMES, SourceSky, measure_sky, pixel_sky, sky_pixels
    from tests.r3d_env import needs_llvm
except ImportError:  # pragma: no cover - the tool tests' environment has them
    np = None
    needs_llvm = unittest.skip("the r3d environment is not installed")

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

    def test_enclosed_one_pixel_and_two_by_two_sky_count(self):
        sky = (156, 195, 231)
        for side in (1, 2):
            with self.subTest(side=side):
                frame = np.zeros((6, 6, 3), dtype=np.uint8)
                frame[:1] = sky
                frame[3:3 + side, 2:2 + side] = sky
                self.assertEqual(sky_pixels(frame, sky), (side * side, 6))

    def test_enclosed_sky_the_source_shows_too_is_not_a_hole(self):
        sky = (156, 195, 231)
        frame = np.zeros((8, 8, 3), dtype=np.uint8)
        frame[:1] = sky
        frame[2:4, 2:6] = sky  # the roof opening, closed in by a beam: the source shows sky here
        frame[6, 3] = sky  # a pinhole in the floor: the source has the floor here
        self.assertEqual(sky_pixels(frame, sky, lambda ys, xs: ys < 4), (1, 8))
        frame[6, 3] = 0
        self.assertEqual(sky_pixels(frame, sky, lambda ys, xs: ys < 4), (0, 8))
        self.assertEqual(sky_pixels(frame, sky), (8, 8))

    def test_an_opening_wider_than_the_source_shows_it_is_not_a_hole(self):
        sky = (156, 195, 231)
        frame = np.zeros((8, 8, 3), dtype=np.uint8)
        frame[:1] = sky
        frame[2:5, 2:6] = sky  # the roof opening with its rim a pixel past the source's on every side
        frame[6, 7] = sky  # a pinhole touching no sky of the source
        source = lambda ys, xs: (ys == 3) & (xs > 2) & (xs < 5)
        self.assertEqual(sky_pixels(frame, sky, source), (1, 8))

    def test_a_pixel_beside_source_sky_by_side_or_corner_is_no_hole(self):
        sky = (156, 195, 231)
        frame = np.zeros((8, 8, 3), dtype=np.uint8)
        frame[:1] = sky
        frame[3, 3] = frame[4, 4] = sky  # corner to corner
        frame[3, 6] = frame[4, 6] = sky  # side by side
        frame[6, 1] = frame[6, 3] = sky  # a pixel apart
        source = {(3, 3), (3, 6), (6, 1)}
        self.assertEqual(sky_pixels(frame, sky, lambda ys, xs: np.array([(y, x) in source for y, x in zip(ys, xs)])),
                         (1, 8))

    def test_sky_the_source_shows_excuses_only_the_pixels_beside_it(self):
        sky = (156, 195, 231)
        frame = np.zeros((6, 8, 3), dtype=np.uint8)
        frame[:1] = sky
        frame[3, 1:7] = sky  # a crack running from sky the source shows at its left end
        self.assertEqual(sky_pixels(frame, sky, lambda ys, xs: xs == 1), (4, 8))

# A camera at the origin looking down +z, and the distance of the walls in front of it. Its right is world -x and
# its up world +y (r3d.poses.camera_basis); with lens 1, a pixel of a view `height` pixels tall spans
# 2 * WALL / height world units on the wall.
FORWARD = np.array([0.0, 0, 0, 0, 0, 1]) if np is not None else None
WALL = 10.0


def quad(x0, x1, y0, y1):
    """A RayQuery of one rectangle on the wall plane."""
    corners = np.array([[x0, y0, WALL], [x1, y0, WALL], [x1, y1, WALL], [x0, y1, WALL]])
    return RayQuery(corners, np.array([[0, 1, 2], [0, 2, 3]]))


@unittest.skipIf(np is None, "the r3d environment is not installed")
@needs_llvm
class PixelSky(unittest.TestCase):
    def test_sky_in_part_of_a_pixel_is_source_sky_where_the_centre_ray_hits(self):
        # One pixel 20 units wide on the wall, whose sides stop 5 short of the pixel's: the centre ray hits it,
        # the rays near the pixel's sides pass it, as sky beside a flagpole does.
        one = (np.array([0]), np.array([0]))
        self.assertFalse(pixel_sky(quad(-5, 5, -20, 20), 1, 1, 1.0, 0.1, FORWARD, *one, samples=1)[0])
        self.assertTrue(pixel_sky(quad(-5, 5, -20, 20), 1, 1, 1.0, 0.1, FORWARD, *one)[0])
        self.assertFalse(pixel_sky(quad(-20, 20, -20, 20), 1, 1, 1.0, 0.1, FORWARD, *one)[0])

    def test_sky_only_in_a_pixel_corner_counts(self):
        # A wall whose edge x + y = 16 cuts one corner off a pixel spanning -10..10: of 8 x 8 rays only the
        # corner one, 1/16 of a pixel in, passes it; 4 x 4 rays, 1/8 in, all hit.
        wall = RayQuery(np.array([[-100, -100, WALL], [116, -100, WALL], [-100, 116, WALL]]), np.array([[0, 1, 2]]))
        one = (np.array([0]), np.array([0]))
        self.assertTrue(pixel_sky(wall, 1, 1, 1.0, 0.1, FORWARD, *one)[0])
        self.assertFalse(pixel_sky(wall, 1, 1, 1.0, 0.1, FORWARD, *one, samples=4)[0])

    def test_each_pixel_is_answered_for_itself(self):
        # A 4 x 2 view whose pixels are 10 units square on the wall; a rectangle covers pixel (y 0, x 3) alone,
        # world x -20..-10 and y 0..10, a little past its sides but short of the neighbours' rays.
        ys, xs = (axis.reshape(-1) for axis in np.mgrid[0:2, 0:4])
        expected = ~((ys == 0) & (xs == 3))
        np.testing.assert_array_equal(pixel_sky(quad(-20.3, -9.7, -0.3, 10.3), 4, 2, 1.0, 0.1, FORWARD, ys, xs), expected)

    def test_a_frame_asks_the_source_at_the_next_pose(self):
        # Frame i is drawn at the pose (i + 1) steps along the clip.
        source = SourceSky.__new__(SourceSky)
        source.query = quad(-20, 20, -20, 20)
        source.width = source.height = 1
        source.lens, source.near = 1.0, 0.1
        source.poses = [np.array([0.0, 0, 0, 0, 0, -1]), FORWARD]  # away from the wall, then at it
        self.assertFalse(source.of_frame(0)(np.array([0]), np.array([0]))[0])


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SkyThroughWalls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scene = load_scene(SCENE)
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
        holed, frames = measure_sky(self.scene, scene, self.out, self.binary)
        print(f"{scene}: {len(holed)}/{frames} holed frames (ceiling {most}): {holed}", flush=True)
        self.assertLessEqual(len(holed), most, f"{scene}: {len(holed)} holed frames exceeds {most}")


if __name__ == "__main__":
    unittest.main()
