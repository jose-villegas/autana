#!/usr/bin/env python3
"""Fail when documentation gives an unambiguous integer constant a wrong value.

    python scripts/check_doc_constants.py [--root ROOT] [--docs-ref REF] [--verbose]

Put ``<!-- doc-constants: ignore -->`` on a line to retain a deliberate
historical value. The allowlist is doc, name, claimed value, and reason,
separated by tabs in scripts/doc_constant_allowlist.txt.
"""
import pathlib
import re
import subprocess
import sys

from check_doc_citations import documentation

DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)\b(.*)$")
ENUM = re.compile(r"\benum\s*(?:[A-Za-z_]\w*\s*)?\{([^{}]*)\}", re.S)
MEMBER = re.compile(r"^\s*([A-Za-z_]\w*)\s*=\s*(.+?)\s*$", re.S)
DECIMAL = re.compile(r"(?<![\w.])(0|[1-9]\d*)(?!\w)")
HEX = re.compile(r"(?<!\w)0[xX][0-9A-Fa-f]+(?!\w)")
LITERAL = re.compile(r"(?:0|[1-9]\d*)[uUlL]*$")
HISTORICAL = re.compile(r"\b(?:was|used to be|before the retune|measured)\b", re.I)
UNIT = re.compile(r"\s*(?:-\s*)?(us|microseconds?|ms|milliseconds?|ns|nanoseconds?|s|seconds?|px|pixels?|"
                  r"cell(?:s)?|byte(?:s)?|KiB|MiB|Hz|kHz|MHz)\b", re.I)
ESCAPE = "<!-- doc-constants: ignore -->"
UNIT_SUFFIXES = {
    "US": "us", "MS": "ms", "NS": "ns", "S": "s", "PX": "px",
    "BYTE": "bytes", "BYTES": "bytes", "KIB": "kib", "MIB": "mib",
    "HZ": "hz", "KHZ": "khz", "MHZ": "mhz",
}
UNIT_NAMES = {
    "us": "us", "microsecond": "us", "microseconds": "us",
    "ms": "ms", "millisecond": "ms", "milliseconds": "ms",
    "ns": "ns", "nanosecond": "ns", "nanoseconds": "ns",
    "s": "s", "second": "s", "seconds": "s",
    "px": "px", "pixel": "px", "pixels": "px",
    "cell": "cells", "cells": "cells", "byte": "bytes", "bytes": "bytes",
    "kib": "kib", "mib": "mib", "hz": "hz", "khz": "khz", "mhz": "mhz",
}
SCALAR_FIELD = re.compile(r"\.([A-Za-z_]\w*)\s*=\s*(0|[1-9]\d*)[uUlL]*\s*(?:,|$)")
MATERIAL_ENTRY = re.compile(r"(?:TWIN_ROW\(\s*|\[\s*MATERIAL_ROW\(\s*)(MAT_[A-Z0-9_]+)")
ARRAY_ENTRY = re.compile(r"\[\s*(MAT(?:X)?_[A-Z0-9_]+|\d+)\s*\]\s*=\s*")
EXTENDED_NAME = re.compile(r"\[\s*(MATX_[A-Z0-9_]+|\d+)\s*\]\s*=\s*\"([^\"]+)\"")
MACRO = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)\b", re.M)
TABLE_FIELDS = frozenset({
    "density", "heat_chance", "heat_ramp", "dissolves", "dissolvable",
    "soaks", "soaked_chance",
})
FIELD_ALIASES = {"heat_chance": ("heat_chance", "heats_to")}
IMPLIED_UNIQUE_FIELDS = frozenset({"soaked_chance"})


class Mismatch:
    def __init__(self, doc, line, name, claimed, defined):
        self.doc = doc
        self.line = line
        self.name = name
        self.claimed = claimed
        self.defined = defined


