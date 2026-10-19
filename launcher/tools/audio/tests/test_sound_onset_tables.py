"""Onset peak tables reject inconsistent captures and stale generated blocks."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "audio_doc_tables", Path(__file__).resolve().parents[1] / "audio_doc_tables.py")
audio = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audio)


class OnsetTablesTest(unittest.TestCase):
    def test_medians_and_worst(self):
        body = audio.render_onset(Path(__file__).parent / "fixtures/onset.txt")
        self.assertIn("start | 3.0 (5.0); n=3", body)
        self.assertIn("baseline median (worst) excess dB; runs", body)

    def test_failed_runs_count_only_successes(self):
        text = (Path(__file__).parent / "fixtures/onset.txt").read_text()
        records = text.splitlines()
        failed = [line for line in records if '\tsound onset start\t' in line]
        for count in (1, len(failed)):
            partial = text
            for line in failed[:count]:
                partial = partial.replace(line, 'baseline\tfailure sound onset start\t{"stderr": "SOUND_ERR capture"}')
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'capture.txt'
                path.write_text(partial)
                body = audio.render_onset(path)
                expected = 'start | 4.0 (5.0); n=2' if count == 1 else 'start | unavailable; n=0'
                self.assertIn(expected, body)
                self.assertIn('stop | 3.0 (5.0); n=3', body)

    def test_arm_order_without_baseline(self):
        text = (Path(__file__).parent / "fixtures/onset.txt").read_text().replace('baseline', 'short')
        records = [line.replace('short\t', 'long\t', 1) for line in text.splitlines() if line.startswith('short\t')]
        text += '\n' + '\n'.join(records)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'capture.txt'
            path.write_text(text)
            body = audio.render_onset(path)
            self.assertLess(body.index('short median'), body.index('long median'))

    def test_invalid_capture_rejected(self):
        text = (Path(__file__).parent / "fixtures/onset.txt").read_text()
        for old, new in [('test-dev', 'other-dev'), ('event=start', 'event=invalid')]:
            with self.subTest(old=old), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "capture.txt"
                path.write_text(text.replace(old, new, 1))
                with self.assertRaises(ValueError):
                    audio.render_onset(path)

    def test_stale_table_is_rejected_without_write(self):
        capture = Path(__file__).parent / "fixtures/onset.txt"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "doc.md"
            path.write_text('<!-- generated: sound-onset -->\n<!-- /generated: sound-onset -->\n')
            self.assertTrue(audio.update_onset(capture, path))
            self.assertFalse(audio.update_onset(capture, path, check=True))
            path.write_text(path.read_text().replace('3.0', '9.0'))
            before = path.read_bytes()
            self.assertTrue(audio.update_onset(capture, path, check=True))
            self.assertEqual(before, path.read_bytes())


if __name__ == "__main__":
    unittest.main()
