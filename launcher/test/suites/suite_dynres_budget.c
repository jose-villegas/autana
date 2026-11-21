/* suite_dynres_budget: dynamic resolution budgets the whole frame. What a
 * shown frame paid outside its draw and upscale, waits for the panel taken
 * out, is learned, and the scaled part gets the target less that. */
#include <stdint.h>
#include <stdlib.h>

#include "raster_rig.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "render/context/render_context.h"
#include "render/context/render_context_internal.h"
#include "render/raster.h"
#include "render/resolution/resolution.h"

static const resolution_step_t steps[] = {{96, 64}, {48, 32}};
#define STEP_COUNT        ((int)(sizeof steps / sizeof steps[0]))
#define TARGET_US         33000
#define DRAW_US           10000
#define UPSCALE_US        1000
#define UNSCALED_US       5000
/* Frames enough for the learned cost to settle within the average's rounding. */
#define SETTLE            64
/* What the integer average may stop short of the sample by: under one step
 * of its weight. */
#define SETTLED_US        RESOLUTION_UNSCALED_WEIGHT_DIVISOR
/* Where the clocks stand when a context opens: the wait counter has run
 * since boot, so only its change over a frame may count. */
#define OPENED_AT_US      1000000
#define WAITED_AT_OPEN_US 600000

static render_context_t* context;
static int64_t now_us;
static int64_t waited_us;

/* The test's context, and the rig if it opened one. */
static void
release_context(void) {
    render_context_release(context);
    free(context);
    context = NULL;
    raster_rig_release();
}

/* A context under the stepped controller at the frame target, its clock
 * started and its frames drawn by the policy at DRAW_US and UPSCALE_US. */
static render_context_t*
open_context(void) {
    context = malloc(sizeof(*context));
    TEST_ASSERT_NOT_NULL(context);
    *context = RENDER_CONTEXT_DEFAULT;
    suite_set_test_cleanup(release_context);
    const resolution_config_t config = resolution_config(steps, STEP_COUNT, STEP_COUNT, TARGET_US);
    render_context_set_dynamic_resolution(context, &config, NULL, 0);
    context->frame.draw_us = DRAW_US;
    context->frame.upscale_us = UPSCALE_US;
    now_us = OPENED_AT_US;
    waited_us = WAITED_AT_OPEN_US;
    render_context_shown(context, now_us, waited_us);
    return context;
}

/* The next draw begins `period_us` after the last, `waited` of it spent
 * waiting for the panel. */
static void
next_draw(render_context_t* c, int32_t period_us, int32_t waited) {
    now_us += period_us;
    waited_us += waited;
    render_context_shown(c, now_us, waited_us);
}

/* `frames` frames the policy drew, each `period_us` long with `waited` of it
 * spent waiting for the panel. */
static void
show(render_context_t* c, int frames, int32_t period_us, int32_t waited) {
    for (int i = 0; i < frames; i++) {
        c->priced = true; /* as a policy's draw leaves it */
        next_draw(c, period_us, waited);
    }
}

/* A frame that finishes its work early and waits for the panel to its
 * period is no costlier than one that does not wait: the scaled budget is
 * the same. */
static void
test_an_early_finished_frame_does_not_grow_the_unscaled_cost(void) {
    const int32_t work_us = DRAW_US + UPSCALE_US + UNSCALED_US;
    render_context_t* c = open_context();
    show(c, SETTLE, work_us, 0);
    const int32_t busy_budget_us = c->ladder.budget_us;
    show(c, SETTLE, TARGET_US, TARGET_US - work_us); /* early, then waiting */
    TEST_ASSERT_EQUAL_INT32(busy_budget_us, c->ladder.budget_us);
    TEST_ASSERT_INT32_WITHIN(SETTLED_US, TARGET_US - UNSCALED_US, c->ladder.budget_us);
}

/* The scaled part gets the target less what frames paid outside it; the
 * first frame's cost is taken whole, as an average starts from its first sample. */
