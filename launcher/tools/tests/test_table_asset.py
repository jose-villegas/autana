"""Named int16 tables round trip and reject malformed rows and entries."""

import pathlib
import sys
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from asset import table_asset

FRAC_BITS = 4


class TableTests(unittest.TestCase):
    def test_round_trip(self):
        rows = {"a": [table_asset.VALUE_MIN, 0, table_asset.VALUE_MAX], "b": [1, 2, 3]}
        self.assertEqual(table_asset.decode(table_asset.encode(rows, FRAC_BITS)), (rows, FRAC_BITS))

    def test_bad_rows_are_rejected(self):
        for rows in ({}, {"x": []}, {"x" * table_asset.NAME_SIZE: [0]},
                     {"Ã©" * (table_asset.NAME_SIZE // 2): [0]},
                     {"x\0y": [0]}, {"a": [0], "b": [0, 1]},
                     {"a": [table_asset.VALUE_MAX + 1]}, {"a": [table_asset.VALUE_MIN - 1]}):
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                table_asset.encode(rows, FRAC_BITS)
        for frac in (-1, table_asset.FRAC_MAX + 1):
            with self.assertRaises(ValueError):
                table_asset.encode({"x": [0]}, frac)

    def test_bad_entries_are_rejected(self):
        good = table_asset.encode({"x": [0]}, FRAC_BITS)
        invalid = [good[:table_asset.HEADER.size - 1], good[:-1], good + b"\0"]
        for offset, value in ((table_asset.AT_VERSION, table_asset.VERSION + 1),
                              (table_asset.AT_ROWS, 0), (table_asset.AT_COLUMNS, 0), (table_asset.AT_RESERVED, 1)):
            bad = bytearray(good)
            bad[offset] = value
            invalid.append(bad)
        bad = bytearray(good)
        bad[table_asset.HEADER.size:table_asset.HEADER.size + table_asset.NAME_SIZE] = b"x" * table_asset.NAME_SIZE
        invalid.append(bad)
        for entry in invalid:
            with self.subTest(entry=entry), self.assertRaises(ValueError):
                table_asset.decode(entry)


if __name__ == "__main__":
    unittest.main()
