"""Internal/acoustic comparison requires paired measurements from the same capture."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "audio_doc_tables.py"
SPEC = importlib.util.spec_from_file_location("audio_doc_tables", SCRIPT)
tables = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tables)


class AudioDocTablesTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.capture = Path(temporary.name) / "capture.txt"
        self.document = Path(temporary.name) / "Audio.md"
        self.records = [("buildid", {"build_id": "fixture-dev"}),
                        ("heap", {"free": 10000, "largest": 8000})]
        for command, hz, path in (("tone 300 500", 300, "acoustic"),
                                   ("tone 1000 500", 1000, "acoustic"),
                                   ("tone 1000 500 30", 1000, "acoustic"),
                                   ("tone 300 500 --internal", 300, "internal"),
                                   ("tone 1000 500 --json --internal", 1000, "internal")):
            internal = path == "internal"
            self.records.append((command, dict(hz=hz, tone_db=-12.3 if internal else -21.5,
                silence_db=-110.0, heard=True, open_dma_bytes=3040, path=path,
                measured_hz=hz + (0.02 if internal else -0.03), hz_error=0.02 if internal else -0.03)))
        self.records.append(("heap", {"free": 10000, "largest": 8000}))
        self.save()

    def save(self):
        self.capture.write_text("# autana --json replies on TEST, 2026-10-19, fixture\n" +
            "\n".join(command + "\t" + json.dumps(reply) for command, reply in self.records))

    def test_comparison_and_regeneration(self):
        rendered = tables.render(self.capture)
        comparison = rendered["audio-loopback"]
        self.assertIn("Internal vs acoustic loopback", comparison)
        self.assertIn("| 300 | -21.5 | -12.3 | -0.03 | +0.02 |", comparison)
        self.assertIn("| 1000 | -21.5 | -12.3 | -0.03 | +0.02 |", comparison)
        self.document.write_bytes("\r\n".join(f"<!-- generated: {name} -->\r\n<!-- /generated: {name} -->"
            for name in rendered).encode())
        self.assertTrue(tables.update(self.capture, self.document))
        self.assertFalse(tables.update(self.capture, self.document, check=True))
        self.assertNotIn(b"\n", self.document.read_bytes().replace(b"\r\n", b""))

    def test_missing_internal_is_rejected_when_block_exists(self):
        self.records = [(command, reply) for command, reply in self.records if "--internal" not in command]
        self.save()
        self.document.write_text("<!-- generated: audio-loopback -->\n<!-- /generated: audio-loopback -->\n")
        with self.assertRaisesRegex(ValueError, "internal"):
            tables.update(self.capture, self.document)

    def test_unpaired_or_missing_frequency_is_rejected(self):
        self.records = [(command, reply) for command, reply in self.records if command != "tone 300 500 --internal"]
        self.save()
        with self.assertRaisesRegex(ValueError, "paired"):
            tables.render(self.capture)

    def test_explicit_default_volume_matches_implicit(self):
        self.records = [(command.replace("tone 300 500 --internal", "tone 300 500 30 --internal"), reply)
                        for command, reply in self.records]
        self.save()
        self.assertIn("| 300 |", tables.render(self.capture)["audio-loopback"])

    def test_null_frequency_is_rendered_as_unavailable(self):
        for command, reply in self.records:
            if command.startswith("tone"):
                reply["measured_hz"] = None
                reply["hz_error"] = None
        self.save()
        self.assertIn("| 300 | -21.5 | -12.3 | n/a | n/a |", tables.render(self.capture)["audio-loopback"])

    def test_legacy_capture_without_comparison_stays_current(self):
        rendered = tables.render(tables.ROOT / tables.CAPTURE)
        self.assertNotIn("audio-loopback", rendered)


if __name__ == "__main__":
    unittest.main()
