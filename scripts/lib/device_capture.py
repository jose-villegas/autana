"""Unity result lines and crash signs shared by device capture tools."""
import re

# A line matching this is a Unity test result. Used both as proof that any
# test ran at all and, around a panic, to name the last few that did.
# Device results allow an empty file field and include ignored tests.
RESULT_RE = re.compile(r"^(?P<file>\S*?):(?P<line>\d+):(?P<name>\w+):(?P<status>PASS|FAIL|IGNORE)(?::\s*(?P<message>.*))?$")
# The console line that asks a running image for its BUILD_ID= line.
BUILD_ID_REQUEST = "BUILDID"
BUILD_ID_RE = re.compile(r"BUILD_ID=([^\s\r\n]+)")
APPS_RE = re.compile(r"APPS name=(.+?) running=([01])(?: console=(\S+))?")
IMU_QUARTER_RE = re.compile(r"IMU_OK quarter=(\d+) quarters=(\d+) sample_ms=(\d+) commit_ms=(\d+)")
BUILD_ID_BYTES_RE = re.compile(BUILD_ID_RE.pattern.encode())
PERF_SEGMENT = re.compile(r"perf: (\S+) cyc avg/min/max (\d+)/(\d+)/(\d+) (\S+) avg (\d+) n=(\d+)")
# The device prints this line only when the self-test loop actually reaches
# its end; absent means the run never finished, for any reason (timeout,
# device wedged, serial dropped). A capture of ONE suite triggered by
# RUNSUITE never prints it at all, which is what --no-complete is for.
SELFTEST_COMPLETE_RE = re.compile(r"SELFTEST_COMPLETE(?:\s+failures=(\d+)\s+elapsed_ms=(\d+))?")
SUITE_OVERFLOW_RE = re.compile(r"SUITE OVERFLOW:")
# Why a capture stopped: its completion line arrived, whatever its tests reported.
CAPTURE_COMPLETE = "complete"


def capture_ended_line(command, reason):
    """The line a capture command prints as it stops."""
    return f"{command} capture ended: {reason}"


# scripts/ci/board-steps.sh prints this once its console tests ran, whatever they reported.
CONSOLE_COMPLETE_LINE = "console tests ended: complete"


def board_steps_completed(lines):
    """Whether board-steps.sh ran its self-test capture and its console tests to the end, whatever
    their tests reported: failed tests are a result, a stage that never completed means the board
    did not run them."""
    ended = {line.strip() for line in lines}
    return {capture_ended_line("selftest", CAPTURE_COMPLETE), CONSOLE_COMPLETE_LINE} <= ended

# Each is a line the firmware prints as it dies: ESP-IDF's panic banner (the
# exception in brackets after "panic'ed"), abort()'s own line, and a failed
# assert(), which aborts after it. Searched, not anchored: a capture can carry
# a log prefix or colour codes in front.
CRASH_LINE_RE = re.compile(r"Guru Meditation|panic'ed|abort\(\) was called|assert failed:")
# Any line worth a look in a capture: an error, panic, abort or assert, or an
# ESP-IDF "E (tick)" log line.
ERROR_LINE_RE = re.compile(r"(?:\berror\b|\bpanic\b|\babort\b|\bassert\b|^E \(\d+\))", re.I)
# The ROM prints its banner and reset cause; firmware prints RESET_REASON.
# A USB reset gap can hide any of them, so count the most frequent form.
BOOT_BANNER = "ESP-ROM:esp32s3"
RESET_CAUSE_RE = re.compile(r"^rst:0x[0-9a-fA-F]+ \(\w+\).*$", re.M)
RESET_REASON_RE = re.compile(r"^RESET_REASON=\w+.*$", re.M)
RESET_LINE_RE = re.compile(r"(?:" + RESET_CAUSE_RE.pattern + "|" + re.escape(BOOT_BANNER)
                           + r"[^\r\n]*|" + RESET_REASON_RE.pattern + ")", re.M)
# The boots a capture may hold before one more is a reboot: its own, for a run
# that starts from a reset; none, for a capture opened on a running board.
BOOTS_FROM_RESET = 1
BOOTS_ON_RUNNING = 0


def reboot_sign(text, boots_allowed):
    """"rebooted: ..." when the capture holds more boots than `boots_allowed`,
    else None."""
    resets = RESET_CAUSE_RE.findall(text)
    boots = max(text.count(BOOT_BANNER), len(resets), len(RESET_REASON_RE.findall(text)))
    if boots <= boots_allowed:
        return None
    return "rebooted: %d boots, expected at most %d%s" % (
        boots, boots_allowed, " (last %s)" % resets[-1].strip() if resets else "")


def crash_signs(text, boots_allowed):
    """Why a capture shows the firmware dying, one line per sign: every crash
    line, then reboot_sign()'s. Empty when it shows none."""
    signs = ["crash: " + line.strip() for line in text.splitlines() if CRASH_LINE_RE.search(line)]
    reboot = reboot_sign(text, boots_allowed)
    return signs + [reboot] if reboot else signs


def suite_failure_signs(text, failed):
    """Dropped suites and sentinel failures beyond the FAIL result count."""
    signs = [line.strip() for line in text.splitlines() if SUITE_OVERFLOW_RE.search(line)]
    complete = SELFTEST_COMPLETE_RE.search(text)
    if complete and complete.group(1) is not None and int(complete.group(1)) > failed:
        signs.append(f"SELFTEST_COMPLETE failures={complete.group(1)}, but {failed} FAIL lines")
    return signs


def results(text, *, ignored=False):
    """Strict Unity records in capture order, with optional ignored tests."""
    return [match.groupdict() for line in text.splitlines()
            if (match := RESULT_RE.match(line.strip())) and
            (ignored or match.group("status") != "IGNORE")]


def parse_apps(lines):
    rows = []
    for line in lines:
        match = APPS_RE.fullmatch(line)
        if match:
            row = {"name": match[1], "running": match[2] == "1"}
            if match[3]:
                row["console"] = match[3]
            rows.append(row)
        elif line.startswith("APPS") and line != "APPS_END":
            raise ValueError("malformed APPS reply: " + line)
    if len({row["name"] for row in rows}) != len(rows):
        raise ValueError("duplicate APPS name")
    return rows
