"""Run the raster on a host with observable frame-cost brackets."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


@unittest.skipUnless(shutil.which("gcc"), "needs gcc")
class ResolveTimerTests(unittest.TestCase):
    def test_resolve_is_charged_only_with_a_hook(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            header = work / "util/runtime/frame_cost.h"
            header.parent.mkdir(parents=True)
            header.write_text('#include <string.h>\nextern int charges;\n'
                              '#define FRAME_COST_BEGIN(mark) ((void)0)\n'
                              '#define FRAME_COST_END(mark, name) (charges += !strcmp(name, "r3d.resolve"))\n')
            source = work / "probe.c"
            source.write_text('''#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "render/raster.h"
#include "r3d_quad_mesh.h"
int charges;
static void clear(const raster_attachment_t* a, const raster_t* r, void* pixels, size_t count) {
    memset(pixels, 0, count * 2);
}
static void resolve(const raster_attachment_t* a, const raster_t* r,
                    const gfx_render_target_t* rows, int index) {}
int main(void) {
    const int16_t positions[4][3] = {{-4, -4, 0}, {4, -4, 0}, {4, 4, 0}, {-4, 4, 0}};
    r3d_quad_t quad;
    r3d_quad_init(&quad, positions);
    r3d_instance_t instance = {.mesh = &quad.mesh};
    raster_t r = {.width = 8, .height = 8, .instances = &instance, .instance_count = 1};
    camera_t camera = {.eye = {0, 0, 100}, .forward = {0, 0, -1}, .half_fov_short_tan = .5f, .near_z = 1};
    raster_attachment_t a = {.bytes_per_pixel = 2, .clear = clear, .resolve = resolve};
    const raster_attachment_t* attachments[] = {&a};
    r.attachments = attachments;
    r.attachment_count = 1;
    r.scratch = malloc(raster_scratch_bytes(&r));
    raster_draw(&r, &camera, 0);
    assert(charges == 1);
    a.resolve = NULL;
    raster_draw(&r, &camera, 0);
    assert(charges == 1);
    r.attachment_count = 0;
    raster_draw(&r, &camera, 0);
    assert(charges == 1);
    free(r.scratch);
    return 0;
}
''')
            binary = work / "probe.exe"
            sources = ["render/raster.c", "render/r3d_pipeline.c", "render/r3d_span.c",
                       "render/r3d_lit_mesh.c", "render/upscale.c", "util/runtime/job.c", "asset/asset_pack.c"]
            command = ["gcc", "-std=gnu11", "-ffunction-sections", "-fdata-sections",
                       "-I", str(work), "-I", str(ROOT / "launcher/main"),
                       "-I", str(ROOT / "launcher/test/stubs"), str(source),
                       "-I", str(ROOT / "launcher/test"),
                       *(str(ROOT / "launcher/main" / name) for name in sources),
                       "-Wl,--gc-sections", "-lm", "-o", str(binary)]
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)


if __name__ == "__main__":
    unittest.main()
