"""Release hot-call checks on disassembly, map, linker and source text fixtures."""

import pathlib
import sys
import tempfile
import unittest
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
4200003c: ffff71 l32r a7, 4200001c <hot-0x4> (7fff <absolute+0x7eff>)
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
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = pathlib.Path(temp.name)

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
        decoded = gate.code_layout.skip_pools(gate.code_layout.parse_objdump(DUMP))
        self.assertEqual(
            [(address, name) for address, target, name in gate.callees(decoded) if runtime(target)],
            [(0x42000020, "floorf"), (0x42000026, "memset"), (0x42000036, "divide"),
             (0x42000039, "memset")],
        )

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
        records = [gate.code_layout.Symbol(address, name, 4, "t") for address, name in
                   [(0x42000020, "pin"), (0x42000024, "row"), (0x40380000, "attr"),
                    (0x40380004, "cloned$constprop$0")]]
        map_text = """.iram0.text 0x40380000 0x8
 .iram1.0 0x40380000 0x4 main/libmain.a(example.c.obj)
 .iram1.1 0x40380004 0x4 main/libmain.a(example.c.obj)
"""
        with patch.object(gate.memory_map, "build_sites", return_value=([site, fragment], lambda _: True)):
            hot = gate.hot_functions(self.root, sources, {}, "CONFIG_LAUNCHER_RELEASE=y\n", records, map_text)
            self.assertEqual(set(records), set(hot))
            with self.assertRaisesRegex(gate.code_layout.LayoutError, "cloned"):
                gate.hot_functions(self.root, sources, {}, "CONFIG_LAUNCHER_RELEASE=y\n", records[:-1], map_text)
            with self.assertRaisesRegex(gate.code_layout.LayoutError, "row"):
                gate.hot_functions(self.root, sources, {}, "CONFIG_LAUNCHER_RELEASE=y\n", records[:1], map_text)

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
        self.assertIn(f"{name}:6: hot calls memset at 0x42000026", bad)
        self.assertIn(f"{name}:9: hot calls memcpy at 0x42000029", bad)
        self.assertIn("??:0: hot calls memcpy at 0x4200002c", bad)
        self.assertIn("??:0: hot calls memcpy at 0x42000030", bad)
        self.assertEqual(8, len(bad))
        for location in (f"{name}:6", f"{name}:7", f"{name}:10", f"{header}:1"):
            self.assertTrue(any(line.startswith(location + ":") and "Library call:" in line for line in bad))
