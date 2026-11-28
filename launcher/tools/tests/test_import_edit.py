"""import_edit: an import file's tables as written, each key with its line, and whether import_settings accepts it."""

import contextlib
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import tomllib
import unittest
from unittest import mock

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from r3d import import_edit, import_settings  # noqa: E402

REPO = TOOLS.parents[1]
BISTRO = TOOLS.parent / "demo" / "bistro" / "bistro.import.toml"

# Every kind of table: a key before any header, header tables, a value and a string over several lines (the string
# holding lines that read as a header and a key), dotted keys, inline tables one inside another, and an array of
# tables whose items hold an inline table and a sub-table of their own.
KINDS = '''\
# An import with every kind of table.
note = "root"
[source]
path = "m.obj"
credit = """Someone,
[fake]
x = 1"""

[materials]
double_sided = [
    "a",  # front and back
    "b",
]

[geometry]
alpha_mask.keep_alpha = 0.5
simplify = { dense_edge = 2.0, seal = { seams = true } }

[[objects.meshes]]
match = { material = "a" }
triangles = 10

[[objects.meshes]]
triangles = 5
[objects.meshes.lod]
levels = 2
'''

KINDS_TABLES = {
    "keys": [{"key": "note", "value": "root", "line": 2}],
    "tables": [
        {"name": "source", "line": 3, "keys": [
            {"key": "path", "value": "m.obj", "line": 4},
            {"key": "credit", "value": "Someone,\n[fake]\nx = 1", "line": 5}]},
        {"name": "materials", "line": 9, "keys": [{"key": "double_sided", "value": ["a", "b"], "line": 10}]},
        {"name": "geometry", "line": 15, "keys": []},
        {"name": "geometry.alpha_mask", "line": 16, "keys": [{"key": "keep_alpha", "value": 0.5, "line": 16}]},
        {"name": "geometry.simplify", "line": 17, "keys": [{"key": "dense_edge", "value": 2.0, "line": 17}]},
        {"name": "geometry.simplify.seal", "line": 17, "keys": [{"key": "seams", "value": True, "line": 17}]},
        {"name": "objects.meshes", "line": 19, "array": True, "items": [
            {"line": 19, "keys": [{"key": "triangles", "value": 10, "line": 21}],
             "tables": [{"name": "match", "line": 20, "keys": [{"key": "material", "value": "a", "line": 20}]}]},
            {"line": 23, "keys": [{"key": "triangles", "value": 5, "line": 24}],
             "tables": [{"name": "lod", "line": 25, "keys": [{"key": "levels", "value": 2, "line": 26}]}]}]},
    ],
}

# The lexer's other paths: literal strings on one line and several, an escaped quote and a # inside a string, a
# quoted and spaced header with a comment after it, a quoted key, a string closed by four quotes, a sub-table named
# before its parent, and no line end after the last value.
LEXER = """\
a = 'x # y'
b = \'\'\'one
two\'\'\'
c = "say \\"hi\\" # not a comment"
[ "s t" . u ]  # comment
"q k" = 1
d = \"\"\"ends with a quote\"\"\"\"
[p.q]
r = 2
[p]
s = 3"""

LEXER_TABLES = {
    "keys": [{"key": "a", "value": "x # y", "line": 1}, {"key": "b", "value": "one\ntwo", "line": 2},
             {"key": "c", "value": 'say "hi" # not a comment', "line": 4}],
    "tables": [
        {"name": "s t.u", "line": 5, "keys": [{"key": "q k", "value": 1, "line": 6},
                                              {"key": "d", "value": 'ends with a quote"', "line": 7}]},
        {"name": "p.q", "line": 8, "keys": [{"key": "r", "value": 2, "line": 9}]},
        {"name": "p", "line": 10, "keys": [{"key": "s", "value": 3, "line": 11}]},
    ],
}

