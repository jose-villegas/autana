"""check_no_soft_double.py on canned `objdump -dr` text: the exemption is the
function's own logging call, and a function's literal pool counts as it."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "build"))

import check_no_soft_double as gate  # noqa: E402

DUMP = """
00000000 <hot>:
   4:\tR_XTENSA_SLOT0_OP\t__muldf3
00000010 <logs_and_computes>:
   8:\tR_XTENSA_SLOT0_OP\t__adddf3
   c:\tR_XTENSA_SLOT0_OP\tesp_log
00000020 <hot_after_a_logger>:
   2:\tR_XTENSA_SLOT0_OP\t__divdf3
00000000 <.literal.literal_only>:
   0:\tR_XTENSA_32\t__extendsfdf2
00000000 <.literal.logger_literal>:
   0:\tR_XTENSA_32\t__floatsidf
   4:\tR_XTENSA_32\tsnprintf
00000030 <clean>:
   6:\tR_XTENSA_SLOT0_OP\t__divsf3
"""


class SoftDoubleGate(unittest.TestCase):
    def setUp(self):
        self.bad = gate.offenders(gate.parse_calls(DUMP))

    def test_a_soft_double_call_with_no_logging_fails(self):
        self.assertEqual(["__muldf3"], self.bad["hot"])

    def test_a_logging_call_exempts_only_its_own_function(self):
        self.assertNotIn("logs_and_computes", self.bad)
        self.assertEqual(["__divdf3"], self.bad["hot_after_a_logger"])

    def test_a_literal_pool_counts_toward_its_function(self):
        self.assertEqual(["__extendsfdf2"], self.bad["literal_only"])
        self.assertNotIn("logger_literal", self.bad)

    def test_single_precision_calls_are_not_soft_double(self):
        self.assertNotIn("clean", self.bad)

    def test_the_offenders_are_exactly_the_three(self):
        self.assertEqual({"hot", "hot_after_a_logger", "literal_only"}, set(self.bad))


if __name__ == "__main__":
    unittest.main()
