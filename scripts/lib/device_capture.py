"""Unity result lines shared by device capture tools."""
import re

# Device results allow an empty file field and include ignored tests.
RESULT_RE = re.compile(r"^(?P<file>\S*?):(?P<line>\d+):(?P<name>\w+):(?P<status>PASS|FAIL|IGNORE)(?::\s*(?P<message>.*))?$")

BUILD_ID_RE = re.compile(r"BUILD_ID=([^\s\r\n]+)")
BUILD_ID_BYTES_RE = re.compile(BUILD_ID_RE.pattern.encode())
SELFTEST_RESULT_RE = re.compile(RESULT_RE.pattern.replace(r"\S*?", r"\S*").replace("PASS|FAIL|IGNORE", "PASS|FAIL"))
RESULT_PREFIX_RE = re.compile(SELFTEST_RESULT_RE.pattern[:SELFTEST_RESULT_RE.pattern.index('(?::')])
SUITE_RESULT_RE = re.compile(rb":\d+:.*:(PASS|FAIL)(?:\r?$|:)", re.MULTILINE)
QEMU_PASS_RE = re.compile(r":PASS$", re.M)
QEMU_IGNORE_RE = re.compile(r":IGNORE")
QEMU_FAIL_RE = re.compile(r"^\S*:\d+:(\w+):FAIL:? ?(.*)$", re.M)


def results(text, *, ignored=True, selftest=False):
    """Strict Unity records in capture order, with optional ignored-test filtering."""
    regex = SELFTEST_RESULT_RE if selftest else RESULT_RE
    return [match.groupdict() for line in text.splitlines()
            if (match := regex.match(line.strip())) and
            (ignored or match.group("status") != "IGNORE")]
