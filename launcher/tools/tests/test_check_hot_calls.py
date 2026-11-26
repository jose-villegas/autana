"""Release hot-call checks on disassembly, map, linker and source text fixtures."""

import contextlib
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "build"))

import check_hot_calls as gate  # noqa: E402

DUMP = """
42000020 <hot>:
42000020: 000025 call8 42000200 <floorf>
42000023: ffff81 l32r a8, 42000018 <hot-0x8> (400011e8 <memset>)
42000026: 0008e0 callx8 a8
42000029: 000006 j 42000033 <hot+0x13>
4200002c: 000025 call8 42000200 <floorf>
42000033: 000025 call8 42000220 <local>
42000036: 000006 j 42000210 <divide+0x4>
42000039: ffff91 l32r a9, 4200001c <hot-0x4> (400011e8 <memset>)
4200003c: ffff71 l32r a7, 4200001c <hot-0x4> (400011e8 <memset>)
4200003f: 207220 mov a7, a2
42000042: 0007e0 callx8 a7
"""

CODE_SECTIONS = {".flash.text"}
IDF_DIR = "idf"

MAP = """
.flash.text 0x42000200 0x30
 .text.floorf 0x42000200 0x10 toolchain/libc.a(libm_math_sf_floor.c.o)
 .text.divide_with_a_long_input_section_name
              0x42000210 0x10 toolchain/libgcc.a(divide.o)
 .text.local  0x42000220 0x10 esp-idf/main/libmain.a(example.c.obj)
.debug_loclists 0x7ff0 0x20
 .debug_loclists 0x7ff0 0x20 toolchain/libc.a(debug.o)
"""


