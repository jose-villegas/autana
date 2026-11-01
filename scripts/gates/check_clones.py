#!/usr/bin/env python3
"""Reject growing jscpd file-pair budgets in tracked first-party C/C++ and Python.

Compare HEAD with its merge-base with --base (origin/main unless a pull
request targets another branch), or HEAD~1 when HEAD is on that branch.
New fragments in each sorted file pair may spend only the tokens of base
fragments that vanished from HEAD, or a fragment a file this change deleted
already held (code moved out of it). A surviving shrink gives no headroom.
--report lists all pairs. --min-tokens N reports another threshold without
checking new pairs. MIN_TOKENS is 80: longer copied helpers and test setup
are detected, including renamed identifiers and changed literal values.
A shorter Python function pairs with another whose body is the same up to
the names it binds (see function_shape). Literal-only fragments and exact self-matches
are excluded. Literal-only and renaming object-like #defines are blanked on both sides
before matching, preserving line numbers; a declaration statement is written
one declarator per statement and parentheses after no name, `)` or `]` are
blanked, so neither a split nor an added grouping counts as copying. The pinned
engine ignores comments and whitespace.
"""
import argparse
import ast
import copy
import itertools
import json
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from check_generated_files import is_generated
from tracked import revision_contents

ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT / "scripts/gates/node_modules/jscpd/run-jscpd.js"
MIN_TOKENS = 80
# Below this many AST nodes a same-shape function is an idiom (`return self`,
# `self.directory.cleanup()`), not a helper worth an owner.
MIN_FUNCTION_NODES = 20
SUFFIXES = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".py"}


def eligible_name(name):
    path = Path(name)
    return (path.suffix in SUFFIXES
            and not name.startswith(("launcher/components/", "launcher/packages/testkit/unity/"))
            and "fixtures" not in path.parts and "fixture" not in path.parts)


def git(root, *args):
    return subprocess.run(["git", *args], cwd=root, check=True,
                          capture_output=True).stdout


def comparison_base(root, head="HEAD", against="origin/main"):
    head = git(root, "rev-parse", head).decode().strip()
    base = git(root, "merge-base", head, against).decode().strip()
    return git(root, "rev-parse", f"{head}~1").decode().strip() if base == head else base


def dependency_problem():
    if shutil.which("node") is None:
        return "Node.js is missing from PATH; install Node.js to run the clone gate."
    if not ENGINE.is_file():
        return "Install the pinned clone engine: npm ci --prefix scripts/gates"
    return None


# Literals and comments cannot turn a data row into executable duplication.
C_TOKENS = re.compile(
    r'//[^\n]*|/\*.*?\*/|(?:u8|[LuU])?(?:"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\')'
    r'|(?:\d[\w.]*(?:[eEpP][+-]?\d+)?|\.\d[\w.]*)|(?P<identifier>[A-Za-z_]\w*)',
    re.DOTALL,
)
PYTHON_TOKENS = re.compile(
    r"\#[^\n]*|(?i:[rubf]*)(?:\"\"\".*?\"\"\"|\'\'\'.*?\'\'\'|\"(?:\\.|[^\"\\])*\"|\'(?:\\.|[^\'\\])*\')"
    r"|(?:\d[\w.]*(?:[eE][+-]?\d+)?|\.\d[\w.]*)|(?P<identifier>[^\W\d]\w*)",
    re.DOTALL,
)

# A word or one punctuation character: what same_fragment and a short function count.
CODE_TOKEN = re.compile(r"\w+|[^\w\s]")

# A suite's list of RUN_TEST lines, or the rows of a data table, matches any
# other long list once identifiers are ignored, and has no shared owner to
# extract; a pair counts only if what remains without them is still a
# clone's length in itself. A table row is a braced initializer holding no
# statement, so a block of code never passes for one. An inline assembly body
# is the same: with literals ignored, its run of string lines matches any other
# such run, its own shifted by a line included.
STRING_RUN = r'(?:"(?:\\.|[^"\\])*"\s*(?:/\*.*?\*/\s*|//[^\n]*\s*)*){2,}'
TEST_REGISTRATION = re.compile(r"\bRUN_TEST\s*\(\s*\w+\s*\)\s*;|\{[^{};]*\}\s*,|" + STRING_RUN, re.DOTALL)
INCLUDE_DIRECTIVE = re.compile(r"(?m)^[ \t]*#[ \t]*include\b(?:[^\n]*\\\n)*[^\n]*")
OBJECT_DEFINE = re.compile(r"(?m)^[ \t]*#[ \t]*define[ \t]+[A-Za-z_]\w*[ \t]+((?:[^\n]*\\\n)*[^\n]*)")
C_LITERAL = (r'''(?:[+-]?(?:0[xX][0-9a-fA-F]+|\d+(?:\.\d*)?(?:[eE][+-]?\d+)?|'''
             r'''\.\d+(?:[eE][+-]?\d+)?)[uUlLfF]*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')''')
