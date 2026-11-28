"""Read timing goals and ceilings emitted by the device perf budget helpers."""
import re

# Searched, not anchored: a real line carries the ESP-IDF log prefix
# ("I (32139) device_tests: PERF TARGET full-size step: ..."), and a target's
# name contains spaces.
PERF_TARGET_RE = re.compile(
    r"PERF TARGET (?!SUMMARY:)(?P<name>.+?): measured (?P<measured>\d+) us, "
    r"goal (?P<goal>\d+) us, distance (?P<distance>[+-]?\d+(?:\.\d+)?%)")
PERF_SUMMARY_RE = re.compile(r"PERF TARGET SUMMARY:\s*(?P<unmet>\d+)\s*unmet")
# A target whose goal is still zero (perf_goal in launcher/test/perf_budget.h).
PERF_UNPEGGED_RE = re.compile(r"PERF TARGET (?P<name>.+?): measured (?P<measured>\d+) us, goal not yet pegged")
PERF_CEILING_RE = re.compile(
    r"device_tests: (?P<name>.+?): \d+ us, (?:under its ceiling|over its ceiling|board ceiling) "
    r"(?P<ceiling>\d+) us")


def parse_perf_targets(text):
    targets = []
    summary_unmet = None
    for line in text.splitlines():
        m = PERF_SUMMARY_RE.search(line)
        if m:
            summary_unmet = int(m.group("unmet"))
            continue
        m = PERF_TARGET_RE.search(line)
        if m:
            targets.append(m.groupdict())
    return targets, summary_unmet


def parse_unpegged_targets(text):
    """Targets measured with no goal yet, as {name, measured}."""
    return [m.groupdict() for m in PERF_UNPEGGED_RE.finditer(text)]


def parse_perf_ceilings(text):
    """Timing guard ceilings, including board failures and QEMU's unenforced limits."""
    return [match.groupdict() for match in PERF_CEILING_RE.finditer(text)]
