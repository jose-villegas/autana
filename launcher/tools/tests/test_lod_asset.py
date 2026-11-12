"""LLOD siblings share the C layout and bind to one mesh's cluster count."""
import pathlib
import sys
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d import lod_asset, mesh_asset
from build.c_header import read_define

class LodAssetTests(unittest.TestCase):
    def test_round_trip_and_c_owner(self):
        pairs = [(mesh_asset.LOD_NONE, 0), (0, mesh_asset.LOD_NONE)]
        groups = [(0, 0, 0, 4, 2, 0)]
        blob = lod_asset.encode(pairs, groups)
        self.assertEqual(lod_asset.decode(blob), (pairs, groups))
        self.assertEqual(lod_asset.HEADER.size, read_define(lod_asset.OWNER, "LLOD_HEADER_SIZE"))
        self.assertEqual(lod_asset.name_for_mesh("room"), "room.lod")
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            owner = pathlib.Path(folder) / "format.h"
            import re
            owner.write_text(re.sub(r"(LLOD_HEADER_SIZE\s+)\d+U",
                                    lambda match: match[1] + str(lod_asset.HEADER.size + 1) + "U",
                                    lod_asset.OWNER.read_text()))
            with self.assertRaisesRegex(ValueError, "size"):
                mesh_asset.checked_struct(owner, "LLOD", lod_asset.HEADER_FIELDS)

    def test_header_and_mesh_refusals(self):
        blob = lod_asset.encode([(mesh_asset.LOD_NONE, 0)], [(0, 0, 0, 4, 2, 0)])
        mesh = mesh_asset.BLOB_HEADER.pack(0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0)
        lod_asset.validate_mesh(blob, mesh)
        wrong = mesh_asset.BLOB_HEADER.pack(0, 0, 2, 0, 1, 0, 0, 0, 0, 0, 0)
        with self.assertRaisesRegex(ValueError, "cluster"):
            lod_asset.validate_mesh(blob, wrong)
        for bad in (blob[:-1], blob + b"x", lod_asset.HEADER.pack(lod_asset.VERSION + 1, 1, 1) + blob[lod_asset.HEADER.size:],
                    lod_asset.HEADER.pack(lod_asset.VERSION, 0, 1)):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                lod_asset.decode(bad)

    def test_fit_and_levels_keys_have_separate_dependencies(self):
        from bake import bake
        from test_bake import AnyStart, TOOL_KEYS, fitted_job
        job, scene = fitted_job()
        before = bake.stage_keys(job, scene, TOOL_KEYS, AnyStart("1" * 64))
        changed = bake.stage_keys(job, scene, {**TOOL_KEYS, "levels": "changed"}, AnyStart("1" * 64))
        self.assertEqual(before["fit"], changed["fit"])
        self.assertNotEqual(before["levels"], changed["levels"])
        other_bytes = bake.stage_keys(job, scene, TOOL_KEYS, AnyStart("2" * 64))
        self.assertNotEqual(before["levels"], other_bytes["levels"])
        self.assertFalse(any("r3d/lod/" in path.as_posix() for path in bake.stage_files("fit")))

    def test_mesh_bakes_are_bundles_and_fits_have_three_stages(self):
        from bake import bake
        from test_bake import AnyStart
        rows = bake.bakes(starts=AnyStart())
        self.assertTrue(all(row.suffix == ".bundle" for row in rows if row.kind in ("mesh", "fit", "levels")))
        fit = next(row for row in rows if row.kind == "fit")
        self.assertFalse(fit.packed)
        self.assertTrue(fit.output.endswith(".fit.bundle"))
        level = next(row for row in rows if row.kind == "levels")
        self.assertTrue(level.packed)
        self.assertEqual(level.start.kind, "fit")

    def test_plain_writer_emits_only_a_bundle(self):
        import tempfile
        import numpy as np
        from r3d.lit_mesh import write_lit_mesh
        with tempfile.TemporaryDirectory() as folder:
            write_lit_mesh(folder, "card", np.array([[0,0,0], [1,0,0], [0,1,0]]), None,
                           np.array([[0,1,2]]), np.array([0]))
            self.assertEqual([p.name for p in pathlib.Path(folder).iterdir()], ["card.bundle"])

    def test_cpu_levels_follow_a_fit_without_touching_copies(self):
        import tempfile
        from unittest import mock
        from types import SimpleNamespace
        from r3d import mesh_bundle
        from r3d.fit import stage
        from r3d.lit_mesh import write_lit_mesh
        from r3d.lod.levels_stage import levels
        from test_mesh_bundle import bundle_fixture
        from test_r3d_lod_bake import wavy_grid
        with tempfile.TemporaryDirectory() as folder:
            work = pathlib.Path(folder)
            source, _, _, job, scene = bundle_fixture(work)
            entries, copies = mesh_bundle.decode(source)
            write_lit_mesh(work, job.asset_name, *wavy_grid())
            rest, _ = mesh_bundle.decode((work / (job.asset_name + mesh_bundle.SUFFIX)).read_bytes())
            entries = [row for row in entries if row[0] != job.asset_name] + rest
            start = work / (job.asset_name + ".start" + mesh_bundle.SUFFIX)
            start.write_bytes(mesh_bundle.encode(entries, copies))
            job.settings.lod = True
            job.renderer.variant = SimpleNamespace(name="fixture")
            job.renderer.fit = SimpleNamespace(steps=1, batch=1, laplacian=0, normal_weight=0,
                                               surface_weight=0, surface_margin=0, budget=1)
            target = work / (job.asset_name + ".fit" + mesh_bundle.SUFFIX)
            def fit_main(args):
                self.assertNotIn("--lod", args)
                incoming = pathlib.Path(args[args.index("--start") + 1])
                output = pathlib.Path(args[args.index("--out") + 1])
                output.mkdir()
                output.joinpath(incoming.name).write_bytes(incoming.read_bytes())
            with mock.patch.dict(sys.modules, {"r3d.appearance_simplify": SimpleNamespace(main=fit_main)}):
                stage.fit(work / "scene.scene.toml", scene, job, work, start=start, target=target)
            fitted, _ = mesh_bundle.decode(target.read_bytes())
            self.assertNotIn(lod_asset.name_for_mesh(job.asset_name), {name for name, _, _ in fitted})
            final, final_copies = mesh_bundle.decode(levels(target.read_bytes(), job.asset_name, True,
                                                           job.settings.meshlet_triangles))
            self.assertIn(lod_asset.name_for_mesh(job.asset_name), {name for name, _, _ in final})
            self.assertEqual(final_copies, copies)
            self.assertEqual([row for row in fitted if row[0] != job.asset_name],
                             [row for row in final if row[0] not in (job.asset_name, lod_asset.name_for_mesh(job.asset_name))])
            # A fit that kept none of its start's copies levels into the bundle a fit that kept them gives: the
            # copies come from the start.
            seeded = mesh_bundle.encode([row for row in fitted if row[0] == job.asset_name], [])
            kept = levels(target.read_bytes(), job.asset_name, True, job.settings.meshlet_triangles,
                          start=start.read_bytes())
            self.assertEqual(levels(seeded, job.asset_name, True, job.settings.meshlet_triangles,
                                    start=start.read_bytes()), kept)
            self.assertEqual(kept, levels(target.read_bytes(), job.asset_name, True, job.settings.meshlet_triangles))
            # A fit whose kept copy holds an LLOD this tool no longer reads levels the same: only its rest is read.
            from asset.asset_pack import build_pack, parse_pack
            packed = parse_pack(target.read_bytes())
            older = lod_asset.HEADER.pack(lod_asset.VERSION - 1, 1, 0)
            packed[lod_asset.name_for_mesh(copies[0]["mesh"])] = (lod_asset.TYPE, older)
            older_fit = build_pack(sorted((name, kind, blob) for name, (kind, blob) in packed.items()))
            with self.assertRaisesRegex(ValueError, "LLOD version"):
                mesh_bundle.decode(older_fit)
            self.assertEqual(levels(older_fit, job.asset_name, True, job.settings.meshlet_triangles,
                                    start=start.read_bytes()), kept)

    def test_bundle_entry_id_is_part_of_the_mesh_key(self):
        import copy
        from bake import bake
        from test_bake import TOOL_KEYS, fitted_job, AnyStart
        job, scene = copy.deepcopy(fitted_job())
        job.settings.meshes = ()
        first = bake.stage_keys(job, scene, TOOL_KEYS, AnyStart())
        job.asset_name += ".other"
        second = bake.stage_keys(job, scene, TOOL_KEYS, AnyStart())
        self.assertNotEqual(first["start"], second["start"])
