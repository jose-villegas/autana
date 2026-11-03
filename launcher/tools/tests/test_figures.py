"""figures.py writes every doc PNG and GIF one way: PNGs without metadata, GIFs on the one global palette."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np
from PIL import Image, PngImagePlugin

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from docs import figures, plot_style  # noqa: E402
from render.check_avi import read_video  # noqa: E402
from test_render_compare import avi_bytes  # noqa: E402


THEME = ("#ffffff", "#000000", plot_style.INK, plot_style.MUTED, plot_style.FAINT, *plot_style.SLOTS,
         plot_style.DARK_BACKGROUND, plot_style.DARK_INK, plot_style.DARK_MUTED, plot_style.DARK_FAINT)


def palette_colours():
    return [tuple(int(v) for v in row) for row in figures.gif_palette()[0]]


class Folder(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.folder = pathlib.Path(directory.name)

    def gif_frames(self, path):
        with Image.open(path) as gif:
            frames = []
            for index in range(gif.n_frames):
                gif.seek(index)
                frames.append(np.asarray(gif.convert("RGB")))
            return frames, gif.info


class PaletteTests(unittest.TestCase):
    def test_the_palette_is_one_full_table_of_distinct_colours_led_by_the_theme(self):
        colours = palette_colours()
        self.assertEqual(len(colours), figures.GIF_COLOURS)
        self.assertEqual(len(set(colours)), figures.GIF_COLOURS)
        self.assertEqual(colours[:2], [(255, 255, 255), (0, 0, 0)])

    def test_a_theme_that_no_longer_fits_beside_the_fitted_colours_is_refused(self):
        with mock.patch.object(figures, "GIF_FITTED", figures.GIF_FITTED[1:]):
            with self.assertRaisesRegex(ValueError, "not 256"):
                figures.gif_palette.__wrapped__()

    def test_nearest_picks_the_closest_row(self):
        table = np.array([[0.0, 0, 0], [1, 0, 0], [0, 5, 0]])
        points = np.array([[0.9, 0, 0], [0, 4, 0], [0.1, 0.1, 0]] * 5000)
        self.assertEqual(figures.nearest(points, table).tolist(), [1, 2, 0] * 5000)

    def test_packed_colours_round_trip(self):
        colours = np.array([[0, 0, 0], [255, 255, 255], [1, 2, 3], [200, 100, 50]], np.uint8)
        self.assertEqual(figures.unpacked_colours(figures.packed_colours(colours)).tolist(), colours.tolist())


class SaveGifTests(Folder):
    def test_every_gif_carries_the_global_palette_and_palette_colours_come_back_exact(self):
        colours = palette_colours()
        frames = [np.full((4, 4, 3), colours[index], np.uint8) for index in (0, 7, 200)]
        figures.save_gif(frames, self.folder / "deep/a.gif", 100)
        figures.save_gif([np.full((2, 3, 3), colours[50], np.uint8)], self.folder / "b.gif", 100)
        for name in ("deep/a.gif", "b.gif"):
            with Image.open(self.folder / name) as gif:
                self.assertEqual(gif.getpalette()[:3 * figures.GIF_COLOURS], [v for c in colours for v in c])
        shown, info = self.gif_frames(self.folder / "deep/a.gif")
        self.assertEqual([tuple(frame[0, 0]) for frame in shown], [colours[0], colours[7], colours[200]])
        self.assertEqual((info["loop"], info["duration"]), (0, 100))

    def test_an_off_palette_colour_takes_its_nearest_entry_in_oklab(self):
        colour = np.array([[222, 179, 94]], np.uint8)
        table = np.array(palette_colours(), np.uint8)
        expected = table[figures.nearest(figures.oklab(colour), figures.oklab(table))[0]]
        figures.save_gif([np.tile(colour, (3, 3, 1))], self.folder / "a.gif", 50)
        shown, _ = self.gif_frames(self.folder / "a.gif")
        self.assertEqual(tuple(shown[0][1, 1]), tuple(expected))

    def test_pillow_images_and_arrays_give_the_same_bytes_every_time(self):
        rng = np.random.default_rng(3)
        frames = [rng.integers(0, 256, (8, 8, 3), dtype=np.uint8) for _ in range(3)]
        figures.save_gif(frames, self.folder / "a.gif", 80)
        figures.save_gif([Image.fromarray(frame) for frame in frames], self.folder / "b.gif", 80)
        self.assertEqual((self.folder / "a.gif").read_bytes(), (self.folder / "b.gif").read_bytes())


class ClipTests(unittest.TestCase):
    def test_a_lower_rate_shows_the_last_frame_landed_on_each_gif_frame(self):
        self.assertEqual(figures.clip(range(10), 10, 5), [0, 2, 4, 6, 8])
        self.assertEqual(figures.clip(range(10), 10, 4), [1, 3, 6, 8])

    def test_frames_are_chosen_as_ffmpegs_fps_filter_chooses_them(self):
        # ffmpeg -vf trim=start_frame=1:end_frame=56,setpts=PTS-STARTPTS,fps=6 on a 10 fps AVI.
        shown = figures.clip(range(60), 10, 6, first=1, end=56)
        self.assertEqual(shown[:10], [1, 3, 5, 6, 8, 10, 11, 13, 15, 16])
        self.assertEqual((len(shown), shown[-1]), (33, 55))

    def test_the_same_rate_keeps_every_frame(self):
        self.assertEqual(figures.clip(range(7), 8, 8), list(range(7)))

    def test_first_end_and_seconds_cut_the_source_first(self):
        self.assertEqual(figures.clip(range(20), 10, 10, first=2, end=6), [2, 3, 4, 5])
        self.assertEqual(figures.clip(range(100), 10, 10, seconds=0.5), [0, 1, 2, 3, 4])


class GifFromAviTests(Folder):
    def test_the_avi_is_resampled_and_turned_counterclockwise(self):
        colours = palette_colours()
        frames = []
        for index in range(6):
            picture = np.full((2, 3, 3), colours[index], np.uint8)
            picture[0, 0] = colours[100]  # the top-left corner, to see the turn
            frames.append(Image.fromarray(picture))
        avi = self.folder / "clip.avi"
        avi.write_bytes(avi_bytes(3, 2, frames, 100))
        figures.main(["gif", str(avi), str(self.folder / "clip.gif"), "--fps", "5", "--quarter", "1"])
        shown, info = self.gif_frames(self.folder / "clip.gif")
        self.assertEqual([tuple(frame[1, 1]) for frame in shown], [colours[0], colours[2], colours[4]])
        self.assertEqual(shown[0].shape, (3, 2, 3))
        self.assertEqual(tuple(shown[0][2, 0]), colours[100])  # counterclockwise: top-left goes to bottom-left
        self.assertEqual(info["duration"], 200)

    def test_rate_replaces_the_avis_own(self):
        frames = [Image.new("RGB", (2, 2), palette_colours()[index]) for index in range(4)]
        avi = self.folder / "clip.avi"
        avi.write_bytes(avi_bytes(2, 2, frames, 100))
        figures.gif_from_avi(avi, self.folder / "clip.gif", 5, rate=5)
        shown, info = self.gif_frames(self.folder / "clip.gif")
        self.assertEqual((len(shown), info["duration"]), (4, 200))


class SavePngTests(Folder):
    def test_metadata_is_dropped_so_one_picture_gives_one_file(self):
        picture = Image.new("RGB", (3, 2), (10, 20, 30))
        tagged = PngImagePlugin.PngInfo()
        tagged.add_text("Software", "anything")
        picture.save(self.folder / "tagged.png", pnginfo=tagged, dpi=(300, 300))
        figures.save_png(self.folder / "tagged.png", self.folder / "deep/a.png")
        figures.save_png(picture, self.folder / "b.png")
        self.assertEqual((self.folder / "deep/a.png").read_bytes(), (self.folder / "b.png").read_bytes())
        with Image.open(self.folder / "b.png") as written:
            self.assertEqual(written.info, {})
            self.assertEqual(written.getpixel((2, 1)), (10, 20, 30))

    def test_the_png_command_turns_a_bmp(self):
        picture = Image.new("RGB", (3, 2), (0, 0, 0))
        picture.putpixel((0, 0), (255, 0, 0))
        picture.save(self.folder / "a.bmp")
        figures.main(["png", str(self.folder / "a.bmp"), str(self.folder / "a.png"), "--quarter", "1"])
        with Image.open(self.folder / "a.png") as written:
            self.assertEqual((written.size, written.getpixel((0, 2))), ((2, 3), (255, 0, 0)))


def solid(colour, width=3, height=2):
    return np.full((height, width, 3), colour, np.uint8)


def total_ms(path):
    """What a viewer shows: the sum of every GIF frame's duration."""
    with Image.open(path) as gif:
        total = 0
        for index in range(gif.n_frames):
            gif.seek(index)
            total += gif.info["duration"]
        return total


