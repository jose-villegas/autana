/* Host suite: the launcher ridge's incremental repaint, audited every frame. */
#include "suites.h"

#ifndef DEVICE_BUILD

#include <stdio.h>
#include <string.h>

#include "testkit/cleanup.h"
#include "unity.h"

#include "core/memory.h"
#include "core/tune.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_test.h"
#include "gfx/present/send/gfx_dirty.h"
#include "ridge_arms.h"
#include "shell/ui/ui_ridge.h"

#define FB_PIXELS ((size_t)GFX_WIDTH * GFX_HEIGHT)
#define FB_BYTES  (FB_PIXELS * sizeof(gfx_color_t))

typedef struct {
    int frames, dissolved;
    int unsent_px, first_unsent_frame;
    int unlike_px, first_unlike_frame;
} audit_t;

/* What a frame is compared with: the frame before it, and a full paint that
 * never touches the ridge's or gfx's state. */
typedef struct {
    gfx_color_t* before;
    gfx_color_t* reference;
    audit_t audit;
} rig_t;

static void
release(void) {
    (void)tune_reset(tune_shared(), TUNE_NAME(ridge, axis_dissolve_ms));
    (void)tune_reset(tune_shared(), TUNE_NAME(ridge, level_tau_ms));
    gfx_reset_for_test();
}

static void
fixture(void) {
    if (gfx_mode_current()->width == 0) {
        TEST_ASSERT_TRUE(gfx_init());
    }
    suite_set_test_cleanup(release);
    ui_ridge_reset_for_test();
}

static void
prime(void) {
    ui_ridge_step(&ridge_arm_idle_input, RIDGE_ARM_FRAME_DT_MS);
    gfx_present();
}

static void
rig_open(rig_t* rig) {
    *rig = (rig_t){.audit = {.first_unsent_frame = -1, .first_unlike_frame = -1}};
    rig->before = memory_alloc(FB_BYTES, MEMORY_PSRAM);
    rig->reference = memory_alloc(FB_BYTES, MEMORY_PSRAM);
    if (rig->before == NULL || rig->reference == NULL) {
        memory_free(rig->before);
        memory_free(rig->reference);
        TEST_FAIL_MESSAGE("no room for the frame copies");
    }
    memcpy(rig->before, gfx_framebuffer(), FB_BYTES);
}