class HotCallsGate(unittest.TestCase):
    def setUp(self):
        self.root = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory()))

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def test_linked_runtime_calls_include_tail_and_literal_targets(self):
        scripts = IDF_DIR + "/components/esp_rom/esp32s3/ld/"
        self.write(scripts + "esp32s3.rom.libc.ld", "memset = 0x400011e8;\n")
        self.write(scripts + "esp32s3.rom.libc_unused.ld", "local = 0x42000220;\n")
        ninja = """build launcher.elf: LINK rule
  LINK_FLAGS = -T esp32s3.rom.libc.ld
build other.elf: LINK rule
  LINK_FLAGS = -T esp32s3.rom.libc_unused.ld
"""
        runtime = gate.runtime_ranges(MAP, ninja, self.root / IDF_DIR, "esp32s3", CODE_SECTIONS)
        elf = self.root / "launcher.elf"
        function = gate.code_layout.Symbol(0x42000020, "hot", 0x25, "t")

        def disassemble(_tool, _elf, *args):
            start = int(next(arg for arg in args if arg.startswith("--start-address=")).split("=")[1], 16)
            return [ins for ins in gate.code_layout.parse_objdump(DUMP) if ins[0] >= start]

        with patch.object(gate.code_layout, "decode", side_effect=disassemble):
            calls = list(gate.linked_calls("objdump", elf, function, [], runtime))
        self.assertEqual(
            calls,
            [("hot", "floorf", 0x42000020), ("hot", "memset", 0x42000026), ("hot", "divide", 0x42000036)],
        )

    def test_linked_calls_keep_jump_table_arms_skipped_by_a_forward_jump(self):
        dump = """42000020: 0000a0 jx a8
42000023: 000006 j 42000029 <hot+0x9>
42000026: 000025 call8 400011e8 <memset>
42000029: f01d retw.n
"""
        function = gate.code_layout.Symbol(0x42000020, "hot", 0xb, "t")
        with patch.object(gate.code_layout, "run", return_value=dump):
            calls = list(gate.linked_calls("objdump", self.root / "launcher.elf", function, [],
                                           lambda address: address == 0x400011e8))
        self.assertEqual(calls, [("hot", "memset", 0x42000026)])

    def test_only_code_sections_contribute_runtime_archive_ranges(self):
        runtime = gate.runtime_ranges(MAP, "build launcher.elf: LINK rule\n", self.root / IDF_DIR,
                                      "esp32s3", CODE_SECTIONS)
        self.assertFalse(runtime(0x7ff0))
        self.assertTrue(runtime(0x42000200))

    def test_note_ends_at_terminator_before_trailing_comment(self):
        name = "launcher/main/example.c"
        path = self.write(name, "// Library call: Copies a.\na(); // copy\nb();\n")
        calls = [("hot", "memcpy", 1), ("hot", "memcpy", 2)]
        self.assertEqual(gate.violations(self.root, calls, [f"{path}:2", f"{path}:3"]),
                         [f"{name}:3: hot calls memcpy at 0x2"])

    def test_note_spans_blank_line_inside_statement(self):
        path = self.write("launcher/main/example.c", "// Library call: Copies a.\na(\n\n    b());\n")
        self.assertEqual(gate.violations(self.root, [("hot", "memcpy", 1)], [f"{path}:4"]), [])

    def test_empty_marker_in_merged_block_fails_independently(self):
        name = "launcher/main/example.c"
        cases = (
            ("// Library call:\n// Library call: Copies a.\n", 3),
            ("/* Library call:\n * Library call: Copies a. */\n", 3),
            ("/* Library call: */\n/* Library call: Copies a. */\n", 3),
            ("/* Library call: */\n/* Unrelated prose. */\n/* Library call: Copies a. */\n", 4),
            ("/* Library call: Library call: Copies a. */\n\n", 1),
        )
        for notes, call_line in cases:
            with self.subTest(notes=notes):
                path = self.write(name, notes + "a();\n")
                bad = gate.violations(self.root, [("hot", "memcpy", 1)], [f"{path}:{call_line}"])
                self.assertEqual(bad, [f"{name}:1: Library call: annotation has no reason"])

    def test_own_source_prefers_innermost_nonzero_line_in_outer_file(self):
        outer = "example.c:30"
        inner = "example.c:12 (discriminator 1)"
        self.assertEqual(gate.own_source(["example.c:0", inner, outer]), inner)
        self.assertEqual(gate.own_source(["other.h:12", outer]), outer)
        self.assertEqual(gate.own_source(["other.h:12", "example.c:0"]), "example.c:0")
        self.assertEqual(gate.own_source([inner, "example.c:20", outer]), inner)

    def test_missing_elf_exits_with_its_path(self):
        done = subprocess.run([sys.executable, gate.__file__, str(self.root)], stdin=subprocess.DEVNULL,
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 2)
        self.assertIn(str(self.root / "launcher.elf"), done.stderr)

    def test_active_layout_and_attribute_sites_supply_the_hot_set(self):
        sources = self.root / "launcher/main"
        self.write("launcher/main/active/pin.c", "CODE_ENTRY_OFFSET(1) void pin(void) {}")
        self.write("launcher/main/active/code_layout.txt", gate.code_layout.format_layout(
            [("row", "function", 0, 4, 1, 0)], metadata=["# Config: CONFIG_LAUNCHER_RELEASE"]))
        self.write("launcher/main/inactive/code_layout.txt", gate.code_layout.format_layout(
            [("diag_only", "function", 0, 4, 1, 0)]))
        site = gate.memory_map.Site("launcher/main/example.c", 2, "engine", "A reason.", 8, "libmain.a",
                                    [("example", "attr"), ("example", "cloned"), ("example", "compiled_out")],
                                    required=[("example", name, "launcher/main/example.c", line)
                                              for name, line in (("attr", 2), ("cloned", 4))])
        fragment = gate.memory_map.Site("launcher/main/test.lf", 1, "engine", "A reason.", 4, "libidf.a",
                                        [("test", "not_ours")])
        def excluded(path, symbol):
            return gate.memory_map.Site(path, 1, "engine", "A reason.", 8, "libmain.a", [],
                                        required=[("example", symbol, path, 1)])

        inactive = excluded("launcher/main/inactive.c", "inactive")
        external = excluded("external/example.c", "external")
        records = [gate.code_layout.Symbol(address, name, 4, section) for address, name, section in
                   [(0x42000020, "pin", "t"), (0x42000024, "row", "t"), (0x40380000, "attr", "W"),
                    (0x40380004, "cloned$constprop$0", "t"), (0x40380008, "inactive", "t"),
                    (0x4038000c, "external", "t")]]
        map_text = """.iram0.text 0x40380000 0x10
 .iram1.0 0x40380000 0x4 main/libmain.a(example.c.obj)
 .iram1.1 0x40380004 0x4 main/libmain.a(example.c.obj)
 .iram1.2 0x40380008 0x4 main/libmain.a(example.c.obj)
 .iram1.3 0x4038000c 0x4 main/libmain.a(example.c.obj)
"""
        inputs, _, _ = gate.memory_map.map_inputs(map_text, {r.address: (r.name, r.size) for r in records})
        link = dict(records=records, placed_inputs=inputs, inlines=[])
        sites = [site, fragment, inactive, external]
        with patch.object(gate.memory_map, "build_sites", return_value=(sites, lambda site: site != inactive)):
            hot = gate.hot_functions(self.root, sources, link)
            self.assertEqual(set(hot), {records[index] for index in (0, 2, 3)})
            with self.assertRaisesRegex(gate.code_layout.LayoutError, "cloned"):
                gate.hot_functions(self.root, sources, dict(link, records=records[:3]))
            with self.assertRaisesRegex(gate.code_layout.LayoutError, "pin"):
                gate.hot_functions(self.root, sources, dict(link, records=records[1:]))

    def test_annotations_cover_a_call_line_or_its_immediate_comment_block(self):
        name = "launcher/main/example.c"
        path = self.write(name, """    clear(); /* Library call: Required by the ABI. */
    /*
     * Library call: The buffer is shared with a driver.
     */
    copy();
    clear(); // Library call:
    // Library call: Only for a different line.

    copy();
    // Library call: Stale.
""")
        header = "launcher/packages/math/include/math/test.h"
        self.write(header, "/* Library call: Stale in a header. */\n")
        calls = [("hot", "memset", 0x42000020), ("hot", "memcpy", 0x42000023),
                 ("hot", "memset", 0x42000026), ("hot", "memcpy", 0x42000029),
                 ("hot", "memcpy", 0x4200002c), ("hot", "memcpy", 0x42000030)]
        locations = [f"{path}:{n}" for n in (1, 5, 6, 9)] + ["??:?", f"{path}:0"]
        bad = gate.violations(self.root, calls, locations)
        self.assertNotIn(f"{name}:5: hot calls memcpy at 0x42000023", bad)
        self.assertIn(f"{name}:6: hot calls memset at 0x42000026", bad)
        self.assertIn(f"{name}:9: hot calls memcpy at 0x42000029", bad)
        self.assertIn("??:0: hot calls memcpy at 0x4200002c", bad)
        self.assertIn("??:0: hot calls memcpy at 0x42000030", bad)
        self.assertEqual(8, len(bad))
        for location in (f"{name}:6", f"{name}:7", f"{name}:10", f"{header}:1"):
            self.assertTrue(any(line.startswith(location + ":") and "Library call:" in line for line in bad))

    def test_a_note_covers_the_whole_statement_or_signature_below_it(self):
        name = "launcher/main/split.c"
        path = self.write(name, """/* Library call: A signature the line table names. */
static void
boxed(int a,
      int b) {
    /* Library call: A statement split over lines. */
    const int f =
        a / b;
    copy();
}
""")
        calls = [("boxed", "memcpy", 0x42000020), ("boxed", "__divsi3", 0x42000023),
                 ("boxed", "memcpy", 0x42000026)]
        locations = [f"{path}:{n}" for n in (4, 7, 8)]
        self.assertEqual([f"{name}:8: boxed calls memcpy at 0x42000026"], gate.violations(self.root, calls, locations))

    def test_inline_ranges_select_calls_at_their_start_but_not_their_end(self):
        source = "launcher/main/example.c"
        self.write(source, "CODE_ENTRY_OFFSET(1) void pin(void) {}")
        caller = gate.code_layout.Symbol(0x42000020, "hot", 0x20, "t")
        pin = gate.code_layout.Symbol(0x42000050, "pin", 0x20, "W")
        site = gate.memory_map.Site(source, 1, "engine", "A reason.", 8, "libmain.a", [],
                                    required=[("example", "attr", source, 1)])
        elf = self.root / "launcher.elf"
        rows = [(f"C:/checkout/{source}", "attr", ".flash.text", start, end)
                for start, end in ((0x42000023, 0x42000029), (0x42000053, 0x42000059))]
        link = dict(elf=elf, records=[caller, pin], placed_inputs=[], inlines=rows)
        with patch.object(gate.memory_map, "build_sites", return_value=([site], lambda _: True)):
            hot = gate.hot_functions(self.root, self.root / "launcher/main", link)
            self.assertEqual(hot, {caller: [(0x42000023, 0x42000029, "attr")], pin: []})
            for function, names in ((caller, ["attr"] * 2), (pin, ["pin"] * 4)):
                addresses = [function.address + offset for offset in (0, 3, 6, 9)]
                dump = "\n".join(f"{address:x}: 000025 call8 42000200 <floorf>" for address in addresses)
                with patch.object(gate.code_layout, "run", return_value=dump):
                    calls = list(gate.linked_calls("objdump", elf, function, hot[function], lambda _: True))
                self.assertEqual(calls, list(zip(names, ["floorf"] * len(names),
                                                 addresses[1:3] if function == caller else addresses)))
            for start, end in ((0x42000010, 0x42000012), (0x4200003e, 0x42000041)):
                link["inlines"] = [(source, "attr", ".flash.text", start, end)]
                with self.subTest(start=start), self.assertRaisesRegex(gate.code_layout.LayoutError, "no ELF caller"):
                    gate.hot_functions(self.root, self.root / "launcher/main", link)

    def test_main_statuses_and_deduplication(self):
        source = self.write("launcher/main/example.c", "/* Library call: A deliberate copy. */\ncopy();\n")
        header = "C:/checkout/inline.h"
        elf = self.write("build/launcher.elf", "")
        script = self.write(f"{IDF_DIR}/components/esp_rom/esp32s3/ld/esp32s3.rom.libc.ld",
                            "memcpy = 0x400011e8;\n")
        link = dict(elf=elf, metadata={"idf_path": str(script.parents[4]), "target": "esp32s3"},
                    maptext="", ninja="build launcher.elf: LINK rule\n  LINK_FLAGS = -T esp32s3.rom.libc.ld\n")
        callers = [gate.code_layout.Symbol(0x42000020, name, 8, "t") for name in ("hot", "alias")]
        hot = {caller: [(0x42000020, 0x42000028, "inlined")] for caller in callers}
        dump = "42000020: 000025 call8 400011e8 <memcpy>\n"
        for status, frames, message in (
            (0, [f"{header}:12", f"{source}:2 (discriminator 4)", f"{source}:3"],
             "no unannotated C-runtime calls in 2 hot functions"),
            (1, [f"{header}:12", f"{source}:3"], "inlined calls memcpy at 0x42000020"),
            (2, [], "incomplete source map"),
            (2, [f"{source}:2"], "tool failed"),
        ):
            with self.subTest(status=status, message=message):
                output = io.StringIO()

                def tool(name, *args, **kwargs):
                    if name == "objdump":
                        return SimpleNamespace(stdout=dump)
                    self.assertEqual(name, "addr2line")
                    self.assertIn("-a", args)
                    self.assertIn("-i", args)
                    addresses = kwargs.get("input", "").splitlines() or [arg for arg in args if arg.startswith("0x")]
                    self.assertEqual(addresses, ["0x42000020"])
                    if message == "tool failed":
                        raise subprocess.CalledProcessError(1, name, stderr=message)
                    return SimpleNamespace(stdout="\n".join(["0x42000020", *frames]) + "\n")

                with patch.object(gate.code_layout, "REPO", self.root), \
                        patch.object(gate.code_layout, "find_tool", side_effect=lambda name: name), \
                        patch.object(gate.memory_map, "read_link", return_value=link), \
                        patch.object(gate, "hot_functions", return_value=hot), \
                        patch.object(gate, "executable_sections", return_value=CODE_SECTIONS), \
                        patch.object(gate, "comment_sources", return_value=[source]), \
                        patch.object(subprocess, "run", side_effect=lambda args, **kwargs: tool(*args, **kwargs)), \
                        contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                    self.assertEqual(gate.main([str(elf.parent)]), status)
                self.assertIn(message, output.getvalue())
                if status == 1:
                    self.assertEqual(output.getvalue().count("inlined calls memcpy"), 1)

    def test_runtime_archive_names_rom_provide_and_range_end(self):
        scripts = IDF_DIR + "/components/esp_rom/esp32s3/ld/"
        self.write(scripts + "esp32s3.rom.libgcc.ld", "PROVIDE ( __divsi3 = 0x400011e8 );\n")
        ninja = 'build launcher.elf: LINK rule\n  LINK_FLAGS = -T "C:/IDF with spaces/esp32s3.rom.libgcc.ld"\n'
        maptext = """.flash.text 0x42000200 0x40
 .text.floorf 0x42000200 0x10 toolchain/libm.a(floor.o)
 .text.copy 0x42000220 0x10 toolchain/liblibc.a(copy.o)
 .text.clear 0x42000230 0x10 toolchain/mylibc.a(clear.o)
"""
        runtime = gate.runtime_ranges(maptext, ninja, self.root / IDF_DIR, "esp32s3", CODE_SECTIONS)
        for address, expected in ((0x42000200, True), (0x4200020f, True), (0x42000210, False),
                                  (0x42000220, False), (0x42000230, False), (0x400011e8, True)):
            with self.subTest(address=hex(address)):
                self.assertEqual(runtime(address), expected)

    def test_note_stops_at_brace_blank_eof_or_end_of_trailing_comment(self):
        name = "launcher/main/example.c"
        path = self.write(name, """// Library call: A statement ending at a brace.
start(
    first(),
    second()
}
next();
// Library call: A blank line follows.

before_next()
still_next();
copy(); // Library call: Only this line.
next();
// Library call: A statement without a terminator.
begin(
    next()""")
        lines = (3, 5, 6, 8, 9, 10, 11, 12, 14, 15)
        calls = [("hot", "memcpy", line) for line in lines]
        self.assertEqual(gate.violations(self.root, calls, [f"{path}:{line}" for line in lines]), sorted([
            f"{name}:{line}: hot calls memcpy at {line:#x}" for line in (6, 8, 9, 10, 12, 15)
        ] + [f"{name}:7: Library call: annotation covers no C-runtime call"]))

    def test_windows_source_location_keeps_the_drive_and_ignores_discriminator(self):
        call = ("hot", "memcpy", 0x42000020)
        with patch.object(gate, "annotations", return_value=({}, {})):
            bad = gate.violations(self.root, [call], ["C:/checkout/example.c:18 (discriminator 2)"])
        self.assertIn("C:/checkout/example.c:18: hot calls memcpy", bad[0])

    def test_map_input_addresses_and_objdump_symbol_offsets(self):
        maptext = """.iram0.text 0x40380000 0x24
 .iram1.0 0x40380000 0x8 main/libmain.a(example.c.obj)
 .iram1.1 0x40380008 0x10 main/libmain.a(example.c.obj)
 .text.direct 0x40380018 0xc main/libmain.a(example.c.obj)
"""
        symbols = {0x40380004: ("single", 4), 0x40380008: ("first", 4), 0x40380010: ("second", 4)}
        inputs, _, _ = gate.memory_map.map_inputs(maptext, symbols)
        self.assertEqual([(item.symbol, item.address) for item in inputs],
                         [("single", 0x40380004), ("first", 0x40380008),
                          ("second", 0x40380010), ("direct", 0x40380018)])
        self.assertEqual([gate.symbol_base(name) for name in ("copy+0x12", "copy-0xa", "copy")], ["copy"] * 3)
