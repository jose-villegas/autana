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
from report_assertions import assert_contains


class FastPathTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.header = self.root / 'launcher/packages/math/include/math/scalar/example.h'
        self.header.parent.mkdir(parents=True)
        self.header.write_text('#define EXAMPLE_LIMIT 1\n'
                               '/* Seed correction. | y = y(2 - ay) | positive normal inputs */\n'
                               '/* exact: y = 1/x */\n'
                               '/* FAST_PATH(fast_f, plain_f, EXAMPLE_LIMIT) */\n'
                               'static inline float example(float x) { return x; }\n')
        source = self.root / 'launcher/packages/math/tests/suite_math_numbers.c'
        source.parent.mkdir(parents=True)
        source.write_text('MATHERROR name=example type=f max=%g unit=ulp method=sweep')
        perf = self.root / 'launcher/packages/math/bench/suite_math_perf.c'
        perf.parent.mkdir(parents=True)
        perf.write_text('')
        self.annotations = paths.annotations(self.root)
        self.rows = [('fast_f', 'example', 'f', 64, 'scalar'), ('plain_f', 'plain', 'f', 64, 'scalar')]
        self.capture = ('MATHERROR name=example type=f max=1 unit=ulp method=sweep\n'
                        'MATHERRORBIN name=example type=f x=2 max=1 unit=ulp\n')

    def test_exact_rows_start_after_a_blank_line(self):
        items = [item for item in paths.annotations() if item.unit == "bit"]
        self.assertTrue(items)
        rendered = paths.render_exact(ROOT, items)
        self.assertTrue(rendered.startswith('\n\n<a id="'))

    def test_package_headers_are_discovered_and_bit_limit_is_named(self):
        header = self.root / 'launcher/packages/other/include/other/check.h'
        header.parent.mkdir(parents=True)
        header.write_text('#define OTHER_BIT_LIMIT 0\n'
                          '/* FAST_PATH(check, check_reference, OTHER_BIT_LIMIT=0 bit) */\n'
                          'static inline unsigned check(unsigned x) { return x; }\n')
        source = self.root / 'launcher/packages/other/tests/suite_check.c'
        source.parent.mkdir(parents=True)
        source.write_text('FASTERROR name=check type=bits max=0 unit=bit method=sweep')
        items = paths.annotations(self.root)
        item = next(item for item in items if item.key == 'check')
        self.assertEqual((item.limit, item.unit), (0, 'bit'))
        header.write_text(header.read_text().replace('OTHER_BIT_LIMIT=0', 'OTHER_BIT_LIMIT=1'))
        with self.assertRaisesRegex(ValueError, 'limit mismatch'):
            paths.annotations(self.root)

    def test_shared_plain_row_links_to_first_page_section(self):
        from dataclasses import replace
        first = replace(self.annotations[0], key="first_path")
        second = replace(first, key="second_path", fast="second_f")
        links = paths.benchmark_links([first, second], self.rows)
        self.assertEqual(links["plain_f"], "../packages/Fast-Paths.md#first-path")
        self.assertEqual(links["fast_f"], "../packages/Fast-Paths.md#first-path")

    def test_links_include_benchmarked_function_but_not_unrendered_error(self):
        item = self.annotations[0]
        rows = [*self.rows, ("example_f", "example", "f", 1, "scalar"),
                ("normalize_f", "normalize (vec3)", "f", 1, "vector")]
        links = paths.benchmark_links([item], rows)
        self.assertEqual(links["example_f"], "../packages/Fast-Paths.md#example")
        self.assertNotIn("normalize_f", links)

    def test_main_benchmark_error_cells_follow_fast_rows(self):
        from unittest.mock import patch
        render_table = bench.render_table
        tables = []

        def record_table(*args, **kwargs):
            table = render_table(*args, **kwargs)
            tables.append(table)
            return table

        with patch.object(paths.bench, "render_table", side_effect=record_table), \
                patch.object(paths, "replace_block", return_value=False):
            self.assertEqual(paths.main(["--check"]), 0)
        lines = tables[0].splitlines()
        for label, kind, section in (("wrap", "f", "mathf-repeat"),
                                     ("normalize (vec3)", "x", "mathx-rsqrt-q30"),
                                     ("nlerp", "x", "mathx-rsqrt-q30"),
                                     ("mathx_rsqrt_q30", "x", "mathx-rsqrt-q30")):
            with self.subTest(label=label):
                row = next(line for line in lines if line.startswith(f"| [{label}]") and f" | {kind} |" in line)
                self.assertTrue(row.endswith(f"| [fast paths](../packages/Fast-Paths.md#{section}) |"), row)
        ordinary = next(line for line in lines if line.startswith("| normalize (vec3) | f |"))
        self.assertTrue(ordinary.endswith("| 1 step (sweep) |"), ordinary)
        unmeasured = next(line for line in lines if line.startswith("| add (vec3) | f |"))
        self.assertTrue(unmeasured.endswith("| - |"), unmeasured)

    def test_unmatched_error_measurements_are_rejected(self):
        for host in (False, True):
            with self.subTest(host=host):
                capture = "MATHERROR name=normalise (vec3) type=f max=0 unit=ulp method=sweep\n"
                if host:
                    file = self.root / paths.HOST_CAPTURE
                    file.parent.mkdir(parents=True, exist_ok=True)
                    file.write_text("MATHTARGET target=host arch=x86_64 execution=host\n" + capture)
                    capture = ""
                with self.assertRaisesRegex(ValueError, "error measurements match no row"):
                    paths.error_evidence(self.root, self.annotations, self.rows, capture)

    def test_annotation_sources_resolve_by_name(self):
        doc = self.root / 'docs/Citations.md'
        doc.parent.mkdir(parents=True)
        doc.write_text('| <a id="example-author"></a>[1] | Example source.<br>`Example author` cited by: [example](example.md) |\n')
        text = self.header.read_text().replace('/* exact:', '/* source: Example author */\n/* exact:')
        self.header.write_text(text)
        self.assertEqual(paths.annotations(self.root)[0].citations, ('example-author',))
        self.header.write_text(text.replace('Example author', 'Unknown author'))
        with self.assertRaisesRegex(ValueError, 'Unknown author'):
            paths.annotations(self.root)

    def test_period_axis_label_uses_folded_range(self):
        from unittest.mock import Mock
        sys.path.insert(0, str(ROOT / 'launcher/tools/math'))
        import fast_path_figures as figures
        for end in (2, 4):
            axis = Mock()
            figures._configure_axis(axis, [(1.01, 0.25, 'ulp'), (end - 0.01, 0.5, 'ulp')], 'period')
            axis.set_xlabel.assert_called_once_with(f'Input mantissa (every input folded into [1, {end}))')

    def test_fractional_bins_keep_integer_summary_contract(self):
        capture = self.capture.replace('max=1 unit=ulp\n', 'max=1.25 unit=ulp axis=period\n')
        paths.validate(self.annotations, self.rows, capture)
        self.assertEqual(bench.parse_bin_axes(capture), {('example', 'f'): 'period'})

    def test_shared_limit_formatter(self):
        self.assertEqual(paths.format_limit(2 ** -31), '2^-31')
        self.assertEqual(paths.format_limit(0), '0')
        self.assertEqual(paths.format_limit(5), '5')

    def test_identity_count_from_summary(self):
        capture = 'MATHERROR name=example type=f max=0 unit=ulp method=sweep inputs=123\n'
        self.assertEqual(bench.parse_input_counts(capture), {('example', 'f'): 123})
        self.assertEqual(bench.parse_errors(capture), {('example', 'f'): '0 ulp (sweep)'})

    def test_figure_axis_modes_and_bounds(self):
        from unittest.mock import Mock
        sys.path.insert(0, str(ROOT / 'launcher/tools/math'))
        import fast_path_figures as figures
        points = [(1.01, 0.25, 'ulp'), (3.99, 0.75, 'ulp')]
        for mode in ('period', 'phase', 'range', None):
            with self.subTest(mode=mode):
                axis = Mock()
                figures._configure_axis(axis, points, mode)
                axis.set_xlim.assert_called_once_with(1.01, 3.99)
                if mode in ('period', 'phase'):
                    axis.set_xscale.assert_not_called()
                else:
                    axis.set_xscale.assert_called_once_with('log', base=2)
        axis = Mock()
        figures._configure_axis(axis, [(0.01, 1, 'lsb'), (0.99, 2, 'lsb')], 'phase')
        axis.set_xlabel.assert_called_once_with('Phase (turns)')

    def test_figures_skip_identity_and_label_exact_limit(self):
        from unittest.mock import patch
        sys.path.insert(0, str(ROOT / 'launcher/tools/math'))
        import fast_path_figures as figures
        saved = []
        labels = []
        def save(fig, path):
            saved.append(path.name)
            labels.extend(fig.axes[0].get_legend_handles_labels()[1])
            import matplotlib.pyplot as plt
            plt.close(fig)
        with patch.object(figures, 'save', side_effect=save):
            self.assertEqual(figures.main(['--out', str(self.root)]), 0)
        self.assertNotIn('fast-path-mathf_repeat.png', saved)
        self.assertIn('fast-path-mathx_rsqrt_q30.png', saved)
        self.assertIn('MATHX_RSQRT_Q30_RELATIVE_LIMIT = 2^-31', labels)

    def test_small_errors_zoom_and_trim_zero_edges(self):
        from unittest.mock import Mock
        sys.path.insert(0, str(ROOT / 'launcher/tools/math'))
        import fast_path_figures as figures
        points = [(1, 0, 'ulp'), (2, 0.01, 'ulp'), (3, 0.02, 'ulp'), (4, 0, 'ulp')]
        axis = Mock()
        figures._configure_axis(axis, points, 'range')
        axis.set_xlim.assert_called_once_with(2, 3)
        self.assertEqual(figures.ERROR_ZOOM_FRACTION, 0.75)
        from unittest.mock import patch
        evidence = ({('example', 'f'): '0.02 ulp (sweep)'}, {('example', 'f'): points},
                    {('example', 'f'): 'hardware'})
        def check_plot(fig, path):
            plotted = fig.axes[0]
            self.assertEqual(plotted.get_xlim(), (2, 3))
            self.assertAlmostEqual(plotted.get_ylim()[1], 0.02 * figures.ERROR_HEADROOM)
            self.assertEqual(len(plotted.lines), 1)
            self.assertIn('Limit: EXAMPLE_LIMIT = 1', plotted.get_title())
            import matplotlib.pyplot as plt
            plt.close(fig)
        with patch.object(figures.fast_paths, 'annotations', return_value=self.annotations), \
                patch.object(figures.fast_paths, 'error_evidence', return_value=evidence), \
                patch.object(figures, 'save', side_effect=check_plot):
            self.assertEqual(figures.main(['--out', str(self.root)]), 0)

    def test_zero_error_with_nonzero_limit_skips_figure(self):
        from unittest.mock import patch
        sys.path.insert(0, str(ROOT / 'launcher/tools/math'))
        import fast_path_figures as figures
        evidence = ({('example', 'f'): '0 ulp (sweep)'},
                    {('example', 'f'): [(1, 0, 'ulp'), (2, 0, 'ulp')]},
                    {('example', 'f'): 'hardware'})
        with patch.object(figures.fast_paths, 'annotations', return_value=self.annotations), \
                patch.object(figures.fast_paths, 'error_evidence', return_value=evidence), \
                patch.object(figures, 'save') as save:
            self.assertEqual(figures.main(['--out', str(self.root)]), 0)
        save.assert_not_called()

    def test_bits_owned_limit_is_evaluated_and_labelled(self):
        self.header.write_text(self.header.read_text().replace('#define EXAMPLE_LIMIT 1',
            '#define EXAMPLE_BITS 13\n#define EXAMPLE_LIMIT (1.0F / (float)(1UL << EXAMPLE_BITS))'))
        source = self.root / 'launcher/packages/math/tests/suite_math_numbers.c'
        source.write_text(source.read_text().replace('unit=ulp', 'unit=relative'))
        item = paths.annotations(self.root)[0]
        self.assertEqual(item.limit, 2 ** -13)
        self.assertEqual(paths.limit_label(item), '2^-13 rel, 13 bits')
        self.header.write_text(self.header.read_text().replace('EXAMPLE_BITS 13', 'EXAMPLE_BITS 14'))
        self.assertEqual(paths.annotations(self.root)[0].limit, 2 ** -14)

    def test_worst_period_bin_uses_edges_and_a_rounded_up_bound_for_the_rest(self):
        points = [(1.015625 + i / 32, 2 ** -16, 'relative') for i in range(32)]
        points[-1] = (points[-1][0], 2 ** -14, 'relative')
        points[0] = (points[0][0], 3.124e-5, 'relative')
        self.assertEqual(paths.worst_bin_line(points),
            'Worst bin: mantissa 1.97-2.00 at 6.1e-5; every other bin <= 3.2e-5.')
        points[10] = (points[10][0], 2 ** -13, 'relative')
        self.assertIn('mantissa 1.31-1.34 at 0.00012', paths.worst_bin_line(points))
        for missing in ([], points[:1]):
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                paths.worst_bin_line(missing)

    def test_the_bound_for_the_rest_never_rounds_below_a_bin(self):
        points = [(1.25, 2 ** -13, 'relative'), (1.75, 3.0001e-5, 'relative'), (1.5, 0, 'relative')]
        self.assertTrue(paths.worst_bin_line(points).endswith('every other bin <= 3.1e-5.'))
        self.assertEqual(paths.ceil_measurement(0), '0')

    def test_relative_period_sections_render_and_require_their_bins(self):
        items = [item for item in paths.annotations()
                 if item.key in ('mathf_recip_approx', 'mathf_rsqrt_approx')]
        self.assertEqual(len(items), 2)
        self.assertTrue(all(item.period_bins for item in items))
        errors = {(item.key, item.kind): '0.00006103515625 relative (proof)' for item in items}
        bins = {key: [(1.25, 2 ** -15, 'relative'), (1.75, 2 ** -14, 'relative')] for key in errors}
        fpu = {item.fast: 'recip0.s rsqrt0.s' for item in items}
        text = paths.render(ROOT, items, [], {}, errors, bins, {}, fpu, 'test target')
        self.assertIn(f"rel, {next(item.bits for item in items if item.bits)} bits", text)
        self.assertEqual(text.count(
            'Worst bin: mantissa 1.50-2.00 at 6.1e-5; every other bin <= 3.1e-5.'), 2)
        for item in items:
            with self.subTest(key=item.key), self.assertRaisesRegex(ValueError, 'missing.*bins'):
                paths.render(ROOT, [item], [], {}, errors, {}, {}, fpu, 'test target')

    def test_relative_period_contract_rejects_missing_bins(self):
        from dataclasses import replace
        item = replace(self.annotations[0], unit='relative', period_bins=True)
        with self.assertRaisesRegex(ValueError, 'missing.*bins'):
            paths.validate([item], self.rows,
                'MATHERROR name=example type=f max=0.5 unit=relative method=sweep')

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
        doc.write_text('[example](../packages/Fast-Paths.md#example)\n')
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
        self.assertIsNotNone(paths.ANNOTATION.fullmatch('/* FAST_PATH(fast_f, plain_f, LIMIT) */'))
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
        for instruction in ('recip0.s', 'rsqrt0.s'):
            with self.subTest(instruction=instruction):
                text = ("42000000 <math_bench_fast_f>:\n"
                        "42000000: 123456 call8 42000100 <seed_kernel>\n"
                        "42000100 <seed_kernel>:\n"
                        f"42000100: 123456 {instruction} f0, f1\n"
                        "42000200 <unrelated>:\n"
                        "42000200: 123456 div0.s f0, f1\n")
                fpu = bench.parse_disassembly(text, self.rows[:1])
                self.assertEqual(f'{instruction}; calls seed_kernel', fpu['fast_f'])
                paths.validate_hardware(self.root, items, fpu)
                without_seed = text.replace(instruction, 'nop.n')
                with self.assertRaisesRegex(ValueError, 'instruction'):
                    paths.validate_hardware(self.root, items, bench.parse_disassembly(without_seed, self.rows[:1]))

    def test_transitive_template_callers_and_tables(self):
        self.header.unlink()
        shutil.copyfile(ROOT / 'launcher/packages/math/tests/suite_math_numbers.c', self.root / 'launcher/packages/math/tests/suite_math_numbers.c')
        (self.root / "docs").mkdir(exist_ok=True)
        shutil.copyfile(ROOT / "docs/Citations.md", self.root / "docs/Citations.md")
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

    def test_elf_sizes_skip_a_table_the_build_did_not_link(self):
        from code_layout import symbols
        from unittest.mock import patch
        sections = [('.iram0.text', 0x1000, 0x100)]
        with patch('code_layout.run', return_value="1000 c T math_bench_fast_f\n"):
            located = symbols('nm', 'elf', records=True)
        with patch.object(paths, 'tables_for', return_value=['seeds']):
            sizes = paths.parse_sizes(paths.elf_sizes(self.rows[:1], {'fast_f': '-'}, located, sections, self.root))
        self.assertEqual(sizes['fast_f'], [('math_bench_fast_f', 12, 'IRAM')])
        with patch('code_layout.run', return_value="2000 c T other\n"):
            located = symbols('nm', 'elf', records=True)
        with patch.object(paths, 'tables_for', return_value=['seeds']), \
             self.assertRaisesRegex(ValueError, "size symbol missing from ELF: math_bench_fast_f"):
            paths.elf_sizes(self.rows[:1], {'fast_f': '-'}, located, sections, self.root)

    def test_measurement_formatters(self):
        for value, expected in ((26.3, "26"), (58.3, "58"), (3000, "3000"),
                                (5104, "5100"), (1.03, "1"), (5.94258e-5, "5.9e-5")):
            with self.subTest(value=value):
                self.assertEqual(paths.format_measurement(value), expected)
        self.assertEqual(paths.format_error("1 ulp (sweep)", "hardware"), "1 step")
        self.assertEqual(paths.format_error("4.61224169968e-10 relative (proof)", "host"),
                         "4.6e-10 rel (host)")
        self.assertEqual(paths.format_error("0 ulp (sweep)", "hardware"), "0 steps")
        self.assertEqual(paths.format_error("-", "hardware"), "-")

    def test_compact_tables_and_formula_details(self):
        items = [item for item in paths.annotations() if item.unit != "bit"]
        rows, fpu = bench.read_fpu(ROOT / "docs/math/data/math_bench_fpu_ops.txt")
        sizes = paths.parse_sizes((ROOT / "docs/math/data/math_bench_sizes.txt").read_text())
        measured = {row[0]: (10.0, 20.0, 5.0) for row in rows}
        measured["div_recip_f"] = (25.0, 40.0, 15.0)
        errors = {(item.key, item.kind): "0 ulp" for item in items}
        bins = {(item.key, item.kind): [(1.0, 0.0, item.unit), (1.5, 0.0, item.unit)] for item in items}
        text = paths.render(ROOT, items, rows, measured, errors, bins, sizes, fpu, "test target")
        summary, details = text.split('<a id="mathf-recip"></a>', 1)
        self.assertEqual(summary.count("| [`"), len(items) * 3)
        assert_contains(self, summary, (
            "`1.0F / x`",
            "10 / 25 (2.5×) | 20 / 40",
            "(normalize)",
            "(nlerp)",
            "### What it replaces",
            "### Cost and error",
            "### Size and builds",
        ))
        for table in summary.split("### ")[1:]:
            for line in table.splitlines():
                if line.startswith("|"):
                    self.assertLessEqual(line.count("|"), 5)
        self.assertIn("Bench rows `mathf_recip_f` vs `div_recip_f`; limit `MATHF_RECIP_ULP_LIMIT` (1 step); sweep.", details)
        self.assertIn("Bench rows `normalize_x` vs `normalize_div_x`", details)
        self.assertIn("Bench rows `nlerp_x` vs `nlerp_div_x`", details)
        self.assertNotIn("<br>", summary)
        self.assertNotIn("`math/", summary)
        self.assertNotIn("`math`", summary)
        assert_contains(self, summary, (
            "| `fmodf(v - lo, hi - lo)` |",
            "| isqrt64 normalization |",
            (f"{sum(size for _, size, region in sizes['normalize_x'] if region == 'IRAM')} IRAM + "
             f"{sum(size for _, size, region in sizes['normalize_x'] if region == 'flash')} flash"),
            f"| {sum(size for _, size, region in sizes['normalize_x'] if region == 'rodata') or '-'} |",
            "ROM",
        ))
        self.assertIn("Builds: `CONFIG_MATH_FPU_RECIP`; QEMU plain; host plain.", details)
        self.assertIn("Builds: `CONFIG_MATH_FPU_FUSED_MADD`; QEMU plain; host fast.", details)
        assert_contains(self, details, (
            "portable; Xtensa needs fused `madd.s`, `msub.s`",
            "Xtensa: `recip0.s`, `madd.s`, `msub.s`",
            "`MATHX_RSQRT_Q30_RELATIVE_LIMIT` (2^-31 rel)",
            "Callers: `render`.",
            "isqrt64 squared length, then 64-bit divide and component scaling",
        ))
        self.assertIn("| Original | Fast path |", details)
        self.assertIn("| $`y = 1/a`$ | $`y = y + y(1 - ay)`$ |", details)
        self.assertNotIn("**Plain way**", details)
        assert_contains(self, details, (
            "Symbols: nonzero input `a`; reciprocal seed `y`.",
            "`sinf` in output LSBs",
            "`[MATHX_RSQRT_MIN, 2^32)`",
        ))
        self.assertEqual(details.count("| Original | Fast path |"), len({item.key for item in items}))
        self.assertNotIn("![Error across", details)
        identity_text = paths.render(ROOT, items, rows, measured, errors, bins, sizes, fpu, "test target",
                                     input_counts={('mathf_repeat', 'f'): 123})
        self.assertIn('Exact: no error over 123 inputs of the sweep.', identity_text)
        self.assertNotIn('fast-path-mathf_repeat.png', identity_text)
        paths.validate_sections(text, items)
        host_sources = {(item.key, item.kind): "host" for item in items}
        host_text = paths.render(ROOT, items, rows, measured, errors, bins, sizes, fpu, "test target", host_sources)
        self.assertEqual(len(items), host_text.count(" (host) |"))
        pending = paths.render(ROOT, items, rows, {}, {}, {}, {}, fpu, "-")
        self.assertIn("capture pending", pending)
        self.assertIn("| - / - (-×) | - / - | - |", pending)

    def test_size_records_are_strict(self):
        text = 'fast_f\tmath_bench_fast_f\t12\tflash\nfast_f\t__divsf3\t32\tIRAM\n'
        self.assertEqual(paths.parse_sizes(text)['fast_f'][0], ('math_bench_fast_f', 12, 'flash'))
        for bad in (text + text, text.replace('12', '-1'), text.replace('flash', 'unknown')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                paths.parse_sizes(bad)


if __name__ == '__main__':
    unittest.main()
