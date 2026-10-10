"""check_no_soft_double.py on canned `objdump -dr` text: the exemption is the
function's own logging call, and a function's literal pool counts as it."""

import pathlib
import sys
import tempfile
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


class ObjectsOfTheFirmware(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.build = pathlib.Path(temp.name, "build")
        self.launcher = pathlib.Path(temp.name, "launcher")
        (self.launcher / "packages/math/include").mkdir(parents=True)
        (self.launcher / "packages/docs_only").mkdir()

    def touch(self, component, name):
        path = self.build / "esp-idf" / component / "CMakeFiles" / f"__idf_{component}.dir" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        return path

    def test_main_and_each_package_with_an_include_folder_are_collected(self):
        wanted = [self.touch("main", "a.c.obj"), self.touch("math", "src/b.c.obj")]
        self.touch("docs_only", "c.c.obj")
        self.touch("esp_lcd", "d.c.obj")
        self.assertEqual(sorted(wanted), gate.objects(self.build, self.launcher))

    def test_no_objects_says_to_build_the_firmware_first(self):
        self.touch("esp_lcd", "d.c.obj")
        with self.assertRaises(SystemExit) as stop:
            gate.objects(self.build, self.launcher)
        self.assertIn("build the firmware first", str(stop.exception))


if __name__ == "__main__":
    unittest.main()
