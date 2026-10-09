"""Committed-tree regression fixtures for the magic literal ratchet."""
import contextlib
import io
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_magic_numbers as gate
from c_constants import constants
from gate_tree import commit, temporary_tree, write

RARE = gate.POWER_MINIMUM
FOLDED_SHIFT = 16
C_PATH = "launcher/main/render/a.c"
PY_PATH = "scripts/tool.py"


class MagicTests(unittest.TestCase):
    def scan(self, files):
        with temporary_tree(files.items()) as root:
            commit(root, ".")
            return gate.scan(root, "HEAD")

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
                  gate.RARE_MINIMUM + gate.ROUND_MULTIPLE, gate.POWER_MINIMUM, gate.POWER_MINIMUM - 1)
        for value, expected in zip(values, (0, 0, 0, 1, 1)):
            with self.subTest(value=value):
                self.assertEqual(len(self.restates(f"#define VALUE {value}\nint x = {value};")), expected)
        value = gate.RARE_MINIMUM + 1
        self.assertEqual(len(self.restates(f"#define VALUE {value}\nint x = {value};")), 1)

    def test_shift_operands_and_shift_expression(self):
        shift = gate.POWER_MINIMUM.bit_length() - 1
        code = f"#define VALUE_SHIFT {shift}\nint x = value >> {shift};\nx <<= {shift};"
        self.assertEqual(len(self.restates(code)), 2)
        self.assertFalse(self.restates(f"#define VALUE {shift}\nint x = value >> {shift};"))
        self.assertFalse(self.restates(f"#define VALUE_SHIFT {shift}\nint x = 1 << {shift};"))

    def test_exempt_contexts_and_folding(self):
        shift = gate.POWER_MINIMUM.bit_length() - 1
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
        self.assertEqual(len(hits[PY_PATH, gate.PROTOCOL]), 1)

    def check_change(self, before, after, expected, rename=False):
        with temporary_tree(before.items()) as root:
            base = commit(root, ".")
            if rename:
                gate.git(root, "mv", C_PATH, "launcher/main/render/renamed.c")
            for path, text in after.items():
                write(root, path, text)
            commit(root, ".")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = gate.main(["--base", base], root=root)
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
        self.check_change({C_PATH: code, header: ""}, {header: f"#define VALUE {RARE}"}, 1)
        self.check_change({C_PATH: "", PY_PATH: 'x = "FRAME_READY"'},
                          {C_PATH: 'char *x = "FRAME_READY";'}, 1)

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
