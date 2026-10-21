"""Tests for the nightly retrieval evaluation summary."""
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import docs_search  # noqa: E402
import eval_summary  # noqa: E402


class EvalSummary(unittest.TestCase):
    def test_latest_result_is_logged_and_summarized_without_changing_history(self):
        for mode in ("hybrid", "lexical"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as folder:
                history = Path(folder) / "history.jsonl"
                summary = Path(folder) / "summary.md"
                latest = json.dumps({"mode": mode, "mrr": 0.75, "top1": 0.5, "top3": 1.0, "rows": 4})
                contents = '{}\n' + latest + '\n'
                history.write_text(contents, encoding="utf-8")
                summary.write_text("Earlier step\n", encoding="utf-8")
                with mock.patch.object(docs_search, "eval_history", return_value=history), \
                     mock.patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": str(summary)}), \
                     mock.patch("sys.stdout", new_callable=io.StringIO) as output:
                    self.assertEqual(eval_summary.main([mode]), 0)
                self.assertEqual(output.getvalue().strip(), latest)
                report = summary.read_text(encoding="utf-8")
                for text in ("Earlier step", latest, "MRR 0.750", "top-1 50%", "top-3 100%", "4 rows"):
                    self.assertIn(text, report)
                self.assertEqual(history.read_text(encoding="utf-8"), contents)

    def test_wrong_mode_fails_after_reporting_actual_result(self):
        with tempfile.TemporaryDirectory() as folder:
            history = Path(folder) / "history.jsonl"
            summary = Path(folder) / "summary.md"
            latest = json.dumps({"mode": "lexical", "mrr": 1.0, "top1": 1.0, "top3": 1.0, "rows": 1})
            history.write_text(latest + "\n", encoding="utf-8")
            with mock.patch.object(docs_search, "eval_history", return_value=history), \
                 mock.patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": str(summary)}), \
                 mock.patch("sys.stdout", new_callable=io.StringIO) as output, \
                 mock.patch("sys.stderr", new_callable=io.StringIO) as errors:
                self.assertEqual(eval_summary.main(["hybrid"]), 1)
            self.assertIn(latest, output.getvalue())
            self.assertIn(latest, summary.read_text(encoding="utf-8"))
            self.assertIn("expected hybrid, got lexical", errors.getvalue())

    def test_missing_empty_or_invalid_history_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            history = Path(folder) / "history.jsonl"
            for contents in (None, "", "not json\n"):
                with self.subTest(contents=contents):
                    if contents is not None:
                        history.write_text(contents, encoding="utf-8")
                    with mock.patch.object(docs_search, "eval_history", return_value=history), \
                         mock.patch("sys.stderr", new_callable=io.StringIO):
                        self.assertEqual(eval_summary.main(["lexical"]), 1)
