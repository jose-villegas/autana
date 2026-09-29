/*
 * The launcher ridge's scripted motions - a tilt sweep, a wobble, a pluck
 * and strum - shared by the host suite that audits every repainted frame
 * and the device suite that times them, so both drive the same frames.
 */
#pragma once

#include <stdbool.h>

#include "gfx/gfx.h"
#include "input/input.h"
#include "ui/ui_ridge.h"

typedef enum {
    ARM_IDLE,
    ARM_PLUCK_STRUM,
    ARM_TILT_SWEEP,
    ARM_TILT_WOBBLE,
    ARM_AMBIENT,
    ARM_AMBIENT_PORTRAIT,
    /* After the device's arms: its loop ends at ARM_AMBIENT_PORTRAIT. */
    ARM_AMBIENT_SHAKE,
    ARM_AMBIENT_BOOT,
} arm_t;

#define RIDGE_ARM_FRAME_DT_MS 16

static const input_t ridge_arm_idle_input = {0};

static inline input_t
ridge_arm_pluck_strum_input(int frame) {
    return (input_t){
        .x = (frame * 19) % GFX_WIDTH,
        .y = GFX_HEIGHT / 2,
        .down = true,
        .pressed = frame == 0,
    };
}

static inline void
ridge_arm_set_sweep_gravity(int frame) {
    static const int gravity[][2] = {
        {-256, 0}, {-192, -128}, {-128, -192}, {0, -256}, {128, -192}, {192, -128},
        {256, 0},  {192, 128},   {128, 192},   {0, 256},  {-128, 192}, {-192, 128},
    };
    const int phase = (frame / 12) % (int)(sizeof gravity / sizeof gravity[0]);
    ui_ridge_set_gravity(gravity[phase][0], gravity[phase][1], 256, 0);
}

/* A hand tilting the board back and forth, about 15 degrees either side of
 * landscape, once a second. */
static inline void
ridge_arm_set_wobble_gravity(int frame) {
    static const int gravity[][2] = {
        {-256, 0}, {-247, 66}, {-222, 128}, {-247, 66}, {-256, 0}, {-247, -66}, {-222, -128}, {-247, -66},
    };
    ui_ridge_set_gravity(gravity[(frame / 8) % 8][0], gravity[(frame / 8) % 8][1], 256, 0);
}

static inline void
ridge_arm_drive(arm_t arm, int frame, input_t* input) {
    *input = ridge_arm_idle_input;
    if (arm == ARM_PLUCK_STRUM) {
        *input = ridge_arm_pluck_strum_input(frame);
    } else if (arm == ARM_TILT_SWEEP) {
        ridge_arm_set_sweep_gravity(frame);
    } else if (arm == ARM_TILT_WOBBLE) {
        ridge_arm_set_wobble_gravity(frame);
    } else if (arm == ARM_AMBIENT_SHAKE) {
        ui_ridge_set_gravity(-256, 0, 256, frame % 40 < 10 ? 120 : 0);
    }
}
