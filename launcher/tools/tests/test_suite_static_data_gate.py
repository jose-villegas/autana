"""Tests for the diagnostics suite static-data gate.

    python -m unittest discover -s launcher/tools/tests
"""
import subprocess
import sys
import unittest
from pathlib import Path


GATE = Path(__file__).resolve().parents[1] / "quality" / "suite_static_data_gate.py"


def run_gate(csv_text):
    return subprocess.run(
        [sys.executable, str(GATE)], input=csv_text, text=True, capture_output=True, check=False
    )


class SuiteStaticDataGateTest(unittest.TestCase):
    def test_missing_suite_rows_fails(self):
        result = run_gate("Object File,.bss,.data\nmain.c.obj,0,0\n")

        self.assertEqual(1, result.returncode)
        self.assertIn("no suite object rows", result.stdout)


if __name__ == "__main__":
    unittest.main()
