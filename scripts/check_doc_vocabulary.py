#!/usr/bin/env python3
"""Fail when current-board documentation uses retired-target vocabulary."""
import pathlib
import re
import sys


def terms(root):
    path = pathlib.Path(root) / "scripts/doc_vocabulary.txt"
    return [(fields[0], fields[1]) for line in path.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#") for fields in [line.split("\t", 1)]]


def exceptions(root):
    path = pathlib.Path(root) / "scripts/doc_vocabulary_exceptions.txt"
    if not path.exists():
        return set()
    return {line.split("\t", 1)[0] for line in path.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")}


def check(root):
    root = pathlib.Path(root)
    skipped = exceptions(root)
    found = []
    for path in sorted((root / "docs").rglob("*.md")):
        name = path.relative_to(root).as_posix()
        if name in skipped:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for term, reason in terms(root):
                if re.search(r"(?<![A-Za-z0-9-])" + re.escape(term) + r"(?![A-Za-z0-9-])", line,
                             re.IGNORECASE):
                    if term.lower() == "no psram" and "reads no PSRAM" in line:
                        continue
                    found.append((name, number, term, reason))
    return found


def main(argv):
    root = pathlib.Path(".") if not argv else pathlib.Path(argv[1]) if argv[:1] == ["--root"] and len(argv) == 2 else None
    if root is None:
        print("usage: check_doc_vocabulary.py [--root ROOT]", file=sys.stderr)
        return 2
    for path, line, term, reason in check(root):
        print(f"{path}:{line}: retired term {term!r}: {reason}")
    return 1 if check(root) else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
