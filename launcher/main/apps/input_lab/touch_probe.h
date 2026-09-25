/*
 * touch_probe - where a finger lands against where it aimed. A target is a
 * square placed at random on the screen; each tap is scored against its
 * centre, and the offsets accumulate into a hit rate, a mean and a spread.
 *
 * Pure logic in the UI's own screen coordinates, so a portrait round and a
 * landscape round measure the same thing: the offset as the person holding
 * the board sees it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int x, y; /* top-left */
    int side;
} touch_probe_target_t;

typedef struct {
    int taps;
    int hits;
    int64_t sum_dx, sum_dy;
    int64_t sum_dx2, sum_dy2;
    int worst_dist2;
} touch_probe_stats_t;

/* A `side` square wholly inside a `screen_w` x `screen_h` screen with
 * `margin` to spare on every edge. `rng` is any nonzero seed, advanced. */
touch_probe_target_t touch_probe_next(uint32_t* rng, int screen_w, int screen_h, int side, int margin);

/* Scores a tap at (x, y); true when it lands inside the target. */
bool touch_probe_record(touch_probe_stats_t* stats, touch_probe_target_t target, int x, int y);

/* Offset from the target's centre to the tap, in screen pixels: positive
 * dx is right of it, positive dy is below it. */
void touch_probe_offset(touch_probe_target_t target, int x, int y, int* dx, int* dy);

float touch_probe_mean_dx(const touch_probe_stats_t* stats);
float touch_probe_mean_dy(const touch_probe_stats_t* stats);
float touch_probe_spread_dx(const touch_probe_stats_t* stats);
float touch_probe_spread_dy(const touch_probe_stats_t* stats);
