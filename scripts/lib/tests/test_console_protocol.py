from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from console_protocol import replies_to, require_development


class ReplyTests(unittest.TestCase):
    def test_prefixed_reply_and_release_refusal(self):
        self.assertEqual(replies_to(b"I (20) console: OPEN_OK\n", "OPEN", ("OPEN_OK",)),
                         (["OPEN_OK"], True))
        with self.assertRaisesRegex(RuntimeError, "needs a development image"):
            require_development(b"W console: ignoring line: 'APPS'", "APPS")

    def test_a_line_split_across_reads_completes_only_at_its_newline(self):
        line = b"TONE heard 1000 Hz: -52.9 dB (silence -109.7 dB), open 1424 B DMA"
        self.assertEqual(replies_to(line[:40], "TONE", ("TONE",)), ([], False))
        self.assertEqual(replies_to(line + b"\r\n", "TONE", ("TONE",)), ([line.decode()], True))
