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
UNIT = re.compile(r"\s*(?:us|ms|s|px|cell(?:s)?|byte(?:s)?|KiB|MiB)\b", re.I)
ESCAPE = "<!-- doc-constants: ignore -->"


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
    return (re.search(r"\b" + re.escape(name.group()) + r"\b\s*(?:`)?\s*(?:\(|=|,?\s*(?:is|are|currently|at|of))[^\d]{0,24}$", before) is not None or
            re.match(r"\s*\(\s*" + re.escape(name.group()) + r"\b", text[number.end():]) is not None or
            re.search(r"\b" + re.escape(name.group()) + r"\b[^|\n]*\|\s*$", before) is not None)


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
                close = [number for number in numbers
                         if adjacent(name, number, sentence) and not UNIT.match(sentence[number.end():])]
                candidates = close
                if not candidates:
                    skipped.append((doc, line, "multiple numbers without an adjacent value"))
                    continue
                for number in candidates:
                    claimed = int(number.group())
                    if (doc, name.group(), claimed) in allowed:
                        skipped.append((doc, line, "allowlisted"))
                    elif claimed != values[name.group()]:
                        mismatches.append(Mismatch(doc, line, name.group(), claimed, values[name.group()]))
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
    print(f"{len(mismatches)} documentation constant mismatch"
          f"{'' if len(mismatches) == 1 else 'es'}")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
