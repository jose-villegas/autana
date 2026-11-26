"""Replay compressed captures and target-only suites through perf acquisition."""
import contextlib
import gzip
import io
import json
from pathlib import Path
import tempfile
import unittest

from test_perf_compare import FakeAutana as CompareAutana, arguments as compare_arguments
from test_perf_sweep import FakeAutana as SweepAutana, arguments as sweep_arguments
import perf_sweep
from layout_measure import capture_metadata, parse_report, run_flash
from capture import require_same_build

FIXTURES = Path(__file__).parent / "fixtures"


class CaptureReplay(unittest.TestCase):
    def test_compressed_capture_replays_in_compare_and_sweep(self):
        for kind in ("compare", "sweep"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as root:
                fake = CompareAutana(root) if kind == "compare" else SweepAutana(root)

                def runner(command, log, timeout):
                    result = fake(command, log, timeout)
                    for _, line in result[1]:
                        if line.startswith("report: "):
                            path = Path(line.removeprefix("report: ")).with_suffix(".log")
                            with gzip.open(str(path) + ".gz", "wb") as stream:
                                stream.write(path.read_bytes())
                            path.unlink()
                    return result

                with contextlib.redirect_stdout(io.StringIO()):
                    if kind == "compare":
                        args = compare_arguments(root)
                        args.project, args.runs = args.project_a, 2
                        record = run_flash(args, 3, runner)
                        runs = record["suites"]["suite"]["runs"]
                        self.assertTrue(all(run["capture"].endswith(".log.gz") for run in runs))
                        self.assertTrue(all(run["rows"] == {"quiet": 10000, "heavy": 10000} for run in runs))
                        self.assertTrue(all(run["owners"]["quiet"] == "test_quiet" for run in runs))
                        self.assertTrue(all(run["instructions"]["quiet"] == 20000 for run in runs))
                    else:
                        args = sweep_arguments(root)
                        self.assertEqual(perf_sweep.run(args, runner), 0)
                        runs = json.loads((args.out / "plan.json").read_text())["captures"]
                        self.assertTrue(runs)
                        self.assertTrue(all(run["capture"].endswith(".log.gz") for run in runs))
                        self.assertTrue(all(run["metrics"]["suite/frame"] == 10000 for run in runs))

    def test_stress_capture_and_report_share_target_rows(self):
        expected = {"stress-stack portrait": 10750, "stress-stack portrait r3d.draw": 7864,
                    "stress-new portrait": 900}
        for name in ("stress_capture.txt", "stress_report.md"):
            with self.subTest(name=name):
                path = FIXTURES / name
                for compressed in (False, True):
                    with self.subTest(compressed=compressed), tempfile.TemporaryDirectory() as root:
                        replay = path
                        if compressed:
                            replay = Path(root) / (path.name + ".gz")
                            with gzip.open(replay, "wb") as stream:
                                stream.write(path.read_bytes())
                        self.assertEqual(parse_report(replay), expected)
                        self.assertEqual(perf_sweep.metrics(replay, replay)[0], expected)
        owners, _ = capture_metadata(FIXTURES / "stress_capture.txt", expected)
        self.assertEqual(owners, {name: "test_stress_stack" for name in expected})

    def test_compressed_build_identity(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "capture.log.gz"
            with gzip.open(path, "wt") as stream:
                stream.write("BUILD_ID=seeded-diag\n")
            self.assertEqual(require_same_build([path]), "seeded-diag")
