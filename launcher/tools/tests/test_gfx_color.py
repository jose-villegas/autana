"""launcher/tools/device/gfx_color.py: the firmware's RGB565 packing for host tools.

    python -m unittest discover -s launcher/tools/tests
"""
import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "device"))

import gfx_color  # noqa: E402


class GfxColorTests(unittest.TestCase):
    def test_expand_replicates_bits_so_full_channels_reach_255(self):
        self.assertEqual(gfx_color.expand(31, 63, 31), (255, 255, 255))
        self.assertEqual(gfx_color.expand(16, 32, 1), (132, 130, 8))

    def test_every_565_value_round_trips_through_expand(self):
        for r5 in range(32):
            for g6 in range(64):
                with self.subTest(r5=r5, g6=g6):
                    r, g, b = gfx_color.expand(r5, g6, r5)
                    self.assertEqual(gfx_color.rgb565(r, g, b), (r5 << 11) | (g6 << 5) | r5)

    def test_arrays_round_trip_like_ints(self):
        native = np.arange(65536, dtype=np.int64)
        r, g, b = gfx_color.expand(native >> 11, (native >> 5) & 63, native & 31)
        self.assertTrue(np.array_equal(gfx_color.rgb565(r, g, b), native))
        self.assertTrue(np.array_equal(gfx_color.swap(gfx_color.swap(native)), native))

    def test_a_step_is_the_levels_one_565_step_of_each_channel_spans(self):
        for channel, (step, one) in enumerate(zip(gfx_color.STEP, (1 << 11, 1 << 5, 1))):
            with self.subTest(channel=channel):
                below, at = [0, 0, 0], [0, 0, 0]
                below[channel], at[channel] = step - 1, step
                self.assertEqual(gfx_color.rgb565(*below), 0)
                self.assertEqual(gfx_color.rgb565(*at), one)

    def test_swap_moves_the_bytes(self):
        self.assertEqual(gfx_color.swap(0xF800), 0x00F8)
        self.assertEqual(gfx_color.swap(gfx_color.rgb565(0xFF, 0, 0)), 0x00F8)


if __name__ == "__main__":
    unittest.main()
