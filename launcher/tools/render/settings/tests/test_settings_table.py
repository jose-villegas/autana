"""settings_table derives each row from render_settings_t's field comments and the default file."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import settings_table  # noqa: E402

HEADER = """
typedef struct {
    bool cull;    /* basic: skips what cannot show */
    bool batched; /* advanced: fewer fork/joins */
    int every;    /* basic [1, 4): frames between picks */
} render_settings_t;
"""
VALUES = {"cull": True, "batched": False, "every": 2}


class SettingsTableTests(unittest.TestCase):
    def test_a_row_per_field_in_struct_order(self):
        rows = [line for line in settings_table.render(HEADER, VALUES).splitlines() if line.startswith("| `")]
        self.assertEqual(rows, ["| `cull` | `true` | `any` | basic | skips what cannot show |",
                                "| `batched` | `false` | `any` | advanced | fewer fork/joins |",
                                "| `every` | `2` | `[1, 4)` | basic | frames between picks |"])

    def test_an_unknown_tier_is_refused(self):
        with self.assertRaisesRegex(ValueError, "tier 'expert'"):
            settings_table.render(HEADER.replace("/* basic:", "/* expert:"), VALUES)

    def test_the_page_matches_the_tree(self):
        self.assertEqual(settings_table.main(["--check"]), 0)


if __name__ == "__main__":
    unittest.main()
