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
