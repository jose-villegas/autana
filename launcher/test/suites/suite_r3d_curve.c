#include <stdlib.h>
#include <string.h>

#include "image/pixel/rgb565.h"
#include "render/curve/r3d_curve.h"
#include "render/r3d_pipeline.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#define SIZE                32
#define PIXELS              (SIZE * SIZE)
#define MID                 (SIZE / 2)
#define SCALE               100
#define FAR_Z               500
#define RED                 RGB565BE(0xFF0000)
#define BLUE                RGB565BE(0x0000FF)
#define BACKGROUND          RGB565BE(0x102030)
#define SEGMENT_HALF_LENGTH 40
#define FADE_RADIUS         1
#define WIDE_RADIUS         15
#define TEST_NEAR           1.0F
#define SKIP_KNOTS          10
#define SKIP_DEPTH_FACTOR   10
#define FADE_SHARE          102
#define VIEW_DISTANCE       400
#define ABOVE_AXIS          60

static gfx_color_t* color;
static uint16_t* depth;

static void
cleanup(void) {
    free(color);
    free(depth);
    color = NULL;
    depth = NULL;
}

static r3d_span_target_t
fixture(void) {
    if (color == NULL) {
        color = malloc(PIXELS * sizeof(*color));
        depth = malloc(PIXELS * sizeof(*depth));
    }
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_NOT_NULL(depth);
    for (int i = 0; i < PIXELS; i++) {
        color[i] = BACKGROUND;
        depth[i] = 0;
    }
    return r3d_span_target(color, depth, SIZE, 0, SIZE);
}

static r3d_lens_t
lens(void) {
    r3d_lens_t result = {.m = mat4f_identity(), .width = SIZE, .height = SIZE, .depth_near = TEST_NEAR};
    result.project = (projectf_t){1, -1, MID, MID + 0.5F, TEST_NEAR};
    result.m.m[0][0] = SCALE;
    result.m.m[1][1] = SCALE;
    return result;
}

static void
draw(const r3d_span_target_t* target, const r3d_curve_knot_t* knots, int count) {
    const r3d_curve_t curve = {
        .count = (uint16_t)count, .lo = {INT16_MIN, INT16_MIN, INT16_MIN}, .hi = {INT16_MAX, INT16_MAX, INT16_MAX}};
    const r3d_curve_set_t curves = {&curve, knots, 1};
    const r3d_lens_t camera = lens();
    r3d_curve_draw(&curves, &camera, target);
}

static int
changed(void) {
    int count = 0;
    for (int i = 0; i < PIXELS; i++) {
        count += color[i] != BACKGROUND;
    }
    return count;
}

static void
test_curve_subpixel_fade(void) {
    const r3d_span_target_t target = fixture();
    const r3d_curve_knot_t knots[] = {{-SEGMENT_HALF_LENGTH, 0, FAR_Z, FADE_RADIUS, RED},
                                      {SEGMENT_HALF_LENGTH, 0, FAR_Z, FADE_RADIUS, RED}};
    draw(&target, knots, 2);
    TEST_ASSERT_EQUAL_HEX16(rgb565be_mix(BACKGROUND, RED, FADE_SHARE), color[MID * SIZE + MID]);
    TEST_ASSERT_EQUAL_INT((2 * SEGMENT_HALF_LENGTH * SCALE / FAR_Z) + 1, changed());
    for (int i = 0; i < PIXELS; i++) {
        if (i / SIZE != MID) {
            TEST_ASSERT_EQUAL_HEX16(BACKGROUND, color[i]);
        }
        TEST_ASSERT_EQUAL_UINT16(0, depth[i]);
    }
}

