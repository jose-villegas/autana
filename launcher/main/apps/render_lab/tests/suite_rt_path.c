/*
 * Portable suite: rt_path.h/.c - the sampler's determinism, the fixed-point
 * running mean, the seed pass against its own direct estimator and its own
 * freedom from per-pixel noise, channel sanity over a sweep of pixels, the
 * light/shadow read the picture must show, the indirect bounce's own
 * colour-bleed contribution measured against a direct-only reference,
 * convergence toward a many-sample reference, and the progressive
 * schedule's own state machine (seed -> accumulate, restart, the
 * allocation-failure fallback). Float results differ in their last bits
 * between x86 and Xtensa, so every assertion here is a tolerance or a
 * relationship, never an exact pixel value - the same rule
 * suite_rt_cornell.c follows.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "apps/render_lab/rt_cornell.h"
#include "apps/render_lab/rt_path.h"
#include "apps/render_lab/rt_refine.h"

static rt_cornell_camera_t
main_camera(int width, int height) {
    rt_cornell_camera_t cam;
    rt_cornell_camera_init(&cam, (r3d_viewport_t){width, height, 0});
    return cam;
}

/* A point in the upright picture, as a fraction of the SHORTER axis's half
 * extent from the centre - mirrors suite_rt_cornell.c's own room_point(),
 * kept as its own copy since a suite exercises one unit and this one needs
 * no more than these few lines of it. */
static void
room_point(int width, int height, float fx, float fy, int* x, int* y) {
    const float half_short = (float)(width < height ? width : height) * 0.5f;
    *x = (int)((float)width * 0.5f + fx * half_short);
    *y = (int)((float)height * 0.5f - fy * half_short);
}

static float
luminance(r3d_vec3f_t c) {
    return c.x + c.y + c.z;
}

/* Determinism */

static void
test_same_pixel_same_sample_index_gives_the_same_colour_twice(void) {
    const rt_cornell_camera_t cam = main_camera(92, 112);
    const r3d_vec3f_t a = rt_path_sample(&cam, 40, 55, 7);
    const r3d_vec3f_t b = rt_path_sample(&cam, 40, 55, 7);

    TEST_ASSERT_EQUAL_FLOAT(a.x, b.x);
    TEST_ASSERT_EQUAL_FLOAT(a.y, b.y);
    TEST_ASSERT_EQUAL_FLOAT(a.z, b.z);
}

static void
test_a_different_sample_index_is_free_to_disagree(void) {
    /* Not a bug if it happens to coincide, but on a scene with a shadow
     * ray, an area light and a cosine bounce it never should - a
     * regression to "the same number every time" is exactly what this
     * catches. */
    const rt_cornell_camera_t cam = main_camera(92, 112);
    const r3d_vec3f_t a = rt_path_sample(&cam, 40, 55, 1);
    const r3d_vec3f_t b = rt_path_sample(&cam, 40, 55, 2);

    TEST_ASSERT_TRUE_MESSAGE(a.x != b.x || a.y != b.y || a.z != b.z,
                             "two different sample indices produced the identical colour");
}

/* Running-mean accumulator */

static void
test_running_mean_is_exact_for_a_constant_sample_stream(void) {
    const r3d_vec3f_t sample = {0.42f, 0.13f, 0.9f};
    rt_path_accum_px_t px = {0, 0, 0};

    rt_path_accum_add(&px, sample, 1);
    const r3d_vec3f_t seeded = rt_path_accum_radiance(px);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, sample.x, seeded.x);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, sample.y, seeded.y);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, sample.z, seeded.z);

    for (uint32_t n = 2; n <= 50; n++) {
        const rt_path_accum_px_t before = px;
        rt_path_accum_add(&px, sample, n);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(before.r, px.r, "a constant stream drifted after the first sample");
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(before.g, px.g, "a constant stream drifted after the first sample");
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(before.b, px.b, "a constant stream drifted after the first sample");
    }
}

static void
test_accum_add_never_stores_a_negative_channel(void) {
    rt_path_accum_px_t px = {0, 0, 0};
    rt_path_accum_add(&px, (r3d_vec3f_t){-1.0f, -0.5f, -100.0f}, 1);

    const r3d_vec3f_t radiance = rt_path_accum_radiance(px);
    TEST_ASSERT_TRUE(radiance.x >= 0.0f && radiance.y >= 0.0f && radiance.z >= 0.0f);
}

