import sys
import unittest
from pathlib import Path
import xml.etree.ElementTree as ET
import tempfile
import contextlib
import io
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build'))
import memory_map as memory
import iram_what_if as probe
from doc_tables import markdown


def chart_documents(doc):
    tiers = doc.parent / memory.TIERS_DOC
    tiers.parent.mkdir(parents=True, exist_ok=True)
    for path, block in ((doc, memory.BLOCK), (tiers, memory.TIERS_BLOCK)):
        path.write_text(f'<!-- generated: {block} -->\n<!-- /generated: {block} -->\n', encoding='utf-8')


@contextlib.contextmanager
def chart_cli(doc, build):
    replacements = dict(render_tiers=lambda builds, root: 'tier fixture', DOC=doc, ROOT=doc.parent, TIERS_DOC=doc.parent / memory.TIERS_DOC,
                        read_build=lambda *args: build, build_sites=lambda *args: ([], lambda site: True),
                        check=lambda *args, **kwargs: [], idf_pin=lambda *args: {'commit': 'fixture'})
    with contextlib.ExitStack() as stack:
        for name, value in replacements.items():
            stack.enter_context(patch.object(memory, name, value))
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        yield


class MemoryChartTests(unittest.TestCase):
    def body(self):
        regions = [('iram0_0_seg shared alias', '0x40378000–0x40378400 (1024 B)'),
                   ('dram0_0_seg shared alias', '0x3fc88000–0x3fc88400 (1024 B)'),
                   ('.iram0.text', 256), ('.iram0.text_end', 16),
                   ('Slack including pad', 16), ('.dram0.data', 32),
                   ('.dram0.bss', 64), ('_data_start', '0x3fc88110'),
                   ('_heap_start', '0x3fc88170')]
        components = [('libmain.a', 'our sites', 16), ('liblcd.a', 'our sites', 16)]
        components += [(f'libpart{i}.a', 'IDF', 20) for i in range(10)]
        components += [('Linker fill / split slack', 'unattributed', 40)]
        return (markdown(['Region / boundary', 'release'], regions) + '\n' +
                markdown(['IRAM component', 'Placement', 'release'], components) + '\n' +
                markdown(['Placement site', 'Symbols', 'release', 'Budget B', 'Reason'],
                         [('a:1', 'f', 32, 32, 'Interrupt latency.')]))

    def test_pool_width_order_and_labels(self):
        from svg_bars import BAR_WIDTH
        tree = ET.fromstring(memory.chart('pool', self.body()))
        segments = tree.findall('.//{*}rect[@data-bytes]')
        self.assertEqual([int(s.attrib['data-bytes']) for s in segments], [224, 32, 16, 96, 656])
        for segment in segments:
            self.assertAlmostEqual(float(segment.attrib['width']), int(segment.attrib['data-bytes']) * BAR_WIDTH / 1024, delta=0.01)
        text = ''.join(tree.itertext())
        for label in ('0.6 KB', '0x3fc88000', '0x3fc88400', '0x40378000', '1 KB = 1024 B'):
            self.assertIn(label, text)

    def test_components_other_sums_remainder(self):
        tree = ET.fromstring(memory.chart('components', self.body()))
        other = tree.find('.//{*}rect[@data-label="other"]')
        self.assertEqual(int(other.attrib['data-bytes']), 40)
        self.assertNotIn('libpart', ''.join(tree.itertext()))
        self.assertIn('Linker fill', ''.join(tree.itertext()))

    def test_parser_missing_row_and_column(self):
        with self.assertRaisesRegex(ValueError, '_heap_start'):
            memory.chart('pool', self.body().replace('_heap_start', '_absent'))
        with self.assertRaisesRegex(ValueError, 'Placement'):
            memory.chart('components', self.body().replace('| Placement |', '| Ownership |'))
        with self.assertRaisesRegex(ValueError, 'differ from site table'):
            memory.chart('pool', self.body().replace('| f | 32 |', '| f | 31 |'))

    def test_what_if_sorted_and_variant_labels(self):
        body = probe.render_results([], [
            ('release', 'small', 20, 0, -32, 'a'),
            ('release', 'large', 100, 0, -128, 'b'),
            ('dev', 'large', 120, 0, -128, 'b'),
            ('diag', 'only-diag', 50, 0, -64, 'c')])
        tree = ET.fromstring(probe.chart(body))
        self.assertEqual([int(s.attrib['data-bytes']) for s in tree.findall('.//{*}rect[@data-bytes]')], [100, 50, 20])
        text = ''.join(tree.itertext())
        for label in ('not additive', 'dev 120', 'heap +128 B', 'only-diag (diag)'):
            self.assertIn(label, text)

    def test_render_external_archive_placement_and_site_sum(self):
        site = memory.Site('launcher/main/linker.lf', 1, 'engine', 'Interrupt latency.', 12,
                           'liblcd.a', [('draw', 'draw')])
        inputs = [memory.Input('liblcd.a', 'draw.c.obj', 'draw', 8),
                  memory.Input('liblcd.a', 'draw.c.obj', 'draw', 4, True),
                  memory.Input('liblcd.a', 'other.c.obj', 'other', 20)]
        builds = []
        for variant in memory.VARIANTS:
            builds.append(dict(variant=variant, build_id='abc-' + variant, idf_commit='fixture',
                               regions={'iram0_0_seg': (0x40378000, 1024), 'dram0_0_seg': (0x3fc88000, 1024)},
                               sections={'.iram0.text': (0, 32), '.dram0.data': (0, 32), '.dram0.bss': (0, 64)},
                               constants={'_heap_start': 0x3fc88080}, size={'layout': [{'name': 'DIRAM', 'used': 128}]},
                               align=256, pad=16, slack=0, inputs=inputs, owned={}, report=[(site, 12, ['draw'])]))
        body = memory.render(builds)
        from doc_tables import parse_table
        headers, rows = parse_table(body, 'IRAM component')
        self.assertIn('Placement', headers)
        self.assertIn('| liblcd.a | our sites | 12 | 12 | 12 |', body)
        self.assertIn('| liblcd.a | IDF | 20 | 20 | 20 |', body)
        for variant in memory.VARIANTS:
            self.assertEqual(sum(int(row[variant]) for row in rows if row['Placement'] == 'our sites'),
                             sum(size for _, size, _ in builds[memory.VARIANTS.index(variant)]['report']))
        self.assertNotIn('| Owner |', body)
        self.assertIn('<details><summary>Regions and boundaries</summary>\n\n|', body)
        self.assertNotIn('```mermaid', body)

    def test_write_refreshes_tables_and_both_charts(self):
        with tempfile.TemporaryDirectory() as directory:
            doc = Path(directory) / 'map.md'
            chart_documents(doc)
            build = dict(variant='release', idf_commit='fixture', inputs=[], owned={})
            with chart_cli(doc, build), patch.object(memory, 'render', return_value=self.body()), patch.object(sys, 'argv', ['memory_map.py', 'fixture', '--write']):
                self.assertEqual(memory.main(), 0)
            for name in memory.CHARTS.values():
                svg = (doc.parent / name).read_bytes()
                self.assertNotIn(b'\r', svg)
                ET.fromstring(svg)
            self.assertIn('Placement', doc.read_text())
            self.assertIn('tier fixture', (doc.parent / memory.TIERS_DOC).read_text())

    def test_tables_charts_match_chart_mode(self):
        build = dict(variant='release', build_id='abc-release', idf_commit='fixture',
                     regions={'iram0_0_seg': (0x40378000, 1024), 'dram0_0_seg': (0x3fc88000, 1024)},
                     sections={'.iram0.text': (0, 32), '.dram0.data': (0, 32), '.dram0.bss': (0, 64)},
                     constants={'_data_start': 0x3fc88020, '_heap_start': 0x3fc88080},
                     size={'layout': [{'name': 'DIRAM', 'used': 128}]}, align=256, pad=16, slack=0,
                     inputs=[memory.Input('libpart.a', 'part.c.obj', 'part', 32)], owned={}, report=[])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            doc = root / 'map.md'
            chart_documents(doc)
            from generated_blocks import replace_block
            replace_block(doc, memory.BLOCK, memory.render([build]))
            with chart_cli(doc, build):
                with patch.object(sys, 'argv', ['memory_map.py', 'fixture', '--tables', str(root / 'tables')]):
                    self.assertEqual(memory.main(), 0)
                self.assertEqual((root / 'tables' / (memory.TIERS_BLOCK + '.md')).read_text(), 'tier fixture')
                for kind, name in memory.CHARTS.items():
                    output = root / (kind + '.svg')
                    with patch.object(sys, 'argv', ['memory_map.py', '--chart', kind, str(output)]):
                        self.assertEqual(memory.main(), 0)
                    self.assertEqual((root / 'tables' / name).read_bytes(), output.read_bytes())


