"""Shell frame-cost captures preserve bracket order and reject empty reports."""
import pathlib
import sys
import tempfile
import unittest

PERF = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PERF))
import frame_cost


class FrameCostTests(unittest.TestCase):
    def test_real_external_report(self):
        path = PERF / "tests/fixtures/sound_mix.txt"
        window = frame_cost.frame_cost_window(path.read_text())
        self.assertEqual(window["brackets"]["present"], (9.54, 10.2))
        self.assertEqual(window["total"], 17.63)
        self.assertEqual(window["external"], {"sound_mix": (0.004, 0.015, 260)})  # magic: the real report fixture stage.
        self.assertNotIn(next(iter(window["external"])), dict(frame_cost.frame_cost_windows(path)[0]))

    def test_a_window_that_dropped_brackets_is_refused_by_every_reader(self):
        whole = "ms/frame avg/worst: a 1/1 | total 1.00"
        cut = "ms/frame avg/worst: a 1/1 | total 1.00 +424 dropped || x 0.002/0.006 ms/block n=9"
        self.assertEqual(frame_cost.frame_cost_window(whole)["brackets"], {"a": (1.0, 1.0)})
        with self.assertRaisesRegex(ValueError, "dropped 424 brackets"):
            frame_cost.frame_cost_window(cut)
        for read in (frame_cost.frame_cost_windows, frame_cost.frame_cost_means):
            with self.assertRaisesRegex(ValueError, "dropped 424 brackets"):
                read(None, f"{whole}\n{cut}\n{whole}\n")

    def test_a_lap_window_that_dropped_laps_is_refused(self):
        whole = "ms/frame laps: lap 179 ns | cpu0 a.b 1.000/2"
        self.assertEqual(set(frame_cost.frame_laps_window(whole)[0]), {"a.b"})
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory) / "laps.log"
            capture.write_text(whole + "\n" + whole + " +3 dropped\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "dropped 3 laps"):
                frame_cost.frame_laps_means(capture)

    def test_order_new_bracket_and_missing_captures(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory) / "frame.log"
            capture.write_text("ms/frame avg/worst: scene 2/3 stage.a 40/41 new.bracket 3/4 stage.b 0/0 | total 45\n"
                               "ms/frame avg/worst: scene 4/5 stage.a 42/43 new.bracket 5/6 stage.b 0/0 | total 51\n")
            self.assertEqual(frame_cost.frame_cost_means(capture),
                             [("scene", 3), ("stage.a", 41), ("new.bracket", 4), ("stage.b", 0)])
            self.assertIsNone(frame_cost.frame_cost_means(pathlib.Path(directory) / 'missing'))
            capture.write_text("perf: scene cyc avg/min/max 1/1/1\n")
            with self.assertRaises(ValueError):
                frame_cost.frame_cost_means(capture)

    def test_quarters_do_not_share_a_bracket_mean(self):
        text = ("ms/frame avg/worst: draw 2/3 | total 2 quarter=portrait\n"
                "ms/frame avg/worst: draw 4/5 | total 4 quarter=landscape\n")
        self.assertEqual(frame_cost.frame_cost_means(None, text),
                         [("draw quarter=portrait", 2), ("draw quarter=landscape", 4)])

    def test_frame_cost_line_without_brackets_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory) / "frame.log"
            capture.write_text("ms/frame avg/worst: | total 45\n")
            with self.assertRaises(ValueError):
                frame_cost.frame_cost_means(capture)
