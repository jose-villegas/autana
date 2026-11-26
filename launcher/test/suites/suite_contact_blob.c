#include <math.h>
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
draw_disc_fixture(const r3d_span_target_t* target, const r3d_lens_t* lens, vec3f_t centre, vec3f_t up, float radius,
                  float strength) {
    void* work = malloc(contact_blobs_work_bytes());
    TEST_ASSERT_NOT_NULL(work);
    const vec3f_t axis = fabsf(up.x) < fabsf(up.y) ? vec3f_unit_x() : vec3f_unit_y();
    const vec3f_t u = vec3f_scale(vec3f_normalize(vec3f_cross(up, axis)), radius);
    const vec3f_t v = vec3f_scale(vec3f_normalize(vec3f_cross(up, u)), radius);
    const contact_blob_t blob = {.centre = centre, .u = u, .v = v, .strength = strength};
    contact_blobs_draw(target, lens, &blob, 1, work);
    free(work);
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
    draw_disc_fixture(&target, &lens, ground, vec3f_unit_z(), BLOB_RADIUS, 0);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xffffff), color[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE]);
    depth[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE] = R3D_DEPTH_NEAREST;
    draw_disc_fixture(&target, &lens, ground, vec3f_unit_z(), BLOB_RADIUS, 1);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xffffff), color[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE]);
    depth[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE] = R3D_DEPTH_EMPTY;
    for (int i = 0; i < BLOB_WIDTH * BLOB_HEIGHT; i++) {
        color[i] = RGB565BE(0xffffff);
    }
    target = r3d_span_target(color + BLOB_CENTRE * BLOB_WIDTH, depth + BLOB_CENTRE * BLOB_WIDTH, BLOB_WIDTH,
                             BLOB_CENTRE, BLOB_CENTRE + 1);
    draw_disc_fixture(&target, &lens, ground, vec3f_unit_z(), BLOB_RADIUS, 1);
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
                draw_disc_fixture(&target, &lens, ground, vec3f_unit_z(), BLOB_RADIUS, 1);
                count += color[pixel] != RGB565BE(0xffffff);
            }
        }
        TEST_ASSERT_EQUAL_INT(covered[d], count);
    }
    free(depth);
    free(color);
}

static void
floor_blob(const r3d_span_target_t* target, const r3d_lens_t* lens, void* scratch, void* user) {
    TEST_ASSERT_NOT_NULL(scratch);
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)scratch % _Alignof(max_align_t));
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
    const float radius = 2;
    const contact_blob_t blob = {.u = {0, 0, radius}, .v = {radius, 0, 0}, .strength = 1};
    contact_blobs_draw(target, lens, &blob, 1, scratch);
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
    const size_t geometry_bytes = raster_scratch_bytes(&raster);
    bool occluded = false;
    const raster_post_t post = {.draw = floor_blob, .scratch_bytes = contact_blobs_work_bytes(), .user = &occluded};
    raster.post = &post;
    const size_t scratch_bytes = raster_scratch_bytes(&raster);
    TEST_ASSERT_EQUAL_UINT(mathi_size_ceil(geometry_bytes, _Alignof(max_align_t)) + post.scratch_bytes, scratch_bytes);
    raster.scratch = malloc(scratch_bytes);
    uint16_t* expected = malloc(SIZE * SIZE * sizeof *expected);
    TEST_ASSERT_NOT_NULL(raster.scratch);
    TEST_ASSERT_NOT_NULL(expected);
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

typedef struct {
    uint16_t* color;
    uint16_t* depth;
    void* work;
    r3d_lens_t lens;
    r3d_span_target_t target;
} blob_fixture_t;

static blob_fixture_t
blob_fixture_make(int size) {
    blob_fixture_t f = {.color = malloc(size * size * sizeof *f.color), .depth = calloc(size * size, sizeof *f.depth)};
    f.work = malloc(contact_blobs_work_bytes());
    TEST_ASSERT_NOT_NULL(f.work);
    TEST_ASSERT_NOT_NULL(f.color);
    TEST_ASSERT_NOT_NULL(f.depth);
    const transformf_t pose = transformf_identity();
    const render_view_t view = render_view_make(&pose, 1, 1, (viewport_t){size, size, 0});
    r3d_lens_init(&f.lens, &view, 1);
    f.target = r3d_span_target(f.color, f.depth, size, 0, size);
    for (int i = 0; i < size * size; i++) {
        f.color[i] = RGB565BE(0xffffff);
    }
    return f;
}

static void
test_round_disc_golden(void) {
    enum { SIZE = 32 };

    blob_fixture_t f = blob_fixture_make(SIZE);
    draw_disc_fixture(&f.target, &f.lens, (vec3f_t){0, 0, BLOB_DEPTH}, vec3f_unit_z(), BLOB_RADIUS, 1);
    const uint32_t crc = crc32_update(0, f.color, SIZE * SIZE * sizeof *f.color);
    free(f.work);
    free(f.depth);
    free(f.color);
    static const uint32_t round_disc_golden = 0xD248B59FU;
    TEST_ASSERT_EQUAL_HEX32(round_disc_golden, crc);
}

