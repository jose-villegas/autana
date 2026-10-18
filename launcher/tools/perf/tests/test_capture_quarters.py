"""Quarter selection preserves the identity of viewport measurements."""
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import capture
from layout_measure import parse_report, parse_timings
import tempfile


class CaptureQuarters(unittest.TestCase):
    def test_expected_quarters_and_unknown_names(self):
        self.assertEqual(capture.capture_quarters('row quarter=landscape'), {'landscape'})
        with self.assertRaisesRegex(ValueError, 'portrait'):
            capture.capture_quarters('row quarter=landscape', expected=('portrait',))
        with self.assertRaises(ValueError):
            capture.select_quarter('row quarter=diagonal')
        with self.assertRaises(ValueError):
            capture.select_quarter('row quarter=portrait', 'diagonal')

    def test_legacy_rows_have_no_quarter(self):
        text = 'mesh mean 90us\nmesh mean 10us quarter=portrait\nmesh mean 20us quarter=landscape'
        self.assertEqual(parse_timings(text), {'mesh': 90, 'mesh quarter=portrait': 10,
                                              'mesh quarter=landscape': 20})
        selected = capture.select_quarter(text, 'landscape')
        self.assertNotIn('quarter=portrait', selected)
        self.assertIn('quarter=landscape', selected)
        self.assertIsNone(capture.quarter_name('mesh mean 90us'))

    def test_render_and_frame_keep_quarters(self):
        rows = parse_timings('\n'.join(f'stage={stage} paused=0 mean_us={value} quarter={quarter}'
            for stage in ('render', 'frame') for quarter, value in (('portrait', 10), ('landscape', 20))))
        self.assertEqual(rows, {f'{stage} paused=0 quarter={quarter}': value
            for stage in ('render', 'frame') for quarter, value in (('portrait', 10), ('landscape', 20))})

    def test_markdown_report_keeps_legacy_and_tagged_rows(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'report.md'
            path.write_text('| Test | Mean us |\n|---|---:|\n| mesh | 90 |\n'
                            '| mesh quarter=portrait | 10 |\n| mesh quarter=landscape | 20 |\n')
            self.assertEqual(parse_report(path), {'mesh': 90, 'mesh quarter=portrait': 10,
                                                  'mesh quarter=landscape': 20})

    def test_render_logs_use_the_shared_quarter_format(self):
        owner = capture.QUARTER_HEADER.parent
        non_render = ('phase=%s', 'PERF TARGET skin ', 'stage=%s', 'dynres_refit_cost:')
        stages = {'SKIN_PERF_STAGE_SKIN', 'SKIN_PERF_STAGE_ANIMATE', 'SKIN_PERF_STAGE_ANIMATION_STEP'}
        for suite in ('sponza', 'raster_scale', 'capybara'):
            text = (owner / f'suite_{suite}_perf.c').read_text()
            for log in re.findall(r'ESP_LOGI\(.*?\);', text, re.S):
                with self.subTest(suite=suite, log=log):
                    if not any(stage in log for stage in non_render):
                        self.assertIn('R3D_PERF_QUARTER_FORMAT', log)
            for stage in re.findall(r'report_stage\(f, (\w+),', text):
                self.assertIn(stage, stages)
