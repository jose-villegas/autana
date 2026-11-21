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

PART = struct.Struct("<IHB")
PRESENT = struct.Struct("<IQHH")
WINDOW = struct.Struct("<HHHHI")
PIXEL_BYTES = 2
RGB_BYTES = 3
OUTPUT_FPS = 30
US_PER_SECOND = 1_000_000
WINDOWS_MAX = 512
STREAM_BYTES_MAX = 2 * screenshot.PANEL_WIDTH * screenshot.PANEL_HEIGHT * (PIXEL_BYTES + 1) + WINDOWS_MAX * WINDOW.size + PRESENT.size
CSV_FIELDS = ("present_seq", "device_us", "host_ns", "windows", "packed_bytes", "dropped_before")


def unpack(data, pixels):
    """Decode Apple PackBits over two-byte pixels, enforcing the expected length."""
    out = bytearray()
    at = 0
    while at < len(data):
        head = data[at]
        at += 1
        if head == 128:
            continue
        count = head + 1 if head < 128 else 257 - head
        size = count * PIXEL_BYTES if head < 128 else PIXEL_BYTES
        if at + size > len(data) or len(out) + count * PIXEL_BYTES > pixels * PIXEL_BYTES:
            raise ValueError("invalid panel PackBits length")
        out.extend(data[at:at + size] if head < 128 else data[at:at + size] * count)
        at += size
    if len(out) != pixels * PIXEL_BYTES:
        raise ValueError("short panel PackBits window")
    return bytes(out)


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

    def feed(self, packet):
        if len(packet) < PART.size:
            self.abandon()
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
            self.dropped += 1
            return None
        return data


class Canvas:
    def __init__(self, width=screenshot.PANEL_WIDTH, height=screenshot.PANEL_HEIGHT):
        self.width, self.height = width, height
        self.rows = [bytearray(width * RGB_BYTES) for _ in range(height)]

    def apply(self, stream):
        seq, us, windows, dropped = PRESENT.unpack_from(stream)
        if windows > WINDOWS_MAX:
            raise ValueError("too many panel windows")
        at, packed, updates = PRESENT.size, 0, []
        for _ in range(windows):
            x0, y0, x1, y1, size = WINDOW.unpack_from(stream, at)
            at += WINDOW.size
            if not (0 <= x0 < x1 <= self.width and 0 <= y0 < y1 <= self.height) or at + size > len(stream):
                raise ValueError("invalid panel window")
            pixels = unpack(stream[at:at + size], (x1 - x0) * (y1 - y0))
            at += size
            packed += size
            rgb = bytearray()
            for code, in struct.iter_unpack(">H", pixels):
                rgb.extend(gfx_color.expand(code >> 11, (code >> 5) & 63, code & 31))
            updates.append((x0, y0, x1, y1, rgb))
        if at != len(stream):
            raise ValueError("trailing panel bytes")
        # Validate every window before changing the persistent canvas.
        for x0, y0, x1, y1, rgb in updates:
            stride = (x1 - x0) * RGB_BYTES
            for row in range(y1 - y0):
                self.rows[y0 + row][x0 * RGB_BYTES:x1 * RGB_BYTES] = rgb[row * stride:(row + 1) * stride]
        return seq, us, windows, packed, dropped

    def frame(self):
        return b"".join(screenshot.turn_rgb_rows(self.rows, self.width, self.height))


class Resample:
    """Emit the latest present at each rational 30 Hz device timestamp."""

    def __init__(self, emit):
        self.emit = emit
        self.first = None
        self.last = None
        self.frame = None
        self.k = 0

    def before(self, us):
        if self.first is not None:
            while self.k * US_PER_SECOND < (us - self.first) * OUTPUT_FPS:
                self.emit(self.frame)
                self.k += 1

    def present(self, us, frame):
        if self.last is not None and us < self.last:
            raise ValueError("panel device time went backwards")
        self.before(us)
        if self.first is None:
            self.first = us
        self.last, self.frame = us, frame

    def finish(self, us=None):
        if self.first is not None:
            self.before(max(self.last + 1, us or self.last + 1))


class Recording:
    def __init__(self, path, log=None, binary=None):
        self.log = sys.stderr if log is None else log
        self.binary = binary or video_encode.resolve(self.log)
        self.path = Path(path)
        self.parts = Parts()
        self.canvas = Canvas()
        self.resample = Resample(self.write)
        self.process = None
        self.received = self.packed = 0
        self.started = self.last_host = None

    def __enter__(self):
        self.file = self.path.with_suffix(".frames.csv").open("w", newline="")
        self.csv = csv.writer(self.file)
        self.csv.writerow(CSV_FIELDS)
        return self

    def write(self, frame):
        if self.process is None:
            size = (self.canvas.height, self.canvas.width)
            self.process = video_encode.open_encoder(self.binary, size, OUTPUT_FPS, self.path, live=True)
        self.process.stdin.write(frame)

    def corrupt(self, packet=None):
        """Discard one present after a packet CRC failure, retaining its sequence."""
        if packet is not None and len(packet) >= PART.size:
            seq, _, _ = PART.unpack_from(packet)
            if seq == self.parts.rejected:
                return
            if self.parts.seq is not None and self.parts.seq != seq:
                self.parts.abandon()
            if self.parts.seq == seq:
                self.parts.abandon()
            else:
                self.parts.dropped += 1
                self.parts.rejected = seq
        elif self.parts.seq is None:
            self.parts.dropped += 1
        else:
            self.parts.abandon()

    def packet(self, data):
        stream = self.parts.feed(data)
        if stream is None:
            return
        host_ns, now = time.time_ns(), time.monotonic()
        try:
            seq, us, windows, packed, dropped = self.canvas.apply(stream)
            self.resample.present(us, self.canvas.frame())
        except (ValueError, struct.error):
            self.parts.dropped += 1
            return
        self.csv.writerow((seq, us, host_ns, windows, packed, dropped))
        self.file.flush()
        self.received += 1
        self.packed += packed
        self.started = now if self.started is None else self.started
        self.last_host = now

    def finish(self, answer):
        self.parts.abandon()
        if self.received == 0 and not answer.startswith("PANELCAP_REFUSED"):
            raise RuntimeError("no complete PANEL presents received")
        now = time.monotonic()
        if self.last_host is not None:
            self.resample.finish(self.resample.last + int((now - self.last_host) * US_PER_SECOND))
        elapsed = max(now - (self.started or now), 1 / OUTPUT_FPS)
        match = re.search(r"dropped=(\d+)", answer)
        dropped = match[1] if match else "unknown"
        print(f"video: presents={self.received} fps={self.received / elapsed:.2f} "
              f"device_dropped={dropped} host_dropped={self.parts.dropped} MB/s={self.packed / elapsed / US_PER_SECOND:.3f}",
              file=self.log)

    def __exit__(self, kind, value, trace):
        self.file.close()
        if self.process is not None:
            self.process.stdin.close()
            if self.process.wait() and kind is None:
                raise RuntimeError("ffmpeg failed writing panel video")
