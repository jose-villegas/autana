"""Files and committed trees for gate regression fixtures."""
import contextlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/device"))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/device/tests"))
import port_guard  # noqa: E402,F401  (before device: no test reaches a real board)
import device


def write(root, path, text):
    target = Path(root) / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8")


def git(root, *args, identity=("t", "t@t"), environment=None):
    """git in `root` as `identity`, so a commit needs no configured user."""
    subprocess.run(["git", "-c", f"user.name={identity[0]}", "-c", f"user.email={identity[1]}", *args], cwd=root,
                   check=True, capture_output=True, env=environment)


def commit(root, *paths, message="fixture", identity=("t", "t@t"), environment=None):
    git(root, "init", "-q", identity=identity)
    git(root, "add", *paths, identity=identity)
    git(root, "commit", "-qm", message, identity=identity, environment=environment)
    return subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, check=True,
                          capture_output=True, text=True).stdout.strip()


@contextlib.contextmanager
def temporary_tree(files=()):
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        for path, text in files:
            write(root, path, text)
        yield root


class ShellGateTestCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.shell = device.git_bash() if os.name == 'nt' else shutil.which('sh')
