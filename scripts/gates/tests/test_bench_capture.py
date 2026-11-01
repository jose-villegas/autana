"""Package benchmark captures enforce row identity and provenance."""
from functools import partial
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
import bench_capture as bench

PLACEHOLDER = "pending"
parse_capture = partial(bench.parse_capture, prefix="MATHBENCH", placeholder=PLACEHOLDER)


class BenchCaptureTests(unittest.TestCase):
    batch = 64
    rows = [("dot_f", "dot (vec3)", "f", batch, "vector"), ("sinf_f", "sinf", "f", batch, "libm")]
    capture = ("BUILD_ID=abcdef-diag\n"
               "MATHBENCH name=dot (vec3) type=f ops=64 warm=12.0 cold=24.0 insn=6.0\n"
               "MATHBENCH name=sinf type=f ops=64 warm=90.0 cold=110.0 insn=30.0\n")

    def test_package_capture_prefix_and_placeholder(self):
        rows = [("crc32_update_rom_64_ram", "crc32_update_rom", "64_ram", 1, "hash")]
        capture = "BUILD_ID=example\nHASHBENCH name=crc32_update_rom type=64_ram ops=1 warm=2.0 cold=3.0 insn=4.0\n"
        build, measured = parse_capture(capture, rows, prefix="HASHBENCH", placeholder="pending")
        self.assertEqual(measured[rows[0][0]], (2.0, 3.0, 4.0))
        self.assertEqual(parse_capture("pending", rows, prefix="HASHBENCH", placeholder="pending"), (None, {}))
        for bad in (capture + capture.splitlines()[-1] + "\n", capture.replace("ops=1", "ops=2")):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                parse_capture(bad, rows, prefix="HASHBENCH")

    def test_separate_bench_and_error_build_provenance(self):
        text = self.capture.replace("BUILD_ID=abcdef-diag", "BUILD_ID=abcdef-diag scope=bench")
        text += "BUILD_ID=123456-diag scope=error\nMATHERROR name=mathf_recip type=f max=1 unit=ulp method=sweep\n"
        build, measured = parse_capture(text, self.rows)
        self.assertEqual("abcdef-diag", build)
        self.assertEqual(2, len(measured))
        for invalid in (text.replace(" scope=error", ""),
                        text.replace("scope=error", "scope=bench"),
                        text.replace("scope=error", "scope=unknown"),
                        text.split("BUILD_ID=123456")[0],
                        text + "BUILD_ID=abcdef-diag scope=bench\n"):
            with self.subTest(text=invalid), self.assertRaises(ValueError):
                parse_capture(invalid, self.rows)

    def test_missing_row_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            parse_capture(self.capture.split("MATHBENCH name=sinf")[0], self.rows)

    def test_malformed_and_duplicate_lines_are_errors(self):
        for text in (self.capture.replace("12.0", "nan"),
                     self.capture.replace("insn=6.0", "insn=0.0"),
                     self.capture + self.capture.splitlines()[1] + "\n",
                     self.capture.replace("cold=24.0", "cold=oops"),
                     self.capture.replace("BUILD_ID=abcdef-diag\n", "")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_capture(text, self.rows)

    def test_capture_identity_noise_and_placeholder(self):
        self.assertEqual((None, {}), parse_capture(PLACEHOLDER + "\n", self.rows))
        for text in (self.capture + "BUILD_ID=other\n", self.capture.replace("abcdef-diag", "bad id"),
                     self.capture.replace("insn=6.0", "insn=6.0 junk"),
                     PLACEHOLDER + "\nnoise\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_capture(text, self.rows)
        self.assertEqual(parse_capture(self.capture, self.rows),
                         parse_capture("noise\n" + self.capture + "noise\n", self.rows))

class HashCaptureTests(unittest.TestCase):
    target = "HASHTARGET target=esp32s3 arch=xtensa execution=hardware\n"
    capture = ("BUILD_ID=example\n" + target +
               "HASHBENCH name=crc32_update_z type=256_ram ops=1 warm=2 cold=3 insn=4\n"
               "HASHBENCH name=crc32_update_a type=128_ram ops=1 warm=5 cold=6 insn=7\n"
               "HASHBENCH name=crc32_update_a type=32_ram ops=1 warm=8 cold=9 insn=10\n"
               "HASHBENCH name=crc32_update_a type=32_flash ops=1 warm=11 cold=12 insn=13\n")

    def test_target_record_prefix(self):
        self.assertEqual(bench.parse_target(self.target, prefix="HASHTARGET"), "esp32s3 (xtensa)")
        self.assertIsNone(bench.parse_target(self.target, prefix="MATHTARGET"))
        with self.assertRaises(ValueError):
            bench.parse_target(self.target.replace("hardware", "qemu"), prefix="HASHTARGET")

    def test_rows_as_found(self):
        build, measured = bench.parse_capture(self.capture, prefix="HASHBENCH")
        self.assertEqual(build, "example")
        self.assertEqual(measured["crc32_update_z", "256_ram"], (2, 3, 4))
        with self.assertRaises(ValueError):
            bench.parse_capture(self.capture + self.capture.splitlines()[-1], prefix="HASHBENCH")

    def test_target_capture_requires_hardware_and_one_identity(self):
        good = 'MATHTARGET target=esp32s3 arch=Xtensa execution=hardware\n'
        self.assertEqual(bench.parse_target(good, prefix="MATHTARGET"), 'esp32s3 (Xtensa)')
        self.assertEqual(bench.parse_target(good.replace(' execution', ' core=LX7 execution'), prefix="MATHTARGET"), 'esp32s3 (Xtensa, LX7)')
        for bad in (good.replace('hardware', 'host'), good + good.replace('esp32s3', 'other'),
                    good.replace(' execution=', ' mode=')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                bench.parse_target(bad, prefix="MATHTARGET")

