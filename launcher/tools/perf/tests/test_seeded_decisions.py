"""Seeded decisions and interrupted acquisitions."""
import contextlib
import importlib.util
import io
import inspect
import json
import math
import pathlib
import random
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from test_perf_compare import FakeAutana, arguments, record_for_rows
import layout_measure as capture
import perf_compare as tool
import seed_statistics as stats


class RevisionTests(unittest.TestCase):
    def measure(self, root, fake, cap=32):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            payload = tool.measure(arguments(root, cap), fake)
        return payload, output.getvalue()

    def test_first_pass_and_split_budget(self):
        with tempfile.TemporaryDirectory() as root:
            with patch.object(tool, "compare", wraps=stats.compare) as compare:
                result, summary = self.measure(root, FakeAutana(root))
            self.assertEqual(result["first_pass"], stats.minimum_seeds(.05))
            self.assertEqual(sorted(item["runs"] for item in result["plan"]), [1]*4+[2]*4)
            self.assertEqual(compare.call_args.args[3], .05/4/2)
            self.assertEqual(compare.call_args.kwargs["permutation_alpha"], .05)
            self.assertIn("R by look", summary)

    def test_one_sided_zero_and_partial_rows(self):
        result = stats.compare({"removed": [100]*4, "zero": [0]*4, "partial": []},
                               {"added": [100]*4, "zero": [0]*4, "partial": [100]*4})
        self.assertEqual({name: row["verdict"] for name, row in result.items()},
                         {"added": "added", "removed": "removed", "zero": "not measured", "partial": "not measured"})

    def test_expected_missing_row_is_preserved_without_partial_mean(self):
        records = [{"suites": {"s": {"runs": [
            {"rows": {"row": 10}, "owners": {}, "instructions": {"row": 20}},
            {"rows": {}, "owners": {}, "instructions": {}}]}}}]
        rows, counters, _ = tool.observations(records)
        self.assertEqual(rows, {"s/row": []})
        self.assertEqual(counters, {})

    def test_missing_entire_flash_is_named_in_summary(self):
        class MissingFlash(FakeAutana):
            def __call__(self, command, log, timeout):
                code, lines, wall = super().__call__(command, log, timeout)
                if "status" not in command and len(self.calls) == 1:
                    for _, line in lines:
                        if line.startswith("report: "):
                            path = pathlib.Path(line[8:]).with_suffix(".log")
                            path.write_text("\n".join(line for line in path.read_text().splitlines()
                                                      if "quiet" not in line))
                return code, lines, wall
        with tempfile.TemporaryDirectory() as root:
            result, summary = self.measure(root, MissingFlash(root))
            missing = result["plan"][0]
            self.assertIn(f"{missing['side'].upper()} incomplete timing seeds for `suite/quiet`: {missing['seed']}", summary)
            self.assertIn("suite/quiet", result["rows"])

    def test_table_failure_has_stderr(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(RuntimeError, "table exploded"):
                capture.make_table('python3 -c "import sys; sys.exit(\'table exploded\')"',
                                   pathlib.Path(root)/"capture", pathlib.Path(root)/"table")

    def test_two_failures_write_incomplete_summary(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(RuntimeError, "two consecutive"):
                self.measure(root, FakeAutana(root, mode="mismatch"))
            payload = json.loads((pathlib.Path(root)/"out/comparison.json").read_text())
            self.assertTrue(payload["incomplete"])
            self.assertEqual(len(payload["plan"]), 2)
            self.assertTrue(all(item.get("error") for item in payload["plan"]))
            self.assertIn("Incomplete", (pathlib.Path(root)/"out/summary.md").read_text())

    def test_mixed_runs_pool_within_variance_and_all_seed_means(self):
        estimate = capture.analyse([[90, 110], [100, 102], [200], [300]], 100, diagnostics=False)
        self.assertEqual(estimate["seeds"], 4)
        self.assertEqual(estimate["mean"], 175.25)
        self.assertAlmostEqual(estimate["sigma_run"], math.sqrt(101)/175.25)
        recommendation = tool.recommendations(
            [{"flash_seconds": 100000, "suites": {"s": {"run_seconds": [1]}}}],
            {"row": [[90, 110], [90, 110], [100]]}, .01)["row"]
        self.assertEqual(recommendation["recommended_runs"], tool.MAX_RUNS)

    def test_required_seeds_uses_actual_alpha(self):
        self.assertGreater(capture.required_seeds(.02, .01, 2, .01, alpha=.001),
                           capture.required_seeds(.02, .01, 2, .01, alpha=.05))

    def test_status_and_second_suite_build_id(self):
        class Runner(FakeAutana):
            def __call__(self, command, log, timeout):
                if "status" in command:
                    self.calls.append(command)
                    return 0, [(0, "free")], 0
                if "--layout-seed" not in command:
                    self.second = command[:]
                    command = command + ["--layout-seed", "3"]
                return super().__call__(command, log, timeout)
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            args.project, args.runs = args.project_a, 2
            args.suite += [("second", "-", "-")]
            fake = Runner(root)
            capture.run_flash(args, 3, fake)
            self.assertEqual(sum("status" in call for call in fake.calls), 2)
            self.assertIn("--expect-build-id", fake.second)
            self.assertEqual(fake.second[fake.second.index("--expect-build-id")+1], "3-diag")

    def test_path_command_resolves_windows_command_launcher(self):
        with patch("shutil.which", return_value="C:/replay/autana.cmd") as resolve:
            self.assertEqual(capture.autana_command("status"), ["C:/replay/autana.cmd", "status"])
            resolve.assert_called_once_with("autana")
        with patch("shutil.which", return_value="C:/replay/python.exe") as resolve:
            self.assertEqual(capture.autana_command("status", override="python fake.py"),
                             ["C:/replay/python.exe", "fake.py", "status"])
            resolve.assert_called_once_with("python")

    def test_doc_stages_imports_parser_owner(self):
        path = tool.Path(__file__).resolve().parents[2]/"render/doc_stages.py"
        spec = importlib.util.spec_from_file_location("doc_stages", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertIs(module.MEAN_RE, capture.MEAN_RE)

    def test_deadline_stops_grandchild_holding_pipe(self):
        with tempfile.TemporaryDirectory() as root:
            marker = pathlib.Path(root)/"alive"
            grandchild = f"import time; from pathlib import Path; time.sleep(3); Path({str(marker)!r}).touch(); time.sleep(10)"
            parent = f"import subprocess,sys,time; subprocess.Popen([sys.executable,'-c',{grandchild!r}]); time.sleep(10)"
            started = time.monotonic()
            with self.assertRaisesRegex(RuntimeError, "timed out"):
                capture.run_stamped([sys.executable, "-c", parent], io.StringIO(), timeout=.1)
            self.assertLess(time.monotonic()-started, 3)
            time.sleep(4)
            self.assertFalse(marker.exists())

    def test_permutation_is_cross_check_and_signs_agree(self):
        for value in (.2, None):
            with patch.object(stats, "permutation", return_value=value):
                self.assertEqual(stats.compare({"r": [100]*4}, {"r": [110]*4})["r"]["verdict"], "inconclusive")
        a = {"r": [1, 1, 1, 10000]*3}
        b = {"r": [100]*12}
        row = stats.row_test(a["r"], b["r"], .01, .05, random.Random(1))
        self.assertLess((row["ratio"]-1)*row["log_difference"], 0)
        with patch.object(stats, "row_test", return_value=dict(row, p=0, permutation=0)):
            self.assertEqual(stats.compare(a, b)["r"]["verdict"], "inconclusive")

    def test_alpha_equality_and_holm_equivalence(self):
        alpha = 2/70
        self.assertEqual(stats.compare({"r": [100]*4}, {"r": [110]*4}, alpha=alpha)["r"]["verdict"], "regressed")
        for field, expected in (("p", "regressed"), ("equivalence", "no change")):
            row = dict(a=100, b=110, ratio=1.1, interval=None, permutation=alpha,
                       log_difference=.1, p=1, equivalence=1)
            row[field] = alpha
            with patch.object(stats, "row_test", return_value=row):
                self.assertEqual(stats.compare({"r": [100]*4}, {"r": [110]*4}, alpha=alpha)["r"]["verdict"], expected)
        self.assertEqual(stats.holm({"a": .02, "b": .025, "c": .04}), {"a": .06, "b": .06, "c": .06})
        row = dict(a=100, b=100, ratio=1, interval=None, permutation=1, p=1, equivalence=.03)
        with patch.object(stats, "row_test", side_effect=lambda *args: row.copy()):
            self.assertEqual(stats.compare({"r": [1]*4}, {"r": [1]*4})["r"]["verdict"], "no change")
            self.assertEqual(stats.compare({"r": [1]*4, "s": [1]*4}, {"r": [1]*4, "s": [1]*4})["r"]["verdict"], "inconclusive")

    def test_subthreshold_difference_requires_non_equivalence(self):
        with patch.object(stats, "permutation", return_value=0):
            row = stats.compare({"r": [100]*4}, {"r": [100.5]*4})["r"]
            self.assertEqual(row["verdict"], "no change")
            with patch.object(stats, "row_test", return_value={key: value for key, value in dict(row, equivalence=1).items() if key != "verdict"}):
                self.assertEqual(stats.compare({"r": [100]*4}, {"r": [100.5]*4})["r"]["verdict"], "regressed")

    def test_monte_carlo_add_one(self):
        class Separated:
            def sample(self, population, count):
                return list(range(count//2))+list(range(count, count+count//2))
        self.assertEqual(stats.permutation([1]*12, [2]*12, .05, Separated(), samples=99), 1/100)

    def test_resolution_settings_are_refused_before_capture(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            args.alpha = 1e-8
            fake = FakeAutana(root)
            with self.assertRaisesRegex(ValueError, "resamples"):
                tool.measure(args, fake)
            self.assertEqual(fake.calls, [])
        self.assertLess(1/(stats.permutation_samples(.0001)+1), .0001/20)

    def test_added_and_removed_rows_do_not_receive_extra_seeds(self):
        class DifferentRows(FakeAutana):
            def __call__(self, command, log, timeout):
                code, lines, wall = super().__call__(command, log, timeout)
                if "status" in command:
                    return code, lines, wall
                side = pathlib.Path(command[command.index("--project")+1]).name
                for _, line in lines:
                    if line.startswith("report: "):
                        path = pathlib.Path(line[8:]).with_suffix(".log")
                        text = path.read_text().replace("quiet both cores", ("removed" if side == "a" else "added") + " both cores").replace("scene=quiet", "scene=" + ("removed" if side == "a" else "added"))
                        path.write_text(text)
                return code, lines, wall
        with tempfile.TemporaryDirectory() as root:
            payload, summary = self.measure(root, DifferentRows(root, noisy=True))
            self.assertEqual(payload["rows"]["suite/removed"]["verdict"], "removed")
            self.assertEqual(payload["rows"]["suite/added"]["verdict"], "added")
            self.assertTrue(all(item["suites"][0][1] == "h"
                                for item in payload["plan"] if item["look"] > 1))
            self.assertIn("removed", summary)
            self.assertIn("added", summary)

    def test_failure_success_failure_resets_streak(self):
        class SometimesFails(FakeAutana):
            attempts = 0
            def __call__(self, command, log, timeout):
                if "status" not in command:
                    self.attempts += 1
                    if self.attempts in (1, 3):
                        raise RuntimeError("intentional failure")
                return super().__call__(command, log, timeout)
        with tempfile.TemporaryDirectory() as root:
            result, _ = self.measure(root, SometimesFails(root), cap=4)
            failed = [item for item in result["plan"] if item.get("error")]
            self.assertEqual(len(failed), 2)
            self.assertTrue(all("intentional failure" in item["error"] for item in failed))
            self.assertFalse(result["incomplete"])
            self.assertEqual(len(result["plan"]), 8)

    def test_decisions_persist_and_k_selects_planned_look(self):
        def recommendations(records, rows, delta, alpha=.025):
            return {name: dict(sigma_run=.01, sigma_flash=.02, recommended_runs=7,
                               recommended_seeds=desired) for name in rows}
        for desired, expected in ((2, 2), (12, 3), (100, 4)):
            with tempfile.TemporaryDirectory() as root:
                row = dict(a=10000, b=10000, ratio=1, interval=None, permutation=1,
                           p_adjusted=1, equivalence_adjusted=1, verdict="inconclusive")
                decisions = {"suite/heavy": row, "suite/quiet": dict(row, verdict="no change")}
                with patch.object(tool, "recommendations", side_effect=recommendations), \
                        patch.object(tool, "run_flash", side_effect=lambda args, seed, runner:
                                     record_for_rows(args, seed, {'quiet': 10000,
                                         'heavy': 10000 + (3000 if seed % 2 else -3000)})), \
                        patch.object(tool, "compare", side_effect=lambda *a, **kw: {
                            name: result.copy() for name, result in decisions.items()}):
                    payload, _ = self.measure(root, FakeAutana(root, noisy=True))
                quiet = payload["rows"]["suite/quiet"]
                self.assertEqual(quiet["look"], 1)
                self.assertEqual(quiet["verdict"], "no change")
                extra = [item for item in payload["plan"] if item["look"] > 1]
                self.assertEqual(extra[0]["look"], expected)
                self.assertTrue(all(item["runs"] == 7 for item in extra))

    def test_later_r_uses_measured_large_flash_cost(self):
        class ExpensiveFlash(FakeAutana):
            def __call__(self, command, log, timeout):
                code, lines, wall = super().__call__(command, log, timeout)
                if "status" in command:
                    return code, lines, wall
                seed = int(command[command.index("--layout-seed")+1])
                center = 100000 + (5000 if seed % 2 else -5000)
                runs = int(command[command.index("--runs")+1])
                rewritten = []
                for at, line in lines:
                    if line.startswith("batch:"):
                        run = len([entry for entry in rewritten if entry[1].startswith("batch:")])
                        rewritten.append((1000+run, line))
                    elif line.startswith("report: "):
                        path = pathlib.Path(line[8:]).with_suffix(".log")
                        value = center + ((30000 if run % 2 else -30000) if runs > 1 else 0)
                        path.write_text(f"I perf: heavy both cores: mean {value}us\n:1:test_heavy:PASS\n")
                        rewritten.append((1001+run, line))
                    else:
                        rewritten.append((at, line))
                return code, rewritten, wall
        with tempfile.TemporaryDirectory() as root:
            result, _ = self.measure(root, ExpensiveFlash(root), cap=16)
            extra = [item for item in result["plan"] if item["look"] > 1]
            self.assertTrue(extra)
            self.assertTrue(all(item["runs"] == tool.MAX_RUNS for item in extra))

    def test_interleaving_replays_both_orders_and_records_fresh_seed(self):
        with tempfile.TemporaryDirectory() as root:
            first, _ = self.measure(root, FakeAutana(root))
            second, _ = self.measure(root, FakeAutana(root))
            self.assertEqual(first["plan"], second["plan"])
            self.assertEqual({tuple(item["side"] for item in first["plan"][i:i+2])
                              for i in range(0, len(first["plan"]), 2)}, {("a", "b"), ("b", "a")})
            args = arguments(root)
            args.rng_seed = None
            with contextlib.redirect_stdout(io.StringIO()):
                fresh = tool.measure(args, FakeAutana(root))
            self.assertIsInstance(fresh["rng_seed"], int)
            plan = json.loads((pathlib.Path(root)/"out/plan.json").read_text())
            self.assertEqual(plan["rng_seed"], fresh["rng_seed"])
            args.rng_seed = None
            with contextlib.redirect_stdout(io.StringIO()):
                another = tool.measure(args, FakeAutana(root))
            self.assertNotEqual(another["rng_seed"], fresh["rng_seed"])

    def test_counter_deltas_and_ambiguous_scenes(self):
        text = ("I xtperf: scene=one event=insn value_per_step=20\n"
                "I xtperf: scene=two event=insn value_per_step=30\n"
                ":1:test_row:PASS\n")
        with tempfile.TemporaryDirectory() as root:
            path = pathlib.Path(root)/"capture.log"
            path.write_text(text)
            self.assertEqual(capture.capture_metadata(path, {"test_row": 10})[1], {})
            class OppositeCounters(FakeAutana):
                def __call__(self, command, log, timeout):
                    code, lines, wall = super().__call__(command, log, timeout)
                    if "status" not in command and pathlib.Path(command[command.index("--project")+1]).name == "b":
                        for _, line in lines:
                            if line.startswith("report: "):
                                path = pathlib.Path(line[8:]).with_suffix(".log")
                                path.write_text(path.read_text().replace(
                                    "scene=heavy event=insn cycles_per_step=99 value_per_step=22000",
                                    "scene=heavy event=insn cycles_per_step=99 value_per_step=18000"))
                    return code, lines, wall
            payload, summary = self.measure(root, OppositeCounters(root, shifted=True))
            for name, delta in (("quiet", 10), ("heavy", -10)):
                line = next(line for line in summary.splitlines() if line.startswith(f"| `suite/{name}`"))
                self.assertEqual(line.split("|")[8].strip(), str(delta))
                self.assertAlmostEqual(payload["rows"]["suite/"+name]["delta_insn"], delta)
            class NoCounters(FakeAutana):
                def __call__(self, command, log, timeout):
                    code, lines, wall = super().__call__(command, log, timeout)
                    if "status" not in command:
                        for _, line in lines:
                            if line.startswith("report: "):
                                path = pathlib.Path(line[8:]).with_suffix(".log")
                                if pathlib.Path(command[command.index("--project")+1]).name == "b":
                                    path.write_text("\n".join(line for line in path.read_text().splitlines()
                                                              if "xtperf" not in line))
                    return code, lines, wall
            _, summary = self.measure(root, NoCounters(root))
            line = next(line for line in summary.splitlines() if line.startswith("| `suite/quiet`"))
            self.assertEqual(line.split("|")[8].strip(), "n/a")

    def test_capture_exit_two_is_failure(self):
        class ExitTwo(FakeAutana):
            def __call__(self, command, log, timeout):
                code, lines, wall = super().__call__(command, log, timeout)
                return (2 if "suite" in command else code), lines, wall
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(RuntimeError, "two consecutive"):
                self.measure(root, ExitTwo(root))

    def test_shell_restore_uses_injected_autana(self):
        with tempfile.TemporaryDirectory() as root:
            root = pathlib.Path(root)
            perf = root/"launcher/tools/perf"
            perf.mkdir(parents=True)
            source = pathlib.Path(tool.__file__).parent
            for name in ("perf_compare.sh", "perf_compare.py", "layout_measure.py", "seed_statistics.py"):
                (perf/name).write_bytes((source/name).read_bytes())
            (root/"scripts/lib").mkdir(parents=True)
            (root/"scripts/lib/process_tree.py").write_bytes((source.parents[2]/"scripts/lib/process_tree.py").read_bytes())
            (root/"scripts/lib/device_capture.py").write_bytes((source.parents[2]/"scripts/lib/device_capture.py").read_bytes())
            helper = root/"launcher/tools/revision_worktree.sh"
            helper.write_text("revision_worktree_setup() { :; }\n"
                              "revision_worktree_cleanup() { :; }\n"
                              "revision_worktree_checkout() {\n"
                              "  if [ -d \"$1\" ]; then printf '%s\\n' \"$1\";\n"
                              "  else printf '%s\\n' \"$RESTORE_TREE\"; fi\n}\n")
            calls = root/"calls.jsonl"
            fake = root/"fake.py"
            fake.write_text("import json,pathlib,sys\n" + inspect.getsource(FakeAutana) + "\n" +
                            f"with open({str(calls)!r}, 'a') as file: file.write(json.dumps(sys.argv[1:])+'\\n')\n"
                            "if 'flash' in sys.argv: sys.exit(0)\n"
                            f"code,lines,wall = FakeAutana({str(root)!r})(sys.argv[1:], None, 60)\n"
                            "for at,line in lines: print(line, flush=True)\nsys.exit(code)\n")
            for name in ("a", "restore"):
                (root/name/"launcher/test").mkdir(parents=True)
                (root/name/"launcher/test/suites.h").write_bytes((source.parents[2]/"launcher/test/suites.h").read_bytes())
            env = dict(__import__("os").environ, RESTORE_TREE=(root/"restore").as_posix())
            done = subprocess.run(["sh", (perf/"perf_compare.sh").as_posix(),
                                   (root/"a").as_posix(), (root/"a").as_posix(),
                                   "--suite", "suite", "-", "-", "--max-seeds", "4",
                                   "--autana", f'"{pathlib.Path(sys.executable).as_posix()}" "{fake.as_posix()}"', "-o", (root/"out").as_posix()],
                                  env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(done.returncode, 0, done.stderr)
            recorded = [json.loads(line) for line in calls.read_text().splitlines()]
            self.assertEqual(sum("flash" in command for command in recorded), 1)
            self.assertTrue(any("status" in command for command in recorded))
            self.assertTrue(any("suite" in command for command in recorded))
            calls.unlink()
            done = subprocess.run(["sh", (perf/"perf_compare.sh").as_posix(),
                                   (root/"a").as_posix(), (root/"a").as_posix(),
                                   "--suite", "suite", "-", "-", "--alpha", "1e-8",
                                   "--autana", f'"{pathlib.Path(sys.executable).as_posix()}" "{fake.as_posix()}"'],
                                  env=env, capture_output=True, text=True, timeout=60)
            self.assertNotEqual(done.returncode, 0)
            self.assertFalse(calls.exists(), "invalid settings must not reach capture or restore")
            done = subprocess.run(["sh", (perf/"perf_compare.sh").as_posix(),
                                   (root/"a").as_posix(), (root/"a").as_posix(),
                                   "--suite", "suite", "x" * 24, "-", "--max-seeds", "4",
                                   "--autana", f'"{pathlib.Path(sys.executable).as_posix()}" "{fake.as_posix()}"'],
                                  env=env, capture_output=True, text=True, timeout=60)
            self.assertNotEqual(done.returncode, 0)
            self.assertFalse(calls.exists(), "invalid filters must not reach capture or restore")

    def test_shell_help_and_missing_arguments_are_messages(self):
        wrapper = pathlib.Path(tool.__file__).with_suffix(".sh")
        help_result = subprocess.run(["sh", wrapper.as_posix(), "-h"], capture_output=True, text=True)
        for option in ("--alpha", "--wait", "--autana", "A A"):
            self.assertIn(option, help_result.stdout)
        for arguments in (("A",), ("A", "B", "--suite", "name")):
            result = subprocess.run(["sh", wrapper.as_posix(), *arguments], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertTrue(result.stderr.strip())

    @patch.object(stats, "permutation_samples", new=lambda alpha: 199)
    def test_default_multilook_aa_smoke(self):
        def noisy_flash(args, seed, runner):
            rng = random.Random(seed + draw*2147483647)
            values = {name: round(100000*math.exp(rng.gauss(0, .15))) for name in ("quiet", "heavy")}
            record = record_for_rows(args, seed, values)
            for entry in record['suites'].values():
                for run in entry['runs']:
                    run['instructions'], run['inventory'] = {}, []
            return record
        false, draws = 0, 3
        looks = set()
        for draw in range(draws):
            with tempfile.TemporaryDirectory() as root:
                with patch.object(tool, "required_seeds", return_value=2), \
                        patch.object(tool, "run_flash", side_effect=noisy_flash), \
                        patch.object(tool, "compare", wraps=stats.compare) as decision:
                    result, _ = self.measure(root, None)
                self.assertTrue(all(call.args[3] == .05/4/2 for call in decision.call_args_list))
                false += any(row["verdict"] in ("regressed", "improved") for row in result["rows"].values())
                looks.update(item["look"] for item in result["plan"])
        self.assertGreaterEqual(len(looks), 3)
        self.assertLessEqual(false, draws*.05 + 3*math.sqrt(draws*.05*.95))
        print(f"Multi-look A/A false decisions: {false}/{draws} ({false/draws:.1%}); looks {sorted(looks)}")


if __name__ == "__main__":
    unittest.main()
