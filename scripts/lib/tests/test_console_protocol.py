import io
import time
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from console_protocol import LineReader, replies_to, require_development


class ReaderTests(unittest.TestCase):
    def test_fragments_pending_lines_and_ansi(self):
        chunks = iter((b"\x1b[32mONE\r\nT", b"WO\x1b[0m\nTHREE\n", b""))
        log = io.BytesIO()
        reader = LineReader(lambda: next(chunks), log, time.monotonic() + 1)
        self.assertEqual(next(reader.lines()), "ONE")
        self.assertEqual(next(reader.lines()), "TWO")
        self.assertEqual(list(reader.lines()), ["THREE"])
        self.assertEqual(log.getvalue(), b"\x1b[32mONE\r\nTWO\x1b[0m\nTHREE\n")

    def test_emit_waits_for_device_line_boundary(self):
        chunks = iter((b"PART", b"IAL\n", b""))
        log = io.BytesIO()
        reader = LineReader(lambda: next(chunks), log, time.monotonic() + 1)

        def read_chunk():
            chunk = next(chunks)
            if chunk == b"IAL\n":
                reader.emit("tour step")
            return chunk

        reader.read_chunk = read_chunk
        self.assertEqual(list(reader.lines()), ["PARTIAL"])
        self.assertEqual(log.getvalue(), b"PARTIAL\ntour step\n")

    def test_prefixed_reply_and_release_refusal(self):
        self.assertEqual(replies_to(b"I (20) console: OPEN_OK\n", "OPEN", ("OPEN_OK",)),
                         (["OPEN_OK"], True))
        with self.assertRaisesRegex(RuntimeError, "needs a development image"):
            require_development(b"W console: ignoring line: 'APPS'", "APPS")
