#!/usr/bin/env python3
"""Compare report timing rows against a percentage threshold.

Seeded comparisons in launcher/tools/perf distinguish layout variation from change.
The verdict mode also requires an absolute microsecond movement.
"""
import argparse
import os
import re
import sys

# report_performance.py's budget table: | `name` | budget | measured | headroom | status |
BUDGET_ROW_RE = re.compile(
    r"^\|\s*`(?P<name>\w+)`\s*\|\s*[^|]+\|\s*(?P<measured>[^|]+?)\s*\|\s*[^|]+\|\s*[^|]+\|\s*$"
)
# Its second table, tests measured with no fixed budget: | `name` | measured |
MEASURED_ROW_RE = re.compile(r"^\|\s*`(?P<name>\w+)`\s*\|\s*(?P<measured>[^|]+?)\s*\|\s*$")

# report_performance.py's headline: `Total run time: 7m 10.6s (430600 ms)`.
TOTAL_RE = re.compile(r"^Total run time:\s*(?P<human>.+?)\s*\((?P<ms>\d+)\s*ms\)")
# Where the report says its capture came from, used only for that file's name.
SOURCE_RE = re.compile(r"^Source:\s*`(?P<path>[^`]+)`")

# A pass-decomposition row in the raw serial capture. The trailing text after
# `us` varies (some rows print a share of the whole, some do not), so only the
# leading value is captured; the two-colon shape is what tells a split's row
# from an ordinary benchmark result on the same log tag.
DECOMP_RE = re.compile(
    r"^I \(\d+\) device_tests: (?P<key>.+?): (?P<us>\d+) us(?![a-z])"
)

# Total run times further apart than this are the signature of a perf-scoped
# capture being compared against a full one - 39 timed tests against 952.
# Deliberately loose: a genuine within-scope pair lands within a few percent,
# so anything near this is already the wrong comparison.
SCOPE_MISMATCH_PCT = 20.0

def parse_report(path: str) -> dict:
    """Returns {test name: measured microseconds}, pooling both of
    report_performance.py's tables - a name appears in exactly one of them,
    so there is no ambiguity to resolve between the two row shapes."""
    measured = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = BUDGET_ROW_RE.match(line)
            if not m:
                m = MEASURED_ROW_RE.match(line)
            if not m:
                continue
            value = m.group("measured").strip()
            if value == "?" or not value.lstrip("+-").isdigit():
                continue  # unmeasured row (test didn't run this capture)
            measured[m.group("name")] = int(value)
    return measured


def parse_total(path: str):
    """(human, milliseconds) from the report's headline, or None."""
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = TOTAL_RE.match(line.strip())
            if m:
                return m.group("human"), int(m.group("ms"))
    return None


def find_raw(path: str):
    """The `*_raw.txt` beside a report.

    By timestamp, not by the report's own `Source:` line: that line records an
    absolute path from whichever checkout took the capture, and a report read
    on another machine - or moved out of the capture checkout, which is the
    normal way one is kept - would send this looking in a directory that does
    not exist here. The stamp in the two filenames is the same by
    construction (any report named *_<stamp>.md sits next to
    performance_<stamp>_raw.txt), so it survives the move."""
    directory = os.path.dirname(os.path.abspath(path))
    base = os.path.basename(path)
    stamp = re.search(r"(\d{8}_\d{6})", base)
    if stamp:
        candidate = os.path.join(directory, "performance_%s_raw.txt" % stamp.group(1))
        if os.path.exists(candidate):
            return candidate
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = SOURCE_RE.match(line.strip())
            if m:
                candidate = os.path.join(directory, os.path.basename(m.group("path")))
                if os.path.exists(candidate):
                    return candidate
            if line.startswith("|"):
                break
    return None


def parse_decomposition(path: str) -> dict:
    """{`<prefix>: <label>`: microseconds} from a raw capture.

    First occurrence wins. A repeat means the same split ran twice in one
    capture, which is a capture worth looking at by hand rather than one to
    silently average."""
    rows = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = DECOMP_RE.match(line.strip())
            if not m:
                continue
            key = m.group("key")
            if ": " not in key:
                continue    # an ordinary benchmark result, not a split's row
            rows.setdefault(key, int(m.group("us")))
    return rows


# Smallest absolute movement, in microseconds, that --verdict will treat as
# real regardless of how large it looks as a percentage. See its use below.
MIN_ABS_DELTA_US = 20


def pct_delta(old: int, new: int) -> float:
    if old == 0:
        return float("inf") if new else 0.0
    return (new - old) / old * 100.0


def format_row(name, old_v, new_v):
    delta = new_v - old_v
    pct = pct_delta(old_v, new_v)
    line = f"| `{name}` | {old_v} | {new_v} | {delta:+d} | {pct:+.1f}% |"
    return line


def print_delta_table(old: dict, new: dict, threshold: float) -> int:
    """Common rows, largest absolute percentage move first. Returns how many
    rows regressed by more than `threshold`.

    Rows that moved less than the threshold are hidden, and the COUNT OF THEM
    IS ALWAYS PRINTED, even when it is zero. A filter that quietly drops rows
    is how a regression leaves a round unnoticed, and the whole point of
    comparing by machine is that nothing falls off the bottom of the table."""
    common = sorted(set(old) & set(new),
                    key=lambda n: -abs(pct_delta(old[n], new[n])))

    print("| Test | Old (us) | New (us) | Delta | Delta % |")
    print("|---|---:|---:|---:|---:|")

    hidden, regressed = 0, 0
    for name in common:
        old_v, new_v = old[name], new[name]
        pct = pct_delta(old_v, new_v)
        if pct > threshold:
            regressed += 1
        if abs(pct) < threshold:
            hidden += 1
            continue
        print(format_row(name, old_v, new_v))
    print()
    print(f"{len(common) - hidden} of {len(common)} rows shown; {hidden} hidden "
          f"for moving less than the {threshold:.1f}% threshold.")
    print()
    return regressed


