/*
 * Portable suite: the startup animation's projection, curve, smoothing and
 * timeline, driven by the motion boot loads from the boot clip's pack.
 *
 * boot_anim.h and the generated boot_anim_curve.h are what is tested here.
 * boot_anim.c, which turns all that into gfx calls, is drawn only on a host,
 * to check the rest pose boot holds without the clip is not blank.
 *
 * THE TABLE IS TESTED AGAINST THE MATHEMATICS, NOT AGAINST ITSELF
 *
 * tools/gen/gen_zeta_curve.py already refuses to emit a table unless its own
 * evaluation of zeta checks out, but that proves nothing about the file
 * actually in the repo, which could be stale, hand-edited, or generated with
 * different constants than boot_anim.h now uses.
 *
 * So the shipped numbers are checked directly, and against the one thing that
 * makes this curve worth drawing: it must pass through zero at every known
 * height, and it must NOT come anywhere near zero anywhere else. Those are
 * the nontrivial zeros of the zeta function in the drawn range, and no table of
 * plausible-looking numbers passes both halves by accident.
 */

#include <stdbool.h>
#include <stdint.h>

#include "anim/anim_tracks_format.h"
#include "suites.h"
#include "unity.h"

#include "boot/boot_anim.h"
#include "draw/draw_font_roles.h"

#include <stdlib.h>

#ifndef DEVICE_BUILD
#include <math.h>
#include <stdio.h>

#include "draw/draw_image_format.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_test.h"
#include "image/pixel/rgb565.h"
#include "image/pixel/rgb888.h"
#include "test_anim_tracks.h"
#include "test_asset_dir.h"
#include "testkit/cleanup.h"
#endif

/* The panel these numbers are laid out for, spelled out so a wrong
 * GFX_WIDTH/HEIGHT cannot quietly agree with them. */
#define PANEL_W 368
#define PANEL_H 448

/* The motion boot ships, read from the boot clip's pack as boot reads it;
 * allocated for the suite's run. */
static boot_anim_motion_t* seed;

/* |zeta|^2 in Q24, so nothing here needs a square root. */
static int32_t
mag_sq(const boot_anim_sample_t* s) {
    return (int32_t)s->re * s->re + (int32_t)s->im * s->im;
}

/* A Q12 magnitude, squared, what mag_sq() is compared against. */
static int32_t
threshold_sq(int32_t q12) {
    return q12 * q12;
}

/* The curve */

static void
test_the_curve_climbs_from_zero_to_the_top(void) {
    TEST_ASSERT_EQUAL_INT(0, boot_anim_curve[0].t);
    TEST_ASSERT_EQUAL_INT(BOOT_ANIM_T_MAX << BOOT_ANIM_TQ, boot_anim_curve[BOOT_ANIM_CURVE_POINTS - 1].t);
}

/* t only ever increases. The picture is a climb, and a sample out of order
 * would draw a segment going back down through everything above it. */
static void
test_the_curve_never_descends(void) {
    for (int i = 1; i < BOOT_ANIM_CURVE_POINTS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(boot_anim_curve[i].t >= boot_anim_curve[i - 1].t,
                                 "the curve went back down - the table is out of order");
    }
}

/* THE test. At each of the five listed heights the curve must reach the t
 * axis, which is zeta vanishing there.
 *
 * 0.15 rather than 0 because the table is a sampling: the nearest sample to a
 * crossing sits a little to one side of it. The measured worst case is 0.084,
 * and the check below that this never happens away from a zero is what stops
 * the tolerance being meaningless. */
static void
test_the_curve_meets_the_axis_at_every_known_zero(void) {
    const int32_t window = 1 << BOOT_ANIM_TQ; /* +/- 1.0 in t */
    const int32_t close = threshold_sq((int32_t)(0.15 * BOOT_ANIM_ONE));

    for (int z = 0; z < BOOT_ANIM_ZEROS; z++) {
        int32_t best = INT32_MAX;
        for (int i = 0; i < BOOT_ANIM_CURVE_POINTS; i++) {
            const int32_t dt = boot_anim_curve[i].t - boot_anim_zero_t[z];
            if (dt >= -window && dt <= window) {
                const int32_t m = mag_sq(&boot_anim_curve[i]);
                if (m < best) {
                    best = m;
                }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(best <= close, "the curve does not reach the t axis at one of the heights where "
                                                "zeta is known to vanish");
    }
}

/* The other half, and the one that gives the test above its teeth: away from
 * a zero the curve must stay well clear of the axis. Measured minimum is
 * 0.527, so 0.35 leaves room without letting a flat or collapsed table pass. */
static void
test_the_curve_keeps_away_from_the_axis_everywhere_else(void) {
    const int32_t window = 2 << BOOT_ANIM_TQ; /* +/- 2.0 in t */
    const int32_t clear = threshold_sq((int32_t)(0.35 * BOOT_ANIM_ONE));

    for (int i = 0; i < BOOT_ANIM_CURVE_POINTS; i++) {
        bool near_a_zero = false;
        for (int z = 0; z < BOOT_ANIM_ZEROS; z++) {
            const int32_t dt = boot_anim_curve[i].t - boot_anim_zero_t[z];
            if (dt >= -window && dt <= window) {
                near_a_zero = true;
            }
        }
        if (near_a_zero) {
            continue;
        }
        TEST_ASSERT_TRUE_MESSAGE(mag_sq(&boot_anim_curve[i]) > clear,
                                 "the curve came close to the t axis at a height where zeta does "
                                 "not vanish");
    }
}

static void
test_every_listed_zero_is_on_the_climb_and_in_order(void) {
    for (int z = 0; z < BOOT_ANIM_ZEROS; z++) {
        TEST_ASSERT_TRUE(boot_anim_zero_t[z] > 0);
        TEST_ASSERT_TRUE(boot_anim_zero_t[z] < (BOOT_ANIM_T_MAX << BOOT_ANIM_TQ));
        if (z > 0) {
            /* draw_zeros() stops at the first zero above the pen, which is
             * only correct while the table is sorted. */
            TEST_ASSERT_TRUE_MESSAGE(boot_anim_zero_t[z] > boot_anim_zero_t[z - 1],
                                     "the zeros must be in increasing order");
        }
    }
}

static void
test_samples_are_clamped_rather_than_read_out_of_range(void) {
    const boot_anim_pt_t first = boot_anim_sample(0);
    const boot_anim_pt_t last = boot_anim_sample(BOOT_ANIM_CURVE_POINTS - 1);

    TEST_ASSERT_EQUAL_INT32(first.t, boot_anim_sample(-1).t);
    TEST_ASSERT_EQUAL_INT32(first.re, boot_anim_sample(-99).re);
    TEST_ASSERT_EQUAL_INT32(last.t, boot_anim_sample(BOOT_ANIM_CURVE_POINTS).t);
    TEST_ASSERT_EQUAL_INT32(last.im, boot_anim_sample(BOOT_ANIM_CURVE_POINTS + 99).im);
}

/* The camera */

static void
test_the_quarter_wave_starts_at_zero_and_ends_at_one(void) {
    TEST_ASSERT_EQUAL_INT(0, trig_sin_quarter[0]);
    TEST_ASSERT_EQUAL_INT(TRIG_SIN_MAX, trig_sin_quarter[64]);
}

static void
test_the_quarter_wave_rises_all_the_way(void) {
    for (int i = 1; i < 65; i++) {
        TEST_ASSERT_TRUE_MESSAGE(trig_sin_quarter[i] > trig_sin_quarter[i - 1],
                                 "the quarter wave must increase at every step - a dip means a "
                                 "transposed or mistyped entry");
    }
}

/* The identity that pins every entry down at once, checked across the full
 * turn rather than just the arc the camera actually uses: a general-purpose
 * table should hold everywhere, and this is what makes trig_sin()/cos()
 * safe to reuse for anything else that turns up needing one. */
static void
test_sin_squared_plus_cos_squared_is_one(void) {
    for (uint32_t phase = 0; phase < TRIG_TURN; phase += 7) {
        const int64_t s = trig_sin((uint16_t)phase);
        const int64_t c = trig_cos((uint16_t)phase);
        const int64_t sum = s * s + c * c;
        const int64_t one = (int64_t)TRIG_SIN_MAX * TRIG_SIN_MAX;

        TEST_ASSERT_TRUE_MESSAGE(sum > one - one / 300 && sum < one + one / 300,
                                 "sin^2 + cos^2 left the neighbourhood of 1");
    }
}

static void
test_the_quarter_points_are_exact(void) {
    TEST_ASSERT_EQUAL_INT(0, trig_sin(0));
    TEST_ASSERT_EQUAL_INT(TRIG_SIN_MAX, trig_sin(TRIG_QUARTER_TURN));
    TEST_ASSERT_EQUAL_INT(0, trig_sin(TRIG_TURN / 2));
    TEST_ASSERT_EQUAL_INT(-TRIG_SIN_MAX, trig_sin(3 * TRIG_QUARTER_TURN));
    TEST_ASSERT_EQUAL_INT(TRIG_SIN_MAX, trig_cos(0));
}

/* A moment after the curve finishes (pen saturates at 2500ms) and before
 * the finale starts (2700ms): the view at full progress. */
#define CURVE_DONE_MS 2600

/* The identity transform isolates projection from motion tracks. */
static boot_anim_view_t
identity_view(void) {
    boot_anim_view_t v;
    v.matrix = mat4x_identity();
    for (int i = 0; i < 4; i++) {
        v.matrix.m[i][i] = R3D_X_UNIT_ONE;
    }
    v.lens = (projectx_t){PANEL_W / 2 * MATHX_ONE, PANEL_W / 2 * MATHX_ONE, PANEL_W / 2 * MATHX_ONE,
                          PANEL_H / 2 * MATHX_ONE, mathf_to_x(R3D_LINE_NEAR_Z)};
    v.units_ok = false;
    return v;
}

/* Meters, as a camera-space point in 1/512 m. */
static vec3x_t
vx(float x, float y, float z) {
    return (vec3x_t){mathf_round_i32(x * (float)R3D_X_UNIT_ONE), mathf_round_i32(y * (float)R3D_X_UNIT_ONE),
                     mathf_round_i32(z * (float)R3D_X_UNIT_ONE)};
}

/* An on-axis point in front of the camera lands on the screen centre. */
static void
test_identity_transform_leaves_an_on_axis_point_at_screen_centre(void) {
    const boot_anim_view_t view = identity_view();
    int x, y;
    TEST_ASSERT_TRUE(boot_anim_project_point(0, BOOT_ANIM_ONE, 0, &view, &x, &y));

    TEST_ASSERT_EQUAL_INT_MESSAGE(PANEL_W / 2, x,
                                  "the on-axis point should land exactly on screen centre under an "
                                  "identity transform");
    TEST_ASSERT_EQUAL_INT_MESSAGE(PANEL_H / 2, y,
                                  "the on-axis point should land exactly on screen centre under an "
                                  "identity transform");
}

/* A farther point projects closer to the centre. re/im map to X/Z:
 * im is depth and re is the offset compared at two depths. */
static void
test_a_point_further_from_the_camera_projects_smaller(void) {
    const boot_anim_view_t view = identity_view();
    const int32_t re = 2 * BOOT_ANIM_ONE;
    const int32_t near_im = 1 * BOOT_ANIM_ONE;
    const int32_t far_im = 4 * BOOT_ANIM_ONE;
    int x_near, y_near, x_far, y_far;

    TEST_ASSERT_TRUE(boot_anim_project_point(re, near_im, 0, &view, &x_near, &y_near));
    TEST_ASSERT_TRUE(boot_anim_project_point(re, far_im, 0, &view, &x_far, &y_far));

    const int centre = PANEL_W / 2;
    const int near_offset = (x_near > centre) ? x_near - centre : centre - x_near;
    const int far_offset = (x_far > centre) ? x_far - centre : centre - x_far;

    TEST_ASSERT_TRUE_MESSAGE(far_offset < near_offset,
                             "a point further from the camera should project closer to screen "
                             "centre than the same point nearer the camera");
}

#define NEAR_PLANE_DIVISOR 8 /* the test's near plane is 1/8 m */

/* A point at the near plane must leave the output untouched; the equality
 * exercises the clip boundary independently of any point further back. */
static void
test_project_point_rejects_a_point_at_the_near_plane(void) {
    boot_anim_view_t view = identity_view();
    view.lens.near_z = MATHX_ONE / NEAR_PLANE_DIVISOR; /* 0.125 m */
    int x = -1, y = -1;

    const bool ok = boot_anim_project_point(0, BOOT_ANIM_ONE / NEAR_PLANE_DIVISOR, 0, &view, &x, &y);

    TEST_ASSERT_FALSE_MESSAGE(ok, "a point exactly at the near plane must be rejected, not "
                                  "projected to a nonsense screen position");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, x, "a rejected point's output x must be left untouched");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, y, "a rejected point's output y must be left untouched");
}

