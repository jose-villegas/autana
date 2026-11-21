/*
 * Portable suite: render/partial through the render context. A context with
 * partial redraw on and one with it off draw the same rig frame after frame:
 * the card moves, holds, or the camera moves, and the two pictures and
 * composed destinations stay identical while the partial one recomposes
 * fewer rows. Under dynamic resolution a still camera redraws only the
 * changed rows at the top size, unpriced, until such a frame costs more than
 * the budget. Every mesh is built inside the test.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "raster_rig.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "image/pixel/rgb565_layout.h"
#include "math/geometry/box3f.h"
#include "math/linear/transformf.h"
#include "render/context/render_context.h"
#include "render/r3d_lit_mesh.h"
#include "render/raster.h"
#include "render/resolution/resolution.h"

#define W         96
#define H         64
#define CARD      1 /* the rig quad the tests move */
#define FAR_EYE_Z -740.0F

/* The card's two faces, red against the white wall, so every move shows. */
static const uint16_t card_colors[2] = {RGB565_RED_MASK, RGB565_RED_MASK};

/* The card's path, rig units from where it starts: a slide, a hold, a step
 * toward the camera and back (the depth scale may refit), and a drop. */
static const vec3f_t card_path[] = {
    {0.0F, 0.0F, 0.0F},  {6.0F, 0.0F, 0.0F},    {12.0F, 3.0F, 0.0F}, {12.0F, 3.0F, 0.0F},
    {12.0F, 3.0F, 0.0F}, {12.0F, 3.0F, -60.0F}, {12.0F, 3.0F, 0.0F}, {12.0F, -25.0F, 0.0F},
};
#define CARD_STEPS ((int)(sizeof card_path / sizeof card_path[0]))

/* Both contexts, their settings and destinations, freed by the cleanup if an
 * assertion stops a test. */
typedef struct {
    render_context_t context[2]; /* [0] partial redraw as the test asks, [1] off */
    render_settings_t settings[2];
    uint16_t destination[2][W * H];
    uint16_t last[W * H];                         /* the full picture one frame back */
    box3f_t many[RENDER_PARTIAL_CHANGES_MAX + 1]; /* more changed boxes than a frame holds */
} pair_t;

static pair_t* pair;

static void
release_pair(void) {
    if (pair != NULL) {
        render_context_release(&pair->context[0]);
        render_context_release(&pair->context[1]);
        free(pair);
        pair = NULL;
    }
    raster_rig_release();
}

static raster_rig_t*
open_pair(bool partial) {
    raster_rig_t* r = raster_rig_open(raster_rig_wall_and_card, RASTER_RIG_QUADS, 1U << CARD, W, H, 0);
    r->quad[CARD].mesh.colors = NULL;
    r->quad[CARD].mesh.face_colors = card_colors;
    suite_set_test_cleanup(release_pair);
    pair = calloc(1, sizeof(*pair));
    TEST_ASSERT_NOT_NULL(pair);
    for (int k = 0; k < 2; k++) {
        pair->settings[k] = RENDER_SETTINGS_DEFAULT;
        pair->settings[k].partial_redraw = k == 0 && partial;
        pair->context[k] = RENDER_CONTEXT_DEFAULT;
        render_context_set_scale(&pair->context[k], 100);
        render_context_set_settings(&pair->context[k], &pair->settings[k]);
    }
    return r;
}

/* The card placed at `offset` from where it starts. */
static void
place_card(raster_rig_t* r, vec3f_t offset) {
    transformf_t pose = transformf_identity();
    transformf_set_position(&pose, offset);
    r->model[CARD] = transformf_compute_matrix(&pose);
}

static box3f_t
card_box(const raster_rig_t* r) {
    return r3d_lit_mesh_world_bounds(&r->quad[CARD].mesh, &r->model[CARD]);
}

/* One frame through both contexts, the partial one told `count` changed
 * boxes (negative changes everything); its frame. Asserts the two pictures
 * and destinations match. */
