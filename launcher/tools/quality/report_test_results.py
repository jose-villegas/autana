#!/usr/bin/env python3
"""Turns a raw device self-test capture (see scripts/device/device.py's own
`selftest`) into a markdown report: a summary line, every failure with its
assertion message, and the full pass/fail list tucked into a collapsible
section so the failures are what a reader actually sees first.

Usage:
    python tools/quality/report_test_results.py <raw_capture.txt> <out.md>

Exit 0 = every test passed, 1 = the report records a failing test (a result
to read, not an error), 2 = the capture has nothing in it to report on.
"""
import argparse
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/device"))
from device_report import results, selftest_markdown


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_path", help="Raw capture from scripts/device/device.py selftest")
    parser.add_argument("out_path", help="Markdown file to write")
    args = parser.parse_args()
    with open(args.capture_path, "r", errors="replace") as f:
        text = f.read()
    parsed = results(text, selftest=True)
    if not parsed:
        print(f"{args.capture_path} contains no test results - nothing ran, so "
              "there is no report to write.", file=sys.stderr)
        return 2
    markdown, passed, failed = selftest_markdown(text, args.capture_path)
    with open(args.out_path, "w", encoding="utf-8") as f:
        f.write(markdown)
    print(f"{len(parsed)} tests, {passed} passed, {failed} failed -> {args.out_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