static void
test_full_mask_rectangle(void) {
    enum { SIZE = 32, MIDDLE = 16 };

    blob_fixture_t f = blob_fixture_make(SIZE);
    contact_mask_t mask = {.coverage = malloc(CONTACT_MASK_BYTES)};
    TEST_ASSERT_NOT_NULL(mask.coverage);
    memset(mask.coverage, UINT8_MAX, CONTACT_MASK_BYTES);
    const contact_blob_t blob = {
        .centre = {0, 0, BLOB_DEPTH}, .u = {1, 0, 0}, .v = {0, 1, 0}, .strength = 1, .mask = &mask};
    contact_blobs_draw(&f.target, &f.lens, &blob, 1, f.work);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            const float s = (x + 0.5F - f.lens.project.cx) * BLOB_DEPTH / f.lens.focal_x;
            const float t = (y + 0.5F - f.lens.project.cy) * BLOB_DEPTH / f.lens.focal_y;
            const bool inside = fabsf(s) < 1 && fabsf(t) < 1;
            TEST_ASSERT_EQUAL_HEX16(inside ? rgb565be_pack(15, 31, 15) : RGB565BE(0xffffff), f.color[y * SIZE + x]);
        }
    }
    free(mask.coverage);
    free(f.work);
    free(f.depth);
    free(f.color);
}

static void
test_mask_last_float_inside_edge(void) {
    enum { SIZE = 1 };

    const float inside = 0.9999999404F;
    blob_fixture_t f = blob_fixture_make(SIZE);
    contact_mask_t mask = {.coverage = calloc(CONTACT_MASK_BYTES, 1)};
    TEST_ASSERT_NOT_NULL(mask.coverage);
    mask.coverage[CONTACT_MASK_BYTES - 1] = UINT8_MAX;
    const contact_blob_t blob = {
        .centre = {-inside, -inside, BLOB_DEPTH}, .u = {1, 0, 0}, .v = {0, 1, 0}, .strength = 1, .mask = &mask};
    contact_blobs_draw(&f.target, &f.lens, &blob, 1, f.work);
    TEST_ASSERT_EQUAL_HEX16(rgb565be_pack(15, 31, 15), f.color[0]);
    free(mask.coverage);
    free(f.color);
    free(f.work);
    free(f.depth);
}

static void
test_multiple_footprints_union_without_double_darkening(void) {
    enum { SIZE = 32, DISCS = 3 };

    static const contact_blob_t blobs[DISCS] = {
        {.centre = {-BLOB_RADIUS, 0, BLOB_DEPTH}, .u = {BLOB_RADIUS, 0, 0}, .v = {0, BLOB_RADIUS, 0}, .strength = 1},
        {.centre = {0, 0, BLOB_DEPTH}, .u = {BLOB_RADIUS, 0, 0}, .v = {0, BLOB_RADIUS, 0}, .strength = 1},
        {.centre = {BLOB_DEPTH, 0, BLOB_DEPTH}, .u = {BLOB_RADIUS, 0, 0}, .v = {0, BLOB_RADIUS, 0}, .strength = 1}};
    blob_fixture_t f = blob_fixture_make(SIZE);
    uint16_t* expected = malloc(SIZE * SIZE * sizeof *expected);
    TEST_ASSERT_NOT_NULL(expected);
    for (int p = 0; p < SIZE * SIZE; p++) {
        expected[p] = RGB565BE(0xffffff);
    }
    for (int i = 0; i < DISCS; i++) {
        for (int p = 0; p < SIZE * SIZE; p++) {
            f.color[p] = RGB565BE(0xffffff);
        }
        contact_blobs_draw(&f.target, &f.lens, &blobs[i], 1, f.work);
        for (int p = 0; p < SIZE * SIZE; p++) {
            if (f.color[p] != RGB565BE(0xffffff)) {
                expected[p] = f.color[p];
            }
        }
    }
    for (int p = 0; p < SIZE * SIZE; p++) {
        f.color[p] = RGB565BE(0xffffff);
    }
    contact_blobs_draw(&f.target, &f.lens, blobs, DISCS, f.work);
    TEST_ASSERT_EQUAL_MEMORY(expected, f.color, SIZE * SIZE * sizeof *expected);
    free(expected);
    free(f.color);
    free(f.work);
    free(f.depth);
}

void
suite_contact_blob(void) {
    RUN_TEST(test_multiple_footprints_union_without_double_darkening);
    RUN_TEST(test_full_mask_rectangle);
    RUN_TEST(test_mask_last_float_inside_edge);
    RUN_TEST(test_round_disc_golden);
    RUN_TEST(test_blob_depth_radius_rows_strength);
    RUN_TEST(test_blob_squared_falloff);
    RUN_TEST(test_blob_coplanar_floor_matches_unoccluded_disc);
}

SUITE_REGISTER(suite_contact_blob);
