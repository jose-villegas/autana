"""The catalogue selects shared owner files and keeps source-owned descriptions."""
import pathlib
import shlex
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "launcher/tools/gen"))
from shared_helpers import CHECK, catalogue, update
from c_comments import EXCLUDED
from check_doc_generated import run_check
from check_generated_files import generated_files
from gate_tree import commit, write
from generated_blocks import check_commands


class SharedHelpers(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        write(self.root, "docs/Shared-Helpers.md",
              "Before\n<!-- generated: shared-helpers -->\n<!-- /generated: shared-helpers -->\nAfter\n")

    def include(self, owner, directories=("one", "two")):
        for directory in directories:
            write(self.root, f"{directory}/use.c", f'#include "{owner}"\n')

    def test_owner_banner_and_public_names_without_helper_comments(self):
        owner = "engine/ui/value.hpp"
        write(self.root, owner, "/* Values for screens. More detail. */\n#pragma once\n"
              "int value(void);\n#define VALUE 1\ntypedef int value_t;\n")
        self.include(owner)
        self.assertEqual([(owner, "Values for screens.", "VALUE, value, value_t")], catalogue(self.root))

    def test_c_types_and_enum_values_are_public_names(self):
        owner = "engine/value.h"
        write(self.root, owner, "/* Values. */\n"
              "typedef struct { int field; } value_t;\n"
              "typedef void (*value_fn)(void);\nenum mode { MODE_A, MODE_B };\n")
        self.include(owner)
        names = catalogue(self.root)[0][2].split(", ")
        self.assertEqual({"value_t", "value_fn", "mode", "MODE_A", "MODE_B"}, set(names))

    def test_cpp_classes_and_pasted_template_names(self):
        owner = "engine/value.hpp"
        write(self.root, owner, "/* Values. */\nclass Value {};\n"
              "#define MAKE(P) \\\n  static inline int P##_get(void) { return 1; }\n")
        self.include(owner)
        self.assertEqual({"Value", "MAKE", "P##_get"}, set(catalogue(self.root)[0][2].split(", ")))

    def test_distinct_directories_not_reference_count(self):
        write(self.root, "engine/value.h", "/* Values. */\nint value(void);\n")
        self.include("engine/value.h", ("one",))
        write(self.root, "one/other.cpp", '#include "engine/value.h"\n')
        self.assertEqual([], catalogue(self.root))

    def test_excluded_owners_and_generated_files(self):
        for owner in ("apps/demo/value.h", "engine/test/value.h", "engine/tests/value.h"):
            write(self.root, owner, "/* Values. */\nint value(void);\n")
            self.include(owner)
            self.assertEqual([], catalogue(self.root))
        write(self.root, "engine/generated.h", "/* GENERATED FILE - do not edit. */\n")
        self.include("engine/generated.h")
        commit(self.root, ".")
        with mock.patch("shared_helpers.EXCLUDED", (*EXCLUDED, *generated_files(self.root))):
            self.assertEqual([], catalogue(self.root))

    def test_vendored_owners_are_excluded(self):
        for owner in ("launcher/components/bsp/value.h",
                      "launcher/components/microui/microui.h",
                      "launcher/test/framework/value.h"):
            with self.subTest(owner=owner):
                write(self.root, owner, "/* Values. */\nint value(void);\n")
                self.include(owner)
                self.assertEqual([], catalogue(self.root))

    def test_include_guard_is_not_a_public_name(self):
        for filename, guard in (("value.h", "VALUE_H"), ("value.hpp", "VALUE_HPP"),
                                ("shared-value.h", "SHARED_VALUE_H"),
                                ("editor/runtime.h", "EDITOR_RUNTIME_H")):
            with self.subTest(filename=filename):
                owner = "engine/" + filename
                write(self.root, owner, "/* Values. */\n"
                      f"#ifndef {guard}\n#define {guard}\n"
                      "#define VALUE_LIMIT 1\nint value(void);\n#endif\n")
                self.include(owner)
                rows = {row[0]: row for row in catalogue(self.root)}
                self.assertEqual("VALUE_LIMIT, value", rows[owner][2])

    def test_python_import_forms_and_public_names(self):
        write(self.root, "tools/common/value.py", '"""Shared values. Details."""\n'
              "LIMIT = 1\nclass Value: pass\ndef public(): pass\ndef _private(): pass\n")
        write(self.root, "tools/one/use.py", "from common.value import public\n")
        write(self.root, "tools/two/use.py", "import common.value as value\n")
        self.assertEqual([("tools/common/value.py", "Shared values.", "LIMIT, Value, public")],
                         catalogue(self.root))

    def test_relative_import_and_local_include(self):
        write(self.root, "pkg/value.py", '"""Values."""\ndef value(): pass\n')
        write(self.root, "pkg/one/use.py", "from ..value import value\n")
        write(self.root, "pkg/two/use.py", "from .. import value\n")
        write(self.root, "engine/value.h", "/* Values. */\nint value(void);\n")
        write(self.root, "engine/use.c", '#include "value.h"\n')
        write(self.root, "other/use.c", '#include "engine/value.h"\n')
        self.assertEqual({"pkg/value.py", "engine/value.h"}, {r[0] for r in catalogue(self.root)})

    def test_stale_check_preserves_crlf(self):
        page = self.root / "docs/Shared-Helpers.md"
        page.write_bytes(page.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        owner = "engine/value.h"
        write(self.root, owner, "/* Values. */\nint value(void);\n")
        self.include(owner)
        self.assertEqual(0, update(self.root))
        before = page.read_bytes()
        write(self.root, owner, "/* Changed values. */\nint value(void);\n")
        self.assertEqual(1, update(self.root, check=True))
        self.assertEqual(before, page.read_bytes())
        self.assertNotIn(b"\n", before.replace(b"\r\n", b""))

    def test_the_named_check_command_fails_a_stale_catalogue_and_writes_nothing(self):
        # The command the marker names, run as the generated-document gate runs it.
        owner = "engine/value.h"
        write(self.root, owner, "/* Values. */\nint value(void);\n")
        self.include(owner)
        update(self.root)
        page = self.root / "docs/Shared-Helpers.md"
        self.assertEqual({"shared-helpers": CHECK}, check_commands(page.read_text(encoding="utf-8")))
        command = f"{CHECK} --root {shlex.quote(self.root.as_posix())}"
        self.assertIsNone(run_check(REPO, command))
        page.write_text(page.read_text(encoding="utf-8").replace("Values.", "Hand edit."), encoding="utf-8")
        before = page.read_bytes()
        self.assertIsNotNone(run_check(REPO, command))
        self.assertEqual(before, page.read_bytes())


if __name__ == "__main__":
    unittest.main()
