"""Named math calls and literal defines share the owning header values."""
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.c_header import read_define  # noqa: E402


class CHeaderTests(unittest.TestCase):
    def test_named_math_call_matches_literal(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "launcher") as temp:
            header = pathlib.Path(temp) / "values.h"
            header.write_text("#define AXIS ((vec3f_t){0.0F, 1.0F, 0.0F})\n"
                              "#define COLOR ((vec3f_t){1.0F, 1.0F, 1.0F})\n")
            expected = [read_define(header, name) for name in ("AXIS", "COLOR")]
            header.write_text("#define AXIS (vec3f_up())\n#define COLOR vec3f_one()\n")
            self.assertEqual([read_define(header, name) for name in ("AXIS", "COLOR")], expected)
