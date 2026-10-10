"""Cached bakes (bake/bake.py): what a key counts and ignores, how stages chain,
the lock file, and that packs built from the cache are the tree's packs byte
for byte. build_pack.py's tree path has its own suite, test_asset_pack.py."""

import contextlib
import copy
import dataclasses
import hashlib
import io
import json
import pathlib
import platform
import shutil
import sys
import tempfile
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


class TokenTests(unittest.TestCase):
    def tokens(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "m.py"
            path.write_text(text, encoding="utf-8")
            return bake.code_tokens(path)

    def test_comments_blank_lines_and_indent_width_do_not_change_the_code(self):
        code = self.tokens("def f(x):\n    return x + 1\n")
        self.assertEqual(code, self.tokens("# what f is\ndef f(x):  # one more\n\n\n  return x  +  1\n"))

    def test_a_changed_expression_or_docstring_does(self):
        code = self.tokens('def f(x):\n    """Adds one."""\n    return x + 1\n')
        self.assertNotEqual(code, self.tokens('def f(x):\n    """Adds one."""\n    return x + 2\n'))
        self.assertNotEqual(code, self.tokens('def f(x):\n    """Adds two."""\n    return x + 1\n'))

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

    def test_the_pose_samplers_c_and_headers_are_in_the_mesh_stage(self):
        names = bake.native_inputs(bake.stage_files("mesh"))
        self.assertTrue({"launcher/tools/anim/track_host.c", "launcher/main/anim/anim_track.h",
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
            self.assertIn("render/render_compare.py", self.names(stage), stage)

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
        recipe = bake.reference_recipe(inputs, TOOL_KEYS)
        fields = {field.name for field in dataclasses.fields(ReferenceInputs)}
        self.assertEqual(set(recipe) - {"sources"}, fields)
        brighter = dataclasses.replace(inputs, tonemap_white=inputs.tonemap_white * 2)
        self.assertNotEqual(bake.digest(recipe), bake.digest(bake.reference_recipe(brighter, TOOL_KEYS)))

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

    def command(self, argv, lock, made=None):
        """Runs bake.py main over the tree with the lock and the runs' makes given and produce.produce stubbed:
        a start is made with MADE_SHA256, a fit with FIT_SHA256. Returns what it did."""
        written, produced = [], []

        def stub(item, cache, lock=None, blender=None):
            produced.append(item)
            return row_of(item, MADE_SHA256 if not item.packed else FIT_SHA256)

        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(bake, "read_lock", return_value=lock), \
                mock.patch.object(bake, "write_lock", side_effect=written.append), \
                mock.patch.object(bake, "runs_made", return_value=made or {}), \
                mock.patch.object(produce, "produce", side_effect=stub), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = bake.main([*argv, str(self.root), "--cache", str(self.cache)])
        return types.SimpleNamespace(code=code, out=out.getvalue(), err=err.getvalue(), produced=produced,
                                     rows={row["output"]: row for row in (written[0] if written else [])})

    def locked(self, sha256, **fields):
        return {self.start.key: row_of(self.start, sha256, **fields)}


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

    def test_the_visibility_size_changes_the_reference_key(self):
        def widen(job):
            width, height = job.renderer.visibility.size
            job.renderer.visibility.size = (width + 1, height)

        self.assertNotEqual(self.reference_key(widen), self.reference_key(lambda job: None))


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
                mock.patch.object(bake, "read_lock", return_value={"k" * 64: {**self.row, "run": 5}}), \
                mock.patch.object(bake, "write_lock", side_effect=written.append), \
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
                mock.patch.object(bake, "read_lock", return_value={"o" * 64: stale}), \
                mock.patch.object(bake, "write_lock", side_effect=written.append), \
                mock.patch("urllib.request.urlopen") as network, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(bake.main(["lock", "--seed", "--cache", str(self.cache)]), 0)
        network.assert_not_called()
        self.assertEqual(written[0], [{"output": "one.mesh", "source": "a.scene.toml", "key": "k" * 64,
                                       "sha256": stale["sha256"], "size": len(refit), "seeded": True}])

    def test_check_says_which_rows_are_seeded(self):
        found = [bake_of("k" * 64, self.tree)]
        with mock.patch.object(bake, "bakes", return_value=found), \
                mock.patch.object(bake, "read_lock", return_value={"k" * 64: {**self.row, "seeded": True}}), \
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
        produce.export([row], self.cache, out)
        made = produce.import_run(out, 41, self.root / "other")
        self.assertEqual(bake.lock_rows(self.found, {}, made)[0]["run"], 41)
        (out / "files" / f"{row['sha256']}.mesh").write_bytes(b"other")
        with self.assertRaisesRegex(bake.BakeMissing, "run 41"):
            produce.import_run(out, 41, self.root / "third")

    def test_two_runs_lock_together(self):
        """A Bakes run's meshes and a Bakes GPU run's fits: neither run alone has every key, both together do."""
        row = self.produce()
        uploads = {41: self.root / "run41", 42: self.root / "run42"}
        produce.export([row], self.cache, uploads[41])
        other = {**row, "key": "k" * 64}
        produce.export([row], self.cache, uploads[42])
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
            produce.export([row], self.cache, upload)
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

    def test_publish_takes_a_rows_file_from_its_run(self):
        row = self.produce()
        out = self.root / "upload"
        produce.export([row], self.cache, out)
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


def filled_cache(directory):
    """Every mesh the tree's lock names, in the cache `directory`: copied from the user cache, fetched
    there first when it lacks one. Returns the mesh bakes and the lock."""
    found = [item for item in bake.bakes([build_pack.DEFAULT_SEARCH]) if item.packed]
    lock = bake.read_lock()
    for path in bake.fetch_all(found, lock, bake.default_cache()).values():
        shutil.copyfile(path, pathlib.Path(directory) / path.name)
    return found, lock


class TreeTests(unittest.TestCase):
    def test_packs_build_offline_from_a_filled_cache_as_they_do_online(self):
        with tempfile.TemporaryDirectory() as directory:
            filled_cache(directory)
            with mock.patch("urllib.request.urlopen") as network:
                offline = build_pack.pack_bytes([build_pack.DEFAULT_SEARCH], cache=pathlib.Path(directory), offline=True)
            network.assert_not_called()
        self.assertEqual(offline, build_pack.pack_bytes([build_pack.DEFAULT_SEARCH]))

    def test_a_mesh_no_bake_keys_fails_instead_of_taking_another_file(self):
        with tempfile.TemporaryDirectory() as directory:
            found, lock = filled_cache(directory)
            dropped = found[0]
            with mock.patch.object(bake, "bakes_in", return_value=[item for item in found if item is not dropped]), \
                    self.assertRaisesRegex(bake.BakeMissing, "no bake keys these meshes") as raised:
                build_pack.pack_bytes([build_pack.DEFAULT_SEARCH], cache=pathlib.Path(directory), offline=True)
        self.assertIn(dropped.output.removesuffix(bake.MESH_SUFFIX), str(raised.exception))

    def test_a_mesh_outside_the_repository_is_its_own_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            found, lock = filled_cache(root)
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
            found, lock = filled_cache(cache)
            scratch = found[0]
            mesh = cache / "scratch.mesh"
            mesh.write_bytes(bake.cached(lock[scratch.key], bake.MESH_SUFFIX, cache).read_bytes())
            name = scratch.output.removesuffix(bake.MESH_SUFFIX)
            rows = {key: row for key, row in lock.items() if key != scratch.key}
            with mock.patch.object(bake, "read_lock", return_value=rows), \
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
        self.found, self.lock = filled_cache(self.cache)
        self.packs = build_pack.pack_files([build_pack.DEFAULT_SEARCH])
        self.waiting = self.found[0]
        entry = self.waiting.output.removesuffix(bake.MESH_SUFFIX)
        self.holder = next(name for name, entries in self.packs.items() if entry in entries)

    def build(self, rows, *flags):
        args = ["-o", str(self.out), "--bake-cache", str(self.cache), "--offline", *flags]
        with mock.patch.object(bake, "read_lock", return_value=rows), \
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
