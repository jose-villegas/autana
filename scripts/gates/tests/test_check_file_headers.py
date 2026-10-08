"""Regression tests for scripts/gates/check_file_headers.py."""
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import check_file_headers  # noqa: E402

HEADER = "/* thing: what it is. */\n"


class ProblemsTest(unittest.TestCase):
    """One tree per case: `files` maps a repo-relative path to its text."""

    def problems(self, files):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            for path, text in files.items():
                target = root / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8")
            return check_file_headers.problems(str(root))

    def test_a_module_needs_a_header_in_its_c_or_its_h(self):
        cases = (
            ({"launcher/main/gfx/gfx.h": HEADER, "launcher/main/gfx/gfx.c": "int x;\n"}, []),
            ({"launcher/main/gfx/gfx.h": "#pragma once\n", "launcher/main/gfx/gfx.c": HEADER + "int x;\n"}, []),
            ({"launcher/main/gfx/gfx.h": "#pragma once\nint f(void);\n", "launcher/main/gfx/gfx.c": "int x;\n"},
             ["launcher/main/gfx/gfx.h"]),
            ({"launcher/test/suites/suites_helper.c": "int x;\n"}, ["launcher/test/suites/suites_helper.c"]),
        )
        for files, expected in cases:
            with self.subTest(files=sorted(files)):
                found = self.problems(files)
                self.assertEqual([line.split(":")[0] for line in found], expected)

    def test_a_header_may_follow_pragma_once_but_not_code(self):
        cases = (
            ("#pragma once\n\n" + HEADER, []),
            ("\n  " + HEADER, []),
            ('#include "gfx/gfx.h" /* the framebuffer */\nint x;\n', ["launcher/main/ui/ui.c"]),
            ("int x; /* thing */\n", ["launcher/main/ui/ui.c"]),
        )
        for text, expected in cases:
            with self.subTest(text=text[:24]):
                found = self.problems({"launcher/main/ui/ui.c": text})
                self.assertEqual([line.split(":")[0] for line in found], expected)

    def test_suites_vendored_and_out_of_scope_files_are_not_checked(self):
        self.assertEqual(self.problems({
            "launcher/test/suites/suite_gfx.c": "int x;\n",
            "launcher/main/apps/sand/tests/suite_sand.c": "int x;\n",
            "launcher/components/microui/src/microui.c": "int x;\n",
            "launcher/main/gfx/draw/gfx_palette_standard_generated.h": "int x;\n",
            "editor/src/main.cpp": "int x;\n",
            "launcher/test/stubs/esp_log.h": "int x;\n",
        }), [])

    def test_every_folder_under_the_firmware_is_in_scope_without_being_listed(self):
        for path in ("launcher/main/new_layer/x.c", "launcher/main/apps/sand/tools/host.c",
                     "launcher/main/util/runtime/x.h", "launcher/main/main.c"):
            with self.subTest(path=path):
                self.assertEqual([line.split(":")[0] for line in self.problems({path: "int x;\n"})], [path])


if __name__ == "__main__":
    unittest.main()
