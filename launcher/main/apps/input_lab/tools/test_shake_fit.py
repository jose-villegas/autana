"""Shake transcript statistics and plot contract."""
import tempfile
import unittest
from pathlib import Path

import shake_fit

FIXTURE = Path(__file__).parent / "fixtures" / "shake.log"


class ShakeFitTest(unittest.TestCase):
    def test_fixture_statistics(self):
        samples = shake_fit.parse(FIXTURE.read_text().splitlines())
        self.assertEqual(len(shake_fit.STEPS) * 2, len(samples))
        stats = shake_fit.summarize(samples)
        self.assertEqual(15, stats["STILL"]["size"]["median"])
        self.assertEqual(19.5, stats["STILL"]["size"]["p95"])
        self.assertEqual(19.9, stats["STILL"]["size"]["p99"])
        self.assertEqual(20, stats["STILL"]["size"]["max"])
        threshold, margin, full = shake_fit.suggest(stats, "size")
        self.assertEqual(stats["TAP"]["size"]["p99"], threshold)
        self.assertEqual(stats["GENTLE"]["size"]["median"] - threshold, margin)
        self.assertEqual(stats["HARD"]["size"]["p95"], full)

    def test_invalid_and_truncated(self):
        self.assertEqual([], shake_fit.parse(["noise", "shake STILL invalid"]))
        with self.assertRaises(ValueError):
            shake_fit.parse(["LABDUMP_TRUNCATED"])
        with self.assertRaises(ValueError):
            shake_fit.suggest(shake_fit.summarize([]), "size")

    def test_plot(self):
        samples = shake_fit.parse(FIXTURE.read_text().splitlines())
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "shake.png"
            shake_fit.plot(samples, output)
            self.assertEqual(b"\x89PNG\r\n\x1a\n", output.read_bytes()[:8])


if __name__ == "__main__":
    unittest.main()
