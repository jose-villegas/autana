"""Synthetic playback level and volume-step measurements."""
import sys
from pathlib import Path
import unittest
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from audio.mic import mic_metrics as metrics

class PlaybackMetricsTests(unittest.TestCase):
    def test_band_active_level(self):
        rate = 22050
        t = np.arange(rate) / rate
        samples = .2 * np.sin(2 * np.pi * 1000 * t) + .4 * np.sin(2 * np.pi * 100 * t)
        self.assertAlmostEqual(metrics.active_level_db(samples, rate, (300, 3400)),
                               20 * np.log10(.2 / np.sqrt(2)), delta=.15)

    def test_step_click_and_settle(self):
        rate = 22050
        at = rate // 2
        t = np.arange(rate) / rate
        sine = .5 * np.cos(2 * np.pi * 1000 * t)
        hard = sine.copy()
        hard[at:] *= .1
        ramp = sine * np.where(t < .5, 1, .1 + .9 * np.exp(-np.maximum(t - .5, 0) / .01))
        injected = sine.copy()
        injected[at] += .8
        self.assertGreater(metrics.click_excess_db(injected, rate, [at], 20)[0],
                           metrics.click_excess_db(sine, rate, [at], 20)[0] + 10)
        self.assertGreater(metrics.click_excess_db(hard, rate, [at], 20)[0],
                           metrics.click_excess_db(ramp, rate, [at], 20)[0])
        self.assertLess(metrics.settle_ms(hard, rate, at), metrics.settle_ms(ramp, rate, at))

class RestartTests(unittest.TestCase):
    def test_restart_preserves_master_and_reads_each_write(self):
        from audio.mic import eq_probe_sweep as sweep
        from unittest.mock import patch
        calls = []
        original = sweep.CSM_ON | 0x40
        def record(*words):
            calls.append(words)
            value = int(words[2], 0) if len(words) == 3 else original
            return f'CODEC reg=0x00 value=0x{value:02X}'
        record.note = lambda *args: None
        with patch.object(sweep.time, 'sleep'):
            sweep.restart(record, {sweep.CSM_REG: original})
        self.assertEqual([int(words[2], 0) for words in calls if len(words) == 3],
                         [(original & ~sweep.CSM_ON) | sweep.CSM_RESETS,
                          original & ~(sweep.CSM_ON | sweep.CSM_RESETS), original])

    def test_restart_verdicts(self):
        from audio.mic import mic_eq_probe_tables as tables
        from audio.mic import eq_probe_sweep as sweep
        from audio_doc_tables import markdown
        records = [(['buildid'], {'build_id': 'synthetic'})]
        for owner in ('adc', 'dac'):
            for case in ('bypass', *sweep.restart_designs(owner)):
                records.append((['probe', owner, case], {}))
                for path in (('acoustic', 'internal') if owner == 'dac' else ('acoustic',)):
                    for hz in sweep.RESTART_HZ:
                        expected = 0 if case == 'bypass' else tables.restart_expected(owner, case, hz)
                        measured = expected + (10 if owner == 'dac' and case == 'half' else 0)
                        words = ['tone', str(hz), '500', '30'] + (['--internal'] if path == 'internal' else [])
                        records.append((words, dict(hz=hz, path=path, tone_db=-20 + measured)))
        body = tables.render_restart(Path('synthetic.txt'), records, lambda *args: '', markdown, '')
        self.assertIn('ADC: follows.', body)
        self.assertIn('DAC: does not follow.', body)

class PlaybackTablesTests(unittest.TestCase):
    def test_fixture_and_generated_block(self):
        import tempfile
        from audio_doc_tables import read_capture, markdown, update_playback
        from audio.mic.playback_dsp_tables import playback_data, render
        root = Path(__file__).resolve().parents[5]
        fixture = root / 'launcher/test/fixtures/mic_playback_capture.txt'
        _, records, _ = read_capture(fixture)
        groups = playback_data(fixture, records)
        self.assertEqual(len(groups), 20)
        self.assertAlmostEqual(groups[0]['takes'][0]['active'], -11, delta=1)
        body = render(fixture, records, markdown)
        self.assertIn('Down click excess (dB)', body)
        self.assertIn('200 Hz THD (dB)', body)
        self.assertNotIn('synthetic', body)
        with tempfile.TemporaryDirectory() as directory:
            document = Path(directory) / 'audio.md'
            document.write_bytes(b'<!-- generated: mic-playback-dsp -->\r\n<!-- /generated: mic-playback-dsp -->\r\n')
            self.assertTrue(update_playback(fixture, document))
            self.assertFalse(update_playback(fixture, document, check=True))
            self.assertNotIn(b'\n', document.read_bytes().replace(b'\r\n', b''))
        broken = [(command, reply) for command, reply in records if command != ['arm'] or reply['index'] != 0]
        with self.assertRaises(ValueError):
            playback_data(fixture, broken)

