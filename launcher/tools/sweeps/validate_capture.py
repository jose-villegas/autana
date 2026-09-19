#!/usr/bin/env python3
"""Answers exactly one question about a raw device capture: is it worth
reading at all?

A capture can look plausible and measure nothing - a timeout with no
SELFTEST_COMPLETE, a crash loop, or (worst, because it produces a clean-
looking report) an image where the suites never actually ran and the device
just sat in the launcher printing its idle frame rate. Each of those burns
a full build+flash+capture cycle before anyone notices. This tool runs
straight after the capture step and before any reporter, so a worthless
capture is rejected with a specific reason instead of turning into a
plausible-looking table.

What counts as "measured something" differs per report, so the caller
declares it with --sentinel rather than this tool knowing any suite's log
lines. Everything else here is true of any capture.

Deliberately read-only: it never modifies, filters or deletes the raw
capture. Stripping a diagnostic before a human reads it is exactly how the
one explaining line goes missing - this only reports what it sees.

Usage:
    python validate_capture.py <raw_capture.txt> [--sentinel TEXT]...
    python validate_capture.py <raw_capture.txt> --no-complete --sentinel TEXT

Exit 0 = valid, non-zero = invalid (one or more checks below failed).
"""
import argparse
import re
import sys

# The device prints this line only when the self-test loop actually reaches
# its end - absent means the run never finished, for any reason (timeout,
# device wedged, serial dropped). A capture of ONE suite triggered by
# RUNSUITE never prints it at all, which is what --no-complete is for.
SELFTEST_COMPLETE_RE = re.compile(r"SELFTEST_COMPLETE(?:\s+failures=(\d+)\s+elapsed_ms=(\d+))?")

# Both phrases appear on ESP-IDF's panic banner; either is sufficient to
# call it a crash. The parenthesised text after "panic'ed" is the exception
# type (e.g. "Stack protection fault") and is worth surfacing verbatim -
# it is usually enough on its own to point at the offending change.
PANIC_LINE_RE = re.compile(r"Guru Meditation|panic'ed")
PANIC_TYPE_RE = re.compile(r"panic'ed\s*\(([^)]+)\)")

# One per boot. More than one means the device reset mid-run - a crash
# loop, not a slow run - which changes what a stall in the capture means.
BOOT_BANNER = "ESP-ROM:esp32s3"

# A line matching this is a Unity test result. Used both as proof that any
# test ran at all and, around a panic, to name the last few that did.
RESULT_RE = re.compile(r"^\S*:\d+:(?P<name>\w+):(?P<status>PASS|FAIL)")

# Not fatal by itself, but its presence means an old diag image: the
# current build disables the task watchdog on purpose, because historically
# a watchdog dump landed inside a timed frame-budget window and inflated
# that one row's measurement by up to 2.6x. A stale image silently
# reintroduces that noise.
TASK_WDT_MARKER = "task_wdt"

CONTEXT_LINES = 5  # how many prior test results to show before a panic


def _panic_context(lines, panic_index):
    """Last few Unity result lines before the first panic - the crashing
    test is usually the very last PASS before the dump starts."""
    context = []
    for line in reversed(lines[:panic_index]):
        if RESULT_RE.match(line.strip()):
            context.append(line.rstrip("\n"))
            if len(context) >= CONTEXT_LINES:
                break
    return list(reversed(context))


