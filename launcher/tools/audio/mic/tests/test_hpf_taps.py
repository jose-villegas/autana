"""Synthetic HPF bursts, threshold boundaries and capture cleanup."""
from pathlib import Path
import hashlib
import sys
import tempfile
import unittest
from unittest.mock import patch
import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from audio.mic import hpf_tap_capture as capture
from audio.mic import hpf_tap_tables as tables
from audio.mic import eq_probe_sweep as sweep
from audio.mic.tests.fake_recorder import FakeRecorder
from audio_doc_tables import markdown, update_hpf_taps


class HpfTapTests(unittest.TestCase):
    def test_thresholds(self):
        self.assertEqual(tables.verdict(-10, 1), 'cuts taps; keeps speech')
        self.assertEqual(tables.verdict(-10, -1), 'cuts taps; keeps speech')
        self.assertEqual(tables.verdict(-9.999, 1.001), 'keeps taps; changes speech')
        self.assertEqual(tables.verdict(-10.001, -.999), 'cuts taps; keeps speech')
        self.assertEqual(tables.verdict(-11, -1.001), 'cuts taps; changes speech')

    def test_capture_and_failure_restore(self):
        for fail in (False, True):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as directory:
                registers = dict.fromkeys(sweep.REGISTERS, 0)
                registers[sweep.CSM_REG] = sweep.CSM_ON
                original = dict(registers)
                fake = FakeRecorder(registers, 'synthetic', lambda words: dict(hz=int(words[1]), tone_db=-20),
                                    fail_command=('mic', 'record') if fail else None)
                args = capture.parser().parse_args(['--board', 'TEST', '--runs', '2', '--out', str(Path(directory) / 'raw.txt')])
                with patch.object(capture, 'autana_run', side_effect=fake), patch.object(sweep.time, 'sleep'):
                    if fail:
                        with self.assertRaises(OSError):
                            capture.capture(args)
                    else:
                        capture.capture(args)
                self.assertEqual(registers, original)
                self.assertIn(('sound', 'stop', 'all'), fake.calls)
                for key in sweep.TUNABLES:
                    self.assertIn(('tune', 'reset', 'mic.' + key), fake.calls)
                if fail:
                    continue
                calls = fake.calls
                takes = [words for words in calls if words[:2] == ('mic', 'record')]
                self.assertEqual(len(takes), 2 * len(capture.arms()))
                expected = [str(word) for at, hz, ms, gain in capture.schedule()
                            for word in ('--at', at, 'tone', hz, ms, gain)]
                for take in takes:
                    self.assertEqual(list(map(str, take[5:-3])), expected)
                stages = [words for words in calls if words[:2] == ('tune', 'mic.hpf_s1')]
                self.assertEqual([int(words[2]) for words in stages[1:]], [arm['code'] for arm in capture.arms()])
                speech = [words for words in calls if words[0] == 'tone']
                self.assertEqual([int(words[1]) for words in speech], list(capture.SPEECH_HZ) * len(takes))

    def test_wav_metrics_and_table(self):
        rate = 22050
        records = [(['runs'], 2)]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, arm in enumerate(capture.arms()):
                records.append((['arm'], dict(arm, index=index)))
                for run in range(2):
                    samples = np.zeros(rate * capture.TAKE_MS // 1000)
                    events = []
                    for at, hz, duration, gain in capture.schedule():
                        block = int(np.ceil(rate * at / 1000 / tables.BLOCK_SAMPLES))
                        start = block * tables.BLOCK_SAMPLES
                        n = rate * duration // 1000
                        amplitude = .2 * (10 ** (-12 / 20) if index and hz == capture.THUMP_HZ else 1)
                        samples[start:start + n] = amplitude * np.sin(2 * np.pi * hz * np.arange(n) / rate)
                        events.append([block, hz, duration, gain])
                    name = f'take-{index}-{run}.wav'
                    sf.write(root / name, samples, rate, subtype='PCM_16')
                    state = dict(rate=rate, samples=len(samples), tones=events)
                    records.append((['mic', 'record', str(capture.TAKE_MS), '-o', name], state))
                    for hz in capture.SPEECH_HZ:
                        records.append((['tone', str(hz)], dict(hz=hz, tone_db=-20)))
            groups = tables.capture_data(root / 'capture.txt', records)
            rows = tables.rows(groups)
            self.assertAlmostEqual(float(rows[1][1]), -12, delta=.02)
            self.assertAlmostEqual(float(rows[1][2]), 0, delta=.01)
            self.assertEqual(rows[1][-1], 'cuts taps; keeps speech')
            self.assertEqual(float(rows[1][3]), 0)
            self.assertEqual(float(rows[1][4]), 0)
            self.assertIn('Code 10 + EQ 150 Hz', tables.render(root / 'capture.txt', records, markdown))
            groups[1]['tones'][2000] = [-22, -22]
            self.assertEqual(float(tables.rows(groups)[1][3]), -2)
            self.assertEqual(tables.rows(groups)[1][-1], 'cuts taps; changes speech')
            for broken in (records[:-1], [pair for pair in records if pair[0] != ['arm'] or pair[1]['index'] != 0]):
                with self.assertRaises(ValueError):
                    tables.capture_data(root / 'capture.txt', broken)
            expected = tables.render(root / 'capture.txt', records, markdown, check=False)
            for wav in root.glob('*.wav'):
                content = wav.read_bytes()
                wav.write_text('version https://git-lfs.github.com/spec/v1\n'
                               + f'oid sha256:{hashlib.sha256(content).hexdigest()}\nsize {len(content)}\n', newline='\n')
            self.assertEqual(tables.render(root / 'capture.txt', records, markdown, check=True), expected)
            sf.write(root / name, samples, rate, subtype='PCM_16')
            state['tones'] = []
            with self.assertRaises(ValueError):
                tables.take_metrics(root / name, state)
            document = root / 'audio.md'
            document.write_bytes(b'<!-- generated: mic-hpf-taps -->\r\n<!-- /generated: mic-hpf-taps -->\r\n')
            self.assertTrue(update_hpf_taps(root / 'absent.txt', document))
            self.assertFalse(update_hpf_taps(root / 'absent.txt', document, check=True))
            self.assertIn('No capture yet.', document.read_text())
            self.assertNotIn(b'\n', document.read_bytes().replace(b'\r\n', b''))


if __name__ == '__main__':
    unittest.main()
