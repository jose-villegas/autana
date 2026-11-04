"""The step summary takes each entry as a paragraph, and writes nothing outside a CI step."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from step_summary import summarize  # noqa: E402


class SummarizeTests(unittest.TestCase):
    def test_each_entry_is_a_paragraph_of_its_own(self):
        with tempfile.TemporaryDirectory() as directory:
            summary = Path(directory) / "summary"
            with mock.patch.dict(os.environ, GITHUB_STEP_SUMMARY=str(summary)):
                summarize("first")
                summarize("second — ünïcode")
            self.assertEqual(summary.read_text(encoding="utf-8"), "first\n\nsecond — ünïcode\n\n")

    def test_outside_a_ci_step_nothing_is_written(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ, clear=True):
            summarize("unseen")
            self.assertEqual(list(Path(directory).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
