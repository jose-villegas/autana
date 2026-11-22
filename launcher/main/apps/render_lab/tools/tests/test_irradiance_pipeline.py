"""Exercise host pose export, triangle reference tracing and masked dE76 sheets."""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[6]
TOOLS = ROOT / "launcher/tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.skin_reference import posed_skin, source_frame, SKIN_COVERAGE
from r3d.skin.skin_frame import EngineSkin
from r3d import scene_asset
from anim import tracks_asset
from asset.asset_pack import build_pack
from asset.engine_frame import AFFINE_SIGNS, to_engine
from build.find_cc import host_cc
from build.c_header import read_define
from tests.skin_probe import POSITION_SCALE, rig_glb
from tests.r3d_env import needs_mitsuba
from irradiance_sheet import compose
from irradiance_figures import reference


FIXTURE = pathlib.Path(__file__).with_name("pose_export_host.c")
POSE_TIME = read_define(FIXTURE, "POSE_TIME_MS") / 1000
POSE_SCALE = read_define(FIXTURE, "POSE_SCALE")
POSE_AT = [read_define(FIXTURE, "POSE_" + axis) for axis in "XYZ"]


class IrradiancePipelineTests(unittest.TestCase):
    @needs_mitsuba
    @unittest.skipUnless(host_cc(), "needs a host compiler")
    def test_seeked_host_pose_reference_and_sheet(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            model = work / "triangle.glb"
            model.write_bytes(rig_glb())
            with patch("r3d.skin.skin_frame.program", return_value=None):
                engine = EngineSkin(model, POSITION_SCALE, work)
            subject = SimpleNamespace(name="subject", kind="mesh_renderer", position=(0, 0, 0),
                                      matrix=np.eye(3).tolist())
            expected = np.diag([POSE_SCALE] * 3 + [1.])
            expected[:3, 3] = POSE_AT
            reference_world, _, _ = posed_skin(engine.asset, "move", POSE_TIME, (0, 0, 0), 0, 1,
                                               world=source_frame(expected))
            centre = reference_world.mean(axis=0)
            normal = np.cross(reference_world[1] - reference_world[0], reference_world[2] - reference_world[0])
            forward = -normal / np.linalg.norm(normal)
            right = np.cross((0, 1, 0), forward)
            right /= np.linalg.norm(right)
            up = np.cross(forward, right)
            # A rolled camera facing the triangle avoids an edge-on rasterization comparison.
            source_camera = np.eye(4)
            source_camera[:3, :3] = np.column_stack((up, right, -forward))
            source_camera[:3, 3] = centre - forward * 4
            mirror = np.diag(AFFINE_SIGNS)
            engine_camera = mirror @ source_camera @ mirror
            camera = SimpleNamespace(name="camera", kind="camera", position=source_camera[:3, 3].tolist(),
                                     matrix=source_camera[:3, :3].tolist(),
                                     component=SimpleNamespace(half_fov_short_tan=0.5, near_z=0.05,
                                                               background=0, path=None))
            scene = SimpleNamespace(objects=[subject, camera], renderers=[SimpleNamespace(
                object=subject, asset_name="mesh", renderer=None)],
                lights=[{"type": "ambient", "color": (1, 1, 1), "intensity": 1}])
            _, skeleton, skin = engine.variants[4]
            clip = tracks_asset.encode(*tracks_asset.clip_tracks(engine.document, engine.binary,
                                                                  engine.asset.animations["move"]))
            pack = work / "triangle.pack"
            entries = [("mesh", b"LMSH", engine.mesh_bytes), ("skin", b"SKIN", skin),
                       (engine.skeleton_name, b"SKEL", skeleton), ("move", b"TRCK", clip),
                       ("scene", b"SCNE", scene_asset.encode(scene))]
            pack.write_bytes(build_pack([(key, kind, to_engine(kind, data)) for key, kind, data in entries]))
            launcher = TOOLS.parent
            host_script = FIXTURE.parents[1] / "render_lab_render_host.sh"
            source_list = subprocess.run(["sh", str(host_script), "--sources"],
                                         check=True, capture_output=True, text=True).stdout.splitlines()
            app_source = "main/apps/render_lab/app_render_lab.c"
            self.assertIn(app_source, source_list)
            sources = [FIXTURE if item == app_source else launcher / item for item in source_list]
            build = work / "build"
            subprocess.run(["cmake", "-S", str(TOOLS / "render/scene"), "-B", str(build),
                "-G", "Ninja" if shutil.which("ninja") else "MinGW Makefiles" if os.name == "nt" else "Unix Makefiles",
                f"-DCMAKE_C_COMPILER={host_cc()}", "-DSCENE_NAME=pose", f"-DSCENE_OUT_DIR={work}",
                "-DSCENE_SOURCES=" + ";".join(map(str, sources)), "-DSCENE_TARGET=render_host",
                "-DSCENE_PROVIDER=render_lab_provider", "-DSCENE_DEFINES=-DCONFIG_LAUNCHER_DEVELOPMENT=0"],
                check=True, capture_output=True, text=True)
            built = subprocess.run(["cmake", "--build", str(build), "--parallel", "4"], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, (built.stdout + built.stderr)[-6000:])
            program = work / ("pose_render.exe" if os.name == "nt" else "pose_render")
            exported = work / "pose.json"
            result = subprocess.run([str(program), "--scene", "fixture", "--quarter", "1", "--no-hud",
                                     "--frames", "2", "--set", "skin.albedo=1", "--console", "0:seek", "--pose-json", str(exported),
                                     "-o", str(work / "ours.bmp")],
                                    check=True, text=True, capture_output=True,
                                    env=dict(os.environ, AUTANA_POSE_TEST_PACK=str(pack),
                                             AUTANA_POSE_TEST_SKELETON=engine.skeleton_name))
            exported_frames = json.loads(exported.read_text())
            self.assertEqual(len(exported_frames), 2)
            self.assertNotEqual(exported_frames[0]["skins"][0]["time"], POSE_TIME)
            skin_pose, = exported_frames[-1]["skins"]
            camera_pose = exported_frames[-1]["camera"]
            self.assertEqual(skin_pose["clip"], "move")
            self.assertAlmostEqual(skin_pose["time"], POSE_TIME, places=6)
            np.testing.assert_allclose(skin_pose["world"], expected)
            self.assertEqual(camera_pose["lens"], 0.5)
            np.testing.assert_allclose(camera_pose["world"], engine_camera, atol=1e-6)
            (work / "background.obj").write_text(
                "mtllib background.mtl\nv 100 0 0\nv 101 0 0\nv 100 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nusemtl plain\nf 1/1 2/2 3/3\n")
            (work / "background.mtl").write_text("newmtl plain\nKd 1 1 1\n")
            (work / "background.import.toml").write_text(
                '[source]\npath = "background.obj"\ncredit = "Test fixture"\n'
                '[output]\nname = "background"\nposition_scale = 16\n')
            reference_scene = work / "reference.scene.toml"
            reference_scene.write_text(
                'tonemap_white = 1\n[sky]\ncolor = [1, 1, 1]\nintensity = 1\nrays = 1\n'
                '[bake]\nray_offset = 0.001\ncolour_merge_step = 1\n'
                '[[objects]]\nname = "camera"\n[objects.camera]\nhalf_fov_short_tan = 0.5\n'
                'near_z = 0.05\nbackground = 0\n'
                '[[objects]]\nname = "background"\n[objects.mesh_renderer]\n'
                'mesh = "background.import.toml"\nbake = true\n')
            try:
                reference(work, reference_scene, model, "subject", 16, "scalar_rgb")
            except subprocess.CalledProcessError:
                self.fail((work / "reference.log").read_text())
            coverage = np.load(work / SKIN_COVERAGE)
            with Image.open(work / "ours.bmp") as picture:
                host_mask = np.any(np.asarray(picture) > 0, axis=2)
            reference_mask = coverage > 0.5
            overlap = (host_mask & reference_mask).sum() / (host_mask | reference_mask).sum()
            print(f"Host/reference mask overlap: {overlap:.6f}")
            self.assertGreater(overlap, 0.6)
            stats = compose([work], work / "sheet.png")[work.name]
            self.assertGreater(stats["pixels"], 0)
            self.assertTrue(np.isfinite(stats["mean"]))
            self.assertTrue((work / "sheet.png").is_file())


if __name__ == "__main__":
    unittest.main()
