"""Tests the host mic metrics (levels, clips, alignment, SNR, STOI, takes), noise generation and EQ designs."""
import sys
import unittest
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import eq_design as e
import mic_metrics as m

class MetricsTests(unittest.TestCase):
    def setUp(self):
        self.rate = 16000
        self.t = np.arange(self.rate * 2) / self.rate
        self.ref = 0.2 * np.sin(2 * np.pi * 1000 * self.t)
    def test_levels(self):
        self.assertAlmostEqual(m.band_level_db(self.ref, 900, 1100, self.rate), -16.9897, places=3)
        self.assertLess(m.low_band_db(self.ref, 200, self.rate), -100)
        self.assertEqual(m.clip_count(np.array([-1., -.5, 0., 1.])), 2)
        self.assertAlmostEqual(m.active_level_db(self.ref, self.rate), -16.9897, delta=.1)
        self.assertAlmostEqual(m.transient_peak_db(self.ref, 1, 100, self.rate), -13.9794, places=3)
    def test_clip_rails(self):
        pcm = np.array([-32768, -32767, 0, 32766, 32767]) / 32768
        self.assertEqual(m.clip_count(pcm), 2)

    def test_alignment(self):
        for seed in (14, 107, 983):
            rng = np.random.default_rng(seed)
            ref = rng.normal(size=4000)
            for offset in (173, -79):
                x = np.pad(ref, (offset, 0)) if offset > 0 else ref[-offset:]
                aligned, target = m.align(x, ref)
                np.testing.assert_allclose(aligned, target)
    def test_snr(self):
        from scipy.signal import butter, sosfilt
        for seed in (41, 107, 983):
            rng = np.random.default_rng(seed)
            noise = rng.normal(size=self.ref.size)
            noise = sosfilt(butter(6, [300, 3400], fs=self.rate, btype='bandpass', output='sos'), noise)
            noise *= np.sqrt(np.mean(self.ref ** 2) / np.mean(noise ** 2)) / 10
            self.assertAlmostEqual(m.speech_band_snr_db(self.ref + noise, self.ref, self.rate), 20, delta=.4)
        self.assertTrue(np.all(m.speech_mask(self.ref, self.rate)))
    def test_stoi(self):
        rng = np.random.default_rng(27)
        ref = rng.normal(size=self.rate * 3) * (1 + np.sin(2 * np.pi * 3 * np.arange(self.rate * 3) / self.rate))
        self.assertGreaterEqual(m.stoi_score(ref, ref, self.rate), .99)
    def test_read_take(self):
        import tempfile
        import soundfile as sf
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'take.wav'
            sf.write(path, self.ref, self.rate, subtype='PCM_16')
            samples, rate = m.read_take(path)
            self.assertEqual(rate, self.rate)
            np.testing.assert_allclose(samples, self.ref, atol=1 / 32768)

    def test_noise_generator(self):
        from generate_noise import generate
        for kind in ('pink', 'babble'):
            for seed in (1, 27, 48):
                x = generate(kind, 2, self.rate, seed)
                self.assertEqual(len(x), self.rate * 2)
                self.assertAlmostEqual(np.max(np.abs(x)), .8)
                np.testing.assert_array_equal(x, generate(kind, 2, self.rate, seed))
        x = generate('pink', 4, self.rate, 9)
        self.assertAlmostEqual(m.band_level_db(x, 500, 1000, self.rate),
                               m.band_level_db(x, 1000, 2000, self.rate), delta=1)

    def test_eq_presets(self):
        expected = [0x0F860E31, 0x20F3E39F, 0x0F860E31, 0x20F7853B, 0x0F0FBDFE]
        coeff = e.eq_design_biquad('highpass', 150, 2 ** -.5, 22050)
        self.assertEqual([e.eq_encode(v) for v in coeff], expected)
        from scipy.signal import freqz
        _, response = freqz(coeff[:3], np.r_[1, coeff[3:]], worN=[50, 150, 1000], fs=22050)
        np.testing.assert_allclose(20 * np.log10(np.abs(response)), [-19.1, -3.01, 0], atol=.1)

    def test_eq(self):
        self.assertEqual(e.eq_encode(1), 0x10000000)
        self.assertEqual(e.eq_encode(-1), 0x30000000)
        coeff = e.eq_design_biquad('highpass', 150, .70710678, self.rate)
        self.assertEqual(len(coeff), 5)
        self.assertAlmostEqual(sum(coeff[:3]), 0, places=10)

    def test_first_order_highpass_matches_scipy(self):
        from scipy.signal import butter
        b, a = butter(1, 300, 'highpass', fs=22050)
        np.testing.assert_allclose(e.eq_design_first_order_highpass(300, 22050), [*b, a[1]], rtol=1e-12)

if __name__ == '__main__': unittest.main()
