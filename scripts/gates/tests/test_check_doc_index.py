"""check_doc_index.resolved_link turns a link on a page into the repository path it names;
heading_anchors gives each heading the id GitHub does."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import check_doc_index  # noqa: E402

PAGE = "docs/math/Page.md"


class ResolvedLinkTests(unittest.TestCase):
    def test_a_link_with_a_scheme_names_no_path(self):
        for target in ("https://example.com/a.png", "mailto:me@example.com", "HTTP://EXAMPLE.COM/a", "data:image/png;base64,AA"):
            with self.subTest(target=target):
                self.assertIsNone(check_doc_index.resolved_link(PAGE, target))

    def test_a_fragment_or_a_query_is_stripped(self):
        for target in ("a.md#top", "a.md?raw=1", "a.md?x=1#top", "a.md#x?y"):
            with self.subTest(target=target):
                self.assertEqual(check_doc_index.resolved_link(PAGE, target), "docs/math/a.md")

    def test_only_a_fragment_or_nothing_names_no_path(self):
        for target in ("", "#top", "?x=1"):
            with self.subTest(target=target):
                self.assertIsNone(check_doc_index.resolved_link(PAGE, target))

    def test_a_link_climbing_above_the_repository_names_no_path(self):
        for target in ("../../../x.png", "../../..", "../../../../x.png"):
            with self.subTest(target=target):
                self.assertIsNone(check_doc_index.resolved_link(PAGE, target))

    def test_a_link_climbing_exactly_to_the_top_names_a_path_there(self):
        self.assertEqual(check_doc_index.resolved_link(PAGE, "../../x.png"), "x.png")

    def test_a_folder_link_is_unchanged_and_gets_no_readme(self):
        self.assertEqual(check_doc_index.resolved_link(PAGE, "../images/"), "docs/images")
        self.assertEqual(check_doc_index.resolved_link(PAGE, "./sub/../other"), "docs/math/other")

    def test_a_percent_escape_is_decoded(self):
        self.assertEqual(check_doc_index.resolved_link(PAGE, "my%20pic.png"), "docs/math/my pic.png")

    def test_a_link_from_a_page_at_the_top_resolves_there(self):
        self.assertEqual(check_doc_index.resolved_link("README.md", "docs/a.md#x"), "docs/a.md")



class HeadingAnchorTests(unittest.TestCase):
    def test_a_repeat_is_numbered_by_occurrence(self):
        anchors = check_doc_index.heading_anchors(["# A", "## A", "### A-1", "# [B](b.md) `c`"])
        self.assertEqual([a for _, _, a in anchors], ["a", "a-1", "a-1-1", "b-c"])

    def test_headings_inside_either_fence_are_not_headings(self):
        lines = ["# Out", "~~~", "# In tilde", "```", "# Still in", "~~~", "```", "# In backticks", "```", "# After"]
        self.assertEqual([(i, t) for i, t, _ in check_doc_index.heading_anchors(lines)],
                         [(0, "Out"), (9, "After")])

    def test_a_closing_run_needs_a_space_before_it(self):
        texts = [t for _, t, _ in check_doc_index.heading_anchors(["# C#", "# Title ##", "# Tail #  "])]
        self.assertEqual(texts, ["C#", "Title", "Tail"])


if __name__ == "__main__":
    unittest.main()
