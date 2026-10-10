"""Cached bakes (bake/bake.py): what a key counts and ignores, how stages chain,
the lock file, and that packs built from the cache are the tree's packs byte
for byte. build_pack.py's tree path has its own suite, test_asset_pack.py."""

import contextlib
import copy
import hashlib
import io
import json
import pathlib
import platform
import sys
import tempfile
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
from test_r3d_import import write_import  # noqa: E402

TOOL_KEYS = {stage: stage * 8 for stage in bake.STAGES}


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
                         "launcher/main/math/scalar/mathf.h"} <= set(names))

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

    def test_a_fit_edit_rekeys_the_fit_alone_and_a_mesh_edit_every_stage(self):
        job, scene = fitted_job()
        keys = bake.stage_keys(job, scene, TOOL_KEYS)
        fit = copy.deepcopy(job)
        fit.renderer.fit.steps += 1
        refit = bake.stage_keys(fit, scene, TOOL_KEYS)
        self.assertEqual(keys["reference"], refit["reference"])
        self.assertNotEqual(keys["fit"], refit["fit"])
        retool = bake.stage_keys(job, scene, {**TOOL_KEYS, "fit": "changed"})
        self.assertEqual((keys["start"], keys["reference"]), (retool["start"], retool["reference"]))
        self.assertNotEqual(keys["fit"], retool["fit"])
        rebaked = bake.stage_keys(job, scene, {**TOOL_KEYS, "mesh": "changed"})
        self.assertTrue(all(keys[stage] != rebaked[stage] for stage in keys))

    def test_recorded_fit_hashes_are_not_inputs(self):
        job, scene = fitted_job()
        recorded = copy.deepcopy(job)
        recorded.renderer.fit.sha256 = recorded.renderer.fit.recipe_sha256 = "0" * 64
        self.assertEqual(bake.stage_keys(job, scene, TOOL_KEYS), bake.stage_keys(recorded, scene, TOOL_KEYS))


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
            f'[source]\npath = "rig.blend"\ncredit = "c"\n{clips}[output]\ndirectory = "."\nname = "rig"\n')

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
            '[source]\npath = "m.glb"\ncredit = "c"\nclips = ["walk"]\n[output]\ndirectory = "."\nname = "m"\n')
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


class TreeTests(unittest.TestCase):
    def test_packs_built_from_a_seeded_cache_are_the_trees_packs(self):
        found = bake.bakes([build_pack.DEFAULT_SEARCH])
        self.assertTrue(found)
        with tempfile.TemporaryDirectory() as directory:
            cache = pathlib.Path(directory)
            rows = bake.seed(found, cache)
            with mock.patch.object(bake, "read_lock", return_value={row["key"]: row for row in rows}), \
                    mock.patch("urllib.request.urlopen") as network:
                cached = build_pack.pack_bytes([build_pack.DEFAULT_SEARCH], cache=cache, offline=True)
            network.assert_not_called()
        self.assertEqual(cached, build_pack.pack_bytes([build_pack.DEFAULT_SEARCH], tree=True))

    def test_a_mesh_no_bake_keys_fails_instead_of_taking_the_trees_copy(self):
        found = bake.bakes([build_pack.DEFAULT_SEARCH])
        dropped = next(item for item in found if item.kind == "mesh")
        with tempfile.TemporaryDirectory() as directory:
            cache = pathlib.Path(directory)
            rows = bake.seed(found, cache)
            with mock.patch.object(bake, "read_lock", return_value={row["key"]: row for row in rows}), \
                    mock.patch.object(bake, "bakes_in", return_value=[item for item in found if item is not dropped]), \
                    self.assertRaisesRegex(bake.BakeMissing, "no bake keys these meshes") as raised:
                build_pack.pack_bytes([build_pack.DEFAULT_SEARCH], cache=cache, offline=True)
        self.assertIn(dropped.output.removesuffix(bake.MESH_SUFFIX), str(raised.exception))

    def test_a_mesh_outside_the_repository_is_its_own_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            imported = write_import(root)
            for entries in build_pack.pack_files([imported]).values():
                for mesh in entries.values():
                    mesh.write_bytes((bake.REPO / "launcher/demo/capybara/capybara.meadow.mesh").read_bytes())
            with mock.patch("urllib.request.urlopen") as network:
                packs = build_pack.pack_bytes([imported], cache=root / "empty", offline=True)
            network.assert_not_called()
        self.assertEqual(list(packs), [imported.name.removesuffix(build_pack.IMPORT)])

    def test_a_replaced_mesh_is_never_fetched(self):
        found = bake.bakes([build_pack.DEFAULT_SEARCH])
        scratch = next(item for item in found if item.kind == "mesh")
        with tempfile.TemporaryDirectory() as directory:
            cache = pathlib.Path(directory)
            rows = {row["key"]: row for row in bake.seed(found, cache) if row["key"] != scratch.key}
            mesh = cache / "scratch.mesh"
            mesh.write_bytes(scratch.tree.read_bytes())
            name = scratch.output.removesuffix(bake.MESH_SUFFIX)
            with mock.patch.object(bake, "read_lock", return_value=rows),                     mock.patch("urllib.request.urlopen") as network:
                packs = build_pack.pack_bytes([build_pack.DEFAULT_SEARCH], [f"{name}={mesh}"], cache=cache, offline=True)
            network.assert_not_called()
        self.assertEqual(packs, build_pack.pack_bytes([build_pack.DEFAULT_SEARCH], tree=True))

    def test_every_key_is_unique(self):
        keys = [found.key for found in bake.bakes([build_pack.DEFAULT_SEARCH])]
        self.assertEqual(len(keys), len(set(keys)))

    def test_a_cold_build_fails_naming_each_bake(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stderr(io.StringIO()) as err:
            code = build_pack.main(["-o", directory, "--bake-cache", str(pathlib.Path(directory) / "cache"),
                                    "--offline"])
        self.assertEqual(code, 2)
        for found in bake.bakes([build_pack.DEFAULT_SEARCH]):
            if found.kind != "blend":  # a pack holds no export
                self.assertIn(found.output, err.getvalue())


if __name__ == "__main__":
    unittest.main()
