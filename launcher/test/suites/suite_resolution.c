#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "render/resolution/resolution.h"

#define BUDGET_US 66000

/* 1, 1.25, 1.5, 2, the floor 2.5 and the recovery step 3 of a 368x448
 * panel, as the board measured their mean frame cost at one load. */
static const resolution_step_t sizes[] = {{368, 448}, {294, 358}, {245, 298}, {184, 224}, {147, 179}, {122, 149}};
static const int32_t step_us[] = {111000, 86000, 72000, 54000, 49000, 44000};
#define STEP_COUNT ((int)(sizeof sizes / sizeof sizes[0]))
#define RECOVERY   5

static resolution_config_t
config(void) {
    return resolution_config(sizes, STEP_COUNT, RECOVERY, BUDGET_US);
}

/* A repeatable +-3% wobble, so a window's mean is never exactly a step's cost. */
static int32_t
wobble(int32_t us, unsigned* seed) {
    *seed = (*seed * 1103515245U) + 12345U;
    const int permille = (int)((*seed >> 16) % 61U) - 30;
    return us + (us * permille / 1000);
}

/* Runs `frames` frames of a load that scales every step's cost by
 * `load_percent`, returning the step the controller ends on. */
static int
run(resolution_control_t* c, const resolution_config_t* config, int frames, int load_percent, unsigned* seed) {
    int step = c->step;
    for (int i = 0; i < frames; i++) {
        step = resolution_control_update(c, config, wobble(step_us[step] * load_percent / 100, seed));
    }
    return step;
}

static void
test_over_budget_it_steps_down_one_step_at_a_time_to_the_first_that_fits(void) {
    const resolution_config_t cfg = config();
    resolution_control_t c;
    resolution_control_init(&c, &cfg, 1);
    unsigned seed = 1;
    int previous = 1;
    for (int i = 0; i < 400; i++) {
        const int step = resolution_control_update(&c, &cfg, wobble(step_us[c.step], &seed));
        TEST_ASSERT_TRUE(step - previous <= 1);
        previous = step;
    }
    TEST_ASSERT_EQUAL_INT(3, c.step);
    TEST_ASSERT_EQUAL_INT(2, c.switches);
}

static void
test_once_the_load_lifts_it_recovers_the_finer_step(void) {
    const resolution_config_t cfg = config();
    resolution_control_t c;
    resolution_control_init(&c, &cfg, 2);
    unsigned seed = 2;
    TEST_ASSERT_EQUAL_INT(2, run(&c, &cfg, 200, 85, &seed));  /* 72 ms at 85% fits */
    TEST_ASSERT_EQUAL_INT(3, run(&c, &cfg, 200, 110, &seed)); /* heavier: 2x */
    TEST_ASSERT_EQUAL_INT(1, run(&c, &cfg, 400, 60, &seed));  /* lighter: up to 1.25x, which fits */
}

static void
test_a_steady_load_settles_and_stays(void) {
    const resolution_config_t cfg = config();
    resolution_control_t c;
    resolution_control_init(&c, &cfg, 0);
    unsigned seed = 3;
    (void)run(&c, &cfg, 200, 100, &seed);
    const int settled = c.switches;
    (void)run(&c, &cfg, 5000, 100, &seed);
    TEST_ASSERT_EQUAL_INT(settled, c.switches);
}

/* Two steps whose costs straddle both thresholds would flip each window;
 * the doubling cooldown spaces the flips out to COOLDOWN_MAX frames. */
