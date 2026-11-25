"""Sky enclosed by walls over one whole camera flythrough at the device frame step.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests -p test_sky_through_walls.py

Builds the host renderer once and samples the scene camera clip with sample_camera_rows
at DT_MS intervals to determine the frame count. ANY clear-colour pixel not
connected to the top row makes a frame holed, including four-pixel pinholes,
unless its patch of enclosed sky holds a pixel the source mesh shows sky in
too, in any part of it: sky the source shows, as the roof opening closed in by
a beam or a notch in its rim, is no hole of the bake, and neither is that
opening's rim a pixel off the source's.
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
    from r3d.obj import load_obj
    from r3d.poses import flight_camera, parse_poses, sample_camera_rows
    from r3d.import_settings import load_scene
    from r3d.leaks import CONNECTED
    from r3d.ray_query import RayQuery
    from pinhole_rays import VIDEO_SCALE, enclosed_sky, pose_rays
    from check_avi import read_video
    from render_compare import expand_565
except ImportError:  # pragma: no cover - the tool tests' environment has them
    np = None

DT_MS = 33
MIN_OPEN_SKY_PIXELS = 100
FRAME_COUNT_SIZE = (1, 1)
# Rays per side of a render pixel when asking the source for sky: a notch in a jagged rim or the gap under a
# flagpole opens to the sky through a channel narrower than a pixel, which a ray through the centre can miss.
SUBPIXELS = 8
# Per scene, the holed frames of its current bake plus one: the ceiling a rebake must not go above.
MOST_HOLED_FRAMES = {"sponza": 1}


def clear_colour():
    """The scene's camera background as the 16-bit framebuffer shows it."""
    rgb = load_scene(SCENE).camera.component.background
    return expand_565((rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF))


def sky_pixels(frame, sky, source_sky=None):
    """(holes, open): the pixels of colour `sky` in an (h, w, 3) RGB frame not connected to its top row, and
    those that are. With `source_sky`, which answers for the (ys, xs) it is given whether the source shows sky
    there, a patch of enclosed pixels, joined by side or corner, is no hole when the source shows sky in any of
    them: the rule r3d/leaks.py seals by."""
    mask, enclosed = enclosed_sky(frame, sky)
    open_ = int(mask.sum() - enclosed.sum())
    ys, xs = np.nonzero(enclosed)
    if source_sky is not None and len(ys):
        patches, count = ndimage.label(enclosed, CONNECTED)
        patch = patches[ys, xs]
        shown = np.zeros(count + 1, dtype=bool)
        shown[patch[source_sky(ys, xs)]] = True
        return int((~shown[patch]).sum()), open_
    return len(ys), open_


def pixel_sky(query, width, height, lens, near, pose, ys, xs, samples=SUBPIXELS):
    """Whether `query`'s mesh leaves any of `samples` x `samples` rays spread over each render pixel (ys, xs) of
    a width x height view from `pose` empty: sky the source shows in part of a pixel counts."""
    origins, directions = pose_rays(width, height, lens, near, pose, samples)
    per_pixel = samples * samples
    rays = ((ys * width + xs)[:, None] * per_pixel + np.arange(per_pixel)).reshape(-1)
    return ~query.blocked(origins[rays], directions[rays]).reshape(-1, per_pixel).all(axis=1)


class SourceSky:
    """Where the scene's source mesh leaves part of a flythrough frame's pixel empty (pixel_sky), for the render
    pixel a video pixel was drawn from."""

    def __init__(self, scene, frames, video_size):
        camera = flight_camera(scene)
        sources = {job.settings.source["path"] for job in scene.renderers}
        if len(sources) != 1:
            raise ValueError(f"the scene's renderers draw {len(sources)} sources; the test traces one")
        positions, _, tri_v, *_ = load_obj(sources.pop())
        self.query = RayQuery(positions, tri_v)
        # Frame i is drawn at (i + 1) * DT_MS, so the last frame's pose is one step past the clip's samples.
        self.width, self.height, self.lens, self.near, self.poses = parse_poses(track_host.poses(
            camera.path.animation, camera.path.node, DT_MS, video_size[0] // VIDEO_SCALE, video_size[1] // VIDEO_SCALE,
            camera.half_fov_short_tan, camera.near_z, until_ms=(frames + 1) * DT_MS, number_format=".9g"))

    def of_frame(self, frame):
        """`source_sky` of sky_pixels for the flythrough's frame `frame`."""
        def source_sky(ys, xs):
            return pixel_sky(self.query, self.width, self.height, self.lens, self.near, self.poses[frame + 1],
                             ys // VIDEO_SCALE, xs // VIDEO_SCALE)
        return source_sky


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

    def test_patches_join_by_side_or_corner(self):
        sky = (156, 195, 231)
        frame = np.zeros((8, 8, 3), dtype=np.uint8)
        frame[:1] = sky
        frame[3, 3] = frame[4, 4] = sky  # corner to corner: one patch
        frame[3, 6] = frame[4, 6] = sky  # side by side: one patch
        frame[6, 1] = frame[6, 3] = sky  # a pixel apart: two patches
        source = {(3, 3), (3, 6), (6, 1)}
        self.assertEqual(sky_pixels(frame, sky, lambda ys, xs: np.array([(y, x) in source for y, x in zip(ys, xs)])),
                         (1, 8))


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

    def test_a_video_frame_asks_the_source_at_its_render_pixel_and_next_pose(self):
        # Frame i is drawn at the pose (i + 1) steps along the clip, and a video pixel at twice the render size.
        source = SourceSky.__new__(SourceSky)
        source.query = quad(-20, 20, -20, 20)
        source.width = source.height = 1
        source.lens, source.near = 1.0, 0.1
        source.poses = [np.array([0.0, 0, 0, 0, 0, -1]), FORWARD]  # away from the wall, then at it
        video_pixel = (np.array([VIDEO_SCALE - 1]), np.array([VIDEO_SCALE - 1]))
        self.assertFalse(source.of_frame(0)(*video_pixel)[0])


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SkyThroughWalls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scene = load_scene(SCENE)
        camera = flight_camera(scene)
        cls.frames = len(sample_camera_rows(
            camera.path.animation, camera.path.node, DT_MS, *FRAME_COUNT_SIZE,
            camera.half_fov_short_tan, camera.near_z)[4])
        cls.scene, cls.source = scene, None
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
        counts = []
        for index, frame in enumerate(read_video(video)[1]):
            if type(self).source is None:
                type(self).source = SourceSky(self.scene, self.frames, frame.shape[1::-1])
            counts.append(sky_pixels(frame, sky, self.source.of_frame(index)))
        video.unlink()
        self.assertEqual(len(counts), self.frames)
        # The path looks up at the open roof, so sky open to the top must turn up: that proves the clear colour
        # matched, so a count of no holes is real.
        self.assertTrue(any(open_ >= MIN_OPEN_SKY_PIXELS for _, open_ in counts), f"{scene}: no sky found")
        holed = [frame for frame, (holes, _) in enumerate(counts) if holes > 0]
        print(f"{scene}: {len(holed)}/{self.frames} holed frames (ceiling {most}): {holed}", flush=True)
        self.assertLessEqual(len(holed), most, f"{scene}: {len(holed)} holed frames exceeds {most}")


if __name__ == "__main__":
    unittest.main()
