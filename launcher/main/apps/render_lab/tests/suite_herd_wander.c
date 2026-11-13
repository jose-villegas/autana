/*
 * Portable suite: herd/herd_wander.h. One seed walks one way at any step size,
 * a seek lands where advancing does, and the copies keep inside the area,
 * apart from each other, and through every gait.
 */
#include <math.h>
#include <string.h>

#include "suites.h"
#include "testkit/suite.h"
#include "unity.h"

#include "apps/render_lab/herd/herd_wander.h"
#include "math/scalar/math_const.h"

#define SEED         0x5EEDu
#define LONG_MS      120000u /* two minutes of walking */
#define SETTLE_MS    2000u   /* the starts may be close; separation has this long to part them */
#define CLOSEST_M    0.5F    /* settled copies never nearer than this (half a body length) */
#define MOVED_M      10.0F   /* every copy walks at least this far in LONG_MS */
#define GAITS_MIN    3       /* every copy goes through at least this many gait steps in LONG_MS */
#define AREA_SLACK_M 1.0F    /* a copy may overshoot the area by an arrive and a push */

static const gait_step_t STEPS[] = {
    {.clip = 0, .ground_mps = 0.0F},
    {.clip = 1, .ground_mps = 0.8F},
    {.clip = 2, .ground_mps = 1.6F},
    {.clip = 3, .ground_mps = 3.5F},
};
static const gait_t GAIT = {STEPS, 4, 0.75F, 1.4F, 0.2F, 300};
static const float LEGS[] = {0.0F, 0.8F, 1.6F, 3.5F};
static const herd_wander_config_t CONFIG = {
    .limits = {.max_speed = 4.0F,
               .max_accel = 6.0F,
               .turn_rate = MATH_PI,
               .arrive_radius = 1.0F,
               .separation_radius = 1.2F,
               .heading_speed = 0.05F,
               .horizon_s = 1.0F},
    .area_radius = 6.0F,
    .leg_min_m = 2.0F,
    .leg_speeds = LEGS,
    .leg_count = 4,
    .graze_min_ms = 1000,
    .graze_max_ms = 4000,
    .gait = &GAIT,
};

static herd_wander_t a, b; /* static: a herd is too large for the test task's stack */

static void
starts(steer2f_agent_t* out, int count) {
    for (int i = 0; i < count; i++) {
        const float angle = MATH_TAU * (float)i / (float)count;
        out[i] = (steer2f_agent_t){{3.0F * cosf(angle), 3.0F * sinf(angle)}, {0.0F, 0.0F}, angle};
    }
}

static void
begin(herd_wander_t* w) {
    steer2f_agent_t s[HERD_WANDER_MAX];
    starts(s, HERD_WANDER_MAX);
    memset(w, 0, sizeof *w);
    herd_wander_start(w, &CONFIG, SEED, s, HERD_WANDER_MAX);
}

static void
assert_same_walkers(const herd_wander_t* x, const herd_wander_t* y) {
    TEST_ASSERT_EQUAL_UINT32(x->t_ms, y->t_ms);
    for (int i = 0; i < HERD_WANDER_MAX; i++) {
        TEST_ASSERT_EQUAL_FLOAT(x->walker[i].agent.position.x, y->walker[i].agent.position.x);
        TEST_ASSERT_EQUAL_FLOAT(x->walker[i].agent.position.y, y->walker[i].agent.position.y);
        TEST_ASSERT_EQUAL_FLOAT(x->walker[i].agent.heading, y->walker[i].agent.heading);
        TEST_ASSERT_EQUAL_INT(x->walker[i].gait.step, y->walker[i].gait.step);
        TEST_ASSERT_EQUAL_FLOAT(x->walker[i].clip_s, y->walker[i].clip_s);
    }
}

static void
test_one_seed_walks_one_way_at_any_step_size(void) {
    static const uint32_t chunks[] = {7, 13, 1, 33, 20, 41, 3};
    begin(&a);
    begin(&b);
    herd_wander_advance(&a, LONG_MS);
    for (uint32_t t = 0, i = 0; t < LONG_MS; i++) {
        const uint32_t dt = chunks[i % (sizeof chunks / sizeof chunks[0])];
        const uint32_t step = t + dt > LONG_MS ? LONG_MS - t : dt;
        herd_wander_advance(&b, step);
        t += step;
    }
    TEST_ASSERT_EQUAL_UINT32(LONG_MS, a.t_ms);
    assert_same_walkers(&a, &b);
}

