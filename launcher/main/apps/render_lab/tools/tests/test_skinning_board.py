"""Board budgets use the last measurements in each run and medians across runs."""
import pathlib
import sys
import unittest
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import report_performance
from report_performance import (PHASES, PHASE_LOADED, PHASE_PLAYING, PREFIX, STAGE_ANIMATE,
                                   STAGE_ANIMATION_STEP, STAGE_RENDER, STAGE_RENDER_DELTA,
                                   STAGE_SKIN, CORES_ONE, CORES_TWO, CORES_UNKNOWN, NANOSECONDS_PER_MICROSECOND, budget, parse_capture)


def run_capture(number, mean):
    lines = [f"RUN={number}"]
    for phase in PHASES:
        lines.append(f"{PREFIX} phase={phase} internal_free={mean} internal_largest={mean}")
    for stage in (STAGE_SKIN, STAGE_ANIMATE):
        for cores in (CORES_ONE, CORES_TWO):
            lines.append(f"{PREFIX} stage={stage} cores={cores} samples=100 vertices=800 worker_samples={100 if cores == CORES_TWO else 0} "
                         f"mean_us={mean} us_per_vertex={mean / 800:.3f}")
    lines.append(f"{PREFIX} stage={STAGE_ANIMATION_STEP} cores={CORES_UNKNOWN} samples=100 vertices=800 mean_us={mean}")
    for paused in (0, 1):
        lines.append(f"{PREFIX} stage={STAGE_RENDER} paused={paused} samples=100 mean_us={mean - paused}")
    lines.append(f"{PREFIX} stage={STAGE_RENDER_DELTA} mean_us=1")
    lines = [line + " joints=22 influences=2 lighting=0 lights=1" if f"stage={STAGE_SKIN} " in line else line
             for line in lines]
    return "\n".join(lines) + "\n"


