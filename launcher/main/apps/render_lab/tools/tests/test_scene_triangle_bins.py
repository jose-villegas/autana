"""Scene bin means preserve all frames and reject incomplete counted fixtures."""
import csv
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import scene_triangle_bins as bins


class SceneTriangleBinsTest(unittest.TestCase):
    def test_fixture_means_and_totals(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "counts.csv"
            with path.open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=[*bins.BIN_FIELDS, "triangles_set_up"])
                writer.writeheader()
                for multiplier in (1, 3):
                    row = {name: i * multiplier for i, name in enumerate(bins.BIN_FIELDS)}
                    writer.writerow({**row, "triangles_set_up": sum(row.values())})
            self.assertEqual({name: i * 2 for i, name in enumerate(bins.BIN_FIELDS)}, bins.mean_bins(path))
            text = path.read_text().splitlines()
            text[-1] = text[-1].rsplit(",", 1)[0] + ",0"
            path.write_text("\n".join(text) + "\n")
            with self.assertRaisesRegex(ValueError, "sum"):
                bins.mean_bins(path)

    def test_generator_passes_scene_frames_portrait_and_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            commands = {"fixture": ["0:example tour"]}
            def render(command, **kwargs):
                self.assertEqual("fixture", command[command.index("--scene") + 1])
                self.assertEqual("0", command[command.index("--quarter") + 1])
                self.assertEqual("3", command[command.index("--frames") + 1])
                self.assertEqual("cull", command[command.index("--view") + 1])
                self.assertEqual("0:example tour", command[command.index("--console") + 1])
                path = pathlib.Path(command[command.index("--counts") + 1])
                with path.open("w", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=[*bins.BIN_FIELDS, "triangles_set_up"])
                    writer.writeheader()
                    writer.writerow({**dict.fromkeys(bins.BIN_FIELDS, 1), "triangles_set_up": len(bins.BIN_FIELDS)})
            with mock.patch.object(bins.subprocess, "run", side_effect=render):
                result = bins.generate(folder / "renderer", [("fixture", 3)], folder, commands)
            self.assertEqual({"fixture": dict.fromkeys(bins.BIN_FIELDS, 1)}, result["scenes"])
            self.assertEqual(commands, result["console"])
            self.assertEqual({"fixture": 3}, result["frames"])


if __name__ == "__main__":
    unittest.main()