static void
test_a_seek_lands_where_advancing_does_both_ways(void) {
    begin(&a);
    begin(&b);
    herd_wander_advance(&a, LONG_MS / 2);
    herd_wander_seek(&b, LONG_MS);
    herd_wander_seek(&b, LONG_MS / 2);
    TEST_ASSERT_EQUAL_UINT32(LONG_MS / 2, b.t_ms);
    assert_same_walkers(&a, &b);
}

static float
gap(const herd_wander_t* w, int i, int j) {
    const vec2f_t d = vec2f_sub(w->walker[i].agent.position, w->walker[j].agent.position);
    return sqrtf(vec2f_dot(d, d));
}

static void
test_copies_keep_inside_the_area_and_apart(void) {
    begin(&a);
    herd_wander_advance(&a, SETTLE_MS);
    for (uint32_t t = SETTLE_MS; t < LONG_MS; t += HERD_WANDER_STEP_MS) {
        herd_wander_advance(&a, HERD_WANDER_STEP_MS);
        for (int i = 0; i < HERD_WANDER_MAX; i++) {
            const vec2f_t p = a.walker[i].agent.position;
            TEST_ASSERT_TRUE(sqrtf(vec2f_dot(p, p)) <= CONFIG.area_radius + AREA_SLACK_M);
            for (int j = i + 1; j < HERD_WANDER_MAX; j++) {
                TEST_ASSERT_TRUE(gap(&a, i, j) >= CLOSEST_M);
            }
        }
    }
}

static void
test_every_copy_walks_and_changes_gait(void) {
    float moved[HERD_WANDER_MAX] = {0};
    bool seen[HERD_WANDER_MAX][4] = {{false}};
    begin(&a);
    for (uint32_t t = 0; t < LONG_MS; t += HERD_WANDER_STEP_MS) {
        vec2f_t before[HERD_WANDER_MAX];
        for (int i = 0; i < HERD_WANDER_MAX; i++) {
            before[i] = a.walker[i].agent.position;
        }
        herd_wander_advance(&a, HERD_WANDER_STEP_MS);
        for (int i = 0; i < HERD_WANDER_MAX; i++) {
            const vec2f_t d = vec2f_sub(a.walker[i].agent.position, before[i]);
            moved[i] += sqrtf(vec2f_dot(d, d));
            TEST_ASSERT_TRUE(a.walker[i].gait.step >= 0 && a.walker[i].gait.step < GAIT.count);
            seen[i][a.walker[i].gait.step] = true;
        }
    }
    for (int i = 0; i < HERD_WANDER_MAX; i++) {
        TEST_ASSERT_TRUE(moved[i] >= MOVED_M);
        int gaits = 0;
        for (int s = 0; s < GAIT.count; s++) {
            gaits += seen[i][s] ? 1 : 0;
        }
        TEST_ASSERT_TRUE(gaits >= GAITS_MIN);
    }
}

static void
test_the_clip_time_follows_the_rate(void) {
    begin(&a);
    herd_wander_advance(&a, LONG_MS / 4);
    const herd_walker_t before = a.walker[0];
    herd_wander_advance(&a, HERD_WANDER_STEP_MS);
    const herd_walker_t after = a.walker[0];
    TEST_ASSERT_TRUE(after.gait.rate > 0.0F);
    if (after.gait.step == before.gait.step) {
        TEST_ASSERT_FLOAT_WITHIN(1.0E-5F, before.clip_s + (after.gait.rate * HERD_WANDER_STEP_MS / 1000.0F),
                                 after.clip_s);
    } else {
        TEST_ASSERT_EQUAL_FLOAT(0.0F, after.clip_s);
    }
}

void
run_herd_wander_suite(void) {
    RUN_TEST(test_one_seed_walks_one_way_at_any_step_size);
    RUN_TEST(test_a_seek_lands_where_advancing_does_both_ways);
    RUN_TEST(test_copies_keep_inside_the_area_and_apart);
    RUN_TEST(test_every_copy_walks_and_changes_gait);
    RUN_TEST(test_the_clip_time_follows_the_rate);
}

SUITE_REGISTER(run_herd_wander_suite);
