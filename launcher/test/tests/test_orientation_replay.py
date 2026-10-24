"""Replay contracts use generated samples; no board capture is retained."""
import importlib.util
from pathlib import Path
import unittest
import os
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("orientation_replay", ROOT / "launcher/tools/input/orientation_replay.py")
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
            for period in (replay.read_define(replay.DISPLAY, "DISPLAY_SAMPLE_MS"), replay.COARSE_SAMPLE_MS):
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
            result = replay.measure(samples, [(crossing + 300_000, 0)], initial=1)
            self.assertEqual(result.wanted, 1)
            self.assertEqual(result.unwanted, 0)
            self.assertEqual(result.latencies_us, [300_000])

    def test_motion_scoring_rejects_false_switch(self):
        scale = replay.read_define(ROOT / "launcher/main/input/input.h", "INPUT_ACCEL_ONE_G")
        samples = [(time, -scale, 0, 0, 0, 0, 0) for time in range(0, 1_000_000, 20_000)]
        result = replay.measure(samples, [(400_000, 0)], initial=1)
        self.assertEqual(result.unwanted, 1)

    def test_transient_angle_crossing_does_not_justify_late_commit(self):
        scale = replay.read_define(ROOT / "launcher/main/input/input.h", "INPUT_ACCEL_ONE_G")
        samples = [(time, *( (0, scale, 0) if 100_000 <= time < 200_000 else (-scale, 0, 0)), 0, 0, 0)
                   for time in range(0, 1_000_000, 20_000)]
        result = replay.measure(samples, [(600_000, 0)], initial=1)
        self.assertEqual(result.unwanted, 1)


if __name__ == "__main__":
    unittest.main()
