"""Small source and map fixtures for the internal-memory gate."""
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build"))
import memory_map as memory
from tool_support import markdown
import iram_what_if as probe
import json
import subprocess
import os
import copy
import shutil


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

    def check(self, placements, inputs, config=None, active=None):
        return memory.check(placements, inputs, config or {}, memory.components(self.root), active or (lambda site: True))

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
        with self.assertRaisesRegex(ValueError, "set the budget"):
            self.check(sites, [memory.Input("libmain.a", "foo.c.obj", "foo", 8)], {})

    def test_unowned_symbol(self):
        with self.assertRaisesRegex(ValueError, "no site"):
            self.check([], [memory.Input("libmain.a", "foo.c.obj", "foo", 8)], {})

    def test_mapping_covers_entries(self):
        sites = self.source("# Cache latency. IRAM budget: 12 B\n[mapping:hot]\narchive: libmain.a\nentries:\n    foo:foo (noflash)\n    bar:bar (noflash_text)\n", "launcher/main/linker.lf")
        report = self.check(sites, [memory.Input("libmain.a", "foo.c.obj", "foo", 4),
                                     memory.Input("libmain.a", "bar.c.obj", "bar", 8)], {})
        self.assertEqual(report[0][1], 12)

    def test_app_owner(self):
        sites = self.source("/* Cache-off caller. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}",
                            "launcher/main/apps/demo/foo.c")
        self.assertEqual(sites[0].owner, "launcher/main/apps/demo/")

    def test_exact_budget(self):
        sites = self.source("/* Cache-off caller. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}")
        for size in (3, 4, 5):
            with self.subTest(size=size):
                inputs = [memory.Input("libmain.a", "foo.c.obj", "foo", size)]
                if size == 4:
                    self.assertEqual(self.check(sites, inputs)[0][1], size)
                else:
                    with self.assertRaisesRegex(ValueError, f"set the budget to {size} B"):
                        self.check(sites, inputs)

    def test_alignment(self):
        for granularity, prefetch, end, expected in ((64, 0, 65, 63), (256, 16, 255, 257)):
            script = f".iram0.text_end : {{ . += {prefetch}; . = ALIGN({granularity}); }}"
            self.assertEqual(memory.rounding(script, end), (granularity, prefetch, expected))
        self.assertEqual(memory.rounding("a = 64; p = 16; .iram0.text_end : { . += p; . = ALIGN(a); }", 63), (64, 16, 65))

    def test_markdown_escaping(self):
        self.assertEqual(markdown(["site", "reason"], [("a|b", "line\nbreak")]),
                         "| site | reason |\n|---|---|\n| a\\|b | line break |\n")

    def test_literal_pool_belongs_to_attribute_function(self):
        text = ".iram0.text 0x40380000 0x20\n .iram1.0.literal\n  0x40380000 0x4 main/libmain.a(foo.c.obj)\n .iram1.0\n  0x40380004 0x8 main/libmain.a(foo.c.obj)\n"
        inputs, _, _ = memory.map_inputs(text, {0x40380004: ("foo", 8)})
        self.assertEqual([(item.symbol, item.size) for item in inputs], [("foo", 8), ("foo", 4)])

    def test_wrapped_output_section(self):
        _, sections, _ = memory.map_inputs(".iram0.text_end\n  0x40380101 0xff\n", {})
        self.assertEqual(sections[".iram0.text_end"], (0x40380101, 255))

    def build(self, config, size):
        placements = self.source("/* Cache-off caller. IRAM budget: release 4 B, dev 8 B, diag 8 B */\nstatic void IRAM_ATTR foo(void) {}", "launcher/main/apps/demo/nested/foo.c")
        inputs = [memory.Input("libmain.a", "foo.c.obj", "foo", size)]
        return dict(config=config, inputs=inputs, owned=memory.components(self.root), report=self.check(placements, inputs, config),
                    sections={".iram0.text": (0x40380000, size), ".dram0.data": (0x3fc80100, 16), ".dram0.bss": (0x3fc80110, 16)},
                    align=256, pad=16, slack=256-size, constants={"_heap_start": 0x3fc80120},
                    regions={"iram0_0_seg": (0x40380000, 1024), "dram0_0_seg": (0x3fc80000, 1024)},
                    size={"layout": [{"name": "DIRAM", "used": size+32}]})

    def test_generated_block_stable(self):
        release, dev = self.build({"CONFIG_LAUNCHER_RELEASE": "y"}, 4), self.build({"CONFIG_LAUNCHER_DEVELOPMENT": "y"}, 8)
        body = memory.render([dev, release], [])
        self.assertEqual(body, memory.render([release, dev], []))
        self.assertIn("| Slack including pad | 252 | 248 |", body)
        self.assertIn("| launcher/main/apps/demo/ |", body)
        self.assertIn("| 4 | 8 | release 4, dev 8, diag 8 | Cache-off caller. |", body)
        self.assertIn("| libmain.a | 4 | 8 |", body)
        self.assertNotIn("ESP-IDF IRAM options", body)
        fixture = self.root / "fixture.json"
        builds = [release, dev]
        for build in builds:
            build["inputs"] = [vars(item) for item in build["inputs"]]
            build["report"] = [(vars(site), size, names) for site, size, names in build["report"]]
            build["owned"] = list(build["owned"])
        fixture.write_text(json.dumps(builds))
        program = """import json, sys
sys.path.insert(0, sys.argv[1])
import memory_map as m
builds=json.load(open(sys.argv[2]))
for b in builds:
 b['inputs']=[m.Input(**i) for i in b['inputs']]
 b['report']=[(m.Site(**s),size,names) for s,size,names in b['report']]
print(m.render(builds, []))
"""
        outputs = [subprocess.check_output([sys.executable, "-c", program, str(memory.ROOT / "launcher/tools/build"), str(fixture)], env=dict(os.environ, PYTHONHASHSEED=seed)) for seed in ("1", "53")]
        self.assertEqual(outputs[0], outputs[1])

    def test_documentation_needs_release_build(self):
        build = self.build({"CONFIG_LAUNCHER_DEVELOPMENT": "y"}, 8)
        with self.assertRaisesRegex(ValueError, "release build"):
            memory.render([build], [])

    def test_render_app_and_engine(self):
        build = self.build({"CONFIG_LAUNCHER_RELEASE": "y"}, 4)
        sites = self.source("/* Interrupt latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR bar(void) {}", "launcher/main/bar.c")
        build["inputs"].append(memory.Input("libmain.a", "bar.c.obj", "bar", 4))
        build["report"] = self.check(sites, build["inputs"], build["config"])
        body = memory.render([build], [])
        self.assertIn("| engine | launcher/main/bar.c:2 | bar | 4 | 4 | Interrupt latency. |", body)
        self.assertIn("| launcher/main/apps/demo/ | launcher/main/apps/demo/nested/foo.c:2 | foo | 4 |", body)

    def test_fresh_scratch_snapshot(self):
        import tool_support
        source = self.root / "source"
        source.mkdir()
        (source / "file.c").write_text("first")
        work = self.root / "work"
        with patch.object(tool_support, "tracked_files", return_value=["file.c"]):
            tree = tool_support.scratch_tree(work, source)
            (tree / "obsolete").write_text("old")
            (source / "file.c").write_text("current")
            with self.assertRaisesRegex(ValueError, "already exists"):
                tool_support.scratch_tree(work, source)
            tool_support.scratch_tree(work, source, fresh=True)
        self.assertEqual((tree / "file.c").read_text(), "current")
        self.assertFalse((tree / "obsolete").exists())

    def test_ownership_metadata(self):
        names = ["main", "esp32_s3_touch_amoled_1_8", "math", "managed"]
        paths = [self.root / "launcher/main", self.root / "launcher/components/esp32_s3_touch_amoled_1_8", self.root / "launcher/packages/math", self.root / "launcher/managed_components/managed"]
        metadata = {"build_components": names, "build_component_paths": list(map(str, paths))}
        (self.root / "project_description.json").write_text(json.dumps(metadata))
        owned = memory.components(self.root, json.loads((self.root / "project_description.json").read_text()))
        self.assertEqual(set(owned), {"libmain.a", "libmath.a", "libesp32_s3_touch_amoled_1_8.a"})
        for archive in owned:
            with self.assertRaisesRegex(ValueError, "no site"):
                memory.check([], [memory.Input(archive, "foo.c.obj", "foo", 1)], {}, owned, lambda site: True)
            self.assertEqual(memory.check([], [memory.Input(archive, "foo.c.obj", "foo", 0)], {}, owned, lambda site: True), [])
        self.assertEqual(memory.check([], [memory.Input("libmanaged.a", "foo.c.obj", "foo", 1)], {}, owned, lambda site: True), [])

    def test_archive_and_object_must_match(self):
        sites = self.source("/* Cache-off caller. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}")
        for item in (memory.Input("libmain.a", "other.c.obj", "foo", 4), memory.Input("libmath.a", "foo.c.obj", "foo", 4)):
            with self.assertRaisesRegex(ValueError, "no site"):
                memory.check(sites, [item], {}, {"libmain.a", "libmath.a"}, lambda site: True)

    def test_comment_must_be_adjacent(self):
        for gap in ("\n", "int other;\n"):
            for name, content in (("launcher/main/foo.c", "/* Cache-off caller. IRAM budget: 4 B */\n"+gap+"static void IRAM_ATTR foo(void) {}"),
                                  ("launcher/main/linker.lf", "# Cache latency. IRAM budget: 4 B\n"+gap+"[mapping:hot]\narchive: libmain.a\nentries:\n    foo:foo (noflash)")):
                with self.subTest(name=name, gap=gap), self.assertRaisesRegex(ValueError, "reason"):
                    self.source(content, name)
                (self.root / name).unlink()

    def test_reason_is_a_sentence(self):
        for reason in ("IRAM budget: 4 B", "TODO. IRAM budget: 4 B", "Cache latency IRAM budget: 4 B"):
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, "reason"):
                self.source(f"/* {reason} */\nstatic void IRAM_ATTR foo(void) {{}}")
        (self.root / "launcher/main/foo.c").unlink()
        for comment in ("", "# TODO.\n"):
            with self.assertRaisesRegex(ValueError, "reason"):
                self.source(comment+"CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=n", "launcher/sdkconfig.defaults")
        self.assertIsNone(self.source("# Cache latency.\nCONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=n", "launcher/sdkconfig.defaults")[0].budget)

    def test_attribute_wins(self):
        attr = self.source("/* Cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}")[0]
        mapping = memory.Site("linker.lf", 1, "engine", "Cache latency.", 8, "libmain.a", [("*", "*")])
        report = self.check([mapping, attr], [memory.Input("libmain.a", "foo.c.obj", "foo", 4), memory.Input("libmain.a", "bar.c.obj", "bar", 8)])
        self.assertEqual([record[1] for record in report], [8, 4])

    def test_budget_parse(self):
        self.assertEqual(memory.parse_budget("Cache latency. IRAM budget: release 4 B, dev 8 B, diag 8 B"), {"release": 4, "dev": 8, "diag": 8})
        self.assertEqual(memory.parse_budget("Cache latency. IRAM budget: 4 B"), 4)
        self.assertIsNone(memory.parse_budget("Cache latency."))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            memory.parse_budget("IRAM budget: release 4 B, release 8 B")

    def test_zero_budget_is_not_missing(self):
        sites = self.source("/* Cache latency. IRAM budget: 0 B */\nstatic void IRAM_ATTR foo(void) {}")
        self.assertEqual(sites[0].budget, 0)
        self.assertEqual(self.check(sites, [], active=lambda site: False)[0][1], 0)
        with self.assertRaisesRegex(ValueError, "placed nothing"):
            self.check(sites, [])

    def test_symbol_reader_preserves_duplicate_names(self):
        import code_layout
        with patch.object(code_layout, "run", return_value="40380000 00000004 t same\n40380004 00000008 t same\n4038000c 00000004 t other\n"):
            self.assertEqual(code_layout.symbols("nm", "elf"), {0x40380000: ("same", 4), 0x40380004: ("same", 8), 0x4038000c: ("other", 4)})
            self.assertEqual(len(code_layout.symbols("nm", "elf", ["same"])), 2)

    def test_check_cli_reports_app_and_engine_without_rendering(self):
        import contextlib
        import io
        app = memory.Site("launcher/main/apps/demo/nested/foo.c", 1, "launcher/main/apps/demo/", "Cache latency.", 4, "libmain.a", [("foo", "foo")])
        engine = memory.Site("launcher/main/bar.c", 1, "engine", "Cache latency.", 8, "libmain.a", [("bar", "bar")])
        build = {"config": {"CONFIG_LAUNCHER_DEVELOPMENT": "y"}, "owned": {"libmain.a"}, "inputs": [memory.Input("libmain.a", "foo.c.obj", "foo", 4), memory.Input("libmain.a", "bar.c.obj", "bar", 8)]}
        output = io.StringIO()
        with patch.object(sys, "argv", ["memory_map.py", "--check", "fixture"]), patch.object(memory, "read_build", return_value=build), patch.object(memory, "build_sites", return_value=([app, engine], lambda site: True)), patch.object(memory, "render", side_effect=AssertionError("check rendered tables")), contextlib.redirect_stdout(output):
            self.assertEqual(memory.main(), 0)
        self.assertIn("dev: launcher/main/apps/demo/", output.getvalue())
        self.assertIn("dev: engine", output.getvalue())

    def test_layout_error_is_one_line(self):
        import contextlib
        import io
        output = io.StringIO()
        with patch.object(sys, "argv", ["memory_map.py", "--check", "fixture"]), patch.object(memory, "read_build", side_effect=memory.LayoutError("missing nm")), contextlib.redirect_stderr(output):
            self.assertEqual(memory.main(), 1)
        self.assertEqual(output.getvalue(), "FAIL IRAM: missing nm\n")

    def test_map_function_sections_and_duplicate_names(self):
        text = ".iram0.text 0x40380000 0x20\n .text.foo 0x40380000 0x4 main/libmain.a(foo.c.obj)\n .literal.foo 0x40380004 0x4 main/libmain.a(foo.c.obj)\n .iram1.0 0x40380008 0x10 main/libmain.a(bar.c.obj)\n"
        inputs, _, _ = memory.map_inputs(text, {0x40380008:("same",4),0x4038000c:("same",8)})
        self.assertEqual([(i.symbol,i.size) for i in inputs], [("foo",4),("foo",4),("same",8),("same",8)])

    def test_non_iram_mapping(self):
        self.assertEqual(self.source("[mapping:patch]\narchive: libmain.a\nentries:\n    * (patchable_entries)", "launcher/main/linker.lf"), [])

    def test_preprocessor_guards(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if compiler is None:
            compiler = memory.find_tool("gcc")
        source = self.root / "launcher/main/foo.c"
        source.parent.mkdir(parents=True)
        source.write_text("#define IRAM_ATTR __attribute__((section(\".iram1\")))\n#ifndef CONFIG_FEATURE\n/* Cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}\n#elif CONFIG_OTHER\n/* Cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR bar(void) {}\n#else\n/* Cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR baz(void) {}\n#endif\n")
        for defines, expected in (([],"foo"),(["-DCONFIG_FEATURE=1", "-DCONFIG_OTHER=1"],"bar"),(["-DCONFIG_FEATURE=1"],"baz")):
            command = {"file":str(source), "directory":str(self.root), "arguments":[compiler,*defines,"-c",str(source),"-o","unused.o"]}
            (self.root / "compile_commands.json").write_text(json.dumps([command]))
            with patch.object(memory,"ROOT",self.root):
                alive=memory.preprocessed_lines(self.root)
            sites=memory.sites(self.root)
            present=[s.entries[0][1] for s in sites if (source,s.line) in alive]
            self.assertEqual(present,[expected])
            active=lambda site:(source,site.line) in alive
            inputs=[memory.Input("libmain.a","foo.c.obj",expected,4)]
            self.assertEqual([size for _,size,_ in self.check(sites,inputs,active=active)].count(4),1)

    def test_fragment_conditionals(self):
        sites=self.source("# Cache latency. IRAM budget: 4 B\n[mapping:hot]\narchive: libmain.a\nentries:\n    if FEATURE = y:\n        foo:foo (noflash)\n    if FEATURE = n:\n        bar:bar (noflash)\n", "launcher/main/linker.lf")
        self.assertEqual(memory.fragment_entries(sites[0],self.root,{}),[("bar","bar")])
        self.assertEqual(memory.fragment_entries(sites[0],self.root,{"CONFIG_FEATURE":"y"}),[("foo","foo")])

    def test_nested_fragment_branches(self):
        sites = self.source("# Cache latency. IRAM budget: 4 B\n[mapping:hot]\narchive: libmain.a\nentries:\n    if FEATURE = y:\n        if OTHER = y:\n            foo:foo (noflash)\n        else:\n            bar:bar (noflash)\n    elif ALTERNATIVE = y:\n        baz:baz (noflash)\n    else:\n        fallback:fallback (noflash)\n", "launcher/main/linker.lf")
        for config, symbol in (({}, "fallback"), ({"CONFIG_ALTERNATIVE": "y"}, "baz"), ({"CONFIG_FEATURE": "y"}, "bar"), ({"CONFIG_FEATURE": "y", "CONFIG_OTHER": "y"}, "foo")):
            self.assertEqual(memory.fragment_entries(sites[0], self.root, config), [(symbol, symbol)])

    def test_fragment_conditions_in_build_gate(self):
        self.source("# Cache latency. IRAM budget: 4 B\n[mapping:hot]\narchive: libmain.a\nentries:\n    if FEATURE = y:\n        foo:foo (noflash)\n", "launcher/main/linker.lf")
        (self.root / "compile_commands.json").write_text("[]")
        for config, inputs, expected in (({}, [], 0), ({"CONFIG_FEATURE": "y"}, [memory.Input("libmain.a", "foo.c.obj", "foo", 4)], 4)):
            build = {"build": self.root, "config": config, "owned": memory.components(self.root)}
            with patch.object(memory, "ROOT", self.root):
                placements, active = memory.build_sites(self.root, build)
            self.assertEqual(memory.check(placements, inputs, config, build["owned"], active)[0][1], expected)

    def test_variant_label(self):
        for config, label in (({"CONFIG_LAUNCHER_RELEASE": "y"}, "release"), ({"CONFIG_LAUNCHER_DEVELOPMENT": "y"}, "dev"), ({"CONFIG_LAUNCHER_SELFTEST": "y", "CONFIG_LAUNCHER_DEVELOPMENT": "y"}, "diag")):
            self.assertEqual(memory.variant_label(config), label)
        with self.assertRaisesRegex(ValueError, "no launcher variant"):
            memory.variant_label({})

    def test_variant_budget_missing(self):
        site=memory.Site("foo.c",1,"engine","Cache latency.",{"release":4},"libmain.a",[("foo","foo")])
        with self.assertRaisesRegex(ValueError,"missing dev"):
            self.check([site],[memory.Input("libmain.a","foo.c.obj","foo",4)],{"CONFIG_LAUNCHER_DEVELOPMENT":"y"})

    def test_probe_selection_and_identity(self):
        self.assertIn("silent-assertions",probe.experiments("release"))
        self.assertNotIn("silent-assertions",probe.experiments("dev"))
        self.assertEqual(probe.experiments("diag",["ringbuf-to-flash","i2c-isr"]),["ringbuf-to-flash"])
        with self.assertRaisesRegex(ValueError,"unknown"):
            probe.experiments("release",["unknown"])
        results={}
        probe.store_result(results,"release:a","id-a",["release","a",4])
        probe.store_result(results,"dev:a","id-b",["dev","a",4])
        with self.assertRaisesRegex(ValueError,"different build id"):
            probe.store_result(results,"release:b","id-c",["release","b",8])
        self.assertNotIn("release:b",results)
        self.assertIn("id-a",probe.render_results(results))

    def test_probe_jobs_must_be_positive(self):
        import contextlib
        import io
        with patch.object(sys, "argv", ["iram_what_if.py", "fixture", "--work", str(self.root), "--jobs", "0"]), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            probe.main()
        self.assertEqual(error.exception.code, 2)

    def test_cli_exit_status(self):
        script=str(memory.ROOT / "launcher/tools/build/memory_map.py")
        for content, expected in (("static void IRAM_ATTR foo(void) {}",1),("/* Cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}",0)):
            path=self.root / "launcher/main/foo.c"
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_text(content)
            done=subprocess.run([sys.executable,script,"--source-only","--root",str(self.root)],capture_output=True,text=True)
            self.assertEqual(done.returncode,expected,done.stderr)



if __name__ == "__main__":
    unittest.main()
