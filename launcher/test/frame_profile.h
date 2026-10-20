/* Plays a clip in real time: each frame advances the clip by the previous frame's wall time.
 * An untimed warm-up frame at dt 0 supplies the first step and is not sampled. */
#pragma once
#include "math/scalar/units.h"
#include "perf_stats.h"

#define FRAME_PROFILE_MIN_FRAME_US MATH_US_PER_MS
#define FRAME_PROFILE_PREFIX       "frame_profile:"
#define FRAME_PROFILE_FRAMES       "frames="
#define FRAME_PROFILE_MEAN         "mean"
#define FRAME_PROFILE_MED          "med"
#define FRAME_PROFILE_P95          "p95"
#define FRAME_PROFILE_WORST        "worst"
#define FRAME_PROFILE_BEST         "best"
#define FRAME_PROFILE_FPS          "fps"
#define FRAME_PROFILE_LOW          "low"
#define FRAME_PROFILE_QUARTER      "quarter="

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

/* Returns zero frames on an invalid duration, an allocation failure, a non-positive frame time,
 * or more frames than the shortest allowed frame time can produce. */
frame_profile_t frame_profile_play(const frame_profile_scenario_t* s, frame_profile_frame_fn frame, void* ctx);
void frame_profile_log(const char* tag, const char* row, const frame_profile_t* p, const char* quarter);
