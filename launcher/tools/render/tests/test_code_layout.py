"""The code layout check finds pinned functions and the baseline each source folder owns."""
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import code_layout  # noqa: E402


class PinnedFunctionTests(unittest.TestCase):
    def test_a_pin_is_found_whether_static_or_not(self):
        source = ("static CODE_ENTRY_OFFSET(6) void\nwalk_rows(int y0) {\n}\n"
                  "CODE_ENTRY_OFFSET(12) raster_stats_t\nraster_draw(const raster_t* raster) {\n}\n"
                  "void\nunpinned(void) {\n}\n")
        self.assertEqual(code_layout.pinned_functions([source]), ["walk_rows", "raster_draw"])

    def test_the_macro_definition_itself_is_not_a_pin(self):
        header = "#define CODE_ENTRY_OFFSET(slots) __attribute__((patchable_function_entry(slots, slots)))\n"
        self.assertEqual(code_layout.pinned_functions([header]), [])


class BaselineDiscoveryTests(unittest.TestCase):
    def test_each_source_folder_owns_its_baseline(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for folder in ("engine", "apps/example"):
                owner = root / folder
                owner.mkdir(parents=True)
                (owner / "walk.c").write_text("CODE_ENTRY_OFFSET(6) void\nwalk_rows(void) {}\n")
                (owner / "code_layout.txt").write_text(code_layout.LAYOUT_HEADER + "\n")
            self.assertEqual(code_layout.baselines(root), {
                root / "engine/code_layout.txt": ["walk_rows"],
                root / "apps/example/code_layout.txt": ["walk_rows"],
            })

    def test_a_pin_without_a_baseline_is_still_checked(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "walk.c").write_text("CODE_ENTRY_OFFSET(0) void\nwalk_rows(void) {}\n")
            self.assertEqual(code_layout.baselines(root), {root / "code_layout.txt": ["walk_rows"]})

    def test_a_baseline_without_pins_is_still_checked(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "code_layout.txt").write_text(code_layout.LAYOUT_HEADER + "\n")
            self.assertEqual(code_layout.baselines(root), {root / "code_layout.txt": []})

    def test_a_baseline_checks_only_its_build_configuration(self):
        text = code_layout.CONFIG_HEADER + "CONFIG_LAUNCHER_RELEASE\n"
        release = "CONFIG_LAUNCHER_RELEASE=y\n"
        diagnostics = "CONFIG_LAUNCHER_SELFTEST=y\n"
        self.assertTrue(code_layout.baseline_applies(text, release))
        self.assertFalse(code_layout.baseline_applies(text, diagnostics))
        self.assertTrue(code_layout.baseline_applies("", diagnostics))
        self.assertFalse(code_layout.baseline_applies("", release))


class LineSpanTests(unittest.TestCase):
    def test_a_loop_inside_one_line_spans_one(self):
        self.assertEqual(code_layout.line_span(0x40, 0x40 + 32, 32), 1)

    def test_a_loop_crossing_a_line_boundary_spans_two(self):
        self.assertEqual(code_layout.line_span(0x5c, 0x5c + 8, 32), 2)


class PoolTests(unittest.TestCase):
    # As objdump decodes r3d_span_triangle_impl: a short jump over a literal
    # pool whose bytes read as a backward branch.
    FOUND = [
        (0x1100, 3, "bnez", "a10, 1120 <f+0x20>"),
        (0x1103, 3, "j", "110c <f+0xc>"),
        (0x1106, 3, "blti", "a5, 7, 10f0 <f-0x10>"),
        (0x1109, 3, "src", "a0, a0, a0"),
        (0x110C, 2, "mov.n", "a10, a6"),
        (0x110E, 3, "j", "1100 <f>"),
        (0x1111, 3, "j", "1200 <f+0x100>"),
        (0x1114, 3, "addi", "a2, a2, 1"),
    ]

    def test_what_a_short_forward_jump_skips_is_dropped(self):
        kept = [address for address, _, _, _ in code_layout.skip_pools(self.FOUND)]
        self.assertEqual(kept, [0x1100, 0x1103, 0x110C, 0x110E, 0x1111, 0x1114])

    def test_code_a_branch_enters_is_kept_though_jumped_over(self):
        found = [
            (0x1300, 3, "beqz", "a2, 1306 <g+0x6>"),
            (0x1303, 3, "j", "130c <g+0xc>"),
            (0x1306, 3, "addi", "a3, a3, 1"),
            (0x1309, 3, "bnez", "a3, 1306 <g+0x6>"),
            (0x130C, 2, "retw.n", ""),
        ]
        self.assertEqual(code_layout.skip_pools(found), found)

    def test_code_entered_past_a_pool_is_decoded_again_from_its_entry(self):
        # As objdump read r3d_span_triangle_impl: a two-byte pool after a
        # short j, then code a beqz enters at 0x1408, read out of step.
        found = self.misdecoded_pool(
            0x1400, ("beqz", "a10, 1408 <h+0x8>"), "140c <h+0xc>",
            [("src", "a0, a0, a0"), ("bgez", "a13, 1300 <h-0x100>")], 0x140C)
        again = [(0x1408, 2, "l32i.n", "a8, a1, 4"), (0x140A, 2, "addi.n", "a8, a8, 1"), (0x140C, 2, "retw.n", "")]
        kept = code_layout.skip_pools(found, lambda start: [ins for ins in again if ins[0] >= start])
        self.assertEqual([ins[2] for ins in kept], ["beqz", "j", "l32i.n", "addi.n", "retw.n"])

    def test_embedded_literal_pool_resumes_at_its_end(self):
        found = [
            (0x42000000, 3, "l32r", "a8, 42000008 <f+0x8> (40002274 <callee>)"),
            (0x42000003, 3, "j", "4200000c <f+0xc>"),
            (0x42000006, 3, "bnez", "a2, 42000009 <f+0x9>"),
            (0x42000009, 3, "call8", "4200100c <unrelated+0xc>"),
            (0x4200000c, 3, "callx8", "a8"),
            (0x4200000f, 2, "retw.n", ""),
        ]
        starts = []

        def again(start):
            starts.append(start)
            return found[-2:]

        self.assertEqual(code_layout.skip_pools(found, again), found[:2] + found[-2:])
        self.assertIn(0x4200000c, starts)

    def test_alignment_before_code_in_a_long_jump_span(self):
        found = self.misdecoded_pool(
            0x42000000, ("bltz", "a4, 42000008 <f+0x8>"), "42000040 <f+0x40>",
            [("srai", "a0, a0, 16"), ("call8", "4200100c <unrelated+0xc>")], 0x42000040)
        again = [
            (0x42000008, 3, "l32r", "a8, 42001000 <pool> (40002274 <callee>)"),
            (0x4200000b, 3, "callx8", "a8"), (0x4200000e, 2, "retw.n", ""),
        ]
        self.assertEqual(code_layout.skip_pools(found, lambda start: again), found[:2] + again + found[-1:])

    def test_redecodes_code_entering_a_literal_pool(self):
        found = [
            (0x42000000, 3, "bnez", "a2, 42000007 <f+0x7>"),
            (0x42000003, 3, "j", "42000010 <f+0x10>"),
            (0x42000006, 3, "msub.s", "f0, f1, f2"),
            (0x42000010, 3, "madd.s", "f0, f1, f2"),
        ]
        again = [(0x42000007, 3, "add.n", "a2, a2, a3")]
        self.assertEqual(code_layout.skip_pools(found, lambda start: again), found[:2] + again + found[-1:])

    def misdecoded_pool(self, start, branch, jump, pool, end):
        return [
            (start, 3, *branch),
            (start + 3, 3, "j", jump),
            (start + 6, 3, *pool[0]),
            (start + 9, 3, *pool[1]),
            (end, 2, "retw.n", ""),
        ]


class SmallLoopTests(unittest.TestCase):
    def entries(self, found, start=0x1000, first_line=20, line_size=32, locations=None):
        def output(tool, *args):
            self.assertEqual(tool, "addr2line")
            self.assertEqual(args[:4], ("-a", "-i", "-e", "image.elf"))
            if locations is not None:
                return locations
            return "\n".join("%s\ninline.h:111\nwalk.c:%d" %
                             (address, first_line if int(address, 16) == start else first_line + 41)
                             for address in args[4:])

        with mock.patch.object(code_layout, "symbols", return_value={start: ("walk_rows", 512)}), \
                mock.patch.object(code_layout, "instructions", return_value=(found, None)), \
                mock.patch.object(code_layout, "find_tool", return_value="addr2line"), \
                mock.patch.object(code_layout, "run", side_effect=output) as lookup:
            entries = code_layout.layout_entries(Path("image.elf"), "nm", "objdump", line_size, ["walk_rows"])
        return entries, lookup.call_count

    def test_a_jump_before_the_function_is_not_a_loop(self):
        entries, _ = self.entries([(0x1006, 3, "j", "0fff <outside>")])
        self.assertEqual([entry.kind for entry in entries], ["function"])

    def test_back_edges_at_the_line_span_limit_are_recorded(self):
        for line_size in (16, 32, 64):
            limit = code_layout.SMALL_LOOP_MAX_LINES * line_size
            with self.subTest(line_size=line_size):
                entries, _ = self.entries([
                    (0x1000 + limit - 4, 3, "bnez", "a2, 1000 <walk_rows>"),
                    (0x1000 + limit - 3, 3, "j", "1000 <walk_rows>"),
                    (0x1000 + limit - 2, 3, "j", "1000 <walk_rows>"),
                ], line_size=line_size)
                self.assertEqual([entry.bytes for entry in entries[1:]], [limit - 1, limit])

    def test_hardware_loops_at_the_line_span_limit_are_recorded(self):
        for line_size in (16, 32, 64):
            limit = code_layout.SMALL_LOOP_MAX_LINES * line_size
            for mnemonic in ("loop", "loopnez", "loopgtz"):
                with self.subTest(line_size=line_size, mnemonic=mnemonic):
                    entries, _ = self.entries([(0x103d, 3, mnemonic,
                                                "%x <walk_rows+end>" % (0x1040 + limit))],
                                              line_size=line_size)
                    self.assertEqual([entry.bytes for entry in entries[1:]], [limit])

    def test_hardware_loops_over_the_line_span_limit_are_dropped(self):
        for line_size in (16, 32, 64):
            limit = code_layout.SMALL_LOOP_MAX_LINES * line_size
            for mnemonic in ("loop", "loopnez", "loopgtz"):
                with self.subTest(line_size=line_size, mnemonic=mnemonic):
                    entries, _ = self.entries([(0x103d, 3, mnemonic,
                                                "%x <walk_rows+end>" % (0x1040 + limit + 1))],
                                              line_size=line_size)
                    self.assertEqual(entries[1:], [])

    def test_a_short_body_spanning_too_many_lines_is_dropped(self):
        for line_size in (16, 32, 64):
            limit = code_layout.SMALL_LOOP_MAX_LINES * line_size
            with self.subTest(line_size=line_size):
                entries, _ = self.entries([(0x1001 + limit - 3, 3, "bnez", "a2, 1001 <walk_rows+1>")],
                                          line_size=line_size)
                self.assertEqual(entries[1:], [])

    def test_a_self_jump_is_not_a_back_edge(self):
        entries, _ = self.entries([(0x1006, 3, "j", "1006 <walk_rows+6>")])
        self.assertEqual([entry.kind for entry in entries], ["function"])

    def test_an_empty_loop_span_is_dropped(self):
        entries, _ = self.entries([(0x1006, 3, "loop", "1009 <walk_rows+9>")])
        self.assertEqual([entry.kind for entry in entries], ["function"])

    def test_incomplete_source_records_raise(self):
        with self.assertRaisesRegex(code_layout.LayoutError, "incomplete source map"):
            self.entries([(0x1020, 3, "j", "1008 <walk_rows+8>")],
                         locations="0x1000\nwalk.c:20\n")

    def test_unknown_source_line_raises(self):
        with self.assertRaisesRegex(code_layout.LayoutError, "no source line"):
            self.entries([], locations="0x1000\n??:0\n")

    def test_zero_source_line_raises(self):
        with self.assertRaisesRegex(code_layout.LayoutError, "no source line"):
            self.entries([], locations="0x1000\nwalk.c:0\n")

    def test_outermost_loop_source_must_match_the_function(self):
        with self.assertRaisesRegex(code_layout.LayoutError, "loop source differs"):
            self.entries([(0x1020, 3, "j", "1008 <walk_rows+8>")],
                         locations="0x1000\ninline.h:111\nwalk.c:20\n"
                                   "0x1020\nwalk.c:61\nother.c:61\n")

    def test_single_frame_source_records_are_handled(self):
        entries, _ = self.entries([(0x1020, 3, "j", "1008 <walk_rows+8>")],
                                  locations="0x1000\nwalk.c:20\n0x1020\nwalk.c:61\n")
        self.assertEqual([entry.source_line for entry in entries], [0, 41])

    def test_an_inserted_loop_does_not_renumber_other_lines(self):
        def row(kind, line, offset, size):
            return SimpleNamespace(function="walk_rows", kind=kind, source_line=line,
                                   offset=offset, bytes=size, lines=2)
        baseline = [row("function", 0, 0, 200), row("bbci-back", 41, 12, 31),
                    row("bbci-back", 41, 8, 40), row("bbci-back", 60, 8, 40)]
        actual = [baseline[0], row("bbci-back", 20, 4, 30), *baseline[1:]]
        self.assertEqual(code_layout.layout_differences(baseline, actual),
                         ["walk_rows bbci-back@+20: row added"])
        self.assertEqual(code_layout.layout_differences(baseline, baseline[:2] + baseline[3:]),
                         ["walk_rows bbci-back@+41[2]: row removed"])

    def test_relative_lines_and_whole_cache_line_shifts_match(self):
        before, calls = self.entries([(0x1020, 3, "j", "1008 <walk_rows+8>")])
        after, shifted_calls = self.entries([(0x1060, 3, "j", "1048 <walk_rows+8>")],
                                            start=0x1040, first_line=120)
        self.assertEqual(before[1].source_line, 41)
        self.assertEqual(after[1].source_line, 41)
        self.assertEqual((calls, shifted_calls), (1, 1))
        self.assertEqual(code_layout.layout_differences(code_layout.layout_rows(before, 32),
                                                       code_layout.layout_rows(after, 32)), [])


class LayoutCheckTests(unittest.TestCase):
    BASELINE = """\
# Generated by: python launcher/tools/render/code_layout.py --write launcher/build.diag
function\tkind\toffset\tbytes\tlines\tsource_line
raster_draw\tfunction\t0\t40\t2\t0
raster_draw\tloop\t4\t20\t1\t41
shade_span\tfunction\t8\t48\t2\t0
"""

    def differences(self, current):
        expected = code_layout.parse_layout(self.BASELINE)
        actual = code_layout.parse_layout(current)
        return code_layout.layout_differences(expected, actual)

    def test_an_unchanged_layout_passes(self):
        self.assertEqual(self.differences(self.BASELINE), [])

    def test_an_offset_change_names_the_row(self):
        current = self.BASELINE.replace("raster_draw\tloop\t4", "raster_draw\tloop\t6")
        self.assertEqual(self.differences(current), ["raster_draw loop@+41: offset 4 -> 6"])

    def test_a_loop_line_count_change_names_the_row(self):
        current = self.BASELINE.replace("raster_draw\tloop\t4\t20\t1", "raster_draw\tloop\t4\t20\t2")
        self.assertEqual(self.differences(current), ["raster_draw loop@+41: lines 1 -> 2"])

    def test_an_added_or_removed_pin_names_the_function(self):
        added = self.BASELINE + "new_draw\tfunction\t16\t24\t2\t0\n"
        removed = self.BASELINE.replace("shade_span\tfunction\t8\t48\t2\t0\n", "")
        self.assertEqual(self.differences(added), ["new_draw function: pin added"])
        self.assertEqual(self.differences(removed), ["shade_span function: pin removed"])

    def test_old_five_column_rows_raise(self):
        with self.assertRaisesRegex(code_layout.LayoutError, "six tab-separated columns"):
            code_layout.parse_layout(code_layout.LAYOUT_HEADER + "\nwalk_rows\tloop\t0\t20\t1\n")

    def test_noninteger_source_line_raises(self):
        with self.assertRaisesRegex(code_layout.LayoutError, "source_line must be integers"):
            code_layout.parse_layout(self.BASELINE.replace("\t41\n", "\tunknown\n"))

    def test_source_lines_round_trip_including_negative_values(self):
        rows = [code_layout.LayoutRow("walk_rows", "function", 0, 40, 2, 0),
                code_layout.LayoutRow("walk_rows", "loop", 4, 20, 1, -7),
                code_layout.LayoutRow("walk_rows", "loop", 4, 20, 1, 41)]
        self.assertEqual(code_layout.parse_layout(code_layout.format_layout(rows)), rows)

    def test_duplicate_keys_keep_their_ordinals(self):
        expected = code_layout.parse_layout(self.BASELINE)
        second = expected[1]._replace(bytes=24)
        expected.append(second)
        actual = expected[:-1] + [second._replace(offset=6)]
        self.assertEqual(code_layout.layout_differences(expected, actual),
                         ["raster_draw loop@+41[2]: offset 4 -> 6"])
        self.assertEqual(len(code_layout.indexed_rows(expected, {"raster_draw"})), 3)

    def test_committed_baselines_parse_with_the_current_header(self):
        paths = sorted(code_layout.SOURCES.rglob("code_layout.txt"))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(path=path):
                self.assertTrue(code_layout.parse_layout(path.read_text(encoding="utf-8")))


class ConfigurationCheckTests(unittest.TestCase):
    def test_check_without_an_applicable_baseline_fails(self):
        self.assert_no_baseline_fails("--check")

    def test_write_without_an_applicable_baseline_fails(self):
        self.assert_no_baseline_fails("--write")

    def assert_no_baseline_fails(self, mode):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build.diag"
            build.mkdir()
            (build / "launcher.elf").touch()
            (build / "sdkconfig").write_text("CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE=32\n"
                                             "CONFIG_LAUNCHER_SELFTEST=y\n")
            baseline = root / "code_layout.txt"
            baseline.write_text(code_layout.CONFIG_HEADER + "CONFIG_LAUNCHER_RELEASE\n" +
                                code_layout.LAYOUT_HEADER + "\n")
            with mock.patch.object(code_layout, "baselines", return_value={baseline: ["walk_rows"]}), \
                    mock.patch.object(code_layout, "layout_entries") as entries, \
                    mock.patch("sys.stderr") as stderr:
                self.assertEqual(code_layout.main([mode, str(build)]), 2)
            entries.assert_not_called()
            message = "".join(call.args[0] for call in stderr.write.call_args_list)
            self.assertIn("no baseline applies", message)
            self.assertIn(str(build), message)



class SameCodeTests(unittest.TestCase):
    BASE = """\
40370000 <walk_rows>:
40370000:\t004136        \tentry\ta1, 32
40370003:\tfffd21        \tl32r\ta2, 4036fff8 <_iram_text_start+0x10>
40370006:\tfcc416        \tbeqz\ta4, 40370000 <walk_rows>
40370009:\t000090        \tretw

40370010 <tick>:
40370010:\t004136        \tentry\ta1, 32
40370013:\t000090        \tretw
"""

    def code(self, text):
        return code_layout.function_code(text)

    def test_code_that_moved_compares_equal(self):
        moved = self.BASE.replace("4037000", "4038000").replace("4037001", "4038001").replace("4036fff8", "4037fff4")
        self.assertEqual(code_layout.code_differences(self.code(self.BASE), self.code(moved)), [])

    def test_a_changed_operand_names_its_function(self):
        changed = self.BASE.replace("beqz\ta4", "beqz\ta5")
        self.assertEqual(code_layout.code_differences(self.code(self.BASE), self.code(changed)),
                         ["walk_rows: changed (4 -> 4 instructions)"])

    def test_a_literal_load_keeps_its_register(self):
        other = self.BASE.replace("l32r\ta2", "l32r\ta3")
        self.assertEqual(code_layout.code_differences(self.code(self.BASE), self.code(other)),
                         ["walk_rows: changed (4 -> 4 instructions)"])

    def test_a_function_only_one_side_has_is_named(self):
        head = self.BASE.replace("<tick>", "<tock>")
        self.assertEqual(code_layout.code_differences(self.code(self.BASE), self.code(head)),
                         ["tick: removed", "tock: added"])

    def test_two_static_functions_sharing_a_name_are_both_kept(self):
        twice = self.BASE.replace("<tick>:", "<walk_rows>:")
        self.assertEqual(sorted(self.code(twice)), ["walk_rows", "walk_rows#2"])

    def test_a_loop_past_alignment_bytes_is_decoded_from_its_entry(self):
        # As objdump sweeps asset_pack_find: a jump, one alignment byte, then
        # a loop entered only from inside the span the jump skips; the sweep
        # reads the byte and the loop's first bytes as one instruction.
        swept = [
            (0x100d, 3, "bnez", "a9, 1016 <f+0x6>"),
            (0x1010, 3, "j", "1040 <f+0x30>"),
            (0x1013, 3, "mul16u", "a1, a0, a0"),
            (0x1016, 2, "mov.n", "a11, a3"),
            (0x1018, 3, "bne", "a5, a8, 1014 <f+0x4>"),
            (0x1040, 2, "retw.n", ""),
        ]
        entry = [(0x1014, 2, "addi.n", "a5, a5, 1"), (0x1016, 2, "mov.n", "a11, a3")]
        self.assertEqual([ins[2] for ins in code_layout.skip_pools(swept, lambda start: entry)],
                         ["bnez", "j", "addi.n", "mov.n", "bne", "retw.n"])

    LONG_CALL = """40370000 <walk_rows>:
40370000:	004136        	entry	a1, 32
40370003:	fffd81        	l32r	a8, 4036fff8 <_iram_text_start+0x10> (42002d50 <draw_x>)
40370006:	0008e0        	callx8	a8
40370009:	000090        	retw
"""

    def test_a_long_call_to_another_callee_differs(self):
        other = self.LONG_CALL.replace("<draw_x>", "<draw_y>")
        self.assertEqual(code_layout.code_differences(self.code(self.LONG_CALL), self.code(other)),
                         ["walk_rows: changed (4 -> 4 instructions)"])

    def test_a_long_call_whose_pool_moved_compares_equal(self):
        moved = self.LONG_CALL.replace("4036fff8 <_iram_text_start+0x10>", "4036ffe0 <_iram_text_start>")
        self.assertEqual(code_layout.code_differences(self.code(self.LONG_CALL), self.code(moved)), [])

    SWITCH = """40370000 <pick>:
40370000:	004136        	entry	a1, 32
40370003:	0008a0        	jx	a8
40370006:	000306        	j	40370014 <pick+0x14>
40370009:	1b0c22        	addi	a2, a2, 1
4037000c:	000106        	j	40370014 <pick+0x14>
4037000f:	1b0c22        	addi	a2, a2, 2
40370012:	f01d          	retw.n
40370014:	f01d          	retw.n
"""

    def test_a_case_a_jump_table_enters_is_compared(self):
        other = self.SWITCH.replace("a2, a2, 1", "a2, a2, 3")
        self.assertEqual(code_layout.code_differences(self.code(self.SWITCH), self.code(other)),
                         ["pick: changed (8 -> 8 instructions)"])

if __name__ == "__main__":
    unittest.main()
