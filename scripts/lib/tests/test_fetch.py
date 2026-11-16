"""A pinned download reaches its target only with the pinned SHA-256, and a failed one leaves nothing behind."""
import hashlib
import io
import pathlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import fetch  # noqa: E402

BODY = b"pinned bytes"


class FetchVerifiedTests(unittest.TestCase):
    def setUp(self):
        self.root = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        self.target = self.root / "cache" / "file.bin"

    def serve(self, body):
        return mock.patch("urllib.request.urlopen", return_value=io.BytesIO(body))

    def test_a_matching_body_becomes_the_target(self):
        with self.serve(BODY) as opened:
            self.assertEqual(fetch.fetch_verified("https://example.invalid/f", self.target,
                                                  hashlib.sha256(BODY).hexdigest(), timeout=7), self.target)
        self.assertEqual(self.target.read_bytes(), BODY)
        self.assertEqual(opened.call_args.kwargs["timeout"], 7)
        self.assertEqual(fetch.file_sha256(self.target), hashlib.sha256(BODY).hexdigest())

    def test_a_mismatched_body_names_both_digests_and_leaves_nothing(self):
        with self.serve(b"tampered"), self.assertRaisesRegex(ValueError, "pinned " + "0" * 64):
            fetch.fetch_verified("https://example.invalid/f", self.target, "0" * 64)
        self.assertEqual(list(self.target.parent.iterdir()), [])

    def test_a_failed_fetch_leaves_nothing(self):
        with mock.patch("urllib.request.urlopen", side_effect=OSError("stalled")), self.assertRaises(OSError):
            fetch.fetch_verified("https://example.invalid/f", self.target, "0" * 64)
        self.assertEqual(list(self.target.parent.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
