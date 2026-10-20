"""Frame reports accept board captures without skinning metadata."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[6]
TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path[:0] = [str(TOOLS), str(ROOT / "launcher/tools"), str(ROOT / "launcher/tools/perf"),
                str(ROOT / "scripts/device")]
import report_performance
from build.c_header import read_define
from device_report import build_budget_section, count_budget_rows


def profile(scene="probe", quarter="portrait"):
    header = ROOT / "launcher/test/frame_profile.h"
    token = lambda name: read_define(header, "FRAME_PROFILE_" + name)
    values = dict(MEAN=48333, MED=48890, P95=58515, WORST=59854, BEST=32574)
    return (f"I (134838) render_lab_profile: {token('PREFIX')} {scene} {token('FRAMES')}751 "
            + " ".join(f"{token(name)} {value}us" for name, value in values.items())
            + f" | {token('FPS')} {token('MEAN')} 20.69 {token('LOW')} 17.09 "
            + f"{token('WORST')} 16.71 {token('QUARTER')}{quarter}")


TARGET = "PERF TARGET probe portrait: measured 48333 us, goal 48000 us, distance +0%"
SUMMARY = ("I (1) sponza_perf: probe both cores: mean 48333us (20.7 fps before present), "
           "worst 59854us, med 48890us, p95 58515us quarter=portrait")


class FrameReports(unittest.TestCase):
    def test_profile_report_and_targets(self):
        text = profile() + "\n" + profile("other", "landscape") + "\n" + TARGET
        text += "\nPERF TARGET other landscape: measured 100 us, goal 200 us, distance -50%"
        report = report_performance.performance_report(text)
        self.assertIn("| Scene | Quarter | Frames | Mean fps | Low fps (1/p95) | p50 ms | p95 ms | Worst ms |", report)
        self.assertIn("| probe | portrait | 751 | 20.7 | 17.1 | 48.9 | 58.5 | 59.9 |", report)
        self.assertLess(report.index("| probe |"), report.index("| other |"))
        self.assertIn("| probe portrait | 48000 | 48333 | -0.7% | UNMET |", report)
        self.assertIn("| other landscape | 200 | 100 | +50.0% | MET |", report)
        self.assertEqual(count_budget_rows(report), 2)

    def test_frame_cost_report(self):
        report = report_performance.performance_report(SUMMARY + "\n" + TARGET)
        self.assertIn("Frame cost along the flythrough", report)
        self.assertIn("| probe | portrait | 20.7 | 48.3 | 48.9 | 58.5 | 59.9 |", report)
        self.assertEqual(count_budget_rows(report), 1)

    def test_device_budget_section(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = pathlib.Path(tmp) / "capture.log"
            text = profile() + "\n" + TARGET
            capture.write_text(text, encoding="utf-8")
            section = "\n".join(build_budget_section(ROOT, "run_render_lab_frame_profile_suite", capture, text))
            self.assertNotIn("unavailable", section)
            self.assertNotIn("zero rows", section)
            self.assertIn("Frame rate", section)

    def test_shared_parser_fields(self):
        from frame_profile import profiles, summaries, poses
        self.assertEqual(profiles(profile())[0], dict(row="probe", frames=751, mean=48333, med=48890,
                         p95=58515, worst=59854, best=32574, fps_mean=20.69, fps_low=17.09,
                         fps_worst=16.71, quarter="portrait"))
        self.assertEqual(summaries(SUMMARY)[0]["mean"], 48333)
        pose = "probe t=   500ms clusters= 123 tris= 456 | both cores: frame   52365us quarter=landscape"
        self.assertEqual(poses(pose)[0], dict(row="probe", time_ms=500, clusters=123, triangles=456,
                                           frame_us=52365, quarter="landscape"))

    def test_legacy_pose_seconds(self):
        from frame_profile import poses
        row = poses("probe t= 2s clusters=4 tris=5 | both cores: frame 51000us quarter=portrait")[0]
        self.assertEqual(row["time_ms"], 2000)

    def test_budget_rows(self):
        self.assertIn("| zero | 0 | 1 | n/a | UNMET |", report_performance.budget_row("zero", 0, 1))
        self.assertIn("| equal | 1 | 1 | +0.0% | MET |", report_performance.budget_row("equal", 1, 1))

    def test_generated_check_detects_edit(self):
        import frame_profile_tables as tables
        import shlex
        with tempfile.TemporaryDirectory() as tmp:
            data = pathlib.Path(tmp)
            document = data / "doc.md"
            document.write_text("<!-- generated: frame-timing-profiles -->\n<!-- /generated: frame-timing-profiles -->\n")
            for board in ("alpha", "beta"):
                (data / f"frame-profile-{board}-board.log").write_text(
                    f"BUILD_ID={board}-diag\n" + "\n".join(profile("gallery", q) for q in tables.QUARTERS))
            pose = data / "poses-gamma-board.log"
            pose.write_text("BUILD_ID=pose-diag\npose_every_ms=500\n" + "\n".join(
                f"gallery t={t}ms clusters=4 tris=5 | both cores: frame 51000us quarter={q}"
                for q in tables.QUARTERS for t in (0, 1000)))
            argv = ["--data-dir", str(data), "--document", str(document),
                    "--pose-capture", str(pose), "--pose-row", "gallery"]
            command = [sys.executable, str(pathlib.Path(tables.__file__)), *argv]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            body = document.read_text()
            self.assertIn("Cells list ALPHA / BETA", body)
            self.assertIn("on GAMMA", body)
            self.assertIn("sampled every 0.5 s", body)
            for build in ("alpha-diag", "beta-diag", "pose-diag"):
                self.assertIn(f"build `{build}`", body)
            self.assertNotIn("Regenerate with", body)
            marker = next(line for line in body.splitlines() if "check: " in line)
            self.assertIn(shlex.join(["python", tables.SCRIPT, *argv]), marker)
            result = subprocess.run([*command, "--check"], cwd=ROOT, capture_output=True)
            self.assertEqual(result.returncode, 0)
            document.write_text(body.replace("20.7 /", "99.9 /", 1))
            result = subprocess.run([*command, "--check"], cwd=ROOT, capture_output=True)
            self.assertEqual(result.returncode, 1)
            selected = tables.timing_tables(data, None, "gallery", ["beta"], pose, "custom")
            self.assertIn("Cells list BETA.", selected)
            self.assertIn("on CUSTOM", selected)
            self.assertNotIn("alpha-diag", selected)
            (data / "frame-profile-beta-board.log").write_text(profile("gallery"))
            with self.assertRaisesRegex(ValueError, "frame-profile-beta-board.log.*missing build id"):
                tables.timing_tables(data, None, "gallery", ["beta"], pose)


if __name__ == "__main__":
    unittest.main()
