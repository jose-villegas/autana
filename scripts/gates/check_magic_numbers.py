#!/usr/bin/env python3
"""Reject growing per-file counts of restated constants and protocol strings.

RESTATE: a plain integer literal in launcher/ or editor/ C/C++ equal to a
constant reachable through the file's #include "..." chain, when the value
is rare (RARE_MINIMUM or more and not a multiple of ROUND_MULTIPLE) or is a
shift count equal to a visible *_SHIFT. Shift expressions such as 1 << 16,
#define/enum/file-scope const initializers, declaration array bounds,
static asserts and data rows of DATA_ROW_MINIMUM numbers or more are not
counted. Fixtures, generated files and c_comments.EXCLUDED paths are skipped.

PROTOCOL: a string literal in launcher/main C that is identifier-shaped
(dots allowed), at least TOKEN_MINIMUM characters, and contains `_` or `.`
or is all upper case, which a Python file under scripts/ or launcher/tools/
repeats; counted on the Python file. Include paths and Python docstrings
are excluded.

HEAD is compared per file and rule with the merge-base of --base, as in
check_clones.py. Use the named constant, or mark a deliberate literal on
its line with /* magic: reason */ or # magic: reason; an empty reason
fails. --report lists every hit and per-module totals.
"""
import argparse
import ast
from collections import Counter, defaultdict
from dataclasses import dataclass
import io
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import time
import tokenize

from c_comments import EXCLUDED, balanced_end, blank_comments, tokens
from c_constants import ConstantIndex, DEFINE, ENUM, INTEGER, SCALAR, file_scope
from check_clones import C_TOKENS, INCLUDE_DIRECTIVE, ROOT, changed_paths, comparison_base, git
from tracked import revision_contents
from c_includes import resolve_include
from idf_vocabulary import FLOW, WORD
from check_generated_files import is_generated
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher/tools"))
from build.packages import FIRST_PARTY  # noqa: E402

RARE_MINIMUM = 300
ROUND_MULTIPLE = 100
DATA_ROW_MINIMUM = 6
TOKEN_MINIMUM = 5
RESTATE = "RESTATE"
PROTOCOL = "PROTOCOL"
C_SUFFIXES = {".c", ".h", ".cpp", ".hpp"}

# launcher/main/CMakeLists.txt exposes main/ and test/, each launcher/packages/<name>/ its include/
# (package_roots); editor/CMakeLists.txt exposes include/ and src/.
INCLUDE_ROOTS = ("launcher/main", "launcher/test", "editor/include", "editor/src")
PACKAGE_INCLUDE = re.compile(r"^(launcher/packages/[^/]+/include)/")
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include\s*"([^"\n]+)"', re.M)
ASSERT = re.compile(r"\b(?:_Static_assert|static_assert)\s*\(")
ARRAY_BOUND = re.compile(
    r"(?:^|[;{}\n])\s*(?P<prefix>(?:[A-Za-z_]\w*[\s*]+)+)"
    r"[A-Za-z_]\w*\s*(?P<bounds>(?:\[[^\[\]]*\]\s*)+)"
)
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
    identity: tuple = ()

    def __str__(self):
        return f"{self.path}:{self.line}: {self.message}"


def eligible_c(path, text):
    return (path.startswith(("launcher/", "editor/")) and Path(path).suffix in C_SUFFIXES
            and not path.startswith(EXCLUDED) and not is_generated(text)
            and not {"fixture", "fixtures"}.intersection(PurePosixPath(path).parts))


def protocol_source(path, text):
    return (path.startswith(FIRST_PARTY) and Path(path).suffix in {".c", ".h"}
            and eligible_c(path, text))


def revision_tree(root, revision):
    names = git(root, "ls-tree", "-r", "--name-only", revision, "--",
                "launcher", "editor", "scripts").decode().splitlines()
    return {name: content.decode("utf-8", errors="replace")
            for name, content in revision_contents(root, revision,
                [name for name in names if Path(name).suffix in C_SUFFIXES | {".py"}])}


