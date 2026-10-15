"""Instruction ratchet contract, independent of an ESP-IDF installation."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/quality"))
SPEC = importlib.util.spec_from_file_location(
    "icount_gate", ROOT / "launcher/tools/quality/icount_gate.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class IcountGateTests(unittest.TestCase):
    def row(self, name="kernel", us=None, reps=1):
        return f"{gate.LINE_PREFIX} {name} us={us or gate.MIN_TOTAL_US} reps={reps}\n"

    def check(self, factor=1):
        measured = gate.parse_log(self.row())
        return gate.compare(measured, {"kernel": measured["kernel"] / factor})

    def test_inside_tolerance(self):
        self.assertEqual([], self.check(1 + gate.TOLERANCE / 2))
        self.assertEqual([], self.check(1 - gate.TOLERANCE / 2))

    def test_increase_requires_reason(self):
        self.assertIn("raise only with a stated reason", " ".join(self.check(1 + gate.TOLERANCE * 2)))

    def test_decrease_requires_lower_baseline(self):
        self.assertIn("lower the baseline", " ".join(self.check(1 - gate.TOLERANCE * 2)))

    def test_missing_and_extra(self):
        errors = gate.compare(gate.parse_log(self.row()), {"absent": 1})
        self.assertIn("kernel: no baseline", " ".join(errors))
        self.assertIn("absent: no measurement", " ".join(errors))

    def test_short_row(self):
        with self.assertRaisesRegex(ValueError, "minimum"):
            gate.parse_log(self.row(us=gate.MIN_TOTAL_US - 1))

    def test_empty_log(self):
        with self.assertRaisesRegex(ValueError, "no ICOUNT"):
            gate.parse_log("boot complete\n")

    def test_conversion(self):
        rows = gate.parse_log(self.row(reps=4))
        self.assertEqual(gate.MIN_TOTAL_US * (gate.INSTRUCTIONS_PER_US >> gate.ICOUNT_SHIFT) / 4,
                         rows["kernel"])

    def test_invalid_and_duplicate(self):
        for text in (self.row(reps=0), self.row() * 2, f"{gate.LINE_PREFIX} broken\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                gate.parse_log(text)

    def test_update_round_trip(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "serial.log"
            baseline = Path(directory) / "baseline.txt"
            log.write_text(self.row("z") + self.row("a", reps=3), encoding="utf-8")
            self.assertEqual(0, gate.main(["--baseline", str(baseline), "--update-baseline", str(log)]))
            self.assertEqual(0, gate.main(["--baseline", str(baseline), str(log)]))
            entries = [line.split("\t") for line in baseline.read_text().splitlines()
                       if not line.startswith("#")]
            self.assertEqual(["a", "z"], [name for _, name in entries])
            self.assertTrue(all(value.isdecimal() for value, _ in entries))


if __name__ == "__main__":
    unittest.main()
