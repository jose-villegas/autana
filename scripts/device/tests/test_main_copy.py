"""A board tool started from a worktree runs the main checkout's copy, so every
session on the machine runs one version of the lock code. Built on a real git
repository with a real worktree: the hand-off is a git fact, not a path rule."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import isolation  # noqa: F401  (redirects the record and lock roots)

MAIN_COPY = Path(__file__).resolve().parents[1] / "main_copy.py"

PROBE = """import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import main_copy
if __name__ == "__main__":
    main_copy.run_main_checkout_copy(__file__)
    print("{copy}", *sys.argv[1:])
    raise SystemExit({status})
"""


def git(cwd, *args):
    subprocess.run(["git", "-C", str(cwd), *args], check=True, capture_output=True)


class MainCopyTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.main = self.root / "main"
        tools = self.main / "scripts" / "device"
        tools.mkdir(parents=True)
        shutil.copy(MAIN_COPY, tools / "main_copy.py")
        (tools / "probe.py").write_text(PROBE.format(copy="worktree-copy", status=0))
        git(self.main, "init", "-q")
        git(self.main, "-c", "user.name=t", "-c", "user.email=t@t", "add", ".")
        git(self.main, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "tools")
        self.worktree = self.root / "wt"
        git(self.main, "worktree", "add", "-q", str(self.worktree))
        # The main checkout's tool differs from the worktree's committed copy.
        (tools / "probe.py").write_text(PROBE.format(copy="main-copy", status=3))

    def run_probe(self, checkout, **env):
        environment = {k: v for k, v in os.environ.items() if k != "AUTANA_DEVICE_TOOLS"}
        environment.update(env)
        return subprocess.run([sys.executable, str(checkout / "scripts" / "device" / "probe.py"),
                               "--board", "X", "status"],
                              capture_output=True, text=True, env=environment)

    def test_a_worktree_copy_runs_the_main_checkouts_with_its_arguments(self):
        result = self.run_probe(self.worktree)
        self.assertEqual(result.stdout.split(), ["main-copy", "--board", "X", "status"])
        self.assertEqual(result.returncode, 3)

    def test_the_main_checkouts_copy_runs_itself(self):
        self.assertEqual(self.run_probe(self.main).stdout.split()[0], "main-copy")

    def test_here_runs_the_worktrees_own_copy(self):
        result = self.run_probe(self.worktree, AUTANA_DEVICE_TOOLS="here")
        self.assertEqual((result.stdout.split()[0], result.returncode), ("worktree-copy", 0))

    def test_outside_git_a_tool_runs_itself(self):
        loose = self.root / "loose" / "scripts" / "device"
        loose.mkdir(parents=True)
        shutil.copy(MAIN_COPY, loose / "main_copy.py")
        (loose / "probe.py").write_text(PROBE.format(copy="loose-copy", status=0))
        environment = dict(os.environ, GIT_CEILING_DIRECTORIES=str(self.root))
        environment.pop("AUTANA_DEVICE_TOOLS", None)
        result = subprocess.run([sys.executable, str(loose / "probe.py")],
                                capture_output=True, text=True, env=environment)
        self.assertEqual(result.stdout.split()[0], "loose-copy")


if __name__ == "__main__":
    unittest.main()
