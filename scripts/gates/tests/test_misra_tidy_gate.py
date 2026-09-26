"""Regression tests for the clang-tidy finding counter."""
from pathlib import Path
import sys
import unittest

QUALITY = Path(__file__).resolve().parents[3] / "launcher/tools/quality"
sys.path.insert(0, str(QUALITY))

import misra_tidy_gate  # noqa: E402


class MisraTidyGateTest(unittest.TestCase):
    def test_combined_check_labels_are_counted_separately(self):
        source = QUALITY.parents[1] / "main/util/tune.c"
        output = (f"{source}:79:5: warning: ignored result "
                  "[bugprone-unused-return-value,cert-err33-c]")
        counts, _ = misra_tidy_gate.count_diagnostics(output)
        self.assertEqual(counts[("bugprone-unused-return-value", "main/util/tune.c")], 1)
        self.assertEqual(counts[("cert-err33-c", "main/util/tune.c")], 1)


if __name__ == "__main__":
    unittest.main()
