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
    def test_duplicate_private_symbols_keep_the_largest_frame_and_both_calls(self):
        frame, calls, terminal, _blocked = gate.parse_elf('''
42000000 <private>:
42000000: 006136 entry a1, 48
42000003: 000025 call8 42000040 <one>
42000006: f01d retw.n
42000020 <private>:
42000020: 004136 entry a1, 32
42000023: 000025 call8 42000060 <two>
42000026: f01d retw.n
42000040 <one>:
42000040: 004136 entry a1, 32
42000043: f01d retw.n
42000060 <two>:
42000060: 004136 entry a1, 32
42000063: f01d retw.n
''')
        self.assertEqual(48, frame['private'])
        self.assertEqual({'one', 'two'}, calls['private'])
        self.assertNotIn('private', terminal)

    def test_incomplete_branch_disassembly_keeps_the_callee(self):
        frame, calls, terminal, _blocked = gate.parse_elf('''
42000000 <caller>:
42000000: 004136 entry a1, 32
42000003: 000025 call8 42000020 <helper>
42000006: f01d retw.n
42000020 <helper>:
42000020: 006136 entry a1, 48
42000023: 000086 j 42000027 <helper+0x7>
42000026: f01d00 subx8 a1, a13, a0
''')
        self.assertNotIn('helper', terminal)
        self.assertEqual({'helper'}, calls['caller'])

    def test_standard_nonreturning_wrap_is_not_an_ordinary_return(self):
        _frame, calls, terminal, blocked = gate.parse_elf('''
42000000 <caller>:
42000000: 004136 entry a1, 32
42000003: 000026 bnez a2, 42000020 <caller+0x20>
42000006: 000025 call8 42000040 <report_failure>
42000009: 000025 call8 42000060 <__wrap_longjmp>
42000020: f01d retw.n
42000040 <report_failure>:
42000040: 006136 entry a1, 48
42000043: f01d retw.n
42000060 <__wrap_longjmp>:
42000060: 002136 entry a1, 16
42000063: f01d retw.n
''')
        self.assertIn('__wrap_longjmp', terminal)
        self.assertNotIn('caller', calls)
        self.assertEqual({'report_failure', '__wrap_longjmp'}, blocked['caller'])

    def test_failure_block_calls_are_excluded_but_returning_init_is_counted(self):
        text = '''
42000000 <caller>:
42000000: 004136 entry a1, 32
42000003: 000026 bnez a2, 42000020 <caller+0x20>
42000006: 000025 call8 42000040 <report_failure>
42000009: 000025 call8 42000060 <bail>
42000020: 000025 call8 42000080 <lazy_init>
42000023: f01d retw.n
42000040 <report_failure>:
42000040: 006136 entry a1, 48
42000043: f01d retw.n
42000060 <bail>:
42000060: 006136 entry a1, 48
42000063: 0008e0 callx8 a8
42000080 <lazy_init>:
42000080: 006136 entry a1, 48
42000083: f01d retw.n
'''
        frame, calls, _terminal, blocked = gate.parse_elf(text)
        self.assertEqual({'lazy_init'}, calls['caller'])
        self.assertEqual({'report_failure', 'bail'}, blocked['caller'])
        graph = ({'caller': 32, 'report_failure': 48, 'lazy_init': 48},
                 {'caller': {'report_failure', 'lazy_init'}}, set(), [])
        with patch.object(gate, 'parse_graph', return_value=graph):
            self.assertEqual({'lazy_init'}, gate.linked_graph([], text)[1]['caller'])

    def test_nonreturning_dispatch_loops_keep_the_harness_reachable(self):
        graph = ({'entry': 32, 'loop': 48, 'shell_step_app': 32},
                 {'entry': {'loop'}, 'loop': {'shell_step_app'}}, set(), [])
        with patch.object(gate, 'parse_graph', return_value=graph):
            frame, calls, _pointers, _bad = gate.linked_graph([], '''
42000000 <entry>:
42000000: 004136 entry a1, 32
42000003: 000025 call8 42000020 <loop>
42000006: f01d retw.n
42000020 <loop>:
42000020: 006136 entry a1, 48
42000023: 000025 call8 42000040 <shell_step_app>
42000040 <shell_step_app>:
42000040: 004136 entry a1, 32
42000043: f01d retw.n
''')
        self.assertEqual(112, gate.deepest('entry', frame, calls,
                                         'shell_step_app')[0])

    def test_nonreturning_library_paths_are_derived_from_instructions(self):
        frame, calls, terminal, _blocked = gate.parse_elf('''
42000000 <caller>:
42000000: 004136 entry a1, 32
42000003: 000025 call8 42000020 <terminal>
42000006: f01d retw.n
42000020 <terminal>:
42000020: 064136 entry a1, 0x320
42000023: 0008e0 callx8 a8
''')
        self.assertEqual({'terminal'}, terminal)
        self.assertEqual({}, calls)

    def test_linked_library_frames_and_direct_calls_are_counted(self):
        frame, calls, _terminal, _blocked = gate.parse_elf('''
42000000 <printf>:
42000000: 004136 entry a1, 32
42000003: 000025 call8 42000020 <_vfprintf_r>
42000006: f01d retw.n
42000020 <_vfprintf_r>:
42000020: 064136 entry a1, 0x320
42000023: 0008e0 callx8 a8
42000026: f01d retw.n
''')
        self.assertEqual((832, ['printf', '_vfprintf_r']),
                         gate.deepest('printf', frame, calls))

    def test_linker_long_calls_are_direct_but_loaded_pointers_are_not(self):
        frame, calls, _terminal, _blocked = gate.parse_elf('''
42000000 <caller>:
42000000: 004136 entry a1, 32
42000003: 000081 l32r a8, 42001000 <pool> (42000020 <callee>)
42000006: 0008e0 callx8 a8
42000009: 000081 l32r a8, 42001004 <pool> (3fc00000 <callback>)
4200000c: 0888 l32i.n a8, a8, 0
4200000e: 0008e0 callx8 a8
42000011: f01d retw.n
42000020 <callee>:
42000020: 006136 entry a1, 48
42000023: f01d retw.n
''')
        self.assertEqual({'callee'}, calls['caller'])
        self.assertEqual(80, gate.deepest('caller', frame, calls)[0])

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
