#include <stdlib.h>
#include <string.h>
#include "hash/crc32.h"
#include "image/pixel/rgb565.h"
#include "r3d_quad_mesh.h"
#include "render/contact/contact_blob.h"
#include "render/raster_internal.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

enum { BLOB_WIDTH = 9, BLOB_HEIGHT = 9, BLOB_CENTRE = 4, BLOB_DEPTH = 4 };

#define BLOB_RADIUS 1.0F

static void
draw_round(const r3d_span_target_t* target, const r3d_lens_t* lens, vec3f_t centre, vec3f_t up, float radius,
           float strength) {
    const vec3f_t axis = up.y == 0 ? vec3f_unit_y() : vec3f_unit_x();
    const contact_disc_t disc = {.centre = centre,
                                 .axis = axis,
                                 .inverse_long2 = 1 / (radius * radius),
                                 .inverse_short2 = 1 / (radius * radius),
                                 .strength = strength};
    contact_blob_draw(target, lens, up, &disc, 1);
}

static void
test_blob_depth_radius_rows_strength(void) {
    uint16_t* color = malloc(BLOB_WIDTH * BLOB_HEIGHT * sizeof *color);
    uint16_t* depth = calloc(BLOB_WIDTH * BLOB_HEIGHT, sizeof *depth);
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_NOT_NULL(depth);
    for (int i = 0; i < BLOB_WIDTH * BLOB_HEIGHT; i++) {
        color[i] = RGB565BE(0xffffff);
    }
    const transformf_t pose = transformf_identity();
    render_view_t view = render_view_make(&pose, 1, 1, (viewport_t){BLOB_WIDTH, BLOB_HEIGHT, 0});
    r3d_lens_t lens;
    r3d_lens_init(&lens, &view, 1);
    /* The centre ray passes through the plane's centre at this depth. */
    const vec3f_t ground = {(BLOB_CENTRE + 0.5F - lens.project.cx) * BLOB_DEPTH / lens.focal_x,
                            -(BLOB_CENTRE + 0.5F - lens.project.cy) * BLOB_DEPTH / lens.focal_y, BLOB_DEPTH};
    r3d_span_target_t target = r3d_span_target(color, depth, BLOB_WIDTH, 0, BLOB_HEIGHT);
    draw_round(&target, &lens, ground, vec3f_unit_z(), BLOB_RADIUS, 0);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xffffff), color[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE]);
    depth[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE] = R3D_DEPTH_NEAREST;
    draw_round(&target, &lens, ground, vec3f_unit_z(), BLOB_RADIUS, 1);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xffffff), color[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE]);
    depth[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE] = R3D_DEPTH_EMPTY;
    for (int i = 0; i < BLOB_WIDTH * BLOB_HEIGHT; i++) {
        color[i] = RGB565BE(0xffffff);
    }
    target = r3d_span_target(color + BLOB_CENTRE * BLOB_WIDTH, depth + BLOB_CENTRE * BLOB_WIDTH, BLOB_WIDTH,
                             BLOB_CENTRE, BLOB_CENTRE + 1);
    draw_round(&target, &lens, ground, vec3f_unit_z(), BLOB_RADIUS, 1);
    TEST_ASSERT_EQUAL_HEX16(rgb565be_pack(15, 31, 15), color[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE]);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xffffff), color[0]);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xffffff), color[(BLOB_CENTRE - 1) * BLOB_WIDTH + BLOB_CENTRE]);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xffffff), color[BLOB_CENTRE * BLOB_WIDTH]);
    free(depth);
    free(color);
}

static void
test_blob_squared_falloff(void) {
    static const float distances[] = {0.0F, 0.5F, 1.0F, 1.5F};
    static const int covered[] = {BAYER4_LEVELS, 9, 0, 0};
    uint16_t* color = malloc(BLOB_WIDTH * BLOB_HEIGHT * sizeof *color);
    uint16_t* depth = calloc(BLOB_WIDTH * BLOB_HEIGHT, sizeof *depth);
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_NOT_NULL(depth);
    const transformf_t pose = transformf_identity();
    const render_view_t view = render_view_make(&pose, 1, 1, (viewport_t){BLOB_WIDTH, BLOB_HEIGHT, 0});
    const r3d_span_target_t target = r3d_span_target(color, depth, BLOB_WIDTH, 0, BLOB_HEIGHT);
    for (size_t d = 0; d < sizeof distances / sizeof distances[0]; d++) {
        int count = 0;
        for (int y = 0; y < BAYER4_SIDE; y++) {
            for (int x = 0; x < BAYER4_SIDE; x++) {
                r3d_lens_t lens;
                r3d_lens_init(&lens, &view, 1);
                lens.project.cx = (float)x + 0.5F;
                lens.project.cy = (float)y + 0.5F;
                const vec3f_t ground = {distances[d] * BLOB_RADIUS, 0, BLOB_DEPTH};
                const int pixel = y * BLOB_WIDTH + x;
                color[pixel] = RGB565BE(0xffffff);
                draw_round(&target, &lens, ground, vec3f_unit_z(), BLOB_RADIUS, 1);
                count += color[pixel] != RGB565BE(0xffffff);
            }
        }
        TEST_ASSERT_EQUAL_INT(covered[d], count);
    }
    free(depth);
    free(color);
}

