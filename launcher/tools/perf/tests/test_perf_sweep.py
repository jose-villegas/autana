"""Console sweeps exercised with an injected foreground runner."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import perf_sweep as tool

FIXTURE = Path(__file__).parent / "fixtures/sweep_capture.log"


class FakeAutana:
    def __init__(self, root, mismatch=False, failed=False):
        self.root = Path(root)
        self.calls = []
        self.build = "seeded-diag"
        self.mismatch = mismatch
        self.failed = failed

    def __call__(self, command, log, timeout):
        self.calls.append(command)
        lines = []
        code = 0
        if "status" in command:
            lines = ["free"]
        elif "buildid" in command:
            lines = ["BUILD_ID=" + self.build]
        elif "--flash" in command:
            project = Path(command[command.index("--project") + 1])
            build = project / "launcher/build.diag/build_id.txt"
            build.parent.mkdir(parents=True, exist_ok=True)
            build.write_text(self.build)
            lines = ["booted BUILD_ID=" + self.build]
        elif self.mismatch and "--set" in command:
            name, value = command[command.index("--set") + 1].split("=")
            lines = [f"device: SET {name} {value}: board holds TUNE_OK {name}=999"]
            code = 1
        elif "suite" in command:
            name = command[command.index("suite") + 1]
            capture = self.root / f"capture_{len(self.calls)}.log"
            capture.write_bytes(FIXTURE.read_bytes())
            capture.with_suffix(".md").write_text("- Ended: complete\n")
            lines = [f"batch: {name} run 1/1", "report: " + str(capture.with_suffix(".md"))]
            code = int(self.failed)
        for line in lines:
            log.write(line + "\n")
        return code, list(enumerate(lines)), 1


def arguments(root, seeds=(1,)):
    project = Path(root) / "project"
    header = project / "launcher/test/suites.h"
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text("#define SUITE_FILTER_LEN 24\n#define SUITE_FILTER_MAX 4\n")
    return tool.parse_args(["run", "--out", str(Path(root) / "out"),
                           "--build", "here=" + str(project), "--knob", "r3d_span.small_max_side=2,3,4",
                           "--knob", "r3d_span.flat_max_rows=1,2", "--runs", "2",
                           "--seeds", *map(str, seeds), "--rng-seed", "42",
                           "--suite", "suite", "counters", "-"])


class SweepTests(unittest.TestCase):
    def test_unknown_after_identity_discards_captures(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            fake = FakeAutana(root)

            def runner(command, log, timeout):
                if "buildid" in command and any("suite" in call for call in fake.calls):
                    log.write("no serial reply\n")
                    return 1, [(0, "no serial reply")], 1
                return fake(command, log, timeout)

            self.assertEqual(tool.run(args, runner), 1)
            saved = json.loads((args.out / "plan.json").read_text())
            self.assertTrue(all("metrics" not in capture for capture in saved["captures"]))

    def test_cross_build_single_seed_is_inconclusive(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            saved = tool.plan(args)
            saved["builds"].append(dict(saved["builds"][0], label="other"))
            for build in range(2):
                for round_index in range(16):
                    saved["captures"].append(dict(build=build, point=0, seed=1, round=round_index,
                        metrics={"suite/frame": 10000 - build * 500}, units={"suite/frame": "us"}))
            args.out.mkdir()
            (args.out / "plan.json").write_text(json.dumps(saved))
            result = tool.report(args.out)
            cell = result["rows"][len(saved["points"])]["metrics"]["suite/frame"]
            self.assertEqual(cell["verdict"], "inconclusive")
            self.assertIn("cannot separate", cell["reason"])

    def test_seeded_plan_visits_all_points_each_round(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root, (1, 2))
            schedules = []
            for rng_seed in (0, 1, 42):
                with self.subTest(rng_seed=rng_seed):
                    args.rng_seed = rng_seed
                    first = tool.plan(args)
                    self.assertEqual(first, tool.plan(args))
                    self.assertEqual(len(first["flashes"]), 2)
                    self.assertTrue(first["hot_tunables"])
                    for flash in first["flashes"]:
                        self.assertIn("--hot-tunables", flash["command"])
                        for order in flash["rounds"]:
                            self.assertEqual(sorted(order), list(range(6)))
                    schedules.append(first["flashes"])
            self.assertNotEqual(schedules[0], schedules[1])
            self.assertNotEqual(schedules[1], schedules[2])

    def test_all_metric_kinds(self):
        metrics, units = tool.metrics(FIXTURE, FIXTURE)
        self.assertEqual(metrics["frame"], 10000)
        self.assertEqual(metrics["r3d.draw.avg"], 1250)
        self.assertEqual(metrics["r3d.draw.worst"], 2000)
        self.assertEqual(metrics["spans.184x224.fill"], 40)
        self.assertEqual(metrics["r3d.draw.cycles"], 500)
        self.assertEqual(metrics["r3d.draw.icache_miss_stall"], 20)
        self.assertEqual(units["r3d.draw.cycles"], "cycles/call")

    def test_tune_mismatch_stops_and_reports(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            fake = FakeAutana(root, mismatch=True)
            self.assertEqual(tool.run(args, fake), 1)
            saved = json.loads((args.out / "plan.json").read_text())
            self.assertIn("=999", saved["error"])
            self.assertEqual(sum("suite" in call and "--flash" not in call for call in fake.calls), 1)
            self.assertTrue((args.out / "sweep.md").exists())

    def test_captures_and_two_failure_stop(self):
        for failed in (False, True):
            with self.subTest(failed=failed), tempfile.TemporaryDirectory() as root:
                args = arguments(root)
                fake = FakeAutana(root, failed=failed)
                self.assertEqual(tool.run(args, fake), int(failed))
                captures = [call for call in fake.calls if "suite" in call and "--flash" not in call]
                self.assertEqual(len(captures), 2 if failed else 12)
                self.assertTrue(all("--expect-build-id" in call for call in captures))
                self.assertEqual(sum("--flash" in call for call in fake.calls), 1)
                self.assertTrue(all("--hot-tunables" in call for call in fake.calls if "--flash" in call))
                self.assertTrue(all("--set" in call for call in captures))

    def test_report_units_verdicts_and_table(self):
        for seeds in ((1,), tuple(range(1, 17))):
            with self.subTest(seeds=seeds), tempfile.TemporaryDirectory() as root:
                args = arguments(root, seeds)
                args.runs = 16 if len(seeds) == 1 else 2
                saved = tool.plan(args)
                saved["captures"] = []
                for seed in seeds:
                    for round_index in range(args.runs):
                        for point in range(len(saved["points"])):
                            value = 10000 + (seed if len(seeds) > 1 else round_index)
                            factor = [1, .95, 1.05, 1, 1, 1][point]
                            saved["captures"].append(dict(build=0, point=point, seed=seed,
                                round=round_index, metrics={"suite/frame": value * factor},
                                units={"suite/frame": "us"}))
                args.out.mkdir()
                (args.out / "plan.json").write_text(json.dumps(saved))
                result = tool.report(args.out)
                self.assertEqual(result["unit"], "seed mean" if len(seeds) > 1 else "run")
                verdicts = [row["metrics"]["suite/frame"]["verdict"] for row in result["rows"][1:]]
                self.assertEqual(verdicts[:3], ["improved", "regressed", "no change"])
                table = (args.out / "sweep.md").read_text()
                self.assertIn("one per build per seed; knobs changed over the console", table)
                self.assertIn("suite/frame", table)
                self.assertIn("r3d_span.small_max_side=2", table)
                if len(seeds) == 1:
                    self.assertIn("layout floor was not measured", table)


if __name__ == "__main__":
    unittest.main()
