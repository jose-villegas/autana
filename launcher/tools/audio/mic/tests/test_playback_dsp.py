"""Synthetic playback level and volume-step measurements."""
import sys
import hashlib
import json
from pathlib import Path
import unittest
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from audio.mic import mic_metrics as metrics
from audio.mic.tests.fake_recorder import FakeRecorder

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
        self.assertIn('ADC: within Q2.28 design tolerance.', body)
        self.assertIn('DAC: Q2.28 words mis-encoded.', body)

class PlaybackTablesTests(unittest.TestCase):
    def test_fixture_and_generated_block(self):
        import tempfile
        import soundfile as sf
        from audio_doc_tables import read_capture, markdown, update_playback
        from audio.mic.playback_dsp_tables import playback_data, render, BLOCK_SAMPLES
        from audio.mic.playback_dsp_capture import TONE_HZ, STEP_DB
        root = Path(__file__).resolve().parents[5]
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / 'capture.txt'
            fixture.write_bytes((root / 'launcher/test/fixtures/mic_playback_capture.txt').read_bytes())
            _, records, _ = read_capture(fixture)
            state = next(reply for command, reply in records if command[:2] == ['mic', 'record'] and reply['writes'])
            rate = state['rate']
            samples = .5 * np.cos(2 * np.pi * TONE_HZ * np.arange(state['samples']) / rate)
            first, last = (write[0] * BLOCK_SAMPLES for write in state['writes'])
            samples[first:last] *= 10 ** (-STEP_DB / metrics.DB_AMPLITUDE)
            sf.write(Path(directory) / 'mic_playback_take.wav', samples, rate, subtype='PCM_16')
            for index, (command, reply) in enumerate(records):
                if command[:2] == ['mic', 'record']:
                    name = f'take-{index}.wav'
                    (Path(directory) / name).write_bytes((Path(directory) / 'mic_playback_take.wav').read_bytes())
                    command[command.index('-o') + 1] = name
            fixture.write_text('# autana --json replies on TEST, 2000-01-01, fixture\n'
                               + '\n'.join(' '.join(command) + '\t' + json.dumps(reply)
                                           for command, reply in records) + '\n')
            groups = playback_data(fixture, records)
            self.assertEqual(len(groups), 20)
            self.assertAlmostEqual(groups[0]['takes'][0]['active'], -11, delta=1)
            body = render(fixture, records, markdown)
            self.assertIn('Down click excess (dB)', body)
            self.assertIn('200 Hz THD (dB)', body)
            self.assertNotIn('synthetic', body)
            document = Path(directory) / 'audio.md'
            document.write_bytes(b'<!-- generated: mic-playback-dsp -->\r\n<!-- /generated: mic-playback-dsp -->\r\n')
            self.assertTrue(update_playback(fixture, document))
            self.assertFalse(update_playback(fixture, document, check=True))
            self.assertNotIn(b'\n', document.read_bytes().replace(b'\r\n', b''))
            for wav in Path(directory).glob('take-*.wav'):
                content = wav.read_bytes()
                wav.write_text('version https://git-lfs.github.com/spec/v1\n'
                               + f'oid sha256:{hashlib.sha256(content).hexdigest()}\nsize {len(content)}\n', newline='\n')
            self.assertFalse(update_playback(fixture, document, check=True))
            broken = [(command, reply) for command, reply in records if command != ['arm'] or reply['index'] != 0]
            with self.assertRaises(ValueError):
                playback_data(fixture, broken)

class CaptureTests(unittest.TestCase):
    def test_ramp_attenuates_both_volume_registers(self):
        from audio.mic import playback_dsp_capture as capture
        from unittest.mock import Mock, patch
        initial = capture.DEFAULTS['alc_gain']
        saved = {capture.ADC_RAMP_REG: 0}
        for owner, register in (('adc', capture.ADC_VOLUME_REG), ('dac', capture.DAC_VOLUME_REG)):
            with self.subTest(owner=owner):
                with patch.object(capture, 'codec', return_value=initial), \
                        patch.object(capture, 'music_take') as take:
                    capture.ramp_take(Mock(), saved, None, dict(block=owner + '-ramp', ramp=0), 'take')
                self.assertEqual(take.call_args.args[3], (
                    (capture.STEP_AT_MS[0], register, initial - capture.REGISTER_STEP),
                    (capture.STEP_AT_MS[1], register, initial)))

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
                for reg in (capture.ADC_VOLUME_REG, capture.DAC_VOLUME_REG):
                    registers[reg] = capture.DEFAULTS['alc_gain']
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
                    self.assertFalse(any(words[:2] == ('sound', 'tone') for words in calls))
                    ramps = [words for words in takes if '--at' in words]
                    for take in ramps:
                        start = take.index('--at')
                        self.assertEqual(take[start:start + 6], ('--at', -capture.SETTLE_MS, 'tone', capture.TONE_HZ,
                                                               capture.TAKE_MS + capture.SETTLE_MS, capture.GAIN))
                    self.assertNotIn(str(Path(directory)), args.out.read_text())

class RestartCaptureTests(unittest.TestCase):
    def test_restart_mode_reads_all_coefficients_and_internal_baseline(self):
        import tempfile
        from unittest.mock import patch
        from audio.mic import eq_probe_sweep as sweep
        from audio_doc_tables import read_capture
        registers = dict.fromkeys(sweep.REGISTERS, 0)
        registers[sweep.CSM_REG] = sweep.CSM_ON
        original = dict(registers)
        runner = FakeRecorder(registers, 'synthetic', lambda words: dict(
            hz=int(words[1]), tone_db=-20,
            path='internal' if '--internal' in words else 'acoustic'))
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
        self.assertEqual(sum('--internal' in words for words in runner.calls), 15)
