/* framegen against the raster itself: a warp to the view a frame was drawn
 * through gives that frame back, and a warp to a moved camera puts the rig's
 * card where a draw through that camera does. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../render_examples/framegen.c"
#include "raster_rig.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "render/context/render_context.h"
#include "render/framegen/framegen.h"
#include "render/r3d_span.h"

#define W              96
#define H              64
/* The camera's sideways step, in rig units: the card (100 nearer than the
 * wall) moves several pixels further than the wall behind it, so a warp that
 * ignored depth would misplace it. */
#define STEP           30.0F
/* Pixels nothing lands on after the step: the strip uncovered at one edge,
 * the gap behind the card's edge and stretch cracks, well under a tenth. */
#define MOST_HOLES     (W * H / 10)
/* Pixels where the warp and the draw disagree on card or wall: a pixel's
 * width along the card's two side edges, rounding either way. */
#define MOST_MISPLACED (2 * H)

/* A frame budget for the pricing test; any value works, the rule is relative. */
#define BUDGET_US      30000

static framegen_target_t target;
/* The pricing test's own, freed by the cleanup if an assertion stops it. */
static render_context_t* context;
static uint16_t* destination;

static void
release_target(void) {
    framegen_target_free(&target);
    if (context != NULL) {
        render_context_release(context);
    }
    free(context);
    free(destination);
    context = NULL;
    destination = NULL;
    raster_rig_release();
}

static raster_rig_t*
open_rig(void) {
    raster_rig_t* r = raster_rig_open(raster_rig_wall_and_card, RASTER_RIG_QUADS, 0, W, H, 0);
    suite_set_test_cleanup(release_target);
    TEST_ASSERT_TRUE(framegen_target_alloc(&target, W, H));
    return r;
}

static void
test_a_warp_to_the_drawn_view_gives_the_frame_back(void) {
    raster_rig_t* r = open_rig();
    const render_view_t view = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    (void)raster_draw(&r->raster, &view);
    framegen_warp(&r->raster, &view, &view, &target);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(raster_depth(&r->raster), target.depth, W * H);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(raster_color(&r->raster), target.colour, W * H);
    TEST_ASSERT_EQUAL_INT(0, framegen_fill(&r->raster, &target));
}

/* The card is the nearer quad. Facing the camera, each quad has one depth,
 * inverse to its distance: the card's is the wall's times their distances'
 * ratio. Above the midway depth is the card. */
static uint16_t
card_floor(uint16_t wall) {
    const float to_wall = -raster_rig_camera.eye.z;
    const float to_card = (float)raster_rig_card[0][2] - raster_rig_camera.eye.z;
    return (uint16_t)((float)wall * (1.0F + (to_wall / to_card)) * 0.5F);
}

static bool
on_card(uint16_t depth, uint16_t card_depth) {
    return depth > card_depth;
}

static void
test_a_warp_to_a_moved_camera_puts_the_card_where_a_draw_does(void) {
    raster_rig_t* r = open_rig();
    fixture_camera_t moved = raster_rig_camera;
    moved.eye.x += STEP;
    const render_view_t drawn = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    const render_view_t now = render_view_fixture(&moved, &r->raster, 0);
    (void)raster_draw(&r->raster, &now);
    uint16_t* truth = malloc(sizeof(uint16_t) * W * H);
    TEST_ASSERT_NOT_NULL(truth);
    memcpy(truth, raster_depth(&r->raster), sizeof(uint16_t) * W * H);
    (void)raster_draw(&r->raster, &drawn);
    framegen_warp(&r->raster, &drawn, &now, &target);
    /* The wall is the farthest drawn depth. */
    uint16_t wall = 0;
    for (int i = 0; i < W * H; i++) {
        wall = truth[i] != R3D_DEPTH_EMPTY && (wall == 0 || truth[i] < wall) ? truth[i] : wall;
    }
    const uint16_t card_depth = card_floor(wall);
    int holes = 0;
    int misplaced = 0;
    int card = 0;
    for (int i = 0; i < W * H; i++) {
        if (target.depth[i] == R3D_DEPTH_EMPTY) {
            holes++;
            continue;
        }
        card += on_card(truth[i], card_depth);
        misplaced += on_card(truth[i], card_depth) != on_card(target.depth[i], card_depth);
    }
    free(truth);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, card, "the card is not in the picture");
    TEST_ASSERT_LESS_OR_EQUAL_INT(MOST_HOLES, holes);
    TEST_ASSERT_LESS_OR_EQUAL_INT(MOST_MISPLACED, misplaced);
    TEST_ASSERT_EQUAL_INT(holes, framegen_fill(&r->raster, &target));
}

/* A context with frame generation and dynamic resolution on, its ladder one
 * step of the rig's size at BUDGET_US per shown frame. */
