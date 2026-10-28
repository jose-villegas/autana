"""CSD evidence, quantization and filter stability."""
import math
from pathlib import Path
import sys
import unittest

import numpy as np
from scipy.signal import butter, freqz

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from audio.mic import eq_design as eq

MEASURED_WORDS = (0x0F860E31, 0x20F3E39F, 0x0F860E31, 0x20F7853B, 0x0F0FBDFE)


class CsdTest(unittest.TestCase):
    def test_designed_shelf_rows(self):
        # Independent designs, with the gain sequence published by Firia Labs CodeX:
        # https://docs.firialabs.com/_downloads/503ea212986fcc9ea8c8530dd11a60b2/codec.py
        ideal = [eq.eq_design_first_order_shelf(250, -gain, 16000) for gain in range(27)]
        ideal_gains = [20 * math.log10((b0 + b1) / (1 - a1)) for b0, b1, a1 in ideal]
        np.testing.assert_allclose(np.diff(ideal_gains), -1, atol=1e-12)
        rows = [[eq.eq_decode(eq.eq_encode(v)) for v in
                 eq.eq_design_first_order_shelf(250, -gain, 16000)] for gain in range(27)]
        feedback = [row[-1] for row in rows]
        self.assertTrue(all(a > b for a, b in zip(feedback, feedback[1:])))
        gains = [20 * math.log10((b0 + b1) / (1 - a1)) for b0, b1, a1 in rows]
        np.testing.assert_allclose(np.diff(gains), -1, atol=.1)

    def test_measured_words(self):
        coeff = list(map(eq.eq_decode, MEASURED_WORDS))
        _, response = freqz(coeff[:3], [1, *(-v for v in coeff[3:])],
                            worN=[50, 100, 150, 1000, 4000], fs=22050)
        np.testing.assert_allclose(20 * np.log10(abs(response)), [.9, 1, 1, .9, 2.3], atol=.3)
        self.assertEqual(eq.eq_decode(0), eq.EQ_TERMS)

    def test_roundtrip_and_zero(self):
        rng = np.random.default_rng(37)
        for _ in range(1000):
            fields = rng.integers(0, 1 << eq.EQ_FIELD_BITS, eq.EQ_TERMS)
            word = 0
            for field in fields:
                word = (word << eq.EQ_FIELD_BITS) | int(field)
            value = eq.eq_decode(word)
            self.assertEqual(eq.eq_decode(eq.eq_encode(value)), value)
        for value in (-eq.EQ_MAX, 0, eq.EQ_MAX):
            self.assertEqual(eq.eq_decode(eq.eq_encode(value)), value)
        self.assertNotEqual(eq.eq_encode(0), 0)
        for value in (-eq.EQ_MAX - 1, eq.EQ_MAX + 1, math.nan, math.inf):
            with self.assertRaises(ValueError):
                eq.eq_encode(value)

    def test_optimal_against_exhaustive_sums(self):
        limit = eq.EQ_TERMS * eq.EQ_UNIT
        reachable = np.zeros(2 * limit + 1, dtype=bool)
        reachable[limit] = True
        for _ in range(eq.EQ_TERMS):
            following = np.zeros_like(reachable)
            for shift in range(eq.EQ_SHIFT_MAX + 1):
                power = eq.EQ_UNIT >> shift
                following[power:] |= reachable[:-power]
                following[:-power] |= reachable[power:]
            reachable = following
        exact = (np.flatnonzero(reachable) - limit) / eq.EQ_UNIT
        for seed in (1, 37, 913):
            for value in np.random.default_rng(seed).uniform(-2, 2, 100):
                error = abs(eq.eq_decode(eq.eq_encode(value)) - value)
                self.assertLessEqual(error, np.min(abs(exact - value)))

    def test_quantized_highpass(self):
        coeff = [eq.eq_decode(eq.eq_encode(v)) for v in
                 eq.eq_design_biquad('highpass', 150, math.sqrt(.5), 22050)]
        denominator = [1, *(-v for v in coeff[3:])]
        _, response = freqz(coeff[:3], denominator, worN=[150], fs=22050)
        self.assertAlmostEqual(20 * math.log10(abs(response[0])), -3, delta=.5)
        self.assertTrue(np.all(np.abs(np.roots(denominator)) < 1))

    def test_first_order_highpass_matches_scipy(self):
        b, a = butter(1, 300, 'highpass', fs=22050)
        np.testing.assert_allclose(eq.eq_design_first_order_highpass(300, 22050), [*b, -a[1]], rtol=1e-12)

    def test_csd_capture_and_verdicts(self):
        import tempfile
        from unittest import mock
        from audio.mic import eq_probe_sweep as sweep
        from audio.mic import mic_eq_probe_tables as tables
        from audio.mic.tests.fake_recorder import FakeRecorder
        from audio_doc_tables import read_capture, markdown
        registers = dict.fromkeys(sweep.REGISTERS, 0)
        original = dict(registers)
        def tone(words):
            gain = 0
            for owner, control, mask, regs in (
                    ('adc', sweep.ADC_CONTROL, sweep.ADC_BYPASS, sweep.ADC_REGS),
                    ('dac', sweep.DAC_CONTROL, sweep.DAC_BYPASS, sweep.DAC_REGS)):
                if registers[control] & mask:
                    continue
                coeff = [eq.eq_decode(int.from_bytes(bytes(registers[r + i] for i in
                         range(sweep.COEFF_BYTES)), 'big')) for r in regs]
                split = eq.NUMERATOR_COEFFICIENTS[owner]
                _, response = freqz(coeff[:split], [1, *(-v for v in coeff[split:])],
                                    worN=[int(words[1])], fs=sweep.SAMPLE_RATE_HZ)
                gain += 20 * math.log10(abs(response[0]))
            return dict(hz=int(words[1]), path='internal' if '--internal' in words else 'acoustic', tone_db=-20 + gain)
        runner = FakeRecorder(registers, 'synthetic-csd', tone)
        with tempfile.TemporaryDirectory() as directory:
            args = sweep.parser().parse_args(['--board', 'TEST', '--mode', 'csd', '--runs', '1',
                                             '--out', str(Path(directory) / 'csd.txt')])
            with mock.patch.object(sweep, 'autana_run', side_effect=runner), mock.patch.object(sweep.time, 'sleep'):
                sweep.capture(args)
            _, records, _ = read_capture(args.out)
            body = tables.render_csd(args.out, records, lambda *args: '', markdown, '')
            self.assertEqual(body.count('PASS'), 4)
            self.assertNotIn('FAIL', body)
            self.assertEqual(registers, original | {sweep.CSM_REG: sweep.CSM_ON})
            for owner, cases in sweep.csd_designs().items():
                for case in cases:
                    points = tables.probe_data(records)[owner, case, 'acoustic']
                    self.assertEqual(set(points), set(sweep.DEFAULT_HZ))
            broken = [(command, dict(reply, tone_db=reply['tone_db'] + 10)
                       if command[:1] == ['tone'] and int(command[1]) == 1000 else reply)
                      for command, reply in records]
            # A common baseline shift cancels; a changed case must fail.
            index = next(i for i, (command, _) in enumerate(broken) if command == ['probe', 'adc', 'csd-highpass'])
            for i in range(index + 1, len(broken)):
                command, reply = broken[i]
                if command[:1] == ['probe']:
                    break
                if command[:2] == ['tone', '1000']:
                    broken[i] = command, dict(reply, tone_db=reply['tone_db'] + 10)
            self.assertIn('adc csd-highpass: FAIL', tables.render_csd(args.out, broken, lambda *args: '', markdown, ''))