# A define naming one identifier only renames it, as a test renames the code it includes.
LITERAL_BODY = re.compile(rf"\s*(?:{C_LITERAL}|[A-Za-z_]\w*|\{{\s*(?:{C_LITERAL}(?:\s*,\s*{C_LITERAL})*\s*,?)?\s*\}})\s*")


def literal_declaration(match):
    """Blank a literal-only or renaming object define, keeping its newline count."""
    body = match[1].replace("\\\n", " ")
    body = re.sub(r"//[^\n]*|/\*.*?\*/", " ", body, flags=re.DOTALL)
    return "\n" * match[0].count("\n") if LITERAL_BODY.fullmatch(body) else match[0]


# One declaration statement is the same code as several: `int a = x, b = y;` and
# `int a = x; int b = y;` differ only in the type words and separators the split
# repeats, so a mechanical split would count as a new clone. Before matching, each
# declaration is written one declarator per statement.
MASKED = re.compile(r"""//[^\n]*|/\*.*?\*/|(?:u8|[LuU])?(?:"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')"""
                    r"""|(?m:^[ \t]*#(?:[^\n]*\\\n)*[^\n]*)""", re.DOTALL)
STATEMENT_START = re.compile(r"(?:\A|[;{}])\s*")
DECLARATION = re.compile(r"((?:[A-Za-z_]\w*[\s*]+)+)([A-Za-z_]\w*)\s*(?=[=;,\[])")
DECLARATOR_PREFIX = re.compile(r"[\s*]*")
NOT_A_TYPE = {"return", "goto", "else", "do", "case", "default", "sizeof", "typeof", "break", "continue"}


def blank(text):
    """The same text with newlines kept and everything else a space."""
    return re.sub(r"[^\n]", " ", text)


def split_declarations(text):
    """Rewrite `T a = x, *b = y;` as `T a = x; T *b = y;` on the same line, the
    form one declaration per statement gives, keeping every line where it was."""
    masked = MASKED.sub(lambda match: blank(match[0]), text)
    edits = []
    for start in STATEMENT_START.finditer(masked):
        found = DECLARATION.match(masked, start.end())
        words = found[1].replace("*", " ").split() if found else []
        if not words or words[0] in NOT_A_TYPE:
            continue
        depth = 0
        for index in range(found.start(2), len(masked)):
            char = masked[index]
            if char in "([{":
                depth += 1
            elif char in ")]}":
                depth -= 1
            elif depth == 0 and char == ";":
                break
            elif depth == 0 and char == ",":
                after = DECLARATOR_PREFIX.match(masked, index + 1).end()
                edits.append((index, after, "; " + " ".join(words) + " " + masked[index + 1:after].replace("\n", " ").strip()))
            if depth < 0:
                break
    for begin, end, replacement in reversed(edits):
        text = text[:begin] + replacement + text[end:]
    return text


def drop_grouping_parentheses(text):
    """Blank every parenthesis pair that does not follow a name, `)` or `]`, or
    follows `return`: a precedence grouping `(a * b) + c`, or a cast. Calls,
    conditions and declarators keep theirs. A rewrite that only adds groupings
    then leaves the token stream as it was; a copy that holds them is counted
    with fewer tokens."""
    masked = MASKED.sub(lambda match: blank(match[0]), text)
    blanked = []
    stack = []
    previous = ""
    for token in re.finditer(r"\w+|\S", masked):
        char = token[0]
        if char == "(":
            grouping = previous == "return" or not (previous[-1:].isalnum() or previous[-1:] in ("_", ")", "]"))
            stack.append(grouping)
            if grouping:
                blanked.append(token.start())
        elif char == ")" and stack and stack.pop():
            blanked.append(token.start())
        previous = char
    chars = list(text)
    for index in blanked:
        chars[index] = " "
    return "".join(chars)


def normalise_c(text):
    return drop_grouping_parentheses(split_declarations(text))