# Arrays of tables whose sub-tables and inner arrays sit in an item before the last.
EARLY_ITEMS = """\
[[variants]]
name = "near"
[variants.lod]
levels = 2
[[variants.steps]]
k = 1
[[variants.steps]]
k = 2
[[variants]]
name = "far"
[[variants.steps]]
k = 3
"""

EARLY_ITEMS_TABLES = [{"name": "variants", "line": 1, "array": True, "items": [
    {"line": 1, "keys": [{"key": "name", "value": "near", "line": 2}], "tables": [
        {"name": "lod", "line": 3, "keys": [{"key": "levels", "value": 2, "line": 4}]},
        {"name": "steps", "line": 5, "array": True, "items": [
            {"line": 5, "keys": [{"key": "k", "value": 1, "line": 6}], "tables": []},
            {"line": 7, "keys": [{"key": "k", "value": 2, "line": 8}], "tables": []}]}]},
    {"line": 9, "keys": [{"key": "name", "value": "far", "line": 10}], "tables": [
        {"name": "steps", "line": 11, "array": True, "items": [
            {"line": 11, "keys": [{"key": "k", "value": 3, "line": 12}], "tables": []}]}]}]}]

SOURCE = '[source]\npath = "m.obj"\ncredit = "test"\n\n[output]\nname = "m"\n'
# geometry.probes shapes a fit, so it needs geometry.simplify, which this file lacks.
PROBES_WITHOUT_SIMPLIFY = SOURCE + "\n[geometry]\nprobes = {}\n"


def run(*argv):
    """import_edit's exit code, stdout and stderr for `argv`."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = import_edit.main([str(arg) for arg in argv])
    return code, out.getvalue(), err.getvalue()


def read(path):
    """read's JSON for the import at `path`, refused unless it exits 0 with nothing on stderr."""
    code, out, err = run("read", path)
    if code != 0 or err:
        raise AssertionError(f"read {path}: exit {code}, stderr {err!r}")
    return json.loads(out)


def every_key(container, prefix=()):
    """(path, key, value, line) of every key in a read container; an item's path holds its index after its array's
    name."""
    for key in container["keys"]:
        yield prefix, key["key"], key["value"], key["line"]
    for table in container["tables"]:
        name = prefix + tuple(table["name"].split("."))
        if table.get("array"):
            for index, item in enumerate(table["items"]):
                yield from every_key(item, name + (index,))
        else:
            yield from every_key(dict(keys=table["keys"], tables=[]), name)


def tracked_imports():
    """Every .import.toml in the tree."""
    listed = subprocess.run(["git", "ls-files", "*.import.toml"], cwd=REPO, capture_output=True, text=True,
                            check=True).stdout.split()
    return [REPO / name for name in listed]


