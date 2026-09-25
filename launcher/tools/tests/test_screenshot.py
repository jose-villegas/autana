"""Regression tests for launcher/tools/device/screenshot.py's shared decode and
wire-protocol logic - the module scripts/device/device.py's own `screenshot`
subcommand imports rather than re-implementing (see its own screenshot()
docstring). No hardware: a FakeConnection stands in for the serial port.

    python -m unittest discover -s launcher/tools/tests
"""
import base64
import json
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "device"))

import screenshot  # noqa: E402


def build_bmp(width, height, rows_bgr):
    """A minimal 24bpp BMP: 14-byte file header + 40-byte BITMAPINFOHEADER,
    bottom-up rows padded to 4 bytes - the same layout
    screenshot_bmp_header() (util/screenshot.h) writes on the device.
    `rows_bgr` is top-down, [(b, g, r), ...] per row; this stores them
    bottom-up as BMP requires."""
    stride = ((width * 3 + 3) // 4) * 4
    pixel_offset = 14 + 40
    pixel_data = bytearray()
    for row in reversed(rows_bgr):
        line = bytearray()
        for b, g, r in row:
            line += bytes((b, g, r))
        line += b"\x00" * (stride - len(line))
        pixel_data += line
    total = pixel_offset + len(pixel_data)

    header = b"BM" + struct.pack("<IHHI", total, 0, 0, pixel_offset)
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixel_data), 0, 0, 0, 0)
    return bytes(header) + info + bytes(pixel_data)


class BmpBytesToPngTests(unittest.TestCase):
    def test_decodes_a_known_pixel_grid(self):
        # top-down RGB: (255,0,0) (0,255,0) / (0,0,255) (255,255,255)
        rows_bgr = [[(0, 0, 255), (0, 255, 0)], [(255, 0, 0), (255, 255, 255)]]
        bmp = build_bmp(2, 2, rows_bgr)

        png = screenshot.bmp_bytes_to_png(bmp)

        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        idat = extract_idat(png)
        raw = zlib.decompress(idat)
        # Two rows, each a filter byte (0) then 2 RGB pixels (6 bytes).
        self.assertEqual(len(raw), 2 * (1 + 6))
        row0 = raw[0:7]
        row1 = raw[7:14]
        self.assertEqual(row0, bytes([0, 255, 0, 0, 0, 255, 0]))
        self.assertEqual(row1, bytes([0, 0, 0, 255, 255, 255, 255]))

    def test_rejects_a_non_bmp(self):
        with self.assertRaisesRegex(ValueError, "BM"):
            screenshot.bmp_bytes_to_png(b"not a bmp at all, but 64 bytes long" + b"\x00" * 30)

    def test_rejects_a_top_down_bitmap(self):
        bmp = bytearray(build_bmp(1, 1, [[(1, 2, 3)]]))
        struct.pack_into("<i", bmp, 22, -1)  # negative height = top-down
        with self.assertRaisesRegex(ValueError, "positive"):
            screenshot.bmp_bytes_to_png(bytes(bmp))


def extract_idat(png):
    pos = 8
    while pos < len(png):
        length, = struct.unpack_from(">I", png, pos)
        tag = png[pos + 4:pos + 8]
        data = png[pos + 8:pos + 8 + length]
        if tag == b"IDAT":
            return data
        pos += 12 + length
    raise AssertionError("no IDAT chunk found")


class FakeConnection:
    """Feeds queued byte chunks to read(), and records every write()."""

    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.writes = []

    def read(self, unused_size):
        return self.chunks.pop(0) if self.chunks else b""

    def write(self, data):
        self.writes.append(data)

    def flush(self):
        pass

    def reset_input_buffer(self):
        pass


class ReadScreenshotTests(unittest.TestCase):
    def wire_lines(self, bmp, state_json=None):
        encoded = base64.b64encode(bmp).decode("ascii")
        lines = [f"SCREENSHOT_BEGIN size={len(bmp)}", f"SCREENSHOT_DATA:{encoded}"]
        if state_json is not None:
            lines.append(f"SCREENSHOT_STATE:{state_json}")
        lines.append("SCREENSHOT_END")
        return ("\n".join(lines) + "\n").encode("ascii")

    def test_triggers_and_decodes_a_complete_capture(self):
        bmp = build_bmp(1, 1, [[(9, 8, 7)]])
        connection = FakeConnection([self.wire_lines(bmp, '{"heap":123}')])

        png, state_json = screenshot.read_screenshot(connection, timeout=1.0)

        self.assertEqual(connection.writes, [screenshot.TRIGGER])
        self.assertEqual(png, screenshot.bmp_bytes_to_png(bmp))
        self.assertEqual(state_json, '{"heap":123}')

    def test_a_capture_with_no_state_line_returns_none_for_it(self):
        bmp = build_bmp(1, 1, [[(1, 1, 1)]])
        connection = FakeConnection([self.wire_lines(bmp)])

        unused_png, state_json = screenshot.read_screenshot(connection, timeout=1.0)

        self.assertIsNone(state_json)

    def test_a_refused_capture_raises_with_the_devices_own_reason(self):
        connection = FakeConnection([b"SCREENSHOT_REFUSED: band mode, no PSRAM room\n"])
        with self.assertRaisesRegex(screenshot.ScreenshotRefused, "band mode"):
            screenshot.read_screenshot(connection, timeout=1.0)

    def test_a_capture_that_never_starts_times_out(self):
        connection = FakeConnection([b"I (1) shell: ordinary log line\n"])
        with self.assertRaisesRegex(RuntimeError, "never saw a"):
            screenshot.read_screenshot(connection, timeout=0.05)

    def test_status_lines_are_reported_as_the_capture_progresses(self):
        bmp = build_bmp(1, 1, [[(1, 2, 3)]])
        connection = FakeConnection([self.wire_lines(bmp)])
        seen = []

        screenshot.read_screenshot(connection, timeout=1.0, on_status=seen.append)

        self.assertTrue(any("sent trigger" in message for message in seen))
        self.assertTrue(any("capturing" in message for message in seen))


class WriteCaptureTests(unittest.TestCase):
    def test_writes_the_png_and_pretty_prints_valid_state_json(self):
        with tempfile.TemporaryDirectory() as directory:
            out = os.path.join(directory, "shot.bmp")
            png_path, state_path = screenshot.write_capture(out, b"PNGDATA", '{"a": 1}')

            self.assertEqual(png_path, os.path.join(directory, "shot.png"))
            self.assertEqual(Path(png_path).read_bytes(), b"PNGDATA")
            self.assertEqual(state_path, os.path.join(directory, "shot.json"))
            self.assertEqual(json.loads(Path(state_path).read_text()), {"a": 1})

    def test_malformed_state_json_is_still_written_raw(self):
        with tempfile.TemporaryDirectory() as directory:
            out = os.path.join(directory, "shot.png")
            unused_png_path, state_path = screenshot.write_capture(out, b"X", "{not json")

            self.assertEqual(Path(state_path).read_text().strip(), "{not json")

    def test_no_state_json_writes_no_state_file(self):
        with tempfile.TemporaryDirectory() as directory:
            out = os.path.join(directory, "shot.png")
            unused_png_path, state_path = screenshot.write_capture(out, b"X", None)

            self.assertIsNone(state_path)
            self.assertFalse(os.path.exists(os.path.join(directory, "shot.json")))


if __name__ == "__main__":
    unittest.main()
