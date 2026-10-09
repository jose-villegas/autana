/* Host suite: the launcher ridge's incremental repaint, audited every frame. */
#include "suites.h"

#ifndef DEVICE_BUILD

#include <stdio.h>
#include <string.h>

#include "test_cleanup.h"
#include "unity.h"

#include "gfx/gfx.h"
#include "gfx/gfx_test.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "ridge_arms.h"
#include "ui/ui_launcher.h"
#include "ui/ui_ridge.h"
#include "util/runtime/memory.h"
#include "util/runtime/tune.h"

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
ignore_tune_reply(const char* line) {
    (void)line;
}

static void
release(void) {
    tune_handle_line("RESET ridge.axis_dissolve_ms", ignore_tune_reply);
    tune_handle_line("RESET ridge.level_tau_ms", ignore_tune_reply);
    gfx_heal_restore_defaults();
    gfx_reset_for_test();
}

/* The launcher's own present settings, so the presenter's heal carries
 * over between frames as it does there. */
static void
fixture(void) {
    if (gfx_mode_current()->width == 0) {
        TEST_ASSERT_TRUE(gfx_init());
    }
    suite_set_test_cleanup(release);
    ui_launcher_heal_opt_in();
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
        ui_ridge_set_gravity(0, 256, 256, 0);
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
    tune_handle_line("SET ridge.axis_dissolve_ms 400", ignore_tune_reply);
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
    tune_handle_line("SET ridge.axis_dissolve_ms 400", ignore_tune_reply);
    tune_handle_line("SET ridge.level_tau_ms 60", ignore_tune_reply);
    start_arm(ARM_TILT_SWEEP);
    rig_t rig;
    rig_open(&rig);
    for (int frame = 0; frame < 144; frame++) {
        const bool upright = frame / 12 % 2 == 0;
        ui_ridge_set_gravity(upright ? -87 : -241, upright ? 241 : 87, 256, 0);
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
    ui_ridge_set_gravity(-240, 88, 256, 0);
    for (int frame = 0; frame < 150; frame++) {
        rig_frame(&rig, &ridge_arm_idle_input);
    }
    ui_ridge_set_gravity(-241, 84, 256, 0);
    for (int frame = 0; frame < 150; frame++) {
        rig_frame(&rig, &ridge_arm_idle_input);
    }
    const int lag = ui_ridge_gradient_lag_for_test();
    const audit_t audit = rig_close(&rig);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, lag, "the gradient never caught up with the ridge");
    assert_audit("settling", &audit);
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
}

#else

void
suite_ridge_repaint(void) {}

#endif

SUITE_REGISTER(suite_ridge_repaint);
