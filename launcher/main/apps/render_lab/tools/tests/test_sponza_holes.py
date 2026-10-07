"""Sky through the walls of the baked Sponza meshes, counted along the camera path.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests

Builds the host renderer once, flies each region-visibility bake (full and
lite) over the path in 600 frames 50 ms apart, and counts per frame the
pixels of the clear colour that are not connected to the top row: sky
showing where a wall should be. A frame with more than HOLE_PIXELS of them
is a holed frame; a bake may have no more holed frames than the 2026-10-04
bake had when counted this way. The few legitimately enclosed sky patches
(the roof opening seen straight up) are in that count too.

Needs numpy and scipy (launcher/tools/r3d/requirements.txt).
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import tomllib
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = TOOLS / "render_lab_render_host.sh"
SCENE = TOOLS.parent / "meshes" / "sponza.scene.toml"
sys.path.insert(0, str(TOOLS.parents[3] / "tools" / "render"))

try:
    import numpy as np
    from scipy import ndimage

    import check_avi
except ImportError:  # pragma: no cover - the tool tests' environment has them
    np = None

FRAMES, DT_MS = 600, 50
HOLE_PIXELS = 50
# Holed frames of the 2026-10-04 bake (da84175a) under this count.
MOST_HOLED_FRAMES = {"sponza": 10, "sponza-lite": 17}


def clear_colour():
    """The scene's camera background as the 16-bit framebuffer shows it."""
    with SCENE.open("rb") as source:
        scene = tomllib.load(source)
    rgb = next(item["camera"]["background"] for item in scene["objects"] if "camera" in item)
    r, g, b = (rgb >> 19) & 0x1F, (rgb >> 10) & 0x3F, (rgb >> 3) & 0x1F
    return (r << 3 | r >> 2), (g << 2 | g >> 4), (b << 3 | b >> 2)


def hole_pixels(path, sky):
    """Per frame of a renderer's --video, the sky pixels not connected to the top row."""
    video = check_avi.read_avi(path)
    stride = (video.width * 3 + 3) // 4 * 4
    bgr_sky = np.array(sky[::-1], dtype=np.uint8)
    counts = []
    for body in video.frames:
        rows = np.frombuffer(body, dtype=np.uint8).reshape(video.height, stride)
        bgr = rows[::-1, : video.width * 3].reshape(video.height, video.width, 3)
        mask = (bgr == bgr_sky).all(axis=2)
        labels, _ = ndimage.label(mask)
        top = labels[0][labels[0] > 0]
        counts.append(int((mask & ~np.isin(labels, top)).sum()))
    return counts


@unittest.skipIf(np is None, "needs numpy and scipy")
class SponzaHoles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = pathlib.Path(tempfile.mkdtemp(prefix="sponza_holes_"))
        env = dict(os.environ, scene_baseline=str(cls.out / "no-baseline.txt"))
        shell = shutil.which("sh") or shutil.which("bash")
        run = subprocess.run([shell, str(SCRIPT), "-o", str(cls.out), "--build-only"], capture_output=True, text=True,
                             timeout=900, env=env)
        cls.build_output = run.stdout + run.stderr
        cls.binary = cls.out / ("render_lab_render.exe" if os.name == "nt" else "render_lab_render")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def holed_frames(self, scene):
        self.assertTrue(self.binary.exists(), self.build_output[-2000:])
        video = self.out / f"{scene}.avi"
        run = subprocess.run([str(self.binary), "--quarter", "1", "--no-hud", "--scene", scene, "--frames", str(FRAMES),
                              "--dt", str(DT_MS), "-o", str(self.out / f"{scene}.bmp"), "--video", str(video)],
                             capture_output=True, text=True, timeout=600)
        self.assertEqual(run.returncode, 0, run.stderr[-2000:])
        counts = hole_pixels(video, clear_colour())
        video.unlink()
        self.assertEqual(len(counts), FRAMES)
        return [frame for frame, count in enumerate(counts) if count > HOLE_PIXELS]

    def test_the_full_bake_shows_no_more_sky_through_walls_than_before(self):
        holed = self.holed_frames("sponza")
        self.assertLessEqual(len(holed), MOST_HOLED_FRAMES["sponza"], f"holed frames {holed}")

    def test_the_lite_bake_shows_no_more_sky_through_walls_than_before(self):
        holed = self.holed_frames("sponza-lite")
        self.assertLessEqual(len(holed), MOST_HOLED_FRAMES["sponza-lite"], f"holed frames {holed}")


if __name__ == "__main__":
    unittest.main()
