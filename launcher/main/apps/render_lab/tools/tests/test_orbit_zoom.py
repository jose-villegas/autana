"""render_lab.orbit_zoom sets where an orbiting camera starts, within the scene's zoom.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests -p test_orbit_zoom.py

Builds the host renderer once and draws the capybara scene's first frame at
several orbit_zoom values, reading where its camera stands from --poses. The
camera moves along one line from the subject, so the step from 100 to 200
is twice the step from 50 to 100 and the camera keeps its turn; past the
scene's farthest zoom it stops.
"""
import csv
import math
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import host_render  # noqa: E402

SCENE = "capybara"
FITS = 100  # LAB_ORBIT_ZOOM_UNIT: the distance that fits the subject
HALF = FITS // 2
DOUBLE = 2 * FITS
# scene_capybara.c's CAPYBARA_FRAME.zoom_out is 3; orbit_zoom's top is 4 (LAB_ORBIT_ZOOM_MAX).
FARTHEST = 3 * FITS
PAST_FARTHEST = 4 * FITS
PLACES = 4  # the poses file's floats agree to this many decimals


class OrbitZoom(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = pathlib.Path(tempfile.mkdtemp(prefix="orbit_zoom_"))
        run, cls.binary = host_render.run(cls.out, "--build-only")
        cls.build_output = run.stdout + run.stderr

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def pose(self, zoom):
        """The camera's position and rotation in the first frame at that orbit_zoom."""
        self.assertTrue(self.binary.exists(), self.build_output[-2000:])
        poses = self.out / f"zoom-{zoom}.csv"
        run = subprocess.run([str(self.binary), "--quarter", "0", "--no-hud", "--scene", SCENE, "--frames", "1",
                              "--set", f"render_lab.orbit_zoom={zoom}", "--poses", str(poses),
                              "-o", str(self.out / f"zoom-{zoom}.bmp")],
                             capture_output=True, text=True, timeout=600)
        self.assertEqual(run.returncode, 0, run.stderr[-2000:])
        with poses.open(newline="") as rows:
            row = next(csv.DictReader(rows))
        return ([float(row[k]) for k in ("px", "py", "pz")], [float(row[k]) for k in ("qx", "qy", "qz", "qw")])

    def test_the_camera_moves_along_one_line_in_proportion(self):
        (half, half_turn), (fits, fits_turn), (double, double_turn) = (self.pose(z) for z in (HALF, FITS, DOUBLE))
        self.assertAlmostEqual(math.dist(fits, double), 2 * math.dist(half, fits), places=PLACES)
        self.assertAlmostEqual(math.dist(half, double), 3 * math.dist(half, fits), places=PLACES)
        for turn in (half_turn, double_turn):
            for got, want in zip(turn, fits_turn):
                self.assertAlmostEqual(got, want, places=PLACES)

    def test_a_zoom_past_the_scene_s_farthest_stops_there(self):
        farthest, _ = self.pose(FARTHEST)
        past, _ = self.pose(PAST_FARTHEST)
        self.assertAlmostEqual(math.dist(farthest, past), 0.0, places=PLACES)


if __name__ == "__main__":
    unittest.main()
