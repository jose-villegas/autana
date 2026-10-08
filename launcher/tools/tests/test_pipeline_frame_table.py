"""Frame stage tables read committed capture formats and tolerate pending owners."""
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "render"))
from r3d import dynres_report
from generated_blocks import apply_tables


class PipelineFrameTableTests(unittest.TestCase):
    def test_frame_table_reads_present_windows_and_optional_resolve(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory) / "present.log"
            capture.write_text("perf: present cyc avg/min/max 240000/1/2 insn avg 1 n=10\n"
                               "ms/frame avg/worst: present 8.00/9.0 | total 8.00\n"
                               "ms/frame avg/worst: ui.paint 1.00/2.0 present 10.00/11.0 | total 11.00\n")
            present = dynres_report.read_present(capture)
            self.assertEqual(present, 9.0)
            stages = {"r3d.census": 1.0, "r3d.cull": 2.0, "r3d.transform": 3.0,
                      "r3d.draw": 4.0, "r3d.upscale": 5.0}
            table = dynres_report.pipeline_table({(184, 224): stages}, present, (184, 224))
            self.assertIn("| census/cull | 3.00 |", table)
            self.assertIn("| resolve | not in capture |", table)
            self.assertIn("| present | 9.00 |", table)
            stages["r3d.resolve"] = 6.0
            table = dynres_report.pipeline_table({(184, 224): stages}, None, (184, 224))
            self.assertIn("| resolve | 6.00 |", table)
            self.assertIn("| present | not in capture |", table)
            self.assertEqual([line.split("|")[1].strip() for line in table.splitlines()[2:8]],
                             ["census/cull", "transform", "draw", "resolve", "upscale", "present"])
            capture.write_text("perf: present cyc avg/min/max 1/1/1 insn avg 1 n=1\n")
            with self.assertRaises(ValueError):
                dynres_report.read_present(capture)

    def test_only_pending_pipeline_tables_may_be_unowned(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            tables = root / "tables"
            tables.mkdir()
            with patch("generated_blocks.tracked_files", return_value=[]):
                for name in ("pipeline-frame-stages", "bake-machine", "bake-steps"):
                    (tables / f"{name}.md").write_text("table\n")
                self.assertFalse(apply_tables(root, tables, check=True))
                (tables / "typo.md").write_text("table\n")
                with self.assertRaises(ValueError):
                    apply_tables(root, tables)


if __name__ == "__main__":
    unittest.main()