/* Seed pass */

static void
test_seeding_a_zeroed_pixel_reproduces_the_direct_estimator(void) {
    const rt_cornell_camera_t cam = main_camera(92, 112);
    const r3d_vec3f_t direct = rt_path_direct_estimate(&cam, 40, 70);

    rt_path_accum_px_t px = {0, 0, 0};
    rt_path_accum_add(&px, direct, 1);
    const r3d_vec3f_t radiance = rt_path_accum_radiance(px);

    TEST_ASSERT_FLOAT_WITHIN(0.05f, direct.x, radiance.x);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, direct.y, radiance.y);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, direct.z, radiance.z);
}

/* Channel sanity over a sweep of pixels */

static bool
is_finite_and_in_range(float v) {
    return isfinite(v) && v >= 0.0f;
}

static void
test_no_nan_negative_or_unbounded_channel_over_a_sweep_of_pixels(void) {
    const int width = 64, height = 48;
    const rt_cornell_camera_t cam = main_camera(width, height);

    for (int y = 3; y < height; y += 7) {
        for (int x = 3; x < width; x += 5) {
            const r3d_vec3f_t direct = rt_path_direct_estimate(&cam, x, y);
            TEST_ASSERT_TRUE_MESSAGE(is_finite_and_in_range(direct.x) && is_finite_and_in_range(direct.y)
                                         && is_finite_and_in_range(direct.z),
                                     "the direct estimate produced a NaN or a negative channel");

            const r3d_vec3f_t sample = rt_path_sample(&cam, x, y, 3);
            TEST_ASSERT_TRUE_MESSAGE(is_finite_and_in_range(sample.x) && is_finite_and_in_range(sample.y)
                                         && is_finite_and_in_range(sample.z),
                                     "a full path sample produced a NaN or a negative channel");

            const r3d_vec3f_t toned = rt_path_tonemap(sample);
            TEST_ASSERT_TRUE_MESSAGE(toned.x >= 0.0f && toned.x < 1.0f && toned.y >= 0.0f && toned.y < 1.0f
                                         && toned.z >= 0.0f && toned.z < 1.0f,
                                     "a tone-mapped channel left [0, 1)");
        }
    }
}

static void
test_neighbouring_pixels_on_a_flat_wall_differ_only_smoothly_in_the_seed_pass(void) {
    /* High enough resolution that one physical pixel is a small step in
     * world space, so a genuinely smooth function reads as a small delta
     * here - a per-pixel random light sample would not. */
    const int width = 200, height = 244;
    const rt_cornell_camera_t cam = main_camera(width, height);
    int x, y;
    room_point(width, height, 0.0f, 0.35f, &x, &y); /* the back wall, flat and unshadowed */

    const r3d_vec3f_t a = rt_path_direct_estimate(&cam, x, y);
    const r3d_vec3f_t b = rt_path_direct_estimate(&cam, x + 1, y);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, a.x, b.x);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, a.y, b.y);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, a.z, b.z);
}

/* Colour bleed */

static rt_cornell_camera_t
straight_down(float x, float z) {
    return (rt_cornell_camera_t){
        .origin = {x, 1.9f, z},
        .forward = {0.0f, -1.0f, 0.0f},
        .right = {1.0f, 0.0f, 0.0f},
        .up = {0.0f, 0.0f, 1.0f},
        .half_fov_short_tan = 0.001f,
        .viewport = {.width = 1, .height = 1, .quarter = 0},
    };
}

/* The mean, over `n` samples, of the full integrator and of the same
 * integrator with every indirect bounce switched off - so a caller can
 * compare what the bounce alone added. */
static void
mean_with_and_without_bounce(float x, float z, int n, r3d_vec3f_t* full, r3d_vec3f_t* direct_only) {
    const rt_cornell_camera_t cam = straight_down(x, z);
    *full = (r3d_vec3f_t){0.0f, 0.0f, 0.0f};
    *direct_only = (r3d_vec3f_t){0.0f, 0.0f, 0.0f};

    for (uint32_t i = 1; i <= (uint32_t)n; i++) {
        const r3d_vec3f_t s = rt_path_sample(&cam, 0, 0, i);
        const r3d_vec3f_t d = rt_path_sample_direct_only(&cam, 0, 0, i);
        *full = r3d_vec3f_add(*full, s);
        *direct_only = r3d_vec3f_add(*direct_only, d);
    }
    *full = r3d_vec3f_scale(*full, 1.0f / (float)n);
    *direct_only = r3d_vec3f_scale(*direct_only, 1.0f / (float)n);
}

