"""Unity result lines shared by device capture tools."""
import re

# Device results allow an empty file field and include ignored tests.
RESULT_RE = re.compile(r"^(?P<file>\S*?):(?P<line>\d+):(?P<name>\w+):(?P<status>PASS|FAIL|IGNORE)(?::\s*(?P<message>.*))?$")
BUILD_ID_RE = re.compile(r"BUILD_ID=([^\s\r\n]+)")
BUILD_ID_BYTES_RE = re.compile(BUILD_ID_RE.pattern.encode())
SELFTEST_COMPLETE_RE = re.compile(r"SELFTEST_COMPLETE(?:\s+failures=(\d+)\s+elapsed_ms=(\d+))?")


def results(text, *, ignored=False):
    """Strict Unity records in capture order, with optional ignored tests."""
    return [match.groupdict() for line in text.splitlines()
            if (match := RESULT_RE.match(line.strip())) and
            (ignored or match.group("status") != "IGNORE")]
