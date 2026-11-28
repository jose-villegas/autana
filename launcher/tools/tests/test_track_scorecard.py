"""Exercise scorecard capture provenance, revision joins, image pixels and part geometry."""
import pathlib
import sys
import unittest
from unittest.mock import patch

import numpy as np
from PIL import Image

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "render" / "scorecard"))

from captures import capture_rows, suite_rows
from report import markdown, nearest_upscale, tint_overlay
from r3d.objects.part_shape import shape_measure


class ScorecardTests(unittest.TestCase):
    def test_capture_keeps_first_run_and_labels_missing_heap(self):
        line = ("frame_profile: sponza-fitted-full frames=3 mean 47205us med 47828us p95 58351us "
                "worst 60334us best 30837us | fps mean 21.18 low 17.14 worst 16.57 quarter=portrait")
        heap = "frame_heap: sponza-fitted-full free=4096 largest=2048 quarter=portrait"
        [without] = capture_rows("job-a", line + "\n" + line + "\n" + heap)
        [with_heap] = capture_rows("job-b", line + "\n" + heap)
        self.assertEqual((without["label"], without["mean"], without["p95"]), ("job-a", 47205, 58351))
        self.assertIsNone(without["heap"])
        self.assertEqual(with_heap["heap"], {"free": 4096, "largest": 2048})

    def test_ceilings_follow_the_suite_rows(self):
        rows = suite_rows("bistro")
        self.assertEqual({row["quarter"] for row in rows}, {"portrait", "landscape"})
        self.assertEqual(len({row["row"] for row in rows}), 2)
        self.assertTrue(all(row["p95"] > row["mean"] > 0 for row in rows))

    def test_table_joins_quarters_and_keeps_missing_capture_distinct(self):
        row = dict(row="sponza", quarter="portrait", p95=2500, mean=1500)
        reading = dict(row, label="job-a", heap=None)
        base = dict(path="launcher/demo/sponza/sponza.scene.toml", ceilings=[row],
                    see_through={"pixels": 1, "ceiling": 2}, sky={"reason": "no source"},
                    held_out={"mean": 2.0, "sets": ["path"], "views": 3}, parts=[], pack_bytes=200)
        parent = dict(revision="aaa", scenes={"sponza": dict(base, captures=[reading])})
        tip = dict(revision="bbb", scenes={"sponza": dict(base, captures=[])})
        text = markdown(parent, tip)
        self.assertIn("1.500 / 2.500 ms (job-a)<br>mean goal 1.500 ms, p95 ceiling 2.500 ms"
                      " | not measured<br>mean goal 1.500 ms, p95 ceiling 2.500 ms", text)
        self.assertIn("not logged (job-a) | not measured", text)
        self.assertIn("not measured (no source)", text)
        self.assertIn("2.000 (path · 3 views)", text)

    def test_held_out_row_uses_fit_rasterizer_and_labels_sets(self):
        from r3d.objects import held_out_reference as held

        probe = ("probe", pathlib.Path("probes.txt"), pathlib.Path("references"))
        with (patch.object(held, "fit_rasterizer_reason", return_value=None),
              patch.object(held, "held_out_sets", return_value=[probe]),
              patch.object(held.appearance, "load_views", return_value=(["pose"], (192, 192))),
              patch.object(held.appearance, "held_out_line",
                           return_value="held-out dE76 (fit rasteriser): 5.594 over 1 views") as owner):
            from types import SimpleNamespace
            scene = SimpleNamespace(camera=SimpleNamespace(component=SimpleNamespace(background=0xFFFFFF)))
            fit = SimpleNamespace(scene=scene)
            score = held.measure_held_out(fit, None, None, "mesh")
            owner.assert_called_once_with("mesh", [(["pose"], (192, 192))], (1.0, 1.0, 1.0))
        self.assertEqual(score, {"mean": 5.594, "sets": ["probe"], "views": 1})
        with patch.object(held, "fit_rasterizer_reason", return_value="CUDA unavailable"):
            self.assertEqual(held.measure_held_out(fit, None, None, "mesh"), {"reason": "CUDA unavailable"})

    def test_nearest_upscale_copies_whole_pixels(self):
        pixels = np.array([[[17, 34, 51], [68, 85, 102]]], dtype=np.uint8)
        actual = nearest_upscale(Image.fromarray(pixels), 2)
        np.testing.assert_array_equal(actual, pixels.repeat(2, axis=0).repeat(2, axis=1))

    def test_overlay_applies_only_the_masked_fraction(self):
        pixels = Image.new("RGB", (3, 1), "black")
        actual = tint_overlay(pixels, [[0, 0.5, 1]], (200, 100, 0), 0.5)
        self.assertEqual(list(actual.getdata()), [(0, 0, 0), (50, 25, 0), (100, 50, 0)])
        for invalid in ([[0]], [[0, 1, float("nan")]], [[0, 1, 2]]):
            with self.assertRaises(ValueError):
                tint_overlay(pixels, invalid, (200, 100, 0), 0.5)

    def test_cube_shift_has_known_front_iou_and_hausdorff(self):
        points = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0],
                           [0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]], dtype=float)
        faces = np.array([[0, 1, 2], [0, 2, 3], [4, 5, 6], [4, 6, 7], [0, 1, 5], [0, 5, 4],
                          [3, 2, 6], [3, 6, 7], [0, 3, 7], [0, 7, 4], [1, 2, 6], [1, 6, 5]])
        # Shorten z to fix the front axis, then shift by half a side in that view.
        points[:, 2] *= 0.75
        points[:, 0] *= 2
        iou, hausdorff = shape_measure(points[faces], (points + [0, 0.5, 0])[faces])
        self.assertAlmostEqual(iou, 1 / 3, delta=0.035)
        self.assertAlmostEqual(hausdorff, 0.5 / np.sqrt(2 ** 2 + 1 + 0.75 ** 2), places=6)