class RenderSceneTests(Folder):
    def stub(self, body):
        path = self.folder / "viewer.sh"
        path.write_text(body, encoding="utf-8")
        return path

    def test_flags_arrive_stringified_in_order_and_stdout_and_stderr_both_reach_the_log(self):
        stub = self.stub('printf "argv:"\nfor a in "$@"; do printf " [%s]" "$a"; done\necho\necho oops >&2\n')
        flag_path = pathlib.Path("some") / "dir" / "x.bin"
        log = self.folder / "logs" / "run.log"
        log.parent.mkdir()
        with mock.patch.object(figures, "SCENE_VIEWER", stub):
            text = figures.render_scene("a.scene", "--frames", 12, flag_path, "--last", log=log)
        self.assertEqual(text, log.read_text())
        self.assertIn(f"argv: [a.scene] [--frames] [12] [{flag_path}] [--last]\n", text)
        self.assertIn("oops\n", text)

    def test_a_failing_viewer_raises(self):
        stub = self.stub("echo before\nexit 3\n")
        with mock.patch.object(figures, "SCENE_VIEWER", stub):
            with self.assertRaises(subprocess.CalledProcessError) as raised:
                figures.render_scene("a.scene", log=self.folder / "run.log")
        self.assertEqual(raised.exception.returncode, 3)
        self.assertIn("before", (self.folder / "run.log").read_text())


