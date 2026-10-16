"""Runtime heap capture and absolute overlay contracts."""
import sys
import tempfile
import unittest
from pathlib import Path
import xml.etree.ElementTree as ET
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build'))
import memory_map as memory
import test_memory_charts as charts
import svg_bars
FIXTURE = Path(__file__).parent / 'fixtures/runtime-heap.txt'

class RuntimeHeapTests(unittest.TestCase):
    def capture(self):
        return memory.parse_runtime_heap(FIXTURE.read_text())

    def test_json_shape_and_floor(self):
        capture = self.capture()
        self.assertIsNone(memory.runtime_floor(capture['release']))
        self.assertEqual(memory.runtime_floor(capture['dev']),
                         ('app:A', dict(free=320, largest=256, total=1200)))
        capture['dev']['phases']['boot']['free'] = 1
        self.assertEqual(memory.runtime_floor(capture['dev'])[0], 'app:A')

    def test_invalid_captures_fail(self):
        good = FIXTURE.read_text()
        for text in ('', good.split('\n\n')[1], good + '\nBUILD_ID=abc-dev\nunmeasured: x\n',
                     good.replace("abc-dev", "bad/id-dev"),
                     good.replace("BUILD_ID=abc-dev", "BUILD_ID=abc-dev\nBUILD_ID=abc-dev"),
                     good.replace('"total": 1200', '"total": 1'),
                     good.replace('"largest": 400', '"largest": 601'),
                     good.replace('"free": 600', '"free": -1'),
                     good.replace('phase=boot {', 'phase=boot HEAP {'),
                     good.replace('phase=boot', 'phase=app:B')):
            with self.subTest(text=text), self.assertRaises(ValueError):
                memory.parse_runtime_heap(text)

    def test_heap_spans_pool_and_measured_ceiling(self):
        body = charts.MemoryChartTests().body().replace('release', 'dev')
        capture = self.capture()
        tree = ET.fromstring(memory.chart('pool', body, capture))
        overlay = tree.find('.//{*}rect[@data-label="In use at runtime (floor)"]')
        self.assertEqual(int(overlay.get('data-bytes')), 1200 - 320)
        extra = tree.find('.//{*}rect[@data-label="Heap outside the linked pool"]')
        self.assertEqual(int(extra.get('data-bytes')), 1200 - 656)
        self.assertAlmostEqual(float(overlay.get('width')), 880 * svg_bars.BAR_WIDTH / 1568, delta=.01)
        text = ''.join(tree.itertext())
        self.assertIn('of 1.2 KB internal heap', text)
        self.assertIn('pool end 0x3fc88400, 1 KB', text)
        capture['dev']['phases']['app:A']['total'] = 655
        with self.assertRaisesRegex(ValueError, 'linked heap'):
            memory.chart('pool', body, capture)

    def test_absolute_overlay_bounds_and_optional_words(self):
        rows = [('A', [('one', 100, '#fff'), ('two', 100, '#fff')], '')]
        tree = ET.fromstring(svg_bars.bars('test', 'test', '', rows, 200,
                            overlays=[[('cover', 50, 120, '#000')]], inside=True))
        overlay = tree.find('.//{*}rect[@data-label="cover"]')
        self.assertEqual(float(overlay.get('x')), 200 + svg_bars.BAR_WIDTH / 4)
        self.assertEqual(tree.findall('.//{*}text[@class="inside"]'), [])
        for overlays in ([[('x', -1, 10, '#000')]], [[('x', 100, 101, '#000')]],
                         [[('x', 0, -1, '#000')]], [[], [('x', 0, 1, '#000')]]):
            with self.assertRaisesRegex(ValueError, 'overlay'):
                svg_bars.bars('test', 'test', '', rows, 200, overlays=overlays)

    def test_headroom_uses_runtime_total(self):
        line = memory.runtime_headline('dev', self.capture()['dev'], 1024, 64, 'abc-dev', 656)
        self.assertIn('free of its 1.2 KB internal heap', line)
        self.assertIn('0.6 KB of that heap is linked in the 1 KB pool', line)
        self.assertIn('0.5 KB lies outside it, added by ESP-IDF at runtime', line)
