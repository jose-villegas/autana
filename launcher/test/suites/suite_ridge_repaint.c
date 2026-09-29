/* Host suite: the launcher ridge's incremental repaint, audited every frame. */
#include "suites.h"

#ifndef DEVICE_BUILD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_cleanup.h"
#include "unity.h"

#include "esp_heap_caps.h"

#include "gfx/gfx.h"
#include "gfx/gfx_test.h"
#include "ridge_arms.h"
#include "ui/ui_ridge.h"

#define FB_PIXELS ((size_t)GFX_WIDTH * GFX_HEIGHT)
#define FB_BYTES  (FB_PIXELS * sizeof(gfx_color_t))

typedef struct {
    int frames;
    int unsent_px, first_unsent_frame;
    int unlike_px, first_unlike_frame;
} audit_t;

static void
fixture(void) {
    if (gfx_mode_current()->width == 0) {
        TEST_ASSERT_TRUE(gfx_init());
    }
    suite_set_test_cleanup(gfx_reset_for_test);
    ui_ridge_reset_for_test();
    ui_ridge_step(&ridge_arm_idle_input, RIDGE_ARM_FRAME_DT_MS);
    gfx_present();
}

/* Every pixel the step changed must lie in a region the next present sends. */
static int
unsent_changes(const gfx_color_t* before) {
    const gfx_color_t* const now = gfx_framebuffer();
    int unsent = 0;
    for (size_t i = 0; i < FB_PIXELS; i++) {
        if (now[i] != before[i] && !gfx_region_dirty((int)(i % GFX_WIDTH), (int)(i / GFX_WIDTH), 1, 1)) {
            unsent++;
        }
    }
    return unsent;
}

/* Swaps the incrementally repainted frame for a full paint of the same
 * state, counts the pixels that differ, then puts the incremental frame
 * back so the next step continues from what it drew. */
static int
unlike_a_full_paint(gfx_color_t* repainted) {
    gfx_color_t* const fb = gfx_framebuffer();
    memcpy(repainted, fb, FB_BYTES);
    ui_ridge_paint();
    int unlike = 0;
    for (size_t i = 0; i < FB_PIXELS; i++) {
        unlike += repainted[i] != fb[i];
    }
    memcpy(fb, repainted, FB_BYTES);
    return unlike;
}

static audit_t
audit_arm(arm_t arm, int frames) {
    audit_t audit = {.first_unsent_frame = -1, .first_unlike_frame = -1};
    gfx_color_t* const before = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    gfx_color_t* const repainted = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(before);
    TEST_ASSERT_NOT_NULL(repainted);
    memcpy(before, gfx_framebuffer(), FB_BYTES);
    for (int frame = 0; frame < frames; frame++) {
        input_t input;
        ridge_arm_drive(arm, frame, &input);
        ui_ridge_step(&input, RIDGE_ARM_FRAME_DT_MS);
        const int unsent = unsent_changes(before);
        gfx_present();
        const int unlike = unlike_a_full_paint(repainted);
        gfx_present();
        if (unsent > 0 && audit.first_unsent_frame < 0) {
            audit.first_unsent_frame = frame;
        }
        if (unlike > 0 && audit.first_unlike_frame < 0) {
            audit.first_unlike_frame = frame;
        }
        audit.unsent_px += unsent;
        audit.unlike_px += unlike;
        audit.frames++;
        memcpy(before, gfx_framebuffer(), FB_BYTES);
    }
    heap_caps_free(before);
    heap_caps_free(repainted);
    return audit;
}

static void
assert_audit(const char* name, const audit_t* audit) {
    if (audit->unsent_px > 0) {
        printf("ridge repaint %s: %d changed px not dirty, first at frame %d\n", name, audit->unsent_px,
               audit->first_unsent_frame);
    }
    if (audit->unlike_px > 0) {
        printf("ridge repaint %s: %d px unlike a full paint, first at frame %d\n", name, audit->unlike_px,
               audit->first_unlike_frame);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, audit->unsent_px, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, audit->unlike_px, name);
}

static void
test_repaint_matches_a_full_paint_and_is_sent_after_a_pluck_and_strum(void) {
    fixture();
    const audit_t audit = audit_arm(ARM_PLUCK_STRUM, 90);
    assert_audit("pluck and strum", &audit);
}

static void
test_repaint_matches_a_full_paint_and_is_sent_through_a_tilt_sweep(void) {
    fixture();
    const audit_t audit = audit_arm(ARM_TILT_SWEEP, 150);
    assert_audit("tilt sweep", &audit);
}

static void
test_repaint_matches_a_full_paint_and_is_sent_while_wobbling(void) {
    fixture();
    const audit_t audit = audit_arm(ARM_TILT_WOBBLE, 150);
    assert_audit("tilt wobble", &audit);
}

void
suite_ridge_repaint(void) {
    /* The ridge allocates its state once and keeps it: do that here, so no
     * single test is charged for it. */
    ui_ridge_reset_for_test();
    RUN_TEST(test_repaint_matches_a_full_paint_and_is_sent_after_a_pluck_and_strum);
    RUN_TEST(test_repaint_matches_a_full_paint_and_is_sent_through_a_tilt_sweep);
    RUN_TEST(test_repaint_matches_a_full_paint_and_is_sent_while_wobbling);
}

#else

void
suite_ridge_repaint(void) {}

#endif

SUITE_REGISTER(suite_ridge_repaint);
