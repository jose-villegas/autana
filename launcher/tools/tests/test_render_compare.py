"""Tests for launcher/tools/render/render_compare.py on tiny synthetic images.

    python -m unittest discover -s launcher/tools/tests
"""
import io
import contextlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "render"))

import check_avi  # noqa: E402
import render_compare  # noqa: E402

CLEAR = (10, 20, 30)
DRAWN = (200, 100, 50)


def image(rows):
    """A W x H image from rows of RGB tuples."""
    picture = Image.new("RGB", (len(rows[0]), len(rows)))
    picture.putdata([pixel for row in rows for pixel in row])
    return picture


class AngleSheetTest(unittest.TestCase):
    def test_each_panel_is_labelled_and_the_scale_sits_below(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name, value in (("a", 10.0), ("b", 30.0)):
                (Path(tmp) / name).mkdir()
                angle = np.full((20, 40), value, dtype=np.float32)
                angle[0, 0] = np.nan
                np.save(Path(tmp) / name / "0000.angle.npy", angle)
            picture = render_compare.angle_sheet([("one", Path(tmp) / "a"), ("two", Path(tmp) / "b")], tile=1.0)
        self.assertEqual(picture.size, (80, 20 + render_compare.LABEL_BAR + 70))
        pixels = np.asarray(picture)
        self.assertEqual(tuple(pixels[render_compare.LABEL_BAR, 0]), (0, 0, 0))
        self.assertGreater(int(pixels[render_compare.LABEL_BAR + 10, 60, 0]), int(pixels[render_compare.LABEL_BAR + 10, 20, 0]))


class MeasureTest(unittest.TestCase):
    def test_identical_images_differ_nowhere(self):
        a = image([[DRAWN, CLEAR], [CLEAR, DRAWN]])
        stats = render_compare.measure(a, a.copy(), CLEAR)
        self.assertEqual(stats.changed, 0)
        self.assertEqual(stats.mean_abs, 0.0)
        self.assertEqual((stats.holes_a, stats.holes_b), (0, 0))

    def test_changed_share_and_mean(self):
        a = image([[(0, 0, 0), (0, 0, 0)], [(0, 0, 0), (0, 0, 0)]])
        b = image([[(30, 0, 0), (0, 0, 0)], [(0, 0, 0), (0, 6, 0)]])
        stats = render_compare.measure(a, b, None)
        self.assertEqual(stats.changed, 2)
        self.assertEqual(stats.total, 4)
        self.assertAlmostEqual(stats.mean_abs, 36 / 12)

    def test_a_hole_is_clear_on_one_side_and_drawn_on_the_other(self):
        a = image([[CLEAR, CLEAR], [DRAWN, DRAWN]])
        b = image([[CLEAR, DRAWN], [DRAWN, CLEAR]])
        stats = render_compare.measure(a, b, CLEAR)
        self.assertEqual(stats.holes_a, 1)
        self.assertEqual(stats.holes_b, 1)

    def test_no_clear_colour_counts_no_holes(self):
        a = image([[CLEAR, DRAWN]])
        b = image([[DRAWN, CLEAR]])
        stats = render_compare.measure(a, b, None)
        self.assertEqual((stats.holes_a, stats.holes_b), (0, 0))

    def test_565_expanded_clear_colour_matches(self):
        # 0x9CC0E6 as the 16-bit framebuffer holds it once expanded to 24 bits.
        a = image([[(156, 195, 231), DRAWN]])
        b = image([[DRAWN, DRAWN]])
        stats = render_compare.measure(a, b, render_compare.parse_rgb("9CC0E6"))
        self.assertEqual(stats.holes_a, 1)


class ReferenceMetricTest(unittest.TestCase):
    def test_identical_images_have_zero_delta_e_and_one_ssim(self):
        picture = image([[DRAWN, CLEAR], [CLEAR, DRAWN]])
        stats = render_compare.reference_measure(picture, picture.copy())
        self.assertEqual((stats.mean_delta_e, stats.p95_delta_e, stats.ssim_luma), (0.0, 0.0, 1.0))

    def test_black_to_white_is_a_hundred_delta_e(self):
        black, white = image([[(0, 0, 0)]]), image([[(255, 255, 255)]])
        stats = render_compare.reference_measure(black, white)
        self.assertAlmostEqual(stats.mean_delta_e, 100.0, places=4)
        self.assertAlmostEqual(stats.p95_delta_e, 100.0, places=4)

    def test_reference_heatmap_is_black_for_a_match(self):
        picture = image([[DRAWN]])
        self.assertEqual(render_compare.reference_heatmap(picture, picture).getpixel((0, 0)), (0, 0, 0))


STEP = [[(0, 0, 0)] * 4 + [(255, 255, 255)] * 4 for _ in range(8)]


class SsimTest(unittest.TestCase):
    def test_the_score_is_the_mean_of_the_ssim_map_over_every_eight_by_eight_window(self):
        rng = np.random.default_rng(3)
        a = rng.integers(0, 256, (12, 13, 3), dtype=np.uint8)
        b = np.clip(a // 2 + rng.integers(0, 90, (12, 13, 3)), 0, 255).astype(np.uint8)
        luma = lambda pixels: pixels / 255.0 @ np.array([0.2126, 0.7152, 0.0722])
        la, lb = luma(a.astype(float)), luma(b.astype(float))
        scores = []
        for y in range(12 - 7):
            for x in range(13 - 7):
                wa, wb = la[y : y + 8, x : x + 8].ravel(), lb[y : y + 8, x : x + 8].ravel()
                ma, mb = wa.mean(), wb.mean()
                cov = ((wa - ma) * (wb - mb)).mean()
                scores.append((2 * ma * mb + 1e-4) * (2 * cov + 9e-4) / ((ma**2 + mb**2 + 1e-4) * (wa.var() + wb.var() + 9e-4)))
        got = render_compare.luma_ssim(Image.fromarray(a), Image.fromarray(b))
        self.assertAlmostEqual(got, float(np.mean(scores)), places=9)
        self.assertEqual(render_compare.luma_ssim(Image.fromarray(a), Image.fromarray(a)), 1.0)

    def test_a_picture_smaller_than_a_window_is_one_window(self):
        a, b = image([[(0, 0, 0), (255, 255, 255)]] * 2), image([[(255, 255, 255), (0, 0, 0)]] * 2)
        self.assertLess(render_compare.luma_ssim(a, b), 0.0)


class Expand565Test(unittest.TestCase):
    def test_an_array_expands_like_the_tuple_of_each_pixel(self):
        pixels = np.array([[[156, 195, 231], [255, 255, 255], [0, 0, 0]]], dtype=np.uint8)
        got = render_compare.expand_565(pixels)
        self.assertEqual([tuple(int(v) for v in pixel) for pixel in got[0]],
                         [render_compare.expand_565(tuple(int(v) for v in pixel)) for pixel in pixels[0]])
        self.assertEqual(got.dtype, np.uint8)


class ReferenceSheetTest(unittest.TestCase):
    def test_a_sheet_is_reference_render_heatmap_and_edges_over_a_colour_scale(self):
        reference = image(STEP)
        sheet = render_compare.reference_sheet([("a", image(STEP), reference)], "render", tile=1.0)
        self.assertEqual(sheet.width, 4 * 8)
        self.assertEqual(sheet.height, render_compare.LABEL_BAR + 8 + 70)


class OutlinedTextTest(unittest.TestCase):
    """Labels must read over any panel, so light text carries a dark outline and the fill survives it."""

    def draw_on(self, background, fill):
        from PIL import ImageDraw

        panel = Image.new("RGB", (120, 30), background)
        render_compare.outlined_text(ImageDraw.Draw(panel), (6, 6), "label", fill, render_compare._font())
        return np.asarray(panel)

    def test_light_text_on_a_white_panel_leaves_a_dark_outline(self):
        pixels = self.draw_on((255, 255, 255), (255, 255, 255))
        self.assertGreater((pixels.max(axis=2) < 64).sum(), 20)

    def test_light_text_on_a_black_panel_still_shows_its_fill(self):
        pixels = self.draw_on((0, 0, 0), (255, 255, 255))
        self.assertGreater((pixels.min(axis=2) > 192).sum(), 20)


class EdgeSplitTest(unittest.TestCase):
    """A reference with one vertical step, and renders that differ from it in one column."""

    STEP = STEP

    def stats_with_error_in(self, column):
        render = [list(row) for row in self.STEP]
        for row in render:
            row[column] = (128, 128, 128)
        return render_compare.reference_measure(image(render), image(self.STEP))

    def test_error_at_the_step_is_edge_error(self):
        stats = self.stats_with_error_in(3)
        self.assertEqual(stats.edge_share, 1.0)
        self.assertEqual(stats.interior_delta_e, 0.0)
        self.assertGreater(stats.edge_delta_e, 0.0)

    def test_error_away_from_the_step_is_interior_error(self):
        stats = self.stats_with_error_in(0)
        self.assertEqual(stats.edge_share, 0.0)
        self.assertEqual(stats.edge_delta_e, 0.0)
        self.assertGreater(stats.interior_delta_e, 0.0)


class HeatmapTest(unittest.TestCase):
    def test_grey_is_amplified_and_holes_are_red(self):
        a = image([[CLEAR, (0, 0, 0), (0, 0, 0)]])
        b = image([[DRAWN, (0, 3, 0), (0, 0, 0)]])
        heat = render_compare.heatmap(a, b, CLEAR, gain=8)
        self.assertEqual(heat.getpixel((0, 0)), (255, 0, 0))
        self.assertEqual(heat.getpixel((1, 0)), (24, 24, 24))
        self.assertEqual(heat.getpixel((2, 0)), (0, 0, 0))

    def test_gain_saturates(self):
        a = image([[(0, 0, 0)]])
        b = image([[(250, 0, 0)]])
        self.assertEqual(render_compare.heatmap(a, b, None, gain=8).getpixel((0, 0)), (255, 255, 255))


class SheetTest(unittest.TestCase):
    def test_row_is_a_then_b_then_heat_and_rows_stack(self):
        a = image([[DRAWN, DRAWN]])
        sheet = render_compare.sheet([("one", a, a), ("two", a, a)], CLEAR, "a", "b", gain=8)
        self.assertEqual(sheet.size, (6, 2 * (1 + render_compare.LABEL_BAR)))

    def test_each_panel_sits_under_a_label_bar_of_its_own(self):
        wide = blank(120, 4)
        pixels = np.asarray(render_compare.sheet([("r", wide, wide)], None, "aaaa", "bbbb"))
        bar = render_compare.LABEL_BAR
        for column in range(3):
            self.assertTrue(pixels[:bar, column * 120 : (column + 1) * 120].any(), "panel %d has no label" % column)

    def test_the_labels_come_from_the_caller(self):
        wide = blank(120, 4)
        bar = render_compare.LABEL_BAR
        one = np.asarray(render_compare.sheet([("r", wide, wide)], None, "smooth", "flat"))[:bar, :240]
        two = np.asarray(render_compare.sheet([("r", wide, wide)], None, "lite", "fitted"))[:bar, :240]
        self.assertFalse((one == two).all())

    def test_a_sheet_without_labels_is_refused(self):
        a = image([[DRAWN]])
        for labels in (("", "b"), ("a", ""), (None, None)):
            with self.assertRaises(ValueError):
                render_compare.sheet([("x", a, a)], None, *labels)

    def test_rows_of_different_widths_are_padded_to_the_widest(self):
        narrow, wide = image([[DRAWN, DRAWN]]), image([[DRAWN, DRAWN, DRAWN, DRAWN]])
        sheet = render_compare.sheet([("n", narrow, narrow), ("w", wide, wide)], CLEAR, "a", "b")
        self.assertEqual(sheet.size, (12, 2 * (1 + render_compare.LABEL_BAR)))
        self.assertEqual(sheet.getpixel((8, render_compare.LABEL_BAR)), (0, 0, 0))

    def test_size_mismatch_is_refused(self):
        with self.assertRaises(ValueError):
            render_compare.sheet([("x", image([[DRAWN]]), image([[DRAWN, DRAWN]]))], None, "a", "b", gain=8)


def blank(width, height, colour=(0, 0, 0)):
    return Image.new("RGB", (width, height), colour)


def with_pixels(picture, points, colour):
    picture = picture.copy()
    for point in points:
        picture.putpixel(point, colour)
    return picture


def blob(x, y, size=3):
    return [(x + i, y + j) for i in range(size) for j in range(size)]


class ClusterTest(unittest.TestCase):
    def setUp(self):
        self.base = blank(80, 60)

    def clusters(self, changed, **kwargs):
        return render_compare.find_clusters(self.base, changed, None, **kwargs)

    def test_identical_images_have_no_clusters(self):
        self.assertEqual(self.clusters(self.base.copy()), [])

    def test_two_separate_blobs_are_two_clusters(self):
        changed = with_pixels(self.base, blob(5, 5) + blob(60, 40), DRAWN)
        self.assertEqual(len(self.clusters(changed)), 2)

    def test_adjacent_pixels_are_one_cluster(self):
        changed = with_pixels(self.base, blob(20, 20) + blob(25, 20), DRAWN)
        found = self.clusters(changed, margin=0)
        self.assertEqual(len(found), 1)
        self.assertEqual((found[0].x0, found[0].x1), (20, 28))

    def test_diagonal_neighbours_are_one_cluster(self):
        changed = with_pixels(self.base, [(30, 30), (31, 31)], DRAWN)
        self.assertEqual(len(self.clusters(changed, grow=0)), 1)

    def test_margin_surrounds_the_changed_pixels(self):
        changed = with_pixels(self.base, blob(30, 20), DRAWN)
        found = self.clusters(changed, margin=5)[0]
        self.assertEqual((found.x0, found.y0, found.x1, found.y1), (25, 15, 38, 28))

    def test_margin_is_clamped_at_the_image_edge(self):
        changed = with_pixels(self.base, blob(0, 0) + blob(77, 57), DRAWN)
        found = sorted(self.clusters(changed, margin=10), key=lambda c: c.x0)
        self.assertEqual((found[0].x0, found[0].y0), (0, 0))
        self.assertEqual((found[1].x1, found[1].y1), (80, 60))

    def test_a_hole_outranks_a_stronger_change(self):
        clear = (10, 20, 30)
        a = with_pixels(blank(80, 60), blob(5, 5, 2), clear)
        b = with_pixels(a, blob(5, 5, 2), DRAWN)
        b = with_pixels(b, blob(50, 30, 6), (255, 255, 255))
        found = render_compare.find_clusters(a, b, clear, count=2)
        self.assertEqual([c.holes for c in found], [4, 0])
        self.assertGreater(found[1].strength, found[0].strength)

    def test_stronger_blob_ranks_first_when_neither_has_holes(self):
        changed = with_pixels(with_pixels(self.base, blob(5, 5), (20, 20, 20)), blob(50, 30), (90, 90, 90))
        found = self.clusters(changed)
        self.assertEqual([c.x0 < 30 for c in found], [False, True])

    def test_a_huge_cluster_is_cut_to_its_strongest_window(self):
        wide = blank(300, 200)
        changed = with_pixels(wide, blob(20, 20, 150), (30, 30, 30))
        changed = with_pixels(changed, blob(140, 140, 10), (250, 250, 250))
        found = render_compare.find_clusters(wide, changed, None, margin=4, max_side=40)
        self.assertEqual(len(found), 1)
        rect = found[0]
        self.assertLessEqual(rect.x1 - rect.x0, 40 + 2 * 4)
        self.assertLessEqual(rect.y1 - rect.y0, 40 + 2 * 4)
        self.assertTrue(rect.x0 <= 140 and rect.x1 >= 150 and rect.y0 <= 140 and rect.y1 >= 150)

    def test_count_limits_the_result(self):
        changed = with_pixels(self.base, blob(5, 5) + blob(40, 5) + blob(5, 40) + blob(40, 40), DRAWN)
        self.assertEqual(len(self.clusters(changed, count=3)), 3)

    def test_small_changes_under_the_threshold_are_ignored(self):
        changed = with_pixels(self.base, blob(5, 5), (4, 4, 4))
        self.assertEqual(self.clusters(changed, threshold=8), [])


class CropSheetTest(unittest.TestCase):
    def test_crops_are_enlarged_without_smoothing(self):
        a = blank(40, 30)
        b = with_pixels(a, [(20, 15)], DRAWN)
        clusters = render_compare.find_clusters(a, b, None, margin=2, grow=0)
        pixels = np.asarray(render_compare.crop_sheet([("t", a, b, clusters)], "a", "b", zoom=4))
        self.assertEqual(int((pixels == DRAWN).all(axis=2).sum()), 16)

    def test_a_crop_over_the_size_cap_is_zoomed_less(self):
        a = blank(400, 300)
        b = with_pixels(a, blob(0, 0, 3) + blob(397, 297, 3), DRAWN)
        clusters = render_compare.find_clusters(a, b, None, margin=0, grow=0, threshold=0)
        pixels = np.asarray(render_compare.crop_sheet([("t", a, b, clusters[:1])], "a", "b", zoom=4))
        self.assertLessEqual(pixels.shape[1], 512 + 100)

    def test_no_difference_writes_no_crops_file(self):
        same = blank(20, 20)
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "x.crops.png"
            self.assertFalse(render_compare.write_crops([("t", same, same, [])], str(target), "a", "b"))
            self.assertFalse(target.exists())

    def test_a_difference_writes_the_crops_file(self):
        a = blank(20, 20)
        b = with_pixels(a, blob(5, 5), DRAWN)
        entries = [("t", a, b, render_compare.find_clusters(a, b, None))]
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "x.crops.png"
            self.assertTrue(render_compare.write_crops(entries, str(target), "a", "b"))
            self.assertTrue(target.exists())

    def test_each_crop_sits_under_a_label_bar_naming_its_side(self):
        a = blank(40, 30)
        b = with_pixels(a, [(20, 15)], DRAWN)
        clusters = render_compare.find_clusters(a, b, None, margin=2, grow=0)
        bar = render_compare.LABEL_BAR
        picture = render_compare.crop_sheet([("t", a, b, clusters)], "smooth", "flat", zoom=4)
        crop = clusters[0].y1 - clusters[0].y0
        self.assertEqual(picture.height, 36 + 2 * (bar + 4 * crop) + 2 * 6)
        pixels = np.asarray(picture)
        self.assertTrue((pixels[36 : 36 + bar, :150] != 24).any())
        self.assertTrue((pixels[36 + bar + 4 * crop + 6 : 36 + 2 * bar + 4 * crop + 6, :150] != 24).any())

    def test_a_crops_sheet_without_labels_is_refused(self):
        with self.assertRaises(ValueError):
            render_compare.crop_sheet([("t", blank(4, 4), blank(4, 4), [])], "a", "")

    def test_no_clusters_still_gives_a_picture(self):
        self.assertGreater(render_compare.crop_sheet([("t", blank(4, 4), blank(4, 4), [])], "a", "b").size[0], 0)


def avi_bytes(width, height, frames, dt_ms):
    """A minimal uncompressed AVI shaped like render_video.c's, from top-down RGB frames."""
    stride = (width * 3 + 3) // 4 * 4

    def chunk(tag, body):
        return tag + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")

    def lst(kind, body):
        return chunk(b"LIST", kind + body)

    avih = struct.pack("<10I", dt_ms * 1000, 0, 0, 0, len(frames), 0, 1, stride * height, width, height) + bytes(16)
    strh = b"vids" + bytes(4) + struct.pack("<3I", 0, 0, 0)
    strh += struct.pack("<5I", dt_ms, 1000, 0, len(frames), stride * height) + bytes(16)
    strf = struct.pack("<IiiHHII", 40, width, height, 1, 24, 0, stride * height) + bytes(16)
    movi = b""
    for picture in frames:
        rows = np.asarray(picture)[::-1, :, ::-1]
        padded = np.zeros((height, stride), dtype=np.uint8)
        padded[:, : width * 3] = rows.reshape(height, width * 3)
        movi += chunk(b"00dc", padded.tobytes())
    hdrl = lst(b"hdrl", chunk(b"avih", avih) + lst(b"strl", chunk(b"strh", strh) + chunk(b"strf", strf)))
    body = b"AVI " + hdrl + lst(b"movi", movi)
    return b"RIFF" + struct.pack("<I", len(body)) + body


class VideoTest(unittest.TestCase):
    def write_avi(self, directory, name, frames, dt_ms=250):
        path = Path(directory) / name
        path.write_bytes(avi_bytes(frames[0].size[0], frames[0].size[1], frames, dt_ms))
        return str(path)

    def test_a_reference_video_scores_each_frame_at_the_scale_it_finds_and_hands_it_to_the_sink(self):
        render = Image.new("RGB", (4, 4), DRAWN)
        reference = Image.new("RGB", (2, 2), DRAWN)
        seen = []
        with tempfile.TemporaryDirectory() as tmp:
            video = self.write_avi(tmp, "a.avi", [render, render], 250)
            for index in range(2):
                reference.save(Path(tmp) / ("%04d.png" % index))
            values, total = render_compare.reference_video(video, tmp, sink=lambda *item: seen.append(item[:2] + (item[3].size,)))
        self.assertEqual(seen, [(4.0, 0, (4, 4)), (4.0, 1, (4, 4))])
        self.assertEqual((len(values), total.mean_delta_e), (2, 0.0))

    def test_main_writes_one_reference_line_per_frame_and_their_mean_to_the_summary(self):
        render = Image.new("RGB", (4, 4), DRAWN)
        with tempfile.TemporaryDirectory() as tmp:
            video = self.write_avi(tmp, "a.avi", [render, render], 250)
            references = Path(tmp) / "reference"
            references.mkdir()
            for index in range(2):
                Image.new("RGB", (2, 2), DRAWN).save(references / ("%04d.png" % index))
            summary = Path(tmp) / "summary.txt"
            argv = ["render_compare.py", "--out", str(Path(tmp) / "x.png"), "--reference-video", video, str(references),
                    "--summary", str(summary)]
            with unittest.mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(io.StringIO()):
                render_compare.main()
            lines = summary.read_text().splitlines()
        self.assertEqual([line.split(":")[0] for line in lines], ["frame 0", "frame 1", "frames mean"])
        self.assertTrue(all(line.startswith(("frame", "frames")) and "mean DeltaE76 0.0000" in line and "luma SSIM 1.000000" in line
                            for line in lines))

    def test_a_reference_video_can_start_at_a_later_frame(self):
        near, far = Image.new("RGB", (2, 2), DRAWN), Image.new("RGB", (2, 2), CLEAR)
        with tempfile.TemporaryDirectory() as tmp:
            video = self.write_avi(tmp, "a.avi", [far, far, near], 250)
            near.save(Path(tmp) / "000.png")
            values, total = render_compare.reference_video(video, tmp, first=2)
        self.assertEqual((len(values), total.mean_delta_e), (1, 0.0))

    def test_read_video_gives_top_down_rgb_frames_and_the_rate(self):
        frame = with_pixels(blank(5, 4), [(1, 0)], DRAWN)
        with tempfile.TemporaryDirectory() as tmp:
            fps, frames = render_compare.read_video(self.write_avi(tmp, "a.avi", [frame, blank(5, 4)]))
            frames = list(frames)
        self.assertEqual(fps, 4.0)
        self.assertEqual(len(frames), 2)
        self.assertEqual(tuple(frames[0][0, 1]), DRAWN)
        self.assertEqual(tuple(frames[0][3, 1]), (0, 0, 0))

    def test_the_avi_reader_gives_size_rate_and_raw_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            video = check_avi.read_avi(self.write_avi(tmp, "a.avi", [blank(5, 4), blank(5, 4)]))
            frames = list(video.frames)
        self.assertEqual((video.width, video.height, video.fps), (5, 4, 4.0))
        self.assertEqual([len(f) for f in frames], [16 * 4, 16 * 4])

    def test_the_avi_reader_refuses_a_file_that_is_not_an_avi(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "x.avi"
            path.write_bytes(b"not an avi at all")
            with self.assertRaises(check_avi.AviError):
                check_avi.read_avi(str(path))

    def test_a_composed_frame_is_a_then_b_then_heat_under_a_label_bar(self):
        a = blank(6, 4, (50, 50, 50))
        b = blank(6, 4, (60, 60, 60))
        picture = render_compare.compose_frame(a, b, None, 8, "aaaa", "bbbb")
        bar = render_compare.LABEL_BAR
        self.assertEqual(picture.size, (18, 4 + bar))
        self.assertEqual(picture.getpixel((0, bar)), (50, 50, 50))
        self.assertEqual(picture.getpixel((6, bar)), (60, 60, 60))
        self.assertEqual(picture.getpixel((12, bar)), (80, 80, 80))

    def test_the_label_bar_carries_text_over_each_panel(self):
        pixels = np.asarray(render_compare.compose_frame(blank(200, 4), blank(200, 4), None, 8, "aaaa", "bbbb"))
        bar = render_compare.LABEL_BAR
        for column in range(3):
            panel = pixels[:bar, column * 200 : (column + 1) * 200]
            self.assertTrue(panel.any(), "panel %d has no label" % column)

    def test_a_still_without_both_labels_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            for labels in ([], ["--label-a", "x"], ["--label-b", "y"]):
                path = Path(tmp) / "a.png"
                blank(4, 4).save(path)
                argv = ["render_compare.py", "--out", str(Path(tmp) / "s.png"), *labels, "--row", "r", str(path), str(path)]
                with unittest.mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as refused:
                        render_compare.main()
                self.assertEqual(refused.exception.code, 2)
                self.assertFalse((Path(tmp) / "s.png").exists())

    def test_a_labelled_still_is_written_with_its_label_bar(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.png"
            blank(4, 4).save(path)
            argv = ["render_compare.py", "--out", str(Path(tmp) / "s.png"), "--label-a", "x", "--label-b", "y",
                    "--row", "r", str(path), str(path)]
            with unittest.mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(io.StringIO()):
                render_compare.main()
            with Image.open(Path(tmp) / "s.png") as shot:
                self.assertEqual(shot.size, (12, 4 + render_compare.LABEL_BAR))

    def test_frame_line_gives_time_share_and_holes(self):
        stats = render_compare.Stats(changed=5, total=20, mean_abs=1.5, holes_a=2, holes_b=3)
        self.assertEqual(render_compare.frame_line(4, 250.0, stats), "4,1000,25.0000,1.5000,2,3")

    def test_worst_frames_put_holes_before_changed_pixels_and_keep_order(self):
        stats = [render_compare.Stats(c, 100, 0.0, h, 0) for c, h in [(9, 0), (5, 3), (2, 0), (1, 7)]]
        self.assertEqual(render_compare.worst_frames(stats, 2), [1, 3])

    def test_video_summary_names_the_worst_frame(self):
        stats = [render_compare.Stats(c, 100, 1.0, 0, 0) for c in (1, 30, 2)]
        self.assertIn("max 30.00% (frame 1, 0.5 s)", render_compare.video_summary(stats, 500.0))

    def test_mismatched_rates_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            one = self.write_avi(tmp, "a.avi", [blank(4, 4)], 100)
            two = self.write_avi(tmp, "b.avi", [blank(4, 4)], 200)
            with self.assertRaises(ValueError):
                render_compare.compare_videos(one, two, str(Path(tmp) / "o.mp4"), None, None, 8, "a", "b")


@unittest.skipIf(shutil.which("sh") is None, "needs a POSIX shell")
class ReferenceModeArgumentsTest(unittest.TestCase):
    SCRIPT = Path(__file__).resolve().parents[1] / "render" / "render_compare.sh"

    def run_sh(self, *args):
        return subprocess.run(["sh", str(self.SCRIPT), *args], capture_output=True, text=True)

    def test_reference_needs_poses_and_a_render(self):
        for args in (["--script", "s.sh", "--reference", "x.scene.toml", "HEAD", "--render", "p", "--frames 2"],
                     ["--script", "s.sh", "--reference", "x.scene.toml", "--poses", "p.txt", "HEAD"]):
            result = self.run_sh(*args)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("--reference SCENE.scene.toml --poses FILE", result.stderr)

    def test_reference_refuses_a_second_revision(self):
        result = self.run_sh("--script", "s.sh", "--reference", "x", "--poses", "p", "HEAD", "HEAD", "--render", "p", "--frames 2")
        self.assertEqual(result.returncode, 2, result.stderr)

    def test_reference_frame_rate_is_30_40_60_or_80(self):
        base = ["--script", "s.sh", "--reference", "x", "--poses", "p", "--render", "p", "--frames 2"]
        refused = self.run_sh(*base, "--fps", "50", "HEAD")
        self.assertEqual(refused.returncode, 2)
        self.assertIn("30, 40, 60 or 80", refused.stderr)
        for fps in ("30", "40", "60", "80"):
            self.assertNotIn("30, 40, 60 or 80", self.run_sh(*base, "--fps", fps, "HEAD").stderr)


if __name__ == "__main__":
    unittest.main()
