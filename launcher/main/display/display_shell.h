/*
 * display_shell: panel startup, saved clock and shell orientation. The sampler
 * and its holds are host-testable in display.c; device entry points live in
 * display_device.c. A hold does not move the UI transform; the shell
 * re-applies it on the next change or after a release.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "display/display.h"

/* Fills screen-axis acceleration in g; false when there is no reading. */
typedef bool (*display_acceleration_reader_t)(vec3f_t* acceleration);

typedef struct {
    display_t display;
    int64_t next_sample_us;
    bool held;
    bool released;
} display_orientation_t;

/* Starts at DISPLAY_DEFAULT_QUARTER, due to sample at once. */
void display_orientation_init(display_orientation_t* o);

/* Reads acceleration through `read` when DISPLAY_SAMPLE_MS has passed since the
 * last attempt, a failed read included, and feeds display_update(). True when
 * the quarter changed or the sampler has been released. */
bool display_orientation_sample(display_orientation_t* o, int64_t now_us, display_acceleration_reader_t read);

void display_orientation_hold(display_orientation_t* o, int quarter);
void display_orientation_release(display_orientation_t* o);

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

/* Holds bypass sensor reads. Lock rotation with display_hold_quarter(display_quarter_now()).
 * Release restarts the filter; changes still pass every judge gate. */
void display_hold_quarter(int quarter);
void display_release_quarter(void);
bool display_quarter_held(void);

/* Samples the shell's orientation, asking `read` for acceleration at most every
 * DISPLAY_SAMPLE_MS. True when the quarter changed or after a release. */
bool display_sample_orientation(int64_t now_us, display_acceleration_reader_t read);

/* Call every frame a finger is down, before sampling orientation. */
void display_orientation_touch(int64_t now_us);
