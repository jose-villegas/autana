"""Regression tests for scripts/gates/tracked.py."""
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

import tracked  # noqa: E402


class TrackedFilesTest(unittest.TestCase):
    def test_outside_git_every_file_matching_the_patterns_is_listed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "docs").mkdir()
            (root / "docs/Guide.md").write_text("x\n", encoding="utf-8")
            (root / "notes.txt").write_text("x\n", encoding="utf-8")
            self.assertEqual(tracked.tracked_files(root, ["*.md"]), ("docs/Guide.md",))

    def test_directory_pathspec_outside_git_includes_descendants(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "launcher/main").mkdir(parents=True)
            (root / "launcher/main/example.c").write_text("x\n", encoding="utf-8")
            (root / "other.c").write_text("x\n", encoding="utf-8")
            self.assertEqual(tracked.tracked_files(root, ["launcher"]), ("launcher/main/example.c",))

    def test_any_other_git_failure_raises_with_gits_message(self):
        # A container running as another user than the checkout's owner:
        # git refuses the repository, and a fallback to the walk would list
        # untracked files as if CI saw them.
        refused = subprocess.CompletedProcess(
            [], 128, "", "fatal: detected dubious ownership in repository at '/w'\n")
        with tempfile.TemporaryDirectory() as temp:
            with mock.patch.object(tracked.subprocess, "run", return_value=refused):
                with self.assertRaisesRegex(RuntimeError, "dubious ownership"):
                    tracked.tracked_files(pathlib.Path(temp))

    def test_committable_skips_what_git_ignores_and_keeps_a_new_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            for name, text in {".gitignore": ".cache/\n", "new.c": "x\n",
                               ".cache/venv/numpy.h": "x\n"}.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text, encoding="utf-8")
            self.assertEqual(tracked.committable(root), [root / ".gitignore", root / "new.c"])


if __name__ == "__main__":
    unittest.main()
