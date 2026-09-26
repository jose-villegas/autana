#!/usr/bin/env python3
"""Count selected clang-tidy diagnostics in first-party launcher/main files."""
import argparse
from collections import Counter
from pathlib import Path
import re
import subprocess
import sys

import complexity_gate as complexity

CHECKS = (
    "readability-math-missing-parentheses",
    "readability-isolate-declaration",
    "readability-implicit-bool-conversion",
    "readability-uppercase-literal-suffix",
    "bugprone-macro-parentheses",
    "misc-use-internal-linkage",
    "bugprone-unused-return-value",
    "cert-err33-c",
    "bugprone-switch-missing-default-case",
    "bugprone-narrowing-conversions",
)
BASELINE = Path(__file__).with_name("misra_tidy_baseline.txt")
DIAGNOSTIC = re.compile(
    r"^(?P<file>.+):(?P<line>\d+):\d+: warning: .+ \[(?P<check>[^]]+)\]$"
)


def count_diagnostics(output):
    counts = Counter()
    locations = {}
    for line in output.splitlines():
        match = DIAGNOSTIC.match(line)
        if not match or match.group("check") not in CHECKS:
            continue
        path = Path(match.group("file")).resolve()
        try:
            rel = path.relative_to(complexity.LAUNCHER_DIR / "main")
        except ValueError:
            continue
        name = "main/" + rel.as_posix()
        key = (match.group("check"), name)
        counts[key] += 1
        locations.setdefault(key, int(match.group("line")))
    return counts, locations


def load_baseline(path):
    counts = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            check, name, count = line.split("\t")
            if check not in CHECKS or (check, name) in counts:
                raise ValueError(f"invalid baseline entry: {line}")
            counts[(check, name)] = int(count)
    return counts


def write_baseline(path, counts):
    lines = ["# check\tfile\tcount"]
    lines.extend(f"{check}\t{name}\t{count}" for (check, name), count
                 in sorted(counts.items()) if count)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--update-baseline", action="store_true")
    parser.add_argument("--fix", choices=CHECKS, metavar="CHECK")
    parser.add_argument("--baseline", type=Path, default=BASELINE)
    args = parser.parse_args()
    if args.update_baseline and args.fix:
        parser.error("--fix and --update-baseline cannot be combined")

    clang_tidy, major = complexity.resolve_clang_tidy()
    db_path, sources, _ = complexity.build_compile_db()
    _, missing = complexity.check_main_coverage(sources)
    if missing:
        sys.exit("unmeasured main files: " + ", ".join(missing))
    files = [name for name in sources if Path(name).resolve().is_relative_to(
        complexity.LAUNCHER_DIR / "main")]
    checks = (args.fix,) if args.fix else CHECKS
    command = complexity.clang_tidy_cmd(clang_tidy, db_path)
    command += ["--checks=-*," + ",".join(checks)]
    if args.fix:
        command.append("--fix")
        for file in files:
            result = subprocess.run(command + [file], capture_output=True, text=True)
            errors = complexity.find_parse_errors(result.stdout + "\n" + result.stderr)
            if result.returncode or errors:
                for location, message in errors[:30]:
                    print(f"{location or '(unattributed)'}: {message}")
                sys.exit(f"clang-tidy failed in {file} ({result.returncode})")
        print(f"clang-tidy {major}: applied {args.fix} fix-its; review the diff")
        return 0
    result = complexity.run_clang_tidy_parallel(command, files)
    errors = complexity.find_parse_errors(result.stdout + "\n" + result.stderr)
    if result.returncode or errors:
        for file, message in errors[:30]:
            print(f"{file or '(unattributed)'}: {message}")
        sys.exit(f"clang-tidy failed ({result.returncode})")
    counts, locations = count_diagnostics(result.stdout + "\n" + result.stderr)
    if not args.baseline.exists() and not args.update_baseline:
        sys.exit(f"missing baseline: {args.baseline}")
    if args.update_baseline:
        write_baseline(args.baseline, counts)
        print(f"wrote {sum(counts.values())} findings to {args.baseline}")
        return 0
    baseline = load_baseline(args.baseline)
    increases = [(key, count, baseline.get(key, 0)) for key, count in counts.items()
                 if count > baseline.get(key, 0)]
    decreases = [(key, old, counts.get(key, 0)) for key, old in baseline.items()
                 if counts.get(key, 0) < old]
    for (check, name), count, old in sorted(increases):
        print(f"FAIL {name}:{locations[(check, name)]}: {check} {old} -> {count}")
    for (check, name), old, count in sorted(decreases):
        print(f"LOWER {name}: {check} {old} -> {count}; use --update-baseline")
    print(f"clang-tidy {major}: {sum(counts.values())} findings, "
          f"{len(increases)} increased (baseline {sum(baseline.values())})")
    return 1 if increases else 0


if __name__ == "__main__":
    sys.exit(main())
