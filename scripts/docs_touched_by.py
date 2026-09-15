#!/usr/bin/env python3
"""List documentation citations for symbols changed since a base revision.

    python scripts/docs_touched_by.py <base-ref> [--json]
"""
import json
import pathlib
import re
import subprocess
import sys

from check_doc_citations import citations

FUNCTION = re.compile(r"\b([a-z_][a-z0-9_]*)\s*\([^;]*\)\s*\{")
MACRO = re.compile(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]+)\b")


def changed_symbols(base):
    result = subprocess.run(
        ["git", "diff", "--find-renames", "--unified=0", "--diff-filter=ACMRD",
         f"{base}...HEAD"],
        check=True, capture_output=True, text=True, encoding="utf-8",
    )
    found = set()
    for line in result.stdout.splitlines():
        if not line or line[0] not in "+-" or line.startswith(("+++", "---")):
            continue
        text = line[1:]
        function = FUNCTION.search(text)
        macro = MACRO.search(text)
        if function:
            found.add(("function", function.group(1)))
        if macro:
            found.add(("macro", macro.group(1)))
    return found


def touched(root, base):
    symbols = changed_symbols(base)
    hits = []
    for citation in citations(root):
        if (citation.kind, citation.value) in symbols:
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
