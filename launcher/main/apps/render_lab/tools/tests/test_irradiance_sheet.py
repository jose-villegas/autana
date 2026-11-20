"""Measure a skin's eroded coverage with the shared dE76 colour conversion."""
import pathlib
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from irradiance_sheet import comparison_row


class IrradianceSheetTests(unittest.TestCase):
    def test_eroded_mask_excludes_background_and_boundary(self):
        ref = Image.new("RGB", (7, 7), "white")
        ours = Image.new("RGB", (7, 7), "black")
        mask = np.zeros((7, 7))
        mask[1:6, 1:6] = 1
        for y in range(2, 5):
            for x in range(2, 5):
                ours.putpixel((x, y), (255, 255, 255))
        row, stats = comparison_row(ref, ours, mask, "pose")
        self.assertEqual(stats, {"mean": 0, "p95": 0, "pixels": 9})
        self.assertEqual(len(row), 3)

    def test_empty_eroded_mask_is_rejected(self):
        image = Image.new("RGB", (3, 3))
        with self.assertRaisesRegex(ValueError, "empty"):
            comparison_row(image, image, np.zeros((3, 3)), "pose")


if __name__ == "__main__":
    unittest.main()
