"""render_lab.framegen reaches only the scenes that declare generates_frames.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests -p test_framegen_scenes.py

Builds the host renderer once and draws each scene's first frames with
render_lab.framegen off and on. A scene whose objects move on their own (the
water, the capybara) must draw the same frames either way; a baked scene
must not, since every second frame there is generated.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[4] / "tools" / "render"))
import host_render  # noqa: E402

try:
    from render_compare import read_video
except ImportError:  # pragma: no cover - the tool tests' environment has it
    read_video = None

FRAMES = 12
DT_MS = 33
UNSUPPORTED = ("water", "capybara")
SUPPORTED = "bistro-fitted-full"


@unittest.skipIf(read_video is None, "Pillow and numpy are not installed")
class FrameGenerationScenes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = pathlib.Path(tempfile.mkdtemp(prefix="framegen_scenes_"))
        run, cls.binary = host_render.run(cls.out, "--build-only")
        cls.build_output = run.stdout + run.stderr

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def frames(self, scene, generating):
        self.assertTrue(self.binary.exists(), self.build_output[-2000:])
        video = self.out / f"{scene}-{generating}.avi"
        run = subprocess.run([str(self.binary), "--quarter", "0", "--no-hud", "--scene", scene, "--frames",
                              str(FRAMES), "--dt", str(DT_MS), "--tune", f"render_lab.framegen={generating}",
                              "-o", str(self.out / f"{scene}-{generating}.bmp"), "--video", str(video)],
                             capture_output=True, text=True, timeout=600)
        self.assertEqual(run.returncode, 0, run.stderr[-2000:])
        return [frame.tobytes() for frame in read_video(video)[1]]

    def generated(self, scene):
        """The frames that differ between render_lab.framegen off and on."""
        return [i for i, (off, on) in enumerate(zip(self.frames(scene, 0), self.frames(scene, 1))) if off != on]

    def test_a_scene_with_its_own_motion_draws_every_frame(self):
        for scene in UNSUPPORTED:
            with self.subTest(scene=scene):
                self.assertEqual([], self.generated(scene), f"{scene} generated these frames")

    def test_a_baked_scene_generates_frames(self):
        self.assertTrue(self.generated(SUPPORTED), f"{SUPPORTED} generated no frame")


if __name__ == "__main__":
    unittest.main()
