"""save_gif writes the frame delays it is given, and shrink_gif merges short frames without changing a GIF's length."""
import pathlib
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image, ImageSequence

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from docs import figures  # noqa: E402

# Consecutive frames differ, so Pillow does not merge two into one on saving.
COLOURS = ("red", "green", "blue", "white", "black", "yellow")


def gif_of(path, delays, size=(40, 20)):
    figures.save_gif([Image.new("RGB", size, COLOURS[index]) for index in range(len(delays))], path, delays=delays)


def first_pixels(path):
    """The top-left colour of every frame of the GIF at `path`."""
    with Image.open(path) as gif:
        return [frame.convert("RGB").getpixel((0, 0)) for frame in ImageSequence.Iterator(gif)]


class Folder(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.folder = pathlib.Path(directory.name)

    def shrunk(self, delays, min_ms, width=160, size=(40, 20)):
        gif_of(self.folder / "in.gif", delays, size)
        figures.shrink_gif(self.folder / "in.gif", self.folder / "out.gif", width, min_ms)
        return self.folder / "out.gif"


class SaveGifDelaysTests(Folder):
    def frames(self, count):
        return [Image.new("RGB", (4, 4), COLOURS[index]) for index in range(count)]

    def test_delays_are_written_exactly_whatever_sequence_holds_them(self):
        for kind in (list, tuple, np.array):
            with self.subTest(kind=kind.__name__):
                out = self.folder / "a.gif"
                figures.save_gif(self.frames(3), out, delays=kind([30, 70, 120]))
                self.assertEqual(figures.gif_delays(out), [30, 70, 120])

    def test_frame_ms_and_delays_together_are_refused(self):
        with self.assertRaisesRegex(ValueError, "not both or neither"):
            figures.save_gif(self.frames(2), self.folder / "a.gif", 100, delays=[100, 100])
        self.assertFalse((self.folder / "a.gif").exists())

    def test_neither_frame_ms_nor_delays_is_refused(self):
        with self.assertRaisesRegex(ValueError, "not both or neither"):
            figures.save_gif(self.frames(2), self.folder / "a.gif")

    def test_a_delay_count_that_is_not_the_frame_count_is_refused(self):
        for delays in ([100], [100, 100, 100]):
            with self.subTest(count=len(delays)):
                with self.assertRaisesRegex(ValueError, f"{len(delays)} delays for 2 frames"):
                    figures.save_gif(self.frames(2), self.folder / "a.gif", delays=delays)


class GifDelaysTests(Folder):
    def test_a_frame_with_no_duration_counts_as_zero(self):
        frames = [Image.new("P", (4, 4), index) for index in range(2)]
        for frame in frames:
            frame.putpalette([0, 0, 0, 255, 255, 255] + [0] * 750)
        frames[0].save(self.folder / "a.gif", save_all=True, append_images=frames[1:])
        with Image.open(self.folder / "a.gif") as gif:
            self.assertNotIn("duration", gif.info)
        self.assertEqual(figures.gif_delays(self.folder / "a.gif"), [0, 0])


class ShrinkGifTests(Folder):
    def test_a_frame_exactly_at_the_minimum_stands_alone(self):
        out = self.shrunk([200, 300, 300], 200)
        self.assertEqual(figures.gif_delays(out), [200, 300, 300])

    def test_a_frame_just_under_the_minimum_absorbs_the_next(self):
        out = self.shrunk([190, 100, 300, 300], 200)
        self.assertEqual(figures.gif_delays(out), [290, 300, 300])
        source = first_pixels(self.folder / "in.gif")
        self.assertEqual(first_pixels(out), [source[0], source[2], source[3]])

    def test_a_short_last_frame_folds_into_the_one_before(self):
        out = self.shrunk([300, 400, 100], 200)
        self.assertEqual(figures.gif_delays(out), [300, 500])
        source = first_pixels(self.folder / "in.gif")
        self.assertEqual(first_pixels(out), source[:2])

    def test_a_short_last_frame_of_two_folds_into_the_first(self):
        out = self.shrunk([300, 100], 200)
        self.assertEqual(figures.gif_delays(out), [400])

    def test_a_short_only_frame_stays(self):
        out = self.shrunk([100], 200)
        self.assertEqual(figures.gif_delays(out), [100])

    def test_unequal_delays_keep_their_total(self):
        delays = [150, 50, 50, 300, 300]
        out = self.shrunk(delays, 200)
        self.assertEqual(figures.gif_delays(out), [200, 350, 300])
        self.assertEqual(sum(figures.gif_delays(out)), sum(delays))

    def test_a_gif_whose_frames_have_no_duration_merges_as_zero(self):
        frames = [Image.new("P", (4, 4), index) for index in range(3)]
        for frame in frames:
            frame.putpalette([0, 0, 0, 255, 255, 255, 255, 0, 0] + [0] * 747)
        frames[0].save(self.folder / "in.gif", save_all=True, append_images=frames[1:])
        figures.shrink_gif(self.folder / "in.gif", self.folder / "out.gif", 160, 200)
        self.assertEqual(figures.gif_delays(self.folder / "out.gif"), [0])

    def test_a_wide_gif_is_scaled_to_the_width_and_keeps_its_shape(self):
        out = self.shrunk([300, 300], 200, width=160, size=(320, 80))
        with Image.open(out) as gif:
            self.assertEqual(gif.size, (160, 40))

    def test_a_gif_narrower_than_the_width_is_not_upscaled(self):
        out = self.shrunk([300, 300], 200, width=160, size=(40, 20))
        with Image.open(out) as gif:
            self.assertEqual(gif.size, (40, 20))


if __name__ == "__main__":
    unittest.main()