static audit_t
rig_close(rig_t* rig) {
    memory_free(rig->before);
    memory_free(rig->reference);
    return rig->audit;
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

static int
unlike_a_full_paint(gfx_color_t* reference) {
    const gfx_color_t* const now = gfx_framebuffer();
    ui_ridge_paint_reference_for_test(reference);
    int unlike = 0;
    for (size_t i = 0; i < FB_PIXELS; i++) {
        unlike += now[i] != reference[i];
    }
    return unlike;
}

/* A strip switch still dissolving in is by design unlike a full paint, but
 * what it changed must still be sent. */
static void
rig_frame(rig_t* rig, const input_t* input) {
    audit_t* const audit = &rig->audit;
    ui_ridge_step(input, RIDGE_ARM_FRAME_DT_MS);
    const int unsent = unsent_changes(rig->before);
    const bool dissolving = ui_ridge_dissolving_for_test();
    const int unlike = dissolving ? 0 : unlike_a_full_paint(rig->reference);
    gfx_present();
    if (unsent > 0 && audit->first_unsent_frame < 0) {
        audit->first_unsent_frame = audit->frames;
    }
    if (unlike > 0 && audit->first_unlike_frame < 0) {
        audit->first_unlike_frame = audit->frames;
    }
    audit->unsent_px += unsent;
    audit->unlike_px += unlike;
    audit->dissolved += dissolving;
    audit->frames++;
    memcpy(rig->before, gfx_framebuffer(), FB_BYTES);
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
start_arm(arm_t arm) {
    ui_ridge_set_ambient(arm >= ARM_AMBIENT);
    if (arm == ARM_AMBIENT_PORTRAIT) {
        ui_ridge_set_gravity((vec3f_t){(0) / RIDGE_FIXTURE_GRAVITY_SCALE, (256) / RIDGE_FIXTURE_GRAVITY_SCALE, 0}, 0);
    }
    if (arm == ARM_AMBIENT_BOOT) {
        ui_ridge_restart_boot_for_test();
    }
    prime();
}

static audit_t
audit_arm(arm_t arm, int frames) {
    start_arm(arm);
    rig_t rig;
    rig_open(&rig);
    for (int frame = 0; frame < frames; frame++) {
        input_t input;
        ridge_arm_drive(arm, frame, &input);
        rig_frame(&rig, &input);
    }
    return rig_close(&rig);
}

static void
test_a_pluck_and_strum_repaint_as_a_full_paint_and_are_sent(void) {
    fixture();
    const audit_t audit = audit_arm(ARM_PLUCK_STRUM, 90);
    assert_audit("pluck and strum", &audit);
}

static void
test_a_tilt_sweep_repaints_as_a_full_paint_and_is_sent(void) {
    fixture();
    const audit_t audit = audit_arm(ARM_TILT_SWEEP, 150);
    assert_audit("tilt sweep", &audit);
}

static void
test_a_wobble_repaints_as_a_full_paint_and_is_sent(void) {
    fixture();
    const audit_t audit = audit_arm(ARM_TILT_WOBBLE, 150);
    assert_audit("tilt wobble", &audit);
}

static void
test_the_ambient_ridge_repaints_as_a_full_paint_and_is_sent(void) {
    fixture();
    const audit_t audit = audit_arm(ARM_AMBIENT, 200);
    assert_audit("ambient landscape", &audit);
}

static void
test_the_ambient_ridge_held_upright_repaints_as_a_full_paint_and_is_sent(void) {
    fixture();
    const audit_t audit = audit_arm(ARM_AMBIENT_PORTRAIT, 300);
    assert_audit("ambient portrait", &audit);
}

static void
test_shaking_plucks_repaint_as_a_full_paint_and_are_sent(void) {
    fixture();
    const audit_t audit = audit_arm(ARM_AMBIENT_SHAKE, 150);
    assert_audit("ambient shaking", &audit);
}

/* From the boot hold through the whole ease-in of the ambient motion. */
static void
test_the_boot_hold_and_ease_in_repaint_as_a_full_paint_and_are_sent(void) {
    fixture();
    const audit_t audit = audit_arm(ARM_AMBIENT_BOOT, 320);
    assert_audit("boot and ease-in", &audit);
}

/* The dissolve a development build can turn on for the strip switch: every
 * pixel it changes is sent, and it ends on a full paint's frame. */
static void
test_a_dissolved_strip_switch_is_sent_and_ends_on_a_full_paint(void) {
    fixture();
    (void)tune_set(tune_shared(), TUNE_NAME(ridge, axis_dissolve_ms), 400);
    const audit_t audit = audit_arm(ARM_TILT_SWEEP, 150);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, audit.dissolved, "the sweep never dissolved a strip switch");
    TEST_ASSERT_FALSE_MESSAGE(ui_ridge_dissolving_for_test(), "the strip switch never finished dissolving in");
    assert_audit("dissolved switch", &audit);
}

/* Crossing the diagonal again before the first dissolve is over restarts
 * it: the levels already painted must not be left showing the old picture. */
static void
test_a_strip_switch_inside_a_running_dissolve_is_sent_and_ends_on_a_full_paint(void) {
    fixture();
    (void)tune_set(tune_shared(), TUNE_NAME(ridge, axis_dissolve_ms), 400);
    (void)tune_set(tune_shared(), TUNE_NAME(ridge, level_tau_ms), 60);
    start_arm(ARM_TILT_SWEEP);
    rig_t rig;
    rig_open(&rig);
    for (int frame = 0; frame < 144; frame++) {
        const bool upright = frame / 12 % 2 == 0;
        ui_ridge_set_gravity((vec3f_t){(upright ? -87 : -241) / RIDGE_FIXTURE_GRAVITY_SCALE,
                                       (upright ? 241 : 87) / RIDGE_FIXTURE_GRAVITY_SCALE, 0},
                             0);
        rig_frame(&rig, &ridge_arm_idle_input);
    }
    for (int frame = 0; frame < 60; frame++) {
        rig_frame(&rig, &ridge_arm_idle_input);
    }
    const int restarts = ui_ridge_dissolve_restarts_for_test();
    const bool dissolving = ui_ridge_dissolving_for_test();
    const audit_t audit = rig_close(&rig);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, restarts, "no switch began inside a running dissolve");
    TEST_ASSERT_FALSE_MESSAGE(dissolving, "the restarted dissolve never finished");
    assert_audit("restarted dissolve", &audit);
}

