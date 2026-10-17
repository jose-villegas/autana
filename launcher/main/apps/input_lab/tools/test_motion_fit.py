import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import motion_fit


class MotionFitTest(unittest.TestCase):
    def fixture(self):
        return motion_fit.parse((Path(__file__).parent / "fixtures" / "motion.log").read_text().splitlines())

    def test_rounds_pose_means_and_device_fit(self):
        rounds, motion = self.fixture()
        self.assertEqual(2, len(rounds))
        self.assertEqual(-4096, rounds[0]["poses"]["face_up"][2])
        self.assertEqual((1, 1000000, -2, 1001000, 3, 999000, 500, -1000, 0), rounds[0]["fit"])
        self.assertEqual(3, len(motion))
        self.assertEqual((32, -64, 0), motion[0]["rotation"])

    def test_replayed_dump_has_the_live_fit_and_poses(self):
        lines = (Path(__file__).parent / "fixtures" / "motion.log").read_text().splitlines()
        round_lines = [line for line in lines if any(word in line for word in
                       ("round imu", "pose ", "calib x off"))]
        live = ["LABDUMP " + line for line in round_lines]
        dump = ["LABDUMP_BEGIN", *live,
                "LABDUMP GRAV face_up down 0.000000 0.000000 -1.000000 bias_dps 0.000000 0.000000 0.000000",
                "LABDUMP turn ms 0 down 0.000000 1.000000 0.000000",
                "LABDUMP_END"]
        self.assertEqual(motion_fit.parse(round_lines)[0], motion_fit.parse(dump)[0])
        self.assertEqual(motion_fit.parse(live), motion_fit.parse(dump))

    def test_rest_excludes_rounds_motion_and_tilt_departure(self):
        def sample(ms, tilt=6, turn=10, before_round=True):
            return {"ms": ms, "tilt": tilt, "turn": turn, "before_round": before_round,
                    "rotation": (64, 128, -64), "raw": (400, 100, -4096),
                    "bearing": 16, "quarter": 3}
        samples = [sample(0), sample(100), sample(200, tilt=9),
                   sample(300), sample(400), sample(500), sample(600, turn=30),
                   sample(700, before_round=False), sample(800, before_round=False)]
        rest = motion_fit.rest_stretch(samples)
        self.assertEqual([300, 400, 500], [s["ms"] for s in rest])
        first, last = motion_fit.endpoints(rest, 0.1)
        self.assertEqual([300, 400], [s["ms"] for s in first])
        self.assertEqual([400, 500], [s["ms"] for s in last])
        self.assertEqual((1000, 2000, -1000), motion_fit.gyro_mean(first))
        self.assertIsNone(motion_fit.gyro_mean([]))
        self.assertEqual([], motion_fit.rest_stretch(samples[-2:]))

    def test_still_limits_and_gap_boundary(self):
        _, samples = self.fixture()
        base = dict(samples[0], before_round=True, turn=motion_fit.STILL_TURN_LIMIT_DPS)
        band = motion_fit.STILL_TILT_BAND_DEG
        gap = motion_fit.MAX_LOG_GAP_MS
        samples = [dict(base, ms=0, tilt=0), dict(base, ms=gap, tilt=band),
                   dict(base, ms=2 * gap + 1, tilt=band)]
        self.assertEqual(samples[:2], motion_fit.rest_stretch(samples))
        self.assertEqual([], motion_fit.rest_stretch([dict(base, turn=base["turn"] + 1)]))
        self.assertEqual([samples[1]], motion_fit.flat_samples([
            dict(base, tilt=motion_fit.FLAT_TILT_LIMIT_DEG), samples[1]]))

    def test_rest_prefers_timestamp_span_over_sample_count(self):
        _, samples = self.fixture()
        base = dict(samples[0], before_round=True)
        dense = [dict(base, ms=ms) for ms in range(0, 101, 10)]
        sparse = [dict(base, ms=ms) for ms in range(1000, 2001, 200)]
        for runs in (dense + sparse, sparse + [dict(s, ms=s["ms"] + 3000) for s in dense]):
            with self.subTest(timestamps=[s["ms"] for s in runs]):
                self.assertEqual(sparse, motion_fit.rest_stretch(runs))

    def test_parse_marks_samples_after_first_round(self):
        line = "I (100) motion raw 1,2,-4096 tilt 6 bearing 16 turn 10 rot 1,2,3 quarter 3"
        _, samples = motion_fit.parse([line, "round imu quarter 1", line])
        self.assertEqual([True, False], [s["before_round"] for s in samples])
        self.assertEqual((16, 10), (samples[0]["bearing"], samples[0]["turn"]))

    def test_flat_quarters_and_transitions_exclude_handling_and_gaps(self):
        _, samples = self.fixture()
        base = samples[0]
        samples = [dict(base, ms=i * 100, quarter=q, bearing=16)
                   for i, q in enumerate((1, 3, 3, 2, 3, 3))]
        samples[3]["turn"] = motion_fit.STILL_TURN_LIMIT_DPS + 1
        samples[4]["raw"] = (0, 0, 4096)
        samples.append(dict(base, ms=5000, quarter=0, tilt=motion_fit.FLAT_TILT_LIMIT_DEG))
        flat = motion_fit.flat_samples(samples)
        self.assertEqual([1, 3, 3, 3], [s["quarter"] for s in flat])
        [(old, sample)] = motion_fit.transitions(samples)
        self.assertEqual((1, 3, 100), (old, sample["quarter"], sample["ms"]))
        text = motion_fit.report([("flat", [], samples)], 60)
        self.assertIn("0.0,25.0,0.0,75.0", text)
        self.assertIn("16", text)

    def test_report_delta_and_restarted_incomplete_round(self):
        rounds, motion = self.fixture()
        text = motion_fit.report([("fixture", rounds, motion)], 10)
        self.assertIn("1,-2,3 / 10,20,30 / 100,200,300", text)
        rounds, motion = motion_fit.parse(["round imu quarter 1", "round imu quarter 1"])
        self.assertEqual(2, len(rounds))
        self.assertIn("incomplete", motion_fit.report([("fixture", rounds, motion)], 10))
        self.assertEqual(([], []), motion_fit.parse(["unrelated", "motion raw 0,0,0"]))

    def test_c_log_formats_parse(self):
        source = (Path(__file__).parents[1] / "app_input_lab.c").read_text()
        source = re.sub(r'"\s*"', "", source)
        for prefix, regex, values, expected in (
            ("motion", motion_fit.MOTION, tuple(range(1, 21)), ("1", "2", "3", "4", "5", "10", "11", "12", "13", "14")),
            ("pose", motion_fit.POSE, ("face_up", 1, -2, 3, -4, 5, -6, 7), ("face_up", "1 -2 3 -4 5 -6 7")),
            ("calib", motion_fit.CALIB, tuple(range(1, 10)), tuple(map(str, range(1, 10)))),
            ("GRAV", re.compile(r"GRAV (\w+) down (-?\d+\.\d{6}) (-?\d+\.\d{6}) (-?\d+\.\d{6}) bias_dps (-?\d+\.\d{6}) (-?\d+\.\d{6}) (-?\d+\.\d{6})"),
             ("face_up", 6, 0., 6, 0., 6, -1., 6, 0., 6, 0., 6, 0.),
             ("face_up", "0.000000", "0.000000", "-1.000000", "0.000000", "0.000000", "0.000000")),
            ("turn", re.compile(r"turn ms (\d+) down (-?\d+\.\d{6}) (-?\d+\.\d{6}) (-?\d+\.\d{6})"),
             (0, 6, 0., 6, 1., 6, 0.), ("0", "0.000000", "1.000000", "0.000000")),
        ):
            with self.subTest(prefix=prefix):
                formats = re.findall(r'"(' + prefix + r' [^"\n]*%[^"\n]+)"', source)
                if prefix == "GRAV":
                    formats = [fmt for fmt in formats if " bias_dps " in fmt]
                self.assertEqual(1, len(formats))
                line = formats[0] % values
                match = regex.fullmatch(line)
                self.assertIsNotNone(match, line)
                self.assertEqual(expected, match.groups())

    def test_cli_rejects_uptime_reset(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "reset.log"
            capture.write_text("I (2000) motion raw 0,0,0 tilt 0 bearing 0 turn 1 rot 1,2,3 quarter 1\n"
                               "I (1000) motion raw 0,0,0 tilt 0 bearing 0 turn 1 rot 1,2,3 quarter 1\n")
            result = subprocess.run([sys.executable, motion_fit.__file__, str(capture)], capture_output=True, text=True)
            self.assertEqual(2, result.returncode)
            self.assertIn("uptime resets", result.stderr)
            self.assertEqual("", result.stdout)

    def test_cli_rejects_nonpositive_window(self):
        capture = Path(__file__).parent / "fixtures" / "motion.log"
        for seconds in ("0", "-1"):
            with self.subTest(seconds=seconds):
                result = subprocess.run([sys.executable, motion_fit.__file__, str(capture),
                                         "--window-seconds", seconds], capture_output=True, text=True)
                self.assertEqual(2, result.returncode)
                self.assertIn("--window-seconds must be positive", result.stderr)
                self.assertEqual("", result.stdout)


if __name__ == "__main__":
    unittest.main()