def include_closure(path, direct):
    closure = set()
    pending = [path]
    while pending:
        name = pending.pop()
        if name not in closure:
            closure.add(name)
            pending.extend(direct.get(name, ()))
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
    spans = [(match.start(), match.end()) for pattern in (DEFINE, ENUM)
             for match in pattern.finditer(code)]
    spans.extend((match.start(), match.end()) for match in SCALAR.finditer(file_scope(code)))
    spans.extend(match.span("bounds") for match in ARRAY_BOUND.finditer(code)
                 if FLOW.isdisjoint(WORD.findall(match["prefix"])))
    for match in ASSERT.finditer(code):
        spans.append((match.start(), balanced_end(code, match.end() - 1, "(", ")")))
    masked = list(code)
    for start, end in spans:
        masked[start:end] = ["\n" if char == "\n" else " " for char in code[start:end]]
    for line, source in enumerate("".join(masked).splitlines(keepends=True), 1):
        numbers = list(INTEGER.finditer(source))
        numeric_count = sum(match.group("identifier") is None for match in C_TOKENS.finditer(source))
        if numeric_count < DATA_ROW_MINIMUM:
            for match in numbers:
                prefix = source[:match.start()]
                if SHIFT_EXPRESSION.search(prefix) or re.match(r"\s*(?:<<|>>)", source[match.end():]):
                    continue
                yield line, match[0], bool(SHIFT_RIGHT.search(prefix))


def restatements(path, text, visible, skipped):
    by_value = defaultdict(list)
    for record, value in visible:
        by_value[value].append(record)
    for line, literal, shift in executable_numbers(text):
        if line in skipped:
            continue
        spelling = re.sub(r"[uUlL]+$", "", literal)
        value = int(spelling, 16 if spelling.lower().startswith("0x") else 10)
        rare = value >= RARE_MINIMUM and value % ROUND_MULTIPLE != 0
        owners = [record for record in by_value[value]
                  if rare or shift and record.name.endswith("_SHIFT")]
        if owners:
            names = ", ".join(f"{record.name} ({record.path}:{record.line})" for record in owners)
            yield Hit(path, line, f"{literal} restates {names}",
                      (RESTATE, value, tuple(record.name for record in owners)))


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


def package_roots(names):
    return tuple(sorted({m.group(1) for name in names if (m := PACKAGE_INCLUDE.match(name))}))


def include_graph(texts):
    roots = INCLUDE_ROOTS + package_roots(texts)
    return {name: tuple(target for include in INCLUDE.findall(blank_comments(text))
                        if (target := resolve_include(name, include, roots, texts.__contains__)))
            for name, text in texts.items() if Path(name).suffix in C_SUFFIXES}


def affected_files(texts, changed, direct):
    return {path for path, text in texts.items() if eligible_c(path, text)
            and include_closure(path, direct).intersection(changed)}


