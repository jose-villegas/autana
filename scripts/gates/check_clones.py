#!/usr/bin/env python3
"""Reject new jscpd pairs in tracked first-party C/C++ and Python.

Compare HEAD with its merge-base with origin/main, or HEAD~1 when HEAD is
on main. Pair keys use sorted filenames and whitespace-normalised fragments.
--report lists all pairs. --min-tokens N reports another threshold without
checking new pairs. MIN_TOKENS is 80: longer copied helpers and test setup
are detected, including renamed identifiers and changed literal values;
short helpers are missed. Literal-only fragments and exact self-matches
are excluded. The pinned engine ignores comments and whitespace.
"""
import argparse
import io
import json
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from check_generated_files import is_generated

ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT / "scripts/gates/node_modules/jscpd/run-jscpd.js"
MIN_TOKENS = 80
SUFFIXES = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".py"}


def eligible_name(name):
    path = Path(name)
    return (path.suffix in SUFFIXES
            and not name.startswith(("launcher/components/", "launcher/test/framework/"))
            and "fixtures" not in path.parts and "fixture" not in path.parts)


def git(root, *args):
    return subprocess.run(["git", *args], cwd=root, check=True,
                          capture_output=True).stdout


def comparison_base(root, head="HEAD"):
    head = git(root, "rev-parse", head).decode().strip()
    base = git(root, "merge-base", head, "origin/main").decode().strip()
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


# A suite's list of RUN_TEST lines matches any other long list once
# identifiers are ignored, and has no shared owner to extract; a pair counts
# only if what remains without them is still a clone's length in itself.
TEST_REGISTRATION = re.compile(r"\bRUN_TEST\s*\(\s*\w+\s*\)\s*;")


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


def revision_contents(root, revision, names, renames=None):
    renames = renames or {}
    requests = "".join(f"{revision}:{renames.get(name, name)}\n" for name in names).encode("utf-8")
    result = subprocess.run(["git", "cat-file", "--batch"], cwd=root, input=requests,
                            check=True, capture_output=True)
    stream = io.BytesIO(result.stdout)
    for name in names:
        header = stream.readline().rstrip(b"\n")
        if header.endswith(b" missing"):
            continue
        _, kind, size = header.split()
        if kind != b"blob":
            raise ValueError(f"Not a source blob: {revision}:{name}")
        content = stream.read(int(size))
        if len(content) != int(size) or stream.read(1) != b"\n":
            raise ValueError(f"Incomplete source blob: {revision}:{name}")
        yield name, content


def scan(root, minimum, names=None, revision="HEAD", renames=None):
    if minimum < 1:
        raise ValueError("minimum must be positive")
    problem = dependency_problem()
    if problem:
        raise RuntimeError(problem)
    if names is None:
        names = sorted(git(root, "ls-tree", "-r", "--name-only", revision, "--",
                           "launcher", "scripts", "editor").decode().splitlines())
    scratch_parent = ROOT / "launcher/test/build"
    scratch_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="clones-", dir=scratch_parent) as folder:
        scratch = Path(folder)
        tree = scratch / "source"
        tree.mkdir()
        names = [name for name in names if eligible_name(name)]
        contents = revision_contents(root, revision, names, renames)
        for name, content in contents:
            if is_generated(content.decode("utf-8")):
                continue
            target = tree / name
            target.parent.mkdir(parents=True, exist_ok=True)
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
        return sorted(filter_pairs(pairs), key=lambda pair: (-pair["tokens"], pair["firstFile"]["name"],
                                              pair["firstFile"]["start"], pair["secondFile"]["name"]))


def changed_paths(root, base, head):
    fields = iter(git(root, "diff", "-M", "--name-status", "-z", f"{base}...{head}")
                  .decode().rstrip("\0").split("\0"))
    changed = set()
    renames = {}
    for status in fields:
        if not status:
            continue
        name = next(fields)
        if status.startswith("R"):
            new_name = next(fields)
            renames[new_name] = name
            name = new_name
        changed.add(name)
    return changed, renames


def describe(pair):
    def location(side):
        item = pair[side]
        return f"{item['name']}:{item['start']}-{item['end']}"
    return f"{location('firstFile')} ~ {location('secondFile')} ({pair['tokens']} tokens)"


def pair_key(pair):
    names = tuple(sorted(pair[side]["name"] for side in ("firstFile", "secondFile")))
    return names, " ".join(pair["fragment"].split())


def check_pairs(pairs, base_pairs):
    existing = {pair_key(pair) for pair in base_pairs}
    added = [pair for pair in pairs if pair_key(pair) not in existing]
    if added:
        print(f"FAIL: {len(added)} new clone pairs; extract a shared owner.")
        for pair in added:
            print(describe(pair))
        return 1
    print(f"PASS: no new clone pairs ({len(pairs)} checked HEAD pairs, {len(base_pairs)} base pairs).")
    return 0


def main(argv=None, root=ROOT):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", action="store_true")
    parser.add_argument("--min-tokens", type=int, default=MIN_TOKENS)
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
        base = comparison_base(root, head)
        changed, renames = changed_paths(root, base, head)
        candidates = [pair for pair in pairs
                      if any(pair[side]["name"] in changed for side in ("firstFile", "secondFile"))]
        names = sorted({pair[side]["name"] for pair in candidates
                        for side in ("firstFile", "secondFile")})
        base_pairs = scan(root, args.min_tokens, names=names, revision=base, renames=renames) if names else []
        result = check_pairs(candidates, base_pairs)
        print(f"{len(pairs)} clone pairs at {args.min_tokens} tokens; {time.perf_counter() - started:.2f}s.")
        return result
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
