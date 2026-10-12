"""Compare an offline flat-patch bake with the fused C runtime kernel."""
import contextlib
import ctypes
import _ctypes
import pathlib
import subprocess
import sys
import tempfile
import unittest

import numpy as np

TOOLS = pathlib.Path(__file__).resolve().parents[1]
ROOT = TOOLS.parents[1]
sys.path.insert(0, str(TOOLS))
from native.shared_library import build_shared
from r3d.light import face_colours, LIGHT_GAMMA
from anim import track_host
from build import packages
from build.c_header import read_define
SKIN_OWNER = TOOLS.parent / "main/render/r3d_skin.h"
NORMAL_SCALE = read_define(SKIN_OWNER, "R3D_SKIN_NORMAL_SCALE")


class PatchEquality(unittest.TestCase):
    def test_baked_patch_matches_runtime_per_vertex_bytes(self):
        compiler = track_host.compiler()
        if sys.platform == "win32":
            compiler = subprocess.check_output(["bash", "-c", 'cygpath -m "$1"', "sh", compiler], text=True).strip()
        with contextlib.ExitStack() as stack:
            cache = pathlib.Path(stack.enter_context(tempfile.TemporaryDirectory()))
            main = ROOT / "launcher/main"
            sources = [pathlib.Path(__file__).with_name("skin_light_probe.c"),
                       main / "render/skin/r3d_skin_light.c", main / "render/skin/r3d_skinning.c"]
            sources += list((ROOT / "launcher/packages/math/src").rglob("*.c"))
            library = build_shared("patch", cache, sources, compiler,
                flags=["-I" + str(root) for root in packages.include_roots()], libraries=["-lm"], log=lambda _: None)
            if sys.platform == "win32":
                stack.callback(_ctypes.FreeLibrary, library._handle)
            library.probe_patch.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_float, ctypes.c_void_p]
            albedo = np.tile([64, 128, 192], (3, 1)).astype(np.uint8)
            direction = np.array([0.36, 0.8, 0.48])
            directional = dict(type="directional", direction=direction, color=[1, 0.8, 0.6], intensity=1.7)
            ambient = dict(type="ambient", color=[0.7, 0.8, 1], intensity=0.15)
            values = np.array([*direction, *[v * directional["intensity"] for v in directional["color"]],
                               *[v * ambient["intensity"] for v in ambient["color"]]], dtype=np.float32)
            tolerance = 2
            class OpenPatch:
                def blocked(self, origins, directions):
                    return np.zeros(len(origins), dtype=bool)
            for normal in ([0, 1, 0], [0, -1, 0], [1, 0, 0], [-1, 0, 0], [0, 0, 1]):
                normal = np.array(normal, dtype=np.int8)
                tangent = np.cross(normal, [1, 0, 0] if normal[0] == 0 else [0, 1, 0])
                points = np.array([[0, 0, 0], tangent, np.cross(normal, tangent)], dtype=float)
                encoded_normal = normal * NORMAL_SCALE
                faces = np.array([[0, 1, 2]])
                for intensity in (0.001, 0.01, 0.1, 0.3, 0.7, 1.7, 4.0, 16.0):
                    directional['intensity'] = intensity
                    values[3:6] = np.array(directional['color']) * intensity
                    for white in (0.0, 0.3, 1.0, 4.0, 16.0):
                        with self.subTest(normal=normal.tolist(), intensity=intensity, white=white):
                            linear_albedo = (albedo[0] / 255) ** LIGHT_GAMMA
                            baked = face_colours(points, faces, np.array([0]), [0], set(),
                                lambda p, spacing, material: np.tile(linear_albedo, (len(p), 1)),
                                OpenPatch(), [directional, ambient], 0.001, white)[0]
                            runtime = np.empty_like(albedo)
                            self.assertEqual(library.probe_patch(albedo.ctypes.data, values.ctypes.data,
                                encoded_normal.ctypes.data, white, runtime.ctypes.data), 1)
                            self.assertLessEqual(np.max(np.abs(runtime.astype(int) - baked)), tolerance)


if __name__ == "__main__":
    unittest.main()
