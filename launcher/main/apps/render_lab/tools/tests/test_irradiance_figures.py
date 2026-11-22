"""Check irradiance generator scheduling and independent view/skin flags."""
import json
import pathlib
import numpy as np
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import irradiance_figures as figures


class IrradianceFiguresTests(unittest.TestCase):
    def test_stills_select_complete_exported_frames(self):
        frames = [{"frame": index, "camera": {"world": np.eye(4).tolist(), "lens": 1, "near": 0.1},
                   "skins": [{"name": "skin", "clip": "move", "time": index / 10,
                              "blend": 1, "frozen_previous": False, "world": np.eye(4).tolist()},
                             {"name": "skin", "clip": "move", "time": index / 10,
                              "blend": blend, "frozen_previous": False, "world": np.eye(4).tolist()}]}
                  for index, blend in enumerate((1, 0.5, 1, 1))]
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            poses = work / "pose.json"
            poses.write_text(json.dumps(frames))
            pictures = [np.full((8, 8, 3), index, np.uint8) for index in range(len(frames))]
            with mock.patch.object(figures, "read_video", return_value=(10, iter(pictures))):
                folders = figures.matched_stills(work / "clip.avi", poses, [100, 200, 300], 100, "skin", work)
            self.assertEqual(len(folders), 3)
            for folder, index in zip(folders, (0, 2, 3)):
                self.assertEqual(json.loads((folder / "pose.json").read_text())[0], frames[index])
                with figures.Image.open(folder / "ours.bmp") as image:
                    self.assertEqual(image.getpixel((0, 0)), (index,) * 3)
            for frame in frames:
                frame["skins"][0]["frozen_previous"] = True
            poses.write_text(json.dumps(frames))
            with self.assertRaisesRegex(ValueError, "no host frames"):
                figures.matched_stills(work / "clip.avi", poses, [0, 300], 100, "skin", work)

    def test_reference_uses_export_without_rebuilding_motion(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(figures.subprocess, "run") as run:
            folder = pathlib.Path(directory)
            figures.Image.new("RGB", (32, 24)).save(folder / "ours.bmp")
            figures.reference(folder, pathlib.Path("scene.toml"), pathlib.Path("rig.glb"), "skin", 4)
            command = run.call_args.args[0]
            self.assertEqual(command[command.index("--pose-json") + 1], str(folder / "pose.json"))
            self.assertNotIn("--time", command)
            self.assertNotIn("--at", command)

    def test_default_stills_span_run(self):
        self.assertEqual(figures.pose_times(18500), [4625, 13875])

    def test_white_override_and_probe_view_reach_host(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(figures.subprocess, "run") as run:
            output = pathlib.Path(folder) / "pose.bmp"
            figures.render(pathlib.Path("renderer"), "test-scene", output, "--frames", 2)
            command = run.call_args.args[0]
            self.assertEqual(command[command.index("--view") + 1], "probes")
            self.assertEqual(command[command.index("--set") + 1], "skin.albedo=1")


if __name__ == "__main__":
    unittest.main()
