/*
 * Portable suite: the baked Control Center table, checked without going
 * through the generator that wrote it. gen_ui_layout.py applies the first
 * three rules at bake time; a generator checking itself is not a test.
 */

#include <stdbool.h>
#include <stdio.h>

#include "suites.h"
#include "unity.h"

#include "ui/control_center_layout_generated.h"

#define MIN_TAP_TARGET   44

/* About 15 px at every edge of the panel is not visible on the device. */
#define PANEL_EDGE_INSET 15

static const control_center_layout_t* const layouts[] = {
    &control_center_layout_portrait,
    &control_center_layout_landscape,
};

static bool
is_tap_target(int element) {
    return element != CONTROL_CENTER_ELEMENT_NOTIFICATIONS_HEADER;
}

static const char*
describe(const control_center_layout_t* layout, int element) {
    static char text[64];
    snprintf(text, sizeof text, "%dx%d canvas, element %d", layout->canvas_width, layout->canvas_height, element);
    return text;
}

static void
test_canvases_are_the_two_panel_orientations(void) {
    TEST_ASSERT_EQUAL_INT(368, control_center_layout_portrait.canvas_width);
    TEST_ASSERT_EQUAL_INT(448, control_center_layout_portrait.canvas_height);
    TEST_ASSERT_EQUAL_INT(448, control_center_layout_landscape.canvas_width);
    TEST_ASSERT_EQUAL_INT(368, control_center_layout_landscape.canvas_height);
}

static void
test_every_element_clears_the_panel_edge(void) {
    for (size_t l = 0; l < sizeof layouts / sizeof layouts[0]; l++) {
        const control_center_layout_t* layout = layouts[l];
        for (int e = 0; e < CONTROL_CENTER_ELEMENT_COUNT; e++) {
            const control_center_layout_rect_t* r = &layout->rects[e];
            const char* where = describe(layout, e);
            TEST_ASSERT_TRUE_MESSAGE(r->width > 0 && r->height > 0, where);
            TEST_ASSERT_TRUE_MESSAGE(r->x >= PANEL_EDGE_INSET && r->y >= PANEL_EDGE_INSET, where);
            TEST_ASSERT_TRUE_MESSAGE(r->x + r->width <= layout->canvas_width - PANEL_EDGE_INSET, where);
            TEST_ASSERT_TRUE_MESSAGE(r->y + r->height <= layout->canvas_height - PANEL_EDGE_INSET, where);
        }
    }
}

static void
test_every_tap_target_is_finger_sized(void) {
    for (size_t l = 0; l < sizeof layouts / sizeof layouts[0]; l++) {
        const control_center_layout_t* layout = layouts[l];
        for (int e = 0; e < CONTROL_CENTER_ELEMENT_COUNT; e++) {
            if (!is_tap_target(e)) {
                continue;
            }
            TEST_ASSERT_TRUE_MESSAGE(layout->rects[e].width >= MIN_TAP_TARGET, describe(layout, e));
            TEST_ASSERT_TRUE_MESSAGE(layout->rects[e].height >= MIN_TAP_TARGET, describe(layout, e));
        }
    }
}

static void
test_no_two_elements_overlap(void) {
    for (size_t l = 0; l < sizeof layouts / sizeof layouts[0]; l++) {
        const control_center_layout_t* layout = layouts[l];
        for (int a = 0; a < CONTROL_CENTER_ELEMENT_COUNT; a++) {
            for (int b = a + 1; b < CONTROL_CENTER_ELEMENT_COUNT; b++) {
                const control_center_layout_rect_t* p = &layout->rects[a];
                const control_center_layout_rect_t* q = &layout->rects[b];
                const bool apart = p->x + p->width <= q->x || q->x + q->width <= p->x || p->y + p->height <= q->y
                                   || q->y + q->height <= p->y;
                TEST_ASSERT_TRUE_MESSAGE(apart, describe(layout, a));
            }
        }
    }
}

void
suite_control_center_layout(void) {
    RUN_TEST(test_canvases_are_the_two_panel_orientations);
    RUN_TEST(test_every_element_clears_the_panel_edge);
    RUN_TEST(test_every_tap_target_is_finger_sized);
    RUN_TEST(test_no_two_elements_overlap);
}

SUITE_REGISTER(suite_control_center_layout);
