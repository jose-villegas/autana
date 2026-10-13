"""Runtime internal heap capture and headroom presentation contracts."""
import sys
import unittest
from pathlib import Path
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build'))
import memory_map as memory
import test_memory_charts as charts
from svg_bars import BAR_WIDTH

FIXTURE = Path(__file__).parent / 'fixtures/runtime-heap.txt'


class RuntimeHeapTests(unittest.TestCase):
    def capture(self):
        return memory.parse_runtime_heap(FIXTURE.read_text())

    def test_line_kinds_resolution_and_floor(self):
        capture = self.capture()
        self.assertEqual(capture['release']['phases']['boot'],
                         dict(free=0, largest=0, resolution=memory.KB_BYTES))
        phase, reading = memory.runtime_floor(capture['dev'])
        self.assertEqual(phase, 'app:Sand')
        self.assertEqual(reading, dict(free=320, largest=256, resolution=1))
        post = memory.parse_runtime_heap(
            'BUILD_ID=x-release\nphase=boot [OK] memory  123 KiB free, DMA block 45 KiB\n')
        self.assertEqual(post['release']['phases']['boot']['free'], 123 * memory.KB_BYTES)
        self.assertEqual(post['release']['phases']['boot']['largest'], 45 * memory.KB_BYTES)

    def test_apps_define_floor_when_boot_has_less_free(self):
        capture = self.capture()['dev']
        capture['phases']['boot']['free'] = 1
        self.assertEqual(memory.runtime_floor(capture)[0], 'app:Sand')

    def test_invalid_captures_fail(self):
        cases = ('phase=boot HEAP free=1 largest=1',
                 'BUILD_ID=x-unknown\nphase=boot HEAP free=1 largest=1',
                 'BUILD_ID=x-dev\nphase=app:Sand HEAP free=1 largest=1',
                 'BUILD_ID=x-dev\nphase=boot nonsense',
                 'BUILD_ID=x-dev\nphase=boot HEAP free=-1 largest=1',
                 'BUILD_ID=x-dev\nphase=boot HEAP free=1 largest=2',
                 'BUILD_ID=x-dev\nphase=boot HEAP free=1 largest=1\nphase=boot HEAP free=1 largest=1',
                 'BUILD_ID=x-dev\nphase=boot HEAP free=1 largest=1\n\nBUILD_ID=y-dev\nphase=boot HEAP free=1 largest=1',
                 'BUILD_ID=x-dev\nphase=boot HEAP free=1 largest=1\nBUILD_ID=y-diag\nphase=boot HEAP free=1 largest=1',
                 '')
        for text in cases:
            with self.subTest(text=text), self.assertRaises(ValueError):
                memory.parse_runtime_heap(text)

    def test_runtime_overlay_headline_and_split_note(self):
        capture = self.capture()
        body = charts.MemoryChartTests().body().replace('release', 'dev')
        body = 'Builds: abc-dev (IDF fixture)\n' + body
        tree = ET.fromstring(memory.chart('pool', body, capture))
        overlay = tree.find('.//{*}rect[@data-label="In use at runtime (floor)"]')
        self.assertEqual(int(overlay.attrib['data-bytes']), 656 - 320)
        self.assertAlmostEqual(float(overlay.attrib['width']), (656 - 320) * BAR_WIDTH / 1024, delta=.01)
        heap = tree.find('.//{*}rect[@data-label="Internal heap"]')
        self.assertEqual(heap.attrib['x'], overlay.attrib['x'])
        self.assertEqual(int(heap.attrib['data-bytes']), 656)
        text = ''.join(tree.itertext())
        self.assertIn('free 0.3 KB, largest 0.2 KB of 1 KB', text)
        self.assertIn('after opening the app that leaves the least', text)
        self.assertIn('IRAM left before the next 256 B split step: 0 B', text)
        self.assertIn('every byte comes from the heap (free 0.3 KB)', text)
        self.assertNotIn('capture from build', text)
        # The heap's parts are labelled by what they are, not by the whole heap's size.
        inside = [node.text for node in tree.iter('{http://www.w3.org/2000/svg}text') if node.get('class') in ('inside', 'overlay-inside')]
        self.assertIn(f'in use {memory.size_label(656 - 320)}', inside)
        self.assertIn(f'free {memory.size_label(320)}', inside)
        self.assertNotIn(memory.size_label(656), inside)
        mismatch = ''.join(ET.fromstring(memory.chart('pool', body.replace('abc-dev', 'new-dev'), capture)).itertext())
        self.assertIn('capture from build abc-dev', mismatch)

    def test_boot_only_post_note_and_no_capture_fallback(self):
        body = 'Builds: abc-release (IDF fixture)\n' + charts.MemoryChartTests().body()
        text = ''.join(ET.fromstring(memory.chart('pool', body, self.capture())).itertext())
        self.assertIn('POST at boot only (KiB resolution)', text)
        with self.assertRaises(FileNotFoundError):
            memory.chart('pool', body, root=FIXTURE.parent)

    def test_older_capture_can_exceed_current_linked_heap(self):
        body = charts.MemoryChartTests().body().replace('abc-release', 'new-release')
        capture = memory.parse_runtime_heap('BUILD_ID=old-release\nphase=boot [ok  ] memory  1 KiB free, DMA block 1 KiB\n')
        tree = ET.fromstring(memory.chart('pool', body, capture))
        overlay = tree.find('.//{*}rect[@data-label="In use at runtime (floor)"]')
        self.assertEqual(int(overlay.attrib['data-bytes']), 0)
        self.assertIn('capture from build old-release', ''.join(tree.itertext()))

    def test_doc_headline_and_prefetch_constraint(self):
        reading = self.capture()['dev']
        self.assertEqual(memory.iram_headroom(80, 16), 64)
        self.assertIn(
            'dev: 0.3 KB free internal heap at its floor (after opening the app that leaves the least), largest block 0.2 KB, '
            'of the 1 KB shared pool; IRAM can grow 64 B before it costs heap.',
            memory.runtime_headline('dev', reading, 1024, 64, 'abc-dev', 656))

    def test_unmeasured_build_states_why_and_draws_no_runtime_share(self):
        reason = 'this build has no console'
        capture = memory.parse_runtime_heap(f'BUILD_ID=abc-release\nunmeasured: {reason}\n')['release']
        self.assertIsNone(memory.runtime_floor(capture))
        headline = memory.runtime_headline('release', capture, 1024, 64, 'abc-release', 656)
        self.assertIn(f'0.6 KB linked internal heap (not measured at runtime: {reason})', headline)
        for bad in ('BUILD_ID=abc-release\nunmeasured: x\nphase=boot HEAP free=1 largest=1\n',
                    'BUILD_ID=abc-release\nphase=boot HEAP free=1 largest=1\nunmeasured: x\n'):
            with self.assertRaises(ValueError):
                memory.parse_runtime_heap(bad)
