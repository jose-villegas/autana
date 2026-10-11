"""Hold the runtime transfer tables to the bake's shading formula."""
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
from r3d.light import encode_srgb8, to_srgb8


class TransferEquality(unittest.TestCase):
    def test_c_tables_and_formula_agree_with_baker(self):
        compiler = subprocess.check_output(
            ["bash", "-c", '. launcher/tools/build/find_cc.sh; c=$(find_cc); command -v "$c"'],
            cwd=ROOT, text=True).strip()
        if sys.platform == "win32":
            compiler = subprocess.check_output(["bash", "-c", 'cygpath -m "$1"', "sh", compiler], text=True).strip()
        with contextlib.ExitStack() as stack:
            temporary = stack.enter_context(tempfile.TemporaryDirectory())
            cache = pathlib.Path(temporary)
            source = cache / "transfer.c"
            source.write_text(r'''#include "render/skin/r3d_skin_light_internal.h"
void probe(float white, float* decode, unsigned char* encode, float* radiance) {
    r3d_skin_light_transfer_t table;
    r3d_skin_light_transfer_build(white, &table);
    for (int i = 0; i < R3D_SKIN_LIGHT_DECODE_SIZE; i++) decode[i] = light_decode(i, &table);
    for (int i = 0; i < R3D_SKIN_LIGHT_RADIANCE_SIZE; i++) {
        int low = i < R3D_SKIN_LIGHT_RADIANCE_LOW_SIZE;
        int shift = low ? R3D_SKIN_LIGHT_RADIANCE_LOW_SHIFT : R3D_SKIN_LIGHT_RADIANCE_SHIFT;
        int bin = low ? i + R3D_SKIN_LIGHT_RADIANCE_LOW_FIRST
                      : i - R3D_SKIN_LIGHT_RADIANCE_LOW_SIZE + R3D_SKIN_LIGHT_RADIANCE_FIRST;
        radiance[i] = light_bits_float(((uint32_t)bin << shift) + (1U << (shift - 1)));
        encode[i] = table.encode[i];
    }
}
void sweep(float white, const float* radiance, int count, unsigned char* encoded) {
    r3d_skin_light_transfer_t table;
    r3d_skin_light_transfer_build(white, &table);
    for (int i = 0; i < count; i++) encoded[i] = light_encode(radiance[i], &table);
}
int min_exp(void) { return R3D_SKIN_LIGHT_RADIANCE_MIN_EXP; }
int max_exp(void) { return R3D_SKIN_LIGHT_RADIANCE_MAX_EXP; }
int decode_size(void) { return R3D_SKIN_LIGHT_DECODE_SIZE; }
int encode_size(void) { return R3D_SKIN_LIGHT_RADIANCE_SIZE; }
float gamma(void) { return R3D_SKIN_LIGHT_GAMMA; }
float byte_scale(void) { return R3D_SKIN_LIGHT_BYTE_SCALE; }
''')
            library = build_shared("transfer", cache, [source, ROOT / "launcher/main/render/skin/r3d_skin_light.c"],
                                   compiler, flags=["-I" + str(ROOT / "launcher/main"),
                                                    "-I" + str(ROOT / "launcher/packages/math/include"),
                                                    "-I" + str(ROOT / "launcher/packages/asset/include")],
                                   libraries=["-lm"], log=lambda _: None)
            if sys.platform == "win32":
                stack.callback(_ctypes.FreeLibrary, library._handle)
            library.gamma.restype = library.byte_scale.restype = ctypes.c_float
            decode = np.empty(library.decode_size(), dtype=np.float32)
            encode = np.empty(library.encode_size(), dtype=np.uint8)
            radiance = np.empty_like(encode, dtype=np.float32)
            library.probe.argtypes = [ctypes.c_float, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
            library.sweep.argtypes = [ctypes.c_float, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
            sweep = np.geomspace(2.0 ** library.min_exp(), 2.0 ** library.max_exp(), 32768).astype(np.float32)
            got = np.empty(len(sweep), dtype=np.uint8)
            albedo = np.arange(len(decode)) / library.byte_scale()
            exact_decode = albedo ** library.gamma()
            np.testing.assert_array_equal(encode_srgb8(exact_decode), np.arange(len(decode)))
            for white in (0, 0.25, 0.5, 1, 2, 4, 16):
                library.sweep(white, sweep.ctypes.data, len(sweep), got.ctypes.data)
                self.assertLessEqual(np.max(np.abs(got.astype(int) - to_srgb8(sweep, white))), 2)
                library.probe(white, decode.ctypes.data, encode.ctypes.data, radiance.ctypes.data)
                np.testing.assert_allclose(decode, exact_decode, rtol=0.004, atol=0)
                self.assertLessEqual(np.max(np.abs(encode.astype(int) - to_srgb8(radiance, white))), 1)
                self.assertLessEqual(np.max(np.abs(encode_srgb8(decode) - np.arange(len(decode)))), 1)


if __name__ == "__main__":
    unittest.main()
