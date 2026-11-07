/*
 * Portable suite: render/partial through the render context. A context with
 * partial redraw on and one with it off draw the same rig frame after frame:
 * the card moves, holds, or the camera moves, and the two pictures and
 * composed destinations stay identical while the partial one recomposes
 * fewer rows. Every mesh is built inside the test.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "raster_rig.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "math/geometry/box3f.h"
#include "math/linear/transformf.h"
#include "render/context/render_context.h"
#include "render/r3d_lit_mesh.h"
#include "render/raster.h"

#define W    96
#define H    64
#define CARD 1 /* the rig quad the tests move */

/* The card's two faces, red against the white wall, so every move shows. */
static const uint16_t card_colors[2] = {0xF800, 0xF800};

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

/* Off, as RENDER_SETTINGS_DEFAULT has it: a held card still redraws every row. */
static void
test_off_redraws_every_row(void) {
    raster_rig_t* r = open_pair(false);
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    place_card(r, card_path[0]);
    (void)frame(r, &view, NULL, -1);
    const box3f_t held = card_box(r);
    TEST_ASSERT_EQUAL_INT(H, rows(frame(r, &view, &held, 0)));
}

void
run_render_partial_suite(void) {
    RUN_TEST(test_a_moving_card_redraws_only_its_rows_and_matches_a_full_redraw);
    RUN_TEST(test_a_new_view_or_unknown_changes_redraw_every_row);
    RUN_TEST(test_off_redraws_every_row);
}

SUITE_REGISTER(run_render_partial_suite);