/* Once the tilting stops, the gradient catches up with the ridge (even a
 * turn of about a degree, less than it follows in one step) and every
 * frame on the way is a full paint's. */
static void
test_the_gradient_catches_up_once_the_tilt_stops(void) {
    fixture();
    start_arm(ARM_TILT_WOBBLE);
    rig_t rig;
    rig_open(&rig);
    input_t input;
    for (int frame = 0; frame < 160; frame++) {
        ridge_arm_drive(ARM_TILT_WOBBLE, frame, &input);
        rig_frame(&rig, &input);
    }
    ui_ridge_set_gravity((vec3f_t){(-240) / RIDGE_FIXTURE_GRAVITY_SCALE, (88) / RIDGE_FIXTURE_GRAVITY_SCALE, 0}, 0);
    for (int frame = 0; frame < 150; frame++) {
        rig_frame(&rig, &ridge_arm_idle_input);
    }
    ui_ridge_set_gravity((vec3f_t){(-241) / RIDGE_FIXTURE_GRAVITY_SCALE, (84) / RIDGE_FIXTURE_GRAVITY_SCALE, 0}, 0);
    for (int frame = 0; frame < 150; frame++) {
        rig_frame(&rig, &ridge_arm_idle_input);
    }
    const int lag = ui_ridge_gradient_lag_for_test();
    const audit_t audit = rig_close(&rig);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, lag, "the gradient never caught up with the ridge");
    assert_audit("settling", &audit);
}

/* The tracker only narrows a cell's box back after a send, so each frame
 * starts from an empty tracker: its boxes then hold this step's marks alone. */
static void
clear_dirty_boxes(void) {
    for (int row = 0; row < STRIP_COUNT; row++) {
        dirty_row_sent(row);
    }
    dirty_frame_sent();
}

/* The bounding box of the pixels changed within `area`. */
static recti_t
changed_box(const gfx_color_t* before, recti_t area) {
    const gfx_color_t* const now = gfx_framebuffer();
    recti_t box = {0};
    for (int y = area.y; y < recti_bottom(area); y++) {
        for (int x = area.x; x < recti_right(area); x++) {
            if (now[(y * GFX_WIDTH) + x] != before[(y * GFX_WIDTH) + x]) {
                box = recti_union(box, (recti_t){x, y, 1, 1});
            }
        }
    }
    return box;
}

static recti_t
cell_rect(int row, int col) {
    return (recti_t){col * COL_WIDTH, row * STRIP_HEIGHT, COL_WIDTH, STRIP_HEIGHT};
}

/* The most a cell's box may hold: each column group's changed box, as far
 * as it reaches into the cell, and each row's span from its first changed
 * pixel to its last (the gradient's marks, which cross unchanged groups). */
static recti_t
marks_bounds_in_cell(const gfx_color_t* before, int row, int col) {
    recti_t bounds = {0};
    for (int y = row * STRIP_HEIGHT; y < (row + 1) * STRIP_HEIGHT; y++) {
        const recti_t span = changed_box(before, (recti_t){0, y, GFX_WIDTH, 1});
        bounds = recti_union(bounds, recti_intersect(span, cell_rect(row, col)));
    }
    for (int x0 = 0; x0 < GFX_WIDTH; x0 += RIDGE_COLUMN_GROUP) {
        const recti_t group =
            changed_box(before, (recti_t){x0, 0, mathi_min(RIDGE_COLUMN_GROUP, GFX_WIDTH - x0), GFX_HEIGHT});
        bounds = recti_union(bounds, recti_intersect(group, cell_rect(row, col)));
    }
    return bounds;
}

