"""figures.py writes every doc PNG and GIF one way: PNGs without metadata, GIFs on the one global palette."""
import pathlib
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image, PngImagePlugin

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from docs import figures  # noqa: E402
from test_render_compare import avi_bytes  # noqa: E402


def palette_colours():
    return [tuple(int(name[at:at + 2], 16) for at in (1, 3, 5)) for name in figures.GIF_PALETTE]


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
    def test_a_lower_rate_takes_the_nearest_source_frame(self):
        self.assertEqual(figures.clip(range(10), 10, 5), [0, 2, 4, 6, 8])
        self.assertEqual(figures.clip(range(10), 10, 4), [0, 3, 5, 8])

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


if __name__ == "__main__":
    unittest.main()
