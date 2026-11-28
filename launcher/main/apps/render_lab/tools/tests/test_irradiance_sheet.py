"""Measure a skin's eroded coverage with the shared dE76 colour conversion."""
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import irradiance_sheet as sheet


class IrradianceSheetTests(unittest.TestCase):
    def test_eroded_mask_excludes_background_and_boundary(self):
        ref = Image.new("RGB", (7, 7), "white")
        ours = Image.new("RGB", (7, 7), "black")
        mask = np.zeros((7, 7))
        mask[1:6, 1:6] = 1
        for y in range(2, 5):
            for x in range(2, 5):
                ours.putpixel((x, y), (255, 255, 255))
        stats = sheet.comparison_row(ref, ours, mask, "pose")[1]
        self.assertEqual(stats, {"mean": 0, "p95": 0, "pixels": 9})

    def test_empty_mask_has_no_crop_or_score(self):
        image = Image.new("RGB", (3, 3))
        panels, stats = sheet.comparison_row(image, image, np.zeros((3, 3)), "pose")
        self.assertEqual(panels, [])
        self.assertEqual(stats, {"mean": None, "p95": None, "pixels": 0})

    def test_sheet_crops_each_visible_skin_and_reports_each_mask(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            ours = Image.new("RGB", (64, 32), "black")
            ref = Image.new("RGB", ours.size, "white")
            masks = np.zeros((3, 32, 64))
            masks[0, 1:6, 1:6] = 1
            masks[1, 20:27, 55:64] = 1
            ours.paste("white", (1, 1, 6, 6))
            ours.save(folder / "ours.bmp")
            ref.save(folder / sheet.REFERENCE_PICTURE)
            np.save(folder / sheet.PER_SKIN_COVERAGE, masks)
            out = folder / "sheet.png"
            with patch.object(sheet, "still_sheet", wraps=sheet.still_sheet) as layout:
                stats = sheet.compose([folder], out)[folder.name]
            row, = layout.call_args.args[0]
            self.assertEqual(row[0][1].tobytes(), ours.tobytes())
            self.assertEqual(len(row), 5)
            for host, reference in (row[1:3], row[3:5]):
                self.assertEqual(host[1].size, reference[1].size)
                self.assertGreater(host[1].width, 9)
                self.assertIn("dE76", reference[0])
            self.assertEqual([skin["skin"] for skin in stats], [1, 2, 3])
            self.assertEqual(stats[0]["mean"], 0)
            self.assertGreater(stats[1]["mean"], 99)
            self.assertEqual(stats[2]["pixels"], 0)
            self.assertIsNone(stats[2]["mean"])
            self.assertTrue(out.is_file())


if __name__ == "__main__":
    unittest.main()
