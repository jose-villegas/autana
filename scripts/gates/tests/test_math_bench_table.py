"""Math measurements must join by identity and remain reproducible without an ELF."""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "math_bench_table", ROOT / "launcher/tools/perf/math_bench_table.py")
bench = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = bench
SPEC.loader.exec_module(bench)


class MathBenchTableTests(unittest.TestCase):
    batch = bench.read_define(bench.SUITE, "MATH_BENCH_BATCH")
    rows = [("dot", "f", True), ("sinf", "f", False)]
    capture = ("BUILD_ID=abcdef-diag\n"
               "MATHBENCH name=dot type=f warm=12.0 cold=24.0 insn=6.0\n"
               "MATHBENCH name=sinf type=f warm=90.0 cold=110.0 insn=30.0\n")

    def test_missing_row_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            bench.parse_capture(self.capture.split("MATHBENCH name=sinf")[0], self.rows)

    def test_malformed_and_duplicate_lines_are_errors(self):
        for text in (self.capture.replace("12.0", "nan"),
                     self.capture.replace("insn=6.0", "insn=0.0"),
                     self.capture + self.capture.splitlines()[1] + "\n",
                     self.capture.replace("cold=24.0", "cold=oops"),
                     self.capture.replace("BUILD_ID=abcdef-diag\n", "")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                bench.parse_capture(text, self.rows)

    def test_ratchet_join_and_cycles_per_instruction(self):
        build, measured = bench.parse_capture(self.capture, self.rows)
        table = bench.render_table(self.rows, measured, {"math_dot_f": 7 * self.batch, "math_sinf_f": 999},
                                   {"dot_f": "madd.s", "sinf_f": "-"})
        self.assertIn("| dot (vec3) | f | 12.0 | 24.0 | 6.0 | 7 | 2.00 | madd.s |", table)
        self.assertIn("| sinf | f | 90.0 | 110.0 | 30.0 | - | 3.00 | - |", table)
        self.assertEqual("abcdef-diag", build)
        with self.assertRaisesRegex(ValueError, "baseline"):
            bench.render_table(self.rows, measured, {}, {"dot_f": "-", "sinf_f": "-"})

    def test_placeholder_is_explicit_and_deterministic(self):
        build, measured = bench.parse_capture(bench.PLACEHOLDER + "\n", self.rows)
        self.assertIsNone(build)
        first = bench.render_document(self.rows, build, measured, {"math_dot_f": 7 * self.batch},
                                      {"dot_f": "madd.s", "sinf_f": "-"})
        second = bench.render_document(list(self.rows), build, dict(measured), {"math_dot_f": 7 * self.batch},
                                       {"sinf_f": "-", "dot_f": "madd.s"})
        self.assertEqual(first, second)
        self.assertIn("Board capture pending", first)
        self.assertIn("sha256=", first)

    def test_disassembly_uses_only_requested_wrapper_symbols(self):
        text = ("42000000 <math_bench_dot_f>:\n  42000000: 123456 madd.s f0, f1, f2\n"
                "42000003 <unrelated>:\n  42000003: 123456 div0.s f0, f1\n"
                "42000006 <math_bench_sinf_f>:\n  42000006: 123456 call8 42001000 <sinf>\n")
        self.assertEqual({"dot_f": "madd.s", "sinf_f": "-"},
                         bench.parse_disassembly(text, self.rows))
        with self.assertRaisesRegex(ValueError, "symbol"):
            bench.parse_disassembly("", self.rows)

    def test_redecodes_code_entering_a_literal_pool(self):
        text = ("42000000 <math_bench_dot_f>:\n"
                "42000000: 123456 bnez a2, 42000007 <math_bench_dot_f+0x7>\n"
                "42000003: 123456 j 42000010 <math_bench_dot_f+0x10>\n"
                "42000006: 123456 msub.s f0, f1, f2\n"
                "42000009: 12 .byte 0x12\n"
                "42000010: 123456 madd.s f0, f1, f2\n")

        def resolve(symbol, body):
            self.assertEqual("math_bench_dot_f", symbol)
            return bench.decode(text=body), lambda start: [(start, 3, "add.n", "a2, a2, a3")]

        self.assertEqual({"dot_f": "madd.s"},
                         bench.parse_disassembly(text, [self.rows[0]], resolve=resolve))


if __name__ == "__main__":
    unittest.main()
