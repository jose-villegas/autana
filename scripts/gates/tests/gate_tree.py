"""Files and committed trees for gate regression fixtures."""
from pathlib import Path
import subprocess


def write(root, path, text):
    target = Path(root) / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8")


def commit(root, *paths):
    git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
    subprocess.run(git + ["init", "-q"], cwd=root, check=True)
    subprocess.run(git + ["add", *paths], cwd=root, check=True)
    subprocess.run(git + ["commit", "-qm", "fixture"], cwd=root, check=True)
