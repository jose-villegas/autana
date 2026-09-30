"""Tests for launcher/tools/render/render_compare.py on tiny synthetic images.

    python -m unittest discover -s launcher/tools/tests
"""
import sys
import unittest
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "render"))

import render_compare  # noqa: E402

CLEAR = (10, 20, 30)
DRAWN = (200, 100, 50)


def image(rows):
    """A W x H image from rows of RGB tuples."""
    picture = Image.new("RGB", (len(rows[0]), len(rows)))
    picture.putdata([pixel for row in rows for pixel in row])
    return picture


class MeasureTest(unittest.TestCase):
    def test_identical_images_differ_nowhere(self):
        a = image([[DRAWN, CLEAR], [CLEAR, DRAWN]])
        stats = render_compare.measure(a, a.copy(), CLEAR)
        self.assertEqual(stats.changed, 0)
        self.assertEqual(stats.mean_abs, 0.0)
        self.assertEqual((stats.holes_a, stats.holes_b), (0, 0))

    def test_changed_share_and_mean(self):
        a = image([[(0, 0, 0), (0, 0, 0)], [(0, 0, 0), (0, 0, 0)]])
        b = image([[(30, 0, 0), (0, 0, 0)], [(0, 0, 0), (0, 6, 0)]])
        stats = render_compare.measure(a, b, None)
        self.assertEqual(stats.changed, 2)
        self.assertEqual(stats.total, 4)
        self.assertAlmostEqual(stats.mean_abs, 36 / 12)

    def test_a_hole_is_clear_on_one_side_and_drawn_on_the_other(self):
        a = image([[CLEAR, CLEAR], [DRAWN, DRAWN]])
        b = image([[CLEAR, DRAWN], [DRAWN, CLEAR]])
        stats = render_compare.measure(a, b, CLEAR)
        self.assertEqual(stats.holes_a, 1)
        self.assertEqual(stats.holes_b, 1)

    def test_no_clear_colour_counts_no_holes(self):
        a = image([[CLEAR, DRAWN]])
        b = image([[DRAWN, CLEAR]])
        stats = render_compare.measure(a, b, None)
        self.assertEqual((stats.holes_a, stats.holes_b), (0, 0))

    def test_565_expanded_clear_colour_matches(self):
        # 0x9CC0E6 as the 16-bit framebuffer holds it once expanded to 24 bits.
        a = image([[(156, 195, 231), DRAWN]])
        b = image([[DRAWN, DRAWN]])
        stats = render_compare.measure(a, b, render_compare.parse_rgb("9CC0E6"))
        self.assertEqual(stats.holes_a, 1)


class HeatmapTest(unittest.TestCase):
    def test_grey_is_amplified_and_holes_are_red(self):
        a = image([[CLEAR, (0, 0, 0), (0, 0, 0)]])
        b = image([[DRAWN, (0, 3, 0), (0, 0, 0)]])
        heat = render_compare.heatmap(a, b, CLEAR, gain=8)
        self.assertEqual(heat.getpixel((0, 0)), (255, 0, 0))
        self.assertEqual(heat.getpixel((1, 0)), (24, 24, 24))
        self.assertEqual(heat.getpixel((2, 0)), (0, 0, 0))

    def test_gain_saturates(self):
        a = image([[(0, 0, 0)]])
        b = image([[(250, 0, 0)]])
        self.assertEqual(render_compare.heatmap(a, b, None, gain=8).getpixel((0, 0)), (255, 255, 255))


class SheetTest(unittest.TestCase):
    def test_row_is_a_then_b_then_heat_and_rows_stack(self):
        a = image([[DRAWN, DRAWN]])
        sheet = render_compare.sheet([("one", a, a), ("two", a, a)], CLEAR, gain=8)
        self.assertEqual(sheet.size, (6, 2))

    def test_size_mismatch_is_refused(self):
        with self.assertRaises(ValueError):
            render_compare.sheet([("x", image([[DRAWN]]), image([[DRAWN, DRAWN]]))], None, gain=8)


if __name__ == "__main__":
    unittest.main()
