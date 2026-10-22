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
                self.assertIn('stop | 3.0 (5.0) |', body)

    def test_arm_order_without_baseline(self):
        text = (Path(__file__).parent / "fixtures/onset.txt").read_text().replace('baseline', 'short')
        records = [line.replace('short\t', 'long\t', 1) for line in text.splitlines() if line.startswith('short\t')]
        text += '\n' + '\n'.join(records)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'capture.txt'
            path.write_text(text)
            body = audio.render_onset(path)
            self.assertLess(body.index('| short |'), body.index('| long |'))

    def test_invalid_capture_rejected(self):
        text = (Path(__file__).parent / "fixtures/onset.txt").read_text()
        for old, new in [('test-dev', 'other-dev'), ('event=start', 'event=invalid')]:
            with self.subTest(old=old), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "capture.txt"
                path.write_text(text.replace(old, new, 1))
                with self.assertRaises(ValueError):
                    audio.render_onset(path)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.document = self.directory / "Sound-Mixer.md"
        self.document.write_text("<!-- generated: sound-onset -->\n<!-- /generated: sound-onset -->\n")
        self.captures = []
        for board, build in (("board-a", "build-a"), ("board-b", "build-b")):
            records = [f"# autana sound-onset capture on {board}, 2026-10-20, fixture",
                       'meta\tstatus\t{}', f'meta\tbuildid\t{{"build_id": "{build}"}}',
                       'meta\truns\t2']
            for arm, length in (("off", 0), ("500us", 500)):
                records.append(f'{arm}\tsettings\t{{"sound.declick_us": {length}}}')
                for event in audio.ONSET_EVENTS:
                    for excess in (1, 3):
                        records.append(f"{arm}\tsound onset {event}\tSOUND onset event={event} "
                                       f"hz=500 gain=33 pre_db=-50.0 peak_db={-50 + excess:.1f} "
                                       f"excess_db={excess:.1f} at_ms=20")
            records.extend([f'meta\tbuildid\t{{"build_id": "{build}"}}', 'meta\tstatus\t{}'])
            path = self.directory / f"sound_onset_capture_{board}.txt"
            path.write_text("\n".join(records) + "\n")
            self.captures.append(path)

    def test_generic_captures_and_stale_check(self):
        before = self.document.read_bytes()
        self.assertTrue(audio.update_onset(self.directory, self.document, check=True))
        self.assertEqual(before, self.document.read_bytes())
        audio.update_onset(self.directory, self.document)
        text = self.document.read_text()
        for board in ("board-a", "board-b"):
            self.assertNotIn(f"Board **{board}**", text)
        self.assertNotIn("build-b", text)
        self.assertNotIn("2026-10-20", text)
        self.assertIn("Capture 1", text)
        self.assertIn("Capture 2", text)
        self.assertEqual(text.count("<picture>"), 1)
        self.assertEqual(text.count("<details>"), 2)
        self.assertIn("[raw replies]", text)
        self.assertEqual(text.count("| Event | off | 500us |"), 2)
        self.assertIn("| stop | 2.0 (3.0) | 2.0 (3.0) |", text)
        self.assertNotIn("n=2", text)
        self.assertFalse(audio.update_onset(self.directory, self.document, check=True))
        self.document.write_text(text.replace("2.0 (3.0)", "stale", 1))
        before = self.document.read_bytes()
        self.assertTrue(audio.update_onset(self.directory, self.document, check=True))
        self.assertEqual(before, self.document.read_bytes())

    def test_partial_runs_are_counted(self):
        path = self.captures[1]
        lines = path.read_text().splitlines()
        index = next(i for i, line in enumerate(lines) if line.startswith("off\tsound onset stop"))
        lines[index] = 'off\tfailure sound onset stop\t{}'
        path.write_text("\n".join(lines) + "\n")
        audio.update_onset(self.directory, self.document)
        self.assertIn("| stop | 3.0 (3.0); n=1 | 2.0 (3.0) |", self.document.read_text())

    def test_changed_build_is_rejected(self):
        path = self.captures[1]
        text = path.read_text()
        index = text.rindex('"build-b"')
        path.write_text(text[:index] + text[index:].replace('"build-b"', '"other"', 1))
        with self.assertRaisesRegex(ValueError, "matching build ids"):
            audio.update_onset(self.directory, self.document)

    def test_different_arms_are_rejected(self):
        path = self.captures[1]
        original = path.read_text()
        for changed in (original.replace("500us", "1ms"),
                        original.replace('"sound.declick_us": 500', '"sound.declick_us": 1000')):
            with self.subTest(changed=changed):
                path.write_text(changed)
                with self.assertRaisesRegex(ValueError, "arms"):
                    audio.update_onset(self.directory, self.document)

    def test_no_captures_is_rejected(self):
        for path in self.captures:
            path.unlink()
        with self.assertRaisesRegex(ValueError, "no sound-onset"):
            audio.update_onset(self.directory, self.document)


if __name__ == "__main__":
    unittest.main()