static bool
holds(recti_t outer, recti_t inner) {
    return recti_is_empty(inner)
           || recti_intersect(outer, inner).w * recti_intersect(outer, inner).h == inner.w * inner.h;
}

/* A column group's mark must reach every changed pixel and stop at their
 * bounding box: the audit above passes a box that is too big. Each cell's
 * box is held between the box of its changed pixels and the most the marks
 * may reach (marks_bounds_in_cell). Blind to: a box too big but inside that. */
static void
test_a_column_groups_dirty_box_covers_what_changed_and_no_more(void) {
    fixture();
    start_arm(ARM_AMBIENT_PORTRAIT);
    rig_t rig;
    rig_open(&rig);
    int checked = 0;
    int loose = 0;
    int short_of = 0;
    for (int frame = 0; frame < 300; frame++) {
        input_t input;
        ridge_arm_drive(ARM_AMBIENT_PORTRAIT, frame, &input);
        clear_dirty_boxes();
        ui_ridge_step(&input, RIDGE_ARM_FRAME_DT_MS);
        for (int row = 0; row < STRIP_COUNT && !all_dirty; row++) {
            for (int col = 0; col < GRID_COLS; col++) {
                const int idx = (row * GRID_COLS) + col;
                const recti_t cell = cell_rect(row, col);
                const recti_t marked = (cell_dirty & (1u << idx)) != 0
                                           ? recti_from_edges(cell_x0[idx], cell_y0[idx], cell_x1[idx], cell_y1[idx])
                                           : (recti_t){0};
                const recti_t changed = changed_box(rig.before, cell);
                const recti_t most = marks_bounds_in_cell(rig.before, row, col);
                checked += !recti_is_empty(marked);
                short_of += !holds(marked, changed);
                loose += !holds(most, marked);
            }
        }
        gfx_present();
        memcpy(rig.before, gfx_framebuffer(), FB_BYTES);
    }
    rig_close(&rig);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, checked, "no cell was marked");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, short_of, "cells whose box misses a changed pixel");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, loose, "cells whose box reaches past their groups' changed pixels");
}

void
suite_ridge_repaint(void) {
    /* The ridge allocates its state once and keeps it: do that here, so no
     * single test is charged for it. */
    ui_ridge_reset_for_test();
    RUN_TEST(test_a_pluck_and_strum_repaint_as_a_full_paint_and_are_sent);
    RUN_TEST(test_a_tilt_sweep_repaints_as_a_full_paint_and_is_sent);
    RUN_TEST(test_a_wobble_repaints_as_a_full_paint_and_is_sent);
    RUN_TEST(test_the_ambient_ridge_repaints_as_a_full_paint_and_is_sent);
    RUN_TEST(test_the_ambient_ridge_held_upright_repaints_as_a_full_paint_and_is_sent);
    RUN_TEST(test_shaking_plucks_repaint_as_a_full_paint_and_are_sent);
    RUN_TEST(test_the_boot_hold_and_ease_in_repaint_as_a_full_paint_and_are_sent);
    RUN_TEST(test_a_dissolved_strip_switch_is_sent_and_ends_on_a_full_paint);
    RUN_TEST(test_a_strip_switch_inside_a_running_dissolve_is_sent_and_ends_on_a_full_paint);
    RUN_TEST(test_the_gradient_catches_up_once_the_tilt_stops);
    RUN_TEST(test_a_column_groups_dirty_box_covers_what_changed_and_no_more);
}

#else

void
suite_ridge_repaint(void) {}

#endif

SUITE_REGISTER(suite_ridge_repaint);
