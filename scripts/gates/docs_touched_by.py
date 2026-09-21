#!/usr/bin/env python3
"""List documentation citations for symbols changed since a base revision.

    python scripts/gates/docs_touched_by.py <base-ref> [--json]
"""
import json
import pathlib
import re
import subprocess
import sys

from check_doc_citations import citations
from code_vocabulary import source_paths

FUNCTION = re.compile(r"\b([a-z_][a-z0-9_]*)\s*\([^;]*\)\s*\{")
PYTHON_FUNCTION = re.compile(r"^\s*def\s+([a-z_][a-z0-9_]*)\s*\(")
MACRO = re.compile(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]+)\b")


def changed_definitions(base):
    result = subprocess.run(
        ["git", "diff", "--find-renames", "--unified=0", "--diff-filter=ACMRD",
         f"{base}...HEAD"],
        check=True, capture_output=True, text=True, encoding="utf-8",
    )
    found = {}
    path = None
    for line in result.stdout.splitlines():
        if line.startswith(("+++ b/", "--- a/")):
            path = line[6:]
            continue
        if not line or line[0] not in "+-" or line.startswith(("+++", "---")):
            continue
        text = line[1:]
        function = FUNCTION.search(text) if path and path.endswith((".c", ".h")) else None
        if path and path.endswith(".py"):
            function = PYTHON_FUNCTION.search(text)
        macro = MACRO.search(text)
        if function:
            found.setdefault(("function", function.group(1)), set()).add(path)
        if macro:
            found.setdefault(("macro", macro.group(1)), set()).add(path)
    return found


def changed_symbols(base):
    return set(changed_definitions(base))


def definition_counts(root):
    counts = {}
    for path in source_paths(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            function = FUNCTION.search(line) if path.suffix in {".c", ".h"} else None
            if path.suffix == ".py":
                function = PYTHON_FUNCTION.search(line)
            macro = MACRO.search(line)
            if function:
                key = ("function", function.group(1))
                counts.setdefault(key, set()).add(path.relative_to(root).as_posix())
            if macro:
                key = ("macro", macro.group(1))
                counts.setdefault(key, set()).add(path.relative_to(root).as_posix())
    return counts


def cites_path(citation, path):
    if citation.kind != "path":
        return False
    return path == citation.value or path.endswith("/" + citation.value)


def touched(root, base):
    definitions = changed_definitions(base)
    symbols = set(definitions)
    counts = definition_counts(root)
    doc_paths = {}
    for citation in citations(root):
        if citation.kind == "path":
            doc_paths.setdefault(citation.doc, []).append(citation)
    hits = []
    for citation in citations(root):
        key = (citation.kind, citation.value)
        changed_paths = definitions.get(key, set())
        if key in symbols and (len(counts.get(key, ())) <= 1 or
                               any(cites_path(path_citation, path)
                                   for path in changed_paths
                                   for path_citation in doc_paths.get(citation.doc, []))):
            hits.append({"doc": citation.doc, "line": citation.line,
                         "kind": citation.kind, "symbol": citation.value})
    return sorted(symbols), hits


def main(argv):
    json_mode = "--json" in argv
    args = [arg for arg in argv if arg != "--json"]
    if len(args) != 1:
        print("usage: docs_touched_by.py <base-ref> [--json]", file=sys.stderr)
        return 2
    base = args[0]
    symbols, hits = touched(pathlib.Path("."), base)
    if json_mode:
        print(json.dumps({"base": base, "symbols": [
            {"kind": kind, "symbol": symbol} for kind, symbol in symbols],
            "citations": hits}, indent=2))
        return 0
    if not hits:
        print(f"No documentation citations match symbols changed since {base}.")
        return 0
    print(f"Documentation citations to review since {base}:")
    for hit in hits:
        suffix = "()" if hit["kind"] == "function" else ""
        print(f"{hit['doc']}:{hit['line']}: cites {hit['symbol']}{suffix}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
