"""Exercise scorecard capture provenance, revision joins, image pixels and part geometry."""
import csv
import gzip
import json
import pathlib
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import numpy as np
from PIL import Image

TOOLS = pathlib.Path(__file__).resolve().parents[1]
REPO = TOOLS.parents[1]
sys.path.insert(0, str(TOOLS / "render" / "scorecard"))

import captures
import frame_profile
import report
import scorecard
from build.c_header import read_define
from capture import QUARTERS
from r3d.objects.part_shape import measure_parts, shape_measure

PORTRAIT, LANDSCAPE = QUARTERS


def profile(row="sponza", quarter=PORTRAIT):
    return (f"frame_profile: {row} frames=3 mean 47205us med 47828us p95 58351us "
            f"worst 60334us best 30837us | fps mean 21.18 low 17.14 worst 16.57 quarter={quarter}")


def heap(row="sponza", quarter=PORTRAIT, free=4096, largest=2048):
    return f"frame_heap: {row} free={free} largest={largest} quarter={quarter}"


def part(material, name, iou, distance):
    return dict(material=material, part=name, source_object="wall", iou=iou, hausdorff=distance)


def revision(name, **scenes):
    return dict(revision=name, scenes=scenes)


class CaptureTests(unittest.TestCase):
    def test_heap_reads_size_t_format_and_preserves_other_patterns(self):
        header = frame_profile.HEADER
        template = read_define(header, "FRAME_PROFILE_HEAP")
        self.assertEqual(template.count("%zu"), 2)
        line = (template.replace("%zu", "%u") + frame_profile.QUARTER_FORMAT) % ("sponza", 8192, 6144, LANDSCAPE)
        self.assertEqual(frame_profile.first_frame_heaps(line),
                         [dict(row="sponza", quarter=LANDSCAPE, free=8192, largest=6144)])
        with self.assertRaisesRegex(ValueError, "format fields differ"):
            frame_profile._log_pattern("FRAME_PROFILE_HEAP", (("row", r"\S+"),))

        summary = "sponza both cores: mean 12us (2.0 fps before present), worst 13us, med 11us, p95 12us"
        peak = "frame_peak: sponza rank=1 t=62ms frame=61us prominence=33us"
        replay = "frame_replay: sponza t=62ms tris=52 | mean 1.234 ms p95 2.000 ms"
        text = "\n".join(f"{line} quarter={PORTRAIT}" for line in (summary, peak, replay))
        [parsed] = frame_profile.profiles(profile())
        self.assertEqual((parsed["row"], parsed["p95"], parsed["quarter"]), ("sponza", 58351, PORTRAIT))
        [parsed] = frame_profile.summaries(text)
        self.assertEqual((parsed["row"], parsed["p95"], parsed["quarter"]), ("sponza", 12, PORTRAIT))
        [parsed] = frame_profile.peaks(text)
        self.assertEqual((parsed["t_ms"], parsed["prominence_us"]), (62, 33))
        [parsed] = frame_profile.replays(text)
        self.assertEqual((parsed["t_ms"], parsed["counts"]), (62, {"tris": 52}))

    def test_each_quarter_keeps_its_own_first_run_heap(self):
        rows = [("sponza", PORTRAIT, 4096), ("bistro", LANDSCAPE, 6144), ("sponza", LANDSCAPE, 8192),
                ("sponza", PORTRAIT, 9999), ("bistro", PORTRAIT, None)]
        lines = [line for row, quarter, free in rows
                 for line in ([profile(row, quarter), heap(row, quarter, free)] if free else [profile(row, quarter)])]
        seen = captures.capture_rows("job-a", "\n".join(lines))
        self.assertEqual({(r["row"], r["quarter"]): r["heap"] for r in seen},
                         {(row, quarter): dict(free=free, largest=2048) if free else None
                          for row, quarter, free in rows if free != 9999})
        self.assertTrue(all(r["label"] == "job-a" for r in seen))
        [missing] = captures.capture_rows("job-b", "\n".join((profile(), profile(), heap())))
        self.assertIsNone(missing["heap"])

    def test_board_validation_missing_profiles_and_gzip(self):
        with self.assertRaisesRegex(ValueError, r"job-a: expected board A308.*20E8"):
            captures.capture_rows("job-a", "BOARD=20E8\n" + profile())
        with tempfile.TemporaryDirectory() as folder:
            plain, zipped = (pathlib.Path(folder) / file for file in ("capture.log", "capture.log.gz"))
            plain.write_text(profile(), encoding="utf-8")
            zipped.write_bytes(gzip.compress(plain.read_bytes()))
            self.assertEqual(captures.labeled_capture("job-a", plain), captures.labeled_capture("job-a", zipped))
            self.assertEqual(len(captures.labeled_capture("job-a", plain)), 1)
            plain.write_text("BOARD=A308\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "job-a: no frame profiles"):
                captures.labeled_capture("job-a", plain)

    def test_goals_and_ceilings_follow_named_quarters_in_the_capture(self):
        cases = [("sponza", LANDSCAPE, 51, 62, "under its ceiling"),
                 ("sponza", PORTRAIT, 71, 82, "over its ceiling"),
                 ("bistro", PORTRAIT, 91, 102, "board ceiling"),
                 ("bistro", LANDSCAPE, 111, None, "")]
        lines = []
        for row, quarter, goal, ceiling, verdict in cases:
            name = f"{row} {quarter}"
            lines.extend((profile(row, quarter), f"device_tests: PERF TARGET {name}: measured 10 us, goal {goal} us, distance -5%"))
            if ceiling is not None:
                suffix = " not enforced" if verdict == "board ceiling" else ""
                lines.append(f"device_tests: {name}: 20 us, {verdict} {ceiling} us{suffix}")
        lines.extend((profile(), "device_tests: PERF TARGET sponza portrait: measured 2 us, goal 99999 us, distance -5%"))
        seen = captures.capture_rows("job-a", "\n".join(lines))
        self.assertEqual([(r["row"], r["quarter"], r["goal"], r["ceiling"]) for r in seen],
                         [(row, quarter, goal, ceiling) for row, quarter, goal, ceiling, _ in cases])

    def test_real_board_log_supplies_sponza_and_bistro_goals(self):
        path = REPO / "docs/render/data/frame-profile-1-board.log"
        rows = captures.labeled_capture("board", path)
        self.assertEqual({(r["row"], r["quarter"]): (r["goal"], r["ceiling"])
                          for scene in ("sponza", "bistro") for r in captures.scene_captures(rows, scene)},
                         {("sponza", PORTRAIT): (48826, None), ("sponza", LANDSCAPE): (46066, None),
                          ("bistro", PORTRAIT): (36375, None), ("bistro", LANDSCAPE): (38062, None)})


class ReportTests(unittest.TestCase):
    def test_tables_cover_one_arm_budgets_heap_and_escaped_cells(self):
        row = dict(row="sponza", quarter=PORTRAIT, mean=1500, p95=2500)
        readings = [dict(row, label="job|a\ntrial", heap=None, goal=1800, ceiling=2700),
                    dict(row, label="job-b", heap=dict(free=5000, largest=3000), goal=1900, ceiling=None)]
        p = revision("parent", sponza=dict(captures=readings, sky=dict(holed=3, frames=10), pack_bytes=12345,
                                           held_out=dict(mean=2.0, sets=["probe", "path"], views=3)))
        t = revision("tip", sponza=dict(sky=dict(holed=4, frames=11, ceiling=5)))
        for parent, tip in ((p, t), (t, p)):
            text = report.markdown(parent, tip)
            self.assertIn("sponza · portrait", text)
            self.assertIn(r"not logged (job\|a trial)<br>5,000 / 3,000 B (job-b)", text)
            self.assertIn("mean goal 1.800 ms, p95 ceiling 2.700 ms", text)
            self.assertIn("mean goal 1.900 ms |", text)
            self.assertIn("12,345", text)
            self.assertIn("Held-out mean dE76", text)
            self.assertIn("2.000 (probe, path · 3 views)", text)
        self.assertIn("| Sky holed frames | 3/10 | 4/11 (ceiling 5) |", report.markdown(p, t))
        self.assertIn("| Pack bytes | 12,345 | not measured |", report.markdown(p, t))
        self.assertIn("| Pack bytes | not measured | 12,345 |", report.markdown(t, p))

    def test_parts_show_minimum_iou_maximum_distance_and_worst_first(self):
        parent = dict(parts=[part("stone", "parent", 0.9, 0.1), part("wood", "only", 0.8, 0.4)])
        tip = dict(parts=[part("stone", "a", 0.5, 0.1), part("stone", "b", 0.6, 0.3),
                          part("glass", "c", 0.2, None)])
        self.assertEqual(report.part_classes([parent, tip]), [
            "| glass | not measured | 0.2000 / — (1 undefined) |",
            "| stone | 0.9000 / 0.10000 | 0.5000 / 0.30000 |",
            "| wood | 0.8000 / 0.40000 | not measured |",
        ])

    def test_writes_parts_csv_and_sheet_only_for_available_inputs(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            Image.new("RGB", (2, 2)).save(root / "pose.png")
            p = revision("p", sponza=dict(parts=[part("wood", "z", 0.8, None), part("wood", "a", 0.9, 0.1)],
                                             images=["pose.png"]),
                         bistro=dict(images=["pose.png"]))
            t = revision("t", sponza=dict(parts=[part("wood", "a", 0.5, 0.2)], images=["pose.png"]))
            report.write_report(root, [p, t], [root, root])
            with (root / "sponza-parts.csv").open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual([(r["part"], r["parent_hausdorff"], r["tip_iou"]) for r in rows],
                             [("a", "0.1", "0.5"), ("z", "", "")])
            self.assertTrue((root / "sponza.png").is_file())
            self.assertFalse((root / "bistro.png").exists())
            self.assertFalse((root / "bistro-parts.csv").exists())

    def test_overlay_spec_and_mask_count(self):
        for spec in ("report", ":nearest_upscale", "report:", "report:UPSCALE"):
            with self.subTest(spec=spec), self.assertRaises(ValueError):
                report.overlay_callable(spec)
        self.assertIs(report.overlay_callable("report:nearest_upscale"), report.nearest_upscale)
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            Image.new("RGB", (2, 2)).save(root / "pose.png")
            arm = dict(images=["pose.png"], poses=[[]], size=[2, 2], path="scene.toml")
            for masks in ([], [np.ones((2, 2))] * 2):
                with self.assertRaises(ValueError):
                    report.write_sheet(root / "sheet.png", [arm, arm], [root, root], lambda *_: masks, root)

    def test_nearest_upscale_copies_whole_pixels(self):
        pixels = np.array([[[17, 34, 51], [68, 85, 102]]], dtype=np.uint8)
        actual = report.nearest_upscale(Image.fromarray(pixels), 2)
        np.testing.assert_array_equal(actual, pixels.repeat(2, axis=0).repeat(2, axis=1))

    def test_overlay_applies_only_the_masked_fraction(self):
        pixels = Image.new("RGB", (3, 1), "black")
        actual = report.tint_overlay(pixels, [[0, 0.5, 1]], (200, 100, 0), 0.5)
        self.assertEqual(list(actual.getdata()), [(0, 0, 0), (50, 25, 0), (100, 50, 0)])
        for invalid in ([[0]], [[0, 1, float("nan")]], [[0, 1, 2]]):
            with self.assertRaises(ValueError):
                report.tint_overlay(pixels, invalid, (200, 100, 0), 0.5)


class CliTests(unittest.TestCase):
    def test_capture_label_and_path_are_both_required(self):
        for spec in ("=capture.log", "job=", "capture.log"):
            with self.assertRaisesRegex(ValueError, "LABEL=PATH"):
                scorecard.parse_capture(spec)

    def test_compare_rejects_revision_without_scorecard_and_unknown_arm(self):
        args = SimpleNamespace(parent="main", tip="tip", capture=[])
        with patch.object(scorecard, "git", side_effect=["old", "new"]):
            with patch.object(scorecard.subprocess, "run", return_value=SimpleNamespace(returncode=1)):
                with self.assertRaisesRegex(ValueError, "parent main .* has no scorecard"):
                    scorecard.compare(args)
        args.capture = [("other", "job=capture")]
        with patch.object(scorecard, "git", side_effect=["old", "new"]):
            with patch.object(scorecard.subprocess, "run", return_value=SimpleNamespace(returncode=0)):
                with self.assertRaisesRegex(ValueError, "must be parent or tip"):
                    scorecard.compare(args)

    def test_table_rejects_a_different_tip_tree(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            p, t = root / "parent.json", root / "tip.json"
            for path, name in ((p, "old"), (t, "new")):
                path.write_text(json.dumps(revision(name)), encoding="utf-8")
            with patch.object(scorecard, "git", return_value="other"):
                with self.assertRaisesRegex(ValueError, "--tip-tree does not match"):
                    scorecard.table(p, t, root, tip_tree=root)


class ShapeTests(unittest.TestCase):
    # Unequal sides fix both in-plane principal axes.
    WALL = np.array([[[0, 0, 0], [4, 0, 0], [4, 2, 0]], [[0, 0, 0], [4, 2, 0], [0, 2, 0]]], dtype=float)

    def test_planar_identity_empty_extra_missing_and_rotation(self):
        wall = self.WALL
        iou, distance = shape_measure(wall, wall)
        self.assertEqual(iou, 1.0)
        self.assertAlmostEqual(distance, 0.0)
        self.assertEqual(shape_measure(wall, wall[:0]), (0.0, None))
        extra = np.concatenate([wall, wall + [0, 0, 20]])
        iou, far = shape_measure(wall, extra)
        self.assertEqual(iou, 1.0)
        self.assertGreater(far, 4.0)

        along_widest_axis = wall + [2, 0, 0]
        self.assertAlmostEqual(shape_measure(wall, along_widest_axis)[0], 0.49, delta=0.04)

        half = wall * [1, 0.5, 1]
        iou, missing = shape_measure(wall, half)
        self.assertAlmostEqual(iou, 0.5, delta=0.04)
        self.assertGreater(missing, 0.2)
        self.assertEqual(shape_measure(wall, half), (iou, missing))
        self.assertAlmostEqual(shape_measure(wall * 32, half * 32)[1], missing)
        rotation = np.array([[0.6, 0, 0.8], [0, 1, 0], [-0.8, 0, 0.6]])
        self.assertAlmostEqual(shape_measure(wall @ rotation, half @ rotation)[0], iou, delta=0.03)
        self.assertEqual(shape_measure(np.zeros((1, 3, 3)), np.zeros((1, 3, 3))), (1.0, 0.0))

    def test_p99_ignores_one_outlier_among_many_points(self):
        from r3d.geometry import directed_distance, triangle_search

        samples = np.repeat([[1., 1., 0.]], 256, axis=0)
        samples[-1, 2] = 100
        search = triangle_search(self.WALL)
        self.assertAlmostEqual(directed_distance(samples, search, percentile=99), 0)
        self.assertAlmostEqual(directed_distance(samples, search), 100)
        with patch("r3d.objects.part_shape.area_samples", return_value=(samples, None)):
            self.assertAlmostEqual(shape_measure(self.WALL, self.WALL)[1], 0)

    def test_part_without_assigned_triangles_is_measured(self):
        wall = self.WALL
        vertices = np.concatenate([wall, wall + [10, 0, 0]]).reshape(-1, 3)
        faces = np.arange(len(vertices)).reshape(-1, 3)
        source = SimpleNamespace(p=vertices, tri_v=faces, tri_o=np.repeat([0, 1], 2), tri_m=np.repeat([0, 1], 2),
                                 names=["stone", "wood"], object_names=["front", "back"])
        front, back = measure_parts(source, vertices, faces[:2])
        self.assertEqual((front["source_object"], front["iou"]), ("front", 1.0))
        self.assertAlmostEqual(front["hausdorff"], 0)
        self.assertEqual((back["source_object"], back["material"], back["iou"]), ("back", "wood", 0.0))
        self.assertGreater(back["hausdorff"], 1)

    def test_box_shift_has_known_front_iou_and_hausdorff(self):
        corners = [(0, 0), (2, 0), (2, 1), (0, 1)]
        points = np.array([(x, y, z) for z in (0, 0.75) for x, y in corners])
        floor = [[0, 1, 2], [0, 2, 3]]
        sides = [[a, b, b + 4] if half == 0 else [a, b + 4, a + 4]
                 for a, b in zip(range(4), (1, 2, 3, 0)) for half in range(2)]
        faces = np.concatenate([floor, np.asarray(floor) + 4, sides])
        iou, hausdorff = shape_measure(points[faces], (points + [0, 0.5, 0])[faces])
        self.assertAlmostEqual(iou, 1 / 3, delta=0.035)
        # The two exposed end faces are each 0.5 units from the other box at the farthest point.
        self.assertAlmostEqual(hausdorff, 0.211999576, places=6)


class HeldOutTests(unittest.TestCase):
    def test_held_out_score_parses_real_log_and_reports_unavailable_sets(self):
        from held_out_reference import appearance, measure_held_out

        scene = SimpleNamespace(camera=SimpleNamespace(component=SimpleNamespace(background=0xFFFFFF)))
        fit = SimpleNamespace(scene=scene)
        sets = [(name, pathlib.Path(name + ".txt"), pathlib.Path(name)) for name in ("probe", "path")]
        with (patch("held_out_reference.fit_rasterizer_reason", return_value=None),
              patch("held_out_reference.held_out_sets", return_value=sets) as held,
              patch.object(appearance, "load_views", side_effect=[(["a"], (2, 3)), (["b", "c"], (2, 3))]),
              patch.object(appearance, "delta_e_total", side_effect=[(12, 6), (60, 12)])):
            self.assertEqual(measure_held_out(fit, None, None, "mesh"),
                             {"mean": 4.0, "sets": ["probe", "path"], "views": 3})
            with (patch.object(appearance, "load_views", return_value=(["a"], (2, 3))),
                  patch.object(appearance, "held_out_line", return_value="5.594 over 1 views")):
                with self.assertRaisesRegex(ValueError, "unrecognized fit rasterizer score"):
                    measure_held_out(fit, None, None, "mesh")
            held.return_value = []
            self.assertEqual(measure_held_out(fit, None, None, "mesh"),
                             {"reason": "the fit declares no held-out views"})
        with patch("held_out_reference.fit_rasterizer_reason", return_value="CUDA unavailable"):
            self.assertEqual(measure_held_out(fit, None, None, "mesh"), {"reason": "CUDA unavailable"})

    def test_held_out_sets_use_cached_or_packed_poses_and_render_missing_pictures(self):
        from bake import bake, produce
        from held_out_reference import held_out_sets
        from r3d.reference_set import HELD_OUT, PROBE_HELD_OUT, PROBE_TRAINING, pack_references

        for pairs in ((HELD_OUT,), (PROBE_HELD_OUT,), (HELD_OUT, PROBE_HELD_OUT)):
            with self.subTest(pairs=pairs), tempfile.TemporaryDirectory() as folder:
                root = pathlib.Path(folder)
                cached = root / "cache"
                cached.mkdir()
                archive = root / "reference.tar.xz"
                sets = SimpleNamespace(held_out=pairs, training=(), coverage=())
                if pairs == (PROBE_HELD_OUT,):
                    sets.training = (PROBE_TRAINING,)
                    (cached / PROBE_TRAINING[0]).touch()
                    (cached / PROBE_TRAINING[1]).mkdir()
                for pose, pictures in pairs:
                    (cached / pose).write_text(pose, encoding="utf-8")
                    (cached / pictures).mkdir()
                    (cached / pictures / "0.png").write_bytes(b"cached image")
                archive.write_bytes(pack_references(cached, sets))
                fit = SimpleNamespace(job=None, scene=None, reference=archive,
                                      stages=dict(reference="ref", reference_pose="pose"))

                def trace(inputs, missing, store):
                    for poses, pictures in missing:
                        pictures.mkdir()
                        (pictures / "0.png").write_bytes(poses.read_bytes())

                with (patch("held_out_reference.reference_inputs", return_value=SimpleNamespace(sets=lambda: sets)),
                      patch.object(produce, "reference_folder", return_value=cached),
                      patch.object(produce, "PoseStore"),
                      patch.object(bake, "fetch", return_value=archive),
                      patch("held_out_reference.render_sets", side_effect=trace)):
                    work = root / "work"
                    unpacked = held_out_sets(fit, None, work)
                    (cached / produce.DONE).touch()
                    hits = held_out_sets(fit, None, work)
                names = ["path" if pair == HELD_OUT else "probe" for pair in pairs]
                self.assertEqual([name for name, _, _ in unpacked], names)
                self.assertEqual([name for name, _, _ in hits], names)
                for (pose, pictures), (_, unpacked_pose, unpacked_pictures), (_, hit_pose, hit_pictures) in zip(
                        pairs, unpacked, hits, strict=True):
                    self.assertEqual(unpacked_pose, work / pose)
                    self.assertEqual(hit_pose, cached / pose)
                    self.assertEqual(unpacked_pose.read_text(encoding="utf-8"), pose)
                    expected = b"cached image" if sets.training else pose.encode()
                    self.assertEqual((unpacked_pictures / "0.png").read_bytes(), expected)
                    self.assertEqual((hit_pictures / "0.png").read_bytes(), b"cached image")
