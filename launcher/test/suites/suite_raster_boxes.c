/*
 * Portable suite: the "boxes" debug view. It leaves the shaded picture as it
 * was, boxes each instance in its mesh's colour, draws only meshes drawn
 * boxview.copies times, and hides or dims the stretches behind what was
 * drawn (boxview.behind).
 */

#include "suites.h"
#include "unity.h"

#ifndef DEVICE_BUILD
#include <string.h>

#include "raster_rig.h"

#include "core/tune.h"
#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_test.h"
#include "image/pixel/rgb565.h"
#include "math/linear/mat3f.h"
#include "math/linear/mat4f.h"
#include "render/view/raster_boxes.h"
#include "render/view/raster_lines.h"
#include "render/view/raster_meshlets.h"

#define W           64
#define H           48

/* The rig's card, its second quad and so its second shape. */
#define CARD        1
/* Moves the card from z = -100, before the wall at z = 0, to z = 50, behind it. */
#define CARD_BEHIND 150.0F
/* The second card's offset from the first, in world units: still in the picture. */
#define CARD_SHIFT  20.0F

static int32_t saved_copies, saved_behind;

static void
tunables_restore(void) {
    (void)tune_set(tune_shared(), TUNE_NAME(boxview, copies), saved_copies);
    (void)tune_set(tune_shared(), TUNE_NAME(boxview, behind), saved_behind);
}

static int32_t
tunable(const char* name) {
    const tune_entry_t* entry = tune_find(tune_shared(), name);
    TEST_ASSERT_NOT_NULL(entry);
    return *entry->value;
}

static raster_rig_t*
rig_open(unsigned placed) {
    raster_rig_t* r = raster_rig_open(raster_rig_wall_and_card, 2, placed, W, H, sizeof(raster_boxes_t));
    TEST_ASSERT_TRUE(gfx_init());
    suite_add_test_cleanup(gfx_reset_for_test);
    saved_copies = tunable(TUNE_NAME(boxview, copies));
    saved_behind = tunable(TUNE_NAME(boxview, behind));
    suite_add_test_cleanup(tunables_restore);
    TEST_ASSERT_EQUAL_INT(RASTER_BOXES_COPIES_DEFAULT, saved_copies);
    TEST_ASSERT_EQUAL_INT(RASTER_BOXES_BEHIND_HIDE, saved_behind);
    return r;
}

static void
set_behind(raster_boxes_behind_t behind) {
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(boxview, behind), behind));
}

static raster_boxes_t*
boxes_of(raster_rig_t* r) {
    return (raster_boxes_t*)r->own;
}

/* Draws the rig with the boxes view attached, then its overlay over a cleared screen. */
static void
draw_with_boxes(raster_rig_t* r, raster_attachment_t* view) {
    *view = raster_boxes_view(boxes_of(r));
    static const raster_attachment_t* attached[1];
    attached[0] = view;
    raster_rig_attach(r, attached, 1);
    r->view = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    raster_draw(&r->raster, &r->view);
    draw_clear(0);
    raster_boxes_overlay(boxes_of(r), W, H);
}

static int
screen_pixels_of(gfx_color_t color) {
    gfx_surface_t surface;
    TEST_ASSERT_TRUE(gfx_draw_begin(&surface));
    int count = 0;
    for (int y = 0; y < H; y++) {
        const gfx_color_t* row = gfx_target_color(surface.target, y);
        for (int x = 0; x < W; x++) {
            count += row[x] == color;
        }
    }
    return count;
}

static gfx_color_t
dimmed(gfx_color_t color) {
    return rgb565be_scale(color, RASTER_LINES_BEHIND_KEEP);
}

/* Every channel of `color`, summed at its own precision. */
static int
brightness(gfx_color_t color) {
    const uint16_t c = rgb565_swap(color);
    return (c >> 11) + ((c >> 5) & 0x3F) + (c & 0x1F);
}

static void
test_boxes_view_leaves_the_shaded_picture_as_drawn(void) {
    raster_rig_t* r = rig_open(0);
    r->view = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    raster_draw(&r->raster, &r->view);
    gfx_color_t* shaded = malloc(sizeof(gfx_color_t) * W * H);
    TEST_ASSERT_NOT_NULL(shaded);
    memcpy(shaded, raster_color(&r->raster), sizeof(gfx_color_t) * W * H);
    raster_attachment_t view;
    draw_with_boxes(r, &view);
    TEST_ASSERT_EQUAL_MEMORY(shaded, raster_color(&r->raster), sizeof(gfx_color_t) * W * H);
    free(shaded);
}

/* The card's box colour: its mesh is the rig's second, so it is the second shape. */
static gfx_color_t
card_color(void) {
    return raster_meshlets_color(1U + CARD);
}

