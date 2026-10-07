"""Main-task chain budgets and compilation coverage."""
import json
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from quality import stack_chain_gate as gate


class StackChainTests(unittest.TestCase):
    def test_derived_harness_and_interrupt_cannot_overbook_the_main_task(self):
        with tempfile.TemporaryDirectory() as directory:
            spec = pathlib.Path(directory) / "stack_chain.txt"
            spec.write_text("root test_frame test\n")
            graph = ({"test_frame": 2400, "main_task": 400,
                      "call_protected": 80}, {"main_task": {"call_protected"}}, set(), [])
            with patch.object(gate, "parse_graph", return_value=graph):
                problems = gate.check_app("app", str(spec), [], 3584, 512, 224)
            self.assertTrue(any("3104 bytes" in p for p in problems))

    def test_an_unreachable_harness_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            spec = pathlib.Path(directory) / "stack_chain.txt"
            spec.write_text("root test_frame test\n")
            graph = ({"test_frame": 32, "main_task": 32,
                      "call_protected": 48}, {}, set(), [])
            with patch.object(gate, "parse_graph", return_value=graph):
                problems = gate.check_app("app", str(spec), [], 3584, 512, 224)
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
            names = [root + "/render/a.c", root + "/util/b.c", root + "/apps/sample/c.c"]
            db.write_text(json.dumps([{"file": n, "directory": directory,
                                       "command": "cc -c " + n} for n in names]))
            self.assertEqual(names[:2], [j[2] for j in gate.app_jobs(str(db))])

    def test_runner_definitions_are_discovered_without_filenames(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "arbitrary.c"
            source.write_text("static void runner(void (*body)(void)) { body(); }")
            db = pathlib.Path(directory) / "compile_commands.json"
            db.write_text(json.dumps([{"file": str(source), "directory": directory,
                                       "command": "cc -c arbitrary.c"}]))
            self.assertEqual([], gate.app_jobs(str(db)))
            jobs = gate.app_jobs(str(db), functions={"runner"})
            self.assertEqual([str(source).replace("\\", "/")], [j[2] for j in jobs])

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
