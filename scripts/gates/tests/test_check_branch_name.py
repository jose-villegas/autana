import pathlib
import subprocess
import unittest


CHECKER = pathlib.Path(__file__).resolve().parents[1] / "check-branch-name.sh"


class BranchNameTests(unittest.TestCase):
    def test_names(self):
        valid = (
            "main",
            "__dolt_remote_info__",
            "feature/branch-name-rule",
            "bugfix/touch-2",
            "hotfix/boot",
            "release/v1.2.0",
            "release/1-2.0",
        )
        invalid = (
            "",
            "codex/branch-name-rule",
            "claude/branch-name-rule",
            "Feature/foo",
            "feature/Foo",
            "feature/with_underbar",
            "feature/two/parts",
            "feature/-leading",
            "feature/trailing-",
            "feature/double--hyphen",
            "feature/v1.2",
            "release/.leading",
            "release/trailing.",
            "release/double..dot",
            "release/mixed.-separators",
            "release/mixed-.separators",
            "release/UPPER",
            "release/extra/path",
        )
        for name in valid:
            with self.subTest(name=name):
                result = subprocess.run(["sh", str(CHECKER), name], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
        for name in invalid:
            with self.subTest(name=name):
                result = subprocess.run(["sh", str(CHECKER), name], capture_output=True, text=True)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(len(result.stderr.splitlines()), 1)
                self.assertIn("feature/<what-it-does>", result.stderr)


if __name__ == "__main__":
    unittest.main()
