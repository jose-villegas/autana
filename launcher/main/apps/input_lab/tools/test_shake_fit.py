"""Shake transcript statistics and plot contract."""
import gzip
import sys
from unittest import mock
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

    def test_non_still_statistics_and_shaking_does_not_raise_threshold(self):
        samples = shake_fit.parse(FIXTURE.read_text().splitlines())
        stats = shake_fit.summarize(samples)
        self.assertEqual({"median": 55, "p95": 59.5, "p99": 59.9, "max": 60}, stats["WALK"]["size"])
        threshold = shake_fit.suggest(stats, "size")[0]
        for sample in samples:
            if sample["step"] in shake_fit.SHAKING:
                sample["size"] *= 1000
        self.assertEqual(threshold, shake_fit.suggest(shake_fit.summarize(samples), "size")[0])

    def test_results_check_stale_without_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory)
            data = app / "tools/data"
            data.mkdir(parents=True)
            capture = data / "shake-arbitrary.log.gz"
            with gzip.open(capture, "wt") as stream:
                stream.write(FIXTURE.read_text())
            doc = app / "README.md"
            doc.write_text("<!-- generated: shake-arbitrary -->\n<!-- /generated: shake-arbitrary -->\n")
            with mock.patch.object(shake_fit, "HEADER", app / "shake_probe.h"), \
                 mock.patch.object(sys, "argv", ["shake_fit", "--results", "--check"]):
                before = doc.read_bytes()
                self.assertEqual(1, shake_fit.main())
                self.assertEqual(before, doc.read_bytes())
                self.assertFalse((app / "images").exists())
                shake_fit.generate_results(doc, app / "images", {"arbitrary": capture})
                self.assertEqual(0, shake_fit.main())
                image = app / "images/shake-arbitrary.png"
                image.unlink()
                doc.write_bytes(doc.read_bytes().replace(b"| STILL | SIZE | 15.0", b"| STILL | SIZE | 99.0"))
                before = doc.read_bytes()
                self.assertEqual(1, shake_fit.main())
                self.assertEqual(before, doc.read_bytes())
                self.assertFalse(image.exists())

    def test_invalid_and_truncated(self):
        self.assertEqual([], shake_fit.parse(["noise", "shake STILL invalid"]))
        with self.assertRaises(ValueError):
            shake_fit.parse(["LABDUMP_TRUNCATED"])
        with self.assertRaises(ValueError):
            shake_fit.suggest(shake_fit.summarize([]), "size")

    def test_gzip_and_legacy_field(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "shake.log.gz"
            with gzip.open(capture, "wt") as stream:
                stream.write(FIXTURE.read_text())
            self.assertEqual(shake_fit.parse(FIXTURE.read_text().splitlines()), shake_fit.read_capture(capture))
        row = "shake 1 0 acc 0,0,-1000 lin 10,0,0 size 10 smooth 10 jerky 999 old 0"
        self.assertEqual(shake_fit.parse([row.replace(" jerky 999", "")]), shake_fit.parse([row]))
        self.assertNotIn("jerky", shake_fit.parse([row])[0])
        self.assertIn("old", shake_fit.parse([row])[0])
        self.assertNotIn("shared", shake_fit.parse([row])[0])
        self.assertIn("shared", shake_fit.parse([row.replace(" old ", " shared ")])[0])

    def test_generate_results(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            doc = root / "README.md"
            doc.write_bytes(b"before\r\n<!-- generated: shake-fixture -->\r\n<!-- /generated: shake-fixture -->\r\nafter\r\n")
            captures = {"fixture": FIXTURE}
            shake_fit.generate_results(doc, root / "images", captures)
            content = doc.read_bytes()
            self.assertIn(b"| STILL | SIZE | 15.0 | 19.5 | 19.9 | 20.0 |", content)
            self.assertTrue(content.startswith(b"before\r\n"))
            self.assertTrue(content.endswith(b"after\r\n"))
            self.assertEqual(b"\x89PNG\r\n\x1a\n", (root / "images/shake-fixture.png").read_bytes()[:8])
            self.assertFalse(shake_fit.generate_results(doc, root / "images", captures, check=True))

    def test_plot(self):
        samples = shake_fit.parse(FIXTURE.read_text().splitlines())
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "shake.png"
            shake_fit.plot(samples, output)
            self.assertEqual(b"\x89PNG\r\n\x1a\n", output.read_bytes()[:8])


if __name__ == "__main__":
    unittest.main()
