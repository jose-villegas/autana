"""Regression tests against the pinned clone engine and its new-pair gate."""
import contextlib
import importlib.util
import io
import os
import shutil
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
path = Path(os.environ.get("CLONE_GATE_SCRIPT", Path(__file__).resolve().parents[1] / "check_clones.py"))
spec = importlib.util.spec_from_file_location("check_clones", path)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)

BLOCK = """int alpha(int input) {
    int result = 0;
    for (int index = 0; index < input; index++) {
        if (index % 3 == 0) {
            result += index * 2;
        } else {
            result -= index / 4;
        }
    }
    while (result < input * input) {
        result = result * 3 + input;
        if (result > input * 10) {
            break;
        }
    }
    return result > 5 ? result : input;
}
"""


CROSS = "def cross(a, b):\n    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])\n"


READY = shutil.which("node") is not None and gate.ENGINE.is_file()
SKIP_REASON = "Install Node.js and run npm ci --prefix scripts/gates"


def repository_commands(root):
    def git(*args):
        return gate.git(root, *args).decode().strip()
    def commit():
        git("add", "-A")
        git("commit", "-m", "test: source")
    return git, commit


def init_repository(git):
    git("init", "-b", "main")
    git("config", "user.email", "test@example.invalid")
    git("config", "user.name", "Test")


