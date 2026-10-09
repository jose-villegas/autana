"""The engine viewer and this app draw the same authored scene at the same pose."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from PIL import Image
import host_render

ROOT = pathlib.Path(__file__).resolve().parents[6]
VIEWER = ROOT / "launcher/tools/render/scenes/scene_viewer_render_host.sh"
SCENE = ROOT / "launcher/demo/sponza/sponza.scene.toml"


class SceneViewerEquivalence(unittest.TestCase):
    def test_viewer_matches_the_app_pixel_for_pixel(self):
        with tempfile.TemporaryDirectory(prefix="viewer_equivalence_") as directory:
            out = pathlib.Path(directory)
            built, app = host_render.run(out / "app", "--build-only")
            self.assertEqual(built.returncode, 0, (built.stdout + built.stderr)[-3000:])
            built = subprocess.run([shutil.which("sh"), str(VIEWER), "--build-only", "-o", str(out / "viewer"),
                                    "--asset-file", str(SCENE)], capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, (built.stdout + built.stderr)[-3000:])
            viewer = out / "viewer" / ("scene_viewer_render.exe" if os.name == "nt" else "scene_viewer_render")
            for frames, dt, quarter, view in ((2, 16, 1, "shaded"), (5, 100, 0, "depth")):
                with self.subTest(frames=frames, dt=dt, quarter=quarter, view=view):
                    common = ["--frames", str(frames), "--dt", str(dt), "--quarter", str(quarter), "--view", view]
                    for binary, options, target in ((app, ["--scene", "sponza", "--no-hud"], "app.bmp"),
                                                    (viewer, ["--scene", "sponza", "--object", "atrium"], "viewer.bmp")):
                        rendered = subprocess.run([str(binary), *options, *common, "-o", str(out / target)],
                                                  capture_output=True, text=True, timeout=120)
                        self.assertEqual(rendered.returncode, 0, rendered.stderr[-3000:])
                    with Image.open(out / "app.bmp") as expected, Image.open(out / "viewer.bmp") as actual:
                        self.assertEqual(actual.size, expected.size)
                        self.assertEqual(actual.tobytes(), expected.tobytes())