class CaptureTests(unittest.TestCase):
    def test_capture_arms_and_failure_cleanup(self):
        from audio.mic import playback_dsp_capture as capture
        from audio.mic import eq_probe_sweep as sweep
        from unittest.mock import patch
        import tempfile
        import json
        for fail in (False, True):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as directory:
                args = capture.parser().parse_args(['--board', 'TEST', '--runs', '1', '--out', str(Path(directory) / 'raw.txt')])
                registers = {reg: 0 for reg in set((*sweep.REGISTERS, capture.ADC_RAMP_REG, capture.ADC_VOLUME_REG,
                                                   capture.DAC_VOLUME_REG))}
                registers[sweep.CSM_REG] = sweep.CSM_ON
                original = dict(registers)
                calls = []
                def runner(command, *words):
                    words = words[2:]
                    calls.append(words)
                    if words[0] == 'codec':
                        reg = int(words[1], 0)
                        if len(words) == 3:
                            registers[reg] = int(words[2], 0)
                        return f'CODEC reg=0x{reg:02X} value=0x{registers[reg]:02X}'
                    if words[:2] == ('mic', 'record') and fail:
                        raise OSError('capture failed')
                    if words[0] == 'buildid':
                        return json.dumps(dict(build_id='synthetic'))
                    if words[0] == 'tone':
                        return json.dumps(dict(hz=int(words[1]), thd_db=-30, peak_dbfs=-6))
                    return 'ok'
                with patch.object(capture, 'autana_run', side_effect=runner), patch.object(sweep.time, 'sleep'):
                    if fail:
                        with self.assertRaises(OSError):
                            capture.capture(args)
                    else:
                        capture.capture(args)
                self.assertEqual(registers, original)
                self.assertIn(('sound', 'stop', 'all'), calls)
                for key in capture.AUDIO_TUNABLES:
                    self.assertIn(('tune', 'reset', 'audio.' + key), calls)
                if not fail:
                    takes = [words for words in calls if words[:2] == ('mic', 'record')]
                    self.assertEqual(len(takes), len(list(capture.arms())))
                    self.assertEqual(sum('--at' in words for words in takes), 2 * len(capture.RAMPS))
                    self.assertNotIn(str(Path(directory)), args.out.read_text())

class RestartCaptureTests(unittest.TestCase):
    def test_restart_mode_reads_all_coefficients_and_internal_baseline(self):
        import json
        import tempfile
        from unittest.mock import patch
        from audio.mic import eq_probe_sweep as sweep
        from audio_doc_tables import read_capture
        calls = []
        registers = dict.fromkeys(sweep.REGISTERS, 0)
        registers[sweep.CSM_REG] = sweep.CSM_ON
        original = dict(registers)
        def runner(command, *words):
            words = words[2:]
            calls.append(words)
            if words[0] == 'buildid':
                return '{"build_id":"synthetic"}'
            if words[0] == 'codec':
                reg = int(words[1], 0)
                if len(words) == 3:
                    registers[reg] = int(words[2], 0)
                return f'CODEC reg=0x{reg:02X} value=0x{registers[reg]:02X}'
            if words[0] == 'tone':
                return json.dumps(dict(hz=int(words[1]), tone_db=-20,
                                       path='internal' if '--internal' in words else 'acoustic'))
            return 'ok'
        with tempfile.TemporaryDirectory() as directory:
            args = sweep.parser().parse_args(['--board', 'TEST', '--mode', 'restart', '--runs', '1',
                                             '--out', str(Path(directory) / 'restart.txt')])
            with patch.object(sweep, 'autana_run', side_effect=runner), patch.object(sweep.time, 'sleep'):
                sweep.capture(args)
            _, records, _ = read_capture(args.out)
        self.assertEqual(registers, original)
        self.assertEqual(sum(command == ['restart'] for command, reply in records), 9)
        readbacks = [(command, reply) for command, reply in records if command[:1] == ['coefficients']]
        self.assertEqual(len(readbacks), 8)
        for command, reply in readbacks:
            registers = sweep.ADC_REGS if command[1] == 'adc' else sweep.DAC_REGS
            self.assertEqual(set(reply), {hex(reg + offset) for reg in registers for offset in range(sweep.COEFF_BYTES)})
        self.assertEqual(sum('--internal' in words for words in calls), 15)
