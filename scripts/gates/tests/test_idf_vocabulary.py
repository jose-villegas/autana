"""Regression tests for scripts/gates/idf_vocabulary.py."""
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import idf_vocabulary  # noqa: E402
from fake_idf import fake_idf, fake_toolchain, write  # noqa: E402


def git(root, *args):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], cwd=root,
                   check=True, capture_output=True)


class OutsideVocabularyTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.temp.name)
        self.idf = fake_idf(base / "esp-idf")
        self.tools = fake_toolchain(base / "espressif")
        self.cache = base / "cache"

    def tearDown(self):
        self.temp.cleanup()

    def outside(self):
        return idf_vocabulary.outside_vocabulary(self.idf, self.tools, self.cache)

    def test_esp_idf_functions_constants_options_and_files_are_vocabulary(self):
        outside = self.outside()
        self.assertIn("fake_ll_cal_clock", outside.functions)
        self.assertIn("FAKE_FREQ_DEFAULT", outside.constants)
        self.assertIn("CONFIG_FAKE_LFN_NONE", outside.constants)
        self.assertIn("FAKE_LFN_NONE", outside.constants)
        self.assertIn("CONFIG_FAKE_LFN", outside.constants)
        self.assertIn("FAKE_WHOLE_ARCHIVE", outside.constants)
        self.assertTrue(outside.has_path("fake_ll.h"))
        self.assertTrue(outside.has_path("hal/fake_ll.h"))
        self.assertTrue(outside.has_path("idf.py"))
        self.assertFalse(outside.has_path("ll.h"))
        self.assertFalse(outside.has_path("other/fake_ll.h"))

    def test_a_name_only_a_comment_spells_is_not_vocabulary(self):
        self.assertNotIn("ghost_ll_function", self.outside().functions)

    def test_the_toolchains_c_library_headers_are_vocabulary(self):
        outside = self.outside()
        self.assertIn("fake_hypot", outside.functions)
        self.assertTrue(outside.has_path("math.h"))

    def test_no_components_directory_is_no_esp_idf(self):
        empty = pathlib.Path(self.temp.name) / "empty"
        empty.mkdir()
        self.assertIsNone(idf_vocabulary.outside_vocabulary(empty, self.tools, self.cache))

    def test_a_second_run_reads_the_cache_instead_of_scanning(self):
        self.outside()
        with mock.patch.object(idf_vocabulary, "scan", side_effect=AssertionError("rescanned")):
            self.assertIn("fake_ll_cal_clock", self.outside().functions)

    def test_a_new_esp_idf_commit_is_scanned_afresh(self):
        git(self.idf, "init", "-q")
        git(self.idf, "add", ".")
        git(self.idf, "commit", "-q", "-m", "one")
        self.assertNotIn("fake_added", self.outside().functions)
        write(self.idf, "components/hal/include/added.h", "void fake_added(void);\n")
        git(self.idf, "add", ".")
        git(self.idf, "commit", "-q", "-m", "two")
        self.assertIn("fake_added", self.outside().functions)

    def test_a_new_esp_idf_version_is_scanned_afresh(self):
        self.assertNotIn("fake_added", self.outside().functions)
        write(self.idf, "components/hal/include/added.h", "void fake_added(void);\n")
        write(self.idf, "tools/cmake/version.cmake", "set(IDF_VERSION_MAJOR 6)\n")
        self.assertIn("fake_added", self.outside().functions)


if __name__ == "__main__":
    unittest.main()
