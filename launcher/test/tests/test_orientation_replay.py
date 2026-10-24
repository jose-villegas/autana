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
            "LABDUMP orientation_step 4 10000 -1",
            "LABDUMP orientation 4 10001 0 0 -4096 0 0 0",
            "LABDUMP orientation_step 9 20000 -1"])
        self.assertEqual(segments, [(0, 0, 0), (4, 10000, -1), (9, 20000, -1)])
        self.assertEqual(samples[0], (1234, 0, 4096, 0, 0, 0, 0))

    def test_truncation_invalid_order_and_missing_segments_are_errors(self):
        for lines in (["LABDUMP_TRUNCATED"],
                      ["LABDUMP orientation 0 1 0 4096 0 0 0 0"],
                      ["LABDUMP orientation_step 0 0 0", "LABDUMP orientation 0 2 0 4096 0 0 0 0",
                       "LABDUMP orientation 0 1 0 4096 0 0 0 0"]):
            with self.subTest(lines=lines), self.assertRaises(ValueError):
                replay.parse_capture(lines)

    def test_hold_segments_count_every_switch_as_false_and_latency_needs_target(self):
        rows = replay.measure([(0, 0, 0), (1, 1000, 1), (4, 2000, -1), (9, 3000, -1)],
                              [(100, 2), (500, 0), (1500, 1), (2100, 2), (2200, 1)], initial=1)
        self.assertEqual(rows[0], (0, 2, 1, 500))
        self.assertEqual(rows[1], (1, 1, 0, 500))
        self.assertEqual(rows[2], (4, 2, 2, None))

    def test_missing_wanted_turn_is_reported(self):
        rows = replay.measure([(0, 0, 0), (9, 1000, -1)], [], initial=1)
        self.assertEqual(rows[0], (0, 0, 0, None))


if __name__ == "__main__":
    unittest.main()
