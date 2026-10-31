#!/usr/bin/env python3
"""Fail on a source file that does not open by saying what it is.

    python scripts/gates/check_file_headers.py [--paths FILE...]

Two rules, one per kind of file:

- Every tracked header (.h, .hh, .hpp, .hxx) and Python module states its
  purpose: a header opens with a comment (a `#pragma once` may come first), a
  module with a docstring, and its first sentence is the purpose that
  file_purpose() reads and launcher/tools/gen/package_headers.py prints.
- A firmware module, a .c and its .h with the same name in the same folder
  (in a package, launcher/packages/<name>/, its include/<name>/<path>.h and
  src/<path>.c), opens one of them with that comment. The scope is every
  tracked .c and .h under launcher/main/, launcher/packages/ and
  launcher/test/suites/; a suite (suite_*.c) that runs tests of its own
  (RUN_TEST) is exempt, its test names say what it proves, but one that only
  includes another file's tests names nothing and needs the comment. A header
  there that declares nothing, only a comment,
  fails too: a folder is described by its README.md, not by an empty header.

Vendored and generated sources are exempt from both (check_comment_length.py
owns that list and the rest of a header's rules).
"""
import ast
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from c_comments import blank_comments, file_header, sources  # noqa: E402
from check_doc_constants import sentences  # noqa: E402
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "launcher/tools"))
from build.packages import FIRST_PARTY, package_part  # noqa: E402

SCOPE = (*FIRST_PARTY, "launcher/test/suites/")
PRAGMA_ONCE = re.compile(r"^\s*#\s*pragma\s+once\b", re.M)
HEADERS = (".h", ".hh", ".hpp", ".hxx")
RUN_TEST = re.compile(r"\bRUN_TEST\(\s*\w+")


def file_purpose(path, text):
    """A header's or Python module's purpose: the first sentence of its opening comment or
    docstring, on one line; empty when it states none."""
    if path.endswith(".py"):
        prose = ast.get_docstring(ast.parse(text, filename=path)) or ""
    else:
        header = file_header(path, text)
        prose = header.text if header else ""
    return " ".join(next(sentences(None, prose), (0, ""))[1].split())


def in_scope(rp, root="."):
    """A firmware source the module rule covers: a suite only when its own text runs no test."""
    if not rp.startswith(SCOPE):
        return False
    if not rp.rsplit("/", 1)[-1].startswith("suite_"):
        return True
    text = (pathlib.Path(root) / rp).read_text(encoding="utf-8", errors="replace")
    return RUN_TEST.search(blank_comments(text)) is None


def module_of(rp):
    """A source's module: its path without the suffix, a package's header and
    source both spelled from the package folder."""
    part = package_part(rp)
    if part is not None:
        name, kind = part
        half = {"include": f"include/{name}/", "src": "src/"}.get(kind)
        package = f"launcher/packages/{name}/"
        if half is not None and rp.startswith(package + half):
            rp = package + rp.removeprefix(package + half)
    return rp.rsplit(".", 1)[0]


def module_problems(root, files):
    selected = {module_of(rp) for rp in files} if files is not None else None
    modules = {}
    for path in sources(root):
        rp = path.relative_to(root).as_posix()
        if in_scope(rp, root):
            modules.setdefault(module_of(rp), []).append(rp)
    found = []
    for stem, files in sorted(modules.items()):
        if selected is not None and stem not in selected:
            continue
        texts = {rp: (pathlib.Path(root) / rp).read_text(encoding="utf-8", errors="replace") for rp in files}
        if not any(file_header(rp, text) for rp, text in texts.items()):
            found.append(f"{sorted(files)[-1]}: no header comment - open it with one saying what the module is")
        for rp, text in sorted(texts.items()):
            if rp.endswith(".h") and not PRAGMA_ONCE.sub("", blank_comments(text)).strip():
                found.append(f"{rp}: declares nothing - describe its folder in a README.md instead")
    return found


def purpose_problems(root, files):
    found = []
    for path in sources(root, extensions=(*HEADERS, ".py")):
        rp = path.relative_to(root).as_posix()
        if files is not None and rp not in files:
            continue
        if not file_purpose(rp, path.read_text(encoding="utf-8", errors="replace")):
            found.append(f"{rp}: no purpose - open it with a comment or docstring saying what it is for")
    return found


def problems(root=".", files=None):
    found = module_problems(root, files)
    # A module already named for its missing header is not named again for each file of it.
    named = {module_of(line.split(":", 1)[0]) for line in found}
    return found + [line for line in purpose_problems(root, files) if module_of(line.split(":", 1)[0]) not in named]


def main():
    found = problems(files=sys.argv[2:] if sys.argv[1:2] == ["--paths"] else None)
    for line in found:
        print(line)
    print(f"{len(found)} file(s) that do not say what they are, or header(s) declaring nothing")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
