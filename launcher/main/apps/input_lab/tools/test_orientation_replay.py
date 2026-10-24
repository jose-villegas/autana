"""Replay contracts cover synthetic motion, gate traces and generated results."""
import importlib.util
from pathlib import Path
import unittest
import os
import shutil
import sys
import tempfile
import subprocess
from unittest import mock

ROOT = Path(__file__).resolve().parents[5]
SPEC = importlib.util.spec_from_file_location("orientation_replay", Path(__file__).with_name("orientation_replay.py"))
replay = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(replay)


class OrientationReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(ROOT / "launcher/tools/math"))
        from math_figures import build_probe
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise RuntimeError("run replay tests with a host compiler on PATH")
        cls.binary = build_probe(cc, replay.SOURCES, cls.directory.name)

    def test_script_turns_end_at_default_and_keep_it_by_name(self):
        import subprocess
        result = subprocess.run([str(self.binary), "--script"], text=True, capture_output=True, check=True)
        rows = [line.split(" ", 2) for line in result.stdout.splitlines()]
        default_name = next(line.split()[2] for line in replay.DISPLAY.read_text().splitlines()
                            if line.startswith("#define DISPLAY_DEFAULT_QUARTER "))
        default = replay.read_define(replay.DISPLAY, default_name)
        count = replay.read_define(replay.DISPLAY, "DISPLAY_LANDSCAPE_UPSIDE_DOWN") + 1
        names = [replay.read_define(replay.DISPLAY, name) for name in (
            "DISPLAY_PORTRAIT_NAME", "DISPLAY_LANDSCAPE_NAME",
            "DISPLAY_PORTRAIT_UPSIDE_DOWN_NAME", "DISPLAY_LANDSCAPE_UPSIDE_DOWN_NAME")]
        for step in range(count + 1):
            target = (default + step) % count
            self.assertEqual(int(rows[step][1]), target)
            self.assertEqual(rows[step][2], ("HOLD " if step == 0 else "TURN TO ") + names[target] + (" AT 45 DEG" if step == 0 else ", HOLD AT 45 DEG"))
        self.assertEqual(int(rows[count + 1][1]), replay.NO_QUARTER)
        for step in range(count + 2, len(rows) - 1):
            self.assertEqual(int(rows[step][1]), default)
            self.assertTrue(rows[step][2].endswith("KEEP " + names[default]))
        self.assertEqual(int(rows[-1][1]), replay.NO_QUARTER)

    def test_replay_validates_capture_targets_against_shared_script(self):
        import subprocess
        result = subprocess.run([str(self.binary), "--script"], text=True, capture_output=True, check=True)
        segments = [(int(step), index, int(target)) for index, (step, target, _) in enumerate(
            line.split(" ", 2) for line in result.stdout.splitlines())]
        replay.validate_targets(self.binary, segments)
        with self.assertRaises(ValueError):
            replay.validate_targets(self.binary, segments[:-1])
        step, time, target = segments[0]
        with self.assertRaises(ValueError):
            replay.validate_targets(self.binary, [(step, time, target + 1), *segments[1:]])

    def test_real_probe_steady_turn_commits_and_flat_offset_does_not(self):
        scale = replay.read_define(ROOT / "launcher/main/input/input.h", "INPUT_ACCEL_ONE_G")
        for model in ("accel", "fused"):
            for period in (replay.display_define("DISPLAY_SAMPLE_MS"), replay.COARSE_SAMPLE_MS):
                with self.subTest(model=model, period=period):
                    samples = [(time, 0, scale, 0, 0, 0, 0) for time in range(0, 2_000_000, 5000)]
                    events = replay.run(self.binary, model, period, 1, samples)
                    self.assertEqual([quarter for _, quarter in events], [0])
                    flat = [(time, scale // 10, 0, -scale, 0, 0, 0) for time in range(0, 2_000_000, 5000)]
                    self.assertEqual(replay.run(self.binary, model, period, 1, flat), [])

    def test_probe_rejects_bad_model_and_nonmonotonic_timestamps(self):
        import subprocess
        for model, samples in (("bad", []), ("accel", [(1, 0, 4096, 0, 0, 0, 0), (0, 0, 4096, 0, 0, 0, 0)])):
            with self.subTest(model=model), self.assertRaises(subprocess.CalledProcessError):
                replay.run(self.binary, model, 20, 1, samples)

    def test_transcript_retains_sample_timestamps_and_annotations(self):
        segments, samples = replay.parse_capture([
            "LABDUMP round orientation",
            "LABDUMP orientation_step 0 0 0",
            "LABDUMP orientation 0 1234 0 4096 0 0 0 0",
            "LABDUMP orientation_step 5 10000 -1",
            "LABDUMP orientation 5 10001 0 0 -4096 0 0 0",
            "LABDUMP orientation_step 10 20000 -1"])
        self.assertEqual(segments, [(0, 0, 0), (5, 10000, -1), (10, 20000, -1)])
        self.assertEqual(samples[0], (1234, 0, 4096, 0, 0, 0, 0))

    def test_ready_gate_samples_remain_in_the_same_motion_step(self):
        lines = ["LABDUMP orientation_step 0 0 1", "LABDUMP orientation_gate 0 0 begin",
                 "LABDUMP orientation 0 1000 -4096 0 0 0 0 0",
                 "LABDUMP orientation_gate 0 2000 end",
                 "LABDUMP orientation 0 3000 0 4096 0 0 0 0",
                 "LABDUMP orientation_step 10 4000 -1"]
        segments, samples = replay.parse_capture(lines)
        self.assertEqual(segments[-1], (10, 4000, -1))
        self.assertEqual(len(samples), 2)
        with self.assertRaises(ValueError):
            replay.parse_capture(lines[:2] + lines[4:])

    def test_truncation_invalid_order_and_missing_segments_are_errors(self):
        for lines in (["LABDUMP_TRUNCATED"],
                      ["LABDUMP orientation 0 1 0 4096 0 0 0 0"],
                      ["LABDUMP orientation_step 0 0 0", "LABDUMP orientation 0 2 0 4096 0 0 0 0",
                       "LABDUMP orientation 0 1 0 4096 0 0 0 0"]):
            with self.subTest(lines=lines), self.assertRaises(ValueError):
                replay.parse_capture(lines)

    def test_motion_scoring_accepts_on_time_and_early_turns(self):
        scale = replay.read_define(ROOT / "launcher/main/input/input.h", "INPUT_ACCEL_ONE_G")
        neutral = (-scale, 0, 0)
        turned = (0, scale, 0)
        for crossing in (1_100_000, 900_000):
            samples = [(time, *(turned if time >= crossing else neutral), 0, 0, 0)
                       for time in range(0, 2_000_000, 20_000)]
            result = replay.measure(self.binary, samples, [(crossing + 300_000, 0)], initial=1)
            self.assertEqual(result.wanted, 1)
            self.assertEqual(result.unwanted, 0)
            self.assertEqual(result.latencies_us, [300_000])

    def test_motion_scoring_rejects_false_switch(self):
        scale = replay.read_define(ROOT / "launcher/main/input/input.h", "INPUT_ACCEL_ONE_G")
        samples = [(time, -scale, 0, 0, 0, 0, 0) for time in range(0, 1_000_000, 20_000)]
        result = replay.measure(self.binary, samples, [(400_000, 0)], initial=1)
        self.assertEqual(result.unwanted, 1)

    def test_transient_angle_crossing_does_not_justify_late_commit(self):
        scale = replay.read_define(ROOT / "launcher/main/input/input.h", "INPUT_ACCEL_ONE_G")
        samples = [(time, *( (0, scale, 0) if 100_000 <= time < 200_000 else (-scale, 0, 0)), 0, 0, 0)
                   for time in range(0, 1_000_000, 20_000)]
        result = replay.measure(self.binary, samples, [(600_000, 0)], initial=1)
        self.assertEqual(result.unwanted, 1)

    def test_scoring_settle_quiet_and_freshness_boundaries(self):
        settle = replay.display_define("DISPLAY_SETTLE_MS") * replay.US_PER_MS
        period = replay.display_define("DISPLAY_SAMPLE_MS") * replay.US_PER_MS
        samples = [(0, 0, 1, 0), (settle, 0, 1, 0)]
        self.assertEqual(1, replay.measure(self.binary, samples, [(settle, 0)], 1).wanted)
        self.assertEqual(1, replay.measure(self.binary, samples, [(settle - 1, 0)], 1).unwanted)
        self.assertEqual(1, replay.measure(self.binary, samples, [(settle, 0)], 1,
                                         [(5, settle, replay.NO_QUARTER)]).unwanted)
        self.assertEqual(1, replay.measure(self.binary, samples, [(settle + 1, 0)], 1).unwanted)
        for age, wanted in ((2 * period, 1), (2 * period + 1, 0)):
            stale = [*samples, (settle + age, -1, 0, 0)]
            self.assertEqual(wanted, replay.measure(self.binary, stale, [(settle + age, 0)], 1).wanted)
        self.assertEqual(1, replay.measure(self.binary, samples, [(settle, 2)], 1).unwanted)

    def test_motion_quarter_agrees_with_c_for_axes_gap_and_tilt(self):
        import math
        vectors = []
        for angle in (0, 45, 67, 68, 90, 180, 270):
            for tilt in (-41, -26, -24, 0, 59, 61, 64, 66, 69, 71, 81):
                a, t = math.radians(angle), math.radians(tilt)
                vectors.append((0, math.sin(a) * math.cos(t), math.cos(a) * math.cos(t), -math.sin(t)))
        replay.classify(self.binary, vectors)
        for old in range(4):
            for sample in vectors:
                expected = replay.candidate(self.binary, *sample[1:], old, True)
                raw = replay.candidate(self.binary, *sample[1:], old, False)
                quarter, valid = replay.motion_quarter(self.binary, sample, old)
                self.assertEqual(None if raw in (old, replay.NO_QUARTER) else raw, quarter)
                self.assertEqual(quarter is not None and expected == quarter, valid)
        self.assertEqual((None, False), replay.motion_quarter(self.binary, (0, 0, 0, 0), 1))

    def test_gate_trace_commits_match_plain_and_retain_state(self):
        scale = replay.read_define(ROOT / "launcher/main/input/input.h", "INPUT_ACCEL_ONE_G")
        samples = [(time, 0, scale, 0, 0, 0, 0) for time in range(0, 200_001, 5000)]
        for model in ("accel", "fused"):
            rows = replay.gate_trace(self.binary, model, 20, 1, samples)
            commits = [row for kind, row in rows if kind == "C"]
            self.assertEqual(replay.run(self.binary, model, 20, 1, samples), commits)
            gates = [row for kind, row in rows if kind == "G"]
            self.assertEqual(list(range(0, 200_001, 20_000)), [row[0] for row in gates])
            self.assertTrue(all(len(row) == 13 for row in gates))
            self.assertEqual(0, gates[0][2])
            self.assertEqual(1, gates[0][1])
            self.assertEqual(0, gates[-1][1])
            self.assertGreater(gates[0][9], 0)

    def test_gate_report_distinguishes_last_longest_and_tilt(self):
        none = -(1 << 63)
        settle = replay.display_define("DISPLAY_SETTLE_MS") * replay.US_PER_MS
        def row(time, prediction, predicted_us, vector=(0, 1, 0)):
            return ("G", (time, 1, prediction, predicted_us, none, none, none, none, *vector, 0, 0))
        rows = [row(0, -1, 0, (-1, 0, 0)), row(settle, -1, 0, (-1, 0, 0)),
                row(2 * settle, 0, 2 * settle), row(3 * settle, 0, 2 * settle)]
        result = list(replay.gate_report(self.binary, rows, [(0, 3 * settle, 0)]))[0]
        self.assertEqual(("settle", settle / 1000, "filtered hysteresis / angle", 2 * settle / 1000), result[3:])
        tilted = [row(0, -1, 0, (0, 1, -10)), row(settle, 0, 0)]
        self.assertEqual("filtered tilt", list(replay.gate_report(self.binary, tilted, [(0, settle, 0)]))[0][3])
        self.assertEqual("none", list(replay.gate_report(self.binary, [], [(0, settle, 0)]))[0][3])

    def test_gate_report_event_cooldowns(self):
        none = -(1 << 63)
        for index, (gate, constant) in enumerate((("after-flat", "DISPLAY_AFTER_FLAT_MS"),
                                                  ("after-accel", "DISPLAY_AFTER_ACCEL_MS"),
                                                  ("after-swing", "DISPLAY_AFTER_SWING_MS"),
                                                  ("touch", "DISPLAY_AFTER_TOUCH_MS"))):
            duration = replay.display_define(constant) * replay.US_PER_MS
            events = [none] * 4
            events[index] = 0
            rows = [("G", (time, 1, 0, -duration, *events, 0, 1, 0, 0, 0))
                    for time in (0, duration - 1, duration)]
            result = list(replay.gate_report(self.binary, rows, [(0, duration, 0)]))[0]
            self.assertEqual((gate, duration / replay.US_PER_MS, gate, duration / replay.US_PER_MS), result[3:])

    def test_results_check_stale_without_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            doc = Path(directory) / "README.md"
            doc.write_text("<!-- generated: orientation-results -->\n<!-- /generated: orientation-results -->\n")
            before = doc.read_bytes()
            with mock.patch.object(replay, "RESULT_DOC", doc), mock.patch.object(sys, "argv", [
                    "orientation_replay", "--results", "--check", "--cc", os.environ.get("CC", "cc")]):
                with self.assertRaises(SystemExit) as error:
                    replay.main()
            self.assertEqual(1, error.exception.code)
            self.assertEqual(before, doc.read_bytes())

    def test_cli_gates_and_generated_check_are_exercised(self):
        command = [sys.executable, str(Path(replay.__file__)), "--results", "--check", "--cc", os.environ.get("CC", "cc")]
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertEqual(0, result.returncode, result.stderr)
        result = subprocess.run([sys.executable, str(Path(replay.__file__)), str(replay.RESULT_CAPTURE),
                                 "--initial-quarter", str(replay.default_quarter()), "--model", "accel", "--gates",
                                 "--cc", os.environ.get("CC", "cc")], text=True, capture_output=True, check=True)
        self.assertIn("last gate", result.stdout)
        self.assertIn("longest hold", result.stdout)


if __name__ == "__main__":
    unittest.main()
