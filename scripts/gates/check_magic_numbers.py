#!/usr/bin/env python3
"""Ratchet visible integer restatements and shared firmware/Python protocol tokens."""
import argparse
import ast
from collections import Counter, defaultdict
from dataclasses import dataclass
import io
from pathlib import Path, PurePosixPath
import re
import subprocess
import time
import tokenize

from c_comments import EXCLUDED, balanced_end, blank_comments, tokens
from c_constants import ConstantIndex, DEFINE, ENUM, INTEGER, SCALAR
from check_clones import C_TOKENS, ROOT, changed_paths, comparison_base, git
from check_generated_files import is_generated

RARE_MINIMUM = 300
ROUND_MULTIPLE = 100
POWER_MINIMUM = 4096
DATA_ROW_MINIMUM = 6
TOKEN_MINIMUM = 5
SHOW_BATCH_SIZE = 64
RESTATE = "RESTATE"
PROTOCOL = "PROTOCOL"
C_SUFFIXES = {".c", ".h", ".cpp", ".hpp"}
INCLUDE_ROOTS = ("launcher/main", "launcher/main/apps", "launcher/test", "editor/include", "editor/src")
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include\s*"([^"\n]+)"', re.M)
ASSERT = re.compile(r"\b(?:_Static_assert|static_assert)\s*\(")
BRACKET = re.compile(r"\[[^\[\]]*\]", re.S)
SHIFT_RIGHT = re.compile(r"(?:<<|>>)=?\s*$")
SHIFT_EXPRESSION = re.compile(r"(?:0[xX][0-9a-fA-F]+|\d+)[uUlL]*\s*(?:<<|>>)\s*$")
TOKEN = re.compile(r"[A-Za-z_][A-Za-z_0-9.]*$")
C_ESCAPE = re.compile(r"/\*\s*magic:\s*(.*?)\*/")
PY_ESCAPE = re.compile(r"#\s*magic:\s*(.*)")


@dataclass(frozen=True)
class Hit:
    path: str
    line: int
    message: str

    def __str__(self):
        return f"{self.path}:{self.line}: {self.message}"


def eligible_c(path, text):
    return (path.startswith(("launcher/", "editor/")) and Path(path).suffix in C_SUFFIXES
            and not path.startswith(EXCLUDED) and not is_generated(text)
            and not {"fixture", "fixtures"}.intersection(PurePosixPath(path).parts))


def revision_tree(root, revision):
    listing = git(root, "ls-tree", "-r", "-l", revision, "--",
                  "launcher", "editor", "scripts").decode().splitlines()
    entries = []
    for entry in listing:
        metadata, name = entry.split("\t", 1)
        if Path(name).suffix in C_SUFFIXES | {".py"}:
            entries.append((name, int(metadata.split()[-1])))
    texts = {}
    for start in range(0, len(entries), SHOW_BATCH_SIZE):
        batch = entries[start:start + SHOW_BATCH_SIZE]
        content = git(root, "show", "--no-ext-diff", "--no-textconv",
                      *(f"{revision}:{name}" for name, _ in batch))
        offset = 0
        for name, size in batch:
            texts[name] = content[offset:offset + size].decode("utf-8", errors="replace")
            offset += size
        if offset != len(content):
            raise ValueError("incomplete revision content")
    return texts


def include_closure(path, texts):
    closure = set()
    pending = [path]
    while pending:
        name = pending.pop()
        if name in closure:
            continue
        closure.add(name)
        for include in INCLUDE.findall(blank_comments(texts[name])):
            directories = (str(PurePosixPath(name).parent), *INCLUDE_ROOTS)
            for directory in directories:
                candidate = str(PurePosixPath(directory) / include)
                # Includes may traverse a parent directory within the tracked tree.
                parts = []
                for part in PurePosixPath(candidate).parts:
                    if part == ".." and parts:
                        parts.pop()
                    elif part != ".":
                        parts.append(part)
                candidate = "/".join(parts)
                if candidate in texts:
                    pending.append(candidate)
                    break
    return closure


def escapes(path, text, python=False):
    found, errors, lines = [], [], set()
    if python:
        comments = [(token.start[0], token.string) for token in
                    tokenize.generate_tokens(io.StringIO(text).readline) if token.type == tokenize.COMMENT]
        pattern = PY_ESCAPE
    else:
        comments = [(text.count("\n", 0, start) + 1, text[start:end])
                    for kind, start, end in tokens(text) if kind == "block"]
        pattern = C_ESCAPE
    for line, comment in comments:
        match = pattern.fullmatch(comment)
        if match:
            reason = match[1].strip()
            if reason:
                found.append(Hit(path, line, f"magic: {reason}"))
                lines.add(line)
            else:
                errors.append(Hit(path, line, "empty magic escape reason"))
    return lines, found, errors


def executable_numbers(text):
    code = blank_comments(text, "code")
    spans = [(match.start(), match.end()) for pattern in (DEFINE, ENUM, SCALAR, BRACKET)
             for match in pattern.finditer(code)]
    for match in ASSERT.finditer(code):
        spans.append((match.start(), balanced_end(code, match.end() - 1, "(", ")")))
    masked = list(code)
    for start, end in spans:
        masked[start:end] = ["\n" if char == "\n" else " " for char in code[start:end]]
    offset = 0
    for line, source in enumerate("".join(masked).splitlines(keepends=True), 1):
        numbers = list(INTEGER.finditer(source))
        numeric_count = sum(match.group("identifier") is None for match in C_TOKENS.finditer(source))
        if numeric_count < DATA_ROW_MINIMUM:
            for match in numbers:
                prefix = code[:offset + match.start()]
                if SHIFT_EXPRESSION.search(prefix) or re.match(r"\s*(?:<<|>>)", source[match.end():]):
                    continue
                yield line, match[0], bool(SHIFT_RIGHT.search(prefix))
        offset += len(source)


