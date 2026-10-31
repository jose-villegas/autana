"""Voice capture sequencing, reference metrics, cache and decision boundaries."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
from scipy import signal
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from audio.mic import eq_voice_capture as capture
from audio.mic import eq_voice_tables as tables
from audio.mic import eq_probe_sweep as sweep
from audio.mic import mic_metrics as metrics
from audio.mic.tests.fake_recorder import FakeRecorder, FakeHostPlayer
import audio_doc_tables as audio
from audio_doc_tables import markdown, read_capture, update_eq_voice


class VoiceTests(unittest.TestCase):
    def test_peak(self):
        from audio.mic.eq_design import eq_design_biquad, eq_decode, eq_encode
        rate = capture.SAMPLE_RATE_HZ
        coeff = eq_design_biquad('peaking', 3000, 1, gain_db=6)
        amplitude = 10 ** (6 / 40)
        warped = 2 * rate * np.tan(np.pi * 3000 / rate)
        b, a = signal.bilinear([1, amplitude * warped, warped ** 2],
                               [1, warped / amplitude, warped ** 2], fs=rate)
        hz = np.linspace(1, rate / 2 - 1, 1000)
        _, expected = signal.freqz(b, a, worN=hz, fs=rate)
        _, peak = signal.freqz(coeff[:3], [1, *(-v for v in coeff[3:])], worN=hz, fs=rate)
        np.testing.assert_allclose(peak, expected, atol=1e-12)
        inverse = eq_design_biquad('peaking', 3000, 1, gain_db=-6)
        _, cut = signal.freqz(inverse[:3], [1, *(-v for v in inverse[3:])], worN=hz, fs=rate)
        np.testing.assert_allclose(peak * cut, 1, atol=1e-12)
        quantized = [eq_decode(eq_encode(v)) for v in coeff]
        self.assertTrue(np.all(abs(np.roots([1, *(-v for v in quantized[3:])])) < 1))
        _, actual = signal.freqz(quantized[:3], [1, *(-v for v in quantized[3:])], worN=capture.SPEECH_HZ, fs=rate)
        np.testing.assert_allclose(20 * np.log10(abs(actual)), tables.designed_response(coeff), atol=.1)

    def test_truncated_speech_rejected(self):
        rate = capture.SAMPLE_RATE_HZ
        rng = np.random.default_rng(37)
        source = np.pad(rng.normal(0, .03, rate * 3), (rate, rate))
        with self.assertRaisesRegex(ValueError, 'truncated'):
            tables.analyze_take(source[2 * rate:], rate, source)

    def test_speech_gap_snr(self):
        rate = capture.SAMPLE_RATE_HZ
        time = np.arange(rate * 4) / rate
        reference = np.zeros(len(time))
        reference[rate:2 * rate] = .1 * np.sin(2 * np.pi * 1000 * time[rate:2 * rate])
        noise = .01 * np.sin(2 * np.pi * 500 * time)
        snr = metrics.speech_gap_snr_db(reference + noise, reference, rate)
        self.assertAlmostEqual(snr, 10 * np.log10(1 + 100 / 1.2), delta=.1)
        self.assertAlmostEqual(metrics.speech_gap_snr_db((reference + noise) * .1, reference, rate), snr)
        with self.assertRaises(ValueError):
            metrics.speech_gap_snr_db(reference[:rate], reference[:rate], rate)

    def test_material_snr(self):
        for seed in (1, 37, 913):
            rng = np.random.default_rng(seed)
            sentence = rng.normal(0, .05, capture.SAMPLE_RATE_HZ * 2)
            clean, noisy = capture.material_samples(sentence, capture.SAMPLE_RATE_HZ, seed)
            noise = noisy - clean
            snr = metrics.active_level_db(clean) - metrics.noise_dbfs(noise)
            self.assertAlmostEqual(snr, capture.NOISE_SNR_DB, delta=.1)
            self.assertEqual(len(clean), len(sentence) + 2 * capture.SAMPLE_RATE_HZ)

    def test_material_builder(self):
        rate = 16000
        time = np.arange(rate * 2) / rate
        source = .1 * np.sin(2 * np.pi * 500 * time)
        with tempfile.TemporaryDirectory() as directory, patch.object(metrics, 'read_take', return_value=(source, rate)):
            materials = capture.build_materials(Path(directory))
            self.assertEqual(len(materials), len(capture.SENTENCES) * 2)
            for sentence in capture.SENTENCES:
                clean, output_rate = sf.read(materials[sentence + '-quiet']['wav'])
                noisy, _ = sf.read(materials[sentence + '-noise']['wav'])
                self.assertEqual(output_rate, capture.SAMPLE_RATE_HZ)
                self.assertEqual(len(clean), 4 * output_rate)
                self.assertTrue(np.all(clean[:output_rate] == 0))
                self.assertTrue(np.all(clean[-output_rate:] == 0))
                self.assertAlmostEqual(metrics.active_level_db(clean, output_rate)
                                       - metrics.noise_dbfs(noisy - clean), capture.NOISE_SNR_DB, delta=.1)

    def test_verdict_boundaries(self):
        self.assertEqual(tables.verdict(-.01, .02, 0, 1.5), 'helps; matches design')
        self.assertEqual(tables.verdict(-.01, 0, 1, 1.5), 'helps; matches design')
        self.assertEqual(tables.verdict(-.01001, .03, 2, 1.50001), 'does not help; differs from design')
        self.assertEqual(tables.verdict(0, .01999, .99999, 0), 'does not help; matches design')

    def test_restore_stopped_codec(self):
        registers = dict.fromkeys(sweep.REGISTERS, 0)
        saved = dict(registers)
        fake = FakeRecorder(registers, 'synthetic', lambda words: {})
        def record(*words):
            return fake('fake', '--board', 'TEST', *words)
        record.note = lambda *args: None
        with patch.object(sweep.time, 'sleep'):
            sweep.restore(record, saved, {'audio.drc': 1})
        self.assertEqual(registers, saved)

    def test_capture_restore_and_level_check(self):
        for failure in (None, 'record', 'quiet', 'clip', 'player', 'sweep', 'tone'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                wav = root / 'source.wav'
                rate = capture.SAMPLE_RATE_HZ
                samples = np.sin(2 * np.pi * 500 * np.arange(rate * 2) / rate) * .1
                sf.write(wav, samples, rate)
                materials = {'sentence-quiet': dict(wav=wav, reference=wav, ms=2000)}
                regs = dict.fromkeys(sweep.REGISTERS, 0)
                regs[sweep.CSM_REG] = sweep.CSM_ON
                original = dict(regs)
                settings = {'mic.' + key: value for key, value in sweep.DEFAULTS.items()}
                settings['audio.drc'] = 1
                initial = dict(settings)
                events = []
                fake = FakeRecorder(regs, 'synthetic', lambda words: dict(hz=int(words[1]), tone_db=-20),
                                    fail_command=('mic', 'record') if failure == 'record' else ('tone',) if failure == 'tone' else None,
                                    tunables=settings)
                def run(command, *words):
                    events.append(words[2:])
                    if words[2:4] == ('mic', 'record') and failure != 'record':
                        if failure == 'sweep' and len(player.calls) == 2:
                            raise OSError('injected sweep failure')
                        take = Path(words[words.index('-o') + 1])
                        x = samples if failure != 'quiet' else samples * 1e-4
                        if failure == 'clip':
                            x = np.ones_like(samples)
                        sf.write(take, x, rate, subtype='PCM_16')
                        state = dict(rate=rate, samples=len(x))
                        take.with_suffix('.json').write_text(json.dumps(state), newline='\n')
                        return json.dumps(state)
                    return fake(command, *words)
                args = capture.parser().parse_args(['--board', 'TEST', '--runs', '2', '--out', str(root / 'capture.txt')])
                player = FakeHostPlayer(events, returncode=1 if failure == 'player' else 0)
                with patch.object(capture, 'build_materials', return_value=materials), \
                     patch.object(capture, 'autana_run', side_effect=run), \
                     patch.object(capture.subprocess, 'Popen', side_effect=player), patch.object(sweep.time, 'sleep'):
                    if failure:
                        with self.assertRaises((OSError, ValueError, RuntimeError)):
                            capture.capture(args)
                    else:
                        capture.capture(args)
                self.assertEqual(regs, original)
                self.assertEqual(settings, initial)
                takes = [event for event in events if event[:2] == ('mic', 'record')]
                self.assertEqual(len(takes), 2 if failure == 'sweep' else 3 if failure == 'tone' else
                                 1 if failure else 1 + len(capture.arms()) * args.runs)
                self.assertEqual(len(player.calls), len(takes))
                for at, event in enumerate(events):
                    if event[:2] == ('mic', 'record'):
                        self.assertEqual(events[at - 1][0], 'host-play')
                if failure:
                    continue
                _, records, _ = read_capture(args.out)
                self.assertEqual([reply['name'] for command, reply in records if command == ['arm']], list(capture.arms()))
                programs = [reply for command, reply in records if command == ['arm']]
                self.assertEqual([arm['coefficients'] for arm in programs], list(capture.arms().values()))
                self.assertEqual([int(e[1]) for e in events if e[0] == 'tone'], [hz for _ in capture.arms() for hz in capture.SPEECH_HZ for _ in range(args.runs)])

    def test_synthetic_metrics_and_lfs_table(self):
        rate = capture.SAMPLE_RATE_HZ
        rng = np.random.default_rng(37)
        source = signal.sosfilt(signal.butter(4, [300, 3400], btype='bandpass', fs=rate, output='sos'), rng.normal(size=rate * 3)) * .1
        clean, noisy = capture.material_samples(source, rate, 37)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ref = root / 'ref.wav'
            sf.write(ref, clean, rate, subtype='FLOAT')
            records = [(['runs'], 1), (['materials'], {'s-quiet': {'sentence': 's', 'condition': 'quiet'},
                                                     's-noise': {'sentence': 's', 'condition': 'noise'}})]
            for index, (name, coeff) in enumerate(capture.arms().items()):
                records.append((['arm'], dict(name=name, coefficients=coeff)))
                for condition, x in (('quiet', clean), ('noise', noisy)):
                    if index:
                        x = signal.lfilter([.8, .2], [1], x)
                    take = root / f'{name}-{condition}.wav'
                    sf.write(take, np.pad(x, (500, 800)), rate, subtype='FLOAT')
                    records.append((['material'], 's-' + condition))
                    records.append((['mic', 'record', '-o', take.name], dict(rate=rate, samples=len(x) + 1300)))
                for hz, db in zip(capture.SPEECH_HZ, tables.designed_response(coeff)):
                    records.append((['tone', str(hz)], dict(hz=hz, tone_db=-20 + db)))
            with patch.object(tables, 'clean_reference', return_value=clean):
                data = tables.capture_data(root / 'capture.txt', records, check=False)
                result = tables.rows(data)
                baseline = tables.analyze_take(noisy, rate, clean)
                filtered = tables.analyze_take(signal.lfilter([.8, .2], [1], noisy), rate, clean)
                self.assertAlmostEqual(float(result[1][2]), filtered['stoi'] - baseline['stoi'], delta=.001)
                self.assertAlmostEqual(float(result[1][3]), filtered['snr'] - baseline['snr'], delta=.01)
                self.assertEqual(float(result[1][6]), 0)
                expected = tables.render(root / 'capture.txt', records, markdown, check=True)
                log = root / 'capture.txt'
                log.write_text('# autana --json replies on TEST, 2000-01-01, synthetic\n'
                               + 'buildid\t{"build_id": "synthetic"}\n'
                               + ''.join(' '.join(command) + '\t' + json.dumps(reply) + '\n'
                                         for command, reply in records), newline='\n')
                document = root / 'audio.md'
                document.write_bytes((audio.ROOT / audio.DOCUMENT).read_bytes())
                update_eq_voice(log, document)
                argv = ['audio_doc_tables.py', '--check', '--document', str(document), '--eq-voice-capture', str(log)]
                with patch.object(sys, 'argv', argv):
                    self.assertEqual(audio.main(), 0)
            for take in root.glob('*.wav'):
                content = take.read_bytes()
                take.write_text('version https://git-lfs.github.com/spec/v1\n'
                                + f'oid sha256:{hashlib.sha256(content).hexdigest()}\nsize {len(content)}\n', newline='\n')
            with patch.object(tables, 'clean_reference', side_effect=AssertionError('LFS needs no source')):
                self.assertEqual(tables.render(root / 'capture.txt', records, markdown, check=True), expected)
                with patch.object(sys, 'argv', argv):
                    self.assertEqual(audio.main(), 0)
            with self.assertRaises(ValueError):
                tables.capture_data(root / 'capture.txt', records[:-1])
            doc = root / 'doc.md'
            doc.write_text('<!-- generated: mic-eq-voice -->\n<!-- /generated: mic-eq-voice -->\n', newline='\n')
            update_eq_voice(root / 'absent.txt', doc)
            self.assertFalse(update_eq_voice(root / 'absent.txt', doc, True))
            self.assertIn('No capture yet.', doc.read_text())


if __name__ == '__main__':
    unittest.main()
