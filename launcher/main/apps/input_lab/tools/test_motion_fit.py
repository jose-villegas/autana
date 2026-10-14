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

    def test_endpoints_use_elapsed_time_and_include_boundary(self):
        _, motion = self.fixture()
        first, last = motion_fit.endpoints(motion, 10)
        self.assertEqual(2, len(first))
        self.assertEqual(2, len(last))
        self.assertEqual((750, -750, 0), motion_fit.gyro_mean(first))
        self.assertEqual((1250, -250, 250), motion_fit.gyro_mean(last))
        self.assertIsNone(motion_fit.gyro_mean([]))

    def test_quarter_transition_retains_flatness_evidence(self):
        _, motion = self.fixture()
        [(old, sample)] = motion_fit.transitions(motion)
        self.assertEqual((1, 2, 0), (old, sample["quarter"], sample["tilt"]))

    def test_report_delta_and_restarted_incomplete_round(self):
        rounds, motion = self.fixture()
        text = motion_fit.report([("fixture", rounds, motion)], 10)
        self.assertIn("1,-2,3 / 10,20,30 / 100,200,300", text)
        rounds, motion = motion_fit.parse(["round imu quarter 1", "round imu quarter 1"])
        self.assertEqual(2, len(rounds))
        self.assertIn("incomplete", motion_fit.report([("fixture", rounds, motion)], 10))
        self.assertEqual(([], []), motion_fit.parse(["unrelated", "motion raw 0,0,0"]))


if __name__ == "__main__":
    unittest.main()
