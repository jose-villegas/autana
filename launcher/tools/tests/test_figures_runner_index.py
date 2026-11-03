"""figures_runner.py index writes or checks the figure index, naming each image's producer, and fails only on a
difference found by --check."""
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "docs"))
import figure_index  # noqa: E402
import figures_runner  # noqa: E402
import producer  # noqa: E402

PAGE = (f"# Figures\n\n<!-- generated: {figure_index.BLOCK} check: {figure_index.CHECK} -->\n"
        f"<!-- /generated: {figure_index.BLOCK} -->\n")
SCRIPT = "launcher/tools/math/plots.py"


def found_of(*outputs, area="math"):
    declared = producer.Producer(area=area, needs=("host",), outputs=tuple(outputs))
    return {SCRIPT: producer.Found(SCRIPT, figures_runner.ROOT / SCRIPT, declared)}


class OwnerTests(unittest.TestCase):
    def test_an_image_name_is_the_path_under_docs_images_with_or_without_that_prefix(self):
        self.assertEqual(figures_runner.image_name("docs/images/math/a.png"), "math/a.png")
        self.assertEqual(figures_runner.image_name("math/a.png"), "math/a.png")

    def test_the_producer_of_an_image_is_the_one_claiming_it_by_name_or_glob(self):
        found = found_of("a.png", "fast-*.png")
        for image in ("docs/images/math/a.png", "math/a.png", "docs/images/math/fast-3.png"):
            with self.subTest(image=image):
                self.assertEqual(figures_runner.producer_of(found, image).name, SCRIPT)

    def test_an_image_no_producer_claims_has_none(self):
        found = found_of("a.png", "fast-*.png")
        for image in ("docs/images/math/b.png", "docs/images/other/a.png", "docs/images/math/fast.png"):
            with self.subTest(image=image):
                self.assertIsNone(figures_runner.producer_of(found, image))


class IndexCommandTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        (self.root / figure_index.PAGE).parent.mkdir(parents=True)
        (self.root / figure_index.PAGE).write_text(PAGE, encoding="utf-8", newline="\n")
        images = self.root / "docs/images/math"
        images.mkdir(parents=True)
        Image.new("RGB", (40, 10), "black").save(images / "claimed.png")
        Image.new("RGB", (40, 10), "white").save(images / "loose.png")
        (self.root / "docs/math").mkdir()
        (self.root / "docs/math/One.md").write_text("![A](../images/math/claimed.png)\n", encoding="utf-8")
        real = figure_index.index
        patches = (mock.patch.object(figures_runner, "find", return_value=found_of("claimed.png")),
                   mock.patch.object(figure_index, "index",
                                     lambda owner, check=False: real(owner, check, self.root)))
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = figures_runner.main(["index", *argv])
        return status, out.getvalue(), err.getvalue()

    def page(self):
        return (self.root / figure_index.PAGE).read_text(encoding="utf-8")

    def test_writing_exits_0_though_it_changed_the_page(self):
        status, out, _ = self.run_main()
        self.assertEqual(status, 0)
        self.assertIn(f"changed {figure_index.PAGE}", out)
        self.assertIn("## math", self.page())

    def test_check_exits_1_with_differences_and_writes_nothing(self):
        status, out, _ = self.run_main("--check")
        self.assertEqual(status, 1)
        self.assertIn(f"changed {figure_index.PAGE}", out)
        self.assertEqual(self.page(), PAGE)

    def test_check_exits_0_once_written_though_an_image_is_shown_nowhere(self):
        self.run_main()
        status, out, err = self.run_main("--check")
        self.assertEqual((status, out), (0, ""))
        self.assertIn("shown nowhere docs/images/math/loose.png", err)

    def test_the_page_names_the_script_that_declares_an_image_and_says_so_when_none_does(self):
        self.run_main()
        rows = {line.split('href="')[1].split('"')[0]: line for line in self.page().splitlines() if 'href="' in line}
        self.assertIn(f"[plots.py](../../{SCRIPT})", rows["../images/math/claimed.png"])
        self.assertNotIn("not declared yet", rows["../images/math/claimed.png"])
        self.assertTrue(rows["../images/math/loose.png"].endswith("| not declared yet |"))


class IndexDispatchTests(unittest.TestCase):
    def status(self, differences, *argv):
        with mock.patch.object(figures_runner, "find", return_value=found_of("a.png")), \
                mock.patch.object(figure_index, "index", return_value=differences) as index, \
                contextlib.redirect_stdout(io.StringIO()):
            return figures_runner.main(["index", *argv]), index

    def test_only_a_check_with_differences_fails(self):
        self.assertEqual(self.status(["changed x"])[0], 0)
        self.assertEqual(self.status(["changed x"], "--check")[0], 1)
        self.assertEqual(self.status([], "--check")[0], 0)
        self.assertEqual(self.status([])[0], 0)

    def test_the_owner_given_is_the_producer_s_script_or_none(self):
        _, index = self.status([])
        owner = index.call_args.args[0]
        self.assertEqual(owner("docs/images/math/a.png"), SCRIPT)
        self.assertFalse(owner("docs/images/math/b.png"))


if __name__ == "__main__":
    unittest.main()
