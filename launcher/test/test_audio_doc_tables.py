"""Capture-derived audio tables and their regeneration contract."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("audio_doc_tables", ROOT / "launcher/tools/audio/audio_doc_tables.py")
audio = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audio)


class AudioTablesTest(unittest.TestCase):
    def test_capture_tables(self):
        tables = audio.render(ROOT / audio.CAPTURE)
        self.assertEqual(set(tables), {"audio-ram", "audio-frequency", "audio-volume", "audio-duration"})
        self.assertIn("56", tables["audio-ram"])
        self.assertIn("3096", tables["audio-ram"])
        self.assertIn("3040", tables["audio-ram"])
        self.assertIn("63488", tables["audio-ram"])
        self.assertIn("500 | -14.9", tables["audio-duration"])
        self.assertIn("100 | -14.1", tables["audio-volume"])
        for body in tables.values():
            self.assertIn("A308", body)
            self.assertIn("2521fd82ef55-dev", body)

    def test_changed_capture_changes_tables(self):
        capture = (ROOT / audio.CAPTURE).read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.txt"
            path.write_text(capture.replace('"tone_db": -31.2', '"tone_db": -32.7'))
            self.assertIn("-32.7", audio.render(path)["audio-frequency"])

    def test_build_mismatch_is_rejected(self):
        capture = (ROOT / audio.CAPTURE).read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.txt"
            path.write_text(capture.replace('2521fd82ef55-dev', 'other-dev', 1))
            with self.assertRaisesRegex(ValueError, "build"):
                audio.render(path)

    def test_check_detects_stale_doc_without_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Audio.md"
            path.write_bytes((ROOT / audio.DOCUMENT).read_bytes())
            self.assertFalse(audio.update(ROOT / audio.CAPTURE, path, check=True))
            original = path.read_bytes().replace(b"-31.2", b"-32.7")
            path.write_bytes(original)
            self.assertTrue(audio.update(ROOT / audio.CAPTURE, path, check=True))
            self.assertEqual(path.read_bytes(), original)
            audio.update(ROOT / audio.CAPTURE, path, check=False)
            self.assertFalse(audio.update(ROOT / audio.CAPTURE, path, check=True))


if __name__ == "__main__":
    unittest.main()
