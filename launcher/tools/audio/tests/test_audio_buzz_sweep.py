"""Buzz captures require a surface description and retain legacy capture readers."""
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import sys

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import audio_buzz_sweep as buzz
import audio_buzz_doc as doc


class BuzzSurfaceTest(unittest.TestCase):
    def test_surface_required(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            buzz.parser().parse_args(["--board", "test"])

    def test_surface_in_header(self):
        with tempfile.TemporaryDirectory() as directory:
            args = buzz.parser().parse_args(["--board", "test", "--surface", "folded cloth, face up",
                                             "--peaks", "0", "--out", str(Path(directory) / "capture.txt")])
            with mock.patch.object(buzz, "autana_run", return_value="{}"), mock.patch.object(buzz, "sweep"):
                buzz.capture(args)
            header = args.out.read_text().splitlines()[0]
            self.assertIn("test,", header)
            self.assertIn("surface=folded cloth, face up", header)

    def test_legacy_capture_still_readable(self):
        self.assertTrue(doc.buzz_data(buzz.ROOT / buzz.CAPTURE))
