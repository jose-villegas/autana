#include "suites.h"
#include "unity.h"

#ifndef DEVICE_BUILD
#include <limits.h>
#include <string.h>
#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_test.h"
#include "raster_rig.h"
#include "render/context/render_context.h"
#include "render/view/raster_probes.h"

#define W               64
#define H               48
#define PROBE_Z         (-150.0F)
#define PROBE_SPACING   125.0F
#define PROBE_SCALE     0.001F
#define PROBE_RADIUS    (PROBE_SPACING * RASTER_PROBES_SIZE_DEFAULT / (float)RASTER_PROBES_SIZE_UNIT)
#define PIXEL_RADIUS    8
#define BACKGROUND      RGB565BE(0x123456)
#define OUTPUT_RB_SHIFT (CHAR_BIT - RGB565_GREEN_AT)
#define OUTPUT_G_SHIFT  (CHAR_BIT - (RGB565_RED_AT - RGB565_GREEN_AT))
#define TONEMAP_WHITE   0.35F

static const int16_t COEFFICIENTS[1][SH_PROBES_COMPONENTS] = {
    {1000, 700, 400, 0, 0, 0, -400, 200, 100, 300, -200, 100},
};
static const uint8_t INSIDE = 1;

typedef struct {
    raster_probes_t view;
    sh_probes_t grid;
    r3d_skin_light_transfer_t transfer;
} fixture_t;

static raster_rig_t*
fixture(void) {
    raster_rig_t* r = raster_rig_open(raster_rig_wall_and_card, 1, 0, W, H, sizeof(fixture_t));
    TEST_ASSERT_TRUE(gfx_init());
    suite_add_test_cleanup(gfx_reset_for_test);
    fixture_t* f = (fixture_t*)r->own;
    f->grid = (sh_probes_t){.origin = {0.0F, 0.0F, PROBE_Z},
                            .spacing = {PROBE_SPACING, PROBE_SPACING, PROBE_SPACING},
                            .dims = {1, 1, 1},
                            .scale = PROBE_SCALE,
                            .coefficients = COEFFICIENTS};
    r3d_skin_light_transfer_build(TONEMAP_WHITE, &f->transfer);
    r->raster.light_transfer = &f->transfer;
    r->raster.probes = &f->grid;
    return r;
}

static void
draw(raster_rig_t* r) {
    fixture_t* f = (fixture_t*)r->own;
    raster_attachment_t attachment = raster_probes_view(&f->view);
    const raster_attachment_t* attachments[] = {&attachment};
    raster_rig_attach(r, attachments, 1);
    r->view = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    /* A projected radius of PIXEL_RADIUS makes both the centre and rim exact pixels. */
    r->view.lens.fx = r->view.lens.fy = PIXEL_RADIUS * (PROBE_Z - raster_rig_camera.eye.z) / PROBE_RADIUS;
    raster_draw(&r->raster, &r->view);
    r->raster.attachments = NULL;
    r->raster.attachment_count = 0;
    draw_clear(BACKGROUND);
    raster_probes_overlay(&f->view, W, H);
}

static gfx_color_t
pixel(int x, int y) {
    gfx_surface_t surface;
    TEST_ASSERT_TRUE(gfx_draw_begin(&surface));
    return gfx_target_color(surface.target, y)[x];
}

static gfx_color_t
irradiance(raster_rig_t* r, vec3f_t normal) {
    fixture_t* f = (fixture_t*)r->own;
    const sh1_rgb_t sh = sh_probes_sample(&f->grid, f->grid.origin);
    const sh1_irradiance_t ambient = r3d_skin_light_ambient_sh(&sh);
    const vec3f_t light = sh1_irradiance_eval(&ambient, normal);
    const uint8_t red = r3d_skin_light_encode(light.x, &f->transfer);
    const uint8_t green = r3d_skin_light_encode(light.y, &f->transfer);
    const uint8_t blue = r3d_skin_light_encode(light.z, &f->transfer);
    return rgb565be_pack(red >> OUTPUT_RB_SHIFT, green >> OUTPUT_G_SHIFT, blue >> OUTPUT_RB_SHIFT);
}

static void
test_probe_centre_and_rim_use_skin_irradiance(void) {
    raster_rig_t* r = fixture();
    draw(r);
    TEST_ASSERT_EQUAL_HEX16(irradiance(r, vec3f_scale(r->view.forward, -1.0F)), pixel(W / 2, H / 2));
    TEST_ASSERT_EQUAL_HEX16(irradiance(r, vec3f_unit_x()), pixel(W / 2 + PIXEL_RADIUS, H / 2));
}