def function_shape(function):
    """The function's AST dump with its docstring, name, decorators and annotations
    dropped and each parameter and local renamed by first use; globals, attributes
    and literals stay as written; and its token count without comments. None for a
    body that is only a docstring or under MIN_FUNCTION_NODES."""
    tree = copy.deepcopy(function)
    tree.name, tree.decorator_list, tree.returns = "", [], None
    if isinstance(tree.body[0], ast.Expr) and isinstance(tree.body[0].value, ast.Constant) \
            and isinstance(tree.body[0].value.value, str):
        tree.body = tree.body[1:]
    if not tree.body:
        return None, 0
    bound = {node.arg for node in ast.walk(tree) if isinstance(node, ast.arg)}
    bound |= {node.id for node in ast.walk(tree) if isinstance(node, ast.Name) and not isinstance(node.ctx, ast.Load)}
    numbers = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.arg):
            node.arg, node.annotation = numbers.setdefault(node.arg, f"_{len(numbers)}"), None
        elif isinstance(node, ast.Name) and node.id in bound:
            node.id = numbers.setdefault(node.id, f"_{len(numbers)}")
    if sum(1 for _ in ast.walk(tree)) < MIN_FUNCTION_NODES:
        return None, 0
    return ast.dump(tree), len(CODE_TOKEN.findall(ast.unparse(tree)))


