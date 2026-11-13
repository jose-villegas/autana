/*
 * capybara_run: a straight run as a function of time, for a character and a
 * third-person camera. It idles, eases up to its peak speed, holds it, eases
 * down to rest at the end and idles again; the camera stands at its face
 * while it is still and swings round to its back as it speeds up, back the
 * same way as it slows. Nothing is integrated: any time is one call.
 */
#pragma once

#include <stdint.h>

typedef struct {
    float start, end; /* along the run, scene units; end > start */
    float peak_speed; /* scene units per second the cruise holds */
    uint32_t idle_ms; /* still, at each end */
    uint32_t ramp_ms; /* easing up, and again easing down */
} capybara_run_t;

typedef struct {
    float at;    /* along the run, scene units */
    float speed; /* scene units per second, never below 0 */
} capybara_run_at_t;

/* The speed the cruise holds: the run's peak, or lower when two ramps at the
 * peak would already overrun the distance (then there is no cruise). */
float capybara_run_cruise_speed(const capybara_run_t* run);

/* From the first idle's start to the last idle's end. */
uint32_t capybara_run_duration_ms(const capybara_run_t* run);

/* Where the run is, and how fast, `t_ms` from its start; held at the end
 * from capybara_run_duration_ms() on. */
capybara_run_at_t capybara_run_at(const capybara_run_t* run, uint32_t t_ms);

/* The camera's angle round the runner from its face (0) to its back (pi),
 * eased by its speed's share of the cruise. */
float capybara_run_camera_angle(const capybara_run_t* run, float speed);
