"""Console stream framing and reply contracts shared by board and emulator."""
import re

# Both success and refusal terminate a framed reply, so callers must stop on either.
FRAMED_ENDINGS = ("_END", "_ERR")
# Firmware logging can wrap replies; only its leading decoration may be discarded.
LOG_PREFIX = re.compile(r"^[IWEVD] \(\d+\) [^:]+:\s*")

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def reply_prefix(line):
    """Derive the reply contract from a console line's verb."""
    words = line.split()
    if not words:
        raise ValueError("empty console line")
    prefix = words[0].upper()
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
