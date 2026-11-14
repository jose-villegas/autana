"""Console packets: binary payloads a board interleaves with its text lines.
The board's console_emit_packet() (main/console/console.c) writes each
packet as a head line and its bytes in one console write:

    <TAG>_BIN <bytes> <crc32 hex>\\n<bytes>

so no other output lands inside a packet. A packet can land inside another
task's line, though, when that line goes out in more than one write; the
line's two halves are joined around it. Standard library only: it runs
under ESP-IDF's Python.
"""

import zlib


class PacketError(ValueError):
    """A packet head that does not parse, or a packet that arrived damaged."""


class Packets:
    """Splits what a board writes into its text lines and the bytes its
    `tag`_BIN packets carry. feed() takes any chunk as it was read and
    returns ("line", bytes) and ("data", bytes) in arrival order; a line is
    returned once its newline has arrived, a packet once all its bytes have.
    A packet whose bytes do not match its crc32 raises PacketError."""

    def __init__(self, tag):
        self.prefix = tag.encode("ascii") + b"_BIN "
        self.pending = bytearray()
        self.partial = bytearray()  # a line's text before a packet that cut into it

    def feed(self, chunk):
        self.pending.extend(chunk)
        out = []
        while True:
            end = self.pending.find(b"\n")
            if end < 0:
                return out
            start = self.pending.find(self.prefix, 0, end)
            head = self.parse_head(start, end)
            if head is None:
                line = bytes(self.partial + self.pending[:end]).rstrip(b"\r")
                del self.pending[:end + 1]
                self.partial.clear()
                out.append(("line", line))
                continue
            count, crc = head
            if len(self.pending) < end + 1 + count:
                return out
            data = bytes(self.pending[end + 1:end + 1 + count])
            self.partial.extend(self.pending[:start])
            del self.pending[:end + 1 + count]
            if zlib.crc32(data) != crc:
                raise PacketError(f"a {count}-byte packet arrived damaged: crc32 {zlib.crc32(data):08x}, "
                                  f"the board sent {crc:08x}")
            out.append(("data", data))

    def parse_head(self, start, end):
        """(bytes, crc32) of the packet head at `start` in the line ending at
        `end`, None when there is none. A head opening the line must parse;
        later in a line, text that only looks like one stays text."""
        if start < 0:
            return None
        try:
            count, crc = bytes(self.pending[start + len(self.prefix):end]).rstrip(b"\r").split()
            return int(count), int(crc, 16)
        except ValueError as error:
            if start > 0 or self.partial:
                return None
            raise PacketError(f"not a packet line: {bytes(self.pending[:end])!r}") from error

    def rest(self):
        """What is left after the last whole line or packet: a line cut off at the end."""
        return bytes(self.partial + self.pending)
