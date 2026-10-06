/*
 * display_shell: the shell's side of display: starting the panel, loading its
 * clock, and turning gravity into orientation. The shell reads the motion
 * sensor and hands gravity in; display never reads it. An app never calls
 * these. Defined in display_device.c, device only, except the sampler, which
 * display.c holds so its cadence is tested on a host. Applying a quarter
 * change to the UI is the caller's.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "display/display.h"
#include "util/math/vec2i.h"

/* The shell's orientation sampler: the quarter and when it may next ask for
 * gravity, so the cadence is as testable as the decision. 10 Hz is enough
 * for reorientation without lag. */
#define DISPLAY_SAMPLE_MS 100

/* Fills `gravity` in screen axes, the units display_update() takes; false
 * when there is no reading. */
typedef bool (*display_gravity_reader_t)(vec2i_t* gravity);

typedef struct {
    display_t display;
    int64_t next_sample_us;
} display_orientation_t;

/* Starts at DISPLAY_DEFAULT_QUARTER, due to sample at once. */
void display_orientation_init(display_orientation_t* o);

/* Reads gravity through `read` when DISPLAY_SAMPLE_MS has passed since the
 * last attempt, a failed read included, and feeds display_update(). True only
 * when the quarter changed. */
bool display_orientation_sample(display_orientation_t* o, int64_t now_us, display_gravity_reader_t read);

/* Where the system panel clock is kept across reboots, in the settings store. */
#define DISPLAY_PANEL_CLOCK_SETTINGS_SPACE "shell"
#define DISPLAY_PANEL_CLOCK_SETTINGS_KEY   "panel_hz"

/* Brings the panel up. False when the graphics cannot start; a development
 * build prints the DMA heap to say why. */
bool display_start(void);

/* Loads the saved panel clock and applies it. After display_start(). */
void display_load_panel_clock(void);

/* Puts the shell's orientation at DISPLAY_DEFAULT_QUARTER. */
void display_reset_quarter(void);

/* The shell's orientation, asking `read` for gravity at DISPLAY_SAMPLE_MS.
 * True when the quarter changed. */
bool display_sample_orientation(int64_t now_us, display_gravity_reader_t read);
