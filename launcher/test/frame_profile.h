/* Scenario playback follows the previous frame's wall time, including its warm-up. */
#pragma once
#include "perf_stats.h"

#define FRAME_PROFILE_MIN_FRAME_US  100
#define FRAME_PROFILE_US_PER_MS     1000
#define FRAME_PROFILE_US_PER_SECOND 1000000
#define FRAME_PROFILE_PREFIX        "frame_profile:"
#define FRAME_PROFILE_FRAMES        "frames="
#define FRAME_PROFILE_MEAN          "mean"
#define FRAME_PROFILE_MED           "med"
#define FRAME_PROFILE_P95           "p95"
#define FRAME_PROFILE_WORST         "worst"
#define FRAME_PROFILE_BEST          "best"
#define FRAME_PROFILE_FPS           "fps"
#define FRAME_PROFILE_LOW           "low"
#define FRAME_PROFILE_QUARTER       "quarter="

/* Advances by dt_us and returns wall time in us; warm-up receives zero. */
typedef int64_t (*frame_profile_frame_fn)(void* ctx, int64_t dt_us);

typedef struct {
    const char* row;
    int64_t duration_us;
} frame_profile_scenario_t;

typedef struct {
    int frames;
    int64_t total_us;
    perf_stats_t frame_us;
} frame_profile_t;

/* Zero frames reports failure, including exhausted sample capacity. Samples are call-owned. */
frame_profile_t frame_profile_play(const frame_profile_scenario_t* s, frame_profile_frame_fn frame, void* ctx);
void frame_profile_log(const char* tag, const char* row, const frame_profile_t* p, const char* quarter);
