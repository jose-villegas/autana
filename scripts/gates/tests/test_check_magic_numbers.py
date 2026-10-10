"""Committed-tree regression fixtures for the magic literal ratchet."""
import contextlib
import io
from pathlib import Path
import sys
import unittest
from unittest import mock
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_magic_numbers as gate
from c_constants import constants
from gate_tree import commit, temporary_tree, write

FOLDED_SHIFT = 16
RARE = 1 << (FOLDED_SHIFT - 4)
C_PATH = "launcher/main/render/a.c"
PY_PATH = "scripts/tool.py"


class MagicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.addClassCleanup(cls.temp.cleanup)
        gate.git(cls.root, "init", "-q")

    def scan(self, files):
        return gate.scan(files, direct=gate.include_graph(files))

    def tree(self, files):
        if (self.root / ".git").exists():
            gate.git(self.root, "rm", "-rf", "--ignore-unmatch", ".")
        for path, text in files.items():
            write(self.root, path, text)
        gate.git(self.root, "add", ".")
        gate.git(self.root, "-c", "user.name=t", "-c", "user.email=t@t",
                 "commit", "--allow-empty", "-qm", "fixture")
        return gate.git(self.root, "rev-parse", "HEAD").decode().strip()

    def restates(self, code, header=""):
        files = {C_PATH: code}
        if header:
            files["launcher/main/value.h"] = header
        return self.scan(files)[0].get((C_PATH, gate.RESTATE), [])

    def test_direct_and_included_constants(self):
        self.assertEqual(len(self.restates(f"#define VALUE {RARE}\nint x = {RARE};")), 1)
        self.assertEqual(len(self.restates(f'#include "value.h"\nint x = {RARE};',
                                         f"#define VALUE {RARE}")), 1)
        hits = self.restates(f"#define FIRST {RARE}\n#define SECOND {RARE}\nint x = 0x{RARE:x}UL;")
        self.assertEqual(len(hits), 1)
        self.assertIn("FIRST", hits[0].message)
        self.assertIn("SECOND", hits[0].message)
        self.assertEqual(len(self.restates(f"#define VALUE {RARE}\nswitch(x) {{ case {RARE}: break; }}")), 1)

    def test_thresholds(self):
        values = (gate.RARE_MINIMUM - 1, gate.RARE_MINIMUM,
                  gate.RARE_MINIMUM + gate.ROUND_MULTIPLE, RARE, RARE - 1)
        for value, expected in zip(values, (0, 0, 0, 1, 1)):
            with self.subTest(value=value):
                self.assertEqual(len(self.restates(f"#define VALUE {value}\nint x = {value};")), expected)
        value = gate.RARE_MINIMUM + 1
        self.assertEqual(len(self.restates(f"#define VALUE {value}\nint x = {value};")), 1)

    def test_shift_operands_and_shift_expression(self):
        shift = RARE.bit_length() - 1
        code = f"#define VALUE_SHIFT {shift}\nint x = value >> {shift};\nx <<= {shift};"
        self.assertEqual(len(self.restates(code)), 2)
        self.assertFalse(self.restates(f"#define VALUE {shift}\nint x = value >> {shift};"))
        self.assertFalse(self.restates(f"#define VALUE_SHIFT {shift}\nint x = 1 << {shift};"))

    def test_exempt_contexts_and_folding(self):
        shift = RARE.bit_length() - 1
        code = f"""#define VALUE (1u << {shift})
enum {{ OTHER = {RARE} }};
static const unsigned scalar = {RARE};
int array[{RARE}];
_Static_assert(VALUE == {RARE}, "limit");
static_assert(VALUE == {RARE}, "limit");
int row[] = {{ {', '.join([str(RARE)] * gate.DATA_ROW_MINIMUM)} }};
// {RARE}
char *text = "{RARE}";
int x = 1 << {shift};
"""
        self.assertFalse(self.restates(code))
        self.assertEqual(len(self.restates(code + f"\nint y = {RARE};")), 1)
        self.assertEqual(len(self.restates(f"const int A = {RARE};\n#define B (A + 1)\nint x = {RARE + 1};")), 1)
        self.assertEqual(len(self.restates(f"#define K {FOLDED_SHIFT}\n#define A (1u << K)\nint x = {1 << FOLDED_SHIFT};")), 1)

    def test_shared_index_keeps_documentation_vocabulary(self):
        with temporary_tree([(C_PATH, f"#define BASE {RARE}\n#define FOLDED (BASE * 2)\n"
                                      f"const int scalar = {RARE};\n#define HEX 0x{RARE:x}U\n")]) as root:
            commit(root, ".")
            values = constants(root)
            self.assertEqual(values["FOLDED"], RARE * 2)
            self.assertEqual(values["scalar"], RARE)
            self.assertEqual(values["HEX"], RARE)
            self.assertEqual(constants(root, literal_only=True), {"BASE": RARE})

    def test_protocol_strings_and_docstrings(self):
        hits, _, _ = self.scan({C_PATH: 'char *x = "FRAME_READY";',
                               PY_PATH: '"""FRAME_READY"""\nx = "FRAME_READY"\n# "FRAME_READY"\n'})
        self.assertEqual(len(hits[(PY_PATH, gate.PROTOCOL)]), 1)
        self.assertNotIn((C_PATH, gate.PROTOCOL), hits)
        self.assertFalse(self.scan({C_PATH: 'char *x = "FRAME_READY";'})[0])
        hits, _, _ = self.scan({"launcher/main/apps/demo/fixtures/a.h": 'char *x = "FRAME_READY";',
                               PY_PATH: 'x = "FRAME_READY"'})
        self.assertFalse(hits)

    def check_change(self, before, after, expected, rename=False, changed=None):
        final = before | after
        renames = {}
        if rename:
            renamed = "launcher/main/render/renamed.c"
            final[renamed] = final.pop(C_PATH)
            renames[renamed] = C_PATH
        output = io.StringIO()
        with (mock.patch.object(gate, "comparison_base", return_value="base"),
              mock.patch.object(gate, "changed_paths", return_value=(set(final) if changed is None else changed, renames, set())),
              mock.patch.object(gate, "revision_tree", side_effect=lambda root, rev: final if rev == "HEAD" else before),
              contextlib.redirect_stdout(output)):
            result = gate.main(["--base", "base"], root=self.root)
        self.assertEqual(result, expected, output.getvalue())
        return output.getvalue()

    def test_ratchet_growth_fall_new_and_rename(self):
        definition = f"#define VALUE {RARE}\n"
        one = definition + f"int x = {RARE};\n"
        two = one + f"int y = {RARE};\n"
        self.check_change({C_PATH: one}, {C_PATH: two}, 1)
        self.check_change({C_PATH: two}, {C_PATH: one}, 0)
        self.check_change({C_PATH: definition}, {"launcher/main/new.c": one}, 1)
        self.check_change({C_PATH: one}, {}, 0, rename=True)

    def test_header_and_c_only_changes(self):
        header = "launcher/main/value.h"
        code = f'#include "value.h"\nint x = {RARE};'
        other = "launcher/main/other.c"
        before = {C_PATH: code, header: "", other: f"#define OTHER {RARE}\nint x = {RARE};"}
        output = self.check_change(before, {header: f"#define VALUE {RARE}"}, 1, changed={header})
        self.assertNotIn(other, output)
        direct = gate.include_graph(before)
        hits = gate.scan(before, gate.affected_files(before, {header}, direct), direct=direct)[0]
        self.assertIn((C_PATH, gate.RESTATE), hits)
        self.assertNotIn((other, gate.RESTATE), hits)
        self.check_change({C_PATH: "", PY_PATH: 'x = "FRAME_READY"'},
                          {C_PATH: 'char *x = "FRAME_READY";'}, 1)

    def test_two_hop_header_change_exposes_growth(self):
        middle = "launcher/main/render/mid.h"
        deep = "launcher/main/render/deep.h"
        before = {C_PATH: f'#include "mid.h"\nint x = {RARE};',
                  middle: '#include "deep.h"\n', deep: ""}
        output = self.check_change(before, {deep: f"#define VALUE {RARE}\n"}, 1, changed={deep})
        self.assertIn(f"{C_PATH}: RESTATE 0 -> 1", output)

    def test_sibling_header_addition_preserves_existing_count(self):
        sibling = "launcher/main/render/value.h"
        root_header = "launcher/main/value.h"
        definition = f"#define VALUE {RARE}\n"
        before = {C_PATH: f'#include "value.h"\nint x = {RARE};', root_header: definition}
        output = self.check_change(before, {sibling: definition}, 0, changed={sibling})
        self.assertIn("RESTATE (compared files)=1 -> 1", output)
        self.check_change(before, {sibling: definition}, 0, rename=True,
                          changed={sibling, "launcher/main/render/renamed.c"})

    def test_sibling_header_deletion_exposes_growth(self):
        sibling = "launcher/main/render/value.h"
        root_header = "launcher/main/value.h"
        other = "launcher/main/other.c"
        with temporary_tree([(C_PATH, f'#include "value.h"\nint x = {RARE};'),
                             (other, "int unrelated = 0;\n"),
                             (sibling, f"#define VALUE {RARE + 1}\n"),
                             (root_header, f"#define VALUE {RARE}\n")]) as root:
            base = commit(root, ".")
            (root / sibling).unlink()
            write(root, other, "int unrelated = 1;\n")
            commit(root, ".")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(gate.main(["--base", base], root=root), 1)
            self.assertIn(f"{C_PATH}: RESTATE 0 -> 1", output.getvalue())

    def test_file_scope_enums_survive_function_masking(self):
        for declaration in (f"enum {{ VALUE = {RARE} }};",
                            f"typedef enum {{ VALUE = {RARE} }} value_t;"):
            with self.subTest(declaration=declaration):
                code = (f"void first(void) {{ enum {{ VALUE = {RARE + 1} }}; }}\n"
                        f"{declaration}\nint x = {RARE};\nint y = {RARE + 1};")
                self.assertEqual([hit.line for hit in self.restates(code)], [3])
                local = f"void f(void) {{ {declaration} }}\nint x = {RARE};"
                self.assertFalse(self.restates(local))

    def test_file_scope_enum_after_macro_call(self):
        for declaration in (f"enum {{ VALUE = {RARE} }};",
                            f"typedef enum {{ VALUE = {RARE} }} value_t;"):
            with self.subTest(declaration=declaration):
                code = f"#define IDENTITY(x) (x)\n{declaration}\nint x = {RARE};"
                self.assertEqual([hit.line for hit in self.restates(code)], [3])
        for scope in ("namespace N", "struct S", "class C", "union U"):
            with self.subTest(scope=scope):
                value = RARE + 1
                code = f"#if defined(X)\n{scope} {{ enum {{ V = {value} }}; }};\nint x = {value};"
                hits = self.scan({"editor/src/a.cpp": code})[0]
                self.assertEqual([hit.line for hit in hits.get(("editor/src/a.cpp", gate.RESTATE), [])], [3])

    def test_cpp_qualified_function_body_masks_local_owners(self):
        for qualifier in ("const", "noexcept", "override", "final", "const noexcept"):
            with self.subTest(qualifier=qualifier):
                code = f"int f() {qualifier} {{ enum {{ LOCAL = {RARE} }}; }}\nint x = {RARE};"
                self.assertFalse(self.scan({"editor/src/a.cpp": code})[0])

    def test_nested_function_bodies_mask_local_owners(self):
        for scope in ("namespace n", 'extern "C"', "class C"):
            for declaration in (f"enum {{ LOCAL = {RARE} }};", f"const int LOCAL = {RARE};"):
                with self.subTest(scope=scope, declaration=declaration):
                    code = f"{scope} {{ void f() {{ {declaration} }} }};\nint x = {RARE};"
                    self.assertFalse(self.scan({"editor/src/a.cpp": code})[0])

    def test_escapes(self):
        code = f"#define VALUE {RARE}\nint x = {RARE}; /* magic: wire value */"
        hits, escapes, errors = self.scan({C_PATH: code})
        self.assertFalse(hits)
        self.assertTrue(escapes)
        self.assertFalse(errors)
        self.assertTrue(self.scan({C_PATH: code.replace("wire value", "")})[2])
        hits, escapes, errors = self.scan({C_PATH: 'char *x = "FRAME_READY";',
                                         PY_PATH: 'x = "FRAME_READY" # magic: wire value'})
        self.assertFalse(hits)
        self.assertTrue(escapes)
        self.assertTrue(self.scan({PY_PATH: 'x = 0 # magic: '})[2])

    def test_visibility_and_transitive_includes(self):
        header = f"#define VALUE {RARE}"
        code = f"int x = {RARE};"
        self.assertFalse(self.restates(code, header))
        self.assertEqual(len(self.restates('#include "value.h"\n' + code, header)), 1)
        files = {C_PATH: '#include "b.h"\n' + code,
                 "launcher/main/render/b.h": '#include "../c.h"',
                 "launcher/main/c.h": header}
        self.assertEqual(len(self.scan(files)[0][C_PATH, gate.RESTATE]), 1)

    def test_local_scalars_and_subscripts(self):
        self.assertFalse(self.restates(f"void f(void) {{ const int local = {RARE}; }}\nint x = {RARE};"))
        code = f"#define VALUE {RARE}\nint table[{RARE}];\nint x = table[{RARE}];"
        hits = self.restates(code)
        self.assertEqual([hit.line for hit in hits], [3])
        self.assertEqual(len(self.restates(code + f"\nvoid f(void) {{ return table[{RARE}]; }}")), 2)
        self.assertEqual(len(self.restates(f"const int VALUE = {RARE};\nint x = {RARE};")), 1)

    def test_local_owners_after_other_bodies(self):
        prefix = "struct S { int member; };\nenum { SMALL = 2 };\nvoid first(void) {}\n"
        for declaration in (f"const int LOCAL = {RARE};", f"enum {{ LOCAL = {RARE} }};"):
            with self.subTest(declaration=declaration):
                code = prefix + f"void f(void) {{ {declaration} }}\nint x = {RARE};"
                self.assertFalse(self.restates(code))
        self.assertFalse(self.restates(f"void f(void) {{ enum {{ LOCAL = {RARE} }}; }}\nint x = {RARE};"))

    def test_local_const_restates_visible_define(self):
        code = f"#define VALUE {RARE}\nvoid f(void) {{ const int x = {RARE}; }}"
        self.assertEqual([hit.line for hit in self.restates(code)], [2])

    def test_assignment_and_multidimensional_bounds(self):
        code = f"#define VALUE {RARE}\ntable[{RARE}] = v;\nint g[2][{RARE}];"
        self.assertEqual([hit.line for hit in self.restates(code)], [2])

    def test_include_search_order_and_commented_include(self):
        files = {C_PATH: f'#include "value.h"\nint x = {RARE};',
                 "launcher/main/render/value.h": f"#define SIBLING {RARE}",
                 "launcher/main/value.h": f"#define ROOT {RARE + 1}"}
        hits = self.scan(files)[0]
        self.assertIn((C_PATH, gate.RESTATE), hits)
        hit = hits[C_PATH, gate.RESTATE][0]
        self.assertIn("SIBLING", hit.message)
        self.assertNotIn("ROOT", hit.message)
        files[C_PATH] = f'/*\n#include "value.h"\n*/\nint x = {RARE};'
        self.assertFalse(self.scan(files)[0])

    def test_exact_token_minimum(self):
        token = "A" * gate.TOKEN_MINIMUM
        hits = self.scan({C_PATH: f'char *x = "{token}";', PY_PATH: f'x = "{token}"'})[0]
        self.assertIn((PY_PATH, gate.PROTOCOL), hits)
        self.assertEqual(len(hits[PY_PATH, gate.PROTOCOL]), 1)

    def test_excluded_restatement(self):
        path = "launcher/components/a.c"
        self.assertFalse(self.scan({path: f"#define VALUE {RARE}\nint x = {RARE};"})[0])

    def test_python_escape_prose(self):
        hits, logged, errors = self.scan({C_PATH: 'char *x = "FRAME_READY";',
                                         PY_PATH: 'x = "FRAME_READY" # see # magic: x'})
        self.assertIn((PY_PATH, gate.PROTOCOL), hits)
        self.assertEqual(len(hits[PY_PATH, gate.PROTOCOL]), 1)
        self.assertFalse(logged)
        self.assertFalse(errors)

    def test_report_editor_revision_and_same_line_counts(self):
        with temporary_tree([("editor/src/a.cpp", f"#define VALUE {RARE}\nint x = {RARE}, y = {RARE};")]) as root:
            commit(root, ".")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(gate.main(["--report"], root=root), 0)
            self.assertIn("editor: RESTATE=2", output.getvalue())
            self.assertIn("(2 hits)", output.getvalue())

    def test_report_included_header_visibility(self):
        header = "launcher/packages/math/include/math/scalar/value.h"
        for included, expected in ((True, 1), (False, 0)):
            with self.subTest(included=included):
                include = '#include "math/scalar/value.h"\n' if included else ""
                with temporary_tree([(header, f"#define VALUE {RARE}\n"),
                                     (C_PATH, f"{include}int x = {RARE};\n")]) as root:
                    commit(root, ".")
                    output = io.StringIO()
                    with contextlib.redirect_stdout(output):
                        self.assertEqual(gate.main(["--report"], root=root), 0)
                    self.assertIn(f"RESTATE={expected} PROTOCOL=0", output.getvalue())
                    if included:
                        self.assertIn(f"VALUE ({header}:1)", output.getvalue())

    def test_report_line_order(self):
        hits = {(PY_PATH, gate.PROTOCOL): [gate.Hit(PY_PATH, line, str(line)) for line in (3, 1, 2)]}
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            gate.report(hits)
        lines = output.getvalue().splitlines()
        self.assertEqual(lines[1:4], [str(hit) for hit in sorted(hits[PY_PATH, gate.PROTOCOL], key=lambda hit: hit.line)])

    def test_continued_define(self):
        code = f"#define VALUE \\\n    ({RARE} + 1)\nint x = {RARE + 1};"
        hits = self.restates(code)
        self.assertEqual([hit.line for hit in hits], [3])
        self.assertIn("VALUE", hits[0].message)
        self.assertFalse(self.restates(f"#define BASE {RARE}\n#define OTHER \\\n    {RARE}"))

    def test_small_plain_shift_and_left_operand(self):
        code = f"#define VALUE_SHIFT {FOLDED_SHIFT}\nint x = {FOLDED_SHIFT};"
        self.assertFalse(self.restates(code))
        self.assertEqual(len(self.restates(code.replace("= ", "= value >> "))), 1)
        self.assertFalse(self.restates(f"#define VALUE {RARE}\nint x = {RARE} << n;"))

    def test_protocol_shapes_and_owner_scope(self):
        short = "A" * (gate.TOKEN_MINIMUM - 1)
        strings = (short, "lowercase", "frame.ready", "FRAME_READY")
        c = "\n".join(f'char *s{i} = "{value}";' for i, value in enumerate(strings))
        py = "\n".join(f'x{i} = "{value}"' for i, value in enumerate(strings))
        hits = self.scan({C_PATH: c, PY_PATH: py})[0][PY_PATH, gate.PROTOCOL]
        self.assertEqual([hit.line for hit in hits], [3, 4])
        self.assertFalse(self.scan({"editor/src/a.cpp": c, "launcher/test/a.c": c,
                                   PY_PATH: py})[0])
        self.assertFalse(self.scan({C_PATH: c, "elsewhere/tool.py": py})[0])
        self.assertFalse(self.scan({C_PATH: '/* "FRAME_READY" */', PY_PATH: py})[0])
        self.assertFalse(self.scan({C_PATH: '#include "frame.ready"', PY_PATH: py})[0])
        for path in ("launcher/main/fixtures/a.c", "launcher/components/a.c"):
            self.assertFalse(self.scan({path: c, PY_PATH: py})[0])
        generated = "// GENERATED FILE - do not edit.\n" + c
        self.assertFalse(self.scan({C_PATH: generated, PY_PATH: py})[0])
        hits = self.scan({C_PATH: c + '\nchar *extra = "FRAME_READY";', PY_PATH: py})[0]
        self.assertIn(f"{C_PATH}:4; 2 C sites", hits[PY_PATH, gate.PROTOCOL][-1].message)

    def test_editor_and_excluded_files(self):
        code = f"#define VALUE {RARE}\nint x = {RARE};"
        hits = self.scan({"editor/src/a.cpp": code})[0]
        self.assertEqual(len(hits["editor/src/a.cpp", gate.RESTATE]), 1)
        self.assertFalse(self.scan({"editor/fixtures/a.cpp": code})[0])
        self.assertFalse(self.scan({"editor/src/a.cpp": "// GENERATED FILE - do not edit.\n" + code})[0])

    def test_short_data_row(self):
        row = ", ".join([str(RARE)] * (gate.DATA_ROW_MINIMUM - 1))
        self.assertEqual(len(self.restates(f"#define VALUE {RARE}\nint row[] = {{ {row} }};")),
                         gate.DATA_ROW_MINIMUM - 1)

    def test_escape_line_and_prose(self):
        code = f"#define VALUE {RARE}\nint x = {RARE}; /* magic: wire */\nint y = {RARE};"
        self.assertEqual([hit.line for hit in self.restates(code)], [3])
        self.assertEqual(len(self.restates(code.replace("magic: wire", "prose magic: wire"))), 2)

    def test_main_escape_exit_codes(self):
        for reason, expected in (("", 1), ("wire", 0)):
            with self.subTest(reason=reason):
                code = f"#define VALUE {RARE}\nint x = {RARE}; /* magic: {reason} */"
                self.check_change({C_PATH: ""}, {C_PATH: code}, expected)
                with (mock.patch.object(gate, "revision_tree", return_value={C_PATH: code}),
                      contextlib.redirect_stdout(io.StringIO())):
                    self.assertEqual(gate.main(["--report"], root=self.root), expected)

    def test_growth_identity_ignores_owner_line(self):
        before = f"#define VALUE {RARE}\nint x = {RARE};"
        after = "\n" + before + f"\nint y = {RARE + 1};"
        after = f"#define OTHER {RARE + 1}\n" + after
        output = self.check_change({C_PATH: before}, {C_PATH: after}, 1)
        self.assertNotIn(f"{RARE} restates", output)
        self.assertIn(f"{RARE + 1} restates OTHER", output)
        self.assertIn("RESTATE (compared files)=1 -> 2", output)

    def test_conflicting_names_do_not_fold(self):
        code = f"#define VALUE {RARE}\n#define VALUE {RARE + 1}\n#define NEXT (VALUE + 2)\nint x = {RARE + 2};"
        self.assertFalse(self.restates(code))

    def test_two_app_modules_and_tools(self):
        files = {f"launcher/main/apps/{name}/a.c": f"#define VALUE {RARE}\nint x = {RARE};"
                 for name in ("first", "second")}
        files[C_PATH] = 'char *s = "FRAME_READY";'
        files["launcher/tools/a.py"] = 'x = "FRAME_READY"'
        self.tree(files)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(gate.main(["--report"], root=self.root), 0)
        for expected in ("apps/first: RESTATE=1", "apps/second: RESTATE=1",
                         "launcher/tools: RESTATE=0 PROTOCOL=1"):
            self.assertIn(expected, output.getvalue())

    def test_report_modules(self):
        with temporary_tree({C_PATH: f"#define VALUE {RARE}\nint x = {RARE};",
                             PY_PATH: 'x = "FRAME_READY"',
                             "launcher/main/apps/demo/a.h": 'char *x = "FRAME_READY";'}.items()) as root:
            commit(root, ".")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(gate.main(["--report"], root=root), 0)
            self.assertIn("render: RESTATE=1", output.getvalue())
            self.assertIn("scripts: RESTATE=0 PROTOCOL=1", output.getvalue())


if __name__ == "__main__":
    unittest.main()