def uncomment(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//.*", "", text)


def add(definitions, name, value):
    definitions.setdefault(name, []).append(value)


def constants(root):
    """Return only names with one literal definition in launcher/."""
    definitions = {}
    for path in sorted((pathlib.Path(root) / "launcher").rglob("*")):
        if path.suffix not in {".c", ".h"} or any(part.startswith("build") for part in path.parts):
            continue
        text = uncomment(path.read_text(encoding="utf-8", errors="replace"))
        for line in text.splitlines():
            match = DEFINE.match(line)
            if not match:
                continue
            literal = match.group(2).strip()
            add(definitions, match.group(1), int(re.sub(r"[uUlL]+$", "", literal)) if LITERAL.fullmatch(literal) else None)
        for body in ENUM.findall(text):
            for field in body.split(","):
                match = MEMBER.match(field)
                if match:
                    literal = match.group(2).strip()
                    add(definitions, match.group(1), int(re.sub(r"[uUlL]+$", "", literal)) if LITERAL.fullmatch(literal) else None)
                elif field.strip():
                    add(definitions, field.strip().split()[0], None)
    return {name: values[0] for name, values in definitions.items()
            if len(values) == 1 and values[0] is not None}


def braced_body(text, start):
    """Return the next balanced braced initializer body, or None."""
    begin = text.find("{", start)
    if begin < 0:
        return None
    depth = 0
    for pos in range(begin, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[begin + 1:pos]
    return None


def array_body(text, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*\[[^]]*\]\s*=", text)
    return braced_body(text, match.end()) if match else None


def scalar_fields(body):
    if body is None:
        return {}
    return {match.group(1): int(match.group(2)) for match in SCALAR_FIELD.finditer(body)
            if match.group(1) in TABLE_FIELDS}


def add_fields(values, entry, fields):
    for field, value in fields.items():
        key = (entry, field)
        if key not in values:
            values[key] = value
        elif values[key] != value:
            values[key] = None


def table_values(root):
    """Return literal material and reaction table fields keyed by entry and field."""
    path = pathlib.Path(root) / "launcher/main/apps/sand/material.c"
    if not path.exists():
        return {}
    text = uncomment(path.read_text(encoding="utf-8", errors="replace"))
    material_entries = {}
    for match in MATERIAL_ENTRY.finditer(text):
        body = braced_body(text, match.end())
        name = re.search(r"\.name\s*=\s*\"([^\"]+)\"", body or "")
        if name:
            material_entries[match.group(1)] = (name.group(1), body)
    material_names = {key: name for key, (name, _) in material_entries.items()}

    extended_names = {key: name for key, name in EXTENDED_NAME.findall(text)}
    values = {}
    for entry, (name, body) in material_entries.items():
        add_fields(values, name, scalar_fields(body))

    for array, names in (("reactions", material_names), ("extended_reactions", extended_names)):
        body = array_body(text, array)
        if body is None:
            continue
        for match in ARRAY_ENTRY.finditer(body):
            entry = names.get(match.group(1))
            if entry is None:
                continue
            initializer = braced_body(body, match.end())
            if initializer is not None:
                add_fields(values, entry, scalar_fields(initializer))
                continue
            macro = re.match(r"([A-Za-z_]\w*)\b", body[match.end():])
            if not macro:
                continue
            definition = next((item for item in MACRO.finditer(text) if item.group(1) == macro.group(1)), None)
            if definition:
                add_fields(values, entry, scalar_fields(braced_body(text, definition.end())))
    return {key: value for key, value in values.items()
            if value is not None and key[0].lower() not in {"empty", "extended"}}


def allowlist(root):
    path = pathlib.Path(root) / "scripts/doc_constant_allowlist.txt"
    allowed = set()
    if not path.exists():
        return allowed
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 4 or not all(fields):
            raise ValueError(f"{path}:{number}: expected doc, name, value, reason")
        if not fields[2].isdigit():
            raise ValueError(f"{path}:{number}: value must be decimal")
        allowed.add((fields[0], fields[1], int(fields[2])))
    return allowed


def adjacent(name, number, text):
    before = text[:number.start()]
    return (re.search(r"\b" + re.escape(name.group()) + r"\b\s*(?:`)?\s*(?:\(|=|,|,?\s*(?:is|are|currently|at|of))[^\d]{0,24}$", before) is not None or
            re.match(r"\s*\(\s*" + re.escape(name.group()) + r"\b", text[number.end():]) is not None or
            re.search(r"\b" + re.escape(name.group()) + r"\b[^|\n]*\|\s*$", before) is not None)


def hypothetical(name, number, text):
    """Whether a nearby value describes a counterfactual setting."""
    reference = r"(?:`?" + re.escape(name.group()) + r"`?\s*(?:=|is|of)?\s*)?"
    before = text[:number.start()][-120:]
    patterns = (
        r"\b(?:at|with)\s+" + reference + r"$",
        r"\bsetting\s+(?:it|`?" + re.escape(name.group()) + r"`?)\s+to\s*$",
        r"\bat\s+a\s+value\s+of\s*$",
        r"\bif\s+(?:it|`?" + re.escape(name.group()) + r"`?)\s+were\s*$",
        r"\b(?:any\s+lower\s+than|below|above)\s*$",
    )
    return any(re.search(pattern, before, re.I) for pattern in patterns)


def unit_for_name(name):
    suffix = name.rsplit("_", 1)[-1].upper()
    return UNIT_SUFFIXES.get(suffix)


def documented_unit(number, text):
    match = UNIT.match(text[number.end():])
    return UNIT_NAMES.get(match.group(1).lower()) if match else None


def field_numbers(entry, field, entries, text):
    """Yield an entry-owned field's first nearby documented number."""
    names = FIELD_ALIASES.get(field, (field,))
    for match in re.finditer(r"\b(?:" + "|".join(map(re.escape, names)) + r")\b", text, re.I):
        if re.match(r"\s*!=", text[match.end():]):
            continue
        owners = [(item.start(), item.group()) for candidate in entries
                  for item in re.finditer(r"\b" + re.escape(candidate) + r"\b", text[:match.start()], re.I)]
        if not owners:
            continue
        owner = max(owners)[1]
        possessive = re.search(r"\b" + re.escape(entry) + r"(?:'s|:|\s*\|)", text[:match.start()], re.I)
        if owner.lower() != entry.lower() and not possessive:
            continue
        number = DECIMAL.search(text, match.end())
        if number and number.start() - match.end() <= 24:
            yield number


def any_field_numbers(field, text):
    names = FIELD_ALIASES.get(field, (field,))
    for match in re.finditer(r"\b(?:" + "|".join(map(re.escape, names)) + r")\b", text, re.I):
        if re.match(r"\s*!=", text[match.end():]):
            continue
        number = DECIMAL.search(text, match.end())
        gap = text[match.end():number.start()] if number else ""
        other_fields = r"\b(?:" + "|".join(map(re.escape, TABLE_FIELDS)) + r"|heats_to)\b"
        if number and number.start() - match.end() <= 24 and not re.search(other_fields, gap, re.I):
            yield number


def table_rows(text):
    """Yield Markdown table data rows with their header cells."""
    lines = text.splitlines()
    number = 0
    while number + 1 < len(lines):
        header, divider = lines[number].strip(), lines[number + 1].strip()
        if not (header.startswith("|") and divider.startswith("|") and
                re.fullmatch(r"[| :\-]+", divider)):
            number += 1
            continue
        headings = [cell.strip() for cell in header.strip("|").split("|")]
        number += 2
        while number < len(lines) and lines[number].lstrip().startswith("|"):
            cells = [cell.strip() for cell in lines[number].strip().strip("|").split("|")]
            yield number + 1, headings, cells
            number += 1


def table_claims(doc, text, values, allowed, mismatches, skipped):
    entries = sorted({entry for entry, _ in values}, key=len, reverse=True)
    for line, headings, cells in table_rows(text):
        row = " | ".join(cells)
        for (entry, field), defined in values.items():
            if not any(re.fullmatch(r"[ `*_]*" + re.escape(entry) + r"[ `*_]*", cell, re.I) for cell in cells):
                continue
            columns = [number for number, heading in enumerate(headings)
                       if re.search(r"\b" + re.escape(field) + r"\b", heading, re.I)]
            if not columns:
                numbers = list(field_numbers(entry, field, entries, row))
                if not numbers:
                    continue
                columns = [None]
            for column in columns:
                if column is None:
                    number = numbers[0]
                    claimed = int(number.group())
                    name = f"{entry}.{field}"
                    if (doc, name, claimed) in allowed:
                        skipped.append((doc, line, "allowlisted"))
                    elif claimed != defined:
                        mismatches.append(Mismatch(doc, line, name, claimed, defined))
                    continue
                if column >= len(cells):
                    skipped.append((doc, line, "table row lacks an integer field value"))
                    continue
                number = DECIMAL.search(cells[column])
                if not number:
                    skipped.append((doc, line, "table row lacks an integer field value"))
                    continue
                claimed = int(number.group())
                name = f"{entry}.{field}"
                if (doc, name, claimed) in allowed:
                    skipped.append((doc, line, "allowlisted"))
                elif claimed != defined:
                    mismatches.append(Mismatch(doc, line, name, claimed, defined))


def masked_tables(text):
    """Keep line numbers while preventing table cells from becoming prose claims."""
    return "\n".join("." if line.lstrip().startswith("|") else line for line in text.splitlines())


def mermaid_lines(text):
    fenced = False
    for number, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith("```"):
            fenced = line.lstrip().startswith("```mermaid")
            continue
        if fenced:
            yield number, line


def sentences(path, text=None):
    """Yield non-code Markdown sentences with their first source line."""
    fenced = False
    pending, first = "", 1
    text = text if text is not None else path.read_text(encoding="utf-8", errors="replace")
    for number, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        if ESCAPE in line:
            line = ESCAPE + " " + line
        if not pending:
            first = number
        pending += (" " if pending else "") + line
        while True:
            end = re.search(r"[.!?](?:\s|$)", pending)
            if not end:
                break
            yield first, pending[:end.end()]
            pending = pending[end.end():].lstrip()
            first = number
    if pending:
        yield first, pending


def documents(root, ref=None):
    if ref is None:
        for path in documentation(root):
            yield path.relative_to(root).as_posix(), path, None
        return
    result = subprocess.run(["git", "ls-tree", "-r", "--name-only", ref],
                            cwd=root, check=True, capture_output=True, text=True, encoding="utf-8")
    for name in result.stdout.splitlines():
        if name.endswith(".md") and (name.startswith("docs/") or "/" not in name):
            text = subprocess.run(["git", "show", f"{ref}:{name}"], cwd=root, check=True,
                                  capture_output=True, text=True, encoding="utf-8").stdout
            yield name, pathlib.Path(name), text


def check(root, verbose=False, docs_ref=None):
    root = pathlib.Path(root)
    values = constants(root)
    material_values = table_values(root)
    allowed = allowlist(root)
    mismatches, skipped = [], []
    names = re.compile(r"\b(" + "|".join(map(re.escape, sorted(values, key=len, reverse=True))) + r")\b") if values else None
    for doc, path, text in documents(root, docs_ref):
        for line, sentence in sentences(path, text):
            if not names:
                continue
            mentioned = list(names.finditer(sentence))
            if not mentioned:
                continue
            if ESCAPE in sentence:
                skipped.append((doc, line, "inline escape"))
                continue
            if HISTORICAL.search(sentence):
                skipped.append((doc, line, "historical wording"))
                continue
            numbers = list(DECIMAL.finditer(sentence))
            if HEX.search(sentence):
                skipped.append((doc, line, "different integer base"))
                continue
            if not numbers:
                continue
            for name in mentioned:
                close = [number for number in numbers if adjacent(name, number, sentence)]
                candidates = close
                if not candidates:
                    skipped.append((doc, line, "multiple numbers without an adjacent value"))
                    continue
                for number in candidates:
                    if hypothetical(name, number, sentence):
                        skipped.append((doc, line, "skipped-on-hypothetical"))
                        continue
                    expected_unit = unit_for_name(name.group())
                    unit = documented_unit(number, sentence)
                    if expected_unit and unit and expected_unit != unit:
                        skipped.append((doc, line, "skipped-on-unit"))
                        continue
                    claimed = int(number.group())
                    if (doc, name.group(), claimed) in allowed:
                        skipped.append((doc, line, "allowlisted"))
                    elif claimed != values[name.group()]:
                        mismatches.append(Mismatch(doc, line, name.group(), claimed, values[name.group()]))
        table_claims(doc, text or path.read_text(encoding="utf-8", errors="replace"), material_values,
                     allowed, mismatches, skipped)
        for line, sentence in sentences(path, masked_tables(text or path.read_text(encoding="utf-8", errors="replace"))):
            if ESCAPE in sentence:
                continue
            if HISTORICAL.search(sentence):
                skipped.append((doc, line, "historical wording"))
                continue
            entries = sorted({entry for entry, _ in material_values}, key=len, reverse=True)
            for (entry, field), defined in material_values.items():
                if not (re.search(r"\b" + re.escape(entry) + r"\b", sentence, re.I) and
                        re.search(r"\b" + re.escape(field) + r"\b", sentence, re.I)):
                    continue
                numbers = list(field_numbers(entry, field, entries, sentence))
                if not numbers:
                    skipped.append((doc, line, "entry and field lack an adjacent value"))
                    continue
                for number in numbers:
                    claimed = int(number.group())
                    name = f"{entry}.{field}"
                    if (doc, name, claimed) in allowed:
                        skipped.append((doc, line, "allowlisted"))
                    elif claimed != defined:
                        mismatches.append(Mismatch(doc, line, name, claimed, defined))
            for field in IMPLIED_UNIQUE_FIELDS:
                owners = [(entry, defined) for (entry, candidate), defined in material_values.items()
                          if candidate == field]
                if len(owners) != 1 or re.search(r"\b" + re.escape(owners[0][0]) + r"\b", sentence, re.I):
                    continue
                numbers = list(any_field_numbers(field, sentence))
                if not numbers:
                    continue
                entry, defined = owners[0]
                claimed = int(numbers[0].group())
                name = f"{entry}.{field}"
                if (doc, name, claimed) in allowed:
                    skipped.append((doc, line, "allowlisted"))
                elif claimed != defined:
                    mismatches.append(Mismatch(doc, line, name, claimed, defined))
        for line, source in mermaid_lines(text or path.read_text(encoding="utf-8", errors="replace")):
            for (entry, field), defined in material_values.items():
                if not (re.search(r"\b" + re.escape(entry) + r"\b", source, re.I) and
                        any(re.search(r"\b" + re.escape(alias) + r"\b", source, re.I)
                            for alias in FIELD_ALIASES.get(field, (field,)))):
                    continue
                numbers = list(any_field_numbers(field, source))
                if not numbers:
                    continue
                claimed = int(numbers[0].group())
                name = f"{entry}.{field}"
                if (doc, name, claimed) in allowed:
                    skipped.append((doc, line, "allowlisted"))
                elif claimed != defined:
                    mismatches.append(Mismatch(doc, line, name, claimed, defined))
    return (mismatches, skipped) if verbose else mismatches


def main(argv):
    root = pathlib.Path(".")
    verbose = False
    docs_ref = None
    while argv:
        if argv[:1] == ["--root"] and len(argv) > 1:
            root, argv = pathlib.Path(argv[1]), argv[2:]
        elif argv[:1] == ["--docs-ref"] and len(argv) > 1:
            docs_ref, argv = argv[1], argv[2:]
        elif argv[:1] == ["--verbose"]:
            verbose, argv = True, argv[1:]
        else:
            print("usage: check_doc_constants.py [--root ROOT] [--docs-ref REF] [--verbose]", file=sys.stderr)
            return 2
    if not root.exists():
        print(f"{root}: no such root", file=sys.stderr)
        return 2
    try:
        result = check(root, verbose, docs_ref)
    except (subprocess.CalledProcessError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2
    mismatches, skipped = result if verbose else (result, [])
    for item in mismatches:
        print(f"{item.doc}:{item.line}: {item.name} documents {item.claimed}, code defines {item.defined}")
    if verbose:
        for doc, line, reason in skipped:
            print(f"{doc}:{line}: skipped: {reason}")
        for reason in sorted(set(item[2] for item in skipped)):
            print(f"{sum(item[2] == reason for item in skipped)} skipped: {reason}")
    print(f"{len(mismatches)} documentation constant mismatch"
          f"{'' if len(mismatches) == 1 else 'es'}")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
