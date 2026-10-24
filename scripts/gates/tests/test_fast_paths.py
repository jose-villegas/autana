"""Fast-path documentation rejects missing source and measurement evidence."""
import shutil
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'launcher/tools/perf'))
import math_bench_table as bench
import fast_paths as paths
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/lib"))
from test_assertions import assert_contains


class FastPathTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.header = self.root / 'launcher/packages/math/include/math/scalar/example.h'
        self.header.parent.mkdir(parents=True)
        self.header.write_text('#define EXAMPLE_LIMIT 1\n'
                               '/* Seed correction. | y = y(2 - ay) | positive normal inputs */\n'
                               '/* MATH_FAST_PATH(fast_f, plain_f, EXAMPLE_LIMIT) */\n'
                               'static inline float example(float x) { return x; }\n')
        source = self.root / 'launcher/packages/math/tests/suite_math_numbers.c'
        source.parent.mkdir(parents=True)
        source.write_text('MATHERROR name=example type=f max=%g unit=ulp method=sweep')
        perf = self.root / 'launcher/test/suites/suite_math_perf.c'
        perf.parent.mkdir(parents=True)
        perf.write_text('')
        self.annotations = paths.annotations(self.root)
        self.rows = [('fast_f', 'example', 'f', 64, 'scalar'), ('plain_f', 'plain', 'f', 64, 'scalar')]
        self.capture = ('MATHERROR name=example type=f max=1 unit=ulp method=sweep\n'
                        'MATHERRORBIN name=example type=f x=2 max=1 unit=ulp\n')

    def test_annotation_parser(self):
        self.assertEqual(len(self.annotations), 1)
        item = self.annotations[0]
        self.assertEqual((item.key, item.fast, item.plain, item.limit), ('example', 'fast_f', 'plain_f', 1))
        paths.validate(self.annotations, self.rows, self.capture)

    def test_limit_requires_a_nonnegative_constant_macro(self):
        original = self.header.read_text()
        for definition in ('', '#define EXAMPLE_LIMIT -1', '#define EXAMPLE_LIMIT nan',
                           '#define EXAMPLE_LIMIT runtime_value', '#define EXAMPLE_LIMIT 1 + runtime_value'):
            with self.subTest(definition=definition), self.assertRaises(ValueError):
                self.header.write_text(original.replace('#define EXAMPLE_LIMIT 1', definition))
                paths.annotations(self.root)

    def test_literal_limit_matches_test_constant_and_unit(self):
        self.header.write_text(self.header.read_text().replace(
            '#define EXAMPLE_LIMIT 1\n', '').replace('EXAMPLE_LIMIT)', 'EXAMPLE_LIMIT=1 ulp)'))
        source = self.root / 'launcher/packages/math/tests/suite_math_numbers.c'
        original = source.read_text()
        source.write_text('#define EXAMPLE_LIMIT 1\n' + original)
        self.assertEqual(paths.annotations(self.root)[0].limit, 1)
        for definition in ('#define EXAMPLE_LIMIT 2\n', '#define EXAMPLE_LIMIT -1\n', ''):
            with self.subTest(definition=definition), self.assertRaises(ValueError):
                source.write_text(definition + original)
                paths.annotations(self.root)
        source.write_text('#define EXAMPLE_LIMIT 1\n' + original)
        self.header.write_text(self.header.read_text().replace('=1 ulp)', '=1 relative)'))
        with self.assertRaises(ValueError):
            paths.annotations(self.root)

    def test_section_missing_and_extra(self):
        for text in ('', '## example\n## unexpected\n'):
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, 'section'):
                paths.validate_sections(text, self.annotations)
        paths.validate_sections('## example\n', self.annotations)

    def test_missing_plain_row(self):
        with self.assertRaisesRegex(ValueError, 'plain_f'):
            paths.validate(self.annotations, self.rows[:1], '')

    def test_limit_exceeded(self):
        with self.assertRaisesRegex(ValueError, 'EXAMPLE_LIMIT'):
            paths.validate(self.annotations, self.rows, self.capture.replace('max=1', 'max=2'))

    def test_missing_summary_or_bins(self):
        for capture in (self.capture.splitlines()[1],):
            with self.subTest(capture=capture), self.assertRaisesRegex(ValueError, 'missing'):
                paths.validate(self.annotations, self.rows, capture)

    def test_missing_reference_link(self):
        doc = self.root / 'docs/math/Scalar.md'
        doc.parent.mkdir(parents=True)
        doc.write_text('Scalar\n')
        with self.assertRaisesRegex(ValueError, 'Scalar'):
            paths.validate_links(self.root, self.annotations)
        doc.write_text('[example](Fast-Paths.md#example)\n')
        paths.validate_links(self.root, self.annotations)
        moved = self.header.parent.parent / 'linear/example.h'
        moved.parent.mkdir()
        self.header.rename(moved)
        with self.assertRaisesRegex(ValueError, 'Linear'):
            paths.validate_links(self.root, paths.annotations(self.root))

    def test_host_error_evidence_is_separate_and_labelled(self):
        capture = self.root / paths.HOST_CAPTURE
        capture.parent.mkdir(parents=True)
        capture.write_text("MATHTARGET target=host arch=x86_64 execution=host\n" + self.capture)
        errors, bins, sources = paths.error_evidence(self.root, self.annotations, self.rows, "")
        self.assertEqual("host", sources["example", "f"])
        self.assertEqual("1 ulp (sweep)", errors["example", "f"])
        self.assertEqual([(2.0, 1.0, "ulp")], bins["example", "f"])
        with self.assertRaisesRegex(ValueError, "duplicate"):
            paths.error_evidence(self.root, self.annotations, self.rows, self.capture)
        capture.write_text(self.capture)
        with self.assertRaisesRegex(ValueError, "target"):
            paths.error_evidence(self.root, self.annotations, self.rows, "")

    def test_error_bins_are_strict(self):
        self.assertEqual(bench.parse_bins(self.capture)[('example', 'f')], [(2.0, 1.0, 'ulp')])
        for capture in (self.capture + self.capture.splitlines()[1] + '\n',
                        self.capture.replace('x=2', 'x=nan'), self.capture.replace('unit=ulp', 'unit=unknown')):
            with self.subTest(capture=capture), self.assertRaises(ValueError):
                bench.parse_bins(capture)

    def test_short_annotation_and_source_contract(self):
        self.assertIsNotNone(paths.ANNOTATION.fullmatch('/* MATH_FAST_PATH(fast_f, plain_f, LIMIT) */'))
        source = self.root / 'launcher/packages/math/tests/suite_math_numbers.c'
        source.write_text('')
        with self.assertRaisesRegex(ValueError, 'MATHERROR'):
            paths.annotations(self.root)

    def test_target_wrapper_requires_its_hardware_instruction(self):
        self.header.write_text(self.header.read_text().replace('return x;', '\n#if defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP\nreturn x;\n#endif\n'))
        config = self.root / 'launcher/packages/math/Kconfig'
        config.write_text('    config MATH_FPU_RECIP\n        help\n            Use recip0.s and rsqrt0.s.\n\nendmenu\n')
        items = paths.annotations(self.root)
        with self.assertRaisesRegex(ValueError, 'instruction'):
            paths.validate_hardware(self.root, items, {'fast_f': 'calls __divsf3'})
        paths.validate_hardware(self.root, items, {'fast_f': 'recip0.s'})

    def test_transitive_template_callers_and_tables(self):
        self.header.unlink()
        shutil.copyfile(ROOT / 'launcher/packages/math/tests/suite_math_numbers.c', self.root / 'launcher/packages/math/tests/suite_math_numbers.c')
        shutil.copytree(ROOT / paths.HEADERS, self.root / paths.HEADERS, dirs_exist_ok=True)
        caller = self.root / 'launcher/main/render/use.c'
        caller.parent.mkdir(parents=True)
        caller.write_text('void draw(void) { quatf_nlerp(a, b, t); vec3x_normalize(v); quatx_nlerp(a, b, t); }\n')
        self.assertEqual(paths.callers(self.root, 'mathf_rsqrt')['render'], {'quatf_nlerp'})
        self.assertEqual(paths.callers(self.root, 'mathx_rsqrt_q30')['render'],
                         {'vec3x_normalize', 'quatx_nlerp'})
        self.assertEqual(set(paths.callers(self.root, 'mathx_rsqrt_q30')), {'render'})
        item = next(item for item in paths.annotations(self.root) if item.key == 'mathx_rsqrt_q30')
        self.assertEqual(paths.tables_for(item, self.root), ['mathx_rsqrt_seeds'])

    def test_target_capture_requires_hardware_and_one_identity(self):
        good = 'MATHTARGET target=esp32s3 arch=Xtensa execution=hardware\n'
        self.assertEqual(bench.parse_target(good), 'esp32s3 (Xtensa)')
        self.assertEqual(bench.parse_target(good.replace(' execution', ' core=LX7 execution')), 'esp32s3 (Xtensa, LX7)')
        for bad in (good.replace('hardware', 'host'), good + good.replace('esp32s3', 'other'),
                    good.replace(' execution=', ' mode=')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                bench.parse_target(bad)

    def test_elf_sizes_keep_aliases_and_classify_sections(self):
        from code_layout import symbols
        from unittest.mock import patch
        nm = "1000 c T math_bench_fast_f\n1000 c T alias\n2000 A __divsf3\n"
        with patch('code_layout.run', return_value=nm):
            located = symbols('nm', 'elf', records=True)
        self.assertEqual(len(located), 3)
        from dataclasses import replace
        rows = self.rows
        second = replace(self.annotations[0], fast='plain_f')
        nm += "3000 c T math_bench_plain_f\n4000 10 D seeds\n"
        with patch('code_layout.run', return_value=nm):
            located = symbols('nm', 'elf', records=True)
        with patch.object(paths, 'annotations', return_value=[*self.annotations, second]), \
             patch.object(paths, 'tables_for', return_value=['seeds']):
            output = paths.elf_sizes(rows, {'fast_f': 'calls __divsf3', 'plain_f': '-'}, located,
                                     [('.iram0.text', 0x1000, 0x3000), ('.flash.rodata', 0x4000, 0x100),
                                      ('.ext_ram.dummy', 0x4000, 0x100)], self.root)
        sizes = paths.parse_sizes(output)
        self.assertIn(('math_bench_fast_f', 12, 'IRAM'), sizes['fast_f'])
        self.assertIn(('__divsf3', None, 'ROM'), sizes['fast_f'])
        for key in ('fast_f', 'plain_f'):
            self.assertIn(('seeds', 16, 'rodata'), sizes[key])

    def test_compact_tables_and_formula_details(self):
        items = paths.annotations()
        rows, fpu = bench.read_fpu(ROOT / "docs/math/data/math_bench_fpu_ops.txt")
        sizes = paths.parse_sizes((ROOT / "docs/math/data/math_bench_sizes.txt").read_text())
        measured = {row[0]: (10.0, 20.0, 5.0) for row in rows}
        measured["div_recip_f"] = (25.0, 40.0, 15.0)
        errors = {(item.key, item.kind): "0 ulp" for item in items}
        bins = {(item.key, item.kind): [(1.0, 0.0, item.unit)] for item in items}
        text = paths.render(ROOT, items, rows, measured, errors, bins, sizes, fpu, "test target")
        summary, details = text.split('<a id="mathf-recip"></a>', 1)
        self.assertEqual(summary.count("| [`"), len(items) * 3)
        assert_contains(self, summary, (
            "`1.0F / x`",
            "10.0 | 25.0 | 2.50 | 20.0 | 40.0",
            "`normalize_x`",
            "`nlerp_x`",
            "### What it replaces",
            "### Cost and error",
            "### Size and builds",
        ))
        self.assertNotIn("<br>", summary)
        self.assertNotIn("`math/", summary)
        self.assertNotIn("`math`", summary)
        assert_contains(self, summary, (
            "| `fmodf(v - lo, hi - lo)` |",
            "| isqrt64 squared length, then 64-bit divide and component scaling |",
            "portable; Xtensa needs fused `madd.s`, `msub.s`",
            "Xtensa: `recip0.s`, `madd.s`, `msub.s`",
            "`MATHX_RSQRT_Q30_RELATIVE_LIMIT` (2^-31)",
            "236 IRAM + 956 flash",
            f"| {sum(size for _, size, region in sizes['normalize_x'] if region == 'rodata')} |",
            "ROM",
            "`CONFIG_MATH_FPU_RECIP`",
            "render",
        ))
        self.assertIn("| `CONFIG_MATH_FPU_RECIP` | plain | plain |", summary)
        self.assertIn("| `CONFIG_MATH_FPU_FUSED_MADD` | plain | fast |", summary)
        self.assertNotIn("|", details)
        self.assertNotIn("**Plain way**", details)
        assert_contains(self, details, (
            "Symbols: nonzero input `a`; reciprocal seed `y`.",
            "`sinf` in output LSBs",
            "`[MATHX_RSQRT_MIN, 2^32)`",
        ))
        self.assertEqual(details.count("```math"), len({item.key for item in items}))
        self.assertEqual(details.count("![Error across"), len({item.key for item in items}))
        paths.validate_sections(text, items)
        host_sources = {(item.key, item.kind): "host" for item in items}
        host_text = paths.render(ROOT, items, rows, measured, errors, bins, sizes, fpu, "test target", host_sources)
        self.assertEqual(len(items), host_text.count(" (host) |"))
        pending = paths.render(ROOT, items, rows, {}, {}, {}, {}, fpu, "-")
        self.assertIn("capture pending", pending)
        self.assertIn("| - | - | - | - | - |", pending)

    def test_size_records_are_strict(self):
        text = 'fast_f\tmath_bench_fast_f\t12\tflash\nfast_f\t__divsf3\t32\tIRAM\n'
        self.assertEqual(paths.parse_sizes(text)['fast_f'][0], ('math_bench_fast_f', 12, 'flash'))
        for bad in (text + text, text.replace('12', '-1'), text.replace('flash', 'unknown')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                paths.parse_sizes(bad)


if __name__ == '__main__':
    unittest.main()
