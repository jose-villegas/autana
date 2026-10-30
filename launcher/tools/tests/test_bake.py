"""Cached bakes (bake/bake.py): what a key counts and ignores, how stages chain,
the lock file, and that packs built from the cache are the tree's packs byte
for byte. build_pack.py's tree path has its own suite, test_asset_pack.py."""

import contextlib
import copy
import dataclasses
import hashlib
import io
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time
import types
import unittest
import urllib.error
import zipfile
from unittest import mock

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "tests"))

from bake import bake, produce  # noqa: E402
from r3d import build_pack  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402
from test_r3d_import import (FIT, HEAD, PATH, SIMPLIFY, VARIANT, VARIANT_OUTPUT, camera, renderer, sun_object,  # noqa: E402
                             write_import, write_scene)

TOOL_KEYS = {stage: stage * 8 for stage in bake.STAGES}
START_SHA256 = "5" * 64
# The bytes a lock row and a run's make give one start, told apart by their first digit.
LOCKED_SHA256 = "1" * 64
MADE_SHA256 = "2" * 64
SEEDED_SHA256 = "3" * 64
FIT_SHA256 = "4" * 64
RUN = 9
FIT_STEPS = 20  # the steps FIT sets


class AnyStart(dict):
    """Starts that all have the bytes `sha256`, whatever their key."""

    def __init__(self, sha256=START_SHA256):
        super().__init__()
        self.sha256 = sha256

    def get(self, key, default=None):
        return self.sha256


def fitted_job():
    """A fitted renderer of the tree and its scene."""
    for path in sorted(build_pack.input_files([build_pack.DEFAULT_SEARCH])):
        if path.name.endswith(build_pack.SCENE):
            scene = load_scene(path)
            for job in scene.renderers:
                if job.renderer.fit is not None:
                    return job, scene
    raise unittest.SkipTest("no fitted renderer in the tree")


def bake_of(key, tree, kind="mesh"):
    return bake.Bake(output="one.mesh", source=tree, holder="one", kind=kind, key=key, suffix=bake.MESH_SUFFIX,
                     tree=tree)


def mesh_process_stub(item, cache, lock=None, blender=None):
    if item.holder == "fail":
        raise bake.BakeMissing("mesh worker failed")
    if item.holder == "settings":
        raise bake.SettingsError("mesh settings failed")
    cache = pathlib.Path(cache)
    if item.holder == "wait":
        deadline = time.monotonic() + 15
        while not (cache / "later.done").exists():
            if time.monotonic() >= deadline:
                raise bake.BakeMissing("later mesh did not finish first")
            time.sleep(0.01)
    payload = cache / f"{item.key}.payload"
    with payload.open("x" if item.holder == "once" else "w") as stream:
        stream.write(str(os.getpid()))
    row = produce.record(item, payload, cache)
    (cache / f"{item.holder}.done").touch()
    return row


class MeshCommandTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)
        self.found = [dataclasses.replace(bake_of(str(index), self.root),
                                         output=f"{index}.mesh", holder=str(index)) for index in range(4)]

    def command(self, capacity=2, again=True, memory_error=False):
        from r3d import process_budget as budget
        out, err = io.StringIO(), io.StringIO()
        rows = []
        free = tuple(floor + capacity * cost for floor, cost in zip(budget.FLOORS, budget.BAKE_BYTES))
        with mock.patch.object(bake, "bakes", return_value=self.found), \
                mock.patch.object(bake, "read_lock_tables", return_value={"bake": {}, "poses": {}}), \
                mock.patch.object(budget, "available_bytes", side_effect=OSError("no procfs") if memory_error else None,
                                  return_value=free), \
                mock.patch.object(budget, "cores_available", return_value=4), \
                mock.patch.object(bake, "cpu_vendor", return_value=None), \
                mock.patch.object(produce, "produce", mesh_process_stub), \
                mock.patch.object(produce, "export", side_effect=lambda row, *_: rows.append(row)), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = bake.main(["bake", "--kind", "mesh", "--cache", str(self.root), "--out", str(self.root / "out"),
                              *(["--again"] if again else [])])
        return types.SimpleNamespace(code=code, out=out.getvalue(), err=err.getvalue(), rows=rows)

    def test_parallel_processes_preserve_found_and_printed_order(self):
        self.found[0] = dataclasses.replace(self.found[0], holder="wait")
        self.found[1] = dataclasses.replace(self.found[1], holder="later")
        done = self.command()
        self.assertEqual(done.code, 0, done.err)
        self.assertEqual(sorted(row["output"] for row in done.rows), sorted(item.output for item in self.found))
        self.assertEqual([line.split()[1] for line in done.out.splitlines() if line.startswith("made ")],
                         [item.output for item in self.found])
        pids = {bake.cached(row, row["suffix"], self.root).read_text() for row in done.rows}
        self.assertGreater(len(pids), 1)
        self.assertNotIn(str(os.getpid()), pids)
        self.assertIn("mesh workers: 2 (memory", done.out)

    def test_capacity_one_runs_serially(self):
        done = self.command(capacity=1)
        self.assertEqual(done.code, 0, done.err)
        self.assertEqual({bake.cached(row, row["suffix"], self.root).read_text() for row in done.rows},
                         {str(os.getpid())})
        self.assertIn("mesh workers: 1 (memory", done.out)

    def test_missing_memory_uses_one_worker(self):
        done = self.command(memory_error=True)
        self.assertEqual(done.code, 0, done.err)
        self.assertIn("mesh workers: 1 (memory unavailable", done.out)

    def test_core_limit_is_reported(self):
        done = self.command(capacity=10)
        self.assertEqual(done.code, 0, done.err)
        self.assertIn("mesh workers: 4 (cores", done.out)

    def test_worker_errors_preserve_command_messages(self):
        for holder, message in (("fail", "mesh worker failed"), ("settings", "mesh settings failed")):
            with self.subTest(holder=holder):
                self.found[0] = dataclasses.replace(self.found[0], holder=holder)
                done = self.command()
                self.assertEqual(done.code, 1)
                self.assertIn(f"bake.py: {message}", done.err)
                self.assertNotIn(self.found[0].output, [row["output"] for row in done.rows])

    def test_a_failed_worker_stops_new_meshes_and_every_finished_one_is_exported(self):
        self.found[2] = dataclasses.replace(self.found[2], holder="fail")
        for capacity in (1, 2):
            with self.subTest(capacity=capacity):
                done = self.command(capacity=capacity)
                self.assertEqual(done.code, 1)
                exported = sorted(row["output"] for row in done.rows)
                made = {path.stem for path in self.root.glob("*.done")}
                self.assertEqual(exported, sorted(item.output for item in self.found if item.holder in made))
                self.assertIn(self.found[0].output, exported)
                if capacity == 1:
                    self.assertEqual(exported, [item.output for item in self.found[:2]])
                for path in self.root.glob("*.done"):
                    path.unlink()

    def test_duplicate_keys_are_produced_once_even_with_again(self):
        self.found[0] = dataclasses.replace(self.found[0], holder="once")
        self.found = [self.found[0], self.found[1], self.found[0]]
        done = self.command()
        self.assertEqual(done.code, 0, done.err)
        self.assertEqual(sorted(row["output"] for row in done.rows), sorted({item.output for item in self.found}))
        self.assertEqual(len(list(self.root.glob("*.payload"))), 2)

    def test_cached_rows_need_no_workers(self):
        self.assertEqual(self.command(capacity=1).code, 0)
        payloads = {path: path.stat().st_mtime_ns for path in self.root.glob("*.payload")}
        done = self.command(again=False)
        self.assertEqual({path: path.stat().st_mtime_ns for path in payloads}, payloads)
        self.assertEqual(done.code, 0, done.err)
        self.assertNotIn("mesh workers:", done.out)