/* The OTHER early-out in r3d_project_segment_cs_x() besides the clip itself
 * (see test_project_segment_cs_clips_asymmetric_coordinates below for that
 * branch): a segment with BOTH endpoints at or behind the near plane has
 * nothing in front of the camera to draw at all, and must return false
 * outright rather than clip against itself. */
static void
test_project_segment_cs_rejects_a_segment_entirely_behind(void) {
    const boot_anim_view_t view = identity_view();
    const vec3x_t p0 = {100, 200, 0};
    const vec3x_t p1 = {-100, -200, r3d_line_near_z(&view)};
    int ax, ay, bx, by;

    const bool ok = r3d_project_segment_cs_x(p0, p1, &view, &ax, &ay, &bx, &by);

    TEST_ASSERT_FALSE_MESSAGE(ok, "a segment with both endpoints at or behind the near plane "
                                  "should be rejected entirely, not clipped against itself");
}

/* r3d_project_segment_cs_x()'s near-plane clip at asymmetric coordinates
 * far outside the panel, where the clipped endpoint lands thousands of
 * pixels out. Verified against an independent 64-bit integer reference,
 * not the function under test. */
static void
test_project_segment_cs_clips_asymmetric_coordinates(void) {
    const boot_anim_view_t view = identity_view();
    const vec3x_t p0 = vx(-3.1F, 2.5F, R3D_LINE_NEAR_Z - 0.168F);
    const vec3x_t p1 = vx(4.0F, -1.8F, R3D_LINE_NEAR_Z + 0.801F);

    int ax, ay, bx, by;
    TEST_ASSERT_TRUE_MESSAGE(r3d_project_segment_cs_x(p0, p1, &view, &ax, &ay, &bx, &by),
                             "a segment with one endpoint in front of the near plane should "
                             "always project");

    /* Double-precision reference for the clip itself: p0 is BEHIND, so
     * IT is what gets replaced by the near-plane crossing point. */
    const int64_t span = (int64_t)p1.z - p0.z;
    const int64_t reach = (int64_t)r3d_line_near_z(&view) - p0.z;
    int ex, ey;
    const vec3x_t exact_clip = {(int32_t)(p0.x + ((((int64_t)p1.x - p0.x) * reach) / span)),
                                (int32_t)(p0.y + ((((int64_t)p1.y - p0.y) * reach) / span)), r3d_line_near_z(&view)};
    r3d_camera_to_screen_x(exact_clip, &view, &ex, &ey);

    const int tolerance = 6;
    TEST_ASSERT_INT_WITHIN_MESSAGE(tolerance, ex, ax,
                                   "the clipped endpoint's screen x should be close to a "
                                   "integer reference");
    TEST_ASSERT_INT_WITHIN_MESSAGE(tolerance, ey, ay,
                                   "the clipped endpoint's screen y should be close to a "
                                   "integer reference");

    /* p1 was already in front: not clipped at all, so it must match
     * projecting it directly, independent of whatever the clip branch
     * above did. */
    int fx, fy;
    r3d_camera_to_screen_x(p1, &view, &fx, &fy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(fx, bx,
                                  "the untouched (already in front) endpoint should project "
                                  "identically whether reached through the clipping path or not");
    TEST_ASSERT_EQUAL_INT_MESSAGE(fy, by,
                                  "the untouched (already in front) endpoint should project "
                                  "identically whether reached through the clipping path or not");
}

/* "Scale renders as 1,1,1 for default values", the exact ask this table
 * exists to satisfy: an untouched space's scale has to read back as 1.0, on every axis, not some other number that happens to look
 * right today. */
static void
test_an_untouched_scale_reads_back_as_identity(void) {
    const boot_anim_timeline_state_t st = boot_anim_timeline_sample(seed, 0);

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0F, st.space.scale.x, "an unscaled space's scale.x should read back as 1.0");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0F, st.space.scale.y, "an unscaled space's scale.y should read back as 1.0");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0F, st.space.scale.z, "an unscaled space's scale.z should read back as 1.0");
}

/*
 * The seed motion
 *
 * A freely animated camera has no blanket off-panel guarantee: a wrong
 * angle can put the scene off-panel, a legitimate edit, not a bug. Worth
 * protecting: the SEED this repo ships, a sanity sweep against
 * boot_anim_motion.glb. A generous margin, not a tight fit; catches the
 * seed going wildly broken, not tunable tighter.
 */

static void
test_the_seed_is_the_boot_clip_read_from_its_pack(void) {
    TEST_ASSERT_NOT_NULL_MESSAGE(seed, "no memory for the seed motion");
    TEST_ASSERT_TRUE_MESSAGE(seed->from_pack, "the boot clip did not load from its pack: see the log above");
    TEST_ASSERT_GREATER_THAN_UINT32(0, seed->clip.duration_ms);
}

#define BOOT_ANIM_TEST_MAX_PANEL_MULTIPLE 3

static void
expect_the_curve_near_the_panel_throughout(const boot_anim_motion_t* motion) {
    for (uint32_t t = 0; t <= BOOT_ANIM_MS; t += 100) {
        const boot_anim_view_t view = boot_anim_view(motion, PANEL_W, PANEL_H, t);
        const int32_t progress = boot_anim_pen(t);
        const int32_t span = (int32_t)(BOOT_ANIM_CURVE_POINTS - 1);
        const int last = (int)(((int64_t)progress * span) >> BOOT_ANIM_Q);

        /* The head (furthest-drawn point) alone is enough to catch a
         * camera pointed somewhere absurd; checking every sample every
         * 100ms as well would be thousands of assertions for the same
         * signal. */
        if (last < 0 || last >= BOOT_ANIM_CURVE_POINTS) {
            continue;
        }
        const boot_anim_pt_t p = boot_anim_sample(last);
        int x, y;
        TEST_ASSERT_TRUE(boot_anim_project_point(p.re, p.im, p.t, &view, &x, &y));

        TEST_ASSERT_TRUE_MESSAGE(x > -PANEL_W * BOOT_ANIM_TEST_MAX_PANEL_MULTIPLE
                                     && x < PANEL_W * (BOOT_ANIM_TEST_MAX_PANEL_MULTIPLE + 1),
                                 "the camera has drifted wildly off in X - not just "
                                 "off-panel, off by panel-widths");
        TEST_ASSERT_TRUE_MESSAGE(y > -PANEL_H * BOOT_ANIM_TEST_MAX_PANEL_MULTIPLE
                                     && y < PANEL_H * (BOOT_ANIM_TEST_MAX_PANEL_MULTIPLE + 1),
                                 "the camera has drifted wildly off in Y - not just "
                                 "off-panel, off by panel-heights");
    }
}

static void
test_the_seeds_curve_stays_near_the_panel_throughout(void) {
    expect_the_curve_near_the_panel_throughout(seed);
}

/* The three axes, projected from the seed's own first pose, must not
 * collapse onto each other: a camera that happened to look straight down
 * one of them would still "work" in the sense of not crashing, but the
 * picture would read as two lines, not three. A weak, non-fragile check on
 * purpose: it is not this test's job to say WHERE the axes should point
 * (that is a creative choice made in the motion glTF), only that they
 * are not degenerate. */
static void
expect_three_distinct_axes(const boot_anim_motion_t* motion) {
    const boot_anim_view_t view = boot_anim_view(motion, PANEL_W, PANEL_H, 0);
    const int32_t one = BOOT_ANIM_ONE;
    int ox, oy, rx, ry, ix, iy, tx, ty;

    TEST_ASSERT_TRUE(boot_anim_project_point(0, 0, 0, &view, &ox, &oy));
    TEST_ASSERT_TRUE(boot_anim_project_point(one, 0, 0, &view, &rx, &ry));
    TEST_ASSERT_TRUE(boot_anim_project_point(0, one, 0, &view, &ix, &iy));
    TEST_ASSERT_TRUE(boot_anim_project_point(0, 0, 1 << BOOT_ANIM_TQ, &view, &tx, &ty));

    TEST_ASSERT_FALSE_MESSAGE(rx == ix && ry == iy, "the real and imaginary axes should not project to the same point");
    TEST_ASSERT_FALSE_MESSAGE(rx == tx && ry == ty, "the real and t axes should not project to the same point");
    TEST_ASSERT_FALSE_MESSAGE(ix == tx && iy == ty, "the imaginary and t axes should not project to the same point");
    TEST_ASSERT_FALSE_MESSAGE(rx == ox && ry == oy, "the real axis should not collapse onto the origin");
}

static void
test_the_seeds_three_axes_project_to_distinct_directions(void) {
    expect_three_distinct_axes(seed);
}

#define FLOOR_DECAY_STEP_MS 800

static void
test_floor_is_flat_through_impact(void) {
    for (uint32_t ms = 0; ms <= BOOT_ANIM_RIPPLE_IMPACT_MS; ms++) {
        const boot_anim_floor_state_t state = boot_anim_floor_state(ms);
        for (int i = -(BOOT_ANIM_MESH_CELLS / 2); i <= BOOT_ANIM_MESH_CELLS / 2; i++) {
            TEST_ASSERT_EQUAL_INT32(0, boot_anim_floor_height(i, 0, &state));
        }
    }
}

static void
test_floor_is_flat_well_ahead_of_the_train(void) {
    const boot_anim_floor_state_t state = boot_anim_floor_state(BOOT_ANIM_RIPPLE_IMPACT_MS + 1);
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_floor_height(BOOT_ANIM_MESH_CELLS / 2, BOOT_ANIM_MESH_CELLS / 2, &state));
}

static void
test_ripple_peak_falls_over_time(void) {
    int previous = INT32_MAX;
    for (uint32_t ms = BOOT_ANIM_RIPPLE_IMPACT_MS + 1; ms < BOOT_ANIM_MS; ms += FLOOR_DECAY_STEP_MS) {
        boot_anim_floor_state_t state = boot_anim_floor_state(ms);
        state.swell = 0;
        int peak = 0;
        for (int j = 0; j <= BOOT_ANIM_MESH_CELLS / 2; j++) {
            for (int i = 0; i <= BOOT_ANIM_MESH_CELLS / 2; i++) {
                peak = mathi_max(peak, mathi_abs(boot_anim_floor_height(i, j, &state)));
            }
        }
        TEST_ASSERT_TRUE(peak < previous);
        previous = peak;
    }
}

static void
test_swell_is_zero_through_its_start(void) {
    for (uint32_t ms = 0; ms <= BOOT_ANIM_TITLE_START_MS; ms++) {
        const boot_anim_floor_state_t state = boot_anim_floor_state(ms);
        TEST_ASSERT_EQUAL_INT32(0, state.swell);
    }
}

#ifndef DEVICE_BUILD
#define FLOOR_REFERENCE_TOLERANCE_M   0.02
#define FLOOR_REFERENCE_STEP_MS       37
#define FLOOR_REFERENCE_TAU           6.2831853071795864769
#define FLOOR_REFERENCE_DEG_PER_TURN  360.0
#define FLOOR_REFERENCE_MS_PER_SECOND 1000.0

/* The swell at (re, im) metres, as authored. */
static double
floor_reference_swell(double re, double im, uint32_t ms) {
    double height = 0;
    if (ms > BOOT_ANIM_TITLE_START_MS) {
        const double fade = fmin(1, (double)(ms - BOOT_ANIM_TITLE_START_MS) / BOOT_ANIM_SWELL_IN_MS);
        for (int k = 0; k < BOOT_ANIM_SWELL_COUNT; k++) {
            const boot_anim_swell_setting_t* sine = &boot_anim_swell_settings[k];
            const double angle = sine->direction_deg * FLOOR_REFERENCE_TAU / FLOOR_REFERENCE_DEG_PER_TURN;
            const double along = (re * cos(angle)) + (im * sin(angle));
            height += BOOT_ANIM_SWELL_HEIGHT_M * fade * sine->weight
                      * sin(FLOOR_REFERENCE_TAU * ((along / sine->wavelength_m) - ((double)ms / sine->period_ms)));
        }
    }
    return height;
}

/* Past the mesh the swell eases out, smoothstepped from the mesh's edge to
 * BOOT_ANIM_FAR_SWELL_FADE_Q12. */
static double
far_reference_swell(double re, double im, uint32_t ms) {
    const double edge = (double)boot_anim_axis_far[0].r / BOOT_ANIM_ONE;
    const double fade = (double)BOOT_ANIM_FAR_SWELL_FADE_Q12 / BOOT_ANIM_ONE;
    const double u = fmin(1, fmax(0, (fade - hypot(re, im)) / (fade - edge)));
    return floor_reference_swell(re, im, ms) * u * u * (3 - (2 * u));
}