static void
test_the_scaled_budget_is_the_target_less_the_unscaled_cost(void) {
    render_context_t* c = open_context();
    TEST_ASSERT_EQUAL_INT32(TARGET_US, c->ladder.budget_us); /* nothing learned yet */
    show(c, 1, DRAW_US + UPSCALE_US + UNSCALED_US, 0);
    TEST_ASSERT_EQUAL_INT32(TARGET_US - UNSCALED_US, c->ladder.budget_us);
    TEST_ASSERT_EQUAL_INT32(UNSCALED_US, render_context_frame(c).unscaled_us);
    show(c, SETTLE, DRAW_US + UPSCALE_US + 2 * UNSCALED_US, 0);
    TEST_ASSERT_INT32_WITHIN(SETTLED_US, TARGET_US - 2 * UNSCALED_US, c->ladder.budget_us);
}

/* One slow frame moves the learned cost by its weight, not to it. */
static void
test_one_slow_frame_moves_the_budget_by_the_weight(void) {
    render_context_t* c = open_context();
    show(c, 1, DRAW_US + UPSCALE_US + UNSCALED_US, 0);
    show(c, 1, DRAW_US + UPSCALE_US + UNSCALED_US + TARGET_US, 0);
    const int32_t sample_us = TARGET_US; /* a sample is capped at the target */
    const int32_t moved_us = (sample_us - UNSCALED_US) / RESOLUTION_UNSCALED_WEIGHT_DIVISOR;
    TEST_ASSERT_EQUAL_INT32(TARGET_US - UNSCALED_US - moved_us, c->ladder.budget_us);
}

/* However much a frame pays outside the scaled part, the scaled part keeps
 * its floor share of the target. */
static void
test_the_scaled_budget_keeps_its_floor(void) {
    render_context_t* c = open_context();
    show(c, SETTLE, DRAW_US + UPSCALE_US + 2 * TARGET_US, 0);
    TEST_ASSERT_EQUAL_INT32(TARGET_US * RESOLUTION_SCALED_MIN_PERCENT / 100, c->ladder.budget_us);
}

/* A new opt-in learns afresh: the target, nothing learned. */
static void
test_a_new_opt_in_forgets_the_learned_cost(void) {
    render_context_t* c = open_context();
    show(c, SETTLE, DRAW_US + UPSCALE_US + UNSCALED_US, 0);
    const resolution_config_t config = resolution_config(steps, STEP_COUNT, STEP_COUNT, TARGET_US);
    render_context_set_dynamic_resolution(c, &config, NULL, 0);
    TEST_ASSERT_EQUAL_INT32(TARGET_US, c->ladder.budget_us);
    TEST_ASSERT_EQUAL_INT32(0, render_context_frame(c).unscaled_us);
}

/* A frame whose draw, upscale and waits came to more than its period, as a
 * mis-timed one, teaches no cost: the scaled part never gets more than the target. */
static void
test_an_overspent_frame_teaches_no_cost(void) {
    render_context_t* c = open_context();
    show(c, 1, DRAW_US + UPSCALE_US, TARGET_US);
    TEST_ASSERT_EQUAL_INT32(0, render_context_frame(c).unscaled_us);
    TEST_ASSERT_EQUAL_INT32(TARGET_US, c->ladder.budget_us);
}

/* Only the waits since the frame's own draw are taken out, not all since boot. */
static void
test_only_the_frames_own_waits_are_taken_out(void) {
    render_context_t* c = open_context();
    show(c, 1, DRAW_US + UPSCALE_US + UNSCALED_US, 0);
    TEST_ASSERT_EQUAL_INT32(UNSCALED_US, render_context_frame(c).unscaled_us);
}

/* A frame no policy priced (held, generated or fixed) teaches nothing, nor
 * does the first draw after an opt-in, which has no frame before it. */
static void
test_an_unpriced_frame_teaches_nothing(void) {
    render_context_t* c = open_context();
    next_draw(c, DRAW_US + UPSCALE_US + UNSCALED_US, 0);
    TEST_ASSERT_FALSE(c->budget.learned);
    const resolution_config_t config = resolution_config(steps, STEP_COUNT, STEP_COUNT, TARGET_US);
    c->priced = true;
    render_context_set_dynamic_resolution(c, &config, NULL, 0);
    next_draw(c, DRAW_US + UPSCALE_US + UNSCALED_US, 0);
    TEST_ASSERT_FALSE(c->budget.learned);
    TEST_ASSERT_EQUAL_INT32(TARGET_US, c->ladder.budget_us);
}