class ClipBase(Folder):
    """AVIs whose every frame has its own colour, so the frames a GIF shows can be told apart."""

    def setUp(self):
        super().setUp()
        self.colours = palette_colours()[10:20]

    def avi(self, count=10, dt_ms=100):
        path = self.folder / "clip.avi"
        path.write_bytes(avi_bytes(3, 2, [solid(self.colours[index]) for index in range(count)], dt_ms))
        return path

    def shown_indexes(self, *flags, count=10, dt_ms=100):
        out = self.folder / "clip.gif"
        figures.main(["gif", str(self.avi(count, dt_ms)), str(out), *flags])
        frames, _ = self.gif_frames(out)
        return [self.colours.index(tuple(frame[0, 0])) for frame in frames]


class ClipFlagTests(ClipBase):
    """gif_from_avi and the gif command cut with --first, --end and --seconds (fps == rate: each frame once)."""

    def test_first_alone_drops_the_start(self):
        self.assertEqual(self.shown_indexes("--fps", "10", "--first", "6"), [6, 7, 8, 9])

    def test_end_alone_drops_the_tail(self):
        self.assertEqual(self.shown_indexes("--fps", "10", "--end", "3"), [0, 1, 2])

    def test_first_and_end_together_take_the_window(self):
        self.assertEqual(self.shown_indexes("--fps", "10", "--first", "2", "--end", "6"), [2, 3, 4, 5])

    def test_seconds_alone_takes_that_much_at_the_avis_own_rate(self):
        self.assertEqual(self.shown_indexes("--fps", "10", "--seconds", "0.4"), [0, 1, 2, 3])

    def test_seconds_is_measured_at_the_rate_override(self):
        self.assertEqual(self.shown_indexes("--fps", "20", "--rate", "20", "--seconds", "0.25"), [0, 1, 2, 3, 4])
        self.assertEqual(self.shown_indexes("--fps", "5", "--rate", "5", "--seconds", "0.4"), [0, 1])

    def test_seconds_counts_from_first(self):
        self.assertEqual(self.shown_indexes("--fps", "10", "--first", "4", "--seconds", "0.3"), [4, 5, 6])

    def test_the_function_takes_the_same_flags(self):
        out = self.folder / "f.gif"
        figures.gif_from_avi(self.avi(), out, 20, rate=20, first=1, end=9, seconds=0.2)  # the AVI's own 10 gives 2
        frames, _ = self.gif_frames(out)
        self.assertEqual([self.colours.index(tuple(frame[0, 0])) for frame in frames], [1, 2, 3, 4])


