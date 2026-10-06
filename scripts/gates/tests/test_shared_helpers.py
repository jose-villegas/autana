"""The catalogue follows source declarations and rejects absent or stale prose."""
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "launcher/tools/gen"))
from shared_helpers import catalogue, update
from gate_tree import write
from check_style_audit import rule_stray_html_comment


class SharedHelpers(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        write(self.root, "docs/Shared-Helpers.md",
              "Before\n<!-- generated: shared-helpers -->\n<!-- /generated: shared-helpers -->\nAfter\n")

    def test_new_helper_appears(self):
        write(self.root, "launcher/main/util/nested/value.h",
              "/* Module banner is not a helper comment. */\n#pragma once\n"
              "/* Clamp to the closed interval. More detail. */\nint clamp(int v, int lo, int hi);\n")
        rows, errors = catalogue(self.root)
        self.assertEqual([], errors)
        self.assertIn(("clamp", "Clamp to the closed interval.",
                       "launcher/main/util/nested/value.h"), rows)

    def test_missing_comment_fails_check(self):
        write(self.root, "scripts/lib/value.py", "def clamp(v):\n    return v\n")
        self.assertEqual(1, update(self.root, check=True))

    def test_underscore_helper_is_skipped(self):
        write(self.root, "scripts/lib/value.py",
              "def _private():\n    pass\n"
              "def public():\n    \"\"\"A public helper.\"\"\"\n    pass\n")
        rows, errors = catalogue(self.root)
        self.assertEqual([], errors)
        self.assertEqual([("public", "A public helper.", "scripts/lib/value.py")], rows)

    def test_stale_table_fails(self):
        write(self.root, "launcher/main/util/value.h", "/* A value. */\n#pragma once\n")
        self.assertEqual(0, update(self.root))
        before = (self.root / "docs/Shared-Helpers.md").read_bytes()
        write(self.root, "launcher/main/util/value.h",
              "/* A value. */\n#pragma once\n/* Return a value. */\nint value(void);\n")
        self.assertEqual(1, update(self.root, check=True))
        self.assertEqual(before, (self.root / "docs/Shared-Helpers.md").read_bytes())

    def test_c_declaration_kinds_and_template_spellings(self):
        write(self.root, "launcher/main/util/value.h",
              "/* Module. */\n#pragma once\n"
              "/* Public prototype. */\nint\nvalue(void);\n"
              "/* Inline arithmetic. */\nstatic inline int\nadd(int a) { return a; }\n"
              "/* Constant expression. */\n#define DOUBLE(v) ((v) * 2)\n"
              "/* Template expansion. */\n#define MAKE(P) \\\n"
              "  /* Template arithmetic. */ \\\n"
              "  static inline P##_t P##_add(P##_t a) { return a; }\n"
              "static int private(void);\n#define VALUE 1\nint (*callback)(void);\n"
              "static inline int _hidden(void) { return 0; }\n")
        rows, errors = catalogue(self.root)
        self.assertEqual([], errors)
        self.assertEqual({"value", "add", "DOUBLE", "MAKE", "P##_add"}, {r[0] for r in rows})

    def test_module_banner_does_not_document_function(self):
        write(self.root, "launcher/main/util/value.h",
              "/* Module banner. */\n#pragma once\nint value(void);\n")
        self.assertIn("value: missing", "\n".join(catalogue(self.root)[1]))

    def test_gfx_rule_rejects_hardware_external_functions_and_state(self):
        for name, prefix in (("hardware", '#include "bsp/display.h"\n'),
                             ("external", "void present(void);\n"),
                             ("state", "static int count;\n"),
                             ("indented", "  static int count;\n"),
                             ("macroinclude", "#include HEADER\n"),
                             ("local", '#include "board.h"\n')):
            write(self.root, f"launcher/main/gfx/{name}.h",
                  "/* Module. */\n#pragma once\n" + prefix +
                  f"/* Arithmetic. */\nstatic inline int {name}(int v) {{ return v; }}\n")
        write(self.root, "launcher/main/gfx/color.h",
              "/* Module. */\n#pragma once\n#include <stdint.h>\n"
              "/* Arithmetic. */\nstatic inline int color(int v) { return v; }\n")
        write(self.root, "launcher/main/gfx/box.h",
              "/* Module. */\n#pragma once\n#include \"gfx/color.h\"\n"
              "/* Bounds. */\nstatic inline int box(int v) { return v; }\n")
        rows, errors = catalogue(self.root)
        self.assertEqual([], errors)
        self.assertEqual({"color", "box"}, {r[0] for r in rows})

    def test_python_module_functions_only(self):
        write(self.root, "launcher/tools/device/value.py",
              "async def value():\n    \"\"\"Device value. Details.\"\"\"\n"
              "    def nested():\n        pass\n"
              "class Class:\n    def method(self):\n        pass\n")
        write(self.root, "scripts/lib/tests/value.py", "def fixture():\n    pass\n")
        rows, errors = catalogue(self.root)
        self.assertEqual([], errors)
        self.assertEqual([("value", "Device value.", "launcher/tools/device/value.py")], rows)

    def test_writer_preserves_crlf_and_reports_missing_c_comment(self):
        page = self.root / "docs/Shared-Helpers.md"
        page.write_bytes(page.read_bytes().replace(b"\n", b"\r\n"))
        write(self.root, "launcher/main/util/value.h",
              "/* Module. */\n#pragma once\nint value(void);\n")
        self.assertEqual(1, update(self.root))
        self.assertIn(b"Missing own comment", page.read_bytes())
        self.assertNotIn(b"\n", page.read_bytes().replace(b"\r\n", b""))

    def test_generated_markers_are_not_stray_comments(self):
        lines = ["<!-- generated: shared-helpers sha256=" + "0" * 64 + " -->",
                 "<!-- /generated: shared-helpers -->"]
        self.assertEqual([], list(rule_stray_html_comment(self.root, "doc.md", lines)))
        malformed = ["<!-- generated: Bad_Name -->"]
        self.assertEqual(1, len(list(rule_stray_html_comment(self.root, "doc.md", malformed))))


if __name__ == "__main__":
    unittest.main()