/* The far lines also ease out over BOOT_ANIM_FAR_SWELL_OUT_MS, smoothstepped,
 * to none by the last time step at or before the crossfade. */
static double
far_line_reference_swell(double re, double im, uint32_t ms) {
    const double end = (double)(BOOT_ANIM_IMAGE_START_MS - (BOOT_ANIM_IMAGE_START_MS % BOOT_ANIM_FLOOR_TIME_STEP_MS));
    const double u = fmin(1, fmax(0, (end - (double)ms) / BOOT_ANIM_FAR_SWELL_OUT_MS));
    return far_reference_swell(re, im, ms) * u * u * (3 - (2 * u));
}

static double
floor_reference_height(int i, int j, uint32_t ms) {
    const double re = (double)(i * BOOT_ANIM_MESH_STEP_Q12) / BOOT_ANIM_ONE;
    const double im = (double)(j * BOOT_ANIM_MESH_STEP_Q12) / BOOT_ANIM_ONE;
    const double radius = hypot(re, im);
    double height = 0;
    if (ms > BOOT_ANIM_RIPPLE_IMPACT_MS) {
        const double tau = (double)(ms - BOOT_ANIM_RIPPLE_IMPACT_MS) / FLOOR_REFERENCE_MS_PER_SECOND;
        const double clock_s = BOOT_ANIM_RIPPLE_CLOCK_S;
        const double group_speed = BOOT_ANIM_RIPPLE_GROUP_SPEED_M_S;
        const double wavelength = BOOT_ANIM_RIPPLE_WAVELENGTH_M;
        const double width = BOOT_ANIM_RIPPLE_WIDTH_M;
        const double dispersion = BOOT_ANIM_RIPPLE_DISPERSION_M_S;
        const double source_radius = BOOT_ANIM_RIPPLE_SOURCE_RADIUS_M;
        const double calm_fraction = BOOT_ANIM_RIPPLE_CALM_FRACTION;
        const double calm_after_s = ((double)BOOT_ANIM_RIPPLE_CALM_AFTER_MS / FLOOR_REFERENCE_MS_PER_SECOND);
        const double w = clock_s * (1 - exp(-tau / clock_s));
        const double group_radius = group_speed * w;
        const double sigma = hypot(width, dispersion * tau);
        const double amplitude =
            BOOT_ANIM_RIPPLE_HEIGHT_M * sqrt(source_radius * width / ((group_radius + source_radius) * sigma));
        const double u = fmin(1, fmax(0, (radius - (calm_fraction * group_radius) + (wavelength / 2)) / wavelength));
        const double calm = tau > calm_after_s ? u * u * (3 - (2 * u)) : 1;
        const double x = (radius - group_radius) / sigma;
        height = -amplitude * calm * exp(-(x * x) / 2)
                 * cos(FLOOR_REFERENCE_TAU * (radius - (2 * group_radius)) / wavelength);
    }
    return height + floor_reference_swell(re, im, ms);
}

static void
test_baked_floor_matches_direct_float(void) {
    double maximum_error = 0;
    for (uint32_t ms = 0; ms <= BOOT_ANIM_MS; ms += FLOOR_REFERENCE_STEP_MS) {
        const boot_anim_floor_state_t state = boot_anim_floor_state(ms);
        for (int j = -(BOOT_ANIM_MESH_CELLS / 2); j <= BOOT_ANIM_MESH_CELLS / 2; j++) {
            for (int i = -(BOOT_ANIM_MESH_CELLS / 2); i <= BOOT_ANIM_MESH_CELLS / 2; i++) {
                const double baked = (double)boot_anim_floor_height(i, j, &state) / BOOT_ANIM_ONE;
                maximum_error = fmax(maximum_error, fabs(baked - floor_reference_height(i, j, ms)));
            }
        }
    }
    printf("FLOOR_REFERENCE max_error_m=%.6f tolerance_m=%.6f\n", maximum_error, FLOOR_REFERENCE_TOLERANCE_M);
    TEST_ASSERT_TRUE(maximum_error < FLOOR_REFERENCE_TOLERANCE_M);
}

/* The swell on the far lines and on the axis lines past the mesh, every
 * line, side and half, against the authored waves eased out by the fade;
 * the far lines are flat from the crossfade on. */
static void
test_baked_far_swell_matches_direct_float(void) {
    const int dirs[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    double maximum_error = 0;
    for (uint32_t ms = 0; ms <= BOOT_ANIM_MS; ms += FLOOR_REFERENCE_STEP_MS) {
        const boot_anim_floor_state_t state = boot_anim_floor_state(ms);
        if (ms >= BOOT_ANIM_IMAGE_START_MS) {
            TEST_ASSERT_EQUAL_UINT16(0, state.far_swell);
        }
        for (int n = 0; n < BOOT_ANIM_FAR_LINES; n++) {
            const boot_anim_far_line_t* line = &boot_anim_far_lines[n];
            for (int k = 0; k < line->count; k++) {
                for (int way = 0; way < 8; way++) {
                    const bool along_im = (way & 1) != 0;
                    const int side = (way & 2) != 0 ? -1 : 1;
                    const int half = (way & 4) != 0 ? -1 : 1;
                    const double c = (double)(side * line->c) / BOOT_ANIM_ONE;
                    const double s = (double)(half * boot_anim_far_samples[line->first + k].s) / BOOT_ANIM_ONE;
                    const double expect =
                        along_im ? far_line_reference_swell(c, s, ms) : far_line_reference_swell(s, c, ms);
                    const double baked =
                        (double)boot_anim_far_line_swell(line, k, along_im, side, half, &state) / BOOT_ANIM_ONE;
                    maximum_error = fmax(maximum_error, fabs(baked - expect));
                    TEST_ASSERT_TRUE(mathi_abs(boot_anim_far_line_swell(line, k, along_im, side, half, &state))
                                     <= boot_anim_swell_bound(&state));
                }
            }
        }
        for (int n = 0; n < BOOT_ANIM_AXIS_FAR_SAMPLES; n++) {
            const double r = (double)boot_anim_axis_far[n].r / BOOT_ANIM_ONE;
            for (int d = 0; d < 4; d++) {
                const int32_t swell = boot_anim_axis_far_height(n, dirs[d][0], dirs[d][1], &state)
                                      - boot_anim_ripple_at(&boot_anim_axis_far[n].node, &state);
                const double expect = far_reference_swell(dirs[d][0] * r, dirs[d][1] * r, ms);
                maximum_error = fmax(maximum_error, fabs(((double)swell / BOOT_ANIM_ONE) - expect));
            }
        }
    }
    printf("FAR_SWELL_REFERENCE max_error_m=%.6f tolerance_m=%.6f\n", maximum_error, FLOOR_REFERENCE_TOLERANCE_M);
    TEST_ASSERT_TRUE(maximum_error < FLOOR_REFERENCE_TOLERANCE_M);
}
#endif

/* The title's idle wave calming down: a different wave entirely from the
 * floor ripple above (see boot_anim_title_wave()), but the same "255 minus
 * a ramp" envelope, so the same three moments are worth pinning: full
 * swing before it starts, still full AT the start (that instant is when
 * the ramp begins, not when it has finished), and nothing left once the
 * fade window has passed. */
static void
test_the_title_wave_is_at_full_swing_before_it_calms(void) {
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_title_wave_reach(0));
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_title_wave_reach(BOOT_ANIM_TITLE_WAVE_OUT_MS));
}

static void
test_the_title_wave_reaches_stillness_after_its_fade(void) {
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_title_wave_reach(BOOT_ANIM_TITLE_WAVE_OUT_MS + BOOT_ANIM_TITLE_WAVE_FADE_MS));
}

/* The point of scaling the AMPLITUDE rather than gating the wave off: the
 * motion has to shrink monotonically through the window, so a letter
 * caught mid-bob rides its own arc down instead of snapping straight. The
 * envelope is what carries that, so it is what gets swept; the wave
 * itself keeps oscillating underneath and would not be monotonic. */
static void
test_the_title_wave_never_swings_wider_as_it_calms(void) {
    const uint32_t out = BOOT_ANIM_TITLE_WAVE_OUT_MS;
    const uint32_t fade = BOOT_ANIM_TITLE_WAVE_FADE_MS;
    int prev = 256;

    for (uint32_t t = out; t <= out + fade; t += (fade / 32) + 1) {
        const int now = boot_anim_title_wave_reach(t);
        TEST_ASSERT_TRUE_MESSAGE(now <= prev, "the title wave's own envelope grew back while it was "
                                              "supposed to be calming");
        prev = now;
    }
}

/* The far samples start on the mesh's edge node, so the axis lines carry on
 * from the mesh without a step, in all four directions and at every moment
 * the floor moves. */
