"""doc_images.sh's bake copies keep their import file; physical_scene.py
drops only the scene's indirect table and occlusion."""

import collections
import pathlib
import shutil
import subprocess
import sys
import tempfile
import tomllib
import unittest
from unittest import mock
from types import SimpleNamespace

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
DEMO_SCENE = ROOT / "launcher/demo/sponza/sponza.scene.toml"
DEMO_IMPORT = ROOT / "launcher/demo/sponza/sponza.import.toml"
APP_SCRIPT = ROOT / "launcher/tools/render/doc_images_demo.sh"


@unittest.skipUnless(shutil.which("sh"), "needs sh")
class DocImageFailureTest(unittest.TestCase):
    def test_scene_poses_equal_track_host_for_the_video_frame_count(self):
        from anim import track_host
        from anim_probe import write_camera_clip
        from r3d.import_settings import SettingsError, load_scene
        from r3d.mesh_import import fidelity_poses
        from test_r3d_import import FIT, FIT_HEAD, SIMPLIFY, VARIANT, VARIANT_OUTPUT, renderer, sun_object, write_import
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            clip = write_camera_clip(work, name="orbit", reach=2.0, degrees=90.0, node="orbit_view")
            write_import(work, body=SIMPLIFY + VARIANT, output=VARIANT_OUTPUT)
            scene_path = work / "gallery.scene.toml"
            scene_path.write_text(FIT_HEAD + sun_object() + renderer(name="painting", extra=
                'variant = "mesh"\nbake = true\n' + FIT.replace('[184, 224]', '[8, 6]')) +
                renderer(name="frame", extra='variant = "mesh"\nbake = true\n') +
                '[[objects]]\nname = "view"\n[objects.camera]\nhalf_fov_short_tan = 0.62\nnear_z = 6\n'
                'path = { animation = "orbit.anim.toml", node = "orbit_view" }\n')
            scene = load_scene(scene_path)
            for frames in (2, 5):
                with self.subTest(frames=frames):
                    expected = track_host.sample(clip, ["--every", 250, "--until", (frames + 1) * 250,
                                                       "--poses", "orbit_view", 8, 6, "0.62", "6"])
                    self.assertEqual(fidelity_poses(scene, "painting", frames, 250).encode(), expected.encode())
                    self.assertTrue(expected.startswith("size 8 6\nlens 0.62 6\n"), expected)
            with self.assertRaisesRegex(SettingsError, "gallery.scene.toml: frame is not a fitted renderer"):
                fidelity_poses(scene, "frame", 2, 250)

    def test_import_examples_use_the_callers_scene_object_pack_and_timing(self):
        import importlib.util
        path = ROOT / "launcher/tools/render/doc_import_examples.py"
        spec = importlib.util.spec_from_file_location("doc_import_examples", path)
        examples = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(examples)
        scene_path = pathlib.Path("gallery.scene.toml")
        job = SimpleNamespace(object=SimpleNamespace(name="painting"), asset_name="gallery.paint",
                              renderer=SimpleNamespace(variant=SimpleNamespace(name="paint")), bake=True)
        geometry = SimpleNamespace(positions=[], rgb=[], tris=[], tri_double=[], scale={})
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            with mock.patch.object(examples, "load_scene", return_value=SimpleNamespace(renderers=[job])), \
                    mock.patch.object(examples, "bake_geometry", return_value=geometry), \
                    mock.patch.object(examples, "write_lit_mesh"), \
                    mock.patch("r3d.bake_fidelity.pack_bytes", return_value={}) as pack, \
                    mock.patch("r3d.bake_fidelity.write_packs"), \
                    mock.patch.object(examples.subprocess, "run") as run:
                examples.main(scene_path, "painting", pathlib.Path("viewer"), work, work, 3, 17)
            pack.assert_called_once_with([scene_path], [f"gallery.paint={work / 'albedo/paint.bundle'}"])
            args = run.call_args_list[0].args[0]
            for flag, value in (("--scene", "gallery"), ("--object", "painting"), ("--frames", "3"), ("--dt", "17")):
                self.assertEqual(args[args.index(flag) + 1], value)
            self.assertEqual(run.call_args_list[0].kwargs["env"]["AUTANA_ASSET_DIR"], str(work / "albedo/assets"))
            self.assertEqual(run.call_args_list[1].args[0][-1], str(work / "committed-smooth.bmp"),
                             "the albedo render sits beside the committed render of the object")
            source = APP_SCRIPT.read_text()
            call = next(line for line in source.splitlines() if '"launcher/tools/render/doc_import_examples.py"' in line)
            script = (ROOT / "scripts/lib/run.sh").read_text() + \
                '\nR3D_PYTHON=sh\nSCENE=gallery.scene.toml\nFULL=painting\nHOST=viewer\nW=.\nRENDER=out\nFIDELITY_FRAMES=3\nFIDELITY_DT=17\n' + call
            stub = work / "launcher/tools/render/doc_import_examples.py"
            stub.parent.mkdir(parents=True)
            stub.write_text('printf "%s\\n" "$@" > argv\n')
            done = subprocess.run(["sh", "-c", script], cwd=work, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual((work / "argv").read_text().splitlines(),
                             ["gallery.scene.toml", "painting", "viewer", ".", "out", "3", "17"])

    def test_tables_use_the_callers_scene_and_object(self):
        sys.path.insert(0, str(ROOT / "launcher/tools/render"))
        import doc_tables
        values = (1, 2, 3, 4, 5)
        def scores(path):
            return {name: values for name in ("frames mean", "two bounces", "direct light only",
                    "intensity 1", "intensity 2", "intensity 3", "albedo boost 2",
                    "intensity 1 (own reference)", "intensity 2 (own reference)",
                    "intensity 3 (own reference)", "albedo boost 2 (own reference)")}
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            with mock.patch.object(doc_tables, "scores", side_effect=scores) as read:
                doc_tables.write_tables(work, work / "tables", "gallery", "painting")
            self.assertEqual({call.args[0].name for call in read.call_args_list},
                             {"committed-smooth-compare.log", "indirect-compare.log", "indirect-look.log"})
            self.assertEqual({path.name for path in (work / "tables").iterdir()},
                             {f"gallery-{suffix}.md" for suffix in ("fidelity", "indirect", "indirect-look")})
            self.assertEqual([line.split(" | ")[0] for line in (work / "tables/gallery-indirect.md").read_text().splitlines()[2:]],
                             ["| painting, direct light", "| painting, indirect light"])

    def test_score_columns_are_the_sampling_tables(self):
        sys.path.insert(0, str(ROOT / "launcher/tools/render"))
        import doc_tables
        from r3d import bake_fidelity
        self.assertEqual(bake_fidelity.table([]).splitlines()[0], doc_tables.score_table([]).splitlines()[0])

    def test_charts_read_the_tables_as_written_and_refuse_a_renamed_column(self):
        sys.path.insert(0, str(ROOT / "launcher/tools/render"))
        import bake_charts
        import doc_tables
        values = (1.0, 2.0, 0.5, 3.0, 0.75)
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            # Every comparison log scores the same, whatever row write_tables asks it for.
            with mock.patch.object(doc_tables, "scores", return_value=collections.defaultdict(lambda: values)):
                doc_tables.write_tables(work, work / "tables", "gallery", "painting")
            bake_charts.write_charts(work / "tables", work / "render", "gallery")
            self.assertEqual({path.name for path in (work / "render").iterdir()},
                             {f"gallery-{name}.png" for name in ("fidelity", "indirect", "indirect-look")})
            fidelity = work / "tables/gallery-fidelity.md"
            fidelity.write_text(fidelity.read_text().replace("Luma SSIM", "SSIM"))
            with self.assertRaisesRegex(ValueError, "missing columns: Luma SSIM"):
                bake_charts.write_charts(work / "tables", work / "render", "gallery")

    def test_camera_board_table_reads_flat_and_style_sweeps(self):
        import json
        sys.path.insert(0, str(ROOT / "launcher/tools/render"))
        import doc_tables
        mesh = "run_gallery_perf_suite/painting_fitted_full quarter=landscape"
        skin = "run_character_perf_suite/render paused=0 joints=7 quarter=landscape"
        with tempfile.TemporaryDirectory() as directory:
            data = pathlib.Path(directory)
            points = [{"r3d_span.small_max_side": side, "r3d_span.small_flat_spread_far": 16,
                       "r3d_span.small_flat_spread_near": 64} for side in (4, 12)]
            def write_sweep(name, points):
                (data / f"{name}-plan.json").write_text(json.dumps({
                    "points": points, "captures": [{"board": "test-board"}]}))
                rows = [{"build": "test", "point": index, "knobs": knobs,
                         "metrics": {metric: {"mean": value, "observations": [value]}
                                     for metric in (mesh, skin)}}
                        for index, (knobs, value) in enumerate(zip(points, (1000, 900)))]
                (data / f"{name}-sweep.json").write_text(json.dumps({"rows": rows}))
            write_sweep("ceiling-gate", points)
            flat = doc_tables.small_fill_board(data, "gallery", "painting_fitted_full")
            self.assertIn("-10.00%", flat)
            self.assertNotIn("GRAIN", flat)
            points = [{"render_lab.small_fill_ceiling": 12, "render_lab.small_fill_gate": 16,
                       "render_lab.small_fill_style": style} for style in (0, 1)]
            write_sweep("styles", points)
            styles = doc_tables.small_fill_board(data, "gallery", "painting_fitted_full")
            self.assertIn("| 12 | 16 | GRAIN | 0.900 | -10.00% |", styles)
            self.assertEqual(styles.count("| Ceiling |"), 2)
            self.assertEqual(styles.count("<details>"), 2)
            self.assertNotIn(next(iter(points[0])).split(".")[0], styles)


    def test_board_sweep_refuses_multiple_boards(self):
        import json
        sys.path.insert(0, str(ROOT / "launcher/tools/render"))
        import doc_tables
        with tempfile.TemporaryDirectory() as directory:
            data = pathlib.Path(directory)
            (data / "styles-plan.json").write_text(json.dumps({
                "points": [{}], "captures": [{"board": "a"}, {"board": "b"}]}))
            (data / "styles-sweep.json").write_text(json.dumps({"rows": [{
                "build": "test", "point": 0, "knobs": {},
                "metrics": {"frame": {"mean": 1, "observations": [1]}}}]}))
            with self.assertRaisesRegex(ValueError, "one board"):
                doc_tables.sweep_rows(data, "styles")

    def test_small_fill_header_lists_only_selectable_styles(self):
        sys.path.insert(0, str(ROOT / "launcher/tools/render"))
        import doc_tables
        self.assertEqual(doc_tables.small_fill_styles(), ["FLAT", "GRAIN", "DITHER"])  # magic: independent expected enum spellings.

    def test_timing_samples_preserves_requested_identity_fields(self):
        sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
        from layout_measure import timing_samples
        text = ("stage=render cores=2 paused=0 joints=22 quarter=portrait mean_us=123 p95_us=150\n"
                "stage=render cores=1 paused=1 joints=7 quarter=landscape mean_us=90 p95_us=100")
        self.assertEqual(list(timing_samples(
            text, identity_fields=("cores", "paused", "joints"), percentiles=True)), [
                ("render cores=2 paused=0 joints=22 quarter=portrait", 123),
                ("render.p95 cores=2 paused=0 joints=22 quarter=portrait", 150),
                ("render cores=1 paused=1 joints=7 quarter=landscape", 90),
                ("render.p95 cores=1 paused=1 joints=7 quarter=landscape", 100)])

    def test_driver_supplies_all_four_demo_arguments(self):
        source = (ROOT / "launcher/tools/render/render_doc_images.sh").read_text()
        assignments = [line for line in source.splitlines() if line.startswith(("DEMO_SCENE=", "DEMO_OBJECT="))]
        calls = [line for line in source.splitlines() if line.startswith('run bash "$TOOLS_DIR/doc_images_demo.sh"')]
        demo_object = next(line for line in assignments if line.startswith("DEMO_OBJECT=")).split("=", 1)[1]
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            stub = root / "doc_images_demo.sh"
            stub.write_text('printf "%s\\n" "$@" > argv\n')
            script = (ROOT / "scripts/lib/run.sh").read_text() + '\nTOOLS_DIR=.\nOUT=out\nWORK=work\n'
            script += "\n".join(assignments + calls)
            done = subprocess.run(["sh", "-c", script], cwd=root, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual((root / "argv").read_text().splitlines(),
                             ["out", "work/demo", DEMO_SCENE.relative_to(ROOT).as_posix(), demo_object])

    def test_orphans_require_a_demo_prefix_and_a_matching_template(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            tools = root / "launcher/tools/render"
            tools.mkdir(parents=True)
            for name in ("render_doc_images.sh", "doc_images_demo.sh", "doc_import_examples.py"):
                shutil.copyfile(ROOT / "launcher/tools/render" / name, tools / name)
            # The helpers --orphans sources, and the figures runner it asks for declared images.
            for name in ("scripts/lib/run.sh", "scripts/lib/python.sh", "scripts/lib/python-min-version",
                         "launcher/tools/build/packages.sh", "launcher/tools/build/find_cc.py",
                         "launcher/tools/docs/producer.py", "launcher/tools/docs/figures_runner.py",
                         "launcher/tools/render/compare_images.py", "scripts/gates/tracked.py"):
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / name, root / name)
            demo = root / "launcher/demo/gallery/gallery.scene.toml"
            demo.parent.mkdir(parents=True)
            demo.touch()
            images = root / "docs/images/render"
            images.mkdir(parents=True)
            for stem in ("gallery-depth", "gallery-missing", "unknown-depth",
                         "gallery-small-fill-smooth", "gallery-small-fill-fast-diff",
                         "gallery-small-fill-dither-diff", "gallery-small-fill-grain-diff"):
                (images / f"{stem}.gif").touch()
            done = subprocess.run(["sh", str(tools / "render_doc_images.sh"), "--orphans"],
                                  cwd=images, capture_output=True, text=True)
            self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
            self.assertNotIn("orphan render/gallery-depth.gif", done.stdout)
            self.assertNotIn("orphan render/gallery-small-fill-", done.stdout)
            self.assertIn("orphan render/gallery-missing.gif", done.stdout)
            self.assertIn("orphan render/unknown-depth.gif", done.stdout)

    def test_scratch_bake_packs_the_replacement_and_renders_its_object(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            tools = root / "launcher/tools/r3d"
            tools.mkdir(parents=True)
            (tools / "mesh_import.py").write_text('printf "%s\n" "$@" > import.argv\n')
            (tools / "build_pack.py").write_text('printf "%s\n" "$@" > pack.argv\n')
            host = root / "host.sh"
            host.write_text('printf "%s\n" "$@" > render.argv\nprintf "%s\n" "$AUTANA_ASSET_DIR" > assets.env\n')
            host.chmod(0o755)
            (root / "scratch").mkdir()
            source = APP_SCRIPT.read_text()
            function = "bake_and_render() {" + source.split("bake_and_render() {", 1)[1].split("\n}", 1)[0] + "\n}\n"
            script = (ROOT / "scripts/lib/run.sh").read_text() + \
                'PYTHON=sh\nR3D_PYTHON=sh\nHOST=./host.sh\nSCENE=gallery.scene.toml\nID=gallery\nDEMO=.\nFIDELITY_FRAMES=5\nFIDELITY_DT=5000\n' + \
                function + 'bake_and_render gallery.scene.toml scratch gallery.paint painting\n'
            done = subprocess.run(["sh", "-c", script], cwd=root, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            # The scratch folder sits inside the repository, where the importer
            # writes only into a folder it is given.
            imported = (root / "import.argv").read_text().splitlines()
            self.assertEqual(imported[0], "gallery.scene.toml")
            self.assertEqual(imported[imported.index("--out") + 1], "scratch")
            self.assertEqual((root / "pack.argv").read_text().splitlines(),
                             ["-o", "scratch/assets", "gallery.scene.toml", "--replace", "gallery.paint=scratch/gallery.paint.bundle"])
            args = (root / "render.argv").read_text().splitlines()
            self.assertEqual(args[args.index("--scene") + 1], "gallery")
            self.assertEqual(args[args.index("--object") + 1], "painting")
            self.assertEqual((root / "assets.env").read_text().strip(), "scratch/assets")

    def test_every_scratch_bake_gets_a_scene_with_its_import_and_animations_beside_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            # The stand-in for R3D_PYTHON: as mesh_import.py it checks the scene it is
            # given ($2) has its import beside it and reaches every animation it names;
            # anything else (physical_scene.py) runs.
            stand_in = root / "r3d_python.sh"
            stand_in.write_text(
                '#!/bin/sh\n'
                'case "$1" in *mesh_import.py) ;; *) exec "$REAL_PYTHON" "$@" ;; esac\n'
                'folder=$(dirname "$2")\n'
                f'test -f "$folder/{DEMO_IMPORT.name}" || {{ echo "no import beside $2"; exit 7; }}\n'
                'for clip in $(sed -n \'s/.*animation = "\\([^"]*\\)".*/\\1/p\' "$2"); do\n'
                '    case "$clip" in /*|?:/*) path=$clip ;; *) path=$folder/$clip ;; esac\n'
                '    test -f "$path" || { echo "missing $clip"; exit 7; }\n'
                'done\n'
                'echo scene_ready\n', encoding="utf-8")
            stand_in.chmod(0o755)
            source = APP_SCRIPT.read_text(encoding="utf-8")
            functions = "bake_and_render() {" + source.split("bake_and_render() {", 1)[1].split("\nlook_bake smooth", 1)[0]
            start = 'run mkdir -p "$W/start"' + source.split('run mkdir -p "$W/start"', 1)[1].split("\n#", 1)[0]
            work = root / "work"
            script = root / "stage.sh"
            script.write_text(
                f'set -e\nW={work.as_posix()}\nID=sponza\nFULL=atrium_fitted_full\nMESH=sponza.atrium_fitted_full\n'
                f'R3D_PYTHON={stand_in.as_posix()}\nPYTHON=true\nHOST=true\nREAL_PYTHON={pathlib.Path(sys.executable).as_posix()}\n'
                f'export REAL_PYTHON\nDEMO={DEMO_SCENE.parent.relative_to(ROOT).as_posix()}\n'
                f'SCENE={DEMO_SCENE.relative_to(ROOT).as_posix()}\n'
                + (ROOT / "scripts/lib/run.sh").read_text(encoding="utf-8")
                + functions + "\nlook_bake direct direct\n" + start + "\n", encoding="utf-8")
            result = subprocess.run(["sh", script.as_posix()], cwd=ROOT, capture_output=True, text=True)
            for bake in ("indirect-direct", "start"):
                log = work / bake / "bake.log"
                text = log.read_text(encoding="utf-8") if log.exists() else "no bake.log"
                self.assertIn("scene_ready", text, f"{bake}: {text}\n{result.stderr}")
            self.assertEqual(result.returncode, 0, result.stderr)


class PhysicalSceneTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import importlib.util
        path = ROOT / "launcher/tools/render/physical_scene.py"
        spec = importlib.util.spec_from_file_location("physical_scene", path)
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    def test_the_committed_scene_loses_its_indirect_table_and_occlusion_and_nothing_else(self):
        text = DEMO_SCENE.read_text(encoding="utf-8")
        committed, physical = tomllib.loads(text), tomllib.loads(self.module.physical_scene(text))
        self.assertIn("indirect", committed)
        self.assertIn("ao", committed["bake"])
        self.assertNotIn("indirect", physical)
        self.assertNotIn("ao", physical["bake"])
        del committed["indirect"], committed["bake"]["ao"]
        self.assertEqual(physical, committed)

    def test_a_table_right_after_indirect_is_kept(self):
        text = "[indirect]\nintensity = 2.0\n[bake]\nray_offset = 0.5\nao = { distance = 1.0, rays = 2 }\nindirect = { bounces = 1, rays = 4 }\n"
        self.assertEqual(self.module.physical_scene(text), "[bake]\nray_offset = 0.5\nindirect = { bounces = 1, rays = 4 }\n")

    def test_an_ao_key_outside_bake_is_left_alone(self):
        text = "[bake]\nray_offset = 0.5\n\n[[objects]]\nname = \"a\"\nao = 1\n"
        self.assertEqual(self.module.physical_scene(text), text)

    def test_each_look_changes_only_its_option(self):
        text = self.module.physical_scene(DEMO_SCENE.read_text(encoding="utf-8"))
        physical = tomllib.loads(text)
        for name, look in self.module.LOOKS.items():
            with self.subTest(look=name):
                scene = tomllib.loads(self.module.physical_scene(text, look=look))
                self.assertEqual(scene["objects"], physical["objects"])
                self.assertEqual("indirect" not in scene["bake"], look.direct)
                self.assertEqual("ao" in scene["bake"], bool(look.bake))
                self.assertEqual("indirect" in scene, bool(look.table))
                ambient = physical["ambient"]["intensity"] if look.ambient is None else look.ambient
                self.assertEqual(scene["ambient"]["intensity"], ambient)

    def test_every_gpu_light_sheet_names_a_look(self):
        sys.path.insert(0, str(ROOT / "launcher/tools/render"))
        import doc_stages
        named = {sheet.reference for sheet in doc_stages.LIGHT_SHEETS}
        named |= {look for sheet in doc_stages.LIGHT_SHEETS for _label, look in sheet.columns}
        self.assertLessEqual(named, set(self.module.LOOKS))

    def test_a_look_the_scene_cannot_take_is_refused(self):
        with self.assertRaisesRegex(ValueError, "ambient"):
            self.module.physical_scene("[bake]\nindirect = { bounces = 1, rays = 4 }\n", look=self.module.LOOKS["ao-occluded"])

    def test_a_copy_in_another_folder_still_reaches_the_camera_animation(self):
        from r3d.import_settings import load_scene
        from r3d.poses import flight_camera
        scene = DEMO_SCENE
        text = self.module.physical_scene(scene.read_text(encoding="utf-8"), scene.parent)
        with tempfile.TemporaryDirectory() as directory:
            copy = pathlib.Path(directory) / "study" / scene.name
            copy.parent.mkdir()
            copy.write_text(text, encoding="utf-8")
            (copy.parent / DEMO_IMPORT.name).write_text(
                DEMO_IMPORT.read_text(encoding="utf-8"), encoding="utf-8")
            path = flight_camera(load_scene(copy)).path
            self.assertIsNotNone(path)


if __name__ == "__main__":
    unittest.main()
