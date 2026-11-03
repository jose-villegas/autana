"""Frame reports accept board captures without skinning metadata."""
import gzip
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
from render.cull_counts import fields
from device_report import build_budget_section, count_budget_rows


def profile(scene="probe", quarter="portrait", frames=751):
    header = ROOT / "launcher/test/frame_profile.h"
    token = lambda name: read_define(header, "FRAME_PROFILE_" + name)
    values = dict(MEAN=48333, MED=48890, P95=58515, WORST=59854, BEST=32574)
    return (f"I (134838) render_lab_profile: {token('PREFIX')} {scene} {token('FRAMES')}{frames} "
            + " ".join(f"{token(name)} {value}us" for name, value in values.items())
            + f" | {token('FPS')} {token('MEAN')} 20.69 {token('LOW')} 17.09 "
            + f"{token('WORST')} 16.71 {token('QUARTER')}{quarter}")


FRAME_PROFILE_HEADER = ROOT / "launcher/test/frame_profile.h"
FORMAT = lambda name: read_define(FRAME_PROFILE_HEADER, "FRAME_PROFILE_" + name) + " quarter=%s"
FRAME = read_define(FRAME_PROFILE_HEADER, "FRAME_PROFILE_TRACE_FRAME")
COUNTS = fields()
PATH_POSES = read_define(FRAME_PROFILE_HEADER, "FRAME_PROFILE_PATH_POSES")
PATH_MS = [0, 40, 80, 120]
FRAME_US = [40000, 52000, 61000, 45000]
PEAKS = [(80, 2), (40, 1)]  # (path ms, frame index), highest first
PROMINENCE_US = 9000
COUNT, PEAK_SET_UP = 100, 180
PATH_DRAW_MS, PEAK_DRAW_MS = 40.0, 47.5


def traced(scene, quarter):
    """A traced playback of `scene`: its profile, frames, frame peaks and their replays."""
    lines = [profile(scene, quarter, len(PATH_MS))]
    frames = "".join(FRAME % pair for pair in zip(PATH_MS, FRAME_US))
    lines.append(FORMAT("TRACE") % (scene, len(PATH_MS), frames, quarter))
    lines += [FORMAT("PEAK") % (scene, rank, t, FRAME_US[i], PROMINENCE_US, quarter)
              for rank, (t, i) in enumerate(PEAKS, 1)]
    def replay(t, draw, set_up):
        counts = " ".join(f"{name}={set_up if name == 'triangles_set_up' else COUNT}" for name in COUNTS)
        return FORMAT("REPLAY") % (scene, t, counts, f"r3d.cull 0.75/0.7  r3d.draw {draw:.2f}/{draw:.1f} | total 51.0",
                                   quarter)
    lines += [replay(i, PATH_DRAW_MS, COUNT) for i in range(PATH_POSES)]
    lines += [replay(t, PEAK_DRAW_MS, PEAK_SET_UP) for t, _ in PEAKS]
    return "\n".join(lines)


PIXELS = 1000
PEAK_TESTED = 4 * PIXELS  # past every path pose's layers


