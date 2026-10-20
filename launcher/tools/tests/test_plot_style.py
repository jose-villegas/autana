import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from docs import plot_style


from PIL import Image


class SaveTests(unittest.TestCase):
    def test_save_makes_the_folder_and_closes_the_figure(self):
        fig, _ = plot_style.subplots(1, 1, (2, 2))
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "deep" / "chart.png"
            plot_style.save(fig, path)
            self.assertTrue(path.read_bytes().startswith(b"\x89PNG"))
        self.assertNotIn(fig.number, plot_style.plt.get_fignums())

    def test_gif_shared_palette_frames_and_close(self):
        frames, colors = 5, 16
        fig, axis = plot_style.subplots(1, 1, (2, 2))
        line, = axis.plot([], [], color=plot_style.SLOTS[0])
        axis.set(xlim=(0, frames), ylim=(0, frames))
        seen = []

        def draw(index):
            seen.append(index)
            line.set_data([0, index + 1], [0, index + 1])

        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "deep" / "chart.gif"
            plot_style.save_gif(fig, draw, frames, path, fps=10, colors=colors)
            with Image.open(path) as gif:
                self.assertEqual(gif.n_frames, frames)
                self.assertLessEqual(len(gif.getpalette()) // 3, colors)
                self.assertEqual(gif.info["loop"], 0)
                self.assertNotIn("comment", gif.info)
                palette = gif.getpalette()
                for index in range(frames):
                    gif.seek(index)
                    self.assertTrue(set(gif.convert("RGB").getdata()) <=
                                    set(zip(palette[::3], palette[1::3], palette[2::3])))
            first = path.read_bytes()
            self.assertNotIn(fig.number, plot_style.plt.get_fignums())
            fig, axis = plot_style.subplots(1, 1, (2, 2))
            line, = axis.plot([], [], color=plot_style.SLOTS[0])
            axis.set(xlim=(0, frames), ylim=(0, frames))
            plot_style.save_gif(fig, draw, frames, path, fps=10, colors=colors)
            self.assertEqual(first, path.read_bytes())
        self.assertEqual(seen[:frames], list(range(frames)))
        self.assertNotIn(fig.number, plot_style.plt.get_fignums())


if __name__ == "__main__":
    unittest.main()
