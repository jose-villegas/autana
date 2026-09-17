#!/usr/bin/env python3
"""Fail when current-board documentation uses retired-target vocabulary."""
import pathlib
import re
import sys

ESCAPE = "<!-- doc-vocabulary: ignore -->"
STALE_EXCEPTION = "stale exception"


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


def retired_uses(path, name, retired_terms):
    found = []
    previous_escape = False
    for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        escaped = ESCAPE in line or previous_escape
        previous_escape = ESCAPE in line
        if escaped:
            continue
        for term, reason in retired_terms:
            if re.search(r"(?<![A-Za-z0-9-])" + re.escape(term) + r"(?![A-Za-z0-9-])", line,
                         re.IGNORECASE):
                found.append((name, number, term, reason))
    return found


def check(root):
    root = pathlib.Path(root)
    skipped = exceptions(root)
    retired_terms = terms(root)
    found = []
    for name in sorted(skipped):
        if not (root / name).is_file():
            found.append((name, 0, STALE_EXCEPTION, "exception entry names no file"))
        elif not retired_uses(root / name, name, retired_terms):
            found.append((name, 0, STALE_EXCEPTION, "file no longer uses a retired term"))
    for path in sorted((root / "docs").rglob("*.md")):
        name = path.relative_to(root).as_posix()
        if name not in skipped:
            found.extend(retired_uses(path, name, retired_terms))
    return found


def main(argv):
    root = pathlib.Path(".") if not argv else pathlib.Path(argv[1]) if argv[:1] == ["--root"] and len(argv) == 2 else None
    if root is None:
        print("usage: check_doc_vocabulary.py [--root ROOT]", file=sys.stderr)
        return 2
    found = check(root)
    for path, line, term, reason in found:
        if term == STALE_EXCEPTION:
            print(f"{path}: stale exception entry: {reason}")
        else:
            print(f"{path}:{line}: retired term {term!r}: {reason}")
    return 1 if found else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
