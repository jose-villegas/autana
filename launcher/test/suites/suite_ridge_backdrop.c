/*
 * The launcher over its ridge backdrop, through the firmware's own ui.c and
 * gfx/: what an idle home screen costs, what a touch wakes, and what is
 * left when the line comes back to rest.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "suites.h"

#ifndef DEVICE_BUILD

#include "input/motion/input_motion.h"
#include "input/motion/shake.h"
#include "input_fake.h"
#include "math/scalar/units.h"
#include "testkit/cleanup.h"
#include "unity.h"

#include "app/app.h"
#include "core/memory.h"
#include "draw/draw_dither.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_test.h"
#include "shell/ui/ui_launcher.h"
#include "shell/ui/ui_ridge.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

#define FRAME_MS       16
#define FRAMES_TO_REST 600
#define PIXELS         ((size_t)GFX_WIDTH * GFX_HEIGHT)

/* Below the app rows, in portrait. */
#define TOUCH_X        60
#define TOUCH_Y        400

static gfx_color_t* reference;
static gfx_color_t* as_boot_left_it;
static gfx_color_t* settled;
static int32_t* spring_reference;
static FILE* steady_frames;

static bool
frame(bool touching, bool pressed) {
    const input_t input = {
        .down = touching,
        .pressed = pressed,
        .x = TOUCH_X,
        .y = TOUCH_Y,
        .press_x = TOUCH_X,
        .press_y = TOUCH_Y,
    };
    ui_launcher_frame(&input, FRAME_MS);
    const bool sends = gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
    gfx_present();
    return sends;
}

static gfx_color_t*
snapshot(void) {
    gfx_color_t* copy = memory_alloc(PIXELS * sizeof *copy, MEMORY_PSRAM);
    if (copy != NULL) {
        memcpy(copy, gfx_framebuffer(), PIXELS * sizeof *copy);
    }
    return copy;
}

static size_t
lit_pixels(const gfx_color_t* pixels) {
    size_t lit = 0;
    for (size_t i = 0; i < PIXELS; i++) {
        lit += pixels[i] != 0;
    }
    return lit;
}

static void
release(void) {
    memory_free(reference);
    memory_free(as_boot_left_it);
    memory_free(settled);
    memory_free(spring_reference);
    spring_reference = NULL;
    input_fake_motion(NULL);
    if (steady_frames != NULL) {
        fclose(steady_frames);
        steady_frames = NULL;
    }
    reference = NULL;
    as_boot_left_it = NULL;
    settled = NULL;
    app_registry_reset_for_test();
    ui_ridge_release_for_test();
    gfx_reset_for_test();
}

static void
fixture(app_t* apps, size_t count) {
    suite_set_test_cleanup(release);
    app_registry_reset_for_test();
    for (size_t i = 0; i < count; i++) {
        app_register(&apps[i]);
    }
    TEST_ASSERT_TRUE(gfx_init());
    ui_ridge_reset_for_test();
    ui_ridge_restart_boot_for_test();
    ui_launcher_init();
}

