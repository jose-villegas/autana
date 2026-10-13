"""Regression tests for the clang-tidy finding counter."""
from pathlib import Path
import sys
import unittest

QUALITY = Path(__file__).resolve().parents[3] / "launcher/tools/quality"
sys.path.insert(0, str(QUALITY))

import misra_tidy_gate  # noqa: E402


class MisraTidyGateTest(unittest.TestCase):
    def test_combined_check_labels_are_counted_separately(self):
        source = QUALITY.parents[1] / "main/services/tune.c"
        output = (f"{source}:79:5: warning: ignored result "
                  "[bugprone-unused-return-value,cert-err33-c]")
        counts, _ = misra_tidy_gate.count_diagnostics(output)
        self.assertEqual(counts[("bugprone-unused-return-value", "main/services/tune.c")], 1)
        self.assertEqual(counts[("cert-err33-c", "main/services/tune.c")], 1)

    def test_a_package_source_counts_and_its_tests_and_other_trees_are_dropped(self):
        launcher = QUALITY.parents[1]
        check = "bugprone-unused-return-value"
        line = f"{{}}:7:3: warning: ignored result [{check}]"
        counted = ("packages/math/src/x.c", "main/y.c")
        dropped = ("packages/math/tests/suite_x.c", "test/suites/suite_y.c", "components/bsp/z.c")
        output = "\n".join(line.format(launcher / name) for name in (*counted, *dropped))
        counts, locations = misra_tidy_gate.count_diagnostics(output)
        self.assertEqual({(check, name): 1 for name in counted}, dict(counts))
        self.assertEqual({(check, name): 7 for name in counted}, locations)


if __name__ == "__main__":
    unittest.main()
