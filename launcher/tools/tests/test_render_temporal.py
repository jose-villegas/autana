"""Triangle-scale temporal changes filter sub-block noise."""
import csv
import pathlib
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))
import render_temporal
from test_render_compare import avi_bytes

BLOCK = render_temporal.TEMPORAL_BLOCK_SIDE
CHANNEL_MAX = render_temporal.CHANNEL_MAX


class TemporalChangeTest(unittest.TestCase):
    def test_identical_motion_has_no_extra_change(self):
        old = np.zeros((2 * BLOCK, 2 * BLOCK, 3), dtype=np.uint8)
        new = np.full_like(old, 200)
        self.assertEqual(render_temporal.temporal_change(old, new, old, new), (0.0, 0.0))

    def test_one_toggling_block_is_counted(self):
        old = np.zeros((BLOCK, BLOCK, 3), dtype=np.uint8)
        new = np.full_like(old, CHANNEL_MAX)
        self.assertEqual(render_temporal.temporal_change(old, old, old, new), (1.0, 100.0))
        self.assertEqual(render_temporal.temporal_change(old, old, new, old), (1.0, 100.0))

    def test_sub_block_checkerboard_is_filtered(self):
        board = np.indices((2 * BLOCK, 2 * BLOCK)).sum(axis=0) % 2
        a = np.repeat((board * CHANNEL_MAX)[:, :, None], 3, axis=2).astype(np.uint8)
        self.assertEqual(render_temporal.temporal_change(a, a, a, CHANNEL_MAX - a), (0.0, 0.0))


class TemporalCsvTest(unittest.TestCase):
    def test_csv_preserves_spatial_metrics_and_averages_transitions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            black = Image.new("RGB", (BLOCK, BLOCK))
            white = Image.new("RGB", (BLOCK, BLOCK), "white")
            a, b, csv_path = root / "a.avi", root / "b.avi", root / "frames.csv"
            a.write_bytes(avi_bytes(BLOCK, BLOCK, [black, black], 100))
            b.write_bytes(avi_bytes(BLOCK, BLOCK, [black, white], 100))
            csv_path.write_text("frame,mean_abs\n0,0\n1,255\n")
            summary = render_temporal.compare_temporal(a, b, csv_path)
            self.assertIn("extra frame-to-frame change 1.00000000", summary)
            self.assertIn("blocks popping 100.0000%", summary)
            with csv_path.open() as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual([row["mean_abs"] for row in rows], ["0", "255"])
            self.assertEqual([float(row["extra_change"]) for row in rows], [0, 1])
            self.assertEqual(render_temporal.compare_temporal(a, b, csv_path), summary)
            self.assertEqual(csv_path.read_text().count("blocks_popping_pct"), 1)

    def test_different_lengths_fail_without_overwriting_csv(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            black = Image.new("RGB", (BLOCK, BLOCK))
            a, b, csv_path = root / "a.avi", root / "b.avi", root / "frames.csv"
            a.write_bytes(avi_bytes(BLOCK, BLOCK, [black], 100))
            b.write_bytes(avi_bytes(BLOCK, BLOCK, [black, black], 100))
            original = "frame,mean_abs\n0,0\n1,0\n"
            csv_path.write_text(original)
            with self.assertRaises(ValueError):
                render_temporal.compare_temporal(a, b, csv_path)
            self.assertEqual(csv_path.read_text(), original)