static void
test_curve_wide_segment(void) {
    const r3d_span_target_t target = fixture();
    const r3d_curve_knot_t knots[] = {{-SEGMENT_HALF_LENGTH, 0, FAR_Z, WIDE_RADIUS, RED},
                                      {SEGMENT_HALF_LENGTH, 0, FAR_Z, WIDE_RADIUS, RED}};
    draw(&target, knots, 2);
    for (int y = MID - 2; y <= MID + 2; y++) {
        TEST_ASSERT_EQUAL_HEX16(RED, color[y * SIZE + MID]);
        TEST_ASSERT_GREATER_THAN_UINT16(0, depth[y * SIZE + MID]);
    }
    TEST_ASSERT_EQUAL_HEX16(BACKGROUND, color[(MID - 4) * SIZE + MID]);
}

static void
test_curve_occlusion(void) {
    const r3d_span_target_t target = fixture();
    for (int i = 0; i < PIXELS; i++) {
        depth[i] = R3D_DEPTH_NEAREST;
    }
    const r3d_curve_knot_t knots[] = {{-SEGMENT_HALF_LENGTH, 0, FAR_Z, FADE_RADIUS, RED},
                                      {SEGMENT_HALF_LENGTH, 0, FAR_Z, FADE_RADIUS, RED}};
    draw(&target, knots, 2);
    TEST_ASSERT_EQUAL_INT(0, changed());
    for (int y = 0; y < SIZE; y++) {
        for (int x = MID; x < SIZE; x++) {
            depth[y * SIZE + x] = 0;
        }
    }
    draw(&target, knots, 2);
    TEST_ASSERT_GREATER_THAN_INT(0, changed());
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < MID; x++) {
            TEST_ASSERT_EQUAL_HEX16(BACKGROUND, color[y * SIZE + x]);
        }
    }
}

static void
test_curve_end_on(void) {
    const r3d_span_target_t target = fixture();
    const r3d_curve_knot_t knots[] = {{0, 0, FAR_Z, FADE_RADIUS, RED}, {0, 0, FAR_Z * 2, FADE_RADIUS, RED}};
    draw(&target, knots, 2);
    TEST_ASSERT_GREATER_THAN_INT(0, changed());
    TEST_ASSERT_LESS_OR_EQUAL_INT(2, changed());
    TEST_ASSERT_NOT_EQUAL(RED, color[MID * SIZE + MID - 1]);
}

static void
test_curve_near_crossing(void) {
    const r3d_span_target_t target = fixture();
    const r3d_curve_knot_t knots[] = {{0, 0, -FAR_Z, 0, BLUE}, {SEGMENT_HALF_LENGTH, 0, FAR_Z, FADE_RADIUS, RED}};
    draw(&target, knots, 2);
    TEST_ASSERT_GREATER_THAN_INT(0, changed());
    for (int i = 0; i < PIXELS; i++) {
        if (i / SIZE < MID - 1 || i / SIZE > MID + 1 || i % SIZE < MID) {
            TEST_ASSERT_EQUAL_HEX16(BACKGROUND, color[i]);
        }
    }
}

static void
test_curve_cover_solid_parity(void) {
    const r3d_span_target_t target = fixture();
    gfx_color_t* expected = malloc(PIXELS * sizeof(*expected));
    TEST_ASSERT_NOT_NULL(expected);
    for (int size = 1; size < MID; size++) {
        const r3d_span_vertex_t a = {MID * R3D_SUBPIXEL, MID * R3D_SUBPIXEL, 0.5F, UINT8_MAX, 0, 0};
        r3d_span_vertex_t b = a, c = a;
        b.x += size * R3D_SUBPIXEL;
        c.y += size * R3D_SUBPIXEL;
        (void)fixture();
        r3d_span_triangle_solid(&target, &a, &b, &c, RED);
        memcpy(expected, color, PIXELS * sizeof(*color));
        (void)fixture();
        r3d_span_triangle_cover(&target, &a, &b, &c, R3D_SPAN_COVERAGE_FULL);
        TEST_ASSERT_EQUAL_MEMORY(expected, color, PIXELS * sizeof(*color));
        for (int i = 0; i < PIXELS; i++) {
            TEST_ASSERT_EQUAL_UINT16(0, depth[i]);
        }
    }
    free(expected);
}

