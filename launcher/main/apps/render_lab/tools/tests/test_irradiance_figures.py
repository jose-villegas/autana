"""Check irradiance generator scheduling and independent view/skin flags."""
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import irradiance_figures as figures


class IrradianceFiguresTests(unittest.TestCase):
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
