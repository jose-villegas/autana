/* display_judge: filtered quarter prediction and event cooldowns. */
#pragma once

#include "display/display.h"
#include "math/linear/vec3f.h"
#include "math/scalar/units.h"

/* Android WindowOrientationListener AccelSensorJudge [75] in docs/Citations.md.
 * This tree uses screen-axis down, board-relative tilt limits and shell sampling. */
#define DISPLAY_SAMPLE_MS             20
#define DISPLAY_FILTER_TAU_MS         200
#define DISPLAY_FILTER_RESET_MS       1000
#define DISPLAY_ACCEL_TOLERANCE_G     0.4F
#define DISPLAY_NEAR_ZERO_G           0.1F
#define DISPLAY_FLAT_DEG              80.0F
#define DISPLAY_FLAT_MS               1000
#define DISPLAY_SWING_DEG             20.0F
#define DISPLAY_SWING_MS              300
#define DISPLAY_MAX_TILT_DEG          80.0F
#define DISPLAY_OVERHEAD_ENTER_DEG    (-40.0F)
#define DISPLAY_OVERHEAD_EXIT_DEG     (-15.0F)
#define DISPLAY_HYSTERESIS_GAP_DEG    45.0F
#define DISPLAY_SETTLE_MS             40
#define DISPLAY_AFTER_FLAT_MS         500
#define DISPLAY_AFTER_ACCEL_MS        500
#define DISPLAY_AFTER_SWING_MS        300
#define DISPLAY_AFTER_TOUCH_MS        500
#define DISPLAY_TILT_MIN_DEG          (-25.0F)
#define DISPLAY_TILT_DEFAULT_MAX_DEG  70.0F
#define DISPLAY_TILT_ADJACENT_MAX_DEG 65.0F
#define DISPLAY_TILT_OPPOSITE_MAX_DEG 60.0F
#define DISPLAY_QUARTER_DEG           90.0F
#define DISPLAY_ADJACENT_MIN_DEG      ((DISPLAY_QUARTER_DEG + DISPLAY_HYSTERESIS_GAP_DEG) / 2.0F)
/* Precomputed companions of the degree limits above. */
#define DISPLAY_FLAT_SIN              0.984807753F
#define DISPLAY_MAX_TILT_SIN          0.984807753F
#define DISPLAY_SWING_SIN             0.342020143F
#define DISPLAY_OVERHEAD_ENTER_SIN    (-0.642787610F)
#define DISPLAY_OVERHEAD_EXIT_SIN     (-0.258819045F)
#define DISPLAY_TILT_MIN_SIN          (-0.422618262F)
#define DISPLAY_TILT_DEFAULT_MAX_SIN  0.939692621F
#define DISPLAY_TILT_ADJACENT_MAX_SIN 0.906307787F
#define DISPLAY_TILT_OPPOSITE_MAX_SIN 0.866025404F
#define DISPLAY_ADJACENT_MIN_COS      0.382683432F
#define DISPLAY_TILT_HISTORY_COUNT    (1 + (DISPLAY_SWING_MS + DISPLAY_SAMPLE_MS - 1) / DISPLAY_SAMPLE_MS)
#define DISPLAY_QUARTER_NONE          (-1)
#define DISPLAY_TIME_NONE             INT64_MIN
/* A bounded filter flush, then the longest event cooldown and settle, rounded to samples.
 * Applies to steady unit acceleration without a held quarter or continuing touch. */
#define DISPLAY_COMMIT_MS             (DISPLAY_FILTER_RESET_MS + DISPLAY_AFTER_FLAT_MS + DISPLAY_SETTLE_MS + 2 * DISPLAY_SAMPLE_MS)

_Static_assert(DISPLAY_AFTER_FLAT_MS >= DISPLAY_AFTER_ACCEL_MS && DISPLAY_AFTER_FLAT_MS >= DISPLAY_AFTER_SWING_MS
                   && DISPLAY_AFTER_FLAT_MS >= DISPLAY_AFTER_TOUCH_MS,
               "commit bound needs the longest cooldown");

typedef struct {
    vec3f_t filtered;
    int64_t sample_us, predicted_us;
    int64_t flat_since_us, flat_ended_us, swing_ended_us, accel_ended_us, touch_ended_us;
    int quarter, predicted;
    bool primed, overhead, flat;

    struct {
        float sine, cosine;
        int64_t time_us;
    } tilts[DISPLAY_TILT_HISTORY_COUNT];

    int tilt_next, tilt_count;
} display_t;

/* Starts at quarter 0. There is no "unknown" orientation: readings correct a
 * wrong guess like any other change. */
void display_init(display_t* d);

/* Acceleration is down at rest, in g: x right, y down, z out. True on a committed change. */
bool display_update(display_t* d, vec3f_t accel_g, int64_t now_us);
/* Call every frame a finger is down, before the orientation sample. */
void display_touch(display_t* d, int64_t now_us);

int display_quarter(const display_t* d);
/* Pure geometric rule; ignores event history. tilt_limits=false tests the raw gap edge. */
int display_candidate(vec3f_t down, int quarter, bool tilt_limits);