/* A pause between two draws is not a frame: its time teaches nothing. */
static void
test_a_pause_teaches_nothing(void) {
    render_context_t* c = open_context();
    show(c, 1, DRAW_US + UPSCALE_US + UNSCALED_US, 0);
    c->priced = true;
    render_context_skip(c);
    next_draw(c, TARGET_US * SETTLE, 0);
    TEST_ASSERT_EQUAL_INT32(TARGET_US - UNSCALED_US, c->ladder.budget_us);
}

/* A gap longer than a period can hold is not a frame either. */
static void
test_an_overlong_period_teaches_nothing(void) {
    render_context_t* c = open_context();
    c->priced = true;
    render_context_shown(c, now_us + (int64_t)INT32_MAX + 1, waited_us);
    TEST_ASSERT_FALSE(c->budget.learned);
}

/* Draws `frames` frames of the rig through `c`, each one's changes unknown. */
static void
draw_frames(render_context_t* c, raster_rig_t* r, int frames) {
    const render_view_t view =
        render_view_fixture_at(&raster_rig_camera, (viewport_t){steps[0].width, steps[0].height, 0});
    for (int i = 0; i < frames; i++) {
        render_context_set_changed(c, NULL, -1);
        TEST_ASSERT_TRUE(render_context_draw(c, r->instance, RASTER_RIG_QUADS, &view, 0));
        (void)render_context_compose(c, NULL, NULL, NULL);
    }
}

/* A frame a policy drew teaches the budget when the next draw begins: its
 * period is known only then. */
static void
test_a_policy_frame_teaches_the_next_draw(void) {
    raster_rig_t* r =
        raster_rig_open(raster_rig_wall_and_card, RASTER_RIG_QUADS, 0, steps[0].width, steps[0].height, 0);
    render_context_t* c = open_context();
    draw_frames(c, r, 1);
    TEST_ASSERT_FALSE(c->budget.learned);
    draw_frames(c, r, 1);
    TEST_ASSERT_TRUE(c->budget.learned);
}

/* A frame at the fixed scale teaches nothing: no policy priced it. */
static void
test_a_fixed_frame_teaches_nothing(void) {
    raster_rig_t* r =
        raster_rig_open(raster_rig_wall_and_card, RASTER_RIG_QUADS, 0, steps[0].width, steps[0].height, 0);
    render_context_t* c = open_context();
    render_context_set_dynamic_resolution(c, NULL, NULL, 0);
    draw_frames(c, r, SETTLE);
    TEST_ASSERT_FALSE(c->budget.learned);
    TEST_ASSERT_EQUAL_INT32(TARGET_US, c->ladder.budget_us);
}

void
run_dynres_budget_suite(void) {
    RUN_TEST(test_an_early_finished_frame_does_not_grow_the_unscaled_cost);
    RUN_TEST(test_the_scaled_budget_is_the_target_less_the_unscaled_cost);
    RUN_TEST(test_one_slow_frame_moves_the_budget_by_the_weight);
    RUN_TEST(test_the_scaled_budget_keeps_its_floor);
    RUN_TEST(test_a_new_opt_in_forgets_the_learned_cost);
    RUN_TEST(test_an_overspent_frame_teaches_no_cost);
    RUN_TEST(test_only_the_frames_own_waits_are_taken_out);
    RUN_TEST(test_an_unpriced_frame_teaches_nothing);
    RUN_TEST(test_a_pause_teaches_nothing);
    RUN_TEST(test_an_overlong_period_teaches_nothing);
    RUN_TEST(test_a_policy_frame_teaches_the_next_draw);
    RUN_TEST(test_a_fixed_frame_teaches_nothing);
}

SUITE_REGISTER(run_dynres_budget_suite);
