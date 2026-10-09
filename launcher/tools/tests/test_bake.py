"""Cached bakes (bake/bake.py): what a key counts and ignores, how stages chain,
the lock file, and that packs built from the cache are the tree's packs byte
for byte. build_pack.py's tree path has its own suite, test_asset_pack.py."""

import contextlib
import copy
import hashlib
import io
import pathlib
import sys
import tempfile
import unittest
import urllib.error
from unittest import mock

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from bake import bake  # noqa: E402
from r3d import build_pack  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402

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

    def test_the_pose_samplers_headers_are_in_the_mesh_stage(self):
        names = bake.native_sources(bake.stage_files("mesh"))
        self.assertIn("launcher/main/anim/anim_track.h", names)
        self.assertIn("launcher/main/util/scalar/mathf.h", names)


class StageTests(unittest.TestCase):
    def names(self, stage):
        return {path.relative_to(TOOLS).as_posix() for path in bake.stage_files(stage)}

    def test_a_stage_counts_its_entries_and_what_they_import(self):
        self.assertTrue({"r3d/mesh_import.py", "r3d/light.py", "r3d/__init__.py"} <= self.names("mesh"))
        self.assertTrue({"r3d/appearance_simplify.py", "r3d/fitted_variant.py"} <= self.names("fit"))

    def test_a_stage_stops_at_another_stages_entry_and_at_the_bake_tool(self):
        self.assertFalse({"r3d/fitted_variant.py", "r3d/appearance_simplify.py", "r3d/reference_render.py"}
                         & self.names("mesh"))
        self.assertNotIn("r3d/appearance_simplify.py", self.names("reference"))
        self.assertNotIn("r3d/mesh_import.py", self.names("fit"))
        for stage in bake.STAGES:
            self.assertFalse(any(name.startswith("bake/") for name in self.names(stage)), stage)

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
        self.assertEqual(cached, build_pack.pack_bytes([build_pack.DEFAULT_SEARCH]))

    def test_a_mesh_no_bake_keys_fails_instead_of_taking_the_trees_copy(self):
        found = bake.bakes([build_pack.DEFAULT_SEARCH])
        with tempfile.TemporaryDirectory() as directory:
            cache = pathlib.Path(directory)
            rows = bake.seed(found, cache)
            with mock.patch.object(bake, "read_lock", return_value={row["key"]: row for row in rows}), \
                    mock.patch.object(bake, "bakes_in", return_value=found[1:]), \
                    self.assertRaisesRegex(bake.BakeMissing, "no bake keys these meshes") as raised:
                build_pack.pack_bytes([build_pack.DEFAULT_SEARCH], cache=cache, offline=True)
        self.assertIn(found[0].output.removesuffix(bake.MESH_SUFFIX), str(raised.exception))

    def test_every_key_is_unique(self):
        keys = [found.key for found in bake.bakes([build_pack.DEFAULT_SEARCH])]
        self.assertEqual(len(keys), len(set(keys)))

    def test_a_cold_build_fails_naming_each_bake(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stderr(io.StringIO()) as err:
            code = build_pack.main(["-o", directory, "--from-cache", str(pathlib.Path(directory) / "cache"),
                                    "--offline"])
        self.assertEqual(code, 2)
        for found in bake.bakes([build_pack.DEFAULT_SEARCH]):
            self.assertIn(found.output, err.getvalue())


if __name__ == "__main__":
    unittest.main()
