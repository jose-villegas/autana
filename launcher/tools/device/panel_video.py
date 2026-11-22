"""Panel capture reassembly, RGB canvas, constant-rate video and timing CSV."""
import csv
import re
import struct
import sys
import time
from pathlib import Path

import gfx_color
import screenshot
import video_encode
from build.c_header import read_define, read_defines

HEADER = Path(__file__).resolve().parents[2] / "main/gfx/present/capture/panelcap.h"
CONSOLE_HEADER = Path(__file__).resolve().parents[2] / "main/app/console/console_panelcap_device.h"
PACKET_TAG = read_define(CONSOLE_HEADER, "PANELCAP_PACKET_TAG")
ERROR_PREFIX = read_define(CONSOLE_HEADER, "PANELCAP_ERROR_PREFIX")
PART = struct.Struct("<IHB")
PRESENT = struct.Struct("<IQHH")
WINDOW = struct.Struct("<HHHH")
assert (PART.size, PRESENT.size, WINDOW.size) == tuple(read_defines(
    HEADER, "PANELCAP_", ("PART_HEAD_BYTES", "HEAD_BYTES", "WINDOW_BYTES")))
assert read_defines(HEADER, "PANELCAP_PART_AT_", ("SEQ", "INDEX", "LAST")) == [0, 4, 6]
assert read_defines(HEADER, "PANELCAP_HEAD_AT_", ("SEQ", "US", "WINDOWS", "DROPPED")) == [0, 4, 12, 14]
assert read_defines(HEADER, "PANELCAP_WINDOW_AT_", ("X0", "Y0", "X1", "Y1")) == [0, 2, 4, 6]
PIXEL_BYTES = read_define(HEADER, "PANELCAP_PIXEL_BYTES")
RGB_BYTES = 3
OUTPUT_FPS = 30
US_PER_SECOND = 1_000_000
WINDOWS_MAX = read_define(HEADER, "PANELCAP_WINDOWS_MAX")
PACK_NOOP = read_define(HEADER, "PANELCAP_PACK_RUN_MAX")
PACK_LITERAL_MAX = PACK_NOOP - 1
PACK_CONTROL_BYTES = 1
# Merged rectangles are disjoint, so each panel pixel needs at most one PackBits header.
STREAM_BYTES_MAX = (screenshot.PANEL_WIDTH * screenshot.PANEL_HEIGHT * (PIXEL_BYTES + PACK_CONTROL_BYTES)
                    + WINDOWS_MAX * WINDOW.size + PRESENT.size)
CSV_FIELDS = ("present_seq", "device_us", "host_ns", "windows", "packed_bytes", "dropped_before")


def unpack(data, at, pixels):
    """Decode one PackBits window and return its pixels and the next byte offset."""
    out = bytearray()
    size_out = pixels * PIXEL_BYTES
    while len(out) < size_out and at < len(data):
        head = data[at]
        at += 1
        if head == PACK_NOOP:
            continue
        literal = head <= PACK_LITERAL_MAX
        count = head + 1 if literal else 2 * PACK_NOOP + 1 - head
        size = count * PIXEL_BYTES if literal else PIXEL_BYTES
        if at + size > len(data) or len(out) + count * PIXEL_BYTES > size_out:
            raise ValueError("invalid panel PackBits length")
        out.extend(data[at:at + size] if literal else data[at:at + size] * count)
        at += size
    if len(out) != size_out:
        raise ValueError("short panel PackBits window")
    return bytes(out), at


class Parts:
    """Reject a whole present on a lost, duplicated, or out-of-order part."""

    def __init__(self):
        self.seq = None
        self.part = 0
        self.data = bytearray()
        self.dropped = 0
        self.rejected = None

    def abandon(self):
        if self.seq is not None:
            self.dropped += 1
            self.rejected = self.seq
        self.seq = None
        self.data.clear()

    def corrupt(self, packet):
        """Discard one present after a packet CRC failure, retaining its sequence."""
        if packet is not None and len(packet) >= PART.size:
            seq, _, _ = PART.unpack_from(packet)
            if seq == self.rejected:
                return
            if self.seq is not None and self.seq != seq:
                self.abandon()
            if self.seq == seq:
                self.abandon()
            else:
                self.dropped += 1
                self.rejected = seq
        elif self.seq is None:
            self.dropped += 1
        else:
            self.abandon()

    def feed(self, packet):
        if len(packet) < PART.size:
            self.corrupt(packet)
            return None
        seq, part, last = PART.unpack_from(packet)
        if seq == self.rejected:
            return None
        if self.seq != seq:
            self.abandon()
            if part != 0:
                self.rejected = seq
                self.dropped += 1
                return None
            self.seq, self.part = seq, 0
        if part != self.part or last not in (0, 1) or len(self.data) + len(packet) - PART.size > STREAM_BYTES_MAX:
            self.abandon()
            return None
        self.data.extend(packet[PART.size:])
        self.part += 1
        if not last:
            return None
        data = bytes(self.data)
        self.seq = None
        self.data.clear()
        if len(data) < PRESENT.size or PRESENT.unpack_from(data)[0] != seq:
            self.rejected = seq
            self.dropped += 1
            return None
        return data


