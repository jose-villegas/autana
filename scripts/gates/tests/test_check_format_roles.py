"""A format field's colour marker follows from its label and its page's header."""
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import check_format_roles  # noqa: E402

HEADER = """#define THING_VERSION 1U
#define THING_MODE_FAST 0U
#define THING_MODE_SLOW_SAFE 1U
#define THING_AT_FRAC_BITS 6U
"""

PAGE = """Layout defines: [`thing.h`](../../launcher/main/thing.h).

```mermaid
packet-beta
0-1: "version = 1"
2: "mode"
3: "frac"
4-7: "🟧 item_count"
8-11: "items_off"
12-15: "🟦 int16 value"
```
"""


class FormatRolesTests(unittest.TestCase):
    def test_each_role_follows_its_rule(self):
        defines = ["THING_MODE_FAST", "THING_MODE_SLOW_SAFE", "THING_AT_FRAC_BITS"]
        roles = {label: check_format_roles.role(label, defines)
                 for label in ("0", "reserved", "magic APAK", "version = 2", "crc32", "items_off", "offset",
                               "item_count", "strings_size", "size", "joints", "mode", "frac", "int16 x")}
        self.assertEqual(roles, {"0": "padding", "reserved": "padding", "magic APAK": "identity",
                                 "version = 2": "identity", "crc32": "identity", "items_off": "offset",
                                 "offset": "offset", "item_count": "count", "strings_size": "count",
                                 "size": "count", "joints": "count", "mode": "code", "frac": "value",
                                 "int16 x": "value"})

    def test_write_marks_each_field_and_check_names_the_wrong_ones(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            (folder / "launcher/main").mkdir(parents=True)
            (folder / "launcher/main/thing.h").write_text(HEADER, encoding="utf-8")
            (folder / "docs/formats").mkdir(parents=True)
            page = folder / "docs/formats/THING.md"
            page.write_text(PAGE, encoding="utf-8")
            with mock.patch.object(check_format_roles, "ROOT", folder):
                text, wrong = check_format_roles.marked(page)
            self.assertEqual(len(wrong), 5)
            self.assertIn('4-7 "🟧 item_count" wants "🟩 item_count"', wrong[2])
            for line in ('0-1: "🟦 version = 1"', '2: "🟪 mode"', '3: "frac"', '4-7: "🟩 item_count"',
                         '8-11: "🟧 items_off"', '12-15: "int16 value"'):
                self.assertIn(line, text)
            page.write_text(text, encoding="utf-8")
            with mock.patch.object(check_format_roles, "ROOT", folder):
                self.assertEqual(check_format_roles.marked(page)[1], [])


if __name__ == "__main__":
    unittest.main()
