"""Tests for probe_fit.py: the probe line it reads, the fit, and the shipped
touch correction held against the captures it was fitted from."""

import os
import re
import tempfile
import unittest

import probe_fit

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "fixtures")
TOUCH_C = os.path.join(HERE, "..", "..", "..", "input", "touch.c")


def probe_line(ax, ay, fx, fy, q=0, mode="random", cal=0):
    """A probe line shaped exactly as the app logs one."""
    return (f"input_lab: probe 7 {mode} q{q} aim 1,2 aim_panel {ax},{ay} first 3,4 settled 5,6 release 7,8 "
            f"panel_first {fx},{fy} panel_settled 9,9 panel_release 9,9 held 90 idle 500 samples 12 miss cal {cal}")


def fixture(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as f:
        return probe_fit.parse(f)


class ParseTest(unittest.TestCase):
    def test_reads_aim_and_first_contact_in_panel_coordinates(self):
        [p] = probe_fit.parse([probe_line(100, 200, 110, 230, q=1, cal=1)])
        self.assertEqual((p["ax"], p["ay"], p["fx"], p["fy"], p["q"], p["cal"]), (100, 200, 110, 230, 1, 1))

    def test_ignores_other_console_lines(self):
        self.assertEqual(probe_fit.parse(["input_lab: round random screen 448x368 quarter 1", "shell: x"]), [])


class FitTest(unittest.TestCase):
    TRUE = ((1.1, 0.02, -20.0), (0.01, 1.2, -30.0))

    def taps(self):
        (a, b, c), (d, e, f) = self.TRUE
        out = []
        for ax in range(60, 320, 37):
            for ay in range(60, 400, 41):
                fx, fy = a * ax + b * ay + c, d * ax + e * ay + f
                out.append({"ax": ax, "ay": ay, "fx": fx, "fy": fy, "q": 0, "cal": 0, "mode": "random"})
        return out

    def test_recovers_a_known_panel_map(self):
        fitted = probe_fit.fit(self.taps())
        for got, want in zip(fitted[0] + fitted[1], self.TRUE[0] + self.TRUE[1]):
            self.assertAlmostEqual(got, want, places=6)

    def test_correcting_with_the_fit_lands_on_the_aim(self):
        taps = self.taps()
        self.assertLess(max(probe_fit.misses(taps, probe_fit.fit(taps))), 1e-6)

    def test_leaves_out_edge_clamped_and_corner_taps(self):
        clamped = {"ax": 200, "ay": 440, "fx": 210, "fy": 447}
        corner = {"ax": 10, "ay": 12, "fx": 50, "fy": 60}
        inside = {"ax": 200, "ay": 200, "fx": 210, "fy": 220}
        self.assertEqual([probe_fit.usable(p) for p in (clamped, corner, inside)], [False, False, True])

    def test_refuses_to_fit_when_every_tap_was_already_corrected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "corrected_only.log")
            with open(path, "w", encoding="utf-8") as f:
                f.write(probe_line(100, 200, 110, 230, mode="grid", cal=1) + "\n")
            with self.assertRaises(SystemExit):
                probe_fit.main([path])


class ShippedCorrectionTest(unittest.TestCase):
    """The touch driver's PANEL_FIT must be the fit of the uncorrected
    capture it came from: a changed coefficient without a new capture, or a
    new capture without a changed coefficient, fails here."""

    def shipped(self):
        with open(TOUCH_C, encoding="utf-8") as f:
            source = f.read()
        body = re.search(r"PANEL_FIT = \{(.*?)\}", source, re.S).group(1)
        return {k: float(v) for k, v in re.findall(r"\.(\w+) = (-?[\d.]+)f", body)}

    def test_the_shipped_fit_is_the_uncorrected_capture_fitted(self):
        probes = [p for p in fixture("uncorrected.log") if probe_fit.usable(p) and p["mode"] == "random"]
        (xx, xy, x0), (yx, yy, y0) = probe_fit.fit(probes)
        shipped = self.shipped()
        for name, got in dict(xx=xx, xy=xy, x0=x0, yx=yx, yy=yy, y0=y0).items():
            self.assertAlmostEqual(shipped[name], got, delta=0.1 if name.endswith("0") else 0.002, msg=name)

    def test_the_corrected_capture_misses_less_than_the_uncorrected_one(self):
        def median_miss(name):
            probes = [p for p in fixture(name) if probe_fit.usable(p) and p["mode"] == "random"]
            return sorted(probe_fit.misses(probes))[len(probes) // 2]

        self.assertLess(median_miss("corrected.log"), 0.7 * median_miss("uncorrected.log"))


if __name__ == "__main__":
    unittest.main()