static void
test_launcher_backdrop_motion_and_rest(void) {
    app_t apps[] = {
        {.name = "Alpha", .summary = "First screen"},
        {.name = "Beta", .summary = "Second screen"},
        {.name = "Gamma", .summary = "Third screen"},
    };
    fixture(apps, sizeof apps / sizeof *apps);
    ui_set_transform(affine2x_identity());

    /* Still first: breathing and the wave never rest, and what follows is
     * about what the launcher costs when nothing moves. */
    ui_ridge_set_ambient(false);
    frame(false, false);
    reference = snapshot();
    if (reference == NULL) {
        TEST_FAIL_MESSAGE("no memory for the frame copy");
    }
    ui_ridge_paint_reference_for_test(reference);
    const int phase_rows = draw_dither_pattern(DRAW_DITHER_SCANLINES4)->height;
    bool shaped = false;
    for (int y = phase_rows; y < GFX_HEIGHT; y++) {
        shaped = shaped
                 || memcmp(reference + (size_t)y * GFX_WIDTH, reference + (size_t)(y % phase_rows) * GFX_WIDTH,
                           GFX_WIDTH * sizeof *reference)
                        != 0;
    }
    TEST_ASSERT_TRUE_MESSAGE(shaped, "the engine pack supplies the shaped ridge, not the level fallback");
    memory_free(reference);
    reference = NULL;

    /* Held upright in portrait from the first frame: down is the panel's +y,
     * a quarter turn from the landscape pose boot hands over in. */
    ui_ridge_set_gravity((vec3f_t){0, 1, 0}, 0);
    frame(false, false);
    frame(false, false);
    as_boot_left_it = snapshot();
    if (as_boot_left_it == NULL) {
        TEST_FAIL_MESSAGE("no memory for the frame copy");
    }
    TEST_ASSERT_TRUE_MESSAGE(as_boot_left_it[0] != 0, "the backdrop is visible behind the launcher");
    TEST_ASSERT_TRUE_MESSAGE(lit_pixels(as_boot_left_it) > 8000, "the backdrop and the app rows are drawn");

    int held_frames_sending = 0;
    for (int i = 0; i < 30; i++) {
        held_frames_sending += frame(false, false);
    }
    TEST_ASSERT_TRUE_MESSAGE(held_frames_sending == 0, "the line keeps boot's pose at first, whatever gravity says");

    /* Near the end the pose moves less per frame than is worth redrawing, so
     * quiet frames come in runs before it has arrived: only a long run means
     * it has stopped. */
    int turning_frames = 0;
    int frames_until_level = 0;
    int quiet_run = 0;
    while (frames_until_level < FRAMES_TO_REST && quiet_run < 60) {
        const bool sends = frame(false, false);
        turning_frames += sends;
        quiet_run = sends || turning_frames == 0 ? 0 : quiet_run + 1;
        frames_until_level++;
    }
    TEST_ASSERT_TRUE_MESSAGE(turning_frames > 10, "then it turns toward level over many frames, not in one jump");
    TEST_ASSERT_TRUE_MESSAGE(frames_until_level < FRAMES_TO_REST, "and stops turning");
    TEST_ASSERT_TRUE_MESSAGE(memcmp(as_boot_left_it, gfx_framebuffer(), PIXELS * sizeof *as_boot_left_it) != 0,
                             "the line has turned");
    memory_free(as_boot_left_it);
    as_boot_left_it = NULL;

    settled = snapshot();
    if (settled == NULL) {
        TEST_FAIL_MESSAGE("no memory for the frame copy");
    }
    ui_ridge_settle();
    ui_invalidate();
    frame(false, false);
    TEST_ASSERT_TRUE_MESSAGE(memcmp(settled, gfx_framebuffer(), PIXELS * sizeof *settled) == 0,
                             "where it stopped is exactly level, not merely close, and leaves pixels unchanged");
    TEST_ASSERT_TRUE_MESSAGE(!frame(false, false), "a level, untouched launcher sends nothing");

    TEST_ASSERT_TRUE_MESSAGE(frame(true, true), "a touch sets the ridge moving on the frame it lands");
    frame(true, false);
    frame(false, false);
    int moving_frames = 0;
    for (int i = 0; i < 20; i++) {
        moving_frames += frame(false, false);
    }
    TEST_ASSERT_TRUE_MESSAGE(moving_frames == 20, "the wave keeps sending while it travels");
    TEST_ASSERT_TRUE_MESSAGE(memcmp(settled, gfx_framebuffer(), PIXELS * sizeof *settled) != 0,
                             "the line is displaced");

    /* Only pixels that change are sent, so the spring's last sub-pixel
     * wobbles leave quiet frames between sends: rest is a long quiet run. */
    int frames_until_quiet = 0;
    int still_run = 0;
    while (frames_until_quiet < FRAMES_TO_REST && still_run < 60) {
        still_run = frame(false, false) ? 0 : still_run + 1;
        frames_until_quiet++;
    }
    TEST_ASSERT_TRUE_MESSAGE(frames_until_quiet < FRAMES_TO_REST, "the line comes to rest");
    TEST_ASSERT_TRUE_MESSAGE(!frame(false, false) && !frame(false, false), "and then sends nothing again");
    TEST_ASSERT_TRUE_MESSAGE(memcmp(settled, gfx_framebuffer(), PIXELS * sizeof *settled) == 0,
                             "at rest the screen is the settled one exactly");

    /* Breathing and the wave: the launcher draws on, never strays far from
     * the ridge, and turned off again is back on the settled screen with
     * nothing left behind. */
    ui_ridge_set_ambient(true);
    int ambient_frames_sending = 0;
    size_t most_lit = 0;
    size_t least_lit = PIXELS;
    for (int i = 0; i < 400; i++) {
        ambient_frames_sending += frame(false, false);
        const size_t lit = lit_pixels(gfx_framebuffer());
        most_lit = lit > most_lit ? lit : most_lit;
        least_lit = lit < least_lit ? lit : least_lit;
    }
    TEST_ASSERT_TRUE_MESSAGE(ambient_frames_sending > 300, "with its ambient motion on, the ridge keeps moving");
    TEST_ASSERT_TRUE_MESSAGE(least_lit > lit_pixels(settled) * 8 / 10 && most_lit < lit_pixels(settled) * 12 / 10,
                             "and the backdrop stays whole while it moves: nothing it covers goes dark");

    ui_ridge_set_ambient(false);
    for (int i = 0; i < 40; i++) {
        frame(false, false);
    }
    TEST_ASSERT_TRUE_MESSAGE(!frame(false, false), "turned off, the launcher is idle again");
    TEST_ASSERT_TRUE_MESSAGE(memcmp(settled, gfx_framebuffer(), PIXELS * sizeof *settled) == 0,
                             "on exactly the settled screen: the motion left nothing behind");
}