def scan(texts, candidates=None, *, direct):
    clean = {name: blank_comments(text) for name, text in texts.items()
             if protocol_source(name, text)}
    index = ConstantIndex({name: text for name, text in texts.items() if eligible_c(name, text)})
    hits, logged, errors = {}, [], []
    owners = defaultdict(list)
    for path, text in sorted(texts.items()):
        restate_source = eligible_c(path, text)
        is_protocol_source = protocol_source(path, text)
        if not restate_source and not is_protocol_source:
            continue
        skipped, found, invalid = escapes(path, text)
        logged.extend(found)
        errors.extend(invalid)
        if restate_source:
            if candidates is None or path in candidates:
                closure = include_closure(path, direct)
                group = list(restatements(path, text, index.visible(closure), skipped))
                if group or candidates is not None:
                    hits[path, RESTATE] = group
        if is_protocol_source:
            source = INCLUDE_DIRECTIVE.sub(lambda match: "\n" * match[0].count("\n"), clean[path])
            for kind, start, end in tokens(source):
                if kind == "literal" and source[start] == '"':
                    try:
                        value = ast.literal_eval(source[start:end])
                    except (SyntaxError, ValueError):
                        continue
                    if protocol_token(value):
                        owners[value].append((path, source.count("\n", 0, start) + 1))
    for path, text in sorted(texts.items()):
        if not path.startswith(("scripts/", "launcher/tools/")) or Path(path).suffix != ".py":
            continue
        skipped, found, invalid = escapes(path, text, python=True)
        logged.extend(found)
        errors.extend(invalid)
        group = [Hit(path, line, f'{value!r} repeats protocol token ({owners[value][0][0]}:{owners[value][0][1]}; '
                           f'{len(owners[value])} C sites)', (PROTOCOL, value))
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
        repeated = Counter((hit.line, hit.message) for hit in group)
        for (line, message), count in sorted(repeated.items()):
            suffix = f" ({count} hits)" if count > 1 else ""
            print(f"{path}:{line}: {message}{suffix}")
        totals[module(path)][rule] += len(group)
    for name, counts in sorted(totals.items()):
        print(f"{name}: RESTATE={counts[RESTATE]} PROTOCOL={counts[PROTOCOL]}")


def main(argv=None, root=ROOT):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"Thresholds: RARE_MINIMUM={RARE_MINIMUM}, ROUND_MULTIPLE={ROUND_MULTIPLE}, "
               f"DATA_ROW_MINIMUM={DATA_ROW_MINIMUM}, TOKEN_MINIMUM={TOKEN_MINIMUM}.")
    parser.add_argument("--base", default="origin/main", help="Compare with this branch's merge-base.")
    parser.add_argument("--report", action="store_true", help="List every HEAD hit and per-module totals.")
    args = parser.parse_args(argv)
    started = time.perf_counter()
    try:
        if args.report:
            texts = revision_tree(root, "HEAD")
            hits, logged, errors = scan(texts, direct=include_graph(texts))
            report(hits)
            grown = []
            old = {}
        else:
            base = comparison_base(root, against=args.base)
            changed, renames, deleted = changed_paths(root, base, "HEAD")
            head_texts = revision_tree(root, "HEAD")
            base_texts = revision_tree(root, base)
            head_direct = include_graph(head_texts)
            base_direct = include_graph(base_texts)
            reverse_renames = {previous: path for path, previous in renames.items()}
            base_changed = {renames.get(path, path) for path in changed} | deleted
            candidates = affected_files(head_texts, changed, head_direct) | {
                reverse_renames.get(path, path) for path in affected_files(base_texts, base_changed, base_direct)}
            hits, logged, errors = scan(head_texts, candidates, direct=head_direct)
            old, _, _ = scan(base_texts, {renames.get(path, path) for path in candidates}, direct=base_direct)
            compared = set(hits) | {(reverse_renames.get(path, path), rule) for path, rule in old}
            old = {(path, rule): old.get((renames.get(path, path), rule), []) for path, rule in compared}
            grown = []
            for (path, rule), group in sorted(hits.items()):
                previous = old.get((path, rule), [])
                if len(group) <= len(previous):
                    continue
                grown.append((path, rule))
                print(f"{path}: {rule} {len(previous)} -> {len(group)}")
                remaining = Counter(hit.identity for hit in previous)
                for hit in group:
                    if remaining[hit.identity]:
                        remaining[hit.identity] -= 1
                    else:
                        print(hit)
        for hit in logged + errors:
            print(hit)
        failed = bool(grown or errors)
        counts = Counter({rule: sum(len(group) for (path, candidate), group in hits.items() if candidate == rule)
                          for rule in (RESTATE, PROTOCOL)})
        summary = (f"RESTATE={counts[RESTATE]} PROTOCOL={counts[PROTOCOL]}" if args.report else
                   "; ".join(
                       f"{rule} ({scope})={sum(len(group) for (path, candidate), group in old.items() if candidate == rule)} -> {counts[rule]}"
                       for rule, scope in ((RESTATE, "compared files"), (PROTOCOL, "all Python"))))
        print(f"{'FAIL' if failed else 'PASS'}: {len(grown)} growing file/rule counts; "
              f"{summary}; "
              f"{time.perf_counter() - started:.2f}s.")
        return int(failed)
    except (OSError, ValueError, SyntaxError, tokenize.TokenError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
