#!/usr/bin/env python3
"""Require declared simulation facts to be read through SAND_SKIP_IF or SAND_FACT_RULE.

Markers on declarations define the vocabulary. Predicate and writer bodies
may inspect facts; assignment targets may maintain them. Comments, literals,
preprocessor definitions and declarations are not executable reads.
"""
import argparse
import pathlib
import re

from c_comments import balanced_end, blank_comments

ROOT = pathlib.Path(__file__).resolve().parents[2] / "launcher/main/apps/sand"
IDENTIFIER = re.compile(r"\b[A-Za-z_]\w*\b")
MARKER = re.compile(r"\bSAND_FACT(?:_WRITER)?\b(?!\s*$)")


def declarations(code):
    """Names and exempt declaration/body spans from marker tokens."""
    facts, spans = set(), []
    for mark in MARKER.finditer(code):
        end = re.search(r"[;{]", code[mark.end():])
        if end is None:
            continue
        end = mark.end() + end.start()
        head = code[mark.end():end]
        calls = re.findall(r"\b([A-Za-z_]\w*)\s*\(", head)
        if code[end] == "{" and calls:
            name = calls[-1]
            if mark.group() == "SAND_FACT":
                facts.add(name)
            spans.append((mark.start(), balanced_end(code, end)))
        elif mark.group() == "SAND_FACT":
            name = re.search(r"\b([A-Za-z_]\w*)\s*(?:\[[^;]*\])?\s*(?:=|$)", head)
            if name:
                facts.add(name[1])
                equal = code.find("=", mark.end(), end)
                spans.append((mark.start(), equal if equal >= 0 else end + 1))
    return facts, spans


def skip_sites(root=ROOT):
    """Source locations, including sites the fingerprint never reaches."""
    sites = []
    for path, raw, code in sources(root):
        for site in re.finditer(r"\bSAND_SKIP_IF\s*\(", code):
            sites.append((path.as_posix(), raw.count("\n", 0, site.start()) + 1))
    return sorted(sites)


def sources(root):
    for path in sorted(pathlib.Path(root).rglob("*")):
        if path.suffix not in {".c", ".h"} or {"tests", "tools"} & set(path.relative_to(root).parts):
            continue
        raw = path.read_text(encoding="utf-8")
        code = blank_comments(raw, mode="code")
        directives = list(re.finditer(r"^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", code, re.M))
        for directive in reversed(directives):
            declaration = re.match(r"[ \t]*#\s*define\s+(\w+)(?:\([^\n)]*\))?", directive[0])
            keep_body = declaration and declaration[1] not in {"SAND_SKIP_IF", "SAND_FACT_RULE", "SAND_FORCED_IF"}
            end = directive.start() + declaration.end() if keep_body else directive.end()
            code = code[:directive.start()] + "".join("\n" if c == "\n" else " "
                                                    for c in code[directive.start():end]) + code[end:]
        yield path, raw, code


def is_write(code, start, end):
    tail = end
    while True:
        bracket = re.match(r"\s*\[", code[tail:])
        if not bracket:
            break
        tail = balanced_end(code, tail + bracket.end() - 1, "[", "]")
    if re.match(r"\s*(?:=(?!=)|\|=|&=|\^=|\+=|-=|\*=|/=|%=|<<=|>>=|\+\+|--)", code[tail:]):
        return True
    return bool(re.search(r"(?:\+\+|--)\s*(?:\w+\s*(?:->|\.)\s*)?$", code[:start]))


def problems(root=ROOT):
    files = list(sources(root))
    facts, exemptions = set(), {}
    found = []
    for path, raw, code in files:
        names, spans = declarations(code)
        facts.update(names)
        exemptions[path] = spans
        defines = blank_comments(raw, mode="code")
        for define in re.finditer(r"^[ \t]*#define\s+(BLOCK_\w+)\s+([^\n]+)", defines, re.M):
            value = define[2].strip()
            if not re.fullmatch(r"(?:0x[\da-fA-F]+|\d+)[uU]?", value) and "<<" not in value:
                continue
            if define[1].endswith(("_W", "_H")):
                continue
            end = raw.find("\n", define.start())
            if "SAND_FACT" in raw[define.start():end if end >= 0 else len(raw)]:
                facts.add(define[1])
            else:
                found.append((path, raw.count("\n", 0, define.start()) + 1,
                              define[1] + " missing SAND_FACT"))
        if path.name == "sand.h":
            for field in re.finditer(r"\bmay_have_\w+\s*;", code):
                if not any(a <= field.start() < b for a, b in spans):
                    found.append((path, raw.count("\n", 0, field.start()) + 1,
                                  field[0][:-1].strip() + " missing SAND_FACT"))

    for path, raw, code in files:
        spans = list(exemptions[path])
        for site in re.finditer(r"\b(?:SAND_SKIP_IF|SAND_FACT_RULE)\s*\(", code):
            spans.append((site.start(), balanced_end(code, site.end() - 1, "(", ")")))
        for token in IDENTIFIER.finditer(code):
            if token[0] not in facts or any(a <= token.start() < b for a, b in spans):
                continue
            if is_write(code, token.start(), token.end()):
                continue
            # Unmarked prototypes and local aliases are declarations, not reads.
            before = code[max(code.rfind(";", 0, token.start()), code.rfind("{", 0, token.start()),
                              code.rfind("}", 0, token.start())) + 1:token.start()]
            if re.fullmatch(r"\s*(?:(?:static|extern|const|inline|bool|uint\d+_t|int)\s+)+", before):
                continue
            found.append((path, raw.count("\n", 0, token.start()) + 1,
                          token[0] + " read outside SAND_SKIP_IF or SAND_FACT_RULE"))
    return [f"{p.as_posix()}:{line}: {message}" for p, line, message in sorted(set(found))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", type=pathlib.Path, default=ROOT)
    parser.add_argument("--sites", action="store_true")
    parser.add_argument("--coverage", type=pathlib.Path)
    args = parser.parse_args()
    if args.coverage:
        coverage(args.root, args.coverage)
        return 0
    if args.sites:
        for path, line in skip_sites(args.root):
            print(f"{path}:{line}")
        return 0
    found = problems(args.root)
    print("\n".join(found) if found else "skip facts: OK")
    return bool(found)


def coverage(root, capture):
    counts = {}
    for row in capture.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"SAND_SKIP (.*):(\d+) (\d+)", row)
        if match:
            key = (match[1].replace("\\", "/").split("/sand/", 1)[-1], int(match[2]))
            counts[key] = counts.get(key, 0) + int(match[3])
    print("skip coverage: file:line would-skip")
    for path, line in skip_sites(root):
        name = pathlib.Path(path).relative_to(root).as_posix()
        count = counts.get((name, line), 0)
        print(f"{name}:{line} {count}" + (" WARNING: untested skip" if count == 0 else ""))


if __name__ == "__main__":
    raise SystemExit(main())
