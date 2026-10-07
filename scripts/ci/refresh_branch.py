#!/usr/bin/env python3
"""Build a doc refresh branch on the current main.

The doc-image workflows render on the commit that triggered them, and main may move before they
finish: a generated table moves to another document, or a workflow file changes (GitHub refuses an
app push whose branch differs from main in one). So nothing of the run's own checkout is replayed:
the branch starts from the current main, takes the run's changed images as files, and writes the
run's tables into whichever documents hold their blocks now.

    python scripts/ci/refresh_branch.py BRANCH MESSAGE --tables DIR [IMAGE ...]

Each IMAGE is a path under docs/images/ as the run left it; one the run deleted is deleted. REFRESH_REMOTE
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

UNCHANGED = 3


def git(*args, cwd):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout


def refresh(root, branch, message, tables, images, remote="origin"):
    """Pushes `branch`: the remote's main plus the run's `images` and `tables`.
    Returns False, pushing nothing, when that changes nothing."""
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
    apply_tables(root, pathlib.Path(tables))
    git("add", "--update", "--", "*.md", cwd=root)
    if not git("diff", "--cached", "--name-only", cwd=root).strip():
        return False
    git("-c", "user.name=github-actions[bot]",
        "-c", "user.email=41898282+github-actions[bot]@users.noreply.github.com",
        "commit", "-q", "-m", message, cwd=root)
    git("push", "-q", "--force", remote, branch, cwd=root)
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("branch")
    parser.add_argument("message")
    parser.add_argument("--tables", required=True, type=pathlib.Path)
    parser.add_argument("images", nargs="*")
    args = parser.parse_args(argv)
    pushed = refresh(pathlib.Path.cwd(), args.branch, args.message, args.tables, args.images,
                     os.environ.get("REFRESH_REMOTE", "origin"))
    return 0 if pushed else UNCHANGED


if __name__ == "__main__":
    sys.exit(main())