static void
test_the_indirect_bounce_tints_the_floor_toward_the_nearest_coloured_wall(void) {
    /* Floor points a short distance from each wall, forward of both boxes
     * (z = 0.2) so neither sits in their shadow - only the wall's own
     * reflected colour should tell the two points apart. */
    const int n = 3000;
    r3d_vec3f_t full, direct_only;

    mean_with_and_without_bounce(-0.85f, 0.2f, n, &full, &direct_only);
    /* Direct-only lighting has no coloured surface between the light and a
     * white floor, so its R/G ratio is the light's own chromaticity,
     * regardless of where on the floor this is measured - proof any shift
     * seen in `full` came from the bounce, not from position-dependent NEE
     * bias. Measured ~1.050 at both this point and the green-wall one. */
    TEST_ASSERT_FLOAT_WITHIN(0.03f, 1.05f, direct_only.x / direct_only.y);
    TEST_ASSERT_TRUE_MESSAGE(full.x / full.y > 1.15f,
                             "a floor point near the red wall must read redder with the bounce than without it");

    mean_with_and_without_bounce(0.85f, 0.2f, n, &full, &direct_only);
    TEST_ASSERT_FLOAT_WITHIN(0.03f, 1.05f, direct_only.x / direct_only.y);
    TEST_ASSERT_TRUE_MESSAGE(full.x / full.y < 1.00f,
                             "a floor point near the green wall must read less red (greener) with the bounce than "
                             "without it");
}

/* Picture sanity: light and shadow */

static void
test_the_light_quad_reads_far_brighter_than_a_wall(void) {
    const int width = 92, height = 112;
    const rt_cornell_camera_t cam = main_camera(width, height);
    int x, y;

    room_point(width, height, 0.0f, 0.67f, &x, &y); /* the ceiling light's own centre */
    const float light_lum = luminance(rt_path_direct_estimate(&cam, x, y));

    room_point(width, height, 0.0f, 0.35f, &x, &y); /* the back wall */
    const float wall_lum = luminance(rt_path_direct_estimate(&cam, x, y));

    TEST_ASSERT_TRUE_MESSAGE(light_lum > wall_lum * 5.0f, "the light quad must read far brighter than a lit wall");
}

static void
test_a_shadowed_floor_point_is_darker_than_a_lit_one_at_the_same_distance(void) {
    /* Same two floor points suite_rt_cornell.c's own shadow test uses: the
     * short box sits between the light and (0.75, 0.10) but not
     * (-0.75, 0.10), which mirrors it in X at the same distance from the
     * light. */
    const rt_cornell_camera_t shadowed_cam = {
        .origin = {0.75f, 1.9f, 0.10f},
        .forward = {0.0f, -1.0f, 0.0f},
        .right = {1.0f, 0.0f, 0.0f},
        .up = {0.0f, 0.0f, 1.0f},
        .half_fov_short_tan = 0.001f,
        .viewport = {.width = 1, .height = 1, .quarter = 0},
    };
    rt_cornell_camera_t lit_cam = shadowed_cam;
    lit_cam.origin.x = -0.75f;

    const float shadowed = luminance(rt_path_direct_estimate(&shadowed_cam, 0, 0));
    const float lit = luminance(rt_path_direct_estimate(&lit_cam, 0, 0));

    TEST_ASSERT_TRUE_MESSAGE(shadowed < lit * 0.5f,
                             "a floor point behind the short box must read markedly darker "
                             "than one at the same distance from the light with a clear view of it");
}

/* Convergence */