class FrameRateTests(ClipBase):
    def test_a_fractional_fps_resamples_a_25_fps_clip_at_80_ms_a_frame(self):
        out = self.folder / "clip.gif"
        figures.main(["gif", str(self.avi(10, 40)), str(out), "--fps", "25/2"])
        frames, _ = self.gif_frames(out)
        self.assertEqual([self.colours.index(tuple(f[0, 0])) for f in frames], [0, 2, 4, 6, 8])
        self.assertEqual(total_ms(out), 5 * 80)

    def test_rate_on_the_command_line_replaces_the_avis_own(self):
        # The AVI says 10 fps; at --rate 20, a 10 fps GIF takes every second frame.
        self.assertEqual(self.shown_indexes("--fps", "10", "--rate", "20", count=8), [0, 2, 4, 6])

    def test_fps_15_delays_come_from_cumulative_centiseconds(self):
        out = self.folder / "clip.gif"
        figures.main(["gif", str(self.avi(6)), str(out), "--fps", "15", "--rate", "15"])
        self.assertEqual(self.durations(out), [70, 60, 70, 70, 60, 70])
        self.assertEqual(sum(self.durations(out)), 6 * 1000 / 15)

    def durations(self, path):
        with Image.open(path) as gif:
            out = []
            for index in range(gif.n_frames):
                gif.seek(index)
                out.append(gif.info["duration"])
            return out


