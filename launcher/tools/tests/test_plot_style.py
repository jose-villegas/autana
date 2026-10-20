import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from docs import plot_style


class SaveTests(unittest.TestCase):
    def test_save_makes_the_folder_and_closes_the_figure(self):
        fig, _ = plot_style.subplots(1, 1, (2, 2))
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "deep" / "chart.png"
            plot_style.save(fig, path)
            self.assertTrue(path.read_bytes().startswith(b"\x89PNG"))
        self.assertNotIn(fig.number, plot_style.plt.get_fignums())


if __name__ == "__main__":
    unittest.main()
