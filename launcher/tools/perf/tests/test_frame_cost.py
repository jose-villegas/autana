"""Shell frame-cost captures preserve bracket order and reject empty reports."""
import pathlib
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
from perf import frame_cost


class FrameCostTests(unittest.TestCase):
    def test_order_new_bracket_and_missing_captures(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory) / "frame.log"
            capture.write_text("ms/frame avg/worst: scene 2/3 r3d.draw 40/41 new.bracket 3/4 present.wait 0/0 | total 45\n"
                               "ms/frame avg/worst: scene 4/5 r3d.draw 42/43 new.bracket 5/6 present.wait 0/0 | total 51\n")
            self.assertEqual(frame_cost.frame_cost_means(capture),
                             [("scene", 3), ("r3d.draw", 41), ("new.bracket", 4), ("present.wait", 0)])
            self.assertIsNone(frame_cost.frame_cost_means(pathlib.Path(directory) / 'missing'))
            capture.write_text("perf: scene cyc avg/min/max 1/1/1\n")
            with self.assertRaises(ValueError):
                frame_cost.frame_cost_means(capture)

    def test_frame_cost_line_without_brackets_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory) / "frame.log"
            capture.write_text("ms/frame avg/worst: | total 45\n")
            with self.assertRaises(ValueError):
                frame_cost.frame_cost_means(capture)
