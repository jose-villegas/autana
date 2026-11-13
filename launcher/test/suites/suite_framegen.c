/* framegen against the raster itself: a warp to the view a frame was drawn
 * through gives that frame back (its depth to the warp reciprocal's
 * precision), a warp to a moved camera puts the rig's card where a draw
 * through that camera does, and the render context composes that generated
 * picture. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../render_examples/framegen.c"
#include "raster_rig.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "math/linear/mat3f.h"
#include "math/scalar/mathf.h"
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
    framegen_warp(&r->raster, NULL, &view, &view, &target);
    /* The board's reciprocal keeps MATHF_RECIP_APPROX_BITS and the word truncates,
     * so a depth may come back that fraction of itself plus one word off. */
    const uint16_t* drawn = raster_depth(&r->raster);
    for (int i = 0; i < W * H; i++) {
        const uint32_t off = (uint32_t)((float)drawn[i] * MATHF_RECIP_APPROX_RELATIVE_LIMIT) + 1U;
        TEST_ASSERT_UINT32_WITHIN(off, drawn[i], target.depth[i]);
    }
    TEST_ASSERT_EQUAL_UINT16_ARRAY(raster_color(&r->raster), target.colour, W * H);
    TEST_ASSERT_EQUAL_INT(0, framegen_fill(&r->raster, NULL, &target));
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

/* The view STEP to the side of the rig's camera. */
static render_view_t
stepped_view(raster_rig_t* r) {
    fixture_camera_t moved = raster_rig_camera;
    moved.eye.x += STEP;
    return render_view_fixture(&moved, &r->raster, 0);
}

static void
test_a_warp_to_a_moved_camera_puts_the_card_where_a_draw_does(void) {
    raster_rig_t* r = open_rig();
    const render_view_t drawn = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    const render_view_t now = stepped_view(r);
    (void)raster_draw(&r->raster, &now);
    uint16_t* truth = malloc(sizeof(uint16_t) * W * H);
    TEST_ASSERT_NOT_NULL(truth);
    memcpy(truth, raster_depth(&r->raster), sizeof(uint16_t) * W * H);
    (void)raster_draw(&r->raster, &drawn);
    framegen_warp(&r->raster, NULL, &drawn, &now, &target);
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
    TEST_ASSERT_EQUAL_INT(holes, framegen_fill(&r->raster, NULL, &target));
}

/* The fill writes the generated frame into the raster's own picture: the
 * warped pixel where one landed, the drawn frame's where nothing did. */
static void
test_the_fill_writes_the_generated_frame_into_the_raster(void) {
    raster_rig_t* r = open_rig();
    const render_view_t drawn = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    const render_view_t now = stepped_view(r);
    (void)raster_draw(&r->raster, &drawn);
    framegen_warp(&r->raster, NULL, &drawn, &now, &target);
    const size_t bytes = sizeof(uint16_t) * W * H;
    uint16_t* colour = malloc(bytes);
    uint16_t* depth = malloc(bytes);
    TEST_ASSERT_TRUE(colour != NULL && depth != NULL);
    memcpy(colour, raster_color(&r->raster), bytes);
    memcpy(depth, raster_depth(&r->raster), bytes);
    const int holes = framegen_fill(&r->raster, NULL, &target);
    const uint16_t* shown_colour = raster_color(&r->raster);
    const uint16_t* shown_depth = raster_depth(&r->raster);
    int counted = 0;
    int wrong = 0;
    for (int i = 0; i < W * H; i++) {
        const bool hole = target.depth[i] == R3D_DEPTH_EMPTY;
        counted += hole;
        const uint16_t want_colour = hole ? colour[i] : target.colour[i];
        const uint16_t landed = target.depth[i] == FRAMEGEN_DEPTH_SKY ? R3D_DEPTH_EMPTY : target.depth[i];
        const uint16_t want_depth = hole ? depth[i] : landed;
        wrong += shown_colour[i] != want_colour || shown_depth[i] != want_depth;
    }
    free(colour);
    free(depth);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, holes, "the moved camera opened no holes");
    TEST_ASSERT_EQUAL_INT(counted, holes);
    TEST_ASSERT_EQUAL_INT(0, wrong);
}

/* A context with frame generation and dynamic resolution on, its ladder one
 * step of the rig's size at BUDGET_US per shown frame. */
