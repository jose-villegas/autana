"""Console replies complete only at their newline, and a release image's refusal is named."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import console_protocol
from console_protocol import replies_to, require_development, reply_prefix


class ReplyTests(unittest.TestCase):
    def test_reply_endings_are_shared_with_tunables(self):
        self.assertEqual(console_protocol.reply_endings("EXAMPLE"),
                         ("EXAMPLE_OK", "EXAMPLE_ERR", "EXAMPLE_END"))
        self.assertEqual(console_protocol.reply_endings(console_protocol.TUNE_LIST),
                         console_protocol.TUNE_REPLY_ENDS)
        self.assertEqual(reply_prefix("sound stats"), ("SOUND", ("SOUND_END", "SOUND_ERR")))

    def test_tunable_verbs_use_tune_reply_contract(self):
        for line in ('SET render.cull 0', 'get render.cull', 'RESET render.cull', 'TUNE'):
            with self.subTest(line=line):
                prefix, endings = reply_prefix(line)
                for reply in ('TUNE_OK render.cull=0', 'TUNE_ERR unknown render.cull', 'TUNE_END count=1'):
                    self.assertEqual(replies_to((reply + '\n').encode(), prefix, endings), ([reply], True))

    def test_prefixed_reply_and_release_refusal(self):
        self.assertEqual(replies_to(b"I (20) console: OPEN_OK\n", "OPEN", ("OPEN_OK",)),
                         (["OPEN_OK"], True))
        with self.assertRaisesRegex(RuntimeError, "needs a development image"):
            require_development(b"W console: ignoring line: 'APPS'", "APPS")

    def test_a_line_split_across_reads_completes_only_at_its_newline(self):
        line = b"TONE heard 1000 Hz: -52.9 dB (silence -109.7 dB), open 1424 B DMA"
        self.assertEqual(replies_to(line[:40], "TONE", ("TONE",)), ([], False))
        self.assertEqual(replies_to(line + b"\r\n", "TONE", ("TONE",)), ([line.decode()], True))

    def test_reply_boundaries(self):
        cases = (
            (b"I (20) console: EXAMPLE data\n", (["EXAMPLE data"], False)),
            (b"\x1b[32mEXAMPLE data\x1b[0m\n", (["EXAMPLE data"], False)),
            (b"EXAMPLE\n", (["EXAMPLE"], False)),
            (b"text EXAMPLE data\n", ([], False)),
            (b"NOTEXAMPLE data\n", ([], False)),
            (b"EXAMPLEMARK data\n", ([], False)),
            (b"EXAMPLE2 data\n", ([], False)),
            (b"EXAMPLE=value\n", (["EXAMPLE=value"], False)),
            (b"I (20) EXAMPLE_END: EXAMPLE data\n", (["EXAMPLE data"], False)),
        )
        for data, expected in cases:
            with self.subTest(data=data):
                self.assertEqual(replies_to(data, "EXAMPLE", ("EXAMPLE_END", "EXAMPLE_ERR")), expected)

    def test_a_stem_reply_claims_every_word_it_starts(self):
        self.assertEqual(replies_to(b"EXAMPLE_ROW a\nEXAMPLE_END\n", "EXAMPLE_", ("EXAMPLE_END",)),
                         (["EXAMPLE_ROW a", "EXAMPLE_END"], True))
