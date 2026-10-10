"""Frame tables follow capture order and require document owners."""
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch
TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "render"))
from r3d import dynres_report
import frame_stages
from generated_blocks import apply_tables

class PipelineFrameTableTests(unittest.TestCase):
    def test_stage_headers_and_cells_share_resolve_column(self):
        base = {'mean': 1000, 'p50': 1000, 'max': 1000, 'r3d.draw': 2.0, 'r3d.upscale': 3.0}
        spans = dict(setup=100, rows=100, span_setup=100, fill=100)
        table = dynres_report.stages_table({(184, 224): dict(base, **{'r3d.resolve': 4.0}),
                                          (92, 112): base}, {(184, 224): spans, (92, 112): spans})
        lines = table.splitlines()
        index = lines[0].split('|').index(' resolve ')
        self.assertEqual(lines[2].split('|')[index].strip(), '4.0')
        self.assertEqual(lines[3].split('|')[index].strip(), 'not in capture')
        self.assertTrue(all(len(line.split('|')) == len(lines[0].split('|')) for line in lines[:4]))

    def test_order_new_bracket_and_missing_captures(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory) / "frame.log"
            capture.write_text("ms/frame avg/worst: scene 2/3 r3d.draw 40/41 new.bracket 3/4 present.wait 0/0 | total 45\n"
                               "ms/frame avg/worst: scene 4/5 r3d.draw 42/43 new.bracket 5/6 present.wait 0/0 | total 51\n")
            self.assertEqual(dynres_report.frame_cost_means(capture),
                             [("scene", 3), ("r3d.draw", 41), ("new.bracket", 4), ("present.wait", 0)])
            self.assertIsNone(dynres_report.frame_cost_means(pathlib.Path(directory) / 'missing'))
            capture.write_text("perf: scene cyc avg/min/max 1/1/1\n")
            with self.assertRaises(ValueError):
                dynres_report.frame_cost_means(capture)

    def test_views_add_their_difference_in_report_order(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "shipped.log").write_text("ms/frame avg/worst: r3d.draw 40/41 present.wait 0/0 frame.rest 1/1 | total 41\n")
            (root / "view-motion.log").write_text(
                "ms/frame avg/worst: r3d.draw 42/43 r3d.resolve 15/15 present.wait 0/0 frame.rest 7/7 | total 64\n")
            order, base, added = frame_stages.stage_costs(root / "shipped.log", str(root / "view-{view}.log"),
                                                    ["depth", "motion"])
            self.assertEqual(order, ["r3d.draw", "r3d.resolve", "present.wait", "frame.rest"])
            self.assertNotIn("r3d.resolve", base)
            self.assertEqual(list(added), ["motion"])
            self.assertEqual(added["motion"], {"r3d.draw": 2, "r3d.resolve": 15, "present.wait": 0, "frame.rest": 6})
            table = frame_stages.table(order, base, added, ["depth", "motion"], [1, 1], "chart.png")
            self.assertIn("| r3d.resolve | not run | +15.00 |", table)
            self.assertIn("| total | 41.00 | +23.00 |", table)
            self.assertIn("Not captured: depth.", table)
            with self.assertRaises(ValueError):
                frame_stages.stage_costs(root / "missing.log", str(root / "view-{view}.log"), ["motion"])

    def test_view_names_come_from_the_view_table(self):
        names = frame_stages.view_names()
        self.assertIn("motion", names)
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "context.c"
            source.write_text("static int x;\n")
            with self.assertRaises(ValueError):
                frame_stages.view_names(source)

    def test_frame_cost_line_without_brackets_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory) / "frame.log"
            capture.write_text("ms/frame avg/worst: | total 45\n")
            with self.assertRaises(ValueError):
                dynres_report.frame_cost_means(capture)

    def test_every_table_needs_a_document_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "bake-steps.md").write_text("table\n")
            with patch("generated_blocks.tracked_files", return_value=[]), self.assertRaises(ValueError):
                apply_tables(root, root)