static void
test_the_axis_lines_leave_the_mesh_without_a_step(void) {
    const int half = BOOT_ANIM_MESH_CELLS / 2;
    const int dirs[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    for (uint32_t ms = 0; ms <= BOOT_ANIM_MS; ms += BOOT_ANIM_FLOOR_TIME_STEP_MS * 7) {
        const boot_anim_floor_state_t state = boot_anim_floor_state(ms);
        for (int d = 0; d < 4; d++) {
            const int i = dirs[d][0] * half;
            const int j = dirs[d][1] * half;
            TEST_ASSERT_EQUAL_INT32(boot_anim_floor_height(i, j, &state),
                                    boot_anim_axis_far_height(0, dirs[d][0], dirs[d][1], &state));
        }
    }
}

static void
test_the_far_samples_climb_from_the_mesh_edge_to_far(void) {
    TEST_ASSERT_EQUAL_INT32((BOOT_ANIM_MESH_CELLS / 2) * BOOT_ANIM_MESH_STEP_Q12, boot_anim_axis_far[0].r);
    for (int n = 1; n < BOOT_ANIM_AXIS_FAR_SAMPLES; n++) {
        TEST_ASSERT_TRUE(boot_anim_axis_far[n].r > boot_anim_axis_far[n - 1].r);
    }
    TEST_ASSERT_EQUAL_INT32(BOOT_ANIM_FAR_Q12, boot_anim_axis_far[BOOT_ANIM_AXIS_FAR_SAMPLES - 1].r);
}

/* The far lines start inside the mesh and thin out with distance: the
 * gaps between them never shrink, and the last is within far. */
static void
test_the_far_lines_thin_out_with_distance(void) {
    TEST_ASSERT_TRUE(boot_anim_far_lines[0].c > 0);
    TEST_ASSERT_TRUE(boot_anim_far_lines[0].c < boot_anim_axis_far[0].r);
    for (int n = 2; n < BOOT_ANIM_FAR_LINES; n++) {
        TEST_ASSERT_TRUE(boot_anim_far_lines[n].c - boot_anim_far_lines[n - 1].c
                         >= boot_anim_far_lines[n - 1].c - boot_anim_far_lines[n - 2].c);
    }
    TEST_ASSERT_TRUE(boot_anim_far_lines[BOOT_ANIM_FAR_LINES - 1].c <= BOOT_ANIM_FAR_Q12);
}

/* Each far line's swell samples run outward from where it leaves the mesh
 * (or from its foot, past the mesh) to where the swell is gone, ending
 * exactly where draw_far_ring() starts the flat rest; lines past the fade
 * take none. The axis lines are flat from the fade on too. */
static void
test_the_far_swell_ends_where_the_flat_lines_begin(void) {
    const int32_t edge = boot_anim_axis_far[0].r;
    for (int n = 0; n < BOOT_ANIM_FAR_LINES; n++) {
        const boot_anim_far_line_t* line = &boot_anim_far_lines[n];
        if (line->c >= BOOT_ANIM_FAR_SWELL_FADE_Q12) {
            TEST_ASSERT_EQUAL_UINT8(0, line->count);
            continue;
        }
        TEST_ASSERT_TRUE(line->count >= 2);
        const boot_anim_far_sample_t* run = &boot_anim_far_samples[line->first];
        TEST_ASSERT_EQUAL_INT32(mathi_half_chord(mathi_max(edge, line->c), line->c), run[0].s);
        for (int k = 1; k < line->count; k++) {
            TEST_ASSERT_TRUE(run[k].s > run[k - 1].s);
            TEST_ASSERT_TRUE(run[k].swell_gain <= run[k - 1].swell_gain);
        }
        TEST_ASSERT_EQUAL_INT32(mathi_half_chord(BOOT_ANIM_FAR_SWELL_FADE_Q12, line->c), run[line->count - 1].s);
        TEST_ASSERT_EQUAL_UINT16(0, run[line->count - 1].swell_gain);
    }
    for (int n = 0; n < BOOT_ANIM_AXIS_FAR_SAMPLES; n++) {
        if (boot_anim_axis_far[n].r >= BOOT_ANIM_FAR_SWELL_FADE_Q12) {
            TEST_ASSERT_EQUAL_UINT16(0, boot_anim_axis_far[n].swell_gain);
        }
    }
}

static void
test_the_far_chord_is_the_floor_of_the_true_half_chord(void) {
    const int32_t radii[] = {boot_anim_axis_far[0].r, BOOT_ANIM_ONE * 37, BOOT_ANIM_FAR_Q12};
    for (size_t i = 0; i < sizeof(radii) / sizeof(radii[0]); i++) {
        const int32_t r = radii[i];
        for (int n = 0; n < BOOT_ANIM_FAR_LINES; n++) {
            const int32_t c = boot_anim_far_lines[n].c;
            const int64_t h = ((int64_t)r * r) - ((int64_t)c * c);
            const int64_t half = mathi_half_chord(r, c);
            if (h <= 0) {
                TEST_ASSERT_EQUAL_INT64(0, half);
                continue;
            }
            TEST_ASSERT_TRUE(half * half <= h);
            TEST_ASSERT_TRUE((half + 1) * (half + 1) > h);
        }
    }
}

/* The floor reaches the mesh's edge until the mesh has arrived, then grows
 * outward, never back, to far over BOOT_ANIM_FAR_GROW_MS. */
static void
test_the_floor_grows_from_the_mesh_edge_to_far(void) {
    const uint32_t start = BOOT_ANIM_GRID_START_MS + ((uint32_t)BOOT_ANIM_GRID_BANDS * BOOT_ANIM_GRID_BAND_MS);
    const int32_t edge = boot_anim_axis_far[0].r;
    TEST_ASSERT_EQUAL_INT32(edge, boot_anim_floor_state(0).far_reach);
    /* Baked every BOOT_ANIM_FLOOR_TIME_STEP_MS: the ends are exact one step out. */
    TEST_ASSERT_EQUAL_INT32(edge, boot_anim_floor_state(start - BOOT_ANIM_FLOOR_TIME_STEP_MS).far_reach);
    TEST_ASSERT_EQUAL_INT32(
        BOOT_ANIM_FAR_Q12,
        boot_anim_floor_state(start + BOOT_ANIM_FAR_GROW_MS + BOOT_ANIM_FLOOR_TIME_STEP_MS).far_reach);
    int32_t prev = edge;
    for (uint32_t ms = start - BOOT_ANIM_FLOOR_TIME_STEP_MS; ms <= start + BOOT_ANIM_FAR_GROW_MS; ms++) {
        const int32_t reach = boot_anim_floor_state(ms).far_reach;
        TEST_ASSERT_TRUE(reach >= prev);
        prev = reach;
    }
}

/* Smoothing */

static boot_anim_pt_t
pt(int32_t re, int32_t im, int32_t t) {
    boot_anim_pt_t p = {re, im, t};
    return p;
}

/* A span runs from the midpoint of the first two control points to the
 * midpoint of the last two, which is what makes consecutive spans join. */
static void
test_a_span_starts_and_ends_halfway_between_its_points(void) {
    const boot_anim_pt_t a = pt(0, 0, 0);
    const boot_anim_pt_t b = pt(BOOT_ANIM_ONE, BOOT_ANIM_ONE / 2, 2 << BOOT_ANIM_TQ);
    const boot_anim_pt_t c = pt(2 * BOOT_ANIM_ONE, -BOOT_ANIM_ONE / 2, 4 << BOOT_ANIM_TQ);

    const boot_anim_pt_t start = boot_anim_spline(a, b, c, 0);
    const boot_anim_pt_t end = boot_anim_spline(a, b, c, BOOT_ANIM_ONE);

    TEST_ASSERT_INT32_WITHIN(1, (a.re + b.re) / 2, start.re);
    TEST_ASSERT_INT32_WITHIN(1, (a.im + b.im) / 2, start.im);
    TEST_ASSERT_INT32_WITHIN(1, (a.t + b.t) / 2, start.t);

    TEST_ASSERT_INT32_WITHIN(1, (b.re + c.re) / 2, end.re);
    TEST_ASSERT_INT32_WITHIN(1, (b.im + c.im) / 2, end.im);
    TEST_ASSERT_INT32_WITHIN(1, (b.t + c.t) / 2, end.t);
}

/* Repeating a control point pins the curve to it, which is how the first and
 * last samples end up actually being drawn rather than half a span in. */
static void
test_a_repeated_point_pins_the_end_of_the_curve(void) {
    const boot_anim_pt_t a = pt(1000, -2000, 300);
    const boot_anim_pt_t b = pt(5000, 1500, 900);

    const boot_anim_pt_t start = boot_anim_spline(a, a, b, 0);
    TEST_ASSERT_INT32_WITHIN(1, a.re, start.re);
    TEST_ASSERT_INT32_WITHIN(1, a.im, start.im);
    TEST_ASSERT_INT32_WITHIN(1, a.t, start.t);
}

/* The convex hull property, which is the whole reason this is a B-spline and
 * not a Catmull-Rom: no point of the curve may leave the box its control
 * points span. An interpolating spline would overshoot here, and the picture
 * would bulge exactly where the curve turns hardest. */
static void
test_a_span_never_leaves_its_control_points_behind(void) {
    const boot_anim_pt_t a = pt(-3000, 500, 0);
    const boot_anim_pt_t b = pt(4000, -2500, 400);
    const boot_anim_pt_t c = pt(-2500, 3000, 800);

    const int32_t lo_re = -3000, hi_re = 4000;
    const int32_t lo_im = -2500, hi_im = 3000;

    for (int32_t t = 0; t <= BOOT_ANIM_ONE; t += 37) {
        const boot_anim_pt_t p = boot_anim_spline(a, b, c, t);
        TEST_ASSERT_TRUE_MESSAGE(p.re >= lo_re - 1 && p.re <= hi_re + 1,
                                 "the spline overshot its control points - that is a Catmull-Rom "
                                 "failure mode and this is supposed to be a B-spline");
        TEST_ASSERT_TRUE_MESSAGE(p.im >= lo_im - 1 && p.im <= hi_im + 1, "the spline overshot its control points");
    }
}

/* Height must not wobble across a span either: the whole curve climbs, so a
 * span between two rising samples has to rise all the way through. */
static void
test_a_span_climbs_steadily_when_its_points_do(void) {
    const boot_anim_pt_t a = pt(0, 0, 100);
    const boot_anim_pt_t b = pt(3000, 1000, 200);
    const boot_anim_pt_t c = pt(-1000, 2000, 300);

    int32_t last = boot_anim_spline(a, b, c, 0).t;
    for (int32_t t = 64; t <= BOOT_ANIM_ONE; t += 64) {
        const int32_t now = boot_anim_spline(a, b, c, t).t;
        TEST_ASSERT_TRUE_MESSAGE(now >= last, "a span dipped on its way up");
        last = now;
    }
}

/* boot_anim_spline_cs()'s whole reason to exist: transforming three
 * control points to camera space and THEN interpolating must land on the
 * same point as interpolating in world space and THEN transforming, which
 * is what lets a caller skip a full matrix transform per drawn sub-point.
 * Checked against a real rotated, translated, perspective view: an
 * identity transform would not catch a bug that only shows up once
 * translation and rotation are mixed in. */
static void
test_spline_cs_matches_transforming_the_world_space_spline(void) {
    const boot_anim_view_t view = boot_anim_view(seed, PANEL_W, PANEL_H, CURVE_DONE_MS);
    const boot_anim_pt_t a = pt(-3000, 1500, 200);
    const boot_anim_pt_t b = pt(2500, -1800, 900);
    const boot_anim_pt_t c = pt(600, 3200, 1600);

    const vec3x_t ta = boot_anim_to_camera_space(a.re, a.im, a.t, &view);
    const vec3x_t tb = boot_anim_to_camera_space(b.re, b.im, b.t, &view);
    const vec3x_t tc = boot_anim_to_camera_space(c.re, c.im, c.t, &view);

    for (int32_t t = 0; t <= BOOT_ANIM_ONE; t += 197) {
        const boot_anim_pt_t world = boot_anim_spline(a, b, c, t);
        const vec3x_t want = boot_anim_to_camera_space(world.re, world.im, world.t, &view);
        const vec3x_t got = boot_anim_spline_cs(ta, tb, tc, t);

        TEST_ASSERT_INT32_WITHIN_MESSAGE(2, want.x, got.x,
                                         "transform-then-interpolate must match interpolate-then-"
                                         "transform, up to the curve table's Q12 rounding");
        TEST_ASSERT_INT32_WITHIN_MESSAGE(2, want.y, got.y,
                                         "transform-then-interpolate must match interpolate-then-"
                                         "transform, up to the curve table's Q12 rounding");
        TEST_ASSERT_INT32_WITHIN_MESSAGE(2, want.z, got.z,
                                         "transform-then-interpolate must match interpolate-then-"
                                         "transform, up to the curve table's Q12 rounding");
    }
}

/* A floor point through the plane helper is the full transform's result, within
 * a few 1/512 m (the plane's steps carry the Q9 matrix, the full transform's t axis more bits), on a real rotated, translated view. */
static void
test_plane_points_match_the_full_camera_space_transform(void) {
    const boot_anim_view_t view = boot_anim_view(seed, PANEL_W, PANEL_H, CURVE_DONE_MS);
    for (int32_t t = -300; t <= 900; t += 300) {
        const boot_anim_plane_t plane = boot_anim_plane(t, &view);
        for (int32_t re = -9000; re <= 9000; re += 4500) {
            for (int32_t im = -9000; im <= 9000; im += 3000) {
                const vec3x_t want = boot_anim_to_camera_space(re, im, t, &view);
                const vec3x_t got = boot_anim_plane_point(&plane, re, im);
                TEST_ASSERT_INT32_WITHIN(3, want.x, got.x);
                TEST_ASSERT_INT32_WITHIN(3, want.y, got.y);
                TEST_ASSERT_INT32_WITHIN(3, want.z, got.z);
            }
        }
    }
}

/* The narrow transform (three 32-bit products) against the 64-bit one over the
 * seed's whole motion: near inputs through either agree within the wide path's Q9 matrix error (about 1/1000 of the distance), and
 * the seed's views are inside the narrow path's range. */
static void
expect_the_narrow_transform_across_the_motion(const boot_anim_motion_t* motion) {
    for (uint32_t ms = 0; ms <= BOOT_ANIM_MS; ms += 250) {
        const boot_anim_view_t view = boot_anim_view(motion, PANEL_W, PANEL_H, ms);
        TEST_ASSERT_TRUE_MESSAGE(view.units_ok, "a view fell outside the narrow transform's range");
        for (int32_t re = -50000; re <= 50000; re += 25000) {
            for (int32_t t = -30000; t <= 30000; t += 15000) {
                const int32_t im = (re / 2) + 7;
                const vec3x_t narrow = r3d_to_camera_space_units(&view, re, t, im);
                const vec3x_t wide =
                    mat4x_apply(&view.matrix, (vec3x_t){re * 16, t * (BOOT_ANIM_SPIRAL_Q9 / 2), im * 16});
                TEST_ASSERT_INT32_WITHIN(24, wide.x, narrow.x);
                TEST_ASSERT_INT32_WITHIN(24, wide.y, narrow.y);
                TEST_ASSERT_INT32_WITHIN(24, wide.z, narrow.z);
            }
        }
    }
}

static void
test_camera_transform_selects_the_narrow_path_at_negative_input_limits(void) {
    const boot_anim_view_t original = boot_anim_view(seed, PANEL_W, PANEL_H, 0);
    TEST_ASSERT_TRUE(original.units_ok);
    for (int axis = 0; axis < 3; axis++) {
        const int32_t limit = axis == 1 ? R3D_X_INPUT_Y_LIMIT : R3D_X_INPUT_XZ_LIMIT;
        for (int sign = -1; sign <= 1; sign += 2) {
            int32_t raw[3] = {0};
            raw[axis] = sign * limit;
            boot_anim_view_t view = original;
            const vec3x_t point = {raw[0] * (MATHX_ONE / BOOT_ANIM_ONE), raw[1] * (BOOT_ANIM_SPIRAL_Q9 / 2),
                                   raw[2] * (MATHX_ONE / BOOT_ANIM_ONE)};
            const vec3x_t wide = mat4x_apply(&view.matrix, point);
            const vec3x_t narrow = r3d_to_camera_space_units(&view, raw[0], raw[1], raw[2]);
            const vec3x_t got = boot_anim_to_camera_space(raw[0], raw[2], raw[1], &view);
            TEST_ASSERT_INT32_WITHIN(1 + limit / (1 << R3D_X_INPUT_SHIFT), wide.x, got.x);
            TEST_ASSERT_INT32_WITHIN(1 + limit / (1 << R3D_X_INPUT_SHIFT), wide.y, got.y);
            TEST_ASSERT_INT32_WITHIN(1 + limit / (1 << R3D_X_INPUT_SHIFT), wide.z, got.z);
            /* A distinct narrow translation makes path selection observable. */
            view.units[0][3] += 1 << R3D_X_INPUT_SHIFT;
            const vec3x_t selected = boot_anim_to_camera_space(raw[0], raw[2], raw[1], &view);
            TEST_ASSERT_EQUAL_INT32(sign < 0 ? narrow.x + 1 : wide.x, selected.x);
            TEST_ASSERT_EQUAL_INT32(sign < 0 ? narrow.y : wide.y, selected.y);
            TEST_ASSERT_EQUAL_INT32(sign < 0 ? narrow.z : wide.z, selected.z);
        }
    }
}

static void
test_the_narrow_camera_transform_agrees_with_the_wide_one_across_the_motion(void) {
    expect_the_narrow_transform_across_the_motion(seed);
}

/* boot_anim_key_to_camera_space() takes no range test: the curve table must stay
 * inside the narrow transform's inputs. */
static void
test_the_curve_table_stays_inside_the_narrow_range(void) {
    for (int i = 0; i < BOOT_ANIM_CURVE_POINTS; i++) {
        const boot_anim_pt_t p = boot_anim_sample(i);
        TEST_ASSERT_TRUE(p.re > -R3D_X_INPUT_XZ_LIMIT && p.re < R3D_X_INPUT_XZ_LIMIT);
        TEST_ASSERT_TRUE(p.im > -R3D_X_INPUT_XZ_LIMIT && p.im < R3D_X_INPUT_XZ_LIMIT);
        TEST_ASSERT_TRUE(p.t > -R3D_X_INPUT_Y_LIMIT && p.t < R3D_X_INPUT_Y_LIMIT);
    }
}

/* Basic level of detail */

/* The pure decision half of the whole-curve sample decimation; see
 * boot_anim_curve_stride()'s own comment. Full detail while the curve is
 * anywhere near panel-sized; samples only start dropping once the whole
 * thing has shrunk to a fraction of it. */
static void
test_lod_stride_tiers_by_extent(void) {
    TEST_ASSERT_EQUAL_INT(1, boot_anim_lod_stride_for_extent(500));
    TEST_ASSERT_EQUAL_INT(1, boot_anim_lod_stride_for_extent(BOOT_ANIM_LOD_STRIDE2_PX));
    TEST_ASSERT_EQUAL_INT(2, boot_anim_lod_stride_for_extent(BOOT_ANIM_LOD_STRIDE2_PX - 1));
    TEST_ASSERT_EQUAL_INT(2, boot_anim_lod_stride_for_extent(BOOT_ANIM_LOD_STRIDE4_PX));
    TEST_ASSERT_EQUAL_INT(4, boot_anim_lod_stride_for_extent(BOOT_ANIM_LOD_STRIDE4_PX - 1));
    TEST_ASSERT_EQUAL_INT(4, boot_anim_lod_stride_for_extent(0));
}

/*
 * Pacing
 *
 * ease_ramp_u8()/ease_out_u8() themselves are tested in suite_tween.c;
 * what is left here is specific to how boot_anim.h USES them, not the
 * primitives themselves.
 */

/* Phase 1 only; see boot_anim_pen()'s own "TWO PHASES" comment. It reaches
 * BOOT_ANIM_CURVE_PHASE1_FRACTION, not BOOT_ANIM_ONE, at the end of
 * BOOT_ANIM_PEN_MS now; test_the_curve_is_finished_by_pen_finish_ms()
 * covers phase 2's own end. */
static void
test_the_pen_runs_from_nothing_to_phase_ones_end(void) {
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_pen(0));
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_pen(BOOT_ANIM_PEN_START_MS));
    TEST_ASSERT_EQUAL_INT32(BOOT_ANIM_CURVE_PHASE1_FRACTION, boot_anim_pen(BOOT_ANIM_PEN_START_MS + BOOT_ANIM_PEN_MS));
}

