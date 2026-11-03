"""Capture metrics remain verifiable without downloaded LFS audio."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from audio.mic import mic_metrics as metrics


class TakeMetricsTests(unittest.TestCase):
    def test_real_pointer_and_refresh(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'take.wav'
            sf.write(path, np.zeros(100), 22050, subtype='PCM_16')
            audio = path.read_bytes()
            digest = hashlib.sha256(audio).hexdigest()
            def analyze(samples, rate):
                return dict(samples=len(samples), rate=rate)
            expected = metrics.read_take_metrics(path, analyze, check=False)
            self.assertEqual(metrics.read_take_metrics(path, analyze, check=True), expected)
            manifest = path.with_name('take_metrics.json')
            entry = json.loads(manifest.read_text())['take.wav']
            self.assertEqual(entry, dict(sha256=digest, metrics=expected))
            with self.assertRaisesRegex(ValueError, 'take metrics stale'):
                metrics.read_take_metrics(path, lambda x, rate: {}, check=True)
            sf.write(path, np.zeros(200), 22050, subtype='PCM_16')
            with self.assertRaisesRegex(ValueError, 'take metrics stale'):
                metrics.read_take_metrics(path, analyze, check=True)
            refreshed = metrics.read_take_metrics(path, analyze, check=False)
            self.assertEqual(refreshed['samples'], 200)
            self.assertEqual(json.loads(manifest.read_text())['take.wav']['sha256'],
                             hashlib.sha256(path.read_bytes()).hexdigest())
            path.write_bytes(audio)
            metrics.read_take_metrics(path, analyze, check=False)
            path.write_text(f'version https://git-lfs.github.com/spec/v1\noid sha256:{digest}\nsize {len(audio)}\n', newline='\n')
            self.assertEqual(metrics.read_take_metrics(path, None, check=True), expected)
            entry['sha256'] = '0' * len(digest)
            manifest.write_text(json.dumps({'take.wav': entry}))
            with self.assertRaisesRegex(ValueError, 'take metrics stale; regenerate with the LFS audio present'):
                metrics.read_take_metrics(path, None, check=True)
            manifest.write_text('{}')
            with self.assertRaisesRegex(ValueError, 'take metrics stale; regenerate with the LFS audio present'):
                metrics.read_take_metrics(path, None, check=True)
            manifest.unlink()
            with self.assertRaisesRegex(ValueError, 'take metrics stale; regenerate with the LFS audio present'):
                metrics.read_take_metrics(path, None, check=True)
