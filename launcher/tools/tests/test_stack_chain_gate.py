"""Main-task chain budgets and compilation coverage."""
import json
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from quality import stack_chain_gate as gate
from build.find_cc import host_cc


class StackChainTests(unittest.TestCase):
    def test_a_compiled_suite_adds_test_roots_without_spec_lines(self):
        cc = host_cc()
        if cc is None:
            self.skipTest("a C preprocessor is required")
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "suite_example.c"
            source.write_text(
                "#define SUITE_REGISTER(fn) void fn##_register(void) { suite_link(&fn##_entry); }\n"
                "#define RUN_TEST(fn) suite_run_test_timed(fn, #fn, __LINE__)\n"
                "#define RUN_TEST_CASE(fn, ...) suite_run_test_timed(fn, __VA_ARGS__, __LINE__)\n"
                "void run_example(void) {\n"
                "    RUN_TEST(test_frame);\n"
                '    RUN_TEST_CASE(test_frame, "case");\n'
                "#ifdef HOST_ONLY\n"
                "    RUN_TEST(test_host_only);\n"
                "#endif\n"
                "}\n"
                "SUITE_REGISTER(run_example)\n"
            )
            entry = {"arguments": [cc], "file": str(source), "directory": directory}
            roots, edges = gate.runner_registrations([(entry, directory, str(source), "")])
            self.assertEqual({f"{source.name}:test_frame": ("run_example",)}, roots)
            self.assertIn(("suites_run_request", "run_example"), edges)

    def test_derived_harness_and_interrupt_cannot_overbook_the_main_task(self):
        with tempfile.TemporaryDirectory() as directory:
            spec = pathlib.Path(directory) / "stack_chain.txt"
            spec.write_text("")
            graph = ({"test_frame": 2400, "main_task": 400, "run_example": 48,
                      "other_suite": 4096, "call_protected": 80},
                     {"main_task": {"run_example", "other_suite"},
                      "run_example": {"call_protected"}, "other_suite": {"call_protected"}},
                     set(), [])
            with patch.object(gate, "parse_graph", return_value=graph):
                problems = gate.check_app("app", str(spec), [], 3584, 512, 224,
                                          test_roots={"test_frame": ("run_example",)})
            self.assertTrue(any("3152 bytes" in p for p in problems))

    def test_dispatch_target_is_bound_to_its_caller(self):
        with tempfile.TemporaryDirectory() as directory:
            spec = pathlib.Path(directory) / "stack_chain.txt"
            spec.write_text(
                "root light frame\n"
                "indirect light>bridge>dispatch : small\n"
                "indirect heavy>dispatch : large\n"
            )
            graph = ({"light": 32, "bridge": 32, "heavy": 32, "dispatch": 64, "small": 128, "large": 4096,
                      "main_task": 32, "shell_step_app": 32},
                     {"main_task": {"shell_step_app"}, "light": {"bridge"}, "bridge": {"dispatch"}, "heavy": {"dispatch"}},
                     {"dispatch"}, [])
            self.assertEqual([], gate.check_app("app", str(spec), [], 3584, 512, graph=graph))
            spec.write_text("root heavy frame\nindirect light>bridge>dispatch : small\n")
            problems = gate.check_app("app", str(spec), [], 3584, 512, graph=graph)
            self.assertTrue(any("dispatch makes a call through a pointer" in problem for problem in problems))
            spec.write_text("root heavy frame\nindirect heavy>dispatch : large\n")
            problems = gate.check_app("app", str(spec), [], 3584, 512, graph=graph)
            self.assertTrue(any("4256 bytes" in problem for problem in problems))

    def test_dispatch_with_no_targets_requires_an_explicit_declaration(self):
        with tempfile.TemporaryDirectory() as directory:
            spec = pathlib.Path(directory) / "stack_chain.txt"
            graph = ({"root": 64, "dispatch": 32, "main_task": 32, "shell_step_app": 32},
                     {"main_task": {"shell_step_app"}, "root": {"dispatch"}}, {"dispatch"}, [])
            spec.write_text("root root frame\n")
            problems = gate.check_app("app", str(spec), [], 3584, 512, graph=graph)
            self.assertTrue(any("dispatch makes a call through a pointer" in problem for problem in problems))
            spec.write_text("root root frame\nindirect dispatch : -\n")
            self.assertEqual([], gate.check_app("app", str(spec), [], 3584, 512, graph=graph))

    def test_an_unreachable_harness_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            spec = pathlib.Path(directory) / "stack_chain.txt"
            spec.write_text("")
            graph = ({"test_frame": 32, "main_task": 32,
                      "call_protected": 48}, {}, set(), [])
            with patch.object(gate, "parse_graph", return_value=graph):
                problems = gate.check_app("app", str(spec), [], 3584, 512, 224,
                                          test_roots={"test_frame": ()})
            self.assertTrue(any("no measured main_task path" in p for p in problems))

    def test_harness_follows_the_deepest_live_path_only(self):
        frames = {"main_task": 48, "a": 80, "b": 160,
                  "call_protected": 48, "unrelated": 4096}
        calls = {"main_task": {"a", "b", "unrelated"},
                 "a": {"call_protected"}, "b": {"call_protected"}}
        self.assertEqual(256, gate.deepest("main_task", frames, calls,
                                         "call_protected")[0])

    def test_engine_jobs_include_every_non_app_source(self):
        with tempfile.TemporaryDirectory() as directory:
            db = pathlib.Path(directory) / "compile_commands.json"
            root = gate.MAIN_DIR.replace("\\", "/")
            names = [root + "/render/a.c", root + "/core/b.c", root + "/apps/sample/c.c"]
            db.write_text(json.dumps([{"file": n, "directory": directory, "output": n + ".obj",
                                       "command": "cc -c " + n} for n in names]))
            self.assertEqual(names[:2], [j[2] for j in gate.app_jobs(str(db))])

    def test_runner_definitions_are_discovered_without_filenames(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "arbitrary.c"
            source.write_text("static void runner(void (*body)(void)) { body(); }")
            db = pathlib.Path(directory) / "compile_commands.json"
            db.write_text(json.dumps([{"file": str(source), "directory": directory,
                                       "output": "arbitrary.c.obj", "command": "cc -c arbitrary.c"}]))
            self.assertEqual([], gate.app_jobs(str(db)))
            jobs = gate.app_jobs(str(db), functions={"runner"})
            self.assertEqual([str(source).replace("\\", "/")], [j[2] for j in jobs])

    def test_a_graph_is_read_from_beside_its_object(self):
        entry = {"directory": "/build", "output": "esp-idf/main/CMakeFiles/m.dir/render/a.c.obj"}
        self.assertEqual(pathlib.Path("/build/esp-idf/main/CMakeFiles/m.dir/render/a.c.ci"),
                         pathlib.Path(gate.graph_file(entry)))

    def test_a_dynamic_name_is_not_a_dynamic_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            ci = pathlib.Path(directory) / "graph.ci"
            ci.write_text('node: { title: "set_dynamic" label: "set_dynamic\\nx.c:1:1" }')
            self.assertEqual([], gate.parse_graph([str(ci)])[3])

    def test_private_pointer_targets_can_be_qualified(self):
        frames = {"a.c:writer": 32, "b.c:writer": 48}
        self.assertEqual(["a.c:writer"], gate.resolve("a.c:writer", frames))


if __name__ == "__main__":
    unittest.main()
