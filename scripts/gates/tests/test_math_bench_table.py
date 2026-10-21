"""Math measurements must join by identity and remain reproducible without an ELF."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
import unittest.mock

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "math_bench_table", ROOT / "launcher/tools/perf/math_bench_table.py")
bench = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = bench
SPEC.loader.exec_module(bench)
from icount_gate import INFO_MARKER


class MathBenchTableTests(unittest.TestCase):
    batch = 64
    rows = [("dot_f", "dot (vec3)", "f", batch), ("sinf_f", "sinf", "f", batch)]
    capture = ("BUILD_ID=abcdef-diag\n"
               "MATHBENCH name=dot (vec3) type=f ops=64 warm=12.0 cold=24.0 insn=6.0\n"
               "MATHBENCH name=sinf type=f ops=64 warm=90.0 cold=110.0 insn=30.0\n")

    def test_error_measurements_join_by_label_and_type(self):
        text = ("noise\nMATHERROR name=normalize (vec3) type=f max=0 unit=ulp method=sweep\n"
                "MATHERROR name=arbitrary label type=x max=5.94258e-05 unit=relative method=proof\n")
        errors = bench.parse_errors(text)
        self.assertEqual("0 ulp (sweep)", errors["normalize (vec3)", "f"])
        self.assertEqual("5.94258e-05 relative (proof)", errors["arbitrary label", "x"])
        rows = [("vector_f", "normalize (vec3)", "f", self.batch),
                ("vector_x", "normalize (vec3)", "x", self.batch)]
        table = bench.render_table(rows, {}, {}, {"vector_f": "-", "vector_x": "-"}, errors=errors)
        lines = table.splitlines()
        self.assertTrue(next(line for line in lines if line.startswith("| normalize (vec3) | f")).endswith("0 ulp (sweep) |"))
        self.assertTrue(next(line for line in lines if line.startswith("| normalize (vec3) | x")).endswith("- |"))

    def test_scalar_without_error_capture_has_no_invented_bound(self):
        rows = [("rsqrt_f", "mathf_rsqrt", "f", self.batch)]
        table = bench.render_table(rows, {}, {}, {"rsqrt_f": "rsqrt0.s"})
        self.assertTrue(next(line for line in table.splitlines() if line.startswith("| mathf_rsqrt |")).endswith("- |"))

    def test_malformed_and_duplicate_error_measurements_raise(self):
        good = "MATHERROR name=mathf_rsqrt type=f max=1 unit=ulp method=proof\n"
        for malformed in (good + good, good.replace("max=1", "max=inf"),
                          good.replace("max=1", "max=nan"), good.replace("max=1", "max=1e+"),
                          good.replace("max=1", "max=1e999"),
                          good.replace("max=1", "max=-1"), good.replace(" type=f", ""),
                          good.replace(" unit=ulp", ""), good.replace(" method=proof", "")):
            with self.subTest(text=malformed), self.assertRaises(ValueError):
                bench.parse_errors(malformed)

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
        table = bench.render_table(self.rows, measured, {"math_dot_f": 448},
                                   {"dot_f": "madd.s", "sinf_f": "-"}, build)
        self.assertIn("| dot (vec3) | f | 12.0 | 24.0 | 6.0 | 7.0 | 2.0 | madd.s |", table)
        self.assertEqual("abcdef-diag", build)
        with self.assertRaisesRegex(ValueError, "baseline"):
            bench.render_table(self.rows, {}, {"math_unknown_f": 1}, {})
        with self.assertRaisesRegex(ValueError, "unexpected"):
            bench.parse_capture(self.capture.replace("sinf", "unknown"), self.rows)

    def test_info_counts_and_device_only_instructions(self):
        rows, fpu = bench.read_fpu(bench.DATA / "math_bench_fpu_ops.txt")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "baseline.txt"
            path.write_text(f"{self.batch}\tmath_sinf_f\t{INFO_MARKER}\n")
            table = bench.render_table(rows, {}, bench.load_baseline(path), fpu)
        self.assertIn("| sinf | f | - | - | - | 1.0 |", table)
        for label in ("mathf_recip", "mathf_rsqrt"):
            self.assertIn(f"| {label} | f | - | - | - | {bench.DEVICE_ONLY_INSTRUCTION} |", table)

    def test_capture_identity_noise_and_placeholder(self):
        self.assertEqual((None, {}), bench.parse_capture(bench.PLACEHOLDER + "\n", self.rows))
        for text in (self.capture + "BUILD_ID=other\n", self.capture.replace("abcdef-diag", "bad id"),
                     self.capture.replace("insn=6.0", "insn=6.0 junk"),
                     bench.PLACEHOLDER + "\nnoise\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                bench.parse_capture(text, self.rows)
        self.assertEqual(bench.parse_capture(self.capture, self.rows),
                         bench.parse_capture("noise\n" + self.capture + "noise\n", self.rows))

    def test_block_check_and_write_preserve_newlines(self):
        for newline in ("\n", "\r\n"):
            with self.subTest(newline=newline), tempfile.TemporaryDirectory() as directory:
                doc = Path(directory) / "Benchmark.md"
                original = ("prose\n<!-- generated: math-benchmark sha256=0000000000000000000000000000000000000000000000000000000000000000 -->\nold\n"
                            "<!-- /generated: math-benchmark -->\nend\n").replace("\n", newline).encode()
                doc.write_bytes(original)
                with unittest.mock.patch.object(bench, "DOCUMENT", doc):
                    self.assertEqual(1, bench.main(["--check"]))
                    self.assertEqual(original, doc.read_bytes())
                    self.assertEqual(0, bench.main([]))
                    self.assertEqual(0, bench.main(["--check"]))
                    good = doc.read_bytes()
                    for corrupted in (good.replace(b"| function |", b"| changed |"),
                                      good.replace(b"check: python launcher/tools/perf/math_bench_table.py --check", b"sha256=" + b"0" * 64)):
                        doc.write_bytes(corrupted)
                        self.assertEqual(1, bench.main(["--check"]))
                        self.assertEqual(corrupted, doc.read_bytes())
                    self.assertTrue(good.startswith(b"prose" + newline.encode()))
                    self.assertNotIn(b"\n", good.replace(newline.encode(), b""))
        self.assertEqual(0, bench.main(["--check"]))

    def test_disassembly_uses_only_requested_wrapper_symbols(self):
        text = ("42000000 <math_bench_dot_f>:\n  42000000: 123456 madd.s f0, f1, f2\n"
                "42000003 <unrelated>:\n  42000003: 123456 div0.s f0, f1\n"
                "42000006 <math_bench_sinf_f>:\n  42000006: 123456 call8 42001000 <sinf>\n")
        self.assertEqual({"dot_f": "madd.s", "sinf_f": "calls sinf"},
                         bench.parse_disassembly(text, self.rows))
        with self.assertRaisesRegex(ValueError, "symbol"):
            bench.parse_disassembly("", self.rows)

    def test_operation_counts_come_from_each_row(self):
        rows = [("dot_f", "dot (vec3)", "f", 2), ("dot_f32_n256_f", "dot_f32_n256", "f", 1)]
        table = bench.render_table(rows, {}, {"math_dot_f": 7, "math_dot_f32_n256_f": 13},
                                   {"dot_f": "-", "dot_f32_n256_f": "-"})
        self.assertIn("| dot (vec3) | f | - | - | - | 3.5 |", table)
        self.assertIn("| dot_f32_n256 | f | - | - | - | 13.0 |", table)
        capture = "BUILD_ID=abc\nMATHBENCH name=dot_f32_n256 type=f ops=1 warm=20 cold=30 insn=10\n"
        _, measured = bench.parse_capture(capture, rows[1:])
        self.assertIn("1.5%", bench.render_table(rows[1:], measured, {}, {"dot_f32_n256_f": "-"}))

    def test_fpu_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fpu.txt"
            good = "dot_f\tdot (vec3)\tf\t64\tmadd.s\n"
            for text, error in ((good + good, "duplicate"), (good.replace("madd.s", "unknown"), "FPU"),
                                ("no tab", "tab")):
                path.write_text(text)
                with self.subTest(text=text), self.assertRaisesRegex(ValueError, error):
                    bench.read_fpu(path)

    def test_disassembly_edges(self):
        for body, expected in (("l32r a8, 10 <pool> (20 <callee>)\nmov.n a8, a2\ncallx8 a8", "calls *a8"),
                               ("callx8 a9", "calls *a9"), ("nop.n", "-"), ("notmadd.s f0, f1", "-"),
                               ("call8 10 <z>\ncall8 20 <a>\ncall8 10 <z>", "calls a, z"),
                               ("madd.s f0, f1\n42000007 <math_bench_dot_f+0x7>:\nmsub.s f0, f1", "madd.s msub.s")):
            code = "42000000 <math_bench_dot_f>:\n" + "\n".join(
                line if "<math_bench_dot_f+" in line else f"42000000: 123456 {line}"
                for line in body.splitlines())
            code += "\n42000020 <math_bench_dot_f32_n256_f>:\n42000020: 123456 div0.s f0, f1\n"
            with self.subTest(body=body):
                self.assertEqual({"dot_f": expected}, bench.parse_disassembly(code, self.rows[:1]))

    def test_from_elf(self):
        with tempfile.TemporaryDirectory() as directory:
            elf, fpu = Path(directory) / "test.elf", Path(directory) / "fpu.txt"
            elf.write_bytes(b"elf")
            manifest = b"sinf_f\tsinf\tf\t64\ndot_f\tdot (vec3)\tf\t2\n\0"
            dump = "1000 " + manifest.hex() + "  ignored ascii\n"
            disassembly = ("2000 <math_bench_sinf_f>:\n2000: 12 nop.n\n"
                           "2010 <math_bench_dot_f>:\n2010: 12 madd.s f0, f1\n")
            located = {4096: ("math_bench_manifest", len(manifest)),
                       8192: ("math_bench_sinf_f", 2), 8208: ("math_bench_dot_f", 2)}
            def run(tool, *args):
                if tool == "nm":
                    return "\n".join(f"{address:x} {size:x} r {name}" for address, (name, size) in located.items())
                return dump if "-s" in args else disassembly
            with unittest.mock.patch.object(bench, "run", side_effect=run), \
                 unittest.mock.patch.dict(bench.instructions.__globals__, run=run), \
                 unittest.mock.patch.object(bench, "find_tool", side_effect=lambda name: name):
                args = ["--from-elf", str(elf), "--fpu", str(fpu)]
                self.assertEqual(0, bench.main(args))
                first = fpu.read_bytes()
                self.assertIn(b"# ELF sha256=" + bench.hashlib.sha256(b"elf").hexdigest().encode(), first)
                self.assertNotIn(b"\r", first)
                self.assertLess(first.index(b"dot_f\t"), first.index(b"sinf_f\t"))
                self.assertEqual(0, bench.main(args))
                self.assertEqual(first, fpu.read_bytes())
                del located[8208]
                with self.assertRaisesRegex(SystemExit, "1"):
                    bench.main(args)

    def test_indirect_calls_resolve_loaded_symbol_names(self):
        text = ("42000000 <math_bench_dot_f>:\n"
                "42000000: 123456 l32r a8, 42001000 <literal> (40002274 <__divsf3>)\n"
                "42000003: 123456 callx8 a8\n"
                "42000006: 123456 l32r a8, 42001004 <literal> (42100000 <sqrtf>)\n"
                "42000009: 123456 callx8 a8\n"
                "4200000c: 123456 madd.s f0, f1, f2\n")
        result = bench.parse_disassembly(text, [self.rows[0]])
        self.assertEqual({"dot_f": "madd.s; calls __divsf3, sqrtf"}, result)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fpu.txt"
            path.write_text("dot_f\tdot (vec3)\tf\t64\t" + result["dot_f"] + "\n", encoding="utf-8")
            self.assertEqual(result, bench.read_fpu(path)[1])

    def test_redecodes_code_entering_a_literal_pool(self):
        text = ("42000000 <math_bench_dot_f>:\n"
                "42000000: 123456 bnez a2, 42000007 <math_bench_dot_f+0x7>\n"
                "42000003: 123456 j 42000010 <math_bench_dot_f+0x10>\n"
                "42000006: 123456 msub.s f0, f1, f2\n"
                "42000009: 12 .byte 0x12\n"
                "42000010: 123456 madd.s f0, f1, f2\n")

        def resolve(symbol, body):
            self.assertEqual("math_bench_dot_f", symbol)
            return bench.parse_objdump(body), lambda start: [(start, 3, "add.n", "a2, a2, a3")]

        self.assertEqual({"dot_f": "madd.s"},
                         bench.parse_disassembly(text, [self.rows[0]], resolve=resolve))


if __name__ == "__main__":
    unittest.main()
