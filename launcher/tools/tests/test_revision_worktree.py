"""Tests for launcher/tools/revision_worktree.sh, the detached checkouts render_compare.sh, perf_compare.sh and
perf_sweep.sh share.

    python -m unittest discover -s launcher/tools/tests
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "revision_worktree.sh"
# The status the shell line below exits with when the checkout fails.
CHECKOUT_FAILED = 7


class RevisionWorktreeTest(unittest.TestCase):
    def test_a_submodule_that_cannot_be_cloned_fails_the_checkout_and_cleanup_removes_its_tree(self):
        if shutil.which("git") is None or shutil.which("sh") is None:
            self.skipTest("needs git and sh")
        with tempfile.TemporaryDirectory() as tmp:
            repo, work = Path(tmp) / "repo", Path(tmp) / "work"
            repo.mkdir()
            work.mkdir()

            def git(*args):
                return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], cwd=repo,
                                      check=True, capture_output=True, text=True).stdout

            git("init", "-q")
            (repo / ".gitmodules").write_text('[submodule "gone"]\n\tpath = gone\n\turl = ../no-such-repo\n')
            commit = subprocess.run(["git", "hash-object", "-w", "--stdin"], input="", cwd=repo,
                                    capture_output=True, text=True, check=True).stdout.strip()
            git("update-index", "--add", "--cacheinfo", f"160000,{commit},gone")
            git("add", ".gitmodules")
            git("commit", "-q", "-m", "a submodule nobody can clone")
            run = subprocess.run(
                ["sh", "-c", '. "$1"; revision_worktree_setup "$2" "$3"; '
                 'tree=$(revision_worktree_checkout HEAD a) || { revision_worktree_cleanup; exit "$4"; }; '
                 'revision_worktree_cleanup',
                 "sh", str(SCRIPT), str(repo), str(work), str(CHECKOUT_FAILED)],
                capture_output=True, text=True)
            self.assertEqual(run.returncode, CHECKOUT_FAILED, run.stdout + run.stderr)
            self.assertIn("cannot check out the submodules of HEAD", run.stderr)
            trees = [line for line in git("worktree", "list", "--porcelain").splitlines()
                     if line.startswith("worktree ")]
            self.assertEqual(len(trees), 1, trees)


if __name__ == "__main__":
    unittest.main()
