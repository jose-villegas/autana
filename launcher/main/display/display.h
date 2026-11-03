/* display: board quarter names and panel geometry, the display face apps include. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* How far anything meant to be read sits from every panel edge: a reading
 * margin, not hidden pixels. The cover glass hides nothing along the straight
 * edges (an edge ruler read on the board) and only the rounded
 * corners below. */
#define DISPLAY_PANEL_SAFE_INSET           15

/* Measured on the panel's cover glass. */
#define DISPLAY_PANEL_CORNER_RADIUS        42

/* Quarter names are board-wiring facts, measured with the orientation
 * overlay rather than derived from source.
 *
 *     0   Portrait               (USB connector to the right)
 *     1   Landscape              (USB connector at the top)
 *     2   Portrait, upside down
 *     3   Landscape, upside down */
#define DISPLAY_PORTRAIT                   0
#define DISPLAY_LANDSCAPE                  1
#define DISPLAY_PORTRAIT_UPSIDE_DOWN       2
#define DISPLAY_LANDSCAPE_UPSIDE_DOWN      3

#define DISPLAY_PORTRAIT_NAME              "portrait"
#define DISPLAY_LANDSCAPE_NAME             "landscape"
#define DISPLAY_PORTRAIT_UPSIDE_DOWN_NAME  "portrait upside down"
#define DISPLAY_LANDSCAPE_UPSIDE_DOWN_NAME "landscape upside down"

#define DISPLAY_QUARTER_COUNT              (DISPLAY_LANDSCAPE_UPSIDE_DOWN + 1)

/* The shell starts landscape because that is this board's normal held
 * orientation. display_init() remains neutral; display_reset_quarter()
 * applies this physical-board choice. */
#define DISPLAY_DEFAULT_QUARTER            DISPLAY_LANDSCAPE

const char* display_quarter_name(int quarter);

/* Returns the horizontal inset where a row meets a rounded canvas corner. */
int display_panel_corner_inset(int radius, int canvas_height, int row);

/* The shell's own orientation, for an app drawing through the shell's
 * transform: knowing when it changed underneath you, without reading the IMU
 * again or duplicating the hysteresis above. */
int display_quarter_now(void);
