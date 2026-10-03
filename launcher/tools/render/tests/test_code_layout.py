import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import code_layout  # noqa: E402


class PinnedFunctionTests(unittest.TestCase):
    def test_a_pin_is_found_whether_static_or_not(self):
        source = ("static RENDER_ENTRY_OFFSET(6) void\nwalk_rows(int y0) {\n}\n"
                  "RENDER_ENTRY_OFFSET(12) raster_stats_t\nraster_draw(const raster_t* raster) {\n}\n"
                  "void\nunpinned(void) {\n}\n")
        self.assertEqual(code_layout.pinned_functions([source]), ["walk_rows", "raster_draw"])

    def test_the_macro_definition_itself_is_not_a_pin(self):
        header = "#define RENDER_ENTRY_OFFSET(slots) __attribute__((patchable_function_entry(slots, slots)))\n"
        self.assertEqual(code_layout.pinned_functions([header]), [])


class LineSpanTests(unittest.TestCase):
    def test_a_loop_inside_one_line_spans_one(self):
        self.assertEqual(code_layout.line_span(0x40, 0x40 + 32, 32), 1)

    def test_a_loop_crossing_a_line_boundary_spans_two(self):
        self.assertEqual(code_layout.line_span(0x5c, 0x5c + 8, 32), 2)


if __name__ == "__main__":
    unittest.main()
