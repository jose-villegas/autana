"""Named math calls and literal defines share the owning header values."""
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.c_header import read_define, tag_text, tag_value  # noqa: E402
from build.c_enum import read_enum  # noqa: E402


class CHeaderTests(unittest.TestCase):
    def test_asset_type_define_reads_little_endian_tag(self):
        with tempfile.TemporaryDirectory() as temp:
            header = pathlib.Path(temp) / "values.h"
            header.write_text("#define ITEM_ASSET ASSET_TYPE('T', 'E', 'S', 'T')\n")
            tag = read_define(header, "ITEM_ASSET")
            self.assertEqual(tag, int.from_bytes(b"TEST", "little"))
            self.assertEqual(tag_text(tag), "TEST")
            self.assertEqual(tag_value("TEST"), tag)

    def test_enum_counts_on_from_explicit_values(self):
        with tempfile.TemporaryDirectory() as temp:
            header = pathlib.Path(temp) / "values.h"
            header.write_text("typedef enum { OTHER } other_t;\n"
                              "typedef enum __attribute__((packed)) { A = 4, /* a, b */\n"
                              "    B, // c,\n    C_COUNT\n} c_t;\n")
            self.assertEqual(read_enum(header, "c_t"), {"A": 4, "B": 5, "C_COUNT": 6})

    def test_named_math_call_matches_literal(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "launcher") as temp:
            header = pathlib.Path(temp) / "values.h"
            header.write_text("#define AXIS ((vec3f_t){0.0F, 1.0F, 0.0F})\n"
                              "#define COLOR ((vec3f_t){1.0F, 1.0F, 1.0F})\n")
            expected = [read_define(header, name) for name in ("AXIS", "COLOR")]
            header.write_text("#define AXIS (vec3f_up())\n#define COLOR vec3f_one()\n")
            self.assertEqual([read_define(header, name) for name in ("AXIS", "COLOR")], expected)

    def test_prefixed_defines_keep_requested_order_and_literal_types(self):
        from build.c_header import read_defines

        with tempfile.TemporaryDirectory() as temp:
            header = pathlib.Path(temp) / "values.h"
            header.write_text('#define ITEM_COUNT 4U\n#define ITEM_LABEL "card"\n#define ITEM_AXIS {1, 2, 3}\n')
            self.assertEqual(read_defines(header, "ITEM_", ("AXIS", "COUNT", "LABEL")),
                             [(1, 2, 3), 4, "card"])
            self.assertEqual(read_defines(header, "ITEM_", ()), [])
            with self.assertRaisesRegex(RuntimeError, "ITEM_MISSING"):
                read_defines(header, "ITEM_", ("COUNT", "MISSING"))
