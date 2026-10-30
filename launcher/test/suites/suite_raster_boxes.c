/*
 * Portable suite: the "boxes" debug view. It leaves the shaded picture as it
 * was, colours boxes by shape, and dims the stretches behind what was drawn.
 */

#include "suites.h"
#include "unity.h"

#ifndef DEVICE_BUILD
#include <string.h>

#include "raster_rig.h"

#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_test.h"
#include "image/pixel/rgb565.h"
#include "math/linear/mat3f.h"
#include "math/linear/mat4f.h"
#include "render/view/raster_boxes.h"
#include "render/view/raster_lines.h"
#include "render/view/raster_meshlets.h"

#define W            64
#define H            48

/* An object's shape, apart from the rig's instance shapes (0 and 1). */
#define OBJECT_SHAPE 5
/* Half an object box's side, in world units: inside the picture, outside the card. */
#define OBJECT_HALF  60
/* Object boxes' depth ranges: before the card (z = -100), and behind the wall (z = 0). */
#define FRONT_NEAR   (-160.0F)
#define FRONT_FAR    (-130.0F)
#define BACK_NEAR    50.0F
#define BACK_FAR     80.0F
/* The second card's offset from the first, in world units: still in the picture. */
#define CARD_SHIFT   20.0F

static raster_rig_t*
rig_open(unsigned placed) {
    raster_rig_t* r = raster_rig_open(raster_rig_wall_and_card, 2, placed, W, H, sizeof(raster_boxes_t));
    TEST_ASSERT_TRUE(gfx_init());
    suite_add_test_cleanup(gfx_reset_for_test);
    return r;
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

static void
test_an_object_before_everything_is_drawn_at_full_colour(void) {
    raster_rig_t* r = rig_open(0);
    const raster_box_t front = {{{-OBJECT_HALF, -OBJECT_HALF, FRONT_NEAR}, {OBJECT_HALF, OBJECT_HALF, FRONT_FAR}},
                                OBJECT_SHAPE};
    raster_boxes_set_objects(boxes_of(r), &front, 1);
    raster_attachment_t view;
    draw_with_boxes(r, &view);
    const gfx_color_t color = raster_meshlets_color(1U + OBJECT_SHAPE);
    TEST_ASSERT_TRUE(screen_pixels_of(color) > 0);
    TEST_ASSERT_EQUAL_INT(0, screen_pixels_of(dimmed(color)));
}

static void
test_an_object_behind_the_wall_is_dimmed(void) {
    raster_rig_t* r = rig_open(0);
    const raster_box_t back = {{{-OBJECT_HALF, -OBJECT_HALF, BACK_NEAR}, {OBJECT_HALF, OBJECT_HALF, BACK_FAR}},
                               OBJECT_SHAPE};
    raster_boxes_set_objects(boxes_of(r), &back, 1);
    raster_attachment_t view;
    draw_with_boxes(r, &view);
    const gfx_color_t color = raster_meshlets_color(1U + OBJECT_SHAPE);
    TEST_ASSERT_TRUE(brightness(dimmed(color)) < brightness(color));
    TEST_ASSERT_TRUE(screen_pixels_of(dimmed(color)) > 0);
    TEST_ASSERT_EQUAL_INT(0, screen_pixels_of(color));
}

static void
test_instances_of_one_mesh_share_a_colour(void) {
    raster_rig_t* r = rig_open(1U << 1);
    r->model[1] = (mat4f_t)MAT4F_IDENTITY;
    r->model[1].m[0][MAT3F_AXES] = CARD_SHIFT;
    r->instance[0].mesh = r->instance[1].mesh; /* two cards, no wall */
    raster_attachment_t view;
    draw_with_boxes(r, &view);
    TEST_ASSERT_TRUE(screen_pixels_of(raster_meshlets_color(1U)) > 0);
    TEST_ASSERT_EQUAL_INT(0, screen_pixels_of(raster_meshlets_color(2U)));
}
#endif

void
run_raster_boxes_suite(void) {
#ifndef DEVICE_BUILD
    RUN_TEST(test_boxes_view_leaves_the_shaded_picture_as_drawn);
    RUN_TEST(test_an_object_before_everything_is_drawn_at_full_colour);
    RUN_TEST(test_an_object_behind_the_wall_is_dimmed);
    RUN_TEST(test_instances_of_one_mesh_share_a_colour);
#endif
}

SUITE_REGISTER(run_raster_boxes_suite);