#define VIBRATION_FRAMES ((int)(4 * MATH_MS_PER_S / FRAME_MS))
#define VIBRATION_G      (SHAKE_START_G / 2)
#define DELIBERATE_G     (SHAKE_FULL_G * 2)

static void
compare_shake_runs(float acceleration_g, bool expect_difference) {
    steady_frames = tmpfile();
    TEST_ASSERT_NOT_NULL(steady_frames);
    bool pixels_differ = false;
    bool springs_differ = false;
    for (int run = 0; run < 2; run++) {
        ui_ridge_release_for_test();
        gfx_reset_for_test();
        fixture(NULL, 0);
        ui_ridge_set_ambient(true);
        input_motion_reset();
        input_motion_t motion = {.acceleration = {-INPUT_ACCEL_ONE_G, 0, 0}};
        input_fake_motion(&motion);
        input_motion_frame(0);
        ui_ridge_set_gravity(input_gravity(), input_shake());
        const spring_line_t* line = ui_ridge_spring_for_test();
        TEST_ASSERT_NOT_NULL(line);
        const size_t spring_bytes = (size_t)line->count * sizeof *line->offset;
        if (run == 0) {
            reference = memory_alloc(PIXELS * sizeof *reference, MEMORY_PSRAM);
            spring_reference = memory_alloc(spring_bytes * 2, MEMORY_PSRAM);
            TEST_ASSERT_NOT_NULL(reference);
            TEST_ASSERT_NOT_NULL(spring_reference);
        }
        rewind(steady_frames);
        for (int i = 1; i <= VIBRATION_FRAMES; i++) {
            motion.acceleration.x =
                -INPUT_ACCEL_ONE_G + (run == 0 ? 0 : (int)((i % 2 ? -1 : 1) * acceleration_g * INPUT_ACCEL_ONE_G));
            input_motion_frame((int64_t)i * FRAME_MS * MATH_US_PER_MS);
            ui_ridge_set_gravity(input_gravity(), input_shake());
            if (!expect_difference) {
                TEST_ASSERT_EQUAL_FLOAT(0, input_shake());
            }
            frame(false, false);
            gfx_color_t* pixels = reference;
            int32_t* offsets = spring_reference;
            int32_t* velocities = offsets + line->count;
            if (run == 0) {
                memcpy(pixels, gfx_framebuffer(), PIXELS * sizeof *pixels);
                memcpy(offsets, line->offset, spring_bytes);
                memcpy(velocities, line->velocity, spring_bytes);
                TEST_ASSERT_EQUAL_size_t(PIXELS, fwrite(pixels, sizeof *pixels, PIXELS, steady_frames));
                TEST_ASSERT_EQUAL_size_t(2, fwrite(offsets, spring_bytes, 2, steady_frames));
            } else {
                TEST_ASSERT_EQUAL_size_t(PIXELS, fread(pixels, sizeof *pixels, PIXELS, steady_frames));
                TEST_ASSERT_EQUAL_size_t(2, fread(offsets, spring_bytes, 2, steady_frames));
            }
            if (run != 0 && !expect_difference) {
                TEST_ASSERT_EQUAL_MEMORY(pixels, gfx_framebuffer(), PIXELS * sizeof *pixels);
                TEST_ASSERT_EQUAL_MEMORY(offsets, line->offset, spring_bytes);
                TEST_ASSERT_EQUAL_MEMORY(velocities, line->velocity, spring_bytes);
            } else if (run != 0) {
                pixels_differ |= memcmp(pixels, gfx_framebuffer(), PIXELS * sizeof *pixels) != 0;
                springs_differ |= memcmp(offsets, line->offset, spring_bytes) != 0
                                  || memcmp(velocities, line->velocity, spring_bytes) != 0;
            }
        }
    }
    if (expect_difference) {
        TEST_ASSERT_EQUAL_FLOAT(1, input_shake());
        TEST_ASSERT_TRUE(pixels_differ);
        TEST_ASSERT_TRUE(springs_differ);
    }
    fclose(steady_frames);
    steady_frames = NULL;
    memory_free(reference);
    reference = NULL;
    memory_free(spring_reference);
    spring_reference = NULL;
    input_fake_motion(NULL);
}

static void
test_speaker_vibration_never_plucks_but_deliberate_shake_does(void) {
    compare_shake_runs(VIBRATION_G, false);
    compare_shake_runs(DELIBERATE_G, true);
}

void
suite_ridge_backdrop(void) {
    RUN_TEST(test_launcher_backdrop_motion_and_rest);
    RUN_TEST(test_speaker_vibration_never_plucks_but_deliberate_shake_does);
}

#else

void
suite_ridge_backdrop(void) {}

#endif

SUITE_REGISTER(suite_ridge_backdrop);