def short_function_pairs(sources, minimum):
    """Pairs of same-shape Python functions under `minimum` tokens, the engine's
    floor, from {name: text}."""
    shapes = {}
    for name, text in sources.items():
        for function in ast.walk(ast.parse(text)):
            if not isinstance(function, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            shape, tokens = function_shape(function)
            if shape and tokens < minimum:
                side = {"name": name, "start": function.lineno, "end": function.end_lineno}
                shapes.setdefault(shape, []).append((side, tokens, ast.get_source_segment(text, function)))
    return [{"format": "python", "tokens": tokens, "fragment": fragment, "firstFile": first, "secondFile": second}
            for group in shapes.values()
            for (first, tokens, fragment), (second, _, _) in itertools.combinations(group, 2)]


def registration_only(pair):
    if pair["format"] == "python":
        return False
    rest = TEST_REGISTRATION.sub(" ", pair["fragment"])
    return pair["fragment"] != rest and len(C_TOKENS.findall(rest)) < MIN_TOKENS // 2


def filter_pairs(pairs):
    def keep(pair):
        if registration_only(pair):
            return False
        first, second = pair["firstFile"], pair["secondFile"]
        same_range = (first["name"] == second["name"]
                      and all(first.get(field) == second.get(field)
                              for field in ("start", "end", "startLoc", "endLoc")))
        tokens = PYTHON_TOKENS if pair["format"] == "python" else C_TOKENS
        return not same_range and any(token.group("identifier")
                                      for token in tokens.finditer(pair["fragment"]))
    return [pair for pair in pairs if keep(pair)]


def scan(root, minimum, names=None, revision="HEAD", renames=None):
    if minimum < 1:
        raise ValueError("minimum must be positive")
    problem = dependency_problem()
    if problem:
        raise RuntimeError(problem)
    if names is None:
        names = sorted(git(root, "ls-tree", "-r", "--name-only", revision, "--",
                           "launcher", "scripts").decode().splitlines())
    scratch_parent = ROOT / "launcher/test/build"
    scratch_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="clones-", dir=scratch_parent) as folder:
        scratch = Path(folder)
        tree = scratch / "source"
        tree.mkdir()
        names = [name for name in names if eligible_name(name)]
        contents = revision_contents(root, revision, names, renames)
        python_sources = {}
        for name, content in contents:
            content = content.replace(b"\r\n", b"\n")
            if is_generated(content.decode("utf-8")):
                continue
            if name.endswith(".py"):
                python_sources[name] = content.decode("utf-8")
            target = tree / name
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.suffix != ".py":
                # A fragment holds only one side, so either side's declaration macros must vanish before matching.
                content = OBJECT_DEFINE.sub(literal_declaration, content.decode("utf-8")).encode("utf-8")
                content = INCLUDE_DIRECTIVE.sub(lambda match: "\n" * match[0].count("\n"),
                                                content.decode("utf-8")).encode("utf-8")
                content = normalise_c(content.decode("utf-8")).encode("utf-8")
            target.write_bytes(content)
        config = scratch / "config.json"
        config.write_text(json.dumps({"minTokens": minimum, "minLines": 0,
                                     "mode": "weak", "ignoreIdentifiers": True,
                                     "ignoreLiterals": True, "format": ["c", "c-header", "cpp", "cpp-header", "python"],
                                     "crossFormats": [["c", "c-header", "cpp", "cpp-header"]], "reporters": ["json"], "output": str(scratch / "report"),
                                     "silent": True, "absolute": True}), encoding="utf-8")
        result = subprocess.run(["node", str(ENGINE), "--config", str(config), str(tree)],
                                cwd=scratch, capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError(f"jscpd failed ({result.returncode}): {result.stderr.strip()}")
        report = json.loads((scratch / "report/jscpd-report.json").read_text(encoding="utf-8"))
        pairs = report["duplicates"]
        for pair in pairs:
            for side in ("firstFile", "secondFile"):
                pair[side]["name"] = Path(pair[side]["name"].removeprefix("\\\\?\\")).relative_to(tree).as_posix()
        pairs += short_function_pairs(python_sources, minimum)
        return sorted(filter_pairs(pairs), key=lambda pair: (-pair["tokens"], pair["firstFile"]["name"],
                                              pair["firstFile"]["start"], pair["secondFile"]["name"]))


def changed_paths(root, base, head):
    fields = iter(git(root, "diff", "-M", "--name-status", "-z", f"{base}...{head}")
                  .decode().rstrip("\0").split("\0"))
    changed = set()
    renames = {}
    deleted = set()
    for status in fields:
        if not status:
            continue
        name = next(fields)
        if status.startswith("R"):
            new_name = next(fields)
            renames[new_name] = name
            name = new_name
        elif status.startswith("D"):
            deleted.add(name)
            continue
        changed.add(name)
    return changed, renames, deleted


def describe(pair):
    def location(side):
        item = pair[side]
        return f"{item['name']}:{item['start']}-{item['end']}"
    return f"{location('firstFile')} ~ {location('secondFile')} ({pair['tokens']} tokens)"


def pair_key(pair):
    return tuple(sorted(pair[side]["name"] for side in ("firstFile", "secondFile")))


def same_fragment(pair, old):
    fragment = CODE_TOKEN.findall(pair["fragment"])
    tokens = iter(CODE_TOKEN.findall(old["fragment"]))
    return pair["tokens"] <= old["tokens"] and all(any(token == previous for previous in tokens)
                                                   for token in fragment)


def check_pairs(pairs, base_pairs, moved=()):
    """`moved` holds base pairs in files this change deleted: a pair whose
    fragment one of them already held moved with its code, and spends it."""
    moved = list(moved)
    existing = {}
    for pair in base_pairs:
        names = pair_key(pair)
        existing.setdefault(names, []).append(pair)
    current = {}
    for pair in pairs:
        current.setdefault(pair_key(pair), []).append(pair)
    grown = {}
    for names, group in current.items():
        remaining = list(existing.get(names, []))
        added = 0
        for pair in sorted(group, key=lambda item: -item["tokens"]):
            match = next((old for old in remaining if same_fragment(pair, old)), None)
            if match is not None:
                remaining.remove(match)
                continue
            match = next((old for old in moved if same_fragment(pair, old)), None)
            if match is not None:
                moved.remove(match)
                continue
            added += pair["tokens"]
        if added > sum(pair["tokens"] for pair in remaining):
            grown[names] = group
    if grown:
        print(f"FAIL: {len(grown)} new or growing clone file pairs; extract a shared owner.")
        for names, group in sorted(grown.items()):
            total = sum(pair["tokens"] for pair in group)
            old_total = sum(pair["tokens"] for pair in existing.get(names, []))
            print(f"{' ~ '.join(names)}: {old_total} -> {total} cloned tokens")
            for pair in group:
                print(describe(pair))
        return 1
    print(f"PASS: no growing clone file pairs ({len(pairs)} checked HEAD pairs, {len(base_pairs)} base pairs).")
    return 0


def main(argv=None, root=ROOT):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", action="store_true")
    parser.add_argument("--min-tokens", type=int, default=MIN_TOKENS)
    parser.add_argument("--base", default="origin/main", help="the branch a pull request targets")
    args = parser.parse_args(argv)
    if args.min_tokens < 1:
        parser.error("--min-tokens must be positive")
    started = time.perf_counter()
    try:
        head = git(root, "rev-parse", "HEAD").decode().strip()
        pairs = scan(root, args.min_tokens, revision=head)
        if args.report or args.min_tokens != MIN_TOKENS:
            for pair in pairs:
                print(describe(pair))
        if args.min_tokens != MIN_TOKENS:
            print(f"{len(pairs)} clone pairs at {args.min_tokens} tokens; {time.perf_counter() - started:.2f}s.")
            return 0
        base = comparison_base(root, head, args.base)
        changed, renames, deleted = changed_paths(root, base, head)
        candidates = [pair for pair in pairs
                      if any(pair[side]["name"] in changed for side in ("firstFile", "secondFile"))]
        names = sorted({pair[side]["name"] for pair in candidates
                        for side in ("firstFile", "secondFile")})
        base_pairs = (scan(root, args.min_tokens, names=names + sorted(deleted), revision=base, renames=renames)
                      if names else [])
        moved = [pair for pair in base_pairs
                 if any(pair[side]["name"] in deleted for side in ("firstFile", "secondFile"))]
        result = check_pairs(candidates, base_pairs, moved)
        print(f"{len(pairs)} clone pairs at {args.min_tokens} tokens; {time.perf_counter() - started:.2f}s.")
        return result
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
