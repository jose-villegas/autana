/*
 * board_geometry: the device's shape and where the IMU sits in it, for figures and
 * host checks. No driver includes, so it links on a host.
 *
 * Device axes: x right, y up the screen, z out of the glass toward the viewer; origin
 * at the centre of the glass. Lengths in millimetres. The case and screen come from
 * the ESP32-S3-Touch-AMOLED-1.8's dimension drawing, the module and PCB from its
 * Waveshare 3D model; chip axes from the QMI8658C datasheet.
 */
#pragma once

/* Display module outline (3D model). */
#define BOARD_DEVICE_WIDTH_MM          33.6F
#define BOARD_DEVICE_HEIGHT_MM         41.2F
/* Plan-view corner radius of that outline (3D model, circle fit to the corner spline). */
#define BOARD_DEVICE_CORNER_RADIUS_MM  6.4F
/* Glass front to the PCB's back face: 6.2 mm module and stack, 1.2 mm PCB (3D model);
 * a placeholder until measured on a board. */
#define BOARD_DEVICE_DEPTH_MM          7.4F

/* Case width and height from the dimension drawing; the glass flush with the front,
 * spanning z = 0 to -BOARD_CASE_DEPTH_MM, is an estimate. */
#define BOARD_CASE_WIDTH_MM            37.60F
#define BOARD_CASE_HEIGHT_MM           45.20F
#define BOARD_CASE_DEPTH_MM            15.00F
/* The case wall follows the module outline (estimate; the drawing gives no radius). */
#define BOARD_CASE_CORNER_RADIUS_MM    (BOARD_DEVICE_CORNER_RADIUS_MM + (BOARD_CASE_WIDTH_MM - BOARD_DEVICE_WIDTH_MM) / 2)
/* The case's back face, square, with a screw in each corner (dimension drawing). */
#define BOARD_CASE_BACK_SIDE_MM        27.60F
#define BOARD_CASE_SCREW_RADIUS_MM     1.80F
/* The screen's visible area through the case (dimension drawing): 368 x 448
 * square pixels of 0.078 mm. */
#define BOARD_SCREEN_WIDTH_MM          28.70F
#define BOARD_SCREEN_HEIGHT_MM         34.94F

/* Right-side button positions and y lengths: vendor dimension drawing (about 7.3 px/mm).
 * Waveshare's open-back photo places BOOT above USB-C and PWR below it, behind the PCB. */
#define BOARD_BOOT_BUTTON_Y_MM         10.8F
#define BOARD_PWR_BUTTON_Y_MM          -10.7F
#define BOARD_BUTTON_SIZE_Y_MM         3.9F
/* Button z size, proud depth and placement behind the PCB are estimates. */
#define BOARD_BUTTON_SIZE_Z_MM         2.5F
#define BOARD_BUTTON_PROUD_MM          0.4F
#define BOARD_BUTTON_CENTRE_Z_MM       (-(BOARD_DEVICE_DEPTH_MM + BOARD_BUTTON_SIZE_Z_MM / 2))

/* USB Type-C receptacle shell dimensions; centred on the right in the vendor drawing.
 * Its centre behind the PCB's back face is estimated from Waveshare's open-back photo. */
#define BOARD_USB_C_Y_MM               0.0F
#define BOARD_USB_C_SIZE_Y_MM          8.94F
#define BOARD_USB_C_SIZE_Z_MM          3.26F
#define BOARD_USB_C_CENTRE_Z_MM        (-(BOARD_DEVICE_DEPTH_MM + BOARD_USB_C_SIZE_Z_MM / 2))

/* Left-side openings: vendor dimension drawing's side view (about 7.3 px/mm),
 * oriented against the PCB using Waveshare's open-back photo. */
#define BOARD_SPEAKER_PORT_DIAMETER_MM 2.5F
#define BOARD_SPEAKER_PORT_Y_MM        17.4F
#define BOARD_SPEAKER_PORT_Z_MM        -8.0F
/* The drawing shows a slot beside the speaker; which opening carries sound is undocumented. */
#define BOARD_SPEAKER_VENT_SIZE_Y_MM   8.7F
#define BOARD_SPEAKER_VENT_SIZE_Z_MM   1.1F
#define BOARD_SPEAKER_VENT_Y_MM        9.0F
#define BOARD_SPEAKER_VENT_Z_MM        -5.4F
/* microSD opening: vendor side view; the photo places the slot on the PCB's module side. */
#define BOARD_SD_SLOT_SIZE_Y_MM        14.5F
#define BOARD_SD_SLOT_SIZE_Z_MM        2.7F
#define BOARD_SD_SLOT_Y_MM             -6.3F
#define BOARD_SD_SLOT_Z_MM             -5.4F
/* Mic opening: vendor side view, behind the PCB in Waveshare's open-back photo. */
#define BOARD_MIC_PORT_DIAMETER_MM     1.0F
#define BOARD_MIC_PORT_Y_MM            -13.3F
#define BOARD_MIC_PORT_Z_MM            -8.5F

/* QMI8658C package, 2.5 x 3.0 x 0.86 mm LGA (datasheet, page 1); the 3.0 mm side runs along chip X. */
#define BOARD_IMU_SIZE_X_MM            3.0F
#define BOARD_IMU_SIZE_Y_MM            2.5F
#define BOARD_IMU_SIZE_Z_MM            0.86F
/* Package centre on the PCB's back face, from the glass centre (3D model, U5). */
#define BOARD_IMU_CENTRE_X_MM          -0.53F
#define BOARD_IMU_CENTRE_Y_MM          0.22F

/* Chip axes as device unit vectors. The 3D model puts U5's pin-1 mark at its +x, -y
 * corner with the marked face toward the back; datasheet Figure 7 puts pin 1 at the
 * chip's -X, +Y corner with Z out of the marked face. */
#define BOARD_IMU_AXIS_X               {0.0F, 1.0F, 0.0F}
#define BOARD_IMU_AXIS_Y               {1.0F, 0.0F, 0.0F}
#define BOARD_IMU_AXIS_Z               {0.0F, 0.0F, -1.0F}