def report_missing(old: dict, new: dict, skip: set, old_title: str, new_title: str):
    """Rows in one side only - a renamed test, or a scope change."""
    for title, missing, source in (
        (old_title, sorted(set(old) - set(new) - set(skip)), old),
        (new_title, sorted(set(new) - set(old) - set(skip)), new),
    ):
        if not missing:
            continue
        print(f"### Only in {title}")
        print()
        for name in missing:
            print(f"- `{name}`: {source[name]} us")
        print()


def decompositions(old_path: str, new_path: str):
    """The gate rows from each report's raw capture, ({}, {}) if absent."""
    out = []
    for path in (old_path, new_path):
        raw = find_raw(path)
        out.append(parse_decomposition(raw) if raw else {})
    return out[0], out[1]


def print_scope_check(old_path: str, new_path: str):
    """The totals, and the warning that the two tables may not be comparable
    at all. Printed first, above the numbers, because it decides whether the
    numbers mean anything."""
    print("## Scope check")
    print()
    old_total, new_total = parse_total(old_path), parse_total(new_path)
    if old_total is None or new_total is None:
        print("One of the reports carries no `Total run time:` line, so the "
              "scope check could not run. Confirm by hand that both captures "
              "were taken the same way before reading a single row below.")
        print()
        return

    print(f"Total run time: {old_total[0]} -> {new_total[0]}.")
    if old_total[1] > 0:
        gap = abs(new_total[1] - old_total[1]) / old_total[1] * 100.0
        if gap > SCOPE_MISMATCH_PCT:
            print()
            print(f"**WARNING: the two runs differ by {gap:.0f}% of wall time.** "
                  "That is the signature of a perf-scoped capture compared "
                  "against a full one, and two differently-scoped images are "
                  "different flash layouts: the absolute microseconds below do "
                  "not compare, and no threshold makes them.")
    print()
    print("Even within one scope, absolute numbers compare only between "
          "captures of the same shape - a change that grows a hot function "
          "relocates everything after it and moves every row together. Use "
          "seeded performance comparison to distinguish layout from change.")
    print()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("old_report", help="Earlier performance_*.md")
    parser.add_argument("new_report", help="Later performance_*.md")
    parser.add_argument("--verdict", action="store_true",
                        help="Print one machine-readable line instead of the "
                             "tables, and exit 0 only if this is a win: at "
                             "least one row improved beyond the threshold "
                             "and none regressed beyond it. For the "
                             "optimisation loop, which cannot read a table.")
    parser.add_argument("--threshold", type=float, default=0.4,
                        help="percent below which a row is hidden, and above "
                             "which a regression fails the run (default 0.4)")
    args = parser.parse_args()

    old_report = parse_report(args.old_report)
    new_report = parse_report(args.new_report)

    if args.verdict:
        # A row is only counted when BOTH reports measured it, so a newly
        # added or newly failing-to-run row can never be read as a win.
        improved, regressed, best, worst = [], [], 0.0, 0.0
        for name, new_v in sorted(new_report.items()):
            if name not in old_report:
                continue
            d = pct_delta(old_report[name], new_v)
            if abs(d) <= args.threshold:
                continue
            # Timer quantisation can dominate percentage movement in tiny rows.
            if abs(new_v - old_report[name]) < MIN_ABS_DELTA_US:
                continue
            if d < 0:
                improved.append(name)
                best = min(best, d)
            else:
                regressed.append(name)
                worst = max(worst, d)
        # A regression beyond the threshold disqualifies the candidate outright,
        # however large the win elsewhere: this loop is not authorised to
        # trade one budget against another. A human decides that.
        win = bool(improved) and not regressed
        print(f"VERDICT {'WIN' if win else 'NO'} threshold={args.threshold:.1f}% "
              f"improved={len(improved)} regressed={len(regressed)} "
              f"best={best:.1f}% worst={worst:.1f}%")
        for name in improved:
            print(f"  improved {name} {pct_delta(old_report[name], new_report[name]):.1f}%")
        for name in regressed:
            print(f"  REGRESSED {name} {pct_delta(old_report[name], new_report[name]):.1f}%")
        return 0 if win else 1

    print(f"# Comparing `{args.old_report}` -> `{args.new_report}`")
    print()
    print_scope_check(args.old_report, args.new_report)
    print("## Timing rows")
    print()
    regressed = print_delta_table(old_report, new_report, args.threshold)
    report_missing(old_report, new_report, set(),
                   "the old report (removed or renamed)",
                   "the new report (added or renamed)")

    old_decomp, new_decomp = decompositions(args.old_report, args.new_report)
    if old_decomp and new_decomp:
        print("## Pass decomposition (from the raw captures)")
        print()
        print("Gate rows, read from the `*_raw.txt` beside each report. These "
              "are upper bounds from measure-by-deleting and need not sum, so "
              "they are not gated on below - a row moving here says where a "
              "step's time went, not that anything regressed.")
        print()
        print_delta_table(old_decomp, new_decomp, args.threshold)
        report_missing(old_decomp, new_decomp, set(),
                       "the old capture's splits",
                       "the new capture's splits")
    elif old_decomp or new_decomp:
        which = "old" if old_decomp else "new"
        print(f"> Only the {which} capture carries pass-decomposition rows, so "
              "there is nothing to diff. That is the normal state between "
              "rounds - the gates are scaffolding.")
        print()

    return 1 if regressed else 0


if __name__ == "__main__":
    sys.exit(main())
