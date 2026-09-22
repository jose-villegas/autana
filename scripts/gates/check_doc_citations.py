#!/usr/bin/env python3
"""Fail when a documentation citation no longer resolves in this tree.

    python scripts/gates/check_doc_citations.py [--root ROOT]
"""
import pathlib
import re
import subprocess
import sys

from code_vocabulary import names

INLINE = re.compile(r"`([^`\n]+)`")
FUNCTION = re.compile(r"^([a-z][a-z0-9_]*)\(\)$")
MACRO = re.compile(r"^[A-Z][A-Z0-9_]*$")
FILE = re.compile(r"^(?:launcher/|apps/|[\w.-]+/)*(?:[\w.-]+\.(?:c|h|py|sh|cmake)|CMakeLists\.txt)$")
SKIP_FENCES = {"sh", "shell", "bash", "console", "text", "output"}
SKIP = {"build", "build.dev", "build.diag", "build.qemu", "build.qemu.perf", "build.qemu.shell", "managed_components", ".git"}
FOREIGN_FUNCTIONS = {"exit", "main", "max", "name", "bsp_display_new"}
FOREIGN_PATHS = {"idf.py"}
FOREIGN_MACRO_PREFIXES = ("ESP", "CONFIG_COMPILER", "CONFIG_LOG", "IDF", "SDMMC", "WHOLE", "LOG", "DP")


class Citation:
    def __init__(self, doc, line, kind, value):
        self.doc = doc
        self.line = line
        self.kind = kind
        self.value = value


def documentation(root):
    root = pathlib.Path(root)
    yield from sorted((root / "docs").rglob("*.md"))
    result = subprocess.run(["git", "ls-files", "*.md"], cwd=root,
                            capture_output=True, text=True)
    if result.returncode:
        yield from sorted(root.glob("*.md"))
        return
    for name in sorted(result.stdout.splitlines()):
        if "/" not in name:
            yield root / name


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


ALLOWLIST = "scripts/gates/doc_citation_allowlist.txt"
PLANS = ("docs/plans/*", "*")


def allowlist(root):
    """(doc, citation) -> allowlist line number."""
    path = pathlib.Path(root) / ALLOWLIST
    allowed = {}
    if not path.exists():
        return allowed
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3 or not all(fields):
            raise ValueError(f"{path}:{number}: expected doc, citation, reason")
        allowed[(fields[0], fields[1])] = number
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


def allowlist_entry(allowed, citation):
    if (citation.doc, citation.value) in allowed:
        return (citation.doc, citation.value)
    if PLANS in allowed and citation.doc.startswith("docs/plans/"):
        return PLANS
    return None


def unresolved(root):
    """Every citation that does not resolve, allowlisted or not."""
    root = pathlib.Path(root)
    functions, macros = names(root)
    missing = []
    for citation in citations(root):
        if citation.kind == "function" and (
                citation.value in FOREIGN_FUNCTIONS or
                citation.value.startswith(("esp_", "xTask", "vTask", "heap_caps_"))):
            continue
        if citation.kind == "path" and citation.value in FOREIGN_PATHS:
            continue
        if citation.kind == "macro":
            if citation.value.startswith(FOREIGN_MACRO_PREFIXES):
                continue
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


def check(root):
    allowed = allowlist(root)
    return [citation for citation in unresolved(root)
            if allowlist_entry(allowed, citation) is None]


def stale_allowlist(root):
    """Allowlist entries that no unresolved citation needs, as (line, doc, citation)."""
    allowed = allowlist(root)
    used = {allowlist_entry(allowed, citation) for citation in unresolved(root)}
    return sorted((number, *entry) for entry, number in allowed.items() if entry not in used)


def main(argv):
    root = pathlib.Path(".")
    if argv[:1] == ["--root"] and len(argv) == 2:
        root = pathlib.Path(argv[1])
    elif argv:
        print("usage: check_doc_citations.py [--root ROOT]", file=sys.stderr)
        return 2
    try:
        missing = check(root)
        stale = stale_allowlist(root)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    for item in missing:
        label = item.value + "()" if item.kind == "function" else item.value
        print(f"{item.doc}:{item.line}: missing {item.kind} citation {label}")
    for number, doc, citation in stale:
        print(f"{ALLOWLIST}:{number}: stale entry {doc} {citation}: nothing left to allow")
    print(f"{len(missing)} missing documentation citation"
          f"{'' if len(missing) == 1 else 's'}, {len(stale)} stale allowlist "
          f"entr{'y' if len(stale) == 1 else 'ies'}")
    return 1 if missing or stale else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