@unittest.skipUnless(READY or os.environ.get("CI"), SKIP_REASON)
class CloneTests(unittest.TestCase):
    def scan(self, files, minimum=30):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name, text in files.items():
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8")
            gate.git(root, "init")
            gate.git(root, "add", ".")
            gate.git(root, "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                     "commit", "-m", "test: source")
            return gate.scan(root, minimum)

    def test_renamed_copy(self):
        renamed = BLOCK.replace("alpha", "beta").replace("input", "value").replace("result", "total").replace("index", "step").replace("3", "7")
        pairs = self.scan({"launcher/main/a.c": BLOCK, "launcher/main/b.c": renamed}, 80)
        self.assertTrue(pairs)
        self.assertGreaterEqual(pairs[0]["tokens"], 80)

    def test_include_directives_do_not_count_towards_code_clones(self):
        includes = ''.join(f'#include "owner/header_{index}.h"\n' for index in range(32))
        includes += '# include <stdint.h>\n#include \\\n    "owner/continued.h"\n'
        for suffix in ('c', 'h', 'cc', 'cpp', 'cxx', 'hpp'):
            with self.subTest(suffix=suffix):
                names = ('launcher/main/a.c', f'launcher/main/b.{suffix}')
                self.assertEqual(self.scan(dict.fromkeys(names, includes), 80), [])
                short = 'int value(void) { return 0; }\n'
                self.assertEqual(self.scan(dict.fromkeys(names, includes + short), 80), [])
                pairs = self.scan(dict.fromkeys(names, includes + BLOCK), 80)
                self.assertTrue(pairs)
                self.assertGreaterEqual(pairs[0]['firstFile']['start'], includes.count('\n'))

    def test_header_and_cpp_are_compared(self):
        for suffix in ("h", "cpp", "hpp"):
            with self.subTest(suffix=suffix):
                self.assertTrue(self.scan({"launcher/main/a.c": BLOCK,
                                           f"launcher/main/b.{suffix}": BLOCK}))

    def test_python_renamed_copy(self):
        a = "def first(value):\n    total = 0\n    for index in range(value):\n        if index > 3:\n            total += index * 2\n        else:\n            total -= index\n    return total\n"
        b = a.replace("first", "second").replace("value", "limit").replace("total", "sum_value").replace("3", "8")
        self.assertTrue(self.scan({"scripts/a.py": a, "scripts/b.py": b}))

    def test_a_short_python_copy_under_the_engine_floor_is_a_pair(self):
        pairs = self.scan({"scripts/a.py": CROSS, "scripts/b.py": CROSS.replace("cross", "outer")}, 80)
        self.assertEqual([(pair["firstFile"]["name"], pair["secondFile"]["name"]) for pair in pairs],
                         [("scripts/a.py", "scripts/b.py")])

    def test_below_minimum(self):
        self.assertEqual(self.scan({"launcher/main/a.c": "int a(int x) {\n int y = x + 2;\n return y;\n}\n",
                                    "launcher/main/b.c": "int a(int x) {\n int y = x + 2;\n return y;\n}\n"}, 80), [])

    def test_comments_and_whitespace(self):
        changed = BLOCK.replace("    ", "\t").replace("return", "/* constraint */ return")
        self.assertTrue(self.scan({"launcher/main/a.c": BLOCK, "launcher/main/b.c": changed}))

    def test_one_line_clone(self):
        block = " ".join(BLOCK.splitlines())
        self.assertTrue(self.scan({"launcher/main/a.c": block,
                                   "launcher/main/b.c": block}, 80))

    def test_within_file(self):
        self.assertTrue(self.scan({"launcher/main/a.c": BLOCK + "\n" + BLOCK}))

    def test_skip_rules(self):
        files = {"launcher/main/a.c": BLOCK, "scripts/tool.py": "pass",
                 "launcher/components/x.c": BLOCK, "launcher/packages/testkit/unity/x.h": BLOCK,
                 "launcher/main/b.h": "/* GENERATED FILE - do not edit. */\n" + BLOCK}
        self.assertEqual(self.scan(files), [])

    def test_whole_gate_committed_changes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            git, commit = repository_commands(root)
            def check(expected, message):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    result = gate.main([], root=root)
                self.assertEqual(result, expected, output.getvalue())
                self.assertIn(message, output.getvalue())
            init_repository(git)
            original = root / "launcher/main/a.c"
            original.parent.mkdir(parents=True)
            original.write_text(BLOCK, encoding="utf-8")
            commit()
            git("update-ref", "refs/remotes/origin/main", "HEAD")
            git("checkout", "-b", "feature/test")
            copy = original.with_name("b.c")
            copy.write_text(BLOCK, encoding="utf-8")
            commit()
            check(1, "FAIL: 1 new or growing clone file pairs")
            git("update-ref", "refs/remotes/origin/main", "HEAD")
            git("mv", "launcher/main/a.c", "launcher/main/renamed.c")
            commit()
            check(0, "PASS: no growing clone file pairs (1 checked HEAD pairs, 1 base pairs)")
            git("update-ref", "refs/remotes/origin/main", "HEAD")
            copy.write_text("\n\n" + BLOCK, encoding="utf-8")
            commit()
            check(0, "PASS: no growing clone file pairs (1 checked HEAD pairs, 1 base pairs)")
            original.write_text(BLOCK, encoding="utf-8")
            self.assertEqual(len(gate.scan(root, 80)), 1)

    def test_whole_gate_splitting_a_file_keeps_its_clone(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            git, commit = repository_commands(root)
            init_repository(git)
            whole = root / "launcher/main/whole.c"
            whole.parent.mkdir(parents=True)
            # Mostly comment, so git sees the split as a deletion, not a rename.
            notes = "".join(f"/* note {i}: what the rest of this file held */\n" for i in range(200))
            whole.write_text(notes + BLOCK + "int pad_old;\n" + BLOCK.replace("alpha", "beta"), encoding="utf-8")
            commit()
            git("update-ref", "refs/remotes/origin/main", "HEAD")
            git("checkout", "-b", "feature/test")
            git("rm", "-q", "launcher/main/whole.c")
            whole.parent.mkdir(parents=True, exist_ok=True)
            (root / "launcher/main/part.c").write_text(BLOCK + "int pad_new;\n" + BLOCK.replace("alpha", "beta"),
                                                       encoding="utf-8")
            commit()
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = gate.main([], root=root)
            self.assertEqual(result, 0, output.getvalue())

    def pair(self, first="a.c", second="b.c", fragment=BLOCK):
        return {"firstFile": {"name": first, "start": 1, "end": 12},
                "secondFile": {"name": second, "start": 1, "end": 12},
                "fragment": fragment, "format": "c", "tokens": 90}

    def test_added_clone_fails_when_another_is_removed(self):
        base = self.scan({"launcher/main/a.c": BLOCK, "launcher/main/b.c": BLOCK}, 80)
        head = self.scan({"launcher/main/c.c": BLOCK, "launcher/main/d.c": BLOCK}, 80)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(gate.check_pairs(head, base), 1)
        self.assertIn("launcher/main/c.c:", output.getvalue())
        self.assertIn("launcher/main/d.c:", output.getvalue())

    def test_shortened_existing_clone_passes(self):
        base = self.scan({"launcher/main/a.c": BLOCK, "launcher/main/b.c": BLOCK}, 80)
        shortened = BLOCK.replace("    return result > 5 ? result : input;", "    return result;")
        head = self.scan({"launcher/main/a.c": shortened, "launcher/main/b.c": shortened}, 80)
        self.assertTrue(head)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.check_pairs(head, base), 0)

    def test_extended_existing_clone_fails(self):
        base = self.scan({"launcher/main/a.c": BLOCK, "launcher/main/b.c": BLOCK}, 80)
        extended = BLOCK.replace("    return result", "    result += input * 7;\n    return result")
        head = self.scan({"launcher/main/a.c": extended, "launcher/main/b.c": extended}, 80)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.check_pairs(head, base), 1)

    def test_extraction_rebounds_existing_clone(self):
        source = BLOCK.replace("    return result", "    for (int extra = 0; extra < 50; extra++) {\n        result += extra * input;\n        result ^= input + extra;\n    }\n    return result")
        start = source.index("    for (int index")
        end = source.index("    while")
        extracted = source[:start] + "    result = shared_loop(input);\n" + source[end:]
        base = self.scan({"launcher/main/a.c": source, "launcher/main/b.c": source}, 80)
        helper = "int shared_loop(int input) {\n    int result = 0;\n" + source[start:end] + "    return result;\n}\n"
        head = self.scan({"launcher/main/a.c": extracted, "launcher/main/b.c": extracted,
                          "launcher/main/helper.c": helper}, 80)
        self.assertTrue(head)
        self.assertLess(sum(p["tokens"] for p in head), sum(p["tokens"] for p in base))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.check_pairs(head, base), 0)

    def test_existing_file_pair_token_growth_fails(self):
        files = {"launcher/main/a.c": BLOCK, "launcher/main/b.c": BLOCK}
        extra = "int another(int value) {\n" + "    value += 3; value *= 7; value ^= 11;\n" * 12 + "    return value;\n}\n"
        files["launcher/main/a.c"] += extra
        base = self.scan(files, 80)
        files["launcher/main/b.c"] += extra
        head = self.scan(files, 80)
        names = ("launcher/main/a.c", "launcher/main/b.c")
        base = [p for p in base if gate.pair_key(p) == names]
        head = [p for p in head if gate.pair_key(p) == names]
        self.assertTrue(base)
        self.assertGreater(sum(p["tokens"] for p in head), sum(p["tokens"] for p in base))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.check_pairs(head, base), 1)

    def test_new_copy_cannot_spend_an_unrelated_shrink_in_the_same_pair(self):
        extra = "int another(int value) {\n" + "    value += 3; value *= 7; value ^= 11;\n" * 12 + "    return value;\n}\n"
        separators = {"launcher/main/a.c": "int unique_a(void) { switch (1) { case 1: return 2; } return 3; }\n",
                      "launcher/main/b.c": "void unique_b(void) { while (1) { continue; } }\n"}
        base = self.scan({name: BLOCK + separator + extra
                          for name, separator in separators.items()}, 80)
        shortened = extra.replace("    value += 3; value *= 7; value ^= 11;\n" * 12,
                                  "    value += 3; value *= 7; value ^= 11;\n" * 5)
        new_copy = "int copied(int value) {\n" + "    if (value > 3) { value /= 7; } else { value -= 11; }\n" * 4 + "    return value;\n}\n"
        head = self.scan({name: BLOCK + separator + shortened + separator + new_copy
                          for name, separator in separators.items()}, 80)
        names = ("launcher/main/a.c", "launcher/main/b.c")
        base = [p for p in base if gate.pair_key(p) == names]
        head = [p for p in head if gate.pair_key(p) == names]
        self.assertGreaterEqual(len(head), 3)
        self.assertLessEqual(sum(p["tokens"] for p in head), sum(p["tokens"] for p in base))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.check_pairs(head, base), 1)

    def test_literal_only_table_rows_are_ignored(self):
        rows = "    {0xAB, 1.5e-2f, 42UL, 'x', \"identifier\"}, // label\n" * 12
        pairs = self.scan({"launcher/main/a.c": rows,
                           "launcher/main/b.c": rows}, 80)
        self.assertEqual(pairs, [])

    def assert_ignored_declaration(self, declaration):
        self.assertEqual(gate.filter_pairs([self.pair(fragment=declaration)]), [])
        copied = self.pair(fragment=BLOCK + declaration)
        self.assertEqual(gate.filter_pairs([copied]), [copied])

    def declaration_commit_check(self, base_files, head_files, expected, message):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            git, commit = repository_commands(root)
            init_repository(git)
            for name, text in base_files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text, encoding="utf-8")
            commit()
            git("update-ref", "refs/remotes/origin/main", "HEAD")
            git("checkout", "-b", "feature/test")
            for name, text in head_files.items():
                (root / name).write_text(text, encoding="utf-8")
            commit()
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = gate.main([], root=root)
            self.assertEqual(result, expected, output.getvalue())
            self.assertIn(message, output.getvalue())

    def test_splitting_a_declaration_never_grows_a_pair(self):
        names = [f"value_{index}" for index in range(24)]
        combined = "void\nsetup(void) {\n    int " + ", ".join(f"{name} = {index}" for index, name in enumerate(names)) + ";\n}\n"
        split = "void\nsetup(void) {\n" + "".join(f"    int {name} = {index};\n" for index, name in enumerate(names)) + "}\n"
        other = split.replace("setup", "prepare")
        files = {"launcher/main/a.c": combined, "launcher/main/b.c": other}
        def sizes(pairs):
            return sorted((gate.pair_key(pair), pair["tokens"]) for pair in pairs)
        self.assertEqual(sizes(self.scan(files, 80)), sizes(self.scan({**files, "launcher/main/a.c": split}, 80)))
        self.declaration_commit_check(files, {"launcher/main/a.c": split}, 0, "PASS: no growing clone file pairs")
        pointers = "void\nsetup(void) {\n    unsigned char *left = 0, *right = 0;\n}\n"
        self.assertIn("; unsigned char *right = 0;", gate.split_declarations(pointers))
        self.assertEqual(gate.split_declarations(pointers).count("\n"), pointers.count("\n"))

    def test_a_copied_function_with_declarations_still_counts(self):
        body = "int\nscale(int input) {\n    const int low = input / 2, high = input * 2;\n" + BLOCK.split("{", 1)[1]
        self.declaration_commit_check({"launcher/main/a.c": body}, {"launcher/main/b.c": body}, 1,
                                      "FAIL: 1 new or growing clone file pairs")

    def test_a_copy_just_above_the_limit_is_still_found(self):
        body = "void\nsetup(int input) {\n" + "".join(f"    const unsigned value_{i} = input + {i} * 3;\n" for i in range(8)) + "}\n"
        self.assertTrue(self.scan({"launcher/main/a.c": body, "launcher/main/b.c": body}, 80))

    def test_declaration_splitting_leaves_other_statements_and_lines_alone(self):
        text = ("void\nf(int a) {\n    return a;\n    for (int i = 0, n = 2; i < n; i++) {\n    }\n"
                "    a = g(a, 1), a = 2;\n    char *s = \"a, b;\"; /* int q, r; */\n}\n")
        split = gate.split_declarations(text)
        self.assertEqual(split.count("\n"), text.count("\n"))
        for kept in ("return a;", "for (int i = 0, n = 2;", "a = g(a, 1), a = 2;", "\"a, b;\"", "/* int q, r; */"):
            self.assertIn(kept, split)
        cpp = "void f() {\n    delete p;\n    throw err;\n}\n"
        self.assertEqual(gate.split_declarations(cpp), cpp)

    def test_adding_grouping_parentheses_never_grows_a_pair(self):
        plain = BLOCK.replace("result += index * 2;", "result += index * 2 + input;")
        grouped = plain.replace("index * 2 + input", "(index * 2) + input")
        files = {"launcher/main/a.c": plain, "launcher/main/b.c": grouped.replace("alpha", "beta")}
        self.declaration_commit_check({**files, "launcher/main/a.c": plain.replace("index * 2 + input", "index * (2 + input)")},
                                      {"launcher/main/a.c": plain}, 0, "PASS: no growing clone file pairs")
        self.assertEqual(len(self.scan(files, 80)), len(self.scan({**files, "launcher/main/b.c": plain.replace("alpha", "beta")}, 80)))

    def test_call_and_condition_parentheses_are_kept(self):
        text = "void\nf(int a) {\n    if (a) {\n        g((a * 2) + 1, (a));\n    }\n}\n"
        self.assertEqual(gate.drop_grouping_parentheses(text), text.replace("(a * 2)", " a * 2 ").replace(", (a)", ",  a "))

    def test_a_run_test_list_is_ignored_but_a_copy_beside_it_is_not(self):
        listing = "}\n\nvoid\nsuite_a(void) {\n" + "".join(f"    RUN_TEST(test_{i});\n" for i in range(24)) + "}\n"
        self.assert_ignored_declaration(listing)

    def test_literal_defines_are_ignored_but_code_and_function_macros_are_not(self):
        listing = "".join(f"#define FIELD_{i} {i}U\n" for i in range(24))
        listing += '#define SCALE 0.5F\n#define MASK 0xFFUL\n#define LABEL "value"\n'
        self.assertEqual(self.scan(dict.fromkeys(("launcher/main/a.c", "launcher/main/b.c"), listing), 80), [])
        functions = "".join(f"#define APPLY_{i}(x) ((x) + {i})\n" for i in range(24))
        self.assertEqual(len(gate.filter_pairs([self.pair(fragment=functions)])), 1)
        expressions = "".join(f"#define VALUE_{i} (input + {i})\n" for i in range(24))
        self.assertEqual(len(gate.filter_pairs([self.pair(fragment=expressions)])), 1)

    def test_renaming_defines_are_ignored(self):
        listing = "".join(f"#define name_{i} other_name_{i}\n" for i in range(24))
        self.assertEqual(self.scan(dict.fromkeys(("launcher/main/a.c", "launcher/main/b.c"), listing), 80), [])

    def test_literal_vector_defines_are_ignored_but_calls_and_identifiers_are_not(self):
        listing = "".join(f"#define ROW_{i} {{{i}, -1, 0xFFU, 1.5F}}\n" for i in range(24))
        listing += '#define CONTINUED {1, \\\n    -2, 3}\n'
        self.assertEqual(self.scan(dict.fromkeys(("launcher/main/a.c", "launcher/main/b.c"), listing), 80), [])
        values = ", ".join(str(i) for i in range(100))
        pairs = self.scan({"launcher/main/a.c": f"static const int values[] = {{{values}}};",
                           "launcher/main/b.h": f"#define VALUES {{{values}}}\n"}, 80)
        self.assertFalse([pair for pair in pairs if pair["firstFile"]["name"] != pair["secondFile"]["name"]])
        for body in ("{value, 1}", "{call(1), 2}", "{1 + 2, 3}"):
            code = "".join(f"#define ROW_{i} {body}\n" for i in range(24))
            self.assertEqual(len(gate.filter_pairs([self.pair(fragment=code)])), 1)

    def test_define_stripping_keeps_clone_line_numbers(self):
        prefix = '#define X 1 // note\n#define ROW {1, \\\n 2, 3}\n'
        plain = self.scan(dict.fromkeys(("launcher/main/a.c", "launcher/main/b.c"), BLOCK), 80)
        stripped = self.scan(dict.fromkeys(("launcher/main/a.c", "launcher/main/b.c"), prefix + BLOCK), 80)
        self.assertEqual(len(stripped), len(plain))
        for original, shifted in zip(plain, stripped):
            for side in ("firstFile", "secondFile"):
                self.assertEqual(shifted[side]["start"], original[side]["start"] + prefix.count("\n"))
                self.assertEqual(shifted[side]["end"], original[side]["end"] + prefix.count("\n"))

    def test_crlf_defines_keep_clone_line_numbers(self):
        prefix = '#define X 1\n#define ROW {1, \\\n 2, 3}\n'
        files = dict.fromkeys(("launcher/main/a.c", "launcher/main/b.c"), prefix + BLOCK)
        contents = [(name, text.replace("\n", "\r\n").encode("utf-8")) for name, text in files.items()]
        with mock.patch.object(gate, "revision_contents", return_value=contents):
            pairs = self.scan(files, 80)
        self.assertTrue(pairs)
        for pair in pairs:
            for side in ("firstFile", "secondFile"):
                self.assertEqual(pair[side]["start"], prefix.count("\n") + 1)
                self.assertEqual(pair[side]["end"], (prefix + BLOCK).count("\n"))

    def test_literal_define_with_line_comment_is_stripped(self):
        listing = "".join(f"#define X_{i} 1 // note\n" for i in range(100))
        self.assertEqual(self.scan(dict.fromkeys(("launcher/main/a.c", "launcher/main/b.c"), listing), 80), [])

    def test_initializer_table_rows_are_ignored_but_statements_are_not(self):
        rows = "".join(f'    {{"row_{i}", build_{i}, {i}u, 0, 1000, 0}},\n' for i in range(16))
        self.assert_ignored_declaration(rows)
        loops = "".join(f"    for (int y = {i}; y < 9; y++) {{ set(s, x, y, CELL); }}\n" for i in range(16))
        self.assertEqual(len(gate.filter_pairs([self.pair(fragment=loops)])), 1)

    def test_code_moved_out_of_a_deleted_file_spends_its_clone(self):
        base = self.scan({"launcher/main/old.c": BLOCK + "int pad_old;\n" + BLOCK.replace("alpha", "beta")}, 80)
        head = self.scan({"launcher/main/new.c": BLOCK + "int pad_new;\n" + BLOCK.replace("alpha", "beta")}, 80)
        self.assertTrue(base and head)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.check_pairs(head, base, moved=base), 0)
            self.assertEqual(gate.check_pairs(head, base), 1)

    def test_a_moved_clone_that_grew_still_fails(self):
        base = self.scan({"launcher/main/old.c": BLOCK + "int pad_old;\n" + BLOCK.replace("alpha", "beta")}, 80)
        grown = BLOCK.replace("    return result", "    result += input * 7;\n    return result")
        head = self.scan({"launcher/main/new.c": grown + "int pad_new;\n" + grown.replace("alpha", "beta")}, 80)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.check_pairs(head, base, moved=base), 1)

    def test_self_match_is_ignored(self):
        self.assertEqual(gate.filter_pairs([self.pair("a.c", "a.c")]), [])

    def test_untouched_existing_clone_passes(self):
        pairs = self.scan({"launcher/main/a.c": BLOCK, "launcher/main/b.c": BLOCK}, 80)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.check_pairs(pairs, pairs), 0)

    def test_key_ignores_locations_file_order_and_whitespace(self):
        base = self.pair()
        head = self.pair("b.c", "a.c", BLOCK.replace("    ", "\t"))
        head["firstFile"]["start"] = 20
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.check_pairs([head], [base]), 0)

    def test_same_file_distinct_ranges_are_kept(self):
        pair = self.pair("a.c", "a.c")
        pair["secondFile"]["start"] = 20
        self.assertEqual(gate.filter_pairs([pair]), [pair])

    def test_fragment_edit_with_equal_budget_passes(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.check_pairs([self.pair(fragment=BLOCK.replace("alpha", "beta"))],
                                             [self.pair()]), 0)

    def test_committed_scan_ignores_worktree_edits(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def git(*args):
                return subprocess.run(["git", *args], cwd=root, check=True,
                                      capture_output=True, text=True).stdout.strip()
            init_repository(git)
            for name in ("a.c", "b.c"):
                target = root / "launcher/main" / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(BLOCK, encoding="utf-8")
            git("add", ".")
            git("commit", "-m", "test: base")
            base = git("rev-parse", "HEAD")
            git("update-ref", "refs/remotes/origin/main", base)
            git("checkout", "-b", "feature/test")
            (root / "launcher/main/a.c").write_text("int single;", encoding="utf-8")
            git("add", ".")
            git("commit", "-m", "test: remove clone")
            self.assertEqual(gate.comparison_base(root), base)
            self.assertTrue(gate.scan(root, 80, revision=base))
            self.assertTrue(gate.scan(root, 80, names=["launcher/main/a.c", "launcher/main/b.c",
                                                      "launcher/main/new.c"], revision=base))
            self.assertEqual(gate.scan(root, 80, names=["launcher/main/a.c"], revision=base), [])
            self.assertEqual(gate.scan(root, 80, revision="HEAD"), [])
            (root / "launcher/main/a.c").write_text(BLOCK, encoding="utf-8")
            self.assertEqual(gate.scan(root, 80, revision="HEAD"), [])
            git("update-ref", "refs/remotes/origin/main", git("rev-parse", "HEAD"))
            self.assertEqual(gate.comparison_base(root), base)
            stacked = git("rev-parse", "HEAD")
            git("update-ref", "refs/remotes/origin/feature/base", stacked)
            git("update-ref", "refs/remotes/origin/main", base)
            git("checkout", "-q", "-b", "feature/stacked")
            (root / "launcher/main/c.c").write_text("int stacked;", encoding="utf-8")
            git("add", ".")
            git("commit", "-m", "test: stacked")
            self.assertEqual(gate.comparison_base(root, against="origin/feature/base"), stacked)

    def test_unrelated_branch_change_skips_base_scan(self):
        pair = self.pair()
        def git(root, *args):
            if args[0] == "diff":
                return b"M\0docs/notes.md\0"
            return b"head\n"
        with mock.patch.object(gate, "git", side_effect=git), \
                mock.patch.object(gate, "comparison_base", return_value="base"), \
                mock.patch.object(gate, "scan", return_value=[pair]) as scan, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.main([]), 0)
        scan.assert_called_once_with(gate.ROOT, 80, revision="head")

    def test_missing_node_message(self):
        with mock.patch.object(gate.shutil, "which", return_value=None):
            self.assertIn("Node.js is missing", gate.dependency_problem())

    def test_missing_engine_message(self):
        with mock.patch.object(gate, "ENGINE", Path("missing-engine.js")):
            self.assertIn("npm ci --prefix scripts/gates", gate.dependency_problem())