def validate(capture_path: str, sentinels=(), require_complete: bool = True):
    """Returns (failures, warnings) - both lists of message strings.
    Empty failures means the capture is valid."""
    with open(capture_path, "r", errors="replace") as f:
        text = f.read()
    lines = text.splitlines()

    failures = []
    warnings = []

    complete = SELFTEST_COMPLETE_RE.search(text)
    if require_complete and not complete:
        failures.append(
            "SELFTEST_COMPLETE not found - the run never finished: either it "
            "timed out or the device stopped talking mid-capture."
        )

    panic_index = None
    panic_type = None
    for i, line in enumerate(lines):
        if PANIC_LINE_RE.search(line):
            panic_index = i
            tm = PANIC_TYPE_RE.search(line)
            panic_type = tm.group(1) if tm else line.strip()
            break
    if panic_index is not None:
        msg = f"panic detected: {panic_type}"
        context = _panic_context(lines, panic_index)
        if context:
            msg += "\n    last tests that PASSED before the panic - the crashing test is the NEXT one registered after these, it died before printing a result:\n      " + \
                "\n      ".join(context)
        failures.append(msg)

    boot_count = text.count(BOOT_BANNER)
    if boot_count == 0:
        failures.append(
            f"no boot banner ({BOOT_BANNER!r}) found - this doesn't look like "
            "a capture that started from a device reset at all."
        )
    elif boot_count > 1:
        failures.append(
            f"{boot_count} boot banners found, expected 1 - the device "
            "rebooted mid-run. That means a crash loop, not a slow run."
        )

    # A capture with no result line in it ran no test, whatever else it
    # contains. Every reporter here turns such a capture into a report that
    # reads like a clean run of nothing, so it is rejected before one is
    # written. Only checked for a whole-run capture: a RUNSUITE window can
    # legitimately close before its suite prints a result, and its sentinel
    # is the proof that it ran.
    if require_complete and not any(RESULT_RE.match(line.strip()) for line in lines):
        failures.append(
            "no test result lines found - nothing ran. The flashed image "
            "either had the suites compiled in but not running (autorun off), "
            "or was the wrong image entirely."
        )

    # What each sentinel means depends entirely on whether the suite ran at
    # all, so the two cases are reported differently - reading them as one
    # thing produced a confidently wrong diagnosis on this tool's first real
    # use, blaming the image when the suite had in fact run and a fixture had
    # simply failed to allocate.
    for sentinel in sentinels:
        if sentinel in text:
            continue
        if not complete:
            failures.append(
                f"measurement sentinel {sentinel!r} not found, and the run "
                "never completed - the flashed image either had the suites "
                "compiled in but not running, or was the wrong image "
                "entirely, or the capture window closed first. Nothing in "
                "this capture was measured."
            )
        else:
            failures.append(
                f"measurement sentinel {sentinel!r} not found, but the suite "
                "DID run to completion - so the image is fine and the tests "
                "themselves failed before logging a measurement. The usual "
                "cause is a fixture that could not allocate: check free heap "
                "in this capture against the ~41 KB one grid needs. There are "
                "no timings in this capture to read."
            )

    wdt_count = sum(1 for line in lines if TASK_WDT_MARKER in line)
    if wdt_count:
        warnings.append(
            f"{wdt_count} line(s) mention {TASK_WDT_MARKER!r} - the current "
            "diag image disables the task watchdog on purpose, so this looks "
            "like an old image. Historically a watchdog dump landed inside a "
            "timed window and inflated that row's measurement up to 2.6x - "
            "treat any single-row spike in this capture with suspicion."
        )

    return failures, warnings


def report(capture_path: str, sentinels=(), require_complete: bool = True) -> bool:
    failures, warnings = validate(capture_path, sentinels, require_complete)
    valid = not failures
    print(f"{'VALID' if valid else 'INVALID'}: {capture_path}")
    for msg in failures:
        print(f"  [FAIL] {msg}")
    for msg in warnings:
        print(f"  [WARN] {msg}")
    if valid:
        with open(capture_path, "r", errors="replace") as f:
            m = SELFTEST_COMPLETE_RE.search(f.read())
        if m and m.group(1) is not None:
            print(f"  SELFTEST_COMPLETE failures={m.group(1)} elapsed_ms={m.group(2)}")
    return valid


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture_path", help="Raw capture from a capture script")
    parser.add_argument(
        "--sentinel", action="append", default=[], metavar="TEXT",
        help="A line the capture must contain to count as having measured "
             "something (repeatable). The caller declares it; this tool "
             "knows no suite's log lines.",
    )
    parser.add_argument(
        "--no-complete", action="store_true",
        help="Do not require SELFTEST_COMPLETE - a capture of one suite "
             "triggered by RUNSUITE never prints it.",
    )
    args = parser.parse_args()

    return 0 if report(args.capture_path, args.sentinel, not args.no_complete) else 1


if __name__ == "__main__":
    sys.exit(main())
