"""autana melody: timing from the board's `melody ?` reply, play and check output, and refusals."""
import contextlib
import io
import json
import math
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "device" / "tests"))
import isolation  # noqa: E402,F401
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import autana  # noqa: E402


INFO = "MELODY_INFO notes=13 check_ms=200 play_max_ms=5000 cents_per_octave=1200"


class MelodyTests(unittest.TestCase):
    def setUp(self):
        self.constants = autana.parse_melody_info(INFO)

    def replies(self, heard=True, measured="440.00 Hz"):
        tone = (f"TONE {'heard' if heard else 'not heard'} 440 Hz: -12.0 dB "
                f"(silence -100.0 dB), path internal, measured {measured}, open 0 B DMA")
        return [f"MELODY note=A4 {tone}"] * self.constants['notes'] + [
            f"MELODY_END notes={self.constants['notes']}"]

    def invoke(self, args, replies, expected=0):
        stream = io.StringIO()
        with mock.patch.object(autana, "send", side_effect=[(0, [INFO]), (0, replies)]) as send, \
                contextlib.redirect_stdout(stream), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(autana.melody(args), expected)
        return stream.getvalue(), send.call_args

    def test_the_board_reports_what_a_run_is_timed_by(self):
        self.assertEqual(self.constants, {"notes": 13, "check_ms": 200, "play_max_ms": 5000, "cents_per_octave": 1200})
        with mock.patch.object(autana, "send", return_value=(0, [INFO])) as send:
            autana.melody_constants()
        self.assertEqual(send.call_args.args, ("MELODY ?",))
        self.assertEqual(send.call_args.kwargs, {"reply": "MELODY_INFO"})

    def test_a_board_without_the_query_stops_before_the_run(self):
        for replies in ([], ["MELODY_INFO notes=13"]):
            with mock.patch.object(autana, "send", return_value=(0, replies)) as send, \
                    self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                autana.melody([])
            self.assertEqual(send.call_count, 1)

    def test_play_and_timeout(self):
        reply = f"MELODY_END notes={self.constants['notes']} ms=4000"
        output, call = self.invoke(["--json"], [reply])
        self.assertEqual(json.loads(output), {"notes": self.constants['notes'], "ms": 4000})
        self.assertEqual(call.args, ("MELODY",))
        self.assertEqual(call.kwargs["until"], ["MELODY_END", "MELODY_ERR"])
        self.assertEqual(call.kwargs["seconds"], self.constants["play_max_ms"] / autana.TONE_MS_PER_SECOND)
        output, _ = self.invoke(["20"], [reply])
        self.assertEqual(output, reply + "\n")

    def test_check_json_and_timeout(self):
        output, call = self.invoke(["30", "--check", "--internal", "--json"], self.replies(measured="443.70 Hz"))
        results = json.loads(output)
        self.assertEqual(len(results), self.constants['notes'])
        for result in results:
            self.assertEqual(result["note"], "A4")
            self.assertTrue(result["heard"])
            self.assertAlmostEqual(result["cents_error"], self.constants["cents_per_octave"] * math.log2(443.7 / 440))
        self.assertEqual(call.args, ("MELODY check 30 internal",))
        self.assertEqual(call.kwargs["seconds"], self.constants['notes'] * (
            autana.TONE_MARGIN_SECONDS + autana.TONE_CAPTURE_COUNT * self.constants["check_ms"] / autana.TONE_MS_PER_SECOND))

    def test_missing_frequency_and_not_heard(self):
        output, _ = self.invoke(["--check", "--json"], self.replies(measured="n/a"))
        self.assertIsNone(json.loads(output)[0]["cents_error"])
        replies = self.replies(False)
        output, _ = self.invoke(["--check"], replies, expected=1)
        self.assertEqual(output, "\n".join(replies) + "\n")

    def test_parse_errors_and_incomplete_streams(self):
        for line in ("MELODY note=H4 TONE heard", "MELODY note=A4 invalid", "OTHER"):
            with self.assertRaises(ValueError):
                autana.parse_melody_note(line, self.constants["cents_per_octave"])
        for replies in ([], ["MELODY_ERR loopback failed"], ["MELODY_END notes=1"],
                        self.replies()[:-1], self.replies()[1:],
                        self.replies()[:-1] + self.replies(),
                        self.replies()[:-1] + [f"MELODY_END notes={self.constants['notes'] - 1}"],
                        [f"MELODY_END notes={self.constants['notes']} ms=bad"]):
            self.invoke(["--check", "--json"], replies, expected=1)

    def test_usage_and_registration(self):
        self.assertIs(autana.COMMANDS["melody"], autana.melody)
        self.assertIn("autana melody", autana.help_text(["tests"]))
        for args in (["--internal"], ["-1"], ["1", "2"], ["--check", "--check"], ["--wat"]):
            with self.assertRaises(SystemExit), mock.patch.object(autana, "send") as send:
                autana.melody(args)
            send.assert_not_called()
