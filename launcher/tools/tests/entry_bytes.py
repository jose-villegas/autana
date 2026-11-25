"""Byte-level helpers for the asset entry tests: values as an entry stores them, and an entry with one field rewritten."""
import struct


def single(value):
    """`value` rounded to the float32 an entry stores."""
    return struct.unpack("<f", struct.pack("<f", value))[0]


def edited(entry, offset, fmt, value):
    """A copy of `entry` with `value` packed as `fmt` at `offset`."""
    out = bytearray(entry)
    struct.pack_into(fmt, out, offset, value)
    return bytes(out)
