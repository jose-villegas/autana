"""The Python name hash agrees with the firmware's shared vectors and constants."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from build import name_hash
from build.c_header import read_define

LAUNCHER = Path(__file__).resolve().parents[2]
VECTORS = LAUNCHER / "test/fixtures/name_hash_vectors.h"


class NameHashTests(unittest.TestCase):
    def test_shared_vectors(self):
        names = read_define(VECTORS, "NAME_HASH_TEST_NAMES")
        hashes = read_define(VECTORS, "NAME_HASH_TEST_HASHES")
        self.assertEqual(len(names), len(hashes))
        for text, expected in zip(names, hashes):
            with self.subTest(name=text):
                actual = name_hash.name_hash(text)
                self.assertEqual(actual, expected)
                self.assertNotEqual(actual, name_hash.NONE)
                self.assertEqual(name_hash.name_hash_bytes(text.encode("utf-8")), expected)

    def test_embedded_nul(self):
        data = read_define(VECTORS, "NAME_HASH_TEST_BYTES").encode("utf-8")
        expected = read_define(VECTORS, "NAME_HASH_TEST_BYTES_HASH")
        self.assertEqual(name_hash.name_hash_bytes(data), expected)
        self.assertNotEqual(expected, name_hash.NONE)


if __name__ == "__main__":
    unittest.main()