static render_context_frame_t
frame(raster_rig_t* r, const render_view_t* view, const box3f_t* changed, int count) {
    render_context_set_changed(&pair->context[0], changed, count);
    for (int k = 0; k < 2; k++) {
        TEST_ASSERT_TRUE(render_context_draw(&pair->context[k], r->instance, RASTER_RIG_QUADS, view, 0));
        (void)render_context_compose(&pair->context[k], pair->destination[k], NULL, NULL);
    }
    TEST_ASSERT_EQUAL_MEMORY(raster_color(&pair->context[1].raster), raster_color(&pair->context[0].raster),
                             sizeof(uint16_t) * W * H);
    TEST_ASSERT_EQUAL_MEMORY(pair->destination[1], pair->destination[0], sizeof pair->destination[0]);
    return render_context_frame(&pair->context[0]);
}

/* The destination rows a frame recomposed. */
static int
rows(render_context_frame_t f) {
    return f.unchanged ? 0 : f.rows_end - f.rows_first;
}

/* Along the card's path the partial picture is the full one; a frame where
 * the card slid recomposes fewer rows than all, one where it held none. */
static void
test_a_moving_card_redraws_only_its_rows_and_matches_a_full_redraw(void) {
    raster_rig_t* r = open_pair(true);
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    place_card(r, card_path[0]);
    TEST_ASSERT_EQUAL_INT(H, rows(frame(r, &view, NULL, -1)));
    for (int i = 1; i < CARD_STEPS; i++) {
        const box3f_t was = card_box(r);
        place_card(r, card_path[i]);
        const bool moved = memcmp(&card_path[i], &card_path[i - 1], sizeof card_path[i]) != 0;
        const box3f_t changed = box3f_union(was, card_box(r));
        memcpy(pair->last, raster_color(&pair->context[1].raster), sizeof pair->last);
        const render_context_frame_t f = frame(r, &view, &changed, moved ? 1 : 0);
        TEST_ASSERT_EQUAL(!moved, f.unchanged);
        /* A move the full picture shows, so a partial one that missed rows differs. */
        TEST_ASSERT_EQUAL(moved, memcmp(pair->last, raster_color(&pair->context[1].raster), sizeof pair->last) != 0);
        if (moved && card_path[i].z == card_path[i - 1].z) {
            TEST_ASSERT_LESS_THAN_INT(H, rows(f)); /* a slide keeps the depth scale */
        }
    }
}

/* A camera that moved, unknown changes, or more boxes than fit: every row,
 * the same picture. */
static void
test_a_new_view_or_unknown_changes_redraw_every_row(void) {
    raster_rig_t* r = open_pair(true);
    render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    place_card(r, card_path[0]);
    (void)frame(r, &view, NULL, -1);
    const box3f_t held = card_box(r);
    TEST_ASSERT_TRUE(frame(r, &view, &held, 0).unchanged);
    fixture_camera_t moved = raster_rig_camera;
    moved.eye.x += 5.0F;
    view = render_view_fixture_at(&moved, (viewport_t){W, H, 0});
    TEST_ASSERT_EQUAL_INT(H, rows(frame(r, &view, &held, 0)));
    TEST_ASSERT_EQUAL_INT(H, rows(frame(r, &view, NULL, -1)));
    for (int i = 0; i <= RENDER_PARTIAL_CHANGES_MAX; i++) {
        pair->many[i] = held;
    }
    TEST_ASSERT_EQUAL_INT(H, rows(frame(r, &view, pair->many, RENDER_PARTIAL_CHANGES_MAX + 1)));
}

/* Off: a held card still redraws every row. */
static void
test_off_redraws_every_row(void) {
    raster_rig_t* r = open_pair(false);
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    place_card(r, card_path[0]);
    (void)frame(r, &view, NULL, -1);
    const box3f_t held = card_box(r);
    TEST_ASSERT_EQUAL_INT(H, rows(frame(r, &view, &held, 0)));
}

/* The kept rows are where the last picture was composed: after a draw
 * composed nowhere (no framebuffer), or into another destination, a held
 * card's frame recomposes every row. */
