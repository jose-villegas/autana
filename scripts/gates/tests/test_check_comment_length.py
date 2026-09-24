"""Regression tests for --changed comment scoping."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "check_comment_length.py"


class CommentLengthTest(unittest.TestCase):
    def git(self, root, *args):
        return subprocess.run(["git", *args], cwd=root, check=True,
                              capture_output=True, text=True)

    def write(self, root, name, text):
        path = root / name
        path.write_text(text, encoding="utf-8")

    def test_split_comment_is_not_new_but_new_over_limit_comment_is(self):
        moved = "moved comment " + "x" * 330
        added = "new comment " + "y" * 510
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.git(root, "init", "-q")
            self.git(root, "config", "user.name", "test")
            self.git(root, "config", "user.email", "test@example.com")
            self.write(root, "original.c",
                       f"/* {moved} */\nvoid old(void) {{}}\n")
            self.git(root, "add", "original.c")
            self.git(root, "commit", "-qm", "base")
            base = self.git(root, "rev-parse", "HEAD").stdout.strip()
            (root / "original.c").unlink()
            self.write(root, "moved.c", f"/* {moved} */\nvoid moved(void) {{}}\n")
            self.write(root, "added.c", f"/* {added} */\nvoid added(void) {{}}\n")
            self.git(root, "add", "-A")
            self.git(root, "commit", "-qm", "split")
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--changed", base,
                 "--limit", "300", "--top", "0"],
                cwd=root, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertNotIn("moved comment", result.stdout)
        self.assertIn("new comment", result.stdout)


if __name__ == "__main__":
    unittest.main()
