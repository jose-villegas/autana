"""Pulse table: medians per volume and length, retried replies, the reach rule."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location("audio_buzz_doc", TOOLS / "audio_buzz_doc.py")
doc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(doc)

HEADER = "# autana --json replies on test, 2026-01-01, one command per line: <command>\t<reply>"


def line(command, shake, floor=1.0, reason=None):
    return f"{command}\t" + json.dumps({"shake_rms_mg": shake, "silence_shake_rms_mg": floor, "shake_reason": reason})


def capture(lines):
    directory = tempfile.TemporaryDirectory()
    path = Path(directory.name) / "pulse.txt"
    path.write_text("\n".join([HEADER, 'buildid\t{"build_id": "x-dev"}', *lines]) + "\n", encoding="utf-8")
    return directory, path


class PulseTableTests(unittest.TestCase):
    def test_medians_retries_and_shortest_reaching_pulse(self):
        long, reach = 10.0, doc.REACH_RATIO
        directory, path = capture([
            *(line("tone 202 400 60", shake) for shake in (long - 1, long, long + 1)),
            line("tone 202 400 60 --pulse 30", None, reason="gap inside pulse"),
            *(line("tone 202 400 60 --pulse 30", shake) for shake in (1.0, 2.0, 3.0)),
            *(line("tone 202 400 60 --pulse 60", long * reach) for _ in range(3)),
            *(line("tone 202 400 60 --pulse 100", long) for _ in range(3)),
            *(line("tone 202 400 30", long) for _ in range(3)),
            *(line("tone 202 400 30 --pulse 60", long * reach - 1) for _ in range(3)),
        ])
        with directory:
            points, retried = doc.pulse_data(path)
            body = doc.render_pulse(path)
        self.assertEqual(retried, 1)
        self.assertEqual(points[(60, 0)]["shake"], long)
        self.assertEqual(points[(60, 30)], dict(runs=3, shake=2.0, floor=1.0))
        self.assertIn("| 60 | 60 ms |", body)
        self.assertIn("| 30 | none tested |", body)
        self.assertIn("1 reply with a gap inside the pulse was retried", body)

    def test_rejects_missing_long_tone_and_unexplained_missing_shake(self):
        for lines, message in (
                ([line("tone 202 400 60 --pulse 30", 1.0)], "no long tone at 60%"),
                ([line("tone 202 400 60", 1.0), line("tone 202 400 60 --pulse 30", None)], "no reason")):
            directory, path = capture(lines)
            with directory, self.assertRaisesRegex(ValueError, message):
                doc.pulse_data(path)


if __name__ == "__main__":
    unittest.main()
