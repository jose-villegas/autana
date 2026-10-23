"""renderer.sh lists the renderer's sources as renderer.py does: every .c below the folders
main/renderer_folders.txt names."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / "launcher/tools/build"
sys.path.insert(0, str(ROOT / "scripts/device"))
sys.path.insert(0, str(BUILD))
sys.path.insert(0, str(ROOT / "scripts/device/tests"))
import port_guard  # noqa: E402,F401  (before device: no test reaches a real board)
import device  # noqa: E402
import renderer  # noqa: E402

SHELL = device.git_bash() if os.name == "nt" else shutil.which("sh")


def shell_sources(launcher):
    script = f'. "{(BUILD / "renderer.sh").as_posix()}"\nrenderer_sources "{Path(launcher).as_posix()}"\n'
    result = subprocess.run([SHELL, "-c", script], capture_output=True, text=True, timeout=120,
                            stdin=subprocess.DEVNULL)
    assert result.returncode == 0, result.stderr
    return result.stdout.splitlines()


def python_sources(launcher):
    return [path.relative_to(launcher).as_posix() for path in renderer.sources(launcher)]


class RendererSources(unittest.TestCase):
    def test_every_c_file_below_a_named_folder_and_nothing_else(self):
        with tempfile.TemporaryDirectory() as temp:
            launcher = Path(temp)
            for name in ("main/alpha/x.c", "main/alpha/deep/y.c", "main/alpha/x.h", "main/beta/z.c", "main/gamma/w.c"):
                (launcher / name).parent.mkdir(parents=True, exist_ok=True)
                (launcher / name).write_text("", encoding="utf-8")
            (launcher / "main/renderer_folders.txt").write_text("# not a folder\nalpha\nbeta\n", encoding="utf-8")
            expected = ["main/alpha/deep/y.c", "main/alpha/x.c", "main/beta/z.c"]
            self.assertEqual(expected, python_sources(launcher))
            self.assertEqual(expected, shell_sources(launcher))

    def test_the_twins_agree_on_this_tree(self):
        launcher = ROOT / "launcher"
        listed = python_sources(launcher)
        self.assertIn("main/render/raster.c", listed)
        self.assertEqual(listed, shell_sources(launcher))


if __name__ == "__main__":
    unittest.main()
