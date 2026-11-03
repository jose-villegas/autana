"""The figure index reads every spelling of an embedded image, lists each image once with the first alt a page
gives it, and keeps a GIF's small copy in step with the GIF in pixels, frame count and frame delays."""
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
HOME = "docs/math/Page.md"


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def made(*names):
    return [(f"docs/images/math/{name}", alt) for name, alt in names]


class EmbeddedSpellingTests(unittest.TestCase):
    def embedded(self, text):
        return figure_index.embedded(HOME, text)

    def test_markdown_titles_in_double_quotes_single_quotes_and_parentheses_are_not_part_of_the_path(self):
        text = ('![a](../images/math/a.png "A title")\n![b](../images/math/b.png \'B title\')\n'
                '![c](../images/math/c.png (C title))\n')
        self.assertEqual(self.embedded(text), made(("a.png", "a"), ("b.png", "b"), ("c.png", "c")))

    def test_an_angle_bracket_path_may_hold_a_space(self):
        self.assertEqual(self.embedded("![a](<../images/math/my pic.png>)"), made(("my pic.png", "a")))

    def test_a_percent_encoded_space_decodes(self):
        self.assertEqual(self.embedded("![a](../images/math/my%20pic.png)"), made(("my pic.png", "a")))

    def test_reference_style_images_find_their_definitions(self):
        text = ("![one][X] ![two][] ![Three]\n\n[x]: ../images/math/x.png\n"
                "[two]: <../images/math/t w o.png>\n[three]: ../images/math/three.png \"t\"\n")
        self.assertEqual(self.embedded(text),
                         made(("x.png", "one"), ("t w o.png", "two"), ("three.png", "Three")))

    def test_a_reference_with_no_definition_names_nothing(self):
        self.assertEqual(self.embedded("![one][nowhere]\n"), [])

    def test_an_html_entity_in_src_is_decoded(self):
        self.assertEqual(self.embedded('<img src="&#46;&#46;/images/math/a.png" alt="x">'), made(("a.png", "x")))

    def test_every_candidate_of_a_srcset_is_a_picture(self):
        text = '<img srcset="../images/math/a.png 1x, ../images/math/b.png 2x,\n ../images/math/c.png 3x">'
        self.assertEqual(self.embedded(text), made(("a.png", ""), ("b.png", ""), ("c.png", "")))

    def test_uppercase_html_names_are_read(self):
        self.assertEqual(self.embedded('<IMG SRC="../images/math/a.png" ALT="Up">'), made(("a.png", "Up")))

    def test_an_image_inside_a_code_fence_is_ignored(self):
        text = "```\n![a](../images/math/a.png)\n<img src=\"../images/math/b.png\">\n```\n![c](../images/math/c.png)\n"
        self.assertEqual(self.embedded(text), made(("c.png", "c")))

    def test_a_path_climbing_above_the_repository_is_nothing(self):
        self.assertEqual(self.embedded("![a](../../../x.png) ![b](../../../../x.png)"), [])

    def test_images_keep_the_order_of_the_page_across_markdown_and_html(self):
        text = ('<img src="../images/math/h.png"> ![m](../images/math/m.png) <img src="../images/math/z.png">\n'
                '![n](../images/math/a.png)\n')
        self.assertEqual([image for image, _ in self.embedded(text)],
                         [f"docs/images/math/{name}" for name in ("h.png", "m.png", "z.png", "a.png")])


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


class ShowingsTests(Tree):
    def test_an_image_shown_twice_on_one_page_is_listed_once_with_the_first_alt(self):
        write(self.root / "docs/math/One.md", "![First](../images/math/wide.png)\n![Second](../images/math/wide.png)\n")
        self.assertEqual(figure_index.showings(self.root)["docs/images/math/wide.png"],
                         [("docs/math/One.md", "First")])

    def test_the_index_page_does_not_count_as_showing_anything(self):
        write(self.root / figure_index.PAGE, PAGE + "![Self](../images/math/wide.png)\n")
        self.assertEqual(figure_index.showings(self.root), {})

    def test_a_page_outside_docs_counts(self):
        write(self.root / "launcher/tools/README.md", "![Tool](../../docs/images/math/wide.png)\n")
        self.assertEqual(figure_index.showings(self.root),
                         {"docs/images/math/wide.png": [("launcher/tools/README.md", "Tool")]})

    def test_an_image_outside_docs_images_is_not_a_showing(self):
        write(self.root / "docs/math/One.md", "![a](../other/a.png) ![b](../../assets/b.png)\n![c](../images/math/wide.png)\n")
        self.assertEqual(list(figure_index.showings(self.root)), ["docs/images/math/wide.png"])


