import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from docs import plot_style


class BarsTests(unittest.TestCase):
    def draw(self, categories, series):
        fig, axis = plot_style.subplots(1, 1, (4, 3))
        plot_style.bars(axis, categories, series)
        fig.canvas.draw()
        return fig, axis

    def test_value_axis_starts_at_zero_even_when_every_value_is_large(self):
        fig, axis = self.draw(["a", "b"], {"x": [90, 95]})
        self.assertEqual(axis.get_ylim()[0], 0)
        plot_style.plt.close(fig)

    def test_long_labels_turn_and_short_ones_stay(self):
        fig, axis = self.draw(["a", "b", "c"], {"x": [1, 2, 3]})
        self.assertEqual(axis.get_xticklabels()[0].get_rotation(), 0)
        plot_style.plt.close(fig)
        fig, axis = self.draw(["a long category name", "another long name", "third long name"], {"x": [1, 2, 3]})
        self.assertEqual(axis.get_xticklabels()[0].get_rotation(), plot_style.LABEL_ANGLE)
        plot_style.plt.close(fig)

    def test_save_makes_the_folder_and_closes_the_figure(self):
        fig, _ = self.draw(["a"], {"x": [1]})
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "deep" / "chart.png"
            plot_style.save(fig, path)
            self.assertTrue(path.read_bytes().startswith(b"\x89PNG"))
        self.assertNotIn(fig.number, plot_style.plt.get_fignums())


if __name__ == "__main__":
    unittest.main()