static render_context_t*
open_context(void) {
    context = calloc(1, sizeof(*context));
    destination = malloc(sizeof(uint16_t) * W * H);
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_NOT_NULL(destination);
    context->scale_percent = RENDER_CONTEXT_DEFAULT_SCALE_PERCENT;
    const resolution_step_t size = {W, H};
    const resolution_config_t ladder = resolution_config(&size, 1, 1, BUDGET_US);
    framegen_example(context, &ladder);
    return context;
}

/* One frame drawn or generated and composed, then given `cost_us` as its
 * measured cost: the host's own timings are not the board's. Whether it was
 * generated. */
static bool
show(render_context_t* c, raster_rig_t* r, const render_view_t* view, int32_t cost_us) {
    TEST_ASSERT_TRUE(render_context_draw(c, r->instance, RASTER_RIG_QUADS, view, 0));
    (void)render_context_compose(c, destination, NULL, NULL);
    const bool generated = framegen_example_generated(c);
    /* After compose: the context decides and prices from the costs before this frame's. */
    *(generated ? &c->generated_us : &c->drawn_us) = cost_us;
    return generated;
}

/* A frame's measured cost and whether it was generated. */
typedef struct {
    int32_t cost_us;
    bool generated;
} shown_t;

/* `steps` shown in turn, each generated or drawn as it says. */
static void
play(render_context_t* c, raster_rig_t* r, const render_view_t* view, const shown_t* steps, size_t count) {
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL(steps[i].generated, show(c, r, view, steps[i].cost_us));
    }
}

/* A drawn frame's budget with frame generation on: the shown frames' budgets
 * less what the generated ones cost, and never less than one frame's. */
static void
test_dynamic_resolution_prices_a_drawn_frame_with_the_one_generated_from_it(void) {
    raster_rig_t* r = open_rig();
    render_context_t* c = open_context();
    const render_view_t view = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    const int32_t shown_us = BUDGET_US;
    const int32_t drawn_us = FRAMEGEN_SHOWN_PER_DRAWN * shown_us; /* a drawn frame that spent its pair's budget */
    const int32_t generated[] = {shown_us / 4, shown_us + shown_us / 2};
    TEST_ASSERT_FALSE(show(c, r, &view, drawn_us));
    for (size_t i = 0; i < sizeof generated / sizeof generated[0]; i++) {
        TEST_ASSERT_TRUE(show(c, r, &view, generated[i]));
        render_context_set_frame_generation(c, true); /* as an app re-applying its settings each frame */
        TEST_ASSERT_FALSE(show(c, r, &view, drawn_us));
        const int32_t shared_us =
            (FRAMEGEN_SHOWN_PER_DRAWN * shown_us) - ((FRAMEGEN_SHOWN_PER_DRAWN - 1) * generated[i]);
        TEST_ASSERT_EQUAL_INT32(shared_us > shown_us ? shared_us : shown_us, c->ladder.budget_us);
    }
    release_target();
}

/* Frames are generated once a generated one costs under
 * (100 - FRAMEGEN_GAIN_PERCENT)% of a drawn one, and then while it costs less:
 * one that saves less stops it, the drawn frames are priced alone, and after
 * FRAMEGEN_RETRY_DRAWN drawn frames, or a forgotten view, one is generated to
 * measure it again. */
static void
test_frames_are_generated_only_while_a_generated_one_costs_less(void) {
    raster_rig_t* r = open_rig();
    render_context_t* c = open_context();
    const render_view_t view = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    const int32_t drawn_us = BUDGET_US;
    const int32_t saves_too_little = drawn_us * (100 - FRAMEGEN_GAIN_PERCENT) / 100;
    const int32_t saves_enough = saves_too_little - 1;
    const shown_t before[] = {{drawn_us, false}, {saves_too_little, true}}, /* not measured yet: tried */
        after[] = {
            {saves_enough, true},                           /* retried, and it pays */
            {drawn_us, false},    {saves_too_little, true}, /* paying: held */
            {drawn_us, false},    {drawn_us, true},         /* saves nothing */
            {drawn_us, false},    {drawn_us, false},
        };
    play(c, r, &view, before, sizeof before / sizeof before[0]);
    for (int i = 0; i < FRAMEGEN_RETRY_DRAWN; i++) {
        TEST_ASSERT_FALSE(show(c, r, &view, drawn_us));
        TEST_ASSERT_EQUAL_INT32(BUDGET_US, c->ladder.budget_us);
    }
    play(c, r, &view, after, sizeof after / sizeof after[0]);
    render_context_forget(c); /* measured again, as at the start */
    play(c, r, &view, before, sizeof before / sizeof before[0]);
    release_target();
}

void
run_framegen_suite(void) {
    RUN_TEST(test_a_warp_to_the_drawn_view_gives_the_frame_back);
    RUN_TEST(test_a_warp_to_a_moved_camera_puts_the_card_where_a_draw_does);
    RUN_TEST(test_dynamic_resolution_prices_a_drawn_frame_with_the_one_generated_from_it);
    RUN_TEST(test_frames_are_generated_only_while_a_generated_one_costs_less);
}

SUITE_REGISTER(run_framegen_suite);
