#!/usr/bin/env python3
"""Build a refresh branch on the current main.

The refresh workflows (doc images, ESP-IDF) work on the commit that triggered them, and main may
move before they finish: a generated table moves to another document, or a workflow file changes
(GitHub refuses an app push whose branch differs from main in one). So nothing of the run's own
checkout is replayed: the branch starts from the current main, takes the run's changed files as
they are, and writes the run's tables, if any, into whichever documents hold their blocks now.
When a changed file is a doc image, the figure index and its thumbnails are regenerated too.

    python scripts/ci/refresh_branch.py BRANCH MESSAGE [--tables DIR] [FILE ...]

Each FILE is a repository-relative path as the run left it; one the run deleted is deleted. REFRESH_REMOTE
names the remote (origin by default). Exits 0 having pushed BRANCH, or 3 when main already matches
the run and there is nothing to commit.
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
from generated_blocks import apply_tables  # noqa: E402
sys.path.insert(0, str(ROOT / "launcher/tools"))
from docs.figure_index import IMAGES, PAGE, THUMBNAILS, WRITE  # noqa: E402

UNCHANGED = 3


def git(*args, cwd):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout


def refresh(root, branch, message, tables, images, remote="origin", derive=None):
    """Pushes `branch`: the remote's main plus the run's changed files (`images`) and `tables`, then
    whatever `derive(root, images)` regenerates from them. Returns False, pushing nothing, when that
    changes nothing."""
    root = pathlib.Path(root)
    with tempfile.TemporaryDirectory() as keep:
        kept = {}
        for image in images:
            source = root / image
            if source.is_file():
                kept[image] = pathlib.Path(keep) / str(len(kept))
                shutil.copyfile(source, kept[image])
            else:
                kept[image] = None
        git("fetch", "--depth=1", remote, "main", cwd=root)
        git("reset", "-q", "--hard", cwd=root)
        git("switch", "-q", "-C", branch, "FETCH_HEAD", cwd=root)
        for image, copy in kept.items():
            target = root / image
            if copy is None:
                git("rm", "-q", "--ignore-unmatch", "--", image, cwd=root)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(copy, target)
            git("add", "--", image, cwd=root)
    if tables is not None:
        apply_tables(root, pathlib.Path(tables))
        git("add", "--update", "--", "*.md", cwd=root)
    if derive is not None:
        derive(root, images)
    if not git("diff", "--cached", "--name-only", cwd=root).strip():
        return False
    git("-c", "user.name=github-actions[bot]",
        "-c", "user.email=41898282+github-actions[bot]@users.noreply.github.com",
        "commit", "-q", "-m", message, cwd=root)
    git("push", "-q", "--force", remote, branch, cwd=root)
    return True


def figure_index(root, files):
    """Regenerate the figure index when a doc image changed, so its page and thumbnails match."""
    if any(name.startswith(IMAGES + "/") for name in files):
        subprocess.run([sys.executable, *WRITE.split()[1:]], cwd=root, check=True)
        git("add", "-A", "--", PAGE, THUMBNAILS, cwd=root)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("branch")
    parser.add_argument("message")
    parser.add_argument("--tables", type=pathlib.Path)
    parser.add_argument("images", nargs="*")
    args = parser.parse_args(argv)
    pushed = refresh(pathlib.Path.cwd(), args.branch, args.message, args.tables, args.images,
                     os.environ.get("REFRESH_REMOTE", "origin"), figure_index)
    return 0 if pushed else UNCHANGED


if __name__ == "__main__":
    sys.exit(main())