static void
test_two_steps_straddling_the_thresholds_are_tried_less_and_less_often(void) {
    static const resolution_step_t pair[] = {{368, 448}, {184, 224}};
    const resolution_config_t cfg = resolution_config(pair, 2, 2, BUDGET_US);
    resolution_control_t c;
    resolution_control_init(&c, &cfg, 0);
    const int32_t cost[2] = {70000, 40000};
    int switches_early = 0;
    for (int i = 0; i < 6000; i++) {
        (void)resolution_control_update(&c, &cfg, cost[c.step]);
        if (i == 1000) {
            switches_early = c.switches;
        }
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(12, switches_early);
    TEST_ASSERT_LESS_OR_EQUAL_INT(switches_early + (5000 / 128) + 1, c.switches);
}

static void
test_one_frame_far_over_budget_drops_two_steps_into_recovery(void) {
    const resolution_config_t cfg = config();
    resolution_control_t c;
    resolution_control_init(&c, &cfg, 3);
    TEST_ASSERT_EQUAL_INT(5, resolution_control_update(&c, &cfg, BUDGET_US * 2));
}

static void
test_ordinary_steps_never_reach_a_recovery_step(void) {
    const resolution_config_t cfg = config();
    resolution_control_t c;
    resolution_control_init(&c, &cfg, 3);
    for (int i = 0; i < 1000; i++) {
        (void)resolution_control_update(&c, &cfg, BUDGET_US * 140 / 100);
    }
    TEST_ASSERT_EQUAL_INT(4, c.step); /* the floor, never past it */
}

static void
test_from_a_recovery_step_it_climbs_back_when_the_load_allows(void) {
    const resolution_config_t cfg = config();
    resolution_control_t c;
    resolution_control_init(&c, &cfg, 5);
    unsigned seed = 4;
    TEST_ASSERT_EQUAL_INT(4, run(&c, &cfg, 400, 100, &seed));
}

/* Frames priced by known weights: the fit gives them back. */
static void
test_the_fit_recovers_the_weights_frames_were_made_with(void) {
    const resolution_config_t cfg = config();
    const resolution_model_t truth = {12000.0F, 1.5F, 3.0F, 20000.0F, {13000, 10000, 9500, 5800, 8500, 5300}};

    enum { COUNT = 4 * 8 };

    resolution_sample_t* samples = malloc(sizeof(*samples) * COUNT);
    TEST_ASSERT_NOT_NULL(samples);
    for (int i = 0; i < COUNT; i++) {
        const int step = i % 4;
        const int triangles = 6000 + (i * 137 % 5000);
        const float upscale = truth.upscale_us[step];
        const float total = resolution_model_predict_us(&truth, &cfg, step, triangles);
        samples[i] = (resolution_sample_t){step, triangles, (int32_t)(total - upscale), (int32_t)upscale};
    }
    resolution_model_t fitted;
    TEST_ASSERT_TRUE(resolution_model_fit(&fitted, &cfg, samples, COUNT));
    TEST_ASSERT_FLOAT_WITHIN(20.0F, truth.base_us, fitted.base_us);
    TEST_ASSERT_FLOAT_WITHIN(0.01F, truth.per_triangle_us, fitted.per_triangle_us);
    TEST_ASSERT_FLOAT_WITHIN(0.01F, truth.per_triangle_row_us, fitted.per_triangle_row_us);
    TEST_ASSERT_FLOAT_WITHIN(50.0F, truth.per_pixel_share_us, fitted.per_pixel_share_us);
    TEST_ASSERT_FLOAT_WITHIN(1.0F, 9500.0F, fitted.upscale_us[2]);
    free(samples);
}

static void
test_frames_of_one_step_cannot_tell_the_weights_apart(void) {
    const resolution_config_t cfg = config();
    resolution_sample_t samples[4];
    for (int i = 0; i < 4; i++) {
        samples[i] = (resolution_sample_t){3, 9000, 50000, 5800};
    }
    resolution_model_t fitted;
    TEST_ASSERT_FALSE(resolution_model_fit(&fitted, &cfg, samples, 4));
}

static const resolution_model_t model = {10000.0F, 2.0F, 4.0F, 20000.0F, {13000, 10000, 9500, 5800, 8500, 5300}};

static void
test_the_predictor_draws_the_finest_step_its_model_says_fits(void) {
    const resolution_config_t cfg = config();
    resolution_predict_t p;
    resolution_predict_init(&p, &cfg, &model, 0);
    for (int step = 0; step < STEP_COUNT; step++) {
        const float us = resolution_model_predict_us(&model, &cfg, step, 6000);
        if (us <= (float)BUDGET_US) {
            TEST_ASSERT_EQUAL_INT(step, resolution_predict_choose(&p, &cfg, 6000));
            return;
        }
    }
    TEST_FAIL_MESSAGE("no step fits 6000 triangles");
}

static void
test_the_predictor_reports_the_price_of_its_chosen_step(void) {
    const resolution_config_t cfg = config();
    resolution_predict_t p;
    resolution_predict_init(&p, &cfg, &model, 0);
    for (int triangles = 3000; triangles <= 30000; triangles += 3000) {
        const int step = resolution_predict_choose(&p, &cfg, triangles);
        TEST_ASSERT_FLOAT_WITHIN(0.01F, resolution_model_predict_us(&p.model, &cfg, step, triangles), p.chosen_us);
    }
}

static void
test_the_predictor_steps_down_on_the_frame_the_load_arrives(void) {
    const resolution_config_t cfg = config();
    resolution_predict_t p;
    resolution_predict_init(&p, &cfg, &model, 0);
    const int light = resolution_predict_choose(&p, &cfg, 3000);
    const int heavy = resolution_predict_choose(&p, &cfg, 12000);
    TEST_ASSERT_GREATER_THAN_INT(light, heavy);
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT((float)BUDGET_US, resolution_model_predict_us(&model, &cfg, heavy, 12000));
}

/* A load whose finer step lands just inside the budget does not pull the
 * predictor back up: going finer needs a margin. */
/* Over budget at every ordinary step but short of the panic share, the
 * predictor holds the floor rather than drop to recovery; past it, it drops. */
static void
test_the_predictor_holds_the_floor_until_the_panic_share(void) {
    const resolution_config_t cfg = config();
    resolution_predict_t p;
    resolution_predict_init(&p, &cfg, &model, 0);
    TEST_ASSERT_GREATER_THAN_FLOAT((float)BUDGET_US, resolution_model_predict_us(&model, &cfg, RECOVERY - 1, 15000));
    TEST_ASSERT_EQUAL_INT(RECOVERY - 1, resolution_predict_choose(&p, &cfg, 15000));
    TEST_ASSERT_EQUAL_INT(RECOVERY, resolution_predict_choose(&p, &cfg, 30000));
}

static void
test_the_predictor_does_not_flip_at_the_edge_of_the_budget(void) {
    const resolution_config_t cfg = config();
    resolution_predict_t p;
    resolution_predict_init(&p, &cfg, &model, 0);
    int triangles = 1000;
    while (resolution_model_predict_us(&model, &cfg, 2, triangles) <= (float)BUDGET_US) {
        triangles += 10;
    }
    TEST_ASSERT_EQUAL_INT(3, resolution_predict_choose(&p, &cfg, triangles));
    const int switches = p.switches;
    for (int i = 0; i < 100; i++) {
        (void)resolution_predict_choose(&p, &cfg, triangles - 30 + (i % 2) * 40);
    }
    TEST_ASSERT_EQUAL_INT(switches, p.switches);
}

/* Draws 20% slower than the prior must still fit the chosen step. */
static void
test_measured_frames_correct_a_model_that_runs_fast(void) {
    const resolution_config_t cfg = config();
    resolution_predict_t p;
    resolution_predict_init(&p, &cfg, &model, 0);
    for (int i = 0; i < 200; i++) {
        const int step = resolution_predict_choose(&p, &cfg, 6000);
        resolution_predict_measured(
            &p, &cfg, 6000,
            (int32_t)(1.2F * (resolution_model_predict_us(&model, &cfg, step, 6000) - model.upscale_us[step])),
            (int32_t)model.upscale_us[step]);
    }
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT(
        1.01F * (float)BUDGET_US,
        1.2F * (resolution_model_predict_us(&model, &cfg, p.step, 6000) - model.upscale_us[p.step])
            + model.upscale_us[p.step]);
}

static resolution_model_t
changed_model(void) {
    resolution_model_t truth = model;
    truth.base_us += 3000.0F;
    truth.per_triangle_us *= 1.5F;
    truth.per_pixel_share_us *= 0.8F;
    return truth;
}

static void
feed_frames(resolution_predict_t* p, const resolution_config_t* cfg, const resolution_model_t* truth, int frames,
            int held_step, unsigned* seed) {
    for (int i = 0; i < frames; i++) {
        *seed = (*seed * 1103515245U) + 12345U;
        p->step = held_step >= 0 ? held_step : (int)((*seed >> 16) % (unsigned)cfg->step_count);
        const int triangles = 1000 + (int)((*seed >> 8) % 19000U);
        const float draw = resolution_model_predict_us(truth, cfg, p->step, triangles) - truth->upscale_us[p->step];
        *seed = (*seed * 1103515245U) + 12345U;
        const float noise = 1.0F + (float)((int)((*seed >> 16) % 101U) - 50) / 1000.0F;
        resolution_predict_measured(p, cfg, triangles, (int32_t)(draw * noise), (int32_t)truth->upscale_us[p->step]);
    }
}

static void
assert_prices(const resolution_predict_t* p, const resolution_config_t* cfg, const resolution_model_t* truth,
              float share) {
    for (int step = 0; step < cfg->step_count; step++) {
        for (int triangles = 1000; triangles <= 20000; triangles += 1000) {
            const float expected = resolution_model_predict_us(truth, cfg, step, triangles);
            TEST_ASSERT_FLOAT_WITHIN(share * expected, expected,
                                     resolution_model_predict_us(&p->model, cfg, step, triangles));
        }
    }
}

static void
check_refit_prices(const resolution_model_t* truth, int warmup_frames, int frames, float error_share) {
    const resolution_config_t cfg = config();
    resolution_predict_t* p = malloc(sizeof(*p));
    TEST_ASSERT_NOT_NULL(p);
    for (unsigned seed_start = 1; seed_start <= 4; seed_start++) {
        unsigned seed = seed_start;
        resolution_predict_init(p, &cfg, &model, 0);
        feed_frames(p, &cfg, &model, warmup_frames, -1, &seed);
        feed_frames(p, &cfg, truth, frames, -1, &seed);
        assert_prices(p, &cfg, truth, error_share);
    }
    free(p);
}

static void
test_refit_converges_over_mixed_frames(void) {
    const resolution_model_t truth = changed_model();
    check_refit_prices(&truth, 0, 300, 0.04F);
}

static void
test_refit_held_step_does_not_drift(void) {
    const resolution_config_t cfg = config();
    const resolution_model_t truth = changed_model();
    resolution_predict_t* p = malloc(sizeof(*p));
    TEST_ASSERT_NOT_NULL(p);
    for (unsigned seed_start = 1; seed_start <= 4; seed_start++) {
        unsigned seed = seed_start;
        resolution_predict_init(p, &cfg, &model, 3);
        feed_frames(p, &cfg, &truth, 10000, 3, &seed);
        for (int i = 0; i < 4; i++) {
            TEST_ASSERT_TRUE(isfinite(p->covariance[i][i]));
            TEST_ASSERT_LESS_OR_EQUAL_FLOAT(p->prior_variance[i], p->covariance[i][i]);
            TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(0.0F, p->covariance[i][i]);
            for (int j = 0; j < 4; j++) {
                TEST_ASSERT_TRUE(isfinite(p->covariance[i][j]));
                TEST_ASSERT_FLOAT_WITHIN(0.01F, p->covariance[i][j], p->covariance[j][i]);
            }
        }
        for (int step = 0; step < cfg.step_count; step++) {
            const float expected = resolution_model_predict_us(&truth, &cfg, step, 9000);
            const float prior = resolution_model_predict_us(&model, &cfg, step, 9000);
            TEST_ASSERT_FLOAT_WITHIN(fabsf(prior - expected) + 0.02F * expected, expected,
                                     resolution_model_predict_us(&p->model, &cfg, step, 9000));
        }
    }
    free(p);
}

static void
test_refit_clips_a_hitch_frame(void) {
    const resolution_config_t cfg = config();
    resolution_predict_t* p = malloc(sizeof(*p));
    TEST_ASSERT_NOT_NULL(p);
    resolution_predict_init(p, &cfg, &model, 3);
    unsigned seed = 1;
    feed_frames(p, &cfg, &model, 300, -1, &seed);
    p->step = 3;
    const float before = resolution_model_predict_us(&p->model, &cfg, 3, 9000);
    resolution_predict_measured(p, &cfg, 9000, (int32_t)(10.0F * before), 0);
    TEST_ASSERT_FLOAT_WITHIN(0.02F * before, before, resolution_model_predict_us(&p->model, &cfg, 3, 9000));
    free(p);
}

static void
test_refit_preserves_a_correct_prior_under_noise(void) {
    check_refit_prices(&model, 0, 1000, 0.025F);
}

static void
test_refit_tracks_a_scene_change(void) {
    const resolution_model_t truth = changed_model();
    check_refit_prices(&truth, 1000, 300, 0.04F);
}

void
run_resolution_suite(void) {
    RUN_TEST(test_over_budget_it_steps_down_one_step_at_a_time_to_the_first_that_fits);
    RUN_TEST(test_once_the_load_lifts_it_recovers_the_finer_step);
    RUN_TEST(test_a_steady_load_settles_and_stays);
    RUN_TEST(test_two_steps_straddling_the_thresholds_are_tried_less_and_less_often);
    RUN_TEST(test_one_frame_far_over_budget_drops_two_steps_into_recovery);
    RUN_TEST(test_ordinary_steps_never_reach_a_recovery_step);
    RUN_TEST(test_from_a_recovery_step_it_climbs_back_when_the_load_allows);
    RUN_TEST(test_the_fit_recovers_the_weights_frames_were_made_with);
    RUN_TEST(test_frames_of_one_step_cannot_tell_the_weights_apart);
    RUN_TEST(test_the_predictor_draws_the_finest_step_its_model_says_fits);
    RUN_TEST(test_the_predictor_reports_the_price_of_its_chosen_step);
    RUN_TEST(test_the_predictor_steps_down_on_the_frame_the_load_arrives);
    RUN_TEST(test_the_predictor_holds_the_floor_until_the_panic_share);
    RUN_TEST(test_the_predictor_does_not_flip_at_the_edge_of_the_budget);
    RUN_TEST(test_measured_frames_correct_a_model_that_runs_fast);
    RUN_TEST(test_refit_converges_over_mixed_frames);
    RUN_TEST(test_refit_held_step_does_not_drift);
    RUN_TEST(test_refit_clips_a_hitch_frame);
    RUN_TEST(test_refit_preserves_a_correct_prior_under_noise);
    RUN_TEST(test_refit_tracks_a_scene_change);
}

SUITE_REGISTER(run_resolution_suite);
