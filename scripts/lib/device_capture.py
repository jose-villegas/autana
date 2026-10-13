"""Unity result lines and crash signs shared by device capture tools."""
import re

# A line matching this is a Unity test result. Used both as proof that any
# test ran at all and, around a panic, to name the last few that did.
# Device results allow an empty file field and include ignored tests.
RESULT_RE = re.compile(r"^(?P<file>\S*?):(?P<line>\d+):(?P<name>\w+):(?P<status>PASS|FAIL|IGNORE)(?::\s*(?P<message>.*))?$")
# The console line that asks a running image for its BUILD_ID= line.
BUILD_ID_REQUEST = "BUILDID"
BUILD_ID_RE = re.compile(r"BUILD_ID=([^\s\r\n]+)")
BUILD_ID_BYTES_RE = re.compile(BUILD_ID_RE.pattern.encode())
PERF_SEGMENT = re.compile(r"perf: (\S+) cyc avg/min/max (\d+)/(\d+)/(\d+) (\S+) avg (\d+) n=(\d+)")
# The device prints this line only when the self-test loop actually reaches
# its end; absent means the run never finished, for any reason (timeout,
# device wedged, serial dropped). A capture of ONE suite triggered by
# RUNSUITE never prints it at all, which is what --no-complete is for.
SELFTEST_COMPLETE_RE = re.compile(r"SELFTEST_COMPLETE(?:\s+failures=(\d+)\s+elapsed_ms=(\d+))?")

# Each is a line the firmware prints as it dies: ESP-IDF's panic banner (the
# exception in brackets after "panic'ed"), abort()'s own line, and a failed
# assert(), which aborts after it. Searched, not anchored: a capture can carry
# a log prefix or colour codes in front.
CRASH_LINE_RE = re.compile(r"Guru Meditation|panic'ed|abort\(\) was called|assert failed:")
# The ROM's first two lines, once per boot: "ESP-ROM:esp32s3-20210327", then
# the reset cause, "rst:0xc (RTC_SW_CPU_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)".
# Either can be lost in a USB reset gap, so a boot is counted by whichever
# shows more often.
BOOT_BANNER = "ESP-ROM:esp32s3"
RESET_LINE_RE = re.compile(r"^rst:0x[0-9a-fA-F]+ \(\w+\).*$", re.M)
# The boots a capture may hold before one more is a reboot: its own, for a run
# that starts from a reset; none, for a capture opened on a running board.
BOOTS_FROM_RESET = 1
BOOTS_ON_RUNNING = 0


def reboot_sign(text, boots_allowed):
    """"rebooted: ..." when the capture holds more boots than `boots_allowed`,
    else None."""
    resets = RESET_LINE_RE.findall(text)
    boots = max(text.count(BOOT_BANNER), len(resets))
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


def results(text, *, ignored=False):
    """Strict Unity records in capture order, with optional ignored tests."""
    return [match.groupdict() for line in text.splitlines()
            if (match := RESULT_RE.match(line.strip())) and
            (ignored or match.group("status") != "IGNORE")]
