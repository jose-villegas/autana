"""Runs render_lab_render_host.sh for the tests in this folder and names the renderer it builds."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "render_lab_render_host.sh"


def run(out, *args):
    """(the script's run into `out`, the renderer binary there). The pinned hashes belong to the compiler that
    pinned them, and these tests compare renders with each other or count their pixels, so no baseline is read."""
    env = dict(os.environ, scene_baseline=str(pathlib.Path(out) / "no-baseline.txt"))
    shell = shutil.which("sh") or shutil.which("bash")
    result = subprocess.run([shell, str(SCRIPT), "-o", str(out), *args], capture_output=True, text=True, timeout=900,
                            env=env)
    return result, pathlib.Path(out) / ("render_lab_render.exe" if os.name == "nt" else "render_lab_render")


class BuiltRenderer(unittest.TestCase):
    """A test class that builds the renderer once into a temporary folder named from `prefix`, removed after."""
    prefix = "render_lab_"

    @classmethod
    def setUpClass(cls):
        cls.out = pathlib.Path(tempfile.mkdtemp(prefix=cls.prefix))
        built, cls.binary = run(cls.out, "--build-only")
        cls.build_output = built.stdout + built.stderr

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def render(self, *args):
        """The renderer's run with `args`; the test fails when the build gave none or the run exits non-zero."""
        self.assertTrue(self.binary.exists(), self.build_output[-2000:])
        result = subprocess.run([str(self.binary), *args], capture_output=True, text=True, timeout=600)
        self.assertEqual(result.returncode, 0, result.stderr[-2000:])
        return result
