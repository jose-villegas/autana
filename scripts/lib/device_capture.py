"""Unity result lines shared by device capture tools."""
import re

# A line matching this is a Unity test result. Used both as proof that any
# test ran at all and, around a panic, to name the last few that did.
# Device results allow an empty file field and include ignored tests.
RESULT_RE = re.compile(r"^(?P<file>\S*?):(?P<line>\d+):(?P<name>\w+):(?P<status>PASS|FAIL|IGNORE)(?::\s*(?P<message>.*))?$")
BUILD_ID_RE = re.compile(r"BUILD_ID=([^\s\r\n]+)")
BUILD_ID_BYTES_RE = re.compile(BUILD_ID_RE.pattern.encode())
# The device prints this line only when the self-test loop actually reaches
# its end; absent means the run never finished, for any reason (timeout,
# device wedged, serial dropped). A capture of ONE suite triggered by
# RUNSUITE never prints it at all, which is what --no-complete is for.
SELFTEST_COMPLETE_RE = re.compile(r"SELFTEST_COMPLETE(?:\s+failures=(\d+)\s+elapsed_ms=(\d+))?")


def results(text, *, ignored=False):
    """Strict Unity records in capture order, with optional ignored tests."""
    return [match.groupdict() for line in text.splitlines()
            if (match := RESULT_RE.match(line.strip())) and
            (ignored or match.group("status") != "IGNORE")]
