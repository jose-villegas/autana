"""Synthetic EQ probe measurements and failure cleanup."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / 'launcher/tools'))
sys.path.insert(0, str(ROOT / 'launcher/tools/audio'))
from audio.mic import eq_probe_sweep as sweep
from audio.mic import mic_eq_probe_tables as tables


class EqProbeTest(unittest.TestCase):
    def test_synthetic_response_and_cutoff(self):
        from audio_doc_tables import read_capture
        _, records, _ = read_capture(ROOT / 'launcher/test/fixtures/mic_eq_probe_capture.txt')
        data = tables.probe_data(records)
        self.assertAlmostEqual(data['adc', 'half', 'acoustic'][100], -6.0206, places=3)
        self.assertAlmostEqual(data['hpf', '4', 'acoustic'][100], -13)
        self.assertAlmostEqual(data['hpf', '10', 'acoustic'][100], -1)
        self.assertAlmostEqual(data['hpf', '12', 'acoustic'][100], 0)
        self.assertAlmostEqual(tables.cutoff({100: -9, 200: -3, 400: 0}), 200)
        self.assertIsNone(tables.cutoff({100: -1, 200: 0}))
        broken = [(cmd, dict(reply, hz=999) if cmd[:1] == ['tone'] else reply) for cmd, reply in records]
        with self.assertRaises(ValueError):
            tables.probe_data(broken)

    def test_generated_block_and_figures(self):
        import audio_doc_tables as audio
        from audio.mic.mic_eq_probe_figures import render
        fixture = ROOT / 'launcher/test/fixtures/mic_eq_probe_capture.txt'
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            document = output / 'probe.md'
            document.write_bytes(b'<!-- generated: mic-eq-probe -->\r\n<!-- /generated: mic-eq-probe -->\r\n')
            self.assertTrue(audio.update_eq_probe(output / 'absent.txt', document))
            self.assertIn('No capture yet.', document.read_text())
            self.assertTrue(audio.update_eq_probe(fixture, document))
            self.assertFalse(audio.update_eq_probe(fixture, document, check=True))
            self.assertIn('Error (dB)', document.read_text())
            self.assertIn('Cutoff relative to code 12', document.read_text())
            self.assertIn('100 Hz raw (dBFS)', document.read_text())
            self.assertNotIn(b'\n', document.read_bytes().replace(b'\r\n', b''))
            render(fixture, output)
            for owner in ('adc', 'dac', 'hpf'):
                light = output / ('mic-eq-probe-' + owner + '.png')
                dark = output / ('mic-eq-probe-' + owner + '-dark.png')
                self.assertTrue(light.exists())
                self.assertNotEqual(light.read_bytes(), dark.read_bytes())

    def test_capture_and_failed_write_restore(self):
        for fail in (False, True):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as directory:
                args = sweep.parser().parse_args(['--board', 'TEST', '--out', str(Path(directory) / 'raw.txt'),
                                                 '--hz', '100,1000', '--hpf-hz', '100,200', '--codes', '4',
                                                 '--runs', '1', '--take', str(Path(directory) / 'take.wav')])
                calls = []
                failed = False
                def runner(command, *words):
                    nonlocal failed
                    self.assertEqual(words[:2], ('--board', 'TEST'))
                    words = words[2:]
                    calls.append(words)
                    if words[0] == 'buildid':
                        return '{"build_id":"synthetic"}'
                    if words[0] == 'codec':
                        reg = int(words[1], 0)
                        if len(words) == 3 and fail and not failed:
                            failed = True
                            raise OSError('write failed')
                        value = int(words[2], 0) if len(words) == 3 else reg
                        return f'CODEC reg=0x{reg:02X} value=0x{value:02X}'
                    if words[0] == 'tone':
                        return json.dumps(dict(hz=int(words[1]), tone_db=-20,
                                               path='internal' if '--internal' in words else 'acoustic'))
                    return 'ok'
                with mock.patch.object(sweep, 'autana_run', side_effect=runner):
                    if fail:
                        with self.assertRaises(OSError):
                            sweep.capture(args)
                    else:
                        sweep.capture(args)
                restored = [(int(w[1], 0), int(w[2], 0)) for w in calls[-len(sweep.REGISTERS):]]
                self.assertEqual(dict(restored), {reg: reg for reg in sweep.REGISTERS})
                self.assertEqual([reg for reg, value in restored[-2:]], [sweep.ADC_CONTROL, sweep.DAC_CONTROL])
                self.assertTrue(all(('tune', 'reset', 'mic.' + name) in calls for name in sweep.TUNABLES))
                text = args.out.read_text()
                self.assertIn('buildid\t', text)
                if not fail:
                    self.assertIn('--internal', text)
                    self.assertIn('mic record 2000', text)


if __name__ == '__main__':
    unittest.main()
