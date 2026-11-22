"""Console stream framing and reply contracts shared by board and emulator."""
import re
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher/tools"))
from build.c_header import read_define

REPO = Path(__file__).resolve().parents[2]
TUNE_HEADER = REPO / "launcher/main/console/console_tune.h"
TUNE_SET = read_define(TUNE_HEADER, "CONSOLE_TUNE_SET")
TUNE_GET = read_define(TUNE_HEADER, "CONSOLE_TUNE_GET")
TUNE_RESET = read_define(TUNE_HEADER, "CONSOLE_TUNE_RESET")
TUNE_LIST = read_define(TUNE_HEADER, "CONSOLE_TUNE_LIST")
TUNE_OK = read_define(TUNE_HEADER, "CONSOLE_TUNE_OK")
TUNE_ERR = read_define(TUNE_HEADER, "CONSOLE_TUNE_ERR")
TUNE_END = read_define(TUNE_HEADER, "CONSOLE_TUNE_END")
TUNE_NAME_MAX = read_define(REPO / "launcher/main/core/tune.h", "TUNE_NAME_MAX")
TUNE_VALUE_MIN = -(2**31)
TUNE_VALUE_MAX = 2**31 - 1

# Both success and refusal terminate a framed reply, so callers must stop on either.
FRAMED_ENDINGS = ("_END", "_ERR")
TUNE_REPLY_ENDS = (TUNE_OK, TUNE_ERR, TUNE_END)
# Firmware logging can wrap replies; only its leading decoration may be discarded.
LOG_PREFIX = re.compile(r"^[IWEVD] \(\d+\) [^:]+:\s*")

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def parse_tunable(name: str, text: str) -> int:
    """Parse a console tunable assignment using the firmware's base-zero syntax."""
    assignment = f"{name}={text}"
    if len(name) > TUNE_NAME_MAX or not re.fullmatch(r"[a-zA-Z0-9_.]+", name):
        raise ValueError(f"{assignment!r}: expected a console name of at most {TUNE_NAME_MAX} characters")
    match = re.fullmatch(r"[+-]?(?:0[xX][0-9a-fA-F]+|(?P<octal>0[0-7]*)|[1-9][0-9]*)", text)
    if not match:
        raise ValueError(f"{assignment!r}: expected a decimal, octal or hexadecimal integer")
    value = int(text, 8 if match["octal"] else 0)
    if not TUNE_VALUE_MIN <= value <= TUNE_VALUE_MAX:
        raise ValueError(f"{assignment!r}: value must fit int32")
    return value


def parse_assignment(text: str) -> tuple[str, int]:
    """Read a NAME=VALUE assignment using the console's integer syntax."""
    name, separator, value = text.partition("=")
    if not separator:
        raise ValueError(f"{text!r}: expected NAME=VALUE")
    return name, parse_tunable(name, value)


def reply_prefix(line):
    """Derive the reply contract from a console line's verb."""
    words = line.split()
    if not words:
        raise ValueError("empty console line")
    prefix = words[0].upper()
    if prefix in (TUNE_SET, TUNE_GET, TUNE_RESET, TUNE_LIST):
        return TUNE_LIST, TUNE_REPLY_ENDS
    return prefix, tuple(prefix + ending for ending in FRAMED_ENDINGS)


def replies_to(data, reply, until):
    """Extract replies even when firmware logging prefixes their lines.
    Only newline-terminated lines count: a read can end mid-line. `reply`
    starts the line as a whole word (HEAP never claims HEAPMARK; HEAP_END and
    BUILD_ID= still answer) unless it ends in a separator (RUNSUITE_)."""
    found = []
    start = re.compile(re.escape(reply) + (r"(?![A-Za-z0-9])" if reply[-1:].isalnum() else ""))
    for raw in data.split(b"\n")[:-1]:
        text = LOG_PREFIX.sub("", ANSI.sub("", raw.decode("ascii", errors="replace"))).strip()
        if not start.match(text):
            continue
        found.append(text)
        if text.startswith(tuple(until)):
            return found, True
    return found, False


def require_development(data, line):
    if ("ignoring line: '" + line).encode("ascii") in data:
        raise RuntimeError("this command needs a development image")