/* Phase 2: continues past phase 1's end rather than sitting still, and
 * reaches the whole curve by BOOT_ANIM_PEN_FINISH_MS, an authored moment
 * of its own, independent of BOOT_ANIM_FADE_START_MS (see boot_anim_pen()'s
 * own "TWO PHASES" comment on why the two were split apart); see
 * test_the_curve_is_finished_by_pen_finish_ms() for that half, kept as its
 * own test since it is really a claim about PEN_FINISH_MS, not about the
 * pen's climb in general. */
static void
test_the_pen_keeps_climbing_through_phase_two(void) {
    const uint32_t phase1_end_ms = BOOT_ANIM_PEN_START_MS + BOOT_ANIM_PEN_MS;
    const uint32_t mid_ms = (phase1_end_ms + BOOT_ANIM_PEN_FINISH_MS) / 2;

    TEST_ASSERT_TRUE_MESSAGE(boot_anim_pen(mid_ms) > BOOT_ANIM_CURVE_PHASE1_FRACTION,
                             "the pen should have climbed past where phase 1 left it");
    TEST_ASSERT_TRUE_MESSAGE(boot_anim_pen(mid_ms) < BOOT_ANIM_ONE,
                             "the pen should not have reached the end of the curve yet");
}

static void
test_the_curve_is_finished_by_pen_finish_ms(void) {
    TEST_ASSERT_EQUAL_INT32_MESSAGE(BOOT_ANIM_ONE, boot_anim_pen(BOOT_ANIM_PEN_FINISH_MS),
                                    "the curve should have reached its full extent by pen_finish_ms");
}

/* Not a claim boot_anim_pen() itself makes (see its own comment on why
 * BOOT_ANIM_PEN_FINISH_MS landing after BOOT_ANIM_FADE_START_MS is only ever a generator
 * warning, not a refusal) but true of the SEED this repo ships, and worth
 * catching if a future edit to the committed timeline quietly breaks it. */
static void
test_the_seed_finishes_the_curve_before_the_dissolve_starts(void) {
    TEST_ASSERT_TRUE_MESSAGE(BOOT_ANIM_PEN_FINISH_MS <= BOOT_ANIM_FADE_START_MS,
                             "the shipped seed's curve is still being drawn when the picture "
                             "begins fading");
}

static void
test_the_picture_is_lit_until_the_dissolve_and_dark_at_the_end(void) {
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ink(0));
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ink(BOOT_ANIM_FADE_START_MS));
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_ink(BOOT_ANIM_MS));
    TEST_ASSERT_TRUE(boot_anim_ink(BOOT_ANIM_MS - 100) < 255);
}

/* boot_anim_photo_reveal()'s own two guaranteed endpoints: the crossfade
 * has not started at or before BOOT_ANIM_IMAGE_START_MS, and it is fully
 * arrived (and stays arrived) once BOOT_ANIM_IMAGE_FADE_MS has passed
 * since. */
static void
test_the_photograph_arrives_over_its_own_window(void) {
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_photo_reveal(0));
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_photo_reveal(BOOT_ANIM_IMAGE_START_MS));
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_photo_reveal(BOOT_ANIM_IMAGE_START_MS + BOOT_ANIM_IMAGE_FADE_MS));
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_photo_reveal(BOOT_ANIM_IMAGE_START_MS + BOOT_ANIM_IMAGE_FADE_MS + 1000));
}

/* Boot mounts the photograph's pack after the first frame, so the first
 * picture never waits on it, and the first frame, drawn at 0 ms, never needs
 * the photograph it has not mounted yet. */
static void
test_the_photograph_is_mounted_after_the_first_frame(void) {
    TEST_ASSERT_FALSE(boot_anim_photo_due(0));
    TEST_ASSERT_TRUE(boot_anim_photo_due(BOOT_ANIM_PHOTO_MOUNT_FRAME));
    TEST_ASSERT_TRUE(boot_anim_photo_due(BOOT_ANIM_PHOTO_MOUNT_FRAME + 1U));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, boot_anim_photo_reveal(0), "the first frame shows the photograph");
}

/* The two clocks (ink and the crossfade) are independent by design, but
 * the renderer multiplies them together, so the picture has to be at full
 * ink the whole time the photograph is arriving or it fades in already
 * dimmed. The generator only WARNS if the two windows overlap, so this is
 * asserted for the timeline this repo actually ships. Ignored while
 * image_start_ms sits at its inert default (== BOOT_ANIM_MS). */
static void
test_the_seed_finishes_the_crossfade_before_the_dissolve_starts(void) {
    if (BOOT_ANIM_IMAGE_START_MS >= BOOT_ANIM_MS) {
        TEST_IGNORE_MESSAGE("this seed authors no photograph crossfade");
    }
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ink(BOOT_ANIM_IMAGE_START_MS));
    TEST_ASSERT_TRUE_MESSAGE(BOOT_ANIM_IMAGE_START_MS + BOOT_ANIM_IMAGE_FADE_MS <= BOOT_ANIM_FADE_START_MS,
                             "the shipped seed is still crossing to the photograph when the "
                             "picture begins dissolving");
}

static void
test_the_floor_fades_in_from_the_origin_outward(void) {
    const uint32_t t = BOOT_ANIM_GRID_START_MS + 2 * BOOT_ANIM_GRID_BAND_MS + 1;

    TEST_ASSERT_TRUE(boot_anim_grid_alpha(t, 1) > boot_anim_grid_alpha(t, 2));
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_grid_alpha(t, 3));
}

static void
test_the_floor_fades_out_with_distance_rather_than_stopping(void) {
    const uint32_t settled = BOOT_ANIM_MS;
    const int start = BOOT_ANIM_GRID_EDGE_FADE_BAND;
    for (int band = 0; band <= start; band++) {
        TEST_ASSERT_EQUAL_UINT8(255, boot_anim_grid_alpha(settled, band));
    }
    for (int band = start + 1; band <= BOOT_ANIM_GRID_FADE; band++) {
        TEST_ASSERT_TRUE(boot_anim_grid_alpha(settled, band) < boot_anim_grid_alpha(settled, band - 1));
    }
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_grid_alpha(settled, BOOT_ANIM_GRID_FADE));
}

static void
test_only_positive_crests_take_the_crest_colour(void) {
    const uint32_t crest = 0x20C0FF;
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(BOOT_ANIM_FLOOR_GREY), boot_anim_floor_color(-BOOT_ANIM_ONE, crest));
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(BOOT_ANIM_FLOOR_GREY), boot_anim_floor_color(0, crest));
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(crest), boot_anim_floor_color(BOOT_ANIM_CREST_HEIGHT_Q12, crest));
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(crest), boot_anim_floor_color(BOOT_ANIM_ONE, crest));
    const int half = BOOT_ANIM_CREST_HEIGHT_Q12 / 2;
    TEST_ASSERT_EQUAL_HEX16(rgb565be_mix(RGB565BE(BOOT_ANIM_FLOOR_GREY), RGB565BE(crest), UINT8_MAX / 4),
                            boot_anim_floor_color(half, crest));
}

/* The nearest of the first drawn + 1 curve samples to the colour node over
 * mesh node (i, j), found the slow way. */
static int
nearest_drawn_sample(int i, int j, int drawn) {
    const int half = BOOT_ANIM_MESH_CELLS / 2;
    const int stride = BOOT_ANIM_CREST_STRIDE;
    const int64_t x = (int64_t)((((i + half + (stride / 2)) / stride) * stride) - half) * BOOT_ANIM_MESH_STEP_Q12;
    const int64_t y = (int64_t)((((j + half + (stride / 2)) / stride) * stride) - half) * BOOT_ANIM_MESH_STEP_Q12;
    int best = 0;
    int64_t best_d = INT64_MAX;
    for (int k = 0; k <= drawn; k++) {
        const int64_t dx = boot_anim_curve[k].re - x;
        const int64_t dy = boot_anim_curve[k].im - y;
        const int64_t d = (dx * dx) + (dy * dy);
        if (d < best_d) {
            best_d = d;
            best = k;
        }
    }
    return best;
}

static void
test_the_crest_runs_give_the_nearest_drawn_sample(void) {
    const int half = BOOT_ANIM_MESH_CELLS / 2;
    const int drawn_step = 37; /* coprime to the run lengths, so runs' insides and ends are both hit */
    for (int j = -half; j <= half; j += 3) {
        for (int i = -half; i <= half; i += 5) {
            for (int drawn = 0; drawn < BOOT_ANIM_CURVE_POINTS; drawn += drawn_step) {
                TEST_ASSERT_EQUAL_INT(nearest_drawn_sample(i, j, drawn), boot_anim_crest_sample(i, j, drawn));
            }
        }
    }
}

