#!/usr/bin/env python3
"""Fail when a tracked document under docs/ cannot be reached by following
Markdown links from the repository's README.md, or when a link's `#anchor`
does not match a heading GitHub will actually generate that id for.

    python scripts/gates/check_doc_index.py [--root ROOT]

A link to a folder reaches that folder's README.md. Only real links count -
a path written in backticks names a document, it does not index it. An
anchor is checked only when the link's target file ends in `.md` - `#L3` in
a link to a script, or a Python `# comment` a naive scan could mistake for a
heading, are never a document heading and are not this gate's concern.
"""
import pathlib
import re
import subprocess
import sys

LINK = re.compile(r"\]\(([^)\s]+)\)")
HEADING = re.compile(r"^(#{1,6})\s+(.+?)\s*#*$")
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


def slugify(text):
    """GitHub's heading-id algorithm: lowercase, drop anything that isn't a
    letter/digit/space/hyphen, then turn each space - not each RUN of
    spaces - into its own hyphen. That last part matters: a heading
    punctuated with an em dash drops the dash but keeps both surrounding
    spaces, so it slugs with a double hyphen, not a single one."""
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"\*\*([^*]*)\*\*", r"\1", text)
    text = re.sub(r"\*([^*]*)\*", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = text.strip().lower()
    text = re.sub(r"[^\w\s-]", "", text)
    return text.replace(" ", "-")


def heading_slugs(path):
    if not path.is_file():
        return None
    slugs, counts = set(), {}
    fenced = False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        m = HEADING.match(line)
        if not m:
            continue
        slug = slugify(m.group(2))
        n = counts.get(slug, 0)
        counts[slug] = n + 1
        slugs.add(slug if n == 0 else f"{slug}-{n}")
    return slugs


def anchor_links(root):
    """(doc, line, displayed_target, fragment, resolved_path) for every
    link whose target - explicit or the same file - resolves to a `.md`
    file and carries a `#fragment`."""
    root = pathlib.Path(root)
    for doc in sorted(tracked_docs(root)):
        path = root / doc
        if not path.is_file():
            continue
        fenced = False
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if line.lstrip().startswith("```"):
                fenced = not fenced
                continue
            if fenced:
                continue
            for target in LINK.findall(line):
                if "://" in target or target.startswith("mailto:"):
                    continue
                file_part, sep, fragment = target.partition("#")
                if not sep or not fragment:
                    continue
                resolved = (path.parent / file_part).resolve() if file_part else path.resolve()
                if resolved.suffix != ".md":
                    continue
                try:
                    resolved.relative_to(root.resolve())
                except ValueError:
                    continue
                yield doc, number, file_part or pathlib.Path(doc).name, fragment, resolved


def check_anchors(root):
    """Every anchor link whose fragment does not match a real heading, as
    (doc, line, target, fragment, reason)."""
    root = pathlib.Path(root)
    bad = []
    cache = {}
    for doc, number, target, fragment, resolved in anchor_links(root):
        key = str(resolved)
        if key not in cache:
            cache[key] = heading_slugs(resolved)
        slugs = cache[key]
        if slugs is None:
            bad.append((doc, number, target, fragment, "target file does not exist"))
        elif fragment not in slugs:
            bad.append((doc, number, target, fragment, "no matching heading"))
    return bad


def main(argv):
    root = pathlib.Path(".")
    if argv[:1] == ["--root"] and len(argv) == 2:
        root = pathlib.Path(argv[1])
    elif argv:
        print("usage: check_doc_index.py [--root ROOT]", file=sys.stderr)
        return 2
    orphans = check(root)
    bad_anchors = check_anchors(root)
    for doc in orphans:
        print(f"{doc}: not reachable by links from {START}")
    for doc, number, target, fragment, reason in bad_anchors:
        print(f"{doc}:{number}: ({target}#{fragment}) {reason}")
    print(f"{len(orphans)} unindexed document{'' if len(orphans) == 1 else 's'}, "
          f"{len(bad_anchors)} anchor link{'' if len(bad_anchors) == 1 else 's'} to no real heading")
    return 1 if (orphans or bad_anchors) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