static void
test_curve_joint(void) {
    const r3d_span_target_t target = fixture();
    const r3d_curve_knot_t knots[] = {{-SEGMENT_HALF_LENGTH, 0, FAR_Z, WIDE_RADIUS, RED},
                                      {0, 0, FAR_Z, WIDE_RADIUS, RED},
                                      {0, SEGMENT_HALF_LENGTH, FAR_Z, WIDE_RADIUS, RED}};
    draw(&target, knots, 3);
    TEST_ASSERT_EQUAL_HEX16(RED, color[MID * SIZE + MID]);
}

static void
test_curve_skip_knots(void) {
    const r3d_span_target_t target = fixture();
    r3d_curve_knot_t knots[SKIP_KNOTS];
    for (int i = 0; i < SKIP_KNOTS; i++) {
        knots[i] = (r3d_curve_knot_t){(int16_t)i, 0, FAR_Z * SKIP_DEPTH_FACTOR, FADE_RADIUS, RED};
    }
    draw(&target, knots, SKIP_KNOTS);
    gfx_color_t* expected = malloc(PIXELS * sizeof(*expected));
    TEST_ASSERT_NOT_NULL(expected);
    memcpy(expected, color, PIXELS * sizeof(*color));
    for (int i = 0; i < PIXELS; i++) {
        color[i] = BACKGROUND;
    }
    const r3d_curve_knot_t ends[] = {knots[0], knots[SKIP_KNOTS - 1]};
    draw(&target, ends, 2);
    TEST_ASSERT_EQUAL_MEMORY(expected, color, PIXELS * sizeof(*color));
    TEST_ASSERT_GREATER_THAN_INT(0, changed());
    free(expected);
}

/* A curve above the camera's axis draws in the picture's upper rows, as the mesh path puts a vertex there:
 * the lens comes from a real view, not a hand-built one. */
static void
test_curve_above_the_axis_draws_above_the_middle(void) {
    const r3d_span_target_t target = fixture();
    const render_view_t view = render_view_fixture_at(
        &(fixture_camera_t){{0, 0, -VIEW_DISTANCE}, {0, 0, 1}, 0.5F, 1.0F}, (viewport_t){SIZE, SIZE, 0});
    r3d_lens_t camera;
    r3d_lens_init(&camera, &view, 1);
    const r3d_curve_knot_t knots[2] = {{-SEGMENT_HALF_LENGTH, ABOVE_AXIS, 0, WIDE_RADIUS, RED},
                                       {SEGMENT_HALF_LENGTH, ABOVE_AXIS, 0, WIDE_RADIUS, RED}};
    const r3d_curve_t curve = {
        .count = 2, .lo = {INT16_MIN, INT16_MIN, INT16_MIN}, .hi = {INT16_MAX, INT16_MAX, INT16_MAX}};
    const r3d_curve_set_t curves = {&curve, knots, 1};
    r3d_curve_draw(&curves, &camera, &target);
    TEST_ASSERT_GREATER_THAN_INT(0, changed());
    for (int i = MID * SIZE; i < PIXELS; i++) {
        TEST_ASSERT_EQUAL_HEX16(BACKGROUND, color[i]);
    }
}

#undef RUN_TEST
#define RUN_TEST(test)                                                                                                 \
    do {                                                                                                               \
        suite_run_test_timed(test, #test, __LINE__);                                                                   \
        cleanup();                                                                                                     \
    } while (0)

static void
run_r3d_curve_suite(void) {
    RUN_TEST(test_curve_subpixel_fade);
    RUN_TEST(test_curve_wide_segment);
    RUN_TEST(test_curve_occlusion);
    RUN_TEST(test_curve_end_on);
    RUN_TEST(test_curve_near_crossing);
    RUN_TEST(test_curve_cover_solid_parity);
    RUN_TEST(test_curve_joint);
    RUN_TEST(test_curve_skip_knots);
    RUN_TEST(test_curve_above_the_axis_draws_above_the_middle);
}

SUITE_REGISTER(run_r3d_curve_suite);