static void
floor_blob(const r3d_span_target_t* target, const r3d_lens_t* lens, void* user) {
    const bool* occluded = user;
    r3d_span_target_t clear = *target;
    if (!*occluded) {
        for (int y = clear.rows.first_row; y < gfx_target_end_row(&clear.rows); y++) {
            uint16_t* depth = gfx_target_depth(&clear.rows, y);
            for (int x = 0; x < lens->width; x++) {
                depth[x] = R3D_DEPTH_EMPTY;
            }
        }
    }
    draw_round(target, lens, vec3f_zero(), vec3f_unit_y(), 2, 1);
}

static void
test_blob_coplanar_floor_matches_unoccluded_disc(void) {
    enum { SIZE = 64, REACH = 20 };

    static const int16_t corners[4][3] = {
        {-REACH, 0, -REACH}, {REACH, 0, -REACH}, {REACH, 0, REACH}, {-REACH, 0, REACH}};
    static const fixture_camera_t camera = {{0, 4, -4}, {0, -1, 1}, 0.62F, 0.1F};
    r3d_quad_t quad;
    r3d_quad_init(&quad, corners);
    const r3d_instance_t instance = {.mesh = &quad.mesh};
    raster_t raster = {.instances = &instance, .instance_count = 1, .width = SIZE, .height = SIZE};
    raster.scratch = malloc(raster_scratch_bytes(&raster));
    uint16_t* expected = malloc(SIZE * SIZE * sizeof *expected);
    TEST_ASSERT_NOT_NULL(raster.scratch);
    TEST_ASSERT_NOT_NULL(expected);
    bool occluded = false;
    const raster_post_t post = {.draw = floor_blob, .user = &occluded};
    raster.post = &post;
    const render_view_t view = render_view_fixture(&camera, &raster, 0);
    (void)raster_draw(&raster, &view);
    memcpy(expected, raster_color(&raster), SIZE * SIZE * sizeof *expected);
    occluded = true;
    (void)raster_draw(&raster, &view);
    const bool same = memcmp(expected, raster_color(&raster), SIZE * SIZE * sizeof *expected) == 0;
    free(expected);
    free(raster.scratch);
    TEST_ASSERT_TRUE(same);
}

static void
test_round_disc_golden(void) {
    enum { SIZE = 32 };

    uint16_t* color = malloc(SIZE * SIZE * sizeof *color);
    uint16_t* depth = calloc(SIZE * SIZE, sizeof *depth);
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_NOT_NULL(depth);
    const transformf_t pose = transformf_identity();
    const render_view_t view = render_view_make(&pose, 1, 1, (viewport_t){SIZE, SIZE, 0});
    r3d_lens_t lens;
    r3d_lens_init(&lens, &view, 1);
    const r3d_span_target_t target = r3d_span_target(color, depth, SIZE, 0, SIZE);
    for (int i = 0; i < SIZE * SIZE; i++) {
        color[i] = RGB565BE(0xffffff);
    }
    draw_round(&target, &lens, (vec3f_t){0, 0, BLOB_DEPTH}, vec3f_unit_z(), BLOB_RADIUS, 1);
    const uint32_t crc = crc32_update(0, color, SIZE * SIZE * sizeof *color);
    free(depth);
    free(color);
    static const uint32_t round_disc_golden = 0xD248B59FU;
    TEST_ASSERT_EQUAL_HEX32(round_disc_golden, crc);
}

static void
test_overlapping_discs_and_elongation(void) {
    enum { SIZE = 32, ALONG_X = 20, ACROSS_Y = 20, MIDDLE = 16 };

    uint16_t* color = malloc(SIZE * SIZE * sizeof *color);
    uint16_t* depth = calloc(SIZE * SIZE, sizeof *depth);
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_NOT_NULL(depth);
    const transformf_t pose = transformf_identity();
    const render_view_t view = render_view_make(&pose, 1, 1, (viewport_t){SIZE, SIZE, 0});
    r3d_lens_t lens;
    r3d_lens_init(&lens, &view, 1);
    const r3d_span_target_t target = r3d_span_target(color, depth, SIZE, 0, SIZE);
    contact_disc_t discs[2] = {
        {.centre = {0, 0, BLOB_DEPTH}, .axis = {1, 0, 0}, .inverse_long2 = 0.25F, .inverse_short2 = 1, .strength = 1}};
    discs[1] = discs[0];
    for (int i = 0; i < SIZE * SIZE; i++) {
        color[i] = RGB565BE(0xffffff);
    }
    contact_blob_draw(&target, &lens, vec3f_unit_z(), discs, 2);
    TEST_ASSERT_EQUAL_HEX16(rgb565be_pack(15, 31, 15), color[MIDDLE * SIZE + MIDDLE]);
    TEST_ASSERT_NOT_EQUAL(RGB565BE(0xffffff), color[MIDDLE * SIZE + ALONG_X]);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xffffff), color[ACROSS_Y * SIZE + MIDDLE]);
    free(depth);
    free(color);
}

void
suite_contact_blob(void) {
    RUN_TEST(test_round_disc_golden);
    RUN_TEST(test_overlapping_discs_and_elongation);
    RUN_TEST(test_blob_depth_radius_rows_strength);
    RUN_TEST(test_blob_squared_falloff);
    RUN_TEST(test_blob_coplanar_floor_matches_unoccluded_disc);
}

SUITE_REGISTER(suite_contact_blob);
