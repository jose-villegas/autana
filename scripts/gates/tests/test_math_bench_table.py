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
    rows = [("dot_f", "dot (vec3)", "f", batch, "vector"), ("sinf_f", "sinf", "f", batch, "libm")]
    capture = ("BUILD_ID=abcdef-diag\n"
               "MATHBENCH name=dot (vec3) type=f ops=64 warm=12.0 cold=24.0 insn=6.0\n"
               "MATHBENCH name=sinf type=f ops=64 warm=90.0 cold=110.0 insn=30.0\n")

    def test_package_capture_prefix_and_placeholder(self):
        rows = [("crc32_update_rom_64_ram", "crc32_update_rom", "64_ram", 1, "hash")]
        capture = "BUILD_ID=example\nHASHBENCH name=crc32_update_rom type=64_ram ops=1 warm=2.0 cold=3.0 insn=4.0\n"
        build, measured = bench.parse_capture(capture, rows, prefix="HASHBENCH", placeholder="pending")
        self.assertEqual(measured[rows[0][0]], (2.0, 3.0, 4.0))
        self.assertEqual(bench.parse_capture("pending", rows, prefix="HASHBENCH", placeholder="pending"), (None, {}))
        for bad in (capture + capture.splitlines()[-1] + "\n", capture.replace("ops=1", "ops=2")):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                bench.parse_capture(bad, rows, prefix="HASHBENCH")

    def test_error_measurements_join_by_label_and_type(self):
        text = ("noise\nMATHERROR name=normalize (vec3) type=f max=0 unit=ulp method=sweep\n"
                "MATHERROR name=arbitrary label type=x max=5.94258e-05 unit=relative method=proof\n")
        errors = bench.parse_errors(text)
        self.assertEqual("0 ulp (sweep)", errors["normalize (vec3)", "f"])
        self.assertEqual("5.94258e-05 relative (proof)", errors["arbitrary label", "x"])

    def test_hex_error_maxima_are_exact_for_all_readers(self):
        for token in ('0x1p-14', '0X1.FFFFFEp-15', '0x0p+0'):
            with self.subTest(token=token):
                text = (f'MATHERROR name=example type=f max={token} unit=relative method=proof\n'
                        f'MATHERRORBIN name=example type=f x=1.5 max={token} unit=relative axis=period\n')
                maximum = float.fromhex(token)
                self.assertEqual(float(bench.parse_errors(text)['example', 'f'].split()[0]), maximum)
                self.assertEqual(bench.parse_bins(text)['example', 'f'], [(1.5, maximum, 'relative')])
        for token in ('0x1p+', '0x1', '0x1p+99999', '-0x1p-14'):
            with self.subTest(token=token), self.assertRaises(ValueError):
                bench.parse_errors(f'MATHERROR name=example type=f max={token} unit=relative method=proof')

    def test_scalar_table_links_rendered_error(self):
        rows = [("rsqrt_f", "mathf_rsqrt", "f", self.batch, "scalar")]
        table = bench.render_table(rows, {}, {}, {"rsqrt_f": "rsqrt0.s"},
                                   errors={("mathf_rsqrt", "f"): "1 ulp (proof)"},
                                   error_links={"rsqrt_f": "../packages/Fast-Paths.md#mathf-rsqrt"})
        self.assertIn("| mathf_rsqrt | f | - | - | - | device-only instruction | - | rsqrt0.s | [fast paths](../packages/Fast-Paths.md#mathf-rsqrt) |", table)
        self.assertIn("max error", table)

    def test_grouped_table_keeps_errors_not_rendered_in_fast_paths(self):
        links = {"dot_f": "../packages/Fast-Paths.md#example", "sinf_f": "../packages/Fast-Paths.md#example"}
        table = bench.render_table(self.rows, {}, {}, {"dot_f": "-", "sinf_f": "-"}, links=links,
                                   errors={("dot (vec3)", "f"): "1 ulp (sweep)"})
        self.assertLess(table.index("#### Vector"), table.index("#### Libm"))
        self.assertIn("\n\n#### Libm\n", table)
        self.assertIn("[dot (vec3)](../packages/Fast-Paths.md#example)", table)
        self.assertIn("[sinf](../packages/Fast-Paths.md#example)", table)
        self.assertIn("| 1 ulp (sweep) |", table)
        self.assertIn("max error", table)

    def test_malformed_and_duplicate_error_measurements_raise(self):
        good = "MATHERROR name=mathf_rsqrt type=f max=1 unit=ulp method=proof\n"
        bad_max = ("inf", "nan", "1e+", "1e999", "-1")
        missing = ("type", "unit", "method")
        texts = [good + good, *(good.replace("max=1", "max=" + value) for value in bad_max),
                 *(" ".join(part for part in good.split(" ") if not part.startswith(key + "=")) for key in missing)]
        for text in texts:
            with self.subTest(text=text), self.assertRaises(ValueError):
                bench.parse_errors(text)

    def test_separate_bench_and_error_build_provenance(self):
        text = self.capture.replace("BUILD_ID=abcdef-diag", "BUILD_ID=abcdef-diag scope=bench")
        text += "BUILD_ID=123456-diag scope=error\nMATHERROR name=mathf_recip type=f max=1 unit=ulp method=sweep\n"
        build, measured = bench.parse_capture(text, self.rows)
        self.assertEqual("abcdef-diag", build)
        self.assertEqual(2, len(measured))
        for invalid in (text.replace(" scope=error", ""),
                        text.replace("scope=error", "scope=bench"),
                        text.replace("scope=error", "scope=unknown"),
                        text.split("BUILD_ID=123456")[0],
                        text + "BUILD_ID=abcdef-diag scope=bench\n"):
            with self.subTest(text=invalid), self.assertRaises(ValueError):
                bench.parse_capture(invalid, self.rows)

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
                                   {"dot_f": "madd.s", "sinf_f": "-"})
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

    def test_document_commands_are_rejected_without_writing(self):
        for newline in ("\n", "\r\n"):
            with self.subTest(newline=newline), tempfile.TemporaryDirectory() as directory:
                doc = Path(directory) / "Benchmark.md"
                original = ("prose\n<!-- generated: math-benchmark check: python launcher/tools/perf/fast_paths.py --check -->\n"
                            "old\n<!-- /generated: math-benchmark -->\nend\n").replace("\n", newline).encode()
                doc.write_bytes(original)
                with unittest.mock.patch.object(bench, "DOCUMENT", doc, create=True):
                    for args in ([], ["--check"]):
                        with self.subTest(args=args):
                            with self.assertRaises(SystemExit) as error:
                                bench.main(args)
                            self.assertEqual(2, error.exception.code)
                            self.assertEqual(original, doc.read_bytes())

    def test_disassembly_uses_only_requested_wrapper_symbols(self):
        text = ("42000000 <math_bench_dot_f>:\n  42000000: 123456 madd.s f0, f1, f2\n"
                "42000003 <unrelated>:\n  42000003: 123456 div0.s f0, f1\n"
                "42000006 <math_bench_sinf_f>:\n  42000006: 123456 call8 42001000 <sinf>\n")
        self.assertEqual({"dot_f": "madd.s", "sinf_f": "calls sinf"},
                         bench.parse_disassembly(text, self.rows))
        with self.assertRaisesRegex(ValueError, "symbol"):
            bench.parse_disassembly("", self.rows)

    def test_operation_counts_come_from_each_row(self):
        rows = [("dot_f", "dot (vec3)", "f", 2, "vector"), ("dot_f32_n256_f", "dot_f32_n256", "f", 1, "calibration")]
        table = bench.render_table(rows, {}, {"math_dot_f": 7, "math_dot_f32_n256_f": 13},
                                   {"dot_f": "-", "dot_f32_n256_f": "-"})
        self.assertIn("| dot (vec3) | f | - | - | - | 3.5 |", table)
        self.assertIn("| dot_f32_n256 | f | - | - | - | 13.0 |", table)
        capture = "BUILD_ID=abc\nMATHBENCH name=dot_f32_n256 type=f ops=1 warm=20 cold=30 insn=10\n"
        _, measured = bench.parse_capture(capture, rows[1:])
        self.assertIn("1.5%", bench.render_table(rows[1:], measured, {}, {"dot_f32_n256_f": "-"}))

    def test_row_family_is_required_and_validated(self):
        for family in bench.ROW_FAMILIES:
            with self.subTest(family=family):
                self.assertEqual(("dot_f", "dot (vec3)", "f", 64, family),
                                 bench.parse_row(f"dot_f\tdot (vec3)\tf\t64\t{family}"))
        for family in ("unknown", "Vector", ""):
            with self.subTest(family=family), self.assertRaisesRegex(ValueError, "family"):
                bench.parse_row(f"dot_f\tdot (vec3)\tf\t64\t{family}")
        with self.assertRaisesRegex(ValueError, "tab-separated"):
            bench.parse_row("dot_f\tdot (vec3)\tf\t64")

    def test_fpu_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fpu.txt"
            good = "dot_f\tdot (vec3)\tf\t64\tvector\tmadd.s\n"
            for text, error in ((good + good, "duplicate"), (good.replace("madd.s", "unknown"), "FPU"),
                                (good.replace("vector", "unknown"), "family"),
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
            manifest = b"sinf_f\tsinf\tf\t64\tlibm\ndot_f\tdot (vec3)\tf\t2\tvector\n\0"
            dump = "1000 " + manifest.hex() + "  ignored ascii\n"
            disassembly = ("42002000 <math_bench_sinf_f>:\n42002000: 12 nop.n\n"
                           "42002010 <math_bench_dot_f>:\n42002010: 12 madd.s f0, f1\n")
            located = {4096: ("math_bench_manifest", len(manifest)),
                       0x42002000: ("math_bench_sinf_f", 2), 0x42002010: ("math_bench_dot_f", 2)}
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
                self.assertIn(b"dot_f\tdot (vec3)\tf\t2\tvector\tmadd.s", first)
                self.assertIn(b"sinf_f\tsinf\tf\t64\tlibm\t-", first)
                self.assertLess(first.index(b"dot_f\t"), first.index(b"sinf_f\t"))
                self.assertEqual(0, bench.main(args))
                self.assertEqual(first, fpu.read_bytes())
                del located[0x42002010]
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
            path.write_text("dot_f\tdot (vec3)\tf\t64\tvector\t" + result["dot_f"] + "\n", encoding="utf-8")
            self.assertEqual(result, bench.read_fpu(path)[1])

    def test_call_targets_must_be_symbol_starts(self):
        text = ("42000000 <math_bench_dot_f>:\n"
                "42000000: 123456 call8 4200100c <unrelated+0xc>\n")
        with self.assertRaisesRegex(ValueError, "call target"):
            bench.parse_disassembly(text, self.rows[:1])
        text = text.replace("call8 4200100c <unrelated+0xc>",
                            "l32r a8, 42001000 <pool> (40002278 <__divdi3+0x4>)")
        text += "42000003: 123456 callx8 a8\n"
        self.assertEqual({"dot_f": "calls *a8"}, bench.parse_disassembly(text, self.rows[:1]))


if __name__ == "__main__":
    unittest.main()
