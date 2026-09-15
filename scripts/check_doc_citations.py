#!/usr/bin/env python3
"""Fail when a documentation citation no longer resolves in this tree.

    python scripts/check_doc_citations.py [--root ROOT]
"""
import pathlib
import re
import sys

from code_vocabulary import names

INLINE = re.compile(r"`([^`\n]+)`")
FUNCTION = re.compile(r"^([a-z_][a-z0-9_]*)\(\)$")
MACRO = re.compile(r"^[A-Z][A-Z0-9_]*$")
FILE = re.compile(r"^(?:launcher/|apps/|[\w.-]+/)*[\w.-]+\.(?:c|h|py|sh)$")
SKIP_FENCES = {"sh", "shell", "bash", "console", "text", "output"}
SKIP = {"build", "build.dev", "build.diag", "managed_components", ".git"}
FOREIGN_FUNCTIONS = {"exit", "main", "max", "name"}
FOREIGN_PATHS = {"idf.py"}


class Citation:
    def __init__(self, doc, line, kind, value):
        self.doc = doc
        self.line = line
        self.kind = kind
        self.value = value


def documentation(root):
    root = pathlib.Path(root)
    yield from sorted((root / "docs").rglob("*.md"))
    yield from sorted(root.glob("*.md"))


def citations(root):
    root = pathlib.Path(root)
    for path in documentation(root):
        fenced = False
        skip = False
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if line.lstrip().startswith("```"):
                if not fenced:
                    language = line.lstrip()[3:].strip().lower()
                    skip = language in SKIP_FENCES
                    fenced = True
                else:
                    fenced = skip = False
                continue
            if skip:
                continue
            for text in INLINE.findall(line):
                function = FUNCTION.fullmatch(text)
                if function:
                    yield Citation(path.relative_to(root).as_posix(), number,
                                   "function", function.group(1))
                elif FILE.fullmatch(text):
                    yield Citation(path.relative_to(root).as_posix(), number,
                                   "path", text)
                elif MACRO.fullmatch(text):
                    yield Citation(path.relative_to(root).as_posix(), number,
                                   "macro", text)


def allowlist(root):
    path = pathlib.Path(root) / "scripts/doc_citation_allowlist.txt"
    allowed = set()
    if not path.exists():
        return allowed
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3 or not all(fields):
            raise ValueError(f"{path}:{number}: expected doc, citation, reason")
        allowed.add((fields[0], fields[1]))
    return allowed


def path_exists(root, value):
    root = pathlib.Path(root)
    if value.startswith("apps/"):
        return (root / "launcher/main" / value).exists()
    if "/" in value:
        candidates = [root / value, root / "launcher" / value,
                      root / "launcher/main" / value]
        return any(path.exists() for path in candidates) or any(
            path.as_posix().endswith("/" + value) for path in root.rglob("*")
            if not any(part in SKIP for part in path.parts))
    return any(path.name == value for path in root.rglob(value)
               if not any(part in SKIP for part in path.parts))


def check(root):
    root = pathlib.Path(root)
    functions, macros = names(root)
    allowed = allowlist(root)
    missing = []
    for citation in citations(root):
        if (citation.doc, citation.value) in allowed:
            continue
        if citation.kind == "function" and (
                citation.value in FOREIGN_FUNCTIONS or
                citation.value.startswith(("esp_", "xTask", "vTask", "heap_caps_"))):
            continue
        if citation.kind == "path" and citation.value in FOREIGN_PATHS:
            continue
        if citation.kind == "macro":
            prefix = citation.value.split("_", 1)[0]
            prefixes = {name.split("_", 1)[0] for name in macros}
            if "_" not in citation.value or prefix not in prefixes:
                continue
        exists = (citation.value in functions if citation.kind == "function" else
                  citation.value in macros if citation.kind == "macro" else
                  path_exists(root, citation.value))
        if not exists:
            missing.append(citation)
    return missing


def main(argv):
    root = pathlib.Path(".")
    if argv[:1] == ["--root"] and len(argv) == 2:
        root = pathlib.Path(argv[1])
    elif argv:
        print("usage: check_doc_citations.py [--root ROOT]", file=sys.stderr)
        return 2
    try:
        missing = check(root)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    for item in missing:
        label = item.value + "()" if item.kind == "function" else item.value
        print(f"{item.doc}:{item.line}: missing {item.kind} citation {label}")
    print(f"{len(missing)} missing documentation citation"
          f"{'' if len(missing) == 1 else 's'}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
