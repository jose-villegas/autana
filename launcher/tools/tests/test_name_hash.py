"""The Python name hash agrees with the firmware's shared vectors and constants."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from build import name_hash
from build.c_header import read_define

LAUNCHER = Path(__file__).resolve().parents[2]
VECTORS = LAUNCHER / "packages/name_hash/tests/name_hash_vectors.h"


class NameHashTests(unittest.TestCase):
    def test_shared_vectors(self):
        names = read_define(VECTORS, "NAME_HASH_TEST_NAMES")
        hashes = read_define(VECTORS, "NAME_HASH_TEST_HASHES")
        hashes64 = read_define(VECTORS, "NAME_HASH64_TEST_HASHES")
        self.assertEqual(len(names), len(hashes))
        self.assertEqual(len(names), len(hashes64))
        for text, expected, expected64 in zip(names, hashes, hashes64):
            with self.subTest(name=text):
                self.assertEqual(name_hash.name_hash64_n(name_hash.OFFSET_BASIS64, text.encode("utf-8")), expected64)
                actual = name_hash.name_hash(text)
                self.assertEqual(actual, expected)
                self.assertNotEqual(actual, name_hash.NONE)
                self.assertEqual(name_hash.name_hash_n(text.encode("utf-8")), expected)

    def test_embedded_nul(self):
        data = read_define(VECTORS, "NAME_HASH_TEST_BYTES").encode("utf-8")
        expected = read_define(VECTORS, "NAME_HASH_TEST_BYTES_HASH")
        self.assertEqual(name_hash.name_hash_n(data), expected)
        self.assertNotEqual(expected, name_hash.NONE)
        self.assertEqual(name_hash.name_hash64_n(name_hash.OFFSET_BASIS64, data),
                         read_define(VECTORS, "NAME_HASH64_TEST_BYTES_HASH"))

    def test_seeded_continue64(self):
        prefix = read_define(VECTORS, "NAME_HASH64_TEST_PREFIX").encode("utf-8")
        suffix = read_define(VECTORS, "NAME_HASH64_TEST_SUFFIX").encode("utf-8")
        seed = name_hash.name_hash64_n(name_hash.OFFSET_BASIS64, prefix)
        self.assertEqual(seed, read_define(VECTORS, "NAME_HASH64_TEST_PREFIX_HASH"))
        continued = name_hash.name_hash64_n(seed, suffix)
        self.assertEqual(continued, read_define(VECTORS, "NAME_HASH64_TEST_JOINED_HASH"))
        self.assertEqual(continued, name_hash.name_hash64_n(name_hash.OFFSET_BASIS64, prefix + suffix))
        self.assertEqual(seed, name_hash.name_hash64_n(seed, b""))


if __name__ == "__main__":
    unittest.main()
