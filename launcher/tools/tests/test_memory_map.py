"""Small source and map fixtures for the internal-memory gate."""
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build"))
import memory_map as memory


class MemoryMapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def source(self, text, name="launcher/main/foo.c"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return memory.sites(self.root)

    def test_missing_comment(self):
        with self.assertRaisesRegex(ValueError, "reason"):
            self.source("static void IRAM_ATTR foo(void) {}")

    def test_missing_budget(self):
        with self.assertRaisesRegex(ValueError, "budget"):
            self.source("/* Cache-off caller. */\nstatic void IRAM_ATTR foo(void) {}")

    def test_attribute_alias(self):
        found = self.source("#define HOT_CODE IRAM_ATTR\n/* Cache-off caller. IRAM budget: 4 B */\nstatic void HOT_CODE foo(void) {}")
        self.assertEqual(found[0].entries, [("foo", "foo")])

    def test_over_budget(self):
        sites = self.source("/* Cache-off caller. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}")
        with self.assertRaisesRegex(ValueError, "exceeds"):
            memory.check(sites, [memory.Input("libmain.a", "foo.c.obj", "foo", 8)], {})

    def test_unowned_symbol(self):
        with self.assertRaisesRegex(ValueError, "no site"):
            memory.check([], [memory.Input("libmain.a", "foo.c.obj", "foo", 8)], {})

    def test_mapping_covers_entries(self):
        sites = self.source("# Cache latency. IRAM budget: 12 B\n[mapping:hot]\narchive: libmain.a\nentries:\n    foo:foo (noflash)\n    bar:bar (noflash_text)\n", "launcher/main/linker.lf")
        report = memory.check(sites, [memory.Input("libmain.a", "foo.c.obj", "foo", 4),
                                     memory.Input("libmain.a", "bar.c.obj", "bar", 8)], {})
        self.assertEqual(report[0][1], 12)

    def test_app_owner(self):
        sites = self.source("/* Cache-off caller. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}",
                            "launcher/main/apps/demo/foo.c")
        self.assertEqual(sites[0].owner, "launcher/main/apps/demo/")

    def test_disabled_site(self):
        sites = self.source("#if CONFIG_FEATURE\n/* Cache-off caller. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}\n#endif")
        self.assertEqual(memory.check(sites, [], {"CONFIG_FEATURE": "n"})[0][1], 0)
        with self.assertRaisesRegex(ValueError, "placed nothing"):
            memory.check(sites, [], {"CONFIG_FEATURE": "y"})

    def test_alignment(self):
        script = "_esp_memprot_align_size = 0x100;\n_esp_memprot_prefetch_pad_size = 0x10;\n. = ALIGN(4);\n.iram0.text_end : { . += _esp_memprot_prefetch_pad_size; . = ALIGN(_esp_memprot_align_size); }"
        self.assertEqual(memory.rounding(script, 0x40380101), (256, 16, 255))

    def test_else_compiled_out(self):
        sites = self.source("#if CONFIG_FEATURE\n#else\n/* Cache-off caller. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}\n#endif")
        self.assertEqual(memory.check(sites, [], {"CONFIG_FEATURE": "y"})[0][1], 0)

    def test_negated_guard_compiled_out(self):
        sites = self.source("#if !CONFIG_FEATURE\n/* Cache-off caller. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}\n#endif")
        self.assertEqual(memory.check(sites, [], {"CONFIG_FEATURE": "y"})[0][1], 0)

    def test_stable_table(self):
        rows = [("a", 2, "reason"), ("b", 3, "reason")]
        self.assertEqual(memory.table(["site", "bytes", "reason"], rows),
                         memory.table(["site", "bytes", "reason"], rows))

    def test_literal_pool_belongs_to_attribute_function(self):
        text = ".iram0.text 0x40380000 0x20\n .iram1.0.literal\n  0x40380000 0x4 main/libmain.a(foo.c.obj)\n .iram1.0\n  0x40380004 0x8 main/libmain.a(foo.c.obj)\n"
        inputs, _, _ = memory.map_inputs(text, {"foo": (0x40380004, 8)})
        self.assertEqual([(item.symbol, item.size) for item in inputs], [("foo", 8), ("foo", 4)])

    def test_wrapped_output_section(self):
        _, sections, _ = memory.map_inputs(".iram0.text_end\n  0x40380101 0xff\n", {})
        self.assertEqual(sections[".iram0.text_end"], (0x40380101, 255))

    def test_generated_block_stable(self):
        build = dict(config={}, inputs=[], sections={".iram0.vectors": (0x40380000, 16),
                     ".iram0.text": (0x40380010, 240), ".dram0.data": (0x3fc80100, 16),
                     ".dram0.bss": (0x3fc80110, 16)}, align=256, pad=0, slack=0,
                     constants={"_heap_start": 0x3fc80120}, regions={"iram0_0_seg": (0x40380000, 1024),
                     "dram0_0_seg": (0x3fc80000, 1024)}, size={"layout": [{"name": "DIRAM", "used": 288}]})
        with patch.object(memory, "kconfig_options", return_value={}):
            self.assertEqual(memory.render([build], []), memory.render([build], []))


if __name__ == "__main__":
    unittest.main()