static void
test_the_axes_are_there_before_the_curve_starts_climbing(void) {
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_axis_reach(BOOT_ANIM_AXES_MS));
    TEST_ASSERT_TRUE_MESSAGE(BOOT_ANIM_AXES_MS <= BOOT_ANIM_PEN_START_MS,
                             "the axes should be drawn before anything is plotted against them");
}

/* Colour */

static void
test_height_changes_the_hue(void) {
    const boot_anim_stroke_t foot = boot_anim_stroke(0, 0);
    const boot_anim_stroke_t top = boot_anim_stroke(BOOT_ANIM_ONE, 0);

    TEST_ASSERT_TRUE_MESSAGE(top.hue - foot.hue > RGB888_HUE_TURN / 2,
                             "the climb should turn most of the way round the wheel, so that "
                             "height reads as colour");
}

/* One pen's trail, sampled within the gap before the next pen contributes
 * anything, so this is that pen alone, fading. */
static void
test_a_pens_trail_fades_behind_it(void) {
    const int32_t pen = BOOT_ANIM_ONE / 2;
    const int32_t step = BOOT_ANIM_TRAIL_GAP / 4;

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(255, boot_anim_stroke(pen, pen).glow,
                                    "the stroke under the leading pen should be at full brightness");

    uint8_t last = 255;
    for (int32_t back = step; back < BOOT_ANIM_TRAIL_GAP; back += step) {
        const uint8_t glow = boot_anim_stroke(pen - back, pen).glow;
        TEST_ASSERT_TRUE_MESSAGE(glow < last, "the trail should fade with distance behind its pen");
        last = glow;
    }
}

/* Every pen is a bright point of its own, spaced back along the curve. That
 * is the whole difference from a single trail: several live bands at once. */
static void
test_every_pen_is_lit_at_its_own_position(void) {
    const int32_t pen = BOOT_ANIM_ONE;

    for (int k = 0; k < BOOT_ANIM_TRAILS; k++) {
        const int32_t at = boot_anim_trail_pos(pen, k);
        TEST_ASSERT_TRUE_MESSAGE(at > 0, "a pen never sets off before the curve is finished");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(255, boot_anim_stroke(at, pen).glow,
                                        "a pen was not at full brightness at its own position");
    }
}

/* And each carries a different part of the wheel, which is what puts most of
 * the gamut on screen at once. */
static void
test_the_pens_carry_different_colours(void) {
    for (int a = 0; a < BOOT_ANIM_TRAILS; a++) {
        for (int b = a + 1; b < BOOT_ANIM_TRAILS; b++) {
            TEST_ASSERT_TRUE_MESSAGE(boot_anim_trail_hue(a) != boot_anim_trail_hue(b),
                                     "two pens carry the same hue - they would read as one band");
        }
    }

    const int spread = boot_anim_trail_hue(BOOT_ANIM_TRAILS - 1) - boot_anim_trail_hue(0);
    TEST_ASSERT_TRUE_MESSAGE(spread > RGB888_HUE_TURN / 2,
                             "the pens should be spread round the wheel, not bunched on one side");
}

/* A pen's hue is mixed in BY STRENGTH rather than switched to, so a piece of
 * curve part way behind a pen is part way toward that pen's colour. Without
 * this the bands snap between colours as the nearest pen changes. */
static void
test_a_pens_colour_arrives_gradually(void) {
    const int32_t pen = BOOT_ANIM_ONE / 2;

    const int under = boot_anim_stroke(pen, pen).hue;
    const int half = boot_anim_stroke(pen - BOOT_ANIM_TRAIL_GAP / 2, pen).hue;
    const int base = boot_anim_stroke(pen - BOOT_ANIM_TRAIL_GAP + 1, pen).hue;

    TEST_ASSERT_TRUE_MESSAGE(under > half && half > base,
                             "a pen's hue should fade in with its trail rather than switch on");
}

/* Beyond the trail's reach, only the base is left, which is what the
 * finished picture is made of, and it must still be lit and still be
 * coloured. */
static void
test_settled_curve_keeps_its_colour(void) {
    const int32_t pen = BOOT_ANIM_ONE;
    const boot_anim_stroke_t settled = boot_anim_stroke(0, pen);

    TEST_ASSERT_TRUE_MESSAGE(settled.glow > 96, "the settled part of the curve should still be clearly lit");
    TEST_ASSERT_TRUE_MESSAGE(settled.bloom < 64, "the settled part should keep its hue rather than wash toward white");
    TEST_ASSERT_EQUAL_UINT8(1, settled.width);
}

/* The bloom is capped well short of white on purpose; see boot_anim.h. A
 * trail that reaches white has no colour left exactly where it is brightest. */
static void
test_the_trail_never_washes_out_to_white(void) {
    for (int32_t along = 0; along <= BOOT_ANIM_ONE; along += 64) {
        const boot_anim_stroke_t s = boot_anim_stroke(along, along);
        TEST_ASSERT_TRUE_MESSAGE(s.bloom <= 128, "the trail bloomed far enough toward white to lose its hue");
    }
}

static void
test_the_live_end_of_the_curve_is_drawn_thicker(void) {
    const int32_t pen = BOOT_ANIM_ONE / 2;

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(BOOT_ANIM_FAT_WIDTH, boot_anim_stroke(pen, pen).width,
                                    "the stroke at the pen should be the fat one");

    /* Just short of the next pen's position, where the leading pen's trail
     * has faded below the threshold and the next one has not arrived. */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, boot_anim_stroke(pen - BOOT_ANIM_TRAIL_GAP + 1, pen).width,
                                    "a stroke between two pens should be thin again");
}

/* The title tests use the UI font role, which draw_title() draws with. */

#define TITLE_FONT (draw_font_ui())

static void
test_the_wobble_is_exactly_flat_once_a_letter_has_arrived(void) {
    TEST_ASSERT_EQUAL_INT(0, boot_anim_title_wobble(0));
    TEST_ASSERT_EQUAL_INT(0, boot_anim_title_wobble(-100));
}

/* THE test: catches a wobble whose frequency stays constant while only
 * its AMPLITUDE shrinks; it would pass every other test here and still
 * be wrong, vibrating to a stop instead of settling. Counts sign changes
 * in each HALF of the flight: phase is driven off d SQUARED, so the
 * first half of the approach (d 1.0 to 0.5) sweeps three quarters of the
 * total phase and the second half only the remaining quarter; comparing
 * the two halves measures exactly that, and needs just one crossing to
 * do it. */
static void
test_the_wobbles_oscillation_slows_as_it_lands(void) {
    int early = 0, late = 0, prev_sign = 0;

    for (int32_t d = BOOT_ANIM_ONE; d >= 0; d -= 4) {
        const int y = boot_anim_title_wobble(d);
        const int sign = y > 0 ? 1 : (y < 0 ? -1 : 0);
        if (sign != 0 && prev_sign != 0 && sign != prev_sign) {
            if (d >= BOOT_ANIM_ONE / 2) {
                early++;
            } else {
                late++;
            }
        }
        if (sign != 0) {
            prev_sign = sign;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(early + late >= 1, "the seed authors no wobble oscillation at all - "
                                                "BOOT_ANIM_TITLE_TURNS_PHASE would have to be under half a turn");
    TEST_ASSERT_TRUE_MESSAGE(early > late, "the wobble should oscillate faster early in the flight than as "
                                           "it lands - a chirp, not a constant vibration fading out");
}

/* "on its final position" now means the ARRIVAL wobble (see
 * boot_anim_title_wobble()) has fully decayed, not that the letter sits
 * dead still: boot_anim_title_wave() keeps a small idle motion going
 * forever, by design, so a landed letter's y is BOOT_ANIM_TITLE_VIEW_Y plus
 * whatever that wave says at this instant, not BOOT_ANIM_TITLE_VIEW_Y alone. */
static void
test_a_letter_lands_exactly_on_its_final_position(void) {
    const uint32_t arrived = BOOT_ANIM_TITLE_START_MS + BOOT_ANIM_TITLE_FLIGHT_MS + 1000;

    for (int i = 0; i < BOOT_ANIM_TITLE_LEN; i++) {
        const vec2i_t p = boot_anim_title_letter(TITLE_FONT, i, arrived);
        const int expected_y = BOOT_ANIM_TITLE_VIEW_Y + boot_anim_title_wave(i, arrived);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected_y, p.y,
                                      "a fully arrived letter should sit on the baseline plus "
                                      "whatever the idle wave says, with no residual ARRIVAL wobble "
                                      "left over");
    }
}

/* Each letter starts later than the one before it: "one after another",
 * not all six arriving as a block. */
static void
test_letters_are_staggered_left_to_right(void) {
    const uint32_t never = 0; /* well before any letter has set off */

    /* Not -1: an off-panel starting position is legitimately a large
     * negative number (BOOT_ANIM_TITLE_ENTRY_PX or so to the left of the
     * panel), which a sentinel of -1 would already be greater than on the
     * very first letter. */
    int last_x = -100000;
    for (int i = 0; i < BOOT_ANIM_TITLE_LEN; i++) {
        const vec2i_t here = boot_anim_title_letter(TITLE_FONT, i, never);
        TEST_ASSERT_TRUE_MESSAGE(here.x > last_x, "letters should be laid out left to right in their resting "
                                                  "row, whatever moment they are drawn at");
        last_x = here.x;

        if (i > 0) {
            /* Halfway between letter (i-1)'s start and letter i's: (i-1)
             * has been moving for half a stagger interval, i has not moved
             * at all yet (ease_ramp_u8() is exactly 0 until STRICTLY past
             * its own start, so checking AT i's start catches neither
             * letter moving; checking here is what actually exercises "an
             * earlier letter is further along"). */
            const uint32_t mid =
                BOOT_ANIM_TITLE_START_MS + (uint32_t)i * BOOT_ANIM_TITLE_STAGGER_MS - BOOT_ANIM_TITLE_STAGGER_MS / 2;
            const vec2i_t at = boot_anim_title_letter(TITLE_FONT, i, mid);
            const vec2i_t prev_at = boot_anim_title_letter(TITLE_FONT, i - 1, mid);
            TEST_ASSERT_TRUE_MESSAGE(prev_at.x >= at.x, "an earlier letter should be at least as far along as a "
                                                        "later one at the same moment");
        }
    }
}

static void
test_a_letter_starts_off_panel_to_the_left(void) {
    const vec2i_t p = boot_anim_title_letter(TITLE_FONT, 0, BOOT_ANIM_TITLE_START_MS);
    TEST_ASSERT_TRUE_MESSAGE(p.x < 0, "a letter should begin off the left edge of the panel, not merely "
                                      "at it");
}

/* Regression guard for the bug boot_anim_title_letter()'s own comment in
 * boot_anim.h describes: a fixed per-letter cell, `i * (8 * SCALE +
 * GAP)`, is correct only for a MONOSPACE font. Checks the real formula
 * directly against draw_font_text_width() (the same pure sum-of-advances
 * function draw_font.h's own suite already pins against a synthetic
 * proportional font) rather than a second, hand-derived copy of the
 * arithmetic that could make the same mistake twice. */
static void
test_final_x_matches_the_advance_sum(void) {
    /* Any moment past every letter's own flight is fine: only the FINAL
     * resting x matters here, and
     * test_a_letter_lands_exactly_on_its_final_position() already covers
     * y/wobble/wave separately. */
    const uint32_t arrived = BOOT_ANIM_TITLE_START_MS + (uint32_t)BOOT_ANIM_TITLE_LEN * BOOT_ANIM_TITLE_STAGGER_MS
                             + BOOT_ANIM_TITLE_FLIGHT_MS + 1000;

    for (int i = 0; i < BOOT_ANIM_TITLE_LEN; i++) {
        const vec2i_t p = boot_anim_title_letter(TITLE_FONT, i, arrived);
        const int prefix_w = draw_font_text_width(TITLE_FONT, BOOT_ANIM_TITLE, i, BOOT_ANIM_TITLE_SCALE);
        const int expected_x = BOOT_ANIM_TITLE_VIEW_X + prefix_w + i * BOOT_ANIM_TITLE_GAP;
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected_x, p.x,
                                      "a landed letter's x must be VIEW_X plus the ADVANCE SUM of "
                                      "every letter before it (draw_font_text_width(), proportional) "
                                      "plus i*GAP of tracking - not a fixed per-letter cell, which "
                                      "is only correct for a monospace font");
    }
}

/* The layout guard, in the same spirit as
 * test_the_seeds_curve_stays_near_the_panel_throughout(): every
 * letter, at every moment of its flight including the wildest part of the
 * wobble, must land within the VIEWER's frame once it is actually visible,
 * BOOT_ANIM_TITLE_VIEW_W/H, not PANEL_W/PANEL_H, since
 * boot_anim_title_letter() lays the word out in that frame and a letter
 * kept inside it stays on the panel by construction. */
