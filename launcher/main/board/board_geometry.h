/*
 * board_geometry: the device's shape and where the IMU sits in it, for figures and
 * host checks. No driver includes, so it links on a host.
 *
 * Device axes: x right, y up the screen, z out of the glass toward the viewer; origin
 * at the centre of the glass. Lengths in millimetres. Shapes come from the ESP32-S3-Touch-
 * AMOLED-1.8's Waveshare 3D model; chip axes from the QMI8658C datasheet.
 */
#pragma once

/* Display module outline, the device's widest part (3D model). */
#define BOARD_DEVICE_WIDTH_MM         33.6F
#define BOARD_DEVICE_HEIGHT_MM        41.2F
/* Plan-view corner radius of that outline (3D model, circle fit to the corner spline). */
#define BOARD_DEVICE_CORNER_RADIUS_MM 6.4F
/* Glass front to the PCB's back face: 6.2 mm module and stack, 1.2 mm PCB (3D model);
 * a placeholder until measured on a board. */
#define BOARD_DEVICE_DEPTH_MM         7.4F

/* The AMOLED's active area corner to corner: Waveshare's "1.8inch" panel,
 * 368 x 448 pixels (product page). display/display_size.h derives a pixel's
 * size from it. */
#define BOARD_PANEL_DIAGONAL_MM       45.72F

/* QMI8658C package, 2.5 x 3.0 x 0.86 mm LGA (datasheet, page 1); the 3.0 mm side runs along chip X. */
#define BOARD_IMU_SIZE_X_MM           3.0F
#define BOARD_IMU_SIZE_Y_MM           2.5F
#define BOARD_IMU_SIZE_Z_MM           0.86F
/* Package centre on the PCB's back face, from the glass centre (3D model, U5). */
#define BOARD_IMU_CENTRE_X_MM         -0.53F
#define BOARD_IMU_CENTRE_Y_MM         0.22F

/* Chip axes as device unit vectors. The 3D model puts U5's pin-1 mark at its +x, -y
 * corner with the marked face toward the back; datasheet Figure 7 puts pin 1 at the
 * chip's -X, +Y corner with Z out of the marked face. */
#define BOARD_IMU_AXIS_X              {0.0F, 1.0F, 0.0F}
#define BOARD_IMU_AXIS_Y              {1.0F, 0.0F, 0.0F}
#define BOARD_IMU_AXIS_Z              {0.0F, 0.0F, -1.0F}
