/* framegen against the raster itself: a warp to the view a frame was drawn
 * through gives that frame back, and a warp to a moved camera puts the rig's
 * card where a draw through that camera does. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

/* A drawn frame's budget with frame generation on: the shown frames' budgets
 * less what the generated ones cost, and never less than one frame's. */
static void
test_dynamic_resolution_prices_a_drawn_frame_with_the_one_generated_from_it(void) {
    raster_rig_t* r = open_rig();
    context = calloc(1, sizeof(*context));
    destination = malloc(sizeof(uint16_t) * W * H);
    render_context_t* c = context;
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NOT_NULL(destination);
    c->scale_percent = RENDER_CONTEXT_DEFAULT_SCALE_PERCENT;
    const render_view_t view = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    const resolution_step_t size = {W, H};
    const int32_t shown_us = BUDGET_US;
    const resolution_config_t ladder = resolution_config(&size, 1, 1, shown_us);
    render_context_set_dynamic_resolution(c, &ladder, NULL, 0);
    render_context_set_frame_generation(c, true);
    const int32_t generated[] = {shown_us / 4, shown_us + shown_us / 2};
    TEST_ASSERT_TRUE(render_context_draw(c, r->instance, RASTER_RIG_QUADS, &view, 0));
    (void)render_context_compose(c, destination, NULL);
    TEST_ASSERT_FALSE(render_context_frame(c).generated);
    for (size_t i = 0; i < sizeof generated / sizeof generated[0]; i++) {
        TEST_ASSERT_TRUE(render_context_draw(c, r->instance, RASTER_RIG_QUADS, &view, 0));
        (void)render_context_compose(c, destination, NULL);
        TEST_ASSERT_TRUE(render_context_frame(c).generated);
        c->generated_us = generated[i];
        TEST_ASSERT_TRUE(render_context_draw(c, r->instance, RASTER_RIG_QUADS, &view, 0));
        (void)render_context_compose(c, destination, NULL);
        TEST_ASSERT_FALSE(render_context_frame(c).generated);
        const int32_t shared_us =
            (FRAMEGEN_SHOWN_PER_DRAWN * shown_us) - ((FRAMEGEN_SHOWN_PER_DRAWN - 1) * generated[i]);
        TEST_ASSERT_EQUAL_INT32(shared_us > shown_us ? shared_us : shown_us, c->ladder.budget_us);
    }
    release_target();
}

void
run_framegen_suite(void) {
    RUN_TEST(test_a_warp_to_the_drawn_view_gives_the_frame_back);
    RUN_TEST(test_a_warp_to_a_moved_camera_puts_the_card_where_a_draw_does);
    RUN_TEST(test_dynamic_resolution_prices_a_drawn_frame_with_the_one_generated_from_it);
}

SUITE_REGISTER(run_framegen_suite);