class Canvas:
    def __init__(self, width=screenshot.PANEL_WIDTH, height=screenshot.PANEL_HEIGHT):
        self.width, self.height = width, height
        self.rows = [bytearray(width * RGB_BYTES) for _ in range(height)]

    def apply(self, stream, before_update=None):
        seq, us, windows, dropped = PRESENT.unpack_from(stream)
        if windows > WINDOWS_MAX:
            raise ValueError("too many panel windows")
        at, packed, updates = PRESENT.size, 0, []
        for _ in range(windows):
            x0, y0, x1, y1 = WINDOW.unpack_from(stream, at)
            at += WINDOW.size
            if not (0 <= x0 < x1 <= self.width and 0 <= y0 < y1 <= self.height):
                raise ValueError("invalid panel window")
            pixels, end = unpack(stream, at, (x1 - x0) * (y1 - y0))
            packed += end - at
            at = end
            rgb = bytearray()
            for code, in struct.iter_unpack(">H", pixels):
                rgb.extend(gfx_color.expand(code >> 11, (code >> 5) & 63, code & 31))
            updates.append((x0, y0, x1, y1, rgb))
        if at != len(stream):
            raise ValueError("trailing panel bytes")
        # Validate every window before changing the persistent canvas.
        if before_update is not None:
            before_update(seq, us)
        for x0, y0, x1, y1, rgb in updates:
            stride = (x1 - x0) * RGB_BYTES
            for row in range(y1 - y0):
                self.rows[y0 + row][x0 * RGB_BYTES:x1 * RGB_BYTES] = rgb[row * stride:(row + 1) * stride]
        return seq, us, windows, packed, dropped

    def frame(self):
        return b"".join(screenshot.turn_rgb_rows(self.rows, self.width, self.height))


class Resample:
    """Emit the latest present at each rational 30 Hz device timestamp."""

    def __init__(self, snapshot, emit):
        self.snapshot, self.emit = snapshot, emit
        self.first = self.last = self.seq = None
        self.k = 0

    def emit_until(self, end):
        if self.k * US_PER_SECOND < end:
            frame = self.snapshot()
            while self.k * US_PER_SECOND < end:
                self.emit(frame)
                self.k += 1

    def present(self, seq, us):
        if self.last is not None and us < self.last:
            raise ValueError("panel device time went backwards")
        if self.seq is not None and seq <= self.seq:
            raise ValueError("panel present sequence did not advance")
        if self.first is None:
            self.first = us
        self.emit_until((us - self.first) * OUTPUT_FPS)
        self.seq, self.last = seq, us

    def finish(self):
        if self.last is not None:
            self.emit_until((self.last - self.first) * OUTPUT_FPS + 1)


class Recording:
    def __init__(self, path, log=None):
        self.log = sys.stderr if log is None else log
        self.path = Path(path)
        self.parts = Parts()
        self.canvas = Canvas()
        self.writer = video_encode.VideoWriter(self.path, OUTPUT_FPS, live=True)
        self.resample = Resample(self.canvas.frame, self.write)
        self.received = self.packed = 0
        self.started = None

    def __enter__(self):
        self.file = self.path.with_suffix(".frames.csv").open("w", newline="")
        self.csv = csv.writer(self.file)
        self.csv.writerow(CSV_FIELDS)
        return self

    def write(self, frame):
        self.writer.write((self.canvas.height, self.canvas.width), frame)

    def packet(self, data):
        stream = self.parts.feed(data)
        if stream is None:
            return
        host_ns, now = time.time_ns(), time.monotonic()
        try:
            seq, us, windows, packed, dropped = self.canvas.apply(stream, self.resample.present)
        except (ValueError, struct.error):
            self.parts.dropped += 1
            return
        self.csv.writerow((seq, us, host_ns, windows, packed, dropped))
        self.file.flush()
        self.received += 1
        self.packed += packed
        self.started = now if self.started is None else self.started

    def finish(self, answer):
        self.parts.abandon()
        if answer.startswith(("PANELCAP_REFUSED", ERROR_PREFIX)):
            return
        if self.received == 0:
            raise RuntimeError("no complete PANEL presents received")
        now = time.monotonic()
        self.resample.finish()
        elapsed = max(now - (self.started or now), 1 / OUTPUT_FPS)
        match = re.search(r"dropped=(\d+)", answer)
        dropped = match[1] if match else "unknown"
        print(f"video: presents={self.received} fps={self.received / elapsed:.2f} "
              f"device_dropped={dropped} host_dropped={self.parts.dropped} MB/s={self.packed / elapsed / US_PER_SECOND:.3f}",
              file=self.log)

    def __exit__(self, kind, value, trace):
        try:
            self.writer.close()
        finally:
            self.file.close()