class RepeatedFrameTests(Folder):
    def total_for(self, sequence, frame_ms):
        colours = palette_colours()
        figures.save_gif([solid(colours[index]) for index in sequence], self.folder / "a.gif", frame_ms)
        return total_ms(self.folder / "a.gif")

    def test_a_run_of_identical_frames_keeps_its_time(self):
        self.assertEqual(self.total_for([5] * 7, 80), 7 * 80)

    def test_a_single_frame_shows_for_one_frame_time(self):
        self.assertEqual(self.total_for([5], 80), 80)

    def test_a_palindrome_keeps_its_time_and_order(self):
        sequence = [3, 4, 5, 4, 3]
        self.assertEqual(self.total_for(sequence, 80), 5 * 80)
        frames, _ = self.gif_frames(self.folder / "a.gif")
        colours = palette_colours()
        slots = []
        with Image.open(self.folder / "a.gif") as gif:
            for index, frame in enumerate(frames):
                gif.seek(index)
                slots += [colours.index(tuple(frame[0, 0]))] * (gif.info["duration"] // 80)
        self.assertEqual(slots, sequence)

    def test_the_time_matches_whether_or_not_frames_merge_at_15_fps(self):
        # Within one centisecond (the GIF's resolution) of frames * frame_ms, merged or not.
        frame_ms = 1000 / 15
        for sequence in ([5] * 6, [3, 4, 5, 6, 7, 8], [3, 4, 5, 4, 3], [5]):
            with self.subTest(sequence=sequence):
                self.assertLessEqual(abs(self.total_for(sequence, frame_ms) - len(sequence) * frame_ms), 10)


class SaveGifInputTests(Folder):
    def gif_bytes(self, frames):
        figures.save_gif(frames, self.folder / "x.gif", 40)
        return (self.folder / "x.gif").read_bytes()

    def test_rgba_and_palette_images_give_their_rgb_equivalents_bytes(self):
        rng = np.random.default_rng(5)
        rgb = [Image.fromarray(rng.integers(0, 256, (6, 5, 3), dtype=np.uint8)) for _ in range(2)]
        expected = self.gif_bytes(rgb)
        self.assertEqual(self.gif_bytes([image.convert("RGBA") for image in rgb]), expected)
        paletted = [image.convert("P") for image in rgb]
        self.assertEqual(self.gif_bytes(paletted), self.gif_bytes([image.convert("RGB") for image in paletted]))
        self.assertNotEqual(self.gif_bytes(paletted), expected)  # the 256-colour reduction really changed them

    def test_frames_of_different_sizes_raise(self):
        with self.assertRaises(ValueError):
            figures.save_gif([solid((1, 2, 3), 4, 4), solid((1, 2, 3), 5, 4)], self.folder / "x.gif", 40)


class OklabTests(Folder):
    def test_white_black_and_greys(self):
        greys = np.array([[level] * 3 for level in range(0, 256, 15)] + [[255] * 3], np.uint8)
        lab = figures.oklab(greys)
        self.assertTrue(np.allclose(lab[-1], [1, 0, 0], atol=1e-3))
        self.assertTrue(np.allclose(lab[0], 0, atol=1e-9))
        self.assertTrue(np.allclose(lab[:, 1:], 0, atol=1e-3))
        self.assertTrue(np.all(np.diff(lab[:-1, 0]) > 0))

    def test_red_matches_the_published_oklab_value(self):
        lab = figures.oklab(np.array([[255, 0, 0]], np.uint8))[0]
        self.assertTrue(np.allclose(lab, [0.6280, 0.2249, 0.1258], atol=1e-3))

    def test_save_gif_picks_the_oklab_nearest_entry_not_the_rgb_nearest(self):
        table = np.array(palette_colours(), np.int64)
        lab_table = figures.oklab(table)
        rng = np.random.default_rng(11)
        found = None
        for colour in rng.integers(0, 256, (3000, 3)):
            by_rgb = int(((table - colour) ** 2).sum(1).argmin())
            by_lab = int(((lab_table - figures.oklab(colour[None])) ** 2).sum(1).argmin())
            if by_rgb != by_lab:
                found = colour, by_rgb, by_lab
                break
        self.assertIsNotNone(found, "no colour whose OKLab and RGB nearest entries differ")
        colour, by_rgb, by_lab = found
        figures.save_gif([np.tile(colour.astype(np.uint8), (2, 2, 1))], self.folder / "a.gif", 50)
        shown, _ = self.gif_frames(self.folder / "a.gif")
        self.assertEqual(tuple(shown[0][0, 0]), tuple(table[by_lab]))
        self.assertNotEqual(tuple(table[by_lab]), tuple(table[by_rgb]))


class ThemeInPaletteTests(Folder):
    def test_the_palette_starts_with_the_theme_in_order(self):
        self.assertEqual(palette_colours()[:len(THEME)], [figures.rgb(name) for name in THEME])

    def test_every_theme_colour_round_trips_through_save_gif_exactly(self):
        wanted = [figures.rgb(name) for name in THEME]
        figures.save_gif([solid(colour) for colour in wanted], self.folder / "t.gif", 100)
        shown, _ = self.gif_frames(self.folder / "t.gif")
        self.assertEqual([tuple(int(v) for v in frame[0, 0]) for frame in shown], wanted)


class ReadVideoTests(Folder):
    def test_rows_and_channels_come_back_for_widths_with_and_without_row_padding(self):
        rng = np.random.default_rng(7)
        for width in (1, 2, 3, 4, 5, 8):
            with self.subTest(width=width):
                pictures = [rng.integers(0, 256, (3, width, 3), dtype=np.uint8) for _ in range(3)]
                path = self.folder / f"w{width}.avi"
                path.write_bytes(avi_bytes(width, 3, pictures, 40))
                fps, frames = read_video(path)
                frames = list(frames)
                self.assertEqual(fps, 25.0)
                self.assertEqual(len(frames), 3)
                for got, picture in zip(frames, pictures):
                    self.assertEqual(got.shape, (3, width, 3))
                    self.assertEqual(got.dtype, np.uint8)
                    self.assertTrue(got.flags["C_CONTIGUOUS"])
                    self.assertTrue(np.array_equal(got, picture))


def chunk_types(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    types, at = [], 8
    while at < len(data):
        (length,) = struct.unpack_from(">I", data, at)
        types.append(data[at + 4:at + 8].decode("ascii"))
        at += 12 + length
    return types


class SavePngMoreTests(Folder):
    def test_the_callers_info_is_not_mutated(self):
        picture = Image.new("RGB", (3, 2), (1, 2, 3))
        picture.info["comment"] = "keep"
        figures.save_png(picture, self.folder / "a.png")
        self.assertEqual(picture.info, {"comment": "keep"})

    def test_an_rgba_picture_keeps_pixels_and_alpha(self):
        picture = Image.new("RGBA", (3, 2), (10, 20, 30, 255))
        picture.putpixel((1, 0), (200, 100, 50, 128))
        picture.putpixel((2, 1), (0, 0, 0, 0))
        figures.save_png(picture, self.folder / "a.png")
        with Image.open(self.folder / "a.png") as written:
            self.assertEqual(written.mode, "RGBA")
            self.assertEqual(list(written.getdata()), list(picture.getdata()))

    def test_the_png_command_writes_no_text_or_physical_size_chunks(self):
        tagged = PngImagePlugin.PngInfo()
        tagged.add_text("Software", "anything")
        Image.new("RGB", (3, 2), (10, 20, 30)).save(self.folder / "t.png", pnginfo=tagged, dpi=(300, 300))
        before = chunk_types((self.folder / "t.png").read_bytes())
        self.assertIn("tEXt", before)
        self.assertIn("pHYs", before)
        figures.main(["png", str(self.folder / "t.png"), str(self.folder / "o.png")])
        after = chunk_types((self.folder / "o.png").read_bytes())
        self.assertEqual(sorted(set(after) & {"tEXt", "pHYs", "iTXt", "zTXt", "tIME"}), [])
        self.assertIn("IDAT", after)


class QuarterTests(Folder):
    """A marked top-left corner of a 3 wide, 2 high picture, turned counterclockwise: (shape, marker row/col)."""

    EXPECTED = {0: ((2, 3), (0, 0)), 1: ((3, 2), (2, 0)), 2: ((2, 3), (1, 2)), 3: ((3, 2), (0, 1))}

    def test_gif_turns_each_quarter_counterclockwise(self):
        colours = palette_colours()
        picture = solid(colours[5])
        picture[0, 0] = colours[100]
        self.folder.joinpath("q.avi").write_bytes(avi_bytes(3, 2, [picture], 100))
        for quarter, (shape, marker) in self.EXPECTED.items():
            with self.subTest(quarter=quarter):
                out = self.folder / f"q{quarter}.gif"
                figures.main(["gif", str(self.folder / "q.avi"), str(out), "--fps", "10", "--quarter", str(quarter)])
                shown, _ = self.gif_frames(out)
                self.assertEqual(shown[0].shape[:2], shape)
                at = np.argwhere((shown[0] == colours[100]).all(2))
                self.assertEqual(at.tolist(), [list(marker)])

    def test_png_turns_each_quarter_the_same_way(self):
        picture = Image.new("RGB", (3, 2), (0, 0, 255))
        picture.putpixel((0, 0), (255, 0, 0))
        picture.save(self.folder / "q.bmp")
        for quarter, (shape, marker) in self.EXPECTED.items():
            with self.subTest(quarter=quarter):
                out = self.folder / f"q{quarter}.png"
                figures.main(["png", str(self.folder / "q.bmp"), str(out), "--quarter", str(quarter)])
                with Image.open(out) as written:
                    pixels = np.asarray(written.convert("RGB"))
                self.assertEqual(pixels.shape[:2], shape)
                at = np.argwhere((pixels == (255, 0, 0)).all(2))
                self.assertEqual(at.tolist(), [list(marker)])


if __name__ == "__main__":
    unittest.main()
