/*
 * display: which way is "up", decided once for the whole shell.
 *
 * Orientation belongs to the physical device, not to any one app's panel, so
 * it is decided here and the shell applies it; every UI surface follows.
 * display.c has no IMU, no gfx, no ui: the gravity vector arrives already
 * read, which is what lets it link and run on a host. The shell's side is
 * display_shell.h.
 *
 * The hysteresis is the module, not a refinement of it. Snapping to whichever
 * of gx/gy is larger puts the boundary at 45 degrees, where a board held near
 * that angle flips the whole shell every frame the tilt wobbles across it. A
 * Schmitt trigger expressed directly in the gravity components replaces it:
 * DISPLAY_HYST_NUM/DEN is 7/4, a small-integer stand-in for tan(60 degrees),
 * and applying that one ratio against whichever quarter is current is what
 * yields "60 degrees out, 30 back" without needing a second constant.
 */
#pragma once

#include <stdbool.h>

/* The cover glass hides roughly this many pixels along every edge of the
 * panel, and more where the corners round off. Anything meant to be read
 * insets by at least this much. Measured on the board. */
#define DISPLAY_PANEL_SAFE_INSET    15

/* Measured on the panel's cover glass. */
#define DISPLAY_PANEL_CORNER_RADIUS 42

/* tan(60 deg) = 1.732..., approximated as a small integer ratio so the
 * hysteresis test is exact integer (cross-multiplied) arithmetic: no
 * division, no float, no rounding to reason about. See this header's
 * top comment for why one ratio, applied relative to whichever quarter
 * is currently committed, is enough to give both the 60-degrees-out and
 * the 30-degrees-back behaviour. */
#define DISPLAY_HYST_NUM            7
#define DISPLAY_HYST_DEN            4

typedef struct {
    /* Which quarter turn currently reads as "upright", numbered the
     * same way gfx_text_turned() and ui_transform_quarter_turn() do: 0
     * upright, 1 top-to-bottom, 2 upside down, 3 bottom-to-top. The only
     * state this module keeps. The hysteresis test above is a pure
     * function of (quarter, gx, gy); nothing here accumulates over
     * time or needs a clock, which is also why display_update() takes
     * no dt: the caller controls how often it is called, and the decision
     * itself does not care. */
    int quarter;
} display_t;

/* Starts upright (quarter 0). There is no "unknown" orientation to
 * represent: a board that has not been read yet is assumed held the most
 * common way, and the first real reading corrects it if that guess was
 * wrong, the same as any other update. */

/* WHAT EACH QUARTER IS, MEASURED NOT DERIVED: which orientation a turn
 * corresponds to is not visible from source, depends on how the case
 * is held versus how the panel's rows/columns are wired, so it was
 * measured, by holding the board each way with a development build's
 * show-orientation overlay on and reading it off the panel.
 *
 *     0   Portrait               (USB connector to the right)
 *     1   Landscape              (USB connector at the top)
 *     2   Portrait, upside down
 *     3   Landscape, upside down */
#define DISPLAY_PORTRAIT              0
#define DISPLAY_LANDSCAPE             1
#define DISPLAY_PORTRAIT_UPSIDE_DOWN  2
#define DISPLAY_LANDSCAPE_UPSIDE_DOWN 3

/* The orientation the SHELL applies at boot, before the first gravity
 * sample arrives; display_reset_quarter() sets it once, after display_init(),
 * which stays a neutral 0: this is a physical fact about one board, not
 * something a device-agnostic module should bake into its reset.
 * DISPLAY_LANDSCAPE, not a bare 1: this board is normally held sideways
 * to its native upright, and the table above confirms that is quarter
 * 1, independent of which edge USB sits on. */
#define DISPLAY_DEFAULT_QUARTER       DISPLAY_LANDSCAPE

void display_init(display_t* d);

/* Feed the current gravity vector, in whatever consistent units the caller's
 * IMU reading uses (screen X/Y axes, not raw sensor axes; see
 * imu_gravity_screen_x()). Returns true when d->quarter actually changed, which is the caller's
 * cue to push a new ui_set_transform(). */
bool display_update(display_t* d, int gx, int gy);

int display_quarter(const display_t* d);

/* Returns the horizontal inset where a row meets a rounded canvas corner. */
int display_panel_corner_inset(int radius, int canvas_height, int row);

/*
 * The shell's side, defined in display_device.c (device only): the one panel,
 * its orientation state and its clock. What only the shell calls is in
 * display_shell.h.
 */

/* The shell's own orientation, for an app drawing through the shell's
 * transform: knowing when it changed underneath you, without reading the IMU
 * again or duplicating the hysteresis above. */
int display_quarter_now(void);

/* The panel clock. Every app starts at the system value, the user's choice
 * kept across reboots. An app wanting another rate sets it with
 * gfx_set_panel_clock_hz(); the shell puts the system value back, and gfx
 * heal back to its defaults, whenever an app starts or exits, so no app
 * restores either. The setter keeps the choice across a reboot; unchanged and
 * unsupported rates are ignored. */
int display_system_panel_clock_hz(void);
void display_set_system_panel_clock_hz(int hz);

/* Whatever the app that just started or exited did to the panel clock or to
 * heal, the next context begins from the system value and heal's defaults. */
void display_restore_system_state(void);
