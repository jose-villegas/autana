"""check_doc_index.resolved_link turns a link on a page into the repository path it names."""
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


if __name__ == "__main__":
    unittest.main()