class SkinningBoard(unittest.TestCase):
    def test_check_exit_status_and_byte_preservation(self):
        with tempfile.TemporaryDirectory() as temp:
            capture = pathlib.Path(temp) / "capture.txt"
            document = pathlib.Path(temp) / "budget.md"
            capture.write_text("BUILD_ID=sample-diag\n" + run_capture(1, 100), encoding="utf-8")
            stale = b"Before\r\n<!-- generated: skinning-budget -->\r\nstale\r\n<!-- /generated: skinning-budget -->\r\nAfter\r\n"
            document.write_bytes(stale)
            argv = ["report_performance", str(capture), str(document)]
            with patch.object(sys, "argv", [*argv, "--check"]):
                self.assertNotEqual(report_performance.main(), 0)
            self.assertEqual(document.read_bytes(), stale)
            with patch.object(sys, "argv", argv):
                self.assertEqual(report_performance.main(), 0)
            matching = document.read_bytes()
            self.assertNotEqual(matching, stale)
            with patch.object(sys, "argv", [*argv, "--check"]):
                self.assertEqual(report_performance.main(), 0)
            self.assertEqual(document.read_bytes(), matching)

    def test_distinct_cells_and_lighting_names(self):
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 100)
        lines = []
        for line in text.splitlines():
            if "phase=" in line:
                index = PHASES.index(line.split("phase=")[1].split()[0])
                line = line.replace("internal_free=100 internal_largest=100",
                                    f"internal_free={1100 + index * 100} internal_largest={1050 + index * 100}")
            elif "stage=" in line:
                stage = line.split("stage=")[1].split()[0]
                if stage in (STAGE_ANIMATE, STAGE_SKIN):
                    core = line.split("cores=")[1].split()[0]
                    mean = {(STAGE_ANIMATE, CORES_ONE): 160, (STAGE_ANIMATE, CORES_TWO): 240,
                            (STAGE_SKIN, CORES_ONE): 320, (STAGE_SKIN, CORES_TWO): 400}[(stage, core)]
                    line = line.replace("mean_us=100", f"mean_us={mean}").replace("us_per_vertex=0.125", f"us_per_vertex={mean / 800:.3f}")
                elif stage == STAGE_ANIMATION_STEP:
                    line = line.replace("mean_us=100", "mean_us=480")
                elif stage == STAGE_RENDER:
                    line = line.replace("mean_us=100", "mean_us=700").replace("mean_us=99", "mean_us=500")
                else:
                    line = line.replace("mean_us=1", "mean_us=200")
            lines.append(line)
        text = "\n".join(lines) + "\n"
        mode = next(mode for mode in report_performance.LIGHTING_MODES if mode != 0)
        text = text.replace("lighting=0", f"lighting={mode}")
        for lights in (1, 2):
            with self.subTest(lights=lights):
                rendered = budget(text.replace("lights=1", f"lights={lights}"))
                for row in (
                    "| Animate: sample, blend, palette | 160 \u00b5s (0.200 \u00b5s / vertex) | 240 \u00b5s (0.300 \u00b5s / vertex) |",
                    "| Skin and light | 320 \u00b5s (0.400 \u00b5s / vertex) | 400 \u00b5s (0.500 \u00b5s / vertex) |",
                    "| Whole animation step | 480 \u00b5s (0.600 \u00b5s / vertex) |",
                    "| Scene render, playing minus paused | +200 \u00b5s (700 vs 500 \u00b5s) |",
                    "| Before the scene loads | 1100 / 1050 |",
                    "| Scene loaded | 1200 / 1150 |",
                    "| Scene playing | 1300 / 1250 |",
                    f"{report_performance.LIGHTING_MODES[mode]} lighting, {lights} light{'s' if lights != 1 else ''}.",
                ):
                    self.assertIn(row, rendered)

    def test_malformed_captures_name_the_refusal_cause(self):
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 100)
        cases = [
            (text.replace("lighting=0", "lighting=999"), "invalid lighting"),
            (text + "RUN=1\n", "duplicate run"),
            (text + "BUILD_ID=other\n", "repeated"),
            (text + "BUILD_ID=sample-diag\n", "repeated"),
            (text.replace("sample-diag", "bad id"), "invalid"),
            (text.replace("RUN=1\n", ""), "no run"),
            (text.replace("vertices=800", "vertices=801", 1), "inconsistent vertices or samples"),
            (text.replace("samples=100", "samples=101", 1), "inconsistent vertices or samples"),
        ]
        for field, old, bad_values in (("joints", 22, (0, -1)), ("influences", 2, (0, -1)), ("lights", 1, (-1,))):
            cases.extend((text.replace(f"{field}={old}", f"{field}={value}"), f"invalid {field}") for value in bad_values)
        for stage, core in report_performance.STAGES:
            lines = [line for line in text.splitlines() if not (f"stage={stage} " in line and f"cores={core} " in line)]
            cases.append(("\n".join(lines), f"missing {stage} {core}"))
        for field, value in (("mean_us", 100), ("vertices", 800), ("samples", 100),
                             ("internal_free", 100), ("internal_largest", 100),
                             ("worker_samples", 100), ("joints", 22), ("influences", 2), ("lighting", 0), ("lights", 1)):
            cases.append((text.replace(f"{field}={value}", f"{field}=bad"), f"invalid {field}"))
        for capture, cause in cases:
            with self.subTest(cause=cause), self.assertRaisesRegex(ValueError, cause):
                parse_capture(capture)

    def test_partial_measurements_render_run_counts_only_when_needed(self):
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 100) + run_capture(2, 900)
        self.assertNotIn("of 2 runs", budget(text))
        for key in (*report_performance.STAGES, *report_performance.RENDER_KEYS):
            stage, slot = key
            field = "cores" if key in report_performance.STAGES else "paused"
            partial = "\n".join(line for line in text.splitlines()
                                if not (f"stage={stage} " in line and (not slot or f"{field}={slot} " in line)
                                        and ("mean_us=900" in line or "mean_us=899" in line
                                             or stage == STAGE_RENDER_DELTA)))
            if stage == STAGE_RENDER_DELTA:
                partial = text.replace(f"{PREFIX} stage={stage} mean_us=1\n", "", 1)
            with self.subTest(key=key):
                self.assertIn(f"{stage}, {slot} core(s): 1 of 2 runs." if key in report_performance.STAGES
                              else f"{stage}, paused={slot}: 1 of 2 runs.", budget(partial))

    def test_rendered_units_and_setup(self):
        capture = "BUILD_ID=sample-diag\n" + run_capture(1, 1600)
        rendered = budget(capture)
        self.assertIn("µs", rendered)
        self.assertNotIn("Â", rendered)
        self.assertIn("800 vertices, 22 joints", rendered)
        self.assertIn("2 influences, direct lighting, 1 light", rendered)
        self.assertIn("above budget on both cores", rendered)

    def test_two_cores_require_every_sample_on_worker(self):
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 100)
        for workers in (0, 99):
            with self.subTest(workers=workers), self.assertRaisesRegex(ValueError, f"ran {workers} of 100"):
                parse_capture(text.replace("worker_samples=100", f"worker_samples={workers}", 1))
        with self.assertRaisesRegex(ValueError, "worker_samples"):
            parse_capture(text.replace("worker_samples=100", "", 1))

    def test_frame_totals_use_difference_of_medians(self):
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 100) + run_capture(2, 900) + run_capture(3, 300)
        text = text.replace("paused=1 samples=100 mean_us=99", "paused=1 samples=100 mean_us=100")
        text = text.replace("paused=1 samples=100 mean_us=299", "paused=1 samples=100 mean_us=50")
        rendered = budget(text)
        self.assertIn("| As a frame runs it (worker use not recorded) | Per frame |", rendered)
        self.assertIn("| Scene render, playing minus paused | +200 µs (300 vs 100 µs) |", rendered)

    def test_required_setup_fields_are_loud(self):
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 100)
        for field, value in (("joints", 22), ("influences", 2), ("lighting", 0), ("lights", 1)):
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, field):
                parse_capture(text.replace(f" {field}={value}", ""))

    def test_setup_must_agree_between_skin_measurements(self):
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 100) + run_capture(2, 100)
        with self.assertRaisesRegex(ValueError, "inconsistent joints"):
            parse_capture(text.replace("joints=22", "joints=23", 1))

    def test_verdict_compares_each_core_with_owner_budget(self):
        from unittest.mock import patch
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 800)
        text = text.replace(f"stage={STAGE_SKIN} cores={CORES_ONE} samples=100 vertices=800 worker_samples=0 mean_us=800 us_per_vertex=1.000",
                            f"stage={STAGE_SKIN} cores={CORES_ONE} samples=100 vertices=800 worker_samples=0 mean_us=1600 us_per_vertex=2.000")
        for limit, expected in ((0.5, "above budget on both cores"),
                                (1, "within budget on two cores; above budget on one core"),
                                (2, "within budget on both cores")):
            with self.subTest(limit=limit), patch("report_performance.BUDGET_NS_PER_VERTEX",
                                                limit * NANOSECONDS_PER_MICROSECOND):
                self.assertIn(expected, budget(text))
                self.assertIn(f"budget is {limit:g} µs", budget(text))

    def test_several_runs_use_median_not_mean(self):
        result = parse_capture("BUILD_ID=sample-diag\n" + run_capture(1, 100)
                               + run_capture(2, 900) + run_capture(3, 300))
        self.assertEqual(result["build"], "sample-diag")
        self.assertEqual(result["run_count"], 3)
        self.assertEqual(result["stages"][(STAGE_SKIN, CORES_ONE)], 300)
        self.assertEqual(result["per_vertex"][(STAGE_SKIN, CORES_ONE)], 0.375)
        self.assertEqual(result["phases"][PHASE_PLAYING], (300, 300))
        self.assertEqual(result["render"], (300, 299, 1))

    def test_last_phase_and_stage_in_each_run_win(self):
        result = parse_capture("BUILD_ID=sample-diag\n" + run_capture(1, 100)
                               + f"{PREFIX} phase={PHASE_LOADED} internal_free=200 internal_largest=150\n"
                               + f"{PREFIX} stage={STAGE_SKIN} cores={CORES_ONE} samples=100 vertices=800 "
                                 "mean_us=400 us_per_vertex=0.500 joints=22 influences=2 lighting=0 lights=1\n")
        self.assertEqual(result["phases"][PHASE_LOADED], (200, 150))
        self.assertEqual(result["stages"][(STAGE_SKIN, CORES_ONE)], 400)

    def test_missing_stage_is_loud_when_no_run_logged_it(self):
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 300) + run_capture(2, 300)
        text = text.replace(f"{PREFIX} stage={STAGE_ANIMATE} cores={CORES_TWO} samples=100 vertices=800 worker_samples=100 mean_us=300 us_per_vertex=0.375\n", "")
        with self.assertRaisesRegex(ValueError, f"{STAGE_ANIMATE}.*{CORES_TWO}"):
            parse_capture(text)

    def test_partial_stage_uses_only_logged_runs_and_counts_them(self):
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 100) + run_capture(2, 900) + run_capture(3, 300)
        text = text.replace(f"{PREFIX} stage={STAGE_RENDER} paused=0 samples=100 mean_us=900\n", "")
        result = parse_capture(text)
        self.assertEqual(result["render"][0], 200)
        self.assertEqual(result["render_counts"], (2, 3, 3))

    def test_missing_phase_and_build_fail(self):
        for report in (parse_capture, report_performance.performance_report):
            with self.subTest(report=report.__name__), self.assertRaisesRegex(ValueError, "build"):
                report(run_capture(1, 100))
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 100)
        with self.assertRaisesRegex(ValueError, PHASE_PLAYING):
            parse_capture(text.replace(f"{PREFIX} phase={PHASE_PLAYING} internal_free=100 internal_largest=100\n", ""))


if __name__ == "__main__":
    unittest.main()