static render_context_t*
open_context(void) {
    context = calloc(1, sizeof(*context));
    destination = malloc(sizeof(uint16_t) * W * H);
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_NOT_NULL(destination);
    *context = RENDER_CONTEXT_DEFAULT;
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

/* How far the card moves on its own between a drawn frame and the one
 * generated from it, in the rig's units. */
#define CARD_MOVE 24.0F

/* A rig whose card (the last quad) is placed, at rest; a context generating. */
static raster_rig_t*
open_moving_rig(void) {
    raster_rig_t* r = raster_rig_open(raster_rig_wall_and_card, RASTER_RIG_QUADS, 1U << 1U, W, H, 0);
    suite_set_test_cleanup(release_target);
    r->model[1] = mat4f_identity();
    (void)open_context();
    return r;
}

/* The pixels where the context's last picture puts the card and a plain
 * draw of the rig as it is now does not, or the other way round: the frame
 * a generated one stands in for. The quads share a colour; depth tells the
 * card from the wall. */
static int
wrong_against_a_draw(raster_rig_t* r, const render_view_t* view) {
    (void)raster_draw(&r->raster, view);
    const uint16_t* truth = raster_depth(&r->raster);
    const uint16_t* shown = raster_depth(&context->raster);
    uint16_t wall = 0;
    for (int i = 0; i < W * H; i++) {
        wall = truth[i] != R3D_DEPTH_EMPTY && (wall == 0 || truth[i] < wall) ? truth[i] : wall;
    }
    const uint16_t card_depth = card_floor(wall);
    int wrong = 0;
    for (int i = 0; i < W * H; i++) {
        wrong += on_card(truth[i], card_depth) != on_card(shown[i], card_depth);
    }
    return wrong;
}

/* A drawn frame with the card moving, then the card moved on and the frame
 * after it generated under `mode`, the camera still. Returns the pixels it
 * gets wrong. */
static int
card_moved_under(render_framegen_moving_t mode) {
    raster_rig_t* r = open_moving_rig();
    render_context_set_framegen_moving(context, mode);
    const render_view_t view = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    render_context_set_moving(context, 1);
    TEST_ASSERT_FALSE(show(context, r, &view, BUDGET_US));
    r->model[1].m[0][MAT3F_AXES] = CARD_MOVE;
    render_context_set_moving(context, 1);
    TEST_ASSERT_TRUE(show(context, r, &view, BUDGET_US));
    const int wrong = wrong_against_a_draw(r, &view);
    release_target();
    return wrong;
}

/* Drawn again over a warp of the picture drawn without it, a moving card is
 * where a draw puts it, and the wall shows where it was. */
static void
test_a_moving_instance_is_drawn_again_over_the_still_part(void) {
    TEST_ASSERT_EQUAL_INT(0, card_moved_under(RENDER_FRAMEGEN_MOVING_STILL));
}

/* Without the still part the warp carries the card's old copy: drawn again
 * over it, the card is right but its old place still shows it; warped alone,
 * it is wrong in both. */
static void
test_without_the_still_part_the_old_copy_shows(void) {
    const int redrawn = card_moved_under(RENDER_FRAMEGEN_MOVING_REDRAW);
    const int warped = card_moved_under(RENDER_FRAMEGEN_MOVING_WARP);
    TEST_ASSERT_GREATER_THAN_INT(0, redrawn);
    TEST_ASSERT_GREATER_THAN_INT(redrawn, warped);
}

/* A frame whose moving count is not the drawn frame's is drawn: the still
 * part it would warp was split at another count. */
static void
test_a_frame_with_another_moving_count_is_drawn(void) {
    raster_rig_t* r = open_moving_rig();
    const render_view_t view = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    render_context_set_moving(context, 0);
    TEST_ASSERT_FALSE(show(context, r, &view, BUDGET_US));
    render_context_set_moving(context, 1);
    TEST_ASSERT_FALSE(show(context, r, &view, BUDGET_US));
    render_context_set_moving(context, 1);
    TEST_ASSERT_TRUE(show(context, r, &view, BUDGET_US));
    release_target();
}

/* Frame generation turned off after a generated frame: the destination
 * holds the warped picture, so the next drawn frame, its view and its
 * objects held, recomposes every row. */
static void
test_a_frame_drawn_after_generation_is_turned_off_recomposes_every_row(void) {
    raster_rig_t* r = open_rig();
    render_context_t* c = open_context();
    const render_view_t drawn = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    const render_view_t now = stepped_view(r);
    TEST_ASSERT_FALSE(show(c, r, &drawn, BUDGET_US));
    TEST_ASSERT_TRUE(show(c, r, &now, BUDGET_US));
    render_context_set_frame_generation(c, false);
    render_context_set_dynamic_resolution(c, NULL, NULL, 0);
    render_context_set_scale(c, 100);
    render_settings_t settings = RENDER_SETTINGS_DEFAULT;
    settings.partial_redraw = true;
    render_context_set_settings(c, &settings);
    render_context_set_changed(c, NULL, 0);
    TEST_ASSERT_TRUE(render_context_draw(c, r->instance, RASTER_RIG_QUADS, &drawn, 0));
    (void)render_context_compose(c, destination, NULL, NULL);
    const render_context_frame_t f = render_context_frame(c);
    TEST_ASSERT_FALSE(f.unchanged);
    TEST_ASSERT_EQUAL_INT(H, f.rows_end - f.rows_first);
    release_target();
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
    RUN_TEST(test_the_fill_writes_the_generated_frame_into_the_raster);
    RUN_TEST(test_a_moving_instance_is_drawn_again_over_the_still_part);
    RUN_TEST(test_without_the_still_part_the_old_copy_shows);
    RUN_TEST(test_a_frame_with_another_moving_count_is_drawn);
    RUN_TEST(test_a_frame_drawn_after_generation_is_turned_off_recomposes_every_row);
    RUN_TEST(test_dynamic_resolution_prices_a_drawn_frame_with_the_one_generated_from_it);
    RUN_TEST(test_frames_are_generated_only_while_a_generated_one_costs_less);
}

SUITE_REGISTER(run_framegen_suite);
