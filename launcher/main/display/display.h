/* display: shell orientation from screen-axis acceleration and elapsed time. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "math/linear/vec3f.h"
#include "math/scalar/units.h"

/* How far anything meant to be read sits from every panel edge: a reading
 * margin, not hidden pixels. The cover glass hides nothing along the straight
 * edges (an edge ruler read on the board) and only the rounded
 * corners below. */
#define DISPLAY_PANEL_SAFE_INSET      15

/* Measured on the panel's cover glass. */
#define DISPLAY_PANEL_CORNER_RADIUS   42

/* Quarter names are board-wiring facts, measured with the orientation
 * overlay rather than derived from source.
 *
 *     0   Portrait               (USB connector to the right)
 *     1   Landscape              (USB connector at the top)
 *     2   Portrait, upside down
 *     3   Landscape, upside down */
#define DISPLAY_PORTRAIT              0
#define DISPLAY_LANDSCAPE             1
#define DISPLAY_PORTRAIT_UPSIDE_DOWN  2
#define DISPLAY_LANDSCAPE_UPSIDE_DOWN 3

#define DISPLAY_PORTRAIT_NAME         "portrait"
#define DISPLAY_LANDSCAPE_NAME        "landscape"

#define DISPLAY_QUARTER_COUNT         (DISPLAY_LANDSCAPE_UPSIDE_DOWN + 1)

/* The shell starts landscape because that is this board's normal held
 * orientation. display_init() remains neutral; display_reset_quarter()
 * applies this physical-board choice. */
#define DISPLAY_DEFAULT_QUARTER       DISPLAY_LANDSCAPE

/* Android WindowOrientationListener AccelSensorJudge, in ms, degrees and g. */
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
#define DISPLAY_MAX_TILT_SIN          DISPLAY_FLAT_SIN
#define DISPLAY_SWING_SIN             0.342020143F
#define DISPLAY_SWING_COS             0.939692621F
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

typedef struct {
    vec3f_t filtered;
    int64_t sample_us, predicted_us;
    int64_t flat_since_us, flat_ended_us, swing_ended_us, accel_ended_us, touch_ended_us;
    int quarter, predicted;
    bool primed, overhead, flat, touched;

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

/* Returns the horizontal inset where a row meets a rounded canvas corner. */
int display_panel_corner_inset(int radius, int canvas_height, int row);

/* The shell's own orientation, for an app drawing through the shell's
 * transform: knowing when it changed underneath you, without reading the IMU
 * again or duplicating the hysteresis above. */
int display_quarter_now(void);

/* The panel clock. Every app starts at the system value, the user's choice
 * kept across reboots. An app wanting another rate sets it with
 * gfx_set_panel_clock_hz(); the shell puts the system value back whenever
 * an app starts or exits, so no app restores it. The setter keeps the choice across a reboot; unchanged and
 * unsupported rates are ignored. */
int display_system_panel_clock_hz(void);
void display_set_system_panel_clock_hz(int hz);

/* Whatever the app that just started or exited did to the panel clock, the
 * next context begins from the system value. */
void display_restore_system_state(void);
