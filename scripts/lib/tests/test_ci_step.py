"""A CI step's summary takes each entry as a paragraph, its outputs one name=value line each; outside a CI step
nothing is written."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ci_step import output, summarize  # noqa: E402


class CiStepTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def test_each_summary_entry_is_a_paragraph_of_its_own(self):
        summary = self.root / "summary"
        with mock.patch.dict(os.environ, GITHUB_STEP_SUMMARY=str(summary)):
            summarize("first")
            summarize("second — ünïcode")
        self.assertEqual(summary.read_text(encoding="utf-8"), "first\n\nsecond — ünïcode\n\n")

    def test_outputs_are_name_value_lines_and_bools_read_as_workflow_expressions_do(self):
        outputs = self.root / "output"
        with mock.patch.dict(os.environ, GITHUB_OUTPUT=str(outputs)):
            output({"enabled": True, "build-development": "false"})
            output({"path": self.root / "manifest.json", "ran": False})
        self.assertEqual(outputs.read_text(encoding="utf-8"),
                         f"enabled=true\nbuild-development=false\npath={self.root / 'manifest.json'}\nran=false\n")

    def test_outside_a_ci_step_nothing_is_written(self):
        with mock.patch.dict(os.environ, clear=True):
            summarize("unseen")
            output({"unseen": True})
        self.assertEqual(list(self.root.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