def overdraw_data(data_dir, scene, quarters):
    """The host overdraw of each of traced(scene)'s poses: each path pose tests one layer more than the one
    before, the frame peaks more than all of them."""
    import frame_peak_overdraw as views
    lines = ["\t".join(views.COLUMNS + views.FIELDS)]
    for quarter in quarters:
        poses = [(views.PATH_POSE, i, PIXELS + i) for i in range(PATH_POSES)]
        poses += [(str(rank), t, PEAK_TESTED) for rank, (t, _) in enumerate(PEAKS, 1)]
        for pose, t, tested in poses:
            counts = dict(pixels=PIXELS, tested=tested, written=PIXELS, tested_max=tested // PIXELS + 1, written_max=1)
            lines.append("\t".join(map(str, (scene, quarter, pose, t, *(counts[name] for name in views.FIELDS)))))
    (data_dir / views.DATA_FILE).write_text("\n".join(lines) + "\n", encoding="utf-8")


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
        text = profile() + "\n" + TARGET
        writers = {"capture.log": lambda path: path.write_text(text, encoding="utf-8"),
                   "capture.log.gz": lambda path: path.write_bytes(gzip.compress(text.encode("utf-8")))}
        for name, write in writers.items():
            with self.subTest(name), tempfile.TemporaryDirectory() as tmp:
                capture = pathlib.Path(tmp) / name
                write(capture)
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

    def test_traced_lines_parse_from_the_header_formats(self):
        from frame_profile import trace, peaks, replays
        text = traced("probe", "landscape")
        self.assertEqual(trace(text)[1], dict(row="probe", t_ms=PATH_MS[1], frame_us=FRAME_US[1], quarter="landscape"))
        self.assertEqual(len(trace(text)), len(PATH_MS))
        cut = text.replace(FRAME % (PATH_MS[-1], FRAME_US[-1]), "", 1)
        with self.assertRaisesRegex(ValueError, "probe landscape: a trace line holds"):
            trace(cut)
        lost = text.replace(f" n={len(PATH_MS)}", f" n={len(PATH_MS) - 1}", 1).replace(FRAME % (PATH_MS[-1], FRAME_US[-1]), "", 1)
        with self.assertRaisesRegex(ValueError, f"probe landscape: {len(PATH_MS) - 1} frames traced of the {len(PATH_MS)}"):
            trace(lost)
        self.assertEqual([row["rank"] for row in peaks(text)], list(range(1, len(PEAKS) + 1)))
        self.assertEqual({row["prominence_us"] for row in peaks(text)}, {PROMINENCE_US})
        pose = replays(text)[0]
        self.assertEqual(pose["counts"], {name: COUNT for name in COUNTS})
        self.assertEqual(pose["stages"], {"r3d.cull": 0.75, "r3d.draw": PATH_DRAW_MS})
        self.assertEqual(pose["quarter"], "landscape")

    def test_frame_peaks_name_the_stage_and_count_above_the_path(self):
        report = report_performance.performance_report(traced("probe", "portrait"))
        self.assertIn("## Frame peaks", report)
        row = next(line for line in report.splitlines() if line.startswith("| probe | portrait | 1 |"))
        self.assertIn(f"| {PEAKS[0][0] / 1000:.2f} | {FRAME_US[PEAKS[0][1]] / 1000:.1f} | {PROMINENCE_US / 1000:.1f} |", row)
        self.assertIn(f"r3d.draw +{PEAK_DRAW_MS - PATH_DRAW_MS:.1f} ms; triangles set up ×{PEAK_SET_UP / COUNT:.2f}", row)

    def test_a_second_run_for_the_heap_is_not_reported(self):
        first = traced("probe", "portrait")
        frames = read_define(FRAME_PROFILE_HEADER, "FRAME_PROFILE_FRAMES")
        second = first.replace(str(FRAME_US[PEAKS[0][1]]), "99999").replace(f"{frames}{len(PATH_MS)} ",
                                                                            f"{frames}752 ", 1)
        report = report_performance.performance_report("\n".join([first, second, traced("other", "portrait")]))
        self.assertIn(f"| probe | portrait | {len(PATH_MS)} |", report)
        self.assertNotIn("752", report)
        self.assertNotIn("100.0", report)
        self.assertEqual(report.count("| probe | portrait | 1 |"), 1)
        self.assertIn("| other | portrait | 1 |", report)

    def test_frame_peaks_fail_without_their_replays(self):
        text = traced("probe", "portrait")
        cut = text.rsplit("\n", 1)[0]
        with self.assertRaisesRegex(ValueError, "probe portrait: .* replays"):
            report_performance.performance_report(cut)

    def test_budget_rows(self):
        self.assertIn("| zero | 0 | 1 | n/a | UNMET |", report_performance.budget_row("zero", 0, 1))
        self.assertIn("| equal | 1 | 1 | +0.0% | MET |", report_performance.budget_row("equal", 1, 1))

    def test_frame_peak_overdraw_tables(self):
        import frame_peak_overdraw as views
        text = "\n".join(traced("gallery", q) for q in views.QUARTERS)
        with tempfile.TemporaryDirectory() as tmp:
            data = pathlib.Path(tmp)
            overdraw_data(data, "gallery", views.QUARTERS)
            body = views.tables(text, ["gallery"], data)
            t, frame = PEAKS[0]
            self.assertIn(f"| gallery | portrait | 1 | {t / 1000:.2f} | {FRAME_US[frame] / 1000:.1f} | "
                          f"{PEAK_TESTED / PIXELS:.2f} | 1.00 | {PEAK_TESTED // PIXELS + 1} | 1 | 100 |", body)
            path_median = (PIXELS + (PATH_POSES - 1) / 2) / PIXELS
            # Every path pose replays in the same ms, so neither correlation is drawn.
            self.assertIn(f"| gallery | landscape | {path_median:.2f} | {PEAK_TESTED / PIXELS:.2f} | 100 | – | – | – |",
                          body)
            # A row the renderer cannot draw by name (a take) has no data and is left out, not missing.
            take = "\n".join(traced("gallery-take", q) for q in views.QUARTERS)
            self.assertNotIn("| gallery-take |", views.tables(text + "\n" + take, ["gallery", "gallery-take"], data))
            overdraw_data(data, "gallery", views.QUARTERS[:1])
            with self.assertRaisesRegex(ValueError, f"{len(PEAKS) + PATH_POSES} poses missing and 0 not in"):
                views.tables(text, ["gallery"], data)

    def test_overdraw_follows_the_ranked_correlation(self):
        import frame_peak_overdraw as views
        self.assertEqual(views.spearman([1, 2, 3], [10, 30, 20]), 0.5)
        self.assertIsNone(views.spearman([1, 1, 1], [1, 2, 3]))

    def test_generated_check_detects_edit(self):
        import frame_profile_tables as tables
        import shlex
        with tempfile.TemporaryDirectory() as tmp:
            data = pathlib.Path(tmp)
            document = data / "doc.md"
            images = ROOT / tables.DOCUMENT.rsplit("/", 1)[0] / "images" / f"test-{data.name}"
            document.write_text("<!-- generated: frame-timing-profiles -->\n<!-- /generated: frame-timing-profiles -->\n")
            (data / "frame-profile-north-board.log").write_text(
                "BUILD_ID=north-diag\n" + "\n".join(profile("gallery", q) for q in tables.QUARTERS))
            (data / "frame-profile-south-board.log").write_text(
                "BUILD_ID=south-diag\n" + "\n".join(traced("gallery", q) for q in tables.QUARTERS))
            overdraw_data(data, "gallery", tables.QUARTERS)
            argv = ["--data-dir", str(data), "--document", str(document), "--peak-board", "south",
                    "--images", str(images)]
            command = [sys.executable, str(pathlib.Path(tables.__file__)), *argv]
            try:
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                self.assertTrue((images / "gallery.png").is_file())
            finally:
                for image in images.glob("*.png"):
                    image.unlink()
                if images.exists():
                    images.rmdir()
            body = document.read_text(encoding="utf-8")
            self.assertIn("Cells list board 1 / board 2", body)
            self.assertIn("Board 2. Each scene's played frames", body)
            self.assertIn(f"](images/{images.name}/gallery.png)", body)
            self.assertIn("| gallery | landscape | 1 |", body)
            self.assertIn("- `frame-peak-overdraw.tsv`", body)
            prose = body.split("-->", 1)[1].split("Sources:")[0]
            for private in ("north", "south"):
                self.assertNotIn(private, prose.lower())
            self.assertNotIn("-diag", body.split("-->", 1)[1])
            marker = next(line for line in body.splitlines() if "check: " in line)
            self.assertIn(shlex.join(["python", tables.SCRIPT, *argv]), marker)
            result = subprocess.run([*command, "--check"], cwd=ROOT, capture_output=True)
            self.assertEqual(result.returncode, 0)
            self.assertFalse(images.exists())
            document.write_text(body.replace("20.7 /", "99.9 /", 1), encoding="utf-8")
            result = subprocess.run([*command, "--check"], cwd=ROOT, capture_output=True)
            self.assertEqual(result.returncode, 1)
            with self.assertRaisesRegex(ValueError, "no frame-profile-north-board.log"):
                tables.timing_tables(data, None, ["south"], "north", images, False)
            with self.assertRaisesRegex(ValueError, "no frame peaks for gallery"):
                tables.timing_tables(data, None, None, "north", images, False)


if __name__ == "__main__":
    unittest.main()
