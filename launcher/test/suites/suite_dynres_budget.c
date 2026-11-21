/* suite_dynres_budget: dynamic resolution budgets the whole frame. What a
 * shown frame paid outside its draw and upscale, waits for the panel taken
 * out, is learned, and the scaled part gets the target less that. */
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "render/context/render_context.h"
#include "render/resolution/resolution.h"

static const resolution_step_t steps[] = {{96, 64}, {48, 32}};
#define STEP_COUNT  ((int)(sizeof steps / sizeof steps[0]))
#define TARGET_US   33000
#define DRAW_US     10000
#define UPSCALE_US  1000
#define UNSCALED_US 5000
/* Frames enough for the learned cost to settle within the average's rounding. */
#define SETTLE      64
/* What the integer average may stop short of the sample by: under one step
 * of its weight. */
#define SETTLED_US  (1 << RESOLUTION_UNSCALED_WEIGHT_SHIFT)

static render_context_t context;

/* A context under the stepped controller at the frame target, its last frame
 * a drawn one that cost DRAW_US and UPSCALE_US. */
static render_context_t*
open_context(void) {
    context = RENDER_CONTEXT_DEFAULT;
    const resolution_config_t config = resolution_config(steps, STEP_COUNT, STEP_COUNT, TARGET_US);
    render_context_set_dynamic_resolution(&context, &config, NULL, 0);
    context.frame.draw_us = DRAW_US;
    context.frame.upscale_us = UPSCALE_US;
    return &context;
}

/* `frames` shown frames, each `period_us` long with `waited_us` of it spent
 * waiting for the panel. */
static void
show(render_context_t* c, int frames, int32_t period_us, int32_t waited_us) {
    for (int i = 0; i < frames; i++) {
        render_context_learn_unscaled(c, period_us, waited_us);
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
    const int32_t moved_us = (sample_us - UNSCALED_US) >> RESOLUTION_UNSCALED_WEIGHT_SHIFT;
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

void
run_dynres_budget_suite(void) {
    RUN_TEST(test_an_early_finished_frame_does_not_grow_the_unscaled_cost);
    RUN_TEST(test_the_scaled_budget_is_the_target_less_the_unscaled_cost);
    RUN_TEST(test_one_slow_frame_moves_the_budget_by_the_weight);
    RUN_TEST(test_the_scaled_budget_keeps_its_floor);
    RUN_TEST(test_a_new_opt_in_forgets_the_learned_cost);
}

SUITE_REGISTER(run_dynres_budget_suite);