static void
test_the_mean_of_many_samples_converges_toward_a_reference(void) {
    const rt_cornell_camera_t cam = main_camera(92, 112);
    int x, y;
    room_point(92, 112, -0.3f, -0.4f, &x, &y); /* an open floor point, lit and shadow-free */

    rt_path_accum_px_t px = {0, 0, 0};
    for (uint32_t n = 1; n <= 400; n++) {
        rt_path_accum_add(&px, rt_path_sample(&cam, x, y, n), n);
    }
    const r3d_vec3f_t reference = rt_path_accum_radiance(px);

    rt_path_accum_px_t early = {0, 0, 0};
    for (uint32_t n = 1; n <= 16; n++) {
        rt_path_accum_add(&early, rt_path_sample(&cam, x, y, n), n);
    }
    const r3d_vec3f_t at_16 = rt_path_accum_radiance(early);

    TEST_ASSERT_FLOAT_WITHIN(0.35f, reference.x, at_16.x);
    TEST_ASSERT_FLOAT_WITHIN(0.35f, reference.y, at_16.y);
    TEST_ASSERT_FLOAT_WITHIN(0.35f, reference.z, at_16.z);
}

/* Progressive schedule */

typedef struct {
    gfx_color_t* fb;
    rt_path_accum_px_t* accum;
    int width, height;
} schedule_fixture_t;

static schedule_fixture_t
schedule_fixture(int width, int height, bool with_accum) {
    schedule_fixture_t f = {
        .fb = malloc(sizeof(*f.fb) * (size_t)width * (size_t)height),
        .accum = with_accum ? calloc((size_t)width * (size_t)height, sizeof(*f.accum)) : NULL,
        .width = width,
        .height = height,
    };
    return f;
}

static void
schedule_fixture_free(schedule_fixture_t* f) {
    free(f->fb);
    free(f->accum);
}

static rt_path_target_t
schedule_target(const schedule_fixture_t* f) {
    return (rt_path_target_t){f->fb, f->accum, f->width, f->height};
}

/* Sizes not a multiple of RT_REFINE_FIRST_STEP, same reason
 * suite_rt_cornell.c's own refinement test picks ragged ones. */
#define SCHED_W 37
#define SCHED_H 23

static void
run_until_seeded(rt_path_schedule_t* sch, const rt_cornell_camera_t* cam, rt_path_target_t target) {
    while (sch->step != 0) {
        rt_path_schedule_advance(sch, cam, target, SCHED_W * SCHED_H);
    }
}

static void
test_the_schedule_seeds_every_pixel_before_it_ever_accumulates(void) {
    const rt_cornell_camera_t cam = main_camera(SCHED_W, SCHED_H);
    schedule_fixture_t f = schedule_fixture(SCHED_W, SCHED_H, true);
    rt_path_schedule_t sch;
    rt_path_schedule_reset(&sch);

    TEST_ASSERT_EQUAL_INT(RT_REFINE_FIRST_STEP, sch.step);
    TEST_ASSERT_EQUAL_UINT32(0, sch.spp);

    run_until_seeded(&sch, &cam, schedule_target(&f));

    TEST_ASSERT_EQUAL_INT(0, sch.step);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, sch.spp, "every pixel must carry exactly one sample once seeding is done");

    schedule_fixture_free(&f);
}

static void
test_a_full_sweep_after_seeding_raises_the_sample_count(void) {
    const rt_cornell_camera_t cam = main_camera(SCHED_W, SCHED_H);
    schedule_fixture_t f = schedule_fixture(SCHED_W, SCHED_H, true);
    rt_path_schedule_t sch;
    rt_path_schedule_reset(&sch);
    run_until_seeded(&sch, &cam, schedule_target(&f));

    TEST_ASSERT_EQUAL_UINT32(1, sch.spp);
    while (sch.spp == 1) {
        rt_path_schedule_advance(&sch, &cam, schedule_target(&f), SCHED_W * SCHED_H);
    }
    TEST_ASSERT_EQUAL_UINT32(2, sch.spp);

    schedule_fixture_free(&f);
}

static void
test_reset_returns_to_the_first_seed_pass_with_the_sample_count_back_at_zero(void) {
    const rt_cornell_camera_t cam = main_camera(SCHED_W, SCHED_H);
    schedule_fixture_t f = schedule_fixture(SCHED_W, SCHED_H, true);
    rt_path_schedule_t sch;
    rt_path_schedule_reset(&sch);
    run_until_seeded(&sch, &cam, schedule_target(&f));
    rt_path_schedule_advance(&sch, &cam, schedule_target(&f), SCHED_W * SCHED_H);

    rt_path_schedule_reset(&sch);
    TEST_ASSERT_EQUAL_INT(RT_REFINE_FIRST_STEP, sch.step);
    TEST_ASSERT_EQUAL_INT(0, sch.seed_y);
    TEST_ASSERT_EQUAL_INT(0, sch.sweep_y);
    TEST_ASSERT_EQUAL_UINT32(0, sch.spp);

    schedule_fixture_free(&f);
}

