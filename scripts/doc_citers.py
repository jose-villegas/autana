#!/usr/bin/env python3
"""Which documents cite what a change touched.

    python scripts/doc_citers.py FILE...          docs citing what each file's
                                                  uncommitted change touches
    python scripts/doc_citers.py --changed REF    warn for docs citing what the
                                                  diff since REF touches, when
                                                  that diff leaves them alone

A citation is what check_doc_citations.py already reads out of a document:
a backticked path, `name()` or MACRO. A change touches a function when a
changed line lies inside it (git's hunk header names the enclosing function)
or defines it, and touches a macro whose #define line changed. A document
citing only the file's path is reported when the change touches no named
function or macro. Plans are skipped - they keep proposed and superseded
names on purpose. Nothing here fails: a touched citation is a reason to
reread the document, not proof it is wrong.
"""
import os
import pathlib
import re
import subprocess
import sys

from check_doc_citations import FOREIGN_FUNCTIONS, citations

SOURCE_SUFFIXES = {".c", ".h", ".py", ".sh"}
# Defined in every suite, so a citation of one says nothing about which.
GENERIC_FUNCTIONS = {"fixture"}
HUNK = re.compile(r"^@@ [^@]* @@ ?(.*)$")
C_NAME = re.compile(r"^([a-z_][a-z0-9_]*)\s*\(")
PY_NAME = re.compile(r"^\s*def ([a-z_][a-z0-9_]*)\s*\(")
MACRO = re.compile(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]+)\b")


def names_on(line, suffix):
    """(functions, macros) a single source line defines or opens."""
    if suffix == ".py":
        found = PY_NAME.match(line)
        return ({found.group(1)} if found else set()), set()
    if suffix in (".c", ".h"):
        function = C_NAME.match(line)
        macro = MACRO.match(line)
        return ({function.group(1)} if function else set()), ({macro.group(1)} if macro else set())
    return set(), set()


def touched_names(root, source, diff_args):
    """(functions, macros) a diff of `source` touches."""
    result = subprocess.run(["git", "diff", "-U0", *diff_args, "--", source], cwd=root,
                            capture_output=True, text=True)
    suffix = pathlib.PurePosixPath(source).suffix
    functions, macros = set(), set()
    for line in result.stdout.splitlines():
        hunk = HUNK.match(line)
        if hunk:
            opened, _ = names_on(hunk.group(1), suffix)
            functions |= opened
        elif line[:1] in "+-" and not line.startswith(("+++", "---")):
            opened, defined = names_on(line[1:], suffix)
            functions |= opened
            macros |= defined
    return functions - FOREIGN_FUNCTIONS - GENERIC_FUNCTIONS, macros


def cites_path(value, source):
    if value.startswith("apps/"):
        value = "launcher/main/" + value
    return source == value or source.endswith("/" + value)


def citers(root, sources, diff_args):
    """source -> {doc: sorted citations of what its diff touched}."""
    root = pathlib.Path(root)
    touched = {source: touched_names(root, source, diff_args) for source in sources
               if pathlib.PurePosixPath(source).suffix in SOURCE_SUFFIXES}
    result = {source: {} for source in touched}
    if not touched:
        return result
    for citation in citations(root):
        if citation.doc.startswith("docs/plans/"):
            continue
        for source, (functions, macros) in touched.items():
            if (citation.kind == "path" and not (functions or macros) and cites_path(citation.value, source) or
                    citation.kind == "function" and citation.value in functions or
                    citation.kind == "macro" and citation.value in macros):
                label = citation.value + "()" if citation.kind == "function" else citation.value
                result[source].setdefault(citation.doc, set()).add(label)
    return {source: {doc: sorted(labels) for doc, labels in docs.items()}
            for source, docs in result.items() if docs}


def describe(docs):
    return "; ".join(f"{doc} ({', '.join(labels)})" for doc, labels in sorted(docs.items()))


def main(argv):
    root = pathlib.Path(".")
    if argv[:1] == ["--changed"] and len(argv) == 2:
        diff = subprocess.run(["git", "diff", "--name-only", f"{argv[1]}...HEAD"], cwd=root,
                              check=True, capture_output=True, text=True)
        changed = [line for line in diff.stdout.splitlines() if line]
        in_ci = os.environ.get("GITHUB_ACTIONS") == "true"
        warnings = 0
        for source, docs in sorted(citers(root, changed, [f"{argv[1]}...HEAD"]).items()):
            untouched = {doc: labels for doc, labels in docs.items() if doc not in changed}
            if not untouched:
                continue
            warnings += 1
            text = f"{source} changed what these documents cite, and they were not updated: {describe(untouched)}"
            print(f"::warning file={source}::{text}" if in_ci else text)
        print(f"{warnings} changed source file{'' if warnings == 1 else 's'} with unrevised citing documents")
        return 0
    if not argv or argv[0].startswith("-"):
        print(__doc__, file=sys.stderr)
        return 2
    sources = [pathlib.Path(arg).as_posix() for arg in argv]
    for source, docs in sorted(citers(root, sources, ["HEAD"]).items()):
        print(f"{source}: {describe(docs)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