class ShortFunctionTests(unittest.TestCase):
    def pairs(self, *texts, minimum=gate.MIN_TOKENS):
        return gate.short_function_pairs({f"scripts/{index}.py": text for index, text in enumerate(texts)}, minimum)

    def test_a_copy_with_renamed_parameters_annotations_and_docstring_pairs(self):
        renamed = ('def outer(left: tuple, right: tuple) -> tuple:\n    """The cross product."""\n'
                   + CROSS.splitlines()[1].replace("a[", "left[").replace("b[", "right[") + "\n")
        self.assertEqual(len(self.pairs(CROSS, renamed)), 1)

    def test_a_nested_copy_pairs_and_three_copies_make_three_pairs(self):
        nested = "def write(rows):\n" + "".join("    " + line + "\n" for line in CROSS.splitlines()) + "    return rows\n"
        self.assertEqual(len(self.pairs(CROSS, nested, CROSS)), 3)

    def test_a_changed_literal_global_or_attribute_does_not_pair(self):
        for changed in (CROSS.replace("b[0])", "b[2])"), CROSS.replace("(a[1]", "(norm(a)[1]"),
                        CROSS.replace("return (a[1]", "return (a.x[1]")):
            with self.subTest(changed=changed):
                self.assertEqual(self.pairs(CROSS, changed), [])

    def test_an_idiom_under_the_node_floor_does_not_pair(self):
        idiom = "def tearDown(self):\n    self.directory.cleanup()\n"
        self.assertEqual(self.pairs(idiom, idiom), [])

    def test_a_function_at_the_engine_floor_is_left_to_the_engine(self):
        self.assertEqual(len(self.pairs(CROSS, CROSS)), 1)
        self.assertEqual(self.pairs(CROSS, CROSS, minimum=gate.function_shape(
            gate.ast.parse(CROSS).body[0])[1]), [])


if __name__ == "__main__":
    unittest.main()