def restatements(path, text, visible, skipped):
    by_value = defaultdict(list)
    for record, value in visible:
        by_value[value].append(record)
    for line, literal, shift in executable_numbers(text):
        if line in skipped:
            continue
        spelling = re.sub(r"[uUlL]+$", "", literal)
        value = int(spelling, 16 if spelling.lower().startswith("0x") else 10)
        rare = (value >= RARE_MINIMUM and value % ROUND_MULTIPLE != 0
                or value >= POWER_MINIMUM and value & (value - 1) == 0)
        owners = [record for record in by_value[value]
                  if rare or shift and record.name.endswith("_SHIFT")]
        if owners:
            names = ", ".join(f"{record.name} ({record.path}:{record.line})" for record in owners)
            yield Hit(path, line, f"{literal} restates {names}")


def protocol_token(value):
    return (len(value) >= TOKEN_MINIMUM and TOKEN.fullmatch(value)
            and ("_" in value or "." in value or value.isupper()))


def python_strings(text):
    tree = ast.parse(text)
    docstrings = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            if (node.body and isinstance(node.body[0], ast.Expr)
                    and isinstance(node.body[0].value, ast.Constant)
                    and isinstance(node.body[0].value.value, str)):
                docstrings.add(id(node.body[0].value))
    for node in ast.walk(tree):
        if (isinstance(node, ast.Constant) and isinstance(node.value, str)
                and id(node) not in docstrings and protocol_token(node.value)):
            yield node.lineno, node.value


def scan(root, revision, candidates=None):
    texts = revision_tree(root, revision)
    index = ConstantIndex({name: text for name, text in texts.items() if Path(name).suffix in C_SUFFIXES})
    hits, logged, errors = {}, [], []
    owners = {}
    for path, text in sorted(texts.items()):
        restate_source = eligible_c(path, text)
        protocol_source = path.startswith("launcher/main/") and Path(path).suffix in {".c", ".h"}
        if not restate_source and not protocol_source:
            continue
        skipped, found, invalid = escapes(path, text)
        logged.extend(found)
        errors.extend(invalid)
        if restate_source:
            closure = include_closure(path, texts)
            if candidates is None or closure.intersection(candidates):
                group = list(restatements(path, text, index.visible(closure), skipped))
                if group:
                    hits[path, RESTATE] = group
        if protocol_source:
            clean = blank_comments(text)
            for kind, start, end in tokens(clean):
                if kind == "literal" and clean[start] == '"':
                    try:
                        value = ast.literal_eval(clean[start:end])
                    except (SyntaxError, ValueError):
                        continue
                    if protocol_token(value):
                        owners.setdefault(value, (path, clean.count("\n", 0, start) + 1))
    for path, text in sorted(texts.items()):
        if not path.startswith(("scripts/", "launcher/tools/")) or Path(path).suffix != ".py":
            continue
        skipped, found, invalid = escapes(path, text, python=True)
        logged.extend(found)
        errors.extend(invalid)
        group = [Hit(path, line, f'{value!r} repeats protocol token ({owners[value][0]}:{owners[value][1]})')
                 for line, value in python_strings(text) if line not in skipped and value in owners]
        if group:
            hits[path, PROTOCOL] = group
    return hits, logged, errors


def module(path):
    parts = PurePosixPath(path).parts
    if parts[:2] == ("launcher", "main"):
        return "/".join(parts[2:4]) if parts[2] == "apps" else parts[2]
    return "/".join(parts[:2]) if parts[0] == "launcher" else parts[0]


def report(hits):
    totals = defaultdict(Counter)
    for (path, rule), group in sorted(hits.items()):
        print(f"{path}: {rule}={len(group)}")
        for hit in group:
            print(hit)
        totals[module(path)][rule] += len(group)
    for name, counts in sorted(totals.items()):
        print(f"{name}: RESTATE={counts[RESTATE]} PROTOCOL={counts[PROTOCOL]}")


def main(argv=None, root=ROOT):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--report", action="store_true")
    args = parser.parse_args(argv)
    started = time.perf_counter()
    try:
        if args.report:
            hits, logged, errors = scan(root, "HEAD")
            report(hits)
            grown = []
            old = {}
        else:
            base = comparison_base(root, against=args.base)
            changed, renames, _ = changed_paths(root, base, "HEAD")
            hits, logged, errors = scan(root, "HEAD", changed)
            old, _, _ = scan(root, base)
            grown = []
            for (path, rule), group in sorted(hits.items()):
                previous = old.get((renames.get(path, path), rule), [])
                if len(group) <= len(previous):
                    continue
                grown.append((path, rule))
                print(f"{path}: {rule} {len(previous)} -> {len(group)}")
                remaining = Counter(hit.message for hit in previous)
                for hit in group:
                    if remaining[hit.message]:
                        remaining[hit.message] -= 1
                    else:
                        print(hit)
        for hit in logged + errors:
            print(hit)
        failed = bool(grown or errors)
        counts = Counter({rule: sum(len(group) for (path, candidate), group in hits.items() if candidate == rule)
                          for rule in (RESTATE, PROTOCOL)})
        print(f"{'FAIL' if failed else 'PASS'}: {len(grown)} growing file/rule counts; "
              f"RESTATE={counts[RESTATE]} PROTOCOL={counts[PROTOCOL]}; "
              f"{time.perf_counter() - started:.2f}s.")
        return int(failed)
    except (OSError, ValueError, SyntaxError, tokenize.TokenError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