static void
test_a_traced_span_covers_at_least_one_row_and_never_runs_past_the_bottom(void) {
    const rt_cornell_camera_t cam = main_camera(SCHED_W, SCHED_H);
    schedule_fixture_t f = schedule_fixture(SCHED_W, SCHED_H, true);
    rt_path_schedule_t sch;
    rt_path_schedule_reset(&sch);

    const rt_path_span_t span = rt_path_schedule_advance(&sch, &cam, schedule_target(&f), 1);
    TEST_ASSERT_TRUE(span.y1 > span.y0);
    TEST_ASSERT_TRUE(span.y1 <= SCHED_H);

    schedule_fixture_free(&f);
}

/* Allocation-failure fallback: no accumulator at all. */

static void
test_with_no_accumulator_the_schedule_seeds_once_and_then_does_nothing(void) {
    const rt_cornell_camera_t cam = main_camera(SCHED_W, SCHED_H);
    schedule_fixture_t f = schedule_fixture(SCHED_W, SCHED_H, false);
    rt_path_schedule_t sch;
    rt_path_schedule_reset(&sch);

    run_until_seeded(&sch, &cam, schedule_target(&f));
    TEST_ASSERT_EQUAL_INT(0, sch.step);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, sch.spp, "the fallback never starts accumulating");

    const rt_path_span_t span = rt_path_schedule_advance(&sch, &cam, schedule_target(&f), SCHED_W * SCHED_H);
    TEST_ASSERT_EQUAL_INT_MESSAGE(span.y0, span.y1, "the fallback picture is drawn once and never revisited");

    schedule_fixture_free(&f);
}

static void
test_with_no_accumulator_the_picture_is_not_left_blank(void) {
    const rt_cornell_camera_t cam = main_camera(SCHED_W, SCHED_H);
    schedule_fixture_t f = schedule_fixture(SCHED_W, SCHED_H, false);
    rt_path_schedule_t sch;
    rt_path_schedule_reset(&sch);
    run_until_seeded(&sch, &cam, schedule_target(&f));

    bool saw_non_black = false;
    for (int i = 0; i < SCHED_W * SCHED_H; i++) {
        if (f.fb[i] != 0) {
            saw_non_black = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_non_black, "the fallback render must actually draw the direct-light picture");

    schedule_fixture_free(&f);
}

void
run_rt_path_suite(void) {
    RUN_TEST(test_same_pixel_same_sample_index_gives_the_same_colour_twice);
    RUN_TEST(test_a_different_sample_index_is_free_to_disagree);

    RUN_TEST(test_running_mean_is_exact_for_a_constant_sample_stream);
    RUN_TEST(test_accum_add_never_stores_a_negative_channel);

    RUN_TEST(test_seeding_a_zeroed_pixel_reproduces_the_direct_estimator);
    RUN_TEST(test_neighbouring_pixels_on_a_flat_wall_differ_only_smoothly_in_the_seed_pass);

    RUN_TEST(test_no_nan_negative_or_unbounded_channel_over_a_sweep_of_pixels);
    RUN_TEST(test_the_indirect_bounce_tints_the_floor_toward_the_nearest_coloured_wall);

    RUN_TEST(test_the_light_quad_reads_far_brighter_than_a_wall);
    RUN_TEST(test_a_shadowed_floor_point_is_darker_than_a_lit_one_at_the_same_distance);

    RUN_TEST(test_the_mean_of_many_samples_converges_toward_a_reference);

    RUN_TEST(test_the_schedule_seeds_every_pixel_before_it_ever_accumulates);
    RUN_TEST(test_a_full_sweep_after_seeding_raises_the_sample_count);
    RUN_TEST(test_reset_returns_to_the_first_seed_pass_with_the_sample_count_back_at_zero);
    RUN_TEST(test_a_traced_span_covers_at_least_one_row_and_never_runs_past_the_bottom);

    RUN_TEST(test_with_no_accumulator_the_schedule_seeds_once_and_then_does_nothing);
    RUN_TEST(test_with_no_accumulator_the_picture_is_not_left_blank);
}

SUITE_REGISTER(run_rt_path_suite);
