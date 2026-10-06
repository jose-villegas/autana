import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_lfs_objects


class MissingTests(unittest.TestCase):
    def test_reports_only_the_objects_the_server_has_an_error_for(self):
        def request(chunk):
            return [{"oid": oid, **({"error": {"code": 404}} if oid == "b" else {})} for oid, _ in chunk]

        self.assertEqual(check_lfs_objects.missing([("a", 1), ("b", 2), ("c", 3)], request), ["b"])

    def test_asks_in_batches_of_one_hundred(self):
        sizes = []

        def request(chunk):
            sizes.append(len(chunk))
            return [{"oid": oid} for oid, _ in chunk]

        self.assertEqual(check_lfs_objects.missing([(str(i), i) for i in range(250)], request), [])
        self.assertEqual(sizes, [100, 100, 50])

    def test_no_pointers_asks_nothing(self):
        self.assertEqual(check_lfs_objects.missing([], lambda chunk: self.fail("asked")), [])


if __name__ == "__main__":
    unittest.main()