class MemoryTierTests(unittest.TestCase):
    def test_tiers_derive_constants_partitions_and_chart_heap(self):
        from doc_tables import parse_table
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            arena = root / 'launcher/main/app/app_arena.h'
            gfx = root / 'launcher/main/gfx/gfx.h'
            for path in (arena, gfx):
                path.parent.mkdir(parents=True)
            arena.write_text('#define APP_ARENA_BYTES (2u * 1024u * 1024u)\n')
            gfx.write_text('#define GFX_WIDTH 128\n#define GFX_HEIGHT 256\n')
            partitions = root / 'launcher/fixture.csv'
            partitions.write_text('# Name, Type, SubType, Offset, Size\nfactory,app,factory,,0x200000,\nassets,data,0x40,,0x300000,\n')
            config = {'CONFIG_ESPTOOLPY_FLASHSIZE': '"16MB"',
                      'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME': '"fixture.csv"'}
            builds = [dict(variant=variant, regions={'dram0_0_seg': (0x3fc88000, 1024)},
                           constants={'_heap_start': 0x3fc88170}, config=config)
                      for variant in memory.VARIANTS]
            body = memory.render_tiers(builds, root)
            _, rows = parse_table(body, 'Tier')
            sizes = {row['Tier']: row['Size'] for row in rows}
            chart = ET.fromstring(memory.chart('pool', MemoryChartTests().body()))
            heap = chart.find('.//{*}rect[@data-label="Internal heap"]')
            expected = int(heap.attrib['data-bytes']) / memory.svg_bars.KB_BYTES
            for variant in memory.VARIANTS:
                self.assertIn(f'{variant} {expected:.1f} KB', sizes['Internal SRAM'])
            self.assertIn('1024 B', sizes['Internal SRAM'])
            self.assertIn('2 MB app arena', sizes['PSRAM'])
            self.assertIn('64 KB full framebuffer', sizes['PSRAM'])
            self.assertIn('5.9 MB PSRAM heap upper bound', sizes['PSRAM'])
            self.assertIn('16 MB total', sizes['Flash'])
            self.assertIn('2 MB app partition', sizes['Flash'])
            self.assertIn('3 MB assets partition', sizes['Flash'])
            arena.write_text('#define APP_ARENA_BYTES (3u * 1024u * 1024u)\n')
            gfx.write_text('#define GFX_WIDTH 256\n#define GFX_HEIGHT 256\n')
            changed = memory.render_tiers(builds, root)
            self.assertIn('3 MB app arena', changed)
            self.assertIn('128 KB full framebuffer', changed)
            self.assertIn('4.9 MB PSRAM heap upper bound', changed)
            partitions.write_text('factory,app,factory,,0x400000,\nassets,data,0x40,,0x100000,\n')
            changed = memory.render_tiers(builds, root)
            self.assertIn('4 MB app partition', changed)
            self.assertIn('1 MB assets partition', changed)
