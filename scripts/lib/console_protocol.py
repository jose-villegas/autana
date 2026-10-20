"""Console stream framing and reply contracts shared by board and emulator."""
import re

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def reply_prefix(line):
    """Derive the reply contract from a console line's verb."""
    words = line.split()
    if not words:
        raise ValueError("empty console line")
    prefix = words[0].upper()
    return prefix, (prefix + "_END", prefix + "_ERR")


def replies_to(data, reply, until):
    """Extract replies even when firmware logging prefixes their lines.
    Only newline-terminated lines count: a read can end mid-line."""
    found = []
    for raw in data.split(b"\n")[:-1]:
        at = raw.find(reply.encode("ascii"))
        if at < 0:
            continue
        text = raw[at:].decode("ascii", errors="replace").strip()
        found.append(text)
        if text.startswith(tuple(until)):
            return found, True
    return found, False


def require_development(data, line):
    if ("ignoring line: '" + line).encode("ascii") in data:
        raise RuntimeError("this command needs a development image")
