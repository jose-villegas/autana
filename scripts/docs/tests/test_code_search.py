"""Code search indexes public names with their declarations, ranks names before purposes, and
fails a file that states no purpose."""
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "scripts/docs"))
sys.path.insert(0, str(REPO / "scripts/gates/tests"))
from code_search import SHOWN, evaluate, index, main, report, search
from c_comments import excluded
from check_generated_files import generated_files
from gate_tree import commit, write
import query_log


class CodeSearch(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name) / "tree"
        self.log = pathlib.Path(self.temp.name) / "usage"
        patch = mock.patch.object(query_log, "log_dir", return_value=self.log)
        patch.start()
        self.addCleanup(patch.stop)

    def include(self, owner, directories=("one", "two")):
        for directory in directories:
            write(self.root, f"{directory}/use.c", f'#include "{owner}"\n')

    def owners(self):
        return {owner.path: owner for owner in index(self.root)}

    def names(self, path):
        return {name for name, _, _ in self.owners()[path].names}

    def run_main(self, *argv):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            code = main(list(argv), self.root)
        return code, out.getvalue()

    def test_owner_purpose_and_public_names_without_helper_comments(self):
        owner = "engine/ui/value.hpp"
        write(self.root, owner, "/* Values for screens. More detail. */\n#pragma once\n"
              "int value(void);\n#define VALUE 1\ntypedef int value_t;\n")
        found = self.owners()[owner]
        self.assertEqual("Values for screens.", found.purpose)
        self.assertEqual({"VALUE", "value", "value_t"}, self.names(owner))

    def test_c_types_and_enum_values_are_public_names(self):
        owner = "engine/value.h"
        write(self.root, owner, "/* Values. */\n"
              "typedef struct { int field; } value_t;\n"
              "typedef void (*value_fn)(void);\nenum mode { MODE_A, MODE_B };\n")
        self.assertEqual({"value_t", "value_fn", "mode", "MODE_A", "MODE_B"}, self.names(owner))

    def test_function_local_enums_are_not_public_names(self):
        owner = "engine/value.h"
        write(self.root, owner, "/* Values. */\n"
              "enum { VALUE_LIMIT = 4 };\n"
              "static inline int value(void) { enum { LOCAL_SHIFT = 2 }; return LOCAL_SHIFT; }\n")
        self.assertEqual({"VALUE_LIMIT", "value"}, self.names(owner))

    def test_cpp_classes_and_pasted_template_names(self):
        owner = "engine/value.hpp"
        write(self.root, owner, "/* Values. */\nclass Value {};\n"
              "#define MAKE(P) \\\n  static inline int P##_get(void) { return 1; }\n")
        self.assertEqual({"Value", "MAKE", "P##_get"}, self.names(owner))

    def test_pasted_registration_callback_is_not_a_public_name(self):
        owner = "engine/suite.h"
        write(self.root, owner, "/* Suite registration. */\n"
              "void suite_register(void);\n"
              "#define SUITE_REGISTER(fn) \\\n"
              "  __attribute__((constructor)) static void fn##_register(void) {}\n")
        self.assertEqual({"SUITE_REGISTER", "suite_register"}, self.names(owner))

    def test_include_guard_is_not_a_public_name(self):
        for filename, guard in (("value.h", "VALUE_H"), ("value.hpp", "VALUE_HPP"),
                                ("shared-value.h", "SHARED_VALUE_H"),
                                ("editor/runtime.h", "EDITOR_RUNTIME_H")):
            with self.subTest(filename=filename):
                owner = "engine/" + filename
                write(self.root, owner, "/* Values. */\n"
                      f"#ifndef {guard}\n#define {guard}\n"
                      "#define VALUE_LIMIT 1\nint value(void);\n#endif\n")
                self.assertEqual({"VALUE_LIMIT", "value"}, self.names(owner))

    def test_each_name_keeps_its_line_and_one_line_declaration(self):
        owner = "engine/value.h"
        write(self.root, owner, "/* Values. */\n#pragma once\n#define VALUE_LIMIT 4\n"
              "typedef struct {\n    int field;\n} value_t;\n\n"
              "static inline int\nvalue_get(const value_t* v) {\n    return v->field;\n}\n"
              "enum mode {\n    MODE_A = 1,\n};\n")
        found = {name: (line, text) for name, line, text in self.owners()[owner].names}
        self.assertEqual((3, "#define VALUE_LIMIT 4"), found["VALUE_LIMIT"])
        self.assertEqual((6, "typedef struct { ... } value_t"), found["value_t"])
        self.assertEqual((9, "static inline int value_get(const value_t* v)"), found["value_get"])
        self.assertEqual((13, "MODE_A = 1,"), found["MODE_A"])

    def test_python_names_signatures_and_first_assignment(self):
        write(self.root, "tools/common/value.py", '"""Shared values. Details."""\n'
              "LIMIT = 1\nLIMIT = 2\nclass Value(dict): pass\n"
              "def public(a, b=2): pass\ndef _private(): pass\n")
        found = {name: (line, text) for name, line, text in self.owners()["tools/common/value.py"].names}
        self.assertEqual({"LIMIT": (2, "LIMIT = 1"), "Value": (4, "class Value(dict)"),
                          "public": (5, "def public(a, b=2)")}, found)

    def test_apps_and_tests_are_indexed_vendored_and_generated_are_not(self):
        for owner in ("apps/demo/value.h", "engine/tests/value.h", "tools/tests/test_value.py"):
            write(self.root, owner, "/* Values. */\nint value(void);\n" if owner.endswith(".h")
                  else '"""Values."""\n')
        for owner in ("launcher/components/bsp/value.h", "launcher/packages/testkit/unity/value.h"):
            write(self.root, owner, "/* Values. */\nint value(void);\n")
        write(self.root, "engine/generated.h", "/* GENERATED FILE - do not edit. */\n")
        commit(self.root, ".")
        with mock.patch("code_search.excluded", return_value=(*excluded(), *generated_files(self.root))):
            self.assertEqual({"apps/demo/value.h", "engine/tests/value.h", "tools/tests/test_value.py"},
                             set(self.owners()))

    def test_users_are_distinct_directories_not_references(self):
        write(self.root, "engine/value.h", "/* Values. */\nint value(void);\n")
        self.include("engine/value.h", ("one",))
        write(self.root, "one/other.cpp", '#include "engine/value.h"\n')
        self.assertEqual({"one"}, self.owners()["engine/value.h"].users)

    def test_python_import_forms_relative_imports_and_local_includes(self):
        for path, text in {
            "tools/common/value.py": '"""Shared values."""\ndef public(): pass\n',
            "tools/one/use.py": "from common.value import public\n",
            "tools/two/use.py": "def late():\n    import common.value as value\n",
            "pkg/value.py": '"""Values."""\ndef value(): pass\n',
            "pkg/one/use.py": "from ..value import value\n",
            "pkg/two/use.py": "from .. import value\n",
            "engine/value.h": "/* Values. */\nint value(void);\n",
            "engine/use.c": '#include "value.h"\n',
            "other/use.c": '/* #include "engine/value.h" */\n#include "engine/value.h"\n',
        }.items():
            write(self.root, path, text)
        owners = self.owners()
        self.assertEqual({"tools/one", "tools/two"}, owners["tools/common/value.py"].users)
        self.assertEqual({"pkg/one", "pkg/two"}, owners["pkg/value.py"].users)
        self.assertEqual({"engine", "other"}, owners["engine/value.h"].users)

    def test_names_rank_before_purposes_and_shared_before_single(self):
        write(self.root, "a/ring.h", "/* Fixed queues of bytes. */\nint ring_push(int);\n")
        write(self.root, "b/fifo.h", "/* A ring of bytes for one reader. */\nint fifo_put(int);\n")
        write(self.root, "c/ring_pool.h", "/* Pools. */\nint ring_pool_take(void);\n")
        self.include("c/ring_pool.h")
        rows = search(index(self.root), ["ring"])
        self.assertEqual(["ring_pool_take", "ring_push", "fifo.h"], [row[2] for row in rows])
        self.assertEqual(["ring_pool_take"], [row[2] for row in search(index(self.root), ["ring"], shared=True)])

    def test_names_matching_more_words_rank_higher_and_rare_words_count_more(self):
        write(self.root, "a/text.h", "/* Text built by appending pieces. */\n"
              "int text_appendf(void);\nint text_vappendf(void);\nint text_clear(void);\n")
        write(self.root, "b/lens.h", "/* A pinhole camera. */\nint lens_fit(void);\n")
        for n in range(3):
            write(self.root, f"c/limit{n}.h", "/* Limits. */\n" f"#define LIMIT{n}_LEN {n}\n")
        rows = search(index(self.root), ["text", "append"])
        self.assertEqual(["text_appendf", "text_vappendf", "text_clear"], [row[2] for row in rows])
        self.assertEqual(["lens_fit"], [row[2] for row in search(index(self.root), ["pinhole", "lenses"])])
        self.assertEqual([], search(index(self.root), ["ring"]))

    def test_one_file_shows_three_names_before_the_next_file(self):
        write(self.root, "a/stage.h", "/* Stages. */\n" + "".join(f"int stage_{n}(void);\n" for n in range(5)))
        write(self.root, "b/stage_name.h", "/* Stage names. */\nint stage_name(void);\n")
        paths = [row[0] for row in search(index(self.root), ["stage"])]
        self.assertEqual(["a/stage.h"] * 3 + ["b/stage_name.h"] + ["a/stage.h"] * 2, paths)

    def test_report_prints_three_lines_a_hit_and_counts_the_rest(self):
        rows = [(f"a/{n}.h", n, f"n{n}", f"int n{n}(void)", "Numbers.") for n in range(SHOWN + 2)]
        lines = report(rows).splitlines()
        self.assertEqual(["a/0.h:0  n0", "    int n0(void)", "    Numbers."], lines[:3])
        self.assertEqual("2 more; add a word to narrow.", lines[-1])
        self.assertEqual(SHOWN * 3 + 1, len(lines))

    def test_a_query_split_between_name_and_purpose_counts_both(self):
        # Each word scores where it matched: twice its rarity on the name, once in the file's prose.
        write(self.root, "a/text.h", "/* Pieces of text. */\nint text_put(void);\n")
        write(self.root, "b/text_other.h", "/* Other things. */\nint text_clear(void);\n")
        write(self.root, "c/pieces.h", "/* Pieces of glass. */\nint glass_count(void);\n")
        rows = search(index(self.root), ["text", "pieces"])
        self.assertEqual(["text_put", "text_clear", "pieces.h"], [row[2] for row in rows])

    def test_no_match_prints_the_nearest_names_and_exits_one(self):
        write(self.root, "a/value.h", "/* Values. */\nint value_get(void);\nint other(void);\n")
        self.assertEqual((0, "a/value.h:2  value_get\n    int value_get(void)\n    Values.\n"), self.run_main("value"))
        code, out = self.run_main("vlue", "gte")
        self.assertEqual(1, code)
        self.assertEqual("no match; nearest names:\na/value.h:2  value_get", out.splitlines()[0] + "\n" + out.splitlines()[1])
        self.assertNotIn("other", out)

    def test_every_search_is_logged_with_its_hits(self):
        write(self.root, "a/value.h", "/* Values. */\nint value(void);\n")
        self.run_main("value")
        self.run_main("--shared", "absent")
        rows = query_log.read(self.log)
        self.assertEqual([("code", "search", "value", [["a/value.h:2 value", None]], False),
                          ("code", "shared", "absent", [], True)],
                         [(r["tool"], r["kind"], r["query"], r["hits"], r["weak"]) for r in rows])
        self.assertTrue(all(r["chars"] > 0 for r in rows))

    def test_eval_ranks_each_row_owner_within_the_shown_results(self):
        write(self.root, "a/ring.h", "/* Fixed queues of bytes. */\nint ring_push(int);\n")
        write(self.root, "b/fifo.h", "/* A ring of bytes for one reader. */\nint fifo_put(int);\n")
        rows = pathlib.Path(self.temp.name) / "rows.tsv"
        rows.write_text("ring push\ta/ring.h\tcase\nring reader\tb/fifo.h\tcase\nbitset\ta/ring.h\tcase\n",
                        encoding="utf-8")
        self.assertEqual([("ring push", 1), ("ring reader", 1), ("bitset", None)],
                         evaluate(index(self.root), rows))

    def test_check_fails_and_names_a_file_without_a_purpose(self):
        write(self.root, "a/value.h", "/* Values. */\nint value(void);\n")
        write(self.root, "a/bare.h", "int bare(void);\n")
        write(self.root, "a/__init__.py", "")
        code, out = self.run_main("--check")
        self.assertEqual(1, code)
        self.assertIn("a/bare.h: no purpose", out)
        self.assertIn("a/__init__.py: no purpose", out)
        self.assertNotIn("a/value.h", out)
        write(self.root, "a/bare.h", "/* Bare values. */\nint bare(void);\n")
        write(self.root, "a/__init__.py", '"""The a package."""\n')
        self.assertEqual(0, self.run_main("--check")[0])


if __name__ == "__main__":
    unittest.main()
