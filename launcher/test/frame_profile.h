/* Plays a clip in real time: each frame advances the clip by the previous frame's wall time.
 * An untimed warm-up frame at dt 0 supplies the first step and is not sampled. */
#pragma once
#include "math/scalar/units.h"
#include "testkit/bench.h"

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

/* A suite timing fixed poses logs one line per pose and one summary per row. */
#define FRAME_PROFILE_POSE         "%s t=%6ums clusters=%4d tris=%5d | both cores: frame %7lldus"

#define FRAME_PROFILE_ROW          "%s both cores: mean %lldus (%.1f fps before present), worst %lldus, med %lldus, p95 %lldus"

/* A traced playback logs every frame at its path time, then its worst frames, worst first;
 * a suite that replays a pose logs what the cull counted there and its frame-cost report. */
#define FRAME_PROFILE_WORST_K      5  /* worst frames named per row and quarter */
#define FRAME_PROFILE_PATH_POSES   64 /* poses replayed evenly along the path, for the counter curves */
#define FRAME_PROFILE_REPLAY_DRAWS 3  /* frames drawn at a replayed pose for its frame-cost report */
/* A trace line carries up to FRAME_PROFILE_LINE_FRAMES frames, its count first, so a burst of lines between
 * two presents stays within what the frame watch queues, and a line cut in transfer shows. */
#define FRAME_PROFILE_LINE_FRAMES  32
#define FRAME_PROFILE_TRACE        "frame_trace: %s n=%d%s"
#define FRAME_PROFILE_TRACE_FRAME  " %d:%d" /* path ms, frame us */
#define FRAME_PROFILE_RANKED       "frame_worst: %s rank=%d t=%dms frame=%dus"
#define FRAME_PROFILE_REPLAY       "frame_replay: %s t=%dms %s | %s"

/* Advances by dt_us and returns wall time in us; warm-up receives zero. */
typedef int64_t (*frame_profile_frame_fn)(void* ctx, int64_t dt_us);

typedef struct {
    const char* row;
    int64_t duration_us;
} frame_profile_scenario_t;

typedef struct {
    int frames;
    int64_t total_us;
    bench_stats_t frame_us;
} frame_profile_t;

/* One played frame: how far along the path it drew, and its wall time. */
typedef struct {
    int32_t path_ms;
    int32_t frame_us;
} frame_profile_frame_t;

typedef struct {
    frame_profile_frame_t* frame;
    int frames;
} frame_profile_trace_t;

/* Returns zero frames on an invalid duration, an allocation failure, a non-positive frame time,
 * or more frames than the shortest allowed frame time can produce. `trace`, when not NULL, keeps
 * every frame until frame_profile_trace_free(); it is empty whenever the profile is. */
frame_profile_t frame_profile_play(const frame_profile_scenario_t* s, frame_profile_frame_fn frame, void* ctx,
                                   frame_profile_trace_t* trace);
void frame_profile_trace_free(frame_profile_trace_t* trace);
void frame_profile_log(const char* tag, const char* row, const frame_profile_t* p, const char* quarter);

/* Fills `index` with up to `k` frames of `trace`, slowest first, the earlier of equal ones first;
 * returns how many. */
int frame_profile_worst(const frame_profile_trace_t* trace, int k, int* index);

/* Every frame of `trace`, then the `count` frames of `worst` ranked from one. */
void frame_profile_log_trace(const char* tag, const char* row, const frame_profile_trace_t* trace, const int* worst,
                             int count, const char* quarter);