class ImportEditTests(unittest.TestCase):
    def fixture(self, text, name="m.import.toml"):
        """`text` written as an import file in a folder of its own."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = pathlib.Path(directory.name) / name
        path.write_text(text, encoding="utf-8", newline="")
        return path

    def test_every_kind_of_table_is_listed_in_file_order_with_its_lines(self):
        result = read(self.fixture(KINDS))
        self.assertEqual({"keys": result["keys"], "tables": result["tables"]}, KINDS_TABLES)

    def test_the_lexer_finds_lines_through_every_string_and_header_form(self):
        result = read(self.fixture(LEXER))
        self.assertEqual({"keys": result["keys"], "tables": result["tables"]}, LEXER_TABLES)

    def test_sub_tables_and_inner_arrays_of_an_item_before_the_last_are_listed_in_it(self):
        self.assertEqual(read(self.fixture(EARLY_ITEMS))["tables"], EARLY_ITEMS_TABLES)

    def test_lines_that_do_not_hold_what_the_file_does_are_refused(self):
        path = self.fixture(KINDS)
        found = import_edit.statements(KINDS)
        wrong_path = [(line, kind, ["nowhere"] if kind == "key" else parts) for line, kind, parts in found]
        wrong_line = [(line + 1, kind, parts) for line, kind, parts in found]
        for name, statements in (("path", wrong_path), ("line", wrong_line)):
            with self.subTest(wrong=name), mock.patch.object(import_edit, "statements", return_value=statements):
                code, out, err = run("read", path)
                self.assertEqual((code, out), (1, ""))
                self.assertIn("lines", err)

    def test_crlf_line_ends_give_the_same_lines(self):
        result = read(self.fixture(KINDS.replace("\n", "\r\n")))
        self.assertEqual(result["tables"], KINDS_TABLES["tables"])

    def test_a_demo_import_is_valid_and_every_value_sits_on_its_line(self):
        result = read(BISTRO)
        self.assertEqual((result["file"], result["valid"], result["error"]), (str(BISTRO), True, None))
        lines = BISTRO.read_text(encoding="utf-8").splitlines()
        values = tomllib.loads(BISTRO.read_text(encoding="utf-8"))
        names = [table["name"] for table in result["tables"]]
        self.assertEqual(names[:5], ["source", "output", "materials", "process", "geometry"])
        self.assertIn("geometry.simplify", names)
        self.assertEqual(len(result["tables"][-1]["items"]), len(values["variants"]))
        for path, key, value, line in every_key(result):
            self.assertRegex(lines[line - 1], rf"^\s*([\w-]+\.)*{key}\s*=|[{{,]\s*{key}\s*=", f"{key} at line {line}")
            node = values
            for part in path:
                node = node[part]
            self.assertEqual(node[key], value)

    def test_every_tracked_import_reads_valid(self):
        for path in tracked_imports():
            with self.subTest(path=path.relative_to(REPO).as_posix()):
                result = read(path)
                self.assertEqual((result["valid"], result["error"]), (True, None))

    def test_an_invalid_import_reports_the_settings_error_and_still_lists_its_tables(self):
        path = self.fixture(PROBES_WITHOUT_SIMPLIFY)
        with self.assertRaises(import_settings.SettingsError) as refusal:
            import_settings.load_import_settings(path)
        self.assertIn("geometry.simplify", str(refusal.exception))
        result = read(path)
        self.assertEqual((result["valid"], result["error"]), (False, str(refusal.exception)))
        self.assertEqual([table["name"] for table in result["tables"]],
                         ["source", "output", "geometry", "geometry.probes"])
        self.assertEqual(result["tables"][-1], {"name": "geometry.probes", "line": 9, "keys": []})

    def test_a_file_that_is_not_toml_is_invalid_with_no_tables(self):
        path = self.fixture(SOURCE + "[geometry\n")
        with self.assertRaises(tomllib.TOMLDecodeError) as refusal:
            tomllib.loads(path.read_text(encoding="utf-8"))
        result = read(path)
        self.assertEqual(result, {"file": str(path), "valid": False, "error": str(refusal.exception),
                                  "keys": [], "tables": []})

    def test_out_writes_the_json_read_prints(self):
        path = self.fixture(KINDS)
        printed = run("read", path)[1]
        for flag in ("--out", "-o"):
            with self.subTest(flag=flag):
                out = path.parent / f"read{flag}.json"
                code, stdout, err = run("read", path, flag, out)
                self.assertEqual((code, stdout, err), (0, "", ""))
                self.assertEqual(out.read_text(encoding="utf-8"), printed)

    def test_a_file_that_is_not_an_import_is_refused(self):
        code, out, err = run("read", self.fixture(SOURCE, "m.scene.toml"))
        self.assertEqual((code, out), (1, ""))
        self.assertIn(".import.toml", err)

    def test_a_missing_import_is_refused(self):
        code, out, err = run("read", pathlib.Path(tempfile.gettempdir()) / "absent" / "m.import.toml")
        self.assertEqual((code, out), (1, ""))
        self.assertTrue(err.startswith("import_edit: "))


if __name__ == "__main__":
    unittest.main()
