"""An asset entry with one field rewritten, for the tests of what a reader refuses."""
import struct


def edited(entry, offset, fmt, value):
    """A copy of `entry` with `value` packed as `fmt` at `offset`."""
    out = bytearray(entry)
    struct.pack_into(fmt, out, offset, value)
    return bytes(out)
