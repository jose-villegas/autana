"""Console stream framing and reply contracts shared by board and emulator."""
import re
import time

ANSI = re.compile(r"\x1b\[[0-9;]*m")


class LineReader:
    """Retain unread lines and log chunks without mixing host and device text."""
    def __init__(self, read_chunk, log, deadline, alive=lambda: True):
        self.read_chunk = read_chunk
        self.log = log
        self.deadline = deadline
        self.alive = alive
        self.pending = b""
        self.boundary = True
        self.messages = []

    def emit(self, line=None):
        if line is not None:
            self.messages.append(line)
        if self.boundary:
            for message in self.messages:
                self.log.write((message + "\n").encode("utf-8"))
            self.log.flush()
            self.messages.clear()

    def finish(self):
        if not self.boundary:
            self.log.write(b"\n")
            self.boundary = True
        self.emit()

    def lines(self, seconds=None):
        deadline = self.deadline
        if seconds is not None:
            deadline = min(deadline, time.monotonic() + seconds)
        while time.monotonic() < deadline and self.alive():
            if b"\n" in self.pending:
                raw, self.pending = self.pending.split(b"\n", 1)
                yield ANSI.sub("", raw.decode("utf-8", errors="replace").rstrip("\r"))
                continue
            chunk = self.read_chunk()
            if chunk is None:
                continue
            if not chunk:
                return
            self.log.write(chunk)
            self.log.flush()
            self.pending += chunk
            self.boundary = chunk.endswith(b"\n")
            if self.boundary and self.messages:
                self.emit()


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
