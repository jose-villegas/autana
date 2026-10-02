#!/usr/bin/env python3
"""Summarise named timing rows from repeated performance reports."""
import argparse
import json
import re
from pathlib import Path


CONTROL_ROWS = (
    "test_a_full_size_step_fits_in_the_frame_budget",
    "test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget",
)
MEAN_RE = re.compile(r"\b(?P<name>\S+) both cores: mean\s+(?P<value>\d+)us\b")
NUMBER_RE = re.compile(r"^[+-]?\d+$")


def cell(text):
    return text.strip().strip("`").replace("**", "")


def parse_markdown(text):
    lines = text.splitlines()
    for index in range(len(lines) - 2):
        if not (lines[index].startswith("|") and lines[index + 1].startswith("|")):
            continue
        headers = [cell(part) for part in lines[index].strip("|").split("|")]
        if not all(re.fullmatch(r"\s*:?-{3,}:?\s*", part) for part in lines[index + 1].strip("|").split("|")):
            continue
        lowered = [header.lower() for header in headers]
        if not any(name in lowered[0] for name in ("test", "name", "scenario", "run", "mesh")):
            continue
        value_index = next((i for i, name in enumerate(lowered)
                            if "measured" in name or "mean" in name), None)
        if value_index is None and len(headers) == 2:
            value_index = 1
        if value_index is None:
            continue
        rows = {}
        for line in lines[index + 2:]:
            if not line.startswith("|"):
                break
            parts = [cell(part) for part in line.strip("|").split("|")]
            if len(parts) <= value_index or not NUMBER_RE.fullmatch(parts[value_index]):
                continue
            rows[parts[0]] = int(parts[value_index])
        if rows:
            return rows
    return {}


def parse_report(path):
    """Return named microsecond rows from a report table or suite capture."""
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    rows = parse_markdown(text)
    if rows:
        return rows
    return {match.group("name"): int(match.group("value"))
            for match in MEAN_RE.finditer(text)}


def worst(runs):
    rows = {}
    for report in runs:
        for name, value in report.items():
            rows[name] = max(rows.get(name, value), value)
    return rows


def percent(old, new):
    if old == 0:
        return float("inf") if new else 0.0
    return (new - old) * 100.0 / old


def compare(a_runs, b_runs, controls=CONTROL_ROWS):
    """Compare the worst observation of every row on each side."""
    a, b = worst(a_runs), worst(b_runs)
    floor = None
    if all(name in a and name in b for name in controls):
        floor = max(0.5, *(abs(percent(a[name], b[name])) for name in controls))
    rows = []
    for name in sorted(set(a) & set(b)):
        delta = b[name] - a[name]
        if delta == 0 or (floor is not None and abs(percent(a[name], b[name])) <= floor):
            verdict = "no change"
        else:
            verdict = "regressed" if delta > 0 else "improved"
        rows.append((name, a[name], b[name], delta, verdict))
    return rows


def write_aggregate(path, runs):
    """Write worst-of-run values in the report shape compare_reports.py reads."""
    lines = ["| Test | Budget (us) | Measured (us) | Headroom | Status |",
             "|---|---:|---:|---:|---|"]
    for name, value in sorted(worst(runs).items()):
        lines.append(f"| `{name}` | ? | {value} | ? | measured |")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_summary(path, label_a, label_b, build_a, build_b, a_paths, b_paths):
    a_runs = [parse_report(report) for report in a_paths]
    b_runs = [parse_report(report) for report in b_paths]
    if not all(a_runs + b_runs):
        raise ValueError("a report contains no named numeric rows")
    rows = compare(a_runs, b_runs)
    a, b = worst(a_runs), worst(b_runs)
    controls = all(name in a and name in b for name in CONTROL_ROWS)
    lines = ["# Performance comparison", "",
             f"- A: `{label_a}`; build ids: {', '.join(f'`{item}`' for item in build_a)}",
             f"- B: `{label_b}`; build ids: {', '.join(f'`{item}`' for item in build_b)}",
             "", "Values are the worst of the captured runs for each row.", "",
             "| Row | A (us) | B (us) | Delta (us) | Verdict |",
             "|---|---:|---:|---:|---|"]
    for name, old, new, delta, verdict in rows:
        lines.append(f"| `{name}` | {old} | {new} | {delta:+d} | {verdict} |")
    if controls:
        floor = max(0.5, *(abs(percent(a[name], b[name])) for name in CONTROL_ROWS))
        lines.extend(["", f"Control-row noise floor: {floor:.1f}%. Rows inside it are no change."])
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return controls


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--label-a", required=True)
    parser.add_argument("--label-b", required=True)
    parser.add_argument("--build-a", action="append", required=True)
    parser.add_argument("--build-b", action="append", required=True)
    parser.add_argument("--a", action="append", required=True, type=Path)
    parser.add_argument("--b", action="append", required=True, type=Path)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    controls = write_summary(args.out, args.label_a, args.label_b, args.build_a,
                             args.build_b, args.a, args.b)
    if controls:
        write_aggregate(args.out.parent / "a" / "worst.md",
                        [parse_report(report) for report in args.a])
        write_aggregate(args.out.parent / "b" / "worst.md",
                        [parse_report(report) for report in args.b])
    (args.out.parent / "comparison.json").write_text(
        json.dumps({"sand": controls}) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
