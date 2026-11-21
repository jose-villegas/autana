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
    A string tag keeps the "data" event contract; an iterable of tags returns
    the matching tag as the event kind. A CRC mismatch raises PacketError."""

    def __init__(self, tag):
        self.multiple = not isinstance(tag, str)
        self.prefixes = {name.encode("ascii") + b"_BIN ": name
                         for name in (tag if self.multiple else (tag,))}
        self.prefix = next(iter(self.prefixes))
        self.pending = bytearray()
        self.partial = bytearray()  # a line's text before a packet that cut into it

    def feed(self, chunk):
        self.pending.extend(chunk)
        out = []
        while True:
            end = self.pending.find(b"\n")
            if end < 0:
                return out
            matches = [(self.pending.find(prefix, 0, end), prefix) for prefix in self.prefixes]
            matches = [(at, prefix) for at, prefix in matches if at >= 0]
            start, self.prefix = min(matches) if matches else (-1, self.prefix)
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
                error = PacketError(f"a {count}-byte packet arrived damaged: crc32 {zlib.crc32(data):08x}, "
                                    f"the board sent {crc:08x}")
                error.packet = data
                raise error
            out.append((self.prefixes[self.prefix] if self.multiple else "data", data))

    def parse_head(self, start, end):
        """(bytes, crc32) of the packet head at `start` in the line ending at
        `end`, None when there is none. A head opening the line must parse;
        later in a line, text that only looks like one stays text."""
        if start < 0:
            return None
        try:
            count, crc = bytes(self.pending[start + len(self.prefix):end]).split()
            return int(count), int(crc, 16)
        except ValueError as error:
            if start > 0 or self.partial:
                return None
            raise PacketError(f"not a packet line: {bytes(self.pending[:end])!r}") from error

    def rest(self):
        """What is left after the last whole line or packet: a line cut off at the end."""
        return bytes(self.partial + self.pending)