static void
test_a_draw_composed_nowhere_or_elsewhere_recomposes_every_row(void) {
    raster_rig_t* r = open_pair(true);
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    place_card(r, card_path[0]);
    (void)frame(r, &view, NULL, -1);
    const box3f_t held = card_box(r);
    render_context_t* c = &pair->context[0];
    render_context_set_changed(c, &held, 0);
    TEST_ASSERT_TRUE(render_context_draw(c, r->instance, RASTER_RIG_QUADS, &view, 0));
    TEST_ASSERT_FALSE(render_context_compose(c, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(H, rows(frame(r, &view, &held, 0)));
    render_context_set_changed(c, &held, 0);
    TEST_ASSERT_TRUE(render_context_draw(c, r->instance, RASTER_RIG_QUADS, &view, 0));
    (void)render_context_compose(c, pair->last, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(H, rows(render_context_frame(c)));
    TEST_ASSERT_EQUAL_MEMORY(pair->destination[1], pair->last, sizeof pair->last);
}

/* A new small fill alone, the card held and the view the same: the kept
 * picture is not reused, every row redraws, and it is the full picture the
 * new fill draws (which differs from the old, so a kept one would show). */
static void
test_a_new_small_fill_redraws_every_row(void) {
    static const uint8_t gradient[4][3] = {{32, 80, 96}, {128, 112, 48}, {224, 160, 144}, {48, 96, 80}};
    raster_rig_t* r = open_pair(true);
    r->quad[CARD].mesh.face_colors = NULL;
    r->quad[CARD].mesh.colors = gradient; /* a gradient, which the styles draw differently */
    fixture_camera_t far = raster_rig_camera;
    far.eye.z = FAR_EYE_Z; /* the card spans 8 pixels: past the default ceiling, inside the fast one */
    const render_view_t view = render_view_fixture_at(&far, (viewport_t){W, H, 0});
    place_card(r, card_path[0]);
    (void)frame(r, &view, NULL, -1);
    const box3f_t held = card_box(r);
    TEST_ASSERT_TRUE(frame(r, &view, &held, 0).unchanged);
    memcpy(pair->last, raster_color(&pair->context[1].raster), sizeof pair->last);
    for (int k = 0; k < 2; k++) {
        pair->settings[k].small_fill = (render_small_fill_settings_t){SMALL_FILL_CEILING_FAST, 0, SMALL_FILL_DITHER};
        render_context_set_settings(&pair->context[k], &pair->settings[k]);
    }
    TEST_ASSERT_EQUAL_INT(H, rows(frame(r, &view, &held, 0)));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(pair->last, raster_color(&pair->context[1].raster), sizeof pair->last));
}

/* Dynamic resolution's ladder in these tests: the top, then half of it. */
static const resolution_step_t dynres_steps[] = {{W, H}, {W / 2, H / 2}};
#define DYNRES_STEPS       ((int)(sizeof dynres_steps / sizeof dynres_steps[0]))
#define DYNRES_FLOOR       (DYNRES_STEPS - 1)
/* A budget no host frame reaches, and one every host frame passes. */
#define DYNRES_ROOMY       1000000000
#define DYNRES_TIGHT       1
/* Held frames past a full controller window, so a priced one would show. */
#define DYNRES_HELD_FRAMES (RESOLUTION_WINDOW_MAX + 2)
/* Frames after a held one over budget, each still the policy's. */
#define DYNRES_HANDED_BACK 3

/* Context [0] with partial redraw on under the stepped controller at
 * `budget_us`, starting at the floor. */
static raster_rig_t*
open_dynres(int32_t budget_us) {
    raster_rig_t* r = open_pair(true);
    const resolution_config_t config = resolution_config(dynres_steps, DYNRES_STEPS, DYNRES_STEPS, budget_us);
    render_context_set_dynamic_resolution(&pair->context[0], &config, NULL, DYNRES_FLOOR);
    return r;
}

/* One frame through context [0] alone, told `count` changed boxes. */
static render_context_frame_t
dynres_frame(raster_rig_t* r, const render_view_t* view, const box3f_t* changed, int count) {
    render_context_t* c = &pair->context[0];
    render_context_set_changed(c, changed, count);
    TEST_ASSERT_TRUE(render_context_draw(c, r->instance, RASTER_RIG_QUADS, view, 0));
    (void)render_context_compose(c, pair->destination[0], NULL, NULL);
    return render_context_frame(c);
}

/* Context [1], partial redraw off at the fixed scale, draws the frame whole:
 * the picture and destination context [0] drew at the top step must be it. */
static void
assert_full_redraw(raster_rig_t* r, const render_view_t* view) {
    TEST_ASSERT_TRUE(render_context_draw(&pair->context[1], r->instance, RASTER_RIG_QUADS, view, 0));
    (void)render_context_compose(&pair->context[1], pair->destination[1], NULL, NULL);
    TEST_ASSERT_EQUAL_MEMORY(raster_color(&pair->context[1].raster), raster_color(&pair->context[0].raster),
                             sizeof(uint16_t) * W * H);
    TEST_ASSERT_EQUAL_MEMORY(pair->destination[1], pair->destination[0], sizeof pair->destination[0]);
}

/* The card slid one step along x, its old and new place the changed box. */
static box3f_t
slide_card(raster_rig_t* r, float x) {
    const box3f_t was = card_box(r);
    place_card(r, (vec3f_t){x, 0.0F, 0.0F});
    return box3f_union(was, card_box(r));
}

/* A still camera under dynamic resolution draws as partial redraw does: one
 * frame at the top size, then only the rows the moving card covers, each the
 * picture a whole redraw gives, and the controller is not moved by them. */
static void
test_dynres_still_view_redraws_only_changed_rows_at_the_top(void) {
    raster_rig_t* r = open_dynres(DYNRES_ROOMY);
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    place_card(r, card_path[0]);
    TEST_ASSERT_EQUAL_INT(W / 2, dynres_frame(r, &view, NULL, -1).width);
    const resolution_control_t before = pair->context[0].control;
    for (int i = 1; i <= DYNRES_HELD_FRAMES; i++) {
        const box3f_t changed = slide_card(r, (float)(i % 2 + 1) * 6.0F); /* back and forth */
        const render_context_frame_t f = dynres_frame(r, &view, &changed, 1);
        TEST_ASSERT_EQUAL_INT(W, f.width);
        TEST_ASSERT_FALSE(pair->context[0].priced); /* nor does it teach the budget */
        if (i == 1) {
            TEST_ASSERT_EQUAL_INT(H, rows(f)); /* to the top: every row */
        } else {
            TEST_ASSERT_LESS_THAN_INT(H, rows(f));
        }
        assert_full_redraw(r, &view);
    }
    TEST_ASSERT_EQUAL_MEMORY(&before, &pair->context[0].control, sizeof before);
}

/* A partial frame at the top that costs more than the budget hands the still
 * camera back to dynamic resolution: the next frame is the controller's. */
static void
test_dynres_still_view_over_budget_returns_to_the_controller(void) {
    raster_rig_t* r = open_dynres(DYNRES_TIGHT);
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    place_card(r, card_path[0]);
    (void)dynres_frame(r, &view, NULL, -1);
    box3f_t changed = slide_card(r, 6.0F);
    TEST_ASSERT_EQUAL_INT(W, dynres_frame(r, &view, &changed, 1).width); /* to the top: not judged */
    changed = slide_card(r, 12.0F);
    TEST_ASSERT_LESS_THAN_INT(H, rows(dynres_frame(r, &view, &changed, 1))); /* partial, over budget */
    changed = slide_card(r, 18.0F);
    const render_context_frame_t f = dynres_frame(r, &view, &changed, 1);
    TEST_ASSERT_EQUAL_INT(W / 2, f.width);
    TEST_ASSERT_EQUAL_INT(H, rows(f));
    for (int i = 0; i < DYNRES_HANDED_BACK; i++) {
        changed = slide_card(r, (float)(i % 2 + 1) * 6.0F);
        TEST_ASSERT_EQUAL_INT(W / 2, dynres_frame(r, &view, &changed, 1).width); /* held, still the policy's */
    }
    fixture_camera_t moved = raster_rig_camera;
    moved.eye.x += 1.0F;
    const render_view_t next = render_view_fixture_at(&moved, (viewport_t){W, H, 0});
    TEST_ASSERT_EQUAL_INT(W / 2, dynres_frame(r, &next, NULL, -1).width); /* a new view: the policy's */
    changed = slide_card(r, 6.0F);
    TEST_ASSERT_EQUAL_INT(W, dynres_frame(r, &next, &changed, 1).width); /* held again: the top */
}

/* A hold is not time the predictor's choice settled in: the first frame the
 * policy draws after a held camera stays at the step it held before. */
static void
test_dynres_hold_does_not_count_toward_a_finer_step(void) {
    static const resolution_model_t free_model = {0}; /* every step fits */
    raster_rig_t* r = open_pair(true);
    resolution_config_t config = resolution_config(dynres_steps, DYNRES_STEPS, DYNRES_STEPS, DYNRES_ROOMY);
    config.up_hold_us = 1; /* any settled time steps finer */
    render_context_set_dynamic_resolution(&pair->context[0], &config, &free_model, DYNRES_FLOOR);
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    place_card(r, card_path[0]);
    TEST_ASSERT_EQUAL_INT(W / 2, dynres_frame(r, &view, NULL, -1).width); /* nothing settled yet */
    const box3f_t changed = slide_card(r, 6.0F);
    TEST_ASSERT_EQUAL_INT(W, dynres_frame(r, &view, &changed, 1).width);  /* held: the top */
    TEST_ASSERT_EQUAL_INT(W / 2, dynres_frame(r, &view, NULL, -1).width); /* the policy's, where it was */
}

/* Frames a held camera draws with frame generation on, past a generation's
 * retry so a warp due again would show. */
#define GENERATING_HELD_FRAMES (FRAMEGEN_RETRY_DRAWN + 2)

/* The card slides `frames` times under the held `view`; each frame is drawn,
 * not warped, and after the first it redraws only the card's rows, the picture
 * and destination a whole redraw gives (`frame()` checks against context [1]). */
static void
assert_held_frames_partial(raster_rig_t* r, const render_view_t* view, int frames) {
    for (int i = 1; i <= frames; i++) {
        const box3f_t changed = slide_card(r, (float)(i % 2 + 1) * 6.0F); /* back and forth */
        const render_context_frame_t f = frame(r, view, &changed, 1);
        TEST_ASSERT_FALSE(f.generated);
        TEST_ASSERT_LESS_THAN_INT(H, rows(f));
    }
}

/* Frame generation does not warp a held camera, even turned on mid-hold:
 * the object moving in it is drawn as partial redraw draws it, at the fixed
 * scale. A camera that moves is generated; the frame after that one is drawn
 * whole and timed, as generation prices it, and the next held frame is
 * partial again. */
static void
test_generation_draws_a_held_view_partially(void) {
    raster_rig_t* r = open_pair(true);
    render_context_t* c = &pair->context[0];
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    place_card(r, card_path[0]);
    TEST_ASSERT_EQUAL_INT(H, rows(frame(r, &view, NULL, -1)));
    assert_held_frames_partial(r, &view, 1);
    render_context_set_frame_generation(c, true);
    assert_held_frames_partial(r, &view, GENERATING_HELD_FRAMES);
    fixture_camera_t moved = raster_rig_camera;
    moved.eye.x += 1.0F;
    const render_view_t next = render_view_fixture_at(&moved, (viewport_t){W, H, 0});
    render_context_set_changed(c, NULL, -1);
    TEST_ASSERT_TRUE(render_context_draw(c, r->instance, RASTER_RIG_QUADS, &next, 0));
    TEST_ASSERT_TRUE(render_context_frame(c).generated);
    (void)render_context_compose(c, pair->destination[0], NULL, NULL);
    const render_context_frame_t drawn = frame(r, &next, NULL, -1);
    TEST_ASSERT_FALSE(drawn.generated);
    TEST_ASSERT_FALSE(c->fixed_drawn); /* timed: its cost prices generation */
    TEST_ASSERT_EQUAL_INT(H, rows(drawn));
    assert_held_frames_partial(r, &next, 1);
}

/* Under dynamic resolution too, a held camera with generation on draws as
 * partial redraw does at the top step, unwarped. */
static void
test_generation_under_dynres_draws_a_held_view_partially_at_the_top(void) {
    raster_rig_t* r = open_dynres(DYNRES_ROOMY);
    render_context_set_frame_generation(&pair->context[0], true);
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    place_card(r, card_path[0]);
    (void)dynres_frame(r, &view, NULL, -1);
    box3f_t changed = slide_card(r, 12.0F);
    const render_context_frame_t top = dynres_frame(r, &view, &changed, 1);
    TEST_ASSERT_FALSE(top.generated);
    TEST_ASSERT_EQUAL_INT(W, top.width); /* to the top: every row */
    TEST_ASSERT_EQUAL_INT(H, rows(top));
    assert_full_redraw(r, &view);
    for (int i = 1; i <= GENERATING_HELD_FRAMES; i++) {
        changed = slide_card(r, (float)(i % 2 + 1) * 6.0F);
        const render_context_frame_t f = dynres_frame(r, &view, &changed, 1);
        TEST_ASSERT_FALSE(f.generated);
        TEST_ASSERT_EQUAL_INT(W, f.width);
        TEST_ASSERT_LESS_THAN_INT(H, rows(f));
        assert_full_redraw(r, &view);
    }
}

/* A held view over budget is handed back to the policy with generation on
 * as without it: its frames are generated again, and the drawn ones are the
 * policy's, every row at its step, until the view changes; a generated
 * frame, which forgets the kept picture, does not end the hand-back. */
static void
test_generation_resumes_on_a_held_view_handed_back(void) {
    raster_rig_t* r = open_dynres(DYNRES_TIGHT);
    render_context_set_frame_generation(&pair->context[0], true);
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    place_card(r, card_path[0]);
    (void)dynres_frame(r, &view, NULL, -1);
    box3f_t changed = slide_card(r, 6.0F);
    TEST_ASSERT_EQUAL_INT(W, dynres_frame(r, &view, &changed, 1).width); /* to the top: not judged */
    changed = slide_card(r, 12.0F);
    TEST_ASSERT_LESS_THAN_INT(H, rows(dynres_frame(r, &view, &changed, 1))); /* partial, over budget */
    int generated = 0;
    for (int i = 0; i < GENERATING_HELD_FRAMES; i++) {
        changed = slide_card(r, (float)(i % 2 + 1) * 6.0F);
        const render_context_frame_t f = dynres_frame(r, &view, &changed, 1);
        generated += f.generated;
        if (!f.generated) { /* a generated frame reports the drawn one's size */
            TEST_ASSERT_EQUAL_INT(W / 2, f.width);
            TEST_ASSERT_EQUAL_INT(H, rows(f));
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(0, generated);
}

void
run_render_partial_suite(void) {
    RUN_TEST(test_a_moving_card_redraws_only_its_rows_and_matches_a_full_redraw);
    RUN_TEST(test_a_new_view_or_unknown_changes_redraw_every_row);
    RUN_TEST(test_off_redraws_every_row);
    RUN_TEST(test_a_draw_composed_nowhere_or_elsewhere_recomposes_every_row);
    RUN_TEST(test_a_new_small_fill_redraws_every_row);
    RUN_TEST(test_dynres_still_view_redraws_only_changed_rows_at_the_top);
    RUN_TEST(test_dynres_still_view_over_budget_returns_to_the_controller);
    RUN_TEST(test_dynres_hold_does_not_count_toward_a_finer_step);
    RUN_TEST(test_generation_draws_a_held_view_partially);
    RUN_TEST(test_generation_under_dynres_draws_a_held_view_partially_at_the_top);
    RUN_TEST(test_generation_resumes_on_a_held_view_handed_back);
}

SUITE_REGISTER(run_render_partial_suite);
