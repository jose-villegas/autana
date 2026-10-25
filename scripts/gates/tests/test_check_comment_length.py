"""Regression tests for --changed comment scoping."""
import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import gate_tree

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "check_comment_length.py"


class CommentLengthTest(unittest.TestCase):
    def test_import_c_comments_runs_no_subprocess(self):
        spec = importlib.util.spec_from_file_location("fresh_c_comments", SCRIPT.with_name("c_comments.py"))
        module = importlib.util.module_from_spec(spec)
        with mock.patch.object(sys, "path", [str(SCRIPT.parent), *sys.path]), \
                mock.patch("subprocess.run", side_effect=AssertionError("subprocess during import")):
            spec.loader.exec_module(module)
        with mock.patch.object(module, "generated_files", return_value=("generated.h",)) as generated:
            self.assertEqual(module.excluded(),
                             ("launcher/components/", "launcher/packages/testkit/unity/", "generated.h"))
            self.assertIs(module.excluded(), module.excluded())
            generated.assert_called_once_with()

    def git(self, root, *args):
        return subprocess.run(["git", *args], cwd=root, check=True,
                              capture_output=True, text=True)

    def test_paths_exclude_vendored_unity_with_relative_or_absolute_names(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.git(root, "init", "-q")
            name = "launcher/packages/testkit/unity/unity.h"
            gate_tree.write(root, name, "/* " + "vendor " * 100 + " */\n")
            for path in (name, str(root / name)):
                with self.subTest(path=path):
                    result = subprocess.run(
                        [sys.executable, str(SCRIPT), "--limit", "500", "--paths", path],
                        cwd=root, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("files scanned   0", result.stdout)

    def test_split_comment_is_not_new_but_new_over_limit_comment_is(self):
        moved = "moved comment " + "x" * 330
        added = "new comment " + "y" * 510
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.git(root, "init", "-q")
            self.git(root, "config", "user.name", "test")
            self.git(root, "config", "user.email", "test@example.com")
            gate_tree.write(root, "original.c",
                            f"/* {moved} */\nvoid old(void) {{}}\n")
            self.git(root, "add", "original.c")
            self.git(root, "commit", "-qm", "base")
            base = self.git(root, "rev-parse", "HEAD").stdout.strip()
            (root / "original.c").unlink()
            gate_tree.write(root, "moved.c", f"/* {moved} */\nvoid moved(void) {{}}\n")
            gate_tree.write(root, "added.c", f"/* {added} */\nvoid added(void) {{}}\n")
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