static void
test_probe_draws_without_lit_skin(void) {
    raster_rig_t* r = fixture();
    fixture_t* f = (fixture_t*)r->own;
    r->raster.light_transfer = NULL;
    r->raster.tonemap_white = TONEMAP_WHITE;
    draw(r);
    const vec3f_t normal = vec3f_scale(r->view.forward, -1.0F);
    const gfx_color_t tonemapped = pixel(W / 2, H / 2);
    TEST_ASSERT_NOT_EQUAL(BACKGROUND, tonemapped);
    TEST_ASSERT_EQUAL_HEX16(irradiance(r, normal), tonemapped);

    r->raster.tonemap_white = 0.0F;
    r3d_skin_light_transfer_build(r->raster.tonemap_white, &f->transfer);
    draw(r);
    TEST_ASSERT_NOT_EQUAL(tonemapped, pixel(W / 2, H / 2));
    TEST_ASSERT_EQUAL_HEX16(irradiance(r, normal), pixel(W / 2, H / 2));
}

static void
assert_background(void) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            TEST_ASSERT_EQUAL_HEX16(BACKGROUND, pixel(x, y));
        }
    }
}

static void
test_probe_behind_geometry_leaves_picture_unchanged(void) {
    raster_rig_t* r = fixture();
    ((fixture_t*)r->own)->grid.origin.z = -PROBE_Z;
    draw(r);
    assert_background();
}

static void
test_no_grid_leaves_shaded_frame_byte_identical(void) {
    raster_rig_t* r = fixture();
    r->raster.probes = NULL;
    r->view = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    raster_draw(&r->raster, &r->view);
    gfx_color_t* shaded = malloc(W * H * sizeof(*shaded));
    TEST_ASSERT_NOT_NULL(shaded);
    memcpy(shaded, raster_color(&r->raster), W * H * sizeof(*shaded));
    fixture_t* f = (fixture_t*)r->own;
    raster_attachment_t attachment = raster_probes_view(&f->view);
    const raster_attachment_t* attachments[] = {&attachment};
    raster_rig_attach(r, attachments, 1);
    raster_draw(&r->raster, &r->view);
    TEST_ASSERT_EQUAL_MEMORY(shaded, raster_color(&r->raster), W * H * sizeof(*shaded));
    free(shaded);
    draw_clear(BACKGROUND);
    raster_probes_overlay(&f->view, W, H);
    assert_background();
    r->raster.attachments = NULL;
    r->raster.attachment_count = 0;
}

static void
test_probes_debug_view_name_round_trips(void) {
    const int view = render_context_debug_view_named("probes");
    TEST_ASSERT_NOT_EQUAL(RENDER_DEBUG_VIEW_UNKNOWN, view);
    TEST_ASSERT_EQUAL_STRING("probes", render_context_debug_view_name(view));
}

static void
test_inside_probe_is_a_cross_instead_of_a_disc(void) {
    raster_rig_t* r = fixture();
    ((fixture_t*)r->own)->grid.inside = &INSIDE;
    draw(r);
    TEST_ASSERT_EQUAL_HEX16(RASTER_PROBES_INSIDE_COLOR, pixel(W / 2, H / 2));
    TEST_ASSERT_EQUAL_HEX16(RASTER_PROBES_INSIDE_COLOR, pixel(W / 2 + RASTER_PROBES_CROSS_RADIUS_PX, H / 2));
    TEST_ASSERT_EQUAL_HEX16(RASTER_PROBES_INSIDE_COLOR, pixel(W / 2, H / 2 + RASTER_PROBES_CROSS_RADIUS_PX));
    TEST_ASSERT_EQUAL_HEX16(BACKGROUND, pixel(W / 2 + 1, H / 2 + 1));
    TEST_ASSERT_EQUAL_HEX16(BACKGROUND, pixel(W / 2 + PIXEL_RADIUS, H / 2));
}
#endif

void
run_raster_probes_suite(void) {
#ifndef DEVICE_BUILD
    RUN_TEST(test_probe_centre_and_rim_use_skin_irradiance);
    RUN_TEST(test_probe_draws_without_lit_skin);
    RUN_TEST(test_probe_behind_geometry_leaves_picture_unchanged);
    RUN_TEST(test_no_grid_leaves_shaded_frame_byte_identical);
    RUN_TEST(test_probes_debug_view_name_round_trips);
    RUN_TEST(test_inside_probe_is_a_cross_instead_of_a_disc);
#endif
}

SUITE_REGISTER(run_raster_probes_suite);
