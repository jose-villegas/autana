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
from fake_idf import TOOLCHAIN_INCLUDE, fake_idf, fake_toolchain, write  # noqa: E402


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


    def test_an_uncommitted_edit_inside_esp_idf_is_scanned_afresh(self):
        git(self.idf, "init", "-q")
        git(self.idf, "add", ".")
        git(self.idf, "commit", "-q", "-m", "one")
        self.assertNotIn("fake_added", self.outside().functions)
        write(self.idf, "components/hal/esp32s3/include/hal/fake_ll.h",
              "void fake_added(void);\n")
        self.assertIn("fake_added", self.outside().functions)

    def test_a_toolchain_header_edited_in_place_is_scanned_afresh(self):
        self.assertNotIn("fake_added", self.outside().functions)
        write(self.tools, TOOLCHAIN_INCLUDE + "/math.h", "double fake_added(double);\n")
        self.assertIn("fake_added", self.outside().functions)

    def test_the_c_library_is_reported_missing_when_no_toolchain_has_headers(self):
        self.assertTrue(self.outside().has_c_library)
        empty = pathlib.Path(self.temp.name) / "no-tools"
        empty.mkdir()
        outside = idf_vocabulary.outside_vocabulary(self.idf, empty, self.cache)
        self.assertFalse(outside.has_c_library)
        self.assertIn("C library", idf_vocabulary.required_missing(outside))
        self.assertIsNone(idf_vocabulary.required_missing(self.outside()))
        self.assertIn("no ESP-IDF", idf_vocabulary.required_missing(None))


class DeclarationsTest(unittest.TestCase):
    """Only what ESP-IDF declares is vocabulary: a name it merely calls,
    quotes or mentions may be one it has since removed."""

    def names(self, header="", source=""):
        with tempfile.TemporaryDirectory() as temp:
            base = pathlib.Path(temp)
            idf = fake_idf(base / "esp-idf")
            write(idf, "components/fake/include/fake.h", header)
            write(idf, "components/fake/fake.c", source)
            return idf_vocabulary.outside_vocabulary(idf, base / "no-tools", base / "cache")

    def test_a_prototype_and_a_definition_resolve(self):
        outside = self.names(
            header="esp_err_t fake_prototype(int x);\n"
                   "static inline __attribute__((always_inline)) int fake_attr_inline(int a)\n"
                   "{\n    return a;\n}\n"
                   "const char *fake_pointer_return(void);\n",
            source="esp_err_t\nfake_gnu_style(void)\n{\n    return ESP_OK;\n}\n"
                   "void IRAM_ATTR fake_defined(int x)\n{\n    (void)x;\n}\n")
        for name in ("fake_prototype", "fake_attr_inline", "fake_pointer_return",
                     "fake_gnu_style", "fake_defined"):
            self.assertIn(name, outside.functions)

    def test_a_macro_an_enum_value_and_a_type_resolve(self):
        outside = self.names(header="#define FAKE_MACRO_FN(x) (x)\n"
                                    "#define FAKE_OBJECT_MACRO 3\n"
                                    "typedef enum {\n    FAKE_ENUM_A = 1,\n    FAKE_ENUM_B,\n} fake_enum_t;\n"
                                    "typedef void (*fake_callback_t)(int);\n"
                                    "struct fake_tag {\n    int field;\n};\n")
        self.assertIn("FAKE_MACRO_FN", outside.functions)
        for name in ("FAKE_MACRO_FN", "FAKE_OBJECT_MACRO", "FAKE_ENUM_A", "FAKE_ENUM_B"):
            self.assertIn(name, outside.constants)
        for name in ("fake_enum_t", "fake_callback_t", "fake_tag"):
            self.assertIn(name, outside.types)

    def test_a_name_only_called_quoted_or_mentioned_does_not_resolve(self):
        outside = self.names(
            header="#define FAKE_WRAP(x) fake_called_in_macro(x)\n",
            source="/* fake_comment_only() FAKE_COMMENT_CONST */\n"
                   "static const char *MSG = \"fake_string_only() FAKE_STRING_CONST\";\n"
                   "int fake_user(int x)\n{\n    fake_called_only(x);\n"
                   "    int y = fake_called_in_initialiser(x);\n"
                   "    return fake_returned(y) + FAKE_USED_CONST;\n}\n")
        for name in ("fake_called_only", "fake_called_in_initialiser", "fake_returned",
                     "fake_called_in_macro", "fake_string_only", "fake_comment_only"):
            self.assertNotIn(name, outside.functions)
        for name in ("FAKE_STRING_CONST", "FAKE_COMMENT_CONST", "FAKE_USED_CONST"):
            self.assertNotIn(name, outside.constants)
        self.assertIn("fake_user", outside.functions)


if __name__ == "__main__":
    unittest.main()