class BodyTests(Tree):
    def body(self, shown, owner=lambda image: None, images=("docs/images/math/narrow.png",)):
        sources = {image: self.root / image for image in images}
        return figure_index.body(list(images), sources, shown, owner)

    def row(self, shown):
        return next(line for line in self.body(shown).splitlines() if 'href="' in line)

    def test_an_image_whose_pages_all_give_an_empty_alt_says_nothing(self):
        row = self.row({"docs/images/math/narrow.png": [("docs/math/One.md", ""), ("docs/math/Two.md", "")]})
        self.assertIn(f"| {figure_index.NOTHING} | [math/One](../math/One.md), [math/Two](../math/Two.md) |", row)

    def test_the_first_non_empty_alt_is_the_one_shown(self):
        row = self.row({"docs/images/math/narrow.png": [("docs/math/One.md", ""), ("docs/math/Two.md", "Second"),
                                                       ("docs/math/Three.md", "Third")]})
        self.assertIn("| Second |", row)
        self.assertNotIn("Third |", row.split("[math/One]")[0])

    def test_a_newline_in_alt_is_a_space_and_a_bar_is_escaped(self):
        row = self.row({"docs/images/math/narrow.png": [("docs/math/One.md", "two\nlines | bar")]})
        self.assertIn("| two lines \\| bar |", row)
        self.assertEqual(row.count("\n"), 0)

    def test_an_image_no_page_shows_has_a_dash_for_pages_and_counts_as_shown_nowhere(self):
        body = self.body({})
        self.assertIn("| [math](#math) | 1 | 0 | 1 |", body)
        self.assertIn(f"| {figure_index.NOTHING} | {figure_index.NOTHING} | not declared yet |", body)


class IndexSourcesTests(Tree):
    def index(self, check=False, root=None):
        owner = {"docs/images/math/wide.png": "launcher/tools/math/plots.py"}.get
        with contextlib.redirect_stderr(io.StringIO()) as warnings:
            differences = figure_index.index(owner, check, root or self.root)
        return differences, warnings.getvalue()

    def thumbnail(self):
        return self.root / figure_index.thumbnail_of("docs/images/anim/spin.gif")

    def keep_pixels_with_delays(self, delays):
        with Image.open(self.thumbnail()) as gif:
            frames = []
            for number in range(gif.n_frames):
                gif.seek(number)
                frames.append(gif.convert("RGB"))
        save_gif(frames, self.thumbnail(), delays=delays)

    def test_the_svgs_and_jpegs_under_docs_images_are_not_listed(self):
        Image.new("RGB", (4, 4)).save(self.root / "docs/images/math/photo.jpg")
        write(self.root / "docs/images/math/v.svg", "<svg/>")
        write(self.root / "docs/math/One.md", "![j](../images/math/photo.jpg) ![v](../images/math/v.svg)\n")
        differences, warnings = self.index()
        page = (self.root / figure_index.PAGE).read_text(encoding="utf-8")
        self.assertNotIn("photo.jpg", page + warnings)
        self.assertNotIn("v.svg", page + warnings)
        self.assertIn("| [math](#math) | 2 | 1 | 2 |", page)

    def test_a_copy_with_the_same_pixels_and_other_delays_is_stale_and_rewritten(self):
        self.index()
        written = gif_delays(self.thumbnail())
        self.keep_pixels_with_delays([100, sum(written) - 100])
        differences, _ = self.index(check=True)
        self.assertIn(f"changed {figure_index.THUMBNAILS}/anim/spin.gif: frame delays", differences)
        self.assertEqual(gif_delays(self.thumbnail()), [100, sum(written) - 100])
        self.index()
        self.assertEqual(gif_delays(self.thumbnail()), written)
        self.assertEqual(self.index(check=True)[0], [])

    def test_a_copy_with_another_frame_count_is_reported(self):
        self.index()
        save_gif([Image.new("RGB", (160, 40), "red")], self.thumbnail(), 100)
        differences, _ = self.index(check=True)
        self.assertEqual(differences, [f"changed {figure_index.THUMBNAILS}/anim/spin.gif: 2 frames vs 1 frames"])

    def test_an_unchanged_copy_yields_nothing(self):
        self.index()
        self.assertEqual(self.index()[0], [])
        self.assertEqual(self.index(check=True)[0], [])

    def test_check_writes_and_deletes_nothing_even_a_nested_orphan(self):
        self.index()
        write(self.root / figure_index.THUMBNAILS / "deep/er/gone.gif", "stale")
        (self.root / figure_index.PAGE).write_text("# Figures\n\n" + PAGE.split("\n\n", 1)[1] + "edited\n",
                                                   encoding="utf-8", newline="\n")
        before = self.snapshot()
        differences, _ = self.index(check=True)
        self.assertIn(f"orphan {figure_index.THUMBNAILS}/deep/er/gone.gif: its GIF under "
                      f"{figure_index.IMAGES}/ is gone", differences)
        self.assertEqual(self.snapshot(), before)
        self.index()
        self.assertFalse((self.root / figure_index.THUMBNAILS / "deep/er/gone.gif").exists())


class BlobFallbackTests(Tree):
    def test_check_reads_images_and_copies_git_holds_when_the_disk_has_none(self):
        write(self.root / "docs/math/One.md", "![](../images/math/wide.png)\n![](../images/math/narrow.png)\n"
                                              "![Spin](../images/anim/spin.gif)\n")
        owner = {}.get
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertTrue(figure_index.index(owner, False, self.root))
        page = (self.root / figure_index.PAGE).read_text(encoding="utf-8")
        self.assertIn('width="160"', page)
        self.assertIn('width="40"', page)
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        subprocess.run(["git", "-c", "core.autocrlf=false", "add", "-A"], cwd=self.root, check=True)
        for folder in (figure_index.IMAGES, figure_index.THUMBNAILS):
            for path in (self.root / folder).rglob("*"):
                if path.is_file():
                    path.unlink()
        with contextlib.redirect_stderr(io.StringIO()) as warnings:
            self.assertEqual(figure_index.index(owner, True, self.root), [])
        self.assertEqual(warnings.getvalue(), "")
        self.assertEqual((self.root / figure_index.PAGE).read_text(encoding="utf-8"), page)


if __name__ == "__main__":
    unittest.main()