static void
test_the_title_stays_on_the_panel_once_visible(void) {
    /* The full glyph cell, not just its anchor corner: (x, y) is where a
     * glyph's cell BEGINS, so the cell's far edge is what actually has to
     * stay on the panel. */
    const int cell_w = TITLE_FONT->cell_w * BOOT_ANIM_TITLE_SCALE;
    const int cell_h = TITLE_FONT->cell_h * BOOT_ANIM_TITLE_SCALE;

    for (int i = 0; i < BOOT_ANIM_TITLE_LEN; i++) {
        const uint32_t start = BOOT_ANIM_TITLE_START_MS + (uint32_t)i * BOOT_ANIM_TITLE_STAGGER_MS;
        for (uint32_t t = start; t <= start + BOOT_ANIM_TITLE_FLIGHT_MS; t += 15) {
            const vec2i_t p = boot_anim_title_letter(TITLE_FONT, i, t);
            if (p.x + cell_w < 0) {
                continue; /* still off-panel to the left, not visible yet */
            }
            TEST_ASSERT_TRUE_MESSAGE(p.x + cell_w <= BOOT_ANIM_TITLE_VIEW_W,
                                     "a letter drifted off the right edge of the viewer's frame");
            TEST_ASSERT_TRUE_MESSAGE(p.y >= 0 && p.y + cell_h < BOOT_ANIM_TITLE_VIEW_H,
                                     "a letter's wobble carried it off the top or bottom of "
                                     "the viewer's frame");
        }
    }
}

/* boot_anim_title_shadow_offset() is the one piece of draw_title()'s
 * shadow logic this file can reach directly: draw_title() itself needs a
 * framebuffer and a panel (see this file's own top comment), so this is
 * also the only test standing between a future edit here and the exact
 * "up-right instead of down-right" bug this function was extracted to fix
 * (see its own comment in boot_anim.h for the panel-space derivation). */
static void
test_title_shadow_offset_turns_reader_frame_into_panel_frame(void) {
    int dx, dy;

    /* The shipped default: 1 right, 1 down in the reader's frame. */
    boot_anim_title_shadow_offset(1, 1, &dx, &dy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, dx,
                                  "1 pixel right in the reader's frame must turn into -1 on the "
                                  "panel's own X - a +1 here is the up-right regression this "
                                  "function exists to prevent");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dy,
                                  "1 pixel down must turn into +1 "
                                  "on the panel's own Y");

    /* Pure right (no vertical component) must not move the panel X at
     * all: panel_x tracks -view_y, so it is only the DOWN component that
     * can touch it. Pure down, in turn, must not move the panel Y at all:
     * panel_y tracks view_x, only the RIGHT component reaches it. The
     * two axes are a plain 90-degree swap, not a general rotation that
     * would mix them. */
    boot_anim_title_shadow_offset(5, 0, &dx, &dy);
    TEST_ASSERT_EQUAL_INT(0, dx);
    boot_anim_title_shadow_offset(0, 5, &dx, &dy);
    TEST_ASSERT_EQUAL_INT(0, dy);

    /* (0, 0) (the "shadow disabled" sentinel draw_title() checks for)
     * must stay the identity, not become a spurious offset. */
    boot_anim_title_shadow_offset(0, 0, &dx, &dy);
    TEST_ASSERT_EQUAL_INT(0, dx);
    TEST_ASSERT_EQUAL_INT(0, dy);
}

#ifndef DEVICE_BUILD
/*
 * The rest pose
 *
 * Without the clip, boot holds an authored pose: it must draw, follow the same
 * view rules as the clip, and leave nothing mounted. Each case loads the
 * motion the way boot does, after the seed has let go of the shipped pack,
 * so the store mounts what the case leaves.
 */

#define FALLBACK_PACK   "./" BOOT_CLIP ".apak"
#define NO_PACKS        "./suite_boot_anim_no_such_folder"
#define FALLBACK_TRACKS 6

static const char* const NODE_PARTS[FALLBACK_TRACKS] = {"camera/translation", "camera/rotation", "camera/scale",
                                                        "space/translation",  "space/rotation",  "space/scale"};

static const char* const TRANSFORM_PARTS[] = {"position", "rotation", "scale"};

static void
six_tracks(test_track_t rows[FALLBACK_TRACKS]) {
    for (int i = 0; i < FALLBACK_TRACKS; i++) {
        const bool rotation = i % 3 == 1;
        rows[i] = (test_track_t){.path = i < 3 ? "camera" : "space",
                                 .field = TRANSFORM_PARTS[i % 3],
                                 .component = ANIM_COMPONENT_TRANSFORM,
                                 .type = rotation ? ANIM_VALUE_QUAT : ANIM_VALUE_VEC3};
    }
}

/* Writes pack BOOT_CLIP in "." holding a TRCK of `rows`, one key
 * each, row i holding i * 10 + 1, + 2, ... so each track can be told apart. */
static void
write_clip(const test_track_t* rows, int count) {
    enum { BYTES = 1024 };

    const uint32_t times_at =
        TEST_TRACKS_HEADER_SIZE + (uint32_t)(count * (TEST_TRACK_ROW_SIZE + TEST_TRACK_STRING_SLOT));
    const uint32_t values_at = times_at + 16U;
    uint8_t* bytes = malloc(BYTES);
    TEST_ASSERT_NOT_NULL(bytes);
    test_pack_t pack = test_pack_begin(bytes, BYTES, 1);
    uint8_t* entry = test_pack_add(&pack, BOOT_CLIP, ANIM_TRACKS_ASSET, values_at + (16U * (uint32_t)count));
    test_tracks_header(entry, count, 1000);
    const float at_start[] = {0.0F};
    test_pack_put_floats(entry + times_at, at_start, 1);
    for (int i = 0; i < count; i++) {
        test_track_t row = rows[i];
        row.times = times_at;
        row.values = values_at + (16U * (uint32_t)i);
        row.keys = 1;
        row.interp = ANIM_STEP;
        test_track_row(entry, i, &row);
        const float base = (float)(i * 10);
        const float values[] = {base + 1.0F, base + 2.0F, base + 3.0F, base + 4.0F};
        test_pack_put_floats(entry + row.values, values, 4);
    }
    const uint32_t size = test_pack_finish(&pack);
    test_write_file(FALLBACK_PACK, bytes, size);
    free(bytes);
}

static void
load_from(const char* dir, boot_anim_motion_t* out) {
    test_asset_dir_use(dir);
    boot_anim_motion_load(out);
    test_asset_dir_restore();
}

static bool
same_track(const anim_track_t* a, const anim_track_t* b) {
    return a->times == b->times && a->values == b->values && a->count == b->count && a->width == b->width
           && a->interp == b->interp && a->quaternion == b->quaternion;
}

static bool
same_node(const anim_node_tracks_t* a, const anim_node_tracks_t* b) {
    return same_track(&a->translation, &b->translation) && same_track(&a->rotation, &b->rotation)
           && same_track(&a->scale, &b->scale);
}

static int
lit_pixels(void) {
    const gfx_color_t* fb = gfx_framebuffer();
    int lit = 0;
    for (int i = 0; i < GFX_WIDTH * GFX_HEIGHT; i++) {
        lit += fb[i] != 0;
    }
    return lit;
}

/* Loads from `dir` and expects exactly the rest pose a missing pack gives,
 * pointing into nothing that was mounted, then draws it a moment before the
 * title and the photograph, so whatever is lit the motion put there. */
static void
expect_the_rest_pose(const char* dir) {
    if (gfx_mode_current()->width == 0) {
        TEST_ASSERT_TRUE(gfx_init());
    }
    suite_set_test_cleanup(gfx_reset_for_test);
    const uint32_t now_ms = BOOT_ANIM_TITLE_START_MS - 1;
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_photo_reveal(now_ms));

    boot_anim_motion_t rest;
    load_from(NO_PACKS, &rest);
    boot_anim_motion_t motion;
    load_from(dir, &motion);
    const bool from_pack = motion.from_pack;
    const bool is_rest = !from_pack && motion.clip.duration_ms == rest.clip.duration_ms
                         && same_node(&motion.camera, &rest.camera) && same_node(&motion.space, &rest.space);
    const image_t no_photo = {0};
    boot_anim_draw_frame(&motion, &no_photo, now_ms);
    boot_anim_motion_release(&motion);
    TEST_ASSERT_FALSE_MESSAGE(from_pack, "the motion came from the pack");
    TEST_ASSERT_TRUE_MESSAGE(is_rest, "a failed load left something other than the rest pose");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(GFX_WIDTH * GFX_HEIGHT / 100, lit_pixels(),
                                         "the rest pose left the panel all but blank");
}

/* A failed load holds no use of the pack: the next load mounts the file as
 * it is then. */
static void
expect_a_good_clip_loads_next(void) {
    test_track_t rows[FALLBACK_TRACKS];
    six_tracks(rows);
    write_clip(rows, FALLBACK_TRACKS);
    boot_anim_motion_t motion;
    load_from(".", &motion);
    const bool from_pack = motion.from_pack;
    boot_anim_motion_release(&motion);
    (void)remove(FALLBACK_PACK);
    TEST_ASSERT_TRUE_MESSAGE(from_pack, "the failed load left its pack mounted");
}

static void
test_with_no_pack_the_rest_pose_draws(void) {
    expect_the_rest_pose(NO_PACKS);
}

static void
test_with_a_track_missing_the_rest_pose_draws_and_nothing_stays_mounted(void) {
    test_track_t rows[FALLBACK_TRACKS];
    six_tracks(rows);
    rows[4] = rows[5];
    write_clip(rows, FALLBACK_TRACKS - 1);
    expect_the_rest_pose(".");
    (void)remove(FALLBACK_PACK);
    expect_a_good_clip_loads_next();
}

/* The camera binds before the space, so this fails after a node is bound. */
static void
test_with_a_malformed_space_the_rest_pose_draws_and_nothing_stays_mounted(void) {
    test_track_t rows[FALLBACK_TRACKS];
    six_tracks(rows);
    rows[4].type = ANIM_VALUE_VEC3;
    write_clip(rows, FALLBACK_TRACKS);
    expect_the_rest_pose(".");
    (void)remove(FALLBACK_PACK);
    expect_a_good_clip_loads_next();
}

static void
test_each_part_of_the_motion_is_the_track_of_its_name(void) {
    test_track_t rows[FALLBACK_TRACKS];
    six_tracks(rows);
    write_clip(rows, FALLBACK_TRACKS);
    boot_anim_motion_t motion;
    load_from(".", &motion);
    const anim_track_t* parts[FALLBACK_TRACKS] = {&motion.camera.translation, &motion.camera.rotation,
                                                  &motion.camera.scale,       &motion.space.translation,
                                                  &motion.space.rotation,     &motion.space.scale};
    float first[FALLBACK_TRACKS];
    for (int i = 0; i < FALLBACK_TRACKS; i++) {
        float out[ANIM_WIDTH_MAX];
        anim_track_sample(parts[i], 0.0F, out);
        first[i] = out[0];
    }
    const bool from_pack = motion.from_pack;
    boot_anim_motion_release(&motion);
    (void)remove(FALLBACK_PACK);
    TEST_ASSERT_TRUE(from_pack);
    for (int i = 0; i < FALLBACK_TRACKS; i++) {
        float expected = (float)(i * 10) + 1.0F;
        if (rows[i].type == ANIM_VALUE_QUAT) {
            const float base = (float)(i * 10);
            const quatf_t unit = quatf_normalize((quatf_t){base + 1, base + 2, base + 3, base + 4});
            expected = unit.x;
        }
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(expected, first[i], NODE_PARTS[i]);
    }
}

/* The rest pose answers to the view rules the shipped clip does. */
static void
test_the_rest_pose_keeps_the_seeds_view_rules(void) {
    boot_anim_motion_t rest;
    load_from(NO_PACKS, &rest);
    TEST_ASSERT_FALSE(rest.from_pack);
    expect_the_curve_near_the_panel_throughout(&rest);
    expect_three_distinct_axes(&rest);
    expect_the_narrow_transform_across_the_motion(&rest);
}

/*
 * The photograph
 *
 * Without the boot picture's pack, or with a picture boot cannot draw, boot
 * draws on without it: the scene stays where the photograph would have
 * covered it, and nothing stays mounted.
 */

#define PICTURE_PACK "./" BOOT_PHOTO ".apak"

/* Writes pack BOOT_PHOTO in "." holding a white IMAG of `width` x `height`, its
 * header stating `version`. */
