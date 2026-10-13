"""Every exact requirement pin is listed beside what is installed, and --drift keeps only the differences."""
import contextlib
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import versions


class VersionsTests(unittest.TestCase):
    def test_exact_pins_in_tracked_requirement_files_only(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            (root / "tools").mkdir()
            (root / "tools" / "requirements.txt").write_text("# comment\nnumpy==2.4.6\nloose>=1\n")
            (root / "requirements-extra.txt").write_text("torch==2.11.0+cu128\n")
            (root / "untracked-requirements.txt").write_text("ghost==1.0\n")
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "add", "tools", "requirements-extra.txt"], cwd=root, check=True)
            self.assertEqual(sorted(versions.pinned_requirements(root)), [
                ("requirements-extra.txt", "torch", "2.11.0+cu128"),
                ("tools/requirements.txt", "numpy", "2.4.6")])

    def test_drift_lists_only_differences_and_fails(self):
        table = [("Python", "3.14.8", "3.14.8"), ("Node", "24.21.0", "24.18.1")]
        with mock.patch.object(versions, "rows", return_value=table), \
             contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(versions.main(["--drift"]), 1)
        self.assertNotIn("Python", out.getvalue())
        self.assertIn("pinned 24.21.0", out.getvalue())
        self.assertIn("here 24.18.1  <- differs", out.getvalue())
        with mock.patch.object(versions, "rows", return_value=table[:1]), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(versions.main(["--drift"]), 0)


if __name__ == "__main__":
    unittest.main()
