"""The scene-file viewer through the shared harness, using temporary authored assets."""
import csv
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from PIL import Image
import numpy as np

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "tests"))
from anim_probe import has_compiler, write_camera_clip  # noqa: E402
from anim import track_host
from build.c_header import read_define
from r3d import build_pack, import_settings
from r3d.lit_mesh import write_lit_mesh  # noqa: E402
from render import render_compare, scene_path  # noqa: E402

SCRIPT = TOOLS / "render/scenes/scene_viewer_render_host.sh"
REPO = TOOLS.parents[1]


@unittest.skipUnless(has_compiler(), "needs sh and a C compiler")
class SceneViewer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="scene viewer ")
        cls.root = pathlib.Path(cls.temp.name)
        cls.scene = cls.root / "room.scene.toml"
        positions = np.array([[-2, -2, -6], [2, -2, -6], [0, 2, -6]], dtype=float)
        write_lit_mesh(cls.root, "card", positions, np.array([[255, 40, 20]] * 3),
                       np.array([[0, 1, 2]]), np.array([True]))
        (cls.root / "card.import.toml").write_text(
            '[source]\npath = "card.obj"\ncredit = "fixture"\n[output]\nname = "card"\n')
        (cls.root / "card.obj").write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n")
        write_camera_clip(cls.root, reach=2.0)
        cls.scene.write_text('''[[objects]]
name = "card"
[objects.mesh_renderer]
mesh = "card.import.toml"
[[objects]]
name = "offset"
position = [2.0, 0.0, -1.0]
[objects.mesh_renderer]
mesh = "card.import.toml"
[[objects]]
name = "camera"
[objects.camera]
half_fov_short_tan = 0.6
near_z = 1.0
background = 0x204080
path = { animation = "fly.anim.toml", node = "camera" }
''')
        cls.out = cls.root / "build"
        build = subprocess.run([shutil.which("sh"), str(SCRIPT), "-o", str(cls.out), "--build-only",
                                "--asset-file", str(cls.scene)], capture_output=True, text=True, timeout=120)
        if build.returncode:
            raise RuntimeError(build.stdout + build.stderr)
        cls.binary = cls.out / ("scene_viewer_render.exe" if os.name == "nt" else "scene_viewer_render")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def render(self, *args):
        out = self.root / "probe.bmp"
        out.unlink(missing_ok=True)
        env = dict(os.environ, AUTANA_ASSET_DIR=str(self.out / "assets"))
        run = subprocess.run([str(self.binary), "--scene", "room", "--frames", "2", "--dt", "100",
                              *args, "-o", str(out)], capture_output=True, text=True, timeout=120, env=env)
        return run, out

    def pixels(self, *args):
        run, out = self.render(*args)
        self.assertEqual(run.returncode, 0, run.stderr)
        with Image.open(out) as image:
            return image.copy()

    def test_triangle_sizes_source_poses_match_the_runtime_engine_clip_pixels(self):
        clip = write_camera_clip(self.root, name="asymmetric", reach=2.0, degrees=30.0)
        source = self.root / "probe.scene.toml"
        source.write_text(self.scene.read_text().replace('path = { animation = "fly.anim.toml"',
                                                        'path = { animation = "asymmetric.anim.toml"'))
        assets = self.root / "probe-assets"
        build_pack.write_packs(assets, build_pack.pack_bytes([source]))
        env = dict(os.environ, AUTANA_ASSET_DIR=str(assets))
        picture = self.root / "runtime.bmp"
        runtime = subprocess.run([str(self.binary), "--scene", "probe", "--object", "card", "--quarter", "0",
                                  "--size", "368x448", "--frames", "6", "--dt", "100", "-o", str(picture)],
                                 capture_output=True, text=True, timeout=120, env=env)
        self.assertEqual(runtime.returncode, 0, runtime.stderr)
        poses = self.root / "source.poses"
        poses.write_text(track_host.poses(clip, "camera", 100, 368, 448, 0.6, 1.0, until_ms=700))
        frames = self.root / "measured"
        frames.mkdir(exist_ok=True)
        report = TOOLS / "r3d/report_triangle_sizes.sh"
        measured_build = self.root / "triangle-sizes"
        built = subprocess.run([shutil.which("sh"), str(report), "--build-only", "-o", str(measured_build)],
                               capture_output=True, text=True, timeout=120)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        binary = pathlib.Path(built.stdout.strip())
        measured = subprocess.run([str(binary), str(assets / "probe.apak"), "card", str(poses),
                                   "--write", str(frames)], capture_output=True, text=True, timeout=120)
        self.assertEqual(measured.returncode, 0, measured.stdout + measured.stderr)
        raw = np.fromfile(frames / "pose_06.raw", dtype="<u2").reshape(448, 368)
        with Image.open(picture) as image:
            pixels = np.asarray(image)
        runtime_mask = np.any(pixels != pixels[0, 0], axis=2)
        measured_mask = raw != 0
        self.assertGreater(np.count_nonzero(measured_mask), 1000)
        np.testing.assert_array_equal(measured_mask, runtime_mask)

    def test_every_declared_view_name_is_accepted(self):
        run, _ = self.render("--view", "invalid")
        self.assertNotEqual(run.returncode, 0)
        names = run.stderr.split("--view is ", 1)[1].split(", not ", 1)[0].split(", ")
        count = read_define(TOOLS.parent / "main/render/context/render_context.h", "RENDER_DEBUG_VIEW_COUNT")
        self.assertEqual(len(names), count + 1)
        self.assertEqual(names[0], "shaded")
        for name in names:
            with self.subTest(name=name):
                run, out = self.render("--view", name)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertTrue(out.is_file())

    def test_selected_renderer_is_nonblank_and_has_declared_size(self):
        picture = self.pixels("--object", "card")
        self.assertEqual(picture.size, (448, 368))
        colours = dict((rgb, count) for count, rgb in picture.getcolors(picture.width * picture.height))
        self.assertGreater(colours.get((255, 40, 16), 0), 1000, "fixture triangle was not drawn")

    def test_each_view_differs_from_shaded(self):
        run, _ = self.render("--view", "invalid")
        names = run.stderr.split("--view is ", 1)[1].split(", not ", 1)[0].split(", ")
        shaded = self.pixels("--object", "card", "--view", "shaded")
        # The room draws each mesh once, which boxes leaves out unless asked.
        own = {"boxes": ("--set", "boxview.copies=1")}
        for name in names[1:]:
            with self.subTest(name=name):
                picture = self.pixels("--object", "card", "--view", name, *own.get(name, ()))
                self.assertFalse(shaded.tobytes() == picture.tobytes())

    def test_unknown_object_names_it_and_lists_renderers(self):
        run, out = self.render("--object", "missing")
        self.assertNotEqual(run.returncode, 0)
        for word in ("missing", "renderers:", "card", "offset"):
            self.assertIn(word, run.stderr)
        self.assertFalse(out.exists())

    def test_missing_scene_id_names_it(self):
        run, out = self.render("--scene", "absent")
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("absent", run.stderr)
        self.assertFalse(out.exists())

    def test_filter_and_repeated_objects_match_authored_render(self):
        authored = self.pixels()
        both = self.pixels("--object", "card", "--object", "offset")
        card = self.pixels("--object", "card")
        self.assertEqual(authored.tobytes(), both.tobytes())
        self.assertNotEqual(authored.tobytes(), card.tobytes())

    def hierarchy_pixels(self, *args):
        """Renders `kid` under `rig` beside an unrelated `other`; the scene TOML has no parent key yet."""
        source = self.root / "family.scene.toml"
        mesh = '[objects.mesh_renderer]\nmesh = "card.import.toml"\n'
        source.write_text(
            f'[[objects]]\nname = "rig"\n{mesh}'
            f'[[objects]]\nname = "kid"\nparent = "rig"\nposition = [2.0, 0.0, -1.0]\n{mesh}'
            f'[[objects]]\nname = "other"\nposition = [-2.0, 0.0, -1.0]\n{mesh}'
            '[[objects]]\nname = "camera"\n[objects.camera]\nhalf_fov_short_tan = 0.6\nnear_z = 1.0\n'
            'background = 0x204080\npath = { animation = "fly.anim.toml", node = "camera" }\n')
        load_object = import_settings.load_object

        def with_parent(value, base, where):
            obj = load_object({key: item for key, item in value.items() if key != "parent"}, base, where)
            obj.parent = value.get("parent")
            return obj

        assets = self.root / "family-assets"
        with mock.patch.object(import_settings, "load_object", with_parent):
            build_pack.write_packs(assets, build_pack.pack_bytes([source]))
        out = self.root / "family.bmp"
        run = subprocess.run([str(self.binary), "--scene", "family", "--frames", "2", "--dt", "100", *args,
                              "-o", str(out)], capture_output=True, text=True, timeout=120,
                             env=dict(os.environ, AUTANA_ASSET_DIR=str(assets)))
        self.assertEqual(run.returncode, 0, run.stderr)
        with Image.open(out) as image:
            return image.tobytes()

    def test_named_parent_brings_its_children_and_named_child_its_ancestors(self):
        parent = self.hierarchy_pixels("--object", "rig")
        self.assertEqual(parent, self.hierarchy_pixels("--object", "kid"))
        self.assertEqual(parent, self.hierarchy_pixels("--object", "rig", "--object", "kid"))
        self.assertNotEqual(parent, self.hierarchy_pixels("--object", "other"))
        self.assertNotEqual(parent, self.hierarchy_pixels())

    def test_camera_time_and_resolution_change_pixels(self):
        first = self.pixels("--object", "card")
        later = self.pixels("--object", "card", "--frames", "5")
        small = self.pixels("--object", "card", "--size", "80x64")
        self.assertNotEqual(first.tobytes(), later.tobytes())
        self.assertEqual(small.size, first.size)
        self.assertNotEqual(first.tobytes(), small.tobytes())

    def test_small_fill_ceiling_range(self):
        maximum = read_define(TOOLS.parent / "main/render/render_view.h", "SMALL_FILL_CEILING_MAX")
        for ceiling in (0, maximum):
            with self.subTest(ceiling=ceiling):
                run, _ = self.render("--small-fill", f"{ceiling},16,0")
                self.assertEqual(run.returncode, 0, run.stderr)
        for ceiling in (maximum + 1, 255, -1):
            with self.subTest(ceiling=ceiling):
                run, _ = self.render("--small-fill", f"{ceiling},16,0")
                self.assertNotEqual(run.returncode, 0)
                self.assertIn("--small-fill", run.stderr)

    def test_unknown_camera_and_invalid_options_fail(self):
        for args, word in ((["--camera", "absent"], "absent"), (["--view", "invalid"], "invalid"),
                           (["--size", "80x64junk"], "--size"), (["--object"], "needs a value")):
            with self.subTest(args=args):
                run, _ = self.render(*args)
                self.assertNotEqual(run.returncode, 0)
                self.assertIn(word, run.stderr)

    def test_panel_portrait_and_video_share_harness_options(self):
        for args in (("--panel",), ("--quarter", "0")):
            self.assertEqual(self.pixels(*args).size, (368, 448))
        video = self.root / "clip.avi"
        self.pixels("--object", "card", "--video", str(video))
        self.assertEqual(video.read_bytes()[:4], b"RIFF")

    def test_wrapper_build_only_uses_the_requested_directory_and_never_renders(self):
        out = self.root / "build only"
        wrapper = TOOLS / "render/scene_viewer.sh"
        done = subprocess.run([shutil.which("sh"), str(wrapper), str(self.scene), "--build-only", "-o", str(out)],
                              cwd=self.root, capture_output=True, text=True, timeout=120)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        binary = out / self.binary.name
        self.assertTrue(binary.is_file())
        self.assertEqual(done.stdout.replace("\\", "/").splitlines(), ["built " + binary.as_posix()])
        self.assertFalse(list(out.rglob("*.bmp")))
        self.assertFalse((self.root / "out.bmp").exists())

    def test_wrapper_builds_the_pack_from_a_scene_outside_the_repository(self):
        target = self.root / "wrapper.bmp"
        wrapper = TOOLS / "render/scene_viewer.sh"
        env = dict(os.environ, AUTANA_ASSET_DIR=str(self.root / "absent"))
        run = subprocess.run([shutil.which("sh"), str(wrapper), self.scene.name, "--object", "card",
                              "--frames", "2", "-o", str(target)], cwd=self.root, env=env,
                             capture_output=True, text=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stderr)
        with Image.open(target) as image:
            self.assertEqual(image.size, (448, 368))
            self.assertGreater(dict((rgb, count) for count, rgb in image.getcolors(448 * 368)).get((255, 40, 16), 0),
                               1000)

    def test_poses_follow_the_camera_one_row_per_frame(self):
        rows = self.root / "poses.csv"
        run, _ = self.render("--poses", str(rows), "--frames", "3")
        self.assertEqual(run.returncode, 0, run.stderr)
        with open(rows) as handle:
            poses = list(csv.DictReader(handle))
        self.assertEqual([pose["ms"] for pose in poses], ["0", "100", "200"])
        self.assertNotEqual([poses[0][axis] for axis in ("px", "py", "pz")],
                            [poses[2][axis] for axis in ("px", "py", "pz")])
        for pose in poses:
            self.assertAlmostEqual(float(np.linalg.norm([float(pose[q]) for q in ("qx", "qy", "qz", "qw")])), 1.0,
                                   places=5)

    def sides(self, b_args="--object\ncard"):
        assets = str(self.out / "assets")
        return [scene_path.Side("all", self.binary, assets, "--scene\nroom"),
                scene_path.Side("card", self.binary, assets, "--scene\nroom\n" + b_args)]

    def test_frame_n_is_frame_n_of_the_rendered_path_on_both_sides(self):
        work = self.root / "path"
        work.mkdir(exist_ok=True)
        sides = self.sides()
        scene_path.render(sides, 4, 100, work)
        written = scene_path.write_frame(sides, 2, 4, 100, work, self.root / "frame.png")
        self.assertEqual([path.name for path in written], ["frame.png", "frame-b.png"])
        pictures = []
        for name, path in zip(scene_path.SIDES, written):
            frames = list(render_compare.read_video(work / f"{name}.avi")[1])
            self.assertEqual(len(frames), 4)
            with Image.open(path) as image:
                pictures.append(np.asarray(image.convert("RGB")))
            np.testing.assert_array_equal(pictures[-1], frames[2])
        self.assertFalse(np.array_equal(pictures[0], pictures[1]), "the offset card should pop out on side b")

    def test_a_frame_past_the_path_end_is_refused(self):
        with self.assertRaises(SystemExit) as refusal:
            scene_path.write_frame(self.sides(), 4, 4, 100, self.root, self.root / "late.png")
        self.assertIn("frames 0 to 3", str(refusal.exception))

    def test_default_timing_is_the_viewers_frame_time_over_the_whole_path(self):
        work = self.root / "timing"
        work.mkdir(exist_ok=True)
        side = self.sides()[0]
        run, _ = self.render("--frames", "1")
        duration_ms = int(re.search(r"camera path (\d+) ms", run.stderr).group(1))
        dt_default = int(re.search(r"\.dt_ms = (\d+)", (TOOLS / "render/scene_viewer.c").read_text()).group(1))
        self.assertEqual(scene_path.timing(side, work), (duration_ms // dt_default + 1, dt_default))
        self.assertEqual(scene_path.timing(side, work, frames=3, dt_ms=7), (3, 7))

    def wrapper(self, *arguments):
        """scene_viewer.sh on the test scene, run from the repository (--compare needs it)."""
        return subprocess.run([shutil.which("sh"), str(TOOLS / "render/scene_viewer.sh"), str(self.scene), *arguments],
                              cwd=REPO, capture_output=True, text=True, timeout=300)

    @unittest.skipUnless(shutil.which("ffmpeg"), "needs ffmpeg")
    def test_wrapper_compares_this_checkout_against_itself_with_other_arguments(self):
        out = self.root / "compare.mp4"
        run = self.wrapper("--compare", ".", "--b-args", "--object card", "--frames", "3", "--dt", "100", "-o", str(out))
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("video: 3 frames", run.stderr)
        self.assertGreater(out.stat().st_size, 0)

    def test_wrapper_frame_writes_both_sides(self):
        out = self.root / "wrapper frame.png"
        run = self.wrapper("--compare", ".", "--b-args", "--object card", "--dt", "100", "--frame", "1", "-o", str(out))
        self.assertEqual(run.returncode, 0, run.stderr)
        for path in (out, out.with_name("wrapper frame-b.png")):
            with Image.open(path) as image:
                self.assertEqual(image.size, (448, 368))

    def test_wrapper_refuses_b_args_without_compare(self):
        run = self.wrapper("--b-args", "--object card")
        self.assertEqual(run.returncode, 2)
        self.assertIn("--b-args needs --compare", run.stderr)

    def test_packer_replaces_a_scratch_mesh_for_one_render_only(self):
        scratch = self.root / "scratch mesh"
        scratch.mkdir()
        positions = np.array([[-2, -2, -6], [2, -2, -6], [0, 2, -6]], dtype=float)
        write_lit_mesh(scratch, "replacement", positions, np.array([[16, 40, 255]] * 3),
                       np.array([[0, 1, 2]]), np.array([True]))
        original = (self.root / "card.mesh").read_bytes()
        out = self.root / "replacement host"
        built = subprocess.run([sys.executable, str(TOOLS / "r3d/build_pack.py"), "-o", str(out / "assets"),
                                str(self.scene), "--replace", f"card={scratch / 'replacement.mesh'}"],
                               capture_output=True, text=True, timeout=120)
        self.assertEqual(built.returncode, 0, built.stderr)
        target = out / "frame.bmp"
        env = dict(os.environ, AUTANA_ASSET_DIR=str(out / "assets"))
        rendered = subprocess.run([str(self.binary), "--scene", "room",
                                   "--object", "card", "--frames", "2", "-o", str(target)],
                                  env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(rendered.returncode, 0, rendered.stderr)
        with Image.open(target) as image:
            self.assertGreater(dict((rgb, count) for count, rgb in image.getcolors(448 * 368)).get((16, 40, 255), 0),
                               1000)
        self.assertEqual((self.root / "card.mesh").read_bytes(), original)

        original_run, original_frame = self.render("--object", "card")
        self.assertEqual(original_run.returncode, 0, original_run.stderr)
        with Image.open(original_frame) as image:
            self.assertGreater(dict((rgb, count) for count, rgb in image.getcolors(448 * 368)).get((255, 40, 16), 0), 1000)