static void
write_picture(int version, int width, int height) {
    const uint32_t pixels_at = 16;
    const uint32_t entry_size = pixels_at + ((uint32_t)(width * height) * sizeof(gfx_color_t));
    const uint32_t capacity = 256 + entry_size;
    uint8_t* bytes = malloc(capacity);
    TEST_ASSERT_NOT_NULL(bytes);
    test_pack_t pack = test_pack_begin(bytes, capacity, 1);
    uint8_t* entry = test_pack_add(&pack, BOOT_PHOTO, DRAW_IMAGE_ASSET, entry_size);
    test_pack_put16(entry, version);
    test_pack_put16(entry + 2, DRAW_IMAGE_FORMAT_RGB565);
    test_pack_put16(entry + 4, width);
    test_pack_put16(entry + 6, height);
    test_pack_put32(entry + 8, (uint32_t)width);
    test_pack_put32(entry + 12, pixels_at);
    memset(entry + pixels_at, 0xFF, entry_size - pixels_at);
    const uint32_t size = test_pack_finish(&pack);
    test_write_file(PICTURE_PACK, bytes, size);
    free(bytes);
}

static void
load_photo_from(const char* dir, image_t* out) {
    test_asset_dir_use(dir);
    boot_anim_photo_load(out);
    test_asset_dir_restore();
}

/* The lit pixels of the shipped motion's frame at `now_ms` drawn over `photo`. */
static int
lit_over(const image_t* photo, uint32_t now_ms) {
    boot_anim_motion_t motion;
    boot_anim_motion_load(&motion);
    boot_anim_draw_frame(&motion, photo, now_ms);
    boot_anim_motion_release(&motion);
    return lit_pixels();
}

/* Loads the picture from `dir` and expects none, holding nothing mounted;
 * then, once the crossfade would be over, expects the frame to light more
 * than the same frame over a black photograph, which shows the title alone. */
static void
expect_no_photograph(const char* dir) {
    if (gfx_mode_current()->width == 0) {
        TEST_ASSERT_TRUE(gfx_init());
    }
    suite_set_test_cleanup(gfx_reset_for_test);
    const uint32_t now_ms = BOOT_ANIM_IMAGE_START_MS + BOOT_ANIM_IMAGE_FADE_MS;
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ink(now_ms));

    image_t photo;
    load_photo_from(dir, &photo);
    const bool no_pixels = photo.pixels == NULL;
    const int without = lit_over(&photo, now_ms);
    boot_anim_photo_release(&photo);

    /* One black row repeated down the panel: a stride of 0. */
    gfx_color_t* black = calloc(GFX_WIDTH, sizeof(gfx_color_t));
    TEST_ASSERT_NOT_NULL(black);
    const image_t covered = {.pixels = black,
                             .format = IMAGE_RGB565BE,
                             .width = GFX_WIDTH,
                             .height = GFX_HEIGHT,
                             .pitch = 0 * sizeof(gfx_color_t)};
    const int title_only = lit_over(&covered, now_ms);
    free(black);

    TEST_ASSERT_TRUE_MESSAGE(no_pixels, "a failed load left the photograph's pixels behind");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(title_only + (GFX_WIDTH * GFX_HEIGHT / 100), without,
                                         "without the photograph the scene did not stay");
}

/* A failed load holds no use of the pack: the next load mounts the shipped
 * pack, not the one that failed. */
static void
expect_a_good_picture_loads_next(void) {
    image_t photo;
    boot_anim_photo_load(&photo);
    const bool from_pack = photo.pixels != NULL;
    boot_anim_photo_release(&photo);
    TEST_ASSERT_TRUE_MESSAGE(from_pack, "the failed load left its pack mounted");
}

static void
test_with_no_pack_the_scene_stays_where_the_photograph_would_be(void) {
    expect_no_photograph(NO_PACKS);
}

static void
test_with_a_picture_not_one_panel_the_scene_stays_and_nothing_stays_mounted(void) {
    write_picture(DRAW_IMAGE_VERSION, 2, 2);
    expect_no_photograph(".");
    (void)remove(PICTURE_PACK);
    expect_a_good_picture_loads_next();
}

static void
test_with_a_malformed_picture_the_scene_stays_and_nothing_stays_mounted(void) {
    write_picture(DRAW_IMAGE_VERSION + 1, 2, 2);
    expect_no_photograph(".");
    (void)remove(PICTURE_PACK);
    expect_a_good_picture_loads_next();
}

/* With the shipped pack, the crossfade ends on the photograph itself: every
 * pixel the title leaves is the picture's. */
static void
test_the_crossfade_ends_on_the_shipped_photograph(void) {
    if (gfx_mode_current()->width == 0) {
        TEST_ASSERT_TRUE(gfx_init());
    }
    suite_set_test_cleanup(gfx_reset_for_test);
    image_t photo;
    boot_anim_photo_load(&photo);
    if (photo.pixels == NULL) {
        TEST_FAIL_MESSAGE("the boot picture did not load from its pack: see the log above");
    }
    (void)lit_over(&photo, BOOT_ANIM_IMAGE_START_MS + BOOT_ANIM_IMAGE_FADE_MS);
    const gfx_color_t* fb = gfx_framebuffer();
    int same = 0;
    for (int y = 0; y < GFX_HEIGHT; y++) {
        const gfx_color_t* row = image_row(&photo, y);
        for (int x = 0; x < GFX_WIDTH; x++) {
            same += fb[(y * GFX_WIDTH) + x] == row[x];
        }
    }
    boot_anim_photo_release(&photo);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(GFX_WIDTH * GFX_HEIGHT * 9 / 10, same,
                                         "the end of the crossfade is not the photograph");
}
#endif

void
run_boot_anim_suite(void) {
    seed = malloc(sizeof *seed);
    if (seed == NULL) {
        RUN_TEST(test_the_seed_is_the_boot_clip_read_from_its_pack);
        return;
    }
    boot_anim_motion_load(seed);
    RUN_TEST(test_the_seed_is_the_boot_clip_read_from_its_pack);
    RUN_TEST(test_the_curve_climbs_from_zero_to_the_top);
    RUN_TEST(test_the_curve_never_descends);
    RUN_TEST(test_the_curve_meets_the_axis_at_every_known_zero);
    RUN_TEST(test_the_curve_keeps_away_from_the_axis_everywhere_else);
    RUN_TEST(test_every_listed_zero_is_on_the_climb_and_in_order);
    RUN_TEST(test_samples_are_clamped_rather_than_read_out_of_range);

    RUN_TEST(test_the_quarter_wave_starts_at_zero_and_ends_at_one);
    RUN_TEST(test_the_quarter_wave_rises_all_the_way);
    RUN_TEST(test_sin_squared_plus_cos_squared_is_one);
    RUN_TEST(test_the_quarter_points_are_exact);
    RUN_TEST(test_identity_transform_leaves_an_on_axis_point_at_screen_centre);
    RUN_TEST(test_a_point_further_from_the_camera_projects_smaller);
    RUN_TEST(test_project_point_rejects_a_point_at_the_near_plane);
    RUN_TEST(test_project_segment_cs_rejects_a_segment_entirely_behind);
    RUN_TEST(test_project_segment_cs_clips_asymmetric_coordinates);
    RUN_TEST(test_an_untouched_scale_reads_back_as_identity);
    RUN_TEST(test_the_seeds_curve_stays_near_the_panel_throughout);
    RUN_TEST(test_the_seeds_three_axes_project_to_distinct_directions);

    RUN_TEST(test_floor_is_flat_through_impact);
    RUN_TEST(test_floor_is_flat_well_ahead_of_the_train);
    RUN_TEST(test_ripple_peak_falls_over_time);
    RUN_TEST(test_swell_is_zero_through_its_start);
#ifndef DEVICE_BUILD
    RUN_TEST(test_baked_floor_matches_direct_float);
    RUN_TEST(test_baked_far_swell_matches_direct_float);
#endif
    RUN_TEST(test_the_title_wave_is_at_full_swing_before_it_calms);
    RUN_TEST(test_the_title_wave_reaches_stillness_after_its_fade);
    RUN_TEST(test_the_title_wave_never_swings_wider_as_it_calms);
    RUN_TEST(test_the_axis_lines_leave_the_mesh_without_a_step);
    RUN_TEST(test_the_far_samples_climb_from_the_mesh_edge_to_far);
    RUN_TEST(test_the_floor_grows_from_the_mesh_edge_to_far);
    RUN_TEST(test_the_far_lines_thin_out_with_distance);
    RUN_TEST(test_the_far_chord_is_the_floor_of_the_true_half_chord);
    RUN_TEST(test_the_far_swell_ends_where_the_flat_lines_begin);

    RUN_TEST(test_a_span_starts_and_ends_halfway_between_its_points);
    RUN_TEST(test_a_repeated_point_pins_the_end_of_the_curve);
    RUN_TEST(test_a_span_never_leaves_its_control_points_behind);
    RUN_TEST(test_a_span_climbs_steadily_when_its_points_do);
    RUN_TEST(test_spline_cs_matches_transforming_the_world_space_spline);
    RUN_TEST(test_plane_points_match_the_full_camera_space_transform);
    RUN_TEST(test_the_curve_table_stays_inside_the_narrow_range);
    RUN_TEST(test_camera_transform_selects_the_narrow_path_at_negative_input_limits);
    RUN_TEST(test_the_narrow_camera_transform_agrees_with_the_wide_one_across_the_motion);
    RUN_TEST(test_lod_stride_tiers_by_extent);

    RUN_TEST(test_the_pen_runs_from_nothing_to_phase_ones_end);
    RUN_TEST(test_the_pen_keeps_climbing_through_phase_two);
    RUN_TEST(test_the_curve_is_finished_by_pen_finish_ms);
    RUN_TEST(test_the_seed_finishes_the_curve_before_the_dissolve_starts);
    RUN_TEST(test_the_picture_is_lit_until_the_dissolve_and_dark_at_the_end);
    RUN_TEST(test_the_photograph_arrives_over_its_own_window);
    RUN_TEST(test_the_photograph_is_mounted_after_the_first_frame);
    RUN_TEST(test_the_seed_finishes_the_crossfade_before_the_dissolve_starts);
    RUN_TEST(test_the_floor_fades_in_from_the_origin_outward);
    RUN_TEST(test_the_floor_fades_out_with_distance_rather_than_stopping);
    RUN_TEST(test_only_positive_crests_take_the_crest_colour);
    RUN_TEST(test_the_crest_runs_give_the_nearest_drawn_sample);
    RUN_TEST(test_the_axes_are_there_before_the_curve_starts_climbing);

    RUN_TEST(test_height_changes_the_hue);
    RUN_TEST(test_a_pens_trail_fades_behind_it);
    RUN_TEST(test_every_pen_is_lit_at_its_own_position);
    RUN_TEST(test_the_pens_carry_different_colours);
    RUN_TEST(test_a_pens_colour_arrives_gradually);
    RUN_TEST(test_settled_curve_keeps_its_colour);
    RUN_TEST(test_the_trail_never_washes_out_to_white);
    RUN_TEST(test_the_live_end_of_the_curve_is_drawn_thicker);

    RUN_TEST(test_the_wobble_is_exactly_flat_once_a_letter_has_arrived);
    RUN_TEST(test_the_wobbles_oscillation_slows_as_it_lands);
    RUN_TEST(test_a_letter_lands_exactly_on_its_final_position);
    RUN_TEST(test_letters_are_staggered_left_to_right);
    RUN_TEST(test_a_letter_starts_off_panel_to_the_left);
    RUN_TEST(test_final_x_matches_the_advance_sum);
    RUN_TEST(test_the_title_stays_on_the_panel_once_visible);
    RUN_TEST(test_title_shadow_offset_turns_reader_frame_into_panel_frame);
    boot_anim_motion_release(seed);
    free(seed);
    seed = NULL;
#ifndef DEVICE_BUILD
    RUN_TEST(test_with_no_pack_the_rest_pose_draws);
    RUN_TEST(test_with_a_track_missing_the_rest_pose_draws_and_nothing_stays_mounted);
    RUN_TEST(test_with_a_malformed_space_the_rest_pose_draws_and_nothing_stays_mounted);
    RUN_TEST(test_each_part_of_the_motion_is_the_track_of_its_name);
    RUN_TEST(test_the_rest_pose_keeps_the_seeds_view_rules);
    RUN_TEST(test_with_no_pack_the_scene_stays_where_the_photograph_would_be);
    RUN_TEST(test_with_a_picture_not_one_panel_the_scene_stays_and_nothing_stays_mounted);
    RUN_TEST(test_with_a_malformed_picture_the_scene_stays_and_nothing_stays_mounted);
    RUN_TEST(test_the_crossfade_ends_on_the_shipped_photograph);
#endif
}

SUITE_REGISTER(run_boot_anim_suite);
SUITE_READS(run_boot_anim_suite, BOOT_CLIP, BOOT_PHOTO);
