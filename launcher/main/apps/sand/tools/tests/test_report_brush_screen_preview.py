"""The brush-screen preview script builds and runs end to end.

    python -m unittest discover -s launcher/main/apps/sand/tools/tests

The script imports a converter from another tool folder by path, and nothing
else exercises that path, so a move of the converter breaks it unseen.
"""
import pathlib
import shutil
import subprocess
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = TOOLS / "report_brush_screen_preview.sh"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class BrushScreenPreviewTest(unittest.TestCase):
    def test_it_writes_a_png_for_each_orientation(self):
        shell = shutil.which("sh")
        if shell is None:
            self.skipTest("no POSIX shell on PATH")
        done = subprocess.run([shell, str(SCRIPT)], capture_output=True, text=True)
        if "No C compiler found" in done.stderr:
            self.skipTest("no host C compiler")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        for name in ("brush_screen_portrait", "brush_screen_landscape"):
            png = TOOLS / "build" / f"{name}.png"
            self.assertTrue(png.read_bytes().startswith(PNG_SIGNATURE), png)


if __name__ == "__main__":
    unittest.main()
