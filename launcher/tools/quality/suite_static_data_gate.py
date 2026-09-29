#!/usr/bin/env python3
"""Reject diagnostics suite objects with static writable data above the cap."""

import csv
import re
import sys


# This leaves room for suite-local state without reserving a working buffer at boot.
SUITE_STATIC_DATA_LIMIT = 1536
SUITE_OBJECT = re.compile(r"(?:^|[/\\])suite_.*\.c\.obj$")


def section_size(row, suffix):
    return sum(int(value or 0) for key, value in row.items() if key.lower().endswith(suffix))


def main():
    rows = csv.DictReader(sys.stdin)
    offenders = []
    suite_rows = 0
    for row in rows:
        name = next((value for key, value in row.items() if key.lower() in {"object file", "file"}), "")
        if not SUITE_OBJECT.search(name):
            continue
        suite_rows += 1
        total = section_size(row, ".bss") + section_size(row, ".data")
        if total > SUITE_STATIC_DATA_LIMIT:
            offenders.append((name, total))

    if suite_rows == 0:
        print("FAIL suite static-data gate found no suite object rows")
        return 1
    if offenders:
        for name, total in sorted(offenders, key=lambda item: item[1], reverse=True):
            print(f"FAIL {name}: {total} bytes of .bss + .data (limit {SUITE_STATIC_DATA_LIMIT})")
        return 1
    print(f"Suite static-data gate: every suite object is at or below {SUITE_STATIC_DATA_LIMIT} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
