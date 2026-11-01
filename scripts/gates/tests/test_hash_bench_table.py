"""Hash benchmark tables follow captured identities and hardware provenance."""
import sys
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
import hash_bench_table as table


class HashBenchTableTests(unittest.TestCase):
    target = "HASHTARGET target=esp32s3 arch=xtensa execution=hardware\n"
    capture = ("BUILD_ID=example\n" + target +
               "HASHBENCH name=crc32_update_z type=256_ram ops=1 warm=2 cold=3 insn=4\n"
               "HASHBENCH name=crc32_update_a type=128_ram ops=1 warm=5 cold=6 insn=7\n"
               "HASHBENCH name=crc32_update_a type=32_ram ops=1 warm=8 cold=9 insn=10\n"
               "HASHBENCH name=crc32_update_a type=32_flash ops=1 warm=11 cold=12 insn=13\n")

    def test_table_sorted_by_candidate_numeric_size_and_source(self):
        rendered = table.render(self.capture)
        rows = [line for line in rendered.splitlines() if line.startswith("| `")]
        self.assertEqual(rows, [
            "| `crc32_update_a` | 32 | flash | 11.0 | 12.0 | 13.0 |",
            "| `crc32_update_a` | 32 | ram | 8.0 | 9.0 | 10.0 |",
            "| `crc32_update_a` | 128 | ram | 5.0 | 6.0 | 7.0 |",
            "| `crc32_update_z` | 256 | ram | 2.0 | 3.0 | 4.0 |",
        ])

    def test_placeholder_has_empty_table_and_pending_line(self):
        rendered = table.render(table.PLACEHOLDER + "\n")
        self.assertIn("Capture pending.", rendered)
        self.assertFalse(any(line.startswith("| `") for line in rendered.splitlines()))

    def test_measurements_require_hardware_target(self):
        for capture in (self.capture.replace(self.target, ""),
                        self.capture.replace("hardware", "qemu")):
            with self.subTest(capture=capture), self.assertRaises(ValueError):
                table.render(capture)