/* The rig with its card moved from before the wall to behind it. */
static raster_rig_t*
rig_with_card_behind(void) {
    raster_rig_t* r = rig_open(1U << CARD);
    r->model[CARD] = (mat4f_t)MAT4F_IDENTITY;
    r->model[CARD].m[2][MAT3F_AXES] = CARD_BEHIND;
    return r;
}

static void
test_an_instance_before_the_wall_is_drawn_at_full_colour(void) {
    raster_rig_t* r = rig_open(0);
    raster_attachment_t view;
    draw_with_boxes(r, &view);
    TEST_ASSERT_TRUE(screen_pixels_of(card_color()) > 0);
    TEST_ASSERT_EQUAL_INT(0, screen_pixels_of(dimmed(card_color())));
}

static void
test_an_instance_behind_the_wall_is_hidden_by_default(void) {
    raster_rig_t* r = rig_with_card_behind();
    raster_attachment_t view;
    draw_with_boxes(r, &view);
    TEST_ASSERT_EQUAL_INT(0, screen_pixels_of(card_color()));
    TEST_ASSERT_EQUAL_INT(0, screen_pixels_of(dimmed(card_color())));
}

static void
test_an_instance_behind_the_wall_is_dimmed_when_asked(void) {
    raster_rig_t* r = rig_with_card_behind();
    set_behind(RASTER_BOXES_BEHIND_DIM);
    raster_attachment_t view;
    draw_with_boxes(r, &view);
    TEST_ASSERT_TRUE(brightness(dimmed(card_color())) < brightness(card_color()));
    TEST_ASSERT_TRUE(screen_pixels_of(dimmed(card_color())) > 0);
    TEST_ASSERT_EQUAL_INT(0, screen_pixels_of(card_color()));
}

static void
test_an_instance_behind_the_wall_is_full_when_asked(void) {
    raster_rig_t* r = rig_with_card_behind();
    set_behind(RASTER_BOXES_BEHIND_FULL);
    raster_attachment_t view;
    draw_with_boxes(r, &view);
    TEST_ASSERT_TRUE(screen_pixels_of(card_color()) > 0);
    TEST_ASSERT_EQUAL_INT(0, screen_pixels_of(dimmed(card_color())));
}

static void
test_instances_of_one_mesh_share_a_colour(void) {
    raster_rig_t* r = rig_open(1U << CARD);
    r->model[CARD] = (mat4f_t)MAT4F_IDENTITY;
    r->model[CARD].m[0][MAT3F_AXES] = CARD_SHIFT;
    static const uint8_t red[4][3] = {{255, 0, 0}, {255, 0, 0}, {255, 0, 0}, {255, 0, 0}};
    r3d_lit_mesh_t copy = *r->instance[1].mesh; /* two cards, no wall, the copy in its own colour set */
    copy.colors = red;
    r->instance[0].mesh = &copy;
    set_behind(RASTER_BOXES_BEHIND_FULL);
    raster_attachment_t view;
    draw_with_boxes(r, &view);
    TEST_ASSERT_TRUE(screen_pixels_of(raster_meshlets_color(1U)) > 0);
    TEST_ASSERT_EQUAL_INT(0, screen_pixels_of(raster_meshlets_color(2U)));
}

static void
test_meshes_drawn_fewer_times_than_boxview_copies_are_left_out(void) {
    raster_rig_t* r = rig_open(0);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(boxview, copies), 2));
    raster_attachment_t view;
    draw_with_boxes(r, &view);
    TEST_ASSERT_EQUAL_INT(0, screen_pixels_of(card_color()));
    const r3d_lit_mesh_t copy = *r->instance[1].mesh; /* two cards, one mesh */
    r->instance[0].mesh = &copy;
    draw_with_boxes(r, &view);
    TEST_ASSERT_TRUE(screen_pixels_of(raster_meshlets_color(1U)) > 0);
}
#endif

void
run_raster_boxes_suite(void) {
#ifndef DEVICE_BUILD
    RUN_TEST(test_boxes_view_leaves_the_shaded_picture_as_drawn);
    RUN_TEST(test_an_instance_before_the_wall_is_drawn_at_full_colour);
    RUN_TEST(test_an_instance_behind_the_wall_is_hidden_by_default);
    RUN_TEST(test_an_instance_behind_the_wall_is_dimmed_when_asked);
    RUN_TEST(test_an_instance_behind_the_wall_is_full_when_asked);
    RUN_TEST(test_instances_of_one_mesh_share_a_colour);
    RUN_TEST(test_meshes_drawn_fewer_times_than_boxview_copies_are_left_out);
#endif
}

SUITE_REGISTER(run_raster_boxes_suite);
