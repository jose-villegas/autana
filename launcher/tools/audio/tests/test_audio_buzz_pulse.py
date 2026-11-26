"""Pulse point expansion and speaker safety for the buzz sweep."""
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location("buzz", Path(__file__).resolve().parents[1] / "audio_buzz_sweep.py")
buzz = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(buzz)
from autana_runner import AutanaRunError


class PulseSweepTests(unittest.TestCase):
    def test_short_pulse_window_uses_board_rejection(self):
        hz, window, volume, pulse = "202", "30", "30", "30"
        tone = ("tone", hz, window, volume, "--pulse", pulse, "--json")
        error = AutanaRunError(1, ("autana", *tone), stderr="TONE usage")
        with tempfile.TemporaryDirectory() as directory:
            args = buzz.parser().parse_args(["--board", "test", "--surface", "desk", "--hz", hz, "--ms", window,
                                            "--volumes", volume, "--pulse", pulse,
                                            "--out", str(Path(directory) / "capture.txt")])
            with mock.patch.object(buzz, "autana_run", side_effect=("{}", "", error)) as run:
                with self.assertRaisesRegex(AutanaRunError, "TONE usage"):
                    buzz.capture(args)
                run.assert_called_with(args.autana, *tone)

    def test_pulse_points(self):
        args = buzz.parser().parse_args(["--board", "test", "--surface", "desk", "--hz", "202", "--volumes", "30,60",
                                        "--pulse", "30,60,100,200", "--runs", "1", "--loud", "0"])
        self.assertEqual(list(buzz.points(args, args.hz)),
                         [(202, volume, pulse) for volume in (30, 60) for pulse in (30, 60, 100, 200)])
        with mock.patch.object(buzz, "autana_run", return_value='{"shake_rms_mg": 1, "gx_mg": -12.5, "gy_mg": 25, "gz_mg": 999}'), mock.patch.object(buzz.time, "sleep"):
            output = io.StringIO()
            buzz.sweep(args, output, args.hz, {})
        self.assertIn("tone 202 400 30 --pulse 30", output.getvalue())
        self.assertIn('"gx_mg": -12.5, "gy_mg": 25, "gz_mg": 999', output.getvalue())

    def test_gap_fields_and_failure_capture(self):
        args = buzz.parser().parse_args(["--board", "test", "--surface", "desk", "--hz", "202", "--volumes", "30",
                                        "--pulse", "30", "--runs", "1", "--loud", "0"])
        for shake in (1, None):
            reply = {"shake_rms_mg": shake, "imu_gaps": 2,
                     "shake_reason": None if shake else "gap inside pulse"}
            output = io.StringIO()
            with mock.patch.object(buzz, "autana_run", return_value=buzz.json.dumps(reply)), mock.patch.object(buzz.time, "sleep"):
                if shake is None:
                    with self.assertRaisesRegex(RuntimeError, "gap inside pulse"):
                        buzz.sweep(args, output, args.hz, {})
                else:
                    buzz.sweep(args, output, args.hz, {})
            self.assertIn('"imu_gaps": 2', output.getvalue())
            if shake is None:
                self.assertEqual(output.getvalue().count('"shake_reason": "gap inside pulse"'), 2)

    def test_pulse_safety(self):
        for pulse in ("0", "501", "401"):
            args = buzz.parser().parse_args(["--board", "test", "--surface", "desk", "--pulse", pulse])
            with mock.patch.object(buzz, "autana_run") as run, self.assertRaises(ValueError):
                buzz.capture(args)
            run.assert_not_called()
        args = buzz.parser().parse_args(["--board", "test", "--surface", "desk", "--hz", "20", "--volumes", "100", "--pulse", "30"])
        with self.assertRaises(ValueError):
            buzz.sweep(args, io.StringIO(), args.hz, {})
