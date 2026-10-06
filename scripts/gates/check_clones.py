#!/usr/bin/env python3
"""Ratcheted jscpd clone count for tracked first-party C/C++ and Python.

--report lists all pairs; --changed REF lists pairs touching changed files.
Both check the whole-tree count. --min-tokens N measures another threshold
without checking the baseline. The pinned engine ignores comments, whitespace,
identifier names and literal values. Extract a shared owner when clones grow;
lower the committed scalar baseline in the same PR when the count falls.
MIN_TOKENS is 80: measurements at 30/40/60/80 gave the smallest report at
80 while retaining copied budget-test helpers. Short clamp helpers fall below
this bound; repeated tables and longer test setup remain counted.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from check_generated_files import is_generated
from tracked import tracked_files

ROOT = Path(__file__).resolve().parents[2]
BASELINE = ROOT / "scripts/gates/clones_baseline.txt"
ENGINE = ROOT / "scripts/gates/node_modules/jscpd/run-jscpd.js"
MIN_TOKENS = 80
SUFFIXES = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".py"}


def source_files(root):
    selected = []
    for name in sorted(tracked_files(root, ("launcher", "scripts", "editor"))):
        path = Path(name)
        if path.suffix not in SUFFIXES:
            continue
        if name.startswith(("launcher/components/", "launcher/test/framework/")):
            continue
        if "fixtures" in path.parts or "fixture" in path.parts:
            continue
        if not is_generated((root / name).read_text(encoding="utf-8")):
            selected.append(name)
    return selected


def scan(root, minimum, names=None):
    if minimum < 1:
        raise ValueError("minimum must be positive")
    if not ENGINE.is_file():
        raise RuntimeError("Install the pinned clone engine: npm ci --prefix scripts/gates")
    names = source_files(root) if names is None else names
    scratch_parent = ROOT / "launcher/test/build"
    scratch_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="clones-", dir=scratch_parent) as folder:
        scratch = Path(folder)
        tree = scratch / "source"
        tree.mkdir()
        for name in names:
            target = tree / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root / name, target)
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
        return sorted(pairs, key=lambda pair: (-pair["tokens"], pair["firstFile"]["name"],
                                              pair["firstFile"]["start"], pair["secondFile"]["name"]))


def describe(pair):
    def location(side):
        item = pair[side]
        return f"{item['name']}:{item['start']}-{item['end']}"
    return f"{location('firstFile')} ~ {location('secondFile')} ({pair['tokens']} tokens)"


def touching(pair, changed):
    return any(pair[side]["name"] in changed for side in ("firstFile", "secondFile"))


def ratchet(count, baseline):
    if count > baseline:
        print(f"FAIL: {count} clone pairs > baseline {baseline}; extract a shared owner.")
        return 1
    print(f"PASS: {count} clone pairs <= baseline {baseline}.")
    if count < baseline:
        print(f"Lower scripts/gates/clones_baseline.txt to {count} in the same PR.")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", action="store_true")
    parser.add_argument("--changed", metavar="REF")
    parser.add_argument("--min-tokens", type=int, default=MIN_TOKENS)
    args = parser.parse_args(argv)
    if args.min_tokens < 1:
        parser.error("--min-tokens must be positive")
    started = time.perf_counter()
    try:
        pairs = scan(ROOT, args.min_tokens)
        changed = None
        if args.changed:
            changed = set(subprocess.run(["git", "diff", "--name-only", args.changed, "--"],
                                        cwd=ROOT, check=True, capture_output=True, text=True).stdout.splitlines())
        if args.report or changed is not None:
            for pair in pairs:
                if changed is None or touching(pair, changed):
                    print(describe(pair))
        print(f"{len(pairs)} clone pairs at {args.min_tokens} tokens; {time.perf_counter() - started:.2f}s.")
        if args.min_tokens != MIN_TOKENS:
            return 0
        return ratchet(len(pairs), int(BASELINE.read_text(encoding="utf-8").strip()))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
