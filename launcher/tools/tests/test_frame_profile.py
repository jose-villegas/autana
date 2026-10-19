"""Compile the real playback and budget owners and round-trip their logs."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / "launcher/tools"), str(ROOT / "launcher/tools/perf")]
from layout_measure import PROFILE_RE, parse_timings
from build.c_header import read_define
sys.path.insert(0, str(ROOT / "scripts/device"))
from device_report import DEVICE_TIMING_RE, PERF_TARGET_RE
SAMPLE_US = read_define(ROOT / "launcher/packages/math/include/math/scalar/units.h", "MATH_US_PER_MS")


@unittest.skipUnless(shutil.which("gcc"), "needs gcc")
class FrameProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binaries = []
        for qemu in (0, 1):
            binary = pathlib.Path(cls.directory.name) / f"probe-{qemu}.exe"
            command = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DHOST_HEAP_ARENA",
                       f"-DCONFIG_LAUNCHER_QEMU={qemu}"]
            for include in ("launcher/test", "launcher/test/stubs", "launcher/test/framework",
                            "launcher/main", "launcher/packages/math/include"):
                command.extend(["-I", str(ROOT / include)])
            command.extend(str(ROOT / source) for source in (
                "launcher/test/fixtures/frame_profile_probe.c", "launcher/test/frame_profile.c",
                "launcher/test/framework/unity.c"))
            subprocess.run([*command, "-o", str(binary)], check=True, capture_output=True, text=True)
            cls.binaries.append(binary)

    def run_case(self, case, qemu=0):
        result = subprocess.run([str(self.binaries[qemu]), case], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stderr

    def test_budget_board_and_qemu(self):
        for qemu in (0, 1):
            with self.subTest(qemu=qemu):
                log = self.run_case("budget", qemu)
                self.assertIn("not yet pegged", log)
                unpegged_guard = next(line for line in log.splitlines() if "ceiling not yet pegged" in line)
                self.assertIsNone(DEVICE_TIMING_RE.search("device_tests: " + unpegged_guard))
                skin = next(PERF_TARGET_RE.finditer(line) for line in log.splitlines()
                            if "skin 2 core(s)" in line)
                self.assertEqual(next(skin)["name"], "skin 2 core(s)")

    def test_floor(self):
        self.run_case("floor")

    def test_boundaries_and_warmup(self):
        for case in ("exact", "short", "warm", "maximum"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_errors_release_samples_and_clear_totals(self):
        for case in ("badw0", "badw-", "badw+", "badm0", "badm-", "badm+",
                     "zero", "negative", "allocation", "overflow", "duration_overflow"):
            with self.subTest(case=case):
                log = self.run_case(case)
                self.assertIn("probe failed", log)
                self.assertIsNone(PROFILE_RE.search(log))
                self.assertEqual(parse_timings(log), {})

    def test_real_log_round_trip(self):
        log = self.run_case("exact")
        self.assertEqual(PROFILE_RE.search(log).groups(), ("probe", str(SAMPLE_US)))
        self.assertEqual(parse_timings(log), {"probe quarter=portrait": SAMPLE_US})


if __name__ == "__main__":
    unittest.main()
