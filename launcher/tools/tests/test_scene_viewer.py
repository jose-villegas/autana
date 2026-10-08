"""The scene-file viewer through the shared harness, using temporary authored assets."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

from PIL import Image
import numpy as np

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "tests"))
from anim_probe import has_compiler, write_camera_clip  # noqa: E402
from r3d.lit_mesh import write_lit_mesh  # noqa: E402

SCRIPT = TOOLS / "render/scenes/scene_viewer_render_host.sh"


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
            '[source]\npath = "card.obj"\ncredit = "fixture"\n[output]\ndirectory = "."\nname = "card"\n')
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

    def test_every_declared_view_name_is_accepted(self):
        header = (TOOLS.parent / "main/render/context/render_context.c").read_text()
        import re
        names = re.findall(r'\{"([a-z]+)", (?:0|sizeof\([^)]*\)), raster_', header)
        self.assertEqual(names, ["depth", "tiles", "motion", "meshlets"])
        for name in ["shaded", *names]:
            with self.subTest(name=name):
                run, out = self.render("--view", name)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertTrue(out.is_file())

    def test_selected_renderer_is_nonblank_and_has_declared_size(self):
        picture = self.pixels("--object", "card")
        self.assertEqual(picture.size, (448, 368))
        colours = dict((rgb, count) for count, rgb in picture.getcolors(picture.width * picture.height))
        self.assertGreater(colours.get((255, 40, 16), 0), 1000, "fixture triangle was not drawn")

    def test_depth_differs_from_shaded(self):
        shaded = self.pixels("--object", "card")
        depth = self.pixels("--object", "card", "--view", "depth")
        self.assertNotEqual(shaded.tobytes(), depth.tobytes())

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

    def test_camera_time_and_resolution_change_pixels(self):
        first = self.pixels("--object", "card")
        later = self.pixels("--object", "card", "--frames", "5")
        small = self.pixels("--object", "card", "--size", "80x64")
        self.assertNotEqual(first.tobytes(), later.tobytes())
        self.assertEqual(small.size, first.size)
        self.assertNotEqual(first.tobytes(), small.tobytes())

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
