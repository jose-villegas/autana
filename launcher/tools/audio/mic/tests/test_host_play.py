"""Exercise host playback against the real PortAudio runtime."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
import soundfile as sf

SCRIPT = Path(__file__).resolve().parents[1] / 'host_play.py'
TONE_RATE = 48000
TONE_SECONDS = 0.2
TONE_HZ = 440
TONE_DBFS = -40
DB_AMPLITUDE = 20


class HostPlayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.listing = cls.run_script('--list')

    @staticmethod
    def run_script(*args):
        return subprocess.run([sys.executable, str(SCRIPT), *args],
                              capture_output=True, text=True, timeout=30)

    def assert_clear_failure(self, result):
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('Traceback', result.stderr)
        lines = [line for line in result.stderr.splitlines()
                 if line.startswith('host_play:')]
        self.assertEqual(len(lines), 1, result.stderr)
        self.assertRegex(lines[0], r'(no output device|PortAudio.*missing)')
        self.assertRegex(lines[0], r'(connect|install)')

    def test_list(self):
        if self.listing.returncode:
            self.assert_clear_failure(self.listing)
        else:
            self.assertRegex(self.listing.stdout, r'Output device [0-9]+: .+ \(.+\)')

    def test_play_quiet_tone(self):
        with tempfile.TemporaryDirectory() as tmp:
            wav = Path(tmp) / 'quiet.wav'
            time = np.arange(round(TONE_RATE * TONE_SECONDS)) / TONE_RATE
            tone = 10 ** (TONE_DBFS / DB_AMPLITUDE) * np.sin(2 * np.pi * TONE_HZ * time)
            sf.write(wav, tone, TONE_RATE, subtype='FLOAT')
            result = self.run_script(str(wav))
        if self.listing.returncode:
            self.assert_clear_failure(result)
        else:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertRegex(result.stderr, r'Playing: device=.+ host_api=.+ rate=48000 Hz duration=0.200 s gain=0.00 dB peak=-40.00 dBFS')
            self.assertRegex(result.stderr, r'Played: 0.200 s status=.+')


if __name__ == '__main__':
    unittest.main()
