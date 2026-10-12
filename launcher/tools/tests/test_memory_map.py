"""Small source and map fixtures for the internal-memory gate."""
import sys
import contextlib
import io
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build"))
import memory_map as memory
import compile_db
import espressif
from doc_tables import markdown
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
        (self.root / "launcher/main").mkdir(parents=True)
        (self.root / "launcher/CMakeLists.txt").write_text('set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/packages")\n')

    def source(self, text, name="launcher/main/foo.c"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return memory.sites(self.root)

    def check(self, placements, inputs, config=None, active=None, variant="release"):
        config = config or {}
        for site in placements:
            if site.path.endswith(".lf") and isinstance(site.entries, bool):
                site.entries = memory.fragment_entries(site, self.root, config)
        return memory.check(placements, inputs, variant, memory.components(self.root), active or (lambda site: True))

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

    def build(self, config, size, variant="release"):
        placements = self.source("/* Cache-off caller. IRAM budget: release 4 B, dev 8 B, diag 8 B */\nstatic void IRAM_ATTR foo(void) {}", "launcher/main/apps/demo/nested/foo.c")
        inputs = [memory.Input("libmain.a", "foo.c.obj", "foo", size)]
        return dict(variant=variant, build_id="abcdef123456-" + variant, idf_commit="fixture-idf", config=config, inputs=inputs, owned=memory.components(self.root), report=self.check(placements, inputs, config, variant=variant),
                    sections={".iram0.text": (0x40380000, size), ".dram0.data": (0x3fc80100, 16), ".dram0.bss": (0x3fc80110, 16)},
                    align=256, pad=16, slack=256-size, constants={"_heap_start": 0x3fc80120},
                    regions={"iram0_0_seg": (0x40380000, 1024), "dram0_0_seg": (0x3fc80000, 1024)},
                    size={"layout": [{"name": "DIRAM", "used": size+32}]})

    def test_generated_block_stable(self):
        release, dev = self.build({"CONFIG_LAUNCHER_RELEASE": "y"}, 4), self.build({"CONFIG_LAUNCHER_DEVELOPMENT": "y"}, 8, variant="dev")
        second = memory.Site("launcher/main/bar.c", 2, "engine", "Interrupt latency.", {"release": 4, "dev": 8}, "libmain.a", [("bar", "bar")])
        for build, count in ((release, 4), (dev, 8)):
            build["inputs"] += [memory.Input("libmain.a", "bar.c.obj", "bar", count), memory.Input("libfixed.a", "same.c.obj", "same", 4)]
            build["report"].append((second, count, ["bar"]))
        extra = memory.Site("launcher/main/extra.c", 2, "engine", "Development latency.", 4, "libmain.a", [("extra", "extra")])
        dev["inputs"] += [memory.Input("libmain.a", "extra.c.obj", "extra", 4), memory.Input("libextra.a", "data.c.obj", "literal_only", 4, True)]
        dev["report"].append((extra, 4, ["extra"]))
        body = memory.render([dev, release])
        self.assertEqual(body, memory.render([release, dev]))
        self.assertIn("| Slack including pad | 252 | 248 |", body)
        self.assertIn("| launcher/main/apps/demo/ |", body)
        self.assertIn("| 4 | 8 | release 4, dev 8, diag 8 | Cache-off caller. |", body)
        self.assertIn("| libmain.a | our sites | 8 | 20 |", body)
        self.assertIn("| libfixed.a | IDF | 4 | 4 |", body)
        self.assertIn("| Linker fill / split slack | unattributed | -8 | -20 |", body)
        self.assertIn("| extra | not built | 4 |", body)
        differs = body.split(memory.DIFFERENCES_HEADING)[1]
        self.assertNotIn("libfixed.a", differs)
        self.assertNotIn("literal_only", differs)
        self.assertIn("extra.c.obj:extra (4 B)", differs)
        row = next(line for line in differs.splitlines() if line.startswith("| libmain.a |"))
        self.assertEqual(row.split("|")[-3:-1], ["  ", " extra.c.obj:extra (4 B) "])
        self.assertIn("Built on IDF fixture-idf; launcher/idf-pin.json pins " + memory.idf_pin()["commit"][:12], body)
        with patch.object(memory, "idf_pin", return_value={"commit": "fixture-idf"}):
            self.assertNotIn("measured bytes can differ", memory.render([release, dev]))
        self.assertEqual(body.count("abcdef123456-release"), 1)
        self.assertIn("IDF fixture-idf", body)
        self.assertIn("\u2013", body)
        self.assertNotIn('```mermaid', body)
        release["sections"][".iram0.text_end"] = (0, 100)
        self.assertNotIn('```mermaid', memory.render([release, dev]))
        del release["sections"][".iram0.text_end"]
        for build in (release, dev):
            build["inputs"].reverse()
            build["report"].reverse()
        self.assertEqual(body, memory.render([dev, release]))
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
print(m.render(builds))
"""
        outputs = [subprocess.check_output([sys.executable, "-c", program, str(memory.ROOT / "launcher/tools/build"), str(fixture)], env=dict(os.environ, PYTHONHASHSEED=seed)) for seed in ("1", "53")]
        self.assertEqual(outputs[0], outputs[1])

    def test_documentation_needs_release_build(self):
        build = self.build({"CONFIG_LAUNCHER_DEVELOPMENT": "y"}, 8, variant="dev")
        with self.assertRaisesRegex(ValueError, "release build"):
            memory.render([build])

    def test_render_app_and_engine(self):
        build = self.build({"CONFIG_LAUNCHER_RELEASE": "y"}, 4)
        sites = self.source("/* Interrupt latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR bar(void) {}", "launcher/main/bar.c")
        build["inputs"].append(memory.Input("libmain.a", "bar.c.obj", "bar", 4))
        build["report"] = self.check(sites, build["inputs"], build["config"])
        body = memory.render([build])
        self.assertIn("| engine | launcher/main/bar.c:2 | bar | 4 | 4 | Interrupt latency. |", body)
        self.assertIn("| launcher/main/apps/demo/ | launcher/main/apps/demo/nested/foo.c:2 | foo | 4 |", body)

    def test_fresh_scratch_snapshot(self):
        import scratch
        source = self.root / "source"
        source.mkdir()
        (source / "file.c").write_text("first")
        work = self.root / "work"
        with patch.object(scratch, "tracked_files", return_value=["file.c"]):
            tree = scratch.scratch_tree(work, source)
            (tree / "obsolete").write_text("old")
            (source / "file.c").write_text("current")
            with self.assertRaisesRegex(ValueError, "already exists"):
                scratch.scratch_tree(work, source)
            scratch.scratch_tree(work, source, fresh=True)
        self.assertEqual((tree / "file.c").read_text(), "current")
        self.assertFalse((tree / "obsolete").exists())

    def test_ownership_metadata(self):
        names = ["main", "esp32_s3_touch_amoled_1_8", "math", "managed"]
        paths = [self.root / "launcher/main", self.root / "launcher/components/esp32_s3_touch_amoled_1_8", self.root / "launcher/packages/math", self.root / "launcher/managed_components/managed"]
        metadata = {"build_components": names + ["external"], "build_component_paths": list(map(str, paths + [self.root.parent / "outside"]))}
        (self.root / "project_description.json").write_text(json.dumps(metadata))
        owned = memory.components(self.root, json.loads((self.root / "project_description.json").read_text()))
        self.assertEqual(set(owned), {"libmain.a", "libmath.a", "libesp32_s3_touch_amoled_1_8.a"})
        for archive in owned:
            with self.assertRaisesRegex(ValueError, "no site"):
                memory.check([], [memory.Input(archive, "foo.c.obj", "foo", 1)], "release", owned, lambda site: True)
            self.assertEqual(memory.check([], [memory.Input(archive, "foo.c.obj", "foo", 0)], "release", owned, lambda site: True), [])
        self.assertEqual(memory.check([], [memory.Input("libmanaged.a", "foo.c.obj", "foo", 1)], "release", owned, lambda site: True), [])

    def test_archive_and_object_must_match(self):
        sites = self.source("/* Cache-off caller. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}")
        for item in (memory.Input("libmain.a", "other.c.obj", "foo", 4), memory.Input("libmath.a", "foo.c.obj", "foo", 4)):
            with self.assertRaisesRegex(ValueError, "no site"):
                memory.check(sites, [item], "release", {"libmain.a", "libmath.a"}, lambda site: True)

    def test_comment_must_be_adjacent(self):
        for gap in ("\n", "int other;\n"):
            for name, content in (("launcher/main/foo.c", "/* Cache-off caller. IRAM budget: 4 B */\n"+gap+"static void IRAM_ATTR foo(void) {}"),
                                  ("launcher/main/linker.lf", "# Cache latency. IRAM budget: 4 B\n"+gap+"[mapping:hot]\narchive: libmain.a\nentries:\n    foo:foo (noflash)")):
                with self.subTest(name=name, gap=gap), self.assertRaisesRegex(ValueError, "reason"):
                    self.source(content, name)
                (self.root / name).unlink()

    def test_reason_is_a_sentence(self):
        for reason in ("IRAM budget: 4 B", "TODO. IRAM budget: 4 B", "Cache latency IRAM budget: 4 B", "Cache latency TODO pending. IRAM budget: 4 B", "Cache latency FIXME pending. IRAM budget: 4 B"):
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
        build = {"variant": "dev", "idf_commit": "fixture", "config": {"CONFIG_LAUNCHER_DEVELOPMENT": "y"}, "owned": {"libmain.a"}, "inputs": [memory.Input("libmain.a", "foo.c.obj", "foo", 4), memory.Input("libmain.a", "bar.c.obj", "bar", 8)]}
        output = io.StringIO()
        with patch.object(sys, "argv", ["memory_map.py", "--check", "fixture"]), patch.object(memory, "read_build", return_value=build), patch.object(memory, "build_sites", return_value=([app, engine], lambda site: True)), patch.object(memory, "render", side_effect=AssertionError("check rendered tables")), patch.object(memory, "idf_pin", return_value={"commit": "fixture"}), contextlib.redirect_stdout(output):
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
        self.assertEqual(self.source("[mapping:patch]\narchive: libmain.a\nentries:\n    # foo:foo (noflash)\n    * (patchable_entries)", "launcher/main/linker.lf"), [])

    def test_preprocessor_guards(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if compiler is None:
            self.skipTest("no host compiler")
        source = self.root / "launcher/main/foo.c"
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text("#define IRAM_ATTR __attribute__((section(\".iram1\")))\n#ifndef CONFIG_FEATURE\n/* Cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}\n#elif CONFIG_OTHER\n/* Cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR bar(void) {}\n#else\n/* Cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR baz(void) {}\n#endif\n")
        for defines, expected in (([],"foo"),(["-DCONFIG_FEATURE=1", "-DCONFIG_OTHER=1"],"bar"),(["-DCONFIG_FEATURE=1"],"baz")):
            command = {"file":str(source), "directory":str(self.root), "arguments":[compiler,*defines,"-c",str(source),"-o","unused.o"]}
            (self.root / "compile_commands.json").write_text(json.dumps([command]))
            alive=memory.preprocessed_lines(self.root, self.root)
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
            placements, active = memory.build_sites(self.root, build)
            self.assertEqual(memory.check(placements, inputs, "release", build["owned"], active)[0][1], expected)

    def test_variant_budget_missing(self):
        site=memory.Site("foo.c",1,"engine","Cache latency.",{"release":4},"libmain.a",[("foo","foo")])
        with self.assertRaisesRegex(ValueError,"missing dev"):
            self.check([site],[memory.Input("libmain.a","foo.c.obj","foo",4)], variant="dev")

    def test_probe_selection_and_identity(self):
        self.assertIn("silent-assertions",probe.experiments("release"))
        self.assertNotIn("silent-assertions",probe.experiments("dev"))
        self.assertEqual(probe.experiments("diag",["ringbuf-to-flash","i2c-isr"]),["ringbuf-to-flash"])
        with self.assertRaisesRegex(ValueError,"unknown"):
            probe.experiments("release",["unknown"])
        baselines = [{"build_id": "id-a-release", "idf_commit": "local-idf"}]
        body = probe.render_results(baselines, [("release", "a", 4, 0, 0, "reason")])
        self.assertEqual(body.count("id-a-release"), 1)
        self.assertIn("local-idf", body)
        self.assertNotIn("| dev |", body)

    def test_probe_jobs_must_be_positive(self):
        import contextlib
        import io
        with patch.object(sys, "argv", ["iram_what_if.py", "fixture", "--work", str(self.root), "--jobs", "0"]), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            probe.main()
        self.assertEqual(error.exception.code, 2)

    def test_mixed_budget_is_rejected(self):
        for clause in ("4 B, dev 8 B", "release 4 B, 8 B"):
            with self.subTest(clause=clause), self.assertRaisesRegex(ValueError, "mixed"):
                memory.parse_budget("IRAM budget: " + clause)

    def test_all_budget_mismatches_are_reported(self):
        sites = [memory.Site("a.c", 1, "engine", "Cache latency.", 4, "libmain.a", [("a", "a")]),
                 memory.Site("b.c", 1, "engine", "Cache latency.", 8, "libmain.a", [("b", "b")])]
        with self.assertRaises(ValueError) as error:
            self.check(sites, [memory.Input("libmain.a", "a.c.obj", "a", 3), memory.Input("libmain.a", "b.c.obj", "b", 7)])
        self.assertIn("set the budget to 3 B", str(error.exception))
        self.assertIn("set the budget to 7 B", str(error.exception))

    def test_probe_config_rewrite(self):
        original = "CONFIG_A=y\n# CONFIG_B is not set\nCONFIG_C=y\n"
        rewritten = probe.rewrite_config(original, {"CONFIG_A": "n", "CONFIG_B": "y"})
        self.assertEqual(memory.configuration(rewritten), {"CONFIG_A": "n", "CONFIG_B": "y", "CONFIG_C": "y"})
        self.assertEqual(rewritten.count("CONFIG_A"), 1)
        self.assertEqual(rewritten.count("CONFIG_B"), 1)

    def test_probe_row_excludes_split_aliases_and_checks_config(self):
        baseline = {"sections": {".iram0.text": (0, 12), ".iram0.text_end": (0, 400), ".dram0.data": (0, 8), ".dram0.dummy": (0, 900)}, "constants": {"_heap_start": 100}}
        measured = {"sections": {".iram0.text": (0, 8), ".iram0.text_end": (0, 800), ".dram0.data": (0, 4), ".dram0.dummy": (0, 300)}, "constants": {"_heap_start": 90}, "config": {"CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH": "y", "CONFIG_RINGBUF_PLACE_ISR_FUNCTIONS_INTO_FLASH": "y"}}
        self.assertEqual(probe.result_row("ringbuf-to-flash", baseline, measured)[1:4], (4, 4, -10))
        measured["config"] = {}
        self.assertEqual(probe.result_row("ringbuf-to-flash", baseline, measured)[1], "config rejected")

    def test_inactive_site_cannot_own_bytes(self):
        sites = self.source("/* Cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}")
        with self.assertRaisesRegex(ValueError, "compiled-out site owns 4 B"):
            self.check(sites, [memory.Input("libmain.a", "foo.c.obj", "foo", 4)], active=lambda site: False)

    def test_drift_only_relaxes_budget_equality(self):
        import contextlib
        import io
        sites = self.source("/* Cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}")
        inputs = [memory.Input("libmain.a", "foo.c.obj", "foo", 8)]
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            memory.check(sites, inputs, "release", memory.components(self.root), lambda site: True, "local-commit", "pinned-commit")
        self.assertIn("build local-commit, pin pinned-commit", output.getvalue())
        with self.assertRaisesRegex(ValueError, "set the budget to 8 B"):
            memory.check(sites, inputs, "release", memory.components(self.root), lambda site: True, "pinned-commit", "pinned-commit")
        with self.assertRaisesRegex(ValueError, "no site"):
            memory.check([], inputs, "release", memory.components(self.root), lambda site: True, "local-commit", "pinned-commit")
        with self.assertRaisesRegex(ValueError, "compiled-out"):
            memory.check(sites, inputs, "release", memory.components(self.root), lambda site: False, "local-commit", "pinned-commit")

    def test_rounding_ignores_other_sections(self):
        script = ".text : { . = ALIGN(4096); } .iram0.text_end : { . += 16; . = ALIGN(256); } .data : { . = ALIGN(64); }"
        self.assertEqual(memory.rounding(script, 255), (256, 16, 257))
        self.assertEqual(memory.rounding(script.replace("text_end :", "text_end (NOLOAD) :"), 255), (256, 16, 257))
        with self.assertRaisesRegex(ValueError, "alignment"):
            memory.rounding(".iram0.text_end : { . += 16; } .data : { . = ALIGN(64); }", 255)

    def test_mapping_symbols_are_not_functions(self):
        text = ".iram0.text 0x40380000 0x8\n .iram1.0 0x40380000 0x8 main/libmain.a(foo.c.obj)\n"
        inputs, _, _ = memory.map_inputs(text, {0x40380000: ("$x", 4), 0x40380004: ("foo", 4)})
        self.assertEqual([(item.symbol, item.size) for item in inputs], [("foo", 8)])

    def test_unsupported_attribute_declaration(self):
        with self.assertRaisesRegex(ValueError, "unsupported IRAM declaration"):
            self.source("/* Cache latency. IRAM budget: 4 B */\nvoid IRAM_ATTR foo(void);\nvoid unrelated(void) {}")

    def test_config_placement_requires_full_line(self):
        path = "launcher/sdkconfig.defaults"
        for suffix in ("garbage", " # trailing"):
            self.assertEqual(self.source("CONFIG_SPI_MASTER_IN_IRAM=y" + suffix, path), [])
        with self.assertRaisesRegex(ValueError, "reason"):
            self.source("CONFIG_SPI_MASTER_IN_IRAM=y", path)

    def test_fragment_rejects_invalid_branches_and_comments(self):
        prefix = "# Cache latency. IRAM budget: 4 B\n[mapping:hot]\narchive: libmain.a\nentries:\n"
        for body, error in (("    elif FEATURE = y:\n", "elif without if"), ("    else:\n", "else without if"), ("    if FEATURE != y:\n", "unsupported")):
            sites = self.source(prefix + body + "        foo:foo (noflash)\n", "launcher/main/linker.lf")
            with self.assertRaisesRegex(ValueError, error):
                memory.fragment_entries(sites[0], self.root, {})
        sites = self.source(prefix + "    if FEATURE = y:\n        foo:foo (noflash)\n    elif OTHER = y:\n        bar:bar (noflash)\n    # hidden:hidden (noflash)\n", "launcher/main/linker.lf")
        self.assertEqual(memory.fragment_entries(sites[0], self.root, {"CONFIG_FEATURE": "y", "CONFIG_OTHER": "y"}), [("foo", "foo")])

    def test_compile_flags_arguments_and_real_command(self):
        import compile_db
        source = "src/foo.c"
        expected = ["gcc", '-DIDF_VER="v5.5.5-496-gc197d718"', "-Iinclude"]
        arguments = [*expected, "-c", source, "-o", "foo.o", "-MF", "foo.d", "-MT", "target", "-MQ", "quoted", "-MD", "-MMD", "-MP"]
        self.assertEqual(compile_db.compile_flags({"file": source, "arguments": arguments}), expected)
        for platform in ("posix", "nt"):
            command = r'gcc -DIDF_VER=\"v5.5.5-496-gc197d718\" -Iinclude -c src/foo.c -o foo.o -MF foo.d -MT target -MQ quoted -MD -MMD -MP'
            with patch.object(compile_db.os, "name", platform):
                self.assertEqual(compile_db.compile_flags({"file": source, "command": command}), expected)
        self.assertEqual(compile_db.compile_flags({"file": r"src\foo.c", "arguments": ["gcc", "-c", "src/foo.c", "-o", "foo.o"]}), ["gcc"])

    def test_scratch_tree_rejects_unsafe_destination(self):
        import scratch
        work = self.root / "work"
        tree = work / "tree"
        tree.mkdir(parents=True)
        outside = self.root / "outside"
        original = Path.resolve
        with patch.object(Path, "is_symlink", return_value=True):
            with self.assertRaisesRegex(ValueError, "outside the scratch directory"):
                scratch.scratch_tree(work, self.root, fresh=True)
        with patch.object(Path, "resolve", lambda path: outside if path == tree else original(path)):
            with self.assertRaisesRegex(ValueError, "outside the scratch directory"):
                scratch.scratch_tree(work, self.root, fresh=True)
        self.assertTrue(tree.exists())

    def test_fragment_experiments_preserve_required_placements(self):
        original = "foo (noflash)\nbar (noflash_text)\nbaz (iram)\n"
        self.assertEqual(probe.scratch_fragment("noflash-text", original, self.root, {}), "foo (noflash_text)\nbar (noflash_text)\nbaz (iram)\n")
        freertos = self.root / "components/freertos/linker.lf"
        freertos.parent.mkdir(parents=True)
        freertos.write_text("if FREERTOS_PLACE_FUNCTIONS_INTO_FLASH = y:\n    tasks:vTaskDelay (default)\n    tasks:xTaskNotifyFromISR (default)\n    port:portYield (default)\n    portasm:helper (default)\n    queue:xQueueGenericSend (default)\n    queue:xQueueReceive (default)\n    queue:xQueueSemaphoreTake (default)\n")
        fragment = probe.scratch_fragment("freertos-non-isr", "", self.root, {})
        self.assertIn("tasks:vTaskDelay (default)", fragment)
        for excluded in ("FromISR", "port:", "portasm:", "xQueueGenericSend", "xQueueReceive", "xQueueSemaphoreTake"):
            self.assertNotIn(excluded, fragment)
        baseline = {"inputs": [memory.Input("libhal.a", "spi_flash_hal_gpspi.c.obj", "gpspi_fn", 4), memory.Input("libother.a", "spi_flash_hal_gpspi.c.obj", "other", 4)]}
        fragment = probe.scratch_fragment("gpspi-flash-hal", "", self.root, baseline)
        self.assertIn("spi_flash_hal_gpspi:gpspi_fn (default)", fragment)
        self.assertNotIn("other", fragment)

    def test_build_sites_header_and_c_guards(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if compiler is None:
            self.skipTest("no host compiler")
        main = self.root / "launcher/main"
        header = main / "hot.h"
        header.write_text('#define IRAM_ATTR __attribute__((section(".iram1")))\n/* Header cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR header_hot(void) {}\n')
        source = main / "foo.c"
        source.write_text('#include "hot.h"\n#if FEATURE\n/* Enabled cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR live(void) {}\n#endif\n#if OTHER\n/* Disabled cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR dead(void) {}\n#endif\n')
        build = {"build": self.root, "config": {}, "owned": memory.components(self.root)}
        for included in (True, False):
            if not included:
                source.write_text(source.read_text().replace('#include "hot.h"', '#define IRAM_ATTR'))
            entry = {"file": "launcher/main/foo.c", "directory": str(self.root), "arguments": [compiler, "-DFEATURE=1", "-Ilauncher/main", "-c", "launcher/main/foo.c", "-o", "unused.o"]}
            (self.root / "compile_commands.json").write_text(json.dumps([entry]))
            placements, active = memory.build_sites(self.root, build)
            self.assertEqual({site.entries[0][1]: active(site) for site in placements}, {"header_hot": included, "live": True, "dead": False})

    def test_read_build_uses_metadata_and_existing_identity(self):
        build = self.root / "build"
        build.mkdir()
        metadata = {"app_elf": "launcher.elf", "idf_path": str(self.root / "idf"), "build_components": ["chosen", "external"], "build_component_paths": [str(self.root / "launcher/components/chosen"), str(self.root.parent / "outside")]}
        (build / "project_description.json").write_text(json.dumps(metadata))
        (build / "build_id.txt").write_text("123456abcdef-diag\n")
        (build / "sdkconfig").write_text("CONFIG_LAUNCHER_SELFTEST=y\n")
        (build / "launcher.map").write_text("iram0_0_seg 0x40380000 0x400\ndram0_0_seg 0x3fc80000 0x400\n.iram0.text 0x40380000 0x4\n")
        ld = build / "esp-idf/esp_system/ld"
        ld.mkdir(parents=True)
        (ld / "sections.ld").write_text(".iram0.text_end : { . = ALIGN(256); }")
        (ld / "memory.ld").write_text("")
        with patch.object(memory, "symbols", return_value={}), patch.object(memory, "find_tool", return_value="nm"), patch.object(memory, "run", side_effect=[json.dumps({"layout": []}), "idf-commit\n"]):
            measured = memory.read_build(build, self.root)
        self.assertEqual(measured["owned"], {"libchosen.a": self.root / "launcher/components/chosen"})
        self.assertEqual(measured["build_id"], "123456abcdef-diag")
        self.assertEqual(measured["variant"], "diag")
        self.assertEqual(measured["idf_commit"], "idf-commit")

    def test_probe_baseline_is_built_even_without_selected_experiments(self):
        from types import SimpleNamespace
        project = self.root / "project"
        launcher = project / "launcher"
        (launcher / "main").mkdir(parents=True)
        (launcher / "main/linker.lf").write_text("original fragment")
        (launcher / "sdkconfig.defaults").write_text("CONFIG_OTHER=y\n")
        (self.root / "sdkconfig").write_text("CONFIG_OTHER=y\n")
        template = {"build": self.root, "metadata": {"idf_path": "idf"}, "variant": "dev"}
        baseline = {"build_id": "fresh-dev", "idf_commit": "local", "sections": {".iram0.text": (0, 12)}, "constants": {"_heap_start": 100}}
        with patch.object(probe.subprocess, "run", return_value=SimpleNamespace(returncode=0)) as run, patch.object(probe, "read_build", return_value=baseline) as read:
            result, rows = probe.what_if(template, self.root, project, ["i2c-isr"])
        self.assertIs(result, baseline)
        self.assertEqual(rows, [])
        self.assertEqual(run.call_count, 1)
        read.assert_called_once_with(launcher / "build.memory", project)
        self.assertEqual((launcher / "main/linker.lf").read_text(), "original fragment")
        measured = dict(baseline, sections={".iram0.text": (0, 8)}, config=probe.OPTIONS["ringbuf-to-flash"]["config"])
        with patch.object(probe.subprocess, "run", return_value=SimpleNamespace(returncode=0)) as run, patch.object(probe, "read_build", side_effect=[baseline, measured]):
            result, rows = probe.what_if(template, self.root, project, ["ringbuf-to-flash"])
        self.assertEqual(run.call_count, 2)
        self.assertEqual(rows[0][2:5], (4, 0, 0))

    def test_probe_invocations_render_only_their_own_rows(self):
        import contextlib
        import io
        build = self.root / "build"
        build.mkdir()
        (build / "project_description.json").write_text(json.dumps({"idf_path": "idf"}))
        (build / "build_id.txt").write_text("old-release")
        baseline = {"build_id": "new-release", "idf_commit": "local"}
        for name in ("first", "second"):
            with patch.object(sys, "argv", ["iram_what_if.py", str(build), "--work", str(self.root)]), patch.object(probe, "scratch_tree", return_value=self.root) as snapshot, patch.object(probe.shutil, "copytree"), patch.object(probe, "what_if", return_value=(baseline, [("release", name, 4, 0, 0, "reason")])), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(probe.main(), 0)
            snapshot.assert_called_once_with(self.root, probe.ROOT, fresh=True)
            body = (self.root / (probe.WHAT_IF_BLOCK + ".md")).read_text()
            self.assertIn("new-release", body)
            self.assertNotIn("old-release", body)
            self.assertIn("| " + name + " |", body)
            if name == "second":
                self.assertNotIn("| first |", body)

    def test_source_discovery_follows_cmake_and_excludes_external_components(self):
        component = self.root / "launcher/custom/chosen"
        component.mkdir(parents=True)
        (component / "foo.c").write_text("/* Cache latency. IRAM budget: 4 B */\nvoid IRAM_ATTR foo(void) {}")
        with tempfile.TemporaryDirectory() as outside:
            external = Path(outside) / "external"
            external.mkdir()
            (self.root / "launcher/CMakeLists.txt").write_text('set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/custom" "' + outside.replace("\\", "/") + '")')
            self.assertEqual(set(memory.components(self.root)), {"libmain.a", "libchosen.a"})
            self.assertEqual(memory.sites(self.root)[0].path, "launcher/custom/chosen/foo.c")

    def test_scratch_refuses_broken_symlink(self):
        import scratch
        tree = self.root / "tree"
        with patch.object(Path, "is_symlink", lambda path: path == tree), patch.object(scratch, "tracked_files", return_value=[]):
            with self.assertRaisesRegex(ValueError, "outside the scratch directory"):
                scratch.scratch_tree(self.root, self.root, fresh=True)

    def budget_builds(self):
        (self.root / "launcher/idf-pin.json").write_text(json.dumps({"commit": "pinned"}))
        sites = self.source("/* Keep the exact reason. IRAM budget: 1 B */\nvoid IRAM_ATTR foo(void) {}\n/* Development interrupt latency. IRAM budget: release 1 B, dev 2 B, diag 2 B */\nvoid IRAM_ATTR bar(void) {}")
        builds = []
        for variant, size in (("release", 4), ("dev", 8), ("diag", 8)):
            inputs = [memory.Input("libmain.a", "foo.c.obj", "foo", size)]
            if variant != "release":
                inputs.append(memory.Input("libmain.a", "foo.c.obj", "bar", 7))
            builds.append({"variant": variant, "idf_commit": "pinned", "inputs": inputs,
                           "owned": memory.components(self.root), "report": [(sites[0], size, ["foo"]), (sites[1], 0 if variant == "release" else 7, ["bar"])]})
        return builds

    def test_update_budgets_rewrites_clauses_then_exact_check_passes(self):
        builds = self.budget_builds()
        memory.update_budgets(self.root, builds)
        expected = "/* Keep the exact reason. IRAM budget: release 4 B, dev 8 B, diag 8 B */\nvoid IRAM_ATTR foo(void) {}\n/* Development interrupt latency. IRAM budget: 7 B */\nvoid IRAM_ATTR bar(void) {}"
        self.assertEqual((self.root / "launcher/main/foo.c").read_text(), expected)
        for build in builds:
            sites = memory.sites(self.root)
            active = lambda site: site.entries[0][1] == "foo" or build["variant"] != "release"
            report = memory.check(sites, build["inputs"], build["variant"], build["owned"], active)
            self.assertEqual([size for _, size, _ in report], [record[1] for record in build["report"]])
        before = (self.root / "launcher/main/foo.c").read_bytes()
        memory.update_budgets(self.root, builds)
        self.assertEqual((self.root / "launcher/main/foo.c").read_bytes(), before)

    def test_update_budgets_preserves_linker_fragment_reasons(self):
        builds = self.budget_builds()
        original = "# Keep dispatch cache independent. IRAM budget: 1 B\n[mapping:hot]\narchive: libdriver.a\nentries:\n    driver:dispatch (noflash)\n"
        site = next(site for site in self.source(original, "launcher/main/linker.lf") if site.path.endswith(".lf"))
        for build in builds:
            build["report"].append((site, 3, ["dispatch"]))
        memory.update_budgets(self.root, builds)
        self.assertEqual((self.root / site.path).read_text(), original.replace("IRAM budget: 1 B", "IRAM budget: 3 B"))

    def test_update_budgets_refuses_unpinned_or_partial_builds_before_writing(self):
        builds = self.budget_builds()
        source = self.root / "launcher/main/foo.c"
        before = source.read_bytes()
        for records, error in ((builds[:1], "missing.*dev.*diag"), (builds + [builds[0]], "duplicate")):
            with self.assertRaisesRegex(ValueError, error):
                memory.update_budgets(self.root, records)
            self.assertEqual(source.read_bytes(), before)
        builds[-1]["idf_commit"] = "local"
        with self.assertRaisesRegex(ValueError, "local.*pinned"):
            memory.update_budgets(self.root, builds)
        self.assertEqual(source.read_bytes(), before)

    def test_update_budgets_cli_uses_accounting_and_preserves_crlf(self):
        import contextlib
        import io
        builds = self.budget_builds()
        source = self.root / "launcher/main/foo.c"
        source.write_bytes(source.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        self.assertNotIn(b"\r\r\n", source.read_bytes())
        def build_sites(root, build):
            return memory.sites(root), lambda site: site.entries[0][1] == "foo" or build["variant"] != "release"
        arguments = ["memory_map.py", "--root", str(self.root), "--update-budgets", "release", "dev", "diag"]
        with patch.object(sys, "argv", arguments), patch.object(memory, "read_build", side_effect=lambda path, root: next(b for b in builds if b["variant"] == str(path))), patch.object(memory, "build_sites", side_effect=build_sites), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(memory.main(), 0)
        content = source.read_bytes()
        self.assertEqual(content.count(b"\n"), content.count(b"\r\n"))
        with patch.object(sys, "argv", [arguments[0], "--root", str(self.root), "--check", *arguments[-3:]]), patch.object(memory, "read_build", side_effect=lambda path, root: next(b for b in builds if b["variant"] == str(path))), patch.object(memory, "build_sites", side_effect=build_sites), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(memory.main(), 0)

    def test_cli_exit_status(self):
        script=str(memory.ROOT / "launcher/tools/build/memory_map.py")
        for content, expected in (("static void IRAM_ATTR foo(void) {}",1),("/* Cache latency. IRAM budget: 4 B */\nstatic void IRAM_ATTR foo(void) {}",0)):
            path=self.root / "launcher/main/foo.c"
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_text(content)
            done=subprocess.run([sys.executable,script,"--source-only","--root",str(self.root)],capture_output=True,text=True)
            self.assertEqual(done.returncode,expected,done.stderr)



class GateRegressions(unittest.TestCase):
    def test_compile_source_spellings(self):
        for source in ("arbitrary.c", "C:/w/dir/arbitrary.c", r"C:\w\dir\arbitrary.c", "c:/w/dir/arbitrary.c"):
            with self.subTest(source=source):
                entry = {"file": "C:/w/dir/arbitrary.c", "directory": "C:/w/dir",
                         "arguments": ["cc", "-DKEEP=y", "-c", source, "-o", "out.o", "-MF", "out.d", "-MT", "target", "-MQ", "quoted"]}
                self.assertEqual(compile_db.compile_flags(entry), ["cc", "-DKEEP=y"])
                entry.pop("arguments")
                entry["command"] = f'cc -DKEEP=y -c "{source}" -o out.o -MF out.d -MT target -MQ quoted'
                self.assertEqual(compile_db.compile_flags(entry), ["cc", "-DKEEP=y"])

    def test_windows_response_and_quotes(self):
        with patch.object(compile_db.os, "name", "nt"):
            entry = {"file": "C:/w dir/foo.c", "directory": "C:/w dir",
                     "command": '"cc.exe" @"file.rsp" "-Iinclude path" -c "foo.c" -o "out.o"'}
            self.assertEqual(compile_db.compile_flags(entry), ["cc.exe", "@file.rsp", "-Iinclude path"])

    def test_main_pin_controls_exactness(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "launcher").mkdir()
            pin = root / "launcher/idf-pin.json"
            site = memory.Site("foo.c", 1, "engine", "Cache latency.", 4, "libmain.a", [("foo", "foo")])
            build = dict(variant="release", idf_commit="measured", owned={"libmain.a"},
                         inputs=[memory.Input("libmain.a", "foo.c.obj", "foo", 8)])
            for commit, flags, status in (("measured", [], 1), ("other", [], 0), ("other", ["--require-pin"], 1)):
                pin.write_text(json.dumps({"commit": commit}))
                output = io.StringIO()
                with patch.object(sys, "argv", ["memory_map.py", "fixture", "--root", str(root), "--check", *flags]), patch.object(memory, "read_build", return_value=build), patch.object(memory, "build_sites", return_value=([site], lambda site: True)), contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                    self.assertEqual(memory.main(), status)
                if status == 0:
                    self.assertIn("NOTE IDF drift: build measured, pin other", output.getvalue())

    def test_unknown_build_suffix(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            for suffix in (*memory.VARIANTS, "unexpected"):
                (build / "build_id.txt").write_text("identity-" + suffix)
                if suffix in memory.VARIANTS:
                    self.assertEqual(memory.build_variant(build), ("identity-" + suffix, suffix))
                else:
                    with self.assertRaisesRegex(ValueError, "unknown build variant"):
                        memory.build_variant(build)

    def test_all_build_problems_are_reported(self):
        site = memory.Site("foo.c", 1, "engine", "Cache latency.", {"release": 4}, "libmain.a", [("foo", "foo")])
        builds = [dict(variant=variant, idf_commit="pinned", owned={"libmain.a"},
                       inputs=[memory.Input("libmain.a", "foo.c.obj", "foo", 8), memory.Input("libmain.a", "other.c.obj", "other", 2)])
                  for variant in memory.VARIANTS]
        output = io.StringIO()
        with patch.object(sys, "argv", ["memory_map.py", "--check", *memory.VARIANTS]), patch.object(memory, "idf_pin", return_value={"commit": "pinned"}), patch.object(memory, "read_build", side_effect=builds), patch.object(memory, "build_sites", return_value=([site], lambda site: True)), contextlib.redirect_stderr(output):
            self.assertEqual(memory.main(), 1)
        for variant in memory.VARIANTS:
            self.assertIn(variant + ":", output.getvalue())
        self.assertEqual(output.getvalue().count("no site"), len(memory.VARIANTS))
        self.assertIn("set the budget to 8 B", output.getvalue())
        for variant in memory.VARIANTS[1:]:
            self.assertIn("missing " + variant, output.getvalue())

    def test_live_empty_site_fails_under_drift(self):
        site = memory.Site("foo.c", 1, "engine", "Cache latency.", 4, "libmain.a", [("foo", "foo")])
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, "placed nothing"):
            memory.check([site], [], "release", {"libmain.a"}, lambda site: True, "local", "pin")

    def test_source_component_declarations(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            component = root / "launcher/custom/chosen"
            component.mkdir(parents=True)
            (component / "foo.c").write_text("/* Cache latency. IRAM budget: 4 B */\nvoid IRAM_ATTR foo(void) {}")
            cmake = root / "launcher/CMakeLists.txt"
            cmake.write_text("set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/custom)\n")
            self.assertEqual(memory.sites(root)[0].path, "launcher/custom/chosen/foo.c")
            for extra in ("set(EXTRA_COMPONENT_DIRS custom)", "set(EXTRA_COMPONENT_DIRS)", "list(APPEND EXTRA_COMPONENT_DIRS custom)"):
                cmake.write_text("set(EXTRA_COMPONENT_DIRS custom)\n" + extra)
                with self.assertRaisesRegex(ValueError, "EXTRA_COMPONENT_DIRS"):
                    memory.components(root)
            cmake.write_text("set(EXTRA_COMPONENT_DIRS ${UNSUPPORTED}/custom)")
            with self.assertRaisesRegex(ValueError, "unsupported EXTRA_COMPONENT_DIRS"):
                memory.components(root)

    def test_stack_jobs_reuse_filtered_flags(self):
        sys.path.insert(0, str(memory.ROOT / "launcher/tools/quality"))
        import stack_chain_gate
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "foo.c"
            source.write_text("void root(void) {}")
            db = root / "compile_commands.json"
            db.write_text(json.dumps([{"directory": str(root), "file": str(source),
                                       "arguments": ["cc", "-DKEEP=y", "-c", "foo.c", "-o", "foo.o", "-MF", "foo.d", "-MT", "target", "-MQ", "quoted"]}]))
            jobs = stack_chain_gate.app_jobs(str(db), str(root))
            self.assertEqual(jobs, [(["cc", "-DKEEP=y"], str(root), str(source).replace("\\", "/"))])

    def test_default_pin_reader(self):
        expected = json.loads((memory.ROOT / "launcher/idf-pin.json").read_text())
        self.assertEqual(espressif.idf_pin(), expected)

    def test_probe_config_prefix_and_bss(self):
        rewritten = probe.rewrite_config("CONFIG_A=y\nCONFIG_AB=y\n", {"CONFIG_A": "n"})
        self.assertEqual(memory.configuration(rewritten), {"CONFIG_A": "n", "CONFIG_AB": "y"})
        baseline = dict(sections={".dram0.data": (0, 8), ".dram0.bss": (0, 16)}, constants={"_heap_start": 100})
        measured = dict(sections={".dram0.data": (0, 4), ".dram0.bss": (0, 8)}, constants={"_heap_start": 88}, config=probe.OPTIONS["ringbuf-to-flash"]["config"])
        self.assertEqual(probe.result_row("ringbuf-to-flash", baseline, measured)[2:4], (12, -12))

    def test_probe_main_two_variant_baselines_and_failures(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            launcher = root / "launcher"
            (launcher / "main").mkdir(parents=True)
            defaults = launcher / "sdkconfig.defaults"
            fragment = launcher / "main/linker.lf"
            original_defaults = "CONFIG_A=y\nCONFIG_AB=y\n"
            original_fragment = "original (noflash)"
            defaults.write_text(original_defaults)
            fragment.write_text(original_fragment)
            builds = []
            for variant in ("release", "dev"):
                build = root / variant
                build.mkdir()
                (build / "build_id.txt").write_text("template-" + variant)
                (build / "project_description.json").write_text(json.dumps({"idf_path": "idf"}))
                (build / "sdkconfig").write_text("CONFIG_A=y\nCONFIG_AB=y\nCONFIG_LAUNCHER_SELFTEST=y\n")
                builds.append(str(build))
            options = {"first": {"variants": ("release",), "config": {"CONFIG_A": "n"}},
                       "second": {"variants": ("release", "dev"), "config": {"CONFIG_A": "n"}}}
            def measured(variant, size):
                return dict(build_id="fresh-" + variant, idf_commit="local", inputs=[],
                            sections={".iram0.text": (0, size)}, constants={"_heap_start": 100}, config={"CONFIG_A": "n"})
            snapshots = []
            def run(command, **kwargs):
                snapshots.append((command[-1], defaults.read_text(), fragment.read_text()))
                return SimpleNamespace(returncode=1 if len(snapshots) == 2 else 0)
            argv = ["iram_what_if.py", *builds, "--work", str(root)]
            with patch.object(sys, "argv", argv), patch.dict(probe.OPTIONS, options, clear=True), patch.object(probe, "scratch_tree", return_value=root), patch.object(probe.shutil, "copytree"), patch.object(probe.subprocess, "run", side_effect=run), patch.object(probe, "read_build", side_effect=[measured("release", 20), measured("release", 12), measured("dev", 40), measured("dev", 24)]), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(probe.main(), 0)
            body = (root / (probe.WHAT_IF_BLOCK + ".md")).read_text()
            self.assertIn("| release | first | build failed |", body)
            self.assertIn("| release | second | 8 |", body)
            self.assertIn("| dev | second | 16 |", body)
            self.assertNotIn("| dev | first |", body)
            self.assertIn("Baseline builds: fresh-release (IDF local); fresh-dev (IDF local)", body)
            self.assertEqual([snapshot[0] for snapshot in snapshots], ["release", "release", "release", "dev", "dev"])
            for index in (0, 3):
                self.assertEqual(snapshots[index][1:], (original_defaults, original_fragment))
            for index in (1, 2, 4):
                self.assertEqual(snapshots[index][1], "CONFIG_AB=y\n")
            self.assertEqual(defaults.read_text(), original_defaults)
            self.assertEqual(fragment.read_text(), original_fragment)
            with patch.object(sys, "argv", argv), patch.dict(probe.OPTIONS, options, clear=True), patch.object(probe, "scratch_tree", return_value=root), patch.object(probe.shutil, "copytree"), patch.object(probe.subprocess, "run", return_value=SimpleNamespace(returncode=1)), patch.object(probe, "read_build") as read, contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(probe.main(), 1)
                read.assert_not_called()
            self.assertIn("baseline build failed", output.getvalue())

    def test_doc_stages_tables_keep_their_previous_text(self):
        import test_doc_stages
        calls = []
        def compare(headers, rows):
            rows = list(rows)
            text = memory.markdown(headers, rows)
            expected = "\n".join(["| " + " | ".join(headers) + " |",
                                  "|" + "|".join("---" for _ in headers) + "|",
                                  *("| " + " | ".join(map(str, row)) + " |" for row in rows)]) + "\n"
            self.assertEqual(text, expected)
            calls.append(text)
            return text
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(test_doc_stages.DocStagesTests)
        with patch.object(test_doc_stages.stages, "markdown", side_effect=compare):
            result = unittest.TextTestRunner(stream=io.StringIO()).run(suite)
        self.assertTrue(result.wasSuccessful(), result.errors or result.failures)
        self.assertTrue(calls)


if __name__ == "__main__":
    unittest.main()