class TokenTests(unittest.TestCase):
    def tokens(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "m.py"
            path.write_text(text, encoding="utf-8")
            return bake.code_tokens(path)

    def test_comments_blank_lines_and_indent_width_do_not_change_the_code(self):
        code = self.tokens("def f(x):\n    return x + 1\n")
        self.assertEqual(code, self.tokens("# what f is\ndef f(x):  # one more\n\n\n  return x  +  1\n"))

    def test_string_statements_do_not_change_the_code(self):
        cases = (
            ('x = 1\n', '"""Module."""\nx = 1\n', '"""Changed."""\nx = 1\n'),
            ('class C:\n    x = 1\n', 'class C:\n    """Class."""\n    x = 1\n',
             'class C:\n    """Changed."""\n    x = 1\n'),
            ('def f():\n    return 1\n', 'def f():\n    """Function."""\n    return 1\n',
             'def f():\n    """Changed."""\n    return 1\n'),
            ('async def f():\n    return 1\n', 'async def f():\n    """Async."""\n    return 1\n',
             'async def f():\n    """Changed."""\n    return 1\n'),
            ('x = 1\n', 'x = 1\n"Bare string."\n', 'x = 1\n"Changed."\n'),
            ('class E(Exception): """x"""\n', 'class E(Exception): """y"""\n',
             'class E(Exception): """Changed."""\n'),
            ('def f():\n    return 1\n', 'def f():\n    (\n        "Multiline"\n        " string"\n    )\n    return 1\n',
             'def f():\n    (\n        "Changed"\n        " string"\n    )\n    return 1\n'),
        )
        for plain, documented, changed in cases:
            with self.subTest(source=documented):
                self.assertEqual(self.tokens(plain), self.tokens(documented))
                self.assertEqual(self.tokens(documented), self.tokens(changed))

    def test_statement_boundaries_change_the_code(self):
        self.assertNotEqual(self.tokens("f\n(x)\n"), self.tokens("f(x)\n"))

    def test_non_string_constant_statements_stay_in_the_code(self):
        for expression in ('b"x"', '0', '...'):
            with self.subTest(expression=expression):
                self.assertNotEqual(self.tokens("x = 1\n"), self.tokens(expression + "\nx = 1\n"))

    def test_changed_expressions_and_expression_strings_change_the_code(self):
        for plain, edited in (('x = 1\n', 'x = 2\n'), ('f("a")\n', 'f("b")\n'),
                              ('x = f"a"\n', 'x = f"b"\n'), ('x = b"a"\n', 'x = b"b"\n')):
            with self.subTest(source=plain):
                self.assertNotEqual(self.tokens(plain), self.tokens(edited))

    def test_a_changed_constant_name_or_string_does(self):
        code = self.tokens('SCALE = 8\nNAME = "a"\nf(SCALE)\n')
        for edit in ('SCALE = 9\nNAME = "a"\nf(SCALE)\n', 'SCALE = 8\nNAME = "b"\nf(SCALE)\n',
                     'SCALE = 8\nNAME = "a"\ng(SCALE)\n'):
            self.assertNotEqual(code, self.tokens(edit), edit)

    def test_imports_and_sys_path_edits_are_where_code_lives_not_what_it_computes(self):
        code = self.tokens('import sys\nsys.path.insert(0, "a")\nfrom pkg.helper import f\nprint(f(1))\n')
        moved = self.tokens('import sys\nsys.path.insert(0, "b/c")\nfrom other.place.helper import (\n    f)\nprint(f(1))\n')
        self.assertEqual(code, moved)
        self.assertNotEqual(code, self.tokens('import sys\nfrom pkg.helper import f\nprint(f(2))\n'))


class ContentKeyTests(unittest.TestCase):
    def test_a_c_file_keys_without_its_include_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "a.c").write_bytes(b'#include "util/scalar/mathf.h"\nint f(void) { return 1; }\n')
            (root / "b.c").write_bytes(b'  #  include "core/mathf.h"\n#include <stdint.h>\nint f(void) { return 1; }\n')
            (root / "c.c").write_bytes(b'#include "core/mathf.h"\nint f(void) { return 2; }\n')
            self.assertEqual(bake.c_content(root / "a.c"), bake.c_content(root / "b.c"))
            self.assertNotEqual(bake.c_content(root / "a.c"), bake.c_content(root / "c.c"))

    def content(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "scratch.c"
            path.write_bytes(text)
            return bake.c_content(path)

    def test_c_comments_layout_code_and_literals(self):
        cases = (
            (b'int a; /* x */\n', b'int a; /* y\n z */\n'),
            (b'int a; // x\n', b'int a; // y\n'),
            (b'int a; // x\\\n continued\nint b;\n', b'int a; // y\\\n changed\nint b;\n'),
            (b'#define A 1 /* x */\n', b'#define A 1 /* y */\n'),
            (b'int a;\n', b'// added\n\n  int  a;\t\n/* more */\n'),
            (b'a/**/b\n', b'a b\n'),
            (b"#error don't\n/* x */\nint a;\n", b"#error don't\n/* y */\nint a;\n"),
            (b"c = '/'; q = '\"'; /* x */\n", b"c = '/'; q = '\"'; /* y */\n"),
            (b"c = '*'; q = '\"'; // x\n", b"c = '*'; q = '\"'; // y\n"),
        )
        changed = (
            (b'char *s = "/* a */";\n', b'char *s = "/* b */";\n'),
            (b'char *s = "// a";\n', b'char *s = "// b";\n'),
            (b'int a;\n', b'int b;\n'),
            (b'#define A 1\nint x;\n', b'#define A 1 int x;\n'),
            (b'char *s = "a  b";\n', b'char *s = "a b";\n'),
            (b"c = '/'/* x */;\n", b"c = '-'/* x */;\n"),
            (b"int n = 1'000; int x;\n", b"int n = 1'000; int y;\n"),
            (b'#define A 1\\\n + 2\n', b'#define A 1 + 2\n'),
        )
        for equal, pairs in ((True, cases), (False, changed)):
            for plain, edited in pairs:
                with self.subTest(source=plain):
                    self.assertEqual(equal, self.content(plain) == self.content(edited))

    def test_c_quote_types_and_escapes_preserve_literal_bytes(self):
        for plain, edited in (
            (br'"a\" /* p */ b"', br'"a\" /* q */ b"'),
            (b'"it\'s /* p */"', b'"it\'s /* q */"'),
        ):
            with self.subTest(source=plain):
                self.assertNotEqual(self.content(plain), self.content(edited))

    def test_c_spliced_literal_comment_bytes_change_key(self):
        plain = b'"p\\\n/* p */";\n/* r */\n'
        edited = plain.replace(b"/* p */", b"/* q */")
        self.assertNotEqual(self.content(plain), self.content(edited))

    def test_c_literal_at_line_start_retains_newline(self):
        plain = b'#define M \\\n"a"\nint x;\n'
        joined = b'#define M \\\n"a" int x;\n'
        self.assertNotEqual(self.content(plain), self.content(joined))

    def test_c_char_quotes_do_not_hide_comments_or_next_line_code(self):
        for literal in (b"'\"'", br"'\''", b"'/'", b"'*'"):
            with self.subTest(literal=literal):
                plain = b"c = " + literal + b"; /* p */\nint a;\n"
                edited = b"c = " + literal + b"; /* q */\nint a;\n"
                code = b"c = " + literal + b"; /* q */\nint b;\n"
                self.assertEqual(self.content(plain), self.content(edited))
                self.assertNotEqual(self.content(plain), self.content(code))

    def test_c_line_boundaries_count(self):
        self.assertNotEqual(self.content(b"#define A 1\n  int x;\n"),
                            self.content(b"#define A 1 int x;\n"))

    def test_c_nonfinal_line_trailing_comments_and_spaces_do_not_count(self):
        plain = self.content(b"int a;\nint b;\n")
        for edited in (b"int a; // x\nint b;\n", b"int a;  \t\nint b;\n"):
            with self.subTest(source=edited):
                self.assertEqual(plain, self.content(edited))

    def test_c_later_line_indentation_and_leading_comments_do_not_count(self):
        plain = self.content(b"int a;\nint b;\n")
        for edited in (b"int a;\n  int b;\n", b"int a;\n/* x */int b;\n"):
            with self.subTest(source=edited):
                self.assertEqual(plain, self.content(edited))

    def test_c_non_utf8_bytes_round_trip_and_carriage_returns_count(self):
        text = b'char *s = "\xff";\n'
        self.assertEqual(self.content(text), hashlib.sha256(text).hexdigest())
        self.assertNotEqual(self.content(b"int a;\r\n"), self.content(b"int a;\n"))

    def test_scratch_stage_keys_ignore_documentation_and_count_each_files_code(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            entry, source, header = (root / name for name in ("entry.py", "scratch.c", "scratch.h"))
            originals = {
                entry: 'import pathlib\nSOURCE = pathlib.Path(__file__).parent / "scratch.c"\nVALUE = 1\n',
                source: '#include "scratch.h"\nint x = 1;\n',
                header: '#define A 1\n',
            }

            def key():
                bake.module_facts.cache_clear()
                paths = bake.closure([entry])
                contents = [bake.digest(bake.code_tokens(path)) for path in paths]
                contents.extend(bake.native_inputs(paths).values())
                # Mirrors tool_digest's combination of code, versions and references.
                return bake.digest([sorted(contents), {}, []])

            try:
                for path, text in originals.items():
                    path.write_bytes(text.encode())
                original = key()
                for path, text in originals.items():
                    with self.subTest(file=path.name):
                        documentation = '"""Documentation."""\n# comment\n' if path == entry else '/* documentation */\n// comment\n'
                        path.write_bytes((documentation + text).encode())
                        self.assertEqual(original, key())
                        path.write_bytes(text.replace('1', '2').encode())
                        self.assertNotEqual(original, key())
                        path.write_bytes(text.encode())
            finally:
                bake.module_facts.cache_clear()

    def test_a_moved_module_keys_the_same_and_an_edited_one_does_not(self):
        def key(layout, value="1"):
            with tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                helper, line = layout
                (root / helper).parent.mkdir(parents=True, exist_ok=True)
                (root / helper).write_text(f"VALUE = {value}\n")
                (root / "entry.py").write_text(f"import pathlib\nimport sys\n{line}\nprint(VALUE)\n")
                bake.module_facts.cache_clear()
                return sorted(bake.digest(bake.code_tokens(path)) for path in bake.closure([root / "entry.py"]))

        here = ("helpers/values.py", 'sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "helpers"))\n'
                                     "from values import VALUE")
        there = ("lib/deep/values.py", 'sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "lib" / "deep"))\n'
                                       "from values import VALUE")
        try:
            self.assertEqual(key(here), key(there))
            self.assertNotEqual(key(here), key(here, value="2"))
        finally:
            bake.module_facts.cache_clear()


class NativeTests(unittest.TestCase):
    def test_c_includes_follows_quoted_headers_beside_the_file_and_in_the_include_dirs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "inc" / "sub").mkdir(parents=True)
            (root / "a.c").write_text('#include "near.h"\n#include "sub/far.h"\n#include <stdio.h>\n')
            (root / "near.h").write_text("")
            (root / "inc" / "sub" / "far.h").write_text('  #  include "deep.h"\n')
            (root / "inc" / "sub" / "deep.h").write_text("")
            found = {path.relative_to(root.resolve()).as_posix() for path in bake.c_includes([root / "a.c"], (root / "inc",))}
        self.assertEqual(found, {"a.c", "near.h", "inc/sub/far.h", "inc/sub/deep.h"})

    def test_define_values_are_keyed_without_compiling_their_include_graph(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            header = root / "owner.h"
            included = root / "runtime.h"
            included.write_text("#define UNUSED 1\n")
            header.write_text('#include "runtime.h"\n#define GAMMA 2.2F\n')
            module = root / "entry.py"
            module.write_text('from pathlib import Path\nfrom build.c_header import read_define\n'
                              'OWNER = Path(__file__).with_name("owner.h")\n'
                              'GAMMA = read_define(OWNER, "GAMMA")\n')
            try:
                before = bake.native_inputs([module])
                self.assertEqual(set(before), {bake.relative(header)})
                included.write_text("#define UNUSED 2\n")
                self.assertEqual(bake.native_inputs([module]), before)
                header.write_text('#include "runtime.h"\n#define GAMMA 2.4F\n')
                self.assertNotEqual(bake.native_inputs([module]), before)
            finally:
                bake.module_facts.cache_clear()

    def test_all_literal_defines_key_even_comprehension_reads(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            header, module = root / "owner.h", root / "entry.py"
            header.write_text('#define MODE 1U\n#define OTHER 2U\n')
            module.write_text('from pathlib import Path\nOWNER = Path(__file__).with_name("owner.h")\n'
                              'MODES = {n: read_define(OWNER, n) for n in ("MODE",)}\n')
            before = bake.native_inputs([module])
            header.write_text('#define MODE 1U // unrelated comment\n#define OTHER 2U\n')
            self.assertEqual(before, bake.native_inputs([module]))
            for text in ('#define MODE 3U\n#define OTHER 2U\n', '#define MODE 1U\n#define OTHER 4U\n'):
                header.write_text(text)
                self.assertNotEqual(before, bake.native_inputs([module]))

    def test_unresolvable_read_define_header_is_loud(self):
        with tempfile.TemporaryDirectory() as directory:
            module = pathlib.Path(directory) / "entry.py"
            module.write_text('VALUE = read_define(unknown_header, "MODE")\n')
            with self.assertRaisesRegex(bake.SettingsError, r'entry.py:1: cannot resolve read_define header'):
                bake.native_inputs([module])
            missing = pathlib.Path(directory) / "missing.py"
            missing.write_text('from pathlib import Path\nOWNER = Path(__file__).with_name("missing.h")\n'
                               'VALUE = read_define(OWNER, "MODE")\n')
            with self.assertRaisesRegex(bake.SettingsError, r'missing.py:3: cannot resolve read_define header'):
                bake.native_inputs([missing])

    def test_the_pose_samplers_c_and_headers_are_in_the_poses_stage(self):
        names = bake.native_inputs(bake.stage_files("poses"))
        self.assertTrue({"launcher/tools/anim/track_host.c", "launcher/main/anim/anim_track.h",
                         "launcher/main/anim/anim_tracks_format.h",
                         "launcher/packages/math/include/math/scalar/mathf.h"} <= set(names))

    def test_a_compiled_submodule_counts_as_its_pinned_commit(self):
        names = bake.native_inputs(bake.stage_files("mesh"))
        pinned = {bake.relative(path): commit for path, commit in bake.submodules().items()}
        for submodule in ("third_party/upstream/meshoptimizer", "third_party/upstream/ufbx"):
            self.assertEqual(names[submodule], pinned[submodule])
        self.assertIn("launcher/tools/fbx/ufbx_glue.c", names)


class ClosureTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name).resolve()
        (self.root / "helpers").mkdir()
        (self.root / "helpers" / "packing.py").write_text("STEP = 8\n")
        bake.module_facts.cache_clear()

    def tearDown(self):
        bake.module_facts.cache_clear()
        self.directory.cleanup()

    def entry(self, text):
        path = self.root / "entry.py"
        path.write_text(text)
        return path

    def test_a_module_reached_through_sys_path_is_counted(self):
        entry = self.entry('import pathlib\nimport sys\nimport numpy\n'
                           'sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "helpers"))\n'
                           'import packing\n')
        self.assertEqual(bake.closure([entry]), sorted([entry, self.root / "helpers" / "packing.py"]))

    def test_a_tracked_module_no_root_holds_fails_naming_it(self):
        entry = self.entry("import gfx_color\n")
        with self.assertRaisesRegex(bake.SettingsError, "gfx_color in .*entry.py"):
            bake.closure([entry])

    def test_a_module_that_imports_this_tool_is_not_followed(self):
        (self.root / "helpers" / "consumer.py").write_text("from bake import bake\n")
        entry = self.entry('import pathlib\nimport sys\n'
                           'sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "helpers"))\n'
                           'import consumer\n')
        self.assertEqual(bake.closure([entry]), [entry])

    def test_consumers_are_followed_when_asked_and_a_from_pathlib_root_is_read(self):
        (self.root / "helpers" / "consumer.py").write_text("from bake import bake\nimport packing\n")
        entry = self.entry('from pathlib import Path\nimport sys\n'
                           'sys.path.insert(0, str(Path(__file__).resolve().parent / "helpers"))\n'
                           'import consumer\n')
        helpers = self.root / "helpers"
        self.assertEqual(bake.closure([entry], consumers=True),
                         sorted([entry, helpers / "consumer.py", helpers / "packing.py"]))


class StageTests(unittest.TestCase):
    def names(self, stage):
        return {path.relative_to(TOOLS).as_posix() for path in bake.stage_files(stage)}

    def test_a_stage_counts_its_entries_and_what_they_import(self):
        self.assertTrue({"r3d/mesh_import.py", "r3d/light.py", "r3d/__init__.py"} <= self.names("mesh"))
        self.assertTrue({"r3d/appearance_simplify.py", "r3d/fitted_variant.py"} <= self.names("fit"))

    def test_modules_reached_through_sys_path_are_in_the_stages_that_import_them(self):
        for stage in ("mesh", "reference", "fit"):
            self.assertIn("device/gfx_color.py", self.names(stage), stage)
        for stage in ("reference", "fit"):
            self.assertIn("render/colour_maths.py", self.names(stage), stage)

    def test_the_render_comparison_tool_is_in_no_stage(self):
        for stage in bake.STAGES:
            self.assertNotIn("render/render_compare.py", self.names(stage), stage)

    def test_a_stage_stops_at_another_stages_entry_and_at_what_consumes_bakes(self):
        self.assertFalse({"r3d/fitted_variant.py", "r3d/appearance_simplify.py", "r3d/reference_render.py"}
                         & self.names("mesh"))
        self.assertNotIn("r3d/appearance_simplify.py", self.names("reference"))
        self.assertNotIn("r3d/mesh_import.py", self.names("fit"))
        for stage in bake.STAGES:
            self.assertFalse(any(name.startswith("bake/") for name in self.names(stage)), stage)
            self.assertNotIn("r3d/build_pack.py", self.names(stage), stage)

    def test_a_fit_edit_rekeys_the_fit_alone(self):
        job, scene = fitted_job()
        keys = bake.stage_keys(job, scene, TOOL_KEYS, AnyStart())
        fit = copy.deepcopy(job)
        fit.renderer.fit.steps += 1
        refit = bake.stage_keys(fit, scene, TOOL_KEYS, AnyStart())
        self.assertEqual((keys["start"], keys["reference"]), (refit["start"], refit["reference"]))
        self.assertNotEqual(keys["fit"], refit["fit"])
        retool = bake.stage_keys(job, scene, {**TOOL_KEYS, "fit": "changed"}, AnyStart())
        self.assertEqual((keys["start"], keys["reference"]), (retool["start"], retool["reference"]))
        self.assertNotEqual(keys["fit"], retool["fit"])

    def test_the_references_do_not_read_the_start(self):
        """Fits that differ only in their start, its budget or its shading, share one reference set."""
        job, scene = fitted_job()
        keys = bake.stage_keys(job, scene, TOOL_KEYS, AnyStart())
        other = copy.deepcopy(job)
        other.renderer.variant.triangles += 1
        other.renderer.shading = "changed"
        moved = bake.stage_keys(other, scene, TOOL_KEYS, AnyStart())
        self.assertNotEqual(keys["start"], moved["start"])
        self.assertEqual(keys["reference"], moved["reference"])
        rebaked = bake.stage_keys(job, scene, {**TOOL_KEYS, "mesh": "changed"}, AnyStart())
        self.assertNotEqual(keys["start"], rebaked["start"])
        self.assertEqual((keys["reference"], keys["fit"]), (rebaked["reference"], rebaked["fit"]))

    def test_the_fit_is_keyed_on_its_starts_bytes_and_unknown_without_them(self):
        job, scene = fitted_job()
        keys = bake.stage_keys(job, scene, TOOL_KEYS, AnyStart())
        self.assertNotEqual(keys["fit"], bake.stage_keys(job, scene, TOOL_KEYS, AnyStart("f" * 64))["fit"])
        self.assertIsNone(bake.stage_keys(job, scene, TOOL_KEYS)["fit"])

    def test_the_reference_key_counts_every_field_its_render_reads(self):
        from r3d.fitted_variant import ReferenceInputs, reference_inputs

        job, scene = fitted_job()
        inputs = reference_inputs(job, scene)
        recipe = bake.reference_recipe(inputs, TOOL_KEYS, bake.Poses({}))
        fields = {field.name for field in dataclasses.fields(ReferenceInputs)}
        self.assertEqual(set(recipe) - {"sources"}, fields)
        brighter = dataclasses.replace(inputs, tonemap_white=inputs.tonemap_white * 2)
        self.assertNotEqual(bake.digest(recipe), bake.digest(bake.reference_recipe(brighter, TOOL_KEYS, bake.Poses({}))))

    def test_a_fit_brings_its_start_as_a_mesh_bake_no_pack_holds(self):
        found = bake.bakes([build_pack.DEFAULT_SEARCH])
        fits = [item for item in found if item.kind == "fit"]
        if not fits:
            self.skipTest("no fitted renderer in the tree")
        starts = {item.key: item for item in found if not item.packed and item.kind == "mesh"}
        for fit in fits:
            self.assertIn(fit.stages["start"], starts)
            self.assertTrue(starts[fit.stages["start"]].output.endswith(bake.START_SUFFIX))


def write_fitted_tree(root, names=("fit",), visibility=PATH):
    """A scene placing one fitted renderer per name, all of one import, so they share a start. Their recipes
    differ in their steps, so each fit is a bake of its own."""
    write_import(root, output=VARIANT_OUTPUT, body=SIMPLIFY + VARIANT)
    objects = "".join(
        renderer(name=name, extra='variant = "mesh"\nbake = true\n' + visibility
                 + FIT.replace(f"steps = {FIT_STEPS}", f"steps = {FIT_STEPS + index}"))
        for index, name in enumerate(names))
    return write_scene(root, objects + sun_object() + camera(path=True, region=False), HEAD)


def row_of(item, sha256, **fields):
    """The row a run or the lock holds for `item`."""
    return {"output": item.output, "source": "scene.scene.toml", "key": item.key, "sha256": sha256, "size": 1,
            "suffix": bake.MESH_SUFFIX, **fields}


class FittedBakeCase(unittest.TestCase):
    """A temporary tree with fitted renderers, and the commands and keys over it."""

    names = ("fit",)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        self.cache = self.root / "cache"
        self.scene_path = write_fitted_tree(self.root, self.names)
        self.start = next(item for item in self.bakes({}) if not item.packed)

    def tearDown(self):
        self.directory.cleanup()

    def bakes(self, starts):
        return bake.bakes([self.root], starts)

    def fits(self, sha256):
        """The fits of the tree when its start has the bytes `sha256`."""
        return [item for item in self.bakes({self.start.key: sha256}) if item.kind == "fit"]

    def fit(self, sha256):
        return self.fits(sha256)[0]

    def command(self, argv, lock, made=None, mesh_cpu_vendor=None, cpu=None):
        """Runs bake.py main over the tree with the lock (naming `mesh_cpu_vendor`), the runs' makes and this host's CPU
        vendor given and produce.produce stubbed: a start is made with MADE_SHA256, a fit with FIT_SHA256.
        Returns what it did."""
        written, produced = [], []

        def stub(item, cache, lock=None, blender=None):
            produced.append(item)
            return row_of(item, MADE_SHA256 if not item.packed else FIT_SHA256)

        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(bake, "read_lock_tables",
                               return_value={"bake": lock, "poses": {}, "mesh_cpu_vendor": mesh_cpu_vendor}), \
                mock.patch.object(bake, "write_lock", side_effect=lambda rows, **kwargs: written.append((rows, kwargs))), \
                mock.patch.object(bake, "runs_made", return_value=made or {}), \
                mock.patch.object(bake, "cpu_vendor", return_value=cpu), \
                mock.patch.object(produce, "produce", side_effect=stub), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = bake.main([*argv, str(self.root), "--cache", str(self.cache)])
        rows, kwargs = written[0] if written else ([], {})
        return types.SimpleNamespace(code=code, out=out.getvalue(), err=err.getvalue(), produced=produced,
                                     rows={row["output"]: row for row in rows}, written=bool(written),
                                     mesh_cpu_vendor=kwargs.get("mesh_cpu_vendor"))

    def locked(self, sha256, **fields):
        return {self.start.key: row_of(self.start, sha256, **fields)}


class CarvedBakeTests(FittedBakeCase):
    names = ("fit", "other")

    def test_each_carved_renderer_has_its_own_bundle_start_and_byte_keyed_fit(self):
        path = self.root / "mesh.import.toml"
        path.write_text(path.read_text() + '\n[[objects.meshes]]\nmatch = { material = "m" }\ntriangles = 4\n')
        found = self.bakes(AnyStart())
        starts = [item for item in found if not item.packed]
        fits = [item for item in found if item.kind == "fit"]
        self.assertEqual(len(starts), len(self.names))
        self.assertEqual(len({item.key for item in starts}), len(self.names))
        self.assertTrue(all(item.output.endswith('.start.bundle') for item in starts))
        self.assertTrue(all(item.output.endswith('.bundle') for item in fits))
        changed = [item for item in self.bakes(AnyStart(MADE_SHA256)) if item.kind == "fit"]
        self.assertTrue(set(item.key for item in fits).isdisjoint(item.key for item in changed))


class FittedBakeTests(FittedBakeCase):
    """The bake, lock and list commands over a tree with one fitted renderer, and the keys it gives."""

    def lock_from_run(self, lock):
        """`lock --from-run` where a run made the start with MADE_SHA256 and the fit of every start there
        could be: the rows it writes."""
        made = {self.start.key: row_of(self.start, MADE_SHA256, run=RUN)}
        for sha256 in (LOCKED_SHA256, SEEDED_SHA256, MADE_SHA256):
            made[self.fit(sha256).key] = row_of(self.fit(sha256), FIT_SHA256, run=RUN)
        return self.command(["lock", "--from-run", str(RUN)], lock, made)

    def test_a_seeded_start_is_replaced_by_the_runs_make_in_the_key_of_its_fit(self):
        done = self.lock_from_run(self.locked(SEEDED_SHA256, seeded=True))
        self.assertEqual(done.code, 0, done.err)
        self.assertEqual(done.rows[self.start.output]["sha256"], MADE_SHA256)
        self.assertEqual(done.rows[self.fit(MADE_SHA256).output]["key"], self.fit(MADE_SHA256).key)

    def test_a_made_start_in_the_lock_wins_over_the_runs_in_the_key_of_its_fit(self):
        done = self.lock_from_run(self.locked(LOCKED_SHA256, run=RUN - 1))
        self.assertEqual(done.code, 0, done.err)
        self.assertEqual(done.rows[self.start.output]["sha256"], LOCKED_SHA256)
        self.assertEqual(done.rows[self.fit(LOCKED_SHA256).output]["key"], self.fit(LOCKED_SHA256).key)

    def test_a_start_only_the_run_made_keys_its_fit_as_the_lock_writes_it(self):
        done = self.lock_from_run({})
        self.assertEqual(done.code, 0, done.err)
        self.assertEqual(done.rows[self.start.output]["sha256"], MADE_SHA256)
        self.assertEqual(done.rows[self.fit(MADE_SHA256).output]["key"], self.fit(MADE_SHA256).key)

    def test_a_run_that_made_the_start_but_not_the_fit_locks_the_start_and_fails_naming_the_fit(self):
        made = {self.start.key: row_of(self.start, MADE_SHA256, run=RUN)}
        done = self.command(["lock", "--from-run", str(RUN)], {}, made)
        self.assertEqual(list(done.rows), [self.start.output])
        self.assertEqual(done.code, 1)
        self.assertIn(self.fit(MADE_SHA256).output, done.err)

    def test_a_start_made_by_the_mesh_pass_keys_its_fit_in_the_fit_pass(self):
        done = self.command(["bake"], {})
        self.assertEqual(done.code, 0, done.err)
        self.assertEqual([item.key for item in done.produced], [self.start.key, self.fit(MADE_SHA256).key])

    def test_the_locks_start_bytes_win_over_bytes_made_again_here_in_the_key_of_its_fit(self):
        """--again makes a locked start once more, with other bytes; its fit is still keyed on the lock's."""
        done = self.command(["bake", "--again"], self.locked(LOCKED_SHA256, run=RUN))
        self.assertEqual(done.code, 0, done.err)
        self.assertIn(f"sha256 {MADE_SHA256}", done.out)
        self.assertEqual([item.key for item in done.produced], [self.start.key, self.fit(LOCKED_SHA256).key])

    def test_a_locked_start_is_not_made_and_keys_its_fit(self):
        done = self.command(["bake"], self.locked(LOCKED_SHA256, run=RUN))
        self.assertEqual([item.key for item in done.produced], [self.fit(LOCKED_SHA256).key])

    def test_a_fit_whose_start_is_neither_locked_nor_made_raises_naming_it_and_makes_nothing(self):
        fit_output = self.fit(MADE_SHA256).output
        for argv in (["bake", "--kind", "fit"], ["bake", "--only", fit_output]):
            with self.subTest(argv=argv):
                done = self.command(argv, {})
                self.assertEqual(done.code, 1)
                self.assertIn(fit_output, done.err)
                self.assertIn("is not locked", done.err)
                self.assertEqual(done.produced, [])

    def test_list_says_a_fit_waits_for_its_start_until_the_start_is_locked(self):
        waiting = self.command(["list"], {})
        lines = {line.split("\t")[0]: line for line in waiting.out.splitlines()}
        self.assertIn("waits for its start", lines[self.fit(MADE_SHA256).output])
        self.assertNotIn("waits for its start", lines[self.start.output])
        ready = self.command(["list"], self.locked(LOCKED_SHA256, run=RUN))
        line = next(line for line in ready.out.splitlines() if line.startswith(self.fit(LOCKED_SHA256).output))
        self.assertIn(self.fit(LOCKED_SHA256).key, line)

    def test_a_fits_key_is_unknown_without_its_starts_sha_and_changes_with_it(self):
        self.assertIsNone(self.fit(None).key)
        self.assertIsNotNone(self.fit(LOCKED_SHA256).key)
        self.assertNotEqual(self.fit(LOCKED_SHA256).key, self.fit(MADE_SHA256).key)

    def reference_key(self, change):
        scene = load_scene(self.scene_path)
        job = copy.deepcopy(scene.renderers[0])
        change(job)
        return bake.stage_keys(job, scene, TOOL_KEYS, AnyStart())["reference"]

    def test_each_pose_spacing_field_changes_the_reference_key(self):
        unchanged = self.reference_key(lambda job: None)
        for field in ("train_every_ms", "held_out_every_ms", "coverage_every_ms"):
            with self.subTest(field=field):
                def step(job):
                    setattr(job.renderer.fit, field, getattr(job.renderer.fit, field) + 1)

                self.assertNotEqual(self.reference_key(step), unchanged)

    def test_the_fit_pose_size_changes_the_reference_key(self):
        def widen(job):
            width, height = job.renderer.fit.size
            job.renderer.fit.size = (width + 1, height)

        self.assertNotEqual(self.reference_key(widen), self.reference_key(lambda job: None))


class PosesTests(FittedBakeCase):
    def setUp(self):
        super().setUp()
        self.scene = load_scene(self.scene_path)
        self.job = self.scene.renderers[0]

    def keys(self, rows=None):
        poses = bake.Poses({}) if rows is None else rows
        tools = {**TOOL_KEYS, "poses": bake.tool_digest("poses")}
        return bake.stage_keys(self.job, self.scene, tools, AnyStart(), poses)

    def test_sampler_closure_is_outside_consumers(self):
        excluded = {"anim/track_host.py", "anim/tracks_asset.py", "build/packages.py"}
        for stage in ("mesh", "reference", "fit"):
            files = bake.stage_files(stage)
            self.assertFalse(excluded & {path.relative_to(TOOLS).as_posix() for path in files})
            native = bake.native_inputs(files)
            self.assertFalse(any(path.startswith(("launcher/main/anim/", "launcher/main/asset/",
                                                 "launcher/packages/math/"))
                                 or path == "launcher/tools/anim/track_host.c" for path in native))

    def test_scene_writer_is_outside_consumers(self):
        for stage in ("mesh", "reference", "fit"):
            with self.subTest(stage=stage):
                files = bake.stage_files(stage)
                self.assertNotIn("r3d/scene_asset.py",
                                 {path.relative_to(TOOLS).as_posix() for path in files})

    def test_encoder_padding_changes_lock_row_key_but_keeps_stage_keys(self):
        from anim import tracks_asset

        before_rows, after_rows = bake.Poses({}), bake.Poses({})
        before = self.keys(before_rows)
        original = tracks_asset.encode
        with mock.patch.object(tracks_asset, "encode", side_effect=lambda *args, **kwargs:
                               original(*args, **kwargs) + bytes(1)):
            after = self.keys(after_rows)
        self.assertNotEqual(set(before_rows.used), set(after_rows.used))
        self.assertEqual(sorted(row["poses"] for row in before_rows.used.values()),
                         sorted(row["poses"] for row in after_rows.used.values()))
        self.assertEqual(before, after)

    def test_native_refactor_changes_lock_row_key_but_keeps_stage_keys(self):
        from anim import track_host

        before_rows, after_rows = bake.Poses({}), bake.Poses({})
        before = self.keys(before_rows)
        with tempfile.TemporaryDirectory() as directory:
            scratch = pathlib.Path(directory) / "track_host.c"
            original = TOOLS / "anim/track_host.c"
            code = original.read_text().replace("return 0;", "return (0);")
            self.assertNotEqual(code, original.read_text())
            scratch.write_text(code)
            sources = tuple(scratch if path == original else path for path in track_host.SOURCES)
            native = bake.native_inputs
            def changed(files):
                result = native(files)
                if any(path.name == "track_host.py" for path in files):
                    result[bake.relative(original)] = bake.c_content(scratch)
                return result
            with mock.patch.object(track_host, "SOURCES", sources), mock.patch.object(bake, "native_inputs", side_effect=changed):
                bake.tool_digest.cache_clear()
                try:
                    after = self.keys(after_rows)
                finally:
                    bake.tool_digest.cache_clear()
        self.assertNotEqual(set(before_rows.used), set(after_rows.used))
        self.assertEqual(sorted(row["poses"] for row in before_rows.used.values()),
                         sorted(row["poses"] for row in after_rows.used.values()))
        self.assertEqual(before, after)

    def test_real_pose_and_spacing_changes_rekey_consumers(self):
        from anim_probe import write_camera_clip

        rows = bake.Poses({})
        before = self.keys(rows)
        write_camera_clip(self.root, reach=2.0)
        moved = bake.Poses({})
        after = self.keys(moved)
        self.assertNotEqual({row["poses"] for row in rows.used.values()}, {row["poses"] for row in moved.used.values()})
        for stage in ("start", "reference", "fit"):
            self.assertNotEqual(before[stage], after[stage])
        self.job.renderer.visibility.every_ms += 1
        spaced = self.keys()
        self.assertNotEqual(after["start"], spaced["start"])
        self.job.renderer.fit.train_every_ms += 1
        trained = self.keys()
        self.assertNotEqual(spaced["reference"], trained["reference"])
        self.assertNotEqual(spaced["fit"], trained["fit"])

    def test_clip_rename_keeps_keys_and_different_clip_or_node_rekeys(self):
        from anim_probe import write_camera_clip

        before = self.keys()
        path = self.scene.camera.component.path
        renamed = path.animation.with_name("renamed.anim.toml")
        path.animation.rename(renamed)
        path.animation = renamed
        self.assertEqual(before, self.keys())
        path.animation = write_camera_clip(self.root, name="different", reach=2.0, props=("still",))
        moved = self.keys()
        self.assertTrue(all(before[stage] != moved[stage] for stage in ("start", "reference", "fit")))
        path.animation = write_camera_clip(self.root, name="node", reach=3.0, node="other")
        path.node = "other"
        still = self.keys()
        self.assertTrue(all(moved[stage] != still[stage] for stage in ("start", "reference", "fit")))

    def test_bad_clip_naming_fails_load(self):
        from anim import clip_name

        for name, value in (("SUFFIX", ".broken.toml"), ("clip_id", mock.Mock(side_effect=ValueError("broken clip id")))):
            with self.subTest(name=name), mock.patch.object(clip_name, name, value):
                with self.assertRaises((ValueError, bake.SettingsError)):
                    load_scene(self.scene_path)

    def test_lock_hit_needs_no_sampler_and_miss_names_lock(self):
        from anim import track_host

        rows = bake.Poses({})
        before = self.keys(rows)
        with mock.patch.object(track_host, "program", side_effect=AssertionError("compiler unavailable")):
            self.assertEqual(before, self.keys(bake.Poses(rows.used)))
        with mock.patch.object(track_host, "program", side_effect=track_host.TrackHostError("no compiler")):
            self.assertIsNone(self.keys(bake.Poses({}))["start"])

    def test_check_resamples_and_rejects_forged_missing_and_unneeded_rows(self):
        rows = bake.Poses({})
        found = bake.bakes([self.root], AnyStart(), rows)
        locked = {item.key: row_of(item, MADE_SHA256) for item in found}
        self.assertEqual(bake.check_poses(found, rows.used), [])
        key = next(iter(rows.used))
        forged = {**rows.used, key: {**rows.used[key], "poses": "forged"}}
        for held, message in ((forged, "differs"), ({}, "missing"),
                              ({**rows.used, "unneeded": {**rows.used[key], "key": "unneeded"}}, "nothing needs")):
            with self.subTest(message=message):
                self.assertTrue(any(message in problem for problem in bake.check_poses(found, held)))

    def test_lock_writes_needed_rows_and_drops_unneeded_rows(self):
        rows = bake.Poses({})
        found = bake.bakes([self.root], poses=rows)
        starts = {item.key: MADE_SHA256 for item in found if not item.packed}
        found = bake.bakes([self.root], starts, rows)
        locked = {item.key: row_of(item, MADE_SHA256) for item in found}
        path = self.root / "lock_row.lock"
        bake.write_lock(list(locked.values()), path, poses=[*[{**row, "poses": "forged"} for row in rows.used.values()],
                        {**next(iter(rows.used.values())), "key": "unneeded"}])
        with mock.patch.object(bake, "LOCK", path), \
                mock.patch.object(bake, "read_lock_tables", return_value={"bake": locked, "poses": bake.read_lock(path, table="poses")}):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(bake.main(["lock", str(self.root)]), 0)
        held = bake.read_lock(path, table="poses")
        self.assertEqual(set(held), set(rows.used))
        self.assertEqual({key: row["poses"] for key, row in held.items()},
                         {key: row["poses"] for key, row in rows.used.items()})
        self.assertTrue(all(set(row) == set(bake.POSE_FIELDS) for row in held.values()))

    def test_missing_sampler_leaves_only_camera_pack_unlocked(self):
        from anim import track_host
        from r3d import skin_pack

        plain = self.root / "plain"
        plain.mkdir()
        imported = write_import(plain, name="plain.import.toml")
        (imported.parent / "mesh.mesh").write_bytes(b"plain mesh")
        packs, jobs = build_pack.pack_jobs([self.root])
        rows = bake.Poses({})
        found = bake.bakes_in(packs, jobs, {}, rows)
        unaffected = [item for item in found if item.scene is None]
        self.assertTrue(unaffected)
        lock = {item.key: row_of(item, bake.file_sha256(item.tree), size=item.tree.stat().st_size) for item in unaffected}
        for item in unaffected:
            bake.store(item.tree, lock[item.key], item.suffix, self.cache)
        for failure in (track_host.TrackHostError("no C compiler found; set CC"),
                        track_host.TrackHostError("no sh"), FileNotFoundError("compiler unavailable")):
            with self.subTest(failure=str(failure)), \
                    mock.patch.object(track_host, "program", side_effect=failure), \
                    mock.patch.object(bake, "read_lock_tables", return_value={"bake": lock, "poses": {}}):
                missing = bake.bakes_in(packs, jobs)
                self.assertTrue(all(item.key is None for item in missing if item.scene is not None))
                with self.assertRaises(bake.BakeUnlocked) as raised:
                    bake.fetch_all(missing, lock, self.cache, offline=True)
                message = str(raised.exception)
                for text in (str(self.scene.camera.component.path.animation), "node", "every_ms", "size",
                             "bake.py lock", "C compiler", "track_host.py", "CI Bakes check"):
                    self.assertIn(text, message)
                self.assertNotIn(str(failure), message)
                skipped = []
                with mock.patch.object(build_pack, "REPO", self.root), \
                        mock.patch.object(build_pack, "pack_entry", side_effect=lambda key, *args, **kw: (key, build_pack.image_asset.TYPE, b"plain")), \
                        mock.patch.object(skin_pack, "entries", return_value=[]):
                    with contextlib.redirect_stderr(io.StringIO()) as err:
                        self.assertEqual(build_pack.main([str(self.root), "-o", str(self.root / "out"),
                                                             "--bake-cache", str(self.cache), "--offline"]), 2)
                    self.assertIn("bake.py lock", err.getvalue())
                    with self.assertRaises(bake.BakeUnlocked):
                        build_pack.pack_bytes([self.root], cache=self.cache, offline=True)
                    built = build_pack.pack_bytes([self.root], cache=self.cache, offline=True, unlocked=skipped)
                self.assertEqual(sorted(built), [imported.name.removesuffix(build_pack.IMPORT)])
                self.assertEqual(sorted(skipped), sorted(name for name in packs if name not in built))

    def test_missing_clip_is_not_a_sampler_failure(self):
        self.scene.camera.component.path.animation.unlink()
        with self.assertRaises((FileNotFoundError, bake.SettingsError)):
            self.keys()

    def test_region_visibility_ignores_clip_changes(self):
        from anim_probe import write_camera_clip

        self.job.renderer.fit = None
        self.job.renderer.visibility.source = "region"
        before = self.keys()
        write_camera_clip(self.root, reach=2.0)
        self.assertEqual(before, self.keys())


class CpuVendorTests(FittedBakeCase):
    """A mesh bake's bytes differ between CPU vendors: the lock takes meshes made only on its own vendor's hosts,
    and a run to upload makes them only there. A fit, made on the GPU host whatever its CPU, is not checked."""

    AMD, INTEL = "AuthenticAMD", "GenuineIntel"

    def made(self, start_cpu, fit_cpu=None):
        """A run's makes: the start mesh on `start_cpu` and its fit on `fit_cpu`; None leaves the CPU out."""
        made = {self.start.key: row_of(self.start, MADE_SHA256, run=RUN, **({"cpu_vendor": start_cpu} if start_cpu else {}))}
        fit = self.fit(MADE_SHA256)
        made[fit.key] = row_of(fit, FIT_SHA256, run=RUN, **({"cpu_vendor": fit_cpu} if fit_cpu else {}))
        return made

    def lock(self, made, mesh_cpu_vendor, lock=None):
        return self.command(["lock", "--from-run", str(RUN)], lock or {}, made, mesh_cpu_vendor=mesh_cpu_vendor)

    def test_a_run_whose_mesh_was_made_on_another_vendor_is_refused_naming_both(self):
        done = self.lock(self.made(self.INTEL), self.AMD)
        self.assertEqual(done.code, 1)
        self.assertFalse(done.written, "a refused run writes no lock")
        for named in (f"run {RUN}", self.start.output, self.INTEL, self.AMD, "re-run the failed Bakes CPU job"):
            self.assertIn(named, done.err)

    def test_a_run_that_names_no_vendor_is_refused(self):
        done = self.lock(self.made(None), self.AMD)
        self.assertEqual(done.code, 1)
        self.assertIn("a CPU it does not name", done.err)

    def test_a_run_on_the_locks_vendor_locks_and_keeps_the_vendor_line(self):
        done = self.lock(self.made(self.AMD, fit_cpu=self.INTEL), self.AMD)
        self.assertEqual(done.code, 0, done.err)
        self.assertEqual(done.rows[self.start.output]["sha256"], MADE_SHA256)
        self.assertEqual(done.rows[self.fit(MADE_SHA256).output]["sha256"], FIT_SHA256)
        self.assertNotIn("cpu_vendor", done.rows[self.start.output], "the vendor is the lock's line, not a row field")
        self.assertEqual(done.mesh_cpu_vendor, self.AMD)

    def test_a_mesh_the_lock_already_holds_is_not_taken_so_not_checked(self):
        made = self.made(self.INTEL)
        made[self.fit(LOCKED_SHA256).key] = row_of(self.fit(LOCKED_SHA256), FIT_SHA256, run=RUN)
        done = self.lock(made, self.AMD, self.locked(LOCKED_SHA256, run=RUN - 1))
        self.assertEqual(done.code, 0, done.err)
        self.assertEqual(done.rows[self.start.output]["sha256"], LOCKED_SHA256)

    def test_a_lock_that_names_no_vendor_takes_the_runs(self):
        done = self.lock(self.made(self.INTEL), None)
        self.assertEqual(done.code, 0, done.err)
        self.assertEqual(done.mesh_cpu_vendor, self.INTEL)

    def test_baking_meshes_to_upload_on_another_vendor_fails_before_making_anything(self):
        out = str(self.root / "upload")
        done = self.command(["bake", "--kind", "mesh", "--out", out], {}, mesh_cpu_vendor=self.AMD, cpu=self.INTEL)
        self.assertEqual(done.code, 1)
        self.assertEqual(done.produced, [])
        for named in (self.INTEL, self.AMD, "re-run the failed Bakes CPU job"):
            self.assertIn(named, done.err)
        for argv, cpu in ((["bake", "--kind", "mesh"], self.INTEL), (["bake", "--kind", "mesh", "--out", out], self.AMD)):
            with self.subTest(argv=argv, cpu=cpu):
                with mock.patch.object(produce, "export"):
                    done = self.command(argv, {}, mesh_cpu_vendor=self.AMD, cpu=cpu)
                self.assertEqual(done.code, 0, done.err)
                self.assertEqual([item.output for item in done.produced], [self.start.output])

    def test_a_run_with_no_mesh_to_make_passes_on_any_vendor(self):
        with mock.patch.object(produce, "export"):
            done = self.command(["bake", "--kind", "mesh", "--out", str(self.root / "upload")],
                                self.locked(LOCKED_SHA256, run=RUN - 1), mesh_cpu_vendor=self.AMD, cpu=self.INTEL)
        self.assertEqual(done.code, 0, done.err)
        self.assertEqual(done.produced, [])

    def test_the_vendor_is_read_from_cpuinfo_and_recorded_with_a_make(self):
        cpuinfo = self.root / "cpuinfo"
        cpuinfo.write_text("processor\t: 0\nvendor_id\t: AuthenticAMD\ncpu family\t: 25\n")
        tree = self.root / "one.mesh"
        tree.write_bytes(b"LMSH baked")
        with mock.patch.object(bake, "CPUINFO", cpuinfo):
            self.assertEqual(bake.cpu_vendor(), self.AMD)
            self.assertEqual(produce.record(bake_of("k" * 64, tree), tree, self.cache)["cpu_vendor"], self.AMD)
        with mock.patch.object(bake, "CPUINFO", self.root / "absent"):
            self.assertIsNone(bake.cpu_vendor())
            self.assertNotIn("cpu_vendor", produce.record(bake_of("j" * 64, tree), tree, self.cache))

    def test_the_vendor_line_reads_back_and_a_lock_without_one_writes_none(self):
        path = self.root / "bakes.lock"
        bake.write_lock([row_of(self.start, MADE_SHA256, run=RUN)], path, mesh_cpu_vendor=self.AMD)
        self.assertEqual(bake.read_lock_tables(path)["mesh_cpu_vendor"], self.AMD)
        bake.write_lock([row_of(self.start, MADE_SHA256, run=RUN)], path)
        self.assertIsNone(bake.read_lock_tables(path)["mesh_cpu_vendor"])
        self.assertNotIn("mesh_cpu_vendor", path.read_text())


class FitGpuTests(FittedBakeCase):
    """A fit's bytes differ between GPU models: a fit's make names its GPU, and its lock row keeps it."""

    GPU = "NVIDIA GeForce RTX 5080 Laptop GPU"

    def test_the_gpu_is_read_from_nvidia_smi_and_recorded_only_with_a_fit(self):
        tree = self.root / "one.bundle"
        tree.write_bytes(b"fit baked")
        with mock.patch.object(bake, "NVIDIA_SMI_NAME", (sys.executable, "-c", f"print({self.GPU!r}); print('x')")):
            self.assertEqual(bake.gpu_name(), self.GPU)
            self.assertEqual(produce.record(bake_of("k" * 64, tree, kind="fit"), tree, self.cache)["gpu"], self.GPU)
            self.assertNotIn("gpu", produce.record(bake_of("j" * 64, tree), tree, self.cache))
        for absent in ((str(self.root / "no-nvidia-smi"),), (sys.executable, "-c", "pass")):
            with self.subTest(absent=absent), mock.patch.object(bake, "NVIDIA_SMI_NAME", absent):
                self.assertIsNone(bake.gpu_name())
                self.assertNotIn("gpu", produce.record(bake_of("i" * 64, tree, kind="fit"), tree, self.cache))

    def test_a_locked_fit_keeps_the_gpu_its_run_made_it_on(self):
        fit = self.fit(MADE_SHA256)
        made = {self.start.key: row_of(self.start, MADE_SHA256, run=RUN, cpu_vendor="AuthenticAMD"),
                fit.key: row_of(fit, FIT_SHA256, run=RUN, gpu=self.GPU)}
        done = self.command(["lock", "--from-run", str(RUN)], {}, made, mesh_cpu_vendor="AuthenticAMD")
        self.assertEqual(done.code, 0, done.err)
        self.assertEqual(done.rows[fit.output]["gpu"], self.GPU)
        self.assertNotIn("gpu", done.rows[self.start.output])
        path = self.root / "bakes.lock"
        bake.write_lock(list(done.rows.values()), path, mesh_cpu_vendor="AuthenticAMD")
        self.assertIn(f'gpu = "{self.GPU}"', path.read_text(encoding="utf-8"))


class KeptWorkTests(FittedBakeCase):
    """A run that fails after making some fits uploads each one it finished, and a rerun makes only the rest:
    from this cache, or on a cold one from the run's uploads made on a host like this one."""

    names = ("first", "second", "third")
    GPU = "NVIDIA GeForce RTX 2060"

    def setUp(self):
        super().setUp()
        self.lock = self.locked(LOCKED_SHA256, run=RUN)
        self.outputs = [item.output for item in self.fits(LOCKED_SHA256)]
        self.out = self.root / "upload"

    def bake(self, cache, fail=None, from_run=None, gpu=GPU):
        """`bake --kind fit --out` into `cache`; making the fit `fail` raises as a lost lock stops the bake.
        Returns its exit code and the outputs it made."""
        produced = []

        def stub(item, cache, lock=None, blender=None):
            if item.output == fail:
                raise bake.BakeMissing("the heavy lock was lost")
            produced.append(item.output)
            path = self.root / item.output
            path.write_text(item.key, encoding="utf-8")
            return produce.record(item, path, cache)

        argv = ["bake", "--kind", "fit", str(self.root), "--cache", str(cache), "--out", str(self.out)]
        with mock.patch.object(bake, "read_lock_tables", return_value={"bake": self.lock, "poses": {}}), \
                mock.patch.object(bake, "cpu_vendor", return_value="AuthenticAMD"), \
                mock.patch.object(bake, "gpu_name", return_value=gpu), \
                mock.patch.object(bake, "run_files", side_effect=lambda run, folder: self.uploads[run]), \
                mock.patch.object(produce, "produce", side_effect=stub), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code = bake.main(argv + (["--from-run", str(from_run)] if from_run else []))
        return code, produced

    def uploaded(self):
        return sorted(row["output"] for row in produce.import_run(self.out, RUN, self.root / "lock-cache").values())

    def failed_run(self):
        """A run that made two fits and failed making the third; its upload is kept as run RUN's."""
        code, produced = self.bake(self.cache, fail=self.outputs[2])
        self.assertEqual(code, 1)
        self.assertEqual(produced, self.outputs[:2])
        self.uploads = {RUN: self.root / "run-upload"}
        shutil.copytree(self.out, self.uploads[RUN])
        shutil.rmtree(self.out)

    def test_a_run_that_fails_after_two_fits_uploads_both(self):
        self.failed_run()
        self.assertEqual(sorted(row["output"] for row in
                                produce.import_run(self.uploads[RUN], RUN, self.root / "lock-cache").values()),
                         sorted(self.outputs[:2]))

    def test_a_rerun_on_the_same_cache_makes_only_the_unfinished_fit_and_uploads_all_three(self):
        self.failed_run()
        code, produced = self.bake(self.cache)
        self.assertEqual(code, 0)
        self.assertEqual(produced, self.outputs[2:])
        self.assertEqual(self.uploaded(), sorted(self.outputs))

    def test_a_rerun_on_a_cold_cache_takes_the_runs_uploads_and_makes_only_the_rest(self):
        self.failed_run()
        code, produced = self.bake(self.root / "cold", from_run=RUN)
        self.assertEqual(code, 0)
        self.assertEqual(produced, self.outputs[2:])
        self.assertEqual(self.uploaded(), sorted(self.outputs))

    def test_a_rerun_on_another_gpu_model_remakes_every_fit(self):
        self.failed_run()
        code, produced = self.bake(self.root / "cold", from_run=RUN, gpu="NVIDIA GeForce RTX 5080 Laptop GPU")
        self.assertEqual(code, 0)
        self.assertEqual(produced, self.outputs)

    def test_a_bake_killed_mid_fit_leaves_every_finished_fit_uploadable(self):
        """A lost heavy lock stops the bake's whole process tree (device.run_to_end): no handler of its own runs."""
        script = textwrap.dedent("""
            import pathlib, sys, time
            from unittest import mock
            sys.path.insert(0, sys.argv[1])
            from bake import bake, produce
            root, lock_sha, last = pathlib.Path(sys.argv[2]), sys.argv[3], sys.argv[4]
            start = next(item for item in bake.bakes([root], {}) if not item.packed)
            lock = {start.key: {"output": start.output, "key": start.key, "sha256": lock_sha, "run": 1}}

            def stub(item, cache, lock=None, blender=None):
                if item.output == last:
                    (root / "started").touch()
                    time.sleep(600)
                path = root / item.output
                path.write_text(item.key, encoding="utf-8")
                return produce.record(item, path, cache)

            with (mock.patch.object(bake, "read_lock_tables", return_value={"bake": lock, "poses": {}}),
                  mock.patch.object(bake, "gpu_name", return_value=None),
                  mock.patch.object(produce, "produce", side_effect=stub)):
                bake.main(["bake", "--kind", "fit", str(root), "--cache", str(root / "cache"), "--out", sys.argv[5]])
            """)
        process = subprocess.Popen([sys.executable, "-c", script, str(TOOLS), str(self.root), LOCKED_SHA256,
                                    self.outputs[2], str(self.out)],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 60
            while not (self.root / "started").exists():
                self.assertIsNone(process.poll(), "the bake ended before its last fit")
                self.assertLess(time.monotonic(), deadline, "the last fit never started")
                time.sleep(0.05)
        finally:
            process.kill()
            process.wait()
        self.assertEqual(self.uploaded(), sorted(self.outputs[:2]))


class OnlyTests(FittedBakeCase):
    def test_an_output_of_another_kind_is_refused_before_making_anything(self):
        def refuse(parser, message):
            raise ValueError(message)
        with mock.patch.object(bake.argparse.ArgumentParser, "error", refuse), \
                self.assertRaisesRegex(ValueError, f"{re.escape(self.start.output)} not of --kind fit"):
            self.command(["bake", "--kind", "fit", "--only", self.start.output], {})


class SharedStartTests(FittedBakeCase):
    """Two fitted renderers of one import."""

    names = ("first", "second")

    def test_fits_that_share_a_start_make_one_start_bake(self):
        found = self.bakes({})
        fits = [item for item in found if item.kind == "fit"]
        starts = [item for item in found if item.kind == "mesh"]
        self.assertEqual(len(fits), len(self.names))
        self.assertEqual(len(starts), 1)
        self.assertEqual({item.stages["start"] for item in fits}, {starts[0].key})
        self.assertEqual({item.start for item in fits}, {starts[0]})

    def test_a_fits_start_is_an_unpacked_mesh_bake_named_for_its_entry(self):
        fit = self.fits(MADE_SHA256)[0]
        self.assertTrue(fit.packed)
        self.assertFalse(self.start.packed)
        self.assertEqual(self.start.output, fit.output.removesuffix(bake.MESH_SUFFIX) + bake.START_SUFFIX)
        self.assertEqual(self.start.kind, "mesh")


class LockTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        self.cache = self.root / "cache"
        self.tree = self.root / "one.mesh"
        self.tree.write_bytes(b"LMSH baked")
        self.row = {"output": "one.mesh", "source": "a.scene.toml", "key": "k" * 64,
                    "sha256": hashlib.sha256(b"LMSH baked").hexdigest(), "size": 10}

    def tearDown(self):
        self.directory.cleanup()

    def test_the_lock_reads_back_what_was_written(self):
        path = self.root / "bakes.lock"
        bake.write_lock([self.row, {**self.row, "key": "j" * 64, "run": 7}], path)
        self.assertEqual(bake.read_lock(path), {"k" * 64: self.row, "j" * 64: {**self.row, "key": "j" * 64, "run": 7}})

    def test_a_seeded_row_reads_back_seeded(self):
        path = self.root / "bakes.lock"
        bake.write_lock([{**self.row, "seeded": True}], path)
        self.assertIn("seeded = true", path.read_text())
        self.assertIs(bake.read_lock(path)["k" * 64]["seeded"], True)

    def test_seeding_marks_every_row_and_a_runs_row_carries_no_mark(self):
        found = [bake_of("k" * 64, self.tree)]
        self.assertTrue(all(row["seeded"] for row in bake.seed(found, self.cache)))
        made = {"k" * 64: {**self.row, "run": 9}}
        self.assertNotIn("seeded", bake.lock_rows(found, {}, made)[0])

    def test_seeding_keeps_a_made_row_and_reseeds_a_seeded_one(self):
        found = [bake_of("k" * 64, self.tree)]
        made = {**self.row, "sha256": "f" * 64, "run": 4, "host": "Linux x86_64"}
        self.assertEqual(bake.seed(found, self.cache, {"k" * 64: made}), [made])
        stale = {**self.row, "sha256": "f" * 64, "seeded": True}
        self.assertEqual(bake.seed(found, self.cache, {"k" * 64: stale})[0]["sha256"], self.row["sha256"])

    def test_a_runs_make_replaces_a_seeded_row_but_never_a_made_one(self):
        found = [bake_of("k" * 64, self.tree)]
        made = {"k" * 64: {**self.row, "sha256": "f" * 64, "run": 9}}
        seeded = bake.lock_rows(found, {"k" * 64: {**self.row, "seeded": True}}, made)[0]
        self.assertEqual((seeded["run"], seeded["sha256"]), (9, "f" * 64))
        self.assertNotIn("seeded", seeded)
        kept = bake.lock_rows(found, {"k" * 64: {**self.row, "run": 4}}, made)[0]
        self.assertEqual((kept["run"], kept["sha256"]), (4, self.row["sha256"]))

    def waiting_fit(self):
        """A fit whose start, key "k" * 64, the lock does not have yet: its own key is unknown."""
        return bake.Bake(output="two.mesh", source=self.tree, holder="two", kind="fit", key=None,
                         suffix=bake.MESH_SUFFIX, tree=self.root / "two.mesh", stages={"start": "k" * 64})

    def test_a_run_locks_a_start_while_its_fit_waits_naming_the_fit(self):
        start = bake_of("k" * 64, self.tree)
        missing = []
        rows = bake.lock_rows([start, self.waiting_fit()], {}, {"k" * 64: {**self.row, "run": 9}}, missing)
        self.assertEqual([row["key"] for row in rows], ["k" * 64])
        self.assertEqual(len(missing), 1)
        self.assertIn("two.mesh", missing[0])
        self.assertIn("is not locked", missing[0])
        with self.assertRaisesRegex(bake.BakeMissing, "two.mesh"):
            bake.lock_rows([start, self.waiting_fit()], {}, {"k" * 64: {**self.row, "run": 9}})

    def test_a_waiting_fit_is_never_seeded(self):
        with self.assertRaisesRegex(bake.BakeMissing, "(?s)two.mesh.*is not locked"):
            bake.seed([self.waiting_fit()], self.cache, {"j" * 64: {**self.row, "output": "two.mesh", "key": "j" * 64}})

    def test_a_fits_start_takes_the_locked_bytes_over_a_runs(self):
        made = {"k" * 64: {**self.row, "sha256": "f" * 64}}
        self.assertEqual(bake.locked_starts({"k" * 64: self.row}, made), {"k" * 64: self.row["sha256"]})
        self.assertEqual(bake.locked_starts({}, made), {"k" * 64: "f" * 64})

    def test_a_fit_fetches_the_start_its_key_names_and_fails_without_it(self):
        start = bake_of("k" * 64, self.tree)
        fit = bake.Bake(output="two.mesh", source=self.tree, holder="two", kind="fit", key="f" * 64,
                        suffix=bake.MESH_SUFFIX, tree=self.root / "two.mesh",
                        stages={"start": "k" * 64, "start_sha256": self.row["sha256"]}, start=start)
        bake.store(self.tree, self.row, bake.MESH_SUFFIX, self.cache)
        path = produce.start_file(fit, self.cache, {"k" * 64: self.row})
        self.assertEqual(path.read_bytes(), self.tree.read_bytes())
        other = {"k" * 64: {**self.row, "sha256": "e" * 64}}
        for lock, cache in (({}, self.root / "empty"), (other, self.root / "empty")):
            with self.assertRaisesRegex(bake.BakeMissing, "two.mesh: its start k+ with sha256 .* is neither locked"):
                produce.start_file(fit, cache, lock)

    def fit_of_start_made_here(self, start_sha256):
        """A fit naming `start_sha256`, whose start this cache made (no lock row) with the bytes of the tree's file."""
        start = bake_of("k" * 64, self.tree)
        produce.record(start, self.tree, self.cache)
        return bake.Bake(output="two.mesh", source=self.tree, holder="two", kind="fit", key="f" * 64,
                         suffix=bake.MESH_SUFFIX, tree=self.root / "two.mesh",
                         stages={"start": "k" * 64, "start_sha256": start_sha256}, start=start)

    def test_a_start_made_in_this_cache_with_the_named_sha_is_used_without_a_lock_row(self):
        fit = self.fit_of_start_made_here(self.row["sha256"])
        self.assertEqual(produce.start_file(fit, self.cache, {}).read_bytes(), self.tree.read_bytes())

    def test_a_start_made_in_this_cache_with_another_sha_is_refused(self):
        fit = self.fit_of_start_made_here("e" * 64)
        with self.assertRaisesRegex(bake.BakeMissing, "two.mesh: its start k+ with sha256 e+ is neither locked"):
            produce.start_file(fit, self.cache, {})

    def test_one_seed_from_a_run_locks_the_start_and_carries_the_fits_bytes(self):
        start = bake_of("k" * 64, self.tree)
        fit = bake.Bake(output="two.mesh", source=self.tree, holder="two", kind="fit", key="n" * 64,
                        suffix=bake.MESH_SUFFIX, tree=self.root / "two.mesh", stages={"start": "k" * 64})
        old_fit = {**self.row, "output": "two.mesh", "key": "o" * 64, "run": 3}
        bake.store(self.tree, old_fit, bake.MESH_SUFFIX, self.cache)
        rows = bake.seed([start, fit], self.cache, {"o" * 64: old_fit}, {"k" * 64: {**self.row, "run": 9}})
        self.assertEqual([(row["key"], row.get("seeded", False)) for row in rows],
                         [("k" * 64, False), ("n" * 64, True)])

    def test_seeding_fills_only_the_keys_the_lock_lacks(self):
        other = self.root / "two.mesh"
        other.write_bytes(b"LMSH other")
        found = [bake_of("k" * 64, self.tree), bake_of("n" * 64, other)]
        written = []
        with mock.patch.object(bake, "bakes", return_value=found), \
                mock.patch.object(bake, "read_lock_tables", return_value={"bake": {"k" * 64: {**self.row, "run": 5}}, "poses": {}}), \
                mock.patch.object(bake, "write_lock", side_effect=lambda rows, **kwargs: written.append(rows)), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(bake.main(["lock", "--seed", "--cache", str(self.cache)]), 0)
        rows = {row["key"]: row for row in written[0]}
        self.assertEqual(rows["k" * 64]["run"], 5)
        self.assertNotIn("seeded", rows["k" * 64])
        self.assertIs(rows["n" * 64]["seeded"], True)

    def test_a_file_not_yet_published_comes_from_the_run_that_made_it(self):
        upload = self.root / "upload"
        (upload / "bakes-cpu" / "files").mkdir(parents=True)
        (upload / "bakes-cpu" / "keys").mkdir()
        (upload / "bakes-cpu" / "files" / f"{self.row['sha256']}.mesh").write_bytes(b"LMSH baked")
        (upload / "bakes-cpu" / "keys" / f"{self.row['key']}.json").write_text(
            json.dumps({**self.row, "suffix": bake.MESH_SUFFIX}))
        gone = urllib.error.HTTPError("url", 404, "Not Found", {}, None)
        with mock.patch("urllib.request.urlopen", side_effect=gone), \
                mock.patch.object(bake, "run_files", return_value=upload) as run:
            paths = bake.fetch_all([bake_of("k" * 64, self.tree)], {"k" * 64: {**self.row, "run": 12}}, self.cache)
        self.assertEqual(run.call_args.args[0], 12)
        self.assertEqual(next(iter(paths.values())).read_bytes(), b"LMSH baked")

    def test_a_runs_uploads_are_read_through_the_api_without_gh(self):
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as zipped:
            zipped.writestr("files/a.mesh", b"mesh")
        listing = {"artifacts": [{"name": "bakes-cpu", "expired": False, "archive_download_url": "zip"},
                                 {"name": "other", "expired": False, "archive_download_url": "never"}]}
        replies = {"zip": archive.getvalue()}
        with mock.patch.dict("os.environ", {"GH_TOKEN": "t"}), \
                mock.patch.object(bake, "github_get", side_effect=lambda url, token: replies.get(url, json.dumps(listing).encode())) as get:
            bake.run_files(7, self.root / "run")
        self.assertEqual((self.root / "run" / "bakes-cpu" / "files" / "a.mesh").read_bytes(), b"mesh")
        self.assertNotIn("never", [call.args[0] for call in get.call_args_list])

    def test_a_refused_rename_onto_the_same_bytes_is_done_and_leaves_no_scratch(self):
        target = self.cache / "a.mesh"
        target.parent.mkdir(parents=True)
        target.write_bytes(b"same")
        with mock.patch("os.replace", side_effect=PermissionError("held")):
            self.assertEqual(bake.place(b"same", target), target)
        self.assertEqual(sorted(path.name for path in self.cache.iterdir()), ["a.mesh"])

    def test_a_refused_rename_onto_other_bytes_retries_then_fails_naming_it(self):
        target = self.cache / "a.mesh"
        target.parent.mkdir(parents=True)
        target.write_bytes(b"other")
        with mock.patch("os.replace", side_effect=PermissionError("held")) as rename, \
                mock.patch("time.sleep"), self.assertRaisesRegex(bake.BakeMissing, "a.mesh: another process holds it"):
            bake.place(b"new", target)
        self.assertEqual(rename.call_count, bake.PLACE_ATTEMPTS)
        self.assertEqual(sorted(path.name for path in self.cache.iterdir()), ["a.mesh"])

    def test_a_process_can_have_a_cache_of_its_own(self):
        with mock.patch.dict("os.environ", {bake.CACHE_VARIABLE: str(self.cache)}):
            self.assertEqual(bake.default_cache(), self.cache)

    def test_a_rekeyed_output_keeps_the_bytes_its_stale_row_locked(self):
        refit = b"LMSH refit"
        stale = {**self.row, "key": "o" * 64, "sha256": hashlib.sha256(refit).hexdigest(), "size": len(refit), "run": 5}
        bake.place(refit, bake.cached(stale, bake.MESH_SUFFIX, self.cache))
        found = [bake_of("k" * 64, self.tree)]
        written = []
        with mock.patch.object(bake, "bakes", return_value=found), \
                mock.patch.object(bake, "read_lock_tables", return_value={"bake": {"o" * 64: stale}, "poses": {}}), \
                mock.patch.object(bake, "write_lock", side_effect=lambda rows, **kwargs: written.append(rows)), \
                mock.patch("urllib.request.urlopen") as network, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(bake.main(["lock", "--seed", "--cache", str(self.cache)]), 0)
        network.assert_not_called()
        self.assertEqual(written[0], [{"output": "one.mesh", "source": "a.scene.toml", "key": "k" * 64,
                                       "sha256": stale["sha256"], "size": len(refit), "seeded": True}])

    def test_check_says_which_rows_are_seeded(self):
        found = [bake_of("k" * 64, self.tree)]
        with mock.patch.object(bake, "bakes", return_value=found), \
                mock.patch.object(bake, "read_lock_tables", return_value={"bake": {"k" * 64: {**self.row, "seeded": True}}, "poses": {}}), \
                contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(bake.main(["check"]), 0)
        self.assertIn("1 of 1 rows are seeded", out.getvalue())
        self.assertIn("one.mesh", out.getvalue())

    def test_check_names_a_missing_key_and_an_unneeded_one(self):
        problems = bake.check([bake_of("n" * 64, self.tree)], {"k" * 64: self.row})
        self.assertEqual(len(problems), 2)
        self.assertIn("n" * 64, problems[0])
        self.assertIn("nothing needs it", problems[1])

    def test_a_cached_file_with_the_locked_bytes_is_used_without_the_network(self):
        bake.store(self.tree, self.row, bake.MESH_SUFFIX, self.cache)
        with mock.patch("urllib.request.urlopen") as network:
            paths = bake.fetch_all([bake_of("k" * 64, self.tree)], {"k" * 64: self.row}, self.cache)
        network.assert_not_called()
        self.assertEqual(next(iter(paths.values())).read_bytes(), b"LMSH baked")

    def test_a_download_is_checked_against_the_lock(self):
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = b"LMSH other"
        with mock.patch("urllib.request.urlopen", return_value=response), \
                self.assertRaisesRegex(bake.BakeMissing, "does not have the SHA-256"):
            bake.fetch_all([bake_of("k" * 64, self.tree)], {"k" * 64: self.row}, self.cache)
        self.assertEqual(list(self.cache.glob("*")), [])

    def test_a_good_download_lands_in_the_cache(self):
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = b"LMSH baked"
        with mock.patch("urllib.request.urlopen", return_value=response):
            paths = bake.fetch_all([bake_of("k" * 64, self.tree)], {"k" * 64: self.row}, self.cache)
        self.assertEqual(next(iter(paths.values())), self.cache / f"{self.row['sha256']}.mesh")

    def test_every_missing_bake_is_named_with_its_key_source_and_way_out(self):
        gone = urllib.error.HTTPError("url", 404, "Not Found", {}, None)
        found = [bake_of("k" * 64, self.tree), bake_of("n" * 64, self.tree, "fit")]
        with mock.patch("urllib.request.urlopen", side_effect=gone), \
                self.assertRaises(bake.BakeMissing) as raised:
            bake.fetch_all(found, {"k" * 64: self.row}, self.cache)
        message = str(raised.exception)
        self.assertIn("2 bakes are not available", message)
        self.assertIn("k" * 64, message)
        self.assertIn("not on the release", message)
        self.assertIn("n" * 64, message)
        self.assertIn("CUDA GPU", message)

    def test_offline_never_downloads(self):
        with mock.patch("urllib.request.urlopen") as network, self.assertRaisesRegex(bake.BakeMissing, "offline"):
            bake.fetch_all([bake_of("k" * 64, self.tree)], {"k" * 64: self.row}, self.cache, offline=True)
        network.assert_not_called()

    def test_bakes_that_only_lack_lock_rows_are_unlocked_and_any_other_miss_is_not(self):
        unlocked = [bake_of("n" * 64, self.tree)]
        with self.assertRaises(bake.BakeUnlocked):
            bake.fetch_all(unlocked, {"k" * 64: self.row}, self.cache, offline=True)
        with self.assertRaises(bake.BakeMissing) as raised:
            bake.fetch_all(unlocked + [bake_of("k" * 64, self.tree)], {"k" * 64: self.row}, self.cache, offline=True)
        self.assertNotIsInstance(raised.exception, bake.BakeUnlocked)

    def test_a_cached_file_with_other_bytes_is_not_used(self):
        self.cache.mkdir()
        (self.cache / f"{self.row['sha256']}.mesh").write_bytes(b"stale")
        with self.assertRaisesRegex(bake.BakeMissing, "offline"):
            bake.fetch_all([bake_of("k" * 64, self.tree)], {"k" * 64: self.row}, self.cache, offline=True)

    def test_publish_runs_only_on_main_in_ci(self):
        with mock.patch.dict("os.environ", {"GITHUB_ACTIONS": "", "GITHUB_REF": "refs/heads/main"}), \
                self.assertRaisesRegex(bake.BakeMissing, "only in the Bakes workflow on main"):
            bake.publish([], {}, self.cache)


class RequirementTests(unittest.TestCase):
    def pins(self, stage):
        return {line.split("==")[0] for _, line in bake.requirement_pins(stage)}

    def test_the_gpu_packages_key_the_fit_alone(self):
        self.assertTrue({"torch", "nvdiffrast"} <= self.pins("fit"))
        self.assertFalse({"torch", "nvdiffrast"} & (self.pins("mesh") | self.pins("reference")))

    def test_the_requirements_beside_a_stage_file_key_it(self):
        self.assertTrue({"numpy", "mitsuba", "pillow"} <= self.pins("mesh"))

    def test_an_untracked_requirements_file_keys_nothing(self):
        (bake.TOOLS / "results").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=bake.TOOLS / "results") as directory:
            copy = pathlib.Path(directory) / "requirements.txt"
            copy.write_text("numpy==0.0.1\n")
            pins = bake.pinned_requirements(bake.stage_files("mesh"))
        self.assertTrue(pins)
        self.assertNotIn(bake.relative(copy), {path for path, _ in pins})

    def test_a_package_that_differs_from_its_pin_refuses_to_bake(self):
        with mock.patch("importlib.metadata.version", return_value="0.0.1"), \
                self.assertRaisesRegex(produce.EnvironmentMismatch, "numpy: .* pins 2.*has 0.0.1"):
            produce.check_environment(("mesh",))

    def test_a_submodule_the_index_lacks_fails(self):
        bake.submodules.cache_clear()
        try:
            with mock.patch.object(bake, "git_lines", return_value=[]), \
                    self.assertRaisesRegex(bake.SettingsError, "checkout is incomplete: .gitmodules names .*"
                                                                "meshoptimizer.*the index has no gitlink"):
                bake.submodules()
        finally:
            bake.submodules.cache_clear()


class ProduceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        self.cache = self.root / "cache"
        (self.root / "tree").mkdir()
        write_import(self.root / "tree")
        self.found = bake.bakes([self.root / "tree"])

    def tearDown(self):
        self.directory.cleanup()

    def produce(self):
        with mock.patch.object(produce, "check_environment"):
            return produce.produce(self.found[0], self.cache)

    def test_a_mesh_is_made_into_the_cache_and_indexed_by_its_key(self):
        row = self.produce()
        self.assertEqual(produce.made(self.found[0].key, self.cache), row)
        self.assertEqual(bake.file_sha256(bake.cached(row, bake.MESH_SUFFIX, self.cache)), row["sha256"])
        self.assertFalse(list((self.root / "tree").glob("*.mesh")), "the tree is never written")
        self.assertEqual(row["host"], f"{platform.system()} {platform.machine()}")
        self.assertEqual(bake.lock_rows(self.found, {}, {row["key"]: {**row, "run": 3}})[0]["host"], row["host"])

    def test_a_locally_made_bake_cannot_be_locked(self):
        self.produce()
        with self.assertRaisesRegex(bake.BakeMissing, "no CI run has made this key"):
            bake.lock_rows(self.found, {})

    def test_a_runs_uploads_lock_with_the_run_and_are_checked(self):
        row = self.produce()
        out = self.root / "upload"
        produce.export(row, self.cache, out)
        made = produce.import_run(out, 41, self.root / "other")
        self.assertEqual(bake.lock_rows(self.found, {}, made)[0]["run"], 41)
        (out / "files" / f"{row['sha256']}.mesh").write_bytes(b"other")
        with self.assertRaisesRegex(bake.BakeMissing, "run 41"):
            produce.import_run(out, 41, self.root / "third")

    def test_two_runs_lock_together(self):
        """A Bakes run's meshes and a Bakes GPU run's fits: neither run alone has every key, both together do."""
        row = self.produce()
        uploads = {41: self.root / "run41", 42: self.root / "run42"}
        produce.export(row, self.cache, uploads[41])
        other = {**row, "key": "k" * 64}
        produce.export(row, self.cache, uploads[42])
        (uploads[42] / produce.KEY_INDEX / f"{row['key']}.json").unlink()
        (uploads[42] / produce.KEY_INDEX / f"{other['key']}.json").write_text(json.dumps(other), encoding="utf-8")
        found = [self.found[0], bake_of(other["key"], self.root / "tree")]
        with mock.patch.object(bake, "run_files", side_effect=lambda run, folder: uploads[run]):
            for alone in (41, 42):
                with self.assertRaisesRegex(bake.BakeMissing, "no CI run has made this key"):
                    bake.lock_rows(found, {}, bake.runs_made([alone], self.root / f"c{alone}"))
            rows = bake.lock_rows(found, {}, bake.runs_made([41, 42], self.root / "both"))
        self.assertEqual([row["run"] for row in rows], [41, 42])

    def test_two_runs_that_made_one_key_differently_fail_naming_both(self):
        row = self.produce()
        uploads = {41: self.root / "run41", 42: self.root / "run42", 43: self.root / "run43"}
        for upload in uploads.values():
            produce.export(row, self.cache, upload)
        other = {**row, "sha256": hashlib.sha256(b"other").hexdigest()}
        (uploads[42] / "files" / f"{other['sha256']}.mesh").write_bytes(b"other")
        (uploads[42] / produce.KEY_INDEX / f"{row['key']}.json").write_text(json.dumps(other), encoding="utf-8")
        with mock.patch.object(bake, "run_files", side_effect=lambda run, folder: uploads[run]):
            with self.assertRaises(bake.BakeMissing) as failed:
                bake.runs_made([41, 42], self.root / "differ")
            same = bake.runs_made([41, 43], self.root / "same")
        for named in (row["key"], "run 41", "run 42", row["sha256"], other["sha256"]):
            self.assertIn(named, str(failed.exception))
        self.assertEqual(same[row["key"]]["run"], 41)

    def test_a_keyed_folder_is_made_once_and_a_cut_make_leaves_nothing_done(self):
        folder = self.cache / produce.REFERENCES / ("k" * 64)
        made = []

        def make(partial):
            made.append(partial)
            (partial / "frame.png").write_bytes(b"frame")

        def cut(partial):
            (partial / "half.png").write_bytes(b"half")
            raise RuntimeError("cut")

        with self.assertRaisesRegex(RuntimeError, "cut"):
            produce.keyed_folder(folder, cut)
        self.assertFalse(folder.exists(), "a cut make is never taken for done")
        self.assertEqual(produce.keyed_folder(folder, make), folder)
        self.assertEqual(sorted(path.name for path in folder.iterdir()), [produce.DONE, "frame.png"])
        self.assertEqual(produce.keyed_folder(folder, make), folder)
        self.assertEqual(len(made), 1, "a done folder is not made again")
        self.assertFalse(made[0].exists(), "the partial folder was renamed into place")

    def test_publish_takes_a_rows_file_from_its_run(self):
        row = self.produce()
        out = self.root / "upload"
        produce.export(row, self.cache, out)
        lock = {row["key"]: {**row, "run": 41}}
        uploads = []

        def gh(args, **_):
            if args[:3] == ["gh", "release", "upload"]:
                uploads.append(pathlib.Path(args[-1]).read_bytes())
            return mock.Mock(returncode=0)

        with mock.patch.dict("os.environ", {"GITHUB_ACTIONS": "true", "GITHUB_REF": "refs/heads/main"}), \
                mock.patch.object(bake, "release_assets", return_value=set()), \
                mock.patch.object(bake, "run_files", side_effect=lambda run, folder: out), \
                mock.patch("subprocess.run", side_effect=gh):
            bake.publish(self.found, lock, self.root / "empty")
        self.assertEqual([hashlib.sha256(data).hexdigest() for data in uploads], [row["sha256"]])


class BlendTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        (self.root / "rig.blend").write_bytes(b"BLENDER-v502 one")
        self.write('clips = ["walk", "run"]\n')

    def tearDown(self):
        self.directory.cleanup()

    def write(self, clips):
        (self.root / "rig.import.toml").write_text(
            f'[source]\npath = "rig.blend"\ncredit = "c"\n{clips}[output]\nname = "rig"\n')

    def found(self):
        return {found.kind: found for found in bake.bakes([self.root / "rig.import.toml"])}

    def test_the_export_comes_before_the_mesh_that_reads_it(self):
        self.assertEqual([found.kind for found in bake.bakes([self.root / "rig.import.toml"])], ["blend", "mesh"])
        self.assertEqual(self.found()["blend"].output, "rig.glb")

    def test_the_blend_its_clips_and_the_exporter_key_the_export_and_the_mesh(self):
        before = self.found()
        self.write('clips = ["walk"]\n')
        clips = self.found()
        (self.root / "rig.blend").write_bytes(b"BLENDER-v502 two")
        blend = self.found()
        for kind in ("blend", "mesh"):
            self.assertEqual(len({before[kind].key, clips[kind].key, blend[kind].key}), 3, kind)

    def test_clips_belong_to_a_blend_source(self):
        (self.root / "m.glb").write_bytes(b"glTF")
        (self.root / "rig.import.toml").write_text(
            '[source]\npath = "m.glb"\ncredit = "c"\nclips = ["walk"]\n[output]\nname = "m"\n')
        with self.assertRaisesRegex(bake.SettingsError, "source.clips"):
            bake.bakes([self.root / "rig.import.toml"])

    def test_the_importer_refuses_a_blend_it_was_not_handed_an_export_of(self):
        from r3d.import_settings import load_import_settings
        from r3d.mesh_import import load_source

        with self.assertRaisesRegex(bake.SettingsError, "exported by .*bake.py"):
            load_source(load_import_settings(self.root / "rig.import.toml"))

    def test_the_export_runs_blender_with_the_clips_and_records_its_bytes(self):
        export = self.found()["blend"]

        def blender(command, **_):
            pathlib.Path(command[command.index("--") + 2]).write_bytes(b"glTF export")
            return mock.Mock(returncode=0, stdout="", stderr="")

        with mock.patch("subprocess.run", side_effect=blender) as run:
            row = produce.produce(export, self.root / "cache", blender="blender")
        command = run.call_args.args[0]
        self.assertEqual(command[:5], ["blender", "--background", "--factory-startup", "--python",
                                       str(bake.TOOLS / bake.BLEND_EXPORT)])
        self.assertEqual(command[-2:], ["--clips", "walk,run"])
        self.assertEqual(row["sha256"], hashlib.sha256(b"glTF export").hexdigest())

    def test_a_failed_export_fails_with_blenders_output(self):
        export = self.found()["blend"]
        failed = mock.Mock(returncode=1, stdout="", stderr="needs Blender 5.2.2, this is 4.2.0")
        with mock.patch("subprocess.run", return_value=failed), \
                self.assertRaisesRegex(bake.BakeMissing, "needs Blender 5.2.2"):
            produce.produce(export, self.root / "cache", blender="blender")


def filled_cache(directory, test):
    """The tree's packed bakes and exports in `directory`, with an in-memory lock fixture for sound
    rows CI has not locked yet. The tracked lock is never written."""
    every = bake.bakes([build_pack.DEFAULT_SEARCH])
    found = [item for item in every if item.packed]
    lock = bake.read_lock()
    for item in found:
        if item.kind == "sound" and item.key not in lock:
            lock[item.key] = produce.made(item.key, bake.default_cache()) or produce.produce(item, bake.default_cache())
    replacement = mock.patch.object(bake, "read_lock", return_value=lock)
    replacement.start()
    test.addCleanup(replacement.stop)
    for path in bake.fetch_all(found + [item for item in every if item.kind == "blend"], lock,
                               bake.default_cache()).values():
        shutil.copyfile(path, pathlib.Path(directory) / path.name)
    return found, lock


def current_lock(path):
    """Writes the lock the tree's own keys need to `path`, every row with a placeholder digest: what the
    key tests read instead of the repo's lock, which a re-keying change leaves stale until it is re-locked."""
    poses = bake.Poses({}, resample=True)
    starts = {item.key: "0" * 64 for item in bake.bakes([build_pack.DEFAULT_SEARCH], {}, poses) if item.key}
    found = bake.bakes([build_pack.DEFAULT_SEARCH], starts, poses)
    made = {item.key: {"output": item.output, "source": bake.relative(item.source), "key": item.key,
                       "sha256": "0" * 64, "size": 0, "run": 1} for item in found}
    bake.write_lock(bake.lock_rows(found, {}, made), path, poses=poses.used.values())


class StdlibKeysTests(unittest.TestCase):
    def test_list_and_check_need_no_numpy_with_held_or_missing_poses(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            blocker = root / "numpy"
            blocker.mkdir()
            (blocker / "__init__.py").write_text('raise ImportError("numpy blocked for key test")\n')
            missing = root / "missing.lock"
            missing.write_text("")
            current = root / "current.lock"
            current_lock(current)
            environment = {**os.environ, "PYTHONPATH": str(root)}
            for lock in (current, missing):
                for command in ("list", "check"):
                    with self.subTest(lock=lock.name, command=command):
                        script = (
                            "import pathlib, sys; "
                            "sys.path.insert(0, sys.argv[1]); "
                            "from bake import bake; "
                            "bake.LOCK = pathlib.Path(sys.argv[2]); "
                            "bake.read_lock_tables.__defaults__ = (bake.LOCK,); "
                            "raise SystemExit(bake.main([sys.argv[3]]))"
                        )
                        result = subprocess.run([sys.executable, "-c", script, str(TOOLS), str(lock), command],
                                                env=environment, capture_output=True, text=True)
                        output = result.stdout + result.stderr
                        self.assertNotIn("numpy blocked", output)
                        self.assertNotIn("Traceback", output)
                        if command == "list":
                            self.assertEqual(result.returncode, 0, output)
                        else:
                            self.assertIn(result.returncode, (0, 1), output)
                            if lock == current:
                                self.assertNotIn("  poses ", output)
                            else:
                                self.assertIn("  poses ", output)

    def test_list_lock_hit_imports_neither_poses_nor_track_host(self):
        script = (
            "import pathlib, sys; sys.path.insert(0, sys.argv[1]); "
            "sys.modules['r3d.poses'] = None; sys.modules['anim.track_host'] = None; "
            "from bake import bake; "
            "bake.LOCK = pathlib.Path(sys.argv[2]); "
            "bake.read_lock_tables.__defaults__ = (bake.LOCK,); "
            "raise SystemExit(bake.main(['list']))"
        )
        with tempfile.TemporaryDirectory() as directory:
            lock = pathlib.Path(directory) / "current.lock"
            current_lock(lock)
            result = subprocess.run([sys.executable, "-c", script, str(TOOLS), str(lock)],
                                    capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class TreeTests(unittest.TestCase):
    def test_packs_build_offline_from_a_filled_cache_as_they_do_online(self):
        with tempfile.TemporaryDirectory() as directory:
            filled_cache(directory, self)
            with mock.patch("urllib.request.urlopen") as network:
                offline = build_pack.pack_bytes([build_pack.DEFAULT_SEARCH], cache=pathlib.Path(directory), offline=True)
            network.assert_not_called()
        self.assertEqual(offline, build_pack.pack_bytes([build_pack.DEFAULT_SEARCH]))

    def test_a_mesh_no_bake_keys_fails_instead_of_taking_another_file(self):
        with tempfile.TemporaryDirectory() as directory:
            found, lock = filled_cache(directory, self)
            dropped = found[0]
            with mock.patch.object(bake, "bakes_in", return_value=[item for item in found if item is not dropped]), \
                    self.assertRaisesRegex(bake.BakeMissing, "no bake keys these meshes") as raised:
                build_pack.pack_bytes([build_pack.DEFAULT_SEARCH], cache=pathlib.Path(directory), offline=True)
        self.assertIn(dropped.output.removesuffix(bake.MESH_SUFFIX), str(raised.exception))

    def test_a_mesh_outside_the_repository_is_its_own_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            found, lock = filled_cache(root, self)
            some_mesh = bake.cached(lock[found[0].key], bake.MESH_SUFFIX, root).read_bytes()
            (root / "tree").mkdir()
            imported = write_import(root / "tree")
            for entries in build_pack.pack_files([imported]).values():
                for mesh in entries.values():
                    mesh.write_bytes(some_mesh)
            with mock.patch("urllib.request.urlopen") as network:
                packs = build_pack.pack_bytes([imported], cache=root / "empty", offline=True)
            network.assert_not_called()
        self.assertEqual(list(packs), [imported.name.removesuffix(build_pack.IMPORT)])

    def test_a_replaced_mesh_is_never_fetched(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = pathlib.Path(directory)
            found, lock = filled_cache(cache, self)
            scratch = found[0]
            mesh = cache / "scratch.mesh"
            mesh.write_bytes(bake.cached(lock[scratch.key], bake.MESH_SUFFIX, cache).read_bytes())
            name = scratch.output.removesuffix(bake.MESH_SUFFIX)
            rows = {key: row for key, row in lock.items() if key != scratch.key}
            with mock.patch.object(bake, "read_lock", side_effect=lambda **kw: (rows, {}) if kw.get("table", "bake") is None else rows), \
                    mock.patch("urllib.request.urlopen") as network:
                packs = build_pack.pack_bytes([build_pack.DEFAULT_SEARCH], [f"{name}={mesh}"], cache=cache, offline=True)
            network.assert_not_called()
        self.assertEqual(packs, build_pack.pack_bytes([build_pack.DEFAULT_SEARCH]))

    def test_every_key_is_unique(self):
        keys = [found.key for found in bake.bakes([build_pack.DEFAULT_SEARCH]) if found.key is not None]
        self.assertEqual(len(keys), len(set(keys)))

    def test_a_cold_build_fails_naming_each_bake(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stderr(io.StringIO()) as err:
            code = build_pack.main(["-o", directory, "--bake-cache", str(pathlib.Path(directory) / "cache"),
                                    "--offline"])
        self.assertEqual(code, 2)
        for found in bake.bakes([build_pack.DEFAULT_SEARCH]):
            if found.packed:  # a pack holds no export and no fit's start
                self.assertIn(found.output, err.getvalue())


class SkipUnlockedTests(unittest.TestCase):
    """build_pack.py --skip-unlocked: what test/run_tests.sh builds on a branch waiting on the lock."""

    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = pathlib.Path(folder.name)
        self.cache, self.out, self.listed = self.root / "cache", self.root / "packs", self.root / "unlocked.txt"
        self.cache.mkdir()
        self.found, self.lock = filled_cache(self.cache, self)
        self.packs = build_pack.pack_files([build_pack.DEFAULT_SEARCH])
        self.waiting = self.found[0]
        entry = self.waiting.output.removesuffix(bake.MESH_SUFFIX)
        self.holder = next(name for name, entries in self.packs.items() if entry in entries)

    def build(self, rows, *flags):
        args = ["-o", str(self.out), "--bake-cache", str(self.cache), "--offline", *flags]
        with mock.patch.object(bake, "read_lock", side_effect=lambda **kw: (rows, {}) if kw.get("table", "bake") is None else rows), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as err:
            return build_pack.main(args), err.getvalue()

    def written(self):
        return sorted(path.name.removesuffix(build_pack.PACK_SUFFIX) for path in self.out.glob("*" + build_pack.PACK_SUFFIX))

    def test_a_tree_with_every_row_writes_every_pack_and_names_none(self):
        code, err = self.build(self.lock, "--skip-unlocked", str(self.listed))
        self.assertEqual(code, 0, err)
        self.assertEqual(self.written(), sorted(self.packs))
        self.assertEqual(self.listed.read_text(), "")

    def test_a_pack_whose_bake_lacks_its_lock_row_is_left_out_and_named(self):
        rows = {key: row for key, row in self.lock.items() if key != self.waiting.key}
        code, err = self.build(rows, "--skip-unlocked", str(self.listed))
        self.assertEqual(code, 0, err)
        self.assertEqual(self.written(), sorted(name for name in self.packs if name != self.holder))
        self.assertEqual(self.listed.read_text(), f"{self.holder}\n")

    def test_without_the_flag_a_missing_lock_row_still_fails(self):
        rows = {key: row for key, row in self.lock.items() if key != self.waiting.key}
        code, err = self.build(rows)
        self.assertEqual(code, 2)
        self.assertIn(self.waiting.output, err)
        self.assertIn("has no row for this key", err)

    def test_a_locked_bake_the_cache_cannot_give_still_fails_with_the_flag(self):
        bake.cached(self.lock[self.waiting.key], bake.MESH_SUFFIX, self.cache).unlink()
        code, err = self.build(self.lock, "--skip-unlocked", str(self.listed))
        self.assertEqual(code, 2)
        self.assertIn(self.waiting.output, err)
        self.assertIn("offline", err)
        self.assertFalse(self.listed.exists())

    def test_one_pack_waiting_and_another_broken_still_fails_naming_the_broken_one(self):
        broken = next(found for found in self.found
                      if found.output.removesuffix(bake.MESH_SUFFIX) not in self.packs[self.holder])
        bake.cached(self.lock[broken.key], bake.MESH_SUFFIX, self.cache).unlink()
        rows = {key: row for key, row in self.lock.items() if key != self.waiting.key}
        code, err = self.build(rows, "--skip-unlocked", str(self.listed))
        self.assertEqual(code, 2)
        self.assertIn(broken.output, err)
        self.assertNotIn(self.waiting.output, err)
        self.assertFalse(self.listed.exists())


if __name__ == "__main__":
    unittest.main()
