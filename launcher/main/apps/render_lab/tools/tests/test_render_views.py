"""The host render's --view shaded|depth|tiles, checked against each other.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests

Runs render_lab_render_host.sh once, then reads the BMPs back: a pixel is
empty in the depth view exactly where the shaded render shows the clear
colour, in each of the three ways the panel is turned; a tile is empty
wherever any pixel of it is; and a view the scene cannot give, an unknown
view name, or a picture that cannot be written fails the run rather than
reporting an image.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import host_render  # noqa: E402

# One label per way the panel is turned: portrait, landscape, landscape flipped.
TURNS = {
    "portrait": ("sponza-portrait", "sponza-depth-portrait", "sponza-tiles-portrait"),
    "landscape": ("sponza-landscape", "sponza-depth-landscape", "sponza-tiles-landscape"),
    "flipped": ("sponza-flipped", "sponza-depth-flipped", "sponza-tiles-flipped"),
}
GREY_SLACK = 12  # the 565 expansion makes a grey's channels differ by a few levels


def read_bmp(path):
    """A 24-bit bottom-up BMP as (width, height, list of rows of (r, g, b))."""
    data = path.read_bytes()
    offset = int.from_bytes(data[10:14], "little")
    width = int.from_bytes(data[18:22], "little", signed=True)
    height = int.from_bytes(data[22:26], "little", signed=True)
    assert int.from_bytes(data[28:30], "little") == 24, "not a 24-bit BMP"
    assert height > 0, "not bottom-up"
    stride = (width * 3 + 3) // 4 * 4
    rows = []
    for y in range(height):
        start = offset + (height - 1 - y) * stride
        rows.append([(data[start + 3 * x + 2], data[start + 3 * x + 1], data[start + 3 * x]) for x in range(width)])
    return width, height, rows


def is_grey(px):
    return max(px) - min(px) <= GREY_SLACK


class RenderViews(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = pathlib.Path(tempfile.mkdtemp(prefix="render_views_"))
        run, cls.binary = host_render.run(cls.out)
        cls.run_output = run.stdout + run.stderr
        cls.built = run.returncode == 0

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def setUp(self):
        self.assertTrue(self.built, self.run_output[-2000:])

    def images(self, turn):
        return [read_bmp(self.out / (label + ".bmp")) for label in TURNS[turn]]

    def clear_colour(self, depth_rows):
        """The one colour of the depth view that is no grey."""
        colours = {px for row in depth_rows for px in row if not is_grey(px)}
        self.assertEqual(1, len(colours), "empty must be one flat colour, no grey: %r" % sorted(colours)[:5])
        return colours.pop()

    def test_the_depth_is_empty_exactly_where_the_shaded_render_shows_the_clear_colour(self):
        for turn in TURNS:
            with self.subTest(turn=turn):
                (sw, sh, shaded), (dw, dh, depth), (tw, th, tiles) = self.images(turn)
                self.assertEqual((sw, sh), (dw, dh))
                self.assertEqual((sw, sh), (tw, th))
                clear = self.clear_colour(depth)
                greys = 0
                for y in range(sh):
                    for x in range(sw):
                        empty_in_shaded = shaded[y][x] == clear
                        self.assertEqual(empty_in_shaded, depth[y][x] == clear, "row %d column %d" % (y, x))
                        greys += is_grey(depth[y][x])
                self.assertGreater(greys, 0, "the depth view drew no grey")
                self.assertGreater(sum(row.count(clear) for row in depth), 0, "the frame has no empty pixel to check")

    def test_a_tile_is_empty_wherever_any_pixel_of_it_is(self):
        for turn in TURNS:
            with self.subTest(turn=turn):
                _, (_, _, depth), (_, _, tiles) = self.images(turn)
                clear = self.clear_colour(depth)
                for y, row in enumerate(depth):
                    for x, px in enumerate(row):
                        if px == clear:
                            self.assertEqual(clear, tiles[y][x], "row %d column %d" % (y, x))
                self.assertGreater(
                    sum(r.count(clear) for r in tiles), sum(r.count(clear) for r in depth), "no tile was emptied"
                )

    def render(self, *args):
        target = self.out / "probe.bmp"
        target.unlink(missing_ok=True)
        run = subprocess.run(
            [str(self.binary), "--quarter", "1", "--no-hud", "--frames", "1", *args, "-o", str(target)],
            capture_output=True,
            text=True,
            timeout=120,
        )
        return run, target

    def test_a_scene_that_draws_no_lit_mesh_cannot_be_asked_for_a_view(self):
        run, target = self.render("--scene", "gouraud", "--view", "depth")
        self.assertNotEqual(0, run.returncode)
        self.assertIn("no depth to show", run.stderr)
        self.assertFalse(target.exists() and target.stat().st_size > 0)

    def test_a_view_option_with_no_value_is_refused_and_writes_nothing(self):
        for option in ("--view", "--scene"):
            with self.subTest(option=option):
                args = ["--scene", "sponza", "--view"] if option == "--view" else ["--view", "depth", "--scene"]
                run, target = self.render(*args)
                self.assertNotEqual(0, run.returncode)
                self.assertIn("needs a value", run.stderr)
                self.assertFalse(target.exists() and target.stat().st_size > 0)

    def test_an_unknown_view_is_refused(self):
        run, _ = self.render("--scene", "sponza", "--view", "octree")
        self.assertNotEqual(0, run.returncode)
        self.assertIn("--view is shaded, depth, tiles, motion, meshlets", run.stderr)

    def test_a_picture_that_cannot_be_written_fails_the_run(self):
        missing = self.out / "no-such-folder" / "view.bmp"
        run = subprocess.run(
            [str(self.binary), "--quarter", "1", "--no-hud", "--frames", "1", "--scene", "sponza", "--view", "depth", "-o", str(missing)],
            capture_output=True,
            text=True,
            timeout=120,
        )
        self.assertNotEqual(0, run.returncode)
        self.assertIn("cannot", run.stderr)

    def test_the_shaded_view_by_name_is_the_default_render(self):
        base, _ = self.render("--scene", "sponza")
        base_bytes = (self.out / "probe.bmp").read_bytes()
        named, _ = self.render("--scene", "sponza", "--view", "shaded")
        self.assertEqual((0, 0), (base.returncode, named.returncode))
        self.assertEqual(base_bytes, (self.out / "probe.bmp").read_bytes())


if __name__ == "__main__":
    unittest.main()
