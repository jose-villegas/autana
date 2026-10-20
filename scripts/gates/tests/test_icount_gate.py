"""Instruction ratchet contract, independent of an ESP-IDF installation."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/quality"))
SPEC = importlib.util.spec_from_file_location(
    "icount_gate", ROOT / "launcher/tools/quality/icount_gate.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class IcountGateTests(unittest.TestCase):
    def row(self, name="kernel", us=200000, reps=1, prefix=gate.LINE_PREFIX):
        return f"{prefix} {name} us={us} reps={reps}\n"

    def run_gate(self, text, baseline="200000000\tkernel\n", update=False):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "serial.log"
            peg = Path(directory) / "baseline.txt"
            log.write_bytes(text.encode() if isinstance(text, str) else text)
            if baseline is not None:
                peg.write_text(baseline, encoding="utf-8")
            args = [str(log), "--baseline", str(peg)]
            if update:
                args.append("--update-baseline")
            result = gate.main(args)
            return result, peg.read_text() if peg.exists() else None

    def test_info_rows_never_ratchet(self):
        info_us = gate.NS_PER_US
        info = self.row(name="info", us=info_us, prefix=gate.INFO_PREFIX)
        _, baseline = self.run_gate(self.row() + info, None, update=True)
        self.assertIn(f"\tinfo\t{gate.INFO_MARKER}", baseline)
        for text in (self.row(), self.row() + info,
                     self.row() + self.row(name="info", us=info_us // 2, prefix=gate.INFO_PREFIX),
                     self.row() + self.row(name="info", us=info_us * 2, prefix=gate.INFO_PREFIX),
                     self.row() + info.replace("info", "new_info")):
            with self.subTest(text=text):
                self.assertEqual(0, self.run_gate(text, baseline)[0])
        self.assertNotEqual(0, self.run_gate(self.row(us=201001) + info, baseline)[0])

    def test_main_rejects_bad_measurements(self):
        for text in (self.row(us=201001), self.row(us=198999),
                     self.row(name="unknown"), self.row() + self.row(name="extra"), "boot\n"):
            with self.subTest(text=text):
                self.assertNotEqual(0, self.run_gate(text)[0])
        self.assertNotEqual(0, self.run_gate(self.row(), "200000000\tkernel\n1\tabsent\n")[0])
        self.assertNotEqual(0, self.run_gate(self.row(), None)[0])

    def test_literal_half_percent_band(self):
        for us, expected in ((199001, 0), (200999, 0), (198999, 1), (201001, 1)):
            with self.subTest(us=us):
                self.assertEqual(expected, self.run_gate(self.row(us=us))[0])

    def test_conversion_at_larger_shift(self):
        with patch.object(gate, "ICOUNT_SHIFT", 4):
            self.assertEqual(12500000, gate.parse_log(self.row())["kernel"])

    def test_invalid_and_duplicate_rows(self):
        for text in (self.row(reps=0), self.row() * 2, f"{gate.LINE_PREFIX} broken\n",
                     self.row().rstrip() + " trailing\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                gate.parse_log(text)

    def test_indented_row_is_ignored(self):
        self.assertEqual({"kernel": 200000000},
                         gate.parse_log(self.row() + " " + self.row(name="ignored")))
        with self.assertRaisesRegex(ValueError, "no ICOUNT"):
            gate.parse_log(" " + self.row())

    def test_update_round_trip_rounds_fractional_count(self):
        text = self.row("z") + self.row("a", us=200000, reps=3)
        result, baseline = self.run_gate(text, None, update=True)
        self.assertEqual(0, result)
        self.assertEqual(["66666667\ta", "200000000\tz"],
                         [line for line in baseline.splitlines() if not line.startswith("#")])
        self.assertEqual(0, self.run_gate(text, baseline)[0])

    def test_main_rejects_invalid_baselines(self):
        for baseline in ("0\tkernel", "-1\tkernel", "NaN\tkernel", "inf\tkernel",
                         "200000000\tkernel\n200000000\tkernel", "200000000 kernel",
                         "200000000\tkernel\tunknown"):
            with self.subTest(baseline=baseline):
                self.assertNotEqual(0, self.run_gate(self.row(), baseline)[0])

    def test_baseline_comments_and_blank_lines(self):
        self.assertEqual(0, self.run_gate(self.row(), "# comment\n\n200000000\tkernel\n\n")[0])

    def test_non_utf8_boot_output(self):
        self.assertEqual(0, self.run_gate(b"boot\xff\n" + self.row().encode())[0])


if __name__ == "__main__":
    unittest.main()
