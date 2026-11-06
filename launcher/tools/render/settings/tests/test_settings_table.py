"""settings_table derives each row from render_settings.h and refuses a field, define or tier it cannot pair."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import settings_table  # noqa: E402

HEADER = """
#define RENDER_SETTINGS_CULL    true /* basic: skips what cannot show */
#define RENDER_SETTINGS_BATCHED true /* advanced: fewer fork/joins */
typedef struct {
    bool cull; /* a comment */
    bool batched;
} render_settings_t;
"""


class SettingsTableTests(unittest.TestCase):
    def test_a_row_per_field_in_struct_order(self):
        rows = [line for line in settings_table.render(HEADER).splitlines() if line.startswith("| `")]
        self.assertEqual(rows, ["| `cull` | `true` | basic | skips what cannot show |",
                                "| `batched` | `true` | advanced | fewer fork/joins |"])

    def test_two_fields_on_one_line_are_both_rows(self):
        joined = HEADER.replace("bool cull; /* a comment */\n    bool batched;", "bool cull, batched;")
        self.assertEqual(settings_table.render(joined), settings_table.render(HEADER))

    def test_a_field_without_its_define_is_refused(self):
        missing = "\n".join(line for line in HEADER.splitlines() if "_BATCHED" not in line)
        with self.assertRaisesRegex(ValueError, "batched has no RENDER_SETTINGS_BATCHED"):
            settings_table.render(missing)

    def test_a_define_without_its_field_is_refused(self):
        extra = HEADER.replace("typedef", "#define RENDER_SETTINGS_FOG true /* basic: haze */\ntypedef")
        with self.assertRaisesRegex(ValueError, "fog has a define but no field"):
            settings_table.render(extra)

    def test_an_unknown_tier_is_refused(self):
        with self.assertRaisesRegex(ValueError, "tier 'expert'"):
            settings_table.render(HEADER.replace("/* basic:", "/* expert:"))


if __name__ == "__main__":
    unittest.main()
