#!/usr/bin/env python3
"""Writes a board's tunable values into the TUNE(...) lines they came from.

    autana tune --json | python launcher/tools/tune/apply.py
    python launcher/tools/tune/apply.py values.json

The input is `autana tune --json` output; this tree's own launcher/main is
what gets rewritten. A value the source already has is left alone, and a
declared macro or expression becomes the number it stood for.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TUNE_LINE = re.compile(r"^TUNE(?:_HOT)?\(\s*(\w+)\s*,\s*(\w+)\s*,\s*([^,]+?)\s*,", re.MULTILINE)


def literal(name, value):
    return f"0x{int(value):06X}" if name.endswith("_rgb") else str(int(value))


def declarations(root):
    """{tunable name: file}, from the TUNE(owner, what, ...) lines under root/launcher/main."""
    found = {}
    for source in (root / "launcher" / "main").rglob("*.[ch]"):
        for owner, what, _value in TUNE_LINE.findall(source.read_text(encoding="utf-8", errors="replace")):
            found[f"{owner}.{what}"] = source
    return found


def same(old, value):
    try:
        return int(old, 0) == int(value)
    except ValueError:
        return False


def apply(values, root):
    """values: {name: int}. Prints each change; returns how many were written."""
    where = declarations(root)
    changed = 0
    for name, value in values.items():
        if name not in where:
            print(f"  {name}: not declared in this tree - skipped")
            continue
        source = where[name]
        text = source.read_bytes().decode("utf-8")
        declared = next(match for match in TUNE_LINE.finditer(text) if f"{match[1]}.{match[2]}" == name)
        if same(declared[3], value):
            continue
        start, end = declared.span(3)
        source.write_bytes((text[:start] + literal(name, value) + text[end:]).encode("utf-8"))
        print(f"  {name}: {declared[3]} -> {literal(name, value)}   ({source.relative_to(root).as_posix()})")
        changed += 1
    print(f"{changed} value(s) written" if changed else "the source already has the board's values")
    return changed


def main(argv):
    if len(argv) > 1:
        sys.exit("usage: apply.py [values.json]   (autana tune --json output; stdin when omitted)")
    text = Path(argv[0]).read_text(encoding="utf-8") if argv else sys.stdin.read()
    try:
        values = {row["name"]: row["value"] for row in json.loads(text)["tunables"]}
    except (ValueError, KeyError, TypeError):
        sys.exit("apply.py: expected `autana tune --json` output")
    apply(values, ROOT)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
