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
        self.assertIn('math/linear', paths.callers(self.root, 'mathx_rsqrt_q30'))
        item = next(item for item in paths.annotations(self.root) if item.key == 'mathx_rsqrt_q30')
        self.assertEqual(paths.tables_for(item, self.root), ['mathx_rsqrt_seeds'])

    def test_call_windows_sum_translation_units_before_scene_statistics(self):
        capture = ('MATHCALLS begin\n'
                   'MATHCALLS fn=example caller=C:/tree/launcher/main/apps/a/use.c per_frame=2\n'
                   'MATHCALLS fn=example caller=C:/tree/launcher/main/apps/b/use.c per_frame=4\n'
                   'MATHCALLS end\nMATHCALLS begin\n'
                   'MATHCALLS fn=example caller=C:/tree/launcher/main/render/use.c per_frame=8\n'
                   'MATHCALLS end\n')
        self.assertEqual(paths.parse_calls(capture), {('example', 'apps (not named)'): (6, 3), ('example', 'render'): (8, 4)})
        self.assertEqual(paths.parse_calls(capture.replace('C:/tree/launcher/', './')),
                         {('example', 'apps (not named)'): (6, 3), ('example', 'render'): (8, 4)})

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

    def test_size_records_are_strict(self):
        text = 'fast_f\tmath_bench_fast_f\t12\tflash\nfast_f\t__divsf3\t32\tIRAM\n'
        self.assertEqual(paths.parse_sizes(text)['fast_f'][0], ('math_bench_fast_f', 12, 'flash'))
        for bad in (text + text, text.replace('12', '-1'), text.replace('flash', 'unknown')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                paths.parse_sizes(bad)


if __name__ == '__main__':
    unittest.main()
