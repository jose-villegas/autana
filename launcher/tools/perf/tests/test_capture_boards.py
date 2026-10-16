"""Capture provenance must agree before comparison."""
import json
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import capture
import perf_compare


class CaptureBoards(unittest.TestCase):
    def test_one_board_job_and_mixed_job(self):
        records = [dict(board=board, suites={"suite": {"runs": [
            dict(capture=name, rows={}, owners={}, instructions={})]}})
                   for board, name in (("90:70:69:FE:A3:08", "a.log"),
                                       ("90:70:69:FE:A3:08", "b.log"))]
        self.assertEqual(perf_compare.observations(records), ({}, {}, {}))
        records[1]["board"] = "90:70:69:FE:B4:09"
        with self.assertRaisesRegex(ValueError, "A308.*a.log.*B409.*b.log"):
            perf_compare.observations(records)

    def test_unknown_is_only_comparable_with_unknown(self):
        capture.require_same_board([{"capture": "a"}, {"capture": "b"}])
        with self.assertRaisesRegex(ValueError, "unknown"):
            capture.require_same_board([{"capture": "a"}, {"capture": "b", "board": "90:70:69:FE:A3:08"}])

    def test_manifest_lookup_and_last_entry(self):
        with tempfile.TemporaryDirectory() as root:
            paths = [pathlib.Path(root) / name for name in ("a.log", "b.log")]
            rows = [dict(capture_path=str(paths[0]), board="90:70:69:FE:B4:09")]
            rows += [dict(capture_path=str(path), board="90:70:69:FE:A3:08") for path in paths]
            (pathlib.Path(root) / "index.jsonl").write_text(
                "".join(json.dumps(row) + "\n" for row in rows))
            capture.require_same_board(paths)
            with self.assertRaises(ValueError):
                capture.require_same_board([*paths, pathlib.Path(root) / "unknown.log"])

    def test_phase_and_sweep_refuse_before_parsing(self):
        import phase_report
        import perf_sweep
        boards = ("90:70:69:FE:A3:08", "90:70:69:FE:B4:09")
        runs = {name: dict(board=board, capture=name) for name, board in zip(("a", "b"), boards)}
        with self.assertRaisesRegex(ValueError, "different boards"):
            phase_report.comparison_lines(runs, ["a", "b"], ["Total"])
        with tempfile.TemporaryDirectory() as root:
            (pathlib.Path(root) / "plan.json").write_text(json.dumps({"captures": list(runs.values())}))
            with self.assertRaisesRegex(ValueError, "different boards"):
                perf_sweep.report(root)

    def test_layout_report_refuses_before_reading_tables(self):
        import layout_pilot
        with tempfile.TemporaryDirectory() as root:
            for name, board in (("a", "90:70:69:FE:A3:08"), ("b", "90:70:69:FE:B4:09")):
                record = dict(board=board, suites={"suite": {"runs": [dict(capture=name)]}})
                (pathlib.Path(root) / (name + ".json")).write_text(json.dumps(record))
            with self.assertRaisesRegex(ValueError, "different boards"):
                layout_pilot.load(root)

    def test_perf_job_uses_selected_board(self):
        import perf_job
        board = "90:70:69:FE:A3:08"
        with patch.object(perf_job, "chosen_board", return_value=board), patch.object(
                perf_job, "duration_history", return_value={"flash": 10}) as history:
            self.assertEqual(perf_job.job_estimate(1, 0, 60, restore=False), 10)
            history.assert_called_once_with(board=board)

    def test_records_without_capture_paths_need_no_index_lookup(self):
        record = {"suites": {"suite": {"runs": [{"rows": {}}]}}}
        with patch.object(capture, "find_manifest_entry", side_effect=AssertionError("no capture path")):
            self.assertIsNone(capture.require_same_board([record]))
