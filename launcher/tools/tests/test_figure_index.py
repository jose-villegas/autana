"""The figure index lists every doc image with its picture, its pages and its producer, and keeps a small
copy of each GIF that matches the GIF."""
import contextlib
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "docs"))
import figure_index  # noqa: E402
from figures import gif_delays, save_gif  # noqa: E402

PAGE = (f"# Figures\n\n<!-- generated: {figure_index.BLOCK} check: {figure_index.CHECK} -->\n"
        f"<!-- /generated: {figure_index.BLOCK} -->\n")


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


class EmbeddedTests(unittest.TestCase):
    def test_markdown_html_and_picture_sources_resolve_from_their_page(self):
        text = ('![A plot](../images/math/a.png)\n'
                '<picture><source media="(prefers-color-scheme: dark)" srcset="../images/math/b-dark.png 2x">\n'
                '<img alt="B &amp; C" src="../images/math/b.png"></picture>\n'
                '![Off site](https://example.com/c.png) ![Anchor](#top)\n')
        self.assertEqual(figure_index.embedded("docs/math/Page.md", text),
                         [("docs/images/math/a.png", "A plot"), ("docs/images/math/b-dark.png", ""),
                          ("docs/images/math/b.png", "B & C")])

    def test_a_link_one_folder_short_resolves_beside_the_page_not_into_docs_images(self):
        self.assertEqual(figure_index.embedded("docs/render/frame/Page.md", '<img src="../images/render/a.gif">'),
                         [("docs/render/images/render/a.gif", "")])


class OpenedTests(unittest.TestCase):
    def test_an_image_not_on_disk_is_read_from_git_s_index(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            Image.new("RGB", (3, 2), "red").save(root / "a.png")
            subprocess.run(["git", "add", "a.png"], cwd=root, check=True)
            (root / "a.png").unlink()
            source = figure_index.readable(root, ["a.png"])["a.png"]
            with Image.open(figure_index.opened(source)) as image:
                self.assertEqual((image.size, image.convert("RGB").getpixel((0, 0))), ((3, 2), (255, 0, 0)))


class Tree(unittest.TestCase):
    """A checkout outside git: docs/images/math/wide.png and narrow.png, a GIF, and pages that show them."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        write(self.root / figure_index.PAGE, PAGE)
        (self.root / "docs/images/math").mkdir(parents=True)
        Image.new("RGB", (400, 100), "white").save(self.root / "docs/images/math/wide.png")
        Image.new("RGB", (40, 10), "black").save(self.root / "docs/images/math/narrow.png")
        frames = [Image.new("RGB", (320, 80), colour) for colour in ("red", "green", "blue", "white")]
        save_gif(frames, self.root / "docs/images/anim/spin.gif", 100)

    def snapshot(self):
        return {path.relative_to(self.root).as_posix(): path.read_bytes()
                for path in sorted(self.root.rglob("*")) if path.is_file()}


class IndexTests(Tree):
    def setUp(self):
        super().setUp()
        write(self.root / "docs/math/One.md", "![](../images/math/wide.png)\n![Spinning](../images/anim/spin.gif)\n")
        write(self.root / "docs/math/Two.md", "![Wide | white](../images/math/wide.png)\n")

    def index(self, check=False):
        owner = {"docs/images/math/wide.png": "launcher/tools/math/plots.py"}.get
        warnings = io.StringIO()
        with contextlib.redirect_stderr(warnings):
            differences = figure_index.index(owner, check, self.root)
        return differences, warnings.getvalue()

    def page(self):
        return (self.root / figure_index.PAGE).read_text(encoding="utf-8")

    def thumbnail(self):
        return self.root / figure_index.thumbnail_of("docs/images/anim/spin.gif")

    def test_rows_carry_the_first_alt_every_page_and_the_producer(self):
        self.index()
        rows = {line.split('href="')[1].split('"')[0]: line for line in self.page().splitlines() if 'href="' in line}
        wide = rows["../images/math/wide.png"]
        self.assertIn('width="160"', wide)
        self.assertIn("Wide \\| white", wide)
        self.assertIn("[math/One](../math/One.md), [math/Two](../math/Two.md)", wide)
        self.assertIn("[plots.py](../../launcher/tools/math/plots.py)", wide)
        narrow = rows["../images/math/narrow.png"]
        self.assertIn('width="40"', narrow)
        self.assertIn(f"| {figure_index.NOTHING} | {figure_index.NOTHING} | not declared yet |", narrow)
        self.assertIn('src="figure-thumbnails/anim/spin.gif"', rows["../images/anim/spin.gif"])

    def test_the_summary_counts_each_area(self):
        self.index()
        self.assertIn("| [anim](#anim) | 1 | 0 | 0 |", self.page())
        self.assertIn("| [math](#math) | 2 | 1 | 1 |", self.page())
        self.assertLess(self.page().index("## anim"), self.page().index("## math"))

    def test_an_image_no_page_shows_is_a_difference_even_once_written(self):
        self.index()
        differences, _ = self.index(check=True)
        self.assertEqual(differences, ["shown nowhere docs/images/math/narrow.png"])

    def test_a_gif_copy_is_narrow_on_the_palette_and_as_long_as_the_gif(self):
        self.index()
        with Image.open(self.thumbnail()) as copy:
            self.assertEqual(copy.size, (figure_index.THUMBNAIL_WIDTH, 40))
            self.assertEqual(copy.n_frames, 2)
        delays = gif_delays(self.thumbnail())
        self.assertEqual(sum(delays), sum(gif_delays(self.root / "docs/images/anim/spin.gif")))
        self.assertTrue(all(delay >= figure_index.THUMBNAIL_FRAME_MS for delay in delays))

    def test_check_reports_without_writing_and_passes_once_written(self):
        differences, _ = self.index(check=True)
        self.assertIn(f"changed {figure_index.PAGE}", differences)
        self.assertTrue(any("new thumbnail" in line for line in differences))
        self.assertFalse(self.thumbnail().exists())
        self.assertNotIn("## math", self.page())
        write(self.root / "docs/math/Three.md", "![](../images/math/narrow.png)\n")
        self.index()
        self.assertEqual(self.index(check=True)[0], [])

    def test_a_changed_gif_makes_its_copy_stale(self):
        self.index()
        save_gif([Image.new("RGB", (320, 80), "red")] * 4, self.root / "docs/images/anim/spin.gif", 100)
        self.assertTrue(any(line.startswith(f"changed {figure_index.THUMBNAILS}/anim/spin.gif")
                            for line in self.index(check=True)[0]))

    def test_a_copy_whose_gif_is_gone_is_an_orphan_and_removed(self):
        self.index()
        (self.root / "docs/images/anim/spin.gif").unlink()
        differences, _ = self.index(check=True)
        self.assertIn(f"orphan {figure_index.THUMBNAILS}/anim/spin.gif: its GIF under "
                      f"{figure_index.IMAGES}/ is gone", differences)
        self.index()
        self.assertFalse(self.thumbnail().exists())


if __name__ == "__main__":
    unittest.main()
