"""launcher/tools/render/render_diff.py reading device captures: the turn a
capture's sidecar records is undone, so the default (turned) view and the
framebuffer bytes of one frame compare identical.

    python -m unittest discover -s launcher/tools/tests
"""
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "device"))
sys.path.insert(0, str(TOOLS / "render"))

import render_diff  # noqa: E402
import screenshot  # noqa: E402
from panel_size import PANEL_HEIGHT, PANEL_WIDTH  # noqa: E402


def panel_bmp():
    """A full panel frame whose every pixel differs from its neighbours'."""
    rows = [[(x & 0xFF, y & 0xFF, (x * 7 + y * 3) & 0xFF) for x in range(PANEL_WIDTH)]
            for y in range(PANEL_HEIGHT)]
    stride = PANEL_WIDTH * 3
    pixels = b"".join(bytes(v for pixel in row for v in pixel) for row in reversed(rows))
    header = b"BM" + (14 + 40 + len(pixels)).to_bytes(4, "little") + bytes(4) + (54).to_bytes(4, "little")
    info = (40).to_bytes(4, "little") + PANEL_WIDTH.to_bytes(4, "little") + PANEL_HEIGHT.to_bytes(4, "little") \
        + (1).to_bytes(2, "little") + (24).to_bytes(2, "little") + bytes(4) + (stride * PANEL_HEIGHT).to_bytes(4, "little") \
        + bytes(16)
    return header + info + pixels


class CaptureTurnTests(unittest.TestCase):
    def diff(self, *paths):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            code = render_diff.main(list(paths))
        return code, out.getvalue()

    def test_every_recorded_turn_compares_identical_to_the_framebuffer(self):
        png = screenshot.bmp_bytes_to_png(panel_bmp())
        state = json.dumps({"orientation_quarter": 1})
        with tempfile.TemporaryDirectory() as directory:
            raw, _ = screenshot.write_capture(str(Path(directory, "raw")), png, state, 0)
            for quarter in (1, 2, 3):
                turned, _ = screenshot.write_capture(str(Path(directory, f"q{quarter}")),
                                                     screenshot.turn_png(png, quarter), state, quarter)
                code, out = self.diff(turned, raw)
                self.assertEqual(code, 0, f"quarter {quarter}: {out}")
                self.assertIn("identical", out)

    def test_a_capture_whose_size_does_not_match_its_recorded_turn_is_refused(self):
        png = screenshot.bmp_bytes_to_png(panel_bmp())
        with tempfile.TemporaryDirectory() as directory:
            wrong, _ = screenshot.write_capture(str(Path(directory, "wrong")), png, "{}", 1)
            with self.assertRaisesRegex(SystemExit, "quarter 1 never produces"):
                self.diff(wrong, wrong)


if __name__ == "__main__":
    unittest.main()
