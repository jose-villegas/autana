"""Console stream framing and reply contracts shared by board and emulator."""
import re

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def replies_to(data, reply, until):
    """Extract replies even when firmware logging prefixes their lines."""
    found = []
    for raw in data.split(b"\n"):
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
