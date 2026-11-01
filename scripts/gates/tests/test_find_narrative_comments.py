"""Tombstone comments are found in the repository the gate runs in, C or C++."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

GATE = pathlib.Path(__file__).resolve().parents[1] / "find_narrative_comments.py"


class TombstoneTests(unittest.TestCase):
    def test_a_cpp_tombstone_outside_launcher_is_found_from_the_repository_top(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            subprocess.check_call(["git", "init", "-q"], cwd=root)
            (root / "src").mkdir()
            (root / "src/view.cpp").write_text("// Seeking moved to frames.cpp.\nint view;\n", encoding="utf-8")
            (root / "src/still.hpp").write_text("// Frames a capture holds.\nint still;\n", encoding="utf-8")
            result = subprocess.run([sys.executable, str(GATE), "--tombstones"], cwd=root,
                                    capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("src/view.cpp:1:", result.stdout)
        self.assertNotIn("still.hpp", result.stdout)


if __name__ == "__main__":
    unittest.main()
