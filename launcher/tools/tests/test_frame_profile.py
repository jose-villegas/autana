"""Round-trip playback log tokens and shared budget tags through capture parsers."""
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / "launcher/tools"), str(ROOT / "launcher/tools/perf"),
                str(ROOT / "scripts/device")]
from layout_measure import PROFILE_RE, parse_timings
from build.c_header import read_define
from device_report import DEVICE_TIMING_RE, PERF_TARGET_RE


class FrameProfileTests(unittest.TestCase):
    def test_log_round_trip(self):
        header = ROOT / "launcher/test/frame_profile.h"
        token = lambda name: read_define(header, "FRAME_PROFILE_" + name)
        sample = read_define(ROOT / "launcher/packages/math/include/math/scalar/units.h", "MATH_US_PER_MS")
        log = (f"{token('PREFIX')} probe {token('FRAMES')}3 "
               + " ".join(f"{token(name)} {sample}us" for name in ("MEAN", "MED", "P95", "WORST", "BEST"))
               + f" | {token('FPS')} {token('MEAN')} 1000.00 {token('LOW')} 1000.00 "
               + f"{token('WORST')} 1000.00 {token('QUARTER')}portrait")
        self.assertEqual(PROFILE_RE.search(log).groups(), ("probe", str(sample)))
        self.assertEqual(parse_timings(log), {"probe quarter=portrait": sample})
        failure = f"{token('PREFIX')} probe failed"
        self.assertIsNone(PROFILE_RE.search(failure))
        self.assertEqual(parse_timings(failure), {})

    def test_shared_budget_tag(self):
        tag = read_define(ROOT / "launcher/test/perf_budget.h", "PERF_BUDGET_TAG")
        log = f"I (1) {tag}: PERF TARGET skin 2 core(s): measured 1000 us, goal 1000 us, distance +0%"
        self.assertEqual(DEVICE_TIMING_RE.search(log)['us'], "1000")
        self.assertEqual(PERF_TARGET_RE.search(log)['name'], "skin 2 core(s)")
        self.assertIsNone(DEVICE_TIMING_RE.search(f"{tag}: probe: ceiling not yet pegged (measured_us=1000)"))


if __name__ == "__main__":
    unittest.main()
