#!/usr/bin/env python3
"""Fail when a tracked document under docs/ cannot be reached by following
Markdown links from the repository's README.md.

    python scripts/check_doc_index.py [--root ROOT]

A link to a folder reaches that folder's README.md. Only real links count -
a path written in backticks names a document, it does not index it.
"""
import pathlib
import re
import subprocess
import sys

LINK = re.compile(r"\]\(([^)\s]+)\)")
START = "README.md"


def tracked_docs(root):
    result = subprocess.run(["git", "ls-files", "docs"], cwd=root,
                            capture_output=True, text=True)
    if result.returncode:
        return {path.relative_to(root).as_posix() for path in (root / "docs").rglob("*.md")}
    return {name for name in result.stdout.splitlines() if name.endswith(".md")}


def link_targets(root, doc):
    path = root / doc
    fenced = False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        for target in LINK.findall(line):
            target = target.split("#", 1)[0]
            if not target or "://" in target or target.startswith("mailto:"):
                continue
            resolved = (path.parent / target).resolve()
            if resolved.is_dir():
                resolved = resolved / "README.md"
            try:
                yield resolved.relative_to(root.resolve()).as_posix()
            except ValueError:
                continue


def reachable(root):
    root = pathlib.Path(root)
    seen = set()
    queue = [START] if (root / START).is_file() else []
    while queue:
        doc = queue.pop()
        if doc in seen or not doc.endswith(".md") or not (root / doc).is_file():
            continue
        seen.add(doc)
        queue.extend(link_targets(root, doc))
    return seen


def check(root):
    root = pathlib.Path(root)
    return sorted(tracked_docs(root) - reachable(root))


def main(argv):
    root = pathlib.Path(".")
    if argv[:1] == ["--root"] and len(argv) == 2:
        root = pathlib.Path(argv[1])
    elif argv:
        print("usage: check_doc_index.py [--root ROOT]", file=sys.stderr)
        return 2
    orphans = check(root)
    for doc in orphans:
        print(f"{doc}: not reachable by links from {START}")
    print(f"{len(orphans)} unindexed document{'' if len(orphans) == 1 else 's'}")
    return 1 if orphans else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
